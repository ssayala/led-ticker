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
| Display | `beb5483e-36e1-4688-b7f5-ea07361b26b3` | read/write |
| Timezone | `beb5483e-36e1-4688-b7f5-ea07361b26b4` | read/write |

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
- There is no preset library on the device; preset chips are managed client-side (e.g. a companion app's local list).

### Locations
Pipe-separated `lat,lon,label` triplets — `47.61,-122.33,Seattle|47.67,-122.12,Redmond`. Up to 5 entries, 47 chars each. The **client** geocodes place names to coordinates — companion apps use the phone's location APIs; CLI users look up `lat`/`lon` themselves — so the device never contacts a geocoding service. `lat`/`lon` are decimal degrees; `label` is the text shown on the matrix (≤23 chars, may contain commas — only the first two commas are delimiters). Malformed or out-of-range entries are skipped.

### Command
Write-only.
- `reload` — force an immediate stock + weather fetch.
- `reset` — factory reset: wipe NVS and reboot. The wipe is deferred behind a 9-second single-digit countdown on the matrix (the BOOT-button hold shows the same digits) — **power-cycle the device during the countdown to abort**, since nothing is written until it finishes. Reverts to `config.h` defaults (clears WiFi, API key, any active sign, and the PIN — full reconfiguration needed after; a new PIN is generated and shown in setup mode). The connection drops when the device restarts; the write is ACKed first.
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

### Display
Write `brightness|scroll_ms` — e.g. `4|70`. Brightness is the MAX7219 intensity, 0–15; scroll speed is ms per scroll step, 20–500 (lower = faster). Out-of-range values are clamped, non-numeric or separator-less payloads are ignored. Reads return current values in the same format.

- Persisted to NVS; survives reboot. Factory reset reverts both to the `config.h` defaults (brightness 2, scroll 70).
- Applied immediately — brightness everywhere, scroll speed including an in-flight scroll. Setup mode keeps its own fixed slower scroll until configuration completes.
- Static-sign "breathing" dips up to 4 intensity levels below the configured brightness and recovers, so the setting is the brightest the sign ever gets; the dip clamps at 0 (a brightness-0 sign holds steady).
- Older firmwares won't expose this characteristic.

### Timezone
Write a POSIX TZ string — e.g. `PST8PDT,M3.2.0,M11.1.0` (US Pacific) or `IST-5:30` (India). Up to 63 bytes; must start with a letter. Reads return the current string.

- Persisted to NVS; applied to the display clock immediately. Factory reset reverts to the `config.h` default (US Pacific).
- NYSE market hours are computed in Eastern Time from UTC, independent of this setting.
- Companion apps map a human-readable timezone picker to these strings; the raw format is only needed from the CLI.
- Older firmwares won't expose this characteristic.

### Auth

A BLE connection is "authenticated" (allowed to write to non-Auth characteristics) once either of these happens:

1. **Bonded link:** The device advertises passkey-entry pairing (`bond=true, MITM=true, SC=true`, IO cap `DISPLAY_ONLY`) and asks the central to pair on every fresh connection. The phone shows its native pairing dialog, and the user types the 6-digit PIN from the LED matrix.
2. **PIN write:** The client writes the 6-digit PIN to the Auth characteristic (`…26b2`, write-only). On match, the connection is authenticated for its lifetime. This is the fallback for clients that decline pairing (e.g., Python CLI on Linux).

Reads always work — auth gates writes only.

> [!NOTE]
> Implementation details regarding connection slots (`AuthSlot`), rate-limiting (5 wrong PINs → 5-second silent lockout), concurrent connection limits, and NVS PIN persistence can be found in the [Firmware Guide: Security & Auth](firmware/FIRMWARE_GUIDE.md#7-security-authentication--rate-limiting).


## Cooldown

Writes that trigger upstream network activity — Tickers, Locations, `Command=reload`, `Command=reset` — share a 10-second cooldown to prevent hammering the APIs if a client retries. Other writes (WiFi, API Key, Mode, Status, Power, Display, Timezone, Auth, `Command=pin-enforce`) are not gated.
