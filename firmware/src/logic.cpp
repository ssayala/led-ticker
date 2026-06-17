#include "logic.h"

#include <stdlib.h>
#include <string.h>

uint8_t parseModePayload(const char* in) {
  if (strcmp(in, "all") == 0) return MASK_ALL;
  if (strcmp(in, "none") == 0) return MASK_NONE_REQUEST;

  char buf[64];
  strncpy(buf, in, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  uint8_t mask = 0;
  char* tok = strtok(buf, ",");
  while (tok) {
    while (*tok == ' ' || *tok == '\t') tok++;
    char* end = tok + strlen(tok);
    while (end > tok && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                         end[-1] == '\r')) {
      *--end = '\0';
    }

    if (strcmp(tok, "stocks") == 0)
      mask |= BIT_STOCKS;
    else if (strcmp(tok, "weather") == 0)
      mask |= BIT_WEATHER;
    else if (strcmp(tok, "clock") == 0)
      mask |= BIT_CLOCK;
    else
      return 0;
    tok = strtok(nullptr, ",");
  }
  return mask;
}

bool parseLocation(const char* entry, ResolvedLocation& out) {
  out.ok = false;
  char buf[MAX_LOCATION_LEN];
  strncpy(buf, entry, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* c1 = strchr(buf, ',');
  if (!c1) return false;
  *c1 = '\0';
  char* c2 = strchr(c1 + 1, ',');
  if (!c2) return false;
  *c2 = '\0';
  const char* label = c2 + 1;
  if (label[0] == '\0') return false;

  char* end;
  float lat = strtof(buf, &end);
  if (end == buf) return false;
  float lon = strtof(c1 + 1, &end);
  if (end == c1 + 1) return false;
  if (lat < -90.0f || lat > 90.0f || lon < -180.0f || lon > 180.0f)
    return false;

  out.lat = lat;
  out.lon = lon;
  strncpy(out.name, label, MAX_LOC_NAME_LEN - 1);
  out.name[MAX_LOC_NAME_LEN - 1] = '\0';
  out.ok = true;
  return true;
}

bool usEasternInDst(const struct tm& u) {
  int m = u.tm_mon + 1;
  if (m < 3 || m > 11) return false;
  if (m > 3 && m < 11) return true;
  int wdayFirst = (u.tm_wday - (u.tm_mday - 1) % 7 + 7) % 7;
  if (m == 3) {
    int secondSunday = 1 + (7 - wdayFirst) % 7 + 7;
    if (u.tm_mday != secondSunday) return u.tm_mday > secondSunday;
    return u.tm_hour >= 7;  // 2:00 EST == 07:00 UTC
  }
  int firstSunday = 1 + (7 - wdayFirst) % 7;
  if (u.tm_mday != firstSunday) return u.tm_mday < firstSunday;
  return u.tm_hour < 6;  // 2:00 EDT == 06:00 UTC
}
