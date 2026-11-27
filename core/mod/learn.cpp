// core/mod/learn.cpp - see learn.h. Control side only, cold.
#include "mod/learn.h"

#include <string.h>

#include "fmt/fmt.h"
#include "mod/mod.h"
#include "mod/scene.h"
#include "device/device.h"
#include "pool/pool.h"
#include "platform.h"

namespace core {
namespace learn {

namespace {

const char kFile[] = "/.rytm/learn.txt";
// 3(c) step 8: each box's own routes (its learn.txt in its own folder: slot 0's loaded at boot, another's at its attach),
// arming and values, the slot in hand's (prj_slot). A controller's CC (ports 1..4) goes to every slot's routes, each in
// its Scope; the Rytm's own CCs (port 0) to slot 0's only.
struct Sl {
  Route r[kRoutes];
  int n = 0;
  float val[kRoutes];       // the route's value 0..127.99 (-1: none came yet)
  uint8_t msb[kRoutes];     // a 14-bit route's last MSB
  uint8_t lsb[kRoutes];     // ... and its last LSB (an MSB alone plays with it: no dip at an MSB step, v2 Task 6)
  bool b14[kRoutes];        // its LSB (CC n + 32) heard: 14 bits
  int armed = -1;           // the What the next CC is learned for, -1 none
  int last = -1;            // the route the last learn made or replaced (the MIDI page's LEARN rows)
  mod::Tid arm_t = 0;
  int8_t arm_depth = 32;
  bool arm_only = false;    // the armed learn replaces the other routes of its What
  bool dirty = false, locked = false;
  int qp_saved = scene::kQpMutedOnly;   // QPERF FADER as the file has it (a change of scene::qp_mode() makes it dirty)
};
Sl g_sl[kVoiceSlots];
inline Sl& L() { return g_sl[prj_slot()]; }

CORE_COLD const char* what_name(const Route& r, char* b, int cap) {
  static const char* const kNames[] = {"fader", "a", "b"};
  if (r.what < kTarget) return kNames[r.what];
  return mod::name(r.t, b, cap);
}
CORE_COLD int find(int port, int ch, int cc) {
  for (int i = 0; i < L().n; ++i)
    if (L().r[i].port == port && L().r[i].ch == ch && L().r[i].cc == cc) return i;
  return -1;
}
CORE_COLD bool add(const Route& r) {   // (replacing a route on the same port / channel / CC)
  int i = find(r.port, r.ch, r.cc);
  if (i < 0) {
    if (L().n >= kRoutes) return false;
    i = L().n++;
  }
  L().r[i] = r, L().val[i] = -1, L().b14[i] = false, L().msb[i] = 0, L().lsb[i] = 0;
  L().dirty = true;
  L().last = i;
  return true;
}
CORE_COLD void apply(int i, float v) {   // v 0..127.99
  const Route& r = L().r[i];
  if (r.what == kFader && L().val[i] >= 0 && (scene::fader_source() != scene::kLearn || mod::held_any())) {
    // another source has the fader (or a held edit plays): a take needs 2 CC steps (7 bit) / 256 (14 bit) from the
    // value acted on last, so a pot's flicker neither steals it nor lets the edit go (v2 Task 6, UX 2a; QPERF's M1)
    float d = v > L().val[i] ? v - L().val[i] : L().val[i] - v;
    if (d < 2.0f) return;
  }
  L().val[i] = v;
  switch (r.what) {
    case kFader: scene::set_fader((uint16_t)(L().b14[i] ? v * 128.0f : v * 129.0f), scene::kLearn); break;
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
CORE_COLD bool take_line(const char* b) {   // R <port> <ch> <cc> <what> [depth], or QP muted|off|always
  const char* p = b;
  if (!strncmp(p, "QP ", 3)) {
    for (int m = scene::kQpMutedOnly; m <= scene::kQpAlways; ++m)
      if (!strcmp(p + 3, scene::qp_mode_name(m))) return scene::set_qp_mode(m), L().qp_saved = m, true;
    return false;
  }
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
  if (L().n >= kRoutes) return false;
  L().r[L().n] = r, L().val[L().n] = -1, L().b14[L().n] = false, L().msb[L().n] = 0, L().lsb[L().n] = 0, ++L().n;
  return true;
}

}  // namespace

static CORE_COLD bool midi_one(int port, uint8_t st, uint8_t d1, uint8_t d2) {
  int ch = st & 15;
  d1 &= 0x7F, d2 &= 0x7F;
  if (L().armed >= 0) {
    Route r = {(uint8_t)port, (uint8_t)ch, d1, (uint8_t)L().armed, L().arm_t, L().arm_depth};
    L().armed = -1;
    if (!add(r)) {
      log(CORE_STR("learn: %d routes already: CC %d not learned"), kRoutes, d1);
      return true;
    }
    if (L().arm_only)
      for (int j = L().n - 1; j >= 0; --j)
        if (j != L().last && L().r[j].what == r.what && (r.what != kTarget || L().r[j].t == r.t)) clear(j);
    char nm[16];
    log(CORE_STR("learn: CC %d on port %d channel %d -> %s"), d1, port, ch + 1, what_name(r, nm, sizeof nm));
  }
  int i = find(port, ch, d1);
  if (i >= 0) {
    // a 14-bit route's MSB with the last LSB when it stays, else the LSB end nearest the old value (up 0, down 127: no
    // overshoot past the LSB that follows, review L1); the LSB refines it
    const int lsb = d2 == L().msb[i] ? L().lsb[i] : d2 > L().msb[i] ? 0 : 127;
    L().msb[i] = d2, L().lsb[i] = (uint8_t)lsb;   // (a repeated MSB before its LSB: the same value again)
    apply(i, L().b14[i] ? (float)(d2 << 7 | lsb) / 128.0f : (float)d2);
    return true;
  }
  if (d1 >= 32 && d1 < 64 && (i = find(port, ch, d1 - 32)) >= 0) {   // the LSB of a route on CC n < 32: 14 bits
    L().b14[i] = true, L().lsb[i] = d2;
    apply(i, (float)(L().msb[i] << 7 | d2) / 128.0f);
    return true;
  }
  return false;
}

CORE_COLD bool midi(int port, uint8_t st, uint8_t d1, uint8_t d2) {
  if ((st & 0xF0) != 0xB0 || port < 0 || port > 4) return false;
  bool took = false;
  for (int s = 0; s < kVoiceSlots; ++s) {   // each slot's routes in its Scope (port 0, the USB-C box's: its slot's only)
    if (!device::live(s) || (kVoiceSlots > 1 && port == 0 && s != device::route_of(0))) continue;
    device::Scope in(device::g_links[s]);
    took = midi_one(port, st, d1, d2) || took;
  }
  return took;
}

CORE_COLD bool arm(const char* what, bool only) {
  Route r = {};
  if (!what || !parse_what(what, &r)) return false;
  L().armed = r.what, L().arm_t = r.t, L().arm_depth = r.depth, L().arm_only = only;
  return true;
}
CORE_COLD void disarm() { L().armed = -1; }
CORE_COLD int armed() { return L().armed; }
CORE_COLD int last() { return L().last; }

CORE_COLD void tick() {
  for (int i = 0; i < L().n; ++i) {
    if (L().r[i].what != kTarget || L().val[i] < 0) continue;
    float w = (L().val[i] - 63.5f) / 63.5f;
    mod::offset(kFirstSource + i, L().r[i].t, (float)L().r[i].depth, w > 1 ? 1 : w < -1 ? -1 : w);
  }
}

CORE_COLD int routes(const Route** out) {
  if (out) *out = L().r;
  return L().n;
}
CORE_COLD bool clear(int i) {
  if (i < 0 || i >= L().n) return false;
  for (int k = i; k + 1 < L().n; ++k) L().r[k] = L().r[k + 1], L().val[k] = L().val[k + 1], L().b14[k] = L().b14[k + 1], L().msb[k] = L().msb[k + 1],
                                  L().lsb[k] = L().lsb[k + 1];
  --L().n;
  L().last = L().last == i ? -1 : L().last > i ? L().last - 1 : L().last;
  L().dirty = true;
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
  int n = sniprintf(o, cap, CORE_STR("learn: %d route%s%s%s"), L().n, L().n == 1 ? "" : "s",
                    L().armed >= 0 ? ", waiting for a CC" : "", L().locked ? " (FILE LOCKED: NEWER)" : "");
  for (int i = 0; i < L().n && n > 0 && n < cap; ++i) {
    char nm[16];
    const Route& r = L().r[i];
    n += sniprintf(o + n, cap - n, CORE_STR("%s%d: port %d ch %d CC %d -> %s"), i ? ", " : ": ", i + 1, r.port, r.ch + 1,
                   r.cc, what_name(r, nm, sizeof nm));
    if (r.what == kTarget && n > 0 && n < cap) n += sniprintf(o + n, cap - n, CORE_STR(" %s%d"), r.depth < 0 ? "-" : "+", r.depth < 0 ? -r.depth : r.depth);
  }
  return true;
}

// one line of learn.txt into the slot in hand's routes; false: a line this build does not read
static CORE_COLD bool line_in(const char* b, int k, int* n) {
  if (k == 0 || b[0] == '#') {
    fmt::Sniffed sn;   // a newer build's version: read what can be read, never written
    return !(k && fmt::text_header(fmt::Bytes{(const uint8_t*)b, (uint32_t)k}, "learn", &sn) && sn.version > 1);
  }
  if (!take_line(b)) return false;
  *n += b[0] == 'R';   // (routes counted; a QP line is a setting)
  return true;
}
static CORE_COLD void said(int n, int bad) {
  if (bad) L().locked = true;
  log(CORE_STR("learn: slot %d: %d routes from %s%s"), prj_slot(), n, kFile,
      bad ? " (lines it does not read: the file is kept, not written)" : "");
}

CORE_COLD void load() {
  L().n = 0, L().locked = false, L().dirty = false;
  scene::set_qp_mode(scene::kQpMutedOnly), L().qp_saved = scene::kQpMutedOnly;   // (no QP line: the default)
  File f;
  fmt::post_wait(kFile);   // a posted save of it lands before it is read back (fmt.h)
  fmt::recover(kFile);     // a power cut between write_atomic's renames: the previous file back
  if (!f.open(kFile, kRead)) return;
  fmt::LineReader rd(f);
  char b[96];
  int n = 0, bad = 0;
  for (int k; (k = rd.line(b, sizeof b)) >= 0;) bad += !line_in(b, k, &n);
  f.close();
  said(n, bad);
}

// 3(c) step 8: another slot's file, read by the card core at its attach, taken in its Scope
CORE_COLD void take_text(const char* t, int len) {
  L().n = 0, L().locked = false, L().dirty = false;
  char b[96];
  int n = 0, bad = 0;
  for (int i = 0; i < len;) {
    int k = 0;
    while (i < len && t[i] != '\n') {
      if (t[i] != '\r' && k < (int)sizeof b - 1) b[k++] = t[i];
      ++i;
    }
    ++i;
    b[k] = 0;
    bad += !line_in(b, k, &n);
  }
  said(n, bad);
}

CORE_COLD bool save() {
  if (!dirty()) return false;
  if (L().locked) {   // 1042
    L().dirty = false, L().qp_saved = scene::qp_mode();
    log(CORE_STR("learn: not saved: %s has lines this build does not read (kept as it is)"), kFile);
    return true;
  }
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
  const int qp = scene::qp_mode();   // (QPERF FADER: the slot in hand's box's, scene.h)
  int n = sniprintf(t, (int)fmt::kPostMax, CORE_STR("#sidecar learn 1\n"));
  if (qp != scene::kQpMutedOnly) n += sniprintf(t + n, (int)fmt::kPostMax - n, CORE_STR("QP %s\n"), scene::qp_mode_name(qp));
  for (int i = 0; i < L().n; ++i) {
    char nm[16];
    const Route& r = L().r[i];
    n += sniprintf(t + n, (int)fmt::kPostMax - n, CORE_STR("R %d %d %d %s"), r.port, r.ch + 1, r.cc, what_name(r, nm, sizeof nm));
    if (r.what == kTarget) n += sniprintf(t + n, (int)fmt::kPostMax - n, CORE_STR(" %d"), r.depth);
    n += sniprintf(t + n, (int)fmt::kPostMax - n, "\n");
  }
  if (!fmt::post_end(kFile, (uint32_t)n)) return false;
  L().dirty = false, L().qp_saved = qp;
  return true;
}
CORE_COLD bool dirty() { return L().dirty || scene::qp_mode() != L().qp_saved; }
CORE_COLD bool locked() { return L().locked; }

CORE_COLD void reset() {
  for (Sl& x : g_sl) {
    x.n = 0, x.armed = -1, x.last = -1, x.arm_only = false, x.arm_t = 0, x.arm_depth = 32;
    x.dirty = x.locked = false, x.qp_saved = scene::kQpMutedOnly;
    for (int i = 0; i < kRoutes; ++i) x.val[i] = -1, x.b14[i] = false, x.msb[i] = 0, x.lsb[i] = 0;
  }
}

}  // namespace learn
}  // namespace core
