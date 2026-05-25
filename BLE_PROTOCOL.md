# BLE Protocol

The device advertises as `LED-Ticker-XXXX`, where `XXXX` is the low 2 bytes of the chip MAC (so multiple boards on the same bench are distinguishable). All characteristics except `Command` are read/write — reads return current state. `Command` is write-only.

Writes are deferred: the BLE callback copies the payload and a main-loop pass applies it. Writes return immediately even when the change triggers network, display, or NVS work — clients should not assume the effect is observable synchronously.

## Service

`4fafc201-1fb5-459e-8fcc-c5c9c331914b`

## Characteristics

| Characteristic | UUID | Access |
|---|---|---|
| Tickers | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | read/write |
| Mode | `beb5483e-36e1-4688-b7f5-ea07361b26a9` | read/write |
| Command | `beb5483e-36e1-4688-b7f5-ea07361b26ab` | write |
| WiFi | `beb5483e-36e1-4688-b7f5-ea07361b26ac` | read/write (read returns SSID only) |
| API Key | `beb5483e-36e1-4688-b7f5-ea07361b26ad` | read/write |
| Locations | `beb5483e-36e1-4688-b7f5-ea07361b26ae` | read/write |
| Status | `beb5483e-36e1-4688-b7f5-ea07361b26af` | read/write |
| Version | `beb5483e-36e1-4688-b7f5-ea07361b26b0` | read |
| Power | `beb5483e-36e1-4688-b7f5-ea07361b26b1` | read/write |

UUID `...26aa` was once a "Messages" characteristic and is **not** registered in the current firmware. Reads against it fail at the GATT layer. Don't reuse the UUID.

## Payload formats

### Tickers
Comma-separated symbols. Example: `AAPL,MSFT,GOOGL`. Up to 10 symbols, 15 chars each.

### Mode
A single category (`stocks`, `weather`, `clock`), the keyword `all`, the keyword `none`, or a comma-separated subset (e.g. `stocks,weather`). The mask is persisted to NVS and survives reboots. The device round-robins through enabled categories, one full pass per cycle. When `clock` is the *only* enabled category, the display switches to a steady "H:MM"; in any mix with other categories it scrolls as "H:MM AM/PM" alongside them.

`none` is sign-only mode: no ambient categories rotate, and between signs the display sits on the dim bouncing-pixel idle state. Persists across reboots like any other mask; reads return `"none"`.

Requesting a subset whose prereqs are missing (e.g. stocks without WiFi/API key) diverts the display to **setup mode** — the device scrolls its BLE name on a loop until the missing pieces are configured (or the 60s inactivity timer falls into the chosen mode).

The Status characteristic is **orthogonal** to the mode mask: an active sign overrides the ambient mode until cleared or expired. When a sign clears (manually or by expiry) on a device whose prereqs are still unmet, the display drops to **idle mode** instead of returning to the setup-name scroll — a single dim pixel bounces around the matrix as a quiet "alive" indicator (the user has already discovered the device by sending the sign, so re-scrolling the name would be noise). Mode reads continue to return the configured mask either way; `idle` is an internal display state, not a settable mode.

### Status
Write `text|N` to set, empty payload to clear.

- `N` is unsigned integer *seconds* until auto-clear; `N=0` means indefinite (until cleared).
- Text up to 96 bytes. Short text (≤5 chars) displays steady; longer text scrolls.
- Reads return `text|M` where `M` is seconds remaining (`0` if indefinite), or empty string when no status is active.
- Status is **RAM-only** — a power cycle clears any active sign and the device resumes its ambient mode.
- Timed signs use the MCU's monotonic `millis()` counter, so they work without WiFi/NTP. A 15-min timer on a no-WiFi device counts down and clears correctly.
- There is no preset library on the device; preset chips are managed client-side (e.g. the iOS app's local list).

### Locations
Pipe-separated zip codes or "City, State" strings — `Seattle, WA|98052|Redmond, WA`. Up to 5 entries, 39 chars each. The device geocodes each via Open-Meteo on first fetch and caches the result; a trailing `, XX` is used as an admin1/country-code filter to disambiguate duplicate city names.

### Command
Write-only.
- `reload` — force an immediate stock + weather fetch.
- `reset` — clear NVS, revert to `config.h` defaults (also clears WiFi, API key, and any active sign — full reconfiguration needed after).

### WiFi
`SSID|password` — split on the first `|`. Passwords may contain `|`; SSIDs may not. Updates credentials, saves to NVS, reconnects immediately. Reads return the SSID only.

### API Key
Plain string — Finnhub API key. Saved to NVS, triggers an immediate stock fetch.

### Version
Read-only. Returns the firmware version (e.g. `0.1.0`) compiled into the device — the `FW_VERSION` define in `src/version.h`. Clients can use this to detect old firmware and prompt the user to flash. Older firmwares (before this characteristic existed) won't expose it; treat its absence as "unknown / pre-0.1.0".

### Power
Write `"on"` or `"off"` (case-insensitive, whitespace-tolerant) to toggle the display on/off switch. Reads return `"on"` or `"off"` reflecting current state. Unknown payloads are ignored (no GATT error; device stays in its current state).

- Power is a third orthogonal layer alongside Mode and Status: writing `"off"` makes the device visually inert (matrix dark, onboard NeoPixel dark, signs suppressed, periodic fetches paused — writes that explicitly request a refresh, e.g. ticker updates or `reload`, still apply) without touching `enabledMask` or any persisted state. Writing `"on"` resumes the saved ambient mode and triggers an immediate fetch so content is fresh on wake.
- Sign writes while off are accepted and stored but not rendered; on wake, an un-expired sign reappears. Timed signs continue counting down while off (they are not paused).
- Power is **RAM-only — not persisted**. A power cycle returns the device to `"on"` with its saved ambient mode. Older firmwares (before this characteristic existed) won't expose it.

## Cooldown

Writes that trigger upstream network activity — Tickers, Locations, `Command=reload`, `Command=reset` — share a 10-second cooldown to prevent hammering the APIs if a client retries. Other writes (WiFi, API Key, Mode, Status) are not gated.
