#include <unity.h>
#include "console.h"

void test_null_input(void) {
  ConsoleCmd c = parseConsoleLine(nullptr);
  TEST_ASSERT_EQUAL(CONSOLE_NONE, c.verb);
  TEST_ASSERT_EQUAL_STRING("", c.arg);
}
void test_empty_line(void) {
  TEST_ASSERT_EQUAL(CONSOLE_NONE, parseConsoleLine("").verb);
}
void test_whitespace_only(void) {
  TEST_ASSERT_EQUAL(CONSOLE_NONE, parseConsoleLine("   ").verb);
}
void test_unknown_verb(void) {
  TEST_ASSERT_EQUAL(CONSOLE_UNKNOWN, parseConsoleLine("frobnicate x").verb);
}
void test_verb_no_arg(void) {
  ConsoleCmd c = parseConsoleLine("help");
  TEST_ASSERT_EQUAL(CONSOLE_HELP, c.verb);
  TEST_ASSERT_EQUAL_STRING("", c.arg);
}
void test_simple_arg(void) {
  ConsoleCmd c = parseConsoleLine("tz PST8PDT");
  TEST_ASSERT_EQUAL(CONSOLE_TZ, c.verb);
  TEST_ASSERT_EQUAL_STRING("PST8PDT", c.arg);
}
void test_arg_preserves_internal_spaces(void) {
  ConsoleCmd c = parseConsoleLine("wifi My Home pass123");
  TEST_ASSERT_EQUAL(CONSOLE_WIFI, c.verb);
  TEST_ASSERT_EQUAL_STRING("My Home pass123", c.arg);
}
void test_leading_whitespace(void) {
  ConsoleCmd c = parseConsoleLine("   mode all");
  TEST_ASSERT_EQUAL(CONSOLE_MODE, c.verb);
  TEST_ASSERT_EQUAL_STRING("all", c.arg);
}
void test_multiple_spaces_between(void) {
  ConsoleCmd c = parseConsoleLine("sign    hello");
  TEST_ASSERT_EQUAL(CONSOLE_SIGN, c.verb);
  TEST_ASSERT_EQUAL_STRING("hello", c.arg);
}
void test_hyphenated_verb(void) {
  ConsoleCmd c = parseConsoleLine("pin-enforce off");
  TEST_ASSERT_EQUAL(CONSOLE_PINENFORCE, c.verb);
  TEST_ASSERT_EQUAL_STRING("off", c.arg);
}
void test_prefix_not_matched(void) {
  TEST_ASSERT_EQUAL(CONSOLE_UNKNOWN, parseConsoleLine("sig hello").verb);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_null_input);
  RUN_TEST(test_empty_line);
  RUN_TEST(test_whitespace_only);
  RUN_TEST(test_unknown_verb);
  RUN_TEST(test_verb_no_arg);
  RUN_TEST(test_simple_arg);
  RUN_TEST(test_arg_preserves_internal_spaces);
  RUN_TEST(test_leading_whitespace);
  RUN_TEST(test_multiple_spaces_between);
  RUN_TEST(test_hyphenated_verb);
  RUN_TEST(test_prefix_not_matched);
  return UNITY_END();
}
