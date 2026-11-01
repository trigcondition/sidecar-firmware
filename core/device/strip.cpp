// core/device/strip.cpp - see strip.h. Cold: the control core, once a millisecond at most, only for a box with Controls.
#include "device/strip.h"

#include <math.h>
#include <string.h>

#include "fx/insert.h"
#include "fx/out.h"
#include "platform.h"

namespace core {
namespace device {
namespace strip {
namespace {

constexpr int kSlot = 0;                   // INSERT slot A
constexpr int kLpCut = 0, kLpRes = 1, kLpMode = 2, kLpDrv = 3;           // FILTER's params
constexpr int kEqLow = 0, kEqMid = 1, kEqHigh = 2, kEqFreq = 3;          // EQ's
enum Stage : uint8_t { kIdle = 0, kAttack, kHold, kDecay };

struct Voice {
  bool known;                     // a controls report came
  float v[kControls];             // the box's values, 0..127.99
  float a, f;                     // the envelopes, 0..1
  float held;                     // seconds left of HOLD
  uint8_t as, fs;                 // their stages
  bool released;                  // the filter's: after a note-off
  int kind;                       // what slot A is: an ins::Type
  int lev, pan, env;              // what was last sent: -1000 none
  int p[ins::kParams];
};
Voice g_v[8];
uint32_t g_last;
bool g_started, g_ready;

// 0 -> 1 ms, 127 -> 10 s. Ours: the box's real times are not measured.
inline float secs(float v) { return 0.001f * powf(10000.0f, v / 127.0f); }

inline int kind_of(const Controls& c, const Voice& x) {
  int t = (int)x.v[kCtlType] & 7;
  switch (c.type[t]) {
    case kFltLp:
      // open and still: no filter at all
      return x.v[kCtlFreq] >= 127.0f && x.v[kCtlReso] < 1.0f && (int)x.v[kCtlFltEnv] == 64 ? ins::kOff : ins::kFilter;
    case kFltHp: return ins::kFilter;
    case kFltBell: return ins::kEq;
    default: return ins::kOff;
  }
}

inline void hold_param(int voice, Voice& x, int k, int v) {
  if (x.p[k] == v) return;
  x.p[k] = v;
  ins::lock(voice, kSlot, k, v);
}

void filter(const Controls& c, int voice, Voice& x) {
  int kind = kind_of(c, x);
  if (kind != x.kind) {
    x.kind = kind;
    for (int k = 0; k < ins::kParams; ++k) x.p[k] = -1;
    ins::set_type(voice, kSlot, kind, true);
  }
  int freq = (int)x.v[kCtlFreq], res = (int)x.v[kCtlReso];
  if (kind == ins::kFilter) {
    hold_param(voice, x, kLpCut, freq);
    hold_param(voice, x, kLpRes, res);
    hold_param(voice, x, kLpMode, c.type[(int)x.v[kCtlType] & 7] == kFltHp ? 127 : 0);
    hold_param(voice, x, kLpDrv, 0);
  } else if (kind == ins::kEq) {
    hold_param(voice, x, kEqLow, 64);
    hold_param(voice, x, kEqMid, res);    // the box's GAIN sits where RESO is
    hold_param(voice, x, kEqHigh, 64);
    hold_param(voice, x, kEqFreq, freq);
  }
}

void env_out(int voice, Voice& x) {
  if (x.kind != ins::kOff) {
    int off = (int)((x.v[kCtlFltEnv] - 64.0f) * 2.0f * x.f);
    if (off != x.env) {
      x.env = off;
      ins::lfo_offset(voice, kSlot, x.kind == ins::kEq ? kEqFreq : kLpCut, off);
    }
  }
  int lev = (int)(x.v[kCtlVol] * x.a), pan = (int)x.v[kCtlPan];
  if (lev != x.lev) x.lev = lev, out::lfo_offset(voice, 0, lev - 100);
  if (pan != x.pan) x.pan = pan, out::lfo_offset(voice, 1, pan - 64);
}

}  // namespace

CORE_COLD void reset() {
  memset(g_v, 0, sizeof g_v);
  for (Voice& x : g_v) x.a = 1.0f, x.kind = ins::kOff, x.lev = x.pan = x.env = -1000;
  g_started = false, g_ready = true;
}

CORE_COLD void report(const Controls& c, int voice, const uint8_t* d) {
  if (!g_ready) reset();
  Voice& x = g_v[voice & 7];
  x.known = true;
  for (int k = 0; k < c.n; ++k)
    if (c.target[k] < kControls) x.v[c.target[k]] = (float)d[2 * k] + (float)d[2 * k + 1] / 128.0f;
  filter(c, voice & 7, x);
  env_out(voice & 7, x);
}

CORE_COLD void trig(int voice) {
  Voice& x = g_v[voice & 7];
  if (!x.known) return;
  x.as = kAttack, x.fs = kAttack, x.released = false;
  x.held = secs(x.v[kCtlAmpH]);
  x.a = x.v[kCtlAmpA] < 2.0f ? 1.0f : 0.0f;   // no attack (a default sound has ATK 1): from the first sample, at once
  x.f = 0.0f;
}

CORE_COLD void off(int voice) { g_v[voice & 7].released = true; }

CORE_COLD void tick(const Controls& c, uint32_t us) {
  (void)c;
  if (!g_started) {
    g_started = true, g_last = us;
    return;
  }
  uint32_t dus = us - g_last;
  if (dus < 1000) return;
  if (dus > 100000) dus = 100000;
  g_last = us;
  const float dt = (float)dus * 1e-6f;
  for (int i = 0; i < 8; ++i) {
    Voice& x = g_v[i];
    if (!x.known) continue;
    if (x.as == kAttack) {
      x.a += dt / secs(x.v[kCtlAmpA]);
      if (x.a >= 1.0f) x.a = 1.0f, x.as = kHold;
    } else if (x.as == kHold) {
      if (x.v[kCtlAmpH] < 127.0f) {   // 127, the box's NOTE: no gate is reported, it holds
        x.held -= dt;
        if (x.held <= 0.0f) x.as = kDecay;
      }
    } else if (x.as == kDecay && x.v[kCtlAmpD] < 127.0f) {   // 127: it stays
      x.a *= expf(-dt * 6.9078f / secs(x.v[kCtlAmpD]));
      if (x.a < 0.002f) x.a = 0.0f, x.as = kIdle;
    }
    if (x.released) {
      x.f *= expf(-dt * 6.9078f / secs(x.v[kCtlFltR]));
    } else if (x.fs == kAttack) {
      x.f += dt / secs(x.v[kCtlFltA]);
      if (x.f >= 1.0f) x.f = 1.0f, x.fs = kDecay;
    } else if (x.fs == kDecay) {
      float sus = x.v[kCtlFltS] / 127.0f;
      x.f = sus + (x.f - sus) * expf(-dt * 6.9078f / secs(x.v[kCtlFltD]));
    }
    env_out(i, x);
  }
}

float amp_env(int voice) { return g_v[voice & 7].a; }
float flt_env(int voice) { return g_v[voice & 7].f; }

}  // namespace strip
}  // namespace device
}  // namespace core
