// core/tests/test_ui_live.cpp - the remote UI's pages drawn by code (core/ui/extra.h) and what feeds them: the input
// tap's reduction (min / max entries of the selected source, per-channel peaks, the voice -> track channel map, an
// overrun), the meters' dB scale, fall and hold, the SCOPE page through the host (LEFT from SIDECAR, its catch mask,
// its encoders, its frame rate and tile budget while music plays, NO back, the tap only while it shows).
#include <cmath>
#include <cstring>
#include <vector>

#include <unistd.h>   // mkdtemp

#include "check.h"
#include "engine.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core::ui;
namespace core {
namespace ui {
int scope_updates();
void scope_reset();
}  // namespace ui
}  // namespace core

// ---- a terminal that counts: 38 tiles, 39 commits, bytes, the last 37 mask's encoders, 3D texts
static int commits = 0, tiles = 0, masks = 0;
static long bytes = 0;
static unsigned mask_enc = 0;
static std::vector<std::string> texts;
static unsigned mask_trigs = 0;
static int led_mode = -1, led_msgs = 0;
static uint8_t led_c[16];
static void terminal(const uint8_t* d, int len) {
  if (len < 7 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54 || d[4] < 0x30 || d[4] > 0x3F) return;
  bytes += len;
  const uint8_t* b = d + 6;
  if (d[4] == 0x38) tiles += b[3] * b[4];
  else if (d[4] == 0x39) ++commits;
  else if (d[4] == 0x37) {
    mask_enc = b[0] | b[1] << 7 | b[2] << 14;
    mask_trigs = 0;
    for (int k = 0; k < 16; ++k)
      if (3 + k / 7 < len - 7 && (b[3 + k / 7] >> (k % 7) & 1)) mask_trigs |= 1u << k;
    ++masks;
  } else if (d[4] == 0x34) {   // 1026: the trig LEDs (mode, c1..c16)
    led_mode = (len == 8 && b[0] == 0) || (len == 7 + 17 && b[0] == 1) ? b[0] : -2;   // 34 0 | 34 1 <c1..c16>
    if (len == 7 + 17) memcpy(led_c, b + 1, 16);
    ++led_msgs;
  } else if (d[4] == 0x3D) {
    texts.emplace_back((const char*)b + 6, (size_t)(len - 7 - 6));
  }
}
static void from_rytm(uint8_t cmd, uint8_t a, uint8_t b, uint8_t c) {
  const uint8_t m[10] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, a, b, c, 0xF7};
  core::engine_sysex(m, 10);
}
static void key(int code) {
  from_rytm(0x31, (uint8_t)(code >> 7), code & 0x7F, 1);
  from_rytm(0x31, (uint8_t)(code >> 7), code & 0x7F, 0);
}
static bool has_text(const char* s) {
  for (auto& t : texts)
    if (t == s) return true;
  return false;
}

// 12 channels of a test signal: channel c a sine of (c + 1) x 50 Hz at amplitude (c + 1) / 16 of full scale
static void signal(int32_t* f, int n, int at) {
  for (int i = 0; i < n; ++i)
    for (int c = 0; c < kTapCh; ++c)
      f[i * kTapCh + c] = (int32_t)(2147483647.0 * (c + 1) / 16 * sin(2 * M_PI * 50.0 * (c + 1) * (at + i) / 48000.0));
}

int main() {
  // ---- dB x 2 below full scale
  CHECK(db2_below(32767) == 0 && db2_below(0) == kMeterFloor && db2_below(1) == kMeterFloor);
  CHECK(std::abs(db2_below(16384) - 12) <= 1 && std::abs(db2_below(3277) - 40) <= 1 && std::abs(db2_below(1036) - 60) <= 1);
  for (uint32_t p = 1; p < 32767; p = p * 5 / 4 + 1) {   // within half a dB everywhere above the floor
    double want = 2 * 20 * log10(32767.0 / p);
    if (want < kMeterFloor) CHECK(std::fabs(db2_below(p) - want) <= 1.0);
  }

  // ---- the tap: nothing while off; MAIN = (L + R) / 2 in entries of kTapFrames; peaks per channel; a voice's channel
  tap_reset();
  int32_t f[64 * kTapCh];
  signal(f, 64, 0);
  tap_in(f, 64, kTapCh);
  CHECK(tap_write_pos() == 0);
  tap_enable(true);
  tap_source(0);
  for (int i = 0; i < 64 * kTapCh; ++i) f[i] = 0;
  for (int i = 0; i < 16; ++i) f[i * kTapCh + 0] = (i - 8) << 20, f[i * kTapCh + 1] = (i - 8) << 20;
  f[3 * kTapCh + 7] = -(1 << 30);   // channel 7's peak
  tap_in(f, 5, kTapCh);              // in two calls: an entry spans them
  tap_in(f + 5 * kTapCh, 11, kTapCh);
  CHECK(tap_write_pos() == 2);
  uint32_t rd = 0;
  int16_t e[8][2];
  CHECK(tap_read(&rd, e, 8) == 2 && rd == 2);
  CHECK(e[0][0] == (-8 << 4) && e[0][1] == (-1 << 4) && e[1][0] == 0 && e[1][1] == (7 << 4));   // >> 16 of (i - 8) << 20
  uint16_t pk[kTapCh];
  tap_take_peaks(pk);
  CHECK(pk[7] == 16383 && pk[0] == (8 << 4) - 1 && pk[2] == 0);   // |x| as ~x for negatives (no overflow)
  tap_take_peaks(pk);
  CHECK(pk[7] == 0 && pk[0] == 0);   // taken: since the last call
  tap_source(2);                     // source 2 = track SD's channel 2 + 1 (track order; SD plays on Sidecar voice 2)
  CHECK(tap_channel_of_voice(1) == 5 && tap_channel_of_voice(2) == 3 && !strcmp(tap_source_name(2), "SD"));
  for (int i = 0; i < 64 * kTapCh; ++i) f[i] = 0;
  for (int i = 0; i < 8; ++i) f[i * kTapCh + 3] = 1 << 28, f[i * kTapCh + 2] = -(1 << 29);
  tap_in(f, 8, kTapCh);
  CHECK(tap_read(&rd, e, 8) == 1 && e[0][0] == (1 << 12) && e[0][1] == (1 << 12));
  for (int k = 0; k < kTapRing / 4 + 10; ++k) tap_in(f, 32, kTapCh);   // more than the ring holds
  int n = 0;
  for (int got; (got = tap_read(&rd, e, 8)) > 0;) n += got;
  CHECK(n == kTapRing - 64 && rd == tap_write_pos());   // an overrun: the oldest the writer is not near, then current
  tap_reset();

  // ---- the meters: a peak rises at once, falls 24 dB a second, the hold stays 1 s
  Meters m;
  uint16_t p0[kTapCh] = {}, p1[kTapCh] = {};
  p1[3] = 32767;
  m.update(p0, 1000);
  CHECK(m.level[3] == kMeterFloor && m.hold[3] == kMeterFloor);
  m.update(p1, 1100);
  CHECK(m.level[3] == 0 && m.hold[3] == 0);
  m.update(p0, 1600);   // 0.5 s: 12 dB lower, the hold still at 0 dB
  CHECK(m.level[3] == 24 && m.hold[3] == 0);
  m.update(p0, 2200);   // past 1 s: the hold falls too, never under the level
  CHECK(m.level[3] == 24 + 28 && m.hold[3] <= m.level[3] && m.hold[3] > 0);
  Canvas c;
  m.draw(c, 34, 14, kSlotFree + 8);
  CHECK(!strcmp(c.txt[kSlotFree + 8].s, "MAIN") && !strcmp(c.txt[kSlotFree + 9].s, "BD") && !strcmp(c.txt[kSlotFree + 10].s, "SD") && !strcmp(c.txt[kSlotFree + 17].s, "IN"));

  // ---- SCOPE through the host
  char tmpl[] = "/tmp/core_ui_live_XXXXXX";
  CHECK(mkdtemp(tmpl));
  core::host_set_card(tmpl);
  core::hooks.to_rytm = terminal;
  core::engine_begin();
  {   // track 1 reported on PLAITS: the only voice page
    uint8_t r[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, 0};
    r[38] = 0xF7;
    core::engine_sysex(r, 39);
  }
  scope_reset();
  uint32_t ms = 1000;
  auto tick = [&](int k, bool audio) {
    static int at = 0;
    for (int i = 0; i < k; ++i) {
      if (audio) {
        int32_t blk[48 * kTapCh];
        signal(blk, 48, at);
        at += 48;
        tap_in(blk, 48, kTapCh);
      }
      remote_tick(++ms);
    }
  };
  from_rytm(0x30, 0, 1, 0);
  tick(30, false);
  CHECK(remote_stats().page == 0 && !tap_on());
  key(0x40);   // LEFT on SIDECAR: the last page, SCOPE
  tick(30, false);
  CHECK(remote_stats().page == 3 && remote_stats().voice == 2 && tap_on());   // extra page 2 (LFO 0, REC 1: not shown here)
  CHECK(mask_enc == 0x07);   // A SRC, B TIME, C GAIN
  CHECK(has_text("SCOPE") && has_text("MAIN") && has_text("SRC MAIN  43MS  AUTO X64"));
  float before[16];
  for (int k = 0; k < 16; ++k) before[k] = core::v[0].p[k];
  from_rytm(0x32, 0, 64 + 1, 0);   // SRC +1: BD (the tracks in order)
  from_rytm(0x32, 1, 64 + 2, 0);   // TIME +2: 171 ms, a sweep
  from_rytm(0x32, 2, 64 + 3, 0);   // GAIN x4
  from_rytm(0x32, 5, 64 + 1, 0);   // F: not the page's (no effect)
  tick(30, false);
  for (int k = 0; k < 16; ++k) CHECK(core::v[0].p[k] == before[k]);   // the scope's knobs edit no voice
  CHECK(has_text("BD") && has_text("SRC BD  171MS  X4"));
  // music for 4 s: at most 15 updates a second, the tiles within the budget, the frame changes
  int c0 = commits, t0 = tiles, u0 = scope_updates();
  long b0 = bytes;
  tick(4000, true);
  int fr = commits - c0, ups = scope_updates() - u0;
  printf("scope: %d frames, %d updates, %d tiles, %ld bytes in 4 s\n", fr, ups, tiles - t0, bytes - b0);
  CHECK(ups >= 4 * 12 && ups <= 4 * 15 + 1 && fr <= ups + 2 && fr >= 4 * 10);
  CHECK(tiles - t0 <= 4 * 900 + 90 && tiles - t0 > 4 * 50);
  const uint8_t* fb = remote_frame();
  int px = 0;
  for (int x = 0; x < 128; ++x)
    for (int y = 11; y < 32; ++y) px += fb[(7 - y / 8) + 8 * x] >> (y % 8) & 1;
  CHECK(px > 128);   // the waveform is drawn
  // the 21 ms timebase, MAIN, AUTO: every update changes the whole wave region, so the budget limits it
  from_rytm(0x32, 0, 64 - 1, 0);
  from_rytm(0x32, 1, 64 - 3, 0);
  from_rytm(0x32, 2, 64 - 3, 0);
  tick(500, true);
  t0 = tiles;
  tick(4000, true);
  printf("scope 21 ms: %d tiles in 4 s\n", tiles - t0);
  CHECK(tiles - t0 <= 4 * 900 + 90);
  key(0x45);   // NO: SIDECAR, the tap off
  tick(30, false);
  CHECK(remote_stats().page == 0 && !tap_on());
  key(0x40);
  tick(30, false);
  CHECK(tap_on());
  // ---- REC: shown once the platform reports a recorder, before SCOPE; YES and encoder A as requests for the platform
  key(0x45);
  tick(30, false);
  RecInfo ri = {};
  ri.state = kRecIdle, ri.free_mb = 12 * 1024 + 300, ri.total_mb = 30000, ri.need_kbps = 1152, ri.in_mask = 0x3 | 1 << 2;
  strcpy(ri.take, "take-0007");
  remote_rec(ri);
  // SIDECAR -> AUTO -> SAMPLE -> SCOPE -> REC (rui1051: POOL, shown with REC, is off the ring: UP + T13 opens it)
  key(0x40), tick(30, false);
  CHECK(remote_stats().page == 3 && remote_stats().voice == 4 && has_text("AUTO") && !has_text("POOL"));
  key(0x40), tick(30, false);
  CHECK(remote_stats().page == 3 && remote_stats().voice == 3 && has_text("SAMPLE") && has_text("YES: RECORD  NO: BACK"));
  key(0x40), tick(30, false);
  CHECK(remote_stats().page == 3 && remote_stats().voice == 2);
  key(0x40), tick(30, false);
  CHECK(remote_stats().page == 3 && remote_stats().voice == 1 && tap_on() && mask_enc == 0x01);   // A (no player yet: the Mini)
  CHECK(has_text("REC") && has_text("IDLE") && has_text("TAKE-0007") && has_text("12 GB") && has_text("YES: REC  A: ARM  3H06M"));
  CHECK(remote_rec_take() == kRecNoRequest);
  key(0x44);   // YES: record
  CHECK(remote_rec_take() == kRecStart && remote_stats().page == 3);
  from_rytm(0x32, 0, 64 + 1, 0);   // A right: arm
  CHECK(remote_rec_take() == kRecArm);
  ri.state = kRecArmed, remote_rec(ri), tick(30, false);
  CHECK(has_text("ARMED") && has_text("PLAY:REC YES:NOW A:OFF"));
  CHECK(!has_text("USB OFF"));
  ri.usb_paused = true, remote_rec(ri), tick(30, false);   // the Mini: USB audio paused for the take, in the card's place
  CHECK(has_text("USB OFF"));
  ri.usb_paused = false, remote_rec(ri), tick(30, false);
  from_rytm(0x32, 0, 64 - 1, 0);   // A left: disarm
  CHECK(remote_rec_take() == kRecDisarm);
  key(0x70);                       // A pushed while armed: disarm too
  CHECK(remote_rec_take() == kRecDisarm);
  // ---- playback: B pushed plays / stops, B turned the channel, C the take, D the level; the playback view while playing
  // or 3 s after a turn; nothing while a take records
  ri.state = kRecIdle, remote_rec(ri);
  PlayInfo pi = {};
  pi.state = kPlayStopped, pi.level = 100, pi.ch_i = 0, pi.channels = 3, pi.take_i = 0, pi.takes = 2, pi.len_ms = 90000;
  strcpy(pi.take, "take-0007"), strcpy(pi.channel, "MASTER");
  key(0x71), from_rytm(0x32, 1, 64 + 1, 0);   // before a player: B pushed / turned are not the page's
  PlayInput in;
  CHECK(!remote_play_take(&in));
  remote_play(pi), tick(30, false);
  CHECK(has_text("IDLE") && has_text("YES: REC  A: ARM  B: PLAY") && mask_enc == 0x0F);   // a player: A..D
  CHECK(!remote_play_take(&in));
  from_rytm(0x32, 1, 64 + 1, 0);   // B right: the next channel
  CHECK(remote_play_take(&in) && in.channel == 1 && !in.toggle && !in.take && !in.level);
  tick(30, false);
  CHECK(has_text("PLAY") && has_text("TAKE-0007") && has_text("MASTER 1/3") && has_text("B: PLAY  C: TAKE  D: 100"));
  key(0x71);                       // B pushed: play
  CHECK(remote_play_take(&in) && in.toggle == 1 && remote_stats().page == 3);
  pi.state = kPlayPlaying, pi.ch_i = 1, pi.pos_ms = 45000, strcpy(pi.channel, "BD");
  remote_play(pi), tick(30, false);
  CHECK(has_text("PLAYING") && has_text("BD 2/3") && has_text("B: STOP  D: LEV 100  1:30"));
  int bar = 0;   // the progress bar: rows 27 / 28 filled to the playhead (half way)
  for (int x = 0; x < 128; ++x) bar += remote_frame()[(7 - 27 / 8) + 8 * x] >> (27 % 8) & 1;
  CHECK(bar >= 62 && bar <= 66);
  from_rytm(0x32, 2, 64 - 1, 0), from_rytm(0x32, 3, 64 + 3, 0);   // C left (a newer take), D +3
  CHECK(remote_play_take(&in) && in.take == -1 && in.level == 3);
  // C turned quickly, each turn's first frame on any ms (even or odd): the playback view throughout, never a frame of
  // the recorder's in between (hardware: "glitchy" while picking a take); 3 s after the last turn, back once
  pi.state = kPlayStopped, remote_play(pi), tick(3100, false);
  CHECK(has_text("IDLE"));
  texts.clear();
  for (int k = 0; k < 24; ++k) {
    from_rytm(0x32, 2, 64 + 1, 0);
    CHECK(remote_play_take(&in) && in.take == 1);
    tick(9 + k % 7 * 5, false);   // the frame after a turn on every ms parity
  }
  CHECK(has_text("PLAY") && !has_text("IDLE"));
  tick(3100, false);
  CHECK(has_text("IDLE"));
  texts.clear();
  ri.state = kRecRecording, remote_rec(ri), tick(30, false);   // a take records: the recorder's view, the play keys idle
  CHECK(has_text("RECORDING") && !has_text("PLAYING"));
  key(0x71), from_rytm(0x32, 1, 64 + 1, 0);
  CHECK(!remote_play_take(&in));
  pi.state = kPlayNone, remote_play(pi);
  ri.state = kRecArmed, remote_rec(ri), tick(30, false);
  key(0x44);                       // YES while armed: record now
  CHECK(remote_rec_take() == kRecStart);
  ri.state = kRecRecording, ri.secs = 3725, remote_rec(ri), tick(30, false);
  CHECK(has_text("RECORDING") && has_text("YES: STOP"));
  int big = 0;   // the time as large digits: 1:02:05 across rows 12..25
  for (int x = 19; x < 90; ++x)
    for (int y = 12; y < 26; ++y) big += remote_frame()[(7 - y / 8) + 8 * x] >> (y % 8) & 1;
  CHECK(big > 150);
  key(0x44);
  CHECK(remote_rec_take() == kRecStop);
  strcpy(ri.error, "no channel selected"), ri.state = kRecIdle, remote_rec(ri), tick(30, false);
  CHECK(has_text("ERR NO CHANNEL SELECTED"));
  // 1026: the trig keys pick the channels where the platform lets them (the Pi): caught, their LEDs (34) the selection
  ri.error[0] = 0;
  CHECK(mask_trigs == 0 && led_msgs == 0);   // the Mini (no ch_select): neither
  ri.ch_select = true, ri.in_mask = 1 << 3 | 1 << 11, remote_rec(ri), tick(30, false);   // SD and IN R
  CHECK(mask_trigs == 0xFFFF && led_mode == 1 && led_msgs == 1);
  CHECK(kLedA == 47 && kLedB == 52);   // rui1026's palette: blue recorded, orange not
  CHECK(led_c[0] == kLedB && led_c[1] == kLedA && led_c[8] == kLedB && led_c[9] == kLedB && led_c[10] == kLedA &&
        led_c[11] == kLedOff && led_c[15] == kLedOff);
  key(0x02), key(0x08), key(0x0C), tick(30, false);   // T3 (RS-CP), T9 (MAIN), T13 (nothing)
  CHECK(remote_rec_toggles() == (1u << 2 | 1u << 8) && remote_rec_toggles() == 0 && remote_stats().page == 3);
  ri.in_mask |= 1 << 4 | 3, remote_rec(ri), tick(30, false);   // the platform toggled them: the LEDs follow
  CHECK(led_msgs == 2 && led_c[2] == kLedA && led_c[8] == kLedA);
  ri.state = kRecRecording, remote_rec(ri), tick(30, false);
  key(0x00), tick(30, false);   // while a take records: ignored, said
  CHECK(remote_rec_toggles() == 0 && has_text("STOP FIRST (CHANNELS)"));
  ri.state = kRecIdle, remote_rec(ri), tick(30, false);
  key(0x41), tick(30, false);   // RIGHT: off the REC page - stock LEDs (34 mode 0), the trig keys free
  CHECK(led_mode == 0 && mask_trigs == 0);
  key(0x40), tick(30, false);   // back: ours again
  CHECK(led_mode == 1 && mask_trigs == 0xFFFF);
  ri.ch_select = false, remote_rec(ri), tick(30, false);
  CHECK(led_mode == 0);
  key(0x45), tick(30, false);   // NO: still back to SIDECAR (not the page's)
  CHECK(remote_stats().page == 0 && remote_rec_take() == kRecNoRequest);
  ri.state = kRecNone, remote_rec(ri);   // no recorder: the page is gone
  key(0x40), tick(30, false);
  CHECK(remote_stats().page == 3 && remote_stats().voice == 2);   // SCOPE: no REC (nor AUTO / SAMPLE) now
  key(0x40), tick(30, false);
  CHECK(remote_stats().page == 1);       // then the last voice page (ux1027: the LFO page is off the ring)
  from_rytm(0x30, 0, 0, 3);   // the page closed: the tap off
  CHECK(!tap_on());
  printf("ALL UI LIVE TESTS PASS\n");
  return 0;
}
