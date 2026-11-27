// pi/engine/masterfx_pi.cpp - see masterfx_pi.h.
#include "masterfx_pi.h"

#include <string.h>

#include "fx/insert.h"
#include "fx/limiter.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/out_measure.h"
#include "fx/out_mix.h"
#include "platform.h"
#include "spsc.h"

namespace masterfx_pi {

struct Fr { float l, r; };
// One per box (two links, phase 3(d)): box 0's is what a lone Rytm always had; box 1's (16 voices) its own stream's
struct Inst {
  Spsc<Fr, kRing> In, Out;
  // core 0's
  float fWet = 0.0f, fStem = 0.0f;
  bool bPrimed = false, bRun = false;
  float Stem[kRing][2];
  float Gain[8][2];
  float fTrim = 1.0f;          // the stem trim, linear
  int nTrim = -1;              // ... of this many dB down
  core::fx::Limiter Lim;
  float fLim = 0.0f, fLowWin = 1.0f;   // the limiter's share of 0/1; its lowest gain in this second
  unsigned nLook = 0, nLowFrames = 0;  // frames in its delay (not 0: it runs); this second's so far
  unsigned nStR = 0, nStW = 0;
  bool bStPrimed = false, bGains = false;
  volatile uint32_t nFrames, nUnder, nDropIn, nDropOut, nStemUnder;
  volatile uint64_t nCycles;   // core 2's busy ticks (as sendfx_pi's: only ever grows)
};
static constexpr int kBoxes = core::master::kBoxes;
static Inst s_I[kBoxes];
static inline Inst& I(int box) { return s_I[box > 0 && box < kBoxes ? box : 0]; }
static uint64_t s_nLastBusy[kBoxes];  // core 3's (Command): each box's last mfx=stats
static uint32_t s_nLastFrames[kBoxes], s_nLastMs[kBoxes];

static inline int32_t Sat(int64_t s) { return s > 2147483647 ? 2147483647 : s < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)s; }
static inline int32_t Add(int32_t a, float x) { return Sat((int64_t)a + (int64_t)(x * 2147483648.0f)); }
static inline float Fl(int32_t a) { return (float)a * (1.0f / 2147483648.0f); }

bool WantStems() { return WantStems(0); }
bool WantStems(int box) { return core::master::control_of(box).src == core::master::kMix || I(box).fStem > 0.0f; }

// a stem's gains on L, R, from the Rytm's report: its pan law's linear split, 0 while it is still on main (it sounds there)
static inline void Target(const Inst& x, const core::master::Control& c, int v, float* t) {
  const float trim = (v == 5 || v == 6 ? kStemHotTrim : 1.0f) * x.fTrim;
  if (!c.known) {
    t[0] = t[1] = trim;
  } else if (!((c.off_main >> v) & 1)) {
    t[0] = t[1] = 0.0f;
  } else {
    const int p = c.pan[v];
    const float r = p <= 64 ? (float)p / 64.0f : 1.0f + (float)(p - 64) / 63.0f;   // 0 .. 1 at centre .. 2
    t[0] = (2.0f - r) * trim, t[1] = r * trim;
  }
}

void Stems(const int32_t (*stems)[8], unsigned n) { Stems(0, stems, n); }

void Stems(int box, const int32_t (*stems)[8], unsigned n) {
  Inst& x = I(box);
  const auto& c = core::master::control_of(box);   // its slot's: its SRC, TRIM and its report (master::rytm())
  float g[8][2], d[8][2];
  if (c.trim != x.nTrim) {   // (the gains' own ramp below smooths it)
    x.nTrim = c.trim;
    x.fTrim = core::out::Mixer::Trim(-10 * x.nTrim);
  }
  const float lim = 2.0f / (float)kGainFrames * (float)n;   // a block's largest move
  for (int v = 0; v < 8; ++v) {
    float t[2];
    Target(x, c, v, t);
    for (int c = 0; c < 2; ++c) {
      if (!x.bGains) x.Gain[v][c] = t[c];
      g[v][c] = x.Gain[v][c];
      float m = t[c] - g[v][c];
      m = m > lim ? lim : m < -lim ? -lim : m;
      x.Gain[v][c] = g[v][c] + m;
      d[v][c] = m / (float)n;
    }
  }
  x.bGains = true;
  for (unsigned f = 0; f < n; ++f) {
    float l = 0.0f, r = 0.0f;
    for (int v = 0; v < 8; ++v) {
      const float x = Fl(stems[f][v]);
      g[v][0] += d[v][0], g[v][1] += d[v][1];
      l += x * g[v][0], r += x * g[v][1];
    }
    if (x.nStW - x.nStR >= kRing) ++x.nStR;   // a backlog: the oldest goes
    float* o = x.Stem[x.nStW++ & (kRing - 1)];
    o[0] = l, o[1] = r;
  }
}

// SRC MIX: a frame of the stems (at their level in the mix: kStemOnPost) onto l, r, in and out over kFadeFrames. Not clipped
// here: the limiter is the only limit they meet, and they come in only once it is in
static inline void AddStems(Inst& z, float& l, float& r, float t) {
  float x = 0.0f, y = 0.0f;
  if (z.bStPrimed) {
    if (z.nStR != z.nStW) {
      const float* i = z.Stem[z.nStR++ & (kRing - 1)];
      x = i[0], y = i[1];
    } else {
      z.bStPrimed = false, z.nStemUnder = z.nStemUnder + 1;
    }
  }
  z.fStem += t > z.fStem ? 1.0f / kFadeFrames : t < z.fStem ? -1.0f / kFadeFrames : 0.0f;
  z.fStem = z.fStem < 0.0f ? 0.0f : z.fStem > 1.0f ? 1.0f : z.fStem;
  l += x * kStemOnPost * z.fStem, r += y * kStemOnPost * z.fStem;
}

void Process(int32_t (*out)[12], unsigned n) {
  Process(0, core::out::kRytmMk2, out, n);   // the Rytm's pairs (the calls without a box)
}

void Process(int box, const core::out::Device& dev, int32_t (*out)[12], unsigned n) {
  Inst& x = I(box);
  const auto& c = core::master::control_of(box);
  // the pairs: dev's POST (the Rytm's 0/1) and its PRE (2/3), which is moved onto POST; a box with no PRE pair of its own
  // (pre == post: the A4, a stereo mix) has only POST
  const int p0 = dev.post[0], p1 = dev.post[1], q0 = dev.pre[0], q1 = dev.pre[1];
  const bool hasPre = q0 != p0 || q1 != p1;
  // stereo=measure reads the plain return, so all of this stands aside for it whatever BYP says
  // SRC MIX: the Rytm's stems, on its stream (dev is its row) whichever slot it holds; another box has no stems to mix
  const bool rytm = &dev == &core::out::kRytmMk2;
  const bool on = core::master::on(box), meas = box == core::out::meas::slot() && core::out::meas::active(), mix = rytm && c.src == core::master::kMix;
  const bool chain = on && !c.byp && !meas;
  // The limiter is first in and last out: the stems and the chain's output, neither of them clipped, reach 0/1 only with it
  // fully in, so its crossfade has nothing over full scale on the direct side, and BYP and SRC RET still end bit for bit
  const bool lim = (mix && !meas) || chain || x.fStem > 0.0f || x.fWet > 0.0f;
  const bool run = on || x.fWet > 0.0f, st = mix || x.fStem > 0.0f;
  if (x.bRun && !run) {
    x.bRun = x.bPrimed = false;
    Fr y;   // what is left in the rings: from core 0's side only the way back
    while (x.Out.pop(&y)) {
    }
  }
  if (!lim && x.fLim <= 0.0f) {
    if (x.nLook) x.nLook = 0, x.Lim.Reset(), core::master::lim_gain_of(box) = 1.0f, x.fLowWin = 1.0f, x.nLowFrames = 0;
    if (!run && !st) return;   // nothing set: no work, not a sample touched
  }
  const bool in = x.fLim >= 1.0f;
  const float ts = mix && !meas && in ? 1.0f : 0.0f, tw = chain && in ? 1.0f : 0.0f, tl = lim ? 1.0f : 0.0f;
  if (st && !x.bStPrimed && x.nStW - x.nStR >= kStemPrime) x.bStPrimed = true;
  if (run) {
    x.bRun = true;
    if (!x.bPrimed && x.Out.size() >= kPrime) x.bPrimed = true;
  }
  float w = x.fWet, u = x.fLim;
  for (unsigned f = 0; f < n; ++f) {
    float l = Fl(out[f][p0]), r = Fl(out[f][p1]);
    if (st) AddStems(x, l, r, ts);
    if (run) {
      Fr i{hasPre ? l + kPreOnPost * Fl(out[f][q0]) : l, hasPre ? r + kPreOnPost * Fl(out[f][q1]) : r};
      if (!x.In.push(i)) x.nDropIn = x.nDropIn + 1;
      Fr y{0.0f, 0.0f};
      bool have = false;
      if (x.bPrimed) {
        have = x.Out.pop(&y);
        if (!have) {   // ran dry: silence on the wet side, counted, and prime again
          x.nUnder = x.nUnder + 1;
          x.bPrimed = false;
        }
      }
      if (x.bPrimed) w += tw > w ? 1.0f / kFadeFrames : tw < w ? -1.0f / kFadeFrames : 0.0f;
      w = w < 0.0f ? 0.0f : w > 1.0f ? 1.0f : w;
      if (w > 0.0f) {   // the dry share of both pairs, the wet share on 0/1. At 0 the channels are not written at all
        const float d = 1.0f - w;
        l = l * d + y.l * w, r = r * d + y.r * w;
        if (hasPre) out[f][q0] = (int32_t)((float)out[f][q0] * d), out[f][q1] = (int32_t)((float)out[f][q1] * d);
      }
    }
    if (lim || u > 0.0f) {   // last on 0/1: what the fade crosses is the direct signal and the same, kLook later
      float a = l, b = r;
      x.Lim.Frame(a, b);
      if (x.nLook < core::fx::Limiter::kLook) ++x.nLook;   // the fade starts with the delay full: no dip
      else u += tl > u ? 1.0f / kLimFade : tl < u ? -1.0f / kLimFade : 0.0f;
      u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
      if (u > 0.0f) {
        const float d = 1.0f - u;
        out[f][p0] = Add(0, l * d + a * u), out[f][p1] = Add(0, r * d + b * u);
      }
    }
  }
  x.fWet = w, x.fLim = u;
  if (x.nLook) {   // the lowest gain, held for the slow readers: /status and mfx=stats
    const float m = x.Lim.TakeLow();
    if (m < x.fLowWin) x.fLowWin = m;
    if (m < core::master::lim_gain_of(box)) core::master::lim_gain_of(box) = m;
    if ((x.nLowFrames += n) >= 48000) core::master::lim_gain_of(box) = x.fLowWin, x.fLowWin = 1.0f, x.nLowFrames = 0;
  }
}

void Render() { Render(0); }

void Render(int box) {
  Inst& z = I(box);
  unsigned n = z.In.size();
  if (n == 0) return;
  if (n > kMaxPerRender) n = kMaxPerRender;
  float y[kMaxPerRender][2];
  unsigned k = 0;
  Fr x;
  while (k < n && z.In.pop(&x)) y[k][0] = x.l, y[k][1] = x.r, ++k;
  const uint32_t t0 = core::cycles();
  core::master::render(box, y, (int)k);
  z.nCycles = z.nCycles + (uint32_t)(core::cycles() - t0);
  for (unsigned i = 0; i < k; ++i)
    if (!z.Out.push(Fr{y[i][0], y[i][1]})) z.nDropOut = z.nDropOut + 1;
  z.nFrames = z.nFrames + k;
}

bool Command(const char* p) {
  if (strcmp(p, "mfx=stats") != 0) return false;
  // 3(c): the slot in hand's box (as the master's settings code, master::cur()): its own chain's counters
  const int b = core::ins::slot_in_hand() < kBoxes ? core::ins::slot_in_hand() : 0;
  const Inst& x = I(b);
  uint32_t ms = core::now_ms(), dt = ms - s_nLastMs[b], fr = x.nFrames, frames = fr - s_nLastFrames[b];
  uint64_t total = x.nCycles, busy = total - s_nLastBusy[b];
  s_nLastMs[b] = ms, s_nLastFrames[b] = fr, s_nLastBusy[b] = total;
  uint64_t window = (uint64_t)dt * core::cycles_per_second() / 1000;
  unsigned load = window ? (unsigned)(busy * 100000 / window) : 0;   // 1/1000 %
  unsigned gr = (unsigned)core::master::gr10(b);
  core::log("mfx: %s%u frames, core %u load %u.%03u percent, wet %u %%, underruns %u, drops in %u out %u, stems short %u, limiter %u.%u dB",
            b ? "slot 1: " : "", frames, b ? 1u : 2u,
            load / 1000, load % 1000, (unsigned)(x.fWet * 100.0f), x.nUnder, x.nDropIn, x.nDropOut, x.nStemUnder, gr / 10, gr % 10);
  return true;
}

Stats Read() { return Read(0); }
Stats Read(int box) {
  const Inst& x = I(box);
  return Stats{x.nFrames, x.nUnder, x.nDropIn, x.nDropOut, x.nStemUnder};
}
float Wet() { return Wet(0); }
float Wet(int box) { return I(box).fWet; }

void Reset() { Reset(0); }

void Reset(int box) {
  Inst& z = I(box);
  Fr x;
  while (z.Out.pop(&x)) {   // (In is core 2's to empty: a few old frames reach the way back and are crossfaded past)
  }
  z.fWet = z.fStem = z.fLim = 0.0f, z.bPrimed = z.bRun = false;
  z.nStR = z.nStW = 0, z.bStPrimed = z.bGains = false;
  z.Lim.Reset(), z.nLook = z.nLowFrames = 0, z.fLowWin = 1.0f, core::master::lim_gain_of(box) = 1.0f;
}

}  // namespace masterfx_pi
