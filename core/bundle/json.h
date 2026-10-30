// core/bundle/json.h - the manifest's JSON (docs/BUNDLES.md "Manifest"): a writer into a caller's buffer, and a
// bounded pull reader for untrusted text. The reader never recurses deeper than kDepth, never reads a string longer
// than the caller's buffer (an error, not a cut), takes integers only (no fraction or exponent past 2^53), and stops
// at the first error with its offset. No allocation.
#pragma once
#include <stdint.h>

namespace core {
namespace bundle {

struct JOut {
  char* p;
  uint32_t cap, n;
  bool over;   // ran out of room (the text is then incomplete)
  void begin(char* buf, uint32_t c) { p = buf, cap = c, n = 0, over = false; if (c) p[0] = 0; }
  void raw(const char* s);
  void str(const char* s);      // a quoted, escaped string
  void num(uint64_t v);
  void inum(int64_t v);
  void key(const char* k);      // "k": (the comma before it is the caller's: comma())
  void comma(bool* first);      // "," unless *first (then *first = false)
};

constexpr int kDepth = 12;
struct JIn {
  const char* s;
  uint32_t n, at;
  int depth;
  const char* err;   // nullptr while fine
  void begin(const char* text, uint32_t len) { s = text, n = len, at = 0, depth = 0, err = nullptr; }
  bool fail(const char* why) { if (!err) err = why; return false; }
  void ws();
  int peek();                                  // the next character after spaces (-1 at the end)
  bool lit(char c);                            // that character next (spaces skipped)
  bool str(char* out, uint32_t cap);           // a string (escapes decoded; \u to UTF-8); too long: an error
  bool num(int64_t* v);                        // an integer
  bool boolean(bool* v);
  bool null();
  bool skip();                                 // any value
  // objects and arrays: obj_begin, then while (next_key(k)) { value }; arr_begin, then while (arr_more()) { value }
  bool obj_begin();
  bool next_key(char* k, uint32_t cap);        // false at '}' (or an error: err)
  bool arr_begin();
  bool arr_more();                             // false at ']' (or an error)
 private:
  bool first_[kDepth + 2];
};

}  // namespace bundle
}  // namespace core
