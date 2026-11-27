// core/host_link.cpp - see host_link.h. Per SysEx message, never per sample: cold.
#include "md/md.h"
#include "mod/scene.h"
#include "device/device.h"
#include "host_link.h"
#include "inst.h"
#include "arp/arp.h"
#if CORE_MSAMP
#include "chop/chop.h"
#endif

#include <string.h>

#include "fmt/fmt.h"
#include "midim/midim.h"
#include "fx/send_fx.h"
#if CORE_POLY
#include "fx/master.h"
#include "fx/insert.h"
#endif
#if CORE_MSAMP
#include "bundle/kits.h"
#include "bundle/prjslot.h"
#endif
#include "pool/pool.h"
#include "rec/rec_names.h"
#include "ui/remote.h"
#include "poly.h"
#include "seq/seq.h"

namespace core {

#if CORE_MSAMP
// The freeze probe (0x4E, every ~5 s while the Rytm's UI task is stuck; its seconds capped at 127): the first probe of a
// freeze is logged once with the uptime, so the start survives the log ring; on the first freeze of a boot the platform
// copies the recent log to the card (hooks.log_snapshot: posted, written by the card's core, never here).
static uint32_t s_freeze_first_ms, s_freeze_last_ms;
static bool s_freeze_on, s_freeze_snapped;
static CORE_COLD void freeze_probe_seen(int secs) {
  uint32_t ms = now_ms();
  if (s_freeze_on && ms - s_freeze_last_ms > 15000) s_freeze_on = false;   // (no probe for 15 s: that freeze ended)
  s_freeze_last_ms = ms;
  if (s_freeze_on) return;
  s_freeze_on = true, s_freeze_first_ms = ms;
  uint32_t t = ms / 1000;
  log(CORE_STR("rui: FREEZE PROBE: the first of this freeze at uptime %02u:%02u:%02u (stuck %d s then)%s"), (unsigned)(t / 3600),
      (unsigned)(t / 60 % 60), (unsigned)(t % 60), secs, s_freeze_snapped || !hooks.log_snapshot ? "" : "; the log goes to the card");
  if (s_freeze_snapped || !hooks.log_snapshot) return;
  s_freeze_snapped = true;   // (once a boot)
  char name[32];
  sniprintf(name, sizeof name, "freeze-%u", (unsigned)t);
  hooks.log_snapshot(name);
}
#endif

// The send FX's TYPE list (0x21, 8 a message) and both FX states (0x22) to the Rytm.
static CORE_COLD void sendfx_names_out() {
  if (!hooks.to_rytm) return;
  uint8_t m[128];
  int total = fx::names_total();
  for (int first = 0; first < (total > 0 ? total : 1); first += 8) {
    int c = total - first < 8 ? total - first : 8;
    hooks.to_rytm(m, fx::sysex_names(m, first == 0, total, first, c));
  }
}
// Each playable TYPE's P2..P5 labels and zones (0x24), after the 0x21 list (whose replace clears the Rytm's labels).
// The Rytm keeps rytm_label_tables() of them (rytm-mods 0029 sfu.c NLT: 16 since rui1049, whose hello says so in its
// caps bit 1; 8 before - a 24 past them is dropped there and that TYPE's P2..P5 stay generic), and the Pi plays 9 with
// labels since SPACE: the TYPEs on FX 1 and FX 2 go first, then the others by id up to the cap. A slot moved to a TYPE
// left out sends the list and the labels again (0x20 below).
constexpr int kRytmLabelTables = 8, kRytmLabelTables1049 = 16;
static CORE_COLD int rytm_label_tables() { return ui::remote_caps() & 2 ? kRytmLabelTables1049 : kRytmLabelTables; }
// each box's own (the box in hand's: its Rytm, whichever slot it holds)
static uint64_t s_labels_sent_b[kVoiceSlots];   // bit id: its 0x24 went in the last burst (ids 1..63)
static uint8_t s_ulabels_sent_b[kVoiceSlots];   // bit u: user TYPE 64 + u's (Plugin API 3's user effects, from their manifests)
static CORE_COLD bool label_sent(int id, uint64_t lo, uint8_t hi) {
  return id < fx::kFirstUser ? (lo >> id & 1) : (hi >> (id - fx::kFirstUser) & 1);
}
static CORE_COLD bool label_id(int id) { return id >= 1 && id < fx::kFirstUser + fx::kUserSlots; }
static CORE_COLD void sendfx_labels_out() {
  if (!hooks.to_rytm) return;
  uint8_t m[160];
  uint64_t sent = 0;
  uint8_t usent = 0;
  int k = 0;
  auto one = [&](int id) {
    if (!label_id(id) || label_sent(id, sent, usent) || k >= rytm_label_tables()) return;
    if (int n = fx::sysex_labels(m, id)) {
      hooks.to_rytm(m, n), ++k;
      if (id < fx::kFirstUser) sent |= 1ull << id;
      else usent |= (uint8_t)(1u << (id - fx::kFirstUser));
    }
  };
  one(fx::cur().knob[0][0]);   // (the box in hand's: the Rytm this goes to, whichever slot it holds)
  one(fx::cur().knob[1][0]);
  for (int id = 1; id < fx::kFirstUser + fx::kUserSlots; ++id) one(id);
  s_labels_sent_b[fx::slot_in_hand()] = sent, s_ulabels_sent_b[fx::slot_in_hand()] = usent;
}
// a slot's TYPE has labels the Rytm was not sent (the cap above)
static CORE_COLD bool sendfx_labels_missing() {
  for (int s = 0; s < fx::kSlots; ++s) {
    int id = fx::cur().knob[s][0];
    const int b = fx::slot_in_hand();
    if (label_id(id) && fx::labels(id) && !label_sent(id, s_labels_sent_b[b], s_ulabels_sent_b[b])) return true;
  }
  return false;
}
// Each box's own (the box in hand's FX and its link: a Rytm in either slot, two Rytms each theirs)
static uint8_t fx_sent_id[kVoiceSlots][fx::kSlots], fx_sent_st[kVoiceSlots][fx::kSlots];
static bool fx_sent_any[kVoiceSlots];        // a state went to this box's Rytm (else the next goes in full)
static bool s_sendfx_hello_b[kVoiceSlots];   // names + labels + states sent since boot (asked or not)
static inline bool& sendfx_hello() { return s_sendfx_hello_b[fx::slot_in_hand()]; }

static CORE_COLD void sendfx_state_out(bool all) {
  const int b = fx::slot_in_hand();
  const fx::Control& c = fx::control_of(b);
  if (!fx_sent_any[b]) all = fx_sent_any[b] = true;
  for (int s = 0; s < fx::kSlots; ++s) {
    uint8_t id = c.knob[s][0], st = fx::state(c, s);
    if (!all && id == fx_sent_id[b][s] && st == fx_sent_st[b][s]) continue;
    fx_sent_id[b][s] = id, fx_sent_st[b][s] = st;
    uint8_t m[32];
    if (hooks.to_rytm) hooks.to_rytm(m, fx::sysex_state(m, s));
  }
}
#if CORE_POLY
// sfx-basewidth (0.5.0; rytm-mods 0029): SEND FX 1 / 2's knobs G and H are BASE and WIDTH of bus 1 / 2's POST EQ
// (ins::eq_bw: the send FX's return filter), kept on the Sidecar - the kit file's PEQ line - never in the Rytm's kit.
// 22 ver 2 <fx> <base> <width> both ways (sim/protocol.py sendfx_bw): the Rytm's is a turn of its knobs, taken and
// remembered as sent (no echo); ours is what its knobs show, sent when a bus's values differ from what the Rytm was last
// told (a kit load, the console, the web), at most every 20 ms, and all again on its ASK and hello. An older 0029 drops
// it (its ver check); without one the Rytm's knobs G / H are empty and nothing changes.
// Both are also sent again every kBwRefreshMs while the link is up: hooks.to_rytm cannot say a message was dropped,
// and a value of ours can cross a turn of the Rytm's in flight (it then shows ours while we keep its turn). The
// refresh puts its knobs back on what plays within 2 s either way; 20 bytes per 2 s.
static const uint32_t kBwRefreshMs = 2000;
// Each box's own (decided 2026-11-26: slot 0 and slot 1 the same): what its Rytm was told of its own buses (ins:: the slot
// in hand's), its 20 ms pace and 2 s refresh, whether it listens. The idle step runs in each slot's Scope (sendfx_idle).
struct Bw {
  int16_t sent[fx::kSlots][2] = {{-1, -1}, {-1, -1}};
  uint32_t ms = 0, all_ms = 0;
  bool up = false;   // the Rytm listens: set by its ASK, first 0x20, hello or a turn; cleared by link_lost() / slot_left()
};
static Bw s_bw_b[kVoiceSlots];
static inline Bw& BW() { return s_bw_b[fx::slot_in_hand()]; }
static CORE_COLD bool sendfx_bw_out() {
  Bw& w0 = BW();
  auto& s_bw_sent = w0.sent;
  if (!hooks.to_rytm || now_ms() - w0.ms < 20) return false;
  if (now_ms() - w0.all_ms >= kBwRefreshMs) {
    for (auto& x : s_bw_sent) x[0] = x[1] = -1;
    w0.all_ms = now_ms();
  }
  bool sent = false;
  for (int s = 0; s < fx::kSlots; ++s) {
    int b, w;
    ins::eq_bw(ins::kBus0 + s, &b, &w);
    if (b == s_bw_sent[s][0] && w == s_bw_sent[s][1]) continue;
    const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x22, 0x02, (uint8_t)s, (uint8_t)b, (uint8_t)w, 0xF7};
    hooks.to_rytm(m, (int)sizeof m);
    s_bw_sent[s][0] = (int16_t)b, s_bw_sent[s][1] = (int16_t)w;
    sent = true;
  }
  if (sent) w0.ms = now_ms();
  return sent;
}
static CORE_COLD void sendfx_bw_all() {   // the Rytm asked or came up: what its knobs show may be its defaults
  Bw& w0 = BW();
  for (auto& x : w0.sent) x[0] = x[1] = -1;
  w0.ms = now_ms() - 20, w0.all_ms = now_ms(), w0.up = true;
  sendfx_bw_out();
}
static CORE_COLD void sendfx_bw_in(int s, int base, int width) {
  const int c = ins::kBus0 + s;
  ins::set_eq_bw(c, base, width);
  fx::edited(mod::tid(mod::kIns, c, 8)), fx::edited(mod::tid(mod::kIns, c, 9));   // (the Rytm's page: a Sidecar edit)
  ins::eq_bw(c, &base, &width);
  BW().sent[s][0] = (int16_t)base, BW().sent[s][1] = (int16_t)width;
  BW().up = true;
}
#endif

// Each box's own (the slot in hand's project, its own fxslots file in its folder, its own Rytm told)
static char fx_project_b[kVoiceSlots][24];   // the project whose fxslots file is loaded
static bool fx_loaded_b[kVoiceSlots];         // one was (none at boot: the first project, an empty name too, is read)
static bool fx_owed_b[kVoiceSlots];                    // the box is told the slots' names when the card core has read them
static uint32_t s_list_ms_b[kVoiceSlots];

CORE_COLD void sendfx_project() {
  const int b = fx::slot_in_hand();
  if (fx_loaded_b[b] && strcmp(fx_project_b[b], prj().key) == 0) return;
  fx_loaded_b[b] = true;
  strncpy(fx_project_b[b], prj().key, sizeof fx_project_b[b] - 1);
  fx::slots_load(prj().key);
  fx_owed_b[b] = true;
  sendfx_step();
}

CORE_COLD bool sendfx_step() {
  const int b = fx::slot_in_hand();
  bool& fx_owed = fx_owed_b[b];
#if CORE_POLY
  if (sendfx_hello() && BW().up && sendfx_bw_out()) return true;   // (after the Rytm was first heard: its ASK / 20 send them all)
#endif
  // a user effect slot changed (fx::list_changed): the list, the labels and the states again, at most every 100 ms
  if ((fx::list_dirty >> b & 1) && !fx_owed && now_ms() - s_list_ms_b[b] >= 100) {
    fx::list_dirty = (uint8_t)(fx::list_dirty & ~(1u << b));
    s_list_ms_b[b] = now_ms();
    sendfx_names_out();
    sendfx_labels_out();
    sendfx_state_out(true);
    return true;
  }
  if (!fx_owed || !fx::slots_step()) return false;
  fx::list_dirty = (uint8_t)(fx::list_dirty & ~(1u << b));   // (the list below has it)
  fx_owed = false;
  sendfx_names_out();
  sendfx_labels_out();
  sendfx_state_out(true);
  return true;
}

// 2026-10-27 (core/bundle/prjslot.h): a switch made by the slot reports, as the 10's below
CORE_COLD void host_project_changed() {
  if (hooks.project_seen) hooks.project_seen();
  else slots_1d(1000);
  sendfx_project();
}

namespace fxin {
static const char kFile[] = "/.rytm/fxin.txt";
// 3(c) step 8: each box's own (its file in its own folder, its 26 / 27 with its own box), the slot in hand's
struct Sl {
  bool want, dirty, hello, pending;   // setting, unsaved, first contact since boot, a 26 awaiting its 27
  int8_t rytm = -1;
  uint32_t ask_ms, sent_ms;   // when the oldest unanswered 26 went out (0: none); when the last went
  fmt::KvKeep keep;           // the lines a newer build wrote that this one does not read (fmt.h, class S: kept)
};
static Sl g_sl[kVoiceSlots];
static inline Sl& S() { return g_sl[prj_slot()]; }

CORE_COLD bool wanted() { return S().want; }
CORE_COLD int rytm() { return S().rytm; }
CORE_COLD bool active() { return S().rytm == 1; }
CORE_COLD const char* state() {
  if (S().rytm >= 0) return S().rytm ? "on" : "off";
  return S().ask_ms && now_ms() - S().ask_ms >= 3000 ? "unsupported" : "unknown";
}
static CORE_COLD void send26(uint8_t v) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x26, v, 0xF7};
  if (!hooks.to_rytm) return;
  hooks.to_rytm(m, (int)sizeof m);   // (the box in hand's link)
  if (S().rytm < 0 && !S().ask_ms) S().ask_ms = now_ms() | 1;
  S().pending = true;
  S().sent_ms = now_ms();
}
CORE_COLD void ask() { send26(0x7F); }
// the file's text into the slot in hand's setting
CORE_COLD void take_text(const char* b, int got) {
  Sl& x = S();
  x.want = got > 0 && strncmp(b, "src=fx", 6) == 0;
  x.keep.n = 0;
  static const char* const kKnown[] = {"src", nullptr};
  if (got > 0) fmt::kv_keep(b, (uint32_t)got, kKnown, &x.keep);
}
CORE_COLD void load() {
  File f;
  S().want = false;
  S().keep.n = 0;
  fmt::post_wait(kFile);   // 0.3.0: a posted save of it lands before we read it back (fmt.h)
  fmt::recover(kFile);   // a power cut between write_atomic's renames: the previous file back, a stray .tmp removed
  if (!f.open(kFile, kRead)) return;
  static char b[1536];
  memset(b, 0, sizeof b);
  int got = f.read(b, sizeof b - 1);
  f.close();
  take_text(b, got);
}
// 0.3.0 power-cut safety: posted to the card core as one atomic write (fmt.h), a newer build's keys kept (class S)
CORE_COLD bool save() {
  Sl& x = S();
  if (!x.dirty) return false;
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
  uint32_t n = (uint32_t)sniprintf(t, 16, "%s", x.want ? "src=fx\n" : "src=in\n");
  n = fmt::kv_append(x.keep, t, n, fmt::kPostMax);
  if (!fmt::post_end(kFile, n)) return false;
  x.dirty = false;
  return true;
}
CORE_COLD void set(bool on) {
  if (on != S().want) S().dirty = true;
  S().want = on;
  send26(on ? 1 : 0);
}
// 27 <s> from the box in hand: a reply to a 26, or the unasked one at USB port-up (the cue to send the setting again)
static CORE_COLD void on_state(uint8_t s) {
  Sl& x = S();
  bool reply = x.pending && now_ms() - x.sent_ms < 1000;   // (a 26 sent into a closed port is no one's reply)
  x.pending = false;
  x.rytm = s ? 1 : 0;
  x.ask_ms = 0;
  if (device::rytm_at(prj_slot())) rec::set_fxin(s != 0);   // (the recorder is the Rytm's: its slot's)
  if (!reply && x.want && s == 0) send26(1);   // a connect: the box forgot; a 00 in reply to our 01 is not repeated
}
CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "fxin", 4) != 0 || (p[4] && p[4] != '=')) return false;
  if (p[4]) {
    const char* v = p + 5;
    if (v[0] == '1' && !v[1]) set(true);
    else if (v[0] == '0' && !v[1]) set(false);
    else if (v[0] == '?' && !v[1]) ask();
    else {
      sniprintf(out, cap, CORE_STR("fxin=1|0|? (IN L/R: FX return | inputs | ask), not \"%s\""), v);
      return true;
    }
  }
  sniprintf(out, cap, CORE_STR("fxin: IN L/R source %s, the Rytm %s%s"), S().want ? "FX return" : "inputs", state(),
            strcmp(state(), "unsupported") == 0 ? " (not supported by this Rytm build)" : "");
  return true;
}
CORE_COLD int json(char* out, int cap) {
  return sniprintf(out, cap, CORE_STR("\"fxin\":{\"want\":%d,\"state\":\"%s\",\"rytm\":%d}"), S().want, state(), S().rytm);
}
// the first message since boot is the host's first contact: tell the box the setting (or ask)
static CORE_COLD void contact() {
  if (S().hello) return;
  S().hello = true;
  if (S().want) send26(1);
  else send26(0x7F);
}
}  // namespace fxin

CORE_COLD void link_reset(const char* why, int slot) {
  // the link's own voices (slot 1's 8..15 leave the Rytm's playing); on 8 voices every link's are 0..7
  const int b = slot < 0 ? device::voice_base() : device::voice_base(device::g_links[slot]);
  for (int vi = b; vi < b + kBoxVoices; ++vi) voice_silence(vi);
  scene::qperf_rebase();   // the QPERF knob's next report a baseline, not a move (core/mod/scene.h)
#if CORE_MSAMP
  midim::kit_changed(slot < 0 ? device::cur().index : slot);   // the box starts over: PRG and the rest of its voices again at the next report
#endif
  // 3(c): the link's own box's send FX (each box its own, its own 100 ms window)
  const int s = slot < 0 ? device::cur().index : slot;
  if (fx::all_sound_off(fx::control_of(s >= 0 && s < kVoiceSlots ? s : 0))) log(CORE_STR("link %s: voices silenced, send FX cleared"), why);
}

CORE_COLD void slot_left(int slot) {
  if (slot < 0 || slot >= device::kLinks) return;
  device::Scope in(device::g_links[slot]);
#if CORE_POLY
  BW().up = false;   // sfx-basewidth: its Rytm went (whichever slot it held); its next hello / ASK sends all again
  seq::slot_left();   // (its transport to STOP: its MIDI and arpeggiator end there; its edits saved by idle)
#endif
#if CORE_MSAMP
  midim::stop(slot);
#endif
  arp::stop(slot);
}

CORE_COLD void link_lost() {
  ui::remote_link_lost();   // the re-hello asking starts over (remote.cpp rehello_tick)
#if CORE_MSAMP
  chop::release_gates();   // a GATE slice would hold to its end: the Rytm's 0x11 will not come
#endif
#if CORE_POLY
  BW().up = false;   // sfx-basewidth: no 2 s refresh (nor change) into a port nobody listens on; its hello sends all
#endif
}

// Transport 0's (the host link's) whole SysEx: its slot (a box's hello moves it to slot 1), then handled as that slot's.
CORE_COLD void host_sysex(const uint8_t* d, int len) { engine_sysex_link(0, d, len); }

// 3(c) step 10: a box's MIDI clock is its own slot's. Transport t's realtime byte / song position goes to the slot its
// messages go to (device::slot_of: a box's hello moved it), into that box's send FX clock (its STUTTER's grid), which
// hands it on to that slot's LFOs (fx::clock_listener -> lfo::clock(slot, ..)). Nothing of it reaches another box's.
CORE_COLD int engine_realtime_link(int t, uint8_t status, uint16_t spp) {
  const int slot = device::slot_of(t, nullptr, 0);
  if (slot < 0) return -1;
  device::Scope in(device::g_links[slot]);
  fx::clock_in(fx::control_of(slot), status, spp);
  return slot;
}

__attribute__((noinline)) CORE_COLD void host_sysex_slot(const uint8_t* d, int len) {
  uint8_t box[device::kMaxMessage];   // a box's message as the engine's own (core/device/device.h); the Rytm: never
  if (device::foreign() && !device::translate(device::cur(), d, len, box)) return;
  if (len >= 6 && d[4] == device::kCmdHello && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54) {   // a box says what it is
    device::hello(device::cur(), d, len);
    return;
  }
  // F0 7D 52 54 cmd voice ...
  if (len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54 && d[4] == 0x27) {   // FXIN_STATE
    if (len >= 7 && d[5] <= 1) fxin::on_state(d[5]);
    return;
  }
  if (len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54) fxin::contact();
  if (len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54 && d[4] == 0x10) {   // the project
    if (hooks.project_seen) hooks.project_seen();   // (the Teensy's plugins: a 1D owed, rate-limited)
    else slots_1d(1000);
#if CORE_MSAMP
    kits::name_seen(d + 5, len - 5);   // (another name: kit reports waiting are of the project before)
#endif
    set_project(d + 5, len - 5);
    sendfx_project();   // the user-FX slot map follows the project
    return;
  }
  if (len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54) {
    if (d[4] == 0x19) {   // ASK (user slots; slots 0x40..0x42: the built-in MD's labels, core/md/md.h)
#if CORE_MD
      if (md::on_ask(d, len)) return;
#endif
      if (hooks.slot_ask) hooks.slot_ask(d, len);
      else slots_ask(d, len);
      return;
    }
    if (d[4] == 0x1E) {   // ASSIGN (user slots)
      if (hooks.slot_assign) hooks.slot_assign(d, len);
      else slots_assign(d, len);
      return;
    }
#if CORE_MSAMP
    if (d[4] == 0x4B) {   // the project slot reports (rytm-mods rui1052; core/bundle/prjslot.h)
      prjslot::sysex(d, len);
      return;
    }
    if (d[4] == 0x4E && len >= 54) {   // rui1052d freeze probe: the Rytm's UI task stuck over 2 s (rytm-mods 0026 fp_check)
      uint32_t w[9];
      for (int i = 0; i < 9; ++i)
        w[i] = (uint32_t)d[8 + 5 * i] << 28 | (uint32_t)d[9 + 5 * i] << 21 | (uint32_t)d[10 + 5 * i] << 14 |
               (uint32_t)d[11 + 5 * i] << 7 | d[12 + 5 * i];
      log(CORE_STR("rui: FREEZE PROBE: the Rytm's UI task %s for %d s; pc %08x stack %08x %08x %08x %08x %08x %08x %08x %08x"),
          d[6] ? "running (a loop)" : "blocked", d[7], (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3],
          (unsigned)w[4], (unsigned)w[5], (unsigned)w[6], (unsigned)w[7], (unsigned)w[8]);
      freeze_probe_seen(d[7]);
      return;
    }
    if (d[4] == 0x4D && len >= 11) {   // rui1052d (round 6): 0026's dropped key presses, its stock-dump waits run out
      log(CORE_STR("rui: the Rytm dropped %d key presses (each with its release: its queue full) and sent past a stock "
                   "dump %d times"), d[6] << 7 | d[7], d[8] << 7 | d[9]);
      return;
    }
    if (d[4] == 0x4C) {   // the kit reports: SAVE / RELOAD KIT (rytm-mods rui1052d; core/bundle/kits.h)
      kits::sysex(d, len);
      return;
    }
    if (d[4] == 0x35 && len >= 10) prjslot::hello(d[8]);   // (its caps byte: bit 2 the 4B reports)
#endif
    if (d[4] == 0x25) {   // SLOT PRINTS (user slots: which machines the project's sounds were made for)
      if (hooks.slot_prints) hooks.slot_prints(d, len);
      return;
    }
    if (d[4] == 0x20) {   // SENDFX (send levels, FX TYPE + params, tags): a TYPE or state change answered by 0x22
      // The Rytm asks (0x23) only until it has a name list, so after this engine (re)starts it may never ask again:
      // the first 0x20 since boot, with no ASK seen yet, sends the names, labels and states unasked, as an ASK would.
      if (!sendfx_hello()) {
        sendfx_hello() = true;
        sendfx_names_out();
        sendfx_labels_out();
        sendfx_state_out(true);
#if CORE_POLY
        sendfx_bw_all();
#endif
      }
      if (fx::take_sendfx(d, len) && d[6] >= 12) {
        if (sendfx_labels_missing()) {   // a TYPE whose labels did not fit the Rytm's tables: again, it first
          sendfx_names_out();
          sendfx_labels_out();
          sendfx_state_out(true);
        } else
          sendfx_state_out(false);
      }
      return;
    }
    if (d[4] == 0x23) {   // SENDFX ASK: the TYPE list and both states
      sendfx_hello() = true;
      sendfx_names_out();
      sendfx_labels_out();
      sendfx_state_out(true);
#if CORE_POLY
      sendfx_bw_all();   // (sfx-basewidth: and both BASE / WIDTH)
#endif
      return;
    }
    if (d[4] == 0x22) {   // sfx-basewidth: 22 ver 2, a turn of SEND FX 1 / 2's BASE / WIDTH (knobs G / H) on the Rytm
#if CORE_POLY
      if (len >= 10 && d[5] == 2 && d[6] < fx::kSlots && d[7] < 128 && d[8] < 128) sendfx_bw_in(d[6], d[7], d[8]);
#endif
      return;
    }
#if CORE_POLY
    if (poly::reply(d, len)) return;   // 28 02: the Rytm's answer to a chord write-back (core/poly.h)
#endif
#if CORE_POLY
    if (arp::sysex(d, len)) return;   // 45: a sound's arpeggiator settings (core/arp/arp.h)
    if (master::report(d, len)) return;   // 50: the stems' pans and who is off ROUTE TO MAIN (core/fx/master.h)
    if (d[4] == 0x2C || d[4] == 0x2D) {   // the sequencer's step ticks and state (core/seq/seq.h)
      seq::sysex(d, len);
      return;
    }
    if (d[4] == 0x35) seq::hello(len >= 9 ? d[6] << 7 | d[7] : -1);   // its seq-track mask forgotten: 2E again
#endif
    if (d[4] == 0x35) link_reset("hello");   // the Rytm's USB port came up: its transport and notes unknown
#if CORE_POLY
    if (d[4] == 0x35 && sendfx_hello()) sendfx_bw_all();   // (sfx-basewidth: a Rytm that came up again shows its defaults)
#endif
    if ((d[4] >= 0x30 && d[4] <= 0x3F) || d[4] == 0x29 || d[4] == 0x2A || d[4] == 0x2B || d[4] == 0x48 || d[4] == 0x49 ||
        d[4] == 0x4A) {
      // the remote UI (core/ui/remote.h; 29 / 2A the SRC view; rui1036: 48 a write's answer, 49 the Pi display's reports;
      // rui1038: 4A the bank list)
      int caps0 = ui::remote_caps();
      ui::remote_sysex(d, len);
      if (d[4] == 0x35 && sendfx_hello() && (ui::remote_caps() & 2) && !(caps0 & 2)) {   // fix1049 (review): a rui1049
        sendfx_names_out();                                                              // hello after the labels went:
        sendfx_labels_out();                                                             // its 16 tables filled now
        sendfx_state_out(true);
      }
      return;
    }
    if (d[4] == 0x09 || d[4] == 0x0B || d[4] == 0x0C) return;   // the retired folder walk
    if (d[4] == 0x13) { send_pool(); send_juno_names(); return; }
    if (d[4] == 0x18) { send_fm_names(); return; }
    if (d[4] == 0x0D && len >= 8) { send_listing(d[5] << 7 | d[6]); return; }
    if (d[4] == 0x0F && len >= 10) { pool_add(d[5] << 7 | d[6], d[7] << 7 | d[8]); return; }
    if (d[4] == 0x12 && len >= 11) { pool_assign(d[5], d[6] << 7 | d[7], d[8] << 7 | d[9]); return; }
  }
  if (len < 7 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54) return;
  int cmd = d[4], vi = device::voice_in(d[5]);   // the link's box voice -> engine voice (slot 1's: 8 + k)
  if (cmd == 0x04 && !device::voice_admitted(vi)) return;   // CPU admission (device/admit.h): a voice its box was not given
  if (cmd == 0x11) {   // NOTE_OFF: the voice's gate closed (plain voice, no flags)
#if CORE_POLY
    if (arp::trig_off(vi)) return;    // the arpeggiator held that trig: its notes end, the note sounding plays out its gate
    if (inst::seq_mono(vi)) return;   // E0: the sequencer's mono note is ended by its own note-off, not the Rytm's
#endif
    voice_note_off(vi);
    return;
  }
  voice_report_at(vi, cmd, d, len);
}

}  // namespace core
