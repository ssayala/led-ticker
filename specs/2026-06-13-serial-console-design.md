# Serial Console — Design

**Date:** 2026-06-13
**Status:** Approved (design), pending implementation plan
**Scope:** A dev/test-only serial command console that drives every device feature
over USB, mirroring the BLE control plane. No new end-user-facing surface.

## Problem

Every setting today is written over BLE: WiFi creds, Finnhub key, tickers,
locations, mode mask, sign text, timer, power, display brightness/scroll,
timezone, and pin-enforce. There is no non-BLE way to provision or exercise the
device. This blocks two things:

1. Quick local testing over the USB serial monitor we already keep open.
2. Any testing at all in Wokwi, where **BLE is not simulated** but serial is — so
   the fetch path (stocks/weather/clock) and sign mode currently can't be reached
   in the simulator without the `#ifdef` compile-time seed, which violates the
   repo's "no build-time secrets" rule.

## Goal / Non-goals

**Goal:** type a command in the serial monitor and have it take effect exactly as
the equivalent BLE write would — same NVS writes, same WiFi/fetch/sign behavior.

**Non-goals:**
- Not a replacement for BLE. iOS app and `tools/led.py` keep working unchanged.
- Not a user-facing provisioning flow (no captive portal / web UI).
- No new feature behavior — only a new *input path* onto existing behavior.

## Approach

A line-based serial console is a thin text front-end onto the **existing
deferred-apply machinery**. It reuses, rather than duplicates, every
`applyPending*()` function.

Data flow, compared to BLE:

```
BLE:    led.py --> characteristic onWrite --> pending* buffer + *UpdatePending flag
                                                        |
Serial: monitor --> console parser ---------------------+--> loop() applyPending*()
```

The console writes the **same** `pending*` buffers and sets the **same**
`*UpdatePending` flags the BLE callbacks use. `loop()` already dispatches those
flags (`if (tzUpdatePending) applyPendingTimezone();` etc.), so no apply logic is
added or changed.

This honors the codebase rule: *copy payload to a pending buffer + set the flag;
`loop()` applies*. The console runs in `loop()` on Core 1, so setting flags and
reading display globals is safe; it never calls `neopixelWrite()`.

### Rejected alternatives

- **WiFi captive portal / web form** — needs a web server, HTML, and captive DNS.
  Too heavy for a dev convenience; not "simple."
- **Compile-time `#ifdef` seed** — violates the no-build-secrets rule and can't
  drive features interactively.

## Components

### 1. Non-blocking line reader (in `loop()`)

`pollSerialConsole()` is called once per `loop()` iteration. It drains
`Serial.available()`, accumulating into a static line buffer until `\n` (or `\r`).
No `delay()`; cooperative. On overflow the line is dropped with an error and the
buffer reset. Empty lines are ignored.

- Line buffer: a fixed `char[288]`. Sized to the largest payload — the locations
  CSV at `BLE_LOCS_BUF_LEN` = `MAX_LOCATIONS(5) × (MAX_LOCATION_LEN(48)+1)` = 245
  bytes — plus the verb word and separators, rounded up. (Tickers are 170,
  `wifi <ssid> <pass>` ~134.) Define it off `BLE_LOCS_BUF_LEN` so it tracks the
  config rather than a magic number.

### 2. Pure tokenizer — `parseConsoleLine(const char* in, ConsoleCmd* out)`

Splits a line into `verb` and the remaining `arg` string (single space delimiter;
`arg` is everything after the first space, untrimmed of internal spaces so
multi-word payloads like SSIDs survive). Pure, no globals, no side effects —
**this is the unit-testable core.** Returns the matched verb as an enum plus a
pointer/copy of the argument span.

### 3. Dispatch — `dispatchConsoleCmd(const ConsoleCmd&)`

Maps a parsed verb to the corresponding pending buffer + flag (or to `pendingCmd`
for command-style verbs). Prints `ok` or a per-verb usage/error line. Verb table:

| Command | Action |
|---|---|
| `wifi <ssid> <pass>` | compose `ssid\|pass` into `pendingWifiStr`, set `wifiUpdatePending` |
| `apikey <key>` | `pendingApiKey`, `apiKeyUpdatePending` |
| `tickers AAPL,MSFT,…` | `pendingTickerStr`, `tickerUpdatePending` |
| `locations <lat,lon,label;…>` | `pendingLocsStr`, `locsUpdatePending` |
| `mode all\|none\|stocks,weather,clock` | `pendingModeStr`, `modeUpdatePending` |
| `sign <text>` | `pendingStatusStr`, `statusUpdatePending` |
| `power on\|off` | `pendingPowerStr`, `powerUpdatePending` |
| `bright <0-15>` | compose `<n>\|<current scroll>` into `pendingDisplayCfgStr`, set flag |
| `scroll <ms>` | compose `<current bright>\|<ms>`, set flag |
| `tz <posix-tz>` | `pendingTzStr`, `tzUpdatePending` |
| `timer <min>\|cancel` | copy into `pendingCmd`, set `cmdPending` |
| `pin-enforce on\|off` | copy into `pendingCmd`, set `cmdPending` |
| `reload`, `reset` | copy into `pendingCmd`, set `cmdPending` |
| `info` | print current state (read-only, see below) |
| `help` | print the verb list |

Notes baked into the table:
- **`wifi`** — the WiFi pending buffer uses a `|` separator (`ssid|pass`); the
  console assembles it from the two space-separated args.
- **`bright`/`scroll`** — `applyPendingDisplayCfg()` expects a combined
  `bright|scroll` payload, so each single-knob command composes the other half
  from the current `displayBrightness` / `scrollSpeedMs` globals (readable in
  `loop()`).
- **`timer`/`pin-enforce`/`reset`/`reload`** route through `pendingCmd` (16-byte
  buffer). `pin-enforce off` is the longest at 15 chars and fits; the dispatcher
  rejects anything `>= 16` with an error, matching the BLE onWrite guard.

### 4. `info` read-back

The one capability BLE write characteristics don't all offer: print current state
to serial — firmware version, WiFi status + IP, active mode mask, PIN,
pin-enforce on/off, brightness/scroll, timer state. Useful for "did that take
effect?" Read-only; touches no pending state.

## Auth & security

The console **bypasses the PIN gate and the 10 s command cooldown** — it sets
`cmdPending` / `*UpdatePending` directly without an `isConnAuthed()` check.

This is intentional and not a security regression: physical USB access already
allows reflashing the entire chip, so the console grants no privilege an attacker
with the cable wouldn't already have. Because of this, the console is compiled in
unconditionally — no build flag, no runtime toggle needed.

## Error handling

- Unknown verb → `error: unknown command 'xyz' (try 'help')`.
- Missing/!malformed args → per-verb usage line; pending flag is **not** set.
- Line over buffer length → dropped with `error: line too long`, buffer reset.
- Empty line → silently ignored.
- Rapid repeated commands → last-write-wins on the shared pending buffer, exactly
  as rapid BLE writes already behave (benign).
- Argument-level validation (bad timezone, bad mode token, bad timer arg) is left
  to the existing `applyPending*()` functions, which already log and reject — the
  console deliberately does not re-implement it.

## Testing

- **Unit (host-native):** `parseConsoleLine()` is pure and the primary test
  target — verb matching, arg splitting, multi-word args (SSID with a space),
  empty/whitespace lines, overflow boundary. This requires adding a PlatformIO
  `env:native` and a `firmware/test/` dir (none exist today). Proposed as part of
  this work since CLAUDE.md asks for unit tests on logic.
- **On-device / Wokwi (manual):** flash (or run the sim), type each verb, confirm
  via `info` and the display. In Wokwi this is the *first* way to exercise the
  WiFi + fetch path without BLE — seed `wifi Wokwi-GUEST` + `apikey <key>` and
  watch `[fetch]` lines.

## Files touched

- `firmware/src/main.cpp` — add `pollSerialConsole()`, `parseConsoleLine()`,
  `dispatchConsoleCmd()`, and one call in `loop()`. (~150 lines, self-contained
  section near the other input-handling code.)
- `firmware/platformio.ini` — add `env:native` for the parser unit test.
- `firmware/test/` — new host test for `parseConsoleLine()`.
- Docs: a short "Serial console" section in `firmware/FIRMWARE_GUIDE.md` and a
  pointer from `WOKWI.md` (serial is the no-BLE path in the sim).

## Open questions

None blocking. If `main.cpp` (2614 lines) feels too big to keep growing, the
console is a clean candidate to extract into `serial_console.{h,cpp}` — but that's
an optional tidy, not required for this change.
