// core/ui/page_rec.cpp - see extra.h: the REC page (the recorder: the Pi's pi/engine/recorder.h, the Mini's
// core/rec/mini_rec.h; and on the Pi the playback of its takes, pi/engine/rec_play.h) and the state the platform hands
// it. Shown only where a platform reports a recorder (remote_rec): the Pi, and the Teensy host build (Sidecar Mini:
// teensy/voiceloop/rec_mini.h). Playback's keys (B / C / D) only where a platform reports a player (remote_play: the
// Pi); elsewhere the page takes encoder A only, as before. Cold.
//
// Keys (the page catches them; NO stays "back to SIDECAR", LEFT / RIGHT / UP / DOWN / PAGE navigate as everywhere, the
// Rytm's RECORD / PLAY / STOP stay its own):
//   T1..T11      (1026, where the platform lets them: the Pi) toggle a channel of the take: T1..T8 the tracks, T9 MAIN,
//                T10 / T11 IN L / R (FX L / R with the FX return on); not while a take records ("STOP FIRST"). Their
//                LEDs (34, rui1026) show the selection: blue recorded, orange not, T12..T16 off.
//   YES          record now (idle or armed), stop (recording)
//   A turned     right: arm (start on the Rytm's next PLAY, stop on STOP), left: disarm;  A pushed: toggles arming
//   B pushed     play / stop the selected take's channel (into the Rytm's MAIN; not while a take records)
//   B turned     the channel of the take to play (MASTER first when recorded, then the others in their file order)
//   C turned     the take (newest first; the last take by default)
//   D turned     the playback level
// Drawn, recording: a record mark (filled while recording, a ring while armed, a small ring when idle), the take's
// elapsed time as large segment digits, the take's name, the card's free space ("USB OFF" instead while the Mini's
// USB audio is paused for an armed or running take) and the time it holds at the
// selection's rate, the twelve meters (the input tap) with a dot over each channel the take records, and what the keys
// do now in the bottom bar (or the recorder's last error). Playing (or 3 s after a B / C / D turn): a play mark, the
// playhead's time, the take and the channel (its number of the take's), a progress bar with the playhead, the level.
#include <string.h>

#include "ui/extra.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

// The platform writes (the Pi's core 0), the page reads (core 3): a sequence count around the copy.
RecInfo r_info = {};
volatile uint32_t r_seq = 0;
volatile int r_req = 0;   // a request for the platform: kRec*
Meters r_meters;
uint32_t r_last_ms = 0;
// playback: the platform's state (the same sequence count), the page's input summed until the platform takes it
PlayInfo p_info = {};
volatile uint32_t p_seq = 0;
volatile int p_toggle = 0, p_dch = 0, p_dtake = 0, p_dlev = 0;
volatile bool p_have = false;      // a platform reports a player (remote_play called): the Pi
volatile bool p_touched = false;   // a B / C / D turn: the playback view shows for kPlayViewMs
uint32_t p_touch_ms = 0;
volatile uint16_t r_toggles = 0;   // 1026: trig keys pressed (bit k: T(k+1)), taken by the platform
uint32_t r_stop_first_ms = 0;      // a trig key while a take records: "STOP FIRST" in the bottom bar a while
bool r_stop_first = false;
constexpr int kChKeys = 11;        // T1..T11
bool p_touch_seen = false;           // p_touch_ms holds a turn's time (never in the future: a view that flips back)
constexpr uint32_t kPlayViewMs = 3000;

CORE_COLD bool play_get(PlayInfo* out) {
  for (int k = 0; k < 4; ++k) {
    uint32_t a = __atomic_load_n(&p_seq, __ATOMIC_ACQUIRE);
    if (a & 1) continue;
    memcpy(out, (const void*)&p_info, sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&p_seq, __ATOMIC_RELAXED) == a) return true;
  }
  return false;
}

CORE_COLD bool rec_get(RecInfo* out) {
  for (int k = 0; k < 4; ++k) {
    uint32_t a = __atomic_load_n(&r_seq, __ATOMIC_ACQUIRE);
    if (a & 1) continue;
    memcpy(out, (const void*)&r_info, sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&r_seq, __ATOMIC_RELAXED) == a) return true;
  }
  return false;
}

// Large digits: 8 x 14, segments 2 px thick (a top, b top right, c bottom right, d bottom, e bottom left, f top left,
// g middle). Original drawing for this page.
UI_ROM const uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
CORE_COLD void digit(Canvas& c, int x, int y, int d) {
  uint8_t m = kSeg[d % 10];
  if (m & 0x01) c.fill(Rect{(int16_t)(x + 1), (int16_t)y, 6, 2});
  if (m & 0x02) c.fill(Rect{(int16_t)(x + 6), (int16_t)(y + 1), 2, 6});
  if (m & 0x04) c.fill(Rect{(int16_t)(x + 6), (int16_t)(y + 7), 2, 6});
  if (m & 0x08) c.fill(Rect{(int16_t)(x + 1), (int16_t)(y + 12), 6, 2});
  if (m & 0x10) c.fill(Rect{(int16_t)x, (int16_t)(y + 7), 2, 6});
  if (m & 0x20) c.fill(Rect{(int16_t)x, (int16_t)(y + 1), 2, 6});
  if (m & 0x40) c.fill(Rect{(int16_t)(x + 1), (int16_t)(y + 6), 6, 2});
}
CORE_COLD int colon(Canvas& c, int x, int y) {
  c.fill(Rect{(int16_t)x, (int16_t)(y + 3), 2, 2});
  c.fill(Rect{(int16_t)x, (int16_t)(y + 9), 2, 2});
  return x + 4;
}
// "M:SS" / "MM:SS" / "H:MM:SS" from x; returns the x after it
CORE_COLD int clock(Canvas& c, int x, int y, uint32_t secs) {
  uint32_t h = secs / 3600, m = secs / 60 % 60, s = secs % 60;
  if (h) {
    digit(c, x, y, (int)(h % 10)), x += 10;
    x = colon(c, x, y);
  }
  if (h || m >= 10) digit(c, x, y, (int)(m / 10)), x += 10;
  digit(c, x, y, (int)(m % 10)), x += 10;
  x = colon(c, x, y);
  digit(c, x, y, (int)(s / 10)), x += 10;
  digit(c, x, y, (int)(s % 10)), x += 10;
  return x;
}

CORE_COLD void disc(Canvas& c, int cx, int cy, int r, bool fill) {
  for (int y = -r; y <= r; ++y)
    for (int x = -r; x <= r; ++x) {
      int d = x * x + y * y;
      if (fill ? d <= r * r + r : (d <= r * r + r && d >= (r - 1) * (r - 1) + (r - 1) - 1)) c.pixel(cx + x, cy + y, kOn);
    }
}

CORE_COLD void play_mark(Canvas& c, int x, int y, bool fill) {   // a triangle pointing right, rows y..y+12
  for (int r = 0; r <= 12; ++r) {
    int len = (r <= 6 ? r : 12 - r) * 3 / 2;
    if (fill) c.fill(Rect{(int16_t)x, (int16_t)(y + r), (int16_t)(len + 1), 1});
    else c.pixel(x, y + r, kOn), c.pixel(x + len, y + r, kOn);
  }
}

CORE_COLD void size_text(char* b, int n, const char* pre, uint32_t mb) {
  if (mb >= 10240) sniprintf(b, n, CORE_STR("%s%lu GB"), pre, (unsigned long)(mb / 1024));
  else if (mb >= 1024) sniprintf(b, n, CORE_STR("%s%lu.%lu GB"), pre, (unsigned long)(mb / 1024), (unsigned long)(mb % 1024 * 10 / 1024));
  else sniprintf(b, n, CORE_STR("%s%lu MB"), pre, (unsigned long)mb);
}

constexpr int kMeterY = 31, kMeterH = 11;

CORE_COLD bool rec_shown() {
  RecInfo i;
  return rec_get(&i) && i.state != kRecNone;
}

CORE_COLD void rec_draw(Canvas& c, int page, int npages, uint32_t ms) {
  RecInfo i;
  if (!rec_get(&i)) i = RecInfo{}, i.state = kRecIdle;
  PlayInfo p;
  if (!play_get(&p)) p = PlayInfo{};
  if (p_touched) p_touched = false, p_touch_ms = ms, p_touch_seen = true;
  bool rec = i.state == kRecRecording, armed = i.state == kRecArmed, busy = rec || i.state == kRecFinishing;
  bool playing = p.state == kPlayPlaying || p.state == kPlayStopping;
  // the playback view: while playing, or a while after a B / C / D turn (never while a take records)
  bool recent = p_touch_seen && ms - p_touch_ms < kPlayViewMs;
  bool p_shown = !busy && p.state != kPlayNone && (playing || recent);
  static const char* const kState[] = {"", "IDLE", "ARMED", "RECORDING", "SAVING"};
  c.clear();
  header(c, "REC", page, npages, p_shown ? (playing ? "PLAYING" : "PLAY") : kState[i.state < 5 ? i.state : 0]);
  char b[kTextLen + 1];
  if (p_shown) {   // the play mark and the playhead's time (rows 12..25), the take and its channel, the progress
    play_mark(c, 3, 12, playing);
    clock(c, 19, 12, p.pos_ms / 1000);
    sniprintf(b, sizeof b, CORE_STR("%.11s"), p.take);
    c.put_text(kSlotFree, kRSmall, 126, 12, b, kAlignRight);
    sniprintf(b, sizeof b, CORE_STR("%.6s %d/%d"), p.channels ? p.channel : "NO WAV", p.ch_i + 1, p.channels);
    c.put_text(kSlotFree + 1, kRSmall, 126, 20, b, kAlignRight);
    int w = p.len_ms ? (int)((uint64_t)(p.pos_ms > p.len_ms ? p.len_ms : p.pos_ms) * 127 / p.len_ms) : 0;
    for (int x = 0; x < 128; x += 2) c.pixel(x, 28, kOn);   // the track: dotted
    if (w) c.fill(Rect{0, 27, (int16_t)w, 2});
    c.fill(Rect{(int16_t)w, 25, 1, 5});                       // the playhead
  } else {
    // the mark and the time (rows 12..25)
    if (rec) disc(c, 8, 18, 6, true);
    else if (armed) disc(c, 8, 18, 6, false), disc(c, 8, 18, 3, false);
    else disc(c, 8, 18, 3, false);
    clock(c, 19, 12, i.secs);
    sniprintf(b, sizeof b, CORE_STR("%.11s"), i.take[0] ? i.take : "NO TAKE");
    c.put_text(kSlotFree, kRSmall, 126, 12, b, kAlignRight);
    if (i.usb_paused && (busy || armed)) {   // the Mini: USB audio paused while the recorder holds its memory
      c.put_text(kSlotFree + 1, kRSmall, 126, 20, "USB OFF", kAlignRight);
    } else if (i.total_mb) {
      size_text(b, sizeof b, "", i.free_mb);
      c.put_text(kSlotFree + 1, kRSmall, 126, 20, b, kAlignRight);
    }
  }
  // the meters (MAIN under T9, between the tracks and IN), a dot over each channel the take records
  r_meters.main_right = i.ch_select;
  if (r_stop_first) r_stop_first = false, r_stop_first_ms = ms | 1;
  if (ms - r_last_ms >= 66 || !r_last_ms) {
    uint16_t pk[kTapCh];
    tap_take_peaks(pk);
    r_meters.update(pk, ms);
    r_last_ms = ms;
  }
  r_meters.draw(c, kMeterY, kMeterH, kSlotFree + 8);
  for (int m = 0; m < kTapCh; ++m) {
    int ch = m;   // meter m shows IN channel m (page_scope.cpp meter_ch)
    if (i.in_mask >> ch & 1) {
      c.fill(Rect{(int16_t)(r_meters.pos(m) + 1), (int16_t)(kMeterY - 3), 3, 2});
    }
  }
  // the bottom bar: what the keys do now, the error, the time the card holds
  if (p_shown) {
    uint32_t len = p.len_ms / 1000;
    if (p.error[0]) sniprintf(b, sizeof b, CORE_STR("ERR %.24s"), p.error);
    else if (playing) sniprintf(b, sizeof b, CORE_STR("B: STOP  D: LEV %d  %lu:%02lu"), p.level, (unsigned long)(len / 60), (unsigned long)(len % 60));
    else sniprintf(b, sizeof b, CORE_STR("B: PLAY  C: TAKE  D: %d"), p.level);
  } else if (i.error[0] || (recent && p.error[0])) {   // the recorder's, or playback's (no take to play)
    sniprintf(b, sizeof b, CORE_STR("ERR %.24s"), i.error[0] ? i.error : p.error);
  } else if (r_stop_first_ms && ms - r_stop_first_ms < 1500) {
    strcpy(b, "STOP FIRST (CHANNELS)");
  } else if (rec) {
    strcpy(b, "YES: STOP");
  } else if (armed) {
    strcpy(b, "PLAY:REC YES:NOW A:OFF");
  } else if (i.state == kRecFinishing) {
    strcpy(b, "SAVING...");
  } else {
    uint32_t left = i.need_kbps ? (uint32_t)((uint64_t)i.free_mb * 1024 / i.need_kbps) : 0;
    if (p.state != kPlayNone) strcpy(b, "YES: REC  A: ARM  B: PLAY");   // the card's time: top right
    else if (left >= 3600) sniprintf(b, sizeof b, CORE_STR("YES: REC  A: ARM  %luH%02luM"), (unsigned long)(left / 3600), (unsigned long)(left / 60 % 60));
    else if (left) sniprintf(b, sizeof b, CORE_STR("YES: REC  A: ARM  %luM"), (unsigned long)(left / 60));
    else strcpy(b, "YES: REC  A: ARM  NO: BACK");
  }
  footer(c, b);
}

CORE_COLD void request(int r) { __atomic_store_n(&r_req, r, __ATOMIC_RELEASE); }

CORE_COLD bool rec_busy() {   // a take records (or closes): no playback, its keys do nothing
  RecInfo i;
  return rec_get(&i) && (i.state == kRecRecording || i.state == kRecFinishing);
}

CORE_COLD void rec_enc(int e, int delta, bool) {
  if (!delta) return;
  if (e >= 1 && e <= 3) {   // B the channel, C the take, D the level
    if (!p_have || rec_busy()) return;
    __atomic_fetch_add(e == 1 ? &p_dch : e == 2 ? &p_dtake : &p_dlev, delta, __ATOMIC_ACQ_REL);
    p_touched = true;
    return;
  }
  if (e != 0) return;
  RecInfo i;
  if (!rec_get(&i)) return;
  if (delta > 0 && i.state == kRecIdle) request(kRecArm);
  else if (delta < 0 && i.state == kRecArmed) request(kRecDisarm);
}

CORE_COLD bool rec_key(int code) {
  RecInfo i;
  if (!rec_get(&i)) return false;
  if (code == 0x44) {   // YES
    request(i.state == kRecRecording ? kRecStop : kRecStart);
    return true;
  }
  if (code == 0x71) {   // B pushed: play / stop
    if (!p_have) return false;
    if (!rec_busy()) __atomic_store_n(&p_toggle, 1, __ATOMIC_RELEASE), p_touched = true;
    return true;
  }
  if (code == 0x70) {   // A pushed
    if (i.state == kRecIdle) request(kRecArm);
    else if (i.state == kRecArmed) request(kRecDisarm);
    return true;
  }
  if (code >= 0 && code < 16 && i.ch_select) {   // T1..T16: T1..T11 a channel, the rest nothing (caught all the same)
    if (code >= kChKeys) return true;
    if (i.state == kRecRecording || i.state == kRecFinishing) r_stop_first = true;
    else __atomic_fetch_or(&r_toggles, (uint16_t)(1u << code), __ATOMIC_ACQ_REL);
    return true;
  }
  return false;
}

CORE_COLD uint16_t rec_trigs() {
  RecInfo i;
  return rec_get(&i) && i.ch_select ? 0xFFFF : 0;
}

// The LEDs: T1..T8 the tracks' channels (the tap's 2..9), T9 MAIN (0 / 1), T10 / T11 IN L / R; A recorded, B not
CORE_COLD bool rec_leds(uint8_t c[16]) {
  RecInfo i;
  if (!rec_get(&i) || !i.ch_select) return false;
  for (int k = 0; k < 16; ++k) {
    int ch = k < 8 ? 2 + k : k == 8 ? 0 : k < kChKeys ? k + 1 : -1;
    c[k] = ch < 0 ? kLedOff : (i.in_mask >> ch & 1) ? kLedA : kLedB;
  }
  return true;
}

CORE_COLD uint8_t rec_encs() { return p_have ? 0x0F : 0x01; }   // A; B C D with a player

}  // namespace

UI_ROM const ExtraPage kRecPage = {"REC", rec_shown, rec_draw, rec_enc, rec_key, 0x01, true, rec_encs, rec_trigs, rec_leds};

CORE_COLD void remote_rec(const RecInfo& i) {
  __atomic_fetch_add(&r_seq, 1u, __ATOMIC_ACQ_REL);
  memcpy((void*)&r_info, &i, sizeof i);
  r_info.take[sizeof r_info.take - 1] = r_info.error[sizeof r_info.error - 1] = 0;
  __atomic_fetch_add(&r_seq, 1u, __ATOMIC_RELEASE);
}

CORE_COLD int remote_rec_take() { return __atomic_exchange_n(&r_req, 0, __ATOMIC_ACQ_REL); }
CORE_COLD uint16_t remote_rec_toggles() { return __atomic_exchange_n(&r_toggles, (uint16_t)0, __ATOMIC_ACQ_REL); }

CORE_COLD void remote_play(const PlayInfo& i) {
  p_have = true;
  __atomic_fetch_add(&p_seq, 1u, __ATOMIC_ACQ_REL);
  memcpy((void*)&p_info, &i, sizeof i);
  p_info.take[sizeof p_info.take - 1] = p_info.channel[sizeof p_info.channel - 1] = p_info.error[sizeof p_info.error - 1] = 0;
  __atomic_fetch_add(&p_seq, 1u, __ATOMIC_RELEASE);
}

CORE_COLD bool remote_play_take(PlayInput* in) {
  in->toggle = __atomic_exchange_n(&p_toggle, 0, __ATOMIC_ACQ_REL);
  in->channel = __atomic_exchange_n(&p_dch, 0, __ATOMIC_ACQ_REL);
  in->take = __atomic_exchange_n(&p_dtake, 0, __ATOMIC_ACQ_REL);
  in->level = __atomic_exchange_n(&p_dlev, 0, __ATOMIC_ACQ_REL);
  return in->toggle || in->channel || in->take || in->level;
}

}  // namespace ui
}  // namespace core
