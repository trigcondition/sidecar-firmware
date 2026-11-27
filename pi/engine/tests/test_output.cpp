// pi/engine/tests/test_output.cpp - the output stage: main mode sums onto MAIN L/R, tracks mode puts each voice on its
// Overbridge channel, the fade-in over 200 ms, the -12 dBFS limit (a soft knee: no flat top), silence (counted) when the
// FIFO runs dry.
#include <cmath>
#include <cstdio>

#include "check.h"
#include "output.h"

static int16_t blk[12][8];

// the looper's playback position: the render frame of the next frame Pull hands out (0.5.0, core/loop)
static void test_pulled_frame() {
  static Output u;
  int32_t f[7][12];
  int16_t b[12][8] = {};
  CHECK(u.PulledFrame() == 0);
  for (int k = 0; k < 10; ++k) u.Push(b, 12);   // 120 render frames 0..119
  u.Pull(f, 7);
  CHECK(u.PulledFrame() == 7);
  u.Pull(f, 7);
  CHECK(u.PulledFrame() == 14);
  u.Reset();   // a new stream drops what is queued: those frames are gone, the next one is 120
  CHECK(u.PulledFrame() == 120);
  u.Push(b, 12);
  u.Pull(f, 7);
  CHECK(u.PulledFrame() == 127);
  u.Reset();
  u.Pull(f, 7);   // an empty FIFO is silence, not a frame
  CHECK(u.PulledFrame() == 132);
  for (int k = 0; k < 200; ++k) u.Push(b, 12);   // 2400 frames into 2048: a full FIFO refuses the rest, counted as passed
  CHECK(u.Level() == Output::kFifo);
  CHECK(u.PulledFrame() == 132 + (2400 - Output::kFifo));
  printf("test_output: pulled frame ok\n");
}

int main() {
  test_pulled_frame();
  static Output o;
  int32_t f[7][12];
  o.Pull(f, 7);   // empty: silence, counted
  CHECK(o.Underflows() == 0 && o.RestartUnderflows() == 7 && f[3][0] == 0);   // no target yet: never armed
  o.SetFifoTarget(96);
  for (int s = 0; s < 12; ++s) for (int v = 0; v < 8; ++v) blk[s][v] = (v == 2) ? 4096 : 0;   // voice 2 at 0.125 (under the knee)
  for (int k = 0; k < 2000; ++k) CHECK(o.Push(blk, 12) == 12 || o.Level() > 2000);
  CHECK(o.Level() == Output::kFifo);
  o.Reset();                                                  // a new stream: the old frames are dropped
  CHECK(o.Level() == 0);
  o.Push(blk, 12);
  o.Pull(f, 7);
  CHECK(f[0][0] == 0 && f[1][0] > 0 && f[1][0] < 2000000);   // the fade starts at 0
  for (int k = 0; k < 1400; ++k) {                            // past 200 ms, the FIFO never empty
    o.Push(blk, 12);
    o.Pull(f, 7);
  }
  float main_l = f[6][0] / 2147483648.0f;
  CHECK(std::fabs(main_l - 0.125f * Output::kMainGain) < 1e-4f && f[6][0] == f[6][1]);
  CHECK(f[6][Output::kVoiceToChannel[2]] == 0);                // main mode: the voice channels silent
  o.SetMode(Output::kTracks);
  o.Pull(f, 7);
  CHECK(f[0][0] == 0 && f[0][1] == 0);
  CHECK(std::fabs(f[0][Output::kVoiceToChannel[2]] / 2147483648.0f - 0.125f) < 1e-4f);   // under the knee: as it came
  for (int s = 0; s < 12; ++s) blk[s][2] = 32767;                // full scale: limited to -12 dBFS (the knee's top)
  while (o.Level()) o.Pull(f, 7);
  o.Push(blk, 12);
  o.Pull(f, 7);
  CHECK(f[0][Output::kVoiceToChannel[2]] <= (int32_t)(Output::kLimit * 2147483648.0f) &&
        f[0][Output::kVoiceToChannel[2]] > (int32_t)(0.9999f * Output::kLimit * 2147483648.0f));
  CHECK(o.TakePeak() > 0.24f && o.TakePeak() == 0.0f);
  {  // a voice over -12 dBFS (a 262 Hz sine peaking at 0.3, as hot engines did before kVoiceGain): the knee rounds its
     // top, it is not flat. The old hard clamp held 0.25 for a run of samples each half cycle (odd harmonics, the clipping
     // users heard); the knee rises to the peak and leaves at most the peak's two samples within 1e-5 of it.
    static int16_t sb[12][8];
    float top = 0.0f;
    int at_top = 0;
    while (o.Level()) o.Pull(f, 7);
    for (int b = 0; b < 16; ++b) {                               // 192 frames: one half cycle and more
      for (int s = 0; s < 12; ++s) {
        for (int v = 0; v < 8; ++v) sb[s][v] = 0;
        sb[s][2] = (int16_t)(0.3f * 32768.0f * std::sin(2.0f * 3.14159265f * 262.0f * (b * 12 + s) / 48000.0f));
      }
      o.Push(sb, 12);
      for (int k = 0; k < 2; ++k) {
        int32_t g[6][12];
        o.Pull(g, 6);
        for (int i = 0; i < 6; ++i) {
          float y = g[i][Output::kVoiceToChannel[2]] / 2147483648.0f;
          if (y > top + 1e-5f) top = y, at_top = 1;
          else if (y > top - 1e-5f) ++at_top;
        }
      }
    }
    CHECK(top < Output::kLimit && top > 0.2f && at_top <= 2);
    o.TakePeak();
  }
  {  // stereo=trim:trk, the voices' own channels only. Up: the gain first, one soft knee to full scale, so a hot voice is not cut at -12 dBFS and clipped again.
     // Down or 0: the -12 dBFS limit, then the gain
    static core::out::Control c;
    const int vch = Output::kVoiceToChannel[2];
    auto once = [&](int16_t s) {
      for (int k = 0; k < 12; ++k) blk[k][2] = s;
      while (o.Level()) o.Pull(f, 7);
      o.Push(blk, 12);
      o.Pull(f, 7);
      return f[0][vch] / 2147483648.0f;
    };
    core::out::defaults(c);
    o.SetOutControl(&c);
    CHECK(once(32767) == Output::kLimit);                        // 0: the limit, as with no control at all
    c.trk10 = 120;
    const float t = std::pow(10.0f, 0.6f);
    CHECK(std::fabs(once(3277) - 0.1f * t) < 1e-3f);             // under the knee: the gain alone
    const float a = once(8192), b = once(11010), d = once(16384);  // 0.25, 0.336 (FM6's peak on the unit), 0.5 at the box
    CHECK(a > 0.7f && a < b && b < d && d <= 0.9999f);           // over the old 0.25: still rising, never past full scale
    CHECK(once(-11010) == -b);
    c.route[3] = core::out::kPre;                                // a voice off TRK: the mix path takes the same trim
    CHECK(std::fabs(once(11010) - b) < 1e-3f);
    c.route[3] = core::out::kTrk;
    c.trk10 = 240;
    CHECK(once(32767) > 0.99f && once(32767) <= 0.9999f);
    c.trk10 = -60;                                               // down: under the limit, as before
    CHECK(std::fabs(once(32767) - 0.25f * std::pow(10.0f, -0.3f)) < 1e-3f);
    for (int k = 0; k < 12; ++k) blk[k][2] = 32767;
    o.SetOutControl(nullptr);
    o.TakePeak();
  }
  const uint8_t map[8] = {4, 7, 5, 8, 10, 9, 11, 6};             // ob_block.h's kVoiceToRytm
  for (int v = 0; v < 8; ++v) CHECK(Output::kVoiceToChannel[v] == map[v]);
  {   // two underrun buckets: the refill burst after a Reset is "restart"; an empty FIFO after it reached its target is not
    static Output u;
    int32_t g[7][12];
    u.SetFifoTarget(96);
    u.Pull(g, 7);                                              // before any stream: not armed
    CHECK(u.Underflows() == 0 && u.RestartUnderflows() == 7 && u.Streams() == 0);
    u.Reset();
    CHECK(u.Streams() == 1);
    for (int k = 0; k < 3; ++k) u.Pull(g, 7);                  // the burst while the control core is behind
    CHECK(u.Underflows() == 0 && u.RestartUnderflows() == 28);
    for (int k = 0; k < 4; ++k) u.Push(blk, 12);               // 48 frames: not a healthy buffer, does not arm
    for (int k = 0; k < 7; ++k) u.Pull(g, 7);                  // 49 pulled: the last frame underruns, still restart
    CHECK(u.Underflows() == 0 && u.RestartUnderflows() == 29);
    for (int k = 0; k < 8; ++k) u.Push(blk, 12);               // at the target: armed on the next Pull
    for (int k = 0; k < 13; ++k) u.Pull(g, 7);
    CHECK(u.Underflows() == 0 && u.Level() == 5);              // 91 of 96 taken, no glitch
    u.Pull(g, 7);                                              // 2 short: a real glitch
    CHECK(u.Underflows() == 2 && u.RestartUnderflows() == 29);
    u.Reset();                                                 // a second Reset mid-session: re-gated, not leaked
    CHECK(u.Streams() == 2);
    u.Pull(g, 7);
    CHECK(u.Underflows() == 2 && u.RestartUnderflows() == 36);
    static int32_t h[12][12];
    static Output d;                                           // the device's boot path: default state, no target yet
    d.Pull(g, 7);
    CHECK(d.Underflows() == 0 && d.RestartUnderflows() == 7 && d.Streams() == 0);
    d.Reset();
    for (int k = 0; k < 8; ++k) d.Push(blk, 12);               // a fill with the target still unset
    d.Pull(g, 7);
    d.Pull(g, 7);
    d.SetFifoTarget(96);                                       // Configure arrives
    while (d.Level()) d.Pull(g, 7);
    CHECK(d.Underflows() == 0);                                // the drain itself was not a glitch...
    d.Reset();
    for (int k = 0; k < 8; ++k) d.Push(blk, 12);
    d.Pull(g, 7);                                              // ...and a target of 96 now arms it
    for (int k = 0; k < 14; ++k) d.Pull(g, 7);
    CHECK(d.Underflows() > 0 && d.Streams() == 2);
    static Output e;                                           // something pulls before any stream exists
    e.SetFifoTarget(96);
    for (int k = 0; k < 8; ++k) e.Push(blk, 12);
    for (int k = 0; k < 14; ++k) e.Pull(g, 7);                 // 98 pulled from 96: the last 2 are empty, but no stream yet
    CHECK(e.Underflows() == 0 && e.RestartUnderflows() == 2 && e.Diagnostics().pre_reset_pulled == 0);
    e.Reset();                                                 // the stream starts: the pre-Reset pulls are on record
    CHECK(e.Diagnostics().pre_reset_pulled == 98 && e.Diagnostics().arm_at == Output::kNone);
    for (int k = 0; k < 8; ++k) e.Push(blk, 12);               // the control core refilled before the first Pull
    e.Pull(g, 7);
    CHECK(e.Diagnostics().arm_at == 0 && e.Diagnostics().arm_level == 96 && e.Diagnostics().reset_level == 0);
    static Output c;                                           // two clean starts: steady-state stays zero
    c.SetFifoTarget(96);
    for (int r = 0; r < 2; ++r) {
      c.Reset();
      c.Pull(g, 7);
      for (int k = 0; k < 8; ++k) c.Push(blk, 12);
      for (int k = 0; k < 500; ++k) c.Push(blk, 12), c.Pull(h, 12);   // steady: 12 in, 12 out
    }
    CHECK(c.Underflows() == 0 && c.RestartUnderflows() == 14 && c.Streams() == 2);
    // A stream that never fills (the render cores cannot keep up: 12 frames in for every 14 out, so the FIFO never
    // reaches its target and never arms). Its dropouts are real ones: once the refill grace after the Reset is over
    // they count as underruns, not as the restart's burst - `under` reading 0 here was the bug.
    static Output s;
    s.SetFifoTarget(96);
    s.Reset();
    uint32_t pulled = 0;
    while (pulled + 14 <= Output::kRestartGrace) s.Push(blk, 12), s.Pull(g, 7), s.Pull(g, 7), pulled += 14;
    const uint32_t restart = s.RestartUnderflows();
    CHECK(s.Underflows() == 0 && restart > 0);                 // within the grace: the restart bucket
    for (int k = 0; k < 100; ++k) s.Push(blk, 12), s.Pull(g, 7), s.Pull(g, 7);   // 1400 more pulled, 1200 pushed
    CHECK(s.Diagnostics().arm_at == Output::kNone);            // still never armed
    CHECK(s.Underflows() == 200 && s.RestartUnderflows() == restart);   // every empty frame after it: a glitch
    CHECK(s.Diagnostics().under_first >= Output::kRestartGrace && s.Diagnostics().under_first != Output::kNone);
    s.Reset();                                                 // a new stream: a new grace
    s.Pull(g, 7);
    CHECK(s.Underflows() == 200 && s.RestartUnderflows() == restart + 7);
    static Output n;                                           // no target (unknown healthy level): never a glitch
    n.Reset();
    for (int k = 0; k < 1000; ++k) n.Pull(g, 7);
    CHECK(n.Underflows() == 0 && n.RestartUnderflows() == 7000);
  }
  printf("test_output: PASS\n");
  return 0;
}
