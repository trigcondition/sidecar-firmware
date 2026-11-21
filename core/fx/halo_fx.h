// core/fx/halo_fx.h - HALO (send FX TYPE 18): a small, lush modulated plate with FREEZE and SHIMMER, in every build
// (the Teensy's too). Included by send_fx.h after its helpers (Smooth, knob01, fast_sin, flush, FX_DSP): not on its own.
//
// Written for the Sidecar (no code taken from anywhere). The idea is the classic figure-8 plate tank (two branches,
// each modulated allpass -> delay -> allpass -> delay, each feeding the other; the published
// "Effect Design Part 1", JAES 1997), with the Sidecar's own sizes, filters, modulation, shimmer and memory layout:
//   - the tank runs at half rate (24 kHz at 48 kHz): a 31-tap half-band FIR down (polyphase: 8 multiplies a tank
//     sample, flat to 9.5 kHz, -59 dB from 14.5 kHz) and a 27-tap one up per channel (flat to 9 kHz, -62 dB from
//     15 kHz); latency ~0.6 ms;
//   - all lines live in ONE 16-bit ring of 4096 samples (8 KB), one pointer for every line (each line a fixed region
//     of the ring, as on a reverb DSP): the 4 input diffusers, the tank's 8 segments, the resamplers' histories. The
//     samples are IEEE half floats (x 4096, so the smallest normal is ~-156 dBFS): 11 bits of precision at every
//     level, so a quiet tail keeps decaying smoothly (16-bit integers truncate a long tail away at ~-50 dB below its
//     start, measured: T60 halved) and nothing saturates (+-16). Converted by the M7's VCVTB, elsewhere by bits; the
//     tank computes in the ring's units (x 4096: scaled once at the input and the output). The
//     whole effect is 8.3 KB: it shares its slot's memory with CHORUS / COMB / CRUSH / RINGMOD (send_fx.h Small),
//     so adding it took no RAM;
//   - the tail is long because of feedback, not delay length (the loop is ~150 ms); the four modulated reads (both
//     branch allpasses, both branch ends; 4-point cubic interpolation: linear's loss on the highs, modulated, cut the
//     2-4 kHz T60 to a third - measured; four LFOs at unrelated rates) smear the modes so a long tail does not ring;
//   - the damping (TONE's one-pole low-pass) sits at each branch's input, so the shimmer's source line is already
//     damped; TONE's high-pass acts on the input only (in the loop it shortened the low mids' decay: measured), the
//     loop has a 12 Hz DC blocker;
//   - SHIMMER: an octave-up pitch shifter that reads the first branch's long delay with two taps moving at twice the
//     speed (integer steps: no interpolation), sine windows (constant power), fed back into the input
//     diffusers: each pass climbs an octave. Its path is band-limited (200 Hz .. 5 kHz: below, the bottom maps
//     onto itself and the windows' sidebands piled up there; above, the octave aliases back) so what climbs out of
//     it leaves: the shimmer blooms and decays with the tail. A ceiling (the branch ends' peak a block, above 2.0 the
//     next block's feedback is turned down; 0.5 for a frozen shimmer) bounds whatever is left (FREEZE + SHIMMER: a
//     cloud that keeps rising, at a fixed level).
//   - FREEZE (DECAY 127): the input fades out, the feedback goes to 1, the damping out (the DC blocker to 1 Hz: a
//     held filter state fed DC in, +7 dB a minute - measured): the tail holds (-2 .. -5 dB a minute, its top dulling
//     through the cubic reads); turning DECAY down lets it decay again.
// No libm in the audio path: the coefficients once a knob change (fast_exp2), everything single precision.
//
// Knobs P2..P5 (0..127, all p-lockable; P1 is the send's ROUTE, not used here):
//   P2 DECAY  T60 0.3 s .. 30 s (exponential; 64 ~ 3 s); 127 FREEZE
//   P3 MOD    the modulation: 0 still (glassy), 64 lush (+-0.5 ms at ~0.6 Hz), 127 deep (+-1 ms at ~1.1 Hz)
//   P4 TONE   dark .. bright: the loop's low-pass 1.2 .. 10 kHz, the input's high-pass 30 .. 250 Hz (thinner when bright)
//   P5 SHIM   the octave-up shimmer: 0 off .. 127 strong
// The return is wet only and wide (L and R from different taps of both branches).
#pragma once

namespace core {
namespace fx {

inline constexpr Zone kHaloZones[] = {{0, 127, 0, 0, 0, "FRZ"}};   // DECAY: the number up to 126, FRZ at 127
inline constexpr KnobLabels kHaloLabels = {{"DCY", "MOD", "TONE", "SHIM"}, kHaloZones, 1, {"Decay", "Modulation", "Tone", "Shimmer"}};

// 2^x for the coefficients (x clamped to [-126, 126]): a 6th-order series on the fraction, the exponent by bits
// (relative error ~2e-6); no libm
inline float fast_exp2(float x) {
  x = fminf(126.0f, fmaxf(-126.0f, x));
  const float fi = floorf(x), f = x - fi;
  const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f +
                                                                                      f * (0.00133336f + f * 0.00015404f)))));
  int32_t b;
  memcpy(&b, &p, 4);
  b += (int32_t)fi * (1 << 23);
  float r;
  memcpy(&r, &b, 4);
  return r;
}

struct Halo {
  static constexpr int kMem = 4096, kMask = kMem - 1;
  static constexpr float kUp = 4096.0f, kDown = 1.0f / 4096.0f, kMax = 65504.0f;   // the ring's scale: x 4096 into a half
  // the lines (tank samples: 24 kHz at 48 kHz): the input diffusers, the two branches, the modulation's reach
  static constexpr int kDif[4] = {43, 31, 107, 79};
  static constexpr int kAp1A = 337, kD1A = 661, kAp2A = 241, kD2A = 557;
  static constexpr int kAp1B = 401, kD1B = 631, kAp2B = 281, kD2B = 509;
  static constexpr int kModAp = 24, kModD2 = 24, kShimW = 640;   // the shimmer's window, inside D1A
  // the regions of the ring: each line's base; a line read at most d samples ago takes d + 1 (+3 for the cubic)
  static constexpr int kDn = 8, kUpN = 7;   // the half-bands' odd taps a side (down, up)
  static constexpr int bDA = 0, bDB = bDA + kDn, bOL = bDB + 2 * kDn, bOR = bOL + 2 * kUpN, bF0 = bOR + 2 * kUpN, bF1 = bF0 + kDif[0] + 1,
                       bF2 = bF1 + kDif[1] + 1, bF3 = bF2 + kDif[2] + 1, bA1 = bF3 + kDif[3] + 1,
                       bAD1 = bA1 + kAp1A + kModAp + 3, bA2 = bAD1 + kD1A + 1, bAD2 = bA2 + kAp2A + 1,
                       bB1 = bAD2 + kD2A + kModD2 + 3, bBD1 = bB1 + kAp1B + kModAp + 3, bB2 = bBD1 + kD1B + 1,
                       bBD2 = bB2 + kAp2B + 1, kEnd = bBD2 + kD2B + kModD2 + 3;
  static_assert(kEnd <= kMem, "HALO's lines exceed its ring");
  static_assert(kShimW + 4 <= kD1A, "the shimmer's window must fit in D1A");
  // the half-bands' odd taps (centre 0.5; equiripple at 48 kHz): down 31 taps (pass 9.5 kHz, stop 14.5 kHz), up
  // 27 taps (9 / 15 kHz)
  static constexpr float kHd[kDn] = {0.315550531f, -0.098052436f, 0.050987028f, -0.029159358f,
                                     0.016595290f, -0.008898610f, 0.004261189f, -0.001809992f};
  static constexpr float kHu[kUpN] = {0.314496715f, -0.095109546f, 0.046728258f, -0.024375518f,
                                      0.012064004f, -0.005222563f, 0.001819877f};

  uint16_t m[kMem];   // half floats
  uint32_t p = 0;
  bool half = false, fresh = true;
  int key = -1;
  int cnt = 0;           // the shimmer's tap position, kShimW - 1 .. 0
  float agc = 1.0f;      // the ceiling's gain on the feedback
  float xa = 0.0f, zl = 0.0f, zr = 0.0f;   // the pending even input; the pending odd outputs
  float lpA = 0.0f, lpB = 0.0f, hpA = 0.0f, hpB = 0.0f, shl1 = 0.0f, shl2 = 0.0f, shy = 0.0f, ph1 = 0.0f, ph2 = 0.0f,
        ph3 = 0.0f, ph4 = 0.0f, hpi = 0.0f, shh1 = 0.0f, shh2 = 0.0f;
  Smooth rk, fz, lpc, hpc, mod, shs;   // ramped per block (Smooth::Ramp)
  float t_rk = 0.0f, t_fz = 0.0f, t_lpc = 1.0f, t_hpc = 0.0f, t_mod = 0.0f, t_shs = 0.0f;

  void Clear() {
    memset(m, 0, sizeof m);
    p = 0, half = false, fresh = true, cnt = 0, agc = 1.0f;
    xa = zl = zr = lpA = lpB = hpA = hpB = shl1 = shl2 = shy = ph1 = ph2 = ph3 = ph4 = hpi = shh1 = shh2 = 0.0f;
  }

  // the knobs' targets (once a change): rk the log2 gain a tank sample (-3 log2(10) / (T60 fs)), the filters'
  // one-pole coefficients, the modulation 0..1, the shimmer's gain
  FX_DSP void Targets(const uint8_t* k, float fs) {
    const int dv = k[0] > 127 ? 127 : k[0];
    const float d = (float)(dv > 126 ? 126 : dv) * (1.0f / 126.0f);
    const float inv_t60 = (1.0f / 0.3f) * fast_exp2(-6.6438562f * d);   // T60 = 0.3 s x 100^d
    t_rk = -9.9657843f * inv_t60 / fs;
    t_fz = dv == 127 ? 1.0f : 0.0f;
    const float t = knob01(k[2]);
    const float flp = 1200.0f * fast_exp2(3.0588937f * t), fhp = 30.0f + 220.0f * t * t;   // 1.2 .. 10 kHz; 30 .. 250 Hz
    t_lpc = 1.0f - fast_exp2(-9.0647202f * flp / fs);   // 1 - e^(-2 pi f / fs)
    t_hpc = 6.2831853f * fhp / fs;
    t_mod = knob01(k[1]);
    t_shs = 0.7f * knob01(k[3]);
  }

  FX_DSP void Process(const float* in, int n, float (*out)[2], const uint8_t* k, float sr, float a) {
    const float fs = 0.5f * sr;   // the tank's rate
    const int kk = (k[0] & 127) | (k[1] & 127) << 7 | (k[2] & 127) << 14 | (k[3] & 127) << 21;
    if (kk != key) key = kk, Targets(k, fs);
    if (fresh) rk.y = t_rk, fz.y = t_fz, lpc.y = t_lpc, hpc.y = t_hpc, mod.y = t_mod, shs.y = t_shs, fresh = false;
    const float gin0 = 1.0f - fz.y;
    rk.Ramp(t_rk, a, n), fz.Ramp(t_fz, a, n), lpc.Ramp(t_lpc, a, n), hpc.Ramp(t_hpc, a, n), mod.Ramp(t_mod, a, n),
        shs.Ramp(t_shs, a, n);
    // the block's constants (block-end values; the input's freeze fade ramped per tank sample)
    const float f = fz.y, nf = 1.0f - f, r = rk.y * nf;   // frozen: gain 1 (log2 0)
    // SHIMMER's feedback 0.8 s g: its path loses what climbs out of its band each pass, so it blooms and then decays
    // with the tail (measured: no runaway below the ceiling); agc the ceiling's gain, ceil lower for a frozen shimmer
    const float s = shs.y;
    const float gA1 = fast_exp2(r * (float)(kAp1A + kD1A)), gA2 = fast_exp2(r * (float)(kAp2A + kD2A)) * agc;
    const float gB1 = fast_exp2(r * (float)(kAp1B + kD1B)), gB2 = fast_exp2(r * (float)(kAp2B + kD2B)) * agc;
    const float sg = 0.8f * s * gA1;   // the shimmer once around: as long as the tail
    const float lim = (s > 0.0f ? kCeil * (1.0f - 0.75f * f) : kCeil) * kUp;   // in the ring's units
    float pk = 0.0f;   // the branch ends' peak this block
    const float cl = lpc.y + (1.0f - lpc.y) * f, ch = hpc.y, cdc = 6.2831853f * (12.0f - 11.0f * f) / fs;   // DC only: 12 Hz, 1 Hz frozen (still moving: a held state would feed DC in)
    const float md = mod.y, dep = (float)kModAp * md, dep2 = (float)kModD2 * md;
    const float r1 = (0.12f + 1.0f * md) / fs;   // the LFOs (Hz / fs): one rate each, none a simple ratio of another
    const float r2 = 1.29f * r1, r3 = 0.77f * r1, r4 = 0.61f * r1;
    const float gin1 = 1.0f - f;
    const int ticks = (n + (half ? 1 : 0)) / 2;
    const float dgin = ticks ? (gin1 - gin0) / (float)ticks : 0.0f;
    float gin = gin0;
    const bool shim = s > 0.0f || shy != 0.0f;
    for (int i = 0; i < n; ++i) {
      if (!half) {   // the even input: kept; the odd output of the last tank sample
        xa = in[i], half = true;
        out[i][0] += zl, out[i][1] += zr;
        continue;
      }
      half = false;
      gin += dgin;
      p = (p - 1) & kMask;
      // down: the half-band's centre (the even input kDn - 1 tank samples ago) and its odd taps (the odd inputs)
      W(bDA, kUp * xa), W(bDB, kUp * in[i]);
      float x = 0.5f * R(bDA, kDn - 1);
#pragma GCC unroll 8
      for (int j = 0; j < kDn; ++j) x += kHd[j] * (R(bDB, kDn - 1 - j) + R(bDB, kDn + j));
      x *= 0.6f * gin;
      hpi += (x - hpi) * ch;   // TONE's high-pass on the input (in the loop it would shorten the low mids' decay)
      x = x - hpi + sg * shy;
      // the input diffusers
      x = Ap(bF0, kDif[0], 0.75f, x), x = Ap(bF1, kDif[1], 0.75f, x);
      x = Ap(bF2, kDif[2], 0.625f, x), x = Ap(bF3, kDif[3], 0.625f, x);
      // the LFOs: the allpasses (1, 2), the branch ends (3, 4)
      ph1 += r1, ph1 -= ph1 >= 1.0f ? 1.0f : 0.0f;
      ph2 += r2, ph2 -= ph2 >= 1.0f ? 1.0f : 0.0f;
      ph3 += r3, ph3 -= ph3 >= 1.0f ? 1.0f : 0.0f;
      ph4 += r4, ph4 -= ph4 >= 1.0f ? 1.0f : 0.0f;
      const float lA = fast_sin(ph1), lB = fast_sin(ph2), mA = fast_sin(ph3), mB = fast_sin(ph4);
      // the branch ends (the other branch's input)
      const float yA = Cubic(bAD2, (float)kD2A + dep2 * mA), yB = Cubic(bBD2, (float)kD2B + dep2 * mB);
      pk = fmaxf(pk, fmaxf(fabsf(yA), fabsf(yB)));
      // branch A: damping, the modulated allpass, D1, the allpass, D2
      {
        float u = x + gB2 * yB;
        lpA += (u - lpA) * cl;
        hpA += (lpA - hpA) * cdc;
        u = lpA - hpA;
        const float z = Cubic(bA1, (float)kAp1A + dep * lA), v = u - 0.7f * z;
        W(bA1, v);
        W(bAD1, z + 0.7f * v);
        const float w = gA1 * R(bAD1, kD1A);
        W(bAD2, Ap(bA2, kAp2A, 0.5f, w));
      }
      {
        float u = x + gA2 * yA;
        lpB += (u - lpB) * cl;
        hpB += (lpB - hpB) * cdc;
        u = lpB - hpB;
        const float z = Cubic(bB1, (float)kAp1B + dep * lB), v = u - 0.7f * z;
        W(bB1, v);
        W(bBD1, z + 0.7f * v);
        const float w = gB1 * R(bBD1, kD1B);
        W(bBD2, Ap(bB2, kAp2B, 0.5f, w));
      }
      // the shimmer: two taps in D1A moving twice as fast (an octave up), sine windows half a window apart
      if (shim) {
        cnt = cnt > 0 ? cnt - 1 : kShimW - 1;
        const int c2 = cnt >= kShimW / 2 ? cnt - kShimW / 2 : cnt + kShimW / 2;
        const float w1 = fast_sin((float)cnt * (0.5f / kShimW)), w2 = fast_sin((float)c2 * (0.5f / kShimW));
        const float sh = w1 * R(bAD1, 4 + cnt) + w2 * R(bAD1, 4 + c2);
        // band-limited: two poles at ~5 kHz (the octave of the top would alias back, fs/3 onto itself) and two
        // high-pass poles at ~200 Hz (the bottom maps onto itself: the window's 37.5 Hz sidebands piled up there)
        shl1 += (sh - shl1) * 0.73f, shl2 += (shl1 - shl2) * 0.73f;
        shh1 += (shl2 - shh1) * 0.052f;
        const float h1 = shl2 - shh1;
        shh2 += (h1 - shh2) * 0.052f;
        shy = h1 - shh2;
      }
      // the outputs: five taps of both branches each, L and R mirrored
      const float L = R(bBD1, kD1B / 9) + R(bBD1, kD1B * 7 / 10) - R(bB2, kAp2B * 5 / 8) - R(bAD1, kD1A * 12 / 25) -
                      R(bAD2, kD2A * 3 / 10);
      const float Rr = R(bAD1, kD1A * 2 / 15) + R(bAD1, kD1A * 3 / 4) - R(bA2, kAp2A * 3 / 5) - R(bBD1, kD1B * 4 / 9) -
                       R(bBD2, kD2B * 2 / 7);
      // up: the half-band's odd taps on the history (this sample's), the centre (kUpN - 1 back) a 48 kHz frame later
      W(bOL, L), W(bOR, Rr);
      float el = 0.0f, er = 0.0f;
#pragma GCC unroll 7
      for (int j = 0; j < kUpN; ++j)
        el += kHu[j] * (R(bOL, kUpN - 1 - j) + R(bOL, kUpN + j)), er += kHu[j] * (R(bOR, kUpN - 1 - j) + R(bOR, kUpN + j));
      constexpr float og = 2.0f * kOut * kDown, oz = kOut * kDown;
      out[i][0] += og * el, out[i][1] += og * er;
      zl = oz * R(bOL, kUpN - 1), zr = oz * R(bOR, kUpN - 1);
    }
    // the ceiling: a tank above kCeil (FREEZE wandering up, SHIMMER piling octaves) is turned down for the next
    // block at once, and let back up over ~0.3 s; never acts on a decaying tail below it
    agc = pk > lim ? agc * lim / pk : agc + (1.0f - agc) * (0.0002f * (float)n);
    lpA = flush(lpA), lpB = flush(lpB), hpA = flush(hpA), hpB = flush(hpB), hpi = flush(hpi);
    shl1 = flush(shl1), shl2 = flush(shl2), shy = flush(shy), shh1 = flush(shh1), shh2 = flush(shh2);
  }

 private:
  static constexpr float kCeil = 2.0f;   // the tank's peak (its ring holds +-16)
  static constexpr float kOut = 0.7f;   // the taps' sum to the return (white noise in: the wet ~-5 dB re the input at 3 s, ~-2 dB at 30 s)
  // a half float (x kUp) <-> float; the software pair keeps normals only (below 2^-14: 0), rounds to nearest even
#if defined(__ARM_ARCH_7EM__) && defined(__ARM_FP) && (__ARM_FP & 2)
  static float H2F(uint16_t h) {
    float r;
    __asm__("vmov %0, %1\n\tvcvtb.f32.f16 %0, %0" : "=t"(r) : "r"((uint32_t)h));
    return r;
  }
  static uint16_t F2H(float x) {
    uint32_t r;
    float t;
    __asm__("vcvtb.f16.f32 %1, %2\n\tvmov %0, %1" : "=r"(r), "=&t"(t) : "t"(fminf(kMax, fmaxf(-kMax, x))));
    return (uint16_t)r;
  }
#else
  static float H2F(uint16_t h) {
    const uint32_t a = h & 0x7FFFu, b = (uint32_t)(h & 0x8000u) << 16 | (a ? (a << 13) + 0x38000000u : 0u);
    float r;
    memcpy(&r, &b, 4);
    return r;
  }
  static uint16_t F2H(float x) {
    uint32_t b;
    memcpy(&b, &x, 4);
    const uint32_t sg = (b >> 16) & 0x8000u, a = b & 0x7FFFFFFFu;
    if (a < 0x38800000u) return (uint16_t)sg;              // below the smallest normal (or 0): 0
    if (a >= 0x477FE000u) return (uint16_t)(sg | 0x7BFFu);  // the largest (no infinity)
    const uint32_t c = a - 0x38000000u;
    return (uint16_t)(sg | ((c + 0x0FFFu + ((c >> 13) & 1u)) >> 13));
  }
#endif
  // the ring in its own units (x kUp)
  float R(int b, int d) const { return H2F(m[(p + (uint32_t)(b + d)) & kMask]); }
  void W(int b, float x) { m[(p + (uint32_t)b) & kMask] = F2H(x); }
  // an allpass of length L (the line holds v): v = x + g z, y = z - g v
  float Ap(int b, int len, float g, float x) {
    const float z = R(b, len), v = x + g * z;
    W(b, v);
    return z - g * v;
  }
  // d samples ago, 4-point cubic (Catmull-Rom); d >= 2
  float Cubic(int b, float d) const {
    const int i = (int)d;
    const float t = d - (float)i;
    const uint32_t q = p + (uint32_t)(b + i);
    const float xm = H2F(m[(q - 1) & kMask]), x0 = H2F(m[q & kMask]), x1 = H2F(m[(q + 1) & kMask]), x2 = H2F(m[(q + 2) & kMask]);
    const float c1 = 0.5f * (x1 - xm), c2 = xm - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
  }
};

}  // namespace fx
}  // namespace core
