// core/mod/learn.cpp - see learn.h. Control side only, cold.
#include "mod/learn.h"

#include <string.h>

#include "fmt/fmt.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "platform.h"

namespace core {
namespace learn {

namespace {

const char kFile[] = "/.rytm/learn.txt";
Route g_r[kRoutes];
int g_n = 0;
float g_val[kRoutes];       // the route's value 0..127.99 (-1: none came yet)
uint8_t g_msb[kRoutes];     // a 14-bit route's last MSB
bool g_14[kRoutes];         // its LSB (CC n + 32) heard: 14 bits
int g_armed = -1;           // the What the next CC is learned for, -1 none
mod::Tid g_arm_t = 0;
int8_t g_arm_depth = 32;
bool g_dirty = false, g_locked = false;

CORE_COLD const char* what_name(const Route& r, char* b, int cap) {
  static const char* const kNames[] = {"fader", "a", "b"};
  if (r.what < kTarget) return kNames[r.what];
  return mod::name(r.t, b, cap);
}
CORE_COLD int find(int port, int ch, int cc) {
  for (int i = 0; i < g_n; ++i)
    if (g_r[i].port == port && g_r[i].ch == ch && g_r[i].cc == cc) return i;
  return -1;
}
CORE_COLD bool add(const Route& r) {   // (replacing a route on the same port / channel / CC)
  int i = find(r.port, r.ch, r.cc);
  if (i < 0) {
    if (g_n >= kRoutes) return false;
    i = g_n++;
  }
  g_r[i] = r, g_val[i] = -1, g_14[i] = false, g_msb[i] = 0;
  g_dirty = true;
  return true;
}
CORE_COLD void apply(int i, float v) {   // v 0..127.99
  const Route& r = g_r[i];
  g_val[i] = v;
  switch (r.what) {
    case kFader: scene::set_fader((uint16_t)(g_14[i] ? v * 128.0f : v * 129.0f), scene::kLearn); break;
    case kA: if (v < scene::kScenes) scene::set_ab((int)v, scene::state().b); break;
    case kB: if (v < scene::kScenes) scene::set_ab(scene::state().a, (int)v); break;
    default: break;   // a target: its offset each tick
  }
}
CORE_COLD bool parse_what(const char* s, Route* r) {   // fader | a | b | NAME[:DEPTH]
  if (!strcmp(s, "fader")) return r->what = kFader, true;
  if (!strcmp(s, "a")) return r->what = kA, true;
  if (!strcmp(s, "b")) return r->what = kB, true;
  char nm[24];
  const char* c = strchr(s, ':');
  const char* d = c ? strchr(c + 1, ':') : nullptr;
  int n = d ? (int)(d - s) : (int)strlen(s);
  if (n <= 0 || n >= (int)sizeof nm) return false;
  memcpy(nm, s, (size_t)n), nm[n] = 0;
  if (!mod::parse(nm, &r->t)) return false;
  r->what = kTarget, r->depth = 32;
  if (d) {
    const char* p = d + 1;
    bool neg = *p == '-';
    if (neg) ++p;
    if (*p < '0' || *p > '9') return false;
    int v = 0;
    while (*p >= '0' && *p <= '9' && v < 1000) v = v * 10 + (*p++ - '0');
    if (*p || v > 64 || (!neg && v > 63)) return false;
    r->depth = (int8_t)(neg ? -v : v);
  }
  return true;
}
CORE_COLD int num(const char** pp) {   // a decimal; -1 none
  const char* p = *pp;
  while (*p == ' ') ++p;
  if (*p < '0' || *p > '9') return -1;
  int v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
  *pp = p;
  return v;
}
CORE_COLD bool take_line(const char* b) {   // R <port> <ch> <cc> <what> [depth]
  const char* p = b;
  if (p[0] != 'R' || p[1] != ' ') return false;
  p += 2;
  int port = num(&p), ch = num(&p), cc = num(&p);
  if (port < 0 || port > 4 || ch < 1 || ch > 16 || cc < 0 || cc > 127) return false;
  while (*p == ' ') ++p;
  char w[32];
  int i = 0;
  while (*p && *p != ' ' && i < (int)sizeof w - 1) w[i++] = *p++;
  w[i] = 0;
  while (*p == ' ') ++p;
  if (*p) {   // a depth: NAME DEPTH -> NAME:DEPTH
    int n = (int)strlen(w);
    if (n + 1 + (int)strlen(p) >= (int)sizeof w) return false;
    w[n] = ':', strcpy(w + n + 1, p);
  }
  Route r = {(uint8_t)port, (uint8_t)(ch - 1), (uint8_t)cc, 0, 0, 0};
  if (!parse_what(w, &r) || (r.what != kTarget && strchr(w, ':'))) return false;
  if (g_n >= kRoutes) return false;
  g_r[g_n] = r, g_val[g_n] = -1, g_14[g_n] = false, ++g_n;
  return true;
}

}  // namespace

CORE_COLD bool midi(int port, uint8_t st, uint8_t d1, uint8_t d2) {
  if ((st & 0xF0) != 0xB0 || port < 0 || port > 4) return false;
  int ch = st & 15;
  d1 &= 0x7F, d2 &= 0x7F;
  if (g_armed >= 0) {
    Route r = {(uint8_t)port, (uint8_t)ch, d1, (uint8_t)g_armed, g_arm_t, g_arm_depth};
    g_armed = -1;
    if (!add(r)) {
      log(CORE_STR("learn: %d routes already: CC %d not learned"), kRoutes, d1);
      return true;
    }
    char nm[16];
    log(CORE_STR("learn: CC %d on port %d channel %d -> %s"), d1, port, ch + 1, what_name(r, nm, sizeof nm));
  }
  int i = find(port, ch, d1);
  if (i >= 0) {
    if (g_14[i]) g_msb[i] = d2, apply(i, (float)d2);
    else g_msb[i] = d2, apply(i, (float)d2);
    return true;
  }
  if (d1 >= 32 && d1 < 64 && (i = find(port, ch, d1 - 32)) >= 0) {   // the LSB of a route on CC n < 32: 14 bits
    g_14[i] = true;
    apply(i, (float)(g_msb[i] << 7 | d2) / 128.0f);
    return true;
  }
  return false;
}

CORE_COLD bool arm(const char* what) {
  Route r = {};
  if (!what || !parse_what(what, &r)) return false;
  g_armed = r.what, g_arm_t = r.t, g_arm_depth = r.depth;
  return true;
}
CORE_COLD void disarm() { g_armed = -1; }
CORE_COLD int armed() { return g_armed; }

CORE_COLD void tick() {
  for (int i = 0; i < g_n; ++i) {
    if (g_r[i].what != kTarget || g_val[i] < 0) continue;
    float w = (g_val[i] - 63.5f) / 63.5f;
    mod::offset(kFirstSource + i, g_r[i].t, (float)g_r[i].depth, w > 1 ? 1 : w < -1 ? -1 : w);
  }
}

CORE_COLD int routes(const Route** out) {
  if (out) *out = g_r;
  return g_n;
}
CORE_COLD bool clear(int i) {
  if (i < 0 || i >= g_n) return false;
  for (int k = i; k + 1 < g_n; ++k) g_r[k] = g_r[k + 1], g_val[k] = g_val[k + 1], g_14[k] = g_14[k + 1], g_msb[k] = g_msb[k + 1];
  --g_n;
  g_dirty = true;
  return true;
}

CORE_COLD bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "learn", 5) != 0 || (p[5] && p[5] != '=')) return false;
  if (cap <= 0) return true;
  o[0] = 0;
  const char* a = p[5] ? p + 6 : nullptr;
  bool ok = true;
  if (!a) {
  } else if (!strcmp(a, "off")) {
    disarm();
  } else if (!strncmp(a, "clear:", 6)) {
    const char* q = a + 6;
    int n = num(&q);
    ok = n >= 1 && !*q && clear(n - 1);
  } else {
    ok = arm(a);
  }
  if (!ok) {
    sniprintf(o, cap, CORE_STR("learn: cannot \"%s\" (learn=fader|a|b|NAME[:DEPTH] | clear:N | off)"), p);
    return true;
  }
  int n = sniprintf(o, cap, CORE_STR("learn: %d route%s%s%s"), g_n, g_n == 1 ? "" : "s",
                    g_armed >= 0 ? ", waiting for a CC" : "", g_locked ? " (FILE LOCKED: NEWER)" : "");
  for (int i = 0; i < g_n && n > 0 && n < cap; ++i) {
    char nm[16];
    const Route& r = g_r[i];
    n += sniprintf(o + n, cap - n, CORE_STR("%s%d: port %d ch %d CC %d -> %s"), i ? ", " : ": ", i + 1, r.port, r.ch + 1,
                   r.cc, what_name(r, nm, sizeof nm));
    if (r.what == kTarget && n > 0 && n < cap) n += sniprintf(o + n, cap - n, CORE_STR(" %+d"), r.depth);
  }
  return true;
}

CORE_COLD void load() {
  g_n = 0, g_locked = false, g_dirty = false;
  File f;
  fmt::post_wait(kFile);   // a posted save of it lands before it is read back (fmt.h)
  fmt::recover(kFile);     // a power cut between write_atomic's renames: the previous file back
  if (!f.open(kFile, kRead)) return;
  fmt::LineReader rd(f);
  char b[96];
  int n = 0, bad = 0;
  for (int k; (k = rd.line(b, sizeof b)) >= 0;) {
    if (k == 0 || b[0] == '#') {
      fmt::Sniffed sn;   // a newer build's version: read what can be read, never written
      if (k && fmt::text_header(fmt::Bytes{(const uint8_t*)b, (uint32_t)k}, "learn", &sn) && sn.version > 1) ++bad;
      continue;
    }
    if (take_line(b)) ++n;
    else ++bad;
  }
  f.close();
  if (bad) g_locked = true;
  log(CORE_STR("learn: %d routes from %s%s"), n, kFile, bad ? " (lines it does not read: the file is kept, not written)" : "");
}

CORE_COLD bool save() {
  if (!g_dirty) return false;
  if (g_locked) {   // 1042
    g_dirty = false;
    log(CORE_STR("learn: not saved: %s has lines this build does not read (kept as it is)"), kFile);
    return true;
  }
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
  int n = sniprintf(t, (int)fmt::kPostMax, CORE_STR("#sidecar learn 1\n"));
  for (int i = 0; i < g_n; ++i) {
    char nm[16];
    const Route& r = g_r[i];
    n += sniprintf(t + n, (int)fmt::kPostMax - n, CORE_STR("R %d %d %d %s"), r.port, r.ch + 1, r.cc, what_name(r, nm, sizeof nm));
    if (r.what == kTarget) n += sniprintf(t + n, (int)fmt::kPostMax - n, CORE_STR(" %d"), r.depth);
    n += sniprintf(t + n, (int)fmt::kPostMax - n, "\n");
  }
  if (!fmt::post_end(kFile, (uint32_t)n)) return false;
  g_dirty = false;
  return true;
}
CORE_COLD bool dirty() { return g_dirty; }
CORE_COLD bool locked() { return g_locked; }

CORE_COLD void reset() {
  g_n = 0, g_armed = -1, g_arm_t = 0, g_arm_depth = 32;
  g_dirty = g_locked = false;
  for (int i = 0; i < kRoutes; ++i) g_val[i] = -1, g_14[i] = false, g_msb[i] = 0;
}

}  // namespace learn
}  // namespace core
