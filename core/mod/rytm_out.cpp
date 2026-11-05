// core/mod/rytm_out.cpp - see rytm_out.h. Control side only, cold.
#include "mod/rytm_out.h"

#include "hooks.h"
#include "platform.h"
#include "song/song.h"

namespace core {
namespace mod {

namespace {

// slot 0 the scene, 1..12 the macros: the value last sent (-1 none since the reset), the one waiting (-1 none), when
struct Slot {
  int16_t sent = -1, want = -1;
  uint32_t at = 0;
};
Slot g_s[13];
bool g_cmd = false;        // the mod command (the hello's caps), not refused since
bool g_said = false;       // the refusal logged
bool g_said_off = false;   // a track without a channel logged
RytmOutStats g_st;

CORE_COLD void sysex(const uint8_t* b, int n) {   // F0 7D 52 54 <kHostCmd> 01 b... F7
  uint8_t m[16] = {0xF0, 0x7D, 0x52, 0x54, kHostCmd, 1};
  int k = 6;
  for (int i = 0; i < n && k < 15; ++i) m[k++] = (uint8_t)(b[i] & 0x7F);
  m[k++] = 0xF7;
  to_rytm(m, k);
  ++g_st.cmds;
}
CORE_COLD void cc(int ch, int c, int v) {   // ch 1..16
  const uint8_t m[3] = {(uint8_t)(0xB0 | ((ch - 1) & 15)), (uint8_t)(c & 0x7F), (uint8_t)(v & 0x7F)};
  to_rytm(m, 3);
  ++g_st.ccs;
}
CORE_COLD int ch_or(int ch, int def) { return ch >= 1 && ch <= 16 ? ch : def; }
CORE_COLD void send(int slot, int v) {
  const song::MidiConfig& c = song::midi_config();
  if (g_cmd) {
    if (slot == 0) {
      const uint8_t b[2] = {kOpScene, (uint8_t)v};
      sysex(b, 2);
    } else {
      const uint8_t b[3] = {kOpPerf, (uint8_t)slot, (uint8_t)v};
      sysex(b, 3);
    }
  } else if (slot == 0) {
    cc(ch_or(c.auto_ch, 14), kSceneCc, v);
  } else {
    cc(ch_or(c.perf_ch, 15), kPerfCc[slot - 1], v);
  }
}
CORE_COLD void offer(int slot, int v, uint32_t ms) {
  Slot& s = g_s[slot];
  if (v == (s.want >= 0 ? s.want : s.sent)) return;   // on change only
  if (s.want >= 0) {
    ++g_st.held;
    if (v == s.sent) {   // back to what the Rytm has: nothing to send
      s.want = -1;
      return;
    }
  }
  if (s.sent < 0 || ms - s.at >= kSpacingMs) {
    s.sent = (int16_t)v, s.want = -1, s.at = ms;
    send(slot, v);
  } else {
    s.want = (int16_t)v;
  }
}

}  // namespace

CORE_COLD void rytm_scene(int n) { offer(0, n < 0 ? 0 : n > 12 ? 12 : n, now_ms()); }
CORE_COLD void rytm_perf(int macro, int v) {
  if (macro < 1 || macro > 12) return;
  offer(macro, v < 0 ? 0 : v > 127 ? 127 : v, now_ms());
}
CORE_COLD void rytm_mute(uint16_t set, uint16_t clr) {
  set &= 0xFFF, clr &= 0xFFF & ~set;
  if (!set && !clr) return;
  if (g_cmd) {
    const uint8_t b[5] = {kOpMute, (uint8_t)(set & 0x7F), (uint8_t)(set >> 7), (uint8_t)(clr & 0x7F), (uint8_t)(clr >> 7)};
    sysex(b, 5);
    return;
  }
  const song::MidiConfig& c = song::midi_config();
  for (int k = 0; k < 12; ++k) {
    if (!((set | clr) >> k & 1)) continue;
    if (c.track_ch[k] < 1 || c.track_ch[k] > 16) {   // the track's channel OFF: no CC 94 on another track's (review L4)
      if (!g_said_off) log(CORE_STR("rytm: track %d has no MIDI channel: its mute not sent by CC"), k + 1), g_said_off = true;
      continue;
    }
    cc(c.track_ch[k], kMuteCc, set >> k & 1);
  }
}
CORE_COLD void rytm_host_cmd(bool on) {
  g_cmd = on, g_said = false;
  for (Slot& s : g_s) s.sent = s.want = -1;   // (a Rytm restart: its scene and macros are its own again)
}
CORE_COLD void rytm_out_forget(int slot) {
  if (slot >= 0 && slot < 13) g_s[slot].sent = g_s[slot].want = -1;
}
CORE_COLD void rytm_ack(int op, int status) {
  if (!status) return;
  ++g_st.refused;
  if (g_cmd && !g_said)
    log(CORE_STR("rytm: the mod refused op %d (status %d): CCs from now on (until its next hello)"), op, status), g_said = true;
  g_cmd = false;
}
CORE_COLD void rytm_out_tick(uint32_t ms) {
  for (int i = 0; i < 13; ++i) {
    Slot& s = g_s[i];
    if (s.want < 0 || ms - s.at < kSpacingMs) continue;
    s.sent = s.want, s.want = -1, s.at = ms;
    send(i, s.sent);
  }
}
CORE_COLD void rytm_out_reset() {
  for (Slot& s : g_s) s = Slot();
  g_cmd = g_said = false;
  g_st = RytmOutStats();
}
CORE_COLD const RytmOutStats& rytm_out_stats() { return g_st; }

}  // namespace mod
}  // namespace core
