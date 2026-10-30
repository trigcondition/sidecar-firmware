// core/msamp/text.h - the few character and number helpers the SFZ parser and its neighbours need, written here: the
// Pi's hosted objects link no C library's ctype or strto* (core/platform.h's board gives only strings and printf).
#pragma once
#include <stdint.h>

namespace core {
namespace msamp {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool is_word(char c) { return is_alpha(c) || is_digit(c) || c == '_'; }
inline char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

// A decimal integer with an optional sign; *end after it (== s: none)
inline long to_long(const char* s, const char** end) {
  const char* p = s;
  bool neg = *p == '-';
  if (*p == '-' || *p == '+') ++p;
  if (!is_digit(*p)) {
    *end = s;
    return 0;
  }
  long v = 0;
  while (is_digit(*p)) v = v * 10 + (*p++ - '0');
  *end = p;
  return neg ? -v : v;
}
inline int to_int(const char* s) {
  const char* e;
  return (int)to_long(s, &e);
}
// A decimal number: sign, digits, '.', digits, an exponent; *end after it (== s: none)
inline double to_double(const char* s, const char** end) {
  const char* p = s;
  bool neg = *p == '-';
  if (*p == '-' || *p == '+') ++p;
  double v = 0;
  bool any = false;
  while (is_digit(*p)) v = v * 10 + (*p++ - '0'), any = true;
  if (*p == '.') {
    ++p;
    double f = 0.1;
    while (is_digit(*p)) v += (*p++ - '0') * f, f *= 0.1, any = true;
  }
  if (!any) {
    *end = s;
    return 0;
  }
  if (*p == 'e' || *p == 'E') {
    const char* q;
    long x = to_long(p + 1, &q);
    if (q != p + 1) {
      p = q;
      double m = 1;
      for (long i = 0; i < (x < 0 ? -x : x) && i < 300; ++i) m *= 10;
      v = x < 0 ? v / m : v * m;
    }
  }
  *end = p;
  return neg ? -v : v;
}

}  // namespace msamp
}  // namespace core
