#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["bleak"]
# ///
"""Control LED-Ticker over BLE.

Usage:
    uv run tools/led.py tickers AAPL MSFT GOOGL
    uv run tools/led.py status "BUSY" 30          # for 30 minutes
    uv run tools/led.py status "ON AIR"           # indefinite
    uv run tools/led.py status clear              # clear active sign
    uv run tools/led.py locations "Seattle, WA" 98052
    uv run tools/led.py mode stocks
    uv run tools/led.py mode stocks weather
    uv run tools/led.py mode clock
    uv run tools/led.py mode all
    uv run tools/led.py power off
    uv run tools/led.py power on

PIN auth (optional — only required when device has `pin-enforce on`):
    uv run tools/led.py pin 482913                # save PIN, reused by future calls
    uv run tools/led.py pin clear                 # forget saved PIN
    uv run tools/led.py --pin 482913 status "HI"  # one-shot override
    LED_TICKER_PIN=482913 uv run tools/led.py ... # env-var override
    uv run tools/led.py pin-enforce on            # device: require PIN for writes
    uv run tools/led.py pin-enforce off           # device: stop requiring PIN
"""

import asyncio
import os
import pathlib
import sys
from bleak import BleakScanner, BleakClient

# The firmware appends "-XXXX" (low 2 bytes of the chip MAC) to the base
# name so multiple units are distinguishable, so match by prefix rather
# than equality.
DEVICE_NAME_PREFIX = "LED-Ticker"
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
TICKER_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
MODE_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a9"
CMD_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26ab"
WIFI_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26ac"
APIKEY_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26ad"
LOCS_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26ae"
STATUS_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26af"
VERSION_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26b0"
POWER_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26b1"
AUTH_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26b2"

PIN_PATH = pathlib.Path.home() / ".config" / "led-ticker" / "pin"

# Populated from --pin parsing before dispatch.
_cli_pin: str | None = None


def _resolve_pin() -> str | None:
    if _cli_pin:
        return _cli_pin
    env = os.environ.get("LED_TICKER_PIN")
    if env:
        return env.strip()
    if PIN_PATH.exists():
        return PIN_PATH.read_text().strip() or None
    return None


async def _auth_for_write(client: BleakClient):
    """Send the saved PIN and verify the connection is now allowed to write.

    Writes the PIN to the Auth characteristic, then reads it back — the
    firmware returns "ok" iff the current connection is authenticated
    (either via bonding or via this PIN write). Anything else means the
    device rejected our PIN; exit with a clear error rather than letting
    the caller's write silently disappear into the void.

    Reads on this device are not gated server-side, so this helper is
    only invoked from the write path.
    """
    pin = _resolve_pin()
    if pin:
        await client.write_gatt_char(AUTH_CHAR_UUID, pin.encode(), response=True)
    try:
        status = (await client.read_gatt_char(AUTH_CHAR_UUID)).decode().strip()
    except Exception:
        # Older firmware (pre-0.3.0) didn't expose Auth as readable — fall
        # through; if writes get dropped the user will see no effect and
        # can re-check `led.py pin` themselves.
        return
    if status == "ok":
        return
    if pin:
        print(
            f"ERROR: device rejected saved PIN ({PIN_PATH}). The PIN was likely\n"
            "       rotated by a factory reset. Read the new PIN off the LED in\n"
            "       setup mode (or from the serial monitor) and run:\n"
            "         led.py pin <new-6-digits>",
            file=sys.stderr,
        )
    else:
        print(
            "ERROR: device has PIN enforcement on and no PIN is configured\n"
            "       client-side. Run: led.py pin <6-digits>  (PIN scrolls on the\n"
            "       LED in setup mode, or appears on the serial monitor at boot).",
            file=sys.stderr,
        )
    sys.exit(2)


async def find_device():
    print(f"Looking for {DEVICE_NAME_PREFIX}-*...")
    device = await BleakScanner.find_device_by_filter(
        lambda d, _ad: bool(d.name and d.name.startswith(DEVICE_NAME_PREFIX)),
        timeout=15,
    )
    if not device:
        print(f"ERROR: no '{DEVICE_NAME_PREFIX}-*' device found. Is it powered on and in range?")
        sys.exit(1)
    print(f"Found {device.name} @ {device.address}, connecting...")
    return device


async def read_chars(*char_uuids: str) -> list[str]:
    # Firmware reads are never auth-gated, so we skip the PIN write+verify
    # entirely. Saves one BLE round-trip per `led.py get …` call.
    device = await find_device()
    async with BleakClient(device) as client:
        return [(await client.read_gatt_char(uuid)).decode() for uuid in char_uuids]


async def send(char_uuid: str, payload: str):
    device = await find_device()
    async with BleakClient(device) as client:
        await _auth_for_write(client)
        await client.write_gatt_char(char_uuid, payload.encode(), response=True)
        shown = payload if payload else "(clear)"
        print(f"Sent: {shown}")


def cmd_tickers(args):
    if not args:
        print("Usage: led.py tickers TICKER [TICKER ...]")
        sys.exit(1)
    payload = ",".join(t.upper().strip() for t in args)
    asyncio.run(send(TICKER_CHAR_UUID, payload))


def cmd_status(args):
    # No args  → read current status
    # "clear"  → clear active sign
    # TEXT     → set indefinite
    # TEXT N   → set for N minutes (0 = indefinite). Wire format is seconds.
    if not args:
        (raw,) = asyncio.run(read_chars(STATUS_CHAR_UUID))
        print(_fmt_status(raw))
        return
    if args[0] == "clear":
        asyncio.run(send(STATUS_CHAR_UUID, ""))
        return
    text = args[0]
    if "|" in text:
        print("ERROR: status text cannot contain '|'")
        sys.exit(1)
    minutes = 0
    if len(args) >= 2:
        try:
            minutes = int(args[1])
        except ValueError:
            print(f"ERROR: duration must be an integer number of minutes, got '{args[1]}'")
            sys.exit(1)
        if minutes < 0:
            print("ERROR: duration must be >= 0 (0 = indefinite)")
            sys.exit(1)
    asyncio.run(send(STATUS_CHAR_UUID, f"{text}|{minutes * 60}"))


def cmd_mode(args):
    valid = {"stocks", "weather", "clock"}
    if not args:
        print("Usage: led.py mode all | none | <category> [<category> ...]")
        print("  where <category> is one of: stocks, weather, clock")
        print("  'none' = sign-only (idle pixel between signs)")
        sys.exit(1)
    if args == ["all"]:
        payload = "all"
    elif args == ["none"]:
        payload = "none"
    else:
        bad = [a for a in args if a not in valid]
        if bad:
            print(f"ERROR: unknown mode token(s): {', '.join(bad)}")
            print("  valid: stocks, weather, clock (or 'all' / 'none')")
            sys.exit(1)
        # de-dupe while preserving order
        seen = []
        for a in args:
            if a not in seen:
                seen.append(a)
        payload = ",".join(seen)
    asyncio.run(send(MODE_CHAR_UUID, payload))


def cmd_locations(args):
    if not args:
        print('Usage: led.py locations "City, State" [ZIP ...]')
        sys.exit(1)
    cleaned = [a.strip() for a in args if a.strip()]
    for loc in cleaned:
        if "|" in loc:
            print("ERROR: location cannot contain '|'")
            sys.exit(1)
    payload = "|".join(cleaned)
    if len(payload.encode()) >= 205:  # MAX_LOCATIONS * (MAX_LOCATION_LEN + 1)
        print(f"ERROR: locations too long ({len(payload.encode())} bytes)")
        sys.exit(1)
    asyncio.run(send(LOCS_CHAR_UUID, payload))


def cmd_apikey(args):
    if not args:
        print("Usage: led.py apikey KEY")
        sys.exit(1)
    asyncio.run(send(APIKEY_CHAR_UUID, args[0]))


def cmd_wifi(args):
    if len(args) < 2:
        print("Usage: led.py wifi SSID PASSWORD")
        sys.exit(1)
    ssid = " ".join(args[:-1])
    password = args[-1]
    if "|" in ssid:
        print("ERROR: SSID cannot contain '|'")
        sys.exit(1)
    asyncio.run(send(WIFI_CHAR_UUID, f"{ssid}|{password}"))


def cmd_power(args):
    if not args or args[0] not in ("on", "off"):
        print("Usage: led.py power on | off")
        sys.exit(1)
    asyncio.run(send(POWER_CHAR_UUID, args[0]))


def cmd_pin(args):
    if not args:
        # Show whatever's currently saved (without leaking it via subprocess
        # listings — print only the first/last digit when long).
        if PIN_PATH.exists():
            saved = PIN_PATH.read_text().strip()
            if saved:
                print(f"Saved PIN: {saved}  (path: {PIN_PATH})")
                return
        print(f"No PIN saved at {PIN_PATH}")
        if os.environ.get("LED_TICKER_PIN"):
            print("(LED_TICKER_PIN env var is set — that will be used)")
        return
    if args[0] == "clear":
        if PIN_PATH.exists():
            PIN_PATH.unlink()
            print(f"Cleared saved PIN ({PIN_PATH})")
        else:
            print("No PIN was saved.")
        return
    pin = args[0].strip()
    if not pin.isdigit() or len(pin) != 6:
        print(f"ERROR: PIN must be exactly 6 digits, got '{pin}'")
        sys.exit(1)
    PIN_PATH.parent.mkdir(parents=True, exist_ok=True)
    PIN_PATH.write_text(pin + "\n")
    PIN_PATH.chmod(0o600)
    print(f"Saved PIN to {PIN_PATH} (future calls will include it automatically)")


def cmd_pin_enforce(args):
    if not args or args[0] not in ("on", "off"):
        print("Usage: led.py pin-enforce on | off")
        print("  on  — device requires PIN auth for every write")
        print("  off — device accepts writes from anyone (default)")
        sys.exit(1)
    asyncio.run(send(CMD_CHAR_UUID, f"pin-enforce {args[0]}"))


def _fmt_status(v: str) -> str:
    if not v:
        return "(no active status)"
    if "|" not in v:
        return v
    text, secs = v.rsplit("|", 1)
    try:
        s = int(secs)
    except ValueError:
        return v
    if s == 0:
        return f"{text} (indefinite)"
    m, sec = divmod(s, 60)
    return f"{text} ({m}m {sec}s remaining)"


GET_READABLE = {
    "wifi": (WIFI_CHAR_UUID, lambda v: v or "(not set)"),
    "apikey": (APIKEY_CHAR_UUID, lambda v: v or "(not set)"),
    "tickers": (TICKER_CHAR_UUID, lambda v: v or "(none)"),
    "status": (STATUS_CHAR_UUID, _fmt_status),
    "locations": (
        LOCS_CHAR_UUID,
        lambda v: "\n".join(
            f"  {i + 1}. {loc}" for i, loc in enumerate(v.split("|") if v else [])
        )
        or "(none)",
    ),
    "mode": (MODE_CHAR_UUID, lambda v: v or "(unknown)"),
    "version": (VERSION_CHAR_UUID, lambda v: v or "(unknown — pre-0.1.0 firmware?)"),
    "power": (POWER_CHAR_UUID, lambda v: v or "(unknown)"),
}


def cmd_get(args):
    if not args or args[0] not in GET_READABLE:
        print(f"Usage: led.py get {'|'.join(GET_READABLE)}")
        sys.exit(1)
    key = args[0]
    uuid, fmt = GET_READABLE[key]
    (raw,) = asyncio.run(read_chars(uuid))
    print(fmt(raw))


def cmd_reload(_args):
    asyncio.run(send(CMD_CHAR_UUID, "reload"))


def cmd_timer(args):
    if not args:
        print("Usage: led.py timer <minutes 1-99 | cancel>")
        sys.exit(1)
    arg = args[0].strip().lower()
    if arg == "cancel":
        asyncio.run(send(CMD_CHAR_UUID, "timer cancel"))
        return
    try:
        mins = int(arg)
    except ValueError:
        print("ERROR: minutes must be an integer 1-99 (or 'cancel')")
        sys.exit(1)
    if not 1 <= mins <= 99:
        print("ERROR: minutes must be 1-99")
        sys.exit(1)
    asyncio.run(send(CMD_CHAR_UUID, f"timer {mins}"))


def cmd_reset(_args):
    confirm = input("Reset all NVS data to config.h defaults (also rotates PIN)? [y/N] ")
    if confirm.strip().lower() != "y":
        print("Aborted.")
        sys.exit(0)
    asyncio.run(send(CMD_CHAR_UUID, "reset"))


COMMANDS = {
    "tickers": cmd_tickers,
    "status": cmd_status,
    "locations": cmd_locations,
    "mode": cmd_mode,
    "power": cmd_power,
    "apikey": cmd_apikey,
    "wifi": cmd_wifi,
    "get": cmd_get,
    "reload": cmd_reload,
    "reset": cmd_reset,
    "timer": cmd_timer,
    "pin": cmd_pin,
    "pin-enforce": cmd_pin_enforce,
}


def _print_help():
    print("Usage: led.py [--pin XXXXXX] <command> [args...]")
    print()
    print("  tickers     AAPL MSFT GOOGL          set stock symbols and reload quotes")
    print("  status      [TEXT [MINUTES] | clear] set / clear the active sign (0 min = indefinite)")
    print("  timer       <minutes 1-99 | cancel>  start/cancel a countdown timer on the LED")
    print("  locations   'Seattle, WA' 98052 ...  set weather locations (zip or city)")
    print("  mode        all | <cat> [<cat> ...]  switch display mode (cat: stocks|weather|clock)")
    print("  power       on | off                 turn display on or off (volatile)")
    print("  apikey      KEY                      set Finnhub API key")
    print("  wifi        SSID PASSWORD            update WiFi credentials and reconnect")
    print("  get         wifi|apikey|tickers|status|locations|mode|version|power  read a setting")
    print("  reload                               force immediate stock refresh")
    print("  reset                                clear NVS and revert to defaults (rotates PIN)")
    print("  pin         [DIGITS | clear]         save / show / clear local PIN cache")
    print("  pin-enforce on | off                 toggle device-side PIN enforcement")


if __name__ == "__main__":
    argv = sys.argv[1:]
    # Optional --pin XXXXXX prefix (or anywhere before the subcommand).
    if argv and argv[0] == "--pin":
        if len(argv) < 2:
            print("ERROR: --pin requires a value")
            sys.exit(1)
        _cli_pin = argv[1].strip()
        argv = argv[2:]
    if not argv or argv[0] not in COMMANDS:
        _print_help()
        sys.exit(1)
    COMMANDS[argv[0]](argv[1:])
