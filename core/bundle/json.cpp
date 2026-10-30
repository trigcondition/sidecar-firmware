// core/bundle/json.cpp - see json.h.
#include "json.h"

#include <string.h>

#include "../platform.h"

namespace core {
namespace bundle {

void JOut::raw(const char* s) {
  uint32_t l = (uint32_t)strlen(s);
  if (n + l + 1 > cap) { over = true; return; }
  memcpy(p + n, s, l + 1);
  n += l;
}
void JOut::str(const char* s) {
  char b[8];
  raw("\"");
  for (const unsigned char* c = (const unsigned char*)s; *c; ++c) {
    if (*c == '"' || *c == '\\') b[0] = '\\', b[1] = (char)*c, b[2] = 0;
    else if (*c < 0x20) sniprintf(b, sizeof b, "\\u%04x", *c);
    else b[0] = (char)*c, b[1] = 0;
    raw(b);
  }
  raw("\"");
}
void JOut::num(uint64_t v) {
  char b[24];
  int k = 23;
  b[k] = 0;
  do b[--k] = (char)('0' + v % 10), v /= 10; while (v);
  raw(b + k);
}
void JOut::inum(int64_t v) {
  if (v < 0) raw("-"), v = -v;
  num((uint64_t)v);
}
void JOut::key(const char* k) {
  str(k);
  raw(":");
}
void JOut::comma(bool* first) {
  if (!*first) raw(",");
  *first = false;
}

void JIn::ws() {
  while (at < n && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at;
}
int JIn::peek() {
  ws();
  return at < n ? (unsigned char)s[at] : -1;
}
bool JIn::lit(char c) {
  if (err) return false;
  if (peek() != (unsigned char)c) return fail("unexpected character");
  ++at;
  return true;
}
static int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool JIn::str(char* out, uint32_t cap) {
  if (!lit('"')) return false;
  uint32_t k = 0;
  auto add = [&](char c) {
    if (k + 1 >= cap) return fail("a string too long");
    out[k++] = c;
    return true;
  };
  while (at < n) {
    char c = s[at++];
    if (c == '"') {
      out[k] = 0;
      return true;
    }
    if ((unsigned char)c < 0x20) return fail("a control character in a string");
    if (c != '\\') {
      if (!add(c)) return false;
      continue;
    }
    if (at >= n) break;
    char e = s[at++];
    if (e == 'u') {
      if (at + 4 > n) break;
      int v = 0;
      for (int i = 0; i < 4; ++i) {
        int h = hexv(s[at + i]);
        if (h < 0) return fail("a bad \\u escape");
        v = v << 4 | h;
      }
      at += 4;
      if (v >= 0xD800 && v <= 0xDFFF) return fail("a surrogate in a string");
      if (v == 0) return fail("a NUL in a string");
      if (v < 0x80) { if (!add((char)v)) return false; }
      else if (v < 0x800) { if (!add((char)(0xC0 | v >> 6)) || !add((char)(0x80 | (v & 63)))) return false; }
      else if (!add((char)(0xE0 | v >> 12)) || !add((char)(0x80 | ((v >> 6) & 63))) || !add((char)(0x80 | (v & 63)))) return false;
      continue;
    }
    char d = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e == '/' ? '/' : e == '\\' ? '\\' : e == '"' ? '"' : 0;
    if (!d) return fail("a bad escape");
    if (!add(d)) return false;
  }
  return fail("a string not closed");
}
bool JIn::num(int64_t* v) {
  if (err) return false;
  ws();
  bool neg = at < n && s[at] == '-';
  if (neg) ++at;
  if (at >= n || s[at] < '0' || s[at] > '9') return fail("a number expected");
  uint64_t x = 0;
  int digits = 0;
  while (at < n && s[at] >= '0' && s[at] <= '9') {
    x = x * 10 + (uint64_t)(s[at++] - '0');
    if (++digits > 16) return fail("a number too large");
  }
  if (at < n && (s[at] == '.' || s[at] == 'e' || s[at] == 'E')) return fail("not an integer");
  if (x > (1ull << 53)) return fail("a number too large");
  *v = neg ? -(int64_t)x : (int64_t)x;
  return true;
}
bool JIn::boolean(bool* v) {
  if (err) return false;
  ws();
  if (n - at >= 4 && !strncmp(s + at, "true", 4)) return at += 4, *v = true, true;
  if (n - at >= 5 && !strncmp(s + at, "false", 5)) return at += 5, *v = false, true;
  return fail("true or false expected");
}
bool JIn::null() {
  if (err) return false;
  ws();
  if (n - at >= 4 && !strncmp(s + at, "null", 4)) return at += 4, true;
  return fail("null expected");
}
bool JIn::obj_begin() {
  if (!lit('{')) return false;
  if (++depth > kDepth) return fail("nested too deeply");
  first_[depth] = true;
  return true;
}
bool JIn::next_key(char* k, uint32_t cap) {
  if (err) return false;
  if (peek() == '}') {
    ++at, --depth;
    return false;
  }
  if (!first_[depth] && !lit(',')) return false;
  first_[depth] = false;
  return str(k, cap) && lit(':');
}
bool JIn::arr_begin() {
  if (!lit('[')) return false;
  if (++depth > kDepth) return fail("nested too deeply");
  first_[depth] = true;
  return true;
}
bool JIn::arr_more() {
  if (err) return false;
  if (peek() == ']') {
    ++at, --depth;
    return false;
  }
  if (!first_[depth] && !lit(',')) return false;
  first_[depth] = false;
  return true;
}
bool JIn::skip() {
  if (err) return false;
  int c = peek();
  if (c == '{') {
    char k[256];
    if (!obj_begin()) return false;
    while (next_key(k, sizeof k))
      if (!skip()) return false;
    return !err;
  }
  if (c == '[') {
    if (!arr_begin()) return false;
    while (arr_more())
      if (!skip()) return false;
    return !err;
  }
  if (c == '"') {
    char b[1024];
    return str(b, sizeof b);
  }
  if (c == 't' || c == 'f') {
    bool b;
    return boolean(&b);
  }
  if (c == 'n') return null();
  int64_t v;
  return num(&v);
}

}  // namespace bundle
}  // namespace core
