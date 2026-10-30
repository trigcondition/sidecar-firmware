// core/livein/livein.cpp - see livein.h. render / post: the voice's render core (hot, no allocation, no card);
// in_block / in_frames: the IN side; the rest: the control core (cold).
#include "livein/livein.h"

#include <math.h>
#include <string.h>

#include "fx/out.h"
#include "machines.h"
#include "platform.h"
#include "voices.h"

namespace core {
namespace livein {

namespace {
constexpr float kSr = 48000.0f;
constexpr uint32_t kMask = kRing - 1;
static_assert((kRing & kMask) == 0, "kRing: a power of two");
constexpr int kMaxN = 16;
// the guard (post): 50 ms windows, a probe of 30 ms at -6 dB, 40 sub-windows of 5 ms watched after it (loops to 170 ms)
constexpr uint32_t kWin = 2400, kSub = 240, kProbe = 1440;
constexpr int kSubs = 40, kDipW = 6;
constexpr uint32_t kMuteFrames = 96000, kRecoverFrames = 24000, kCoolOk = 144000, kCoolTrip = 48000;
constexpr float kCeil = 0.891f * 32767.0f;     // the limiter: -1 dBFS of the voice's int16 return
// the measure: 5 clicks (1 kHz, 2 ms, -9 dBFS) 0.5 s apart after 0.1 s of the input's floor
constexpr int kClicks = 5, kClickLen = 96;
constexpr uint32_t kClickGap = 24000, kNoise = 4800;

inline float db2lin(float db) { return expf(db * 0.11512925f); }
inline float lin2db(float x) { return x > 1e-9f ? 20.0f * log10f(x) : -180.0f; }
inline float tcoef(float ms) { return 1.0f - expf(-1000.0f / (ms * kSr)); }   // a one-pole's step for time constant ms
inline float absf(float x) { return x < 0 ? -x : x; }
inline float clampp(float p) { return p < 0 ? 0 : p > 127 ? 127 : p; }

// ---- the ring (one writer)
alignas(64) float g_ring[kRing][2];
volatile uint32_t g_w = 0;

// ---- control -> render
volatile bool g_force[kVoices];
volatile uint32_t g_trig[kVoices], g_off[kVoices], g_reset[kVoices], g_mreq[kVoices], g_mstop[kVoices];
volatile int g_fifo = 96;

// ---- render -> control (plain stores of whole words; read by the control core and any core's status)
struct Pub {
  volatile uint32_t blocks, underruns, skips, limited, probes, trips;
  volatile int32_t in_db10, measured, meas_found;
  volatile uint32_t gate_open, muted, stream, backlog, meas_state;
};
Pub pub[kVoices];

// ---- the render core's, per voice
struct R {
  bool on;
  uint32_t rd, last_w;
  uint64_t frame;                   // the render-frame count (the measure's time base)
  uint32_t seen_trig, seen_off, seen_reset, seen_mreq, seen_mstop;
  // the read point's backlog over a second
  uint32_t bl_sum, bl_n, bl_min, bl_frames, idle_frames;
  bool cut;
  // the gate and TRIG's envelope
  float det, gate_g;
  int hold_left;
  bool open;
  int stage;                         // 0 idle, 1 attack, 2 decay / sustain, 3 release
  float env;
  // the meter
  float pk;
  uint32_t pk_frames;
  // render -> post this block
  float blk_pow;
  // the guard
  float win_in, win_out;
  uint32_t win_n;
  float prev_db;
  int rise, high;
  int gstate;                        // 0 monitor, 1 probe, 2 muted, 3 recovering
  int phase, lag0;
  uint32_t gt, cool;
  float g, g_target, g_coef;
  float sub[kSubs], sub_acc;
  uint32_t sub_n;
  int nsub;
  float lim;
  // the measure
  int mstate;                        // 0 idle, 1 floor, 2 listening
  float nf;
  uint32_t mcnt;
  int mk, nfound, emit;              // emit: the click's next sample (-1 none)
  uint64_t t0;
  bool got;
  int32_t found[kClicks];
};
R r[kVoices];

void clear_voice(R& s) {
  float lim = 1.0f;
  memset(&s, 0, sizeof s);
  s.lim = lim, s.g = s.g_target = 1.0f, s.g_coef = tcoef(2.0f), s.emit = -1, s.bl_min = 0xffffffffu, s.prev_db = -180.0f;
  s.open = true, s.gate_g = 1.0f;
}

void publish_measure(int vi, R& s) {
  int n = s.nfound;
  int32_t a[kClicks];
  for (int i = 0; i < n; ++i) a[i] = s.found[i];
  for (int i = 1; i < n; ++i)   // (insertion sort: five numbers)
    for (int j = i; j > 0 && a[j] < a[j - 1]; --j) {
      int32_t t = a[j];
      a[j] = a[j - 1], a[j - 1] = t;
    }
  pub[vi].meas_found = n;
  if (n >= 3 && a[n - 1] - a[0] <= 48) {
    pub[vi].measured = a[n / 2];
    pub[vi].meas_state = 2;
  } else {
    pub[vi].meas_state = 3;
  }
  s.mstate = 0;
}

uint32_t logged_trips[kVoices], logged_meas[kVoices];
}  // namespace

// ---- the knobs
CORE_COLD int input_of(float p1) { return eng_zone(p1, kInputs); }
CORE_COLD int mon_of(float p5) { return eng_zone(p5, kMons); }
float gain_db(float p2) {
  float d = (clampp(p2) - 64.0f) * (24.0f / 64.0f);
  return d > 24.0f ? 24.0f : d;
}
float gate_db(float p3) { return p3 < 1.0f ? 0.0f : -80.0f + (clampp(p3) - 1.0f) * (60.0f / 126.0f); }
float hold_ms(float p4) {
  float u = clampp(p4) / 127.0f;
  return 1000.0f * u * u;
}
float atk_ms(float p6) { return 0.5f * expf(clampp(p6) / 127.0f * 8.29405f); }   // 0.5 ms .. 2 s (x 4000)
float dec_ms(float p7) { return 5.0f * expf(clampp(p7) / 127.0f * 6.90776f); }   // 5 ms .. 5 s (x 1000)

CORE_COLD const char* label(int k) {
  static const char* const l[9] = {"INPT", "GAIN", "GATE", "HOLD", "MON", "ATK", "DEC", "SUS", "REL"};
  return k >= 0 && k < 9 ? l[k] : "";
}
static CORE_COLD void ms_text(float ms, char* b, int cap) {
  if (ms < 10.0f) sniprintf(b, cap, "%d.%dMS", (int)ms, (int)(ms * 10) % 10);
  else if (ms < 1000.0f) sniprintf(b, cap, "%dMS", (int)(ms + 0.5f));
  else sniprintf(b, cap, "%d.%dS", (int)(ms / 1000), (int)(ms / 100) % 10);
}
CORE_COLD void value_text(int k, float p, char* b, int cap) {
  static const char* const in[kInputs] = {"L", "R", "L+R", "STR"};
  switch (k) {
    case 0: sniprintf(b, cap, "%s", in[input_of(p)]); break;
    case 1: {
      int d = (int)lroundf(gain_db(p));
      sniprintf(b, cap, d > 0 ? "+%dDB" : "%dDB", d);
      break;
    }
    case 2:
      if (p < 1.0f) sniprintf(b, cap, "OFF");
      else sniprintf(b, cap, "%dDB", (int)lroundf(gate_db(p)));
      break;
    case 3: ms_text(hold_ms(p), b, cap); break;
    case 4: sniprintf(b, cap, "%s", mon_of(p) == kMonTrig ? "TRIG" : "ALW"); break;
    case 5: ms_text(atk_ms(p), b, cap); break;
    case 6:
    case 8: ms_text(dec_ms(p), b, cap); break;
    case 7: sniprintf(b, cap, "%d", (int)clampp(p)); break;
    default: sniprintf(b, cap, "-");
  }
}

// ---- the IN side
void in_block(const int32_t* f, int n, int stride) {
  const out::Device& d = out::device();
  const int cl = d.audio_in[0], cr = d.audio_in[1];
  uint32_t w = g_w;
  for (int i = 0; i < n; ++i, f += stride) {
    float* x = g_ring[(w + i) & kMask];
    x[0] = (float)f[cl] * (1.0f / 2147483648.0f);
    x[1] = (float)f[cr] * (1.0f / 2147483648.0f);
  }
  __atomic_store_n(&g_w, w + (uint32_t)n, __ATOMIC_RELEASE);
}
void in_frames(const float* l, const float* rr, int n) {
  uint32_t w = g_w;
  for (int i = 0; i < n; ++i) {
    float* x = g_ring[(w + i) & kMask];
    x[0] = l[i];
    x[1] = rr ? rr[i] : l[i];
  }
  __atomic_store_n(&g_w, w + (uint32_t)n, __ATOMIC_RELEASE);
}
bool wanted() {
  for (int i = 0; i < kVoices; ++i)
#if CORE_MSAMP
    if (g_force[i] || v[i].li) return true;
#else
    if (g_force[i]) return true;
#endif
  return false;
}

// ---- the control core
CORE_COLD bool forced(int vi) { return g_force[vi & 7]; }
CORE_COLD void force(int vi, bool on) {
  vi &= 7;
  if (g_force[vi] == on) return;
  g_force[vi] = on;
#if CORE_MSAMP
  V& x = v[vi];
  if (on && x.user < 0) {   // at once (no report needed: an effects lane plays as soon as it is set)
    x.li = true, x.live = true;
    x.ms = x.wt = x.gr = x.cp = x.fm = x.juno = x.md = x.daisy = x.dsps = x.grain = false;
  } else if (!on) {
    x.li = false;
  }
#endif
  silence(vi);
}
CORE_COLD void trig(int vi) { __atomic_fetch_add(&g_trig[vi & 7], 1u, __ATOMIC_RELEASE); }
CORE_COLD void release(int vi) { __atomic_fetch_add(&g_off[vi & 7], 1u, __ATOMIC_RELEASE); }
CORE_COLD void silence(int vi) { __atomic_fetch_add(&g_reset[vi & 7], 1u, __ATOMIC_RELEASE); }
CORE_COLD void set_fifo(int frames) { g_fifo = frames < 0 ? 0 : frames > 4096 ? 4096 : frames; }
CORE_COLD int fifo() { return g_fifo; }

CORE_COLD int latency_ms10(int vi, bool* measured) {
  const Pub& u = pub[vi & 7];
  int32_t m = u.measured;
  if (measured) *measured = m >= 0;
  int frames = m >= 0 ? m : out::device().loop_frames + g_fifo + (int)u.backlog;
  return (int)(((int64_t)frames * 10000 + 24000) / 48000);
}

CORE_COLD Stats stats(int vi) {
  const Pub& u = pub[vi & 7];
  Stats s = {};
  s.frames_in = g_w;
  s.blocks = u.blocks, s.underruns = u.underruns, s.skips = u.skips, s.limited = u.limited, s.probes = u.probes;
  s.trips = u.trips;
  s.in_db10 = (int16_t)u.in_db10, s.gate_open = (uint8_t)u.gate_open, s.muted = (uint8_t)u.muted;
  s.stream = (uint8_t)u.stream, s.backlog = (uint16_t)u.backlog, s.measured = u.measured;
  s.meas_state = (uint8_t)u.meas_state, s.meas_found = (int16_t)u.meas_found;
  return s;
}

CORE_COLD int status_short(int vi, char* b, int cap) {
  Stats s = stats(vi);
  if (!s.stream) return sniprintf(b, cap, "LIVE NO IN");
  if (s.muted) return sniprintf(b, cap, "LIVE MUTED");
  if (!s.gate_open) return sniprintf(b, cap, "LIVE GATED");
  int d = s.in_db10 <= -990 ? -99 : (s.in_db10 - 5) / 10;
  return sniprintf(b, cap, "LIVE %dDB", d);
}

CORE_COLD void step() {
  for (int vi = 0; vi < kVoices; ++vi) {
    const Pub& u = pub[vi];
    if (u.trips != logged_trips[vi]) {
      logged_trips[vi] = u.trips;
      log(CORE_STR("livein: voice %d feedback - the input follows the voice (its output reaches AUDIO IN: the main out "
                   "patched to the input?): muted 2 s (trip %lu)"),
          vi, (unsigned long)u.trips);
    }
    uint32_t ms = u.meas_state;
    if (ms != logged_meas[vi]) {
      logged_meas[vi] = ms;
      if (ms == 2) {
        int f = u.measured;
        log(CORE_STR("livein: voice %d round trip %d frames (%d.%d ms), %d of %d clicks"), vi, f, f * 10 / 48 / 10,
            f * 10 / 48 % 10, (int)u.meas_found, kClicks);
      } else if (ms == 3) {
        log(CORE_STR("livein: voice %d measure failed: %d of %d clicks heard back (patch the main out to AUDIO IN, the "
                     "voice's route audible there)"),
            vi, (int)u.meas_found, kClicks);
      }
    }
  }
}

static CORE_COLD int voice_line(int vi, char* out, int cap) {
  Stats s = stats(vi);
  bool m;
  int ms10 = latency_ms10(vi, &m);
  const float* p = v[vi].p;
  char t[9][12];
  for (int k = 0; k < 9; ++k) value_text(k, p[k], t[k], sizeof t[k]);
  int d = s.in_db10 <= -990 ? -99 : s.in_db10 / 10;
  int n = sniprintf(out, cap,
                    "voice %d: in %d dBFS%s, gate %s, INPT %s GAIN %s GATE %s HOLD %s MON %s ATK %s DEC %s SUS %s REL %s; "
                    "round trip %d.%d ms (%s",
                    vi, d, s.stream ? "" : " (no stream)", s.muted ? "MUTED (feedback)" : s.gate_open ? "open" : "closed",
                    t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], ms10 / 10, ms10 % 10, m ? "measured" : "est.");
  if (n < cap && !m)
    n += sniprintf(out + n, cap - n, ": loop %d + fifo %d + backlog %d frames", out::device().loop_frames, (int)g_fifo,
                   (int)s.backlog);
  if (n < cap)
    n += sniprintf(out + n, cap - n, "); underruns %lu skips %lu limited %lu probes %lu trips %lu",
                   (unsigned long)s.underruns, (unsigned long)s.skips, (unsigned long)s.limited, (unsigned long)s.probes,
                   (unsigned long)s.trips);
  return n;
}

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "livein", 6) != 0 || (p[6] && p[6] != '=')) return false;
  const char* a = p[6] ? p + 7 : "";
  if (!strncmp(a, "voice:", 6)) {
    int vi = a[6] - '0';
    if (vi < 0 || vi > 7 || (a[7] && strcmp(a + 7, ":off"))) {
      sniprintf(out, cap, "livein: voice:V or voice:V:off, V 0..7");
      return true;
    }
    bool on = strcmp(a + 7, ":off") != 0;
    force(vi, on);
    sniprintf(out, cap, "livein: voice %d %s", vi, on ? "plays LIVE IN (AUDIO IN through its INSERT chain and route)"
                                                     : "plays its own machine");
    return true;
  }
  if (!strncmp(a, "measure", 7)) {
    if (!strcmp(a + 7, ":stop")) {
      for (int i = 0; i < kVoices; ++i) __atomic_fetch_add(&g_mstop[i], 1u, __ATOMIC_RELEASE);
      sniprintf(out, cap, "livein: measure stopped");
      return true;
    }
    int vi = -1;
    if (a[7] == ':' && a[8] >= '0' && a[8] <= '7' && !a[9]) vi = a[8] - '0';
    else if (!a[7])
      for (int i = 0; i < kVoices && vi < 0; ++i)
#if CORE_MSAMP
        if (v[i].li) vi = i;
#else
        if (g_force[i]) vi = i;
#endif
    if (vi < 0) {
      sniprintf(out, cap, "livein: measure:V (V 0..7) on a LIVE IN voice (livein=voice:V first)");
      return true;
    }
#if CORE_MSAMP
    if (!v[vi].li) {
      sniprintf(out, cap, "livein: voice %d does not play LIVE IN (livein=voice:%d first)", vi, vi);
      return true;
    }
#endif
    pub[vi].meas_state = 1, pub[vi].meas_found = 0;
    logged_meas[vi] = 1;
    __atomic_fetch_add(&g_mreq[vi], 1u, __ATOMIC_RELEASE);
    sniprintf(out, cap,
              "livein: measuring voice %d's round trip: 5 clicks over 3 s on the voice (its own sound muted meanwhile); "
              "the device's main out must reach its AUDIO IN (a cable)%s; `livein` shows the result",
              vi, out::control.route[vi] == out::kTrk ? " - TRK: the track's AMP must be open (OUT POST is simpler)" : "");
    return true;
  }
  if (a[0]) {
    sniprintf(out, cap, "livein: livein, livein=voice:V[:off], livein=measure[:V], livein=measure:stop");
    return true;
  }
  int n = sniprintf(out, cap, "livein: AUDIO IN = %s IN %d/%d, %lu frames in",
                    out::device().name, out::device().audio_in[0], out::device().audio_in[1], (unsigned long)g_w);
  bool any = false;
  for (int i = 0; i < kVoices && n < cap; ++i) {
#if CORE_MSAMP
    if (!v[i].li && !g_force[i]) continue;
#else
    if (!g_force[i]) continue;
#endif
    any = true;
    n += sniprintf(out + n, cap - n, " | ");
    if (n < cap) n += voice_line(i, out + n, cap - n);
  }
  if (!any && n < cap) sniprintf(out + n, cap - n, "; no voice plays LIVE IN (livein=voice:V)");
  return true;
}

CORE_COLD void reset() {
  for (int i = 0; i < kVoices; ++i) {
    g_force[i] = false;
    clear_voice(r[i]);
    r[i].seen_trig = g_trig[i], r[i].seen_off = g_off[i], r[i].seen_reset = g_reset[i];
    r[i].seen_mreq = g_mreq[i], r[i].seen_mstop = g_mstop[i];
    memset((void*)&pub[i], 0, sizeof pub[i]);
    pub[i].measured = -1, pub[i].in_db10 = -1000, pub[i].gate_open = 1;
    logged_trips[i] = 0, logged_meas[i] = 0;
  }
  memset(g_ring, 0, sizeof g_ring);
  g_w = 0;
  g_fifo = 96;
}
// (statics' first state: as reset() leaves them)
static struct Init {
  Init() { reset(); }
} s_init;

// ---- the render core
void render(int vi, const float* p, float* l, float* rr, bool stereo, int n) {
  R& s = r[vi];
  Pub& u = pub[vi];
  if (n > kMaxN) n = kMaxN;
  uint32_t rq = __atomic_load_n(&g_reset[vi], __ATOMIC_ACQUIRE);
  if (rq != s.seen_reset) {   // a fresh start (left LIVE IN, came back, CC 120): the counters stay
    uint32_t t = s.seen_trig, o = s.seen_off, mr = s.seen_mreq, ms = s.seen_mstop;
    uint64_t fr = s.frame;
    clear_voice(s);
    s.seen_reset = rq, s.seen_trig = t, s.seen_off = o, s.seen_mreq = mr, s.seen_mstop = ms, s.frame = fr;
    u.muted = 0;
  }
  // the input: the read point
  float in[kMaxN][2];
  uint32_t w = __atomic_load_n(&g_w, __ATOMIC_ACQUIRE);
  if (!s.on) {   // (re)start (the voice came to LIVE IN, or silence()): the newest block
    uint32_t have = w < (uint32_t)n ? w : (uint32_t)n;
    s.rd = w - have;
    s.on = true;
  }
  if (s.cut) {   // a second whose backlog never fell under kSlack: back to the newest block
    s.cut = false;
    if (w - s.rd > (uint32_t)n) s.rd = w - (uint32_t)n, ++u.skips;
  }
  uint32_t avail = w - s.rd;
  if (avail > (uint32_t)kRing - kMaxN) s.rd = w - (uint32_t)n, avail = (uint32_t)n, ++u.skips;   // overrun: fell behind
  uint32_t take = avail < (uint32_t)n ? avail : (uint32_t)n;
  for (uint32_t i = 0; i < take; ++i) {
    const float* x = g_ring[(s.rd + i) & kMask];
    in[i][0] = x[0], in[i][1] = x[1];
  }
  for (int i = (int)take; i < n; ++i) in[i][0] = in[i][1] = 0.0f;
  if (take < (uint32_t)n) ++u.underruns;
  s.rd += take;
  // the backlog (beyond this block) over a second; the stream's presence
  uint32_t extra = avail > (uint32_t)n ? avail - (uint32_t)n : 0;
  s.bl_sum += extra, ++s.bl_n;
  if (extra < s.bl_min) s.bl_min = extra;
  s.bl_frames += (uint32_t)n;
  if (s.bl_frames >= 48000) {
    u.backlog = s.bl_sum / (s.bl_n ? s.bl_n : 1);
    s.cut = s.bl_min > (uint32_t)kSlack && s.bl_min != 0xffffffffu;
    s.bl_sum = s.bl_n = s.bl_frames = 0, s.bl_min = 0xffffffffu;
  }
  if (w != s.last_w) s.idle_frames = 0, s.last_w = w;
  else s.idle_frames += (uint32_t)n;
  u.stream = s.idle_frames < 9600;   // 0.2 s
  // the measure: the floor, then each click's way back (the raw input, either side)
  uint32_t mq = __atomic_load_n(&g_mreq[vi], __ATOMIC_ACQUIRE), mst = __atomic_load_n(&g_mstop[vi], __ATOMIC_ACQUIRE);
  if (mst != s.seen_mstop) {
    s.seen_mstop = mst;
    if (s.mstate) s.mstate = 0, s.emit = -1, u.meas_state = 0;
  }
  if (mq != s.seen_mreq) {
    s.seen_mreq = mq;
    s.mstate = 1, s.nf = 0, s.mcnt = 0, s.mk = 0, s.nfound = 0, s.emit = -1, s.got = false;
    u.meas_state = 1, u.meas_found = 0;
  }
  if (s.mstate == 1) {
    for (int i = 0; i < n; ++i) {
      float a = absf(in[i][0]) > absf(in[i][1]) ? absf(in[i][0]) : absf(in[i][1]);
      if (a > s.nf) s.nf = a;
    }
    s.mcnt += (uint32_t)n;
    if (s.mcnt >= kNoise) s.mstate = 2, s.t0 = s.frame, s.emit = 0, s.got = false;   // the first click: this block
  } else if (s.mstate == 2) {
    float thr = s.nf * 8.0f > 0.003f ? s.nf * 8.0f : 0.003f;
    for (int i = 0; i < n && !s.got; ++i) {
      uint64_t idx = s.frame + (uint64_t)i;
      if (idx <= s.t0) continue;
      float a = absf(in[i][0]) > absf(in[i][1]) ? absf(in[i][0]) : absf(in[i][1]);
      if (a > thr) s.found[s.nfound++] = (int32_t)(idx - s.t0), s.got = true, u.meas_found = s.nfound;
    }
    if (s.frame + (uint64_t)n >= s.t0 + kClickGap) {   // the next click (or the end)
      if (++s.mk >= kClicks) publish_measure(vi, s);
      else s.t0 = s.frame, s.emit = 0, s.got = false;   // (post sends it from this block's first frame)
    }
  }
  // the knobs
  const int inp = input_of(p[0]);
  const float g = db2lin(gain_db(p[1]));
  const bool gate_on = p[2] >= 1.0f;
  const float thr = gate_on ? db2lin(gate_db(p[2])) : 0.0f, thr_lo = thr * 0.708f;
  const float hold = hold_ms(p[3]);
  const int hold_frames = (int)(hold * (kSr / 1000.0f));
  const float c_open = tcoef(0.5f), c_close = tcoef(hold * 0.25f + 5.0f), det_rel = 1.0f - tcoef(5.0f);
  const bool trig_mode = mon_of(p[4]) == kMonTrig;
  const float a_step = 1000.0f / (atk_ms(p[5]) * kSr);
  const float dk = expf(-6.90776f * 1000.0f / (dec_ms(p[6]) * kSr)), sus = clampp(p[7]) / 127.0f;
  const float rk = expf(-6.90776f * 1000.0f / (dec_ms(p[8]) * kSr));
  uint32_t tg = __atomic_load_n(&g_trig[vi], __ATOMIC_ACQUIRE), of = __atomic_load_n(&g_off[vi], __ATOMIC_ACQUIRE);
  if (tg != s.seen_trig) s.seen_trig = tg, s.seen_off = of, s.stage = 1;
  if (of != s.seen_off) s.seen_off = of, s.stage = s.stage ? 3 : 0;
  const bool st = stereo && inp == kInStereo && rr;
  float pw = 0.0f;
  for (int i = 0; i < n; ++i) {
    float a, b;
    if (st) a = in[i][0] * g, b = in[i][1] * g;
    else a = b = (inp == kInL ? in[i][0] : inp == kInR ? in[i][1] : 0.5f * (in[i][0] + in[i][1])) * g;
    float m = 0.5f * (a + b);
    pw += m * m;
    float pk = absf(a) > absf(b) ? absf(a) : absf(b);
    if (pk > s.pk) s.pk = pk;
    // the gate
    s.det = pk > s.det ? pk : s.det * det_rel;
    if (!gate_on) s.open = true;
    else if (s.det > thr) s.open = true, s.hold_left = hold_frames;
    else if (s.det < thr_lo) {
      if (s.hold_left > 0) --s.hold_left;
      else s.open = false;
    }
    s.gate_g += ((s.open ? 1.0f : 0.0f) - s.gate_g) * (s.open ? c_open : c_close);
    // TRIG's envelope
    float e = 1.0f;
    if (trig_mode) {
      if (s.stage == 1) {
        s.env += a_step;
        if (s.env >= 1.0f) s.env = 1.0f, s.stage = 2;
      } else if (s.stage == 2) {
        s.env = sus + (s.env - sus) * dk;
        if (sus <= 0.0f && s.env < 1e-4f) s.env = 0.0f, s.stage = 0;
      } else if (s.stage == 3) {
        s.env *= rk;
        if (s.env < 1e-4f) s.env = 0.0f, s.stage = 0;
      }
      e = s.env;
    }
    float k = s.gate_g * e;
    l[i] = a * k;
    if (st) rr[i] = b * k;
    else if (stereo && rr) rr[i] = l[i];
  }
  s.blk_pow = pw / (float)n;
  u.gate_open = s.open;
  s.pk_frames += (uint32_t)n;
  if (s.pk_frames >= 14400) {   // 0.3 s
    u.in_db10 = (int32_t)(s.pk > 1e-5f ? lin2db(s.pk) * 10.0f : -1000.0f);
    s.pk = 0.0f, s.pk_frames = 0;
  }
}

// one click sample (1 kHz, 2 ms, -9 dBFS from its first sample on), int16 units of the voice's return (0.7 headroom)
static inline float click(int k) { return 0.355f * sinf(6.2831853f * 1000.0f * (float)k / kSr + 0.3f) * 0.7f * 32767.0f; }

void post(int vi, int16_t (*ring)[8], int16_t (*ring_r)[8], const int* row, int n) {
  R& s = r[vi];
  Pub& u = pub[vi];
  if (n > kMaxN) n = kMaxN;
  const bool meas = s.mstate != 0;
  const float lim_rel = tcoef(50.0f), kk = 1.0f / (0.7f * 32767.0f);
  bool limited = false;
  float opw = 0.0f;
  for (int i = 0; i < n; ++i) {
    float xl = ring[row[i]][vi], xr = ring_r ? ring_r[row[i]][vi] : xl;
    // the limiter (-1 dBFS, instant attack, 50 ms release), then the guard's gain (a probe's dip must survive it)
    float a = absf(xl) > absf(xr) ? absf(xl) : absf(xr);
    if (a * s.lim > kCeil) s.lim = kCeil / a, limited = true;
    else s.lim += (1.0f - s.lim) * lim_rel;
    s.g += (s.g_target - s.g) * s.g_coef;
    float gg = s.lim * s.g;
    xl *= gg, xr *= gg;
    if (meas) {   // the measure: the voice's own sound muted, its clicks instead
      xl = xr = 0.0f;
      if (s.emit >= 0) {
        xl = xr = click(s.emit);
        if (++s.emit >= kClickLen) s.emit = -1;
      }
    }
    float m = 0.5f * (xl + xr) * kk;
    opw += m * m;
    ring[row[i]][vi] = (int16_t)(xl > 32767.0f ? 32767.0f : xl < -32768.0f ? -32768.0f : xl);
    if (ring_r) ring_r[row[i]][vi] = (int16_t)(xr > 32767.0f ? 32767.0f : xr < -32768.0f ? -32768.0f : xr);
  }
  if (limited) ++u.limited;
  ++u.blocks;
  // the guard: windows of the input's (after GAIN) and the output's power
  if (!meas) {
    float ip = s.blk_pow * (float)n;
    switch (s.gstate) {
      case 0:   // monitor
        s.win_in += ip, s.win_out += opw, s.win_n += (uint32_t)n;
        if (s.win_n >= kWin) {
          float idb = 10.0f * log10f(s.win_in / (float)s.win_n + 1e-18f), odb = 10.0f * log10f(s.win_out / (float)s.win_n + 1e-18f);
          s.cool = s.cool > s.win_n ? s.cool - s.win_n : 0;
          if (idb > s.prev_db + 0.5f) ++s.rise;
          else if (idb < s.prev_db - 1.0f) s.rise = 0;
          s.high = idb > -6.0f ? s.high + 1 : 0;
          s.prev_db = idb;
          s.win_in = s.win_out = 0.0f, s.win_n = 0;
          if (!s.cool && odb > -50.0f && idb > -30.0f && (s.rise >= 4 || s.high >= 6)) {
            s.gstate = 1, s.phase = 0, s.gt = 0, s.nsub = 0, s.sub_acc = 0, s.sub_n = 0;
            s.g_target = 0.5f, s.g_coef = tcoef(2.0f);
            ++u.probes;
          }
        }
        break;
      case 1: {   // a probe: -6 dB for kProbe frames, the input watched in sub-windows after it
        s.gt += (uint32_t)n;
        if (s.gt >= kProbe) s.g_target = 1.0f;
        s.sub_acc += ip, s.sub_n += (uint32_t)n;
        if (s.sub_n >= kSub) s.sub[s.nsub++] = s.sub_acc / (float)s.sub_n, s.sub_acc = 0, s.sub_n = 0;
        if (s.nsub < kSubs) break;
        // the dip: kDipW sub-windows (the probe's length) quieter than the two on each side, by the most
        int best = -1;
        float br = 0.0f;
        for (int j = 2; j + kDipW + 2 <= kSubs; ++j) {
          float dip = 0.0f, side = 0.0f;
          for (int k = 0; k < kDipW; ++k) dip += s.sub[j + k];
          side = s.sub[j - 2] + s.sub[j - 1] + s.sub[j + kDipW] + s.sub[j + kDipW + 1];
          dip /= (float)kDipW, side *= 0.25f;
          float ratio = side / (dip + 1e-18f);
          if (ratio > br) br = ratio, best = j;
        }
        bool dipped = best >= 0 && br > 2.5f;
        if (dipped && s.phase == 0) {   // again: a loop puts the dip at the same place
          s.phase = 1, s.lag0 = best, s.gt = 0, s.nsub = 0, s.sub_acc = 0, s.sub_n = 0;
          s.g_target = 0.5f;
          ++u.probes;
        } else if (dipped && (best - s.lag0 <= 1 && s.lag0 - best <= 1)) {   // a loop: mute
          s.gstate = 2, s.gt = 0, s.g_target = 0.0f, s.g_coef = tcoef(10.0f);
          u.muted = 1;
          ++u.trips;
        } else {
          s.gstate = 0, s.g_target = 1.0f, s.cool = kCoolOk, s.rise = s.high = 0;
          s.win_in = s.win_out = 0.0f, s.win_n = 0;
        }
        break;
      }
      case 2:   // muted
        s.gt += (uint32_t)n;
        if (s.gt >= kMuteFrames) s.gstate = 3, s.gt = 0, s.g_target = 1.0f, s.g_coef = tcoef(150.0f), u.muted = 0;
        break;
      default:   // recovering
        s.gt += (uint32_t)n;
        if (s.gt >= kRecoverFrames) {
          s.gstate = 0, s.g_coef = tcoef(2.0f), s.cool = kCoolTrip, s.rise = s.high = 0;
          s.win_in = s.win_out = 0.0f, s.win_n = 0, s.prev_db = -180.0f;
        }
    }
  }
  s.frame += (uint64_t)n;
}

}  // namespace livein
}  // namespace core
