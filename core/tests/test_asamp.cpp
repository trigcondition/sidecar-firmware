// core/tests/test_asamp.cpp - the AUTO-SAMPLER driven through its remote page (core/ui/page_auto.cpp, the Rytm's AUTO
// page) as on the hardware: the list knobs step one entry a detent and stop at their ends (SRC: 6-OP FM .. EXT, however
// far or fast it is turned; B: the patch / track / MIDI output; SAMPLE's knobs too), and YES runs a whole plan for every
// source kind, each take recorded with sound in it: the Sidecar's 6-OP FM, JUNO and a voice copy (offline), a Rytm
// track (a fake Rytm sounding the track whose channel the note came on, on that track's Overbridge stem) and an external
// synth (a fake synth on the USB-A port's and the computer's MIDI, its audio on AUDIO IN L / R: stereo and mono).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "check.h"
#include "engine.h"
#include "host_link.h"
#include "keys.h"
#include "msamp/msamp.h"
#include "samp/autosamp.h"
#include "samp/dsp.h"
#include "samp/sampler.h"
#include "ui/extra.h"
#include "voices.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 1000000;
static const ui::ExtraPage& P = ui::kAutoPage;

static std::string slurp(const std::string& p) {
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "";
  std::string t;
  char b[4096];
  size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) t.append(b, n);
  fclose(f);
  return t;
}
static uint32_t rd(const std::string& t, size_t at, int n) {
  uint32_t v = 0;
  for (int i = 0; i < n; ++i) v |= (uint32_t)(uint8_t)t[at + i] << (8 * i);
  return v;
}
// A WAV's channels and the RMS of each channel (24-bit PCM)
static int wav_rms(const std::string& w, double* rms, uint32_t* frames) {
  if (w.size() < 44 || w.compare(0, 4, "RIFF")) return 0;
  int ch = (int)rd(w, 22, 2);
  size_t at = 12;
  while (at + 8 <= w.size()) {
    uint32_t n = rd(w, at + 4, 4);
    if (!w.compare(at, 4, "data")) {
      uint32_t fr = n / (3 * ch);
      double s[2] = {0, 0};
      for (uint32_t i = 0; i < fr; ++i)
        for (int c = 0; c < ch; ++c) {
          int32_t v = (int32_t)(rd(w, at + 8 + (i * ch + c) * 3, 3) << 8) >> 8;
          s[c] += (double)v * v;
        }
      for (int c = 0; c < ch; ++c) rms[c] = fr ? std::sqrt(s[c] / fr) / 8388608.0 : 0;
      *frames = fr;
      return ch;
    }
    at += 8 + n + (n & 1);
  }
  return 0;
}

// ---- the fakes: a Rytm (a note on track t's channel sounds t on its stem) and a synth (AUDIO IN L / R)
struct Voice {
  bool on = false;
  int note = 0, vel = 0, stem = -1;
  uint64_t at = 0, off = 0;
  double ph = 0;
};
static Voice g_rytm, g_synth;
static int g_rytm_on = 0, g_synth_on = 0, g_synth_off = 0, g_synth_port = -1, g_synth_ch = -1;
static void fake_rytm(const uint8_t* m, int n) {   // hooks.to_rytm: the host link (SysEx dropped here)
  if (n != 3) return;
  int ch = m[0] & 15;
  if ((m[0] & 0xF0) == 0x90 && m[2]) {
    if (ch >= samp::kATracks) return;   // (the auto channel and beyond: no track)
    g_rytm.on = true, g_rytm.note = m[1], g_rytm.vel = m[2], g_rytm.stem = samp::track_stem(ch);
    g_rytm.at = g_us + 6000, g_rytm.off = 0;
    ++g_rytm_on;
  } else if ((m[0] & 0xF0) == 0x80 || (m[0] & 0xF0) == 0x90) {
    g_rytm.off = g_us + 6000;
  }
}
static bool fake_midi_out(int port, const uint8_t* m, int n) {   // the platform's EXT outputs (the Pi: cores.cpp)
  if (port != samp::kPortUsb && port != samp::kPortDev) return false;
  if (n == 0) return true;
  if (n != 3) return false;
  g_synth_port = port, g_synth_ch = m[0] & 15;
  if ((m[0] & 0xF0) == 0x90 && m[2]) {
    g_synth.on = true, g_synth.note = m[1], g_synth.vel = m[2], g_synth.at = g_us + 3000, g_synth.off = 0;
    ++g_synth_on;
  } else {
    g_synth.off = g_us + 3000;
    ++g_synth_off;
  }
  return true;
}
static double sound(Voice& v, uint64_t t, double tau_us) {
  if (!v.on || t < v.at) return 0;
  double env = v.off && t > v.off ? std::exp(-(double)(t - v.off) / tau_us) : 1.0;
  v.ph += 2 * M_PI * 440.0 * std::pow(2.0, (v.note - 69) / 12.0) / 48000;
  return 0.4 * v.vel / 127.0 * env * std::sin(v.ph);
}

// One 250 us block: the Overbridge IN (the fakes' audio), the control core's tick, the card core's polls
static void block() {
  int32_t f[kBlock * 12] = {0};
  for (int k = 0; k < kBlock; ++k) {
    uint64_t t = g_us + k * 1000000ull / 48000;
    double r = sound(g_rytm, t, 80000), s = sound(g_synth, t, 120000);
    if (g_rytm.stem >= 0) f[k * 12 + 2 + g_rytm.stem] = (int32_t)(r * 2147483000.0);
    f[k * 12 + 10] = (int32_t)(s * 2147483000.0);          // AUDIO IN L
    f[k * 12 + 11] = (int32_t)(-0.5 * s * 2147483000.0);   // AUDIO IN R: half, inverted (told apart from L)
  }
  if (samp::asamp_wants_in()) samp::asamp_in_block(f, kBlock, 12);
  for (int i = 0; i < 64 && msamp::step(); ++i) {}
  samp::tick();
  samp::asamp_tick();
  for (int i = 0; i < 4 && samp::poll(); ++i) {}
  for (int i = 0; i < 4 && samp::asamp_poll(); ++i) {}
  g_us += 250;
  host_fake_time(true, g_us);
}

// A 0x03 PARAMS report: voice vb & 7 plays machine vb & 0x78 with P1..P16 p
static void params(int vb, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)vb};
  p.resize(16, 0);
  for (float x : p) {
    int i = (int)x;
    m.push_back(i), m.push_back((int)((x - i) * 128) & 0x7f);
  }
  m.push_back(0xf7);
  host_sysex(m.data(), (int)m.size());
}

// The page's plan with a short range (2 notes, 1 layer, 1 round robin, short hold and tail)
static void short_plan() {
  samp::Plan p = samp::asamp_plan();
  p.lo = 48, p.hi = 60, p.step = 12, p.nvel = 1, p.vels[0] = 100, p.rr = 1;
  p.hold_ms = 400, p.tail_ms = 400, p.gap_ms = 50, p.loop = true;
  samp::asamp_configure(p);
}
static void turn(int e, int d) { P.enc(e, d, false); }

// YES on the page, the run to its end; every take's WAV has sound in each channel; returns the status
static samp::AStatus run(const char* what, int want_ch, int want_takes = 2) {
  CHECK(P.key(0x44));
  samp::AStatus s;
  samp::asamp_status(&s);
  CHECK(s.state == samp::kATaking);
  for (int i = 0; i < 4000000; ++i) {
    samp::asamp_status(&s);
    if (s.state == samp::kADone || s.state == samp::kAFailed || s.state == samp::kACancelled) break;
    block();
  }
  for (int i = 0; i < 400; ++i) block();
  printf("test_asamp: %s: state %d, %d/%d takes written, error '%s', %s\n", what, s.state, s.written, s.total, s.error,
         s.sfz);
  CHECK(s.state == samp::kADone && s.total == want_takes && s.written == want_takes && !s.error[0]);
  std::string t = slurp(s.sfz);
  CHECK(!t.empty());
  std::string dir = std::string(s.sfz).substr(0, std::string(s.sfz).rfind('/'));
  int regions = 0;
  for (size_t at = t.find("<region> sample="); at != std::string::npos; at = t.find("<region> sample=", at + 1), ++regions) {
    size_t e = t.find(' ', at + 16);
    std::string wav = dir + "/samples/" + t.substr(at + 16, e - at - 16);
    double r[2] = {0, 0};
    uint32_t fr = 0;
    int ch = wav_rms(slurp(wav), r, &fr);
    printf("test_asamp:   %s: %d ch, %u frames, rms %.3f %.3f\n", wav.c_str(), ch, fr, r[0], r[1]);
    CHECK(ch == want_ch && fr > 48000 / 4);
    for (int c = 0; c < ch; ++c) CHECK(r[c] > 0.01);   // not silent (normalised to -1 dBFS: well above)
  }
  CHECK(regions == want_takes);
  CHECK(P.key(0x45));   // NO on DONE: the plan again
  return s;
}

// ---- 1. the list knobs: one entry a detent, clamped at the ends, the same end however it is turned
static void test_steps() {
  samp::Plan p = samp::asamp_plan();
  p.src = samp::kAJuno, p.pat = 5;
  samp::asamp_configure(p);
  // all the way left, every way: always 6-OP FM, never another kind (the old (src + delta) & 3 landed on any)
  for (int d : {-1, -1, -1, -2, -5, -7, -64, -3}) {
    turn(0, d);
    CHECK(samp::asamp_plan().src == samp::kAFm);
  }
  // one kind a detent, in order, then EXT stays
  const samp::ASrc order[] = {samp::kAJuno, samp::kAVoice, samp::kATrack, samp::kAExt, samp::kAExt, samp::kAExt};
  for (samp::ASrc o : order) {
    turn(0, 1);
    CHECK(samp::asamp_plan().src == o);
  }
  for (int d : {5, 63, 1}) {
    turn(0, d);
    CHECK(samp::asamp_plan().src == samp::kAExt);
  }
  // a turn past an end keeps the entry (B not reset)
  samp::Plan q = samp::asamp_plan();
  q.midi_ch = 6, q.port = samp::kPortDev;
  samp::asamp_configure(q);
  turn(0, 1);
  CHECK(samp::asamp_plan().midi_ch == 6 && samp::asamp_plan().port == samp::kPortDev);
  // EXT's B: USB 1..16, DEV 1..16, RYTM 1..16, clamped; B pushed: the input L+R -> L -> R -> L+R
  turn(1, -100);
  CHECK(samp::asamp_plan().port == samp::kPortUsb && samp::asamp_plan().midi_ch == 0);
  turn(1, 17);
  CHECK(samp::asamp_plan().port == samp::kPortDev && samp::asamp_plan().midi_ch == 1);
  turn(1, 100);
  CHECK(samp::asamp_plan().port == samp::kPortRytm && samp::asamp_plan().midi_ch == 15);
  CHECK(samp::asamp_plan().in == samp::kInLR && P.key(0x71) && samp::asamp_plan().in == samp::kInL);
  CHECK(P.key(0x71) && samp::asamp_plan().in == samp::kInR && P.key(0x71) && samp::asamp_plan().in == samp::kInLR);
  // TRK's B: the 12 tracks BD..CB, clamped, the channel the track's
  turn(0, -1);
  CHECK(samp::asamp_plan().src == samp::kATrack && samp::asamp_plan().track == 0);
  CHECK(!P.key(0x71));   // (B pushed: EXT's only)
  for (int t = 1; t < samp::kATracks; ++t) {
    turn(1, 1);
    CHECK(samp::asamp_plan().track == t && samp::asamp_plan().midi_ch == t);
  }
  turn(1, 1), turn(1, 9);
  CHECK(samp::asamp_plan().track == samp::kATracks - 1);
  turn(1, -40);
  CHECK(samp::asamp_plan().track == 0);
  char sn[24], in[40];
  q = samp::asamp_plan();
  const char* names[samp::kATracks] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
  const int stems[samp::kATracks] = {0, 1, 2, 2, 3, 4, 5, 5, 6, 6, 7, 7};
  for (int t = 0; t < samp::kATracks; ++t) {
    q.track = t;
    samp::asamp_names(q, sn, sizeof sn, in, sizeof in);
    CHECK(!strcmp(sn, names[t]) && samp::track_stem(t) == stems[t]);
  }
  // FM's B: clamped to the list (the ROM's 96 here)
  turn(0, -10);
  turn(1, -5);
  CHECK(samp::asamp_plan().src == samp::kAFm && samp::asamp_plan().pat == 0);
  P.enc(1, 50, true);   // FUNC: 8 a detent
  CHECK(samp::asamp_plan().pat == samp::asamp_entries(samp::kAFm) - 1);
  // SAMPLE's knobs clamp as well: A from IN L/R to VOICE 8, B NOW..BAR, D TRANS..OFF
  const ui::ExtraPage& S = ui::kSampPage;
  S.enc(0, -100, false);
  CHECK(samp::config().src == samp::kSrcIn);
  S.enc(0, -1, false);
  CHECK(samp::config().src == samp::kSrcIn);
  S.enc(0, 1, false);
  CHECK(samp::config().src == samp::kSrcMain);
  S.enc(0, 100, false);
  CHECK(samp::config().src == samp::kSrcVoice && samp::config().sub == 7);
  S.enc(0, 3, false);
  CHECK(samp::config().src == samp::kSrcVoice && samp::config().sub == 7);
  S.enc(0, -100, false);
  S.enc(1, -9, false);
  CHECK(samp::config().start == samp::kManual);
  S.enc(1, 9, false);
  CHECK(samp::config().start == samp::kOnBar);
  S.enc(1, -9, false);
  S.enc(3, 9, false);
  CHECK(samp::config().slices == samp::kNoSlices);
  S.enc(3, -9, false);
  CHECK(samp::config().slices == samp::kTransients);
  printf("test_asamp: list knobs (one entry a detent, clamped ends) PASS\n");
}

// ---- 2. a whole run per source kind from the page: every take with sound in it
static void test_runs() {
  short_plan();
  // the Sidecar's engines (offline): 6-OP FM ROM 1, JUNO patch 4, a copy of voice 1
  turn(0, -10);
  turn(1, -200);
  CHECK(samp::asamp_plan().src == samp::kAFm);
  samp::AStatus s = run("6-OP FM", 1);
  CHECK(s.offline);
  turn(0, 1), turn(1, 4);
  CHECK(samp::asamp_plan().src == samp::kAJuno && samp::asamp_plan().pat == 4);
  run("JUNO", 1);
  params(kJuno | 0, {9, 64, 64, 64, 64, 64, 64, 127});   // voice 1 plays JUNO patch 10: its copy
  for (int i = 0; i < 40; ++i) block();
  turn(0, 1);
  CHECK(samp::asamp_plan().src == samp::kAVoice && samp::asamp_plan().voice == 0);
  run("VOICE 1", 1);
  // a Rytm track: CP (track 4, channel 4) sounds on the RS/CP stem (IN 4). Before: the 8 stems were the list and the
  // note went to channel stem + 1, so BT..CY/CB sent their notes to another track (BT's to CP ...): "no sound".
  hooks.to_rytm = fake_rytm;
  turn(0, 1), turn(1, 3);
  CHECK(samp::asamp_plan().src == samp::kATrack && samp::asamp_plan().track == 3 && samp::asamp_plan().midi_ch == 3);
  g_rytm_on = 0;
  s = run("TRK CP", 1, 1);   // (not chromatic: one note, the track's own pitch)
  CHECK(!s.offline && g_rytm_on == 1 && s.zones == 1);
  samp::Plan p = samp::asamp_plan();
  p.rr = 2;   // CY (track 11, channel 11) on the CY/CB stem (IN 9): 2 round robins of its one note
  samp::asamp_configure(p);
  turn(1, 7);
  CHECK(samp::asamp_plan().track == 10);
  g_rytm_on = 0;
  run("TRK CY", 1, 2);
  CHECK(g_rytm_on == 2);
  p = samp::asamp_plan();
  p.rr = 1;
  samp::asamp_configure(p);
  // an external synth: without a MIDI output the start refuses (no silent run); with one, on the USB-A device (stereo,
  // L+R) and on the computer's MIDI (mono, R)
  turn(0, 1);
  CHECK(samp::asamp_plan().src == samp::kAExt);
  samp::asamp_set_midi_out(nullptr);
  char err[100];
  samp::Plan e = samp::asamp_plan();
  e.port = samp::kPortUsb;
  CHECK(!samp::asamp_start(e, err, sizeof err) && strstr(err, "no MIDI out"));
  samp::asamp_set_midi_out(fake_midi_out);
  e.port = samp::kPortDev;
  CHECK(!samp::asamp_start(e, err, sizeof err) && strstr(err, "USB MIDI device"));   // none plugged in
  keys::source(0, true, "Synth");   // a USB MIDI device on the Pi's USB-A
  turn(1, -100), turn(1, 16 + 4);   // DEV 5
  CHECK(samp::asamp_plan().port == samp::kPortDev && samp::asamp_plan().midi_ch == 4 && samp::asamp_plan().in == samp::kInLR);
  g_synth_on = g_synth_off = 0;
  s = run("EXT DEV 5 L+R", 2);
  CHECK(g_synth_on == 2 && g_synth_off == 2 && g_synth_port == samp::kPortDev && g_synth_ch == 4 && !s.offline);
  std::string t = slurp(s.sfz);
  CHECK(t.find("pitch_keycenter=48") != std::string::npos && t.find("pitch_keycenter=60") != std::string::npos);
  {   // the stereo WAV interleaved right: R is -L/2 frame for frame (the fake's AUDIO IN R)
    std::string w = slurp("/MULTISAMPLE/EXT_CH5/samples/EXT_CH5_C4_v100_rr1.wav");
    size_t at = w.find("data") + 8;
    double err = 0, pk = 0;
    for (uint32_t i = 0; at + i * 6 + 6 <= w.size(); ++i) {
      double l = (int32_t)(rd(w, at + i * 6, 3) << 8) >> 8, r = (int32_t)(rd(w, at + i * 6 + 3, 3) << 8) >> 8;
      err = std::fmax(err, std::fabs(r + 0.5 * l)), pk = std::fmax(pk, std::fabs(l));
    }
    printf("test_asamp: stereo: |R + L/2| at most %.1f of a peak %.0f\n", err, pk);
    CHECK(pk > 4e6 && err < pk * 1e-3);
  }
  turn(1, -100);   // USB 1, input R
  CHECK(P.key(0x71) && P.key(0x71) && samp::asamp_plan().in == samp::kInR);
  run("EXT USB 1 R", 1);
  CHECK(g_synth_port == samp::kPortUsb && g_synth_ch == 0);
  keys::source(0, false, "");
  printf("test_asamp: a run per source kind (FM, JUNO, VOICE, TRK, EXT stereo / mono), every take sounding PASS\n");
}

int main() {
  char tmpl[] = "/tmp/core_asamp_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples' '" + g_card + "/MULTISAMPLE'").c_str()) == 0);
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin() && card_ok);
  for (int i = 0; i < 4000 && msamp::step(); ++i) {}
  test_steps();
  test_runs();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_asamp: PASS\n");
  return 0;
}
