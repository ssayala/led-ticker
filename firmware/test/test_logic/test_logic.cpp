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
  return UNITY_END();
}
