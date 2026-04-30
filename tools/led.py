#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.11"
# dependencies = ["bleak"]
# ///
"""Control LED-Ticker over BLE.

Usage:
    uv run tools/led.py tickers AAPL MSFT GOOGL
    uv run tools/led.py messages "Take a break!" "Drink water!" "Stand up!"
    uv run tools/led.py locations "Seattle, WA" 98052
    uv run tools/led.py mode stocks
    uv run tools/led.py mode messages
    uv run tools/led.py mode all
"""

import asyncio
import sys
from bleak import BleakScanner, BleakClient

# The firmware appends "-XXXX" (low 2 bytes of the chip MAC) to the base
# name so multiple units are distinguishable, so match by prefix rather
# than equality.
DEVICE_NAME_PREFIX = "LED-Ticker"
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
TICKER_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
MODE_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a9"
MSGS_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26aa"
CMD_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26ab"
WIFI_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26ac"
APIKEY_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26ad"
LOCS_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26ae"


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
    device = await find_device()
    async with BleakClient(device) as client:
        return [(await client.read_gatt_char(uuid)).decode() for uuid in char_uuids]


async def send(char_uuid: str, payload: str):
    device = await find_device()
    async with BleakClient(device) as client:
        await client.write_gatt_char(char_uuid, payload.encode(), response=True)
        print(f"Sent: {payload}")


def cmd_tickers(args):
    if not args:
        print("Usage: led.py tickers TICKER [TICKER ...]")
        sys.exit(1)
    payload = ",".join(t.upper().strip() for t in args)
    asyncio.run(send(TICKER_CHAR_UUID, payload))


def cmd_messages(args):
    if not args:
        print("Usage: led.py messages MSG [MSG ...]")
        sys.exit(1)
    payload = "|".join(args)
    if len(payload.encode()) >= 512:
        print(f"ERROR: messages too long ({len(payload.encode())} bytes, max 511)")
        sys.exit(1)
    asyncio.run(send(MSGS_CHAR_UUID, payload))


def cmd_mode(args):
    if not args or args[0] not in ("stocks", "messages", "weather", "all"):
        print("Usage: led.py mode stocks|messages|weather|all")
        sys.exit(1)
    asyncio.run(send(MODE_CHAR_UUID, args[0]))


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


GET_READABLE = {
    "wifi": (WIFI_CHAR_UUID, lambda v: v or "(not set)"),
    "apikey": (APIKEY_CHAR_UUID, lambda v: v or "(not set)"),
    "tickers": (TICKER_CHAR_UUID, lambda v: v or "(none)"),
    "messages": (
        MSGS_CHAR_UUID,
        lambda v: "\n".join(
            f"  {i + 1}. {m}" for i, m in enumerate(v.split("|") if v else [])
        ),
    ),
    "locations": (
        LOCS_CHAR_UUID,
        lambda v: "\n".join(
            f"  {i + 1}. {loc}" for i, loc in enumerate(v.split("|") if v else [])
        )
        or "(none)",
    ),
    "mode": (MODE_CHAR_UUID, lambda v: v or "(unknown)"),
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


def cmd_reset(_args):
    confirm = input("Reset all NVS data to config.h defaults? [y/N] ")
    if confirm.strip().lower() != "y":
        print("Aborted.")
        sys.exit(0)
    asyncio.run(send(CMD_CHAR_UUID, "reset"))


COMMANDS = {
    "tickers": cmd_tickers,
    "messages": cmd_messages,
    "locations": cmd_locations,
    "mode": cmd_mode,
    "apikey": cmd_apikey,
    "wifi": cmd_wifi,
    "get": cmd_get,
    "reload": cmd_reload,
    "reset": cmd_reset,
}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print("Usage: led.py <command> [args...]")
        print()
        print("  tickers   AAPL MSFT GOOGL         set stock symbols and reload quotes")
        print("  messages  'msg1' 'msg2' ...        set scrolling messages (persisted)")
        print("  locations 'Seattle, WA' 98052 ...  set weather locations (zip or city)")
        print("  mode      stocks|messages|weather|all  switch display mode")
        print("  apikey    KEY                      set Finnhub API key")
        print(
            "  wifi      SSID PASSWORD             update WiFi credentials and reconnect"
        )
        print("  get       wifi|apikey|tickers|messages|locations|mode  read a setting")
        print("  reload                            force immediate stock refresh")
        print("  reset                             clear NVS and revert to defaults")
        sys.exit(1)
    COMMANDS[sys.argv[1]](sys.argv[2:])
