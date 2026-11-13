// core/tests/test_ui_wave.cpp - DSP SAMPLE's and GRAIN's pages (core/ui/page_wave.cpp): the overview of the pool slot's
// file (exact min / max per column for a short file, read in runs for a long one, 24-bit stereo, an unreadable or
// empty slot), read a column a millisecond while the page is open and kept; the page chosen by MOD (GRAIN's zone with
// the sample RAM); the markers following STA / END / LOP; the grain window following POS / SIZE.
#include <cmath>
#include <cstring>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "lib/pools.h"
#include "core_port.h"   // host_fake_time
#include "engine.h"
#include "pool/pool.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "pool/pcache.h"
#include "lib/library.h"
#include "engine.h"
#include "voices.h"
#include "wav.h"
#include "rui_png.h"

using namespace core::ui;

static std::vector<std::string> texts;
static void terminal(const uint8_t* d, int len) {
  if (len > 13 && d[4] == 0x3D) texts.emplace_back((const char*)d + 12, (size_t)(len - 13));
}
static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  core::engine_sysex(m, 10);
}
static void report03(int vi, int m, const uint8_t p[16]) {
  uint8_t r[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(m | vi)};
  for (int k = 0; k < 16; ++k) r[6 + 2 * k] = p[k], r[7 + 2 * k] = 0;
  r[38] = 0xF7;
  core::engine_sysex(r, 39);
}
// SAMPLER (0.5.0): DSP SAMPLE's flag with its sound tag (machines.h kSmplTag), the 41-byte 0x03
static void report03_smpl(int vi, const uint8_t p[16], int tag = core::kSmplTag) {   // (another tag: DSP SAMPLE's)
  uint8_t r[41] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(core::kDspSample | vi)};
  for (int k = 0; k < 16; ++k) r[6 + 2 * k] = p[k], r[7 + 2 * k] = 0;
  r[38] = (uint8_t)(tag >> 7), r[39] = (uint8_t)(tag & 0x7F), r[40] = 0xF7;
  core::engine_sysex(r, 41);
}
static bool shown(const char* s) {
  uint8_t t[36 * kTextSlots];
  int n = sent_texts(t, kTextSlots);
  for (int i = 0; i < n; ++i)
    if (!strcmp((const char*)t + 36 * i + 4, s)) return true;
  return false;
}
static int px(int x, int y) { return remote_frame()[(7 - y / 8) + 8 * x] >> (y % 8) & 1; }
static int col_px(int x, int y0, int y1) {
  int n = 0;
  for (int y = y0; y <= y1; ++y) n += px(x, y);
  return n;
}

static int16_t a_at(int i) { return (int16_t)(30000.0 * exp(-i / 4000.0) * sin(i * 0.05)); }

int main() {
  char tmpl[] = "/tmp/core_ui_wave_XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples/Kit"));
  const int na = 24000, nb = 3 * 48000;
  write_wav("/Samples/Kit/a.wav", na, 1, 16, 48000, [](int i, int) { return (int32_t)a_at(i); });
  write_wav("/Samples/Kit/b.wav", nb, 2, 24, 48000, [](int i, int c) {   // a ramp up on the left, a burst on the right
    return (int32_t)(c == 0 ? (i * 20000 / (3 * 48000)) : (i > 96000 && i < 97000 ? -25000 : 0));
  });
  write_wav("/Samples/Kit/c.wav", 100, 1, 16, 48000, [](int, int) { return (int32_t)0; }, 3);   // float: not indexed
  setenv("CORE_SAMPLE_RAM_MB", "16", 1);   // the sample RAM (the Pi, a Teensy with PSRAM): MOD picks the mode
  core::hooks.to_rytm = terminal;
  CHECK(core::engine_begin() && core::card_ok && core::psram);
  for (int i = 0; i < 2000 && !core::pools::ready(); ++i) core::idle_stream();   // the project's pool lists in
  int kit = -1;
  for (int k = 1; k < core::cards.folders(); ++k)
    if (!strcmp(core::cards.folder(k).name, "Kit")) kit = k;
  CHECK(kit > 0);
  core::pool_assign(1, kit, 0);   // a.wav
  core::pool_assign(2, kit, 1);   // b.wav
  CHECK(core::npool == 2);

  // voice 1 on DSP SAMPLE, SMP 1, STA 0 END 120, LOP 0, MOD 0 (ONE)
  uint8_t p[16] = {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100};
  report03(0, core::kDspSample, p);
  const Page* pg = page_for_voice(core::kDspSample, 0);
  CHECK(pg && !strcmp(pg->title, "DSP SAMPLE") && page_for_machine(core::kDspSample) == pg && pg->enc[4] == 12);
  uint32_t ms = 1000;
  // the host's clock follows the remote's (as on the Pi: remote_tick gets now_ms()), so pictures paced by now_ms()
  // (extra.h LiveKey, GRANULAR's kFrameMs) see the same time as the remote's ticks
  auto tick = [&](int k) { for (int i = 0; i < k; ++i) core::host_fake_time(true, (uint64_t)++ms * 1000), remote_tick(ms); };
  from_rytm(0x30, 0, 1, 0);
  tick(30);
  from_rytm(0x31, 0, 0x41, 1), tick(30);
  from_rytm(0x31, 0, 0x41, 1), tick(30);   // SIDECAR, SYSTEM, then voice 1's page
  CHECK(remote_stats().page == 1 && remote_stats().voice == 0);
  int st = 0, prog = 0;
  wave_of_voice(0, &st, &prog);
  CHECK(st == 1 && prog > 0 && prog < 128);   // being read: a column a millisecond
  CHECK(shown("LOADING") && shown("DSP SAMPLE"));
  tick(200);
  const void* w = wave_of_voice(0, &st, &prog);
  CHECK(st == 2 && prog == 128);
  for (int c = 0; c < 128; ++c) {   // the overview equals the file's min / max per column (short columns: read whole)
    int lo = 32767, hi = -32768, glo, ghi;
    for (int i = c * na / 128; i < (c + 1) * na / 128; ++i) lo = a_at(i) < lo ? a_at(i) : lo, hi = a_at(i) > hi ? a_at(i) : hi;
    wave_column(w, c, &glo, &ghi);
    CHECK(glo == lo && ghi == hi);
  }
  CHECK(shown("ONE") && !shown("LOADING"));
  int left = col_px(2, 11, 24), right = col_px(99, 11, 24);
  CHECK(left >= 10 && right <= 3);   // loud at the start, near silent at the end (normalised to the file's peak)
  // STA / END: the region's lines move, outside dithered
  from_rytm(0x32, 4, 64 + 60, 0), tick(30);   // STA 60
  from_rytm(0x32, 5, 64 - 30, 0), tick(30);   // END 90
  CHECK((int)core::v[0].p[12] == 60 && (int)core::v[0].p[13] == 90);
  int xs = 1 + 60 * 99 / 120, xe = 1 + 90 * 99 / 120;   // the waveform in x 1..100
  CHECK(col_px(xs, 11, 24) == 14 && col_px(xe, 11, 24) == 14);   // the full-height marker lines
  CHECK(col_px(20, 11, 24) <= 7);                                // left of STA: dithered (every other pixel at most)
  // LOP >= 1 on the streamed / ONE voice: it loops - brackets over the region
  from_rytm(0x32, 6, 64 + 10, 0), tick(30);
  CHECK(px(xs + 1, 10) && px(xe - 1, 10) && shown("ONE"));   // with the sample RAM: MOD 0 = ONE, looping by LOP
  // a long 24-bit stereo file: read in runs, ready, the burst on the right channel seen
  p[10] = 2, p[12] = 0, p[13] = 120, p[14] = 0;
  report03(0, core::kDspSample, p);
  tick(30);
  wave_of_voice(0, &st, &prog);
  CHECK(st == 1);
  tick(200);
  wave_of_voice(0, &st, &prog);
  CHECK(st == 2);
  int burst = 1 + (85 * 100 + 127) / 128;   // frame 96000 of 144000: column 85, screen x 67 (100 columns from x 1)
  CHECK(col_px(burst, 18, 24) >= 3);   // the burst reaches down (a run of the four the column reads covers it)
  // back to slot 1: kept (no reading)
  p[10] = 1;
  report03(0, core::kDspSample, p);
  tick(2);
  wave_of_voice(0, &st, &prog);
  CHECK(st == 2);
  // an empty slot and none
  p[10] = 5;
  report03(0, core::kDspSample, p);
  tick(30);
  CHECK(shown("EMPTY SLOT"));
  p[10] = 0;
  report03(0, core::kDspSample, p);
  tick(30);
  CHECK(shown("SMP OFF"));

  // ---- 0.5.0, SAMPLER (a 0x20 report with SAMPLER's tag): page 1 SMP STRT END PLAY / TUNE FINE LOOP LEV (owner 2026-11-12), the SRC view
  // the same page; PLAY's name, REV's arrow, a loop from LOOP's point; the labels the LFO names params by; a page edit
  // keeps the voice SAMPLER; DSP SAMPLE's page as before
  {
    const std::string pd = rui_png_dir("RUI_PAGES", "build/rui_pages");   // the pages for the docs and the review
    uint8_t fb[kBytes];
    auto png = [&](const char* n) { preview_sent(fb); CHECK(rui_png(pd, n, fb)); };
    const Page* old = page_for_voice(core::kDspSample, 0);
    char lb[8];
    auto vl = [&](int vi, int k) { return std::string(voice_label(vi, k, lb, sizeof lb)); };
    // DSP SAMPLE: its SMPL page's labels and (2026-11, the targets' names) its SRC page's: P5 MIX, P1 ENG (an older Rytm's
    // SRC page here: SMP on SMPL's knob C, P11, is no SRC knob of it)
    CHECK(vl(0, 4) == "MIX" && vl(0, 0) == "ENG" && vl(0, 10) == "" && vl(0, 8) == "TUN" && page_for_machine(core::kDspSample) == old);
    p[10] = 1, p[11] = 0, p[12] = 0, p[13] = 120, p[14] = 0;
    report03_smpl(0, p);
    tick(30);
    CHECK(core::v[0].smpl && remote_voice_smpl(0));
    const Page* dt = page_for_voice(core::kSmplMachine, 0);
    CHECK(dt != old && src_view_page(dt) == dt && page_for_machine(core::kSmplMachine) == dt && !strcmp(dt->title, "SAMPLER"));
    CHECK(page_for_voice(core::kDspSample, 0) == old);   // (DSP SAMPLE's own, whatever the voice)
    const char* want[8] = {"SMP", "STRT", "END", "PLAY", "TUNE", "FINE", "LOOP", "LEV"};
    const int enc[8] = {10, 12, 13, 11, 8, 9, 14, 15};   // the P-numbers stay the words: only the knobs moved
    for (int e = 0; e < 8; ++e) CHECK(!strcmp(dt->label[e], want[e]) && dt->enc[e] == enc[e]);
    // the knob cells under the waveform (E..H) show TUNE FINE LOOP LEV; STRT / END are the waveform's markers
    int below[4] = {-1, -1, -1, -1};
    for (int i = 0; i < dt->ncells; ++i)
      if (dt->cells[i].row == 1 && dt->cells[i].col < 4) below[dt->cells[i].col] = dt->cells[i].in[0];
    CHECK(below[0] == 8 && below[1] == 9 && below[2] == 14 && below[3] == 15);
    CHECK(vl(0, 8) == "TUNE" && vl(0, 14) == "LOOP" && vl(0, 10) == "SMP" && vl(0, 4) == "");   // (P1..P8: no knobs on SAMPLER)
    CHECK(shown("SAMPLER"));
    // (STRT 60 END 90 still: the page's own turns above are held over the reports)
    const int xs = 1 + (int)core::v[0].p[12] * 99 / 120, xe = 1 + (int)core::v[0].p[13] * 99 / 120;
    CHECK(shown("FWD") && !px(xs + 1, 10));
    png("sampler_fwd.png");
    const int arrow = px(xs + 4, 12), l0 = (int)core::v[0].p[14];
    from_rytm(0x32, 3, 64 + 15, 0), tick(30);   // PLAY 15: REV, the arrow (inverted) at the region's top left
    CHECK((int)core::v[0].p[11] == 15 && shown("REV") && px(xs + 4, 12) != arrow && core::v[0].smpl);
    png("sampler_rev.png");
    from_rytm(0x32, 3, 64 + 14, 0), tick(30);   // PLAY 29: FLP, brackets over the whole region (LOOP 0: OFF)
    CHECK(shown("FLP") && px(xs + 1, 10) && px(xe - 1, 10));
    from_rytm(0x32, 6, 64 + 75 - (int)core::v[0].p[14], 0), tick(30);   // LOOP 75: the loop from there, not from STRT
    int xl = 1 + 75 * 99 / 120;
    CHECK((int)core::v[0].p[14] == 75 && px(xl + 1, 10) && !px(xs + 1, 10) && px(xe - 1, 10));
    png("sampler_flp_loop75.png");
    from_rytm(0x32, 3, 64 + 28, 0), tick(30);   // PLAY 57: SLICE (DSP SAMPLE's GRAIN zone)
    CHECK(shown("SLICE") && page_for_voice(core::kSmplMachine, 0) == dt);
    png("sampler_slice.png");
    from_rytm(0x32, 3, 64 + 15, 0), tick(30);   // PLAY 72: GRAIN, its page with SAMPLER's knobs
    const Page* g = page_for_voice(core::kSmplMachine, 0);
    CHECK(!strcmp(g->title, "GRAIN") && !strcmp(g->label[4], "TUNE") && !strcmp(g->label[1], "POS") && g->enc[4] == 8 &&
          g->enc[1] == 12 && !strcmp(g->label[6], "DENS") && g->enc[6] == 14);
    png("sampler_grain.png");
    auto turn = [&](int e, int d) {   // (a turn's delta: +-63 at most)
      for (; d; tick(30)) { int k = d > 60 ? 60 : d < -60 ? -60 : d; from_rytm(0x32, (uint8_t)e, (uint8_t)(64 + k), 0), d -= k; }
    };
    turn(3, -(int)core::v[0].p[11]);   // PLAY and LOOP back as they were (held turns)
    turn(6, l0 - (int)core::v[0].p[14]);
    CHECK(core::v[0].smpl);
    report03(0, core::kDspSample, p);   // a report without the tag field: still SAMPLER (review M2)
    tick(30);
    CHECK(core::v[0].smpl && remote_voice_smpl(0) && shown("FWD"));
    report03_smpl(0, p, 0);   // DSP SAMPLE again (its tag, 0): its pages as they were
    tick(30);
    CHECK(!core::v[0].smpl && page_for_voice(core::kDspSample, 0) == old && (int)core::v[0].p[11] == 0 && shown("ONE"));
    png("dsps_page.png");
  }

  // GRAIN: MOD in its zone (56) with the sample RAM -> the GRAIN page; the window inverted around POS, SIZE wide
  {
    p[10] = 1, p[11] = 56, p[12] = 60, p[13] = 64, p[14] = 100;
    report03(0, core::kDspSample, p);
    tick(30);
    const Page* g = page_for_voice(core::kDspSample, 0);
    CHECK(g && !strcmp(g->title, "GRAIN") && shown("GRAIN") && shown("BD") && !strcmp(g->label[4], "POS"));
    CHECK(remote_stats().page == 1);
    int xc = 1 + 60 * 125 / 120;   // GRAIN's waveform: the cell's whole width (x 1..126)
    // SIZE 64: 10 ms x 50^(64/127) = 72 ms of the 0.5 s file -> ~18 columns of 126: the box's top edge (row 10) spans it
    int top = 0;
    for (int x = xc - 7; x <= xc + 7; ++x) top += px(x, 10);
    CHECK(top == 15 && col_px(xc, 10, 12) == 3 && !px(xc - 30, 10));
    from_rytm(0x32, 5, 64 - 40, 0), tick(30);   // SIZE 24: 21 ms -> ~5 columns
    top = 0;
    for (int x = xc - 7; x <= xc + 7; ++x) top += px(x, 10);
    CHECK(top < 15 && top >= 5);
    from_rytm(0x32, 4, 64 + 30, 0), tick(30);   // POS 90: the box moves right
    int xc2 = 1 + 90 * 125 / 120;
    CHECK(col_px(xc2, 10, 12) == 3 && !px(xc, 10));
  }
  // ---- a RAM-only mode's first trig, its sample not in RAM (no preload, 7ad): it sounds when the sample is in, not never
  {
    core::lib::Ref r;
    core::lib::ref_for_path("/Samples/Kit/b.wav", &r);
    p[10] = 2, p[11] = 56;
    uint8_t t[41] = {0xF0, 0x7D, 0x52, 0x54, 0x04, (uint8_t)core::kDspSample, 64, 100};
    for (int k = 0; k < 16; ++k) t[8 + 2 * k] = p[k], t[9 + 2 * k] = 0;
    t[40] = 0xF7;
    report03(0, core::kDspSample, p);   // (the voice off the sample first: its pin goes)
    p[10] = 1;
    report03(0, core::kDspSample, p);
    tick(2);
    core::pcache::forget(r);
    CHECK(core::pcache::find(r, core::pcache::kWhole) < 0);
    const uint32_t late0 = core::ram_late;
    core::engine_sysex(t, 41);          // SMP 2 p-locked on the trig
    CHECK(!core::from_ram[0]);          // not in yet: it waits
    for (int i = 0; i < 4000 && !core::from_ram[0]; ++i) core::idle_prepare(), core::idle_stream(), tick(1);
    CHECK(core::from_ram[0] && core::ram_late == late0);
    CHECK(core::pcache::find(r, core::pcache::kWhole) >= 0);
    // its gate closed during the wait: it plays and closes, nothing hangs (GRAIN holds while the gate is open)
    report03(0, core::kDspSample, p);
    uint8_t noff[7] = {0xF0, 0x7D, 0x52, 0x54, 0x11, 0, 0xF7};
    core::engine_sysex(noff, 7);
    p[10] = 1;
    report03(0, core::kDspSample, p);
    tick(2);
    core::pcache::forget(r);
    CHECK(core::pcache::find(r, core::pcache::kWhole) < 0);
    core::engine_sysex(t, 41);
    CHECK(!core::from_ram[0]);
    core::engine_sysex(noff, 7);        // the gate, before the sample is in
    for (int i = 0; i < 4000 && !core::from_ram[0]; ++i) core::idle_prepare(), core::idle_stream(), tick(1);
    CHECK(core::from_ram[0]);
    static float bl[256];
    static float pa[8] = {};
    for (int i = 0; i < 400 && core::engines[0].playing(); ++i) core::engines[0].Render(pa, bl, 256);
    CHECK(!core::engines[0].playing());
  }
  // ---- machine graphics (page_gfx.cpp): JUNO's envelope, 6-OP FM's algorithms, GRAIN's cloud
  for (int alg = 0; alg < 32; ++alg) {   // every operator placed; a modulator above what it modulates; within the cell
    int8_t col[6], lev[6];
    int nc = fm_algorithm_layout(alg, col, lev);
    CHECK(nc >= 1 && nc <= 6);
    for (int op = 0; op < 6; ++op) CHECK(col[op] >= 0 && col[op] < nc && lev[op] >= 0 && lev[op] < 4);
  }
  int8_t col[6], lev[6];
  CHECK(fm_algorithm_layout(31, col, lev) == 6 && lev[0] == 0 && lev[5] == 0);   // 32: six carriers side by side
  CHECK(fm_algorithm_layout(0, col, lev) == 2 && lev[5] == 3 && col[5] == col[2]);  // 1: 6 > 5 > 4 > 3, 2 > 1
  {   // fm_info (the Pi display's VOICE page): the ROM's first voice (no FM list on the test card)
    FmInfo f;
    CHECK(fm_info(0, &f) && f.alg >= 0 && f.alg < 32 && f.name[0] && f.fb >= 0 && f.fb <= 7);
    int8_t c2[6], l2[6];
    CHECK(fm_algorithm_layout(f.alg, c2, l2) == f.ncols && !memcmp(c2, f.col, 6) && !memcmp(l2, f.lev, 6));
    int cars = 0;
    for (int k = 0; k < 6; ++k) {
      CHECK(f.out[k] <= 99 && f.freq[k] > 0);
      if (f.car >> k & 1) {
        ++cars;
        CHECK(f.lev[k] == 0);
      }
    }
    CHECK(cars >= 1);
    printf("fm_info: ROM voice 1 \"%s\" alg %d fb %d, op1 out %d freq %d%s\n", f.name, f.alg + 1, f.fb, f.out[0], f.freq[0],
           f.fixed[0] ? " Hz" : " /100");
  }
  Canvas cv;
  fm_algorithm_render(0, cv);
  int on = 0;
  for (int i = 0; i < 1024; ++i) on += __builtin_popcount(cv.fb[i]);
  CHECK(on > 40);
  // the cloud: SIZE wider grains -> more pixels; DENS more grains
  auto cloud_px = [&]() { int n = 0; for (int x = 64; x < 96; ++x) n += col_px(x, 33, 47); return n; };
  int c_small = cloud_px();
  from_rytm(0x32, 5, 64 + 60, 0), tick(30);   // SIZE up
  CHECK(cloud_px() != c_small);
  // JUNO on voice 2: its page, the envelope moves with ATK
  uint8_t j[16] = {5, 70, 30, 64, 10, 60, 50, 127, 64, 64, 64, 64, 64, 64, 64, 64};
  report03(1, core::kJuno, j);
  usleep(350000);   // past JUNO's knob hold after a patch change (300 ms of the platform clock)
  tick(30);
  const Page* jp = page_for_machine(core::kJuno);
  CHECK(jp && !strcmp(jp->title, "JUNO") && page_for_machine(core::kFm) && !strcmp(page_for_machine(core::kFm)->title, "6-OP FM"));
  from_rytm(0x31, 0, 0x41, 1), tick(30);   // voice 2's page
  CHECK(remote_stats().page == 1 && remote_stats().voice == 1 && shown("JUNO") && shown("ATK"));
  auto env_px = [&]() { int n = 0; for (int x = 0; x < 30; ++x) n += col_px(x, 33, 47); return n; };
  int e0 = env_px();
  int a0 = (int)core::v[1].p[4];   // the patch's own ATK (JUNO's knob sync)
  from_rytm(0x32, 4, 64 + 50, 0), tick(30);   // ATK +50: a longer attack
  CHECK((int)core::v[1].p[4] == a0 + 50 && env_px() != e0);
  // the GRAIN machine (1026): its placeholder flag 0x18 is CHOP's since rui1051 (no Rytm ever sent it for GRAIN); a
  // report with it plays CHOP, not DSP SAMPLE's GRAIN mode; GRAIN's own page stays (machines.h kGrainMachine)
  uint8_t g[16] = {0, 64, 64, 64, 100, 64, 64, 100, 64, 64, 1, 0, 60, 80, 90, 100};   // P5 (MIX) 100: ignored
  report03(2, core::kChopMachine, g);
  CHECK(core::v[2].cp && !core::v[2].grain && !core::v[2].dsps);
  const Page* gp = page_for_voice(core::kGrainMachine, 2);
  CHECK(gp && !strcmp(gp->title, "GRAIN") && !strcmp(gp->label[3], "SPRY") && !strcmp(gp->label[4], "POS"));
  from_rytm(0x31, 0, 0x41, 1), tick(30);   // voice 3's page: CHOP's
  CHECK(remote_stats().page == 1 && remote_stats().voice == 2 && shown("CHOP") && shown("SLC") && shown("MRPH"));
  report03(2, core::kDspSample, g);   // the same words on DSP SAMPLE: MOD 0 is ONE, not GRAIN
  CHECK(!core::v[2].grain && core::dsp_mode(core::v[2]) == rytm::SampleModes::kOneShot);
  from_rytm(0x30, 0, 0, 3);
  // ---- 1027, the SRC view on DSP SAMPLE: the sample the Rytm's SMPL page picked (2A's P11), not the voice's state (SMP
  // OFF here: nothing sounded since), its overview read while the view shows (the TRK + YES page closed); MOD (P12)
  // into GRAIN's zone on the Rytm turns the view into GRAIN
  {
    uint8_t q[16] = {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100};   // voice 0: SMP OFF, MOD 0
    report03(0, core::kDspSample, q);
    uint8_t mm[9 + 24] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, 0, 12};   // 33: BD on DSP SAMPLE (vb 0x20), the rest analog
    for (int k = 0; k < 12; ++k) mm[8 + 2 * k] = k ? 0 : 45, mm[9 + 2 * k] = k ? 0x7F : 0x20;
    mm[8 + 24] = 0xF7;
    core::engine_sysex(mm, 9 + 24);
    uint8_t v2[10 + 32] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 0, 16};
    const uint8_t words[16] = {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 2, 0, 0, 120, 0, 100};   // SMP 2 (b.wav), ONE
    for (int k = 0; k < 16; ++k) v2[9 + 2 * k] = words[k], v2[10 + 2 * k] = 0;
    v2[9 + 32] = 0xF7;
    core::engine_sysex(v2, 10 + 32);
    const uint8_t on[9] = {0xF0, 0x7D, 0x52, 0x54, 0x29, 1, 0, 1, 0xF7};
    core::engine_sysex(on, 9);
    tick(400);
    int s2 = 0, p2 = 0;
    wave_of_slot(2, &s2, &p2);
    CHECK(remote_stats().page == 4 && shown("DSP SAMPLE") && !shown("SMP OFF") && !shown("LOADING") && s2 == 2);
    CHECK((int)core::v[0].p[10] == 0);   // the voice untouched
    CHECK(col_px(1 + (85 * 100 + 127) / 128, 18, 24) >= 3);   // b.wav's burst drawn
    {   // the playhead: a voice reading at half the file inverts that column of the waveform, gone when it stops
      int xh = 1 + 32767 * 99 / 65534, c0 = col_px(xh, 11, 24);
      bool was = core::v[0].dsps, live = core::v[0].live;
      core::v[0].dsps = true, core::v[0].live = true;
      core::g_dsp_head[0] = 1 + 32767;
      tick(100);
      int c1 = col_px(xh, 11, 24);
      CHECK(core::dsp_playhead(0) == 32767 && c1 != c0);
      core::g_dsp_head[0] = 0;
      tick(100);
      CHECK(core::dsp_playhead(0) == -1 && col_px(xh, 11, 24) == c0);
      core::v[0].dsps = was, core::v[0].live = live;
    }
    CHECK(shown("ENG"));   // a 0.3.x Rytm (no 36-name asks): its knob A is still ENG
    remote_rytm_040(true);
    tick(400);
    // the Rytm's encoders on its SRC page are P1..P8 (DSP SAMPLE's resonator): the view shows those (SMP, STR..LEV), and a
    // turn of MIX (P5, 2A first 4) moves its knob. (It showed P9..P16 only, the SMPL page's: dead under the encoders.)
    CHECK(shown("SMP") && shown("MIX") && shown("LEV") && !shown("FIN") && !shown("STA"));
    auto knob_px = [&]() { int n = 0; for (int x = 0; x < 32; ++x) n += col_px(x, 31, 50); return n; };
    int k0 = knob_px();
    const uint8_t mix[12] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 4, 1, 127, 0, 0xF7};
    core::engine_sysex(mix, 12);
    tick(30);
    CHECK(knob_px() != k0);
    const uint8_t mod[12] = {0xF0, 0x7D, 0x52, 0x54, 0x2A, 1, 0, 11, 1, 56, 0, 0xF7};   // MOD 56: GRAIN's zone
    core::engine_sysex(mod, 12);
    tick(30);
    CHECK(shown("GRAIN") && shown("SMP") && !shown("POS") && remote_stats().page == 4);   // the SRC page's knobs under the grain view
    const uint8_t off[9] = {0xF0, 0x7D, 0x52, 0x54, 0x29, 1, 0, 0, 0xF7};
    core::engine_sysex(off, 9);
  }
  // ---- 0.5.0 review M2: the page's own 0x03 (a page edit) never turns a SAMPLER voice or page into DSP SAMPLE, and a
  // report without the tag field leaves the voice as it was: before the voice's first trig (a fresh 33 names SAMPLER), a
  // kit change from DSP SAMPLE to SAMPLER, a shared voice (RS / CP) whose first track is DSP SAMPLE
  {
    remote_rytm_040(false), remote_reset();   // (a fresh link without the SRC view: the voice pages in the page cycle)
    from_rytm(0x30, 0, 0, 3), tick(30);
    auto t33 = [&](int t, int m) {
      const uint8_t mm[11] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, (uint8_t)m, 0x20, 0xF7};
      core::engine_sysex(mm, 11);
    };
    auto open_voice = [&](int vi) {
      from_rytm(0x30, 0, 0, 3), tick(30);
      from_rytm(0x30, 0, 1, 0), tick(30);
      for (int i = 0; i < 16 && !(remote_stats().page == 1 && remote_stats().voice == vi); ++i) from_rytm(0x31, 0, 0x41, 1), tick(30);
      CHECK(remote_stats().page == 1 && remote_stats().voice == vi);
    };
    auto edit = [&]() { from_rytm(0x32, 1, 65, 0), tick(30), from_rytm(0x32, 1, 63, 0), tick(30); };   // knob B up, down
    uint8_t q[16] = {0, 64, 64, 64, 0, 64, 64, 100, 64, 64, 1, 0, 0, 120, 0, 100};
    report03_smpl(0, q, 0), tick(30);   // BD's last report: DSP SAMPLE's (its tag)
    t33(0, core::kRytmSampler), tick(30);   // then a 33: BD on SAMPLER, no trig since
    CHECK(!core::v[0].smpl && remote_voice_smpl(0));
    open_voice(0);
    CHECK(shown("SAMPLER"));
    edit();
    CHECK(core::v[0].smpl);   // the edit's 0x03 carried SAMPLER's tag
    report03(0, core::kDspSample, q), tick(30);
    CHECK(core::v[0].smpl);   // no tag field: no news
    report03_smpl(0, q, 0), t33(0, 45), tick(30);   // BD's kit: DSP SAMPLE (its trig and 33)
    CHECK(!core::v[0].smpl && !remote_voice_smpl(0) && shown("DSP SAMPLE"));
    t33(0, core::kRytmSampler), tick(30);   // the next kit: SAMPLER, before its first trig
    edit();
    CHECK(core::v[0].smpl);
    t33(2, 45), t33(3, core::kRytmSampler);   // RS (first on voice 8) DSP SAMPLE, CP SAMPLER; CP played last
    report03_smpl(7, q), tick(30);
    CHECK(core::v[7].smpl && !remote_voice_smpl(7));
    open_voice(7);
    edit();
    CHECK(core::v[7].smpl);
    report03(7, core::kDspSample, q), tick(30);
    CHECK(core::v[7].smpl);
    report03_smpl(7, q, 0), tick(30);   // RS's trig (DSP SAMPLE's tag): DSP SAMPLE
    CHECK(!core::v[7].smpl);
    // the reverse (re-review LOW 1): RS SAMPLER (first on the voice), CP DSP SAMPLE, CP selected on the Rytm and played
    // last: an edit is CP's, so no SAMPLER tag; with RS selected, the edit is RS's: SAMPLER
    auto select = [&](int t, int m) {   // 49 02: the Rytm's selected track, its machine, its name
      uint8_t mm[22] = {0xF0, 0x7D, 0x52, 0x54, 0x49, 1, 2, (uint8_t)t, (uint8_t)m};
      for (int i = 0; i < 12; ++i) mm[9 + i] = i < 2 ? (uint8_t)"RSCP"[2 * (t - 2) + i] : 0x20;
      mm[21] = 0xF7;
      core::engine_sysex(mm, 22);
      tick(30);
    };
    t33(2, core::kRytmSampler), t33(3, 45), tick(30);
    select(3, 45);
    CHECK(remote_selected_track(nullptr) == 3);
    report03_smpl(7, q, 0), tick(30);   // CP's trig
    CHECK(!core::v[7].smpl && remote_voice_smpl(7));
    open_voice(7);
    edit();
    CHECK(!core::v[7].smpl);   // CP's edit: DSP SAMPLE stays
    select(2, core::kRytmSampler);
    edit();
    CHECK(core::v[7].smpl);   // RS's edit: SAMPLER
    from_rytm(0x30, 0, 0, 3), tick(30);
  }
  printf("ALL UI WAVE TESTS PASS\n");
  return 0;
}
