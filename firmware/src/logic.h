#pragma once

#include <stdint.h>
#include <time.h>

// Pure logic extracted from main.cpp: no Arduino / display / NVS deps and no
// globals, so it is unit-tested on the host (test/test_logic). Keep it that
// way — anything touching hardware or shared state belongs in main.cpp.

// ---------------------------------------------------------------------------
// Display category bitmask
// ---------------------------------------------------------------------------
// 0x02 is reserved (the tombstoned BIT_MESSAGES); legacy NVS masks are stripped
// via `& MASK_ALL` on load in main.cpp.
#define BIT_STOCKS 0x01
#define BIT_WEATHER 0x04
#define BIT_CLOCK 0x08
#define MASK_ALL (BIT_STOCKS | BIT_WEATHER | BIT_CLOCK)

// Parser sentinel for the explicit "none" mode (sign-only with idle pixel
// between signs). Distinct from 0 (which parseModePayload returns for invalid
// input) so applyPendingMode can tell them apart. Never stored in enabledMask —
// that holds 0 when "none" is the active selection.
#define MASK_NONE_REQUEST 0x80

// Parse a Mode-characteristic payload into an enabled-category mask:
//   "all"  -> MASK_ALL
//   "none" -> MASK_NONE_REQUEST
//   CSV of stocks|weather|clock (surrounding whitespace tolerated) -> OR of bits
//   anything malformed (unknown token, empty) -> 0
uint8_t parseModePayload(const char* in);

// ---------------------------------------------------------------------------
// Weather locations
// ---------------------------------------------------------------------------
#define MAX_LOCATION_LEN 48  // "lat,lon,label" entry from the client
#define MAX_LOC_NAME_LEN 24  // display label shown on the matrix

struct ResolvedLocation {
  bool ok;
  float lat;
  float lon;
  char name[MAX_LOC_NAME_LEN];
};

// Parse a "lat,lon,label" entry into coordinates + display label. The client
// (iOS app / CLI) does the geocoding; the device never resolves place names. No
// network. Returns false on a malformed entry or out-of-range coordinates,
// leaving out.ok false so the fetch skips it.
bool parseLocation(const char* entry, ResolvedLocation& out);

// ---------------------------------------------------------------------------
// US Eastern DST
// ---------------------------------------------------------------------------
// True if US Eastern Time observes DST at the given UTC time (second Sunday of
// March 02:00 EST -> first Sunday of November 02:00 EDT). `u` must be a UTC tm;
// the transition is resolved to the hour.
bool usEasternInDst(const struct tm& u);
