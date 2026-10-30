// core/lib/ref.cpp - lib::Ref's text form (library.h, the formats spec 6.6): "<sha256 64 hex>|- <size> <path hint>".
// Portable and pure (no globals, no card): the library, the pool lists (core/lib/pools.h) and the format registry's
// readers (core/fmt/formats.cpp, the fuzz target) share it.
#include <string.h>

#include "../platform.h"
#include "library.h"
#include "sha256.h"

namespace core {
namespace lib {

bool parse_ref(const char* line, Ref* out) {
  if (!line || !out) return false;
  memset(out, 0, sizeof *out);
  const char* p = line;
  if (*p == '-') out->has_sha = false, ++p;
  else if (unhex_prefix(p, out->sha, 32)) out->has_sha = true, p += 64;
  else return false;
  if (*p++ != ' ' || *p < '0' || *p > '9') return false;
  uint64_t s = 0;
  int digits = 0;
  while (*p >= '0' && *p <= '9') {
    if (++digits > 19) return false;   // (a size past 10^19: not a file)
    s = s * 10 + (uint64_t)(*p++ - '0');
  }
  if (*p++ != ' ' || *p != '/') return false;
  size_t l = 0;
  while (p[l] && p[l] != '\t' && p[l] != '\r' && p[l] != '\n') ++l;
  if (l >= (size_t)kPath) return false;
  memcpy(out->hint, p, l);
  out->hint[l] = 0;
  out->size = s;
  return true;
}
int format_ref(const Ref& r, char* out, int cap) {
  char sha[65];
  if (r.has_sha) hex(r.sha, 32, sha);
  else sha[0] = '-', sha[1] = 0;
  int n = sniprintf(out, cap, "%s %llu %s", sha, (unsigned long long)r.size, r.hint);
  if (n <= 0 || n >= cap) {
    if (cap > 0) out[0] = 0;
    return 0;
  }
  return n;
}

}  // namespace lib
}  // namespace core
