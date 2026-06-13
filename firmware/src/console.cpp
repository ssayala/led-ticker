#include "console.h"
#include <string.h>

namespace {
struct VerbEntry {
  const char* name;
  ConsoleVerb verb;
};
const VerbEntry kVerbs[] = {
    {"wifi", CONSOLE_WIFI},        {"apikey", CONSOLE_APIKEY},
    {"tickers", CONSOLE_TICKERS},  {"locations", CONSOLE_LOCATIONS},
    {"mode", CONSOLE_MODE},        {"sign", CONSOLE_SIGN},
    {"power", CONSOLE_POWER},      {"bright", CONSOLE_BRIGHT},
    {"scroll", CONSOLE_SCROLL},    {"tz", CONSOLE_TZ},
    {"timer", CONSOLE_TIMER},      {"pin-enforce", CONSOLE_PINENFORCE},
    {"reload", CONSOLE_RELOAD},    {"reset", CONSOLE_RESET},
    {"info", CONSOLE_INFO},        {"help", CONSOLE_HELP},
};
bool isSpace(char c) { return c == ' ' || c == '\t'; }
}  // namespace

ConsoleCmd parseConsoleLine(const char* line) {
  ConsoleCmd cmd = {CONSOLE_NONE, ""};
  if (!line) return cmd;

  while (isSpace(*line)) line++;       // skip leading whitespace
  if (*line == '\0') {                 // blank line -> CONSOLE_NONE
    cmd.arg = line;                    // points at the NUL inside the input
    return cmd;
  }

  const char* verbStart = line;
  const char* p = line;
  while (*p && !isSpace(*p)) p++;      // verb token = up to next whitespace
  size_t verbLen = (size_t)(p - verbStart);

  const char* arg = p;
  while (isSpace(*arg)) arg++;         // arg = first non-space after the verb
  cmd.arg = arg;

  for (const VerbEntry& e : kVerbs) {
    if (strlen(e.name) == verbLen && strncmp(verbStart, e.name, verbLen) == 0) {
      cmd.verb = e.verb;
      return cmd;
    }
  }
  cmd.verb = CONSOLE_UNKNOWN;
  return cmd;
}
