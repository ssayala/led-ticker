#pragma once

// Serial console command parser. Pure (no Arduino deps) so it is unit-tested
// on the host. The dispatcher that acts on these lives in main.cpp.

enum ConsoleVerb {
  CONSOLE_NONE,     // blank line
  CONSOLE_UNKNOWN,  // unrecognized verb
  CONSOLE_WIFI,
  CONSOLE_APIKEY,
  CONSOLE_TICKERS,
  CONSOLE_LOCATIONS,
  CONSOLE_MODE,
  CONSOLE_SIGN,
  CONSOLE_POWER,
  CONSOLE_BRIGHT,
  CONSOLE_SCROLL,
  CONSOLE_TZ,
  CONSOLE_TIMER,
  CONSOLE_PINENFORCE,
  CONSOLE_RELOAD,
  CONSOLE_RESET,
  CONSOLE_INFO,
  CONSOLE_HELP,
};

struct ConsoleCmd {
  ConsoleVerb verb;
  const char* arg;  // points into the input line (after the first space); "" if none
};

// Split a NUL-terminated line into verb + arg. Non-destructive: `arg` points
// into `line`, so `line` must outlive the returned struct. Leading whitespace
// is skipped; arg keeps internal spaces (so SSIDs/passwords survive).
ConsoleCmd parseConsoleLine(const char* line);
