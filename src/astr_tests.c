#include "arena.h"
#include "utest.h"

UTEST(astr, split_leading_sep) {
  int i = 0;
  for (astr_split(it, ",", astr(",,a,b"))) {
    i++;
  }
  ASSERT_EQ(i, 4);
}

UTEST(astr, split_trailing_sep) {
  int i = 0;
  for (astr_split(it, ",", astr("a,b,,"))) {
    i++;
  }
  ASSERT_EQ(i, 3);
}

UTEST(astr, split_all_sep) {
  int i = 0;
  for (astr_split(it, ",", astr(",,,"))) {
    i++;
  }
  ASSERT_EQ(i, 3);
}

UTEST(astr, split_no_sep) {
  int i = 0;
  for (astr_split(it, ",", astr("hello"))) {
    i++;
  }
  ASSERT_EQ(i, 1);
}

UTEST(astr, split_empty) {
  int i = 0;
  for (astr_split(it, ",", astr(""))) {
    i++;
  }
  ASSERT_EQ(i, 0);
}

UTEST(astr, split_empty_separator_returns_input) {
  int count = 0;
  for (astr_split(it, "", astr("hello"))) {
    count++;
    ASSERT_TRUE(astr_equals(it.token, astr("hello")));
    if (count > 1)
      break;
  }
  ASSERT_EQ(count, 1);
}

UTEST(astr, split_single_char) {
  int i = 0;
  for (astr_split(it, ",", astr("x"))) {
    i++;
  }
  ASSERT_EQ(i, 1);
}

UTEST(astr, split_multibyte_sep) {
  int i = 0;
  for (astr_split(it, "->", astr("a->b->->c"))) {
    i++;
  }
  ASSERT_EQ(i, 4);
}

UTEST(astr, split_by_char_leading_sep) {
  int i = 0;
  for (astr_split_by_char(it, ",", astr(",,a,b"))) {
    i++;
  }
  ASSERT_EQ(i, 2);
}

UTEST(astr, split_by_char_all_sep) {
  int i = 0;
  for (astr_split_by_char(it, ",", astr(",,,"))) {
    i++;
  }
  ASSERT_EQ(i, 0);
}

UTEST(astr, split_by_char_empty) {
  int i = 0;
  for (astr_split_by_char(it, ",", astr(""))) {
    i++;
  }
  ASSERT_EQ(i, 0);
}

UTEST(astr, split_by_char_trailing_sep) {
  int i = 0;
  for (astr_split_by_char(it, ",", astr("a,b,,"))) {
    i++;
  }
  ASSERT_EQ(i, 2);
}

UTEST(astr, split_by_char_no_sep) {
  int i = 0;
  for (astr_split_by_char(it, ",", astr("hello"))) {
    i++;
  }
  ASSERT_EQ(i, 1);
}

UTEST(astr, split_by_char_single_char) {
  int i = 0;
  for (astr_split_by_char(it, ",", astr("x"))) {
    i++;
  }
  ASSERT_EQ(i, 1);
}

UTEST(astr, split_by_char_multi_charset) {
  // Multiple separator characters
  int i = 0;
  astr tokens[5];
  for (astr_split_by_char(it, ",| $", astr("a,b|c d$e"))) {
    tokens[i++] = it.token;
  }
  ASSERT_EQ(i, 5);
  ASSERT_TRUE(astr_equals(tokens[0], astr("a")));
  ASSERT_TRUE(astr_equals(tokens[1], astr("b")));
  ASSERT_TRUE(astr_equals(tokens[2], astr("c")));
  ASSERT_TRUE(astr_equals(tokens[3], astr("d")));
  ASSERT_TRUE(astr_equals(tokens[4], astr("e")));
}

UTEST(astr, split_by_char_consecutive_sep) {
  // Consecutive mixed separators treated as one gap
  int i = 0;
  for (astr_split_by_char(it, ", ", astr("a,  ,b"))) {
    i++;
  }
  ASSERT_EQ(i, 2);
}

UTEST(astr, split_by_char_only_sep_single) {
  int i = 0;
  for (astr_split_by_char(it, ",", astr(","))) {
    i++;
  }
  ASSERT_EQ(i, 0);
}

UTEST(astr, split_by_char_embedded_nul) {
  // \0 in data should be treated as separator (not content)
  char data[] = "a\0b\0c";
  astr s = {data, 5};
  int i = 0;
  for (astr_split_by_char(it, ",", s)) {
    i++;
  }
  ASSERT_EQ(i, 3);
}

UTEST(astr, trim_ascii) {
  astr s = astr_trim(astr("  hello  "));
  ASSERT_TRUE(astr_equals(s, astr("hello")));
}

UTEST(astr, trim_tabs_newlines) {
  astr s = astr_trim(astr("\t\nhello\r\n"));
  ASSERT_TRUE(astr_equals(s, astr("hello")));
}

UTEST(astr, trim_all_whitespace) {
  astr s = astr_trim(astr("   "));
  ASSERT_EQ(s.len, 0);
}

UTEST(astr, trim_empty) {
  astr s = astr_trim(astr(""));
  ASSERT_EQ(s.len, 0);
}

UTEST(astr, trim_no_whitespace) {
  astr s = astr_trim(astr("hello"));
  ASSERT_TRUE(astr_equals(s, astr("hello")));
}

UTEST(astr, trim_preserves_high_bytes) {
  // UTF-8: "é" = \xc3\xa9 — must NOT be trimmed
  astr s = astr_trim_left(astr("\xc3\xa9tail"));
  ASSERT_TRUE(astr_equals(s, astr("\xc3\xa9tail")));

  s = astr_trim_right(astr("head\xc3\xa9"));
  ASSERT_TRUE(astr_equals(s, astr("head\xc3\xa9")));
}

UTEST(astr, trim_left_only) {
  astr s = astr_trim_left(astr("  hello  "));
  ASSERT_TRUE(astr_equals(s, astr("hello  ")));
}

UTEST(astr, trim_right_only) {
  astr s = astr_trim_right(astr("  hello  "));
  ASSERT_TRUE(astr_equals(s, astr("  hello")));
}

UTEST(astr, concat_self_at_tip) {
  // Place a string at the arena tip, then concat with itself
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  astr s = astr_format(arena, "hello");  // s is at arena tip
  astr doubled = astr_concat(arena, s, s);
  ASSERT_EQ(doubled.len, 10);
  ASSERT_TRUE(astr_equals(doubled, astr("hellohello")));
}

UTEST(astr, concat_normal) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  astr a = astr_format(arena, "hello");
  astr b = astr_format(arena, "world");
  astr c = astr_concat(arena, a, b);
  ASSERT_EQ(c.len, 10);
  ASSERT_TRUE(astr_equals(c, astr("helloworld")));
}

UTEST(astr, concat_empty_head) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  astr a = astr("");
  astr b = astr_format(arena, "world");
  astr c = astr_concat(arena, a, b);
  ASSERT_TRUE(astr_equals(c, astr("world")));
}

UTEST(astr, concat_empty_tail) {
  enum { size = KB(4) };
  byte mem[size] = {0};
  Arena arena[] = {arena_init(mem, size)};

  astr a = astr_format(arena, "hello");
  astr b = astr("");
  astr c = astr_concat(arena, a, b);
  ASSERT_TRUE(astr_equals(c, astr("hello")));
}

UTEST(astr, contains_match) {
  ASSERT_TRUE(astr_contains(astr("hello world"), astr("world")));
}

UTEST(astr, contains_no_match) {
  ASSERT_FALSE(astr_contains(astr("hello world"), astr("xyz")));
}

UTEST(astr, contains_empty_needle) {
  ASSERT_TRUE(astr_contains(astr("hello"), astr("")));
}

UTEST(astr, contains_empty_haystack) {
  ASSERT_FALSE(astr_contains(astr(""), astr("a")));
}

UTEST(astr, find_found) {
  ASSERT_EQ(6, astr_find(astr("hello world"), astr("world")));
}

UTEST(astr, find_not_found) {
  ASSERT_EQ(-1, astr_find(astr("hello world"), astr("xyz")));
}

UTEST(astr, find_empty_needle) {
  ASSERT_EQ(0, astr_find(astr("hello"), astr("")));
}

UTEST(astr, find_first_of_multiple) {
  ASSERT_EQ(0, astr_find(astr("abab"), astr("ab")));
}

UTEST(astr, compare_equal) {
  ASSERT_EQ(0, astr_compare(astr("abc"), astr("abc")));
}

UTEST(astr, compare_less) {
  ASSERT_TRUE(astr_compare(astr("abc"), astr("abd")) < 0);
}

UTEST(astr, compare_greater) {
  ASSERT_TRUE(astr_compare(astr("abd"), astr("abc")) > 0);
}

UTEST(astr, compare_prefix_ordering) {
  ASSERT_TRUE(astr_compare(astr("ab"), astr("abc")) < 0);
  ASSERT_TRUE(astr_compare(astr("abc"), astr("ab")) > 0);
}
