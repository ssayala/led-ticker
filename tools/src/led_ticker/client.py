"""BLE transport: an async core wrapped by a synchronous LedTicker facade.

The facade owns one event loop per `with` block, so a single connection is
reused across many operations. Module-level one-shots wrap a one-op session.
"""
from __future__ import annotations

import asyncio

from bleak import BleakClient, BleakScanner

from . import protocol as P
from .auth import resolve_pin
from .errors import AmbiguousDeviceError, AuthError, DeviceNotFoundError


class LedTicker:
    def __init__(self, select=None, address=None, name_prefix=P.DEVICE_NAME_PREFIX,
                 scan_timeout=4.0, timeout=15.0, pin=None):
        self._select = select
        self._address = address
        self._prefix = name_prefix
        self._scan_timeout = scan_timeout
        self._timeout = timeout
        self._pin = pin
        self._loop = None
        self._client = None
        self._authed = False

    # -- lifecycle ----------------------------------------------------------
    def __enter__(self):
        self._loop = asyncio.new_event_loop()
        try:
            self._client = self._run(self._connect())
        except BaseException:
            self._loop.close()
            self._loop = None
            raise
        return self

    def __exit__(self, *exc):
        try:
            if self._client is not None and self._client.is_connected:
                self._run(self._client.disconnect())
        finally:
            self._loop.close()
            self._loop = None
            self._client = None
            self._authed = False

    def _run(self, coro):
        return self._loop.run_until_complete(coro)

    async def _connect(self):
        if self._address:
            device = await BleakScanner.find_device_by_address(
                self._address, timeout=self._timeout
            )
            if device is None:
                raise DeviceNotFoundError(f"no device with address {self._address}")
        else:
            matches = await _discover_raw(self._prefix, self._scan_timeout)
            if self._select:
                matches = [
                    (dev, rssi) for dev, rssi in matches
                    if P.device_matches(dev.name, dev.address, self._select)
                ]
            if not matches:
                if self._select:
                    raise DeviceNotFoundError(
                        f"no '{self._prefix}-*' device matching '{self._select}'"
                    )
                raise DeviceNotFoundError(f"no '{self._prefix}-*' device found")
            if len(matches) > 1:
                raise AmbiguousDeviceError(
                    candidates=[_to_info(dev, rssi) for dev, rssi in matches]
                )
            device = matches[0][0]
        client = BleakClient(device)
        await client.connect()
        return client

    async def _ensure_authed(self):
        if self._authed:
            return
        pin = resolve_pin(self._pin)
        if pin:
            await self._client.write_gatt_char(P.AUTH_CHAR_UUID, pin.encode(), response=True)
        try:
            status = (await self._client.read_gatt_char(P.AUTH_CHAR_UUID)).decode().strip()
        except Exception:
            # Pre-0.3.0 firmware: Auth char not readable. Best-effort; proceed.
            self._authed = True
            return
        if status == "ok":
            self._authed = True
            return
        raise AuthError(pin_present=bool(pin))

    async def _write(self, uuid, payload):
        await self._ensure_authed()
        await self._client.write_gatt_char(uuid, payload, response=True)

    async def _read(self, uuid):
        return (await self._client.read_gatt_char(uuid)).decode()

    # -- writes -------------------------------------------------------------
    def set_tickers(self, symbols):
        self._run(self._write(P.TICKER_CHAR_UUID, P.encode_tickers(symbols)))

    def set_status(self, text, minutes=0):
        self._run(self._write(P.STATUS_CHAR_UUID, P.encode_status(text, minutes)))

    def clear_status(self):
        self._run(self._write(P.STATUS_CHAR_UUID, b""))

    def set_timer(self, minutes):
        payload = f"timer {P.validate_timer_minutes(minutes)}".encode()
        self._run(self._write(P.CMD_CHAR_UUID, payload))

    def cancel_timer(self):
        self._run(self._write(P.CMD_CHAR_UUID, b"timer cancel"))

    def set_locations(self, locations):
        self._run(self._write(P.LOCS_CHAR_UUID, P.encode_locations(locations)))

    def set_mode(self, tokens):
        self._run(self._write(P.MODE_CHAR_UUID, P.encode_mode(tokens)))

    def set_power(self, on):
        self._run(self._write(P.POWER_CHAR_UUID, b"on" if on else b"off"))

    def set_display(self, brightness, speed):
        self._run(self._write(P.DISPLAY_CHAR_UUID, P.encode_display(brightness, speed)))

    def set_brightness(self, brightness):
        cur = self.get_display()
        # None means pre-Display firmware / empty read; fall back to range minimum.
        speed = cur.scroll_ms if cur else P.SCROLL_MS_RANGE.start
        self.set_display(brightness, speed)

    def set_scroll_speed(self, speed):
        cur = self.get_display()
        # None means pre-Display firmware / empty read; fall back to range minimum.
        brightness = cur.brightness if cur else P.BRIGHTNESS_RANGE.start
        self.set_display(brightness, speed)

    def set_timezone(self, tz):
        self._run(self._write(P.TZ_CHAR_UUID, P.validate_timezone(tz).encode()))

    def set_apikey(self, key):
        self._run(self._write(P.APIKEY_CHAR_UUID, key.encode()))

    def set_wifi(self, ssid, password):
        self._run(self._write(P.WIFI_CHAR_UUID, P.encode_wifi(ssid, password)))

    def set_pin_enforce(self, on):
        self._run(self._write(P.CMD_CHAR_UUID, b"pin-enforce on" if on else b"pin-enforce off"))

    def reload(self):
        self._run(self._write(P.CMD_CHAR_UUID, b"reload"))

    def reset(self):
        self._run(self._write(P.CMD_CHAR_UUID, b"reset"))

    # -- reads (never auth-gated server-side) -------------------------------
    def get_version(self):
        return self._run(self._read(P.VERSION_CHAR_UUID))

    def get_wifi(self):
        return self._run(self._read(P.WIFI_CHAR_UUID))

    def get_apikey(self):
        return self._run(self._read(P.APIKEY_CHAR_UUID))

    def get_tickers(self):
        raw = self._run(self._read(P.TICKER_CHAR_UUID))
        return raw.split(",") if raw else []

    def get_status(self):
        return P.parse_status(self._run(self._read(P.STATUS_CHAR_UUID)))

    def get_locations(self):
        return P.parse_locations(self._run(self._read(P.LOCS_CHAR_UUID)))

    def get_mode(self):
        return self._run(self._read(P.MODE_CHAR_UUID))

    def get_power(self):
        return self._run(self._read(P.POWER_CHAR_UUID))

    def get_display(self):
        return P.parse_display(self._run(self._read(P.DISPLAY_CHAR_UUID)))

    def get_timezone(self):
        return self._run(self._read(P.TZ_CHAR_UUID))


async def _discover_raw(name_prefix, scan_timeout):
    """Enumerate matching devices as (BLEDevice, rssi), strongest signal first."""
    found = await BleakScanner.discover(timeout=scan_timeout, return_adv=True)
    matches = [
        (dev, adv.rssi)
        for dev, adv in found.values()
        if dev.name and dev.name.startswith(name_prefix)
    ]
    matches.sort(key=lambda t: (t[1] is None, -(t[1] or 0)))
    return matches


def _to_info(dev, rssi):
    return P.DeviceInfo(name=dev.name, address=dev.address, rssi=rssi)


def scan(name_prefix=P.DEVICE_NAME_PREFIX, scan_timeout=4.0):
    """List LED-Ticker devices in range, strongest signal first."""
    loop = asyncio.new_event_loop()
    try:
        matches = loop.run_until_complete(_discover_raw(name_prefix, scan_timeout))
    finally:
        loop.close()
    return [_to_info(dev, rssi) for dev, rssi in matches]


_CONN_KW = ("select", "address", "name_prefix", "scan_timeout", "timeout", "pin")


def _one_shot(method, *args, **kwargs):
    conn = {k: kwargs.pop(k) for k in list(kwargs) if k in _CONN_KW}
    with LedTicker(**conn) as d:
        return getattr(d, method)(*args, **kwargs)


# Module-level one-shots: each opens a fresh connection, does one op, disconnects.
def set_tickers(symbols, **kw): return _one_shot("set_tickers", symbols, **kw)
def set_status(text, minutes=0, **kw): return _one_shot("set_status", text, minutes, **kw)
def clear_status(**kw): return _one_shot("clear_status", **kw)
def set_timer(minutes, **kw): return _one_shot("set_timer", minutes, **kw)
def cancel_timer(**kw): return _one_shot("cancel_timer", **kw)
def set_locations(locations, **kw): return _one_shot("set_locations", locations, **kw)
def set_mode(tokens, **kw): return _one_shot("set_mode", tokens, **kw)
def set_power(on, **kw): return _one_shot("set_power", on, **kw)
def set_display(brightness, speed, **kw): return _one_shot("set_display", brightness, speed, **kw)
def set_brightness(brightness, **kw): return _one_shot("set_brightness", brightness, **kw)
def set_scroll_speed(speed, **kw): return _one_shot("set_scroll_speed", speed, **kw)
def set_timezone(tz, **kw): return _one_shot("set_timezone", tz, **kw)
def set_apikey(key, **kw): return _one_shot("set_apikey", key, **kw)
def set_wifi(ssid, password, **kw): return _one_shot("set_wifi", ssid, password, **kw)
def set_pin_enforce(on, **kw): return _one_shot("set_pin_enforce", on, **kw)
def reload(**kw): return _one_shot("reload", **kw)
def reset(**kw): return _one_shot("reset", **kw)
def get_version(**kw): return _one_shot("get_version", **kw)
def get_wifi(**kw): return _one_shot("get_wifi", **kw)
def get_apikey(**kw): return _one_shot("get_apikey", **kw)
def get_tickers(**kw): return _one_shot("get_tickers", **kw)
def get_status(**kw): return _one_shot("get_status", **kw)
def get_locations(**kw): return _one_shot("get_locations", **kw)
def get_mode(**kw): return _one_shot("get_mode", **kw)
def get_power(**kw): return _one_shot("get_power", **kw)
def get_display(**kw): return _one_shot("get_display", **kw)
def get_timezone(**kw): return _one_shot("get_timezone", **kw)
