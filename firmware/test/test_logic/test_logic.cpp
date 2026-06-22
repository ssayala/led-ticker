#include <unity.h>
#include <string.h>
#include <time.h>

#include "logic.h"

// ---------------------------------------------------------------------------
// parseModePayload
// ---------------------------------------------------------------------------
void test_mode_all(void) {
  TEST_ASSERT_EQUAL_UINT8(MASK_ALL, parseModePayload("all"));
}
void test_mode_none(void) {
  TEST_ASSERT_EQUAL_UINT8(MASK_NONE_REQUEST, parseModePayload("none"));
}
void test_mode_single(void) {
  TEST_ASSERT_EQUAL_UINT8(BIT_STOCKS, parseModePayload("stocks"));
  TEST_ASSERT_EQUAL_UINT8(BIT_WEATHER, parseModePayload("weather"));
  TEST_ASSERT_EQUAL_UINT8(BIT_CLOCK, parseModePayload("clock"));
}
void test_mode_csv(void) {
  TEST_ASSERT_EQUAL_UINT8(BIT_STOCKS | BIT_WEATHER,
                          parseModePayload("stocks,weather"));
  TEST_ASSERT_EQUAL_UINT8(MASK_ALL, parseModePayload("stocks,weather,clock"));
}
void test_mode_csv_with_spaces(void) {
  TEST_ASSERT_EQUAL_UINT8(BIT_STOCKS | BIT_CLOCK,
                          parseModePayload("stocks, clock"));
  TEST_ASSERT_EQUAL_UINT8(BIT_WEATHER, parseModePayload("  weather  "));
}
void test_mode_unknown_token_rejects_whole(void) {
  TEST_ASSERT_EQUAL_UINT8(0, parseModePayload("bogus"));
  TEST_ASSERT_EQUAL_UINT8(0, parseModePayload("stocks,bogus"));
}
void test_mode_empty(void) {
  TEST_ASSERT_EQUAL_UINT8(0, parseModePayload(""));
}

// ---------------------------------------------------------------------------
// parseLocation
// ---------------------------------------------------------------------------
void test_loc_valid(void) {
  ResolvedLocation r;
  TEST_ASSERT_TRUE(parseLocation("37.7749,-122.4194,SF", r));
  TEST_ASSERT_TRUE(r.ok);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 37.7749f, r.lat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -122.4194f, r.lon);
  TEST_ASSERT_EQUAL_STRING("SF", r.name);
}
void test_loc_label_keeps_spaces(void) {
  ResolvedLocation r;
  TEST_ASSERT_TRUE(parseLocation("51.5,-0.12,New York City", r));
  TEST_ASSERT_EQUAL_STRING("New York City", r.name);
}
void test_loc_missing_second_comma(void) {
  ResolvedLocation r;
  TEST_ASSERT_FALSE(parseLocation("37.77,-122.42", r));
  TEST_ASSERT_FALSE(r.ok);
}
void test_loc_no_comma(void) {
  ResolvedLocation r;
  TEST_ASSERT_FALSE(parseLocation("garbage", r));
}
void test_loc_empty_label(void) {
  ResolvedLocation r;
  TEST_ASSERT_FALSE(parseLocation("37.77,-122.42,", r));
}
void test_loc_non_numeric(void) {
  ResolvedLocation r;
  TEST_ASSERT_FALSE(parseLocation("abc,0,X", r));
}
void test_loc_out_of_range(void) {
  ResolvedLocation r;
  TEST_ASSERT_FALSE(parseLocation("91.0,0,X", r));    // lat > 90
  TEST_ASSERT_FALSE(parseLocation("0,181,X", r));     // lon > 180
  TEST_ASSERT_FALSE(parseLocation("-90.1,0,X", r));   // lat < -90
}
void test_loc_label_truncated(void) {
  ResolvedLocation r;
  // 30-char label is clamped to MAX_LOC_NAME_LEN-1 (23) chars.
  TEST_ASSERT_TRUE(parseLocation("0,0,ABCDEFGHIJKLMNOPQRSTUVWXYZ0123", r));
  TEST_ASSERT_EQUAL_UINT(MAX_LOC_NAME_LEN - 1, strlen(r.name));
}

// ---------------------------------------------------------------------------
// usEasternInDst — build a proper UTC tm (with tm_wday) via timegm.
// ---------------------------------------------------------------------------
static struct tm utc(int year, int mon1, int mday, int hour, int min) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = mon1 - 1;
  t.tm_mday = mday;
  t.tm_hour = hour;
  t.tm_min = min;
  time_t e = timegm(&t);
  struct tm out;
  gmtime_r(&e, &out);
  return out;
}

void test_dst_winter_false(void) {
  TEST_ASSERT_FALSE(usEasternInDst(utc(2026, 1, 15, 12, 0)));
  TEST_ASSERT_FALSE(usEasternInDst(utc(2026, 12, 15, 12, 0)));
}
void test_dst_summer_true(void) {
  TEST_ASSERT_TRUE(usEasternInDst(utc(2026, 7, 15, 12, 0)));
}
void test_dst_march_start_boundary(void) {
  // 2026: DST starts Sun Mar 8 at 02:00 EST == 07:00 UTC.
  TEST_ASSERT_FALSE(usEasternInDst(utc(2026, 3, 7, 23, 0)));   // Sat, day before
  TEST_ASSERT_FALSE(usEasternInDst(utc(2026, 3, 8, 6, 59)));   // before 07:00 UTC
  TEST_ASSERT_TRUE(usEasternInDst(utc(2026, 3, 8, 7, 0)));     // at the switch
  TEST_ASSERT_TRUE(usEasternInDst(utc(2026, 3, 9, 0, 0)));     // Mon, day after
}
void test_dst_november_end_boundary(void) {
  // 2026: DST ends Sun Nov 1 at 02:00 EDT == 06:00 UTC.
  TEST_ASSERT_TRUE(usEasternInDst(utc(2026, 10, 31, 23, 0)));  // day before
  TEST_ASSERT_TRUE(usEasternInDst(utc(2026, 11, 1, 5, 59)));   // before 06:00 UTC
  TEST_ASSERT_FALSE(usEasternInDst(utc(2026, 11, 1, 6, 0)));   // at the switch
  TEST_ASSERT_FALSE(usEasternInDst(utc(2026, 11, 2, 0, 0)));   // day after
}

// ---------------------------------------------------------------------------
// nextBit — cyclic rotation order
// ---------------------------------------------------------------------------
void test_next_bit_cycle(void) {
  TEST_ASSERT_EQUAL_UINT8(BIT_WEATHER, nextBit(BIT_STOCKS));
  TEST_ASSERT_EQUAL_UINT8(BIT_CLOCK, nextBit(BIT_WEATHER));
  TEST_ASSERT_EQUAL_UINT8(BIT_STOCKS, nextBit(BIT_CLOCK));
}

// ---------------------------------------------------------------------------
// formatModeName — inverse of parseModePayload
// ---------------------------------------------------------------------------
void test_format_setup(void) {
  char buf[64];
  formatModeName(buf, sizeof(buf), BIT_STOCKS, true);  // mask ignored when setup
  TEST_ASSERT_EQUAL_STRING("setup", buf);
}
void test_format_named_masks(void) {
  char buf[64];
  formatModeName(buf, sizeof(buf), 0, false);
  TEST_ASSERT_EQUAL_STRING("none", buf);
  formatModeName(buf, sizeof(buf), MASK_ALL, false);
  TEST_ASSERT_EQUAL_STRING("all", buf);
}
void test_format_subset_order(void) {
  char buf[64];
  formatModeName(buf, sizeof(buf), BIT_STOCKS | BIT_CLOCK, false);
  TEST_ASSERT_EQUAL_STRING("stocks,clock", buf);
}
void test_format_parse_roundtrip(void) {
  // Every non-empty subset survives format -> parse unchanged. The empty mask
  // formats to "none", which parses back to the MASK_NONE_REQUEST sentinel (not
  // 0) by design so applyPendingMode can tell "none" from invalid input.
  for (uint8_t mask = 0; mask <= MASK_ALL; mask++) {
    if (mask & ~MASK_ALL) continue;  // skip masks with reserved bits set
    char buf[64];
    formatModeName(buf, sizeof(buf), mask, false);
    uint8_t expected = (mask == 0) ? MASK_NONE_REQUEST : mask;
    TEST_ASSERT_EQUAL_UINT8(expected, parseModePayload(buf));
  }
}

// ---------------------------------------------------------------------------
// isMarketOpenAt — weekday 09:30-16:00 US Eastern, DST-aware
// ---------------------------------------------------------------------------
static time_t utcEpoch(int year, int mon1, int mday, int hour, int min) {
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon = mon1 - 1;
  t.tm_mday = mday;
  t.tm_hour = hour;
  t.tm_min = min;
  return timegm(&t);
}

void test_market_weekday_open_dst(void) {
  // Wed Jul 15 2026, 14:00 UTC == 10:00 EDT — open.
  TEST_ASSERT_TRUE(isMarketOpenAt(utcEpoch(2026, 7, 15, 14, 0)));
}
void test_market_open_close_edges_dst(void) {
  // EDT = UTC-4: open 13:30 UTC, close 20:00 UTC.
  TEST_ASSERT_FALSE(isMarketOpenAt(utcEpoch(2026, 7, 15, 13, 29)));
  TEST_ASSERT_TRUE(isMarketOpenAt(utcEpoch(2026, 7, 15, 13, 30)));
  TEST_ASSERT_TRUE(isMarketOpenAt(utcEpoch(2026, 7, 15, 19, 59)));
  TEST_ASSERT_FALSE(isMarketOpenAt(utcEpoch(2026, 7, 15, 20, 0)));
}
void test_market_open_edge_standard_time(void) {
  // EST = UTC-5: open 14:30 UTC. Wed Jan 14 2026.
  TEST_ASSERT_FALSE(isMarketOpenAt(utcEpoch(2026, 1, 14, 14, 29)));
  TEST_ASSERT_TRUE(isMarketOpenAt(utcEpoch(2026, 1, 14, 14, 30)));
}
void test_market_weekend_closed(void) {
  // Sat Jul 18 / Sun Jul 19 2026 midday — closed regardless of hour.
  TEST_ASSERT_FALSE(isMarketOpenAt(utcEpoch(2026, 7, 18, 15, 0)));
  TEST_ASSERT_FALSE(isMarketOpenAt(utcEpoch(2026, 7, 19, 15, 0)));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_mode_all);
  RUN_TEST(test_mode_none);
  RUN_TEST(test_mode_single);
  RUN_TEST(test_mode_csv);
  RUN_TEST(test_mode_csv_with_spaces);
  RUN_TEST(test_mode_unknown_token_rejects_whole);
  RUN_TEST(test_mode_empty);
  RUN_TEST(test_loc_valid);
  RUN_TEST(test_loc_label_keeps_spaces);
  RUN_TEST(test_loc_missing_second_comma);
  RUN_TEST(test_loc_no_comma);
  RUN_TEST(test_loc_empty_label);
  RUN_TEST(test_loc_non_numeric);
  RUN_TEST(test_loc_out_of_range);
  RUN_TEST(test_loc_label_truncated);
  RUN_TEST(test_dst_winter_false);
  RUN_TEST(test_dst_summer_true);
  RUN_TEST(test_dst_march_start_boundary);
  RUN_TEST(test_dst_november_end_boundary);
  RUN_TEST(test_next_bit_cycle);
  RUN_TEST(test_format_setup);
  RUN_TEST(test_format_named_masks);
  RUN_TEST(test_format_subset_order);
  RUN_TEST(test_format_parse_roundtrip);
  RUN_TEST(test_market_weekday_open_dst);
  RUN_TEST(test_market_open_close_edges_dst);
  RUN_TEST(test_market_open_edge_standard_time);
  RUN_TEST(test_market_weekend_closed);
  return UNITY_END();
}
