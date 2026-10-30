// pi/engine/poolweb.cpp - see poolweb.h.
#include "poolweb.h"

#include <stdint.h>
#include <string.h>

#include "lib/pools.h"

namespace poolweb {

namespace {

namespace P = core::pools;

// the value of name in "a=1&b=2" (no decoding: the page sends plain tokens), "" when absent
void param(const char* q, const char* name, char* out, int cap) {
  out[0] = 0;
  int nl = (int)strlen(name);
  for (const char* p = q; p && *p;) {
    if (!strncmp(p, name, nl) && p[nl] == '=') {
      p += nl + 1;
      int n = 0;
      while (*p && *p != '&' && n < cap - 1) out[n++] = *p++;
      out[n] = 0;
      return;
    }
    p = strchr(p, '&');
    if (p) ++p;
  }
}

struct Out {
  char* b;
  int cap, n;
  bool full;
  void raw(const char* s, int len = -1) {
    if (len < 0) len = (int)strlen(s);
    if (n + len >= cap) { full = true; return; }
    memcpy(b + n, s, len);
    n += len;
    b[n] = 0;
  }
  void num(uint64_t v) {
    char t[24];
    int k = 0;
    do t[k++] = (char)('0' + v % 10), v /= 10; while (v);
    char r[24];
    for (int i = 0; i < k; ++i) r[i] = t[k - 1 - i];
    raw(r, k);
  }
  // a part written into the rest of the buffer by fn (its length, 0: did not fit)
  template <typename F> bool part(F fn) {
    if (cap - n < 64) { full = true; return false; }
    int k = fn(b + n, cap - n);
    if (k <= 0 || n + k >= cap) return false;
    n += k;
    b[n] = 0;
    return true;
  }
};

}  // namespace

int json(const char* params, char* out, int cap) {
  if (cap < 256) return 0;
  char lt[8], bt[4], ut[4];
  param(params ? params : "", "l", lt, sizeof lt);
  param(params ? params : "", "b", bt, sizeof bt);
  param(params ? params : "", "u", ut, sizeof ut);
  int l = P::list_of_tag(lt);
  if (l < 0) l = P::kSamples;
  int bank = bt[0] >= '0' && bt[0] <= '7' ? bt[0] - '0' : bt[0] >= 'A' && bt[0] <= 'H' ? bt[0] - 'A'
           : bt[0] >= 'a' && bt[0] <= 'h' ? bt[0] - 'a' : 0;
  Out o{out, cap - 1, 0, false};
  out[0] = 0;
  o.raw("{\"st\":");
  if (!o.part([](char* b, int c) { return P::status_json(b, c); })) o.raw("{}");
  P::Ram r = P::ram();
  o.raw(",\"ram\":{\"budget\":"), o.num(r.budget), o.raw(",\"used\":"), o.num(r.used), o.raw(",\"tables\":"), o.num(r.tables);
  o.raw(",\"entries\":"), o.num(r.entries), o.raw(",\"pct\":"), o.num(r.pct), o.raw("}");
  if (ut[0] == '1') {
    static int16_t un[P::kCodes];   // (core 0's web tasks take turns: Circle's scheduler is cooperative)
    int n = P::select_unused(l, un, P::kCodes);
    o.raw(",\"unused\":[");
    for (int i = 0; i < n; ++i) {
      if (i) o.raw(",");
      o.num((uint64_t)un[i]);
    }
    o.raw("],\"warn\":\""), o.raw(P::kUnusedWarning), o.raw("\"");
  }
  o.raw(",\"bank\":");
  // the bank last (the largest part): what is left of the buffer, less the closing brace
  int keep = o.n;
  o.cap -= 1;
  if (!o.part([l, bank](char* b, int c) { return P::bank_json(l, bank, b, c); })) {
    o.n = keep;
    o.raw("null");
  }
  o.cap += 1;
  o.raw("}");
  return o.full ? 0 : o.n;
}

}  // namespace poolweb
