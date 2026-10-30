// core/fx/out_measure.cpp - see out_measure.h.
#include "fx/out_measure.h"

#include <math.h>
#include <string.h>


namespace core {
namespace out {
namespace meas {

namespace {

constexpr float kImp = 0.15f;              // the impulse (below the POST pair's knee)
constexpr uint32_t kImpAt = 2400;          // frames into an impulse phase it goes out (50 ms)

volatile uint8_t g_req = 0;                // control -> core 0: 1 start, 2 start with trk, 3 stop
volatile uint8_t g_phase = kIdle;
int g_voice = 0;
bool g_trk = false;
Result g_res;

// core 0's
uint32_t o_n = 0;                          // OUT frames into the phase
uint64_t o_total = 0, i_total = 0;         // frames since the start, each side
uint64_t emit_out[2];                      // the impulses' OUT frame (since the start)
uint8_t i_phase = kIdle;
uint32_t i_n = 0;                          // IN frames into the phase (as the IN side saw it)
double gc = 0, gs = 0;                     // Goertzel-free: the 1 kHz correlation (cos, sin) over the measured half
uint32_t gn = 0;
float* imp_buf = nullptr;                  // an impulse phase's |MAIN IN| (kImpLen; allocated at the first start)
uint64_t imp_t0 = 0;                       // its first IN frame (since the start)
uint32_t imp_n = 0;

float db(float a) { return a > 1e-9f ? 20.0f * log10f(a) : -180.0f; }

Phase next(Phase p) {
  switch (p) {
    case kFloor: return kTonePost;
    case kTonePost: return kTonePre;
    case kTonePre: return g_trk ? kToneTrk : kImpPost;
    case kToneTrk: return kImpPost;
    case kImpPost: return g_trk ? kImpTrk : kDone;
    default: return kDone;
  }
}
uint32_t len(int p) { return p == kFloor ? kNoise : p == kImpPost || p == kImpTrk ? kImpLen : kToneLen; }

// the IN side closes a phase: its measurement into the result
void close_in(int p) {
  if (p == kFloor || p == kTonePost || p == kTonePre || p == kToneTrk) {
    float a = gn ? 2.0f * (float)sqrt(gc * gc + gs * gs) / (float)gn : 0.0f;   // the 1 kHz amplitude
    if (p == kFloor) g_res.floor_db = db(a);
    else {
      int k = p == kTonePost ? 0 : p == kTonePre ? 1 : 2;
      g_res.gain_db[k] = db(a) - db(kLevel);
      g_res.heard[k] = db(a) > g_res.floor_db + 20.0f;
    }
  } else if (p == kImpPost || p == kImpTrk) {
    int k = p == kImpPost ? 0 : 1;
    float pk = 0;
    for (uint32_t i = 0; i < imp_n; ++i) pk = imp_buf[i] > pk ? imp_buf[i] : pk;
    g_res.delay[k] = -1;
    if (pk > kImp * 0.01f && db(pk) > g_res.floor_db + 20.0f)
      for (uint32_t i = 0; i < imp_n; ++i)
        if (imp_buf[i] >= 0.5f * pk) {
          g_res.delay[k] = (int32_t)((imp_t0 + i) - emit_out[k]);
          break;
        }
  }
  gc = gs = 0, gn = 0, imp_n = 0;
}

}  // namespace

void start(bool trk, int voice) {
  if (!imp_buf) imp_buf = new float[kImpLen];
  g_voice = voice & 7;
  g_req = trk ? 2 : 1;
}
void stop() { g_req = 3; }
Phase phase() { return (Phase)g_phase; }
const Result& result() { return g_res; }
// (after the OUT side's last phase, until the IN side has closed it)
bool active() { return g_req != 0 || (g_phase != kIdle && !(g_phase == kDone && g_res.done)); }

bool out_frame(float* pre, float* post, float* trk, int* trk_voice) {
  uint8_t rq = g_req;
  if (rq) {
    g_req = 0;
    if (rq == 3) {
      g_phase = kIdle;
    } else {
      memset(&g_res, 0, sizeof g_res);
      g_res.delay[0] = g_res.delay[1] = -1;
      g_trk = rq == 2, g_res.trk = g_trk;
      g_phase = kFloor, o_n = 0, o_total = i_total = 0, i_phase = kIdle, i_n = 0;
      emit_out[0] = emit_out[1] = 0;
    }
  }
  *pre = *post = *trk = 0.0f;
  *trk_voice = g_voice;
  uint8_t p = g_phase;
  if (p == kIdle || p == kDone) return false;
  float t = kLevel * sinf(6.2831853f * kTone * (float)o_n / 48000.0f);
  if (p == kTonePost) *post = t;
  else if (p == kTonePre) *pre = t;
  else if (p == kToneTrk) *trk = t;
  else if ((p == kImpPost || p == kImpTrk) && o_n == kImpAt) {
    emit_out[p == kImpPost ? 0 : 1] = o_total;
    if (p == kImpPost) *post = kImp;
    else *trk = kImp;
  }
  ++o_total;
  if (++o_n >= len(p)) {
    o_n = 0;
    g_phase = next((Phase)p);
  }
  return true;
}

void in_frame(int32_t l, int32_t r) {
  uint8_t p = g_phase;
  if (p == kIdle) return;
  if (p != i_phase) {   // the OUT side moved on: the last phase measured, the next one begins
    if (i_phase != kIdle && i_phase != kDone) close_in(i_phase);
    i_phase = p, i_n = 0;
    if (p == kDone) g_res.done = true;
  }
  if (p == kDone) return;
  float x = ((float)l + (float)r) * (0.5f / 2147483648.0f);
  if (p == kImpPost || p == kImpTrk) {   // (from halfway to the impulse: the last phase's tail gone by then)
    if (i_n >= kImpAt / 2) {
      if (!imp_n) imp_t0 = i_total;
      if (imp_buf && imp_n < kImpLen) imp_buf[imp_n++] = x < 0 ? -x : x;
    }
  } else if (i_n >= len(p) / 2) {   // the tone's (the floor's) second half: past any latency
    double ph = 6.283185307179586 * kTone * (double)i_total / 48000.0;
    gc += x * cos(ph), gs += x * sin(ph), ++gn;
  }
  ++i_n, ++i_total;
}

void reset() {
  g_req = 0, g_phase = kIdle;
  memset(&g_res, 0, sizeof g_res);
  i_phase = kIdle, i_n = 0, o_n = 0, gn = 0, imp_n = 0;
}

}  // namespace meas
}  // namespace out
}  // namespace core
