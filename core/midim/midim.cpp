// core/midim/midim.cpp - see midim.h. Per message: cold.
#include "midim.h"

#include <stdio.h>
#include <string.h>

#include "hooks.h"
#include "machines.h"
#include "platform.h"
#include "poly.h"

namespace core {
namespace midim {
namespace {

constexpr int kBank = 0, kSub = 1, kProg = 2, kBend = 3, kTouch = 4, kWheel = 5, kCc0 = 6, kSlots = kCc0 + kCcs;

struct Held {
  bool used;
  uint8_t dst, ch, note, vi, src, mnote;
  uint32_t age;
  uint32_t ms;             // when it went out
};
struct Vo {
  int8_t dst, ch;          // what the voice's words say (-1 no report yet)
  int16_t want[kSlots];    // -1 OFF
  int16_t sent[kSlots];    // -1 nothing sent (or forgotten)
  int16_t sent_cc[kCcs];   // the CC number sent[] was sent on
  bool dirty;              // differs: tick's to send
};
struct RetryOff {
  bool used;
  uint8_t dst, ch, note;
  uint32_t ms;
};
struct Sent {            // the last note-ons sent, for echo()
  bool used;
  uint8_t dst, ch, note, vel;
  uint32_t ms;
};
constexpr int kSentLog = 32;
Held held[kMaxHeld];
RetryOff retry[kMaxHeld];
Sent sent_log[kSentLog];
int sent_w = 0;
// DIN's note-offs that a lost record may have taken (midim.h, "LOST ON THE WAY"): two generations, a set of (channel, note); owed: to go again
uint32_t din_off[2][16][4], din_owed[16][4];
bool din_any = false, din_down = false;
uint32_t din_roll_ms = 0;
Vo vo[kVoices];
Stats st;
Out out_fn = nullptr;
int32_t tokens[kDests];
uint32_t last_ms = 0, age_n = 0;

CORE_COLD int budget(int d) { return d == kDin ? kDinBytesPerSec : kUsbBytesPerSec; }

// One channel message to a destination; n 2 or 3
CORE_COLD bool emit(int d, const uint8_t* m, int n) {
  if (d == kDin) {
    uint8_t x[11] = {0xF0, 0x7D, 0x52, 0x54, kDinCmd, 0x01, (uint8_t)(m[0] & 0x7F), m[1]};
    int k = 8;
    if (n == 3) x[k++] = m[2];
    x[k++] = 0xF7;
    to_rytm(x, k);
    ++st.din;
    return true;
  }
  if (out_fn && out_fn(d, m, n)) return true;
  ++st.lost;
  return false;
}
CORE_COLD void value_msg(int d, int b0, int d1, int d2) {
  uint8_t m[3] = {(uint8_t)b0, (uint8_t)(d1 & 0x7F), (uint8_t)(d2 & 0x7F)};
  emit(d, m, 3);
  tokens[d] -= 3;
  ++st.values;
}

CORE_COLD void off_entry(Held& h) {   // the entry goes; its note's off only when no other holder of it is left
  h.used = false;
  --st.held;
  for (const Held& o : held)
    if (o.used && o.dst == h.dst && o.ch == h.ch && o.note == h.note) return;
  uint8_t m[3] = {(uint8_t)(0x80 | h.ch), h.note, 0};
  ++st.offs;
  if (h.dst == kDin) din_off[0][h.ch][h.note >> 5] |= 1u << (h.note & 31);
  if (emit(h.dst, m, 3)) return;
  for (RetryOff& r : retry)     // refused (a full queue): tick tries again
    if (!r.used) {
      r = {true, h.dst, h.ch, h.note, now_ms()};
      return;
    }
}

CORE_COLD void on_entry(int d, int ch, int note, int vel, int vi, int src, int mnote) {
  if (d == kOff || note < 0 || note > 127) return;
  Held* free_e = nullptr;
  Held* oldest = &held[0];
  for (Held& h : held) {
    if (!h.used && !free_e) free_e = &h;
    if (h.used && (int32_t)(h.age - oldest->age) < 0) oldest = &h;
  }
  if (!free_e) {   // the table full: the oldest released, so nothing is sent that could not be ended
    off_entry(*oldest);
    free_e = oldest;
    ++st.steals;
  }
  uint8_t m[3] = {(uint8_t)(0x90 | ch), (uint8_t)note, (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel)};
  emit(d, m, 3);
  sent_log[sent_w] = {true, (uint8_t)d, (uint8_t)ch, (uint8_t)note, m[2], now_ms()};
  sent_w = (sent_w + 1) % kSentLog;
  *free_e = {true, (uint8_t)d, (uint8_t)ch, (uint8_t)note, (uint8_t)vi, (uint8_t)src, (uint8_t)mnote, ++age_n, now_ms()};
  ++st.ons;
  if (++st.held > st.peak) st.peak = st.held;
}

// What the words ask for (midim.h)
CORE_COLD void read_words(Vo& x, const float* p) {
  int d = eng_zone(p[0], kDests), ch = eng_zone(p[1], 16);
  if (d != x.dst || ch != x.ch) {   // another destination has heard none of it
    for (int k = 0; k < kSlots; ++k) x.sent[k] = -1;
  }
  x.dst = (int8_t)d, x.ch = (int8_t)ch;
  x.want[kBank] = p[2] >= 1 ? (int16_t)p[2] - 1 : -1;
  x.want[kSub] = p[3] >= 1 ? (int16_t)p[3] - 1 : -1;
  x.want[kProg] = p[4] >= 1 ? (int16_t)p[4] - 1 : -1;
  if (p[5] >= 1) {
    int b = 8192 + (int)((p[5] - 64) * 8192.0f / 63.0f);
    x.want[kBend] = (int16_t)(b < 0 ? 0 : b > 16383 ? 16383 : b);
  } else {
    x.want[kBend] = -1;
  }
  x.want[kTouch] = p[6] >= 1 ? (int16_t)(((int)p[6] - 1) * 127 / 126) : -1;
  x.want[kWheel] = p[7] >= 1 ? (int16_t)(((int)p[7] - 1) * 127 / 126) : -1;
  for (int k = 0; k < kCcs; ++k) {
    int c = (int)p[12 + k];
    x.want[kCc0 + k] = c >= 1 ? (int16_t)p[8 + k] : -1;
    if (c >= 1 && c - 1 != x.sent_cc[k]) x.sent[kCc0 + k] = -1;   // another CC: its value to send
  }
}

// What differs from what was sent, in the trig's order. force: all of it (before a note); else while the budget lasts.
CORE_COLD void flush(Vo& x, const float* p, bool force) {
  int d = x.dst;
  x.dirty = false;
  if (d <= kOff) return;
  int ch = x.ch;
  if (x.want[kBank] != x.sent[kBank] || x.want[kSub] != x.sent[kSub] || x.want[kProg] != x.sent[kProg]) {
    if (x.want[kBank] >= 0 || x.want[kSub] >= 0 || x.want[kProg] >= 0) {
      if (!force && tokens[d] <= 0) { x.dirty = true; return; }
      if (x.want[kBank] >= 0) value_msg(d, 0xB0 | ch, 0, x.want[kBank]);
      if (x.want[kSub] >= 0) value_msg(d, 0xB0 | ch, 32, x.want[kSub]);
      if (x.want[kProg] >= 0) {
        uint8_t m[2] = {(uint8_t)(0xC0 | ch), (uint8_t)x.want[kProg]};
        emit(d, m, 2);
        tokens[d] -= 2;
        ++st.values;
      }
    }
    x.sent[kBank] = x.want[kBank], x.sent[kSub] = x.want[kSub], x.sent[kProg] = x.want[kProg];
  }
  for (int k = kBend; k < kSlots; ++k) {
    int w = x.want[k];
    if (w == x.sent[k]) continue;
    if (w >= 0) {
      if (!force && tokens[d] <= 0) { x.dirty = true; return; }
      if (k == kBend) value_msg(d, 0xE0 | ch, w & 0x7F, w >> 7);
      else if (k == kTouch) {
        uint8_t m[2] = {(uint8_t)(0xD0 | ch), (uint8_t)w};
        emit(d, m, 2);
        tokens[d] -= 2;
        ++st.values;
      } else if (k == kWheel) value_msg(d, 0xB0 | ch, 1, w);
      else {
        int c = (int)p[12 + k - kCc0] - 1;
        value_msg(d, 0xB0 | ch, c, w);
        x.sent_cc[k - kCc0] = (int16_t)c;
      }
    }
    x.sent[k] = (int16_t)w;
  }
}

float words[kVoices][16];

CORE_COLD bool held_note(int d, int ch, int note) {
  for (const Held& h : held)
    if (h.used && h.dst == d && h.ch == ch && h.note == note) return true;
  return false;
}

// What is owed, under the byte budget: a note played again meanwhile has its own off to come
CORE_COLD void din_again() {
  din_any = false;
  for (int ch = 0; ch < 16; ++ch)
    for (int w = 0; w < 4; ++w)
      while (din_owed[ch][w]) {
        if (tokens[kDin] <= 0) {
          din_any = true;
          return;
        }
        int b = __builtin_ctz(din_owed[ch][w]), note = w * 32 + b;
        din_owed[ch][w] &= ~(1u << b);
        if (held_note(kDin, ch, note)) continue;
        uint8_t m[3] = {(uint8_t)(0x80 | ch), (uint8_t)note, 0};
        emit(kDin, m, 3);
        tokens[kDin] -= 3;
        ++st.din_again;
      }
}

CORE_COLD void forget(Vo& x) {
  x.dirty = false;
  for (int k = 0; k < kSlots; ++k) x.sent[k] = x.want[k] = -1;
  for (int k = 0; k < kCcs; ++k) x.sent_cc[k] = -1;
}

}  // namespace

CORE_COLD void set_out(Out f) { out_fn = f; }

CORE_COLD void report(int vi, int note, int vel, const float* p) {
  vi = voice_wrap(vi);
  Vo& x = vo[vi];
  memcpy(words[vi], p, sizeof words[vi]);
  read_words(x, p);
  if (note >= 0) note_off(vi);   // mono: the trig's note before it ends with it
  flush(x, p, note >= 0);
  if (note >= 0 && x.dst > kOff) on_entry(x.dst, x.ch, note, vel, vi, kTrigSrc, note);
}

CORE_COLD void note_off(int vi) {
  for (Held& h : held)
    if (h.used && h.vi == (voice_wrap(vi)) && h.src == kTrigSrc) off_entry(h);
}

CORE_COLD void live_on(int vi, int note, int vel, int ch, int mnote) {
  Vo& x = vo[voice_wrap(vi)];
  live_off(vi, ch, mnote);   // the same key again
  if (x.dst > kOff) {
    flush(x, words[voice_wrap(vi)], true);   // as a trig: a bank and program before the note (the arp's notes, the sequencer's, the keys')
    on_entry(x.dst, x.ch, note, vel, voice_wrap(vi), ch, mnote);
  }
}

CORE_COLD void live_off(int vi, int ch, int mnote) {
  for (Held& h : held)
    if (h.used && h.vi == (voice_wrap(vi)) && h.src == ch && (mnote < 0 || h.mnote == mnote)) off_entry(h);
}

CORE_COLD int voice_notes(int vi) {
  int n = 0;
  for (const Held& h : held) n += h.used && h.vi == (voice_wrap(vi));
  return n;
}

CORE_COLD void silence(int vi) {
  for (Held& h : held)
    if (h.used && h.vi == (voice_wrap(vi))) off_entry(h);
}

CORE_COLD void leave(int vi) {
  silence(vi);
  Vo& x = vo[voice_wrap(vi)];
  x.dst = x.ch = -1;
  forget(x);
}

// 3(c) step 9: slot >= 0 a box's own voices only (its kit, its link), -1 every voice
static inline bool of_slot(int vi, int slot) { return slot < 0 || slot_of_voice(vi) == slot; }
CORE_COLD void kit_switched(int slot) {
  uint32_t ms = now_ms();
  for (Held& h : held)
    if (h.used && of_slot(h.vi, slot) && !((h.src == kTrigSrc || h.src == poly::kSeqCh) && ms - h.ms <= kFreshHeldMs)) off_entry(h);   // (kSeqCh: the arp's note of that trig)
  for (int vi = 0; vi < kVoices; ++vi)
    if (of_slot(vi, slot)) forget(vo[vi]);
}

CORE_COLD void kit_changed(int slot) {
  for (int vi = 0; vi < kVoices; ++vi) {
    if (!of_slot(vi, slot)) continue;
    silence(vi);
    forget(vo[vi]);
  }
}

CORE_COLD void link_lost() { kit_changed(); }

CORE_COLD void link_state(bool up) { din_down = !up; }

CORE_COLD void resync() {
  for (int ch = 0; ch < 16; ++ch)
    for (int w = 0; w < 4; ++w) {
      din_off[0][ch][w] |= din_off[1][ch][w];   // this try may be lost too: kept for the next, kOffKeepMs more
      din_off[1][ch][w] = 0;
      din_owed[ch][w] |= din_off[0][ch][w];
      din_any = din_any || din_owed[ch][w];
    }
  din_roll_ms = now_ms();
}

CORE_COLD bool echo(int dest, int ch, int note, int vel) {
  uint32_t ms = now_ms();
  for (Sent& s : sent_log)
    if (s.used && s.dst == dest && s.ch == ch && s.note == note && s.vel == vel && ms - s.ms <= kEchoMs) {
      s.used = false;
      ++st.echoes;
      return true;
    }
  return false;
}

CORE_COLD void stop(int slot) {
  for (Held& h : held)
    if (h.used && (h.src == kTrigSrc || h.src == poly::kSeqCh) && of_slot(h.vi, slot)) off_entry(h);
}

// Each box's own clock (decided 2026-11-26): the USB-A devices are every box's, so a box's realtime bytes go there only
// while its voices alone send there. With both boxes' voices on USB-A no clock goes at all (a device follows one clock).
CORE_COLD int usba_slots() {
  int m = 0;
  for (int vi = 0; vi < kVoices; ++vi)
    if (vo[vi].dst == kUsbA) m |= 1 << slot_of_voice(vi);
  return m;
}
CORE_COLD void realtime(int slot, uint8_t status) {
  if (!out_fn || slot < 0 || slot >= kVoiceSlots) return;
  if (usba_slots() == 1 << slot) out_fn(kUsbA, &status, 1);
}
CORE_COLD void realtime(uint8_t status) { realtime(0, status); }

void tick(uint32_t ms) {
  uint32_t dt = ms - last_ms;
  if (!dt) return;
  last_ms = ms;
  if (dt > 100) dt = 100;
  for (int d = kDin; d < kDests; ++d) {
    int b = budget(d);
    tokens[d] += (int32_t)(dt * (uint32_t)b / 1000u);
    if (tokens[d] > b / 20) tokens[d] = b / 20;   // 50 ms of burst
  }
  if (din_any) din_again();
  if (din_down) din_roll_ms = ms;   // nothing is known to have arrived while the link is off
  else if (ms - din_roll_ms >= kOffKeepMs / 2) {
    din_roll_ms = ms;
    memcpy(din_off[1], din_off[0], sizeof din_off[1]);
    memset(din_off[0], 0, sizeof din_off[0]);
  }
  for (int vi = 0; vi < kVoices; ++vi)
    if (vo[vi].dirty) flush(vo[vi], words[vi], false);
  for (RetryOff& r : retry) {
    if (!r.used) continue;
    bool again = false;
    for (const Held& h : held) again = again || (h.used && h.dst == r.dst && h.ch == r.ch && h.note == r.note);
    uint8_t m[3] = {(uint8_t)(0x80 | r.ch), r.note, 0};
    if (again) r.used = false;   // played again meanwhile: its own off ends it
    else if (out_fn && out_fn(r.dst, m, 3)) r.used = false, ++st.retried;
    else if (ms - r.ms > kOffRetryMs) r.used = false;   // (counted lost by emit)
  }
}

CORE_COLD const Stats& stats() { return st; }
CORE_COLD int dest_of(int vi) { return vo[voice_wrap(vi)].dst > 0 ? vo[voice_wrap(vi)].dst : (int)kOff; }

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strcmp(p, "midim") != 0) return false;
  static const char* const kNames[kDests] = {"-", "DIN", "USBC", "USBA"};
  int n = sniprintf(out, cap, CORE_STR("midim: %d notes held (peak %d), %lu on, %lu off, %lu values, %lu to DIN, %lu lost, %lu steals; voices"),
                    st.held, st.peak, (unsigned long)st.ons, (unsigned long)st.offs, (unsigned long)st.values, (unsigned long)st.din,
                    (unsigned long)st.lost, (unsigned long)st.steals);
  for (int vi = 0; vi < kVoices && n < cap; ++vi)
    n += sniprintf(out + n, cap - n, " %s/%d", kNames[dest_of(vi)], vo[vi].ch >= 0 ? vo[vi].ch + 1 : 0);
  return true;
}

CORE_COLD void reset() {
  memset(held, 0, sizeof held);
  memset(&st, 0, sizeof st);
  memset(tokens, 0, sizeof tokens);
  memset(retry, 0, sizeof retry);
  memset(sent_log, 0, sizeof sent_log);
  memset(din_off, 0, sizeof din_off);
  memset(din_owed, 0, sizeof din_owed);
  din_any = din_down = false;
  din_roll_ms = 0;
  sent_w = 0;
  last_ms = 0, age_n = 0;
  for (Vo& x : vo) {
    x.dst = x.ch = -1;
    forget(x);
  }
}

}  // namespace midim
}  // namespace core
