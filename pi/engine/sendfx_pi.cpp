// pi/engine/sendfx_pi.cpp - see sendfx_pi.h.
#include "sendfx_pi.h"

#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "platform.h"
#include "spsc.h"

#include <string.h>

#include <new>

namespace sendfx_pi {

struct Stem { float v[8], vs[3]; };   // vs: the Sidecar voices' sends to the buses (fx-ins)
struct Ret { float l, r, pl, pr, s[4]; };   // the PRE pair (onto 2/3) and the POST pair (onto 0/1), each slot's effect and STUTTER on the pair its P1 names
static Spsc<Stem, kRing> s_Stems;
static Spsc<Ret, kRing> s_Rets;
// Built by Init() (CKernel::Initialize, core 0, after the logger), not as a static object: SendFx is 5.9 MB with
// lookup tables and a full clear, which would otherwise run inside sysinit's static constructors, before the first
// log line, with nothing printed if it ever stalled. Its memory is the platform's (Init), reserved once at boot and
// not in .bss (Circle's KERNEL_MAX_SIZE counts .bss); hosted (the Mac tests), Fx() builds it on the heap on first use.
static core::fx::SendFx* volatile s_pFx = nullptr;
static core::fx::SendFx& Fx() {
  if (!s_pFx) s_pFx = new core::fx::SendFx(48000.0f);
  return *s_pFx;
}
// s_nCycles (render core 2's busy ticks) and s_nFrames only ever grow: Command (core 3) reads them and keeps what it saw
// last, so there is no read-and-clear shared with core 2 (a clear lost what core 2 added in between). The busy total is
// 64-bit and the elapsed time milliseconds: core::cycles() is 32-bit and wraps every 79.5 s at 54 MHz, so a "sendfx" a
// few minutes after the last one (or after boot) measured against the wrapped difference - 1-2 % read as ~60 %.
static volatile uint64_t s_nCycles;
static volatile uint32_t s_nFrames, s_nUnder, s_nDropIn, s_nDropOut;
static volatile float s_Peak;
static bool s_bPrimed = false;   // core 0's (AddReturn)
static uint64_t s_nLastBusy;     // core 3's (Command): the busy ticks, frames and clock at the last "sendfx"
static uint32_t s_nLastFrames, s_nLastMs;

unsigned long Bytes() { return sizeof(core::fx::SendFx); }

void Init(void* mem) {
  if (!s_pFx) s_pFx = new (mem) core::fx::SendFx(48000.0f);
}

void Clock(uint8_t status, uint16_t spp) { core::fx::clock_in(status, spp); }

void AllSoundOff() {
  if (core::fx::all_sound_off()) core::log("sendfx: all sound off -> cleared");
}

bool On() { return core::fx::any_on() || core::ins::bus3_on(); }

// core 0 only (the OUT fill writes, the IN completion reads): the voices' bus sends and stem stand-ins waiting for their
// stem frame
struct Vq { float vs[3], st[8]; };
static Vq s_Vs[1024];
static unsigned s_nVsR = 0, s_nVsW = 0;
static uint32_t s_nVsDrop = 0;
void FromVoices(const int16_t (*v)[8], unsigned n, const float (*feed)[8]) {
  if (!On()) return;
  // (a voice off TRK queues every frame, silent ones too: the queue's depth - the lag - stays the stems')
  bool sends = core::ins::sends_on(), fd = feed && core::out::any_stereo();
  if (!sends && !fd) return;
  float o[16][3];
  while (n) {
    unsigned k = n > 16 ? 16 : n;
    if (sends) core::ins::voice_sends(v, (int)k, o);
    for (unsigned i = 0; i < k; ++i) {
      if (s_nVsW - s_nVsR >= kVoiceLag) ++s_nVsR, ++s_nVsDrop;   // a backlog: the oldest goes
      Vq& d = s_Vs[s_nVsW++ & 1023];
      for (int b = 0; b < 3; ++b) d.vs[b] = sends ? o[i][b] : 0.0f;
      for (int s = 0; s < 8; ++s) d.st[s] = fd ? feed[i][s] : 0.0f;
    }
    v += k, n -= k;
    if (feed) feed += k;
  }
}

void Read(View* v) {
  for (int k = 0; k < 2; ++k) {
    const char* n = core::fx::type_name(core::fx::control.knob[k][0], k);
    unsigned i = 0;
    for (; n && n[i] && i < sizeof v->type[k] - 1; ++i) v->type[k][i] = n[i];
    v->type[k][i] = 0;
    v->post[k] = core::fx::control.route == core::fx::kRouteMain || (core::fx::control.route == core::fx::kRouteSlot && core::fx::control.knob[k][1] >= 64);
    for (int t = 0; t < 12; ++t) v->send[k][t] = core::fx::control.send[t][k];
  }
}

void FromRytm(const int32_t (*stems)[8], unsigned n) {
  if (!On()) return;
  for (unsigned f = 0; f < n; ++f) {
    Stem s;
    for (int v = 0; v < 8; ++v) s.v[v] = (float)stems[f][v] * (1.0f / 2147483648.0f);
    if (s_nVsR != s_nVsW) {
      const Vq& d = s_Vs[s_nVsR++ & 1023];
      s.vs[0] = d.vs[0], s.vs[1] = d.vs[1], s.vs[2] = d.vs[2];
      for (int v = 0; v < 8; ++v) s.v[core::out::device().stem[v]] += d.st[v];   // a PRE / POST voice in its silent stem's place
    } else {
      s.vs[0] = s.vs[1] = s.vs[2] = 0.0f;
    }
    if (!s_Stems.push(s)) s_nDropIn = s_nDropIn + 1;
  }
}

void Render() {
  unsigned n = s_Stems.size();
  if (n == 0) return;
  if (n > kMaxPerRender) n = kMaxPerRender;
  uint32_t t0 = core::cycles();
  float st[core::fx::SendFx::kMaxFrames][8], o[core::fx::SendFx::kMaxFrames][2], m[core::fx::SendFx::kMaxFrames][2];   // m: the POST pair
  float sl[core::fx::SendFx::kMaxFrames][4];   // each FX's own return (the recorder's FX1/FX2)
  float vs[core::fx::SendFx::kMaxFrames][3], b3[core::fx::SendFx::kMaxFrames];   // fx-ins: the voices' bus sends
  unsigned k = 0;
  Stem s;
  bool anyv = false;
  while (k < n && s_Stems.pop(&s)) {
    for (int v = 0; v < 8; ++v) st[k][v] = s.v[v];
    vs[k][0] = s.vs[0], vs[k][1] = s.vs[1], vs[k][2] = b3[k] = s.vs[2];
    anyv |= s.vs[0] != 0.0f || s.vs[1] != 0.0f || s.vs[2] != 0.0f;
    ++k;
  }
  Fx().Process(st, (int)k, o, core::fx::control, m, sl, anyv ? vs : nullptr);
  core::ins::bus3(b3, (int)k, o, m);   // bus 3: the dry bus's chain onto its pair (nothing while off)
  float pk = s_Peak;
  for (unsigned i = 0; i < k; ++i) {
    if (!s_Rets.push(Ret{o[i][0], o[i][1], m[i][0], m[i][1], {sl[i][0], sl[i][1], sl[i][2], sl[i][3]}})) s_nDropOut = s_nDropOut + 1;
    float a = o[i][0] < 0 ? -o[i][0] : o[i][0], b = m[i][0] < 0 ? -m[i][0] : m[i][0];
    if (b > a) a = b;
    if (a > pk) pk = a;
  }
  s_Peak = pk;
  s_nFrames = s_nFrames + k;
  s_nCycles = s_nCycles + (uint32_t)(core::cycles() - t0);   // (one block's ticks: far below the 32-bit wrap)
}

static int32_t Add(int32_t a, float x) {
  int64_t s = (int64_t)a + (int64_t)(x * 2147483648.0f);
  return s > 2147483647 ? 2147483647 : s < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)s;
}

void AddReturn(int32_t (*out)[12], unsigned n, float (*slots)[4]) {
  if (!s_bPrimed) {
    if (s_Rets.size() < kPrime) return;
    s_bPrimed = true;
  }
  for (unsigned f = 0; f < n; ++f) {
    Ret r;
    if (!s_Rets.pop(&r)) {   // ran dry: silence, and prime again
      s_nUnder = s_nUnder + 1;
      s_bPrimed = false;
      return;
    }
    if (slots) slots[f][0] = r.s[0], slots[f][1] = r.s[1], slots[f][2] = r.s[2], slots[f][3] = r.s[3];
    out[f][2] = Add(out[f][2], r.l);   // PRE: MAIN FX, through the analog distortion + compressor
    out[f][3] = Add(out[f][3], r.r);
    if (r.pl != 0.0f || r.pr != 0.0f) out[f][0] = Add(out[f][0], r.pl), out[f][1] = Add(out[f][1], r.pr);   // POST: MAIN
  }
}

static uint32_t Take(volatile uint32_t* p) {
  uint32_t v = *p;
  *p = 0;
  return v;
}

bool Command(const char* p) {
  char r[400];
  if (strcmp(p, "killfx") == 0) {   // the display's "kill tails"
    core::log(core::fx::all_sound_off() ? "sendfx: kill tails -> cleared" : "sendfx: kill tails ignored (a request within 100 ms)");
    return true;
  }
  if (!core::fx::command(p, r, sizeof r)) return false;
  core::log("%s", r);
  if (p[6] == 0) {   // "sendfx": the stats since the last one
    uint32_t ms = core::now_ms(), dt_ms = ms - s_nLastMs, fr = s_nFrames, frames = fr - s_nLastFrames;
    uint64_t total = s_nCycles, busy = total - s_nLastBusy;
    s_nLastMs = ms, s_nLastFrames = fr, s_nLastBusy = total;
    uint64_t window = (uint64_t)dt_ms * core::cycles_per_second() / 1000;   // the window in ticks (no 32-bit wrap)
    unsigned load = window ? (unsigned)(busy * 100000 / window) : 0;        // 1/1000 %
    unsigned ns = frames ? (unsigned)((double)busy * 1e9 / core::cycles_per_second() * 12 / frames) : 0;   // (a long window: busy x 1e9 passes 2^64)
    unsigned pk = (unsigned)(s_Peak * 1000.0f);
    s_Peak = 0.0f;
    core::log("sendfx: %u frames, core 2 load %u.%03u percent, %u ns a 12-frame block, underruns %u, drops in %u out %u, "
              "peak %u/1000, faults %u",
              frames, load / 1000, load % 1000, ns, Take(&s_nUnder), Take(&s_nDropIn), Take(&s_nDropOut), pk,
              Fx().Faults());
  }
  return true;
}

}  // namespace sendfx_pi
