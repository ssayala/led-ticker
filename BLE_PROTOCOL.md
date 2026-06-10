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
| Auth | `beb5483e-36e1-4688-b7f5-ea07361b26b2` | write |

UUID `...26aa` was once a "Messages" characteristic and is **not** registered in the current firmware. Reads against it fail at the GATT layer. Don't reuse the UUID.

## Payload formats

### Tickers
Comma-separated symbols. Example: `AAPL,MSFT,GOOGL`. Up to 10 symbols, 15 chars each.

### Mode
A single category (`stocks`, `weather`, `clock`), `all`, `none`, or a comma-separated subset (e.g. `stocks,weather`). Persisted to NVS. The device round-robins through enabled categories. When `clock` is the *only* enabled category the display shows a steady `H:MM`; mixed with others it scrolls `H:MM AM/PM`.

`none` is sign-only mode: no ambient rotation; between signs the display sits on the dim bouncing-pixel idle state. Reads return `"none"`.

Requesting a subset whose prereqs are missing (e.g. stocks without WiFi/API key) diverts the display to **setup mode** — the device scrolls its BLE name until the missing pieces are configured (or the 60s inactivity timer falls into the chosen mode).

The Status characteristic is **orthogonal** to the mode mask: an active sign overrides ambient until cleared or expired. When a sign clears on a device whose prereqs are still unmet, the display drops to **idle** (a single dim bouncing pixel) rather than the setup-name scroll — the user already found the device, so re-scrolling the name would be noise. Mode reads still return the configured mask; idle is an internal display state, not a settable mode.

### Status
Write `text|N` to set, empty payload to clear.

- `N` is unsigned integer *seconds* until auto-clear; `N=0` means indefinite (until cleared).
- Text up to 96 bytes. Short text (≤5 chars) displays steady; longer text scrolls.
- Reads return `text|M` where `M` is seconds remaining (`0` if indefinite), or empty string when no status is active.
- **RAM-only** — a power cycle clears any active sign and resumes ambient.
- Timed signs use the MCU's monotonic `millis()` counter, so they work without WiFi/NTP.
- There is no preset library on the device; preset chips are managed client-side (e.g. the iOS app's local list).

### Locations
Pipe-separated zip codes or "City, State" strings — `Seattle, WA|98052|Redmond, WA`. Up to 5 entries, 39 chars each. The device geocodes each via Open-Meteo on first fetch and caches the result; a trailing `, XX` is used as an admin1/country-code filter to disambiguate duplicate city names.

### Command
Write-only.
- `reload` — force an immediate stock + weather fetch.
- `reset` — clear NVS, revert to `config.h` defaults (also clears WiFi, API key, any active sign, and the PIN — full reconfiguration needed after; a new PIN is generated and shown in setup mode).
- `pin-enforce on` / `pin-enforce off` — toggle the Auth PIN gate on writes. **On is the default** on a fresh flash.
- `timer <minutes>` — countdown sign (1–99 whole minutes): live `MM:SS`, then a randomly-chosen end animation (fireworks / sonar pulse / sparkle), then ambient resumes. No cooldown — feels immediate, like a sign. Mutually exclusive with the text sign: each cancels the other. RAM-only and fire-and-forget — reads don't report timer state, so clients track the countdown locally.
- `timer cancel` — stop a running countdown and resume ambient immediately.

### WiFi
`SSID|password` — split on the first `|`. Passwords may contain `|`; SSIDs may not. Updates credentials, saves to NVS, reconnects immediately. Reads return the SSID only.

### API Key
Plain string — Finnhub API key. Saved to NVS, triggers an immediate stock fetch.

### Version
Read-only. Returns the firmware version (e.g. `0.1.0`) — the `FW_VERSION` define in `firmware/src/version.h`. Older firmwares won't expose this characteristic; treat its absence as "unknown / pre-0.1.0".

### Power
Write `"on"` or `"off"` (case-insensitive, whitespace-tolerant) to toggle the display. Reads return current state. Unknown payloads are ignored (no GATT error; state unchanged).

- Power is a third orthogonal layer alongside Mode and Status: `"off"` makes the device visually inert (matrix dark, NeoPixel dark, signs suppressed, periodic fetches paused — explicit-refresh writes like ticker updates or `reload` still apply) without touching `enabledMask` or any persisted state. `"on"` resumes the saved ambient mode and triggers an immediate fetch.
- Sign writes while off are accepted and stored but not rendered; on wake, an un-expired sign reappears. Timed signs keep counting down while off.
- **RAM-only — not persisted.** A power cycle returns the device to `"on"`. Older firmwares won't expose it.

### Auth
A BLE connection is "authenticated" (allowed to write to non-Auth characteristics) once either of these happens:

1. **Bonded link.** The device advertises passkey-entry pairing (`bond=true, MITM=true, SC=true`, IO cap `DISPLAY_ONLY`) and asks the central to pair on every fresh connection; iOS shows its native "Bluetooth Pairing Request" dialog and the user types the 6-digit PIN from the LED matrix. `onPassKeyRequest` returns `nvsPin` — the same PIN as the Auth-write path. Bonded reconnects are silent and encrypted from the start. Just-Works pairing is deliberately *not* used — it would let any iPhone in range bond silently.
2. **PIN write.** The client writes the 6-digit PIN to the Auth characteristic (`…26b2`, write-only). On match the connection is authenticated for its lifetime. Fallback for clients that decline pairing (Python CLI on Linux, etc.).

Reads always work — auth gates writes only.

- The PIN is generated on first boot and persisted to NVS. Recovery channels: scrolled on the matrix in setup mode, printed to serial at every boot. Factory reset (`Command=reset` or a 10s BOOT-button hold) rotates it.
- Enforcement is **on by default**. Both shipped clients pass the gate (iOS via bonding, CLI via Auth), so there's no reason to disable it; `Command=pin-enforce off` exists as an escape hatch (e.g. probing from a generic GATT browser).
- Rate limit: 5 wrong PINs from one connection → 5-second silent lockout on that connection's Auth slot. The connection stays open; only further Auth writes are dropped.
- Maximum 4 concurrent connections are tracked. A 5th connection works but its auth slot logs "FULL" and its writes are rejected while enforcement is on.
- Race window: writes that arrive between connect and bond/auth completion are rejected as unauthed. Bonded reconnects have no window — the link is encrypted before the connection event fires.

## Cooldown

Writes that trigger upstream network activity — Tickers, Locations, `Command=reload`, `Command=reset` — share a 10-second cooldown to prevent hammering the APIs if a client retries. Other writes (WiFi, API Key, Mode, Status, Auth, `Command=pin-enforce`) are not gated.
