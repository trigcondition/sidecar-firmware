// core/fx/out.cpp - see out.h: the OUT settings (knobs, LFO offsets -> out::control), the mute gate's targets, the kit
// file's OUT line, the console, /status. The control core's (the Pi's core 3), between blocks.
#include "fx/out.h"

#include <math.h>
#include <string.h>

#include "fx/out_measure.h"
#include "platform.h"
#if CORE_MSAMP
#include "pool/pcache.h"
#endif

namespace core {
namespace out {

Control control;
void (*on_change)() = nullptr;

namespace {

uint8_t g_knob[kVoices][4];      // LEV PAN WID DEC
int16_t g_lfo[kVoices][3];
int8_t g_track[kVoices];         // the track that last triggered the voice (-1 unknown)
uint16_t g_mutes = 0;
Stats g_stats;

struct Init {
  Init() { reset(); }
} g_init;

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
bool same_ci(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
    if (x != y) return false;
  }
  return *a == *b;
}
int num(const char* p) {   // a decimal number (a leading '-' allowed), -100000 none
  bool neg = *p == '-';
  if (neg) ++p;
  if (*p < '0' || *p > '9') return -100000;
  int v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
  return neg ? -v : v;
}
const char* field(const char* p, char* b, int cap) {
  int n = 0;
  while (*p && *p != ':') {
    if (n < cap - 1) b[n++] = *p;
    ++p;
  }
  b[n] = 0;
  return *p == ':' ? p + 1 : p;
}
int voice_of(const char* s) {   // "V1".."V8" (or "1".."8"), -1 none
  if ((s[0] == 'V' || s[0] == 'v') && s[1] >= '1' && s[1] <= '8' && !s[2]) return s[1] - '1';
  if (s[0] >= '1' && s[0] <= '8' && !s[1]) return s[0] - '1';
  return -1;
}
void changed() {
  ++g_stats.sets;
  if (on_change) on_change();
}
void refresh(int vi) {
  control.lev[vi] = (uint8_t)clampi(g_knob[vi][0] + g_lfo[vi][0], 0, 127);
  control.pan[vi] = (uint8_t)clampi(g_knob[vi][1] + g_lfo[vi][1], 0, 127);
  control.wid[vi] = (uint8_t)clampi(g_knob[vi][2] + g_lfo[vi][2], 0, 127);
  control.dec[vi] = g_knob[vi][3];
}
void gates() {
  for (int v = 0; v < kVoices; ++v) {
    int t = g_track[v];
    bool muted;
    if (t >= 0) muted = (g_mutes >> t) & 1;
    else {
      int a = device().tracks[v][0], b = device().tracks[v][1];
      muted = ((g_mutes >> a) & 1) && (b < 0 || ((g_mutes >> b) & 1));
    }
    control.open[v] = muted ? 0 : 1;
  }
}

}  // namespace

int route_from_name(const char* s) {
  if (same_ci(s, "TRK") || same_ci(s, "TRACK")) return kTrk;
  if (same_ci(s, "PRE")) return kPre;
  if (same_ci(s, "POST")) return kPost;
  return -1;
}

float dec_coef(int v) {
  if (v <= 0) return 1.0f;
  float t60 = 0.02f * powf(500.0f, (float)(v - 1) / 126.0f);
  return expf(-6.9078f / (t60 * 48000.0f));
}
void dec_text(int v, char* b, int cap) {
  if (v <= 0) {
    sniprintf(b, cap, "OFF");
    return;
  }
  float t = 0.02f * powf(500.0f, (float)(v - 1) / 126.0f);
  int ms = (int)(t * 1000.0f + 0.5f);
  if (ms < 1000) sniprintf(b, cap, CORE_STR("%dMS"), ms);
  else if (ms < 10000) sniprintf(b, cap, CORE_STR("%d.%dS"), ms / 1000, (ms % 1000) / 100);
  else sniprintf(b, cap, CORE_STR("%dS"), (ms + 500) / 1000);
}
void value_text(int k, int v, char* b, int cap) {
  if (k == 1) {
    if (v == 64) sniprintf(b, cap, "C");
    else sniprintf(b, cap, CORE_STR("%c%d"), v < 64 ? 'L' : 'R', v < 64 ? 64 - v : v - 64);
    return;
  }
  if (k == 3) return dec_text(v, b, cap);
  sniprintf(b, cap, CORE_STR("%d"), v);
}

void set_route(int vi, int r) {
  if (vi < 0 || vi >= kVoices || r < 0 || r >= kRoutes) return;
  if (control.route[vi] != r) ++g_stats.xfades;
  control.route[vi] = (uint8_t)r;
  changed();
}
void set_param(int vi, int k, int v) {
  if (vi < 0 || vi >= kVoices || k < 0 || k > 3) return;
  g_knob[vi][k] = (uint8_t)clampi(v, 0, 127);
  refresh(vi);
  changed();
}
int param(int vi, int k) { return vi >= 0 && vi < kVoices && k >= 0 && k <= 3 ? g_knob[vi][k] : 0; }
void lfo_offset(int vi, int k, int off) {
  if (vi < 0 || vi >= kVoices || k < 0 || k > 2) return;
  g_lfo[vi][k] = (int16_t)clampi(off, -127, 127);
  refresh(vi);
}
void set_mutes(uint16_t mask) {
  g_mutes = mask;
  gates();
}
void trig_track(int vi, int track) {
  if (vi < 0 || vi >= kVoices) return;
  g_track[vi] = (int8_t)(track >= 0 && track < 12 ? track : -1);
  gates();
}
bool any_stereo() {
  for (int v = 0; v < kVoices; ++v)
    if (control.route[v] != kTrk) return true;
  return false;
}
void set_trim(int pair, int db10) {
  if (pair < 0 || pair > 1) return;
  control.trim10[pair] = (int16_t)clampi(db10, -240, 120);
}

bool from_line(const char* const* w, int n) {
  if (n < 3 || !same_ci(w[0], "OUT")) return false;
  int vi = voice_of(w[1]), r = route_from_name(w[2]);
  if (vi < 0 || r < 0) return false;
  set_route(vi, r);
  for (int k = 0; k < 4 && 3 + k < n; ++k) {
    int v = same_ci(w[3 + k], "OFF") ? 0 : num(w[3 + k]);
    if (v < 0 || v > 127) return false;
    set_param(vi, k, v);
  }
  return true;
}
int line(int vi, char* b, int cap) {
  if (vi < 0 || vi >= kVoices) return 0;
  if (control.route[vi] == kTrk && g_knob[vi][0] == 100 && g_knob[vi][1] == 64 && g_knob[vi][2] == 64 && !g_knob[vi][3])
    return 0;
  return sniprintf(b, cap, CORE_STR("OUT V%d %s %d %d %d %d\n"), vi + 1, route_name(control.route[vi]), g_knob[vi][0],
                   g_knob[vi][1], g_knob[vi][2], g_knob[vi][3]);
}
void clear() {
  for (int v = 0; v < kVoices; ++v) {
    control.route[v] = kTrk;
    g_knob[v][0] = 100, g_knob[v][1] = 64, g_knob[v][2] = 64, g_knob[v][3] = 0;
    refresh(v);
  }
}

static int voice_text(int vi, char* o, int cap) {
  char a[8], b[8], c[8], d[8];
  value_text(0, g_knob[vi][0], a, sizeof a), value_text(1, g_knob[vi][1], b, sizeof b);
  value_text(2, g_knob[vi][2], c, sizeof c), value_text(3, g_knob[vi][3], d, sizeof d);
  return sniprintf(o, cap, CORE_STR("V%d %s LEV %s PAN %s WID %s DEC %s%s"), vi + 1, route_name(control.route[vi]), a, b, c,
                   d, control.open[vi] ? "" : " (muted)");
}

bool command(const char* p, char* o, int cap) {
#if CORE_MSAMP
  if (!strncmp(p, "poolmono", 8) && (!p[8] || p[8] == '=')) {   // stereo files summed at load from now on (pcache.h)
    if (p[8]) pcache::set_mono(p[9] == '1');
    sniprintf(o, cap, "poolmono: %s (loads from now on; entries in RAM keep their channels)",
              pcache::mono() ? "1: stereo files summed at load" : "0: stereo files kept stereo");
    return true;
  }
#endif
  if (!strncmp(p, "stereo", 6) && (!p[6] || p[6] == '=')) {
    const char* q = p[6] ? p + 7 : "";
    char a[12], b[12], c[12];
    q = field(q, a, sizeof a);
    q = field(q, b, sizeof b);
    field(q, c, sizeof c);
    if (same_ci(a, "trim")) {
      int pair = same_ci(b, "pre") ? 0 : same_ci(b, "post") ? 1 : -1, v = num(c);
      if (pair < 0 || v == -100000) {
        sniprintf(o, cap, "stereo=trim:pre|post:DB10 (tenths of a dB, -240..120)");
        return true;
      }
      set_trim(pair, v);
    } else if (same_ci(a, "measure")) {   // core/fx/out_measure.h
      if (same_ci(b, "stop")) {
        meas::stop();
        sniprintf(o, cap, "stereo: measure stopped");
        return true;
      }
      if (b[0] && !same_ci(b, "trk")) {
        sniprintf(o, cap, "stereo=measure (POST, PRE), stereo=measure:trk[:V] (and voice V's track: its VCA open), stereo=measure:stop");
        return true;
      }
      int v = c[0] >= '1' && c[0] <= '8' ? c[0] - '1' : 0;
      meas::start(same_ci(b, "trk"), v);
      sniprintf(o, cap, CORE_STR("stereo: measuring (about %d s): tones and impulses on POST, PRE%s; the Rytm's MAIN IN read back "
                                 "(stereo=measure:result when done)"), same_ci(b, "trk") ? 6 : 4, same_ci(b, "trk") ? ", TRK" : "");
      return true;
    } else if (same_ci(a, "result")) {
      meas::text(o, cap);
      return true;
    } else if (a[0]) {
      sniprintf(o, cap, "stereo, stereo=trim:pre|post:DB10, stereo=measure[:trk[:V]]|measure:stop, stereo=result");
      return true;
    }
    int n = sniprintf(o, cap, CORE_STR("stereo: trim PRE %d POST %d (0.1 dB)"), control.trim10[0], control.trim10[1]);
    for (int v = 0; v < kVoices && n > 0 && n < cap; ++v)
      if (control.route[v] != kTrk) {
        int k = sniprintf(o + n, cap - n, ", ");
        if (k > 0) n += k;
        if (n < cap) n += voice_text(v, o + n, cap - n);
      }
    return true;
  }
  if (strncmp(p, "out=", 4) != 0) return false;
  const char* q = p + 4;
  if (!strcmp(q, "main") || !strcmp(q, "tracks")) return false;   // the output stage's mode
  if (same_ci(q, "list")) {
    int n = sniprintf(o, cap, "out:");
    for (int v = 0; v < kVoices && n > 0 && n < cap; ++v) {
      int k = sniprintf(o + n, cap - n, v ? ", " : " ");
      if (k > 0) n += k;
      if (n < cap) n += voice_text(v, o + n, cap - n);
    }
    return true;
  }
  char a[8], b[8], c[8];
  q = field(q, a, sizeof a);
  q = field(q, b, sizeof b);
  field(q, c, sizeof c);
  int vi = voice_of(a);
  static const char kHelp[] =
      "out=Vn:trk|pre|post, out=Vn:lev|pan|wid:0..127, out=Vn:dec:off|1..127, out=Vn, out=list (out=main|tracks: the output stage)";
  if (vi < 0) {
    sniprintf(o, cap, "%s", kHelp);
    return true;
  }
  int r = route_from_name(b);
  if (r >= 0) {
    set_route(vi, r);
  } else if (b[0]) {
    int k = same_ci(b, "lev") ? 0 : same_ci(b, "pan") ? 1 : same_ci(b, "wid") ? 2 : same_ci(b, "dec") ? 3 : -1;
    int v = k == 3 && same_ci(c, "off") ? 0 : num(c);
    if (k < 0 || v < 0 || v > 127) {
      sniprintf(o, cap, "%s", kHelp);
      return true;
    }
    set_param(vi, k, v);
  }
  int n = sniprintf(o, cap, "out: ");
  if (n > 0 && n < cap) voice_text(vi, o + n, cap - n);
  return true;
}

int json(char* o, int cap) {
  int n = sniprintf(o, cap, CORE_STR("\"out\":["));
  for (int v = 0; v < kVoices && n < cap; ++v) {
    int k = sniprintf(o + n, cap - n, CORE_STR("%s{\"route\":\"%s\",\"lev\":%d,\"pan\":%d,\"wid\":%d,\"dec\":%d,\"open\":%d}"),
                      v ? "," : "", v >= 0 && control.route[v] == kPre ? "pre" : control.route[v] == kPost ? "post" : "trk",
                      g_knob[v][0], g_knob[v][1], g_knob[v][2], g_knob[v][3], control.open[v]);
    if (k < 0 || k >= cap - n) return cap;
    n += k;
  }
  int k = sniprintf(o + n, n < cap ? cap - n : 0, CORE_STR("],\"trim\":[%d,%d],"), control.trim10[0], control.trim10[1]);
  if (k > 0) n += k;
  if (n < cap) n += meas::json(o + n, cap - n);
  return n;
}

const Stats& stats() { return g_stats; }

void reset() {
  defaults(control);
  for (int v = 0; v < kVoices; ++v) {
    g_knob[v][0] = 100, g_knob[v][1] = 64, g_knob[v][2] = 64, g_knob[v][3] = 0;
    g_lfo[v][0] = g_lfo[v][1] = g_lfo[v][2] = 0;
    g_track[v] = -1;
  }
  g_mutes = 0;
  g_stats = Stats();
}

// the measurement's texts (out_measure.h; here with the console: the Pi's integer printf)
namespace meas {
int text(char* o, int cap) {
  const Result& r = result();
  if (!r.done) {
    static const char* const kN[] = {"idle", "floor", "tone POST", "tone PRE", "tone TRK", "impulse POST", "impulse TRK", "done"};
    return sniprintf(o, cap, "measure: %s%s", kN[(int)phase() < 8 ? (int)phase() : 0],
                     (int)phase() == kIdle ? " (stereo=measure[:trk[:V]] starts it; docs/STEREO.md)" : "...");
  }
  auto d10 = [](float x) { return (int)(x * 10.0f + (x < 0 ? -0.5f : 0.5f)); };
  char fl[12], g[3][12];   // signed tenths ("-12.5"; the Pi's printf has no float)
  auto t10 = [&](float x, char* b) {
    int v = d10(x), a = v < 0 ? -v : v;
    sniprintf(b, 12, CORE_STR("%s%d.%d"), v < 0 ? "-" : "+", a / 10, a % 10);
  };
  t10(r.floor_db, fl), t10(r.gain_db[0], g[0]), t10(r.gain_db[1], g[1]), t10(r.gain_db[2], g[2]);
  int n = sniprintf(o, cap, CORE_STR("measure: MAIN IN floor %s dB; POST %s dB%s, PRE %s dB%s"), fl, g[0],
                    r.heard[0] ? "" : " (NOT IN MAIN IN)", g[1], r.heard[1] ? "" : " (NOT IN MAIN IN)");
  if (r.trk && n > 0 && n < cap)
    n += sniprintf(o + n, cap - n, CORE_STR(", TRK %s dB%s"), g[2], r.heard[2] ? "" : " (silent: the track's VCA closed?)");
  if (n > 0 && n < cap) n += sniprintf(o + n, cap - n, CORE_STR("; impulse POST %d frames"), (int)r.delay[0]);
  if (r.trk && n > 0 && n < cap) {
    n += sniprintf(o + n, cap - n, CORE_STR(", TRK %d frames"), (int)r.delay[1]);
    if (r.delay[0] >= 0 && r.delay[1] >= 0 && n < cap)
      n += sniprintf(o + n, cap - n, CORE_STR(" (TRK - POST %d)"), (int)(r.delay[1] - r.delay[0]));
    if (r.heard[0] && r.heard[2] && n < cap) {
      n += sniprintf(o + n, cap - n, CORE_STR("; to match TRK: stereo=trim:post:%d"), d10(r.gain_db[2] - r.gain_db[0]));
      if (r.heard[1] && n < cap) n += sniprintf(o + n, cap - n, CORE_STR(" stereo=trim:pre:%d"), d10(r.gain_db[2] - r.gain_db[1]));
    }
  }
  return n;
}

int json(char* o, int cap) {
  const Result& r = result();
  auto d10 = [](float x) { return (int)(x * 10.0f + (x < 0 ? -0.5f : 0.5f)); };
  return sniprintf(o, cap,
                   CORE_STR("\"measure\":{\"phase\":%d,\"done\":%d,\"floor10\":%d,\"gain10\":[%d,%d,%d],\"heard\":[%d,%d,%d],"
                            "\"delay\":[%d,%d],\"trk\":%d}"),
                   (int)(int)phase(), r.done ? 1 : 0, d10(r.floor_db), d10(r.gain_db[0]), d10(r.gain_db[1]), d10(r.gain_db[2]),
                   r.heard[0], r.heard[1], r.heard[2], (int)r.delay[0], (int)r.delay[1], r.trk ? 1 : 0);
}

}  // namespace meas

}  // namespace out
}  // namespace core
