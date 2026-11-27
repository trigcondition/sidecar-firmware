// pi/engine/sendfx_pi.cpp - see sendfx_pi.h.
#include "sendfx_pi.h"

#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "platform.h"
#include "spectrum_pi.h"
#include "spsc.h"

#include <string.h>

#include <new>

namespace sendfx_pi {

static constexpr int kBoxes = core::kVoiceSlots;   // instances: 2 on 16 voices, 1 on 8 (where nothing calls box 1)

struct Stem { float v[8], vs[3]; };   // vs: the Sidecar voices' sends to the buses (fx-ins)
struct Ret { float l, r, pl, pr, s[4]; };   // the PRE pair (onto 2/3) and the POST pair (onto 0/1), each slot's effect and STUTTER on the pair its P1 names
// core 0 only (the OUT fill writes, the IN completion reads): the voices' bus sends and stem stand-ins waiting for their
// stem frame
struct Vq { float vs[3], st[8]; };
// One per box (two links, phase 3(d)): the Rytm's (slot 0 or 1) is what a lone Rytm always had, its stems pace it (FromRytm);
// another box's has no stems (FromVoices paces it)
struct Inst {
  Spsc<Stem, kRing> Stems;
  volatile float BusPk[3];   // the buses' peaks since TakeBusPeaks (core 2 writes, core 0 takes: racy floats, a meter)
  Spsc<Ret, kRing> Rets;
  core::fx::SendFx* volatile pFx;
  // nCycles (its render core's busy ticks: box 0 core 2, box 1 core 1, cores.cpp ChainCore) and nFrames only ever grow (see Command)
  volatile uint64_t nCycles;
  volatile uint32_t nFrames, nUnder, nDropIn, nDropOut;
  volatile float Peak;
  bool bPrimed;   // core 0's (AddReturn)
  Vq Vs[1024];
  unsigned nVsR, nVsW;
  uint32_t nVsDrop;
};
static Inst s_I[kBoxes];
static inline Inst& I(int box) { return s_I[box > 0 && box < kBoxes ? box : 0]; }
// Built by Init() (CKernel::Initialize, core 0, after the logger), not as a static object: SendFx is 5.9 MB with
// lookup tables and a full clear, which would otherwise run inside sysinit's static constructors, before the first
// log line, with nothing printed if it ever stalled. Its memory is the platform's (Init), reserved once at boot and
// not in .bss (Circle's KERNEL_MAX_SIZE counts .bss); hosted (the Mac tests), Fx() builds it on the heap on first use.
// The SendFx: built by Init() (CKernel::Initialize, core 0, after the logger), not as a static object: SendFx is 5.9 MB with
// lookup tables and a full clear, which would otherwise run inside sysinit's static constructors, before the first
// log line, with nothing printed if it ever stalled. Its memory is the platform's (Init), reserved once at boot and
// not in .bss (Circle's KERNEL_MAX_SIZE counts .bss); hosted (the Mac tests), Fx() builds it on the heap on first use.
static core::fx::SendFx& Fx(Inst& x) {
  if (!x.pFx) x.pFx = new core::fx::SendFx(48000.0f);
  return *x.pFx;
}
// nCycles (the render core's busy ticks) and nFrames only ever grow: Command (core 3) reads them and keeps what it saw
// last, so there is no read-and-clear shared with core 2 (a clear lost what core 2 added in between). The busy total is
// 64-bit and the elapsed time milliseconds: core::cycles() is 32-bit and wraps every 79.5 s at 54 MHz, so a "sendfx" a
// few minutes after the last one (or after boot) measured against the wrapped difference - 1-2 % read as ~60 %.
static uint64_t s_nLastBusy[kBoxes];   // core 3's (Command): the busy ticks, frames and clock at each box's last "sendfx"
static uint32_t s_nLastFrames[kBoxes], s_nLastMs[kBoxes];

static constexpr unsigned long kFxBytes = ((unsigned long)sizeof(core::fx::SendFx) + 63) & ~63ul;
unsigned long Bytes() { return kBoxes * kFxBytes; }

void Init(void* mem) {
  for (int b = 0; b < kBoxes; ++b)
    if (!s_I[b].pFx) s_I[b].pFx = new ((char*)mem + b * kFxBytes) core::fx::SendFx(48000.0f);
}

core::fx::SendFx* Instance(int box) { return &Fx(I(box)); }

void Clock(uint8_t status, uint16_t spp) { ClockBox(0, status, spp); }
void ClockBox(int box, uint8_t status, uint16_t spp) { core::fx::clock_in(core::fx::control_of(box), status, spp); }

void AllSoundOff() { AllSoundOff(0); }
void AllSoundOff(int box) {
  if (core::fx::all_sound_off(core::fx::control_of(box))) core::log("sendfx: all sound off -> cleared%s", box ? " (slot 1)" : "");
}

bool On() { return On(0); }
bool On(int box) { return core::fx::any_on(core::fx::control_of(box)) || core::ins::bus3_on(box); }

static void Voices(int box, bool stems, const int16_t (*v)[8], unsigned n, const float (*feed)[8]);
void FromVoices(const int16_t (*v)[8], unsigned n, const float (*feed)[8]) { Voices(0, true, v, n, feed); }
void FromVoices(int box, const int16_t (*v)[8], unsigned n, const float (*feed)[8]) { Voices(box, box == 0, v, n, feed); }
void FromVoices(int box, const core::out::Device& dev, const int16_t (*v)[8], unsigned n, const float (*feed)[8]) {
  Voices(box, &dev == &core::out::kRytmMk2, v, n, feed);
}

// stems: box is the Rytm's (its stems pace it, FromRytm), else its voices' sends are its input
static void Voices(int box, bool stems, const int16_t (*v)[8], unsigned n, const float (*feed)[8]) {
  if (!On(box)) return;
  Inst& x = I(box);
  if (!stems) {   // no stems: each frame of the voices' sends is the send FX's input frame (silent ones too: the tails)
    const bool sends = core::ins::sends_on(box);
    float o[16][3];
    while (n) {
      unsigned k = n > 16 ? 16 : n;
      if (sends) core::ins::voice_sends(box, v, (int)k, o);
      for (unsigned i = 0; i < k; ++i) {
        Stem s;
        for (int c = 0; c < 8; ++c) s.v[c] = 0.0f;
        for (int b = 0; b < 3; ++b) s.vs[b] = sends ? o[i][b] : 0.0f;
        if (!x.Stems.push(s)) x.nDropIn = x.nDropIn + 1;
      }
      v += k, n -= k;
    }
    return;
  }
  // (a voice off TRK queues every frame, silent ones too: the queue's depth - the lag - stays the stems')
  bool sends = core::ins::sends_on(box), fd = feed && core::out::any_stereo(box);
  if (!sends && !fd) return;
  float o[16][3];
  while (n) {
    unsigned k = n > 16 ? 16 : n;
    if (sends) core::ins::voice_sends(box, v, (int)k, o);
    for (unsigned i = 0; i < k; ++i) {
      if (x.nVsW - x.nVsR >= kVoiceLag) ++x.nVsR, ++x.nVsDrop;   // a backlog: the oldest goes
      Vq& d = x.Vs[x.nVsW++ & 1023];
      for (int b = 0; b < 3; ++b) d.vs[b] = sends ? o[i][b] : 0.0f;
      for (int s = 0; s < 8; ++s) d.st[s] = fd ? feed[i][s] : 0.0f;
    }
    v += k, n -= k;
    if (feed) feed += k;
  }
}

void Read(View* v) { Read(0, v); }

void Read(int box, View* v) {
  const core::fx::Control& c = core::fx::control_of(box);
  for (int k = 0; k < 2; ++k) {
    const char* n = core::fx::type_name_in(box, c.knob[k][0]);   // (its own user effects' names)
    unsigned i = 0;
    for (; n && n[i] && i < sizeof v->type[k] - 1; ++i) v->type[k][i] = n[i];
    v->type[k][i] = 0;
    v->post[k] = c.route == core::fx::kRouteMain || (c.route == core::fx::kRouteSlot && c.knob[k][1] >= 64);
    for (int t = 0; t < 12; ++t) v->send[k][t] = c.send[t][k];
    int b, w;
    core::ins::eq_bw(core::ins::kBus0 + k, &b, &w);   // (slot 0's knobs: 3(c) gives slot 1 its own)
    v->bw[k][0] = (uint8_t)b, v->bw[k][1] = (uint8_t)w;
  }
}

Stats ReadStats(int box) {
  const Inst& x = I(box);
  return Stats{x.nFrames, x.nUnder, x.nDropIn, x.nDropOut, On(box)};
}

void FromRytm(const int32_t (*stems)[8], unsigned n) { FromRytm(0, stems, n); }

void FromRytm(int box, const int32_t (*stems)[8], unsigned n) {
  if (!On(box)) return;
  Inst& x = I(box);
  for (unsigned f = 0; f < n; ++f) {
    Stem s;
    for (int v = 0; v < 8; ++v) s.v[v] = (float)stems[f][v] * (1.0f / 2147483648.0f);
    if (x.nVsR != x.nVsW) {
      const Vq& d = x.Vs[x.nVsR++ & 1023];
      s.vs[0] = d.vs[0], s.vs[1] = d.vs[1], s.vs[2] = d.vs[2];
      for (int v = 0; v < 8; ++v) s.v[core::out::kRytmMk2.stem[v]] += d.st[v];   // a PRE / POST voice in its silent stem's place
    } else {
      s.vs[0] = s.vs[1] = s.vs[2] = 0.0f;
    }
    if (!x.Stems.push(s)) x.nDropIn = x.nDropIn + 1;
  }
}

void Render() { Render(0); }

void Render(int box) {
  Inst& x = I(box);
  unsigned n = x.Stems.size();
  if (n == 0) return;
  if (n > kMaxPerRender) n = kMaxPerRender;
  uint32_t t0 = core::cycles();
  float st[core::fx::SendFx::kMaxFrames][8], o[core::fx::SendFx::kMaxFrames][2], m[core::fx::SendFx::kMaxFrames][2];   // m: the POST pair
  float sl[core::fx::SendFx::kMaxFrames][4];   // each FX's own return (the recorder's FX1/FX2)
  float vs[core::fx::SendFx::kMaxFrames][3], b3[core::fx::SendFx::kMaxFrames];   // fx-ins: the voices' bus sends
  unsigned k = 0;
  Stem s;
  bool anyv = false;
  while (k < n && x.Stems.pop(&s)) {
    for (int v = 0; v < 8; ++v) st[k][v] = s.v[v];
    vs[k][0] = s.vs[0], vs[k][1] = s.vs[1], vs[k][2] = b3[k] = s.vs[2];
    anyv |= s.vs[0] != 0.0f || s.vs[1] != 0.0f || s.vs[2] != 0.0f;
    ++k;
  }
  Fx(x).Process(st, (int)k, o, core::fx::control_of(box), m, sl, anyv ? vs : nullptr);
  float b3o[core::fx::SendFx::kMaxFrames];   // bus 3's chain output (silence while off), for the spectrum (spectrum_pi.h)
  const bool spec = box == 0 && spectrum_pi::WantBus3();   // (the spectrum: box 0's)
  if (spec) memset(b3o, 0, sizeof(float) * k);
  core::ins::bus3(box, b3, (int)k, o, m, spec ? b3o : nullptr);   // bus 3: the dry bus's chain onto its pair (nothing while off)
  if (spec) spectrum_pi::FromBus3(b3o, k);
  float pk = x.Peak, bp[3] = {0.0f, 0.0f, 0.0f};   // bp: the buses' meters (B1, B2: each FX's own return; B3: its input)
  for (unsigned i = 0; i < k; ++i) {
    if (!x.Rets.push(Ret{o[i][0], o[i][1], m[i][0], m[i][1], {sl[i][0], sl[i][1], sl[i][2], sl[i][3]}})) x.nDropOut = x.nDropOut + 1;
    float a = o[i][0] < 0 ? -o[i][0] : o[i][0], b = m[i][0] < 0 ? -m[i][0] : m[i][0];
    if (b > a) a = b;
    if (a > pk) pk = a;
    for (int j = 0; j < 4; ++j) {
      const float x = sl[i][j] < 0 ? -sl[i][j] : sl[i][j];
      if (x > bp[j >> 1]) bp[j >> 1] = x;
    }
    const float x3 = b3[i] < 0 ? -b3[i] : b3[i];
    if (x3 > bp[2]) bp[2] = x3;
  }
  x.Peak = pk;
  for (int j = 0; j < 3; ++j)
    if (bp[j] > x.BusPk[j]) x.BusPk[j] = bp[j];
  x.nFrames = x.nFrames + k;
  x.nCycles = x.nCycles + (uint32_t)(core::cycles() - t0);   // (one block's ticks: far below the 32-bit wrap)
}

void TakeBusPeaks(float* pk) { TakeBusPeaks(0, pk); }

void TakeBusPeaks(int box, float* pk) {
  Inst& x = I(box);
  for (int j = 0; j < 3; ++j) {
    const float p = x.BusPk[j];
    pk[j] = p > 1.0f ? 1.0f : p;
    x.BusPk[j] = 0.0f;
  }
}

static int32_t Add(int32_t a, float x) {
  int64_t s = (int64_t)a + (int64_t)(x * 2147483648.0f);
  return s > 2147483647 ? 2147483647 : s < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)s;
}

void AddReturn(int32_t (*out)[12], unsigned n, float (*slots)[4]) { AddReturn(0, core::out::device(), out, n, slots); }

void AddReturn(int box, const core::out::Device& dev, int32_t (*out)[12], unsigned n, float (*slots)[4]) {
  Inst& x = I(box);
  if (!x.bPrimed) {
    if (x.Rets.size() < kPrime) return;
    x.bPrimed = true;
  }
  const uint8_t* pre = dev.pre;   // the device's table: the Rytm's 2 / 3, a stereo mix's main pair
  for (unsigned f = 0; f < n; ++f) {
    Ret r;
    if (!x.Rets.pop(&r)) {   // ran dry: silence, and prime again
      x.nUnder = x.nUnder + 1;
      x.bPrimed = false;
      return;
    }
    if (slots) slots[f][0] = r.s[0], slots[f][1] = r.s[1], slots[f][2] = r.s[2], slots[f][3] = r.s[3];
    out[f][pre[0]] = Add(out[f][pre[0]], r.l);   // PRE: MAIN FX, through the analog distortion + compressor
    out[f][pre[1]] = Add(out[f][pre[1]], r.r);
    if (r.pl != 0.0f || r.pr != 0.0f) out[f][dev.post[0]] = Add(out[f][dev.post[0]], r.pl), out[f][dev.post[1]] = Add(out[f][dev.post[1]], r.pr);   // POST: MAIN
  }
}

static uint32_t Take(volatile uint32_t* p) {
  uint32_t v = *p;
  *p = 0;
  return v;
}

bool Command(const char* p) {
  char r[400];
  // 3(c): the slot in hand's box (as fx::command's settings): its own send FX, stats and kill (slot 1's lines say so)
  const int b = core::fx::slot_in_hand() < kBoxes ? core::fx::slot_in_hand() : 0;
  const char* sl = b ? "slot 1: " : "";
  if (strcmp(p, "killfx") == 0) {   // the display's "kill tails"
    core::log(core::fx::all_sound_off(core::fx::control_of(b)) ? "sendfx: %skill tails -> cleared" : "sendfx: %skill tails ignored (a request within 100 ms)", sl);
    return true;
  }
  if (!core::fx::command(p, r, sizeof r)) return false;
  core::log("%s", r);
  if (p[6] == 0) {   // "sendfx": the stats since the box's last one
    Inst& x = I(b);
    uint32_t ms = core::now_ms(), dt_ms = ms - s_nLastMs[b], fr = x.nFrames, frames = fr - s_nLastFrames[b];
    uint64_t total = x.nCycles, busy = total - s_nLastBusy[b];
    s_nLastMs[b] = ms, s_nLastFrames[b] = fr, s_nLastBusy[b] = total;
    uint64_t window = (uint64_t)dt_ms * core::cycles_per_second() / 1000;   // the window in ticks (no 32-bit wrap)
    unsigned load = window ? (unsigned)(busy * 100000 / window) : 0;        // 1/1000 %
    unsigned ns = frames ? (unsigned)((double)busy * 1e9 / core::cycles_per_second() * 12 / frames) : 0;   // (a long window: busy x 1e9 passes 2^64)
    unsigned pk = (unsigned)(x.Peak * 1000.0f);
    x.Peak = 0.0f;
    core::log("sendfx: %s%u frames, core %u load %u.%03u percent, %u ns a 12-frame block, underruns %u, drops in %u out %u, "
              "peak %u/1000, faults %u",
              sl, frames, b ? 1u : 2u, load / 1000, load % 1000, ns, Take(&x.nUnder), Take(&x.nDropIn), Take(&x.nDropOut), pk,
              Fx(x).Faults());
  }
  return true;
}

}  // namespace sendfx_pi
