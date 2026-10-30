// core/seq/seq.cpp - see seq.h: the sequencer's state, the Rytm's 2C / 2D, the planner and its events, the recorder,
// the trig-key / encoder editor, the LEDs, the card. Control side only; per block: pre_block's compares.
#include "seq/seq.h"

#if CORE_POLY
#include "seq/rytm_dump.h"
#include "seq/rng.h"
#include <stdlib.h>
#include <string.h>

#include "hooks.h"   // hooks.transport: the mods' PLAY / STOP to the platform (the recorder)
#include "fx/send_fx.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "inst.h"
#include "lfo.h"
#include "machines.h"
#include "platform.h"
#include "poly.h"
#include "fmt/fmt.h"   // write_atomic / recover: every build's pattern saves (0.3.0 power-cut safety)
#include "pool/pool.h"
#if CORE_MSAMP
#include "store/store.h"
#endif
#include "sysex.h"
#include "ui/remote.h"
#include "voices.h"

namespace core {
namespace seq {
void ne_cover_outside();   // NOTE EDIT's undo (below ui_key): install() runs it before a pattern goes
void ne_leave();           // (below ui_key): seqmode_end runs it when NOTE EDIT's track is no poly track now
void start_empty(int r);   // (below ne_undo) fix1028: RESTORE n NOTES? answered NO
void ne_undo();

namespace {

constexpr int kEvents = 1024;               // seqtop: P4 / M4's repeats (a step: up to kRtNotes notes)
constexpr int kRtNotes = kNotes * kRtMax;   // (an 8-note chord x 16: every repeat)
constexpr int kLookahead = 1536;            // frames planned beyond the next block (32 ms)
constexpr int kDefaultLatency = 192;        // frames the notes start early (4 ms; calibrate: seq=lat:N)
// per pattern poly: the next pattern's mask goes to the Rytm this many frames (plus the latency) before the predicted
// switch - its first step plays with the new capture, the old pattern's last step with the old one
constexpr int kSwitchLead = kLookahead;
constexpr uint32_t kSaveMs = 1000, kTapMs = 450, kChordGapMs = 60;
constexpr uint8_t kLedOff = 0, kLedBeyond = 2, kLedNote = 47, kLedChord = 79, kLedPlay = 111, kLedHeld = 63,
                  kLedPageDim = 43, kLedPageOn = 47;
// seq-ux: a step with locks magenta (the palette's 80..95 ramp, a colour stock never lights the trig keys in), a lock
// trig (locks, no notes) dim magenta
constexpr uint8_t kLedLock = 95, kLedLockOnly = 85;
constexpr uint8_t kLedNeSel = 27;   // U1 NOTE EDIT: a step with selected notes (green)

struct Ev {
  int64_t f;
  int64_t sf;   // its step's start (host frame, without the latency): what a switch cuts by
  uint16_t id;
  uint8_t on, vi, note, vel, track;   // on: 1 a note-on, 0 its off, 2 a step's locks (seq-ux: note = its lset slot)
  uint8_t pred;   // planned from the predicted next pattern (kept by the switch it predicted, else dropped)
  uint8_t step;   // seqtop: the step it belongs to (the mapped one; 0xFF a preview's) and its track's step count
  int32_t v;
};
// seqtop: the last notes started (seq=played, the e2e's view of the order): the track, its step count, the Rytm's step
// count when it started, the step, the pitch
struct Played {
  int8_t track;
  uint8_t step, pitch;
  int32_t v, a;
};
constexpr int kPlayed = 64;
Played played[kPlayed];
uint32_t played_n = 0;
struct Open {   // a recorded note waiting for its note-off
  bool used;
  int8_t track;
  uint8_t pitch;
  int idx;
  double x;     // its start in steps
};

Pattern* cur = nullptr;    // playing and edited
Pattern* nxt = nullptr;    // prefetched (nxt->index the pattern, -1 none)
Pattern* sav = nullptr;    // a dirty pattern left at a switch, saved by idle()
bool sav_pending = false;

Follower fol;
StepClock clk[kRytmTracks];
bool restart[kRytmTracks];
int64_t planned[kRytmTracks];   // the next step count to plan
int64_t play_abs[kRytmTracks];  // the last tick's step count
uint64_t hframes = 0;
int transport = kStop;
// Every change of `transport` goes through here: the platform hears it as MIDI realtime (hooks.transport: the Pi's
// recorder arms on it, whatever the Rytm's TRANSPORT SEND says) - a PLAY taken from the ticks before the TRANSPORT
// report, the report itself, and a reset all count. Unchanged: nothing (the report repeats).
void set_transport(int t) {
  if (t == transport) return;
  const int was = transport;
  transport = t;
  if (hooks.transport) hooks.transport(t == kPlay ? (was == kPause ? 0xFB : 0xFA) : 0xFC);
}
int64_t transport_frame = INT64_MIN;   // the Rytm frame of the last TRANSPORT (older ticks after it: ignored)
double bpm = 0;
// per pattern poly (2026-10-26): the seq tracks NOW - derived, never set: the current pattern's slots that are on (Track::on)
// whose Rytm track plays a Sidecar machine or has not been reported yet (remask). Every meaning downstream is as before
// (the 2E, the pin, the 46, in_mask, owns_track / owns_voice, the pads, the pages).
uint16_t mask = 0;
// tracks.txt's mask= line: no longer read for membership; kept (and set / cleared with each track turned on / off, as
// an older build wrote it) so an older Sidecar reading the project plays it as it did
uint16_t legacy_mask = 0;
int mask_sent = -1;
bool hello_due = true;          // 2E with a resync owed (boot, a hello)
uint16_t mutes = 0;
// fix1049 (hardware 2026-10-23: soloing a Rytm track left the Sidecar voices playing): 2D 0D SOLO, the soloed tracks
// (rytm-mods 0026 1049; an older Rytm never sends it: 0). The tracks heard: not muted, and soloed when any is.
uint16_t solos = 0;
CORE_COLD uint16_t silenced_now() { return (uint16_t)(mutes | (solos ? ~solos & 0x1FFF : 0)); }
bool rec_live = false, rec_grid = false;
int pat_now = 0, pat_queued = -1, pat_loaded = -1;
bool load_due = true;
bool seqmode = false;
int seqmode_track = -1;
bool we_opened = false;
bool show_wanted = false;
bool named = false;         // a project was named since boot (0x10)   // the SEQ page was asked for: in the page list from then on
int latency = kDefaultLatency;
int quant = 0;

Ev ev[kEvents];
int nev = 0;
uint16_t ev_id = 0;
uint8_t sounding[kVoices][128];
Open opens[16];
uint32_t free_seed = 0x12345678u;   // E1: FREE patterns' seed, a new one at each PLAY (roll_seed)

bool dirty = false;
uint32_t dirty_ms = 0;
bool save_now = false;
bool tracks_dirty = false;
uint32_t tracks_ms = 0;   // fix1049: the last change to tracks.txt's settings (its save waits kSaveMs after it)
// fix1049 (the POLY CONFIG knob's retriggers): every knob step wrote tracks.txt at once, on the control core (the Pi's
// core 3, which renders a block only between its passes: a card write is a dropout, heard as the playing note
// retriggering while the knob turned). Now a change only stamps the time; idle() writes once it has settled.
CORE_COLD bool tracks_refused();   // (below: the RAM store's loading project)
CORE_COLD bool tracks_touch() {   // false: refused (the project still loading: the settings put back)
  if (tracks_refused()) return false;
  tracks_dirty = true, tracks_ms = now_ms();
  return true;
}

struct Ui {
  int track = 0;
  int page = 0;
  bool follow = true;
  uint16_t held = 0;
  uint32_t held_ms[16];
  bool held_used[16], held_first[16];
  bool page_held = false, page_used = false;
  int sel = 0;
  int touched = -1;
  uint8_t chord[kNotes], chord_vel[kNotes];
  int chord_n = 0;
  uint32_t chord_ms = 0;
  uint8_t leds[16];
  // seq-ux (1036): the prompt the SEQ page shows (YES / NO), the lock page while trigs are held (hold trig + SRC ..),
  // the page opened by the Rytm's TRK + TRIG (rui1037; rui1036's FUNC + LFO; NO there closes it again)
  int prompt = 0, prompt_track = -1, prompt_sel = 0, prompt_n = 0, prompt_locks = 0;
  int prompt_pat = -1;   // fix1028 review: the pattern the track's menu / OFF / RESTORE was asked in
  uint32_t off_hint_ms = 0;   // R2: B turned down on a poly track (footer hint for a while)
  int lockpg = -1;
  bool view_opened = false;
  bool preview_yes = false;   // rui1037: YES's press played the held steps: its release is ours too
  // rui1050: the TRIG view's pages (seq.h kTv* without a held trig, kSv* with one; TRIG cycles them), FUNC held (the
  // SEQ page catches it), the lock page's encoder pushes (a long one: that knob's COND; FUNC + one: its SLIDE), a
  // transient footer
  int tv = 0, sv = 0;
  bool func = false;
  uint8_t push_down = 0, push_done = 0;
  uint32_t push_ms[8];
  int cond_e = -1;
  char msg[28];
  uint32_t msg_ms = 0;
  bool leds_sent = false;
  uint8_t pleds[4];
  bool pleds_sent = false, pleds_stock = true;
  uint32_t leds_ms = 0;
  // U1 NOTE EDIT (below ui_key): on, the highlighted note (step idx, pitch; -1 none), the track and pattern it is of, the
  // selection (a pitch bitmap a step) and its count, a counter that moves on every change (the touchscreen)
  bool ne = false;
  int ne_idx = -1, ne_pitch = -1, ne_track = -1, ne_pattern = -1, ne_nsel = 0;
  uint32_t ne_gen = 0;
  uint32_t ne_sel[kMaxSteps][4];
  uint32_t ne_edits = 0;               // review round 1: st.edits when the selection was last pruned
  int ne_undo_e = -1;                  // the gesture of the last undo level (a knob, -2 a fill, -3 a delete), its time,
  uint32_t ne_undo_ms = 0;             // track, pattern and the highlight after it (round 2: one level a gesture)
  int ne_undo_r = -1, ne_undo_p = -1, ne_undo_key = -1;
  uint32_t ne_del_ms = 0;              // FUNC + NO asked (more than one note): its time | 1, how many
  int ne_del_n = 0;
  uint32_t msg_dur = 2500;             // the transient footer's time
} ui;
// NOTE EDIT's undo (review rounds 1 / 2; below ui_key): the platform's hooks, the track before the gesture running and
// after the last one
const UndoHooks* ne_hooks = nullptr;
Track ne_pre, ne_after;
bool ne_after_on = false;
int ne_after_r = -1, ne_after_p = -1;

Stats st;
uint32_t view_gen = 0;          // seq-ux: TRK + TRIG's requests (rui1036: FUNC + LFO) (the Pi's touch editor follows view_track)
int view_track = -1;

// ---- seq-ux (1036): the steps' parameter locks, applied at their step on the host (seq.h Lock). A step with notes
// or locks on a track that has locks applies its set: its params locked, every other param the track locked before
// back to its base (the Rytm's value, which keeps coming in: params_in / the FX knob filter); a step without either
// changes nothing (a lock holds until the track's next trig or lock trig, as the Rytm's own p-locks under a ringing
// note). STOP gives every base back. P1..P16 per voice (the LFOs on top: the lock is their base), FX P2..P5 globally
// (the last track that locked one owns it).
struct VLock {
  uint16_t on;
  float base[16], val[16];
};
VLock vlk[kVoices];
struct FLock {
  uint8_t on, seen;
  int8_t owner[8];
  uint8_t base[8], val[8], rep[8];   // rep: the Rytm's last value of FX s P(2 + i) (i = 4 s + k - 2)
};
FLock flk;
int8_t ilk_owner[kVoices][8];   // fx-ins: the track whose step locked voice v's insert param j (slot j / 4), -1 none
struct IlkInit {
  IlkInit() { memset(ilk_owner, -1, sizeof ilk_owner); }
} ilk_init;
uint8_t (*fx_prev)(int, int, uint8_t) = nullptr;   // the knob filter before ours (the LFOs')
struct LockSet {
  uint8_t n;
  Lock l[kLocks];
  // seqtop X1: lock i slides (bit i) to to[i], reached at host frame end[i]
  uint16_t slide;
  uint16_t to[kLocks];
  int64_t end[kLocks];
};
// X1: a track's slides now (from the lock set applied last): param p from `from` at f0 to `to` at f1
struct Slide {
  uint8_t on;
  uint16_t from, to, last;
  int64_t f0, f1;
};
Slide slides[kRytmTracks][kLocks];
LockSet cur_set[kRytmTracks];   // the set each track applied last (the slides re-apply it with their values)
uint16_t slide_any = 0;         // the tracks with a slide running
constexpr int kLockSets = 64;
LockSet lset[kLockSets];
int lset_w = 0;
CORE_COLD float lock_float(int v) { return (float)(v >> 7) + (float)(v & 127) / 128.0f; }
CORE_COLD uint8_t fx_play(int sl, int k, uint8_t v) { return fx_prev ? fx_prev(sl, k, v) : v; }
CORE_COLD uint8_t fx_filter(int sl, int k, uint8_t val) {   // fx::knob_filter: the Rytm's FX knob reports
  if (k >= 2 && k <= 5 && sl >= 0 && sl < 2) {
    int i = 4 * sl + k - 2;
    uint8_t bit = (uint8_t)(1u << i);
    flk.rep[i] = val, flk.seen |= bit;
    if (flk.on & bit) {
      flk.base[i] = val;
      return fx_play(sl, k, flk.val[i]);
    }
  }
  return fx_play(sl, k, val);
}

// ---- seq-p2: the Rytm's edits (2D 0A) on the host's data: a clipboard and one undo per level, as stock keeps them
Pattern* clip_p = nullptr;      // COPY PAT: the whole pattern (index -1 none)
Pattern* undo_p = nullptr;      // the pattern before the last PASTE / CLEAR PAT (index = its pattern, -1 none)
int undo_p_kind = 0;            // 1 clear, 2 paste
Track clip_t;                   // COPY TRK (rytm -1: none copied; an empty track copied: rytm >= 0, no steps)
Track undo_t;                   // the track before the last PASTE / CLEAR TRK
int undo_t_kind = 0, undo_t_track = -1, undo_t_pat = -1;
Step clip_g[kStepsPerPage];     // a page (COPY PAGE)
bool clip_g_valid = false;
Step undo_g[kStepsPerPage];
int undo_g_kind = 0, undo_g_track = -1, undo_g_pat = -1, undo_g_page = -1;
uint32_t undo_g_edits = 0;      // seq mode's own page paste / clear: undone by the same gesture with no edit between
Step clip_r[kStepsPerPage];     // trigs (COPY TRIGS): the held steps in order
int clip_r_n = 0;
Step undo_r[kStepsPerPage];
uint16_t undo_r_mask = 0;
int undo_r_kind = 0, undo_r_track = -1, undo_r_pat = -1, undo_r_page = -1;
uint32_t undo_r_edits = 0;
// 2D 04 SCALE: the Rytm tracks' lengths and speeds (-1 unknown), the pattern's (len, speed, mode, change length)
int sc_len[kRytmTracks], sc_spd[kRytmTracks];
int pat_len = -1, pat_spd = -1, pat_mode = -1, pat_chg = -1;
CORE_COLD int pat_len_of() { return pat_mode == 0 && pat_len >= 2 ? pat_len : -1; }   // PATTERN scale's length (-1 none)
bool fill = false;
// the analog pin (rytm-mods 1033): the seq tracks (unless pin_seq is off) and pin_extra (the host's poly tracks)
uint32_t pat_changed_ms = 0;    // a pattern change (02 / 03): the SCALE reports that follow are the new pattern's
uint16_t pin_extra = 0;
bool pin_seq = true;
uint16_t clock_extra = 0;       // core/song's clock-only tracks (2E bytes 3 / 4)
int clock_sent = -1;
int pin_sent = -1;              // the 2E's pin part last sent (pin_extra | !pin_seq << 12), -1 none
uint16_t silent_sent = 0xFFFF;  // the last 46 (0xFFFF: none since the hello)
uint32_t heard_ms[kRytmTracks]; // the last time something of the host's sounded on the track's voice
constexpr uint32_t kSilentMs = 400;
// latency calibration (seq=cal): clicks on a voice, found again in its Rytm -> host stream
constexpr int kCalClicks = 5, kCalClickFrames = 96;
struct Cal {
  volatile int state = 0;       // 0 idle, 1 a click due at `at`, 3 rendering it, 2 rendered: waiting for it to come back
  volatile int vi = -1;
  volatile int64_t at = 0;      // the click's first frame (host render frames)
  volatile int64_t found = -1;  // its arrival (host frames), -1 not yet
  volatile int32_t floor = 0;   // the stream's peak before the click (its noise)
  int track = -1, n = 0, res[kCalClicks];
  uint32_t next_ms = 0, deadline_ms = 0;
  bool auto_done = false, manual = false;
  uint32_t fed_ms = 0;          // the platform fed cal_audio lately (a stream to calibrate on)
} cal;
// switch prediction: a queued pattern's step 0 planned ahead, at the master length's end
bool predict = true;
int pred_to = -1;               // the pattern predicted to start, -1 none
int64_t pred_frame = 0;         // its Rytm frame
int64_t pred_abs[kRytmTracks];  // per track: the step count of the switch (-1 none)
int pred_planned[kRytmTracks];  // the new pattern's steps planned so far
int64_t pat_start = -1;         // the Rytm frame of the playing pattern's step 0 (the first tick after PLAY / a switch)
int pat_start_track = -1;

CORE_COLD int voice_of(int r) { return ui::remote_track_voice(r); }
CORE_COLD bool in_mask(int r) { return r >= 0 && r < kRytmTracks && (mask >> r & 1); }
// per pattern poly: Rytm track r plays a Sidecar machine, or its machine is not reported yet (a Pi reboot: the 33 comes
// only at the next pattern / kit change - until then its capture stays)
CORE_COLD bool plays_sidecar(int r) { return ui::remote_track_machine(r) != 0x7F; }
CORE_COLD uint16_t mask_of(const Pattern& p) {
  if (p.index < 0) return 0;
  uint16_t m = 0;
  for (const Track& t : p.t)
    if (t.rytm >= 0 && t.rytm < kRytmTracks && t.on && plays_sidecar(t.rytm)) m |= (uint16_t)(1u << t.rytm);
  return m;
}
CORE_COLD bool slot_on(int r) {   // track r is a poly track in the current pattern (whatever it plays now)
  const Track* t = cur && r >= 0 && r < kRytmTracks ? track_of(*cur, r) : nullptr;
  return t && t->on;
}
uint16_t clock_now = 0;   // the clock-only tracks the last 2E named (core/song's, a queued pattern's new seq tracks)
CORE_COLD int ui_track() { return seqmode && seqmode_track >= 0 && seqmode_track < kRytmTracks ? seqmode_track : ui.track; }
CORE_COLD double tempo_period() { return bpm > 1 ? step_frames(bpm, 2) : 0; }

int pat_len_of();
CORE_COLD int eff_len(const Track* t, int r) {
  // rui1051 (the hardware report: a 2-page poly track shown as one page while stopped): on PATTERN scale (2D 04's mode
  // 0) every track plays the pattern's length, whatever its own length byte says - known from the 2D 04 before any
  // clock tick (playing, the ticks' length agreed)
  int pl = pat_len_of();
  int len = t && t->length ? t->length : pl > 0 ? pl : clk[r].rytm_len;
  return len < 1 ? 16 : len > kMaxSteps ? kMaxSteps : len;
}
CORE_COLD int npages_of(int len) { return (len + kStepsPerPage - 1) / kStepsPerPage; }
// seq1112: the one place (inside this file: the SEQ page, the console and 2D 04 all land here) a track's length is written (the SEQ page's C / TLEN, seq=len:T:N, and the Rytm's 2D 04
// SCALE). A grow copies the pages it reveals the Rytm's way (pattern.cpp grow_pages); a shrink does nothing, so the
// steps past the new length stay in the pattern and on the card and come back when the length grows again.
// The compare is on the **effective** length (eff_len: AUTO resolved against the Rytm's own), because that is what
// the player and the pages show - leaving AUTO for the same number of steps is not a growth.
CORE_COLD void set_length(Track& t, int r, int len) {
  int was = eff_len(&t, r);
  t.length = (uint16_t)(len < 0 ? 0 : len > kMaxSteps ? kMaxSteps : len);
  int now = eff_len(&t, r);
  if (now > was) grow_pages(t, was, now);
}
// seqtop P2: a track's own step clock, a ratio of the Rytm track's ("virtual" steps v: Sidecar step v starts at the Rytm
// track's step position v x den / num). planned[] counts these. FOLLOW (1:1): v = the Rytm's step count, as before.
CORE_COLD Ratio rat_of(const Track* t) { return speed_ratio(t ? t->speed : 0); }
CORE_COLD double vpos(Ratio q, int64_t v) { return q.num == q.den ? (double)v : (double)v * q.den / q.num; }
CORE_COLD int64_t vfloor_i(Ratio q, int64_t a) { return a < 0 ? 0 : a * q.num / q.den; }            // the step at a
CORE_COLD int64_t vceil_i(Ratio q, int64_t a) { return a < 0 ? 0 : (a * q.num + q.den - 1) / q.den; }   // the first at / after a
CORE_COLD int64_t vceil_d(Ratio q, double x) {
  if (x <= 0) return 0;
  double y = x * q.num / q.den;
  int64_t v = (int64_t)y;
  return (double)v < y - 1e-9 ? v + 1 : v;
}

CORE_COLD void send(const uint8_t* m, int n) { to_rytm(m, n); }
CORE_COLD void send_cmd(uint8_t cmd, const uint8_t* b, int nb) {
  uint8_t m[32];
  int n = 0;
  m[n++] = 0xF0, m[n++] = 0x7D, m[n++] = 0x52, m[n++] = 0x54, m[n++] = cmd, m[n++] = 1;
  for (int i = 0; i < nb && n < 31; ++i) m[n++] = b[i] & 0x7F;
  m[n++] = 0xF7;
  send(m, n);
}

// ---- E0: the poly config a track plays with: the pattern's own (its v4 block), else the Rytm track's default - the
// last config set on it in this project (tracks.txt "poly=T:..." lines), so a track keeps its voicing across patterns
// as an Analog Four keeps its POLY CONFIG in the kit. All zero: AUTO.
PolyCfg poly_def[kRytmTracks];
CORE_COLD PolyCfg cfg_of(const Track* t, int r) {
  if (t && !poly_default(t->poly)) return t->poly;
  return r >= 0 && r < kRytmTracks ? poly_def[r] : PolyCfg{};
}

// ---- the events
CORE_COLD void push(int64_t f, int64_t sf, uint16_t id, int on, int vi, int note, int vel, int track, bool pred,
                    int step = 0xFF, int64_t v_ = -1) {
  if (nev >= kEvents) {
    ++st.dropped_queue;
    return;
  }
  ev[nev++] = {f, sf, id, (uint8_t)on, (uint8_t)vi, (uint8_t)note, (uint8_t)vel, (uint8_t)track, (uint8_t)pred, (uint8_t)step,
               (int32_t)v_};
}
CORE_COLD void note_on(int vi, int note, int vel, int r) {
  if (sounding[vi][note] < 255) ++sounding[vi][note];
  // the voice renders from now on: on an empty Rytm track no trig (0x04) ever made it live (seq-p2: the pinned voice
  // carries the host's notes alone)
  // E0: through the track's poly config (core/inst.h): its machine's own pool, the mono voice, or its instances
  const Track* t = r >= 0 && r < kRytmTracks && cur ? track_of(*cur, r) : nullptr;
  if (inst::seq_on(vi, note, vel, cfg_of(t, r))) voice_wake(vi);
  ++st.notes_on;
}
CORE_COLD void note_off(int vi, int note) {
  if (!sounding[vi][note]) return;
  if (--sounding[vi][note]) return;
  inst::seq_off(vi, note);
  ++st.notes_off;
}
// The note-ons waiting (of track r, -1 every track) from host frame `from` on (by_step: of the steps from there on):
// dropped with their note-offs. pred: 0 every one, 1 the predicted ones only, 2 all but the predicted.
CORE_COLD void cancel_ons(int r, int64_t from, bool by_step = false, int pred = 0) {
  for (int i = 0; i < nev; ++i) {
    Ev& e = ev[i];
    if (!e.on || (r >= 0 && e.track != r) || (by_step ? e.sf : e.f) < from) continue;
    if ((pred == 1 && !e.pred) || (pred == 2 && e.pred)) continue;
    uint16_t id = e.id;
    for (int j = 0; j < nev; ++j)
      if (ev[j].id == id && !ev[j].on) ev[j] = ev[--nev];
    // (the on itself may have moved: find it again)
    for (int j = 0; j < nev; ++j)
      if (ev[j].id == id && ev[j].on) {
        ev[j] = ev[--nev];
        break;
      }
    ++st.cancelled;
    i = -1;   // the array changed: again from the start (few events)
  }
}
// Every note of track r (-1 all) ended now, the waiting ones dropped.
CORE_COLD void release(int r) {
  for (int i = 0; i < nev;) {
    if (r < 0 || ev[i].track == r) ev[i] = ev[--nev];
    else ++i;
  }
  for (int t = 0; t < kRytmTracks; ++t) {
    if (r >= 0 && t != r) continue;
    int vi = voice_of(t);
    if (vi < 0) continue;
    bool any = false;
    for (int n = 0; n < 128; ++n) any |= sounding[vi][n] != 0;
    if (!any) continue;
    memset(sounding[vi], 0, sizeof sounding[vi]);
    inst::seq_off(vi, -1);
  }
}

// Track r's locks from now (ls null: none, every base back).
CORE_COLD void apply_locks(int r, const LockSet* ls) {
  int vi = r >= 0 && r < kRytmTracks ? voice_of(r) : -1;
  int n = ls ? ls->n : 0;
  if (vi >= 0 && vi < kVoices) {
    VLock& L = vlk[vi];
    V& x = v[vi];
    uint16_t want = 0;
    float nv[16];
    for (int i = 0; i < n; ++i)
      if (ls->l[i].param < 16) want |= (uint16_t)(1u << ls->l[i].param), nv[ls->l[i].param] = lock_float(ls->l[i].value);
    if (want || L.on) {
      float p[16];
      for (int k = 0; k < 16; ++k) p[k] = lfo::base_of(vi, k, x.p[k]);   // what plays without an LFO on it
      for (int k = 0; k < 16; ++k) {
        uint16_t bit = (uint16_t)(1u << k);
        if (want & bit) {
          if (!(L.on & bit)) L.base[k] = p[k];
          L.val[k] = nv[k], p[k] = nv[k];
        } else if (L.on & bit) {
          p[k] = L.base[k];
        }
      }
      L.on = want;
      lfo::voice_params(vi, p, 16, false);   // an LFO on a locked param: the lock is its base
      memcpy(x.p, p, sizeof p);
      voice_params_changed(vi);
      ++st.locks_applied;
    }
  }
  if (vi >= 0 && vi < kVoices) {   // fx-ins: the voice's INSERT params (core/fx/insert.h), owned by the track that locked
    uint8_t iwant = 0, iv[8];
    for (int i = 0; i < n; ++i)
      if (ls->l[i].param >= kLockIns && ls->l[i].param < kLockIns + 8) {
        int j = ls->l[i].param - kLockIns;
        iwant |= (uint8_t)(1u << j), iv[j] = (uint8_t)(ls->l[i].value >> 7);
      }
    for (int j = 0; j < 8; ++j) {
      if (iwant >> j & 1) {
        ilk_owner[vi][j] = (int8_t)r;
        ins::lock(vi, j / 4, j % 4, iv[j]);
        ++st.locks_applied;
      } else if (ilk_owner[vi][j] == r) {
        ilk_owner[vi][j] = -1;
        ins::lock(vi, j / 4, j % 4, -1);
      }
    }
  }
  uint8_t fwant = 0, fv[8];
  for (int i = 0; i < n; ++i)
    if (ls->l[i].param >= kLockFx && ls->l[i].param < kLockIns) {
      int j = ls->l[i].param - kLockFx;
      fwant |= (uint8_t)(1u << j), fv[j] = (uint8_t)(ls->l[i].value >> 7);
    }
  for (int j = 0; j < 8; ++j) {
    uint8_t bit = (uint8_t)(1u << j);
    int sl = j / 4, k = 2 + j % 4;
    if (sl >= fx::kSlots) continue;
    if (fwant & bit) {
      if (!(flk.on & bit)) flk.base[j] = flk.seen & bit ? flk.rep[j] : fx::control.knob[sl][k];
      flk.on |= bit, flk.owner[j] = (int8_t)r, flk.val[j] = fv[j];
      fx::control.knob[sl][k] = fx_play(sl, k, fv[j]);
      ++st.locks_applied;
    } else if ((flk.on & bit) && flk.owner[j] == r) {
      flk.on &= (uint8_t)~bit;
      fx::control.knob[sl][k] = fx_play(sl, k, flk.base[j]);
    }
  }
}
CORE_COLD void unlock_all() {
  for (int r = 0; r < kRytmTracks; ++r) apply_locks(r, nullptr);
  memset(slides, 0, sizeof slides), slide_any = 0;   // X1
}
// X1: a step's lock set applied (an event): its values now, its slides from here. A slide still running on a param the
// new set does not lock goes on (a trig between a slide and its target does not cut it; the target's own lock does).
CORE_COLD void apply_set(int r, const LockSet* ls) {
  if (r < 0 || r >= kRytmTracks) {
    apply_locks(r, ls);
    return;
  }
  LockSet& m = cur_set[r];
  LockSet nw = {};
  if (ls) nw = *ls;
  for (int i = 0; slide_any >> r & 1 && i < m.n; ++i) {
    const Slide& sl = slides[r][i];
    bool locked = false;
    for (int k = 0; k < nw.n; ++k) locked |= nw.l[k].param == m.l[i].param;
    if (!sl.on || locked || nw.n >= kLocks || sl.f1 <= (int64_t)hframes) continue;
    nw.slide |= (uint16_t)(1u << nw.n), nw.to[nw.n] = sl.to, nw.end[nw.n] = sl.f1;
    nw.l[nw.n++] = {m.l[i].param, 0, sl.last};
  }
  memset(slides[r], 0, sizeof slides[r]);
  slide_any &= (uint16_t)~(1u << r);
  m = nw;
  apply_locks(r, &m);
  for (int i = 0; i < m.n; ++i)
    if ((m.slide >> i & 1) && m.end[i] > (int64_t)hframes) {
      slides[r][i] = {1, m.l[i].value, m.to[i], m.l[i].value, (int64_t)hframes, m.end[i]};
      slide_any |= (uint16_t)(1u << r);
    }
}
// X1: every 4 blocks, the running slides' values (the track's set again, through apply_locks: machine params, FX, INSERT)
CORE_COLD void slide_tick() {
  if (!slide_any || (hframes / kBlock) & 3) return;
  for (int r = 0; r < kRytmTracks; ++r) {
    if (!(slide_any >> r & 1)) continue;
    LockSet& ls = cur_set[r];
    bool changed = false, running = false;
    for (int i = 0; i < ls.n; ++i) {
      Slide& sl = slides[r][i];
      if (!sl.on) continue;
      int64_t h = (int64_t)hframes;
      double x = sl.f1 > sl.f0 ? (double)(h - sl.f0) / (double)(sl.f1 - sl.f0) : 1.0;
      if (x >= 1) x = 1, sl.on = 0;
      else running = true;
      uint16_t val = (uint16_t)llround(sl.from + ((double)sl.to - sl.from) * (x < 0 ? 0 : x));
      if (val != sl.last) ls.l[i].value = val, sl.last = val, changed = true;
    }
    if (changed) apply_locks(r, &ls);
    if (!running) slide_any &= (uint16_t)~(1u << r);
  }
}

// ---- the card
// 2026-10-26, the RAM store (phase 2): the project the sequencer's state is of - its files are this one's. Not pool.h's
// project: that is the new name from a switch on, while the new project's store fills and the old project is still the
// one loaded here (its edits go to its own folder until project_changed, from the fill's end, loads the new one).
char g_proj[sizeof project] = "";
CORE_COLD void folder_of(const char* proj, char* fn, int cap) {
  char n[kKeyMax];
  project_key(proj, n, sizeof n);
  sniprintf(fn, cap, CORE_STR("/.rytm/seq/%s"), n);
}
CORE_COLD void folder(char* fn, int cap) { folder_of(g_proj, fn, cap); }
CORE_COLD void file_ext_of(const char* proj, char* fn, int cap, int index, const char* ext) {
  char d[64];
  folder_of(proj, d, sizeof d);
  sniprintf(fn, cap, CORE_STR("%s/p%03d.%s"), d, index, ext);
}
CORE_COLD void file_ext(char* fn, int cap, int index, const char* ext) { file_ext_of(g_proj, fn, cap, index, ext); }
CORE_COLD void file_of(char* fn, int cap, int index) { file_ext(fn, cap, index, "sqp"); }
// 1042 (data safety): a pattern whose file did not load - a newer Sidecar's version, a bad CRC, cut short, unreadable
// - is locked until the project changes or the file loads: its edits stay in memory and save_pattern refuses to write
// it, so the original is never replaced by an empty pattern plus the edit (before, both callers ignored the failed
// load and the next save did exactly that). Keyed by pattern index (the save buffer and the prefetch copy the pattern).
// 2026-10-26, the RAM store (Task 5 fix round 1): 3 - the project's store is still filling (kLoading below): an edit
// on the pattern is refused at once (undone, logged, shown), never kept in memory and dropped when the load ends
uint8_t pat_lock[128];
constexpr uint8_t kLockLoading = 3;
CORE_COLD int lock_of(int index) { return index >= 0 && index < 128 ? pat_lock[index] : 0; }
// While a project's store fills (core/pool/pool.h project_fill_begin -> loading_begin, until project_changed at its
// end), the Rytm is on that project but the sequencer cannot have its files yet: an edit of the loading project's
// tracks.txt settings (the mask, the latency, POLY CONFIG's defaults, ...) or of a pattern loaded meanwhile is refused
// up front with that reason (the log, the SEQ page's footer, st.loading_refused) - before, it was taken, then
// overwritten by the load or refused at the save with "damaged". The pattern still loaded from the old project (the
// one playing when the Rytm switched) stays editable: its edits go to the old project's files before the re-key.
bool g_loading = false;
struct TracksWas {   // the tracks.txt settings when the loading began: a refused edit puts them back
  uint16_t mask, pin_extra;
  int latency, quant;
  bool pin_seq, manual;
  PolyCfg poly[kRytmTracks];
} g_tracks_was;
CORE_COLD void tracks_keep() {
  g_tracks_was.mask = legacy_mask, g_tracks_was.pin_extra = pin_extra, g_tracks_was.latency = latency;
  g_tracks_was.quant = quant, g_tracks_was.pin_seq = pin_seq, g_tracks_was.manual = cal.manual;
  memcpy(g_tracks_was.poly, poly_def, sizeof poly_def);
}
CORE_COLD void refused_log(const char* what) {
  ++st.loading_refused;
  log(CORE_STR("seq: %s refused: project %s is still loading (try again once it is)"), what, project);
}
CORE_COLD bool tracks_refused() {
  if (!g_loading) return false;
  legacy_mask = g_tracks_was.mask, pin_extra = g_tracks_was.pin_extra, latency = g_tracks_was.latency;
  quant = g_tracks_was.quant, pin_seq = g_tracks_was.pin_seq, cal.manual = g_tracks_was.manual;
  memcpy(poly_def, g_tracks_was.poly, sizeof poly_def);
  refused_log("a track setting");
  return true;
}
// fix1049: idle()'s saves (tracks.txt once the edits settled, a pattern's working copy) posted to the card queue: the
// Pi's control core (core 3) renders a block only between its passes and its FIFO holds 2 ms, and a card write there
// is a dropout (a POLY CONFIG knob's steps wrote tracks.txt each, ~20 ms of card time each on the card model: heard as
// the playing note retriggering while the knob turned). The card core (core 0) writes it, fmt::write_atomic's
// .tmp / .prev.
//
// 2026-10-26, the RAM store (phase 2): with CORE_MSAMP every pattern file (.sqp, .sqs, .prv) and tracks.txt is read
// from the store a project load fills (core/store/store.h: the files this project wants, want_files) and written into
// it (write-through, while the store is this project's) and through fmt::post_queue (the card core writes it, never a
// wait here: one queued write per path, a newer one in its place). So the control core never touches the card for
// the sequencer: a prefetch or a switch is a copy from RAM (it read the .sqp from the card: ~14 ms at every switch on
// the device), a save a copy into RAM and the queue. The store's fill never reads a path whose write is still queued.
#if CORE_MSAMP
// the store is this project's (the one a load filled, or is filling): the sequencer's writes go into it too
CORE_COLD bool store_ours() { return !strcmp(g_proj, project); }
// a file's bytes to the card (fmt::post_queue: queued behind the writes ahead, never waited for), into the store first
CORE_COLD bool post_file(const char* fn, const uint8_t* d, uint32_t n) {
  if (store_ours()) store::put(fn, d, n);   // (an old project's file saved while the next one's fills: not in it)
  return fmt::post_queue(fn, (const char*)d, n);
}
#endif
CORE_COLD bool load_into(Pattern& p, int index) {
  clear(p, index);
  if (!card_ok || index < 0) return true;
  char fn[96];
  file_of(fn, sizeof fn, index);
#if CORE_MSAMP
  const uint8_t* d;
  uint32_t len;
  store::State sst = store::get(fn, &d, &len);
  if (sst == store::kAbsent) return true;   // none yet: empty
  if (sst != store::kPresent) {
    // kLocked: there but not readable; kUnknown: not in the store (the project still loading, or a store full): never
    // taken for empty - 1042: never written over
    bool loading = sst == store::kUnknown && g_loading;
    if (index < 128) pat_lock[index] = loading ? kLockLoading : 2;
    if (loading) {   // (not a failed load: the project's own files come at the fill's end)
      log(CORE_STR("seq: pattern %c%02d: not editable until project %s is loaded"), 'A' + index / 16 % 8,
          index % 16 + 1, project);
      return false;
    }
    ++st.load_fails;
    log(CORE_STR("seq: %s %s: not editable"), fn, sst == store::kLocked ? "cannot be read" : "is not in the store");
    return false;
  }
  int n = (int)len;
  const uint8_t* buf = d;
#else
  File f;
  bool opened = f.open(fn, kRead);
  // fix1049 / 0.3.0: a cut between write_atomic's renames (the card core's posted save, or the sync one)
  if (!opened) fmt::recover(fn), opened = f.open(fn, kRead);
  if (!opened) {
    if (!cards.sd().exists(fn)) return true;   // none yet: empty
    if (index < 128) pat_lock[index] = 2;      // 1042: there but not readable now: never written over
    ++st.load_fails;
    log(CORE_STR("seq: %s cannot be read: not editable"), fn);
    return false;
  }
  static uint8_t* buf = nullptr;
  if (!buf) buf = new uint8_t[pack_bound()];
  int n = (int)f.read(buf, (size_t)pack_bound());
  f.close();
#endif
  ++st.loads;
  if (!unpack(p, index, buf, n)) {
    bool newer = unpack_error() == kUnpackNewer;   // E2: a newer version, or a newer Sidecar's required block
    if (index < 128) pat_lock[index] = newer ? 1 : 2;
    ++st.load_fails;
    log(CORE_STR("seq: pattern %c%02d was %s: not editable (%s kept as it is)"), 'A' + index / 16 % 8, index % 16 + 1,
        newer ? "saved by a newer Sidecar" : "damaged", fn);
    clear(p, index);
    return false;
  }
  if (index < 128) pat_lock[index] = 0;
  if (unpack_unkept())
    log(CORE_STR("seq: %s: %d blocks of a newer Sidecar too big to keep (lost at the next save)"), fn, unpack_unkept());
  return true;
}
CORE_COLD bool write_pattern(const Pattern& p, const char* ext, bool posted = false);
CORE_COLD bool save_pattern(const Pattern& p, bool posted = false) {
  if (!card_ok || p.index < 0) return false;
  if (lock_of(p.index)) {   // 1042: its file did not load: never written over
    ++st.save_refused;
    log(CORE_STR("seq: pattern %c%02d not saved: its file is locked (%s)"), 'A' + p.index / 16 % 8, p.index % 16 + 1,
          lock_of(p.index) == 1 ? "a newer Sidecar's" : lock_of(p.index) == kLockLoading ? "its project still loading" : "damaged");
    return false;
  }
  return write_pattern(p, "sqp", posted);
}
// the pattern packed to pNNN.<ext> (fmt::write_atomic): sqp the working copy, sqs the saved state (1042), prv the
// working copy a reload replaced (1042)
CORE_COLD bool write_pattern(const Pattern& p, const char* ext, bool posted) {
  static uint8_t* buf = nullptr;
  if (!buf) buf = new uint8_t[pack_bound()];
  char fn[96];
  (void)posted;
  int n = pack(p, buf, pack_bound());
  file_ext(fn, sizeof fn, p.index, ext);
#if CORE_MSAMP
  // the RAM store (phase 2): every save posted, idle()'s and the switch's alike (into the store, then queued for the
  // card core: write_atomic there); a file the store holds as locked is never written over (1042: a .sqs or .prv that
  // did not read; a locked .sqp is refused by save_pattern already)
  const uint8_t* had;
  uint32_t hn;
  if (store_ours() && store::get(fn, &had, &hn) == store::kLocked) {
    ++st.save_refused;
    log(CORE_STR("seq: %s not written: it is there but could not be read (kept as it is)"), fn);
    return false;
  }
  bool ok = n > 0 && post_file(fn, buf, (uint32_t)n);
#else
  // 0.3.0 power-cut safety: the sync save goes through write_atomic, as the posted one already did. Before, the old
  // file was removed and the .tmp renamed over it: a cut in between left no pattern at all. (write_atomic reads the
  // .tmp back, keeps the old one as .prev until the new one is in place, and makes /.rytm/seq/<project> itself.)
  bool ok = n > 0 && fmt::write_atomic(fn, buf, (uint32_t)n);
#endif
  if (ok) ++st.saves;
  else ++st.save_fails, log(CORE_STR("seq: cannot write %s"), fn);
  return ok;
}
CORE_COLD int clampi(int v, int lo, int hi);
bool tracks_locked = false;   // 1042: tracks.txt there but not readable: not written over until the next project load
CORE_COLD void load_tracks() {
  legacy_mask = 0, pin_extra = 0, pin_seq = true, cal.manual = false;
  tracks_locked = false;
  memset(poly_def, 0, sizeof poly_def);
  if (!card_ok) return;
  char d[64], fn[96], b[512];
  folder(d, sizeof d);
  sniprintf(fn, sizeof fn, CORE_STR("%s/tracks.txt"), d);
#if CORE_MSAMP
  int n;
  {   // the RAM store (phase 2): no card call on this core
    const uint8_t* td;
    uint32_t tn;
    store::State sst = store::get(fn, &td, &tn);
    if (sst == store::kAbsent) return;   // none: the defaults
    if (sst != store::kPresent) {        // there but unreadable, or not in the store: never written over
      tracks_locked = true;
      log(CORE_STR("seq: %s %s: kept as it is (not written)"), fn, sst == store::kLocked ? "cannot be read" : "is not in the store");
      return;
    }
    n = tn < sizeof b - 1 ? (int)tn : (int)sizeof b - 1;   // (as the card read: its first 511 bytes)
    memcpy(b, td, (size_t)n);
  }
#else
  // fix1049 / 0.3.0: a cut inside write_atomic (the card queue's posted write, or the sync one): .tmp / .prev
  fmt::recover(fn);
  File f;
  if (!f.open(fn, kRead)) {
    tracks_locked = cards.sd().exists(fn);
    if (tracks_locked) log(CORE_STR("seq: %s cannot be read: kept as it is (not written)"), fn);
    return;
  }
  int n = (int)f.read(b, sizeof b - 1);
  f.close();
#endif
  b[n < 0 ? 0 : n] = 0;
  const char* m = strstr(b, "mask=");
  if (m) {   // 0x... (no strtol on the Pi)
    unsigned v = 0;
    for (const char* c = m + 5 + (m[5] == '0' && (m[6] == 'x' || m[6] == 'X') ? 2 : 0); *c; ++c) {
      int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
      if (d < 0) break;
      v = v * 16 + (unsigned)d;
    }
    legacy_mask = (uint16_t)(v & 0xFFF);   // (per pattern poly: an older build's; membership is the pattern's)
  }
  const char* l = strstr(b, "lat=");
  if (l) latency = atoi(l + 4);
  const char* q = strstr(b, "quant=");
  if (q) quant = atoi(q + 6) ? 1 : 0;
  const char* pm = strstr(b, "pin=");
  if (pm) {
    unsigned v = 0;
    for (const char* c = pm + 4 + (pm[4] == '0' && (pm[5] == 'x' || pm[5] == 'X') ? 2 : 0); *c; ++c) {
      int d = *c >= '0' && *c <= '9' ? *c - '0' : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10 : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10 : -1;
      if (d < 0) break;
      v = v * 16 + (unsigned)d;
    }
    pin_extra = (uint16_t)(v & 0xFFF);
  }
  const char* ps = strstr(b, "pinseq=");
  if (ps) pin_seq = atoi(ps + 7) != 0;
  const char* lm = strstr(b, "latset=");
  if (lm) cal.manual = atoi(lm + 7) != 0;
  for (const char* q = strstr(b, "poly="); q; q = strstr(q + 5, "poly=")) {   // E0: poly=T:V,A,U,D,S,G (T 1..12)
    int a[7] = {0}, k = 0;
    const char* c = q + 5;
    for (; k < 7 && *c >= '0' && *c <= '9'; ++k) {
      a[k] = atoi(c);
      while (*c >= '0' && *c <= '9') ++c;
      if (*c != ':' && *c != ',') { ++k; break; }
      ++c;
    }
    int r = a[0] - 1;
    if (k < 7 || r < 0 || r >= kRytmTracks) continue;
    PolyCfg& pc = poly_def[r];
    pc.voices = (uint8_t)clampi(a[1], 0, kMaxVoicesCfg), pc.alloc = (uint8_t)(a[2] < kAllocs ? a[2] : 0);
    pc.unison = (uint8_t)clampi(a[3], 0, kMaxUnison), pc.detune = (uint8_t)clampi(a[4], 0, 127);
    pc.spread = (uint8_t)clampi(a[5], 0, 127), pc.group = (uint8_t)clampi(a[6], 0, kPolyGroups);
  }
}
// tracks.txt's text (the mask, the latency, the pins, E0's default poly configs)
CORE_COLD int tracks_text(char* b, int cap) {
  int n = sniprintf(b, cap, CORE_STR("mask=0x%03x\nlat=%d\nquant=%d\npin=0x%03x\npinseq=%d\nlatset=%d\n"), legacy_mask, latency,
                    quant, pin_extra, pin_seq ? 1 : 0, cal.manual ? 1 : 0);
  for (int r = 0; r < kRytmTracks && n < cap - 40; ++r) {   // E0: the tracks' default poly configs (none: no line)
    const PolyCfg& pc = poly_def[r];
    if (!poly_default(pc))
      n += sniprintf(b + n, cap - n, CORE_STR("poly=%d:%d,%d,%d,%d,%d,%d\n"), r + 1, pc.voices, pc.alloc, pc.unison,
                     pc.detune, pc.spread, pc.group);
  }
  return n;
}
CORE_COLD void save_tracks(bool posted = false) {
  if (!card_ok) return;
  char d[64], fn[96], b[512];
  folder(d, sizeof d);
  sniprintf(fn, sizeof fn, CORE_STR("%s/tracks.txt"), d);
  if (tracks_locked) return;   // 1042
  (void)posted;
  int n = tracks_text(b, sizeof b);
#if CORE_MSAMP
  // the RAM store (phase 2): idle()'s and a switch's alike, into the store and queued for the card core (never a wait)
  if (n > 0 && !post_file(fn, (const uint8_t*)b, (uint32_t)n)) log(CORE_STR("seq: cannot write %s"), fn);
#else
  // 0.3.0 power-cut safety: as write_pattern's sync path, through fmt::write_atomic (it reads the .tmp back and keeps
  // the old tracks.txt as .prev until the new one is in place, and makes /.rytm/seq/<project> itself). Before, the old
  // file was removed and the .tmp renamed over it: a cut in between left no POLY CONFIG for the project at all.
  if (n > 0) fmt::write_atomic(fn, b, (uint32_t)n);
#endif
}

CORE_COLD void remask(bool rel = false);
// fix1028 review: the poly track's menu, its OFF and RESTORE questions are about a track of one pattern - another
// pattern (a switch, a load) or the track no poly track now (the console, POLY CONFIG, a Rytm machine) drops them, so
// their YES / NO never acts on another pattern's track
CORE_COLD void prompt_drop_stale() {
  if (ui.prompt != kPromptMenu && ui.prompt != kPromptOff && ui.prompt != kPromptRestore) return;
  if (cur && ui.prompt_pat == cur->index && in_mask(ui.prompt_track)) return;
  log(CORE_STR("seq: the SEQ page's question on track %d dropped (its pattern or poly track gone)"), ui.prompt_track + 1);
  ui.prompt = 0;
}
// the undos of pattern index (all of them: -1) forgotten - the Rytm's UNDO then finds nothing to undo
CORE_COLD void disarm_undo(int index) {
  if (undo_p && (index < 0 || undo_p->index == index)) undo_p->index = -1, undo_p_kind = 0;
  if (index < 0 || undo_t_pat == index) undo_t_pat = -1, undo_t_kind = 0, undo_t_track = -1;
  if (index < 0 || undo_g_pat == index) undo_g_pat = -1, undo_g_kind = 0, undo_g_track = -1;
  if (index < 0 || undo_r_pat == index) undo_r_pat = -1, undo_r_kind = 0, undo_r_track = -1;
}
CORE_COLD void mark_dirty() {
  if (cur && lock_of(cur->index) == kLockLoading) {   // a pattern of the loading project: the edit undone at once
    clear(*cur, cur->index);   // (all it held: load_into left it empty)
    remask(true);
    disarm_undo(cur->index);   // round 2: a refused paste / clear leaves no undo (it would undo onto the real pattern)
    refused_log("a pattern edit");
    return;
  }
  dirty = true, dirty_ms = now_ms();
  ++st.edits;
}

// The pattern `index` becomes the current one (the prefetched copy when it is that one); the old one's unsaved edits
// go to `sav` for idle().
CORE_COLD void install(int index) {
  if (cur->index == index && pat_loaded == index) return;
  ne_cover_outside();   // round 3 (NB-3): an outside edit after a NOTE EDIT gesture becomes a level before its pattern goes
  int was_lock = lock_of(index);   // (the loaded project's lock of it, before a fill's stand-in replaces it: below)
  if (dirty && cur->index >= 0) {
    if (sav_pending) save_pattern(*sav);   // (two switches inside a second: the older one written now)
    memcpy(sav, cur, sizeof *sav);
    sav_pending = true;
    dirty = false;
  }
  if (nxt->index == index) {
    Pattern* t = cur;
    cur = nxt, nxt = t;
    nxt->index = -1;
    ++st.prefetched;
  } else {
    load_into(*cur, index);
  }
  pat_loaded = index;
  if (sav_pending && sav->index == index) {   // back to it
    memcpy(cur, sav, sizeof *cur), sav_pending = false, dirty = true;
    // (the loaded project's pattern again, while the next one loads: saved into its own folder - g_proj still names
    // it - with its own lock: load_into just set the loading project's kLockLoading on the index; a locked file of the
    // old project (1042: damaged, a newer Sidecar's) stays locked, never written over)
    if (lock_of(index) == kLockLoading) pat_lock[index] = was_lock == kLockLoading ? 0 : was_lock;
  }
  for (Track& t : cur->t)
    for (Step& s : t.step)
      for (int k = 0; k < s.n; ++k) s.note[k].fresh = 0;
  remask();   // per pattern poly: the seq tracks are this pattern's (its slots that are on)
  prompt_drop_stale();   // fix1028 review
}

// ---- rui1040: trig conditions (seq.h kCond*)
bool cond_last[kRytmTracks];   // each track's last condition result (PRE; NEI reads the track before's)
// draw: E1's keyed random bits for this step's probability (rng_key(.., kRngProb)): the same answer however often the
// step is planned (a replan, a predicted switch), the same on every PLAY with a fixed seed
CORE_COLD bool cond_pass_i(const Step& s, int r, int64_t iter, uint32_t draw) {
  int c = cond_of(s), a = 0, b = 0;
  bool prob = s.prob && s.prob < 100;
  if (!c && !prob) return true;
  bool ok = true, sets = true;
  switch (c) {
    case kCondNone: break;
    case kCondFill: ok = fill; break;
    case kCondNotFill: ok = !fill; break;
    case kCondPre: ok = r >= 0 && r < kRytmTracks && cond_last[r], sets = false; break;
    case kCondNotPre: ok = !(r >= 0 && r < kRytmTracks && cond_last[r]), sets = false; break;
    case kCondNei: ok = r > 0 && r <= kRytmTracks && cond_last[r - 1], sets = false; break;
    case kCondNotNei: ok = !(r > 0 && r <= kRytmTracks && cond_last[r - 1]), sets = false; break;
    case kCond1st: ok = iter == 0; break;
    case kCondNot1st: ok = iter != 0; break;
    default:
      if (cond_ab(c, &a, &b)) ok = iter >= 0 && (int)(iter % b) == a - 1;
      break;
  }
  if (ok && prob) {
    ok = rng_chance(draw, s.prob);
    sets = true;
  }
  if (sets && r >= 0 && r < kRytmTracks) cond_last[r] = ok;
  return ok;
}
// seqtop V1: a lock's own condition at its step (FILL, 1ST, A:B on the step's loop, the probability from the keyed RNG)
CORE_COLD bool lock_pass(const Lock& l, int r, const StepAt& at, uint32_t draw) {
  int c = l.cond, a = 0, b = 0;
  (void)r;
  if (!c) return true;
  if (c >= kConds) return rng_chance(draw, c - kConds + 1);
  switch (c) {
    case kCondFill: return fill;
    case kCondNotFill: return !fill;
    case kCond1st: return at.loop == 0;
    case kCondNot1st: return at.loop != 0;
    default: return cond_ab(c, &a, &b) && at.loop >= 0 && (int)(at.loop % b) == a - 1;
  }
}
CORE_COLD int cond_knob_i(const Step& s) {
  int c = cond_of(s);
  if (c) return c;
  int p = s.prob ? s.prob : 100;
  return p >= 100 ? 0 : kConds + p - 1;
}
CORE_COLD void cond_knob_set_i(Step& s, int v) {
  v = v < 0 ? 0 : v > kCondKnob - 1 ? kCondKnob - 1 : v;
  if (v < kConds) cond_set(s, v), s.prob = 100;
  else cond_set(s, kCondNone), s.prob = (uint8_t)(v - kConds + 1);
}
CORE_COLD const char* cond_text_i(const Step& s, char* b, int cap) {
  static const char* const kNames[9] = {"---", "FILL", "!FILL", "PRE", "!PRE", "NEI", "!NEI", "1ST", "!1ST"};
  int c = cond_of(s), a = 0, bb = 0;
  if (cond_ab(c, &a, &bb)) sniprintf(b, cap, CORE_STR("%d:%d"), a, bb);
  else if (c) sniprintf(b, cap, "%s", kNames[c]);
  else if (s.prob && s.prob < 100) sniprintf(b, cap, CORE_STR("%d%%"), s.prob);
  else sniprintf(b, cap, "%s", kNames[0]);
  return b;
}

// ---- the planner
// E1: track t's seed in pattern p (its own, the pattern's fixed one, else FREE's of this PLAY)
CORE_COLD uint32_t eff_seed(const Pattern& p, const Track& t) {
  return t.seed_own ? t.seed : p.seed_fixed ? p.seed : free_seed;
}
// seqtop R1b: SEED MOD. The offset is sampled once per QUANT unit (a step, a loop, a bar of 16 steps; ORDER always per
// loop) and kept in a small history keyed by (the play segment, the pattern, the unit), so a step planned twice - a
// replan, the prediction's plan kept by its switch - gets the offset it got the first time. A segment is one stretch of
// one pattern (restart_all); the predicted pattern's steps are planned in the next one.
struct SmodHist {
  bool used;
  uint32_t gen;
  int pat;
  int64_t key;
  int16_t off;
};
SmodHist smh[kRytmTracks][8];   // the STEP / BAR units
SmodHist sml[kRytmTracks][4];   // the loops' (review: in one ring with the steps, a loop's offset was pushed out by 8 steps
int smh_w[kRytmTracks], sml_w[kRytmTracks];   // and sampled again mid-loop)
int smod_last[kRytmTracks];   // the last offset (the hysteresis' reference, the display), -1 none
uint32_t seg_gen = 0;
CORE_COLD void smod_reset() {
  memset(smh, 0, sizeof smh), memset(smh_w, 0, sizeof smh_w), memset(sml, 0, sizeof sml), memset(sml_w, 0, sizeof sml_w);
  for (int& x : smod_last) x = -1;
}
CORE_COLD int smod_sample(const Track& t, int r) {   // the source now -> a whole offset 0..RANGE-1, with hysteresis
  int vi = voice_of(r), range = t.smod_range < 2 ? 2 : t.smod_range;
  if (vi < 0 || vi >= kVoices || t.smod_src < 1 || t.smod_src > 16) return 0;
  float raw = v[vi].p[t.smod_src - 1] / 128.0f * (float)range;
  int last = smod_last[r], off = (int)raw;
  if (last >= 0 && last < range && raw >= (float)last - 0.25f && raw < (float)last + 1.25f) off = last;
  off = clampi(off, 0, range - 1);
  smod_last[r] = off;
  return off;
}
// level: 0 notes, 1 chance, 2 order; sample false: only what was sampled (the playhead, the LEDs), else the last offset
CORE_COLD int smod_off(const Pattern& p, const Track& t, int r, int64_t v_, int level, bool pred, bool sample) {
  if (!t.smod_src || level > (t.smod_aff >= kAffOrder ? 2 : t.smod_aff) || r < 0 || r >= kRytmTracks) return 0;
  int64_t cyc = dir_cycle(eff_len(&t, r), t.dir), key;
  bool lp = level == 2 || t.smod_quant == kSqLoop;
  if (lp) key = v_ / cyc * 4 + 1;
  else key = t.smod_quant == kSqStep ? v_ * 4 : v_ / 16 * 4 + 2;
  uint32_t g = seg_gen + (pred ? 1 : 0);
  SmodHist* ring = lp ? sml[r] : smh[r];
  int nr = lp ? 4 : 8;
  for (int i = 0; i < nr; ++i)
    if (ring[i].used && ring[i].gen == g && ring[i].pat == p.index && ring[i].key == key) return ring[i].off;
  if (!sample) return smod_last[r] > 0 ? smod_last[r] : 0;
  int off = smod_sample(t, r);
  int& w = lp ? sml_w[r] : smh_w[r];
  ring[w++ % nr] = {true, g, p.index, key, (int16_t)off};
  return off;
}
CORE_COLD uint32_t seed_of(const Pattern& p, const Track& t, int r, int64_t v_, int level, bool pred, bool sample) {
  return eff_seed(p, t) + (uint32_t)smod_off(p, t, r, v_, level, pred, sample);
}
// ---- seqtop H1 / V3: the FX column's order commands, through the step map. The walk is a pure function of the track,
// the seed and the count: it starts again at each loop's start (step 1 of the track's direction) and walks the steps to
// the count, each step's JMP / JRN / LOP / SKP / DIR deciding the next, the direction's own rule otherwise (the same
// rules as step_for, so a track without order commands maps exactly as step_for). One cached walk per track and kind
// (playing / predicted) makes the planner's next count one step of work; any edit, load or new segment starts it again.
CORE_COLD bool has_order_scan(const Track& t) {
  for (const Step& s : t.step)
    if (fx_order(s.fx[0].cmd) || fx_order(s.fx[1].cmd)) return true;
  return false;
}
// (review: a scan of 256 commands at every map_step, up to len times a step for a slide's target - cached per track,
// any edit or load scans again)
struct OrdCache {
  const Track* t;
  int pat;
  uint32_t edits, loads;
  bool v;
};
OrdCache ocache[2 * kTracks];
CORE_COLD bool has_order(const Track& t, const Pattern& p) {
  int i = (int)(&t - p.t);
  if (i < 0 || i >= kTracks) return has_order_scan(t);
  OrdCache& c = ocache[(&p == cur ? 0 : kTracks) + i];
  if (c.t != &t || c.pat != p.index || c.edits != st.edits || c.loads != st.loads)
    c = {&t, p.index, st.edits, st.loads, has_order_scan(t)};
  return c.v;
}
constexpr int kLops = 4;   // LOP commands counting at once (review: one counter made two overlapping LOPs loop forever)
struct WState {
  int idx;
  uint8_t dir;
  int8_t up;
  int8_t lop_at[kLops];
  uint8_t lop_left[kLops];
};
struct WCache {
  bool valid;
  const Pattern* p;
  int pat, len, dir0;
  uint32_t edits, loads, gen, seed;
  int64_t v;
  WState s;
};
WCache wcache[kRytmTracks][3];   // the plan's, the predicted plan's, the slide search's (its own: it walks ahead)
CORE_COLD int w_adv(int idx, int dir, int8_t* up, int len, uint32_t sd, int r, int pos, int64_t key, int k) {
  switch (dir) {
    case kDirRev: return (idx - 1 + len) % len;
    case kDirPend: {
      if (len < 2) return 0;
      int nx = idx + *up;
      if (nx >= len) *up = -1, nx = len - 2;
      if (nx < 0) *up = 1, nx = 1;
      return nx;
    }
    case kDirPing: {
      int nx = idx + *up;
      if (nx >= len) *up = -1, nx = len - 1;
      if (nx < 0) *up = 1, nx = 0;
      return nx;
    }
    case kDirRnd: return rng_below(rng_key(sd, r, pos, key, kRngDir, k), len);
    case kDirBrown: return ((idx + brown_delta(rng_key(sd, r, pos, key, kRngDir, k))) % len + len) % len;
    default: return (idx + 1) % len;
  }
}
CORE_COLD StepAt walk_step(const Pattern& p, const Track& t, int r, int64_t v_, uint32_t sd, int slot) {
  int len = eff_len(&t, r), cyc = dir_cycle(len, t.dir);
  if (v_ < 0) v_ = 0;
  int64_t loop = v_ / cyc, v0 = loop * cyc, key = t.seed_same ? -1 : loop;
  WCache& c = wcache[r][slot];
  WState s;
  int64_t x;
  if (c.valid && c.p == &p && c.pat == p.index && c.len == len && c.dir0 == t.dir && c.edits == st.edits &&
      c.loads == st.loads && c.gen == seg_gen && c.seed == sd && c.v <= v_ && c.v >= v0) {
    s = c.s, x = c.v;
  } else {
    s = {t.dir == kDirRev ? len - 1 : t.dir == kDirRnd ? rng_below(rng_key(sd, r, 0, key, kRngDir), len) : 0, t.dir, 1,
         {-1, -1, -1, -1}, {0, 0, 0, 0}};
    x = v0;
  }
  for (; x < v_; ++x) {   // from the step at count x to x + 1
    const Step& st_ = t.step[s.idx];
    int next = -1, skip = 0;
    for (int col = 0; col < 2; ++col) {
      const Fx& f = st_.fx[col];
      const Fx& o = st_.fx[1 - col];
      if (!fx_order(f.cmd)) continue;
      // (the draws keyed by the position in the loop, not the step: a step visited twice in a loop draws again)
      int px = (int)(x % cyc);
      if (o.cmd == kFxChn && !rng_chance(rng_key(sd, r, px, key, kRngFx, 8 + col), o.val)) continue;
      switch (f.cmd) {
        case kFxDir: s.dir = f.val < kDirs ? f.val : 0, s.up = 1; break;
        case kFxJmp: next = f.val >= 1 && f.val <= len ? f.val - 1 : next; break;
        case kFxJrn: {
          int w = f.val;
          uint32_t d = rng_key(sd, r, px, key, kRngJump, col);
          next = !w || 2 * w + 1 >= len ? rng_below(d, len) : ((s.idx + rng_below(d, 2 * w + 1) - w) % len + len) % len;
          break;
        }
        case kFxSkp: skip = f.val; break;
        case kFxLop: {
          int n = (f.val & 15) + 1, times = (f.val >> 4) + 1, j = -1;
          for (int q = 0; q < kLops && j < 0; ++q) j = s.lop_at[q] == s.idx ? q : -1;
          for (int q = 0; q < kLops && j < 0; ++q)   // a LOP met anew: its own counter
            if (s.lop_at[q] < 0) j = q, s.lop_at[q] = (int8_t)s.idx, s.lop_left[q] = (uint8_t)times;
          if (j < 0) break;   // (more LOPs counting at once than kLops: this one plays through)
          if (s.lop_left[j]) {
            --s.lop_left[j];
            int back = (n - 1) * (s.dir == kDirRev ? -1 : (s.dir == kDirPend || s.dir == kDirPing) ? s.up : 1);
            next = ((s.idx - back) % len + len) % len;
          } else {
            s.lop_at[j] = -1;   // done: the next time it is met it loops again
          }
          break;
        }
        default: break;
      }
    }
    if (next >= 0) {
      s.idx = next;
      continue;
    }
    int pos = (int)((x + 1) % cyc);
    for (int k = 0; k <= skip; ++k) s.idx = w_adv(s.idx, s.dir, &s.up, len, sd, r, pos, key, k);
  }
  c = {true, &p, p.index, len, t.dir, st.edits, st.loads, seg_gen, sd, x, s};
  return {s.idx, loop};
}
// seqtop: the step that plays at the track's step count v (P1: its direction; H1: its jumps) and the loop it is in (E1's
// step map; FWD: v mod len, loop v / len, as before)
// slot: the walk cache to use (0 the plan's, 1 the predicted plan's, 2 the slide search's); BROWN walks too (its
// step_for walks from the loop's start every time: the cache makes the next count one step)
CORE_COLD StepAt map_step(const Pattern& p, const Track* t, int r, int64_t v_, bool pred = false, bool sample = false,
                          int slot = -1) {
  int len = eff_len(t, r);
  if (!t) return step_for(len, v_);
  bool ord = has_order(*t, p);
  uint32_t sd = ord || t->dir == kDirRnd || t->dir == kDirBrown ? seed_of(p, *t, r, v_, 2, pred, sample) : 0;
  if ((ord || t->dir == kDirBrown) && r >= 0 && r < kRytmTracks)
    return walk_step(p, *t, r, v_, sd, slot >= 0 ? slot : pred ? 1 : 0);
  return step_for(len, v_, t->dir, sd, r, t->seed_same != 0);
}
// X1: the next step after count a (in play order, less than a loop on) that locks param: how many steps on, its value
// (review: one walk ahead per planned step, its steps listed once in ahead[], not a search per sliding lock)
uint8_t ahead[kMaxSteps];
int ahead_n = 0;
CORE_COLD void slide_ahead(const Pattern& p, const Track& t, int r, int64_t a, bool pred) {
  int len = eff_len(&t, r);
  ahead_n = 0;
  for (int k = 1; k < len; ++k) ahead[ahead_n++] = (uint8_t)map_step(p, &t, r, a + k, pred, false, 2).index;
}
CORE_COLD bool slide_target(const Track& t, int param, int64_t* j, uint16_t* to) {
  for (int k = 0; k < ahead_n; ++k) {
    int x = lock_of(t.step[ahead[k]], param);
    if (x >= 0) {
      *j = k + 1, *to = (uint16_t)x;
      return true;
    }
  }
  return false;
}
CORE_COLD void plan_step(const Pattern& p, Track& t, int r, int64_t a, double fs, double per, bool pred) {
  StepAt at = map_step(p, &t, r, a, pred, true);
  int idx = at.index;
  int64_t lk = t.seed_same ? -1 : at.loop;
  Step* sp = &t.step[idx];
  // H1: the FX column. A column runs unless the other column's CHN fails (its chance, from the keyed RNG); FIL p: with
  // FILL held this step plays page p's step instead (that step's own commands then)
  auto fx_on = [&](const Step& x, int col) {
    const Fx& o = x.fx[1 - col];
    return o.cmd != kFxChn || rng_chance(rng_key(seed_of(p, t, r, a, 1, pred, true), r, idx, lk, kRngFx, 8 + col), o.val);
  };
  for (int col = 0; fill && col < 2; ++col)
    if (sp->fx[col].cmd == kFxFil && fx_on(*sp, col) && sp->fx[col].val >= 1) {
      int i2 = (sp->fx[col].val - 1) * kStepsPerPage + idx % kStepsPerPage;
      if (i2 < kMaxSteps) sp = &t.step[i2];
      break;
    }
  Step& s = *sp;
  bool notes = s.n && (s.flags & 1);
  if ((!notes && !s.nl) || (s.flags & 2) || (silenced_now() >> r & 1)) return;
  // rui1040: the trig condition (probability, FILL, PRE, NEI, 1ST, A:B); E1: the probability from the keyed RNG
  if (!cond_pass_i(s, r, at.loop, rng_key(seed_of(p, t, r, a, 1, pred, true), r, idx, lk, kRngProb))) return;
  // R1: the step's notes drawn in the pattern's scale (TRACK: every step; STEPS: the flagged ones)
  uint8_t rp[kNotes];
  bool rnd = notes && (t.rnd_mode == kRndTrack || (t.rnd_mode == kRndSteps && (s.xf & kXfRnd)));
  if (rnd) {
    uint8_t wr[kNotes];
    for (int k = 0; k < s.n; ++k) wr[k] = s.note[k].pitch;
    rnd_notes(s.n, seed_of(p, t, r, a, 0, pred, true), r, idx, lk, p.key, p.scale, t.rnd_low, t.rnd_oct, t.rnd_wght,
              t.rnd_wght == kWChord ? chord_pcs(s, p.key, p.scale) : 0, wr, rp);
  }
  int vi = voice_of(r);
  if (vi < 0) return;
  double fs0 = fs;   // the step's place on the grid (X1: a slide's target is counted from it)
  // swing: every second step late; P1 (Q3): under another direction the tick's, so the groove stays
  bool ord = has_order(t, p);   // (H1's jumps: the tick's too)
  if (t.swing > 50 && ((t.dir || ord ? (int)(a & 1) : idx) & 1)) fs += (t.swing / 50.0 - 1.0) * per;
  // H1's plan-time commands of the step (each column unless the other's CHN fails)
  int dly = 0, cut = -1, oct = 0, trs = 0, rnr = 0, rnv = 0, rtg = 0;
  for (int col = 0; col < 2; ++col) {
    const Fx& f = s.fx[col];
    if (!f.cmd || fx_order(f.cmd) || f.cmd == kFxChn || f.cmd == kFxFil || !fx_on(s, col)) continue;
    switch (f.cmd) {
      case kFxRtg: rtg = f.val; break;
      case kFxDly: dly += f.val; break;
      case kFxCut: cut = cut < 0 || f.val < cut ? f.val : cut; break;
      case kFxOct: oct += (int8_t)f.val; break;
      case kFxTrs: trs += (int8_t)f.val; break;
      case kFxRnd: rnr = f.val; break;
      case kFxRnv: rnv = f.val; break;
      default: break;
    }
  }
  fs += dly * per / kSub;   // DLY: the whole step later (its locks too)
  // seq-ux: the step's locks (or none: the bases back) before its first note, the set copied (a predicted pattern's
  // step fires before its pattern is the current one)
  if (s.nl || has_locks(t)) {
    int lo = 0;
    for (int k = 0; k < s.n && notes; ++k) lo = s.note[k].micro < lo ? s.note[k].micro : lo;
    int64_t lf = fol.to_host((int64_t)(fs + lo * per / kSub)) - latency;
    if (lf < (int64_t)hframes) lf = (int64_t)hframes;
    int slot = lset_w++ % kLockSets;
    LockSet& ls = lset[slot];
    ls.n = 0, ls.slide = 0;
    if (s.slide) slide_ahead(p, t, r, a, pred);
    for (int k = 0; k < s.nl && k < kLocks; ++k) {   // V1: a lock whose own condition fails is left out (its base back)
      const Lock& l = s.lock[k];
      if (!lock_pass(l, r, at, rng_key(seed_of(p, t, r, a, 1, pred, true), r, idx, lk, kRngLock, l.param))) continue;
      int64_t j;
      uint16_t to;
      if ((s.slide >> (l.param & 31) & 1) && slide_target(t, l.param, &j, &to)) {   // X1: to the param's next lock
        int64_t end = fol.to_host((int64_t)(fs0 + (double)j * per)) - latency;
        bool sw = !pred && pred_to >= 0 && pred_abs[r] >= 0 && fs0 + (double)j * per >= (double)pred_frame;   // (a switch first)
        if (!sw) ls.slide |= (uint16_t)(1u << ls.n), ls.to[ls.n] = to, ls.end[ls.n] = end;
      }
      ls.l[ls.n++] = l;
    }
    push(lf, fol.to_host((int64_t)fs0), ++ev_id, 2, vi, slot, 0, r, pred);   // (sf: the grid's - a replan cuts by it)
  }
  if (!notes) return;
  out::trig_track(vi, r);   // a shared voice's mute gate follows this track now (core/fx/out.h)
  // the notes that play: the step's own (R1's pitches), or P4 / M4's repeats of them
  RtNote rn[kRtNotes];
  int nr = 0;
  uint8_t pit[kNotes];
  for (int k = 0; k < s.n; ++k) pit[k] = rnd ? rp[k] : s.note[k].pitch;
  if (rt_on(s.rt) || rtg >= 2) {
    for (int k = 0; k < s.n; ++k) s.note[k].fresh = 0;
    const Step* rs = &s;
    Step tmp;
    if (!rt_on(s.rt)) {   // H1 RTG n: n repeats of the chord, AUTO
      tmp = s;
      tmp.rt = {(uint8_t)rtg, 0, kVcFlat, 0, kRtChord, 0, 0};
      rs = &tmp;
    }
    nr = rt_expand(*rs, pit, p.key, p.scale, p.qnt && p.scale, seed_of(p, t, r, a, 0, pred, true), r, idx, lk, rn, kRtNotes);
  } else {
    for (int k = 0; k < s.n; ++k) {
      Note& n = s.note[k];
      if (n.fresh) {   // just recorded (its pad already sounded): from the next pass
        n.fresh = 0;
        continue;
      }
      rn[nr++] = {n.micro, pit[k], n.vel, n.len};
    }
  }
  bool deg = p.qnt && p.scale;   // (H1's TRS / RND: scale degrees with the pattern's scale and QNT on)
  uint32_t nsd = rnr || rnv ? seed_of(p, t, r, a, 0, pred, true) : 0;
  for (int k = 0; k < nr; ++k) {
    RtNote n = rn[k];
    if (cut >= 0) {   // CUT: nothing sounds past n / 96 of the step
      if (n.off >= cut) continue;
      if (n.len > cut - n.off) n.len = (uint16_t)(cut - n.off < 1 ? 1 : cut - n.off);
    }
    int pitch = n.pitch + 12 * oct;
    if (trs) pitch = deg ? degree_move(pitch, p.key, p.scale, trs) : pitch + trs;
    if (rnr) {   // RND +-r (each note its own draw)
      int d = rng_below(rng_key(nsd, r, idx, lk, kRngFx, 100 + k), 2 * rnr + 1) - rnr;
      if (d) pitch = deg ? degree_move(pitch, p.key, p.scale, d) : pitch + d;   // (0: as written, never quantised)
    }
    pitch = clampi(pitch, 0, 127);
    if (rnv) n.vel = (uint8_t)clampi(n.vel + rng_below(rng_key(nsd, r, idx, lk, kRngFx, 200 + k), 2 * rnv + 1) - rnv, 1, 127);
    if (p.qnt == kQntPlay) pitch = quantise(pitch, p.key, p.scale);   // M1: QNT PLAY
    int64_t on = fol.to_host((int64_t)(fs + n.off * per / kSub)) - latency;
    int64_t off = on + (int64_t)(n.len * per / kSub);
    if (off < on + kBlock) off = on + kBlock;
    if (on < (int64_t)hframes) {
      int64_t late = (int64_t)hframes - on;
      ++st.late;
      if (late > (int64_t)st.late_frames_max) st.late_frames_max = (uint32_t)late;
      off += late;
      on = (int64_t)hframes;
    }
    uint16_t id = ++ev_id;
    int64_t sf = fol.to_host((int64_t)fs0);   // the step's place on the grid (review: DLY or swing moved it past the next
    push(on, sf, id, true, vi, pitch, n.vel, r, pred, idx, a);   // step's cut, and a replan lost it)
    push(off, sf, id, false, vi, pitch, 0, r, pred, idx, a);
  }
}

// ---- the switch prediction: with a pattern queued (2D 02's next, prefetched in nxt), the switch comes at the next
// multiple of the change length (per-track scale mode, CHG > 1) or of the master length, counted at the pattern's speed
// from its step 0 [code: rytm-mods 1033 sc_poll's note]. The new pattern's first steps are planned ahead like any
// other (pred), so its step 0 lands on the Rytm's block, not 2D 03's arrival later; 2D 03 at that frame keeps them,
// anything else drops them and plans the old pattern again from there.
CORE_COLD void drop_prediction(bool miss) {
  if (pred_to < 0) return;
  cancel_ons(-1, INT64_MIN, false, 1);
  for (int r = 0; r < kRytmTracks; ++r) {
    // (P2: planned[] counts the track's own steps; its plan stops before the predicted switch, at most there)
    int64_t v = pred_abs[r] >= 0 ? vceil_i(rat_of(track_of(*cur, r)), pred_abs[r]) : 0;
    if (pred_abs[r] >= 0 && planned[r] > v) planned[r] = v;
    pred_abs[r] = -1, pred_planned[r] = 0;
  }
  pred_to = -1;
  if (miss) ++st.predict_miss;
}
CORE_COLD void update_prediction() {
  int sw = pat_mode == 1 && pat_chg > 1 ? pat_chg : pat_len;   // (len 1: INF / not a length: no prediction)
  if (sw < 2) sw = 0;
  bool want = predict && transport == kPlay && pat_queued >= 0 && pat_queued != cur->index && nxt->index == pat_queued &&
              pat_start >= 0 && sw >= 2 && pat_start_track >= 0;
  StepClock& c = clk[want ? pat_start_track : 0];
  if (want && (!c.on || c.period <= 0)) want = false;
  if (!want) {
    if (pred_to >= 0 && !(transport == kPlay && pat_queued == pred_to)) drop_prediction(true);
    return;
  }
  int ps = pat_spd >= 0 && pat_spd <= 6 ? pat_spd : 2;
  double mper = c.period * speed_mult(c.speed) / speed_mult(ps);   // frames a master step
  double now = (double)fol.to_rytm((int64_t)hframes);
  if (pred_to == pat_queued) {   // made: kept until its 03 (which comes after the frame), else given up 2 steps past it
    // per pattern poly: a track poly in the next pattern only gets its clock from the 2E's clock part (tick) after the
    // prediction was made: its switch step from then on (nothing of it was planned yet)
    uint16_t nm = mask_of(*nxt);
    for (int r = 0; r < kRytmTracks; ++r) {
      StepClock& k = clk[r];
      if (pred_abs[r] < 0 && (nm >> r & 1) && k.on && k.period > 0 && (double)pred_frame > k.last_frame)
        pred_abs[r] = (int64_t)llround(k.step_at((double)pred_frame)), pred_planned[r] = 0;
    }
    if (now < (double)pred_frame + 2 * mper) return;
    drop_prediction(true);
  } else if (pred_to >= 0) {
    drop_prediction(true);
  }
  double m = (now - (double)pat_start) / mper;
  if (m < 0) m = 0;
  int64_t b = ((int64_t)(m / sw) + 1) * sw;
  if ((double)b - m < 1.0) b += sw;   // queued inside the boundary's last step: the Rytm decided already (the next one)
  int64_t f = pat_start + (int64_t)llround((double)b * mper);
  pred_to = pat_queued, pred_frame = f;
  ++st.predicted;
  uint16_t nm = mask_of(*nxt);   // per pattern poly: the next pattern's seq tracks too (their clock: tick's 2E)
  for (int r = 0; r < kRytmTracks; ++r) {
    StepClock& k = clk[r];
    pred_abs[r] = (in_mask(r) || (nm >> r & 1)) && k.on && k.period > 0 ? (int64_t)llround(k.step_at((double)f)) : -1;
    pred_planned[r] = 0;
  }
}

CORE_COLD void plan() {
  if (transport != kPlay || !fol.valid) return;
  int64_t horizon = (int64_t)hframes + kBlock + kLookahead;
  update_prediction();
  // per pattern poly: a track is planned from the current pattern while it is a seq track there, and from the
  // predicted one while it is one THERE (a slot that is off, or a released track, plays nothing of the Sidecar's)
  uint16_t nm = pred_to >= 0 && nxt->index == pred_to ? mask_of(*nxt) : 0;
  for (int r = 0; r < kRytmTracks; ++r) {
    bool here = in_mask(r), there = (nm >> r & 1) && pred_abs[r] >= 0;
    if (!here && !there) continue;
    StepClock& c = clk[r];
    if (!c.on || c.period <= 0) continue;
    Track* t = here ? track_of(*cur, r, false) : nullptr;
    Track* nt = there ? track_of(*nxt, r, false) : nullptr;
    Ratio q = rat_of(t), nq = rat_of(nt);   // P2: the track's own step clock (1:1 FOLLOW)
    double per = c.period * q.den / q.num, nper = c.period * nq.den / nq.num;
    int64_t v0 = vceil_i(q, c.abs);
    if (planned[r] < v0) planned[r] = v0;   // behind the Rytm (a jump, a late start): from its step on
    for (int guard = 0; guard < 32; ++guard) {
      int64_t a = planned[r];
      double x = vpos(q, a);
      if (pred_to >= 0 && pred_abs[r] >= 0 && x >= (double)pred_abs[r] - 1e-9) {
        // the predicted pattern's steps from the switch on, at its own speed (the current pattern's plan stops here)
        int k = pred_planned[r];
        double fs = c.frame_of((double)pred_abs[r] + vpos(nq, k));
        if (fol.to_host((int64_t)(fs - nper)) - latency > horizon) break;   // a note up to a step early
        if (nt) plan_step(*nxt, *nt, r, k, fs, nper, true);
        pred_planned[r] = k + 1;
        continue;
      }
      double fs = c.frame_of(x);
      int64_t earliest = fol.to_host((int64_t)(fs - per)) - latency;   // a note up to a step early
      if (earliest > horizon) break;
      if (t) plan_step(*cur, *t, r, a, fs, per, false);
      planned[r] = a + 1;
    }
  }
}

int restart_skip[kRytmTracks];   // steps of the new pattern a confirmed prediction planned already (the restart's tick)
CORE_COLD void restart_all() {
  ++seg_gen;   // R1b: a new segment (the predicted pattern's steps were planned in it already)
  for (int r = 0; r < kRytmTracks; ++r) restart[r] = true, planned[r] = 0, play_abs[r] = 0, restart_skip[r] = 0;
  pat_start = -1, pat_start_track = -1;
  memset(cond_last, 0, sizeof cond_last);   // rui1040: PRE / NEI start false at PLAY and at a switch
}

// E1: FREE's seed for this PLAY (a fixed seed ignores it): new at each PLAY from STOP, kept through a pause and the
// pattern switches, so a replan or a predicted switch inside one PLAY agrees with the first plan
bool seed_held = false;   // tests / the console (seq=seed:hold:N): the next PLAYs keep this FREE seed
CORE_COLD void roll_seed() {
  smod_reset();   // R1b: a PLAY from STOP samples its offsets anew
  if (!seed_held) free_seed = (uint32_t)rng_mix((uint64_t)free_seed ^ (uint64_t)now_ms() << 20 ^ hframes);
}
CORE_COLD void on_stop() {
  drop_prediction(false);
  release(-1);
  unlock_all();   // seq-ux: every locked param back to its base
  for (int r = 0; r < kRytmTracks; ++r) clk[r].reset(), restart[r] = true, planned[r] = 0, play_abs[r] = -1;
  for (Open& o : opens) o.used = false;
  if (dirty) save_now = true;
}

// ---- the Rytm's messages
CORE_COLD int64_t frame_of_blk(const uint8_t* b) {
  uint32_t blk = (uint32_t)b[0] << 21 | (uint32_t)b[1] << 14 | (uint32_t)b[2] << 7 | b[3];
  return fol.unwrap(blk) * kBlkFrames;
}

CORE_COLD void on_ticks(const uint8_t* b, int n) {
  int64_t f = frame_of_blk(b);
  fol.sample((int64_t)hframes, f);
  st.offset = fol.offset;
  if (transport != kPlay && f < transport_frame) return;   // fired before a stop / pause that came first
  if (transport == kStop) set_transport(kPlay), restart_all(), roll_seed();   // the ticks came before the TRANSPORT (or it was lost)
  if (transport == kPause) set_transport(kPlay);
  int cnt = b[4];
  for (int i = 0; i < cnt; ++i) {
    const uint8_t* t = b + 5 + 4 * i;
    int r = t[0], step = t[1], len = t[2], sp = t[3];
    ++st.ticks;
    if (r >= kRytmTracks) continue;
    StepClock& c = clk[r];
    int jumps = c.jumps;
    double pred = c.on && c.period > 0 ? c.frame_of((double)c.abs + 1) : (double)f;
    bool rs = restart[r];
    c.tick(step, len, sp, (double)f, tempo_period(), rs);
    restart[r] = false;
    st.err_last = (int32_t)((double)f - pred);
    if (!rs && c.on) {
      int32_t e = st.err_last < 0 ? -st.err_last : st.err_last;
      if (e > st.err_max && c.abs > 2) st.err_max = e;
    }
    if (c.jumps != jumps) {   // the clock moved: what was planned on the old one goes, from the step after this tick
      cancel_ons(r, (int64_t)hframes);
      planned[r] = vfloor_i(rat_of(track_of(*cur, r)), c.abs) + 1;
    }
    if (rs) {
      planned[r] = vceil_i(rat_of(track_of(*cur, r)), c.abs) + restart_skip[r];
      restart_skip[r] = 0;
      if (pat_start < 0 && c.abs == 0) pat_start = (int64_t)f, pat_start_track = r;   // the pattern's step 0
    }
    if (pat_start_track < 0 && c.on && (in_mask(r) || (clock_now >> r & 1)) && c.abs >= 0)   // (a first tick that is not
                                                                                               // step 0: from its clock)
      pat_start = (int64_t)c.frame_of(0), pat_start_track = r;
    play_abs[r] = c.abs;
  }
  plan();   // a step 0 after PLAY: at once (late), not a block later
}

CORE_COLD void on_switch(int to, const uint8_t* blk) {
  int64_t f = frame_of_blk(blk);
  fol.sample((int64_t)hframes, f);
  ++st.switches;
  StepClock& c = clk[pat_start_track >= 0 ? pat_start_track : 0];
  int64_t tol = c.period > 0 ? (int64_t)(c.period / 2) : 2400;
  bool hit = pred_to == to && f - pred_frame <= tol && pred_frame - f <= tol;
  int skip[kRytmTracks];
  for (int r = 0; r < kRytmTracks; ++r) skip[r] = hit ? pred_planned[r] : 0;
  if (hit) {   // the predicted switch: its planned steps stay (no longer predicted), the old pattern's go
    cancel_ons(-1, fol.to_host(f) - kBlkFrames, true, 2);
    for (int i = 0; i < nev; ++i) ev[i].pred = 0;
    pred_to = -1;
    for (int r = 0; r < kRytmTracks; ++r) pred_abs[r] = -1, pred_planned[r] = 0;
    ++st.predict_ok;
  } else {
    drop_prediction(pred_to >= 0);
    cancel_ons(-1, fol.to_host(f) - kBlkFrames, true);   // the old pattern's steps from the switch point on
  }
  install(to);
  pat_now = to, pat_queued = -1, pat_changed_ms = now_ms();
  rytm_dump::pattern_changed(to);
  restart_all();
  for (int r = 0; r < kRytmTracks; ++r) restart_skip[r] = skip[r];
}

// ---- seq-p2: the Rytm's edits (2D 0A) applied to the host's data, as stock applies them to its own
// Track r (-1 every one) planned again from its first step that has not started yet: that step's events and every later
// one's go (by their step), the steps before keep theirs (seqtop: before, a replan between a step's start and its tick's
// arrival - the tick comes up to a round trip late - planned that started step again, a second, late note). The
// predicted next pattern's steps stay (an edit changes the current pattern only).
CORE_COLD void replan(int r) {
  for (int t = 0; t < kRytmTracks; ++t) {
    if (r >= 0 && t != r) continue;
    StepClock& c = clk[t];
    if (!c.on || c.period <= 0 || !fol.valid) {
      cancel_ons(t, (int64_t)hframes);
      if (c.on) planned[t] = vfloor_i(rat_of(track_of(*cur, t)), c.abs) + 1;
      continue;
    }
    Ratio q = rat_of(track_of(*cur, t));
    int64_t v = vceil_d(q, c.step_at((double)fol.to_rytm((int64_t)hframes + latency)));   // its micro-0 note not started
    int64_t v1 = vfloor_i(q, c.abs) + 1;
    if (v < v1) v = v1;
    cancel_ons(t, fol.to_host((int64_t)c.frame_of(vpos(q, v))) - 1, true, 2);
    planned[t] = v;
  }
}
CORE_COLD void ensure_pattern(int p) {   // the edit's pattern is the current one (2D 02 normally made it so)
  if (p < 0 || p > 127 || (cur->index == p && pat_loaded == p)) return;
  install(p);
  pat_now = p, load_due = false;
}
CORE_COLD void copy_track(Track& d, const Track& s) {   // steps, length, defaults, swing; d keeps its Rytm track
  int8_t r = d.rytm;
  d = s;
  d.rytm = r;
}
CORE_COLD int page_of_ui(int r) {
  const Track* t = track_of(*cur, r);
  int np = npages_of(eff_len(t, r));
  return ui.page < np ? ui.page : np - 1;
}
// The one allocator of a track's slot in the current pattern (fix1051b / fix1053, per pattern poly 2026-10-26): its
// own, a free one, else one that holds no steps and is OFF (a slot that is on is that track's poly track, empty or not;
// a slot holding steps is never taken, on or off). A pattern holds kTracks slots; nullptr: none to be had (four slots
// that hold steps or are on).
CORE_COLD Track* slot_for(int r) {
  if (r < 0 || r >= kRytmTracks) return nullptr;
  if (Track* t = track_of(*cur, r, true)) return t;
  for (Track& x : cur->t) {
    if (x.rytm == r || x.on || used_steps(x)) continue;
    log(CORE_STR("seq: pattern %c%02d: track %d takes track %d's slot (no steps, not a poly track here)"),
        'A' + cur->index / 16 % 8, cur->index % 16 + 1, r + 1, x.rytm + 1);
    track_init(x, r);
    return &x;
  }
  return nullptr;
}
CORE_COLD Track* seq_track(int r, bool create) {
  Track* t = track_of(*cur, r, false);
  if (t || !create || !in_mask(r)) return t;
  if ((t = slot_for(r))) return t;
  log(CORE_STR("seq: pattern %c%02d: no track slot for track %d (%d sequenced tracks a pattern, all with steps)"),
      'A' + cur->index / 16 % 8, cur->index % 16 + 1, r + 1, kTracks);
  return nullptr;
}
// per pattern poly (2026-10-26, replaces fix1051b's reconcile_mask): the seq tracks are the current pattern's. mask is
// derived (mask_of): on every install, every edit of the pattern's slots, every set_seq, and in tick() (a 33 changed a
// track's machine). A track leaving it gets its locked params and slides back (rel: its sounding notes released too -
// not at a switch, where the old pattern's notes end as planned).
// Seq mode over for the host: the Rytm's 2D 08 off, or (fix1028) its track left the mask. The Rytm's own seq mode is
// "the selected track in the 2E's mask and GRID REC on" (rytm-mods 0026 rui.c sq_tick), so it ends at the next 2E -
// the host does not wait for that report: the held trigs, NOTE EDIT on that track, our trig LEDs (tick) and the page
// seq mode opened go now. view_too: the page TRK + TRIG opened as well (a TURN OFF: no poly page left over the track).
CORE_COLD void seqmode_end(bool view_too) {
  if (ui.ne && !in_mask(ui.ne_track)) ne_leave();   // (NOTE EDIT is a poly track's)
  seqmode = false;
  ui.held = 0, ui.page_held = false;
  if ((we_opened || (view_too && ui.view_opened)) && ui::remote_open()) ui::remote_request(false);
  we_opened = false;
  if (view_too) ui.view_opened = false, ui.prompt = 0;
}
CORE_COLD void remask(bool rel) {
  if (!cur) return;
  uint16_t m = mask_of(*cur), gone = (uint16_t)(mask & ~m);
  if (m == mask) return;
  for (int r = 0; r < kRytmTracks; ++r) {
    if (!(gone >> r & 1)) continue;
    if (rel) release(r);
    apply_locks(r, nullptr);
    memset(slides[r], 0, sizeof slides[r]), slide_any &= (uint16_t)~(1u << r), cur_set[r].n = 0;
  }
  log(CORE_STR("seq: pattern %c%02d: poly tracks 0x%03x (were 0x%03x)"), 'A' + cur->index / 16 % 8, cur->index % 16 + 1, m,
      mask);
  mask = m;
  prompt_drop_stale();   // fix1028 review
  if (seqmode && seqmode_track >= 0 && (gone >> seqmode_track & 1)) {   // fix1028: its track is no poly track now
    log(CORE_STR("seq: track %d left the poly tracks: seq mode over"), seqmode_track + 1);
    seqmode_end(false);
  }
}
// 1042: the Rytm's SAVE / RELOAD PATTERN and TRACK. The Sidecar keeps the saved state of a pattern as pNNN.sqs (stock
// keeps its own saved copy of each pattern); a reload replaces the working copy with it and first keeps the working copy
// as pNNN.prv (one deep, never lost by a reload). Without a saved state (the Sidecar never saw a SAVE of it) a reload
// keeps the steps: nothing is ever replaced by a guess. A locked pattern (its file did not load) is neither saved nor
// reloaded.
Pattern* saved_p = nullptr;
CORE_COLD bool load_saved(int index) {   // pNNN.sqs into saved_p: false when there is none (or it does not load)
  if (!saved_p) saved_p = new Pattern;
  clear(*saved_p, index);
  if (!card_ok) return false;
  char fn[96];
  file_ext(fn, sizeof fn, index, "sqs");
#if CORE_MSAMP
  // the RAM store (phase 2): no card call on this core (a .sqs there but unreadable: none to reload - and the store
  // holds it locked, so write_pattern never writes over it)
  const uint8_t* buf;
  uint32_t len;
  store::State sst = store::get(fn, &buf, &len);
  if (sst == store::kUnknown)   // (not "no saved state": the store does not have this folder now)
    log(CORE_STR("seq: %s not in the store (project %s %s): nothing reloaded"), fn, project,
        g_loading ? "still loading" : "not loaded");
  if (sst != store::kPresent) return false;
  int n = (int)len;
#else
  File f;
  if (!f.open(fn, kRead)) return false;
  static uint8_t* buf = nullptr;
  if (!buf) buf = new uint8_t[pack_bound()];
  int n = (int)f.read(buf, (size_t)pack_bound());
  f.close();
#endif
  if (unpack(*saved_p, index, buf, n)) return true;
  log(CORE_STR("seq: %s does not load: the reload keeps the steps"), fn);
  clear(*saved_p, index);
  return false;
}
CORE_COLD void on_save_reload(int op, int r) {
  int idx = cur->index;
  if (idx < 0) return;
  if (lock_of(idx)) {
    log(CORE_STR("seq: pattern %c%02d is locked%s: its %s not followed"), 'A' + idx / 16 % 8, idx % 16 + 1,
        lock_of(idx) == kLockLoading ? " (its project still loading)" : "", op == kPSave || op == kTSave ? "save" : "reload");
    return;
  }
  if (op == kPSave) {
    if (write_pattern(*cur, "sqs")) ++st.saved;
    return;
  }
  if (op == kTSave) {
    if (r < 0) return;
    if (!load_saved(idx)) memcpy(saved_p, cur, sizeof *saved_p);   // no saved state yet: the others as they are now
    const Track* w = track_of(*cur, r);
    Track* s = track_of(*saved_p, r, w != nullptr);
    if (s && w) copy_track(*s, *w);
    else if (s) clear_steps(*s, 0, kMaxSteps);
    if (write_pattern(*saved_p, "sqs")) ++st.saved;
    return;
  }
  if (!load_saved(idx)) {
    log(CORE_STR("seq: pattern %c%02d reloaded on the Rytm: no saved Sidecar state, the steps kept"), 'A' + idx / 16 % 8,
        idx % 16 + 1);
    return;
  }
  if (!write_pattern(*cur, "prv")) {   // the working copy first; not written: nothing replaced
    log(CORE_STR("seq: pattern %c%02d: the reload not followed (its working copy could not be kept)"), 'A' + idx / 16 % 8,
        idx % 16 + 1);
    return;
  }
  if (op == kPReload) {
    for (int k = 0; k < kTracks; ++k) cur->t[k] = saved_p->t[k];
    replan(-1);
  } else {
    if (r < 0) return;
    const Track* s = track_of(*saved_p, r);
    Track* w = track_of(*cur, r, s != nullptr);
    if (w && s) copy_track(*w, *s);
    else if (w) clear_steps(*w, 0, kMaxSteps);
    replan(r);
  }
  ++st.reloads;
  mark_dirty();
}
CORE_COLD void on_edit_op(const uint8_t* a);
// per pattern poly: a pattern / track paste, clear, undo or reload brings its slots' on flags along
CORE_COLD void on_edit(const uint8_t* a) {
  on_edit_op(a);
  remask(true);
}
CORE_COLD void on_edit_op(const uint8_t* a) {
  int op = a[0], p = a[1], r = a[2] < kRytmTracks ? a[2] : -1, ok = a[3], src = a[4];
  uint32_t arg = (uint32_t)a[5] | (uint32_t)a[6] << 7 | (uint32_t)a[7] << 14;
  ++st.edits_rytm;
  ensure_pattern(p);
  // round 2: a copy of a loading pattern (its empty stand-in) is refused: a later paste would wipe the target's tracks
  if ((op == kPCopy || op == kTCopy || op == kGCopy || op == kRCopy) && ok && lock_of(cur->index) == kLockLoading)
    return refused_log("a copy");   // (the clipboard as it was)
  if (src == 1) {   // rui1040: seq mode's own gesture (held trigs / PAGE + FUNC + REC / STOP / PLAY, the page open or not):
    for (int k = 0; k < 16; ++k)   // the held keys' releases are no tap toggle and no page step
      if ((arg >> k & 1) || (ui.held >> k & 1)) ui.held_used[k] = true;
    if (ui.page_held) ui.page_used = true;
  }
  switch (op) {
    case kPReload:
    case kPSave:
    case kTReload:
    case kTSave:
      on_save_reload(op, r);   // (the functions' result is no success flag: not checked)
      return;
    case kPCopy:
      if (!ok) return;
      memcpy(clip_p, cur, sizeof *clip_p);
      return;
    case kPPaste:
    case kPClear:
      if (!ok || (op == kPPaste && clip_p->index < 0)) return;
      memcpy(undo_p, cur, sizeof *undo_p), undo_p_kind = op == kPPaste ? 2 : 1;
      if (op == kPPaste) {
        for (int k = 0; k < kTracks; ++k) cur->t[k] = clip_p->t[k];
      } else {
        clear(*cur, cur->index);
      }
      replan(-1);
      mark_dirty();
      return;
    case kPUndo:
      if (!ok || undo_p->index != cur->index || (int)arg != undo_p_kind) return;
      for (int k = 0; k < kTracks; ++k) cur->t[k] = undo_p->t[k];
      undo_p->index = -1, undo_p_kind = 0;
      ++st.undos;
      replan(-1);
      mark_dirty();
      return;
    case kTCopy: {
      if (!ok || r < 0) return;
      const Track* t = track_of(*cur, r);
      if (t) clip_t = *t;
      else track_init(clip_t, r);
      return;
    }
    case kTPaste:
    case kTClear: {
      if (!ok || r < 0 || (op == kTPaste && clip_t.rytm < 0)) return;
      Track* t = track_of(*cur, r, op == kTPaste && used_steps(clip_t) > 0);
      if (!t) return;   // nothing here and nothing to paste
      undo_t = *t, undo_t_kind = op == kTPaste ? 2 : 1, undo_t_track = r, undo_t_pat = cur->index;
      if (op == kTPaste) copy_track(*t, clip_t);
      else clear_steps(*t, 0, kMaxSteps);
      replan(r);
      mark_dirty();
      return;
    }
    case kTUndo: {
      if (!ok || r < 0 || undo_t_track != r || undo_t_pat != cur->index || (int)arg != undo_t_kind) return;
      Track* t = slot_for(r);
      if (t) copy_track(*t, undo_t);
      undo_t_track = -1, undo_t_kind = 0;
      ++st.undos;
      replan(r);
      mark_dirty();
      return;
    }
    case kGCopy: {
      if (!ok || r < 0) return;
      const Track* t = track_of(*cur, r);
      int pg = page_of_ui(r);
      for (int i = 0; i < kStepsPerPage; ++i)
        if (t) clip_g[i] = t->step[pg * kStepsPerPage + i];
        else memset(&clip_g[i], 0, sizeof clip_g[i]);
      clip_g_valid = true;
      return;
    }
    case kGPaste:
    case kGClear: {
      if (!ok || r < 0 || (op == kGPaste && !clip_g_valid)) return;
      int pg = page_of_ui(r), kind = op == kGPaste ? 2 : 1;
      Track* t = slot_for(r);
      if (!t) return;
      // seq mode's own gesture: a second paste / clear with no edit since undoes the first (as stock's UNDO PASTE)
      if (src == 1 && undo_g_kind == kind && undo_g_track == r && undo_g_pat == cur->index && undo_g_page == pg &&
          undo_g_edits == st.edits) {
        for (int i = 0; i < kStepsPerPage; ++i) t->step[pg * kStepsPerPage + i] = undo_g[i];
        undo_g_kind = 0;
        ++st.undos;
      } else {
        for (int i = 0; i < kStepsPerPage; ++i) undo_g[i] = t->step[pg * kStepsPerPage + i];
        undo_g_kind = kind, undo_g_track = r, undo_g_pat = cur->index, undo_g_page = pg;
        for (int i = 0; i < kStepsPerPage; ++i)
          if (op == kGPaste) t->step[pg * kStepsPerPage + i] = clip_g[i];
          else memset(&t->step[pg * kStepsPerPage + i], 0, sizeof(Step));
      }
      replan(r);
      mark_dirty();
      undo_g_edits = st.edits;
      return;
    }
    case kGUndo: {
      if (!ok || r < 0 || undo_g_track != r || undo_g_pat != cur->index || (int)arg != undo_g_kind) return;
      Track* t = slot_for(r);
      if (!t) return;
      for (int i = 0; i < kStepsPerPage; ++i) t->step[undo_g_page * kStepsPerPage + i] = undo_g[i];
      undo_g_kind = 0;
      ++st.undos;
      replan(r);
      mark_dirty();
      return;
    }
    case kRCopy:
    case kRPaste:
    case kRClear: {
      uint16_t m = (uint16_t)(arg & 0xFFFF);
      if (!ok || r < 0 || !m) return;   // a stock trig op (src 0) has no mask: not the host's (its trig keys were stock's)
      int pg = page_of_ui(r);
      Track* t = track_of(*cur, r, op != kRCopy);
      if (op == kRCopy) {
        clip_r_n = 0;
        for (int k = 0; k < kStepsPerPage; ++k)
          if (m >> k & 1) {
            if (t) clip_r[clip_r_n] = t->step[pg * kStepsPerPage + k];
            else memset(&clip_r[clip_r_n], 0, sizeof(Step));
            ++clip_r_n;
          }
        return;
      }
      if (!t || (op == kRPaste && !clip_r_n)) return;
      int kind = op == kRPaste ? 2 : 1;
      if (undo_r_kind == kind && undo_r_track == r && undo_r_pat == cur->index && undo_r_page == pg && undo_r_mask == m &&
          undo_r_edits == st.edits) {   // the same gesture again: undone
        int j = 0;
        for (int k = 0; k < kStepsPerPage; ++k)
          if (m >> k & 1) t->step[pg * kStepsPerPage + k] = undo_r[j++];
        undo_r_kind = 0;
        ++st.undos;
      } else {
        int j = 0;
        for (int k = 0; k < kStepsPerPage; ++k)
          if (m >> k & 1) undo_r[j++] = t->step[pg * kStepsPerPage + k];
        undo_r_kind = kind, undo_r_track = r, undo_r_pat = cur->index, undo_r_page = pg, undo_r_mask = m;
        j = 0;
        for (int k = 0; k < kStepsPerPage; ++k) {
          if (!(m >> k & 1)) continue;
          Step& d = t->step[pg * kStepsPerPage + k];
          if (op == kRPaste) d = clip_r[j++ % clip_r_n];
          else memset(&d, 0, sizeof d);
        }
      }
      replan(r);
      mark_dirty();
      undo_r_edits = st.edits;
      return;
    }
    default:
      return;
  }
}

// 2D 04: the Rytm's SCALE. A seq track with its own length follows a change the user made on the Rytm (the first
// report, a resync's, only sets what is known); AUTO follows anyway. The pattern's: the switch prediction's lengths.
CORE_COLD void on_scale(const uint8_t* a, int n) {
  ++st.scales;
  int len = a[1] | a[2] << 7, sp = a[3], mode = a[4];
  if (a[0] == kNone) {
    if (n < 7) return;
    pat_len = len, pat_spd = sp, pat_mode = mode, pat_chg = a[5] | a[6] << 7;
    return;
  }
  int r = a[0];
  if (r >= kRytmTracks) return;
  int old = sc_len[r];
  sc_len[r] = len, sc_spd[r] = sp;
  if (len >= 1) clk[r].rytm_len = len;
  if (sp <= 6) clk[r].speed = sp;
  if (old < 0 || old == len || now_ms() - pat_changed_ms < 300) return;   // known now, or another pattern's
  Track* t = track_of(*cur, r, false);
  if (t && t->length && in_mask(r)) {
    set_length(*t, r, len);   // seq1112: longer -> the new pages copied, as on the Rytm's own track
    mark_dirty();
  }
}

CORE_COLD void on_state(const uint8_t* a, int n) {
  ++st.states;
  switch (a[0]) {
    case kTransport: {
      if (n < 6 || a[1] > 2) break;
      int64_t f = frame_of_blk(a + 2);
      fol.sample((int64_t)hframes, f);
      transport_frame = f;
      int s = a[1];
      if (s == kStop) {
        if (transport != kStop) on_stop();
        set_transport(kStop);
      } else if (s == kPause) {
        if (transport == kPlay) {
          release(-1);
          for (StepClock& c : clk) c.on = false;   // continue re-anchors on its first tick
        }
        set_transport(kPause);
      } else {
        if (transport == kStop) restart_all(), roll_seed();
        set_transport(kPlay);
      }
      return;
    }
    case kPattern:
      if (n < 3) break;
      if (a[1] < 128 && a[1] != pat_now && transport == kPlay && a[1] == pat_queued) {
        // the queued pattern became the active one a step before its switch (rytm-mods 1032 reports it so; 1033
        // keeps the playing one until 03): its 03 follows at the switch block, which does the change
      } else if (a[1] < 128 && a[1] != pat_now) {   // a change without a switch (stopped, a direct jump): now
        if (transport == kPlay) drop_prediction(false), cancel_ons(-1, (int64_t)hframes), release(-1), restart_all();
        pat_now = a[1];
        load_due = true;
        pat_changed_ms = now_ms();
      }
      if (!(transport == kPlay && a[1] == pat_queued && a[1] != pat_now)) pat_queued = a[2] == kNone ? -1 : a[2];
      rytm_dump::pattern_changed(pat_now);
      return;
    case kScale:
      if (n < 6) break;
      on_scale(a + 1, n - 1);
      return;
    case kEdit:
      if (n < 9) break;
      on_edit(a + 1);
      return;
    case kDirty:
      if (n < 2) break;
      ++st.dirties;
      rytm_dump::dirty();
      return;
    case kFill:
      if (n < 2) break;
      fill = a[1] != 0;
      return;
    case kSwitch:
      if (n < 7) break;
      on_switch(a[2] & 0x7F, a + 3);
      return;
    case kMutes:
    case kSolo: {   // (fix1049: SOLO as MUTE: the tracks it silences)
      if (n < 3) break;
      uint16_t m = (uint16_t)(a[1] | (a[2] & 0x3F) << 7), was = silenced_now();
      if (a[0] == kMutes) mutes = m;
      else solos = m, ++st.solos;
      uint16_t now_muted = (uint16_t)(silenced_now() & ~was);
      out::set_mutes(silenced_now());   // the PRE / POST voices' mute gates (core/fx/out.h)
      for (int r = 0; r < kRytmTracks; ++r)
        if (now_muted >> r & 1) release(r);
      return;
    }
    case kRec:
      if (n < 3) break;
      rec_live = a[1] != 0, rec_grid = a[2] != 0;
      return;
    case kTempo: {
      if (n < 4) break;
      int t = a[1] << 14 | a[2] << 7 | a[3];
      if (t > 0) bpm = t / 120.0;
      return;
    }
    case kSeqMode: {
      if (n < 3) break;
      bool on = a[1] != 0;
      int tr = a[2] < kRytmTracks ? a[2] : -1;
      if (on && tr >= 0) {
        seqmode = true, seqmode_track = tr;
        ui.track = tr;
        if (!ui::remote_open()) we_opened = true;
        show_wanted = true;
        ui::remote_show_page("SEQ");
      } else {
        seqmode_end(false);
      }
      return;
    }
    case kPos:
      if (n < 8) break;
      fol.sample((int64_t)hframes, frame_of_blk(a + 1));
      st.dropped_rytm = a[7];
      return;
    default:
      return;
  }
  ++st.ticks_bad;
}

// ---- editing
CORE_COLD bool seq_shown() {
  const char* p = ui::remote_page_title();
  return p && !strcmp(p, "SEQ");
}
CORE_COLD Track* ui_track_data(bool create) { return create ? slot_for(ui_track()) : track_of(*cur, ui_track(), false); }
CORE_COLD Note new_note(const Track& t, int pitch, int vel) {
  return {(uint8_t)(pitch & 0x7F), (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel), t.dlen ? t.dlen : (uint16_t)(kSub * 3 / 4), 0, 0};
}
CORE_COLD void fill_default(Track& t, Step& s, bool last_chord = true) {
  s.n = 0, s.flags = 1, s.prob = 100, s.strum = 0, s.chd = 0, s.inv = 0;
  if (last_chord && ui.chord_n)
    for (int i = 0; i < ui.chord_n; ++i) add_note(s, new_note(t, ui.chord[i], ui.chord_vel[i]));
  else
    add_note(s, new_note(t, t.dnote, t.dvel));
}
CORE_COLD void toggle(int k) {
  Track* t = ui_track_data(true);
  if (!t) return;
  int idx = ui.page * kStepsPerPage + k;
  if (idx >= kMaxSteps) return;
  Step& s = t->step[idx];
  if (s.n || s.nl) memset(&s, 0, sizeof s);   // a trig off takes its locks (seq-ux) and every seqtop extra with it
  else fill_default(*t, s);
  mark_dirty();
}
CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

CORE_COLD void edit_step(Track& t, Step& s, int e, int d, bool func) {
  // an empty step's first turn: the track's default note (dbg: a chord made by turning starts from C3 / dnote, not from
  // whatever was played last; a tap still takes the last chord played, the pads under a held trig their own notes)
  if (!s.n && e != 7) fill_default(t, s, false);
  int lo = 0, hi = s.n;   // the notes edited: every one (SEL 0) or the selected
  if (ui.sel > 0 && ui.sel <= s.n) lo = ui.sel - 1, hi = ui.sel;
  switch (e) {
    case 0:   // M1: QNT EDIT / PLAY - by scale degree (FUNC: by semitone); else by semitone (FUNC: by octave)
      for (int k = lo; k < hi; ++k)
        s.note[k].pitch = (uint8_t)(cur->qnt && cur->scale && !func ? degree_move(s.note[k].pitch, cur->key, cur->scale, d)
                                                                    : clampi(s.note[k].pitch + d * (func && !(cur->qnt && cur->scale) ? 12 : 1), 0, 127));
      if (hi - lo == 1) t.dnote = s.note[lo].pitch;
      break;
    case 1:
      for (int k = lo; k < hi; ++k) s.note[k].vel = (uint8_t)clampi(s.note[k].vel + d * (func ? 8 : 1), 1, 127);
      break;
    case 2:
      for (int k = lo; k < hi; ++k)
        s.note[k].len = (uint16_t)clampi(s.note[k].len + d * (func ? kSub : kSub / 16), 1, 16 * kSub);
      t.dlen = s.note[lo].len;
      break;
    case 3:
      for (int k = lo; k < hi; ++k) s.note[k].micro = (int8_t)clampi(s.note[k].micro + d * (func ? 12 : 2), -kSub, kSub);
      break;
    case 4:
    case 5: {
      int low = 127;
      for (int k = 0; k < s.n; ++k) low = s.note[k].pitch < low ? s.note[k].pitch : low;
      uint8_t x[kNotes];   // the root under the lowest note: the inversion's lowest interval below it
      int key = cur->key, scale = cur->qnt ? cur->scale : 0;   // M1: QNT on - diatonic chords in the pattern's scale
      int root = low - (poly::expand(60, {s.chd, s.inv, 0, 0}, x) ? x[0] - 60 : 0);
      if (scale) {   // the scale root whose chord starts on the lowest note (else the lowest note itself)
        root = quantise(low, key, scale);
        for (int k = 0; k < 12; ++k) {
          int c = degree_move(quantise(low, key, scale), key, scale, -k);
          if (diatonic_chord(c, s.chd, s.inv, key, scale, x) && x[0] == low) {
            root = c;
            break;
          }
        }
      }
      int chd = s.chd, inv = s.inv;
      if (e == 4) chd = clampi(chd + d, 0, poly::kChords - 1);
      else inv = clampi(inv + d, 0, poly::kInvs - 1);
      set_chord_in(s, root, chd, inv, s.note[0].vel, s.note[0].len, s.strum, key, scale);
      ui.sel = 0;
      break;
    }
    case 6:
      apply_strum(s, clampi(s.strum + d * (func ? 8 : 2), -kSub / 2, kSub / 2));
      break;
    case 7:
      ui.sel = clampi(ui.sel + d, 0, s.n);
      break;
  }
}

// ---- seq-ux (1036): the prompts, the lock pages
// Can Rytm track r be a seq track: 1 its machine is a Sidecar machine (E0: every one - the note machines' pools, the
// instance pool's PLAITS / DAISYSP / JUNO / user machines, DSP SAMPLE mono; unknown before the Rytm's 33
// too), 0 an analog machine. (2 and 3, JUNO and the other mono machines before E0, are no longer answered.)
CORE_COLD int can_seq(int r) {
  int vb = ui::remote_track_machine(r);
  if (vb < 0) return 1;
  return vb == 0x7F ? 0 : 1;
}
CORE_COLD void say(const char* m, uint32_t dur = 2500);   // a transient footer for dur ms
// per pattern poly: track r made a poly track (or not) in the CURRENT pattern: its slot's on flag. Off keeps the steps
// (silent; on again brings them back). On needs a slot: its own, a free one, or an empty one that is off - four slots
// that hold steps (or are on) refuse it with the reason.
CORE_COLD void set_seq(int r, bool on) {
  if (r < 0 || r >= kRytmTracks || on == slot_on(r)) return;
  if (g_loading) return refused_log(on ? "a seq track on" : "a seq track off");
  Track* t = on ? slot_for(r) : track_of(*cur, r, false);
  if (!t) {
    log(CORE_STR("seq: track %d is no seq track: pattern %c%02d's %d slots all hold steps or poly tracks"), r + 1,
        'A' + cur->index / 16 % 8, cur->index % 16 + 1, kTracks);
    say("4 POLY TRACKS IN PATTERN");
    return;
  }
  t->on = on ? 1 : 0;
  legacy_mask = (uint16_t)(on ? legacy_mask | 1u << r : legacy_mask & ~(1u << r));
  tracks_touch();   // (tracks.txt's mask= for an older build)
  mark_dirty();     // the flag is the pattern's (its file)
  remask(true);
  // fix1028 (hardware: after TURN OFF the poly steps and the poly page stayed until NO and GRID REC off): the SEQ page
  // seq mode or TRK + TRIG opened over this track closes now, as when the selection leaves a seq track (selected_track)
  if (!on && r == ui_track() && (ui.view_opened || we_opened)) seqmode_end(true);
  log(CORE_STR("seq: track %d %s in pattern %c%02d"), r + 1, on ? "is a seq track" : "is no seq track (its steps kept)",
      'A' + cur->index / 16 % 8, cur->index % 16 + 1);
}
// The lock page's encoder e: the param it locks (-1 none). SRC A..H = P1..P8, SMPL = P9..P16, FX A..D = FX1 P2..P5,
// E..H = FX2's; TRIG, FLTR, AMP and LFO lock nothing on the host (the Rytm's analog side, its own LFO).
CORE_COLD int lock_param_of(int pg, int e) {
  if (e < 0 || e > 7) return -1;
  return pg == kLpSrc ? e : pg == kLpSmpl ? 8 + e : pg == kLpFx && e / 4 < fx::kSlots ? kLockFx + e : pg == kLpIns ? kLockIns + e
                                                                                                             : -1;
}
// The value a held step's lock of param starts from: the Rytm's (the SRC view's 2A, else the voice's own) / the FX
// knob's, 14 bits
CORE_COLD int base_value(int r, int param) {
  if (param < 16) return ui::remote_track_param(r, param) << 7;
  if (param >= kLockIns && param < kLockIns + 8) {   // fx-ins: the insert's knob (core/fx/insert.h)
    int vi = voice_of(r);
    return vi >= 0 && vi < kVoices ? ins::param(vi, (param - kLockIns) / 4, (param - kLockIns) % 4) << 7 : 0;
  }
  int j = param - kLockFx, sl = j / 4, k = 2 + j % 4;
  if (sl >= fx::kSlots) return 0;
  return ((flk.seen >> j & 1) ? flk.rep[j] : fx::control.knob[sl][k]) << 7;
}
CORE_COLD void lock_turn(int e, int d, bool func) {
  int r = ui_track(), prm = lock_param_of(ui.lockpg, e);
  if (prm < 0) return;
  Track* t = ui_track_data(true);
  if (!t) return;
  for (int k = 0; k < 16; ++k) {
    if (!(ui.held >> k & 1)) continue;
    Step& s = t->step[ui.page * kStepsPerPage + k];
    int cur_v = lock_of(s, prm);
    if (cur_v < 0) cur_v = base_value(r, prm);
    int nv = clampi((cur_v >> 7) + d * (func ? 8 : 1), 0, 127) << 7;
    if (set_lock(s, prm, nv)) ++st.locks_set;
    ui.held_used[k] = true;
  }
  mark_dirty();
}
CORE_COLD void lock_clear(int prm) {   // the held steps' lock of prm (-1 every one)
  Track* t = ui_track_data(false);
  if (!t) return;
  for (int k = 0; k < 16; ++k) {
    if (!(ui.held >> k & 1)) continue;
    Step& s = t->step[ui.page * kStepsPerPage + k];
    if (prm < 0) s.nl = 0, s.slide = 0;
    else clear_lock(s, prm);
    ui.held_used[k] = true;
  }
  mark_dirty();
}
CORE_COLD void close_view() {   // back to the Rytm's screen (the page TRK + TRIG opened)
  ui.view_opened = false, ui.prompt = 0, ui.lockpg = -1;
  if (ui::remote_open() && !seqmode) ui::remote_request(false);
}
void menu_dir(int d);   // rui1050 (below)
void menu_print();
CORE_COLD void prompt_answer(bool yes) {
  prompt_drop_stale();   // fix1028 review (install / remask drop it too: belt and braces)
  int r = ui.prompt_track, kind = ui.prompt;
  ui.prompt = 0;
  if (!kind) return;
  if (kind == kPromptMenu) {   // UX review S2: the track's menu
    if (!yes) return;
    if (ui.prompt_sel == kMenuOff) ui.prompt = kPromptOff, ui.prompt_pat = cur->index;   // TURN OFF: asked (its steps stay on the card)
    else if (ui.prompt_sel == kMenuLen || ui.prompt_sel == kMenuQnt) ui.touched = ui.prompt_sel;   // LENGTH (C) / QUANTISE (D): shown
    else if (ui.prompt_sel == kMenuDir) menu_dir(1), ui.prompt = kPromptMenu;   // rui1050: PLAY MODE, the menu stays
    else if (ui.prompt_sel == kMenuPrint) menu_print();                        // rui1050: PRINT RND
    return;
  }
  if (yes && kind == kPromptMake) {
    // fix1028 (the user's "Ask on make"): its slot's kept notes (an earlier TURN OFF) - asked about once it is on
    // (review: the notes within its length - what plays - and the steps that hold locks only)
    const Track* k = track_of(*cur, r, false);
    int kept = 0, locks = 0, len = k ? eff_len(k, r) : 0;
    for (int i = 0; k && i < len && i < kMaxSteps; ++i) kept += k->step[i].n, locks += !k->step[i].n && k->step[i].nl;
    set_seq(r, true);
    if ((kept || locks) && in_mask(r))
      ui.prompt = kPromptRestore, ui.prompt_track = r, ui.prompt_n = kept, ui.prompt_locks = locks, ui.prompt_pat = cur->index;
  } else if (kind == kPromptRestore) {
    if (!yes) start_empty(r);
  } else if (yes && kind == kPromptOff) set_seq(r, false);
  else if (!yes && (kind == kPromptMake || kind == kPromptExplain) && ui.view_opened) close_view();
}
// rui1037, the step preview (trigs held + YES): the held steps' notes - the whole chord, each note's velocity, length
// and micro offset (a broken chord as written, from now) - on the track's voice through the planner's events (pre_block
// starts them at the next block and ends each after its length, so no note hangs whatever becomes of YES's release;
// release() / STOP drop them as any planned note). The first held step with locks applies them before its notes (as
// plan_step: a track with locks and a step without any gets its bases back); stopped, the bases come back after the
// last note. Nothing advances, nothing is recorded or edited; the transport stopped or playing (a step the sequencer
// plays on top just sounds twice, the voices count their notes). The step's length is the track's step at its tempo
// (the clock's period while it runs; else the reported tempo, else 120 BPM, at the track's speed).
CORE_COLD void preview() {
  int r = ui_track(), vi = voice_of(r);
  Track* t = ui_track_data(false);
  if (!t || vi < 0 || vi >= kVoices) return;
  double per = clk[r].period > 0 ? clk[r].period : step_frames(bpm > 1 ? bpm : 120.0, clk[r].speed);
  Ratio q = rat_of(t);   // P2: the track's own step
  per = per * q.den / q.num;
  int64_t now = (int64_t)hframes, end = now;
  bool any_locks = has_locks(*t), locked = false;
  for (int k = 0; k < 16; ++k) {
    if (!(ui.held >> k & 1)) continue;
    ui.held_used[k] = true;   // no tap toggle at the trig's release
    int idx = ui.page * kStepsPerPage + k;
    if (idx >= kMaxSteps) continue;
    const Step& s = t->step[idx];
    bool notes = s.n && (s.flags & 1);
    if (!locked && (s.nl || (any_locks && notes))) {
      int slot = lset_w++ % kLockSets;
      LockSet& ls = lset[slot];
      ls.n = s.nl > kLocks ? kLocks : s.nl, ls.slide = 0;
      memcpy(ls.l, s.lock, sizeof(Lock) * ls.n);
      push(now, now, ++ev_id, 2, vi, slot, 0, r, false);
      locked = true;
    }
    if (!notes) continue;
    int lo = 0;
    for (int i = 0; i < s.n; ++i) lo = s.note[i].micro < lo ? s.note[i].micro : lo;
    for (int i = 0; i < s.n; ++i) {
      const Note& n = s.note[i];
      int64_t on = now + (int64_t)((n.micro - lo) * per / kSub);
      int64_t off = on + (int64_t)(n.len * per / kSub);
      if (off < on + kBlock) off = on + kBlock;
      uint16_t id = ++ev_id;
      push(on, now, id, 1, vi, n.pitch, n.vel, r, false);
      push(off, now, id, 0, vi, n.pitch, 0, r, false);
      end = off > end ? off : end;
    }
  }
  if (locked && transport != kPlay) {   // stopped: the locked params back to their bases after the last note
    int slot = lset_w++ % kLockSets;
    lset[slot].n = 0, lset[slot].slide = 0;
    push(end, end, ++ev_id, 2, vi, slot, 0, r, false);
  }
  ++st.previews;
}

CORE_COLD void settings_enc(int e, int d, bool func) {
  int r = ui_track();
  switch (e) {
    case 0:
      if (!seqmode) ui.track = clampi(ui.track + d, 0, kRytmTracks - 1), ui.page = 0;
      break;
    case 1: {   // seq-ux: on through the make prompt's checks; UX review R2: off only through the track's menu (FUNC + DOWN, updown1029; DOWN ux1027; YES before) - a
      bool on = d > 0;   // turn down shows how, it never asks by itself
      if (on && !in_mask(r)) ui.prompt = can_seq(r) == 1 ? kPromptMake : kPromptExplain, ui.prompt_track = r;
      else if (!on && in_mask(r)) ui.off_hint_ms = now_ms() | 1;
      break;
    }
    case 2: {
      Track* t = slot_for(r);
      if (!t) break;
      int len = t->length;   // 0 AUTO, then 16 32 .. 128 (FUNC: by one step)
      if (func) len = clampi(len + d, 0, kMaxSteps);
      else if (d > 0) len = clampi((len / 16 + 1) * 16, 0, kMaxSteps);
      else len = len % 16 ? len / 16 * 16 : clampi(len - 16, 0, kMaxSteps);
      set_length(*t, r, len);
      ui.page = clampi(ui.page, 0, npages_of(eff_len(t, r)) - 1);
      mark_dirty();
      break;
    }
    case 3:
      quant = clampi(quant + d, 0, 1), tracks_touch();
      break;
    case 4:
      ui.follow = d > 0;
      break;
    case 5:
      latency = clampi(latency + d * (func ? 480 : 48), 0, 4800);
      if (tracks_touch()) cal.manual = true;   // by hand: no automatic calibration over it (round 3: only if taken)
      break;
    default:
      break;
  }
}

// ---- rui1050: the Rytm side of seqtop (docs/SEQUENCER.md "rui1050"): the TRIG view's pages, the lock pages' COND /
// SLIDE, the poly track menu's PLAY MODE / PRINT RND. Every edit is the model's own field (as the console's seq=spd /
// dir / key / rnd / smod / rtrg / fx / lcond / slide set it), replanned and saved as there.
CORE_COLD void say(const char* m, uint32_t dur) {
  sniprintf(ui.msg, sizeof ui.msg, "%s", m);
  ui.msg_ms = now_ms() | 1, ui.msg_dur = dur;
}
// R1b's SEED MOD source on the track's machine: a param the machine leaves free (rui1050, rytm-mods 0021 / 0026: named
// SEED on the Rytm where it could be - DAISYSP's SMPL B (P11, was FB), DSP SAMPLE's SRC F (P6, was FB; its SMPL page is
// full); 6-OP FM's and JUNO's SMPL C (P12) keep stock's BR label (0021's page code has no room). PLAITS, MULTISAMPLE, WAVETABLE, GRANULAR and the user machines have none: -1.
CORE_COLD int seed_slot(int r) {
  int vb = ui::remote_track_machine(r);
  if (vb < 0 || vb == 0x7F) return -1;
  switch (vb & 0x78) {
    case kDaisy: return 11;
    case kDspSample: return 6;
    case kFm: case kJuno: return 12;
    default: return -1;
  }
}
CORE_COLD void smod_forget(int r) {   // R1b: the offsets sampled under the old setting go (as seq=smod)
  if (r < 0 || r >= kRytmTracks) return;
  smod_last[r] = -1, memset(smh[r], 0, sizeof smh[r]), memset(sml[r], 0, sizeof sml[r]);
}
// The track pages without a held trig (TRIG cycles DEFAULTS -> SCALE -> RANDOM): DEFAULTS' E DIR, F SPD (FUNC: the free
// ratios too), G TLEN (the track's length, as the notes view's C); SCALE's A KEY, B SCL, C QNT (the pattern's); RANDOM's
// A SEED (fully down FREE; FUNC +-100), B RNG (R1's octaves), C WGHT, D MODE, E SRNG (SEED MOD's range; fully down OFF,
// up from OFF its source: the machine's SEED slot), F SQNT, G SAFF. A..D of DEFAULTS are rui1040's (the new steps').
CORE_COLD void track_page_enc(int e, int d, bool func) {
  int r = ui_track();
  Track* t = ui_track_data(true);
  if (!t) return;
  if (ui.tv == kTvDefaults) {
    switch (e) {
      case 0: t->dnote = (uint8_t)clampi(t->dnote + d * (func ? 12 : 1), 0, 127); break;
      case 1: t->dvel = (uint8_t)clampi(t->dvel + d * (func ? 8 : 1), 1, 127); break;
      case 2: t->dlen = (uint16_t)clampi(t->dlen + d * (func ? kSub : kSub / 16), 1, 16 * kSub); break;
      case 3: t->swing = (uint8_t)clampi((t->swing > 50 ? t->swing : 50) + d, 50, 80); break;
      case 4: t->dir = (uint8_t)clampi(t->dir + d, 0, kDirs - 1), replan(r); break;
      case 5: t->speed = (uint8_t)speed_step(t->speed, d, func), replan(r); break;
      case 6: settings_enc(2, d, func); return;   // (marks it dirty)
      default: return;
    }
  } else if (ui.tv == kTvScale) {
    if (e == 0) cur->key = (uint8_t)clampi(cur->key + d, 0, 11);
    else if (e == 1) cur->scale = (uint8_t)clampi(cur->scale + d, 0, kScales - 1);
    else if (e == 2) cur->qnt = (uint8_t)clampi(cur->qnt + d, 0, kQnts - 1);
    else return;
    replan(-1);
  } else if (ui.tv == kTvRandom) {
    switch (e) {
      case 0: {   // the pattern's seed: FREE below 0
        int v = cur->seed_fixed ? cur->seed : -1;
        v = v < 0 && d > 0 ? d - 1 : clampi(v + d * (func ? 100 : 1), -1, 9999);   // (up from FREE: 0 first, FUNC too)
        if (v < 0) cur->seed_fixed = 0, cur->seed = 0;   // (no seed block: v2 / v3 again, as seq=seed:free)
        else cur->seed_fixed = 1, cur->seed = (uint16_t)v;
        replan(-1);
        break;
      }
      case 1: t->rnd_oct = (uint8_t)clampi((t->rnd_oct ? t->rnd_oct : kRndOctDef) + d, 1, 4); break;
      case 2: t->rnd_wght = (uint8_t)clampi(t->rnd_wght + d, 0, kWeights - 1); break;
      case 3: t->rnd_mode = (uint8_t)clampi(t->rnd_mode + d, 0, kRndModes - 1); break;
      case 4:
        if (!t->smod_src) {
          if (d <= 0) return;
          int sl = seed_slot(r);
          if (sl < 1) { say("NO SEED SLOT ON IT"); return; }
          t->smod_src = (uint8_t)sl;
          if (t->smod_range < 2) t->smod_range = kSmodRangeDef;
        } else {
          int sl = seed_slot(r);
          if (sl >= 1 && t->smod_src != sl) t->smod_src = (uint8_t)sl;   // (the machine changed since: its own slot)
          int rg = t->smod_range + d * (func ? 8 : 1);
          if (rg < 2) t->smod_src = 0;
          else t->smod_range = (uint8_t)clampi(rg, 2, 64);
        }
        smod_forget(r);
        break;
      case 5: t->smod_quant = (uint8_t)clampi(t->smod_quant + d, 0, kSqs - 1), smod_forget(r); break;
      case 6: t->smod_aff = (uint8_t)clampi(t->smod_aff + d, 0, kAffs - 1), smod_forget(r); break;
      default: return;
    }
    replan(r);
  }
  mark_dirty();
}
// The NOTE knob under held trigs (the notes view's A and the TRIG page's): a value RND below the lowest note (pitch 0)
// flags the step random (R1: STEPS mode then, when the track's was OFF); turning up from RND writes the notes again
CORE_COLD void note_turn(Track& t, Step& s, int d, bool func) {
  if (s.xf & kXfRnd) {
    if (d > 0) s.xf &= (uint8_t)~kXfRnd;
    return;
  }
  uint8_t was[kNotes];
  int n0 = s.n;
  for (int k = 0; k < s.n; ++k) was[k] = s.note[k].pitch;
  edit_step(t, s, 0, d, func);
  bool moved = s.n != n0;
  for (int k = 0; k < s.n && !moved; ++k) moved = s.note[k].pitch != was[k];
  if (d < 0 && n0 && !moved) {   // already at the bottom (C-1, or the scale's lowest degree): RND
    s.xf |= kXfRnd;
    if (!t.rnd_mode) t.rnd_mode = kRndSteps;
  }
}
// RTRG (P4 / M4): A RATE, B CNT (OFF, 2..16), C VCRV, D PCRV (-24..24), E MODE, F OCT, G GATE (1..100 %)
CORE_COLD void rtrg_turn(Track& t, Step& s, int e, int d, bool func) {
  Rtrg& x = s.rt;
  switch (e) {
    case 0: x.rate = (uint8_t)clampi(x.rate + d, 0, kRtRates - 1); break;
    case 1: {
      int c = x.cnt < 2 ? 0 : x.cnt;
      c = clampi(c + d * (func ? 4 : 1), 0, kRtMax);
      if (c == 1) c = d > 0 ? 2 : 0;
      x.cnt = (uint8_t)c;
      break;
    }
    case 2: x.vcrv = (uint8_t)clampi(x.vcrv + d, 0, kVcs - 1); break;
    case 3: x.pcrv = (int8_t)clampi(x.pcrv + d * (func ? 12 : 1), -24, 24); break;
    case 4: x.mode = (uint8_t)clampi(x.mode + d, 0, kRtModes - 1); break;
    case 5: x.oct = (uint8_t)clampi((x.oct ? x.oct : 1) + d, 1, 4); break;
    case 6: {
      int g = clampi((x.gate ? x.gate : 100) + d * (func ? 10 : 1), 1, 100);
      x.gate = (uint8_t)(g == 100 ? 0 : g);
      break;
    }
    default: return;
  }
  if (!s.n && rt_on(x)) fill_default(t, s, false);
}
// FX (H1 / V3): A CMD1, B VAL1, C CMD2, D VAL2. A new command starts on a value of its own; LOP's VAL turns N (FUNC: X)
CORE_COLD int fx_default(int c) {
  static const int8_t kDef[kFxCmds] = {0, 2, 24, 48, 50, 2, 20, 1, 7, 1, 0, 0, 1, 1, 1};
  return c >= 0 && c < kFxCmds ? kDef[c] : 0;
}
CORE_COLD void fx_turn(Step& s, int e, int d, bool func) {
  if (e > 3) return;
  Fx& f = s.fx[e / 2];
  if (!(e & 1)) {
    int c = clampi(f.cmd + d, 0, kFxCmds - 1);
    if (c != f.cmd) f.cmd = (uint8_t)c, f.val = (uint8_t)(int8_t)fx_default(c);
    return;
  }
  if (!f.cmd) return;
  if (f.cmd == kFxLop) {
    int n = (f.val & 15) + 1, k = (f.val >> 4) + 1;
    if (func) k = clampi(k + d, 1, 16);
    else n = clampi(n + d, 1, 16);
    f.val = (uint8_t)((k - 1) << 4 | (n - 1));
    return;
  }
  int lo = 0, hi = 0;
  fx_range(f.cmd, &lo, &hi);
  bool sgn = f.cmd == kFxOct || f.cmd == kFxTrs;
  int v = sgn ? (int8_t)f.val : f.val;
  v = clampi(v + d * (func ? 8 : 1), lo, hi);
  f.val = (uint8_t)(sgn ? (uint8_t)(int8_t)v : v);
}
// The lock pages (V1 / X1): a long push puts that knob on its lock's COND (the label gets a ?; a turn steps ---, FILL,
// !FILL, 1ST, !1ST, the A:Bs, 1..99 %; another long push: the value again); FUNC + push toggles the lock's SLIDE (on for
// every held step that locks the param, off when all of them slid). Only a step that locks the param changes.
CORE_COLD int lcond_index(int c) {   // the knob's order of the lock condition codes
  static const uint8_t kFirst[5] = {kCondNone, kCondFill, kCondNotFill, kCond1st, kCondNot1st};
  for (int i = 0; i < 5; ++i)
    if (c == kFirst[i]) return i;
  if (c >= kCondAB && c < kCondKnob) return 5 + (c - kCondAB);
  return 0;
}
CORE_COLD int lcond_code(int i) {
  static const uint8_t kFirst[5] = {kCondNone, kCondFill, kCondNotFill, kCond1st, kCondNot1st};
  if (i < 5) return kFirst[i < 0 ? 0 : i];
  return kCondAB + (i - 5);
}
CORE_COLD int lcond_count() { return 5 + (kCondKnob - kCondAB); }
CORE_COLD void lcond_turn(int prm, int d, bool func) {
  Track* t = ui_track_data(false);
  int n = 0;
  for (int k = 0; t && k < 16; ++k) {
    if (!(ui.held >> k & 1)) continue;
    Step& s = t->step[ui.page * kStepsPerPage + k];
    for (int i = 0; i < s.nl; ++i)
      if (s.lock[i].param == prm) {
        s.lock[i].cond = (uint8_t)lcond_code(clampi(lcond_index(s.lock[i].cond) + d * (func ? 8 : 1), 0, lcond_count() - 1));
        ++n;
      }
    ui.held_used[k] = true;
  }
  if (!n) say("NO LOCK: TURN IT FIRST");
  else replan(ui_track()), mark_dirty();   // (a step planned already plays its new condition)
}
CORE_COLD void slide_toggle(int prm) {
  Track* t = ui_track_data(false);
  int locks = 0, sliding = 0;
  for (int pass = 0; pass < 2; ++pass)
    for (int k = 0; t && k < 16; ++k) {
      if (!(ui.held >> k & 1)) continue;
      Step& s = t->step[ui.page * kStepsPerPage + k];
      ui.held_used[k] = true;
      if (lock_of(s, prm) < 0) continue;
      if (!pass) {
        ++locks, sliding += (s.slide >> prm & 1) != 0;
      } else {
        bool on = sliding < locks;
        s.slide = on ? s.slide | 1u << prm : s.slide & ~(1u << prm);
      }
    }
  if (!locks) { say("NO LOCK: TURN IT FIRST"); return; }
  say(sliding < locks ? "SLIDE ON (TO NEXT LOCK)" : "SLIDE OFF");
  replan(ui_track());
  mark_dirty();
}
// The poly track menu's PLAY MODE (the direction: d +-1, wrapping) and PRINT RND (the loop playing, else the first)
CORE_COLD void menu_dir(int d) {
  Track* t = ui_track_data(true);
  if (!t) return;
  t->dir = (uint8_t)((t->dir + d + kDirs) % kDirs);
  replan(ui_track());
  mark_dirty();
}
CORE_COLD void menu_print() {
  int r = ui_track();
  const Track* t = track_of(*cur, r);
  int n = 0;
  for (int i = 0; t && t->rnd_mode && i < kMaxSteps; ++i)
    n += t->step[i].n && (t->rnd_mode == kRndTrack || (t->step[i].xf & kXfRnd));
  if (!n) { say("NO RANDOM NOTES TO PRINT"); return; }
  char line[24], out[96];
  sniprintf(line, sizeof line, CORE_STR("seq=print:%d"), r + 1);
  command(line, out, sizeof out);
  char b[28];
  sniprintf(b, sizeof b, CORE_STR("PRINTED: %d STEP%s"), n, n == 1 ? "" : "S");
  say(b);
}

// P2: the track's own step playing now (its count since PLAY: the Rytm's last tick, interpolated toward the next one)
CORE_COLD double vnow(int r, const Track* t) {
  double x = (double)play_abs[r];
  if (clk[r].period > 0 && fol.valid) {
    double y = clk[r].step_at((double)fol.to_rytm((int64_t)hframes));
    x = y < x ? x : y > x + 1 ? x + 1 : y;
  }
  Ratio q = rat_of(t);
  return q.num == q.den ? x : x * q.num / q.den;
}
CORE_COLD int play_step_of(int r, const Track* t) {
  if (transport != kPlay || !clk[r].on || play_abs[r] < 0) return -1;
  Ratio q = rat_of(t);
  int64_t v = q.num == q.den ? play_abs[r] : (int64_t)vnow(r, t);
  return map_step(*cur, t, r, v).index;
}

}  // namespace

CORE_COLD void init() {
  if (!cur) cur = new Pattern, nxt = new Pattern, sav = new Pattern, clip_p = new Pattern, undo_p = new Pattern;
  // the notes as the voices hear them (core/poly.h set_note_sink): the Rytm's mono pads' steals left out, a pad
  // chord's note-offs at its release, so a recorded chord keeps its pads' times and its group's length
  poly::set_note_sink(midi);
  if (fx::knob_filter != fx_filter) fx_prev = fx::knob_filter, fx::knob_filter = fx_filter;   // seq-ux: FX locks, the LFOs after
  reset();
  if (card_ok && project[0]) {   // the last project (/.rytm/project): its seq tracks and A01 until the Rytm says more
    named = true;
    sniprintf(g_proj, sizeof g_proj, "%s", project);
#if CORE_MSAMP
    // the RAM store (phase 2): its tracks and A01 once the boot's fill is done (core/pool/pool.h project_fill_step ->
    // project_changed); until then none of its files is loaded here, so none is written over (1042)
    g_loading = true, tracks_keep();
    pat_lock[0] = kLockLoading;
#else
    load_tracks();
    cur->index = -1;
    install(0);
#endif
  }
}

CORE_COLD void reset() {
  if (!cur) cur = new Pattern, nxt = new Pattern, sav = new Pattern, clip_p = new Pattern, undo_p = new Pattern;
  clear(*cur, 0), clear(*nxt, -1), clear(*sav, -1), clear(*clip_p, -1), clear(*undo_p, -1);
  undo_p_kind = 0;
  memset(&clip_t, 0, sizeof clip_t), memset(&undo_t, 0, sizeof undo_t);
  clip_t.rytm = -1, undo_t_kind = 0, undo_t_track = -1, undo_t_pat = -1;
  clip_g_valid = false, undo_g_kind = 0, undo_g_track = -1, undo_g_pat = -1, undo_g_page = -1, undo_g_edits = 0;
  clip_r_n = 0, undo_r_mask = 0, undo_r_kind = 0, undo_r_track = -1, undo_r_pat = -1, undo_r_page = -1, undo_r_edits = 0;
  for (int r = 0; r < kRytmTracks; ++r) sc_len[r] = sc_spd[r] = -1, heard_ms[r] = 0, pred_abs[r] = -1, pred_planned[r] = 0,
                                        restart_skip[r] = 0;
  pat_len = pat_spd = pat_mode = pat_chg = -1, fill = false;
  pin_extra = 0, pin_seq = true, pin_sent = -1, silent_sent = 0xFFFF, clock_extra = 0, clock_sent = -1;
  cal.state = 0, cal.vi = -1, cal.track = -1, cal.n = 0, cal.found = -1, cal.auto_done = false, cal.manual = false;
  cal.fed_ms = 0;
  predict = true, pred_to = -1, pat_start = -1, pat_start_track = -1;
  rytm_dump::reset();
  sav_pending = false;
  fol.reset();
  for (int r = 0; r < kRytmTracks; ++r) clk[r].reset(), restart[r] = true, planned[r] = 0, play_abs[r] = -1;
  set_transport(kStop);   // a take recording through a project load / hello ends (FC), as at the Rytm's STOP
  hframes = 0, transport_frame = INT64_MIN, bpm = 0, mask = 0, legacy_mask = 0, clock_now = 0, mask_sent = -1, hello_due = true, mutes = 0, solos = 0;
  out::set_mutes(0);
  rec_live = rec_grid = false;
  pat_now = 0, pat_queued = -1, pat_loaded = 0, load_due = false;
  seqmode = false, seqmode_track = -1, we_opened = false, show_wanted = false, named = false;
  latency = kDefaultLatency, quant = 0;
  seed_held = false;
  smod_reset(), seg_gen = 0;   // seqtop R1b
  played_n = 0;
  nev = 0, ev_id = 0;
  memset(sounding, 0, sizeof sounding);
  memset(opens, 0, sizeof opens);
  dirty = save_now = tracks_dirty = false;
  ui = Ui();
  memset(vlk, 0, sizeof vlk), memset(&flk, 0, sizeof flk), lset_w = 0;   // seq-ux
  memset(slides, 0, sizeof slides), slide_any = 0;                         // seqtop X1
  view_gen = 0, view_track = -1;
  memset(&st, 0, sizeof st);
  st.cal_last = -1;
}

CORE_COLD bool sysex(const uint8_t* d, int len) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  if (len < 8 || (d[4] != kTickCmd && d[4] != kStateCmd) || d[len - 1] != 0xF7) return false;
  if (d[5] != 1) {
    ++st.ticks_bad;
    return true;
  }
  const uint8_t* b = d + 6;
  int n = len - 7;
  if (d[4] == kTickCmd) {
    if (n < 5 || b[4] < 1 || n != 5 + 4 * b[4]) {
      ++st.ticks_bad;
      return true;
    }
    on_ticks(b, n);
  } else {
    on_state(b, n);
  }
  return true;
}

CORE_COLD void set_clock_tracks(uint16_t m) { clock_extra = (uint16_t)(m & 0xFFF); }

CORE_COLD void hello() {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  hello_due = true;
  mask_sent = -1, pin_sent = -1, clock_sent = -1, silent_sent = 0xFFFF;
  ui.leds_sent = false, ui.pleds_sent = false;
  ui.func = false;   // rui1050: a FUNC release lost with the link
  ++st.resyncs;
}

// 1042 (data safety): pool.cpp's set_project takes the new name before project_changed runs, so project_changed's saves
// of the old project's unsaved edit went into the NEW project's folder (folder() reads the name), over its file of the
// same index. Called before the name changes: the old project's edits saved in its own folder.
CORE_COLD int pattern_locked(int index) { return lock_of(index); }
#if CORE_MSAMP
CORE_COLD void want_files() {
  char d[64], fn[96];
  folder_of(project, d, sizeof d);
  sniprintf(fn, sizeof fn, CORE_STR("%s/tracks.txt"), d);
  store::want(fn);
  for (int i = 0; i < 128; ++i) file_ext_of(project, fn, sizeof fn, i, "sqp"), store::want(fn);
  for (int i = 0; i < 128; ++i) file_ext_of(project, fn, sizeof fn, i, "sqs"), store::want(fn);   // 1042's saved states
}
CORE_COLD void rekey(const char* name) { sniprintf(g_proj, sizeof g_proj, "%s", name ? name : ""); }
CORE_COLD void loading_begin() {
  if (!project[0]) return;   // (the boot with no project named: nothing of one to keep apart)
  if (!cur) reset();
  g_loading = true;
  tracks_keep();
  nxt->index = -1;   // round 2: an old pattern prefetched before the switch never comes up editable during the fill
}
#endif
CORE_COLD void project_leaving() {
  if (!cur || !named) return;   // not named yet: project_changed keeps the edits for the first name
  if (tracks_dirty) save_tracks(), tracks_dirty = false;   // fix1049: a settling tracks.txt into its own folder too
  if (dirty && cur->index >= 0) save_pattern(*cur), dirty = false;
  if (sav_pending) save_pattern(*sav), sav_pending = false;
}

CORE_COLD void project_changed() {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  // NOTE EDIT (review round 2, N4): the undo history and the state after the last gesture are the old project's
  if (ne_hooks && ne_hooks->clear) ne_hooks->clear();
  ne_after_on = false, ui.ne_undo_e = -1, ui.ne_del_ms = 0;
  g_loading = false;   // (the store is full: the project's files are here now)
  bool was_named = named;
  if (!named) {   // the first project the Rytm names: what was set before (the boot's "", a console) is this one's
    if (!project[0]) return;   // (the boot's fill with no project named: nothing named yet)
    named = true;
    memset(pat_lock, 0, sizeof pat_lock);   // 1042: another folder's files
    sniprintf(g_proj, sizeof g_proj, "%s", project);
    char d[64], fn[96];
    folder(d, sizeof d);
    sniprintf(fn, sizeof fn, CORE_STR("%s/tracks.txt"), d);
#if CORE_MSAMP
    const uint8_t* td;
    uint32_t tn;
    bool has = card_ok && store::get(fn, &td, &tn) != store::kAbsent;   // (unknown: taken as there - never a guess)
#else
    File f;
    bool has = card_ok && f.open(fn, kRead);
    if (has) f.close();
#endif
    if (!has && (legacy_mask || dirty)) {
      tracks_touch();
      if (dirty) dirty_ms = now_ms();
      log(CORE_STR("seq: the tracks set before the project was named kept for %s"), project);
      return;
    }
  }
  // the old project's unsaved edits into its own folder: g_proj still names it and its locks still hold (1042; with the
  // RAM store this runs once the new project's store is full, and an edit made meanwhile is the old project's)
  if (dirty && cur->index >= 0) save_pattern(*cur), dirty = false;
  if (sav_pending) save_pattern(*sav), sav_pending = false;
  // its tracks.txt too (as project_leaving: a settling edit is never overwritten by the load below and lost); not
  // before the first name - those settings are the named project's only when it has none (above)
  if (was_named && tracks_dirty) save_tracks(), tracks_dirty = false;
  memset(pat_lock, 0, sizeof pat_lock);   // 1042: another folder's files
  disarm_undo(-1);   // (round 2: an undo of another project's pattern, or of a refused edit, never lands on this one's)
  sniprintf(g_proj, sizeof g_proj, "%s", project);   // the sequencer's files are this project's from here
  release(-1);
  load_tracks();
  mask_sent = -1;
  nxt->index = -1;
  pat_loaded = -1;
  cur->index = -1;
  install(pat_now);   // its file now (from the store with CORE_MSAMP; a small read without)
  load_due = false;
}

CORE_COLD bool owns_voice(int vi) {
  if (!mask) return false;
  for (int r = 0; r < kRytmTracks; ++r)
    if ((mask >> r & 1) && voice_of(r) == vi) return true;
  return false;
}

CORE_COLD uint64_t frames() { return hframes; }
CORE_COLD bool any_tracks() { return mask != 0; }
CORE_COLD bool shown_wanted() { return show_wanted; }

void pre_block() {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  int64_t h = (int64_t)hframes;
  if (nev) {
    int64_t due = h + kBlock / 2;   // a block's events at its start: within half a block either way
    // note-offs first (a retrigger of the same pitch then starts after its own end), then a step's locks (seq-ux),
    // then note-ons (they start with their step's locks)
    static const uint8_t kPass[3] = {0, 2, 1};
    for (int pass = 0; pass < 3; ++pass)
      for (int i = 0; i < nev;) {
        Ev& e = ev[i];
        if (e.f < due && e.on == kPass[pass]) {
          if (e.on == 2) apply_set(e.track, &lset[e.note % kLockSets]);
          else if (e.on) {
            note_on(e.vi, e.note, e.vel, e.track);
            if (e.step != 0xFF && e.track < kRytmTracks)   // seqtop: the order played (seq=played)
              played[played_n++ % kPlayed] = {(int8_t)e.track, e.step, e.note, e.v, (int32_t)play_abs[e.track]};
          }
          else note_off(e.vi, e.note);
          ev[i] = ev[--nev];
        } else {
          ++i;
        }
      }
  }
  slide_tick();   // seqtop X1
  plan();
  hframes += kBlock;
}

CORE_COLD void midi(uint8_t status, uint8_t d1, uint8_t d2) {
  int kind = status & 0xF0, ch = status & 0x0F;
  if (kind != 0x90 && kind != 0x80) return;
  int off = 0, vi = poly::bound_voice(ch, &off);
  note(vi, kind == 0x90 && d2, clampi(d1 + (vi >= 0 ? off : 0), 0, 127), d2, -1);
}

CORE_COLD bool owns_track(int t) { return in_mask(t); }

CORE_COLD void params_in(int vi, float* p, int np) {
  if (vi < 0 || vi >= kVoices) return;
  VLock& L = vlk[vi];
  for (int k = 0; k < np && k < 16 && L.on; ++k)
    if (L.on >> k & 1) L.base[k] = p[k], p[k] = L.val[k];
}
CORE_COLD int seqmode_on() { return seqmode && seqmode_track >= 0 && seqmode_track < kRytmTracks ? seqmode_track : -1; }
CORE_COLD bool rytm_live_rec() { return rec_live && transport == kPlay; }

CORE_COLD void note(int vi, bool on, int pitch, int vel, int track) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  uint8_t d2 = (uint8_t)vel;
  pitch = clampi(pitch, 0, 127);
  uint32_t ms = now_ms();
  // the last chord played (a tap's notes, a held empty step's first turn): notes a Sidecar voice plays only. dbg: the
  // Rytm's own channel notes (an analog track's trig, a pad on an unbound channel: no voice) came in here too, so a chord
  // made by turning on an empty step started from such a note - C0 on hardware (Pi fe1e176) - not from the track's
  // default note
  if (on && vi >= 0) {
    if (ms - ui.chord_ms > kChordGapMs) ui.chord_n = 0;
    ui.chord_ms = ms;
    bool seen = false;
    for (int i = 0; i < ui.chord_n; ++i) seen |= ui.chord[i] == pitch;
    if (!seen && ui.chord_n < kNotes) ui.chord[ui.chord_n] = (uint8_t)pitch, ui.chord_vel[ui.chord_n++] = d2;
  }
  // step entry: a trig held, the pads / keys set its notes
  if (ui.held && (seqmode || seq_shown()) && on) {
    Track* t = ui_track_data(true);
    if (!t) return;
    for (int k = 0; k < 16; ++k) {
      if (!(ui.held >> k & 1)) continue;
      Step& s = t->step[ui.page * kStepsPerPage + k];
      if (ui.held_first[k]) s.n = 0, s.flags = 1, s.prob = 100, s.chd = 0, s.inv = 0, s.strum = 0, ui.held_first[k] = false;
      add_note(s, new_note(*t, pitch, d2));
      ui.held_used[k] = true;
    }
    mark_dirty();
    return;
  }
  if (!rec_live || transport != kPlay || vi < 0) return;
  int r = -1;   // the seq track on that voice (the caller's, then the one shown, then the first)
  if (in_mask(track) && voice_of(track) == vi) r = track;
  if (r < 0 && in_mask(ui_track()) && voice_of(ui_track()) == vi) r = ui_track();
  for (int t = 0; t < kRytmTracks && r < 0; ++t)
    if (in_mask(t) && voice_of(t) == vi) r = t;
  if (r < 0 || !clk[r].on || clk[r].period <= 0) return;
  Track* t = slot_for(r);
  if (!t) return;
  Ratio q = rat_of(t);   // P2: recorded in the track's own steps
  double x = clk[r].step_at((double)fol.to_rytm((int64_t)hframes)) * q.num / q.den;
  if (on) {
    int64_t sa = (int64_t)(x + 0.5 + 1e6) - 1000000;   // the nearest step (floor of x + 0.5)
    int micro = quant ? 0 : clampi((int)((x - (double)sa) * kSub + (x >= (double)sa ? 0.5 : -0.5)), -kSub / 2, kSub / 2);
    if (sa < 0) sa = 0;
    int idx = map_step(*cur, t, r, sa).index;
    Note n = new_note(*t, pitch, d2);
    n.micro = (int8_t)micro;
    n.fresh = sa >= planned[r] ? 1 : 0;   // its step is not planned yet: not again on top of the pad's own sound
    add_note(t->step[idx], n);
    for (Open& o : opens)
      if (!o.used) {
        o = {true, (int8_t)r, (uint8_t)pitch, idx, x};
        break;
      }
    ++st.recorded;
    mark_dirty();
  } else {
    for (Open& o : opens) {
      if (!o.used || o.track != r || o.pitch != pitch) continue;
      o.used = false;
      Step& s = t->step[o.idx];
      int l = clampi((int)((x - o.x) * kSub + 0.5), 1, 16 * kSub);
      for (int k = 0; k < s.n; ++k)
        if (s.note[k].pitch == pitch) s.note[k].len = (uint16_t)l;
      mark_dirty();
    }
  }
}

// ---- U1 NOTE EDIT (docs/SEQ-IDEAS.md U1, docs/SEQUENCER.md "Built: NOTE EDIT"): DATA walks a highlight through the
// shown poly track's notes in play order (a note is (step idx, pitch): a step never holds a pitch twice, add_note); A..E
// edit the selection when it has notes, else the highlighted note. The selection is a pitch bitmap a step, so it follows
// edits that move notes (ne_move) and is dropped with another pattern / track.
CORE_COLD bool ne_sel_has(int idx, int p) { return idx >= 0 && idx < kMaxSteps && p >= 0 && p < 128 && (ui.ne_sel[idx][p >> 5] >> (p & 31) & 1); }
CORE_COLD void ne_sel_set(int idx, int p, bool on) {
  if (idx < 0 || idx >= kMaxSteps || p < 0 || p > 127) return;
  if (on) ui.ne_sel[idx][p >> 5] |= 1u << (p & 31);
  else ui.ne_sel[idx][p >> 5] &= ~(1u << (p & 31));
}
CORE_COLD void ne_sel_clear() { memset(ui.ne_sel, 0, sizeof ui.ne_sel), ui.ne_nsel = 0; }
CORE_COLD bool ne_has(const Step& s, int p) {
  for (int k = 0; k < s.n; ++k)
    if (s.note[k].pitch == p) return true;
  return false;
}
// the notes DATA walks, in play order (key idx << 7 | pitch, ascending): scope -1 the track's steps up to its length, else
// that step's
CORE_COLD int ne_list(const Track* t, int len, int scope, uint16_t* out) {
  int n = 0;
  if (!t) return 0;
  int a = scope >= 0 ? scope : 0, b = scope >= 0 ? scope + 1 : len;
  for (int i = a; i < b && i < kMaxSteps; ++i) {
    const Step& s = t->step[i];
    uint8_t p[kNotes];
    int m = 0;
    for (int k = 0; k < s.n && m < kNotes; ++k) {
      int j = m++;
      p[j] = s.note[k].pitch;
      for (; j > 0 && p[j] < p[j - 1]; --j) { uint8_t x = p[j]; p[j] = p[j - 1], p[j - 1] = x; }
    }
    for (int k = 0; k < m; ++k) out[n++] = (uint16_t)(i << 7 | p[k]);
  }
  return n;
}
uint16_t ne_buf[kMaxSteps * kNotes];
CORE_COLD void ne_bump() { ++ui.ne_gen; }
CORE_COLD void ne_go(int idx, int p) {
  if (ui.ne_idx != idx || ui.ne_pitch != p) ne_bump();
  ui.ne_idx = idx, ui.ne_pitch = p;
  if (idx >= 0) ui.page = idx / kStepsPerPage;   // the page follows the highlight
}
CORE_COLD int ne_scope() {   // the first held step (its chord is DATA's walk), -1 none
  for (int k = 0; k < 16; ++k)
    if (ui.held >> k & 1) return ui.page * kStepsPerPage + k;
  return -1;
}
// The highlight against the data as it is now: another track or pattern drops the selection; a note that is gone
// (the touchscreen, an undo) is looked up as the first note at or after its step (else the last one). The track no
// longer a poly track: NOTE EDIT off. True: a note is highlighted.
CORE_COLD void ne_recount() {
  int c = 0;
  for (int i = 0; i < kMaxSteps; ++i)
    for (int w = 0; w < 4; ++w) c += __builtin_popcount(ui.ne_sel[i][w]);
  ui.ne_nsel = c;
}
// review round 1: selection bits that name no note any more (the touchscreen, an undo, the Rytm's edits) dropped
CORE_COLD void ne_sel_prune(const Track* t) {
  if (!ui.ne_nsel) return;
  for (int i = 0; i < kMaxSteps; ++i) {
    uint32_t keep[4] = {0, 0, 0, 0};
    if (t)
      for (int k = 0; k < t->step[i].n; ++k) {
        int p = t->step[i].note[k].pitch;
        keep[p >> 5] |= 1u << (p & 31);
      }
    for (int w = 0; w < 4; ++w) ui.ne_sel[i][w] &= keep[w];
  }
  ne_recount();
}
CORE_COLD bool ne_fix() {
  int r = ui_track();
  if (!ui.ne) return false;
  if (!in_mask(r)) {
    ui.ne = false, ne_sel_clear(), ne_bump();
    return false;
  }
  if (r != ui.ne_track || cur->index != ui.ne_pattern) {
    ui.ne_track = r, ui.ne_pattern = cur->index;
    ne_sel_clear(), ne_bump();
    ui.ne_undo_e = -1, ui.ne_del_ms = 0;   // (round 2: no gesture or question carries over)
  }
  const Track* t = track_of(*cur, r);
  if (ui.ne_edits != st.edits) {   // the data changed (here or elsewhere): stale selection bits go
    ui.ne_edits = st.edits;
    int was = ui.ne_nsel;
    ne_sel_prune(t);
    if (was != ui.ne_nsel) ne_bump();
  }
  int len = eff_len(t, r), n = ne_list(t, len, -1, ne_buf);
  if (!n) {
    if (ui.ne_idx >= 0) ui.ne_idx = -1, ne_bump();
    return false;
  }
  if (ui.ne_idx >= 0 && ui.ne_idx < len && ne_has(t->step[ui.ne_idx], ui.ne_pitch)) return true;
  int want = ui.ne_idx < 0 ? 0 : ui.ne_idx << 7 | (ui.ne_pitch & 0x7F), k = 0;
  while (k < n && ne_buf[k] < want) ++k;
  if (k == n) k = n - 1;
  ui.ne_idx = -1;
  ne_go(ne_buf[k] >> 7, ne_buf[k] & 0x7F);
  return true;
}
// Into NOTE EDIT (DATA turned or pushed on a poly track's SEQ page): a held trig's lowest note; else the last
// highlighted note if it is still there and on the page shown, else the page's first note (after the last: the
// pattern's first) - docs/SEQ-IDEAS.md U1
CORE_COLD void ne_land() {
  int r = ui_track();
  const Track* t = track_of(*cur, r);
  int len = eff_len(t, r), n = ne_list(t, len, -1, ne_buf);
  int pg = clampi(ui.page, 0, npages_of(len) - 1);
  bool keep = ui.ne_track == r && ui.ne_pattern == cur->index && ui.ne_idx >= 0 && ui.ne_idx < len && t &&
              ne_has(t->step[ui.ne_idx], ui.ne_pitch) && ui.ne_idx / kStepsPerPage == pg;
  if (ui.ne_track != r || ui.ne_pattern != cur->index) ne_sel_clear();
  ui.ne_track = r, ui.ne_pattern = cur->index, ui.ne_edits = st.edits;
  if (!ui.ne) ui.ne = true, ui.tv = kTvNotes, ui.sv = kSvNotes, ui.ne_undo_e = -1, ui.ne_del_ms = 0, ne_bump();
  int sc = ne_scope();
  if (sc >= 0 && sc < len && t && t->step[sc].n) {   // review round 1: a trig held - its step
    ne_list(t, len, sc, ne_buf);
    ui.ne_idx = -1;
    ne_go(sc, ne_buf[0] & 0x7F);
    return;
  }
  if (keep) return;
  if (!n) {
    ui.ne_idx = -1, ui.ne_pitch = -1;
    return;
  }
  int k = 0;
  while (k < n && (ne_buf[k] >> 7) < pg * kStepsPerPage) ++k;
  if (k == n) k = 0;
  ui.ne_idx = -1;
  ne_go(ne_buf[k] >> 7, ne_buf[k] & 0x7F);
}
// DATA turned: d notes along (func: d steps along), in the held step's chord while one is held; no wrap
CORE_COLD void ne_turn(int d, bool func) {
  int r = ui_track();
  ui.ne_undo_e = -1, ui.ne_del_ms = 0;   // (round 2: a new gesture; FUNC + NO asks again)
  if (!ui.ne) {
    ne_land();
    log(CORE_STR("seq: NOTE EDIT on track %d: S%d pitch %d"), r + 1, ui.ne_idx + 1, ui.ne_pitch);
    return;
  }
  bool was = ui.ne_idx >= 0 && ui.ne_track == r && ui.ne_pattern == cur->index;
  const Track* t0 = track_of(*cur, r);
  if (was && !(ui.ne_idx < eff_len(t0, r) && t0 && ne_has(t0->step[ui.ne_idx], ui.ne_pitch))) {
    ne_fix();   // the note went meanwhile: this turn lands on the next one
    return;
  }
  int sc = ne_scope();
  if (sc < 0 && ui.ne_idx >= 0 && ui.ne_idx / kStepsPerPage != ui.page) {   // PAGE moved away: the page's first note
    ne_land();
    return;
  }
  if (!ne_fix()) return;
  const Track* t = track_of(*cur, r);
  int n = ne_list(t, eff_len(t, r), sc, ne_buf);
  if (!n) return;
  int cur_k = 0, want = ui.ne_idx << 7 | ui.ne_pitch;
  while (cur_k < n - 1 && ne_buf[cur_k] < want) ++cur_k;
  int k = cur_k;
  if (func && sc < 0) {   // by step: the first note of the next / previous step with notes
    int step = ui.ne_idx;
    for (int i = 0; i < (d < 0 ? -d : d); ++i) {
      if (d > 0) {
        while (k < n && (ne_buf[k] >> 7) == step) ++k;
        if (k == n) { k = cur_k; break; }
        step = ne_buf[k] >> 7, cur_k = k;
      } else {
        while (k > 0 && (ne_buf[k] >> 7) >= step) --k;
        if ((ne_buf[k] >> 7) >= step) { k = cur_k; break; }
        step = ne_buf[k] >> 7;
        while (k > 0 && (ne_buf[k - 1] >> 7) == step) --k;
        cur_k = k;
      }
    }
  } else {
    k = clampi(k + d, 0, n - 1);
  }
  ne_go(ne_buf[k] >> 7, ne_buf[k] & 0x7F);
}
// The targets (keys into out, play order): a trig held - its highlighted note, the selection ignored (review round 1);
// else the selection when it has notes, else the highlighted note. How many.
CORE_COLD int ne_targets(const Track& t, int len, uint16_t* out) {
  int n = 0;
  bool held = ne_scope() >= 0;
  if (ui.ne_nsel && !held) {
    for (int i = 0; i < len && i < kMaxSteps; ++i)
      for (int k = 0; k < t.step[i].n; ++k)
        if (ne_sel_has(i, t.step[i].note[k].pitch)) out[n++] = (uint16_t)(i << 7 | t.step[i].note[k].pitch);
    if (n) return n;
  }
  if (ui.ne_idx >= 0 && ui.ne_idx < len && (!held || ui.ne_idx == ne_scope()) && ne_has(t.step[ui.ne_idx], ui.ne_pitch))
    out[n++] = (uint16_t)(ui.ne_idx << 7 | ui.ne_pitch);
  return n;
}
// ---- review rounds 1 / 2: the undo levels go to the platform's history (seq.h UndoHooks; the Pi: pi/ui/seq_touch's).
// A gesture's level is the track before it, saved only when the gesture changed something. One level a gesture: the
// same knob on the same note (the highlight as the last detent left it) of the same track and pattern, within
// kNeUndoMs, with that track unchanged since (N2). The track after the last gesture is kept (ne_after): when its
// content changed outside NOTE EDIT since (the step pages, a pad into a held trig, live recording, the Rytm's own ops;
// round 3: the track's bytes against the snapshot, not the global edit count - an edit elsewhere is not one), that
// state becomes a level before the next level, an undo, another pattern (install) or another track's gesture, so an
// UNDO takes back the outside edit alone (N3). The touchscreen's UNDO / REDO end the snapshot (undo_prepare).
CORE_COLD int ne_key() { return ui.ne_idx << 7 | (ui.ne_pitch & 0x7F); }
CORE_COLD bool ne_after_changed() {   // the snapshot's track (in the current pattern) is not what the snapshot holds
  if (!ne_after_on || !cur || ne_after_p != cur->index) return false;
  const Track* t = track_of(*cur, ne_after_r);
  return !t || memcmp(t, &ne_after, sizeof ne_after) != 0;
}
CORE_COLD void ne_snapshot(const Track& t, int r) {
  memcpy(&ne_after, &t, sizeof ne_after);   // (whole bytes: the comparison above is a memcmp)
  ne_after_on = true, ne_after_r = r, ne_after_p = cur->index;
}
CORE_COLD void ne_cover_outside() {
  if (!ne_after_changed()) return;
  if (ne_hooks && ne_hooks->save) ne_hooks->save(ne_after_r, ne_after_p, ne_after);
  ne_after_on = false;
}
// a gesture of kind (a knob 0..4, -2 a fill, -3 a delete) begins: true when it needs a level (the track then in ne_pre)
CORE_COLD bool ne_begin(int kind) {
  uint32_t ms = now_ms();
  int r = ui_track();
  bool same = kind >= 0 && kind == ui.ne_undo_e && ms - ui.ne_undo_ms < kNeUndoMs && ui.ne_undo_r == r &&
              ui.ne_undo_p == cur->index && ui.ne_undo_key == ne_key() && ne_after_on && ne_after_r == r &&
              ne_after_p == cur->index && !ne_after_changed();
  if (same) return false;
  ne_cover_outside();
  const Track* t = track_of(*cur, r);
  if (t) ne_pre = *t;
  return t != nullptr;
}
// the gesture done (after its mark_dirty): its level saved if it needed one and changed something; the state after it
CORE_COLD void ne_commit(int kind, bool need, bool done) {
  if (!done) return;   // a refused gesture: no level
  int r = ui_track();
  if (need && ne_hooks && ne_hooks->save) ne_hooks->save(r, cur->index, ne_pre);
  ui.ne_undo_e = kind, ui.ne_undo_ms = now_ms(), ui.ne_undo_r = r, ui.ne_undo_p = cur->index, ui.ne_undo_key = ne_key();
  const Track* t = track_of(*cur, r);
  if (t) ne_snapshot(*t, r);
}
CORE_COLD void ne_undo() {
  int r = ui_track();
  Track* t = ui_track_data(false);
  ui.ne_undo_e = -1;
  if (g_loading || lock_of(cur->index) == kLockLoading) {   // refused before a level is taken (round 2)
    say("PROJECT LOADING");
    return;
  }
  ne_cover_outside();
  if (!t || !ne_hooks || !ne_hooks->undo || !ne_hooks->undo(r, cur->index, *t)) {
    say("NOTHING TO UNDO");
    return;
  }
  ne_sel_prune(t);
  ne_fix();
  replan(r), mark_dirty(), ne_bump();
  ne_snapshot(*t, r);
  say("UNDONE");
}
// fix1028: RESTORE n NOTES? answered NO - the kept steps cleared as an edit: the track before it a level in the
// platform's history (as a NOTE EDIT gesture's), so FUNC + YES on the SEQ page or the touchscreen's UNDO bring them back
CORE_COLD void start_empty(int r) {
  if (!cur || !in_mask(r)) return;   // (review: prompt_answer dropped a question of another pattern / track already)
  if (g_loading || lock_of(cur->index) == kLockLoading) {   // review: refused, and said
    say("PROJECT LOADING: NOTES KEPT");
    return;
  }
  Track* t = track_of(*cur, r, false);
  bool any = false;
  for (int i = 0; t && i < kMaxSteps && !any; ++i) any = step_used(t->step[i]);
  if (!any) {   // review: emptied meanwhile (the touchscreen, the console) - no level that undoes nothing
    say("EMPTY");
    return;
  }
  bool hist = ne_hooks && ne_hooks->save;
  ne_cover_outside();
  if (hist) ne_hooks->save(r, cur->index, *t);
  clear_steps(*t, 0, kMaxSteps);
  replan(r), mark_dirty(), ne_bump();
  ne_snapshot(*t, r);
  log(CORE_STR("seq: track %d made poly again, its kept notes cleared%s"), r + 1, hist ? " (an undo level)" : " (no history)");
  say(hist ? "EMPTY  FUNC+YES: UNDO" : "EMPTY");
}
CORE_COLD bool shown_poly() { return cur && in_mask(ui_track()); }
// Bitmaps of one ne_move call (O(n) a shift): the targets, the landings this shift (cleared at each call and after each
// shift: N1); the notes each step keeps, those arriving this shift; each target's landing this shift (np cached)
uint32_t ne_tbit[kMaxSteps][4], ne_land_bit[kMaxSteps][4];
uint8_t ne_stay[kMaxSteps], ne_arr[kMaxSteps];
uint16_t ne_tg[kMaxSteps * kNotes];
int16_t ne_ni[kMaxSteps * kNotes];
uint8_t ne_np[kMaxSteps * kNotes], ne_di[kMaxSteps * kNotes];
uint32_t ne_work = 0;
// the pattern's scale as a table (I6 round 2: a degree step is O(1)): its pitches 0..127 ascending, each pitch's
// quantised degree index
uint8_t ne_deg_tab[128], ne_deg_of[128];
int ne_deg_n = 0, ne_deg_key = -1, ne_deg_scale = -1;
CORE_COLD void ne_deg_build() {
  if (ne_deg_key == cur->key && ne_deg_scale == cur->scale) return;
  ne_deg_key = cur->key, ne_deg_scale = cur->scale, ne_deg_n = 0;
  for (int p = 0; p < 128; ++p)
    if (in_scale(p, cur->key, cur->scale)) ne_deg_tab[ne_deg_n++] = (uint8_t)p;
  for (int p = 0, k = 0; p < 128; ++p) {
    int q = quantise(p, cur->key, cur->scale);
    while (k + 1 < ne_deg_n && ne_deg_tab[k] < q) ++k;
    int j = k;
    while (j > 0 && ne_deg_tab[j] > q) --j;
    ne_deg_of[p] = (uint8_t)j;
  }
}
CORE_COLD bool bit_has(const uint32_t (*b)[4], int i, int p) { return b[i][p >> 5] >> (p & 31) & 1; }
// the shifts a detent tries at most (I6 round 2): past them the footer says NO ROOM THERE / NO PITCH FREE
constexpr int kNeTriesPos = 16, kNeTriesSemi = 12, kNeTriesDeg = 7, kNeTriesOct = 2;
// A POS / D PTCH: every target moved by shift (steps / semitones / scale degrees), the whole shape at once. A shift
// where a note would land on a pitch its step keeps, on another moved note (I2: degrees can merge two notes), or past
// kNotes a step is skipped (the shift grows, kNeTries* at most); the track's ends (step 1 .. the length, pitch 0..127)
// stop it. POS: a step whose every note moves, into a step that will be free, moves whole (its trig: condition, locks,
// ratchet, FX columns, CHD / INV / strum); else its notes go alone and a step left with locks stays a lock trig
// (C1 / I1). True: moved.
CORE_COLD bool ne_move(Track& t, int len, bool pos, int d, bool func) {
  ne_work = 0;
  int n = ne_targets(t, len, ne_tg);
  if (!n || !d) return false;
  bool degrees = !pos && cur->qnt && cur->scale && !func;
  int unit = !pos && func && !(cur->qnt && cur->scale) ? 12 : 1;
  int tries = pos ? kNeTriesPos : degrees ? kNeTriesDeg : unit == 12 ? kNeTriesOct : kNeTriesSemi;
  int sg = d > 0 ? 1 : -1;
  if (degrees) {
    ne_deg_build();
    for (int i = 0; i < n; ++i) ne_di[i] = ne_deg_of[ne_tg[i] & 0x7F];
  }
  memset(ne_tbit, 0, sizeof ne_tbit), memset(ne_land_bit, 0, sizeof ne_land_bit);
  for (int i = 0; i < n; ++i) ne_tbit[ne_tg[i] >> 7][(ne_tg[i] & 0x7F) >> 5] |= 1u << (ne_tg[i] & 31);
  for (int i = 0; i < len; ++i) {
    const Step& s = t.step[i];
    int k = 0;
    for (int j = 0; j < s.n; ++j) k += !bit_has(ne_tbit, i, s.note[j].pitch);
    ne_stay[i] = (uint8_t)k, ne_arr[i] = 0;
  }
  int sh = d * unit;
  for (int tr = 0;; sh += sg * unit, ++tr) {
    if (tr >= tries) return false;
    bool ok = true;
    int i = 0;
    for (; i < n; ++i) {
      ++ne_work;
      int idx = ne_tg[i] >> 7, p = ne_tg[i] & 0x7F;
      int ni = pos ? idx + sh : idx, np = p;
      if (!pos) {
        if (degrees) {
          int di = ne_di[i] + sh;
          np = di >= 0 && di < ne_deg_n ? ne_deg_tab[di] : -1;
        } else {
          np = p + sh;
        }
      }
      if (ni < 0 || ni >= len || np < 0 || np > 127) return false;   // an end: no further shift helps
      ne_ni[i] = (int16_t)ni, ne_np[i] = (uint8_t)np;
      if ((!pos && np == p) || bit_has(ne_land_bit, ni, np) ||           // (two moved notes on one)
          (ne_has(t.step[ni], np) && !bit_has(ne_tbit, ni, np)) ||     // a pitch there that stays
          ne_stay[ni] + ++ne_arr[ni] > kNotes) {
        ok = false;
        ++i;
        break;
      }
      ne_land_bit[ni][np >> 5] |= 1u << (np & 31);
    }
    if (ok) break;
    for (int j = 0; j < i; ++j)   // this shift's marks cleared (the ones set: O(n), the landings cached)
      ne_land_bit[ne_ni[j]][ne_np[j] >> 5] &= ~(1u << (ne_np[j] & 31)), ne_arr[ne_ni[j]] = 0;
  }
  bool hl_moves = bit_has(ne_tbit, ui.ne_idx < 0 ? 0 : ui.ne_idx, ui.ne_pitch < 0 ? 0 : ui.ne_pitch) && ui.ne_idx >= 0;
  int hl_to_i = -1, hl_to_p = -1;
  if (!pos) {   // PTCH: in place (the shift is checked: no pitch twice in a step)
    static uint8_t was_sel[kMaxSteps * kNotes], at[kMaxSteps * kNotes];
    for (int i = 0; i < n; ++i) {   // each target's place in its step first (no note moves between steps here)
      int idx = ne_tg[i] >> 7, p = ne_tg[i] & 0x7F;
      const Step& s = t.step[idx];
      for (int k = 0; k < s.n; ++k)
        if (s.note[k].pitch == p) at[i] = (uint8_t)k;
      was_sel[i] = ne_sel_has(idx, p), ne_sel_set(idx, p, false);
    }
    for (int i = 0; i < n; ++i) {
      int idx = ne_tg[i] >> 7, p = ne_tg[i] & 0x7F, np = ne_np[i];
      t.step[idx].note[at[i]].pitch = (uint8_t)np;
      if (was_sel[i]) ne_sel_set(idx, np, true);
      if (hl_moves && idx == ui.ne_idx && p == ui.ne_pitch) hl_to_i = idx, hl_to_p = np;
    }
  } else {
    // whole steps: every note a target; in descending order of the move a step's target counts as free when it is
    // unused or itself moves whole (a run of chords moving one step)
    static uint8_t whole[kMaxSteps];
    static Step ws[kMaxSteps];
    for (int i = 0; i < len; ++i) whole[i] = t.step[i].n && ne_stay[i] == 0;
    for (int k = 0; k < len; ++k) {
      int i = sh > 0 ? len - 1 - k : k;
      if (!whole[i]) continue;
      int ni = i + sh;
      bool free = !step_used(t.step[ni]) || (whole[ni] == 2);
      whole[i] = free ? 2 : 1;   // 2: moves whole
    }
    struct Mv { Note nt; int16_t ni; uint8_t sel; };
    static Mv mv[kMaxSteps * kNotes];
    int nm = 0;
    for (int i = 0; i < len; ++i) {   // take every moving note out (a whole step: its copy kept)
      Step& s = t.step[i];
      if (!s.n) continue;
      bool any = ne_stay[i] < s.n;
      if (!any) continue;
      if (whole[i] == 2) ws[i] = s;
      for (int k = 0; k < s.n;) {
        int p = s.note[k].pitch;
        if (!bit_has(ne_tbit, i, p)) { ++k; continue; }
        Mv& m = mv[nm++];
        m.nt = s.note[k], m.ni = (int16_t)(i + sh), m.sel = ne_sel_has(i, p);
        ne_sel_set(i, p, false);
        if (hl_moves && i == ui.ne_idx && p == ui.ne_pitch) hl_to_i = i + sh, hl_to_p = p;
        for (int j = k + 1; j < s.n; ++j) s.note[j - 1] = s.note[j];
        --s.n;
      }
      if (!s.n && (whole[i] == 2 || !s.nl)) memset(&s, 0, sizeof s);   // moved whole, or a trig off; locks: a lock trig
    }
    for (int i = len - 1; i >= 0; --i) {   // the whole steps first, their notes put in below
      if (whole[i] != 2) continue;
      Step& dst = t.step[i + sh];
      dst = ws[i];
      dst.n = 0;
    }
    for (int i = 0; i < nm; ++i) {
      Step& s = t.step[mv[i].ni];
      bool empty = !step_used(s);
      add_note(s, mv[i].nt);
      if (empty) s.flags |= 1, s.prob = s.prob ? s.prob : 100;
      if (mv[i].sel) ne_sel_set(mv[i].ni, mv[i].nt.pitch, true);
    }
  }
  if (hl_to_i >= 0) ne_go(hl_to_i, hl_to_p);
  ne_recount(), ne_bump();
  return true;
}
// A..E (F..H: the v4 blocks to come, nothing yet): POS NUDG LEN PTCH VEL. A trig held with no notes: the track's note
// there first (as the step edit's first turn), highlighted.
CORE_COLD void ne_enc(int e, int d, bool func) {
  int r = ui_track();
  int sc = ne_scope();
  Track* t = ui_track_data(sc >= 0);
  if (!t) return;
  int len = eff_len(t, r);
  if (e > 4) return;
  bool filled = false, need = false;
  if (sc >= 0 && sc < len && !t->step[sc].n) {   // (the fill and the turn: one level)
    need = ne_begin(-2), filled = true;
    fill_default(*t, t->step[sc], false);
    ui.ne_idx = -1;
    ne_go(sc, t->step[sc].note[0].pitch);
    ui.ne_edits = st.edits;
  }
  if (!ne_fix()) return;
  bool done = false;
  if (e == 0 && func) e = 1, func = false;   // FUNC + POS: by 1/96 (as NUDG)
  int n = ne_targets(*t, len, ne_tg);
  if (!n) return;
  if (!filled) need = ne_begin(e);
  if (e == 0 || e == 3) {
    done = ne_move(*t, len, e == 0, d, func);
    if (!done) say(e == 0 ? "NO ROOM THERE" : "NO PITCH FREE");
  } else {
    for (int i = 0; i < n; ++i) {
      Step& s = t->step[ne_tg[i] >> 7];
      for (int k = 0; k < s.n; ++k) {
        if (s.note[k].pitch != (ne_tg[i] & 0x7F)) continue;
        Note& x = s.note[k];
        if (e == 1) x.micro = (int8_t)clampi(x.micro + d * (func ? 12 : 1), -kSub, kSub);
        else if (e == 2) x.len = (uint16_t)clampi(x.len + d * (func ? kSub : kSub / 16), 1, 16 * kSub);
        else x.vel = (uint8_t)clampi(x.vel + d * (func ? 8 : 1), 1, 127);
        if (e == 2 && n == 1) t->dlen = x.len;   // (the edited note's: a new note's length follows it)
      }
    }
    done = true;
  }
  if (done || filled) replan(r), mark_dirty(), ne_bump();
  ne_commit(e, need, done || filled);
}
// FUNC + NO: the targets deleted - more than one asks first (a second FUNC + NO within kNeDelMs); the highlight on to
// the next note (else the previous), the selection cleared; a step left empty is cleared as a trig off (its locks too)
constexpr uint32_t kNeDelMs = 2000;
CORE_COLD void ne_delete() {
  int r = ui_track();
  if (!ne_fix()) return;
  Track* t = ui_track_data(false);
  if (!t) return;
  int len = eff_len(t, r), n = ne_targets(*t, len, ne_tg);
  if (!n) return;
  uint32_t ms = now_ms();
  if (n > 1 && !(ui.ne_del_ms && ms - ui.ne_del_ms < kNeDelMs && ui.ne_del_n == n)) {
    ui.ne_del_ms = ms | 1, ui.ne_del_n = n;
    char b[28];
    sniprintf(b, sizeof b, CORE_STR("F+NO AGAIN: DEL %d NOTES"), n);
    say(b);
    return;
  }
  ui.ne_del_ms = 0;
  bool need = ne_begin(-3);
  int all = ne_list(t, len, -1, ne_buf), k = 0, want = ui.ne_idx << 7 | ui.ne_pitch;
  memset(ne_tbit, 0, sizeof ne_tbit);
  for (int i = 0; i < n; ++i) ne_tbit[ne_tg[i] >> 7][(ne_tg[i] & 0x7F) >> 5] |= 1u << (ne_tg[i] & 31);
  while (k < all && ne_buf[k] < want) ++k;
  int next = -1;
  for (int j = k; j < all && next < 0; ++j)
    if (!bit_has(ne_tbit, ne_buf[j] >> 7, ne_buf[j] & 0x7F)) next = ne_buf[j];
  for (int j = k - 1; j >= 0 && next < 0; --j)
    if (!bit_has(ne_tbit, ne_buf[j] >> 7, ne_buf[j] & 0x7F)) next = ne_buf[j];
  for (int i = 0; i < n; ++i) {
    Step& s = t->step[ne_tg[i] >> 7];
    remove_pitch(s, ne_tg[i] & 0x7F);
    if (!s.n) memset(&s, 0, sizeof s);   // an emptied step: as a trig off (its locks with it)
  }
  ne_sel_clear();
  if (next >= 0) ne_go(next >> 7, next & 0x7F);
  else ui.ne_idx = -1, ui.ne_pitch = -1, ne_bump();
  char b[28];
  sniprintf(b, sizeof b, CORE_STR("%d NOTE%s DELETED"), n, n == 1 ? "" : "S");
  say(b);
  replan(r), mark_dirty();
  ne_commit(-3, need, true);
}
// The DATA push: the highlighted note in / out of the selection; FUNC + push: every note, or none when all are in. The
// footer says what it did (I5: the Rytm drops a DATA turn ~0.55 s after the push: the message is that pause)
CORE_COLD void ne_push(bool func) {
  if (!ui.ne) {
    ne_land();
    return;
  }
  if (!ne_fix()) return;
  ui.ne_undo_e = -1, ui.ne_del_ms = 0;
  int r = ui_track();
  const Track* t = track_of(*cur, r);
  if (func) {
    int n = ne_list(t, eff_len(t, r), -1, ne_buf);
    bool all = ui.ne_nsel == n;
    ne_sel_clear();
    if (!all)
      for (int i = 0; i < n; ++i) ne_sel_set(ne_buf[i] >> 7, ne_buf[i] & 0x7F, true);
  } else {
    ne_sel_set(ui.ne_idx, ui.ne_pitch, !ne_sel_has(ui.ne_idx, ui.ne_pitch));
  }
  ne_recount(), ne_bump();
  char b[28];
  if (ui.ne_nsel) sniprintf(b, sizeof b, CORE_STR("SELECTED %d"), ui.ne_nsel);
  else sniprintf(b, sizeof b, "NONE SELECTED");
  say(b, 1000);
}
CORE_COLD void ne_leave() {
  if (!ui.ne) return;
  ui.ne = false, ne_sel_clear(), ne_bump();
  ui.touched = -1;
}

CORE_COLD bool ui_key(int code, bool down, bool shown) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  if (code == 0x80) {   // rui1050: FUNC (the SEQ page catches it, the Rytm keeps it too: both sides'); never taken
    ui.func = down;
    return false;
  }
  if (!seqmode && !shown) return false;
  uint32_t ms = now_ms();
  // seq-ux: a prompt on the SEQ page takes YES / NO; the trig keys and PAGE do nothing under it
  if (ui.prompt && shown) {
    if (ui.prompt == kPromptMenu && (code == 0x42 || code == 0x43)) {   // S2: the menu's rows
      if (down) ui.prompt_sel = clampi(ui.prompt_sel + (code == 0x42 ? -1 : 1), 0, kMenuRows - 1);
      return true;
    }
    if (ui.prompt == kPromptMenu && ui.prompt_sel == kMenuDir && (code == 0x40 || code == 0x41)) {   // rui1050: PLAY MODE
      if (down) menu_dir(code == 0x41 ? 1 : -1);
      return true;
    }
    if (code == 0x44 || code == 0x45) {
      if (down) prompt_answer(code == 0x44);
      return true;
    }
    return code < 16 || code == 0x84;
  }
  // U1 NOTE EDIT: the DATA push (caught only on a poly track's page, data_wanted): the selection (FUNC: all / none)
  if (code == 0x78) {
    if (down && shown && in_mask(ui_track())) ne_push(ui.func);
    return shown;
  }
  // review round 1: FUNC + YES in NOTE EDIT - the undo (the platform's history: the touchscreen's UNDO is the same)
  if (code == 0x44 && ui.ne && ui.func && shown) {
    if (down) ne_undo();
    return true;
  }
  // fix1028: FUNC + YES on a poly track's SEQ page (no trig held) is the same undo (RESTORE n NOTES? answered NO)
  if (code == 0x44 && ui.func && shown && !ui.held && in_mask(ui_track())) {
    if (down) ne_undo();
    return true;
  }
  // UX review S2: the poly track's menu (BACK / TURN OFF / LENGTH / QUANTISE ...) on its SEQ page, no trig held. ux1027
  // (the user, 2026-10-27): on DOWN, not YES - a caught YES took the Rytm's own YES combos (YES + PLAY / SCNE: its quick
  // saves). YES is caught now only while yes_wanted (a question, FUNC + YES's undo, trig + YES's preview). DOWN moved
  // to the next page on the SIDECAR ring before, a ring the page has no more (core/ui/remote.cpp standalone).
  // updown1029 (the user, 2026-10-29): on FUNC + DOWN - UP / DOWN alone are the Rytm's (CHRO's octave on the pads), caught
  // only while updown_wanted (FUNC held, a question); review 6's guard was against a stray menu inside a FUNC chord,
  // FUNC is the menu's own modifier now (FUNC + DOWN is nothing else here: the DOWN is caught, stock never sees it)
  if (code == 0x43 && shown && !ui.held && ui.func && !ui.page_held && in_mask(ui_track())) {   // (not PAGE held + DOWN)
    if (down) ui.prompt = kPromptMenu, ui.prompt_track = ui_track(), ui.prompt_sel = 0, ui.prompt_pat = cur->index;
    return true;
  }
  // rui1037: trigs held + YES - the step preview (above); YES's release goes where its press went
  if (code == 0x44 && down && ui.held) {
    preview();
    ui.preview_yes = true;
    return true;
  }
  if (code == 0x44 && !down && ui.preview_yes) {
    ui.preview_yes = false;
    return true;
  }
  // rui1040: TRIG (the user's request after rui1039): the SEQ page's two views, as stock's TRIG page beside the grid -
  // the notes (the roll; a held step's keyboard and values) and the trig values (a held step's NOTE VEL LEN MICRO COND
  // STRUM CHD INV, else the track's defaults for new steps: NOTE VEL LEN SWING). The page closed (seq mode): it opens on
  // the notes; open: TRIG toggles the view; under held trigs too (it ends a lock page; the trigs' release is no tap).
  // (TRK + TRIG, the SEQ page in / out, never comes here: the Rytm eats it.)
  // rui1050: TRIG cycles the pages - no trig held NOTES -> DEFAULTS -> SCALE -> RANDOM, trigs held NOTES -> TRIG -> RTRG
  // -> FX (from a lock page: TRIG, as stock's TRIG key picks its page). The step pages stay where they were while the
  // track's are on (a hold on the notes view starts on the notes).
  if (code == 0x30) {
    if (down) {
      if (shown && !ui.held) ne_leave();   // U1: TRIG leaves NOTE EDIT (on to DEFAULTS, as from the notes); a trig held:
                                           // its step pages as before, NOTE EDIT again at the next hold
      if (!shown) ui.tv = kTvNotes, ui.sv = kSvNotes, ui::remote_show_page("SEQ");
      else if (ui.held) ui.sv = ui.lockpg >= 0 ? kSvTrig : (ui.sv + 1) % kSvPages;
      else ui.tv = (ui.tv + 1) % kTvPages, ui.sv = ui.tv == kTvNotes ? kSvNotes : ui.sv ? ui.sv : kSvTrig;
      ui.lockpg = -1, ui.touched = -1, ui.cond_e = -1, ui.push_down = 0, ui.msg_ms = 0;
      for (int k = 0; k < 16; ++k)
        if (ui.held >> k & 1) ui.held_used[k] = true;
    }
    return true;
  }
  // seq-ux: trigs held - a page key opens its lock page (the page opens if it was closed: seq mode), NO clears the
  // held steps' locks, an encoder's push the lock of its param on the lock page
  int lp = code == 0x10 ? kLpFx : code >= 0x30 && code <= 0x35 ? code - 0x30 : -1;
  if (ui.held && lp >= 0) {
    if (down) {
      if (lp == kLpFx && ui.lockpg == kLpFx) lp = kLpIns;   // fx-ins: FX again - the voice's INSERT params
      ui.lockpg = lp, ui.touched = -1, ui.cond_e = -1, ui.push_down = 0, ui.msg_ms = 0;
      for (int k = 0; k < 16; ++k)
        if (ui.held >> k & 1) ui.held_used[k] = true;   // no tap toggle at the release
      if (!shown) ui::remote_show_page("SEQ");
    }
    return true;
  }
  // U1 NOTE EDIT: FUNC + NO deletes the highlighted note / the selection; NO alone back a level (the selection, then
  // NOTE EDIT itself); with trigs held NO still clears their locks
  if (ui.ne && shown && code == 0x45 && (ui.func || !ui.held) && !(ui.held && ui.sv != kSvNotes)) {
    if (down) {
      if (ui.func) ne_delete();
      else if (ui.ne_nsel) ne_sel_clear(), ne_bump();
      else ne_leave();
    }
    return true;
  }
  if (ui.held && code == 0x45) {
    if (down) lock_clear(-1);
    return true;
  }
  // an encoder's push on the lock page: a short one clears its lock (at the release: rui1050), a long one (kLongPushMs,
  // seen by tick() while held) its COND, FUNC + push its SLIDE
  if (ui.held && code >= 0x70 && code <= 0x77) {
    int e = code - 0x70, prm = ui.lockpg >= 0 ? lock_param_of(ui.lockpg, e) : -1;
    uint8_t bit = (uint8_t)(1u << e);
    if (down) {
      ui.push_down |= bit, ui.push_done &= (uint8_t)~bit, ui.push_ms[e] = ms;
      if (prm >= 0 && ui.func) slide_toggle(prm), ui.push_done |= bit;
    } else if (ui.push_down & bit) {
      ui.push_down &= (uint8_t)~bit;
      if (prm >= 0 && !(ui.push_done & bit)) {
        if (ms - ui.push_ms[e] >= kLongPushMs) {
          if (ui.cond_e != e && held_lock(prm) < 0) say("NO LOCK: TURN IT FIRST");
          else ui.cond_e = ui.cond_e == e ? -1 : e, log(CORE_STR("seq: lock page knob %c: %s"), 'A' + e, ui.cond_e == e ? "COND" : "value");
        }
        else {
          lock_clear(prm);
          if (ui.cond_e == e) ui.cond_e = -1;
        }
      }
      ui.push_done &= (uint8_t)~bit;
    }
    return true;
  }
  if (code == 0x45 && down && shown && ui.view_opened && !ui.held) {   // NO on the page TRK + TRIG opened: back
    close_view();
    return true;
  }
  // fix1028 review: on the SEQ page of a track that is no poly track (TURN OFF, the page list) the trig keys and PAGE are
  // not the sequencer's - a tap would edit kept steps nobody sees (the page asks the Rytm for none of them: seq_trigs)
  if ((code < 16 || code == 0x84) && !seqmode && !in_mask(ui_track()) && !(code < 16 && !down && (ui.held >> code & 1)))
    return false;
  if (code >= 0 && code < 16) {
    uint16_t bit = (uint16_t)(1u << code);
    Track* t = ui_track_data(false);
    int np = npages_of(eff_len(t, ui_track()));
    if (down) {
      if (ui.page_held) {   // PAGE held + a trig: that page
        if (code < np) ui.page = code;
        ui.page_used = true;
        ui.follow = false;
        return true;
      }
      if (!ui.held) {   // rui1050: a new hold - on the notes view the step's notes, on a track page its step pages
        if (ui.tv == kTvNotes) ui.sv = kSvNotes;
        else if (ui.sv == kSvNotes) ui.sv = kSvTrig;
      }
      ui.held |= bit;
      ui.held_ms[code] = ms, ui.held_used[code] = false, ui.held_first[code] = true;
      ui.sel = 0;
      ui.touched = -1;
      if (ui.ne && ne_fix() && t) {   // U1: the held step's lowest note highlighted (its chord is DATA's walk now)
        int idx = ui.page * kStepsPerPage + code, n = idx < kMaxSteps ? ne_list(t, eff_len(t, ui_track()), idx, ne_buf) : 0;
        if (n && ui.ne_idx != idx) ne_go(idx, ne_buf[0] & 0x7F);
      }
    } else if (ui.held & bit) {
      ui.held &= (uint16_t)~bit;
      int idx = ui.page * kStepsPerPage + code;
      if (ui.ne && idx < kMaxSteps) {   // U1: a tap never toggles a step off; on an empty step it adds the note
        bool tap = !ui.held_used[code] && ms - ui.held_ms[code] < kTapMs;
        t = ui_track_data(false);
        if (tap && (!t || !t->step[idx].n) && !(t && t->step[idx].nl)) {
          if (idx >= eff_len(t, ui_track())) say("BEYOND THE LENGTH");   // (it would never play)
          else toggle(code), t = ui_track_data(false);
        }
        int n = t ? ne_list(t, eff_len(t, ui_track()), idx, ne_buf) : 0;
        if (n && ui.ne_idx != idx) ne_go(idx, ne_buf[0] & 0x7F);
        if (n) ne_fix();
      } else if (!ui.held_used[code] && ms - ui.held_ms[code] < kTapMs) toggle(code);
      if (!ui.held) ui.lockpg = -1, ui.cond_e = -1, ui.push_down = 0, ui.touched = -1;   // seq-ux: the last trig let go: the
                                                                                         // lock page ends (rui1050: and the
                                                                                         // step knob's footer)
    }
    return true;
  }
  if (code == 0x84) {   // PAGE: a press steps the page; held, the trig LEDs are the page map
    if (down) {
      ui.page_held = true, ui.page_used = false;
    } else if (ui.page_held) {
      ui.page_held = false;
      Track* t = ui_track_data(false);
      int np = npages_of(eff_len(t, ui_track()));
      if (!ui.page_used) ui.page = (ui.page + 1) % (np < 1 ? 1 : np);
      ui.follow = false;   // a page chosen by hand: follow off until turned on again (E)
    }
    return true;
  }
  return false;
}

CORE_COLD bool ui_enc(int e, int delta, bool func, bool shown) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  // U1 NOTE EDIT: DATA (encoder 8; the page catches it on a poly track only, data_wanted) walks the highlight
  if (e == 8) {
    if (!shown || !delta || !in_mask(ui_track())) return false;
    if (ui.prompt || (ui.held && ui.lockpg >= 0)) return true;   // (nothing under a prompt or a lock page)
    ui.touched = 8;
    ne_turn(delta, func);
    return true;
  }
  if ((!seqmode && !shown) || e < 0 || e > 7 || !delta) return false;
  ui.touched = e;
  if (ui.prompt && shown) return true;   // seq-ux: nothing under a prompt
  if (ui.held && ui.lockpg >= 0) {      // seq-ux: the lock page: the held steps' locks (rui1050: a knob on COND, its condition)
    if (e == ui.cond_e && lock_param_of(ui.lockpg, e) >= 0) lcond_turn(lock_param_of(ui.lockpg, e), delta, func);
    else lock_turn(e, delta, func);
    return true;
  }
  if (ui.ne && shown && !(ui.held && ui.sv != kSvNotes)) {   // U1 NOTE EDIT: A..E on the highlighted note / the selection
    for (int k = 0; k < 16; ++k)
      if (ui.held >> k & 1) ui.held_used[k] = true;
    ne_enc(e, delta, func);
    return true;
  }
  if (ui.held && (ui.sv == kSvRtrg || ui.sv == kSvFx)) {   // rui1050: RTRG / FX of every held step
    Track* t = ui_track_data(true);
    if (!t) return true;
    for (int k = 0; k < 16; ++k)
      if (ui.held >> k & 1) {
        Step& s = t->step[ui.page * kStepsPerPage + k];
        if (ui.sv == kSvRtrg) rtrg_turn(*t, s, e, delta, func);
        else fx_turn(s, e, delta, func);
        ui.held_used[k] = true;
      }
    replan(ui_track());
    mark_dirty();
    return true;
  }
  if (ui.held && ui.sv == kSvTrig) {   // rui1040: the trig values of the held steps: A NOTE B VEL C LEN D MICRO E COND F STRUM
    static const int8_t kMap[8] = {0, 1, 2, 3, -1, 6, 4, 5};   // G CHD H INV (edit_step's), E COND here
    Track* t = ui_track_data(true);
    if (!t) return true;
    for (int k = 0; k < 16; ++k)
      if (ui.held >> k & 1) {
        Step& s = t->step[ui.page * kStepsPerPage + k];
        if (e == 0) note_turn(*t, s, delta, func), replan(ui_track());   // rui1050: RND below the lowest note
        else if (kMap[e] >= 0) edit_step(*t, s, kMap[e], delta, func);
        else {   // COND: the trig condition / probability on one knob, as stock's
          if (!s.n) fill_default(*t, s, false);
          cond_knob_set_i(s, cond_knob_i(s) + delta * (func ? 8 : 1));
        }
        ui.held_used[k] = true;
      }
    mark_dirty();
    return true;
  }
  if (ui.held) {
    Track* t = ui_track_data(true);
    if (!t) return true;
    for (int k = 0; k < 16; ++k)
      if (ui.held >> k & 1) {
        Step& s = t->step[ui.page * kStepsPerPage + k];
        if (e == 0) note_turn(*t, s, delta, func), replan(ui_track());   // rui1050: RND below the lowest note
        else edit_step(*t, s, e, delta, func);
        ui.held_used[k] = true;
      }
    mark_dirty();
    return true;
  }
  if (shown && ui.tv != kTvNotes) {   // rui1040: the track's defaults for new steps; rui1050: DEFAULTS / SCALE / RANDOM
    track_page_enc(e, delta, func);
    return true;
  }
  if (shown) settings_enc(e, delta, func);
  return true;
}

CORE_COLD bool leds(uint8_t c[16]) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  int r = ui_track();
  const Track* t = track_of(*cur, r);
  int len = eff_len(t, r), np = npages_of(len);
  int ps = play_step_of(r, t);
  if (ui.page_held) {
    for (int k = 0; k < 16; ++k)
      c[k] = k >= np ? kLedOff : (ps >= 0 && ps / kStepsPerPage == k) ? kLedPlay : k == ui.page ? kLedPageOn : kLedPageDim;
    return true;
  }
  if (ui.ne) ne_fix();
  for (int k = 0; k < 16; ++k) {
    int idx = ui.page * kStepsPerPage + k;
    const Step* s = t && idx < kMaxSteps ? &t->step[idx] : nullptr;
    bool sel = false;
    for (int w = 0; ui.ne && ui.ne_nsel && idx < kMaxSteps && w < 4; ++w) sel |= ui.ne_sel[idx][w] != 0;
    if (ui.held >> k & 1) c[k] = kLedHeld;
    else if (ui.ne && idx == ui.ne_idx) c[k] = kLedHeld;   // U1: the highlighted note's step (as a held trig)
    else if (idx >= len) c[k] = kLedBeyond;
    else if (idx == ps) c[k] = kLedPlay;
    else if (sel) c[k] = kLedNeSel;                        // U1: a step with selected notes (the playhead over it)
    else if (s && s->nl) c[k] = s->n ? kLedLock : kLedLockOnly;   // seq-ux: a locked step
    else if (s && s->n) c[k] = s->n > 1 ? kLedChord : kLedNote;
    else c[k] = kLedOff;
  }
  return true;
}

CORE_COLD void page_leds(uint8_t c[4]) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  int r = ui_track();
  const Track* t = track_of(*cur, r);
  int np = npages_of(eff_len(t, r)), ps = play_step_of(r, t);
  int pp = ps >= 0 ? ps / kStepsPerPage : -1, g = ui.page / 4 * 4;
  for (int i = 0; i < 4; ++i) {
    int p = g + i;
    c[i] = p >= np ? kLedOff : p == ui.page ? kLedPageOn : p == pp ? kLedPlay : kLedPageDim;
  }
}

// ---- the pin's silence (46): a pinned track whose voice has nothing of the host's sounding, nothing planned, for
// kSilentMs (release tails included: poly's voice activity) gets its VCA closed; anything planned opens it at once
// (planning runs 32 ms ahead of the note, the 46 needs a USB MIDI hop).
static CORE_COLD uint16_t pinned() { return (uint16_t)((pin_seq ? mask : 0) | pin_extra); }
static CORE_COLD void silence_tick(uint32_t ms) {
  uint16_t pm = pinned(), sil = 0;
  if (!pm) return;
  for (int r = 0; r < kRytmTracks; ++r) {
    if (!(pm >> r & 1)) continue;
    int vi = voice_of(r);
    bool busy = vi >= 0 && (poly::active(vi) || poly::voice_notes(vi) > 0 || poly::pad_sounding(vi));   // fix1052: its pad
    for (int i = 0; i < nev && !busy; ++i) busy = ev[i].track == r;
    for (int n = 0; n < 128 && vi >= 0 && !busy; ++n) busy = sounding[vi][n] != 0;
    if (cal.state && cal.vi == vi) busy = true;
    if (busy) heard_ms[r] = ms;
    else if (ms - heard_ms[r] >= kSilentMs) sil |= (uint16_t)(1u << r);
  }
  if (sil == silent_sent) return;
  uint8_t b[2] = {(uint8_t)(sil & 0x7F), (uint8_t)(sil >> 7 & 0x1F)};
  send_cmd(kSilentCmd, b, 2);
  silent_sent = sil;
  ++st.silents;
}

// ---- latency calibration: kCalClicks clicks 150 ms apart on track cal.track's voice; each one's round trip (rendered at
// cal.at, back at cal.found); the median becomes the latency. Automatic once after a hello when a pinned seq track
// exists, the platform feeds its stream and the transport is stopped; seq=cal:T any time; a latency set by hand
// (seq=lat, the page's F) stays until the next seq=cal.
static CORE_COLD void cal_start(int r) {
  int vi = voice_of(r);
  if (r < 0 || vi < 0) return;
  cal.track = r, cal.vi = vi, cal.n = 0, cal.state = 0, cal.next_ms = now_ms(), cal.manual = false;
  cal.deadline_ms = now_ms() + 3000;
}
static CORE_COLD void cal_tick(uint32_t ms) {
  if (!cal.auto_done && !cal.manual && transport == kStop && ms - cal.fed_ms < 500 && fol.valid) {
    uint16_t pm = pinned() & mask;
    for (int r = 0; r < kRytmTracks && pm; ++r)
      if (pm >> r & 1) {
        cal_start(r);
        cal.auto_done = true;
        break;
      }
  }
  if (cal.track < 0) return;
  if (cal.state == 2 && cal.found >= 0) {   // back
    cal.res[cal.n++] = (int)(cal.found - cal.at);
    cal.state = 0;
    cal.next_ms = ms + 150;
  }
  if (cal.state == 2 && (int32_t)(ms - cal.deadline_ms) > 0) {   // never came back (no pin, no stream, muted)
    cal.state = 0, cal.track = -1, cal.vi = -1;
    ++st.cal_fails;
    return;
  }
  if (cal.n >= kCalClicks) {
    int v[kCalClicks];
    memcpy(v, cal.res, sizeof v);
    for (int i = 0; i < kCalClicks; ++i)
      for (int j = i + 1; j < kCalClicks; ++j)
        if (v[j] < v[i]) { int x = v[i]; v[i] = v[j]; v[j] = x; }
    int m = v[kCalClicks / 2];
    if (m > 0 && m < 9600 && v[kCalClicks - 1] - v[0] < 480) {
      latency = m;
      if (tracks_touch()) {   // (round 3: refused while the project loads - not counted or reported as calibrated)
        st.cal_last = m;
        ++st.cals;
        log(CORE_STR("seq: latency calibrated on track %d: %d frames (%d..%d)"), cal.track + 1, m, v[0], v[kCalClicks - 1]);
      }
    } else {
      ++st.cal_fails;
      log(CORE_STR("seq: calibration on track %d inconsistent (%d..%d frames): latency kept"), cal.track + 1, v[0],
          v[kCalClicks - 1]);
    }
    cal.track = -1, cal.vi = -1;
    return;
  }
  if (cal.state == 0 && (int32_t)(ms - cal.next_ms) >= 0) {
    cal.found = -1;
    cal.floor = 0;
    cal.at = (int64_t)hframes + 2 * kBlock;   // a block or two ahead of the render
    cal.state = 1;
    cal.deadline_ms = ms + 1000;
  }
}

CORE_COLD void tick(uint32_t ms) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  // rui1050: a lock page's encoder held kLongPushMs: its COND (or back to its value), at once while it is still held
  for (int e = 0; e < 8 && ui.push_down; ++e)
    if ((ui.push_down >> e & 1) && !(ui.push_done >> e & 1) && ui.held && ui.lockpg >= 0 &&
        ms - ui.push_ms[e] >= kLongPushMs && lock_param_of(ui.lockpg, e) >= 0) {
      ui.push_done |= (uint8_t)(1u << e);
      if (ui.cond_e != e && held_lock(lock_param_of(ui.lockpg, e)) < 0) { say("NO LOCK: TURN IT FIRST"); continue; }
      ui.cond_e = ui.cond_e == e ? -1 : e;
      log(CORE_STR("seq: lock page knob %c: %s"), 'A' + e, ui.cond_e == e ? "COND" : "value");
    }
  remask(true);   // per pattern poly: a 33 (a track's machine) since
  // per pattern poly, the switch: the Rytm must hear the next pattern's capture before it plays that pattern's first step
  // (or the step plays with the old one). With a pattern queued (prefetched in nxt) its new seq tracks are clocked (the
  // 2E's clock part: their ticks, nothing else - the prediction plans their first steps); kSwitchLead + the latency
  // before the predicted switch its mask goes out whole. A miss drops the prediction: the current mask again.
  uint16_t wm = mask, ck = clock_extra;
  if (transport == kPlay && pat_queued >= 0 && pat_queued != cur->index && nxt->index == pat_queued) {
    uint16_t nm = mask_of(*nxt);
    if (pred_to == pat_queued && fol.valid &&
        (double)pred_frame - (double)fol.to_rytm((int64_t)hframes) <= (double)(kSwitchLead + latency))
      wm = nm;
    else
      ck |= (uint16_t)(nm & ~mask);
  }
  int pin_now = pin_extra | (pin_seq ? 0 : 1 << 12);
  if (hello_due || mask_sent != wm || pin_sent != pin_now || clock_sent != ck) {
    // the 7-byte form (rytm-mods 1033's pin part) only when it says more than the 3- / 5-byte one: older builds refuse it
    uint8_t b[7] = {(uint8_t)(wm & 0x7F), (uint8_t)(wm >> 7 & 0x1F), (uint8_t)((hello_due ? 2 : 0) | (pin_seq ? 0 : 4)),
                    (uint8_t)(ck & 0x7F), (uint8_t)(ck >> 7 & 0x1F), (uint8_t)(pin_extra & 0x7F),
                    (uint8_t)(pin_extra >> 7 & 0x1F)};
    send_cmd(kMaskCmd, b, pin_now ? 7 : ck ? 5 : 3);
    if (wm != mask) ++st.mask_early;
    mask_sent = wm, hello_due = false, pin_sent = pin_now, clock_sent = ck, clock_now = ck;
    silent_sent = 0xFFFF;   // the pin's silence again after every 2E
    ++st.masks;
  }
  silence_tick(ms);
  cal_tick(ms);
  rytm_dump::set_playing(transport == kPlay);
  rytm_dump::tick(ms);
  // follow: the edited page goes with the playhead
  int r = ui_track();
  const Track* t = track_of(*cur, r);
  int ps = play_step_of(r, t);
  if (ui.follow && ps >= 0 && !ui.held && !ui.page_held && !ui.ne) ui.page = ps / kStepsPerPage;   // (NOTE EDIT: the highlight's)
  bool shown = seq_shown();
  // the trig LEDs with the page closed (seq mode): ours (with the page open, remote.cpp sends the page's leds_now)
  if (seqmode && !ui::remote_open()) {
    uint8_t c[16];
    leds(c);
    if ((!ui.leds_sent || memcmp(c, ui.leds, 16)) && ms - ui.leds_ms >= 10) {
      uint8_t b[17];
      b[0] = 1;
      memcpy(b + 1, c, 16);
      send_cmd(0x34, b, 17);
      memcpy(ui.leds, c, 16), ui.leds_sent = true, ui.leds_ms = ms;
    }
  } else if (ui.leds_sent && !ui::remote_open()) {
    uint8_t b = 0;
    send_cmd(0x34, &b, 1);
    ui.leds_sent = false;
  } else if (ui::remote_open()) {
    ui.leds_sent = false;
  }
  // the PAGE LEDs (2F) while seq mode is on or the page shows; stock again after
  if (seqmode || shown) {
    uint8_t c[4];
    page_leds(c);
    if (!ui.pleds_sent || ui.pleds_stock || memcmp(c, ui.pleds, 4)) {
      send_cmd(kPageLedsCmd, c, 4);
      memcpy(ui.pleds, c, 4), ui.pleds_sent = true, ui.pleds_stock = false;
    }
  } else if (!ui.pleds_stock) {
    uint8_t c[4] = {kNone, kNone, kNone, kNone};
    send_cmd(kPageLedsCmd, c, 4);
    ui.pleds_stock = true;
  }
}

CORE_COLD bool idle() {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  if (load_due) {   // a pattern change without a switch, a project's first
    load_due = false;
    install(pat_now);
    return true;
  }
  if (pat_queued >= 0 && pat_queued != cur->index && nxt->index != pat_queued) {   // prefetch
    int was_lock = lock_of(pat_queued);
    load_into(*nxt, pat_queued);
    if (sav_pending && sav->index == pat_queued) {
      memcpy(nxt, sav, sizeof *nxt);
      // final review (Important 1), as install's "back to it": the copy is the loaded (old) project's pattern, so the
      // index keeps that project's lock, not the stand-in's kLockLoading load_into set while the next project's store
      // fills - otherwise the save of sav just below was refused ("its project still loading") and the edit lost.
      // (sav, during a fill, only ever holds the old project's pattern: mark_dirty refuses an edit of a stand-in; and
      // save_pattern writes it under g_proj, the old project's key until project_changed.)
      if (lock_of(pat_queued) == kLockLoading) pat_lock[pat_queued] = was_lock == kLockLoading ? 0 : was_lock;
    }
    return true;
  }
  if (sav_pending) {
    save_pattern(*sav, true);
    sav_pending = false;
    return true;
  }
  if (tracks_dirty && (save_now || now_ms() - tracks_ms >= kSaveMs)) {   // fix1049: once the edits settled
    tracks_dirty = false;
    save_tracks(true);
    return true;
  }
  if (dirty && (save_now || now_ms() - dirty_ms >= kSaveMs)) {
    dirty = false, save_now = false;
    save_pattern(*cur, true);
    return true;
  }
  return false;
}

// ---- the calibration click (render side) and its return (the platform's stream side; on the Pi an interrupt: only
// the volatile words of `cal` are touched)
static const int kStem[8] = {0, 3, 1, 4, 6, 5, 7, 2};   // physical voice -> the stream's voice channel (BD SD RS/CP ..)
void cal_render(int16_t (*ring)[8], const int* rows, int v0, int v1) {
  if (cal.state != 1 && cal.state != 3) return;
  int vi = cal.vi;
  if (vi < v0 || vi >= v1) return;
  int64_t bf = (int64_t)hframes - kBlock;   // this block's first frame (pre_block counted it)
  if (cal.state == 1) {
    if (cal.at >= bf + kBlock) return;      // not yet
    if (cal.at < bf) cal.at = bf;           // late: from here (the round trip is measured from where it went)
    cal.state = 3;                          // rendering
  }
  for (int s = 0; s < kBlock; ++s) {
    int64_t f = bf + s;
    if (f < cal.at || f >= cal.at + kCalClickFrames) continue;
    ring[rows[s]][vi] = (int16_t)((((f - cal.at) / 24) & 1) ? -12000 : 12000);   // a 1 kHz square burst, 2 ms, -9 dBFS
  }
  if (bf + kBlock >= cal.at + kCalClickFrames) cal.state = 2;
}
void cal_audio(const int32_t* s, int n, int stride, int64_t arrival) {
  cal.fed_ms = now_ms();
  int st_ = cal.state, vi = cal.vi;
  if (!st_ || vi < 0 || vi > 7 || cal.found >= 0) return;
  const int32_t* x = s + kStem[vi];
  int32_t fl = cal.floor;
  if (st_ != 2) {   // before the click is out: the stream's own level
    for (int i = 0; i < n; ++i) {
      int32_t a = x[i * stride] < 0 ? -x[i * stride] : x[i * stride];
      if (a > fl) fl = a;
    }
    cal.floor = fl;
    return;
  }
  int32_t thr = fl > 0x7FFFFFFF / 8 ? 0x7FFFFFFF / 2 : 4 * fl;
  if (thr < (1 << 21)) thr = 1 << 21;   // -60 dBFS at least
  for (int i = 0; i < n; ++i) {
    int32_t a = x[i * stride] < 0 ? -x[i * stride] : x[i * stride];
    if (a > thr) {
      cal.found = arrival - (n - 1 - i);
      return;
    }
  }
}
bool cal_wanted() { return cal.state != 0; }

double play_pos(int r) {
  if (!cur || r < 0 || r >= kRytmTracks) return -1;
  const Track* t = track_of(*cur, r);
  if (play_step_of(r, t) < 0 || clk[r].period <= 0 || !fol.valid) return -1;
  int len = eff_len(t, r);
  double x = vnow(r, t);   // P2: in the track's own steps
  int64_t v = (int64_t)x;
  StepAt at = map_step(*cur, t, r, v);   // E1: the mapped step plus how far into it the playhead is
  double m = (double)at.index + (x - (double)v);
  return m >= len ? m - len : m;
}

CORE_COLD int pattern_now() { Pattern* p = cur; return p ? p->index : -1; }
CORE_COLD int transport_now() { return transport; }

CORE_COLD NoteEdit note_edit() {
  if (!cur) reset();
  NoteEdit o = {};
  o.idx = -1, o.pitch = -1, o.held = -1;
  ne_fix();
  o.on = ui.ne, o.gen = ui.ne_gen, o.sel = ui.ne ? ui.ne_nsel : 0;
  if (!ui.ne || ui.ne_idx < 0) return o;
  int r = ui_track();
  const Track* t = track_of(*cur, r);
  o.idx = ui.ne_idx, o.pitch = ui.ne_pitch, o.held = ne_scope();
  int n = ne_list(t, eff_len(t, r), o.held >= 0 && o.held < eff_len(t, r) ? o.held : -1, ne_buf), want = o.idx << 7 | o.pitch;
  o.count = n;
  for (int k = 0; k < n; ++k)
    if (ne_buf[k] == want) o.pos = k + 1;
  n = ne_list(t, eff_len(t, r), o.idx, ne_buf);
  o.chord_n = n;
  for (int k = 0; k < n; ++k)
    if (ne_buf[k] == want) o.chord_i = k + 1;
  return o;
}
CORE_COLD bool note_selected(int idx, int pitch) { return ui.ne && ne_sel_has(idx, pitch); }
CORE_COLD bool note_edit_range(int e, int* lo, int* hi) {
  if (!cur || !ne_fix()) return false;
  int r = ui_track();
  const Track* t = track_of(*cur, r);
  if (!t) return false;
  int n = ne_targets(*t, eff_len(t, r), ne_tg);
  *lo = 1 << 30, *hi = -(1 << 30);
  for (int i = 0; i < n; ++i) {
    int idx = ne_tg[i] >> 7;
    const Step& s = t->step[idx];
    for (int k = 0; k < s.n; ++k) {
      const Note& x = s.note[k];
      if (x.pitch != (ne_tg[i] & 0x7F)) continue;
      int v = e == 0 ? idx : e == 1 ? x.micro : e == 2 ? x.len : e == 3 ? x.pitch : x.vel;
      *lo = v < *lo ? v : *lo, *hi = v > *hi ? v : *hi;
    }
  }
  return n > 0;
}
CORE_COLD void set_undo_hooks(const UndoHooks* h) { ne_hooks = h; }
CORE_COLD void undo_prepare(int r) {   // (seqed=undo / redo: the snapshot ends with it - round 3, NB-1)
  if (!cur) return;
  ne_cover_outside();
  if (r == ne_after_r) ne_after_on = false;
}
CORE_COLD void level_taken(int r) {
  if (cur && r == ne_after_r && cur->index == ne_after_p) ne_after_on = false;
}
CORE_COLD uint32_t note_edit_work() { return ne_work; }
CORE_COLD void note_moved(int r, int fi, int fp, int ti, int tp) {
  if (!ui.ne || r != ui.ne_track || !cur || cur->index != ui.ne_pattern || fi < 0 || fi >= kMaxSteps || ti < 0 || ti >= kMaxSteps) return;
  bool sel = ne_sel_has(fi, fp);
  if (sel) ne_sel_set(fi, fp, false), ne_sel_set(ti, tp, true);
  if (fi == ui.ne_idx && fp == ui.ne_pitch) ne_go(ti, tp);
  ne_bump();
}
CORE_COLD bool data_wanted() {
  if (!cur) return false;
  return in_mask(ui_track());
}
// ux1027: YES is the SEQ page's only while a question shows (the menu, the make / RESTORE / TURN OFF prompts), FUNC is
// held (FUNC + YES: the undo) or a trig is held (trig + YES: the step preview); else the Rytm's (rytm-mods 0026 catches
// per key: the host names YES in its mask at FUNC's or the trig's press, which it sees - FUNC is caught on both sides)
CORE_COLD bool yes_wanted() { return ui.prompt || ui.func || ui.held; }
// updown1029 (the user, 2026-10-29): UP / DOWN are the SEQ page's only while a question shows (the menu's rows; under
// the others nothing, as the trig keys) or FUNC is held (FUNC + DOWN: the menu); else the Rytm's - in CHRO the pads'
// octave. A trig held: none of the page's (stock never saw the poly trig, its arrow is the octave too)
CORE_COLD bool updown_wanted() { return ui.prompt || ui.func; }

View view() {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  View w = {};
  int r = ui_track();
  w.track = r;
  w.seq = in_mask(r);
  w.seqmode = seqmode;
  w.t = track_of(*cur, r);
  w.length = eff_len(w.t, r);
  w.npages = npages_of(w.length);
  w.page = ui.page < w.npages ? ui.page : w.npages - 1;
  w.play_step = play_step_of(r, w.t);
  w.play_pos = play_pos(r);
  w.held = -1;
  for (int k = 0; k < 16; ++k)
    if (ui.held >> k & 1) {
      w.held = k;
      break;
    }
  w.held_mask = ui.held;
  w.page_held = ui.page_held;
  w.transport = transport;
  w.rec_live = rec_live, w.rec_grid = rec_grid;
  w.pattern = cur->index, w.queued = pat_queued;
  w.bpm = bpm;
  w.quant = quant;
  w.follow = ui.follow;
  w.sel = ui.sel;
  w.touched = ui.touched;
  w.latency = latency;
  w.loaded = pat_loaded == cur->index;
  w.locked = lock_of(cur->index);
  w.loading = g_loading;
  w.rytm_len = pat_len_of() > 0 ? pat_len_of() : sc_len[r] >= 0 ? sc_len[r] : (clk[r].on ? clk[r].rytm_len : -1);
  w.rytm_speed = sc_spd[r] >= 0 ? sc_spd[r] : (clk[r].on ? clk[r].speed : -1);
  w.fill = fill;
  w.swing = w.t && w.t->swing > 50 ? w.t->swing : 50;
  w.clip_track = clip_t.rytm >= 0;
  w.clip_pattern = clip_p->index >= 0;
  w.prompt = ui.prompt;
  w.prompt_track = ui.prompt_track;
  w.prompt_n = ui.prompt_n, w.prompt_locks = ui.prompt_locks;
  w.prompt_undo = ne_hooks && ne_hooks->save;
  w.prompt_why = ui.prompt == kPromptExplain ? can_seq(ui.prompt_track) : 0;
  w.prompt_sel = ui.prompt_sel;
  w.off_hint = ui.off_hint_ms && (int32_t)(now_ms() - ui.off_hint_ms) < 3000;
  w.lock_page = ui.held ? ui.lockpg : -1;
  w.view_opened = ui.view_opened;
  w.tpage = ui.held ? ui.sv : ui.tv;
  w.trigview = w.tpage != 0;
  w.cond_e = ui.held && ui.lockpg >= 0 ? ui.cond_e : -1;
  w.func = ui.func;
  w.msg = ui.msg_ms && now_ms() - (ui.msg_ms & ~1u) < ui.msg_dur ? ui.msg : nullptr;   // (msg_ms: the time | 1)
  return w;
}

CORE_COLD int held_lock(int param) {
  if (!cur) reset();
  const Track* t = track_of(*cur, ui_track());
  if (!t) return -1;
  for (int k = 0; k < 16; ++k)
    if (ui.held >> k & 1) {
      int v = lock_of(t->step[ui.page * kStepsPerPage + k], param);
      if (v >= 0) return v;
    }
  return -1;
}
CORE_COLD int lock_param(int page, int e) { return lock_param_of(page, e); }
CORE_COLD bool pattern_seed(int* seed) {
  if (!cur) reset();
  *seed = cur->seed;
  return cur->seed_fixed != 0;
}
CORE_COLD void pattern_scale(int* key, int* scale, int* qnt) {
  if (!cur) reset();
  *key = cur->key, *scale = cur->scale, *qnt = cur->qnt;
}
CORE_COLD int held_lock_cond(int param, bool* slide) {
  if (!cur) reset();
  const Track* t = track_of(*cur, ui_track());
  for (int k = 0; t && k < 16; ++k) {
    if (!(ui.held >> k & 1)) continue;
    const Step& s = t->step[ui.page * kStepsPerPage + k];
    for (int i = 0; i < s.nl; ++i)
      if (s.lock[i].param == param) {
        if (slide) *slide = (s.slide >> param & 1) != 0;
        return s.lock[i].cond;
      }
  }
  if (slide) *slide = false;
  return -1;
}
CORE_COLD int seed_slot_of(int r) { return r >= 0 && r < kRytmTracks ? seed_slot(r) : -1; }
CORE_COLD int seed_offset(int r) {
  if (!cur || r < 0 || r >= kRytmTracks) return 0;
  const Track* t = track_of(*cur, r);
  return t && t->smod_src && smod_last[r] > 0 ? smod_last[r] : 0;
}
CORE_COLD bool cond_pass(const Step& s, int r, int64_t iter) {
  return cond_pass_i(s, r, iter, rng_key(free_seed, r, 0, iter, kRngProb));
}
CORE_COLD int cond_knob(const Step& s) { return cond_knob_i(s); }
CORE_COLD void cond_knob_set(Step& s, int v) { cond_knob_set_i(s, v); }
CORE_COLD const char* cond_text(const Step& s, char* b, int cap) { return cond_text_i(s, b, cap); }
CORE_COLD int lock_base(int param) { return base_value(ui_track(), param); }

CORE_COLD void view_request(int r) {
  if (!cur) reset();
  if (r < 0 || r >= kRytmTracks) return;
  if (!seqmode) ui.track = r;   // (seq mode: its track, the Rytm's selected one: this one)
  ui.view_opened = true, show_wanted = true, ui.lockpg = -1;
  ui.prompt = in_mask(r) ? 0 : can_seq(r) == 1 ? kPromptMake : kPromptExplain, ui.prompt_track = r;
  ++view_gen, view_track = r;
  log(CORE_STR("seq: TRK + TRIG: track %d's SEQ page%s"), r + 1,
      ui.prompt == kPromptMake ? ", make prompt" : ui.prompt == kPromptExplain ? ", not a poly machine" : "");
}
CORE_COLD void selected_track(int t) {
  if (!cur) reset();
  if (t < 0 || t >= kRytmTracks || t == ui.track || ui.prompt) return;
  ui.held = 0, ui.page_held = false, ui.lockpg = -1, ui.touched = -1, ui.cond_e = -1, ui.push_down = 0, ui.func = false, ui.msg_ms = 0;
  if (in_mask(t)) {
    ui.track = t, ui.page = 0;
    return;
  }
  if ((ui.view_opened || we_opened) && ui::remote_open()) {
    ui.view_opened = false, we_opened = false;
    ui::remote_request(false);
    log(CORE_STR("seq: track %d selected (no seq track): the SEQ page closed"), t + 1);
  }
}
CORE_COLD void page_closed() {
  if (!cur) reset();
  ui.view_opened = false, ui.prompt = 0;
  ne_leave();   // U1
  ui.func = false, ui.push_down = 0, ui.cond_e = -1, ui.tv = kTvNotes, ui.sv = kSvNotes, ui.msg_ms = 0;   // rui1050 (a FUNC
}                                                                                                        // release lost with the page)
CORE_COLD uint32_t view_requests(int* track) {
  if (track) *track = view_track;
  return view_gen;
}

CORE_COLD void view_set(int track, int page, int held, bool follow) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  ui.track = clampi(track, 0, kRytmTracks - 1);
  ui.page = clampi(page, 0, kMaxPages - 1);
  ui.held = held >= 0 && held < 16 ? (uint16_t)(1u << held) : 0;
  if (held >= 0 && held < 16) ui.held_ms[held] = now_ms(), ui.held_used[held] = true, ui.held_first[held] = false;
  ui.follow = follow;
}

const Stats& stats() { return st; }

CORE_COLD int pending(int64_t* f, uint8_t* on, uint8_t* voice, uint8_t* note, int max) {
  int k = 0;
  for (int i = 0; i < nev && k < max; ++i, ++k) f[k] = ev[i].f, on[k] = ev[i].on, voice[k] = ev[i].vi, note[k] = ev[i].note;
  return k;
}

CORE_COLD void set_latency(int frames) { latency = frames < 0 ? 0 : frames; }
CORE_COLD Pattern& edit_pattern() {
  if (!cur) reset();
  return *cur;
}
CORE_COLD int poly_json(char* out, int cap) {
  if (!cur) return 0;   // (core 0's /status: never core 3's state from here)
  int n = sniprintf(out, cap, "\"poly\":[");
  bool first = true;
  for (int r = 0; r < kRytmTracks && n < cap; ++r) {
    if (!in_mask(r)) continue;
    const Track* t = track_of(*cur, r);
    PolyCfg c = cfg_of(t, r);
    char info[48] = "";
    inst::Resolved rs;
    int m = -1, vi = voice_of(r);
    if (vi >= 0 && inst::resolve_for(vi, c, &rs, &m)) inst::describe_short(rs, m, info, sizeof info);
    n += sniprintf(out + n, cap - n, "%s{\"t\":%d,\"v\":%d,\"a\":%d,\"u\":%d,\"d\":%d,\"s\":%d,\"g\":%d,\"own\":%d,\"info\":\"%s\"}",
                   first ? "" : ",", r + 1, c.voices, c.alloc, c.unison, c.detune, c.spread, c.group,
                   t && !poly_default(t->poly) ? 1 : 0, info);
    first = false;
  }
  if (n < cap) n += sniprintf(out + n, cap - n, "]");
  return n < cap ? n : 0;
}
CORE_COLD int seqx_json(char* out, int cap) {
  if (!cur) return 0;   // (core 0's /status: plain reads of core 3's pattern, as poly_json)
  const Pattern& p = *cur;
  int n = sniprintf(out, cap, "\"seqx\":{\"key\":%d,\"scale\":%d,\"qnt\":%d,\"seed\":%d,\"fixed\":%d,\"t\":[", p.key, p.scale,
                    p.qnt, p.seed, p.seed_fixed ? 1 : 0);
  bool first = true;
  for (int r = 0; r < kRytmTracks && n < cap; ++r) {
    const Track* t = in_mask(r) ? track_of(p, r) : nullptr;
    if (!t) continue;
    n += sniprintf(out + n, cap - n, "%s{\"t\":%d,\"spd\":%d,\"dir\":%d,\"rnd\":[%d,%d,%d,%d],\"smod\":[%d,%d,%d,%d],\"off\":%d,\"x\":[",
                   first ? "" : ",", r + 1, t->speed, t->dir, t->rnd_mode, t->rnd_low, t->rnd_oct, t->rnd_wght, t->smod_src,
                   t->smod_range, t->smod_quant, t->smod_aff, smod_last[r] > 0 ? smod_last[r] : 0);
    first = false;
    bool f2 = true;
    for (int i = 0; i < kMaxSteps && n < cap - 160; ++i) {
      const Step& s = t->step[i];
      bool lc = false;
      for (int k = 0; k < s.nl; ++k) lc |= s.lock[k].cond != 0;
      if (!rt_on(s.rt) && !s.fx[0].cmd && !s.fx[1].cmd && !(s.xf & kXfRnd) && !lc && !s.slide) continue;
      const Rtrg& x = s.rt;
      n += sniprintf(out + n, cap - n, "%s{\"s\":%d,\"rt\":[%d,%d,%d,%d,%d,%d,%d],\"fx\":[[%d,%d],[%d,%d]],\"r\":%d,\"lc\":[",
                     f2 ? "" : ",", i + 1, x.cnt, x.rate, x.vcrv, x.pcrv, x.mode, x.oct, x.gate, s.fx[0].cmd,
                     s.fx[0].cmd == kFxOct || s.fx[0].cmd == kFxTrs ? (int8_t)s.fx[0].val : s.fx[0].val, s.fx[1].cmd,
                     s.fx[1].cmd == kFxOct || s.fx[1].cmd == kFxTrs ? (int8_t)s.fx[1].val : s.fx[1].val, s.xf & kXfRnd ? 1 : 0);
      f2 = false;
      bool f3 = true;
      for (int k = 0; k < s.nl && n < cap - 40; ++k)
        if (s.lock[k].cond) n += sniprintf(out + n, cap - n, "%s[%d,%d]", f3 ? "" : ",", s.lock[k].param + 1, s.lock[k].cond), f3 = false;
      n += sniprintf(out + n, cap - n, "],\"sl\":[");
      f3 = true;
      for (int k = 0; k < s.nl && n < cap - 20; ++k)
        if (s.slide >> (s.lock[k].param & 31) & 1) n += sniprintf(out + n, cap - n, "%s%d", f3 ? "" : ",", s.lock[k].param + 1), f3 = false;
      n += sniprintf(out + n, cap - n, "]}");
    }
    if (n < cap) n += sniprintf(out + n, cap - n, "]}");
  }
  if (n < cap) n += sniprintf(out + n, cap - n, "]}");
  return n < cap ? n : 0;
}
CORE_COLD PolyCfg poly_of(int rytm) {
  if (!cur) reset();
  return cfg_of(track_of(*cur, rytm), rytm);
}
CORE_COLD Track* edit_track(int rytm, bool create) {
  if (!cur) reset();
  Track* t = track_of(*cur, rytm, false);
  if (!t && create && (t = track_of(*cur, rytm, true))) t->on = 1, remask();   // (a track made to edit: a poly track)
  return t;
}
CORE_COLD Track* edit_seq_track(int rytm, bool create) { return owns_track(rytm) ? edit_track(rytm, create) : nullptr; }
CORE_COLD void touched() { mark_dirty(); }

// A demo pattern on track r: two bars of broken chords (strummed up, then down), a loose-timed bass-free voicing,
// four pages, so the playback, the SEQ page and the LEDs show without entering steps.
static CORE_COLD void demo(int r) {
  if (g_loading) return refused_log("a demo pattern");   // (round 3: its mask would be put back, its steps kept)
  Track* t = slot_for(r);
  if (!t) return;
  for (Step& s : t->step) s.n = 0;
  t->length = 64;
  static const uint8_t roots[4] = {57, 53, 60, 55};   // Am F C G
  static const uint8_t chd[4] = {2, 1, 1, 1};
  for (int bar = 0; bar < 4; ++bar) {
    int b = bar * 16;
    set_chord(t->step[b], roots[bar], chd[bar], 1, 96, kSub * 6, 16);      // a slow upward strum, held 6 steps
    set_chord(t->step[b + 6], roots[bar] + 12, chd[bar], 0, 80, kSub * 2, -20);   // down, an octave up
    set_chord(t->step[b + 10], roots[bar], chd[bar], 2, 70, kSub * 2, 8);  // a fast roll
    Step& s = t->step[b + 13];   // a loose single note, 1/4 step late
    s.n = 0;
    add_note(s, {(uint8_t)(roots[bar] + 19), 90, (uint16_t)(kSub * 2), (int8_t)(kSub / 4), 0});
    Step& s2 = t->step[b + 14];   // and one early
    s2.n = 0;
    add_note(s2, {(uint8_t)(roots[bar] + 16), 75, (uint16_t)(kSub * 1), (int8_t)(-kSub / 3), 0});
  }
  t->on = 1, legacy_mask |= (uint16_t)(1u << r), tracks_touch();
  mark_dirty();
  remask();
}

// seqtop: track r's settings in one line (the console's seq=x:T, the tests)
static CORE_COLD int x_line(int r, char* out, int cap) {
  const Track* t = track_of(*cur, r);
  if (!t) return sniprintf(out, cap, CORE_STR("seq: track %d is no seq track in this pattern"), r + 1);
  uint32_t base = eff_seed(*cur, *t);
  int off = t->smod_src && smod_last[r] > 0 ? smod_last[r] : 0;
  return sniprintf(out, cap, CORE_STR("seq: track %d SPD %s DIR %s KEY %s %s QNT %s RND %s low %d oct %d %s SMOD %s%d range %d %s %s "
                                      "seed %lu%s+%d"),
                   r + 1, speed_name(t->speed), dir_name(t->dir), key_name(cur->key), scale_name(cur->scale),
                   cur->qnt == kQntPlay ? "PLAY" : cur->qnt ? "EDIT" : "OFF", rnd_mode_name(t->rnd_mode), t->rnd_low,
                   t->rnd_oct, weight_name(t->rnd_wght), t->smod_src ? "P" : "OFF", t->smod_src, t->smod_range,
                   t->smod_quant == kSqStep ? "STEP" : t->smod_quant == kSqBar ? "BAR" : "LOOP",
                   t->smod_aff == kAffAll ? "ALL" : t->smod_aff == kAffOrder ? "+ORDER" : t->smod_aff == kAffChance ? "+CHANCE" : "NOTES",
                   (unsigned long)(t->seed_own ? t->seed : cur->seed_fixed ? cur->seed : base),
                   t->seed_own ? " own" : cur->seed_fixed ? "" : " FREE", off);
}
// R1: print - the random notes of loop `loop` written into the steps as plain notes (the flags / TRACK mode off)
static CORE_COLD int print_rnd(int r, int64_t loop) {
  Track* t = track_of(*cur, r, false);
  if (!t || !t->rnd_mode) return 0;
  int n = 0;
  uint32_t sd = eff_seed(*cur, *t) + (uint32_t)(t->smod_src && smod_last[r] > 0 ? smod_last[r] : 0);
  for (int i = 0; i < kMaxSteps; ++i) {
    Step& s = t->step[i];
    if (!s.n || !(t->rnd_mode == kRndTrack || (s.xf & kXfRnd))) continue;
    uint8_t wr[kNotes], rp[kNotes];
    for (int k = 0; k < s.n; ++k) wr[k] = s.note[k].pitch;
    rnd_notes(s.n, sd, r, i, t->seed_same ? -1 : loop, cur->key, cur->scale, t->rnd_low, t->rnd_oct, t->rnd_wght,
              t->rnd_wght == kWChord ? chord_pcs(s, cur->key, cur->scale) : 0, wr, rp);
    for (int k = 0; k < s.n; ++k) s.note[k].pitch = rp[k];
    s.xf &= (uint8_t)~kXfRnd;
    ++n;
  }
  t->rnd_mode = kRndOff;
  return n;
}

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (!cur) reset();   // (before engine_begin: the tests that skip it)
  if (strncmp(p, "seq", 3) || (p[3] && p[3] != '=')) return false;
  {   // seqtop: the model's own commands (core/seq/xcmd.cpp, the touch editor's host build applies them too)
    int xt = -2;
    int rc = command_x(*cur, p, &xt, out, cap);
    if (rc) {
      if (rc > 0) {
        replan(xt);   // (-1: every track)
        mark_dirty();
        bool sm = !strncmp(p, "seq=smod:", 9);
        if (sm && xt >= 0)   // (the offsets sampled under the old setting go too)
          smod_last[xt] = -1, memset(smh[xt], 0, sizeof smh[xt]), memset(sml[xt], 0, sizeof sml[xt]);
        if ((sm || !strncmp(p, "seq=rnd:", 8)) && xt >= 0) x_line(xt, out, cap);
      }
      return true;
    }
  }
  if (!strncmp(p, "seq=track:", 10)) {   // seq=track:T:on|off (T 1..12)
    int r = atoi(p + 10) - 1;
    const char* q = strchr(p + 10, ':');
    if (r < 0 || r >= kRytmTracks || !q) return sniprintf(out, cap, CORE_STR("seq=track:T:on|off (T 1..12)")), true;
    bool on = !strcmp(q + 1, "on");
    if (g_loading && on != slot_on(r))
      return set_seq(r, on), sniprintf(out, cap, CORE_STR("seq: track %d not changed: project %s is still loading"),
                                       r + 1, project), true;
    set_seq(r, on);
    return sniprintf(out, cap, CORE_STR("seq: track %d %s, mask 0x%03x"), r + 1, on ? "sequenced" : "stock", mask), true;
  }
  if (!strncmp(p, "seq=lat:", 8)) {
    set_latency(atoi(p + 8));
    if (!tracks_touch())   // (round 2: and not marked as set by hand)
      return sniprintf(out, cap, CORE_STR("seq: latency not changed: project %s is still loading"), project), true;
    cal.manual = true;
    return sniprintf(out, cap, CORE_STR("seq: notes start %d frames early"), latency), true;
  }
  if (!strncmp(p, "seq=cal", 7) && (!p[7] || p[7] == ':')) {   // seq=cal[:T]: calibrate now (T: that track's voice)
    int r = p[7] == ':' ? atoi(p + 8) - 1 : -1;
    for (int t = 0; t < kRytmTracks && r < 0; ++t)
      if (in_mask(t)) r = t;
    if (r < 0 || r >= kRytmTracks || voice_of(r) < 0)
      return sniprintf(out, cap, CORE_STR("seq=cal:T: a track with a voice (and its pin, and the Rytm's stream)")), true;
    cal_start(r);
    return sniprintf(out, cap, CORE_STR("seq: calibrating on track %d (voice %d): %d clicks"), r + 1, voice_of(r) + 1, kCalClicks), true;
  }
  if (!strncmp(p, "seq=pin:", 8)) {   // seq=pin:T:on|off: more pinned tracks (a poly track the Rytm sequences)
    int r = atoi(p + 8) - 1;
    const char* q = strchr(p + 8, ':');
    if (r < 0 || r >= kRytmTracks || !q) return sniprintf(out, cap, CORE_STR("seq=pin:T:on|off (T 1..12)")), true;
    pin_extra = (uint16_t)(!strcmp(q + 1, "on") ? pin_extra | 1u << r : pin_extra & ~(1u << r));
    if (!tracks_touch())
      return sniprintf(out, cap, CORE_STR("seq: pin not changed: project %s is still loading"), project), true;
    return sniprintf(out, cap, CORE_STR("seq: pinned 0x%03x (seq tracks %s)"), pinned(), pin_seq ? "pinned" : "not pinned"), true;
  }
  if (!strncmp(p, "seq=pinseq:", 11)) {   // the seq tracks' pin (on by default)
    pin_seq = strcmp(p + 11, "off") != 0;
    if (!tracks_touch())
      return sniprintf(out, cap, CORE_STR("seq: pin not changed: project %s is still loading"), project), true;
    return sniprintf(out, cap, CORE_STR("seq: seq tracks %s, pinned 0x%03x"), pin_seq ? "pinned" : "not pinned", pinned()), true;
  }
  if (!strncmp(p, "seq=swing:", 10)) {   // seq=swing:T:N (50..80; 50 straight)
    int r = atoi(p + 10) - 1;
    const char* q = strchr(p + 10, ':');
    Track* t = q ? slot_for(r) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=swing:T:50..80")), true;
    int v = clampi(atoi(q + 1), 50, 80);
    t->swing = (uint8_t)(v > 50 ? v : 0);
    replan(r);
    mark_dirty();
    return sniprintf(out, cap, CORE_STR("seq: track %d swing %d%%"), r + 1, v), true;
  }
  if (!strncmp(p, "seq=step:", 9)) {   // seq=step:T:S:PITCH[:VEL]: a note on step S (1..) of track T (tests, the console)
    int r = atoi(p + 9) - 1;
    const char* q = strchr(p + 9, ':');
    const char* q2 = q ? strchr(q + 1, ':') : nullptr;
    int si = q ? atoi(q + 1) - 1 : -1;
    Track* t = q2 && si >= 0 && si < kMaxSteps ? slot_for(r) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=step:T:S:PITCH[:VEL]")), true;
    const char* q3 = strchr(q2 + 1, ':');
    add_note(t->step[si], new_note(*t, atoi(q2 + 1), q3 ? atoi(q3 + 1) : t->dvel));
    replan(r);
    mark_dirty();
    return sniprintf(out, cap, CORE_STR("seq: track %d step %d: %d notes"), r + 1, si + 1, t->step[si].n), true;
  }
  if (!strncmp(p, "seq=cond:", 9)) {   // rui1040: seq=cond:T:S:C - step S (1..) of track T, the COND knob's C (0 none,
    int r = atoi(p + 9) - 1;            // 1..8 FILL !FILL PRE !PRE NEI !NEI 1ST !1ST, 9..43 1:2 .. 8:8, 44..142 1..99 %)
    const char* q = strchr(p + 9, ':');
    const char* q2 = q ? strchr(q + 1, ':') : nullptr;
    int si = q ? atoi(q + 1) - 1 : -1;
    Track* t = q2 && si >= 0 && si < kMaxSteps ? slot_for(r) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=cond:T:S:C (C 0..%d)"), kCondKnob - 1), true;
    cond_knob_set_i(t->step[si], atoi(q2 + 1));
    replan(r);
    mark_dirty();
    char b[12];
    return sniprintf(out, cap, CORE_STR("seq: track %d step %d: %s"), r + 1, si + 1, cond_text_i(t->step[si], b, sizeof b)), true;
  }
  if (!strncmp(p, "seq=len:", 8)) {   // seq=len:T:N: track T's own length (0 AUTO: the Rytm track's)
    int r = atoi(p + 8) - 1;
    const char* q = strchr(p + 8, ':');
    Track* t = q ? slot_for(r) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=len:T:0..%d"), kMaxSteps), true;
    set_length(*t, r, atoi(q + 1));
    replan(r);
    mark_dirty();
    return sniprintf(out, cap, CORE_STR("seq: track %d length %d"), r + 1, t->length), true;
  }
  if (!strncmp(p, "seq=x:", 6)) {   // seqtop: seq=x:T - track T's seqtop settings
    int r = atoi(p + 6) - 1;
    if (r < 0 || r >= kRytmTracks) return sniprintf(out, cap, CORE_STR("seq=x:T (T 1..12)")), true;
    x_line(r, out, cap);
    return true;
  }
  if (!strncmp(p, "seq=print:", 10)) {   // seqtop R1: seq=print:T[:LOOP] - the random notes of loop LOOP (1..; default: the
                                         // loop playing, else the first) written into the steps
    int r = atoi(p + 10) - 1;
    const char* q = strchr(p + 10, ':');
    if (r < 0 || r >= kRytmTracks) return sniprintf(out, cap, CORE_STR("seq=print:T[:LOOP]")), true;
    const Track* t = track_of(*cur, r);
    int64_t loop = q ? atoi(q + 1) - 1 : 0;
    if (!q && t && transport == kPlay && play_abs[r] >= 0) loop = map_step(*cur, t, r, (int64_t)vnow(r, t)).loop;
    if (loop < 0) loop = 0;
    int n = print_rnd(r, loop);
    replan(r);
    mark_dirty();
    return sniprintf(out, cap, CORE_STR("seq: track %d: loop %ld's random notes printed on %d steps"), r + 1, (long)loop + 1, n), true;
  }
  if (!strncmp(p, "seq=predict:", 12)) {
    predict = strcmp(p + 12, "off") != 0;
    if (!predict) drop_prediction(false);
    return sniprintf(out, cap, CORE_STR("seq: switch prediction %s"), predict ? "on" : "off"), true;
  }
  if (!strncmp(p, "seq=quant:", 10)) {
    quant = atoi(p + 10) ? 1 : 0;
    if (!tracks_touch())
      return sniprintf(out, cap, CORE_STR("seq: quantise not changed: project %s is still loading"), project), true;
    return sniprintf(out, cap, CORE_STR("seq: record quantise %s"), quant ? "1/16" : "OFF (micro kept)"), true;
  }
  if (!strncmp(p, "seq=demo:", 9)) {
    int r = atoi(p + 9) - 1;
    if (r < 0 || r >= kRytmTracks) return sniprintf(out, cap, CORE_STR("seq=demo:T (T 1..12)")), true;
    if (g_loading) return demo(r), sniprintf(out, cap, CORE_STR("seq: no demo: project %s is still loading"), project), true;
    demo(r);
    return sniprintf(out, cap, CORE_STR("seq: demo pattern (broken chords) on track %d"), r + 1), true;
  }
  if (!strncmp(p, "seq=lock:", 9)) {   // seq=lock:T:S:P:V - step S (1..) of track T locks param P (1..16 P1..P16,
                                        // 17..20 FX1 P2..P5, 21..24 FX2's, 25..28 INSERT A P1..P4,
                                        // 29..32 INSERT B's) to V (0..127); V -1: that lock removed
    int a[4] = {0, 0, 0, 0}, k = 0;
    const char* q = p + 9;
    for (; k < 4 && *q; ++k) {
      a[k] = atoi(q);
      q = strchr(q, ':');
      if (!q) { ++k; break; }
      ++q;
    }
    int r = a[0] - 1, si = a[1] - 1, prm = a[2] - 1;
    Track* t = k == 4 && si >= 0 && si < kMaxSteps && prm >= 0 && prm < kLockParams ? slot_for(r) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=lock:T:S:P:V (P 1..16 the machine's, 17..24 FX1 / FX2 P2..P5, 25..32 INSERT A / B P1..P4; V 0..127, -1 off)")), true;
    Step& st_ = t->step[si];
    bool ok = a[3] < 0 ? clear_lock(st_, prm) : set_lock(st_, prm, clampi(a[3], 0, 127) << 7);
    replan(r);
    mark_dirty();
    return sniprintf(out, cap, CORE_STR("seq: track %d step %d: %d locks%s"), r + 1, si + 1, st_.nl, ok ? "" : " (unchanged)"), true;
  }
  if (!strncmp(p, "seq=played", 10) && (!p[10] || p[10] == ':')) {   // seqtop: seq=played[:T] - the last notes started (of track
    int r = p[10] ? atoi(p + 11) - 1 : -1;                              // T), oldest first, "v/a:step:pitch" (v the track's step
    int k = sniprintf(out, cap, CORE_STR("seq: played"));              // count, a the Rytm's; step 1..), then forgotten
    uint32_t from = played_n > (uint32_t)kPlayed ? played_n - kPlayed : 0;
    for (uint32_t i = from; i < played_n && k < cap - 24; ++i) {
      const Played& x = played[i % kPlayed];
      if (r < 0 || x.track == r) k += sniprintf(out + k, cap - k, " %ld/%ld:%d:%d", (long)x.v, (long)x.a, x.step + 1, x.pitch);
    }
    played_n = 0;
    return true;
  }
  if (!strncmp(p, "seq=steps:", 10)) {   // dbg: a track's steps with notes, "step:pitch,pitch.. " (tests, the console)
    const Track* t = track_of(*cur, atoi(p + 10) - 1);
    int k = sniprintf(out, cap, CORE_STR("seq: steps"));
    for (int i = 0; t && i < kMaxSteps && k < cap - 24; ++i) {
      if (!t->step[i].n) continue;
      k += sniprintf(out + k, cap - k, " %d:", i + 1);
      for (int j = 0; j < t->step[i].n && k < cap - 6; ++j) k += sniprintf(out + k, cap - k, j ? ",%d" : "%d", t->step[i].note[j].pitch);
      char b[12];
      if (cond_knob_i(t->step[i]) && k < cap - 12) k += sniprintf(out + k, cap - k, "[%s]", cond_text_i(t->step[i], b, sizeof b));
    }
    return true;
  }
  if (!strncmp(p, "seq=clear:", 10)) {
    Track* t = track_of(*cur, atoi(p + 10) - 1, false);
    if (t)
      for (Step& s : t->step) s.n = 0, s.nl = 0;
    mark_dirty();
    return sniprintf(out, cap, CORE_STR("seq: track cleared")), true;
  }
  if (!strncmp(p, "seq=seed:", 9)) {   // E1: seq=seed:free | seq=seed:N (0..9999, fixed) - the pattern's seed;
                                        // seq=seed:hold:N - FREE's seed held at N over PLAYs (tests, A/B listening)
    const char* q = p + 9;
    if (!strncmp(q, "hold:", 5)) {
      free_seed = (uint32_t)atoi(q + 5), seed_held = true;
      return sniprintf(out, cap, CORE_STR("seq: FREE seed held at %lu"), (unsigned long)free_seed), true;
    }
    if (!strcmp(q, "free")) cur->seed_fixed = 0, cur->seed = 0, seed_held = false;   // (no seed block: v2 / v3 again)
    else if (*q >= '0' && *q <= '9') cur->seed_fixed = 1, cur->seed = (uint16_t)clampi(atoi(q), 0, 9999);
    else return sniprintf(out, cap, CORE_STR("seq=seed:free|0..9999|hold:N")), true;
    replan(-1);
    mark_dirty();
    if (cur->seed_fixed) return sniprintf(out, cap, CORE_STR("seq: pattern seed %d (fixed)"), cur->seed), true;
    return sniprintf(out, cap, CORE_STR("seq: pattern seed FREE")), true;
  }
  if (!strncmp(p, "seq=tseed:", 10)) {   // E1: seq=tseed:T:follow | seq=tseed:T:N (its own) | seq=tseed:T:same:0|1
    int r = atoi(p + 10) - 1;
    const char* q = strchr(p + 10, ':');
    Track* t = q && r >= 0 && r < kRytmTracks ? slot_for(r) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=tseed:T:follow|0..9999|same:0|1")), true;
    ++q;
    if (!strcmp(q, "follow")) t->seed_own = 0, t->seed = 0;
    else if (!strncmp(q, "same:", 5)) t->seed_same = atoi(q + 5) ? 1 : 0;
    else if (*q >= '0' && *q <= '9') t->seed_own = 1, t->seed = (uint16_t)clampi(atoi(q), 0, 9999);
    else return sniprintf(out, cap, CORE_STR("seq=tseed:T:follow|0..9999|same:0|1")), true;
    replan(r);
    mark_dirty();
    return sniprintf(out, cap, CORE_STR("seq: track %d seed %s%d%s"), r + 1, t->seed_own ? "" : "follows the pattern's, ",
                     t->seed_own ? t->seed : cur->seed, t->seed_same ? " (same every loop)" : ""), true;
  }
  if (!strncmp(p, "seq=poly:", 9)) {   // E0: seq=poly:T (show) | seq=poly:T:auto | seq=poly:T:voices=N,alloc=reassign|
                                        // rotate|reset,unison=N,detune=N,spread=N,group=N (any of them: inst_alloc.h parse_cfg)
    int r = atoi(p + 9) - 1;
    const char* q = strchr(p + 9, ':');
    if (r < 0 || r >= kRytmTracks)
      return sniprintf(out, cap, CORE_STR("seq=poly:T[:auto|voices=0..8,alloc=reassign|rotate|reset,unison=1..8,detune=0..127,spread=0..127,group=0..4]")), true;
    Track* t = q ? slot_for(r) : track_of(*cur, r, false);
    PolyCfg c = cfg_of(t, r);
    if (q) {
      char err[80];
      if (!inst::parse_cfg(q + 1, &c, err, sizeof err)) return sniprintf(out, cap, CORE_STR("seq=poly: %s"), err), true;
      if (!t) return sniprintf(out, cap, CORE_STR("seq: no free seq track slot for track %d"), r + 1), true;
      PolyCfg was = t->poly;
      t->poly = c, poly_def[r] = c;
      if (!tracks_touch()) {   // (round 2: the pattern's copy put back with the default)
        t->poly = was;
        return sniprintf(out, cap, CORE_STR("seq: poly config not changed: project %s is still loading"), project), true;
      }
      mark_dirty();
    }
    inst::Resolved rs = {};
    int m = -1, vi = voice_of(r);
    bool ok = vi >= 0 && inst::resolve_for(vi, c, &rs, &m);
    int n = sniprintf(out, cap, CORE_STR("seq: track %d poly "), r + 1);
    if (n < cap) inst::describe(c, ok ? &rs : nullptr, m, out + n, cap - n);
    return true;
  }
  if (!strcmp(p, "seq=show")) {
    show_wanted = true;
    ui::remote_show_page("SEQ");
    return sniprintf(out, cap, CORE_STR("seq: the SEQ page")), true;
  }
  if (p[3]) return sniprintf(out, cap, CORE_STR("seq | seq=track:T:on|off | seq=lat:N | seq=cal[:T] | seq=quant:0|1 | seq=pin:T:on|off | seq=pinseq:on|off | seq=swing:T:N | seq=predict:on|off | seq=step:T:S:P[:V] | seq=lock:T:S:P:V | seq=cond:T:S:C | seq=lcond:T:S:P:C | seq=slide:T:S:P:0|1 | seq=fx:T:S:C:CMD:V | seq=rtrg:T:S:K=V,.. | seq=len:T:N | seq=spd:T:R | seq=dir:T:D | seq=key:K:S[:Q] | seq=qnt:Q | seq=quantise:T | seq=rnd:T:K=V,.. | seq=rndstep:T:S:0|1 | seq=print:T[:L] | seq=smod:T:K=V,.. | seq=x:T | seq=seed:free|N|hold:N | seq=tseed:T:follow|N|same:0|1 | seq=poly:T[:auto|K=V,..] | seq=demo:T | seq=clear:T | seq=show")), true;
  sniprintf(out, cap, CORE_STR("seq: %s pattern %d%s, mask 0x%03x, bpm x10 %d, ticks %lu (bad %lu), switches %lu, on %lu off %lu, late %lu "
                               "(max %lu fr), cancelled %lu, rec %lu, loads %lu (prefetched %lu), saves %lu (fail %lu), "
                               "offset %ld fr, phase err %ld (max %ld) fr, lat %d fr%s, rytm dropped %lu, edits %lu (undo %lu), "
                               "scale %lu, dirty %lu, pinned 0x%03x silent 0x%03x, predicted %lu (ok %lu miss %lu), cal %lu (fail %lu, last %ld), "
                               "locks applied %lu set %lu, previews %lu, views %lu%s"),
            transport == kPlay ? "PLAY" : transport == kPause ? "PAUSE" : "STOP", cur->index, seqmode ? " SEQMODE" : "",
            mask, (int)(bpm * 10), (unsigned long)st.ticks, (unsigned long)st.ticks_bad, (unsigned long)st.switches,
            (unsigned long)st.notes_on, (unsigned long)st.notes_off, (unsigned long)st.late,
            (unsigned long)st.late_frames_max, (unsigned long)st.cancelled, (unsigned long)st.recorded,
            (unsigned long)st.loads, (unsigned long)st.prefetched, (unsigned long)st.saves, (unsigned long)st.save_fails,
            (long)fol.offset, (long)st.err_last, (long)st.err_max, latency, cal.manual ? " (set)" : "",
            (unsigned long)st.dropped_rytm, (unsigned long)st.edits_rytm, (unsigned long)st.undos, (unsigned long)st.scales,
            (unsigned long)st.dirties, pinned(), silent_sent == 0xFFFF ? 0 : silent_sent, (unsigned long)st.predicted,
            (unsigned long)st.predict_ok, (unsigned long)st.predict_miss, (unsigned long)st.cals, (unsigned long)st.cal_fails,
            (long)st.cal_last, (unsigned long)st.locks_applied, (unsigned long)st.locks_set, (unsigned long)st.previews,
            (unsigned long)view_gen, ui.prompt == kPromptMake ? " (make prompt)" : ui.prompt == kPromptExplain ? " (why not)" :
            ui.prompt == kPromptOff ? " (off prompt)" : "");
  return true;
}

CORE_COLD uint16_t silenced() { return silenced_now(); }

}  // namespace seq
}  // namespace core
#endif
