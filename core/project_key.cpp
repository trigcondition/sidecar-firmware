// core/project_key.cpp - see project_key.h.
#include "project_key.h"

#include <string.h>

#include "platform.h"

namespace core {

// A slot key (2026-10-27, core/bundle/prjslot.h): '@' and three digits, 000..128. Passed through unchanged by both rules
// (a Rytm name never makes one: '@' is not kept, so a name with it becomes '_')
CORE_COLD bool is_slot_key(const char* k) {
  if (!k || k[0] != '@') return false;
  for (int i = 1; i <= 3; ++i)
    if (k[i] < '0' || k[i] > '9') return false;
  return !k[4] && (k[1] - '0') * 100 + (k[2] - '0') * 10 + (k[3] - '0') <= 128;
}

static void keep(const char* n, char* out, int cap, bool song_v0) {
  if (cap <= 0) return;
  if (is_slot_key(n) && cap >= 5) {
    memcpy(out, n, 5);
    return;
  }
  int max = cap - 1 < kKeyMax - 1 ? cap - 1 : kKeyMax - 1, k = 0;
  for (const char* c = n ? n : ""; *c && k < max; ++c) {
    char x = *c;
    bool ok = (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == '-' || x == '_' ||
              (!song_v0 && (x == ' ' || x == '.'));
    out[k++] = ok ? x : '_';
  }
  if (!song_v0)
    while (k && (out[k - 1] == ' ' || out[k - 1] == '.')) --k;
  out[k] = 0;
  if (!k) sniprintf(out, cap, "%s", song_v0 ? "_none" : "_default");
}

CORE_COLD void project_key(const char* n, char* out, int cap) { keep(n, out, cap, false); }
CORE_COLD void project_key_song_v0(const char* n, char* out, int cap) { keep(n, out, cap, true); }

CORE_COLD void project_key_sampler(const char* n, char* out, int cap) {
  if (cap <= 0) return;
  const char* in = n && n[0] ? n : "Sidecar";
  int k = 0;
  for (; *in && k < cap - 1; ++in) {
    char c = *in;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    out[k++] = ok ? c : '_';
  }
  if (!k && cap > 1) out[k++] = 'X';
  out[k] = 0;
}

}  // namespace core
