// core/tests/test_sampler.cpp - SAMPLER (0.5.0, docs/SAMPLER.md): DSP SAMPLE's engine as a Digitakt sample machine,
// from the sample RAM (the Pi). Who is SAMPLER: a 0x20 report with machines.h kSmplTag (53 41; another tag: DSP SAMPLE, whatever the
// hello says), a track whose 33 names machine 43; no resonator (P1..P8 up play exactly as dry; DSP SAMPLE keeps it:
// test_dsps_golden); PLAY's 9 zones (FWD REV FLP RLP, then SLICE .. WAVE; DSP SAMPLE's 7 MOD zones untouched); REV from
// END down to STA; FLP with LOOP 0 (OFF) looping STA..END, FWD once; LOOP's point: FLP loops [point, END], RLP [STA,
// point], a point outside STA..END the whole region (review M1: and then it streams, no wait for the sample RAM); a
// sample longer than the sample RAM holds streams whole with its loop point ignored (M2); TUNE 4 / 124 = -60 / +60
// semitones; SMP OFF silent, nothing streamed, nothing waiting; the sequencer's lock pages; the sample editor's preview
// plays once (L1); a report without the tag mid-note. Voice 0, SMP A001 = a ramp 0.1 .. 0.6 over 48000 frames, A002 a
// 100 s file (longer than the sample RAM's kMaxFrames: cut).
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "core_port.h"   // host_fake_time
#include "engine.h"
#include "machines.h"
#include "pool/pool.h"
#include "pool/sample_ram_pool.h"
#include "seq/seq.h"
#include "ui/remote.h"
#include "voices.h"
#include "wav.h"

using namespace core;

static constexpr int kN = 48000;
static constexpr int kLong = 100 * 48000;   // A002's frames (> SampleRamPool::kMaxFrames)
static double ramp(double f) { return 0.1 + 0.5 * f / kN; }   // the sample's value at frame f (as stored: 16-bit)

// P1..P16: P1..P8 no knobs on SAMPLER (the Rytm sends 0; up here, as an old sound's), TUNE FINE SMP PLAY STRT END LOOP LEV
static float P[16];
static void reset_p() {
  const float d[16] = {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100};
  memcpy(P, d, sizeof P);
}
static int g_tag = kSmplTag;   // the reports' sound tag (0: DSP SAMPLE's on a 0.5.0 Rytm; -1: no tag field, a 0.4.x Rytm's)
static void send(int cmd) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)(kDspSample | 0)};
  if (cmd == 0x04) m.push_back(60), m.push_back(100);
  for (float x : P) {
    int i = (int)x;
    m.push_back((uint8_t)i), m.push_back((uint8_t)((int)((x - i) * 128) & 0x7f));
  }
  if (g_tag >= 0) m.push_back((uint8_t)(g_tag >> 7 & 0x7f)), m.push_back((uint8_t)(g_tag & 0x7f));
  m.push_back(0xf7);
  engine_sysex(m.data(), (int)m.size());
}
static void hello(int caps) {   // the Rytm's 35: <uptime hi> <lo> <caps>
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, 0x35, 1, 0, 40, (uint8_t)caps, 0xF7};
  engine_sysex(m, 10);
}
static void track33(int t, int machine, int vb) {   // 1025: one track's machine (0..52) and voice byte
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, (uint8_t)machine, (uint8_t)vb, 0xF7};
  engine_sysex(m, sizeof m);
}

static uint64_t g_us = 1000000;
static int16_t ring[kBlock][kVoices];
// n frames of voice 0 after the last trig: the raw rows (raw) and the sample's own value (tanh(x) * 0.7, inverted)
static std::vector<double> render(int frames, std::vector<int16_t>* raw = nullptr) {
  std::vector<double> o;
  uint64_t mach[kMachines] = {0};
  for (int b = 0; b < frames / kBlock; ++b) {
    idle_stream();
    render_voices(ring, kBlock, 0, 0, kVoices, mach);
    for (int i = 0; i < kBlock; ++i) {
      o.push_back(atanh(ring[i][0] / (0.7 * 32767.0)));
      if (raw) raw->push_back(ring[i][0]);
    }
  }
  return o;
}
static void trig() { send(0x04); }
static void settle() {   // the trig's prepare and first refills
  for (int i = 0; i < 10; ++i) idle_prepare(), idle_stream();
}
static void warm() {   // the sample in the sample RAM, voice 0 playing from it
  send(0x03);
  for (int i = 0; i < 20000 && !from_ram[0]; ++i) {
    if (i % 50 == 0) trig();
    idle_prepare();
    for (int k = 0; k < 8; ++k) idle_stream();
    g_us += 1000, host_fake_time(true, g_us);
  }
  CHECK(from_ram[0]);
}
static bool near(double a, double b, double e = 0.003) { return std::fabs(a - b) < e; }
static int sounding_frames(int cap) {   // frames voice 0 sounds from the last trig, in blocks, at most cap
  std::vector<double> o = render(cap);
  int n = 0;
  for (double x : o) n += x != 0.0;
  return n;
}

// SAMPLER by the report's tag and by 33's machine byte; the hello's caps decide nothing (no caps bit 64 any more). A
// report without the tag field says nothing (review M2): the voice stays what it was; another tag or machine ends it.
static void t_identity() {
  reset_p();
  g_tag = 0;
  send(0x03);
  CHECK(v[0].dsps && !v[0].smpl);   // DSP SAMPLE: its own tag (0)
  g_tag = -1;
  send(0x03);
  CHECK(v[0].dsps && !v[0].smpl);   // no tag field (a 0.4.x Rytm): DSP SAMPLE stays
  {   // H1: the Rytm's own bytes (rytm-mods 0021 SAMPLER_TAG 0x29C1, on the wire 53 41), not built from kSmplTag
    uint8_t r[41] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(kDspSample | 0)};
    for (int k = 0; k < 16; ++k) r[6 + 2 * k] = (uint8_t)P[k], r[7 + 2 * k] = 0;
    r[38] = 0x53, r[39] = 0x41, r[40] = 0xF7;
    engine_sysex(r, 41);
    CHECK(v[0].dsps && v[0].smpl);
    r[39] = 0x50;   // 53 50: not SAMPLER's
    engine_sysex(r, 41);
    CHECK(v[0].dsps && !v[0].smpl);
  }
  g_tag = kSmplTag;
  send(0x03);
  CHECK(v[0].dsps && v[0].smpl && dsp_mode(v[0]) == 0);
  g_tag = -1;
  send(0x03);
  send(0x04);
  CHECK(v[0].smpl);   // no tag field, 0x03 or 0x04: still SAMPLER
  g_tag = 0x224D;   // another tag (MD's): DSP SAMPLE
  send(0x03);
  CHECK(v[0].dsps && !v[0].smpl);
  g_tag = kSmplTag;
  send(0x03);
  {   // another machine ends it: a short report on PLAITS, then a short one on DSP SAMPLE's flag
    std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, 0x03, (uint8_t)(kPlaits | 0)};
    for (int k = 0; k < 32; ++k) m.push_back(0);
    m.push_back(0xf7);
    engine_sysex(m.data(), (int)m.size());
    CHECK(!v[0].smpl && !v[0].dsps);
    g_tag = -1;
    send(0x03);
    CHECK(v[0].dsps && !v[0].smpl);
  }
  hello(127);   // a 0.5.0 Rytm's hello: still by the tag
  g_tag = 0;
  send(0x03);
  CHECK(!v[0].smpl);
  CHECK(ui::remote_voice_smpl(0) == false);
  g_tag = kSmplTag;
  send(0x03);
  CHECK(ui::remote_voice_smpl(0));   // before any 33: the voice's own reports
  track33(0, 45, kDspSample | 0);    // BD on DSP SAMPLE (45)
  CHECK(!ui::remote_track_smpl(0) && !ui::remote_voice_smpl(0));
  track33(0, kRytmSampler, kDspSample | 0);   // BD on SAMPLER (43): DSP SAMPLE's flag, its own machine number
  CHECK(ui::remote_track_smpl(0) && ui::remote_voice_smpl(0) && (ui::remote_track_machine(0) & 0x78) == kDspSample);
  CHECK(!strcmp(machine_short(kSmplMachine), "SAMPLR") && !strcmp(machine_long(kSmplMachine), "SAMPLER"));
  CHECK(sample_machine(kSmplMachine) && sample_machine(kDspSample) && !sample_machine(kGranMachine));
  CHECK(machine_of((uint8_t)(kDspSample | 3)) == kDspSample);
}

static void t_zones() {
  const float step[9] = {0, 15, 29, 43, 57, 72, 86, 100, 114};   // the Rytm's stored values (ceil(128 k / 9))
  const char* names[9] = {"FWD", "REV", "FLP", "RLP", "SLICE", "GRAIN", "STRCH", "PLUCK", "WAVE"};
  const int modes[9] = {0, 0, 1, 1, 2, 3, 4, 5, 6};   // rytm::SampleModes ONE LOOP SLICE GRAIN STRCH PLUCK WAVE
  for (int k = 0; k < 9; ++k) {
    CHECK(dsps_play_zone(true, step[k]) == k && dsps_mode_of(true, step[k]) == modes[k] &&
          !strcmp(dsps_play_name(true, step[k]), names[k]));
    if (k) CHECK(dsps_play_zone(true, step[k] - 1) == k - 1);   // each zone's first value
  }
  CHECK(dsps_play_zone(true, 127) == kPlayWave);
  // DSP SAMPLE: MOD's 7 zones as before (57: GRAIN there, SLICE on SAMPLER)
  CHECK(dsps_play_zone(false, 57) == -1 && dsps_mode_of(false, 57) == 3 && !strcmp(dsps_play_name(false, 57), "GRAIN"));
  CHECK(dsps_mode_of(false, 15) == 0 && dsps_mode_of(false, 19) == 1 && !strcmp(dsps_play_name(false, 0), "ONE"));
}

// No resonator on SAMPLER: its P1..P8 up (an old sound's MIX 64 and the resonator's knobs) play exactly the dry sample;
// the same sound as DSP SAMPLE (no tag) runs the resonator
static void t_no_resonator() {
  reset_p();
  g_tag = kSmplTag;
  warm();
  trig();
  std::vector<int16_t> dry, smpl, dsps;
  render(4800, &dry);
  P[0] = 90, P[1] = 120, P[2] = 100, P[3] = 30, P[4] = 64, P[5] = 99, P[6] = 100, P[7] = 127;
  trig();
  render(4800, &smpl);
  CHECK(dry == smpl);
  CHECK(near(atanh(dry[0] / (0.7 * 32767.0)), ramp(0)));
  g_tag = 0;
  trig();
  render(4800, &dsps);
  CHECK(dsps != dry);   // MIX 64: the resonator mixed in
  g_tag = kSmplTag;
  reset_p();
}

static void t_rev_ram() {
  reset_p();
  P[11] = 15;   // REV
  warm();
  trig();
  std::vector<double> o = render(24000);
  CHECK(near(o[0], ramp(kN - 1)) && near(o[12000], ramp(kN - 1 - 12000)) && o[100] < o[0]);
  CHECK(sounding_frames(30000) > 23000 && !engines[0].playing());   // down to STA, then silent (48000 in all)
  P[12] = 30, P[13] = 90;   // STRT 30 END 90: from END (frame 3/4) down to STA (frame 1/4)
  trig();
  o = render(1200);
  CHECK(near(o[0], ramp(0.75 * (kN - 1))));
  P[12] = 90, P[13] = 30;   // END before STRT: REV plays it forward (from END up to STRT)
  trig();
  o = render(1200);
  CHECK(near(o[0], ramp(0.25 * (kN - 1))) && o[600] > o[0]);
  P[11] = 0;                // and FWD backwards (from STRT down to END)
  trig();
  o = render(1200);
  CHECK(near(o[0], ramp(0.75 * (kN - 1))) && o[600] < o[0]);
  reset_p();
}

// LOOP 0 = OFF: FLP loops the whole STRT..END; FWD plays it once
static void t_loop_off() {
  reset_p();
  P[13] = 12;   // END 12: frames 0..4800
  P[11] = 29;   // FLP
  warm();
  trig();
  std::vector<double> o = render(4800 * 4);
  CHECK(engines[0].playing());
  double lo = 1, hi = 0;
  for (double x : o) lo = x < lo ? x : lo, hi = x > hi ? x : hi;
  CHECK(near(lo, ramp(0)) && near(hi, ramp(4800), 0.005));   // the whole region, over and over
  CHECK(near(o[4800 * 3 + 10], ramp(10), 0.005));            // back at STRT each time
  P[11] = 0;    // FWD: once
  trig();
  CHECK(sounding_frames(4800 * 3) <= 4812 && !engines[0].playing());
  reset_p();
}

// LOOP's point: FLP plays STRT..END, then loops [LOOP, END]; RLP plays END..STRT, then loops [STRT, LOOP]; a point
// outside STRT..END (review M1: either side), the whole region
static void t_loop_point() {
  reset_p();
  P[13] = 24, P[14] = 12;   // END 24 (frame 9600), LOOP 12 (frame 4800)
  P[11] = 29;               // FLP
  warm();
  trig();
  std::vector<double> o = render(9600 * 4);
  CHECK(near(o[0], ramp(0)));
  double lo = 1, hi = 0;
  for (size_t i = 9700; i < o.size(); ++i) lo = o[i] < lo ? o[i] : lo, hi = o[i] > hi ? o[i] : hi;
  CHECK(near(lo, ramp(4800), 0.005) && near(hi, ramp(9600), 0.005));
  P[11] = 43;               // RLP
  trig();
  o = render(9600 * 4);
  CHECK(near(o[0], ramp(9600), 0.005) && o[50] < o[0]);
  lo = 1, hi = 0;
  for (size_t i = 9700; i < o.size(); ++i) lo = o[i] < lo ? o[i] : lo, hi = o[i] > hi ? o[i] : hi;
  CHECK(near(lo, ramp(0), 0.005) && near(hi, ramp(4800), 0.005));
  for (float pt : {30.0f, 24.0f}) {   // past END, on END: the whole region
    P[14] = pt;
    trig();
    o = render(9600 * 3);
    lo = 1, hi = 0;
    for (size_t i = 9700; i < o.size(); ++i) lo = o[i] < lo ? o[i] : lo, hi = o[i] > hi ? o[i] : hi;
    CHECK(near(lo, ramp(0), 0.005) && near(hi, ramp(9600), 0.005));
  }
  P[12] = 6, P[14] = 3;     // before STRT (STRT 6): the whole region too
  P[11] = 29;
  trig();
  o = render(9600 * 3);
  lo = 1, hi = 0;
  for (size_t i = 9700; i < o.size(); ++i) lo = o[i] < lo ? o[i] : lo, hi = o[i] > hi ? o[i] : hi;
  CHECK(near(lo, ramp(2400), 0.005) && near(hi, ramp(9600), 0.005));
  reset_p();
}

// D2: TUNE 4..124 is -60..+60 semitones (the engine does not clamp it to the stock record's +-24)
static void t_tune() {
  reset_p();
  warm();
  P[8] = 124;   // +60: 32 times as fast
  trig();
  int n = sounding_frames(4800);
  CHECK(n >= kN / 32 - 2 && n <= kN / 32 + kBlock);
  P[8] = 4;     // -60: a 32nd
  trig();
  render(9600);
  CHECK(engines[0].playing() && std::fabs(engines[0].Progress() * kN - 9600.0 / 32) < 2);
  P[8] = 88;    // +24 (the old record's end) still as before
  trig();
  n = sounding_frames(24000);
  CHECK(n >= kN / 4 - 2 && n <= kN / 4 + kBlock);
  reset_p();
}

// SMP OFF: silent at once, no stream, no wait for a sample (a RAM-only PLAY too)
static void t_smp_off() {
  reset_p();
  warm();
  P[10] = 0;
  P[11] = 57;   // SLICE: plays from the sample RAM only (a sample to wait for, were there one)
  send(0x03);
  CHECK(prepared[0][0] == -1 && !prep_pending[0]);
  uint32_t late = ram_late;
  trig();
  CHECK(!from_ram[0] && !streams[0].playing() && !trig_pending[0]);
  std::vector<double> o = render(4800);
  for (double x : o) CHECK(x == 0.0);
  g_us += 2000000, host_fake_time(true, g_us);   // past the RAM wait's second
  for (int i = 0; i < 10; ++i) idle_stream();
  CHECK(ram_late == late);
  P[11] = 0;    // FWD (a mode that streams): no stream either
  trig();
  settle();
  CHECK(!streams[0].playing() && !from_ram[0]);
  reset_p();
}

// M1 (a point outside the region streams: no wait) on A002 before the sample RAM has it, then M2: once it is there
// (cut: its first kMaxFrames only), a loop point is ignored and the whole file streams
static void t_long_sample() {
  reset_p();
  P[10] = 2, P[11] = 29, P[14] = 127;   // A002, FLP, LOOP past END 120
  trig();
  settle();
  CHECK(streams[0].playing() && !from_ram[0]);
  P[11] = 57;   // SLICE: from the sample RAM only - A002 there (cut: its first kMaxFrames), as before
  send(0x03);
  for (int i = 0; i < 20000 && !from_ram[0]; ++i) {
    if (i % 50 == 0) trig();
    idle_prepare(), idle_stream(), g_us += 1000, host_fake_time(true, g_us);
  }
  CHECK(from_ram[0]);
  P[11] = 29;
  for (float lp : {60.0f, 0.0f}) {   // FLP with a loop point, and without: the whole file from the card
    P[14] = lp;
    trig();
    settle();
    CHECK(streams[0].playing() && !from_ram[0] && !trig_pending[0]);
    std::vector<double> o = render(1200);
    CHECK(o[600] != 0.0);
  }
  reset_p();
  warm();       // A001 (fits): a loop point plays from the sample RAM
  P[11] = 29, P[14] = 60;
  trig();
  CHECK(from_ram[0]);
  reset_p();
}

// L1: the sample editor's preview plays its region once, whatever FLP's loop point (the render keeps it off)
static void t_preview() {
  reset_p();
  P[11] = 29, P[14] = 60;
  warm();
  static int16_t pcm[kN + 1];
  for (int i = 0; i <= kN; ++i) pcm[i] = (int16_t)std::lround(32767.0 * ramp(i));
  const DspOverride o = {pcm, (uint32_t)kN, 48000.0f, 1, pool_slot(P[10]), -1, -1};
  dsps_override(0, &o);
  dsps_preview(0, 12000, 13200);   // 1200 frames
  int n = sounding_frames(9600);
  CHECK(n >= 1188 && n <= 1200 + kBlock && !engines[0].playing());
  dsps_override(0, nullptr);
  reset_p();
}

// A report without the tag while SAMPLER's REV plays: DSP SAMPLE's rules from then on, no crash; the next trig forward
static void t_tag_flip() {
  reset_p();
  P[11] = 15;   // REV
  warm();
  trig();
  std::vector<double> o = render(2400);
  CHECK(o[600] < o[0]);
  g_tag = -1;   // a report without the tag field mid-note: still SAMPLER, still REV
  send(0x03);
  CHECK(v[0].smpl);
  o = render(2400);
  CHECK(o[600] < o[0]);
  g_tag = 0;   // DSP SAMPLE's tag mid-note
  send(0x03);
  CHECK(!v[0].smpl);
  o = render(2400);
  for (double x : o) CHECK(std::isfinite(x));
  trig();   // MOD 15: ONE, forward
  o = render(2400);
  CHECK(near(o[0], ramp(0)) && o[1200] > o[0]);
  g_tag = kSmplTag;
  reset_p();
}

// The sequencer's lock pages (trigs held + SRC / SMPL): SAMPLER's page 1 locks its knobs' own params (SMP STRT END PLAY /
// TUNE FINE LOOP LEV since the 2026-11-12 layout: SRC knob B STRT, P13; E TUNE, P9), page 2 none; DSP SAMPLE P1..P8 / P9..P16 as before; another machine untouched
static void t_seq_locks() {
  track33(0, kRytmSampler, kDspSample | 0);
  const int want[8] = {10, 12, 13, 11, 8, 9, 14, 15};
  for (int e = 0; e < 8; ++e) CHECK(seq::lock_param(seq::kLpSrc, e) == want[e] && seq::lock_param(seq::kLpSmpl, e) == -1);
  track33(0, 45, kDspSample | 0);
  for (int e = 0; e < 8; ++e) CHECK(seq::lock_param(seq::kLpSrc, e) == e && seq::lock_param(seq::kLpSmpl, e) == 8 + e);
  track33(0, 44, kPlaits | 0);
  CHECK(seq::lock_param(seq::kLpSrc, 1) == 1 && seq::lock_param(seq::kLpSmpl, 1) == 9);
}

int main() {
  char tmpl[] = "/tmp/core-sampler-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples/S") && fs.mkdir("/.rytm/projects/SMP"));
  write_wav("/Samples/S/ramp.wav", kN, 1, 16, 48000, [](int i, int) { return (int32_t)std::lround(32767.0 * ramp(i)); });
  write_wav("/Samples/S/long.wav", kLong, 1, 16, 48000, [](int i, int) { return (int32_t)(8000 + (i % 4800)); });
  FILE* f = fopen(host_path("/.rytm/project").c_str(), "wb");
  fputs("SMP", f);
  fclose(f);
  f = fopen(host_path("/.rytm/projects/SMP/samples.pool.txt").c_str(), "wb");   // SMP 1 = A001, 2 = A002
  fprintf(f, "#sidecar samplepool 1\nA001 sample - %d /Samples/S/ramp.wav\tramp\nA002 sample - %d /Samples/S/long.wav\tlong\n",
          44 + 2 * kN, 44 + 2 * kLong);
  fclose(f);
  setenv("CORE_POOL_RAM_MB", "64", 1);
  setenv("CORE_SAMPLE_RAM_MB", "16", 1);   // the Pi: the sample plays from the sample RAM
  CHECK(engine_begin() && card_ok && psram && engines);
  for (int i = 0; i < 2000 && npool < 2; ++i) idle_stream();
  CHECK(npool == 2 && pool[0].folder >= 0 && pool[1].folder >= 0);
  CHECK(kLong > (int)rytm::SampleRamPool::kMaxFrames);

  t_long_sample();   // first: A002 not in the sample RAM yet
  t_identity();
  t_zones();
  t_no_resonator();
  t_rev_ram();
  t_loop_off();
  t_loop_point();
  t_tune();
  t_smp_off();
  t_preview();
  t_tag_flip();
  t_seq_locks();
  CHECK(system((std::string("rm -rf '") + tmpl + "'").c_str()) == 0);
  printf("test_sampler: PASS\n");
  return 0;
}
