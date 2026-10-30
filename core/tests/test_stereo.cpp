// core/tests/test_stereo.cpp - stereo samples (docs/STEREO.md, the stereo / POST design): the pool RAM keeps a file's
// channels (pcache: 1 or 2, interleaved), poolmono=1 sums at load as before, a mono reader of a stereo entry gets the
// summed load's value bit for bit (16-bit), 3+ channels give their first two, the guard frame is a whole frame, a
// machine's fill (MULTISAMPLE's heads) can be stereo.
#include "timing.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include <chrono>

#include "check.h"
#include "fx/insert.h"
#include "fx/out_mix.h"
#include "notes.h"
#include "voices.h"
#include "engines/stream_voice.h"
#include "fx/out.h"
#include "lib/library.h"
#include "pool/card_index.h"
#include "pool/stream_budget.h"
#include <Sampler.h>
#include "msamp/cardq.h"
#include "platform.h"
#include "pool/pcache.h"
#include "wg_util.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 50000000;
static uint64_t g_card3 = 0;   // card time spent on the control core (core 3): must stay 0 (the card core's work)
static void run(int n) {
  for (int i = 0; i < n; ++i) {
    uint64_t c3 = host_card_us(3);
    pcache::step();
    g_card3 += host_card_us(3) - c3;
    host_card_core(0);
    for (int k = 0; k < 8 && cardq::service(); ++k) {}
    host_card_core(3);
    g_us += 1000;
    host_fake_time(true, g_us);
  }
}
static lib::Ref ref(const char* p) {
  lib::Ref r;
  CHECK(lib::ref_for_path(p, &r) && r.has_sha);
  return r;
}
static int load(const char* p) {
  int e = pcache::want(ref(p), p);
  CHECK(e >= 0);
  for (int i = 0; i < 4000 && pcache::state(e) == pcache::kLoading; ++i) run(1);
  return e;
}
// the test signal: L a 440 Hz sine, R 660 Hz (channel c of frame i), 16-bit values as written
static double sig(int i, int c) { return 0.5 * std::sin(2 * M_PI * (c ? 660 : 440) * i / 48000.0); }
static int16_t q16(double x) {   // as wg::wav_bytes writes 16-bit
  long v = std::lround(x * 32767.0);
  return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

int main() {
  char tmpl[] = "/tmp/stereo-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/S'").c_str()) == 0);
  wg::write_file(g_card + "/S/lr.wav", wg::wav_bytes(48000, 2, 16, 48000, 1, "", sig));
  wg::write_file(g_card + "/S/lr24.wav", wg::wav_bytes(48000, 2, 24, 44100, 1, "", sig));
  wg::write_file(g_card + "/S/m.wav", wg::wav_bytes(48000, 1, 16, 48000, 1, "", sig));
  wg::write_file(g_card + "/S/quad.wav", wg::wav_bytes(4800, 4, 16, 48000, 1, "", [](int i, int c) { return (c + 1) * 0.1 + 0 * i; }));
  wg::write_file(g_card + "/S/lr2.wav", wg::wav_bytes(48000, 2, 16, 48000, 1, "", [](int i, int c) { return sig(i + 7, c); }));
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  card_ok = true;
  lib::begin(true);
  for (int i = 0; i < 200000 && (i < 50 || lib::busy() || lib::stats().state != lib::kReady); ++i) lib::step();
  cardq::reset();
  cardq::set_worker(true);
  host_card_model(true);
  pcache::begin(16u << 20);
  CHECK(!pcache::mono());

  // ---- a stereo file: ch 2, interleaved L R exactly as the file (16-bit), the guard a whole frame, 2x the bytes
  uint64_t u0 = pcache::stats().used;
  int e = load("/S/lr.wav");
  pcache::Info in;
  CHECK(pcache::get(e, &in) && in.whole && in.ch == 2 && in.frames == 48000 && in.rate == 48000);
  for (int i = 0; i < 48000; ++i) CHECK(in.pcm[2 * i] == q16(sig(i, 0)) && in.pcm[2 * i + 1] == q16(sig(i, 1)));
  CHECK(in.pcm[2 * 48000] == in.pcm[2 * 47999] && in.pcm[2 * 48000 + 1] == in.pcm[2 * 47999 + 1]);
  CHECK(pcache::stats().used - u0 >= 48001u * 4);
  // a mono reader: (L + R) / 2 truncated = what the summed load gave (the poolmono path below), bit for bit
  std::vector<int16_t> sum(48000);
  for (int i = 0; i < 48000; ++i) sum[i] = pcache::mono_at(in.pcm, in.ch, (uint32_t)i);
  // ---- a mono file stays one channel
  int m = load("/S/m.wav");
  CHECK(pcache::get(m, &in) && in.ch == 1 && in.pcm[100] == q16(sig(100, 0)) && pcache::mono_at(in.pcm, 1, 100) == in.pcm[100]);
  // ---- 3+ channels: the first two as L / R
  int qd = load("/S/quad.wav");
  CHECK(pcache::get(qd, &in) && in.ch == 2 && in.pcm[10] == q16(0.1) && in.pcm[11] == q16(0.2));
  // ---- 24-bit 44.1 kHz stereo: ch 2 at its rate, each channel within 1 LSB of the 16-bit rounding
  int e24 = load("/S/lr24.wav");
  CHECK(pcache::get(e24, &in) && in.ch == 2 && in.rate == 44100);
  for (int i = 0; i < 48000; i += 97) CHECK(std::abs(in.pcm[2 * i] - q16(sig(i, 0))) <= 1 && std::abs(in.pcm[2 * i + 1] - q16(sig(i, 1))) <= 1);

  // ---- poolmono=1: a load sums (one channel, half the bytes), equal to the stereo entry's mono read, bit for bit
  pcache::set_mono(true);
  CHECK(pcache::mono());
  uint64_t u1 = pcache::stats().used;
  int e2 = load("/S/lr2.wav");
  CHECK(pcache::get(e2, &in) && in.ch == 1 && in.frames == 48000);
  CHECK(pcache::stats().used - u1 < 48001u * 4);
  pcache::set_mono(false);
  // the same file (lr2) natively for the comparison: forget, load again
  std::vector<int16_t> summed(in.pcm, in.pcm + 48000);
  pcache::forget(ref("/S/lr2.wav"));
  run(5);
  int e3 = load("/S/lr2.wav");
  CHECK(pcache::get(e3, &in) && in.ch == 2);
  for (int i = 0; i < 48000; ++i) CHECK(pcache::mono_at(in.pcm, 2, (uint32_t)i) == summed[i]);
  (void)sum;

  // ---- a machine's stereo fill (MULTISAMPLE's head): ch 2, frames x 2 samples, the guard frame
  lib::Ref rq = ref("/S/m.wav");
  pcache::forget(rq);
  run(5);
  int16_t* dst = nullptr;
  int have = -1;
  int f = pcache::fill_begin(rq, 100, 48000, 48000, &dst, &have, 2);
  CHECK(f >= 0 && dst);
  for (int i = 0; i < 200; ++i) dst[i] = (int16_t)i;
  pcache::fill_end(f, 100, true);
  CHECK(pcache::get(f, &in) && in.ch == 2 && in.frames == 100 && in.pcm[200] == 198 && in.pcm[201] == 199);
  // grown to whole by a want: the file's own layout (mono here)
  CHECK(pcache::want(rq, "/S/m.wav") == f);
  for (int i = 0; i < 4000 && !(pcache::get(f, &in) && in.whole); ++i) run(1);
  CHECK(pcache::get(f, &in) && in.whole && in.ch == 1 && in.frames == 48000);

  CHECK(g_card3 == 0);   // every stereo load's card work on the card core, none on the control core
  printf("test_stereo: pcache stereo / poolmono / 3+ channels / fills OK (no card time on core 3)\n");
  pcache::Info lrin;
  CHECK(pcache::get(e, &lrin) && lrin.ch == 2);

  // ---- DSP SAMPLE from RAM (SampleModes): a stereo sample at unity (TUN 64 FIN 64, note 60, ONE STA 0 END 120, LEV
  // 100): with a right channel its L and R exactly as stored; without, the mono read = the summed load's (bit for bit)
  {
    CHECK(pcache::get(e, &in) && in.ch == 2);
    const float sp[8] = {64, 64, 0, 0, 0, 120, 0, 100};
    static rytm::SampleModes a, b;
    a.Init(1), b.Init(1);
    a.Set(in.pcm, in.frames, in.rate, 2);
    std::vector<int16_t> mono(in.frames + 1);
    for (uint32_t i = 0; i <= in.frames; ++i) mono[i] = pcache::mono_at(in.pcm, 2, i);
    b.Set(mono.data(), in.frames, in.rate, 1);
    a.Trigger(60, sp), b.Trigger(60, sp);
    float l[480], r[480], m[480];
    a.Render(sp, l, 480, r);
    for (int i = 0; i < 480; ++i) CHECK(l[i] == in.pcm[2 * i] / 32768.0f && r[i] == in.pcm[2 * i + 1] / 32768.0f);
    a.Trigger(60, sp);
    a.Render(sp, l, 480);
    b.Render(sp, m, 480);
    for (int i = 0; i < 480; ++i) CHECK(l[i] == m[i]);
    // SLICE / GRAIN / STRCH read both channels too (L != R where the file's differ); WAVE / PLUCK: mono on both
    for (float mod : {40.0f, 60.0f, 75.0f}) {   // SLICE GRAIN STRCH zones
      const float sq[8] = {64, 64, 0, mod, 0, 120, 64, 100};
      a.Trigger(60, sq);
      double d = 0;
      for (int k = 0; k < 20; ++k) {
        a.Render(sq, l, 480, r);
        for (int i = 0; i < 480; ++i) d += std::fabs(l[i] - r[i]);
      }
      CHECK(d > 1.0);
    }
    const float sw[8] = {64, 64, 0, 125, 0, 120, 64, 100};   // WAVE
    a.Trigger(60, sw);
    a.Render(sw, l, 480, r);
    for (int i = 0; i < 480; ++i) CHECK(l[i] == r[i]);
  }
  // ---- DSP SAMPLE streamed (StreamVoice): the stereo file's L and R (16-bit: exact); mono: the sum as before
  {
    static uint8_t scratch[64 * 1024];
    static card::Index idx;
    CHECK(system(("mkdir -p '" + g_card + "/Samples/S' && cp '" + g_card + "/S/lr.wav' '" + g_card + "/Samples/S/lr.wav'").c_str()) == 0);
    CHECK(idx.begin(log, scratch, sizeof scratch));
    int fo = -1;
    card::FileRec rec;
    for (int f = 0; f < idx.folders() && fo < 0; ++f)
      for (int i = 0; idx.file(f, i, &rec); ++i)
        if (!strcmp(rec.name, "lr.wav")) {
          fo = f;
          break;
        }
    CHECK(fo >= 0 && rec.channels == 2);
    static uint8_t head[rytm::StreamVoice::kHead], ring[rytm::StreamVoice::kRing];
    static rytm::StreamVoice sv;
    sv.attach(head, ring);
    CHECK(sv.prepare(idx, fo, 0, 0.0f));
    sv.trigger();
    std::vector<float> L, R, M;
    float l[12], r[12];
    for (int b = 0; b < 2000; ++b) {
      while (sv.refill()) {}
      sv.render(0, 0, 1, false, 1.0f, l, 12, r);
      L.insert(L.end(), l, l + 12), R.insert(R.end(), r, r + 12);
    }
    for (int i = 0; i < 24000; ++i) CHECK(L[i] == q16(sig(i, 0)) / 32768.0f && R[i] == q16(sig(i, 1)) / 32768.0f);
    sv.trigger();
    for (int b = 0; b < 2000; ++b) {
      while (sv.refill()) {}
      sv.render(0, 0, 1, false, 1.0f, l, 12);
      M.insert(M.end(), l, l + 12);
    }
    for (int i = 0; i < 24000; ++i) CHECK(M[i] == (q16(sig(i, 0)) + q16(sig(i, 1))) * (0.5f / 32768.0f));
    CHECK(sv.underruns() == 0);
  }
  // ---- DEC (out.h): OFF none; 1 = 20 ms to -60 dB, 127 = 10 s
  CHECK(out::dec_coef(0) == 1.0f);
  {
    float g = 1.0f, k = out::dec_coef(1);
    for (int i = 0; i < 960; ++i) g *= k;
    CHECK(std::fabs(20 * std::log10(g) + 60) < 0.5);
    g = 1.0f, k = out::dec_coef(127);
    for (int i = 0; i < 480000; ++i) g *= k;
    CHECK(std::fabs(20 * std::log10(g) + 60) < 1.0);
  }
  printf("test_stereo: DSP SAMPLE from RAM and streamed: L / R exact, mono the summed value; DEC OK\n");

  // ---- the streaming budget (core/pool/stream_budget.h): the model's counts for set numbers, and a simulation of N
  // streams served by such a volume (the neediest first, every read at the p99, the read-ahead MULTISAMPLE keeps):
  // within the budget no stream runs dry; far over it they do; the volume of a path; the SD card test's count
  {
    sbudget::clear();
    const sbudget::Bench sd = sbudget::bench(0), usb3 = sbudget::bench(3);
    CHECK(!sd.measured && sd.read_mbps == 12.0f);
    int sd_st = sbudget::streams(0, 48000, 2, 24), sd_mono = sbudget::streams(0, 48000, 1, 16);
    int usb_st = sbudget::streams(3, 48000, 2, 24);
    printf("test_stereo: budget (defaults): SD %d stereo 24-bit / %d mono 16-bit streams, USB 3 %d stereo 24-bit\n", sd_st,
           sd_mono, usb_st);
    CHECK(sd_st >= sbudget::kWantStreams && usb_st > sd_st && sd_mono > 2 * sd_st);
    CHECK(sbudget::vol_of("/Samples/a.wav") == 0 && sbudget::vol_of("SD:/a.wav") == 0 && sbudget::vol_of("USB:/a.wav") == 1 &&
          sbudget::vol_of("NVME:/x") == 4);
    sbudget::set_bench(0, 12.0f, 25.0f);   // a card with slow reads (p99 25 ms): stereo forced to RAM
    CHECK(sbudget::bench(0).measured && sbudget::streams(0, 48000, 2, 24) < sbudget::kWantStreams);
    CHECK(!sbudget::fits("/Samples/a.wav", 48000, 2, 24) && sbudget::fits("USB3:/a.wav", 48000, 2, 24));
    // the simulation: a volume serving one 32 KB read at a time (p99 latency + the piece at its rate), each to the stream
    // with the least ahead; each stream plays bps; a run of 60 s; heads of 500 ms, a 300 ms read-ahead target
    auto sim = [](const sbudget::Bench& b, int n, int bytes) {
      const double bps = 48000.0 * 2 * bytes, piece = sbudget::kPiece, t = b.p99_ms * 1e-3 + piece / (b.read_mbps * 1048576.0);
      std::vector<double> ahead(n, 0.5);
      double now = 0, worst = 1e9;
      while (now < 60.0) {
        int k = 0;
        for (int i = 1; i < n; ++i) if (ahead[i] < ahead[k]) k = i;
        if (ahead[k] > 0.3) {   // everyone has the read-ahead: the volume idles a millisecond
          for (double& a : ahead) a -= 0.001;
          now += 0.001;
          continue;
        }
        for (double& a : ahead) a -= t;   // the read takes t; every stream plays meanwhile
        ahead[k] += piece / bps;
        now += t;
        for (double a : ahead) worst = a < worst ? a : worst;
      }
      return worst;   // < 0: a stream ran dry
    };
    for (int bytes : {3, 2}) {
      for (const sbudget::Bench& b : {sd, usb3, sbudget::Bench{12.0f, 25.0f, true}}) {
        int nb = sbudget::streams_for(b, 48000, 2, bytes * 8);
        double w_in = sim(b, nb, bytes), w_over = sim(b, (int)(nb / sbudget::kShare) * 2 + 2, bytes);
        printf("test_stereo: %.0f MB/s p99 %.0f ms, stereo %d-bit: budget %d streams, least ahead %.0f ms; %d streams: %.0f ms\n",
               b.read_mbps, b.p99_ms, bytes * 8, nb, w_in * 1000, (int)(nb / sbudget::kShare) * 2 + 2, w_over * 1000);
        CHECK(nb >= 1 && w_in > 0.05 && w_over < 0);
      }
    }
    sbudget::set_sd_voices(24);   // the SD card test's verdict (stereo 24-bit voices), when no bench
    sbudget::clear();
    sbudget::set_sd_voices(24);
    CHECK(sbudget::streams(0, 48000, 2, 24) == 24 && sbudget::streams(0, 48000, 1, 24) == 48);
    char o[400];
    CHECK(sbudget::command("stream=bench:USB:30:4", o, sizeof o) && sbudget::bench(1).measured && strstr(o, "USB "));
    CHECK(sbudget::json(o, sizeof o) > 0 && strstr(o, "\"vol\":\"SD\",\"stereo\":24"));
    sbudget::clear();
  }
  // ---- the costs on this Mac, and the Pi 4's estimated from PLAITS' boot-bench ratio (0.73 % of a core a voice there:
  // docs/pi-engine.md, as render_insfx's INSERT estimate)
  {
    // the thread's CPU clock (tests/timing.h): a pass preempted by the parallel suite is not counted as slow
    struct clk { static uint64_t now() { return timing_cpu_ns(); } };
    auto secs = [](uint64_t a) { return (double)(timing_cpu_ns() - a) * 1e-9; };
    voices_init();
    NoteAdapter na([](const uint8_t* d, int n) { host_sysex(d, n); });
    na.SetMachine(0, "plaits");
    na.Channel(0x90, 48, 100);
    static int16_t blk[kBlock][kVoices];
    uint64_t m2[kMachines] = {0}, sum = 0;
    for (int b = 0; b < 4000; ++b) sum += render_voices(blk, kBlock, 0, 0, 1, m2);
    double plaits = sum / 1e9 * 100, ratio = 0.73 / plaits;   // (4000 blocks = 1 s: % of a core)
    // the OUT mixer, 8 voices on POST (a crossfade every 4800 frames), per second of audio
    static core::out::Mixer mx;
    core::out::Control oc;
    core::out::defaults(oc);
    int16_t l[8], r[8];
    float trk[8], pre[2], post[2], feed[8];
    auto t0 = clk::now();
    for (int i = 0; i < 48000; ++i) {
      if (i % 4800 == 0) {
        for (int v = 0; v < 8; ++v) oc.route[v] = (uint8_t)(i / 4800 & 1 ? core::out::kPost : core::out::kPre);
        mx.Block(oc, 0xFF);
      }
      for (int v = 0; v < 8; ++v) l[v] = (int16_t)(i * 7 + v), r[v] = (int16_t)(i * 5 - v);
      mx.Frame(oc, l, r, trk, pre, post, feed);
      core::out::Mixer::Pairs(oc, pre, post);
    }
    double mix = secs(t0) * 100;
    for (int v = 0; v < 8; ++v) oc.route[v] = core::out::kPost;   // steady: every voice on POST, no crossfade
    mx.Block(oc, 0xFF);
    for (int i = 0; i < 4800; ++i) mx.Frame(oc, l, r, trk, pre, post, feed);
    t0 = clk::now();
    for (int i = 0; i < 48000; ++i) {
      if (i % 7 == 0) mx.Block(oc, 0xFF);   // (once a transport block, as Output::Pull)
      for (int v = 0; v < 8; ++v) l[v] = (int16_t)(i * 7 + v), r[v] = (int16_t)(i * 5 - v);
      mx.Frame(oc, l, r, trk, pre, post, feed);
      core::out::Mixer::Pairs(oc, pre, post);
    }
    double steady = secs(t0) * 100;
    // a voice's INSERT chain (COMP + EQ: the dearest pair), mono vs stereo, 8 voices, a second
    static float lines[ins::kLines][ins::kLine];
    double chain[2];
    for (int st = 0; st < 2; ++st) {
      ins::Engine en(48000.0f);
      en.Place(lines);
      en.Reset();
      ins::Control c = {};
      for (int v = 0; v < 8; ++v) c.type[v][0] = ins::kComp, c.type[v][1] = ins::kEq;
      float x[kBlock], y[kBlock];
      auto t1 = clk::now();
      for (int n = 0; n < 4000; ++n)
        for (int v = 0; v < 8; ++v) {
          for (int s = 0; s < kBlock; ++s) x[s] = y[s] = 0.3f * std::sin(0.05f * (n * kBlock + s) * (v + 1));
          if (st) en.ProcessVoiceStereo(v, x, y, kBlock, c);
          else en.ProcessMono(v, x, kBlock, c);
        }
      chain[st] = secs(t1) * 100;
    }
    // DSP SAMPLE from RAM (ONE, unity), mono vs stereo, one voice, a second
    double smp[2];
    for (int st = 0; st < 2; ++st) {
      static rytm::SampleModes a;
      a.Init(1);
      a.Set(lrin.pcm, lrin.frames, lrin.rate, 2);
      const float sp[8] = {64, 64.5f, 0, 0, 0, 120, 1, 100};   // a little detuned: interpolation, looped
      a.Trigger(60, sp);
      float o1[kBlock], o2[kBlock];
      auto t2 = clk::now();
      for (int n = 0; n < 4000; ++n) a.Render(sp, o1, kBlock, st ? o2 : nullptr);
      smp[st] = secs(t2) * 100;
    }
    printf("test_stereo: CPU (Mac / Pi 4 est., %% of a core): OUT mixer 8 voices on POST %.3f / %.2f, switching every "
           "100 ms %.3f / %.2f; INSERT COMP+EQ 8 voices "
           "mono %.2f / %.1f, stereo %.2f / %.1f; DSP SAMPLE RAM a voice mono %.3f / %.2f, stereo %.3f / %.2f "
           "(PLAITS %.3f here, x %.1f)\n",
           steady, steady * ratio, mix, mix * ratio, chain[0], chain[0] * ratio, chain[1], chain[1] * ratio, smp[0], smp[0] * ratio, smp[1],
           smp[1] * ratio, plaits, ratio);
    CHECK(chain[1] < chain[0] * 3.0 && mix < 5.0);
  }
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);
  printf("test_stereo: OK\n");
  return 0;
}
