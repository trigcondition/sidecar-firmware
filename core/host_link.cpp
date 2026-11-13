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
static uint64_t s_labels_sent;   // bit id: its 0x24 went in the last burst (ids 1..63)
static uint8_t s_ulabels_sent;   // bit u: user TYPE 64 + u's (Plugin API 3's user effects, from their manifests)
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
  one(fx::control.knob[0][0]);
  one(fx::control.knob[1][0]);
  for (int id = 1; id < fx::kFirstUser + fx::kUserSlots; ++id) one(id);
  s_labels_sent = sent, s_ulabels_sent = usent;
}
// a slot's TYPE has labels the Rytm was not sent (the cap above)
static CORE_COLD bool sendfx_labels_missing() {
  for (int s = 0; s < fx::kSlots; ++s) {
    int id = fx::control.knob[s][0];
    if (label_id(id) && fx::labels(id) && !label_sent(id, s_labels_sent, s_ulabels_sent)) return true;
  }
  return false;
}
static uint8_t fx_sent_id[fx::kSlots] = {0xFF, 0xFF}, fx_sent_st[fx::kSlots];
static bool s_sendfx_hello;   // names + labels + states sent since boot (asked or not)

static CORE_COLD void sendfx_state_out(bool all) {
  for (int s = 0; s < fx::kSlots; ++s) {
    uint8_t id = fx::control.knob[s][0], st = fx::state(s);
    if (!all && id == fx_sent_id[s] && st == fx_sent_st[s]) continue;
    fx_sent_id[s] = id, fx_sent_st[s] = st;
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
static int16_t s_bw_sent[fx::kSlots][2] = {{-1, -1}, {-1, -1}};
static uint32_t s_bw_ms, s_bw_all_ms;
static bool s_bw_up;   // the Rytm listens: set by its ASK, first 0x20, hello or a turn; cleared by link_lost()
static CORE_COLD bool sendfx_bw_out() {
  if (!hooks.to_rytm || now_ms() - s_bw_ms < 20) return false;
  if (now_ms() - s_bw_all_ms >= kBwRefreshMs) {
    for (auto& x : s_bw_sent) x[0] = x[1] = -1;
    s_bw_all_ms = now_ms();
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
  if (sent) s_bw_ms = now_ms();
  return sent;
}
static CORE_COLD void sendfx_bw_all() {   // the Rytm asked or came up: what its knobs show may be its defaults
  for (auto& x : s_bw_sent) x[0] = x[1] = -1;
  s_bw_ms = now_ms() - 20, s_bw_all_ms = now_ms(), s_bw_up = true;
  sendfx_bw_out();
}
static CORE_COLD void sendfx_bw_in(int s, int base, int width) {
  const int c = ins::kBus0 + s;
  ins::set_eq_bw(c, base, width);
  fx::edited(mod::tid(mod::kIns, c, 8)), fx::edited(mod::tid(mod::kIns, c, 9));   // (the Rytm's page: a Sidecar edit)
  ins::eq_bw(c, &base, &width);
  s_bw_sent[s][0] = (int16_t)base, s_bw_sent[s][1] = (int16_t)width;
  s_bw_up = true;
}
#endif

static char fx_project[24] = "\x01";   // the project whose fxslots file is loaded (never a real name at boot)

static bool fx_owed = false;           // the Rytm is told the slots' names when the card core has read them

CORE_COLD void sendfx_project() {
  if (strcmp(fx_project, project) == 0) return;
  strcpy(fx_project, project);
  fx::slots_load(project);
  fx_owed = true;
  sendfx_step();
}

static uint32_t s_list_ms = 0;
CORE_COLD bool sendfx_step() {
#if CORE_POLY
  if (s_sendfx_hello && s_bw_up && sendfx_bw_out()) return true;   // (after the Rytm was first heard: its ASK / 20 send them all)
#endif
  // a user effect slot changed (fx::list_changed): the list, the labels and the states again, at most every 100 ms
  if (fx::list_dirty && !fx_owed && now_ms() - s_list_ms >= 100) {
    fx::list_dirty = false;
    s_list_ms = now_ms();
    sendfx_names_out();
    sendfx_labels_out();
    sendfx_state_out(true);
    return true;
  }
  if (!fx_owed || !fx::slots_step()) return false;
  fx::list_dirty = false;   // (the list below has it)
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
static bool s_want, s_dirty, s_hello, s_pending;   // setting, unsaved, first contact since boot, a 26 awaiting its 27
static int8_t s_rytm = -1;
static uint32_t s_ask_ms, s_sent_ms;   // when the oldest unanswered 26 went out (0: none); when the last went

CORE_COLD bool wanted() { return s_want; }
CORE_COLD int rytm() { return s_rytm; }
CORE_COLD bool active() { return s_rytm == 1; }
CORE_COLD const char* state() {
  if (s_rytm >= 0) return s_rytm ? "on" : "off";
  return s_ask_ms && now_ms() - s_ask_ms >= 3000 ? "unsupported" : "unknown";
}
static CORE_COLD void send26(uint8_t v) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x26, v, 0xF7};
  if (!hooks.to_rytm) return;
  hooks.to_rytm(m, (int)sizeof m);
  if (s_rytm < 0 && !s_ask_ms) s_ask_ms = now_ms() | 1;
  s_pending = true;
  s_sent_ms = now_ms();
}
CORE_COLD void ask() { send26(0x7F); }
static fmt::KvKeep s_keep;   // the lines a newer build wrote that this one does not read (fmt.h, class S: kept)
CORE_COLD void load() {
  File f;
  s_want = false;
  s_keep.n = 0;
  fmt::post_wait(kFile);   // 0.3.0: a posted save of it lands before we read it back (fmt.h)
  fmt::recover(kFile);   // a power cut between write_atomic's renames: the previous file back, a stray .tmp removed
  if (!f.open(kFile, kRead)) return;
  static char b[1536];
  memset(b, 0, sizeof b);
  int got = f.read(b, sizeof b - 1);
  f.close();
  s_want = strncmp(b, "src=fx", 6) == 0;
  static const char* const kKnown[] = {"src", nullptr};
  if (got > 0) fmt::kv_keep(b, (uint32_t)got, kKnown, &s_keep);
}
// 0.3.0 power-cut safety: posted to the card core as one atomic write (fmt.h), a newer build's keys kept (class S)
CORE_COLD bool save() {
  if (!s_dirty) return false;
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
  uint32_t n = (uint32_t)sniprintf(t, 16, "%s", s_want ? "src=fx\n" : "src=in\n");
  n = fmt::kv_append(s_keep, t, n, fmt::kPostMax);
  if (!fmt::post_end(kFile, n)) return false;
  s_dirty = false;
  return true;
}
CORE_COLD void set(bool on) {
  if (on != s_want) s_dirty = true;
  s_want = on;
  send26(on ? 1 : 0);
}
// 27 <s> from the Rytm: a reply to a 26, or the unasked one at USB port-up (the cue to send the setting again)
static CORE_COLD void on_state(uint8_t s) {
  bool reply = s_pending && now_ms() - s_sent_ms < 1000;   // (a 26 sent into a closed port is no one's reply)
  s_pending = false;
  s_rytm = s ? 1 : 0;
  s_ask_ms = 0;
  rec::set_fxin(s != 0);
  if (!reply && s_want && s == 0) send26(1);   // a connect: the Rytm forgot; a 00 in reply to our 01 is not repeated
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
  sniprintf(out, cap, CORE_STR("fxin: IN L/R source %s, the Rytm %s%s"), s_want ? "FX return" : "inputs", state(),
            strcmp(state(), "unsupported") == 0 ? " (not supported by this Rytm build)" : "");
  return true;
}
CORE_COLD int json(char* out, int cap) {
  return sniprintf(out, cap, CORE_STR("\"fxin\":{\"want\":%d,\"state\":\"%s\",\"rytm\":%d}"), s_want, state(), s_rytm);
}
// the first message since boot is the host's first contact: tell the Rytm the setting (or ask)
static CORE_COLD void contact() {
  if (s_hello) return;
  s_hello = true;
  if (s_want) send26(1);
  else send26(0x7F);
}
}  // namespace fxin

CORE_COLD void link_reset(const char* why) {
  for (int vi = 0; vi < kVoices; ++vi) voice_silence(vi);
  scene::qperf_rebase();   // the QPERF knob's next report a baseline, not a move (core/mod/scene.h)
#if CORE_MSAMP
  midim::kit_changed();   // the Rytm starts over: PRG and the rest again at the next report
#endif
  if (fx::all_sound_off()) log(CORE_STR("link %s: voices silenced, send FX cleared"), why);
}

CORE_COLD void link_lost() {
#if CORE_MSAMP
  chop::release_gates();   // a GATE slice would hold to its end: the Rytm's 0x11 will not come
#endif
#if CORE_POLY
  s_bw_up = false;   // sfx-basewidth: no 2 s refresh (nor change) into a port nobody listens on; its hello sends all
#endif
}

__attribute__((noinline)) CORE_COLD void host_sysex(const uint8_t* d, int len) {
  uint8_t box[device::kMaxMessage];   // a box's message as the engine's own (core/device/device.h); the Rytm: never
  if (device::foreign() && !device::translate(device::g_link, d, len, box)) return;
  if (len >= 6 && d[4] == device::kCmdHello && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54) {   // a box says what it is
    device::hello(device::g_link, d, len);
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
      if (!s_sendfx_hello) {
        s_sendfx_hello = true;
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
      s_sendfx_hello = true;
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
    if (d[4] == 0x35 && s_sendfx_hello) sendfx_bw_all();   // (sfx-basewidth: a Rytm that came up again shows its defaults)
#endif
    if ((d[4] >= 0x30 && d[4] <= 0x3F) || d[4] == 0x29 || d[4] == 0x2A || d[4] == 0x2B || d[4] == 0x48 || d[4] == 0x49 ||
        d[4] == 0x4A) {
      // the remote UI (core/ui/remote.h; 29 / 2A the SRC view; rui1036: 48 a write's answer, 49 the Pi display's reports;
      // rui1038: 4A the bank list)
      int caps0 = ui::remote_caps();
      ui::remote_sysex(d, len);
      if (d[4] == 0x35 && s_sendfx_hello && (ui::remote_caps() & 2) && !(caps0 & 2)) {   // fix1049 (review): a rui1049
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
  int cmd = d[4], vi = d[5] & 7;
  if (cmd == 0x11) {   // NOTE_OFF: the voice's gate closed (plain voice, no flags)
#if CORE_POLY
    if (arp::trig_off(vi)) return;    // the arpeggiator held that trig: its notes end, the note sounding plays out its gate
    if (inst::seq_mono(vi)) return;   // E0: the sequencer's mono note is ended by its own note-off, not the Rytm's
#endif
    voice_note_off(vi);
    return;
  }
  voice_report(cmd, d, len);
}

}  // namespace core
