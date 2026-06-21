"""Wire protocol for the LED-Ticker BLE service — pure, no I/O.

Every function returns a value or raises ValidationError / ProtocolError, so
the whole module is host-testable without Bluetooth.
"""
from __future__ import annotations

from dataclasses import dataclass

from .errors import ProtocolError, ValidationError

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
DISPLAY_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26b3"
TZ_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26b4"

BRIGHTNESS_RANGE = range(0, 16)       # MAX7219 intensity
SCROLL_MS_RANGE = range(20, 501)      # ms per scroll step (lower = faster)
MAX_LOCATION_LEN = 48                 # firmware cap; one entry must be < this
MAX_LOCATIONS_PAYLOAD = 245           # MAX_LOCATIONS * (MAX_LOCATION_LEN + 1)
MODE_CATEGORIES = ("stocks", "weather", "clock")


@dataclass(frozen=True)
class Status:
    text: str
    seconds: int  # 0 = indefinite

    @property
    def indefinite(self) -> bool:
        return self.seconds == 0


@dataclass(frozen=True)
class Display:
    brightness: int
    scroll_ms: int


def _no_pipe(value: str, what: str) -> None:
    if "|" in value:
        raise ValidationError(f"{what} cannot contain '|'")


def encode_tickers(symbols: list[str]) -> bytes:
    syms = [s.upper().strip() for s in symbols if s.strip()]
    if not syms:
        raise ValidationError("at least one ticker symbol is required")
    return ",".join(syms).encode()


def encode_status(text: str, minutes: int = 0) -> bytes:
    _no_pipe(text, "status text")
    if not isinstance(minutes, int) or isinstance(minutes, bool) or minutes < 0:
        raise ValidationError("minutes must be an integer >= 0 (0 = indefinite)")
    return f"{text}|{minutes * 60}".encode()


def parse_status(raw: str) -> Status | None:
    if not raw:
        return None
    if "|" not in raw:
        return Status(text=raw, seconds=0)
    text, secs = raw.rsplit("|", 1)
    try:
        return Status(text=text, seconds=int(secs))
    except ValueError:
        raise ProtocolError(f"unparseable status from device: {raw!r}")


def encode_display(brightness: int, speed: int) -> bytes:
    if brightness not in BRIGHTNESS_RANGE:
        raise ValidationError(
            f"brightness must be {BRIGHTNESS_RANGE.start}-{BRIGHTNESS_RANGE.stop - 1}, got {brightness}"
        )
    if speed not in SCROLL_MS_RANGE:
        raise ValidationError(
            f"scroll speed must be {SCROLL_MS_RANGE.start}-{SCROLL_MS_RANGE.stop - 1}, got {speed}"
        )
    return f"{brightness}|{speed}".encode()


def parse_display(raw: str) -> Display | None:
    if not raw:
        return None
    if "|" not in raw:
        raise ProtocolError(f"unparseable display settings from device: {raw!r}")
    b, s = raw.split("|", 1)
    try:
        return Display(brightness=int(b), scroll_ms=int(s))
    except ValueError:
        raise ProtocolError(f"unparseable display settings from device: {raw!r}")


def encode_wifi(ssid: str, password: str) -> bytes:
    _no_pipe(ssid, "SSID")
    return f"{ssid}|{password}".encode()


def validate_location(loc: str) -> str:
    """Normalize a 'LAT,LON,LABEL' entry or raise ValidationError.

    The firmware splits on the first two commas only, so the label may contain
    commas. Coordinates are supplied by the client (no on-device geocoding).
    """
    _no_pipe(loc, "location")
    parts = loc.split(",", 2)
    if len(parts) != 3:
        raise ValidationError(f'"{loc}" is not "LAT,LON,LABEL" (e.g. 47.61,-122.33,Seattle)')
    lat_s, lon_s, label = parts[0].strip(), parts[1].strip(), parts[2].strip()
    if not label:
        raise ValidationError(f'"{loc}" has an empty label')
    try:
        lat, lon = float(lat_s), float(lon_s)
    except ValueError:
        raise ValidationError(f'"{loc}" has non-numeric coordinates')
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        raise ValidationError(f'"{loc}" coordinates out of range')
    entry = f"{lat_s},{lon_s},{label}"
    if len(entry.encode()) >= MAX_LOCATION_LEN:
        raise ValidationError(f'"{loc}" is too long (>= {MAX_LOCATION_LEN} bytes)')
    return entry


def encode_locations(locations: list[str]) -> bytes:
    entries = [validate_location(loc.strip()) for loc in locations if loc.strip()]
    if not entries:
        raise ValidationError("at least one location is required")
    payload = "|".join(entries)
    if len(payload.encode()) >= MAX_LOCATIONS_PAYLOAD:
        raise ValidationError(f"locations too long ({len(payload.encode())} bytes)")
    return payload.encode()


def parse_locations(raw: str) -> list[str]:
    return raw.split("|") if raw else []


def normalize_mode(tokens: list[str]) -> str:
    if tokens == ["all"]:
        return "all"
    if tokens == ["none"]:
        return "none"
    bad = [t for t in tokens if t not in MODE_CATEGORIES]
    if bad or not tokens:
        joined = ", ".join(bad) or "(none given)"
        raise ValidationError(
            f"unknown mode token(s): {joined}; valid: {', '.join(MODE_CATEGORIES)} (or 'all'/'none')"
        )
    seen: list[str] = []
    for t in tokens:
        if t not in seen:
            seen.append(t)
    return ",".join(seen)


def encode_mode(tokens: list[str]) -> bytes:
    return normalize_mode(tokens).encode()


def validate_pin(pin: str) -> str:
    pin = pin.strip()
    if not pin.isdigit() or len(pin) != 6:
        raise ValidationError(f"PIN must be exactly 6 digits, got '{pin}'")
    return pin


def validate_timezone(tz: str) -> str:
    tz = tz.strip()
    if not tz or not tz[0].isalpha() or len(tz.encode()) >= 64:
        raise ValidationError(
            'timezone must be a POSIX TZ string (< 64 bytes), e.g. "PST8PDT,M3.2.0,M11.1.0"'
        )
    return tz


def validate_timer_minutes(mins: int) -> int:
    if not 1 <= mins <= 99:
        raise ValidationError("timer minutes must be 1-99")
    return mins
