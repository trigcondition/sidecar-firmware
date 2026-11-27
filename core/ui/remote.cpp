// core/ui/remote.cpp - see remote.h. Cold (program flash on the Teensy): a tick draws only while the page is open.
#include "md/md.h"   // CORE_MD
#include "sidecar_features.h"
#include "ui/remote.h"

#include <string.h>

#include "ui/extra.h"
#include "ui/page_keys.h"
#include "lib/srclib.h"

#include "fx/send_fx.h"
#include "fx/insert.h"
#include "device/device.h"
#include "hooks.h"
#include "keys.h"
#include "lfo.h"
#include "mod/mod.h"
#include "mod/rytm_out.h"
#include "mod/rytm_param.h"
#include "mod/scene.h"
#include "mod/xypad.h"
#include "mod/target.h"
#include "machines.h"
#include "midim/midim.h"
#include "poly.h"
#include "arp/arp.h"
#include "pool/pool.h"
#include "seq/seq.h"
#include "seq/rytm_dump.h"
#include "song/song.h"
#include "version.h"
#include "voice_name.h"
#include "voices.h"
#if CORE_MSAMP
#include "granular/granular.h"
#include "msamp/msamp.h"
#include "msamp/sdtest.h"
#include "wavetable/wavetable.h"
#include "bundle/prjslot.h"
#include "bundle/rename.h"
#include "nmach.h"
#include "livein/livein.h"
#endif

namespace core {
namespace ui {

namespace {

enum : uint8_t {
  kMode = 0x30, kKey = 0x31, kEnc = 0x32, kRect = 0x38, kCommit = 0x39, kOpen = 0x3A, kAlive = 0x3C,
  kText = 0x3D, kTextClear = 0x3E, kEcho = 0x3F, kMask = 0x37, kHello = 0x35, kSplash = 0x36, kMachines = 0x33,
  kKit = 0x3B,
  kDisplay = 0x49,   // rui1036b: Rytm -> host 49 1 <page> (UP + T1..T5; T6..T13 asked of rui1038 / rui1046), 49 2 <track> <machine> <name 12>, 49 3 <track>
                     // {<value> <label 4>} x 8: the Pi display's page and its VOICE view (the selected track)
  kWrite = 0x48,   // rui1036: host -> Rytm 48 <track> <param 0..15> <value> [<fraction>]: P(param + 1) set as a knob turn
                   // sets it; Rytm -> host 48 <track> <param> <status 0 written, 1 refused, 2 no such parameter>
  // 1027 SRC view (rytm-mods 0026 rui1027): Rytm -> host 29 <track> <on>, the stock SRC page of track t shown (also
  // on a machine change there and after each hello) / left; 2A <track> <first> <n> {<int> <frac>} x n that track's
  // P<first+1>.. (the active kit's words: P1..P8 the SRC knobs A..H, P9..P16 the SMPL page's; all 16 after each 29 t 1,
  // then the changed span each tick). No host -> Rytm 29 (0022 passes only 30..3F): the Rytm shows our frame from the
  // first 39 after its 29 t 1 while 3C / frames keep coming; 3A x 0 with the page closed gives stock back.
  kSrcView = 0x29, kSrcVals = 0x2A,
  // 1027d diagnostic: 2B <state> <views> <vt0..vt4> <age>: state bits 0 view on, 1 covered, 2 committed, 3 host idle
  // >= 1 s, 4 TRK + YES open, 5 splash, 6 our frame shown; the view list's length; the top view's vtable (vt0 << 28 |
  // .. | vt4); ticks since it changed. On a change, and once a second on the SRC page: logged when state / top change.
  kSrcDiag = 0x2B,
  // rui1038 (rytm-mods 0026 "1038"): Rytm -> host 4A <track> <on> <hi> <lo> <why>: the bank list of GRANULAR SRC /
  // WAVETABLE TBL / MULTISAMPLE INST opened by knob A on the SRC view (on 1, why 0; hi.lo P1 then) or closed (on 0: why 1
  // YES - commit the cursor with 48 <track> 0 <index> <bank << 4> <flags 1>, 2 NO, 3 another knob, 4 another key, 5 the
  // view ended). While open, knob A's turns (32 0) and LEFT RIGHT UP DOWN (31) are the list's.
  kBankList = 0x4A,
  kLeds = 0x34,  // 1026 (rytm-mods 0026 rui1026): host -> Rytm 0 (stock) | 1 <c1..c16> (palette 0..111) the trig keys' LEDs   // 1026 (provisional, rytm-mods 0026): Rytm -> host <kit 0..127>, the active kit (after each hello, on a change)
};
enum : int { kKeyLeft = 0x40, kKeyRight = 0x41, kKeyUp = 0x42, kKeyDown = 0x43, kKeyNo = 0x45, kKeyPage = 0x84 };
// 1022: the catch mask (37): what a page takes; everything else works on the Rytm as on its stock screen (pads, the
// trig and bank keys, MUTE / CHRO / SCNE / PERF, FIX, RTRG, FILL, FUNC, TEMPO, and on a voice page the LEVEL knob
// and its push and an encoder the page does not use (the status pages catch every knob); RECORD / PLAY / STOP are the Rytm's whatever the mask says). Every page takes the
// navigation keys and the keys that open or change a stock screen (rytm-mods 0026 design.md section 10: the screen
// would change under the page), plus the encoders it uses and their pushes. Not TRK (0.5.0, owner: "UP is now the
// Sidecar key"): the first version's TRK shortcuts are gone from the host's keys, and TRK is stock's again, so TRK +
// pad is stock's own selection and stock lights the pads while it is held (a quick second TRK can change the stock
// main screen under the page: cosmetic; UP + YES leaves without it). Before the first mask every key is still caught:
// poly::rytm_trk (31 0x86) covers that.
constexpr uint8_t kCatchKeys[] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x84,   // LEFT RIGHT UP DOWN YES NO PAGE
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35,         // TRIG SRC SMPL FLTR AMP LFO (the stock pages)
    0x10, 0xB0, 0xB4, 0xB5, 0xB1, 0x81, 0x82,   // FX GLOBAL SAMPLER PLAYMODE QPER SONG CHAIN
};
constexpr uint8_t kKeyPushA = 0x70;   // PUSH_A .. PUSH_H 0x70..0x77
constexpr int kEncData = 8;           // LEVEL / DATA (its push kKeyPushA + 8 = 0x78)
// rui1050: the SEQ page also catches FUNC - a caught FUNC is both sides' on the Rytm (rytm-mods 0026 key_owner 2: stock
// keeps it held, its FUNC combos stay), so the page knows it at an encoder's push (FUNC + push: a lock's SLIDE)
constexpr uint8_t kKeyFunc = 0x80;
constexpr int kMaskBytes = 3 + 37;
// A frame at most every kTickMs (status pages, the Rytm's own value changes); after a key or a turn at once (the next
// remote_tick at least kInputMs after the last frame). The Rytm renders at 20 Hz: a frame waits for its next render
// either way, so this is the host's part of the round trip, not more frames on the screen. 1020: 50 ms, no input path.
constexpr uint32_t kTickMs = 25, kInputMs = 8, kAliveMs = 250;
constexpr uint32_t kScreenTurnMs = 1500;   // 0.5.0 screens: how long a turned knob stays named on the title bar and in the bottom bar
// 0.4.3 (rytm-lag): every commit (39) makes the Rytm run its whole stock draw and panel diff (rytm-mods 0026 ev_tick
// sets ctrl + 0x20), bounded by its 156 250 baud panel link. While the Rytm's transport plays, a page that redraws by
// itself (the SRC view's values, SCOPE, a playhead) commits at most every kPlayCommitMs (5 a second, as the SEQ page's
// playhead already does; docs/SONG.md "Traffic to the Rytm"). What waits is not lost: the next commit diffs against
// what the Rytm has, so it carries the latest frame. On an open remote page a key or a turn of the user's own (31 / 32)
// goes at once, as when stopped; the SRC view gets the plain cap (the user's own knobs there come as 2A, which this
// file cannot tell from other value changes). Another page and a whole frame go at once. Stopped: unchanged.
constexpr uint32_t kPlayCommitMs = 200;
// 0.4.4: on the SRC view a knob's own value (a 2A of one value: a turn, or a CC into the Rytm) or a held step's (its
// locks as they are turned) commits at most every kSrcUserMs while playing: twice the plain cap, half the Rytm's
// render rate, so a turn follows the knob and a constant CC stream still does not redraw the Rytm at 20 Hz.
constexpr uint32_t kSrcUserMs = 100;
// The Rytm's 2B says the SRC view is covered (state bit 1: the machine list, any cover): nothing of it is drawn while it
// is. 0026 sends a 2B every second while the view is there, so a cover not renewed for kCoverMs is taken as ended (a lost
// 2B must never leave the view blank): one whole frame follows. No 2B at all (an older mod): never covered.
constexpr uint32_t kCoverMs = 3000;
// A QUICK PERF knob's fader move on the open MORPH page (it arrives as the Rytm's 2D reports, not as a 31 / 32): a commit
// at most every kQpCommitMs while playing; each commit makes the Rytm run its whole stock draw, so not every frame.
constexpr uint32_t kQpCommitMs = 50;
constexpr uint32_t kQpLapseMs = 100;
constexpr uint8_t kEchoNone = 0x7F;
// 1023: the connect splash (rytm-mods 0026 1023): the Rytm's hello (35) carries its uptime in 50 ms ticks; when it has
// been up kSplashMinUp or more (the link came up after the Rytm's boot, not with it: a cable plugged in, the Sidecar
// started later, a host restart) and the page is closed, the splash shows for kSplashMs: the icon slides in over
// kSlideMs, a line under it runs down to the end, then 36 0. The Rytm holds a splash kSplashTicks at most by itself.
constexpr uint32_t kSplashMs = 3000, kSlideMs = 150, kSlideStepMs = 50, kSplashStepMs = 100;
constexpr int kSplashMinUp = 60;          // 3 s of the Rytm's ticks
// The Rytm's first hello comes 0.5 s after its USB port came up (rytm-mods 0026 HELLO_TICKS), so a page the user opened
// within that half second is open when the hello arrives: the page opened on this link, not a stale one. An open older
// than this (a page open across an unplug: the unplug and the 0.5 s make the gap longer) is the old link's.
constexpr uint32_t kHelloOpenMs = 600;
constexpr uint8_t kSplashTicks = 64;      // 3.2 s: if our 36 0 is lost
constexpr RFont kRBold = {7, 10, 0, 7};   // the Rytm's bold font: 10 rows, 6 px glyphs + 1 [measured, rytm-emu 1.73]
// The USB trident, 42 x 19, drawn for this project: a round plug, the shaft and its arrow, a branch up to a round end and
// one down to a square end. Row bitmaps, bit x = column x.
constexpr int kIconW = 42, kIconH = 19, kIconX = 6, kIconY = 21;
UI_ROM const uint64_t kUsbIcon[kIconH] = {
    0x0000f000000, 0x0001f800000, 0x0001fff0000, 0x0001fff8000, 0x0001f80c000, 0x00c0f006000, 0x03c00003000,
    0x0fc0000181e, 0x1fc00000c3f, 0x3ffffffffff, 0x3ffffffffff, 0x1fc0006003f, 0x0fc000c001e, 0x03cfc180000,
    0x00cfc300000, 0x000ffe00000, 0x000ffc00000, 0x000fc000000, 0x000fc000000,
};

// the voice byte's machine (voice_report's flags) from a voice's state
CORE_COLD int machine_of_state(int vi) {
  const V& x = v[vi];
#if CORE_MSAMP
  if (x.user < 0 && x.ms) return kMultiMachine;   // MULTISAMPLE (the Rytm's 0x30, or msamp=voice:V)
  if (x.user < 0 && x.wt) return kWaveMachine;    // WAVETABLE (0x28, or wt=voice:V)
  if (x.user < 0 && x.gr) return kGranMachine;    // GRANULAR (0x38, or gr=voice:V)
  if (x.user < 0 && x.li) return kLiveMachine;    // LIVE IN (0x78, or livein=voice:V)
  if (x.user < 0 && x.cp) return kChopMachine;    // CHOP (0x18, or chop=voice:V)
  if (x.user < 0 && x.mi) return kMidiMachine;    // MIDI (0x43 / 0x44)
#endif
  return x.user >= 0 ? 0x50 + 8 * x.user : x.fm ? kFm : x.juno ? kJuno : x.grain ? kGrainMachine : x.dsps ? kDspSample : x.daisy ? kDaisy
                                                                                 : x.md ? kMd : kPlaits;
}
// The machine the Rytm's track plays, as its reports name it (the voice byte, the same flags the engine plays the voice
// by), or kNoMachine before the first report: the voice's state says PLAITS then, which an analog track, or one just
// set to JUNO / 6-OP FM on the SRC page, is not. 0021 reports a voice only while it sounds (a trig, or a knob while
// it plays; [measured, rytm-emu rui1023: the SRC page's machine list sends nothing]), so a track's page appears at its
// first trig on an EXT machine and follows every later report live (page_ids runs every frame).
constexpr int kNoMachine = -1;
bool reported[kVoices];
// 1025 (rytm-mods 0026 "1025", 33): the Rytm tells each track's machine itself, at once (the SRC list's cursor, YES, a
// kit or project load) and as a snapshot after its hello and the page's open: per track (0..11) the voice byte its
// 03 / 04 would carry, or 0x7F (not a Sidecar machine). Once one came, it decides alone; before (an older Rytm build),
// the voices' reports do.
constexpr uint8_t kNotOurs = 0x7F;
uint8_t track_vb[12];
// SAMPLER (machines.h kSmplMachine): 33's machine byte says 43 for a track whose vb is DSP SAMPLE's 0x20
bool track_smpl[12];
uint8_t track_rm[12];   // 0.5.0: 33's machine byte as it came (core/mod/rytm_param.h: the RT targets' synth labels)
// The voice each track plays on, as 0021's 03 / 04 carry it (the firmware's voice -> track owner table at 0x40290dd8,
// 00 04 01 05 08 06 0a 02 on rui1026's base [measured, rytm-emu dtpeek], and the Rytm's live 04s on the Pi [measured
// 2026-10-19: SD on JUNO trigs voice 2, CH on 6-OP FM voice 4, MT / HT voice 5]); a shared voice's second track (CP OH
// HT CB) takes it over at its trig. 0026's 33 puts its own table's voice in vb (BD 0, SD 1, RS/CP 2 ...), right only
// for BD and MT / HT, so its voice bits are replaced by this one's and only its machine flag is kept.
UI_ROM const uint8_t kTrackVoice[12] = {0, 2, 7, 7, 1, 3, 5, 5, 4, 4, 6, 6};
// 0x40 is MD (machines.h kMd): ours where the build has it (CORE_MD); on a build without, not ours (as analog)
// machine: 33's machine byte; MIDI has no flag (machines.h): 0x80 and the voice here, machine_of reads it
CORE_COLD uint8_t track_vb_of(int t, uint8_t vb, int machine) {
  vb &= 0x7F;
#if CORE_MSAMP
  if (machine == kRytmMidi && vb != kNotOurs && !(vb & 0x78)) return (uint8_t)(kMidiMachine | kTrackVoice[t]);
#else
  (void)machine;
#endif
#if !CORE_MD
  if ((vb & 0x78) == kMd) return kNotOurs;
#endif
  return vb == kNotOurs ? kNotOurs : (uint8_t)((vb & 0x78) | kTrackVoice[t]);
}
bool have_tracks = false;
// per pattern poly, review round 1: the tracks a 33 has named since the link came up (bit t). A single-track 33 (one
// track's machine changed in the SRC list) leaves the others unreported, not analog: remote_track_machine says -1 for
// them (the sequencer keeps such a track captured after a Pi reboot until the Rytm reports it)
uint16_t tracks_known = 0;
UI_ROM const char* const kTrackName[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
// A voice's name for the user: the track(s) that play it (core/voice_name.h), never the Sidecar's voice number (voice 1
// is BT's, voice 2 SD's).
CORE_COLD const char* voice_label(int vi) { return voice_name(vi); }
// rc040 review H1: another track on t's voice (a shared voice's pair) plays JUNO too
CORE_COLD bool juno_partner(int t) {
  for (int u = 0; u < 12; ++u)
    if (u != t && kTrackVoice[u] == kTrackVoice[t] && track_vb[u] != kNotOurs && machine_of(track_vb[u]) == kJuno) return true;
  return false;
}
// 0.5.0 (owner 2026-11-15, hardware: a new SAMPLER, a sample picked in the pool list: SAMPLE EDIT said NO SAMPLE until it
// had played; WAVETABLE knew its table only after a note): the voice's own state comes from the Rytm's 0x03 / 0x04
// reports, which come only while it plays, so a pick or a turn on an idle voice reached the host as the SRC view's 2A
// alone (src14) and stayed there. A new src14 of track t selects that entry on its voice as the next report would:
// WAVETABLE's TBL, GRANULAR's SRC, MULTISAMPLE's INST (P1: their tables, samples and instruments load, their views show
// them). CHOP takes src14 itself (chop.cpp preload_step); DSP SAMPLE / SAMPLER's SMP is read from src14 where it is
// used (SAMPLE EDIT: sedit::open_track; the SRC view draws the 2A's), the voice's state left as its reports set it.
// Only on a voice whose last report was that machine (its flags) and no other track of ours on the voice with the same
// machine (whose sound the voice plays is the one that trigged last: unknown here).
CORE_COLD void src_to_voice(int t, int c14, int k = 0) {
  if (!have_tracks || t < 0 || t >= 12 || track_vb[t] == kNotOurs || c14 < 0) return;
#if CORE_MSAMP
  const int m = machine_of(track_vb[t]), vi = kTrackVoice[t];
  for (int u = 0; u < 12; ++u)
    if (u != t && kTrackVoice[u] == vi && track_vb[u] != kNotOurs && machine_of(track_vb[u]) == m) return;
  const V& x = v[vi];
  const int e = srclib::code_of14(c14);
  if (k == 12) {   // 2026-11-24 TBLB: WAVETABLE's table B (P13)
    if (m == kWaveMachine && x.wt) wt::kit_src_b(vi, e);
    return;
  }
  if (m == kWaveMachine && x.wt) wt::kit_src(vi, e);
  else if (m == kGranMachine && x.gr) gr::kit_src(vi, e);
  else if (m == kMultiMachine && x.ms) msamp::kit_src(vi, e);
#endif
}
CORE_COLD int machine_of_voice(int vi) {
  vi &= 7;
  if (have_tracks) {
    for (int t = 0; t < 12; ++t)   // the first track on this voice with a Sidecar machine (CP after RS, OH after CH...)
      if (track_vb[t] != kNotOurs && (track_vb[t] & 7) == vi) return machine_of(track_vb[t]);
    return kNoMachine;
  }
  return reported[vi] && (CORE_MD || !v[vi].md) ? machine_of_state(vi) : kNoMachine;   // (MD without CORE_MD: none)
}
CORE_COLD const char* short_name(int m) { return m == kNoMachine ? "-" : machine_short(m); }   // (core/machines.h)
// SAMPLER on voice vi: its first Sidecar track's 33 once one came (as machine_of_voice), else its reports' tag
CORE_COLD bool voice_smpl(int vi) {
  vi &= 7;
  if (have_tracks) {
    for (int t = 0; t < 12; ++t)
      if (track_vb[t] != kNotOurs && (track_vb[t] & 7) == vi) return track_smpl[t] && machine_of(track_vb[t]) == kDspSample;
    return false;
  }
  return v[vi].smpl && v[vi].dsps && !v[vi].grain;
}
// The machine a voice's name and page go by: SAMPLER's own number for DSP SAMPLE's flag with SAMPLER's tag / 33
CORE_COLD int named_machine(int m, int vi) { return m == kDspSample && voice_smpl(vi) ? kSmplMachine : m; }

CORE_COLD int v_param(int vi, int k) {
  int x = (int)v[vi & 7].p[k & 15];
  return x < 0 ? 0 : x > 127 ? 127 : x;
}

// The SRC view's source: a track's P1..P16 as the Rytm's stock page shows them (2A), the voice's own until they come;
// a sample's data from the voice the track plays on. Only reads: the engine has the sound from 0021's reports.
class TrackSource : public Source {
 public:
  int t = 0;
  CORE_COLD int param(int k) const override;
  CORE_COLD int voice() const override { return kTrackVoice[t]; }
  CORE_COLD int code(int k) const override;   // rui1038: the 2A's 14 bits (the bank in its fraction)
};

class VoiceSource : public Source {
 public:
  int vi = 0;
  CORE_COLD int param(int k) const override {
    int x = (int)v[vi].p[k & 15];
    return x < 0 ? 0 : x > 127 ? 127 : x;
  }
  CORE_COLD int voice() const override { return vi; }
};

// The status pages (the first two): SIDECAR, every track's machine and the send FX (TYPE, PRE / POST); SYSTEM, U1..U5,
// the recorder, Overbridge and underruns, each core's load as a bar, the network (up / down and the address only).
// Stock geometry: the 9-row title bar (the page dots at its right), the 9-row bottom bar, rows 10..53 between.
char pair_code[12];   // remote_pair_code: "1234 5678", "" none
struct StatusView {
  RemoteStatus st = {-1, 0, {-1, -1, -1}, 0, 0, 0, 0, {0, 0, 0, 0}, {{0}, {0}, {0}, {0}, {0}}};
  CORE_COLD void draw(Canvas& c, int which, int page, int npages) {
    c.clear();
    // SYSTEM carries the Sidecar's version in its title ("SYSTEM 0.1.0": 12 characters at 5 px = 60 px, clear of the page
    // dots from x = 78; docs/versioning.md); the commit is on the web pages and the console
    header(c, which ? "SYSTEM " SIDECAR_VERSION : kStatusTitle, page, npages, nullptr);
    if (which) system(c);
    else sidecar(c);
  }
  // Every item fits by the widest advance (5 px, kRSmall) or is centred (put_text bounds a left-aligned item by that
  // advance, which cut "PLAITS" to "PLAIT" in the last column).
  CORE_COLD void sidecar(Canvas& c) {
    char b[kTextLen + 1];
    // the voices in the Rytm's track order (BD SD RS/CP BT / LT MT/HT CH/OH CY/CB): two rows of four 32 px columns, the
    // voice's first track (two letters; of a shared voice's two the other is on the touchscreen and the web) in a 10 x 7
    // badge (pixels: the Rytm inverts only a glyph's own cell, which drew a bare inverted glyph), the machine centred in
    // the 22 px after it (the Rytm draws "PLAITS" 24 px wide [measured, rytm-emu rui1021])
    static UI_ROM const uint8_t kShownOrder[8] = {0, 2, 7, 1, 3, 5, 4, 6};
    const bool box = device::foreign();
    for (int n = 0; n < (box ? device::active().voices : kVoices); ++n) {
      int vi = box ? n : kShownOrder[n];
      int x = 32 * (n & 3), y = 13 + 11 * (n >> 2);
      c.fill(Rect{(int16_t)x, (int16_t)(y - 1), 10, 7}, kOn);
      sniprintf(b, sizeof b, "%.2s", voice_name(vi));
      c.put_text(kSlotFree + 2 * n, kRSmall, x + 1, y, b, kTextInvert);
      c.put_text(kSlotFree + 2 * n + 1, kRSmall, x + 21, y, short_name(named_machine(machine_of_voice(vi), vi)), kAlignCentre);
    }
    // FX1, FX2 a row each: "FX1" in a badge, the TYPE's name (12 characters: 60 px), PRE / POST at the right
    for (int sl = 0; sl < 2; ++sl) {
      int y = 38 + 9 * sl;
      const char* t = sl < fx::kSlots ? fx::type_name(fx::control.knob[sl][0], sl) : nullptr;
      bool off = !t || !*t || !strcmp(t, "OFF");
      bool post = fx::control.route == fx::kRouteMain ||
                  (fx::control.route == fx::kRouteSlot && fx::control.knob[sl][1] >= 64);
      c.fill(Rect{1, (int16_t)(y - 1), 17, 7}, kOn);
      sniprintf(b, sizeof b, CORE_STR("FX%d"), sl + 1);
      c.put_text(kSlotFree + 16 + 3 * sl, kRSmall, 2, y, b, kTextInvert);
      sniprintf(b, sizeof b, CORE_STR("%.12s"), off ? "OFF" : t);
      c.put_text(kSlotFree + 17 + 3 * sl, kRSmall, 24, y, b);
      c.put_text(kSlotFree + 18 + 3 * sl, kRSmall, 126, y, off ? nullptr : post ? "POST" : "PRE", kAlignRight);
    }
    if (pair_code[0]) {   // the pairing code (remote_pair_code), between the voices and the send FX
      sniprintf(b, sizeof b, CORE_STR("PAIR %s"), pair_code);
      c.put_text(kSlotFree + 22, kRSmall, 64, 31, b, kAlignCentre);
    }
#if CORE_POLY
    const char* kl = keys::line(b, sizeof b);   // the Pi's own keyboards: the last one played and where it went (core/keys.h)
    footer(c, kl ? kl : "YES: MENU  NO: LEAVE");   // else the menu and how to leave (UX review R3 / R4)
#else
    footer(c, "YES: MENU  NO: LEAVE");
#endif
  }
  CORE_COLD void system(Canvas& c) {
    char b[kTextLen + 1];
    // U1..U5 in two 64 px columns: "U1 " and the whole name (8 characters: 55 px at most)
    for (int u = 0; u < 5; ++u) {
      sniprintf(b, sizeof b, CORE_STR("U%d %.8s"), u + 1, st.user[u][0] ? st.user[u] : "-");
      c.put_text(kSlotFree + u, kRSmall, 2 + 64 * (u & 1), 12 + 8 * (u >> 1), b);
    }
    // the recorder beside U5: a dot and the time while recording (hours of minutes fit), else its state
    static const char* const kRec[] = {"REC -", "REC IDLE", "REC ARM", "REC", "REC OFF"};
    if (st.rec == 3) {
      c.fill(Rect{66, 29, 3, 3}, kOn);
      sniprintf(b, sizeof b, CORE_STR("REC %lu:%02lu"), (unsigned long)(st.rec_secs / 60), (unsigned long)(st.rec_secs % 60));
      c.put_text(kSlotFree + 5, kRSmall, 71, 28, b);
    } else {
      sniprintf(b, sizeof b, "%s", kRec[st.rec < 5 ? st.rec : 0]);
      c.put_text(kSlotFree + 5, kRSmall, 66, 28, b);
    }
    sniprintf(b, sizeof b, CORE_STR("OB %s"), st.ob < 0 ? "-" : st.ob ? "OK" : "STOP");
    c.put_text(kSlotFree + 6, kRSmall, 2, 37, b);
    sniprintf(b, sizeof b, CORE_STR("UND %lu"), (unsigned long)st.underruns);
#if CORE_MSAMP
    c.put_text(kSlotFree + 7, kRSmall, 44, 37, b);
    c.put_text(kSlotFree + 11, kRSmall, 86, 37, sdtest::line());   // the SD card test's verdict (msamp/sdtest.h)
#else
    c.put_text(kSlotFree + 7, kRSmall, 66, 37, b);
#endif
    for (int k = 0; k < 3; ++k) {   // each core: a bar (rows 46..50), its percent beside it
      Rect o = {(int16_t)(2 + 42 * k), 46, 18, 5};
      c.rect(o, kOn);
      int pct = st.cpu[k];
      if (pct >= 0) {
        int w = (pct > 100 ? 100 : pct) * (o.w - 2) / 100;
        if (w) c.fill(Rect{(int16_t)(o.x + 1), (int16_t)(o.y + 1), (int16_t)w, 3}, kOn);
        sniprintf(b, sizeof b, CORE_STR("%d%%"), pct);
      } else {
        c.dither(Rect{(int16_t)(o.x + 1), (int16_t)(o.y + 1), (int16_t)(o.w - 2), 3});
        strcpy(b, "-");
      }
      c.put_text(kSlotFree + 8 + k, kRSmall, o.x + o.w + 2, 46, b);
    }
    // the network in the bottom bar: up or down and the address, never the Wi-Fi's name or key
    const uint8_t* ip = st.ip;
    bool addr = ip[0] | ip[1] | ip[2] | ip[3];
    if (st.eth == 2 && addr) sniprintf(b, sizeof b, CORE_STR("ETH UP %u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
    else if (st.wifi == 2 && addr) sniprintf(b, sizeof b, CORE_STR("WIFI UP %u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
    else if (st.eth == 2 || st.wifi == 2) sniprintf(b, sizeof b, CORE_STR("%s UP, NO ADDRESS"), st.eth == 2 ? "ETH" : "WIFI");
    else if (st.eth || st.wifi) strcpy(b, "NET DOWN");
    else strcpy(b, "NET -");
    footer(c, b);
  }
};

constexpr uint8_t kPageLfo = 1;   // 30's page byte (rui1026): 0 the default (SIDECAR), 1 the LFO page
constexpr uint8_t kPageSeq = 0x10;   // rui1036: 0x10 + t (16..27) the SEQ page of track t (rui1037: TRK + TRIG; rui1036: FUNC + LFO)
constexpr uint8_t kPageIns = 2;      // fx-ins: 2 the INSERT page (proposed rytm-mods 0026 mapping: TRK + FX; docs/INSERT-FX.md)
constexpr uint8_t kPageInsOut = 3;   // rui1047: 3 the INSERT page on its OUT view (UP + AMP; stereo / POST, docs/STEREO.md); an older host reads it as the SIDECAR page
int kit_said = -1;                   // the kit the last 3B named
constexpr uint8_t kPageArp = 4;      // 7ae: UP + FLTR, the ARP page (page_arp.cpp); an older host reads it as the SIDECAR page
constexpr uint8_t kPagePoly = 0x20;  // rui1048: 0x20 + t (32..43) POLY CONFIG of track t (the Rytm's track menu, FUNC + CHRO; page_poly.cpp); an older host reads it as the SIDECAR page
constexpr int kExtra0 = -10;   // s.voice of extra page k (extra.h: SCOPE, REC): kExtra0 - k
constexpr int kNoPage = 1000;   // no page id (voices 0.., -1 SIDECAR, -2 SYSTEM, extra pages kExtra0 - k)
constexpr int kMaxIds = 2 + kVoices + 16;

struct State {
  bool open = false;
  int voice = -1;            // the page: -1 SIDECAR, -2 SYSTEM, kExtra0 - k extra page k, else the voice whose machine page shows
  bool sent_valid = false;   // `sent` is what the Rytm's shadow holds
  bool page_changed = true;
  uint8_t seq = 0;
  uint32_t last_draw = 0, last_tx = 0;
  bool clock_set = false;
  bool map_over = false;     // rui1050: UP's map shown over the open page (49 1 5 1), map_back the page under it
  int map_back = -1;
  bool up_down = false;      // 0.5.0 FILL: UP held on the open page (its 31s, whichever page takes them): up_right_fill
  // 0.5.0 (spec-050 2.6): the keys an engine page with key_up took at their press (bit c: code c), and that page
  uint32_t held_keys[8] = {};
  const ExtraPage* held_page = nullptr;
  // 0.5.0 UI feel: the held UP / DOWN / LEFT / RIGHT a page took repeats (kRepeatFirstMs ...): its code (-1 none), the page,
  // the press, the next repeat
  int rep_code = -1, rep_voice = 0;
  uint32_t rep_t0 = 0, rep_next = 0;
  // 0.5.0 v2 (EDIT, momentary: the owner 2026-11-14): PAGE held on a page that locks (its step held back until it is
  // let go alone: page_owed); the trig keys the gesture took (T15 / T16 = the side, scene::edit_side(); T1..T12 picked
  // that side's scene), their releases its too; the target a turn locked last while the side is held (the bottom bar
  // shows its locked value), 0 none
  bool page_down = false, page_owed = false;
  uint8_t push_eaten = 0;   // 0.5.2: the SRC view's pushes (A..H) a held side took (their releases too)
  uint16_t edit_keys = 0;
  uint16_t edit_tid = 0;
  bool edit_refused = false;   // review: a knob with no target turned under the held side (NOT LOCKABLE)
  bool edit_unset = false;     // v2: a turn with the side's scene unset (SET A: T1-12)
  // rui1051 (the user's report on rui1050: NO on a page opened by its shortcut went to SIDECAR, a second NO out), nav044
  // (0.4.4, the user: "several NOs to leave after visiting more than one tab" - rui1051 walked back through the pages
  // the shortcuts came from): the page shown was put up by a shortcut (UP / TRK + key, UP + T, the host's own page
  // request), so NO leaves at once, whatever it was opened over. A page picked in the MENU or reached by LEFT / RIGHT
  // is not one (NO goes to SIDECAR, as before); the page closing forgets it.
  bool shortcut = false;
  // sedit050 (the 0.4.4 merge): the Sidecar page SAMPLE EDIT was opened over (POOL's EDIT) and that page's
  // shortcut flag, so closing the edit goes back there; kNoPage: opened from the Rytm's own screen (the SRC view) or
  // the MENU, so closing it follows nav044 (a shortcut's page closes, else SIDECAR)
  int sedit_back = kNoPage;
  bool sedit_back_sc = false;
  // ux1027: the 30's page byte (kPageLfo, kPageIns, kPageInsOut) that put up the page shown (via_voice): the same UP +
  // key again on it leaves (as NO), as UP + TRIG leaves the SEQ page (rytm-mods 0026 rui1037)
  int via = -1, via_voice = 0;
  // held edits: per voice a mask of P1..P16, their values, the Rytm's last value of each, the machine they are for
  uint16_t held[kVoices] = {};
  uint8_t held_v[kVoices][16] = {};
  uint8_t rytm_v[kVoices][16] = {};
  uint8_t held_m[kVoices] = {};
  bool rytm_seen[kVoices] = {};
  bool self = false;         // remote's own 0x03 in voice_report: no hold logic
  bool input = false;        // a key or a turn since the last frame: draw at once
  bool user = false;         // 0.4.3: a key or a turn of the user's (31 / 32) since the last commit: not held while playing
  bool qp_user = false;      // a QPERF fader move on the open MORPH page since the last commit (kQpCommitMs while playing)
  uint32_t qp_ms = 0;        // when it was last set: the flag lapses after kQpLapseMs without another move (no commit may follow)
  bool src_user = false;     // 0.4.4: a knob's value on the SRC view (a 2A of one value, or a held step's) since the last commit
  uint32_t commit_ms = 0;    // 0.4.3: the last page / SRC view commit (kPlayCommitMs) and what it showed (page_key)
  int commit_page = -1000;
  bool covered = false;      // the last 2B said the SRC view is covered; cover_ms: when (the renewal is the 2B's heartbeat)
  uint32_t cover_ms = 0;
  int touched = -1;          // the encoder turned last (0..7): its cell's tiles go first, the bottom bar names it
  uint32_t touch_ms = 0;     // 0.5.0 screens: when (they name the knob for kScreenTurnMs; touched / src_last >= 0 say it is set)
  uint32_t src_last_ms = 0;  // ... the SRC view's last changed knob
  // the local echo (3F) as the Rytm has it: per encoder the slot (kEchoNone: none), the value, flags and prefix
  uint8_t echo_slot[8], echo_val[8], echo_flags[8];
  char echo_pre[8][12];
  bool echo_valid = false;
  bool legacy = false;       // the 1020 behaviour (tests: before / after): 50 ms frames, no echo, one commit
  uint8_t mask[kMaskBytes] = {};   // 1022: the catch mask the Rytm has (37), valid after the first since opening
  bool mask_valid = false;
  bool splash = false;       // 1023: the connect splash runs (the page closed)
  uint32_t now_ms = 0, open_ms = 0;   // the last remote_tick's clock; when the page was last opened (30 on)
  // 0.5.2 re-hello: Rytm traffic came but no hello (the host started after the Rytm's port-up hellos): 3C with flags bit 0
  // asks for one (rehello_tick); tries counted, the last at rh_ms
  bool rh_traffic = false;
  uint8_t rh_tries = 0;
  uint32_t rh_ms = 0;
  bool caps36 = false;       // the Rytm's hello said it understands 36 (the splash, the popups)
  int caps = 0;              // fix1049: the hello's caps byte whole (bit 1: 16 send FX label tables, rui1049)
  uint16_t p13_last = 0;     // 2026-11-24 TBLB: bit t, the last P(k+1) touched on track t was P13 (the names bank follows it)
  int caps2 = 0;             // 0.5.0 FILL: the hello's 4th byte (bit 0 kCaps2Arrows: UP + LEFT / RIGHT as 49 1 1 15 / 16)
  bool presets_told = false; // 0.4.0: the Rytm knows, since its last hello, that JUNO's / FM's PAT lists are drawn here (4A 7F 1)
  bool popup = false, popup_clock = false;   // F1 / F4: a popup owed / running (the page closed)
  bool popup_over = false, modal_shown = false;   // the pairing code's modal: over a page too, and drawn last frame
  uint32_t popup_t0 = 0, popup_last = 0, popups = 0, popup_ms = 0;   // popup_ms: how long it stays (0: kPopupMs)
  uint32_t popup_36 = 0;     // the last 36 sent for it (the page closed: the Rytm's own count, kPopupWindowMs)
  int modal_key = -1;        // the key that took the modal off (over an open page): its release is eaten too
  uint32_t status_popups = 0;   // of them, the status popups (not `over`): what a status popup's caller waits for
  char popup_l1[kTextLen + 1], popup_l2[kTextLen + 1];
  bool underrun_told = false;   // F4: the session's first underrun popped up
  bool splash_clock = false;
  uint32_t splash_t0 = 0;
  uint8_t leds[16] = {};     // 1026: the trig LEDs the Rytm has from us (34 mode 1), valid while leds_on
  bool leds_on = false;
  // 1027: the SRC view: the track whose stock SRC page the Rytm shows (-1 none), whether we draw it (1: frames sent
  // since its 29 t 1, 0 none, -1 not decided yet), the Rytm's own values of each track's P1..P16 (2A; `src_known` which came), the one
  // changed last (2A n 1: the bottom bar names it)
  int8_t src_track = -1, src_told = -1, src_last = -1;
  uint8_t srcv[12][16] = {};
  // 1027e: a track's sound values while a held step's (2A <t | 0x40>) are drawn: put back at its unflagged 2A or at
  // any 29 (the view gone or moved: the Rytm's release there sends nothing), so only the drawing ever saw the locks
  uint8_t keep_v[12][16] = {}, keep_l[12][16] = {};
  uint16_t keep_known[12] = {}, keep_mask = 0;
  uint8_t srcl[12][16] = {};      // rui1038: the 2A's fraction bytes (P1 of GRANULAR / WAVETABLE / MULTISAMPLE: bank << 4)
  int16_t src14[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // P1's last 14 bits by 2A or our 48 (song)
  // 0.5.0: which is newer, the Rytm's own word on a track's src14 (its 2A; our 48s are only asked) or its voice's last
  // report (remote_track_src14_fresh): one count of both
  uint32_t src14_at[12] = {}, rep_at[8] = {}, at_n = 0;
  int16_t wr_pat[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // P1 our 48 wrote, its answer owed (presetpick)
  uint16_t wr_cnt[12][16] = {};  // the Rytm's 48 answers received (vparam follows only the last write's)
  uint8_t wr_ans[12][16] = {};   // remote_track_write's answers: 0 none, 1 owed, 2 + the Rytm's status
  uint16_t src_known[12] = {};
  // rc5 (page-2 defaults): the Rytm's machine list on top (2B), each track's voice byte when it opened, the tracks a
  // one-track 33 moved while it was open; after it closed, the tracks owed page 2 (p2_due): the voice byte at the open
  // (p2_from), the one written for (p2_vb), the stage (0 waiting for the choice to settle, 1 P9..P12 sent, 2 P13..P16
  // sent), its time, the params still to send (p2_want) and not answered yet (p2_ans), the resends
  bool mlist = false;
  uint8_t mlist_vb[12] = {};
  uint16_t mlist_moved = 0;
  uint16_t p2_due = 0;
  uint8_t p2_from[12] = {}, p2_vb[12] = {}, p2_stage[12] = {}, p2_want[12] = {}, p2_ans[12] = {}, p2_tries[12] = {};
  uint32_t p2_ms[12] = {};
  RemoteStats stats = {};
};

CORE_RAM2 State s;               // RAM2 on the Teensy, as the canvases (constructed at boot)
CORE_RAM2 Canvas work, sent;   // 2 KB
PageView view;
VoiceSource vsrc;
CORE_RAM2 StatusView ssrc;
TrackSource tsrc;
bool src_log = false;   // the console's "rui=srclog=1": each 29 / 2A and SRC view commit logged
// The Rytm shows the machine pages on its own SRC page (rui1027+: it sent a 29, 2A or 2B): the TRK + YES list then has
// no voice pages (SIDECAR, SYSTEM and the live pages); an older build keeps them. Until remote_reset (a boot).
bool src_capable = false;
// rui1036b (49): the display's page requests and the Rytm's selected track (its machine, name, P1..P8 and labels)
struct Disp {
  volatile uint32_t req_gen;
  volatile int req_page;
  volatile uint32_t sel_gen;
  volatile int sel = -1, machine = -1;
  // 0.5.0 (LFO FINAL): the FX track selected (a 0026 that reports it: 49 1 2 12 ..., the proposal in docs/lfo.md; sel
  // keeps the last sound track for every other reader) and the LFO page's count of the selection's changes
  volatile bool fx = false;
  volatile uint32_t lfo_gen;
  char name[13];
  uint8_t val[8];
  char label[8][5];
  volatile uint32_t info_gen;
} disp;
const char* want_title = nullptr;   // remote_show_page: the page to show at the next open

CORE_COLD int TrackSource::param(int k) const {
  k &= 15;
  return s.src_known[t] >> k & 1 ? s.srcv[t][k] : v_param(kTrackVoice[t], k);
}
CORE_COLD int TrackSource::code(int k) const {
  k &= 15;
  if (s.src_known[t] >> k & 1) return srclib::code_of14(s.srcv[t][k] << 7 | s.srcl[t][k]);
  return Source::code(k);
}

// A box draws no text of ours (the Rytm's text routine has no counterpart there): its frames carry the text in their pixels, in the host's font (preview()), so only 38s and a 39 go.
// `wire` is what the diff and the send read: `work` on the Rytm, whose bytes stay what they were. The items stay in `work`, which the pages draw over.
CORE_RAM2 Canvas baked;
Canvas* wire = &work;
CORE_COLD void wire_ready() {
  wire = &work;
  if (!device::foreign()) return;
  preview(work, baked.fb);
  memset(baked.txt, 0, sizeof baked.txt);
  wire = &baked;
}

CORE_COLD void send(const uint8_t* m, int n) {
  to_rytm(m, n);
  s.stats.bytes += (uint32_t)n;
}

CORE_COLD int head(uint8_t* m, uint8_t cmd) {
  m[0] = 0xF0, m[1] = 0x7D, m[2] = 0x52, m[3] = 0x54, m[4] = cmd, m[5] = 1;
  return 6;
}

CORE_COLD void send_short(uint8_t cmd, const uint8_t* b, int nb) {   // nb <= kMaskBytes
  uint8_t m[8 + kMaskBytes];
  int n = head(m, cmd);
  for (int i = 0; i < nb; ++i) m[n++] = b[i] & 0x7F;
  m[n++] = 0xF7;
  send(m, n);
}

// 38: tile columns c0..c0+nc-1 of framebuffer page p, column-major (byte = page + 8 x column), 7 -> 8 packed
CORE_COLD void send_rect(int c0, int p, int nc) {
  uint8_t raw[8 * kTileCols], m[16 + 8 * kTileCols + kTileCols * 8 / 7 + 2];
  int nr = 0;
  for (int x = c0 * 8; x < (c0 + nc) * 8; ++x) raw[nr++] = wire->fb[p + 8 * x];
  int n = head(m, kRect);
  m[n++] = s.seq;
  m[n++] = (uint8_t)c0;
  m[n++] = (uint8_t)p;
  m[n++] = (uint8_t)nc;
  m[n++] = 1;
  for (int i = 0; i < nr; i += 7) {
    int g = nr - i < 7 ? nr - i : 7;
    uint8_t hi = 0;
    for (int j = 0; j < g; ++j) hi |= (uint8_t)((raw[i + j] >> 7) << j);
    m[n++] = hi;
    for (int j = 0; j < g; ++j) m[n++] = raw[i + j] & 0x7F;
  }
  m[n++] = 0xF7;
  send(m, n);
  s.stats.rects++;
}

// 3D: slot i of `work` (empty: the slot cleared)
CORE_COLD void send_text(int i) {
  const TextItem& t = wire->txt[i];
  uint8_t m[16 + kTextLen];
  int n = head(m, kText);
  m[n++] = s.seq, m[n++] = (uint8_t)i, m[n++] = t.x & 0x7F, m[n++] = t.y & 0x3F, m[n++] = t.font & 0x7F;
  m[n++] = t.flags & 0x7F;
  for (const char* c = t.s; *c; ++c) m[n++] = (uint8_t)*c & 0x7F;
  m[n++] = 0xF7;
  send(m, n);
  s.stats.texts++;
}

CORE_COLD const ExtraPage* extra_of(int id) { return id <= kExtra0 ? &extra_page(kExtra0 - id) : nullptr; }
// ux1027 (the user, 2026-10-27: "it should simply replace that page and not work like some kind of modal with more
// sibling pages"): a page off the LEFT / RIGHT ring (ExtraPage::off_ring: LFO, POLY, CHOP, MENU, KEYS; page_keys.h
// sc_shortcut_only: SEQ, INSERT, POOL, CHOP) replaces the Rytm's screen as the SRC view does - no page dots, and LEFT /
// RIGHT / DOWN / PAGE are its own or nothing, never the ring's. NO (or its UP + key again) leaves it.
CORE_COLD bool standalone(const ExtraPage* x) { return x && (x->off_ring || sc_shortcut_only(x->title)); }
CORE_COLD bool scrolls_updown(const ExtraPage* x);   // (below, with the mask's rule)
// ux1027 (review 5): the page shown goes from `from` to `to`: leaving the LFO page drops its design view (it opens on
// the main view again, T1..T16 not caught)
CORE_COLD void lfo_left(int from, int to) {
  const ExtraPage* x = extra_of(from);
  if (from != to && x && !strcmp(x->title, "LFO")) lfo_page_reset();
  if (from != to && x && (!strcmp(x->title, "INSERT") || !strcmp(x->title, "MASTER"))) sc_mon_page_left();   // 0.5.2: MON ends with the page
}

CORE_COLD void shortcut_over(int from) {   // a page shown by a shortcut over `from` (the MENU's pick: none)
  const ExtraPage* x = extra_of(from);
  s.shortcut = !(x && !strcmp(x->title, "MENU"));
}

// The pages in order (the title bar's dots): SIDECAR, SYSTEM, the voices with a page, the extra pages shown.
CORE_COLD int page_ids(int* ids) {
  int n = 0;
  ids[n++] = -1, ids[n++] = -2;
  for (int vi = 0; vi < kVoices && !src_capable; ++vi)
    if (page_for_voice(named_machine(machine_of_voice(vi), vi), vi)) ids[n++] = vi;
  for (int k = 0; k < extra_pages() && n < kMaxIds; ++k) {
    // R3: MENU / KEYS only while shown; rui1051: the pages only their keys open too (page_keys.h sc_shortcut_only)
    if ((extra_page(k).off_ring || sc_shortcut_only(extra_page(k).title)) && s.voice != kExtra0 - k) continue;
    if (!extra_for_box(extra_page(k).title)) continue;
    if (!extra_page(k).shown || extra_page(k).shown()) ids[n++] = kExtra0 - k;
  }
  return n;
}

CORE_COLD int page_at(const int* ids, int n) {   // the current page's index (0 when it is not in the list)
  for (int i = 0; i < n; ++i)
    if (ids[i] == s.voice) return i;
  return 0;
}

CORE_COLD void go(int dir) {   // dir: -1 previous, +1 next, 0 SIDECAR
  int ids[kMaxIds], n = page_ids(ids);
  int at = page_at(ids, n);
  at = dir == 0 ? 0 : (at + dir + n) % n;
  int nv = ids[at];
  s.shortcut = false;   // rui1051: the ring (or SIDECAR) forgets that a shortcut put the page up
  if (nv != s.voice) lfo_left(s.voice, nv), s.voice = nv, s.page_changed = true, s.touched = -1, s.via = -1;
}

// nav044: NO on a page a shortcut put up leaves in one press - the page asked closed (3A 0 0) - not back through the
// pages under it (true: done)
CORE_COLD bool go_back() {
  if (!s.shortcut) return false;
  s.shortcut = false, s.via = -1;
  remote_request(false);
  return true;
}

#if CORE_POLY && CORE_MSAMP
// sedit050: SAMPLE EDIT closed: back to the Sidecar page it was opened over, with that page's own NO rule (true: done)
CORE_COLD bool sedit_back_to() {
  int b = s.sedit_back;
  s.sedit_back = kNoPage;
  if (b == kNoPage || b == s.voice) return false;
  const ExtraPage* x = extra_of(b);
  if (x && x->shown && !x->shown()) return false;   // (gone from the list meanwhile)
  if (b >= 0 && (src_capable || !page_for_voice(named_machine(machine_of_voice(b), b), b))) return false;
  lfo_left(s.voice, b), s.voice = b, s.page_changed = true, s.touched = -1, s.via = -1;
  s.shortcut = s.sedit_back_sc;
  return true;
}
#endif

CORE_COLD void set_param(int vi, int k, int nv) {
  mod::manual(mod::tid(mod::kV, vi, k), (float)nv);   // 0.5.0: a page edit wins over a scene's morph (spec-050 2.5)
  V& x = v[vi];
  int m = machine_of_voice(vi);
  if (s.held_m[vi] != m) s.held[vi] = 0, s.held_m[vi] = (uint8_t)m;
  if (!(s.held[vi] & (1u << k))) s.rytm_v[vi][k] = (uint8_t)(int)mod::base_of(vi, k, x.p[k]);   // the Rytm's value until now
  s.held[vi] |= (uint16_t)(1u << k);
  s.held_v[vi][k] = (uint8_t)nv;
  // a 0x03 PARAMS for the voice, its machine and every P (the edited one whole, the others as they are)
  uint8_t r[41] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(m | vi)};
  int n = 6;
  uint32_t t0 = now_us();
  for (int i = 0; i < 16; ++i) {
    float p = i == k ? (float)nv : mod::base_of(vi, i, x.p[i]);   // an LFO's target: its base, not the modulated value
    int iv = (int)p;
    iv = iv < 0 ? 0 : iv > 127 ? 127 : iv;
    int fr = (int)((p - iv) * 128.0f);
    r[n++] = (uint8_t)iv;
    r[n++] = (uint8_t)(fr < 0 ? 0 : fr > 127 ? 127 : fr);
  }
  // SAMPLER stays SAMPLER: the track the edit is for (the Rytm's selected one, when it plays this voice: a shared voice's
  // pair may be one of each) says, else the voice (its last report, or its page's track)
  const int t = disp.sel;
  bool smpl = have_tracks && t >= 0 && t < 12 && kTrackVoice[t] == vi && track_vb[t] != kNotOurs
                  ? track_smpl[t] && machine_of(track_vb[t]) == kDspSample
                  : x.smpl || voice_smpl(vi);
  if (m == kDspSample && smpl) r[n++] = (uint8_t)(kSmplTag >> 7 & 0x7F), r[n++] = (uint8_t)(kSmplTag & 0x7F);
  r[n++] = 0xF7;
  mod::note_lookup(now_us() - t0);
  s.self = true;
  voice_report(0x03, r, n);
  s.self = false;
  s.stats.edits++;
}

// 0.5.0 v2 (EDIT, momentary): on a voice page and the pages that name their knobs' targets (ExtraPage::enc_tid:
// INSERT, MASTER), PAGE held takes T15 / T16; PAGE + T15 (T16) held is EDIT A (B): a turn locks the knob's target into
// that side's scene (from the scene's lock, else from the base) instead of editing it, and T1..T12 make scene n that
// side. T15 / T16 alone stay the Rytm's trigs; PAGE alone steps the page at its release. The SEQ page keeps its trig keys.
CORE_COLD bool edit_page(const ExtraPage* x) {
  if (!s.open) return false;
  return x ? x->enc_tid != nullptr : s.voice >= 0 && !src_capable;
}
CORE_COLD uint16_t page_trigs(const ExtraPage* x) {
  uint16_t t = x && x->trigs_now ? x->trigs_now() : 0;
  if (!edit_page(x)) return t;
  return (uint16_t)(t | (scene::edit_side() >= 0 ? 0xCFFF : s.page_down ? 0xC000 : 0));
}
CORE_COLD void edit_end() {   // the side let go (or the gesture dropped): EDIT off
  scene::set_edit_side(-1);
  s.edit_tid = 0, s.edit_refused = false, s.edit_unset = false;
}
// the side's lock of t moved by `units` (whole units of the target; lo..hi its range there), from the lock, else from
// the base; the value a voice page's step gives when `voice_nv` >= 0
CORE_COLD void edit_lock(mod::Tid t, int units, int voice_nv) {
  const int n = scene::side_scene(scene::edit_side());
  if (n < 0) {   // the side has no scene: refused (SET A: T1-12)
    s.edit_unset = true, s.edit_tid = 0;
    return;
  }
  s.edit_unset = false;
  const scene::Scene& sc = scene::state().s[n];
  int cur = -1;
  for (int i = 0; i < sc.n; ++i)
    if (sc.l[i].t == t) cur = sc.l[i].v;
  int nv;
  if (voice_nv >= 0) {
    nv = voice_nv * 128;
  } else {
    mod::Range r = mod::range(t);
    bool off = mod::is_offset(mod::kind_of(t));
    int lo = off || r.lo < 0 ? 0 : (int)r.lo, hi = off || r.hi > 127 ? 127 : (int)r.hi;
    int u = cur >= 0 ? (cur + 64) / 128 : (int)(mod::base(t) + 0.5f);
    u += units;
    nv = (u < lo ? lo : u > hi ? hi : u) * 128;
  }
  if (scene::lock(n, t, (uint16_t)nv)) s.edit_tid = t;
}

CORE_COLD void on_enc(int e, int delta, bool func) {
  s.input = true;
  const bool lock = scene::edit() && edit_page(extra_of(s.voice));
  if (const ExtraPage* x = extra_of(s.voice)) {
    if (e < 0 || e >= 8 || !(extra_encs(*x) >> e & 1)) return;
    if (!lock) x->enc(e, delta, func);
    else if (mod::Tid t = x->enc_tid(e)) edit_lock(t, delta * (func ? 8 : 1), -1), s.edit_refused = false;
    else s.edit_refused = true, s.edit_tid = 0;
    return;
  }
  if (s.voice < 0 || src_capable || e < 0 || e > 7) return;
  s.touched = e, s.touch_ms = now_ms();
  const Page* p = page_for_voice(named_machine(machine_of_voice(s.voice), s.voice), s.voice);
  if (!p || p->enc[e] == kNone) return;
  int k = p->enc[e] & 15, cur = (int)mod::base_of(s.voice, k, v[s.voice].p[k]);
  detent_scope(s.voice);
  if (is_list(*p, k)) delta = list_turn(e, delta, list_size(*p, k));
  if (!delta) return;
  if (lock) {   // EDIT: the scene's lock, from its value (else the base), never the voice
    mod::Tid t = mod::tid(mod::kV, s.voice, k);
    const int n = scene::side_scene(scene::edit_side());
    if (n >= 0)
      for (int i = 0; i < scene::state().s[n].n; ++i)
        if (scene::state().s[n].l[i].t == t) cur = (scene::state().s[n].l[i].v + 64) / 128;
    edit_lock(t, 0, step_value(*p, k, cur < 0 ? 0 : cur > 127 ? 127 : cur, delta, func));
    s.edit_refused = false;
    return;
  }
  int nv = step_value(*p, k, cur < 0 ? 0 : cur > 127 ? 127 : cur, delta, func);
  if (nv != cur) set_param(s.voice, k, nv);
}

// 0.5.0: every key still held goes up at the page that took it (a hello, the page closing: the releases are lost)
CORE_COLD void release_held() {
  const ExtraPage* x = s.held_page;
  for (int c = 0; c < 256; ++c)
    if (s.held_keys[c >> 5] >> (c & 31) & 1) {
      s.held_keys[c >> 5] &= ~(1u << (c & 31));
      if (x && x->key_up) x->key_up(c);
    }
  s.held_page = nullptr;
  s.edit_keys = 0, s.page_owed = false;
  edit_end();
  s.rep_code = -1;   // (and nothing repeats on: the release is lost too)
}

CORE_COLD bool key_held(int code) { return code >= 0 && code < 256 && (s.held_keys[code >> 5] >> (code & 31) & 1); }

// 0.5.0 FILL (owner, 2026-11-15 on hardware: "UP + RIGHT (FILL mode) doesn't do anything"): rytm-mods 0026 sends UP +
// RIGHT as 49 1 1 16 only while its page is closed. A hand holds UP a moment before the arrow: after 0.4 s the Rytm asks
// for the combo map (49 1 4 1), the host opens the page on KEYS, and RIGHT then reaches that open page as a 31, where it
// did nothing, and UP's release closed the map: nothing at all. With a page open UP and RIGHT are always its 31s. So
// RIGHT with UP held is FILL here too: UP held on the open page (its 31), or UP's map up (from the stock screen, 49 1 4;
// over a page, 49 1 5). FILL itself keeps its own UP + RIGHT (it closes: the combo toggles).
CORE_COLD bool up_right_fill() {
#if CORE_POLY
  if (!s.up_down && !keys_map_on() && !s.map_over) return false;
  const ExtraPage* x = extra_of(s.voice);
  if (x && !strcmp(x->title, "FILL")) {   // FILL's own UP + RIGHT (it closes), also when UP went down before FILL opened
    if (x->key) x->key(kKeyUp);           // (its press never reached FILL: the map's, or held over the open)
    return false;
  }
  fill_page_open(-1);   // (UP's release then leaves FILL up: the map's end closes or restores only KEYS)
  return true;
#else
  return false;
#endif
}


CORE_COLD void key_down(int code);

CORE_COLD void on_key(int code, bool down) {
  if (!down) {   // 0.5.0: a release goes to the page that took its press (key_up), else nowhere
    if (code == kKeyPage && s.page_owed) {   // v2: PAGE let go alone: its own step now
      s.page_owed = false, s.input = true;
      key_down(kKeyPage);
      return;
    }
    if (code >= 0 && code < 16 && (s.edit_keys >> code & 1)) {   // v2: a key of the EDIT gesture let go
      s.edit_keys &= (uint16_t)~(1u << code);
      if (code == 14 + scene::edit_side()) edit_end();
      s.input = true;
      return;
    }
    if (key_held(code)) {
      s.held_keys[code >> 5] &= ~(1u << (code & 31));
      s.input = true;
      if (s.held_page && s.held_page->key_up) s.held_page->key_up(code);
    }
    return;
  }
  s.input = true;
  if (edit_page(extra_of(s.voice))) {   // v2: the EDIT gesture
    const int side = scene::edit_side();
    if (code == kKeyPage) {   // held back: it may start the gesture (its step at the release, alone); under EDIT nothing
      if (side < 0) s.page_owed = true;
      return;
    }
    if ((code == 0x0E || code == 0x0F) && side < 0 && s.page_down) {   // PAGE + T15 / T16: EDIT A / B
      s.page_owed = false, s.edit_keys |= (uint16_t)(1u << code);
      edit_end();
      scene::set_edit_side(code - 0x0E);
      return;
    }
    if (code >= 0 && code < 16 && side >= 0) {   // under the side: T1..T12 make scene n that side; the rest nothing
      s.edit_keys |= (uint16_t)(1u << code);
      if (code < scene::kScenes) {
        const scene::State& st = scene::state();
        scene::set_ab(side == 0 ? code : st.a, side == 1 ? code : st.b);
        s.edit_unset = false;
      }
      return;
    }
  }
  key_down(code);
}

CORE_COLD void key_down(int code) {
  if (const ExtraPage* x = extra_of(s.voice))
    if (x->key && x->key(code)) {
      if (x->key_up && code >= 0 && code < 256) {
        if (s.held_page != x) release_held();
        s.held_page = x, s.held_keys[code >> 5] |= 1u << (code & 31);
      }
      // 0.5.0: UP / DOWN of a page that scrolls with them, LEFT / RIGHT it took: they repeat while held
      if (code == kKeyLeft || code == kKeyRight || ((code == kKeyUp || code == kKeyDown) && scrolls_updown(x)))
        s.rep_code = code, s.rep_voice = s.voice, s.rep_t0 = now_ms(), s.rep_next = s.rep_t0 + kRepeatFirstMs;
#if CORE_POLY && CORE_MSAMP
      // SAMPLE EDIT's DISCARD EDIT? YES closed the edit: the page is out of the list, so back where it was opened from
      // (SAMPLE EDIT alone: other pages a key hides keep their own way, PROJECT's answer note among them)
      if (x == &kSeditPage && extra_of(s.voice) == x && !x->shown() && !sedit_back_to() && !go_back()) go(0);
#endif
      return;
    }
  if (code == kKeyUp) return;   // fix1049: UP is the Sidecar's modifier (page_keys.h), no page arrow: LEFT / RIGHT step
  if (standalone(extra_of(s.voice)) && (code == kKeyLeft || code == kKeyRight || code == kKeyDown || code == kKeyPage))
    return;   // ux1027: no siblings
  if (code == kKeyLeft) go(-1);
  else if (code == kKeyRight || code == kKeyDown || code == kKeyPage) go(+1);
#if CORE_POLY && CORE_MSAMP
  else if (code == kKeyNo && extra_of(s.voice) == &kSeditPage && sedit_back_to()) {}   // sedit050: back to its opener
#endif
  else if (code == kKeyNo && go_back()) {}                           // nav044: a shortcut's page: out at once
  else if (code == kKeyNo && s.voice == -1) remote_request(false);   // NO on SIDECAR leaves, as TRK + YES (3A 0 0)
  else if (code == 0x44 && s.voice == -1) menu_open();               // YES on SIDECAR: the MENU (UX review R3)
  else if (code == kKeyNo) go(0);
}

// The bottom bar of a voice page: the touched encoder's parameter (or encoder A's) as stock's "BD LEV:", with a level
// box; the encoders' local echo bindings follow it.
CORE_COLD const Cell* cell_of(const Page& p, int param, int* idx) {
  for (int i = 0; i < p.ncells; ++i)
    for (int k = 0; k < 4; ++k)
      if (p.cells[i].in[k] == param) { *idx = i; return &p.cells[i]; }
  return nullptr;
}

CORE_COLD void voice_footer(const Page& p, const char* name, const Source& src, int e) {
  int k = p.enc[e], ci = -1;
  char b[kTextLen + 1];
  if (k == kNone) { footer(work, nullptr); return; }
  const Cell* c = cell_of(p, k, &ci);
  int v = src.param(k);
  if (const char* nm = src_value_name(p, k, src)) {   // GRANULAR's SRC, WAVETABLE's TBL, MULTISAMPLE's INST: the name
    sniprintf(b, sizeof b, CORE_STR("%s %s:%.16s"), name, p.label[e], nm);
    footer(work, b);
  } else if (c && c->kind == kList) {
    sniprintf(b, sizeof b, CORE_STR("%s %s:%.8s"), name, p.label[e], list_name(c->names, eng_zone((float)v, list_count(c->names))));
    footer(work, b);
  } else if (c && (c->kind == kKnobBi || c->kind == kPan)) {
    sniprintf(b, sizeof b, CORE_STR("%s %s:%s%d"), name, p.label[e], v < 64 ? "-" : "+", v < 64 ? 64 - v : v - 64);
    footer(work, b, v, true);
  } else {
    sniprintf(b, sizeof b, CORE_STR("%s %s:%d"), name, p.label[e], v);
    footer(work, b, v);
  }
}

// 3F for every encoder whose binding the Rytm does not have yet: numeric parameters echo into the bottom bar's slot
// ("BD HRM:" + the value), lists and the SIDECAR page none.
CORE_COLD void send_echoes(const Page* p, int vi) {
  if (device::foreign()) return;
  for (int e = 0; e < 8; ++e) {
    uint8_t slot = kEchoNone, val = 0, fl = 0;
    char pre[12] = "";
    int ci = -1;
    const Cell* c = p && !s.legacy && p->enc[e] != kNone ? cell_of(*p, p->enc[e], &ci) : nullptr;
    if (c && c->kind != kList && !src_value_name(*p, p->enc[e], vsrc)) {   // (a source's name: the frame shows it)
      slot = (uint8_t)voice_screen_echo_slot(p, e);
      if (!slot) slot = kEchoNone;
      val = (uint8_t)v_param(vi, p->enc[e]);
      fl = 1 | (c->kind == kKnobBi || c->kind == kPan ? 2 : 0);   // FUNC x 8 as step_value; bipolar shown signed
      if (slot == kSlotFoot) sniprintf(pre, sizeof pre, CORE_STR("%s %s:"), voice_label(vi), p->label[e]);
    }
    if (s.echo_valid && s.echo_slot[e] == slot && (slot == kEchoNone || (s.echo_val[e] == val && s.echo_flags[e] == fl &&
                                                                           !strcmp(s.echo_pre[e], pre))))
      continue;
    uint8_t m[24];
    int n = head(m, kEcho);
    m[n++] = (uint8_t)e, m[n++] = slot, m[n++] = val & 0x7F, m[n++] = 0, m[n++] = 127, m[n++] = fl;
    for (const char* q = pre; *q; ++q) m[n++] = (uint8_t)*q & 0x7F;
    m[n++] = 0xF7;
    send(m, n);
    s.echo_slot[e] = slot, s.echo_val[e] = val, s.echo_flags[e] = fl;
    strcpy(s.echo_pre[e], pre);
    s.stats.echoes++;
  }
  s.echo_valid = true;
}

// 37 when the page's catch mask is not the one the Rytm has: the navigation and stock-screen keys, the encoders the
// page uses and their pushes (the status pages: all nine, LEVEL too, so a turn there edits nothing underneath; the live
// pages without a Page, SCOPE / REC, pass their own encoders in `encs`).
#if CORE_POLY
CORE_COLD bool seq_data() { return seq::data_wanted(); }
CORE_COLD bool seq_yes() { return seq::yes_wanted(); }
CORE_COLD bool seq_updown() { return seq::updown_wanted(); }
#else
CORE_COLD bool seq_data() { return false; }
CORE_COLD bool seq_yes() { return true; }
CORE_COLD bool seq_updown() { return true; }
#endif
// Whether page x (null: a voice page or SIDECAR) names UP / DOWN in the catch mask (updown044: a ring page always, a page
// alone only when it sets `updown`, the SEQ page per moment). send_mask's `updown`, and 0.5.0's "UP held on a page that
// scrolls with it".
CORE_COLD bool claims_updown(const ExtraPage* x) {
  if (x && !strcmp(x->title, "SEQ")) return seq_updown();
  // t050-d (the owner: "insert eq and then the down key does nothing"): INSERT stands alone, so UP / DOWN stay the Rytm's
  // except on its EQ and SIDECHAIN views, where DOWN goes in and out of SIDECHAIN
  return !standalone(x) || x->updown || (x == &kInsPage && ins_wants_updown());
}
// 0.5.0 UI feel: the page really moves with UP / DOWN (its rows, its banks), so UP held on it is a scroll and the combo map
// (49 1 5 1) stays closed. claims_updown, less the ring pages that leave UP alone (a voice page, SCOPE: DOWN is their next
// page; they do not set `updown`) and the pages that use UP as their modifier (key_up: CHOP's ALL, FILL's UP + RIGHT).
CORE_COLD bool scrolls_updown(const ExtraPage* x) {
  if (!x || !claims_updown(x)) return false;
  return !strcmp(x->title, "SEQ") || (x->updown && !x->key_up);
}
// U1 NOTE EDIT: data - the SEQ page on a poly track also takes DATA (LEVEL, encoder 8) and its push (0x78): rytm-mods
// 0026 forwards any encoder 0..15 the mask names as 32 <e> (design.md 1022, "LEVEL = 8" [measured]), no mod change
// ux1027: yes false - the SEQ page leaves YES to the Rytm (seq::yes_wanted: its YES combos, the quick saves); rytm-mods
// 0026 catches per key (key_owner), so YES is named only while the page wants it (a question, FUNC or a trig held)
// updown1029 (the user, 2026-10-29), updown044 (0.4.4: every page alone, ExtraPage::updown): updown false - the page
// leaves UP / DOWN to the Rytm (seq::updown_wanted: CHRO's octave on the pads), named only on the track menu or with
// FUNC held (FUNC + DOWN: the poly track's menu)
CORE_COLD void send_mask(const Page* p, uint8_t encs = 0, uint16_t trigs = 0, bool func = false, bool data = false,
                         bool yes = true, bool updown = true) {
  uint8_t b[kMaskBytes] = {};
  if (data) b[kEncData / 7] |= (uint8_t)(1u << (kEncData % 7)), b[3 + (kKeyPushA + kEncData) / 7] |= (uint8_t)(1u << ((kKeyPushA + kEncData) % 7));
  for (uint8_t k : kCatchKeys)
    if ((yes || k != 0x44) && (updown || (k != kKeyUp && k != kKeyDown))) b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
  if (func) b[3 + kKeyFunc / 7] |= (uint8_t)(1u << (kKeyFunc % 7));   // rui1050: FUNC on both sides (the Rytm keeps it too)
  for (int k = 0; k < 16; ++k)   // T1..T16 (key codes 0..15): a page that takes them now (the LFO design view)
    if (trigs >> k & 1) b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
  if (!p && !encs)
    for (int e = 0; e < 9; ++e) {   // A..H and LEVEL (index 8, push 0x78)
      int k = kKeyPushA + e;
      b[e / 7] |= (uint8_t)(1u << (e % 7));
      b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
    }
  for (int e = 0; e < 8; ++e)
    if ((p && p->enc[e] != kNone) || (encs >> e & 1)) {
      int k = kKeyPushA + e;
      b[e / 7] |= (uint8_t)(1u << (e % 7));
      b[3 + k / 7] |= (uint8_t)(1u << (k % 7));
    }
  if (s.mask_valid && !memcmp(b, s.mask, sizeof b)) return;
  int n = sizeof b;
  while (n > 3 && !b[n - 1]) --n;   // the key bytes up to the last caught code
  send_short(kMask, b, n);
  memcpy(s.mask, b, sizeof b);
  s.mask_valid = true;
  s.stats.masks++;
}

// 34 (rui1026): the trig keys' LEDs, mode 1 with c[0..15] (palette colours, kLed*), or mode 0 alone (stock again)
CORE_COLD void send_leds(const uint8_t* c) {
  uint8_t b[17] = {};
  b[0] = c ? 1 : 0;
  if (c) memcpy(b + 1, c, 16);
  send_short(kLeds, b, c ? 17 : 1);
  s.leds_on = c != nullptr;
  if (c) memcpy(s.leds, c, 16);
  s.stats.leds++;
}
// A page that lights the trig keys (ExtraPage::leds_now): sent when it shows and on every change, mode 0 when it is left
CORE_COLD void update_leds(const ExtraPage* x) {
  if (device::foreign()) return;
  uint8_t c[16];
  bool want = !s.legacy && x && x->leds_now && x->leds_now(c);
  if (want && (!s.leds_on || memcmp(c, s.leds, 16))) send_leds(c);
  else if (!want && s.leds_on) send_leds(nullptr);
}

// One commit: the tiles in `tiles` and the text slots in `texts` (from `work`), then 39. Returns the tiles sent.
CORE_COLD int send_part(const uint32_t tiles[4], uint64_t texts, bool clear_first) {
  int n = 0;
  s.seq = (uint8_t)((s.seq + 1) & 0x7F);
  if (clear_first && !device::foreign()) {
    uint8_t b = s.seq;
    send_short(kTextClear, &b, 1);
  }
  for (int pg = 0; pg < kTilePages; ++pg)
    for (int c = 0; c < kTileCols;) {
      int t = pg * kTileCols + c;
      if (!(tiles[t >> 5] >> (t & 31) & 1)) { ++c; continue; }
      int c0 = c;
      while (c < kTileCols && (tiles[(pg * kTileCols + c) >> 5] >> ((pg * kTileCols + c) & 31) & 1)) ++c;
      send_rect(c0, pg, c - c0);
      n += c - c0;
    }
  for (int i = 0; i < kTextSlots; ++i)
    if (texts >> i & 1) send_text(i);
  uint8_t b = s.seq;
  send_short(kCommit, &b, 1);
  s.stats.frames++;
  return n;
}

// 0.4.3 (rytm-lag): the Rytm's transport plays (seq's TRANSPORT, rytm_dump::set_playing)
CORE_COLD bool rytm_plays() {
#if CORE_POLY
  return rytm_dump::is_playing();
#else
  return false;
#endif
}
// 0.4.3: a page's changed frame waits (kPlayCommitMs) - only while playing; never a whole frame, another page than the
// last commit's (a page switch: UP's map, a page shown) or a 31 / 32 of the user's on an open page (the SRC view's 2A
// values are capped too)
// 1027e: the sound's values back for every track a held step's are drawn on (-1), or for track t
CORE_COLD void src_unhold(int t) {
  for (int k = 0; k < 12; ++k)
    if ((t < 0 || k == t) && (s.keep_mask >> k & 1)) {
      memcpy(s.srcv[k], s.keep_v[k], 16), memcpy(s.srcl[k], s.keep_l[k], 16);
      s.src_known[k] = s.keep_known[k], s.keep_mask &= (uint16_t)~(1u << k);
      if (k == s.src_track) s.input = true;
    }
}
CORE_COLD int page_key() { return s.open ? s.voice : 1000 + s.src_track; }
CORE_COLD bool commit_held(uint32_t ms) {
  return !s.legacy && s.sent_valid && !s.user && page_key() == s.commit_page && rytm_plays() &&
         ms - s.commit_ms < (s.src_user ? kSrcUserMs : s.qp_user && ms - s.qp_ms < kQpLapseMs ? kQpCommitMs : kPlayCommitMs);
}
CORE_COLD void committed(uint32_t ms) { s.commit_ms = ms, s.user = false, s.src_user = false, s.qp_user = false, s.commit_page = page_key(); }
// The cover is over (a 2B without it, or none for kCoverMs): the Rytm's shadow is not ours any more, so one whole frame
// and commit of what is current now go out at the next tick, not held.
CORE_COLD void cover_ended() {
  s.covered = false, s.sent_valid = false, s.page_changed = true, s.input = true;
}
CORE_COLD bool src_covered(uint32_t ms) {
  if (s.covered && (int32_t)(ms - s.cover_ms) >= (int32_t)kCoverMs) cover_ended();   // (signed: a stamp after ms is not over)
  return s.covered;
}

// The tiles a rectangle touches (glass rows y..y+h-1: page 7 - row / 8).
CORE_COLD void rect_tiles(const Rect& r, uint32_t m[4]) {
  for (int x = r.x / 8; x <= (r.x + r.w - 1) / 8 && x < kTileCols; ++x)
    for (int y = r.y / 8; y <= (r.y + r.h - 1) / 8 && y < 8; ++y) {
      int t = (7 - y) * kTileCols + x;
      m[t >> 5] |= 1u << (t & 31);
    }
}

// the pairing code's modal (below): a small box over whatever is showing
constexpr int kModalX = 24, kModalY = 16, kModalW = 80, kModalH = 32;
constexpr int kSlotModal = kTextSlots - 2;   // above anything a page uses (kSlotFree is 32)
CORE_COLD void modal_draw(Canvas& c);
CORE_COLD bool modal_tick(uint32_t ms);

CORE_COLD void frame(uint32_t ms) {
  uint32_t t0 = now_us();
  const Page* p = nullptr;
  if (s.voice >= 0 && src_capable) s.voice = -1, s.page_changed = true;   // a voice page shown when the Rytm said 29 / 2A / 2B
  if (s.voice >= 0) {
    p = page_for_voice(named_machine(machine_of_voice(s.voice), s.voice), s.voice);
    if (!p) s.voice = -1, s.page_changed = true;
    vsrc.vi = s.voice;
    if (p) p = src_list_page(p, vsrc);   // P1 turned: its list (GRANULAR / WAVETABLE / MULTISAMPLE)
  }
  if (s.page_changed) view.invalidate(), s.page_changed = false;
  int ids[kMaxIds], n = page_ids(ids), at = page_at(ids, n);
  if (s.voice <= kExtra0 && ids[at] != s.voice) s.voice = -1, at = 0, s.via = -1;   // an extra page no longer shown
  const ExtraPage* x = extra_of(s.voice);
  if (standalone(x)) at = 0, n = 1;   // ux1027: no siblings, no page dots
  tap_enable(x && x->tap);
  char tag[12] = "";   // 0.5.0 v2: EDIT held (PAGE + T15 / T16) - every page's header says so, with the side
  if (scene::edit()) sniprintf(tag, sizeof tag, "EDIT %c", scene::edit_side() ? 'B' : 'A');
  header_tag(p ? "" : tag);   // (a voice page puts it after the voice's name itself)
  int drawn = 1;
  if (p) {
    char right[16];
    vsrc.vi = s.voice;
    if (tag[0]) sniprintf(right, sizeof right, "%s %s", voice_label(s.voice), tag);   // UX 7: the voice kept ("V1 EDIT A")
    else sniprintf(right, sizeof right, "%s", voice_label(s.voice));
    const bool turned = s.touched >= 0 && turn_live(ms, s.touch_ms, kScreenTurnMs);   // 0.5.0 screens: a turn names the knob for a moment
    if (!voice_screen(work, view, *p, vsrc, right, at, n, turned ? s.touched : -1, false, &drawn)) {
      drawn = view.draw(work, *p, vsrc, right, at, n);
      voice_footer(*p, voice_label(s.voice), vsrc, s.touched >= 0 ? s.touched : 0);
    }
  } else if (x) {
    view.invalidate();
    x->draw(work, at, n, ms);
  } else {
    view.invalidate();   // the status pages are drawn whole; a voice page after them starts clean
    ssrc.draw(work, s.voice == -2 ? 1 : 0, at, n);
  }
  const int edit_n = scene::side_scene(scene::edit_side());
  if (scene::edit() && s.edit_unset && edit_page(x)) {   // v2: the side has no scene yet
    footer(work, scene::edit_side() ? "SET B: T1-12" : "SET A: T1-12");
  } else if (scene::edit() && s.edit_refused && edit_page(x) && edit_n >= 0) {   // EDIT: that knob names no target
    char b[kTextLen + 1];
    sniprintf(b, sizeof b, "S%d: NOT LOCKABLE", edit_n + 1);
    footer(work, b);
  }
  if (scene::edit() && s.edit_tid && edit_page(x) && edit_n >= 0) {   // EDIT: the bottom bar shows the scene's locked value
    const scene::Scene& sc = scene::state().s[edit_n];
    for (int i = 0; i < sc.n; ++i)
      if (sc.l[i].t == s.edit_tid) {
        char nm[16], val[8], b[kTextLen + 1];
        mod::label(s.edit_tid, nm, sizeof nm);   // (its machine's name for the param: "V1:HRM")
        morph_value_text(s.edit_tid, sc.l[i].v, val, sizeof val);
        int n2 = sniprintf(b, sizeof b, "S%d %s:%s", edit_n + 1, nm, val);
        footer(work, b, n2 <= 16 ? sc.l[i].v >> 7 : -1);
      }
  }
  s.stats.widgets += (uint32_t)drawn;
  {   // the pairing code's modal goes on top of the page, and the page comes back whole when it goes
    bool on = modal_tick(ms);
    if (on) modal_draw(work);
    else if (s.modal_shown) {
      work.put_text(kSlotModal, kRSmall, 0, 0, nullptr);
      work.put_text(kSlotModal + 1, kRSmall, 0, 0, nullptr);
      view.invalidate(), s.page_changed = true;   // as a page change does: redrawn from the model, not from pixels
    }
    s.modal_shown = on;
  }
  bool sq = x && !strcmp(x->title, "SEQ");
  // 0.5.0: MORPH's FUNC + YES (EDIT) needs FUNC (both sides'); SAMPLE EDIT's FUNC + YES (save) too, LOOP's FUNC + T8
  bool fn = sq || (x && (!strcmp(x->title, "MORPH") || !strcmp(x->title, "SAMPLE EDIT") || !strcmp(x->title, "FILL") || !strcmp(x->title, "LOOP")));   // FILL: FUNC + YES / NO; LOOP: FUNC + T8 (a cut)
  bool updown = claims_updown(x);   // updown044: a page alone only when it uses them
  if (!s.legacy) send_mask(p, x ? extra_encs(*x) : 0, page_trigs(x), fn, sq && seq_data(),
                            !sq || seq_yes(), updown);   // before the frame: the keys and knobs the page shows are its own from now on
  update_leds(x);
  if (!s.sent_valid) s.echo_valid = false;
  send_echoes(p, s.voice);
  uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint64_t texts = 0;
  wire_ready();
  int tiles = s.sent_valid ? wire->diff_tiles(sent, mask) : kTiles;
#if CORE_UI_TOOLS
  work.check_overlap();
#endif
  int ntext = s.sent_valid ? wire->diff_text(sent, &texts) : 0;
  if (!s.sent_valid)
    for (int i = 0; i < kTextSlots; ++i)
      if (wire->txt[i].s[0]) texts |= 1ull << i, ++ntext;
  if ((tiles || ntext) && !commit_held(ms)) {
    // the touched knob's cell and the bottom bar first, in a commit of their own, when there is more
    uint32_t first[4] = {}, rest[4];
    uint64_t tfirst = 0;
    int ci = -1;
    if (!s.legacy && s.sent_valid && p && s.touched >= 0 && p->enc[s.touched] != kNone && (s.user || !rytm_plays()) &&
        cell_of(*p, p->enc[s.touched], &ci)) {
      const Cell& c = p->cells[ci];
      rect_tiles(cell(c.col, c.row, c.cols, c.rows), first);
      rect_tiles(Rect{0, kFooterY, kW, kFooterH}, first);
      tfirst = (1ull << (kSlotCells + ci)) | (1ull << kSlotFoot);
    }
    int nf = 0, nr = 0;
    for (int i = 0; i < 4; ++i) rest[i] = mask[i] & ~first[i], first[i] &= mask[i], nf += __builtin_popcount(first[i]),
                                nr += __builtin_popcount(rest[i]);
    uint64_t trest = texts & ~tfirst;
    tfirst &= texts;
    if ((nf || tfirst) && (nr || trest)) {
      send_part(first, tfirst, false);
      send_part(rest, trest, false);
      s.stats.split++;
    } else {
      send_part(mask, texts, !s.sent_valid);
    }
    committed(ms);
    sent = *wire;
    s.sent_valid = true;
    s.last_tx = ms;
    s.stats.tiles += (uint32_t)tiles;
    uint32_t dt = now_us() - t0;
    s.stats.draw_us_sum += dt;
    if (dt > s.stats.draw_us_max) s.stats.draw_us_max = dt;
  }
  s.stats.page = (uint8_t)(x ? 3 : s.voice == -2 ? 2 : s.voice < 0 ? 0 : 1);
  s.stats.voice = (uint8_t)(x ? kExtra0 - s.voice : s.voice < 0 ? 0 : s.voice);
}

// ---- 1027: the SRC view. The machine page of the track whose stock SRC page shows, drawn from the Rytm's own values
// (2A): every key and knob stays the Rytm's (no 37, no 3F, no 34), the host only draws. A track whose machine has no
// page gets none (nothing sent, the Rytm shows stock; 3A 0 0 if we drew it before). The TRK + YES page comes first:
// while it is open the view waits; after it a whole frame again.
CORE_COLD int track_machine(int t) {
#if CORE_MSAMP
  // msamp=voice:V (the fallback until the Rytm reports 0x30 itself): a Sidecar track on V shows MULTISAMPLE
  if (nm::forced(kTrackVoice[t]) && (!have_tracks || (track_vb[t] != kNotOurs && user_slot_of(track_vb[t] & 0x78) < 0)))
    return nm::forced(kTrackVoice[t]);   // (wt= / gr=voice:V the same way)
  if (livein::forced(kTrackVoice[t]) && (!have_tracks || (track_vb[t] != kNotOurs && user_slot_of(track_vb[t] & 0x78) < 0)))
    return kLiveMachine;   // livein=voice:V: the LIVE IN page on a Sidecar track of V
#endif
  if (have_tracks) return track_vb[t] == kNotOurs ? kNoMachine : machine_of(track_vb[t]);
  return machine_of_voice(kTrackVoice[t]);
}
// track_machine, with SAMPLER's own number (machines.h kSmplMachine) for its pages and lists: 33's machine byte 43
CORE_COLD int track_named(int t) {
  int m = track_machine(t);
  return m == kDspSample && (have_tracks ? track_smpl[t] : voice_smpl(kTrackVoice[t])) ? kSmplMachine : m;
}
CORE_COLD const Page* track_page(int t) {   // DSP SAMPLE's GRAIN view by the Rytm's MOD (P12) once it came
  tsrc.t = t;   // test-1031: 6-OP FM / MULTISAMPLE with a chord on show it (the Rytm's CHD..VOI, poly::view)
  const int m = track_named(t);
  if (const Page* bp = src_bank_page(t, m)) return bp;   // rui1038: the bank list open on this track
  return src_list_page(chord_view_page(src_view_page(page_for_voice(m, kTrackVoice[t], s.src_known[t] >> 11 & 1 ? s.srcv[t][11] : -1)), tsrc), tsrc);
}

CORE_COLD void src_draws(bool page) {
  if (!page && s.src_told == 1 && !s.open) remote_request(false);   // 3A 0 0: stock again until our next commit
  s.src_told = (int8_t)(page ? 1 : 0);
}

CORE_COLD void src_tick(uint32_t ms) {
  int t = s.src_track;
  const Page* p = track_page(t);
  if ((p ? 1 : 0) != s.src_told) {
    src_draws(p != nullptr);
    if (p) s.sent_valid = false, s.page_changed = true, s.clock_set = false;   // the Rytm's shadow: a whole frame
  }
  if (!p) return;
  const bool covered = src_covered(ms);   // (the engine follows the machine at once; only the drawing waits)
  if (covered) s.sent_valid = false;
  wave_idle(ms);   // the sample overview of the Rytm's SMP being read (one column at most)
  if (!s.clock_set) s.clock_set = true, s.last_draw = ms - kTickMs, s.last_tx = ms;
  // a 2A at once (the Rytm sends one a 20 Hz tick at most; the 2As a USB read brought are taken before this tick, so a
  // burst is drawn once, with its latest values)
  if (!covered && (s.input || ms - s.last_draw >= kTickMs)) {
    s.last_draw = ms;
    s.input = false;
    if (s.page_changed) view.invalidate(), s.page_changed = false;
    tsrc.t = t;
    char right[12];   // UX review S1: a poly track says so in the SRC view's title ("BD POLY")
#if CORE_POLY
    sniprintf(right, sizeof right, "%s%s", kTrackName[t], seq::owns_track(t) ? " POLY" : "");
#else
    sniprintf(right, sizeof right, "%s", kTrackName[t]);
#endif
    int drawn = 0;
    const int e = enc_of_param(*p, s.src_last);   // -1: a parameter on no knob of this page (a CC, a lock): no knob named
    const bool turned = e >= 0 && turn_live(ms, s.src_last_ms, kScreenTurnMs);
    if (!voice_screen(work, view, *p, tsrc, right, 0, 0, turned ? e : -1, true, &drawn)) {
      drawn = view.draw(work, *p, tsrc, right, 0, 0);
      voice_footer(*p, kTrackName[t], tsrc, e < 0 ? 0 : e);
    }
    s.stats.widgets += (uint32_t)drawn;
    uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint64_t texts = 0;
    wire = &work;   // (a box has no SRC view)
    int tiles = s.sent_valid ? work.diff_tiles(sent, mask) : kTiles;
#if CORE_UI_TOOLS
    work.check_overlap();
#endif
    if (s.sent_valid) work.diff_text(sent, &texts);
    else
      for (int i = 0; i < kTextSlots; ++i)
        if (work.txt[i].s[0]) texts |= 1ull << i;
    if ((tiles || texts) && !commit_held(ms)) {
      bool whole = !s.sent_valid;
      send_part(mask, texts, whole);
      committed(ms);
      sent = work;
      s.sent_valid = true;
      s.last_tx = ms;
      s.stats.tiles += (uint32_t)tiles;
      s.stats.src_frames++;
      if (src_log) core::log("rui src: commit %lu ms, track %d, %d tiles, texts %llx%s", (unsigned long)ms, t, tiles,
                       (unsigned long long)texts, whole ? " (whole)" : "");
    }
    s.stats.page = 4;
    s.stats.voice = (uint8_t)t;
  }
  if (ms - s.last_tx >= kAliveMs) {
    send_short(kAlive, nullptr, 0);
    s.last_tx = ms;
    s.stats.keepalives++;
  }
}

// The splash at t ms: the stock title bar (the product's name, USB at its right), the icon (sliding in from the left
// for kSlideMs), CONNECTED in the Rytm's bold font, the voices and send FX, the network address (or the link), a line
// running down over kSplashMs, the bottom bar naming how the pages open.
CORE_COLD void splash_draw(Canvas& c, uint32_t t) {
  char b[kTextLen + 1];
  const device::Screen* bx = device::box_screen();
  c.clear();
  header(c, kStatusTitle, 0, 0, "USB");
  int dx = 0;
  if (t < kSlideMs) {   // ease out: fast first, settling
    int r = (int)(kSlideMs - t);
    dx = (kIconX + kIconW) * r * r / (int)(kSlideMs * kSlideMs);
  }
  for (int y = 0; y < kIconH; ++y) {
    uint64_t row = kUsbIcon[y];
    for (int x = 0; x < kIconW; ++x)
      if (row >> x & 1) c.pixel(kIconX - dx + x, kIconY + y);
  }
  c.put_text(kSlotFree, kRBold, 54, 19, "CONNECTED");
  sniprintf(b, sizeof b, CORE_STR("%d VOICES  %d FX"), bx ? device::active().voices : kVoices, fx::kSlots);
  c.put_text(kSlotFree + 1, kRSmall, 54, 33, b);
  const uint8_t* ip = ssrc.st.ip;
  if ((ssrc.st.eth == 2 || ssrc.st.wifi == 2) && (ip[0] | ip[1] | ip[2] | ip[3]))
    sniprintf(b, sizeof b, CORE_STR("%u.%u.%u.%u"), ip[0], ip[1], ip[2], ip[3]);
  else
    strcpy(b, "USB AUDIO+MIDI");
  c.put_text(kSlotFree + 2, kRSmall, 54, 41, b);
  int w = t >= kSplashMs ? 0 : (int)((kSplashMs - t) * 124 / kSplashMs);
  if (w > 0) c.hline(2, 2 + w - 1, 52);
  char f[32];
  sniprintf(f, sizeof f, "%s: SIDECAR PAGES", sc_combo_keys("pages"));   // the device's table (ui/page_keys.h)
  footer(c, bx ? bx->keys : f);
}

// The splash's frames: at once, then every kSlideStepMs while the icon slides, every kSplashStepMs after (only the
// tiles that changed: the line's end, ~1 tile a frame); 36 after the first frame, 36 0 at the end.
CORE_COLD void splash_tick(uint32_t ms) {
  bool first = !s.splash_clock;
  if (first) s.splash_clock = true, s.splash_t0 = ms, s.last_draw = ms - kSplashStepMs;
  uint32_t t = ms - s.splash_t0;
  if (t >= kSplashMs) {
    uint8_t b = 0;
    send_short(kSplash, &b, 1);
    s.splash = false;
    s.sent_valid = false;   // the Rytm's shadow holds the splash; the page sends its whole frame on opening anyway
    // 3A 0 0: the shadow (CONNECTED) stays hidden until our next commit. Not when the SRC view's page follows: its whole
    // frame replaces the shadow, and the Rytm acts on a 3A at its next 50 ms UI tick (sv.base = commits), which would
    // also hide that frame if it came in the same tick
    if (!s.open && !(s.src_track >= 0 && track_page(s.src_track))) remote_request(false);
    return;
  }
  if (!first && ms - s.last_draw < (t < kSlideMs + kSlideStepMs ? kSlideStepMs : kSplashStepMs)) return;
  s.last_draw = ms;
  splash_draw(work, t);
  uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint64_t texts = 0;
  wire_ready();
  int tiles = s.sent_valid ? wire->diff_tiles(sent, mask) : kTiles;
  if (s.sent_valid) wire->diff_text(sent, &texts);
  else
    for (int i = 0; i < kTextSlots; ++i)
      if (wire->txt[i].s[0]) texts |= 1ull << i;
  if (tiles || texts) send_part(mask, texts, !s.sent_valid);
  else send_short(kAlive, nullptr, 0);
  s.stats.tiles += (uint32_t)tiles;
  sent = *wire;
  s.sent_valid = true;
  s.last_tx = ms;
  if (first) {
    uint8_t b = kSplashTicks;
    send_short(kSplash, &b, 1);
    s.stats.splashes++;
  }
}

// F1 / F4: a popup - the stock title bar, the text in a box in the middle; shown as the splash is (36), kPopupMs
constexpr uint32_t kPopupMs = 800;
constexpr uint32_t kSplashTicksMax = 127;     // 36's byte (50 ms ticks, a MIDI data byte)
constexpr uint32_t kPopupWindowMs = 6000;     // a 36 again before the last one's 127 ticks run out
constexpr uint32_t kPopupAliveMs = 500;       // a host message at least this often while it is up (the Rytm: 1 s)
CORE_COLD void popup_draw(Canvas& c) {
  c.clear();
  header(c, kStatusTitle, 0, 0, nullptr);
  c.rect(Rect{4, 16, 120, 32}, kOn);
  c.rect(Rect{6, 18, 116, 28}, kOn);
  c.put_text(kSlotFree, kRSmall, 64, s.popup_l2[0] ? 23 : 29, s.popup_l1, kAlignCentre);
  if (s.popup_l2[0]) c.put_text(kSlotFree + 1, kRSmall, 64, 34, s.popup_l2, kAlignCentre);
}
// The pairing code's modal (pi/engine/paircode.h): a small box in the middle of the 128x64 panel, over whatever is
// showing. It is the one thing a user needs in order to use the device at all, so unlike a status popup it does not
// defer to a Sidecar page being open.
//
// It OVERLAYS rather than replaces: the page is drawn into `work` as usual and this goes on top, so the diff sends
// only the box's tiles. Coming back is the mechanism the remote UI already has for a page change - invalidate, and
// the page redraws itself from the model - so nothing of the page's state is kept in pixels and none can be lost.
// The Rytm draws each text item itself, in a box it clears, so any of the page's items under the modal have to be
// blanked while it is up; the page's own redraw puts them back.
CORE_COLD void modal_draw(Canvas& c) {
  const Rect box{kModalX, kModalY, kModalW, kModalH};
  for (int i = 0; i < kTextSlots; ++i) {     // the page's text under the box: blanked while it is up
    if (!c.txt[i].s[0] || i == kSlotModal || i == kSlotModal + 1) continue;
    int y = c.txt[i].y;
    if (y + 6 <= kModalY || y >= kModalY + kModalH) continue;
    int w = (int)strlen(c.txt[i].s) * 5, x = text_left(c.txt[i], w);
    if (x + w <= kModalX || x >= kModalX + kModalW) continue;
    c.put_text(i, kRSmall, 0, 0, nullptr);
  }
  c.fill(Rect{kModalX + 1, kModalY + 1, kModalW - 2, kModalH - 2}, kOff);   // the page does not show through it
  c.rect(box, kOn);
  c.rect(Rect{kModalX + 2, kModalY + 2, kModalW - 4, kModalH - 4}, kOn);
  c.put_text(kSlotModal, kRSmall, 64, s.popup_l2[0] ? 23 : 29, s.popup_l1, kAlignCentre);
  c.put_text(kSlotModal + 1, kRSmall, 64, 34, s.popup_l2[0] ? s.popup_l2 : nullptr, kAlignCentre);
}
// Called from frame(), which only runs while a page is open. True: it is on screen this frame.
CORE_COLD bool modal_tick(uint32_t ms) {
  if (!s.popup || !s.popup_over) return false;
  if (!s.popup_clock) s.popup_clock = true, s.popup_t0 = ms, ++s.popups;
  if (ms - s.popup_t0 < (s.popup_ms ? s.popup_ms : kPopupMs)) return true;
  s.popup = false, s.popup_clock = false;
  return false;
}

// 36 for `left` ms more (+ 200 ms: the Rytm ends it by itself if our 36 0 is lost), at most the 127 ticks a MIDI data
// byte carries (send_short masks to 7 bits: the old 12 s asked for 244 ticks and sent 116)
CORE_COLD void popup_hold(uint32_t ms, uint32_t left) {
  uint32_t ticks = left / 50 + 4;
  uint8_t b = (uint8_t)(ticks < kSplashTicksMax ? ticks : kSplashTicksMax);
  send_short(kSplash, &b, 1);
  s.popup_36 = ms, s.last_tx = ms;
}
// A key or a turn with the page closed while the pairing code's popup is up: the Rytm's splash ended on it (its own
// rule), so the popup is over here too - no 36 again to bring it back over a user who has read it and moved on. The key
// itself is the Rytm's and goes on as usual.
CORE_COLD void popup_key_closed() {
  if (s.open || !s.popup || !s.popup_over || !s.popup_clock) return;
  s.popup = false, s.popup_clock = false;
  s.sent_valid = false;   // (the Rytm's shadow holds the popup's frame)
}
CORE_COLD void popup_tick(uint32_t ms) {
  const uint32_t hold = s.popup_ms ? s.popup_ms : kPopupMs;
  if (!s.popup_clock) {
    s.popup_clock = true, s.popup_t0 = ms;
    popup_draw(work);
    uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint64_t texts = 0;
    wire_ready();
    for (int i = 0; i < kTextSlots; ++i)
      if (wire->txt[i].s[0]) texts |= 1ull << i;
    send_part(mask, texts, true);
    sent = *wire;
    s.sent_valid = true;
    popup_hold(ms, hold);
    ++s.popups;
    if (!s.popup_over) ++s.status_popups;
    return;
  }
  if (ms - s.popup_t0 < hold) {
    // The Rytm ends a splash after 1 s without a host message, and its 36 holds 127 ticks (6.35 s) at most: a popup
    // longer than a second (the pairing code's 30 s) went after about one, which is how the code was never readable
    // on the Rytm (device 2026-10-29). So: a 3C every half second, and the 36 again for what is left once its window
    // nears its end. A key or a turn the Rytm reports while it is up ends it here too (popup_key_closed).
    if (ms - s.popup_36 >= kPopupWindowMs) popup_hold(ms, hold - (ms - s.popup_t0));
    else if (ms - s.last_tx >= kPopupAliveMs) send_short(kAlive, nullptr, 0), s.last_tx = ms;
    return;
  }
  uint8_t b = 0;
  send_short(kSplash, &b, 1);
  s.popup = false;
  s.sent_valid = false;   // (the Rytm's shadow holds the popup; the page sends its whole frame on opening anyway)
}

// rui1038 / rui1046 (rytm-mods 0026: 49 1 <code>): what each code is comes from the device's table (ui/page_keys.h).
// A page with no title on the Rytm's screen (its `rytm` 0) is the platform's (pi/engine/voice_pi.cpp); a page with one
// (6 SONG, 7 REC, 9 SCOPE, 10 SIDECAR the status, 11 SAMPLE - again while it shows: AUTO, the auto-sampler -, 12
// PROJECT, 13 POOL) opens there as remote_show_page does (at once when the page is open, else asked to open),
// unless the platform's touchscreen has that page (remote_display_owns: the Pi with a display).
volatile uint16_t disp_owned = 0;   // bit p: the platform's touchscreen shows page p (remote_display_owns)
// 0.5.0 FILL (rytm-mods 0026, fill-keys): a Rytm whose hello's caps2 has kCaps2Arrows forwards UP + LEFT / RIGHT (on the
// stock screen: the page closed, no trig held, no menu, no list) as 49 1 1 kPageGenerator / kPageFill - once the host
// has claimed them with 4A 7E 1, after every hello (the Rytm clears the claim at each hello and when its USB port goes
// down); without the claim they stay stock's. An older Rytm counts a 4A 7E as a bad message: only with the bit.
static bool g_fill_keys_off = false, g_fill_claimed = false;
static bool g_nohello_told = false;   // the log line of the no-hello claim, once per link
static uint32_t g_claim_ms = 0;   // the last claim's tick time
static volatile bool g_going_down = false;
CORE_COLD void fill_claim() {   // (L7: only a build with the FILL page claims the Rytm's UP + RIGHT)
#if CORE_POLY
  // (no hello seen yet is unknown, not an older mod: the Rytm says hello only at its USB port-up, so a host that started
  // after it never hears one and would never claim; a 0.4.x mod counts the 4A 7E as one bad message. A box (A4 /
  // Digitakt) never gets the guess: only its own hello's caps2 claims)
  if ((s.stats.hellos > 0 || device::foreign()) && !(s.caps2 & kCaps2Arrows)) {   // an older mod: the Rytm's claim is gone at the hello, and 4A 7E 0 would be a bad message
    g_fill_claimed = false;
    return;
  }
  if (g_fill_keys_off) return;
  const uint8_t m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0x7E, 1, 0xF7};
  to_rytm(m, 9);
  g_fill_claimed = true, g_claim_ms = s.now_ms;
#endif
}
// 0.5.0 v2 P2: the scene edit on the Rytm's own pages armed after every hello (the Rytm clears it at each); an older
// mod (no kCaps2Capture) counts 5A 01 0B as a refused command: never sent to it
CORE_COLD void edit_arm() {
#if CORE_POLY
  if (s.caps2 & kCaps2Capture) mod::rytm_edit_arm(true);
#endif
}
// 2026-11-24 TBLB (rytm-mods 0026): "the host draws the bank list of P13" (4A 7D 1), after every hello, only to a Rytm whose hello's
// caps2 has kCaps2ListP13: it opens a P13 list (4A with the param byte) only after this, so a host that cannot draw it never gets one
// (an older host would read the frame as P1's); an older mod counts the 4A 7D as a bad message and never asks for one
CORE_COLD void list_p13_announce() {
#if CORE_MSAMP
  if (!(s.caps2 & kCaps2ListP13)) return;
  const uint8_t m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0x7D, 1, 0xF7};
  to_rytm(m, 9);
#endif
}
CORE_COLD void fill_unclaim() {
  if (!g_fill_claimed) return;
  g_fill_claimed = false;
  const uint8_t m[9] = {0xF0, 0x7D, 0x52, 0x54, 0x4A, 1, 0x7E, 0, 0xF7};
  to_rytm(m, 9);
}
CORE_COLD bool extra_here(const char* title) {   // an ExtraPage of that title is in the list now (or none is: the status)
  for (int k = 0; k < extra_pages(); ++k)
    if (!strcmp(extra_page(k).title, title)) return extra_for_box(title) && (!extra_page(k).shown || extra_page(k).shown());
  return true;
}
CORE_COLD void display_remote_page(int page) {
  if (page >= 0 && page < 16 && (disp_owned >> page & 1)) return;   // the touchscreen's page, not the Rytm's screen
  const sc_page_key_t* p = sc_page_of_code(page);
  const char* t = p && p->rytm ? (sc_streq(p->rytm, "SIDECAR") ? kStatusTitle : p->rytm) : nullptr;
  if (!t) return;
  // rui1046: a page that is not in the list now (PROJECT without the bundle feed, POOL without the recorder's pools):
  // the Rytm says so (a popup on the stock screen) instead of opening on another page; SONG is asked to show (its ARM)
  if (!strcmp(t, "SONG") && !extra_here(t)) {
    char o[8];
    song::command("song=show", o, sizeof o);
  }
  if (!extra_here(t)) {
    char m[kTextLen + 1];
    sniprintf(m, sizeof m, CORE_STR("%s: NOT HERE NOW"), p->name);
    remote_popup(m);
    return;
  }
  if (sc_streq(t, "SAMPLE")) {   // rui1052: SAMPLE by name, not by its key (the table moved it to T11)
    const char* now = remote_page_title();
    if (now && !strcmp(now, "SAMPLE")) t = "AUTO";
  }
  remote_show_page(t);
}

// rc5 (0.4.1, the owner on hardware: a DSP SAMPLE voice silent, its TUN / STA / END left by the machine before): a
// machine chosen in the Rytm's machine list gets its SRC knobs' (P1..P8) defaults from stock's set_machine, which leaves
// page 2 (P9..P16, the stock sample words 17..24) as the sound had them (rytm-mods 0026 design "1034", "1051"; measured
// in rytm-emu gate040 and o174: LIVE IN -> DSP SAMPLE kept LIVE IN's 77 0 0 0 0 0 0 0, i.e. TUN +13, END 0, LEV 0).
// 0026 writes page 2's defaults itself for LIVE IN, CHOP and MIDI, 0021 for MD and the user slots; for the machines
// below the Pi does it, as 0026's cu_defs_tick does: once the list has closed (2B: MachineListView no longer on top),
// for a track a one-track 33 moved while it was open, and kSettleMs later still on another machine than at the open,
// P9..P16 written as knob turns (48), the records' defaults. Measured order (both OSes): the previews' 33s, NO's
// restoring 33 and YES's last 33 all come before the 2B of the close; no snapshot 33, no other view over the list. The
// wait and the re-check cover a later one. 0026 queues 8 48s and drops the rest (rui.c WQ_N): page 2 goes as two groups
// of 4 a UI tick apart, and params not answered within kAnswerMs go once more (logged). A kit, sound or project load is
// no list choice: its words stay. Values: DSP SAMPLE's records are copies of stock's sample records, which PLAITS's and
// DAISYSP's page 2 rely on (0021 ext.c); MULTISAMPLE / WAVETABLE / GRANULAR / 6-OP FM's E..H: 0026's cu_p2k,
// cu_wtp2k, cu_grp2k, CU_CHORD.
constexpr uint32_t kMachineListVt173 = 0x401E5BF4u;   // MachineListView's vtable, OS 1.73 (0026's cu_prep)
constexpr uint32_t kMachineListVt174 = 0x401E5EA4u;   // OS 1.74 (notes os174 address map; 0026 sends sv.top_vt raw)
constexpr uint32_t kSettleMs = 150, kGroupMs = 60, kAnswerMs = 1000;   // >= 2 UI ticks (50 ms); one tick; the answers
struct PageTwo {
  uint8_t machine, mask, v[8];
};
UI_ROM const PageTwo kPageTwo[] = {
    {kDspSample, 0xFF, {64, 64, 0, 0, 0, 120, 0, 100}},   // TUN FIN SMP MOD STA END LOP LEV
    {kPlaits, 0xFF, {64, 64, 0, 0, 0, 120, 0, 100}},
    {kDaisy, 0xFF, {64, 64, 0, 0, 0, 120, 0, 100}},
    {kMultiMachine, 0xFF, {64, 64, 0, 0, 0, 0, 0, 0}},     // TUNE FINE STRT LOOP / CHD INV SPR VOI
    {kWaveMachine, 0xFF, {0, 64, 100, 64, 0, 0, 0, 0}},    // ATK DEC SUS ENV / TBLB POSB MIX FTYP
    {kGranMachine, 0xFF, {10, 60, 0, 0, 0, 0, 0, 0}},      // ATK REL HOLD ANIM / CHD INV SPR VOI
    {kFm, 0xF0, {0, 0, 0, 0, 0, 0, 0, 0}},                 // E..H: CHD INV SPR VOI (A..D are FM's own)
};
CORE_COLD const PageTwo* page_two(int m) {
  for (const PageTwo& p : kPageTwo)
    if (p.machine == m) return &p;
  return nullptr;
}
CORE_COLD void page_two_cancel() { s.p2_due = 0, s.mlist_moved = 0; }
CORE_COLD void machine_list(bool on) {
  if (on && !s.mlist) {
    memcpy(s.mlist_vb, track_vb, sizeof s.mlist_vb), s.mlist_moved = 0;
    // re-opened within a choice's settle wait: that choice goes on in this list (its machine at the first open, so a NO
    // here still gets the first choice's defaults), nothing written for a machine this list's cursor only previews
    for (int t = 0; t < 12; ++t)
      if ((s.p2_due >> t & 1) && s.p2_stage[t] == 0)
        s.p2_due &= (uint16_t)~(1u << t), s.mlist_vb[t] = s.p2_from[t], s.mlist_moved |= (uint16_t)(1u << t);
  }
  if (!on && s.mlist && have_tracks)
    for (int t = 0; t < 12; ++t)
      if (s.mlist_moved >> t & 1)
        s.p2_due |= (uint16_t)(1u << t), s.p2_from[t] = s.mlist_vb[t], s.p2_stage[t] = 0, s.p2_ms[t] = s.now_ms;
  if (!on) s.mlist_moved = 0;
  s.mlist = on;
}
// one group (params 8 + first .. 8 + first + 3) of what is still to send, if the track still plays that machine
CORE_COLD void page_two_group(int t, int first) {
  const PageTwo* p = page_two(machine_of(s.p2_vb[t]));
  for (int k = first; k < first + 4 && p; ++k)
    if ((s.p2_want[t] >> k & 1) && remote_track_write(t, 8 + k, p->v[k], 0, 0)) s.p2_ans[t] |= (uint8_t)(1u << k);
  s.p2_want[t] &= (uint8_t)~(0x0Fu << first);
}
CORE_COLD void page_two_tick(uint32_t ms) {
  for (int t = 0; t < 12 && s.p2_due; ++t) {
    if (!(s.p2_due >> t & 1)) continue;
    uint16_t bit = (uint16_t)(1u << t);
    uint32_t dt = ms - s.p2_ms[t];
    if (s.p2_stage[t] > 0 && track_vb[t] != s.p2_vb[t]) {   // the track moved on meanwhile: nothing more for it
      s.p2_due &= (uint16_t)~bit;
      continue;
    }
    if (s.p2_stage[t] == 0) {
      if (dt < kSettleMs || s.mlist) continue;   // (the list open again: machine_list took the choice back)
      uint8_t vb = track_vb[t], from = s.p2_from[t];
      const PageTwo* p = vb == kNotOurs ? nullptr : page_two(machine_of(vb));
      // NO, or back to the machine it had (its words are its own), or a machine whose page 2 the Rytm sets / has none
      if (!p || (from != kNotOurs && machine_of(from) == machine_of(vb))) {
        s.p2_due &= (uint16_t)~bit;
        continue;
      }
      s.p2_vb[t] = vb, s.p2_want[t] = p->mask, s.p2_ans[t] = 0, s.p2_tries[t] = 0;
      core::log("rui: track %s chose %s in the machine list: P9..P16 its defaults", kTrackName[t], machine_long(machine_of(vb)));
      page_two_group(t, 0);
      s.p2_stage[t] = 1, s.p2_ms[t] = ms;
    } else if (s.p2_stage[t] == 1) {
      if (dt < kGroupMs) continue;
      page_two_group(t, 4);
      s.p2_stage[t] = 2, s.p2_ms[t] = ms;
    } else if (!s.p2_ans[t]) {
      s.p2_due &= (uint16_t)~bit;   // all answered
    } else if (dt >= kAnswerMs) {
      core::log("rui: track %s page 2: no answer for P9..P16 mask %02x (the Rytm's 48 queue full?)%s", kTrackName[t],
                s.p2_ans[t], s.p2_tries[t] ? ": given up" : ": sent again");
      if (s.p2_tries[t]++) {
        s.p2_due &= (uint16_t)~bit;
        continue;
      }
      s.p2_want[t] = s.p2_ans[t], s.p2_ans[t] = 0;
      page_two_group(t, 0);
      s.p2_stage[t] = 1, s.p2_ms[t] = ms;
    }
  }
}

}  // namespace

// FILL's claim (remote.h)
CORE_COLD void remote_fill_keys(bool on) {   // (on: an explicit claim, a going down's too)
  if (on) g_going_down = false;
  g_fill_keys_off = !on;
  if (on) fill_claim();
  else fill_unclaim();
}
bool remote_fill_keys_on() { return !g_fill_keys_off; }
void remote_going_down() { g_going_down = true; }

CORE_COLD bool remote_is_list(int vi, int k) {
  if (vi < 0 || vi >= kVoices || k < 0 || k > 15) return false;
  const Page* p = page_for_voice(named_machine(machine_of_voice(vi), vi), vi);
  return p && is_list(*p, k);
}

CORE_COLD void remote_sysex(const uint8_t* d, int len) {
  if (len < 7 || d[5] != 1 || d[len - 1] != 0xF7) { s.stats.bad++; return; }
  const uint8_t* b = d + 6;
  int n = len - 7;
  if (d[4] == kSrcView || d[4] == kSrcVals || d[4] == kSrcDiag) src_capable = true;
  if (s.stats.hellos == 0 && d[4] != kHello && !g_going_down && !device::foreign()) s.rh_traffic = true;   // (re-hello below)
#if CORE_POLY
  // 2026-11-23: Rytm traffic and no hello (the host started after the Rytm's port-up): claim UP + LEFT / RIGHT once; the
  // 2 s claim (remote_tick) goes on from there, and the first hello, if one ever comes, decides as before. Never for a box.
  if (s.stats.hellos == 0 && !g_fill_claimed && !g_fill_keys_off && !g_going_down && d[4] != kHello && !device::foreign()) {
    fill_claim();
    if (g_fill_claimed && !g_nohello_told) {
      g_nohello_told = true;   // (once per link: a hello clears it)
      core::log("rui: Rytm traffic before any hello: claimed UP+RIGHT, caps unknown");
    }
  }
#endif
#if CORE_POLY && CORE_MSAMP
  // an UP combo the mod ate (UP + T1..T14 49 1 n, UP + YES / LFO / FX / AMP / TRIG 30, UP + SRC 29 / the bank list, the map
  // 49 1 5): UP's release that follows is no tap (CHOP's ALL toggle)
  if (d[4] == kMode || d[4] == kSrcView || d[4] == kBankList || (d[4] == kDisplay && len > 6 && (d[6] == 1 || d[6] == 4 || d[6] == 5)))
    chop_page_up_cancel();   // (49 2 / 49 3: the track's info and the knob echoes are state, no combo)
#endif
  switch (d[4]) {
    case kMode:
      if (n < 3) break;
      if (b[1]) {
        bool was_open = s.open;
        int was = s.voice;
        // ux1027: UP + LFO / UP + FX again on the page it put up: out. UP + AMP again keeps re-aiming the OUT view at the
        // Rytm's selected track (rui1047), never leaves (review finding 4)
        if (was_open && b[0] == s.via && was == s.via_voice && extra_of(was) && (b[0] == kPageLfo || b[0] == kPageIns)) {
          s.via = -1;
          s.input = true, s.mask_valid = false;
          if (!go_back()) go(0);   // out, as NO (nav044: the page asked closed), else SIDECAR
          return;
        }
        if (!s.open) s.stats.opens++;
        s.open_ms = s.now_ms;
        if (s.src_track >= 0) s.src_told = -1;   // 1027: the page comes first; after it the view's whole frame again
        s.open = true;
        s.voice = -1;   // the SIDECAR page first; 1026: page 1 = the LFO page (TRK + LFO, also again while open)
        if (b[0] == kPageLfo || b[0] == kPageIns || b[0] == kPageInsOut)
          for (int k = 0; k < extra_pages(); ++k)
            if (!strcmp(extra_page(k).title, b[0] == kPageLfo ? "LFO" : "INSERT")) s.voice = kExtra0 - k;
#if CORE_POLY
        if (b[0] == kPageInsOut) ins_open_out();   // rui1047: the OUT view of the selected track (UP + AMP; again while open: back to it)
        if (b[0] >= kPageSeq && b[0] < kPageSeq + 12) {   // seq-ux: TRK + TRIG (rui1036: FUNC + LFO), the SEQ page of track b[0] - 0x10
          seq::view_request(b[0] - kPageSeq);
          for (int k = 0; k < extra_pages(); ++k)
            if (!strcmp(extra_page(k).title, "SEQ")) s.voice = kExtra0 - k;
        }
        if (b[0] == kPageArp)
          for (int k = 0; k < extra_pages(); ++k)
            if (!strcmp(extra_page(k).title, "ARP")) s.voice = kExtra0 - k;
        if (b[0] < kPagePoly || b[0] >= kPagePoly + 12) poly_page_closed();   // rui1048: another page asked: POLY's state gone
        if (b[0] >= kPagePoly && b[0] < kPagePoly + 12) {   // rui1048: the track menu's POLY CONFIG of track b[0] - 0x20
          poly_page_open(b[0] - kPagePoly, true);
          for (int k = 0; k < extra_pages(); ++k)
            if (!strcmp(extra_page(k).title, "POLY")) s.voice = kExtra0 - k;
        }
#endif
        if (want_title) {   // the host asked for this page (remote_show_page: the sequencer's SEQ page)
          for (int k = 0; k < extra_pages(); ++k)
            if (!strcmp(extra_page(k).title, want_title)) s.voice = kExtra0 - k;
          want_title = nullptr;
        }
        if (!was_open || s.voice == -1) s.shortcut = false;   // (SIDECAR: NO leaves, as before)
        if (was_open) lfo_left(was, s.voice);
        s.via = b[0], s.via_voice = s.voice;
        if (s.voice != -1 && !(was_open && was == s.voice))
          s.shortcut = true;   // rui1051, nav044: a shortcut's page: NO leaves
        s.page_changed = true;
        s.sent_valid = false;   // the Rytm's shadow: unknown, the whole frame goes
        s.last_draw = s.last_tx = 0;
        s.clock_set = false;
        s.echo_valid = false;
        s.mask_valid = false;   // the Rytm forgets the mask when the page left by its timeout: always again
        s.touched = -1;
        s.splash = false;       // the user opened the page: it replaces the splash (the Rytm ended it)
        s.popup = false, s.popup_clock = false;   // (and a popup)
        s.leds_on = false;      // the Rytm shows its own LEDs until a page sends 34
      } else {
        if (s.open) s.stats.closes++;
        if (s.open) lfo_left(s.voice, -1);   // ux1027 (review 5): the LFO page opens on its main view again
        s.open = false;
        s.shortcut = false, s.via = -1, s.sedit_back = kNoPage;
        keys_map_drop(), s.map_over = false;   // (UP's map goes with the page)
        release_held();                        // 0.5.0: and every key a page holds
        s.up_down = false;
        s.page_down = s.page_owed = false;     // (v2 review M1: a PAGE held across the close sends no release here)
#if CORE_POLY
        seq::page_closed();   // seq-ux: a prompt, TRK + TRIG's view end with the page
        poly_page_closed();   // rui1048
        core::poly::rytm_trk(false);   // 1042: a TRK held over the close sends no release (the page's own)
#endif
        s.clock_set = false;   // 1027: an SRC view still on starts again (a whole frame)
        s.leds_on = false;   // the Rytm restores its own LEDs on leaving (a 34 counts only while the page is open)
        tap_enable(false);
      }
      return;
    case kKey:
      if (n < 3) break;
      s.stats.keys++;
      s.user = true;
      s.rep_code = -1;   // 0.5.0: any key event stops a repeat (a modifier let go too); a page that takes the press starts it again
      if (b[2]) popup_key_closed();
      // the pairing code's modal over an open page: a key takes it off (the user has read it) and does nothing else;
      // its release is eaten too, so the page does not see half a press. (With the page closed no key reaches us: the
      // Rytm's splash ends on a key by itself, popup_tick.)
      if (b[2] && s.open && s.popup && s.popup_over && s.modal_shown) {
        s.modal_key = b[0] << 7 | b[1];
        remote_popup_end();
        s.input = true;
        return;
      }
      if (!b[2] && s.modal_key == (b[0] << 7 | b[1])) {
        s.modal_key = -1;
        return;
      }
#if CORE_POLY
      if ((b[0] << 7 | b[1]) == 0x86) core::poly::rytm_trk(b[2] != 0);   // 1042: TRK + pad is a selection (core/poly.h)
#endif
      if (s.open && (b[0] << 7 | b[1]) == kKeyUp) s.up_down = b[2] != 0;
      if (s.open && b[2] && (b[0] << 7 | b[1]) == kKeyRight && up_right_fill()) {   // 0.5.0: UP + RIGHT, the page open
        s.input = true;
        return;
      }
      if (!s.open && src_bank_key(b[0] << 7 | b[1], b[2])) {   // rui1038: the bank list's LEFT RIGHT UP DOWN (0.5.0: 2 = with FUNC)
        s.input = true;
        return;
      }
      if (!s.open && s.src_track >= 0 && b[0] == 0 && b[1] >= 0x70 && b[1] <= 0x77) {   // rui1036: the SRC view's pushes
        // 0.5.2: a knob press under PAGE + T15 / T16 is the scene edit's (2D 01 14), not the view's: no slice editor, no list,
        // no row action; the release of a press it took goes the same way, a side let go in between or not
        const uint8_t pbit = (uint8_t)(1u << (b[1] - 0x70));
        if (b[2] != 0 && scene::edit_side() >= 0) s.push_eaten |= pbit;
        if (s.push_eaten & pbit) {
          if (b[2] == 0) s.push_eaten &= (uint8_t)~pbit;
          s.input = true;
          return;
        }
#if CORE_POLY && CORE_MSAMP
        if (chop_src_push(s.src_track, b[1], b[2] != 0)) {   // CHOP: a push of B..H opens the slice editor
          s.input = true;
          return;
        }
        if (sedit_src_push(s.src_track, b[1], b[2] != 0)) {   // DSP SAMPLE: a push of B opens SAMPLE EDIT
          s.input = true;
          return;
        }
#endif
        if (b[1] == kKeyPushA) src_list_push(b[2] != 0);   // P1's: held - the list stays open; a click - the row acts
        s.input = true;
        return;
      }
      // v2: PAGE held, whoever takes it (seq mode's sequencer too): PAGE + T15 / T16 start EDIT on a page that locks
      if (s.open && (b[0] << 7 | b[1]) == kKeyPage) s.page_down = b[2] != 0;
#if CORE_POLY
      {   // the sequencer's trig keys and PAGE: in seq mode (the page open or not) and on its SEQ page
        const ExtraPage* x = s.open ? extra_of(s.voice) : nullptr;
        int code = b[0] << 7 | b[1];
        // 0.5.0: a trig key another engine page takes now (MORPH's scenes, the LFO design view's steps), or the release
        // of one it took, is that page's, not the sequencer's (seq mode sends the trig keys on a seq track)
        // review: a release of a key a page holds is that page's whatever page shows now (SEQ too)
        bool page_trig = code < 16 && s.open &&
                         ((b[2] && !(x && !strcmp(x->title, "SEQ")) && (page_trigs(x) >> code & 1)) ||
                          (!b[2] && (key_held(code) || (s.edit_keys >> code & 1))));
        if (!page_trig && seq::ui_key(code, b[2] != 0, x && !strcmp(x->title, "SEQ"))) {
          s.input = true;
          return;
        }
      }
#endif
      if (s.open) on_key(b[0] << 7 | b[1], b[2] != 0);
      return;
    case kEnc:
      if (n < 3) break;
      s.stats.encs++;
      s.user = true;
      popup_key_closed();   // (a turn ends the Rytm's splash as a key does)
      if (!s.open && src_bank_enc(b[0], (int)b[1] - 64, b[2] != 0)) {   // rui1038: knob A in the bank list
        s.input = true;
        return;
      }
#if CORE_POLY
      {
        const ExtraPage* x = s.open ? extra_of(s.voice) : nullptr;
        // rui1048: POLY CONFIG's knobs are its own, also in seq mode (seq::ui_enc takes every turn then)
        // review (0.5.0): MORPH's knobs, and a turn under EDIT's held scene key, are the page's too
        bool own = (x && (!strcmp(x->title, "POLY") || !strcmp(x->title, "MORPH") || !strcmp(x->title, "LOOP"))) || (scene::edit() && edit_page(x));
        if (!own && seq::ui_enc(b[0], (int)b[1] - 64, b[2] != 0, x && !strcmp(x->title, "SEQ"))) {
          s.input = true;
          return;
        }
      }
#endif
      if (s.open) on_enc(b[0], (int)b[1] - 64, b[2] != 0);
      return;
    case kMachines: {   // 1025: <t0> <n> {<machine 0..52> <vb | 7F>} x n
      if (n < 2 || b[0] >= 12 || b[1] < 1 || b[0] + b[1] > 12 || n != 2 + 2 * b[1]) break;
      if (!have_tracks) memset(track_vb, kNotOurs, sizeof track_vb), memset(track_smpl, 0, sizeof track_smpl), memset(track_rm, 0x7F, sizeof track_rm);
      // One track turned JUNO / 6-OP FM (the SRC list, n 1): its PAT list (16 / 17) pushed now. The Rytm keeps one
      // list cache for both (and the U slots' lists); the 17 that follows every 16 at a project load evicts JUNO's,
      // and the Rytm asks again only once in 200 draws [measured, rytm-emu rui1025: ~38 s of factory names].
      if (b[1] == 1 && track_vb[b[0]] != track_vb_of(b[0], b[3], b[2])) {
        if ((b[3] & 0x78) == kJuno) send_juno_names();
        else if ((b[3] & 0x78) == kFm) send_fm_names();
      }
      if (b[1] != 1) page_two_cancel();                          // rc5: a snapshot (a load) is no list choice
      else if (s.mlist) s.mlist_moved |= (uint16_t)(1u << b[0]);
      for (int i = 0; i < b[1]; ++i) {
        const int t = b[0] + i;
        const uint8_t vb = track_vb_of(t, b[3 + 2 * i], b[2 + 2 * i]);
        if (vb != track_vb[t] || b[2 + 2 * i] != track_rm[t]) s.src14[t] = -1;   // 0.5.0: another machine's knob is no SMP / TBL
        track_vb[t] = vb, tracks_known |= (uint16_t)(1u << t), track_smpl[t] = b[2 + 2 * i] == kRytmSampler, track_rm[t] = b[2 + 2 * i];
      }
      have_tracks = true;
      for (int vi = 0; vi < kVoices; ++vi) {   // polyuser-4 F-b1: a voice with no report yet takes its tracks' machine
        int m = -1;
        for (int t = 0; t < 12; ++t)
          if (kTrackVoice[t] == vi && track_vb[t] != kNotOurs && !(track_vb[t] & kMidiMachine))
            m = m < 0 || m == machine_of(track_vb[t]) ? machine_of(track_vb[t]) : -2;   // (-2: the pair disagrees)
        if (m >= 0) voice_machine_hint(vi, (uint8_t)(m | vi));
      }
#if CORE_MSAMP
      for (int vi = 0; vi < kVoices; ++vi) {
        bool any = false, other = false;
        for (int t = 0; t < 12; ++t)
          if (kTrackVoice[t] == vi && track_vb[t] != kNotOurs) (track_vb[t] & kMidiMachine ? any : other) = true;
        const V& x = v[vi];
        if (x.mi && !any) v[vi].mi = false, midim::leave(vi);   // no track on the voice plays it any more (ext_render's 0x11 stopped with it)
        // The machine known before the voice's first report (a Pi restart, BD on MIDI: a note from a key or the sequencer would play PLAITS). Only a
        // voice with no other machine: a change from one is the next report's, which ends what that machine plays
        else if (!x.mi && any && !other && x.user < 0 && !nm::is(vi) && !x.li && !x.fm && !x.juno && !x.md && !x.daisy) v[vi].mi = true;
      }
#endif
#if CORE_POLY
      arp::machines();   // a voice's machine changed: its arpeggio ends
#endif
      s.stats.machines++;
      return;
    }
    case kBankList: {   // rui1038: <track> <on> <hi> <lo> <why>
      if (n < 5 || b[0] >= 12 || (b[1] > 1 && b[1] != 3)) break;   // rui1040: on 3 FUNC + YES (the files for the row)
      int t = b[0], param = 0;
      if (n >= 6 && b[1] == 0) {   // a close frame closes whatever its param byte says (the list's own state is reset)
        if (b[5] == 12) param = 12;
      } else if (n >= 6) {   // 2026-11-24 TBLB: the optional 6th byte, the list's parameter: 12 (P13, WAVETABLE's table B) is the only value
        if (b[5] != 12 || (b[1] == 1 && track_named(t) != kWaveMachine)) break;
        param = 12;
      }
      src_bank_list(t, kTrackVoice[t], track_named(t), srclib::code_of14(b[2] << 7 | b[3]), b[1], b[4], param);
      s.input = true;
      return;
    }
    case kSrcView:   // 1027: <track> <on>
      if (n < 2 || b[0] >= 12 || b[1] > 1) break;
      s.stats.src_views++;
      src_unhold(-1);
      if (src_log) core::log("rui src: 29 %lu ms, track %d, %s", (unsigned long)now_ms(), b[0], b[1] ? "on" : "off");
      if (b[1]) {
        s.src_track = (int8_t)b[0];
        src_list_quiet();
        s.src_told = -1;   // a whole frame at the next tick: the Rytm shows ours only after a 39 since this 29
        s.src_last = -1;   // a splash running (a hello just before) ends first, then the view's whole frame
      } else if (b[0] == s.src_track) {
        s.src_track = -1, s.src_told = -1, s.covered = false;
        if (!s.open) s.sent_valid = false;   // the Rytm's own screen is back: its shadow is not ours
      }
      return;
    case kSrcDiag: {   // 1027d: logged when the state or the top view changes (or with srclog on)
      if (n < 8) break;
      static uint32_t last = 0xFFFFFFFFu;
      const bool st_cov = b[0] & 2;
      uint32_t vt = (uint32_t)b[2] << 28 | (uint32_t)b[3] << 21 | (uint32_t)b[4] << 14 | (uint32_t)b[5] << 7 | b[6];
      if (st_cov) s.covered = true, s.cover_ms = s.now_ms;   // (the tick's clock, as src_covered's)
      else if (s.covered) {
        if (s.open) s.covered = false;   // a Sidecar page is up: nothing of the SRC view to redraw
        else cover_ended();
      }
      machine_list(vt == kMachineListVt173 || vt == kMachineListVt174);   // rc5: the list's choice gets page 2's defaults once it closes
      uint32_t key = (uint32_t)b[0] << 24 ^ vt ^ (uint32_t)b[1] << 16;
      if (key == last && !src_log) return;
      last = key;
      int st = b[0];
      core::log("rui: 2B %lu ms: SRC view %s%s%s%s%s%s%s, views %d, top %08lx (%d ms)", (unsigned long)now_ms(),
                st & 1 ? "on" : "off", st & 2 ? " covered" : "", st & 4 ? " committed" : "", st & 8 ? " host-idle" : "",
                st & 16 ? " page-open" : "", st & 32 ? " splash" : "", st & 64 ? " SHOWN" : " not-shown", b[1],
                (unsigned long)vt, b[7] * 50);
      return;
    }
    case kSrcVals: {   // 1027: <track> <first> <n> {<int> <frac>} x n
      // 1027e (rytm-mods 0026): <track | 0x40> while a step is held in GRID REC: that step's values (its locks, else the
      // sound's), drawn only - not the sound's for the song, the bank list or a MIDI machine; the sound's follow at release
      const bool held = b[0] & 0x40;
      if (n < 5 || (b[0] & 0x3F) >= 12 || b[1] > 15 || b[2] < 1 || b[1] + b[2] > 16 || n != 3 + 2 * b[2]) break;
      int t = b[0] & 0x3F;
      if (held && !(s.keep_mask >> t & 1)) {   // the sound's values kept until the step is let go
        memcpy(s.keep_v[t], s.srcv[t], 16), memcpy(s.keep_l[t], s.srcl[t], 16);
        s.keep_known[t] = s.src_known[t], s.keep_mask |= (uint16_t)(1u << t);
      } else if (!held) {
        s.keep_mask &= (uint16_t)~(1u << t);   // the sound's own values come with it
      }
      const bool has13 = !held && b[1] <= 12 && 12 < b[1] + b[2];   // 2026-11-24 TBLB: this frame carries P13
      const int old13 = has13 && (s.src_known[t] >> 12 & 1) ? s.srcv[t][12] << 7 | s.srcl[t][12] : -1;
      for (int i = 0; i < b[2]; ++i) {
        s.srcv[t][b[1] + i] = b[3 + 2 * i] & 0x7F;
        s.srcl[t][b[1] + i] = b[4 + 2 * i] & 0x7F;
        s.src_known[t] |= (uint16_t)(1u << (b[1] + i));
      }
      bool p1_changed = false;
      if (!held) {   // rui1038: the banked knob's 14 bits (song): P1, or DSP SAMPLE's SMP P11 (rui1045)
        int kb = remote_track_src_param(t);
        if (b[1] <= kb && kb < b[1] + b[2]) {
          const int16_t c14 = (int16_t)((b[3 + 2 * (kb - b[1])] & 0x7F) << 7 | (b[4 + 2 * (kb - b[1])] & 0x7F));
          p1_changed = c14 != s.src14[t];
          if (p1_changed) src_to_voice(t, c14);   // 0.5.0: an idle voice's pick (src_to_voice)
          s.src14[t] = c14, s.src14_at[t] = ++s.at_n;
        }
      }
      if (has13 && track_vb[t] != kNotOurs && machine_of(track_vb[t]) == kWaveMachine) {   // 2026-11-24 TBLB: WAVETABLE's table B
        const int w13 = s.srcv[t][12] << 7 | s.srcl[t][12];
        const bool p13_changed = w13 != old13;
        // the names bank follows the knob touched last; P1 and P13 changed in one frame (a kit load): P1 first (the names are P1's)
        if (p1_changed) s.p13_last &= (uint16_t)~(1u << t);
        else if (p13_changed) s.p13_last |= (uint16_t)(1u << t);
        // an idle voice's pick of B (as P1's above): only a real change of one knob (the first sight of P13 and a frame of
        // several values are a kit's load or a report: the voice's own reports set B then)
        if (p13_changed && old13 >= 0 && b[2] == 1) src_to_voice(t, w13, 12);
      } else if (p1_changed) {
        s.p13_last &= (uint16_t)~(1u << t);
      }
#if CORE_MSAMP
      if (!held && have_tracks && track_vb[t] != kNotOurs && (track_vb[t] & kMidiMachine) && s.src_known[t] == 0xFFFF) {
        float w[16];   // MIDI: a knob sends with the voice idle too (its 0x03s only come while it plays)
        for (int k = 0; k < 16; ++k) w[k] = s.srcv[t][k] + s.srcl[t][k] / 128.0f;
        midim::report(kTrackVoice[t], -1, 0, w);
      }
#endif
      if (b[2] == 1) s.src_last = (int8_t)b[1], s.src_last_ms = now_ms();
      else if (t == s.src_track) src_list_quiet();   // a kit's values at once (a load): no list for its SRC
      if (t == s.src_track) {   // drawn at once (the input path), a burst coalesced to its latest values
        s.input = true;
        if ((held || b[2] == 1) && !s.open) s.src_user = true;   // 0.4.4: a knob's (kSrcUserMs while playing)
      }
      s.stats.src_vals++;
      if (src_log) core::log("rui src: 2A %lu ms, track %d, first %d, n %d, P%d = %d.%d", (unsigned long)now_ms(), t, b[1], b[2],
                       b[1] + 1, b[3], b[4]);
      return;
    }
    case kDisplay: {   // rui1036b
      if (n < 2) break;
      if (b[0] == 4 && n >= 2) {   // rui1044: UP held alone on the stock screen (1) / let go (0): the combo map
        // 0.5.2: the Rytm drops the claim at every USB port drop and the host claimed at a hello only: UP held 0.4 s
        // shows a quick UP + RIGHT may be unclaimed; claim again (idempotent on the Rytm; not when off / going down)
        if (b[1] && !g_going_down) fill_claim();
        keys_map(b[1] != 0);
        return;
      }
      if (b[0] == 5 && n >= 2) {   // rui1050: UP held alone over an open page (1): the map over it; let go (0): that page again
        if (b[1] && s.open && !s.map_over && scrolls_updown(extra_of(s.voice))) return;   // 0.5.0: UP scrolls this page: no map
        if (b[1] && s.open && !s.map_over) {   // (a repeated 1: the page under it kept)
          s.map_back = s.voice, s.map_over = true;
          bool sc = s.shortcut;
          keys_map_over(true);
          s.shortcut = sc;   // rui1051: the map is no page of its own (NO's way stays)
        } else if (!b[1] && s.map_over) {
          s.map_over = false;
          if (keys_map_over(false) && s.open && s.voice != s.map_back) s.voice = s.map_back, s.page_changed = true, s.input = true, s.via = -1;
        }
        return;
      }
      if (!SIDECAR_MORPH && b[0] == 1 && n >= 2 && b[1] == 14) {   // UP + T14 (the rc5b mod still forwards it): the MORPH page is off
        remote_popup("MORPH: 0.5.2");
        return;
      }
      if (b[0] == 1 && n >= 2 && (b[1] == kPageFill || b[1] == kPageGenerator)) {   // 0.5.0: UP + RIGHT / LEFT
#if CORE_POLY
        if (b[1] == kPageFill) fill_page_open(-1);
        else remote_popup("GENERATOR: 0.5.2");   // (the pattern generator comes in 0.5.2: the key is bound already)
#else
        remote_popup(b[1] == kPageFill ? "FILL: NOT HERE" : "GENERATOR: 0.5.2");
#endif
        return;
      }
      if (b[0] == 1 && n >= 2 && sc_page_of_code(b[1])) {
        const sc_page_key_t* p = sc_page_of_code(b[1]);
        disp.req_page = b[1];
        disp.req_gen = disp.req_gen + 1;
        display_remote_page(b[1]);   // a page with a title on the Rytm's own screen (the table's `rytm`)
        if (disp_owned && (!p->rytm || (b[1] < 16 && (disp_owned >> b[1] & 1)))) {   // F1: the touchscreen took it - the Rytm says so
          char t[kTextLen + 1];
          sniprintf(t, sizeof t, CORE_STR("TOUCH: %s"), p->name);
          remote_popup(t);
        }
        return;
      }
      if (b[0] == 2 && n >= 3 && b[1] == 12) {   // LFO FINAL: the FX track (a 0026 that names it; the LFO page only)
        if (!disp.fx) disp.fx = true, disp.lfo_gen = disp.lfo_gen + 1;
        return;
      }
      if (b[0] == 2 && n >= 15 && b[1] < 12) {
        bool moved = disp.sel != b[1];
        if (moved || disp.fx) disp.lfo_gen = disp.lfo_gen + 1;
        disp.fx = false;
        for (int i = 0; i < 12; ++i) disp.name[i] = (char)(b[3 + i] >= 0x20 && b[3 + i] < 0x7F ? b[3 + i] : 0);
        disp.name[12] = 0;
        disp.machine = b[2], disp.sel = b[1];
        if (moved) disp.sel_gen = disp.sel_gen + 1;
#if CORE_POLY
        if (moved) seq::selected_track(b[1]), core::poly::selected_voice(remote_track_voice(b[1]));   // rui1040: the SEQ page
                                                  // follows the selection; the pads' binding too
        // 0.5.0: the track pages INSERT's track view, ARP and POLY follow the selection (a plain pad press selects its
        // track too, 49 1 2), also to a track of the Rytm's own synth, which each shows in its own way (INSERT its footer,
        // ARP and POLY their analog screens): "follow, never close". SEQ is the one that closes (owner 2026-11-15): on a
        // selection of a track that is no poly track seq::selected_track asks the Rytm to close it (3A 0 0), so the user
        // edits that track's own trigs; a poly track still follows. (A pattern or kit load that moves the selection does
        // the same.)
#endif
        disp.info_gen = disp.info_gen + 1;
        return;
      }
      if (b[0] == 3 && n >= 2 + 40 && b[1] < 12) {
        for (int k = 0; k < 8; ++k) {
          disp.val[k] = b[2 + 5 * k] & 0x7F;
          for (int i = 0; i < 4; ++i) disp.label[k][i] = (char)(b[3 + 5 * k + i] >= 0x20 && b[3 + 5 * k + i] < 0x7F ? b[3 + 5 * k + i] : 0);
          disp.label[k][4] = 0;
        }
        disp.info_gen = disp.info_gen + 1;
        return;
      }
      break;
    }
    case kWrite:   // rui1036: 48 <track> <param> <status>: the Rytm's answer to our write (logged when refused)
      if (n < 3) break;
      if (b[0] < 12 && b[1] >= 8 && b[1] < 16 && (s.p2_due >> b[0] & 1) && s.p2_stage[b[0]])   // rc5: page 2's answered
        s.p2_ans[b[0]] &= (uint8_t)~(1u << (b[1] - 8));
      if (b[0] < 12 && b[1] < 16) s.wr_ans[b[0]][b[1]] = (uint8_t)(2 + b[2]), ++s.wr_cnt[b[0]][b[1]];
      if (b[2] == 4) core::log("rui: 48 write track %d P%d locked on the steps held", b[0] + 1, b[1] + 1);   // (1038)
      else if (b[2]) core::log("rui: 48 write track %d P%d refused (status %d)", b[0] + 1, b[1] + 1, b[2]);
      if (b[0] < 12 && b[1] == 0 && s.wr_pat[b[0]] >= 0) {   // presetpick: PAT written into a JUNO track's sound (not a
        int t = b[0], pat = s.wr_pat[t];                     // lock, status 4): its patch's knobs now, whatever its voice
        s.wr_pat[t] = -1;                                    // is doing (a silent one reports nothing until its next trig)
        // rc040 review H1: on a shared voice (RS/CP, MT/HT, CH/OH, CY/CB) the Rytm routes the 15 to OWNER(voice), the
        // track that trigged it last, which the Pi cannot tell. With the partner track also JUNO that may be the partner:
        // its CUT..REL would be overwritten. So no sync then (as before presetpick: the next 0x03 carrying the PAT syncs).
        // With the partner not JUNO, a JUNO voice (x.juno, juno_sound_pat) can only be this track's.
        if (!b[2] && have_tracks && track_vb[t] != kNotOurs && machine_of(track_vb[t]) == kJuno && !juno_partner(t))
          core::juno_sound_pat(kTrackVoice[t], pat);
      }
      return;
    case kKit:   // 1026: the Rytm's active kit: the LFOs are per kit (core/lfo.h)
      if (n < 1 || b[0] > 127) break;
      lfo::kit_changed(b[0]);
      if (b[0] != kit_said) {  // (a 3B also comes with every page open and hello: the same kit is no change)
        kit_said = b[0];
        for (int t = 0; t < 12; ++t) s.src14[t] = -1;   // 0.5.0: the old kit's SMP / TBL / SRC / INST (its 2As come again)
        page_two_cancel();     // rc5: a kit loaded around a list choice: its machines and words are the kit's
#if CORE_MSAMP
        midim::kit_switched(); // MIDI: its held notes off; the new kit's PRG and the rest go out at its reports
#endif
#if CORE_POLY
        arp::kit_changed();    // the arpeggios end; the new kit's settings come as 45s
#endif
      }
#if CORE_POLY
      ins::kit_changed(b[0]);   // the inserts are per kit too (core/fx/insert.h)
      mod::rp_kit(b[0]);        // the Rytm's own params: the kit byte of every message (core/mod/rytm_param.h)
      xypad::kit_changed(b[0]);   // the XY pad (core/mod/xypad.h): released first, its fader back before the scenes save
      scene::kit_changed(b[0]);   // and the scenes (core/mod/scene.h)
#endif
#if CORE_MSAMP
      gr::kit_changed(b[0]);   // GRANULAR's picks are per kit too
#endif
      return;
    case kHello: {   // 1023: <uptime >> 7> <uptime & 0x7F> <caps>: the splash when the Rytm was up before the link
      g_nohello_told = false;
      s.presets_told = false;
      s.mlist = false, page_two_cancel();   // rc5: a new link: no list choice owed
      s.covered = false;   // (the Rytm's 2B says again)
      if (n < 3) break;
      s.stats.hellos++;
      int up = b[0] << 7 | b[1];
      if (s.stats.hellos > 1) voice_reports_forget();   // the Rytm restarted: its 33 re-seeds the engines (not the Pi's own boot)
      // A new link (a replug, the Rytm's or our restart): whatever the Rytm showed before is gone - its page left by its
      // 1 s timeout, its SRC view says 29 t 1 again after this hello if it is on. Stale state here (the page open, a
      // view's track, a splash running when the cable went) blocked the splash and the view until a restart.
      bool was_open = s.open, was_view = s.src_track >= 0, was_splash = s.splash;
      if (s.open && s.now_ms - s.open_ms <= kHelloOpenMs) {
        // The user opened the page just before this (first) hello: it is theirs and the Rytm still shows it - no splash,
        // no close (a splash over it would be ignored by the Rytm, which holds the page, while we stopped serving it).
        // What the hello invalidates on the Rytm's side is sent again, as after an open.
        s.sent_valid = false, s.mask_valid = false, s.echo_valid = false, s.leds_on = false, s.clock_set = false;
        release_held();   // 0.5.0: a key held across the new link sends no release
        s.up_down = false;
        s.caps36 = (b[2] & 1) != 0, s.caps = b[2], s.caps2 = n >= 4 ? b[3] : 0;
        s.page_down = s.page_owed = false;   // (v2 review M1: a key held across the new link sends no release)
        mod::rytm_host_cmd((b[2] & mod::kCapsHostCmd) != 0);   // 0.5.0: the mod command (core/mod/rytm_out.h)
        fill_claim();
        edit_arm();
        list_p13_announce();
#if CORE_POLY
        mod::rp_hello(b[2]);   // 0.5.0: the Rytm's own params (caps bit 64; core/mod/rytm_param.h): what was sent is gone
#endif
        core::log("rui: hello, Rytm up %d.%d s, caps %d, page opened %d ms ago: kept", up / 20, (up % 20) / 2, b[2],
                  (int)(s.now_ms - s.open_ms));
        src_names_resend();
        return;
      }
      if (s.open) s.stats.closes++;
      s.open = false, s.leds_on = false, s.mask_valid = false, s.echo_valid = false, s.sent_valid = false;
      s.src_track = -1, s.src_told = -1, s.splash = false, s.clock_set = false;
      tap_enable(false);
      s.caps36 = (b[2] & 1) != 0, s.caps = b[2], s.caps2 = n >= 4 ? b[3] : 0;
      s.page_down = s.page_owed = false;   // (v2 review M1: a key held across the new link sends no release)
      mod::rytm_host_cmd((b[2] & mod::kCapsHostCmd) != 0);   // 0.5.0: the mod command (core/mod/rytm_out.h)
      fill_claim();
      edit_arm();
      list_p13_announce();
#if CORE_POLY
      mod::rp_hello(b[2]);   // 0.5.0: the Rytm's own params (caps bit 64; core/mod/rytm_param.h): what was sent is gone
#endif
      keys_map_drop(), s.map_over = false;   // (a release lost with the link: no map left up)
      sc_mon_page_left();                    // 0.5.2: and no MON left on
      release_held();                        // 0.5.0: and every key a page holds
      s.up_down = false;
      // A status popup goes with the old link. The pairing code's modal does not: it is owed until it has been on
      // this Rytm's screen, and a hello arriving while it was owed or up (the Rytm restarting, or saying hello again
      // after a silence) dropped it, so the code was never seen (audit 2026-10-26). It starts again, on this link.
      if (!(s.popup && s.popup_over && s.caps36 && !s.legacy)) s.popup = false;
      s.popup_clock = false;
      bool splash = (b[2] & 1) && !s.legacy && up >= kSplashMinUp;
      core::log("rui: hello, Rytm up %d.%d s, caps %d%s%s%s: %s", up / 20, (up % 20) / 2, b[2], was_open ? ", page was open" : "",
                was_view ? ", SRC view was on" : "", was_splash ? ", splash was running" : "", splash ? "splash" : "alive");
      src_names_resend();   // rui1036: the Rytm's names (47) again
      if (splash) s.splash = true, s.splash_clock = false;
      else send_short(kAlive, nullptr, 0);   // an answer: the Rytm says hello again only while the host is silent
      return;
    }
    default:
      return;   // 38..3F are the host's own
  }
  s.stats.bad++;
}

// 0.5.0 UI feel: the Rytm sends a held key's press and release only, so a page that took UP / DOWN / LEFT / RIGHT gets them
// again from here, the way a list scrolls on the Rytm: the first after kRepeatFirstMs, then every kRepeatMs. It stops on the
// release (remote_sysex), another key, a page change, the page closing, a hello (release_held), when the page stops taking
// the key (the end of its list: false), and after kRepeatCapMs, so a release lost with the link cannot repeat for ever.
static CORE_COLD void key_repeat_tick(uint32_t ms) {
  const ExtraPage* x = s.open ? extra_of(s.voice) : nullptr;
  if (!x || !x->key || (x->shown && !x->shown()) || s.voice != s.rep_voice || ms - s.rep_t0 >= (uint32_t)kRepeatCapMs) {
    s.rep_code = -1;
    return;
  }
  for (int n = 0; (int32_t)(ms - s.rep_next) >= 0; ++n) {
    if (n == 4) {   // (a stalled engine: no burst, on from now)
      s.rep_next = ms + kRepeatMs;
      break;
    }
    if (!x->key(s.rep_code)) {
      s.rep_code = -1;
      return;
    }
    s.rep_next += kRepeatMs, s.input = true;
  }
}

// 0.5.2 re-hello: the Rytm sends its hello only at its USB port-up, so a host that started later has no caps (host command,
// QPERF, edit arm, the 36s ...). Once Rytm traffic has come and no hello, ask: 3C with flags bit 0 (kCaps2Rehello answers it
// with one hello; an older mod takes it as a plain keepalive: its 3C reads no payload). Once, then every kRehelloMs, 3 tries.
// Never for a box (A4 / Digitakt), never going down. A hello ends it (hellos > 0).
constexpr uint32_t kRehelloMs = 5000;
constexpr int kRehelloTries = 3;
CORE_COLD void rehello_tick(uint32_t ms) {
  if (!s.rh_traffic || s.stats.hellos > 0 || g_going_down || device::foreign() || s.rh_tries >= kRehelloTries) return;
  if (s.rh_tries && (int32_t)(ms - s.rh_ms) < (int32_t)kRehelloMs) return;
  const uint8_t flags = 1;
  send_short(kAlive, &flags, 1);
  s.rh_tries++, s.rh_ms = ms;
  if (s.rh_tries == 1) core::log("rui: Rytm traffic but no hello: asking it to say hello again");
}

// The Rytm's USB MIDI is gone (core::link_lost): the next one starts the asking again (a new Rytm, or one that missed its hello)
CORE_COLD void remote_link_lost() { s.rh_traffic = false, s.rh_tries = 0, s.rh_ms = 0; }

CORE_COLD void remote_tick(uint32_t ms) {
  if (g_going_down && !g_fill_keys_off) g_fill_keys_off = true, fill_unclaim();   // (a planned reboot: remote.h)
  // 0.5.2: the claim again every ~2 s while linked (the Rytm drops it at each USB port drop; a 4A 7E 1 only sets its flag)
  if (g_fill_claimed && !g_going_down && (int32_t)(ms - g_claim_ms) >= 2000) fill_claim(), g_claim_ms = ms;
  if (!s.presets_told && s.src_track >= 0) {
    int m = track_machine(s.src_track);
    if (m == core::kJuno || m == core::kFm) {
      const uint8_t cap[9] = {0xF0, 0x7D, 0x52, 0x54, kBankList, 1, 0x7F, 1, 0xF7};
      core::to_rytm(cap, 9);
      s.presets_told = true;
    }
  }
  s.now_ms = ms;
  rehello_tick(ms);
  vparam_tick(ms);   // the touchscreen's VOICE knob writes' answers (a few compares when none is followed)
  if (s.rep_code >= 0) key_repeat_tick(ms);
  page_two_tick(ms);   // rc5: a machine list choice's page-2 defaults
#if CORE_POLY && CORE_MSAMP
  static uint32_t rename_seen = 0;   // UX review B1: a rename's question shows the PROJECT page (it asks there)
  if (rename::asks() != rename_seen) {
    rename_seen = rename::asks();
    if (rename::state() == rename::kAsk) remote_show_page("PROJECT");
  }
  static uint32_t prjslot_seen = 0;   // 2026-10-27: the slot reports' questions too (core/bundle/prjslot.h)
  if (prjslot::asks() != prjslot_seen) {
    prjslot_seen = prjslot::asks();
    if (prjslot::ask()) remote_show_page("PROJECT");
  }
#endif
  if (have_tracks) src_names_tick();   // rui1036: the source lists' names for the Rytm (47), on a change
  // the pairing code's modal comes before the connect splash and the SRC view: it is the one thing the user needs
  if (s.popup && s.popup_over && !s.open) { popup_tick(ms); return; }
  if (s.splash && !s.open) { splash_tick(ms); return; }
  if (s.popup && !s.open && s.src_track < 0) { popup_tick(ms); return; }
  if (!s.open && s.src_track >= 0) { src_tick(ms); return; }
  if (!s.open) return;
#if CORE_POLY && CORE_MSAMP
  // SAMPLE EDIT closed itself after MAKE WT for a WAVETABLE voice: back to where it was opened from (the SRC view)
  if (extra_of(s.voice) == &kSeditPage && sedit_page_leaves() && !sedit_back_to() && !go_back()) go(0);
#endif
  wave_idle(ms);   // a sample overview being read (page_wave.cpp): one column at most
  uint32_t tick = s.legacy ? 50 : kTickMs;
  if (!s.clock_set) s.clock_set = true, s.last_draw = ms - tick, s.last_tx = ms;
  if (ms - s.last_draw >= tick || (s.input && !s.legacy && ms - s.last_draw >= kInputMs)) {
    s.last_draw = ms;
    s.input = false;
    frame(ms);
  }
  if (ms - s.last_tx >= kAliveMs) {
    send_short(kAlive, nullptr, 0);
    s.last_tx = ms;
    s.stats.keepalives++;
  }
}

// What was sent to the Rytm may not have reached it (records dropped between here and its USB port, no replug and so no
// hello): nothing tells this file which, and a diff against a frame the Rytm never got leaves it on the stock page or on
// a page with holes. Everything owed since a hello goes again, as after one, with nothing of the Rytm's state reset: a
// whole frame, the mask, the LEDs, the splash or the modal over, the names.
CORE_COLD void remote_resync() {
  s.sent_valid = false, s.mask_valid = false, s.echo_valid = false, s.leds_on = false, s.clock_set = false;
  s.src_told = -1, s.splash_clock = false, s.presets_told = false;
  if (s.popup_over) s.popup_clock = false;
  src_names_resend();
  ++s.stats.resyncs;
}

CORE_COLD void remote_request(bool on) {
  uint8_t b[2] = {0, (uint8_t)(on ? 1 : 0)};
  send_short(kOpen, b, 2);
}

CORE_COLD bool remote_open() { return s.open; }
CORE_COLD int remote_caps() { return s.caps; }
CORE_COLD int remote_caps2() { return s.caps2; }

CORE_COLD void remote_show_page(const char* title) {
  if (!strcmp(title, "SYSTEM")) {   // (MENU's SYSTEM: the second status page)
    if (s.open && s.voice != -2) s.shortcut = false, lfo_left(s.voice, -2), s.voice = -2, s.page_changed = true, s.input = true, s.via = -1;
    return;
  }
  if (!strcmp(title, kStatusTitle)) {   // the status page (SIDECAR, the first): the page opens on it
    if (s.open && s.voice != -1) s.shortcut = false, lfo_left(s.voice, -1), s.voice = -1, s.page_changed = true, s.input = true, s.via = -1;
    else if (!s.open && !device::foreign()) want_title = nullptr, remote_request(true);
    return;
  }
  if (s.open) {
    for (int k = 0; k < extra_pages(); ++k)
      if (!strcmp(extra_page(k).title, title) && s.voice != kExtra0 - k) {
#if CORE_POLY && CORE_MSAMP
        if (!strcmp(title, "SAMPLE EDIT")) {   // sedit050: closing the edit comes back here (not over the MENU)
          const ExtraPage* now = extra_of(s.voice);
          bool menu = now && !strcmp(now->title, "MENU");
          s.sedit_back = menu ? kNoPage : s.voice, s.sedit_back_sc = s.shortcut;
        }
#endif
        if (!strcmp(title, "MENU")) s.shortcut = false;   // nav044 review: the MENU (YES on SIDECAR): NO to SIDECAR
        else shortcut_over(s.voice);   // rui1051, nav044: NO leaves (the MENU's pick: SIDECAR, as before)
        lfo_left(s.voice, kExtra0 - k), s.via = -1;
        s.voice = kExtra0 - k, s.page_changed = true, s.input = true;
      }
    return;
  }
  if (device::foreign()) return;   // a box's screen opens by its user alone (a4_rx takes 3A's close only): nothing to ask, and a title left here would land the next open on it
#if CORE_POLY && CORE_MSAMP
  if (!strcmp(title, "SAMPLE EDIT")) s.sedit_back = kNoPage;   // sedit050: from the Rytm's own screen: closing leaves
#endif
  want_title = title;
  remote_request(true);
}

CORE_COLD void remote_morph_qperf_move() {
  const char* t = remote_page_title();
  if (t && !strcmp(t, "MORPH")) s.qp_user = true, s.qp_ms = s.now_ms;   // other pages do not change with the fader: they keep the plain cap
}
CORE_COLD const char* remote_page_title() {
  const ExtraPage* x = s.open ? extra_of(s.voice) : nullptr;
  return x ? x->title : nullptr;
}

CORE_COLD void remote_status(const RemoteStatus& st) {
  ssrc.st = st;
  if (st.underruns && !s.underrun_told) {   // F4: the session's first dropout, once (it never nags)
    int hi = -1;
    for (int k = 0; k < 3; ++k) hi = st.cpu[k] > hi ? st.cpu[k] : hi;
    char b[kTextLen + 1];
    if (hi >= 0) sniprintf(b, sizeof b, CORE_STR("LOAD %d%%  SEE STATUS"), hi);
    else sniprintf(b, sizeof b, "SEE STATUS (UP+T10)");
    // told once it is shown: a popup refused (the pairing code owed or up) is offered again at the next status
    s.underrun_told = remote_popup("SIDECAR AUDIO DROPOUT", b);
  }
}
CORE_COLD void remote_pair_code(const char* g) {
  sniprintf(pair_code, sizeof pair_code, "%s", g ? g : "");
  s.page_changed = true;   // (the status page redraws whole with it)
}
CORE_COLD const char* remote_pair_code_now() { return pair_code; }
const RemoteWebUi* webui_hooks = nullptr;
CORE_COLD void remote_webui(const RemoteWebUi* h) { webui_hooks = h; }
CORE_COLD const RemoteWebUi* remote_webui() { return webui_hooks; }
const RemoteUsb* usb_hooks = nullptr;
CORE_COLD void remote_usb(const RemoteUsb* h) { usb_hooks = h; }
CORE_COLD const RemoteUsb* remote_usb() { return usb_hooks; }
CORE_COLD bool remote_popup(const char* l1, const char* l2, uint32_t hold_ms, bool over) {
  // caps36 and legacy are what the Rytm can do, and no policy changes that: an older mod cannot draw this at all.
  // `open` and `splash` are policy - deferring to a Sidecar page is right for a status popup and wrong for the
  // pairing code, which is what the user needs before the device can be used at all.
  if (!s.caps36 || s.legacy) return false;
  if (!over && (s.open || s.splash)) return false;
  // the pairing code's popup (`over`) wins: a status popup never replaces it, owed or up - it would overwrite the code
  // the user is reading off the screen (audit 2026-10-26). It is refused, as over an open page, and offered again.
  if (!over && s.popup && s.popup_over) return false;
  uint32_t ms = now_ms();
  // the same text again while it is still up is the same popup; once it has gone, asking again shows it again
  // (`pair=show` right after the code's modal timed out did nothing before this)
  if (s.popup && s.popups && !strcmp(s.popup_l1, l1 ? l1 : "") && ms - s.popup_last < 2000) return true;
  sniprintf(s.popup_l1, sizeof s.popup_l1, "%s", l1 ? l1 : "");
  sniprintf(s.popup_l2, sizeof s.popup_l2, "%s", l2 ? l2 : "");
  s.popup_ms = hold_ms > kPopupMaxMs ? kPopupMaxMs : hold_ms;   // (the 36 is sent again for a longer one: popup_tick)
  s.popup = true, s.popup_over = over, s.popup_clock = false, s.popup_last = ms;
  return true;
}

// Take it off now (a device paired: the code is no longer wanted on screen). It goes through the same expiry as a
// timeout, so the page underneath comes back the same way.
CORE_COLD void remote_popup_end() {
  if (!s.popup) return;
  if (!s.popup_clock) { s.popup = false; return; }   // never drawn: nothing to take down
  s.popup_ms = 1;
  s.popup_t0 = now_ms() - 2;
}
CORE_COLD uint32_t remote_popups() { return s.popups; }
CORE_COLD const char* remote_popup_text() { return s.popup_l1; }
CORE_COLD uint32_t remote_status_popups() { return s.status_popups; }
CORE_COLD void remote_legacy(bool on) { s.legacy = on; }
CORE_COLD void remote_src_log(bool on) { src_log = on; }

CORE_COLD void remote_params(int vi, int m, float* p, int np) {
  vi &= 7;
  reported[vi] = true;
  s.rep_at[vi] = ++s.at_n;
  if (s.self) return;
  if (s.held_m[vi] != m) s.held[vi] = 0, s.held_m[vi] = (uint8_t)m;   // another machine: its edits go
  for (int k = 0; k < np && k < 16; ++k) {
    int rv = (int)p[k];
    uint16_t bit = (uint16_t)(1u << k);
    if (s.held[vi] & bit) {
      if (s.rytm_seen[vi] && rv != s.rytm_v[vi][k]) s.held[vi] &= (uint16_t)~bit;   // the Rytm's value moved: it wins
      else p[k] = s.held_v[vi][k];
    }
    s.rytm_v[vi][k] = (uint8_t)(rv < 0 ? 0 : rv > 127 ? 127 : rv);
  }
  s.rytm_seen[vi] = true;
}

#if CORE_UI_TOOLS   // the renderer and the benchmark: the Mac build only (core/Makefile)
namespace {
class DemoSource : public Source {
 public:
  uint8_t v[16] = {};
  int8_t pk[2 * 64];
  uint8_t sl[3] = {32, 64, 96};
  DemoSource() {   // a decaying two-tone burst, then a second hit: something to see
    for (int i = 0; i < 64; ++i) {
      int env = i < 32 ? 127 - i * 3 : 127 - (i - 32) * 4;
      if (env < 4) env = 4;
      int a = env * (sin256(i * 37) + 127) / 254;
      pk[2 * i] = (int8_t)-a, pk[2 * i + 1] = (int8_t)(a * 3 / 4 + 4 > 127 ? 127 : a * 3 / 4 + 4);
    }
  }
  int param(int k) const override { return v[k & 15] & 0x7F; }
  int peaks(int, const int8_t** p, const uint8_t** s, int* ns) const override {
    *p = pk, *s = sl, *ns = 3;
    return 64;
  }
};
DemoSource demo;
PageView render_view;

const Page* page_of(int which, int voice, const Source** src) {
  if (which == 1) {
    vsrc.vi = voice & 7;
    *src = &vsrc;
    return src_list_page(page_for_voice(named_machine(machine_of_state(voice & 7), voice & 7), voice & 7), vsrc);
  }
  if (which == 2 || which == 3) { *src = &demo; return &kDemoPages[which - 2]; }
  return nullptr;
}
}  // namespace

CORE_COLD bool remote_render(int which, int voice, const uint8_t* vals, Canvas& c) {
  if (vals) memcpy(demo.v, vals, 16);
  if (which == 5) { splash_draw(c, vals ? vals[0] * 25u : 1000u); return true; }   // the splash at vals[0] x 25 ms
  if (which >= 6) {   // 6 + k: extra page k (extra.h: LFO, REC, SCOPE), its live part as it is now
    if (which - 6 >= extra_pages()) return false;
    extra_page(which - 6).draw(c, 2 + kVoices + (which - 6), 2 + kVoices + extra_pages(), now_ms() | 1);
    return true;
  }
  if (which == 0 || which == 4) { ssrc.draw(c, which ? 1 : 0, which ? 1 : 0, 2); return true; }
  const Source* src = nullptr;
  const Page* p = page_of(which, voice, &src);
  if (!p) return false;
  char right[8] = "";
  if (which == 1) sniprintf(right, sizeof right, "%s", voice_label(voice));
  render_view.invalidate();
  render_view.draw(c, *p, *src, right, which >= 2 ? which - 2 : which, 2);
  if (which == 1) {   // the bottom bar as the host draws it on that page (encoder A)
    Canvas keep = work;
    work = c;
    voice_footer(*p, voice_label(voice & 7), *src, 0);
    c = work;
    work = keep;
  }
  return true;
}

CORE_COLD void remote_bench(int which, int voice, int n, float* full_us, float* one_us) {
  static Canvas a, b;
  *full_us = *one_us = 0;
  if (n <= 0) return;
  uint32_t m[4];
  uint64_t tm;
  if (which >= 6 && which - 6 < extra_pages()) {   // an extra page: drawn whole each frame
    uint32_t t0 = cycles();
    for (int i = 0; i < n; ++i) extra_page(which - 6).draw(a, 2, 4, now_ms() | 1), a.diff_tiles(b, m), a.diff_text(b, &tm);
    *full_us = *one_us = (float)(cycles() - t0) * (1e6f / (float)cycles_per_second()) / n;
    return;
  }
  if (which == 0 || which == 4) {
    uint32_t t0 = cycles();
    for (int i = 0; i < n; ++i) ssrc.draw(a, which ? 1 : 0, 0, 2), a.diff_tiles(b, m), a.diff_text(b, &tm);
    *full_us = *one_us = (float)(cycles() - t0) * (1e6f / (float)cycles_per_second()) / n;
    return;
  }
  const Source* src = nullptr;
  const Page* p = page_of(which, voice, &src);
  if (!p) return;
  uint32_t t0 = cycles();
  for (int i = 0; i < n; ++i) {
    render_view.invalidate();
    render_view.draw(a, *p, *src, "V1", 1, 2);
    a.diff_tiles(b, m);
    a.diff_text(b, &tm);
  }
  uint32_t t1 = cycles();
  // one change a round: the demo's / the voice's first encoder's parameter moved by one, then back
  int k = p->enc[0] == kNone ? 0 : p->enc[0];
  for (int i = 0; i < n; ++i) {
    if (which >= 2) demo.v[k] = (uint8_t)((demo.v[k] + 1) & 0x7F);
    else if (which == 1) v[voice & 7].p[k] = (float)(((int)v[voice & 7].p[k] + 1) & 0x7F);
    render_view.draw(a, *p, *src, "V1", 1, 2);
    a.diff_tiles(b, m);
    a.diff_text(b, &tm);
    b = a;
  }
  uint32_t t2 = cycles();
  float us = 1e6f / (float)cycles_per_second();
  *full_us = (float)(t1 - t0) * us / n;
  *one_us = (float)(t2 - t1) * us / n;
}

#endif  // CORE_UI_TOOLS

CORE_COLD const RemoteStats& remote_stats() { return s.stats; }
CORE_COLD const uint8_t* remote_frame() { return sent.fb; }
CORE_COLD void preview_sent(uint8_t out[kBytes]) { preview(sent, out); }
CORE_COLD int sent_texts(uint8_t* out, int max) {
  int n = 0;
  for (int i = 0; i < kTextSlots && n < max; ++i) {
    const TextItem& t = sent.txt[i];
    if (!t.s[0]) continue;
    uint8_t* o = out + 36 * n++;
    o[0] = t.x, o[1] = t.y, o[2] = t.font, o[3] = t.flags;
    memset(o + 4, 0, 32);
    memcpy(o + 4, t.s, strlen(t.s));
  }
  return n;
}

CORE_COLD void remote_forget_voices() { memset(reported, 0, sizeof reported); }
CORE_COLD int remote_track_machine(int t) {
  return have_tracks && t >= 0 && t < 12 && (tracks_known >> t & 1) ? track_vb[t] : -1;
}
CORE_COLD int remote_track_rytm_machine(int t) { return remote_track_machine(t) >= 0 ? track_rm[t] : -1; }
CORE_COLD bool remote_track_smpl(int t) {
  return remote_track_machine(t) >= 0 && (track_vb[t] & 0x78) == kDspSample && track_smpl[t];
}
CORE_COLD bool remote_voice_smpl(int vi) { return voice_smpl(vi); }
CORE_COLD int remote_track_voice(int t) { return t >= 0 && t < 12 ? kTrackVoice[t] : -1; }
CORE_COLD int remote_src_track() { return s.src_track; }
CORE_COLD void remote_display_owns(uint16_t pages) { disp_owned = pages; }
CORE_COLD bool remote_display_owned(int page) { return page >= 0 && page < 16 && (disp_owned >> page & 1); }
CORE_COLD bool remote_display_present() { return disp_owned != 0; }
CORE_COLD uint32_t remote_display_request(int* page) {
  if (page) *page = disp.req_page;
  return disp.req_gen;
}
CORE_COLD int remote_selected_track(uint32_t* gen) {
  if (gen) *gen = disp.sel_gen;
  return disp.sel;
}
CORE_COLD int remote_lfo_track(uint32_t* gen) {
  if (gen) *gen = disp.lfo_gen;
  return disp.fx ? 12 : disp.sel;
}
// The machine voice vi plays by name (machine_of_voice, before any report its state's), for its parameters' labels
CORE_COLD int remote_voice_machine(int vi) {
  int m = machine_of_voice(vi & 7);
  return named_machine(m == kNoMachine ? machine_of_state(vi & 7) : m, vi & 7);
}
CORE_COLD uint32_t remote_track_info(int* track, int* machine, char name[13], uint8_t val[8], char label[8][5]) {
  if (track) *track = disp.sel;
  if (machine) *machine = disp.machine;
  if (name) memcpy(name, disp.name, 13);
  if (val) memcpy(val, disp.val, 8);
  if (label) memcpy(label, disp.label, sizeof disp.label);
  return disp.info_gen;
}
// 2026-11-24 TBLB: a written P13 (WAVETABLE) is the track's P13 word at once: the names bank follows it and the Rytm's echo is no change
static void p13_written(int t, int value, int lo) {
  s.p13_last |= (uint16_t)(1u << t);
  s.srcv[t][12] = (uint8_t)(value < 0 ? 0 : value > 127 ? 127 : value), s.srcl[t][12] = (uint8_t)(lo & 0x7f);
  s.src_known[t] |= (uint16_t)(1u << 12);
}
CORE_COLD int remote_param_write(int vi, int machine, int k, int value, int lo) {
  int n = 0;
  for (int t = 0; t < 12; ++t) {
    if (kTrackVoice[t] != (vi & 7) || !have_tracks || track_vb[t] == kNotOurs || machine_of(track_vb[t]) != machine) continue;
    uint8_t b[4] = {(uint8_t)t, (uint8_t)(k & 15), (uint8_t)(value < 0 ? 0 : value > 127 ? 127 : value), (uint8_t)(lo & 0x7f)};
    send_short(kWrite, b, lo > 0 ? 4 : 3);   // rui1038: the fraction byte (a bank) only when there is one: older builds too
    if (k == remote_track_src_param(t)) s.src14[t] = (int16_t)(b[2] << 7 | b[3]);
    if (k == 12 && machine == kWaveMachine) p13_written(t, value, lo);   // 2026-11-24 TBLB: the names bank follows the knob touched last
    else if (k == remote_track_src_param(t)) s.p13_last &= (uint16_t)~(1u << t);
    ++n;
  }
  return n;
}
CORE_COLD bool remote_track_write(int t, int k, int value, int lo, int flags) {
  if (t < 0 || t >= 12 || !have_tracks || track_vb[t] == kNotOurs) return false;
  uint8_t b[5] = {(uint8_t)t, (uint8_t)(k & 15), (uint8_t)(value < 0 ? 0 : value > 127 ? 127 : value), (uint8_t)(lo & 0x7f),
                  (uint8_t)(flags & 0x7f)};
  send_short(kWrite, b, flags ? 5 : lo > 0 ? 4 : 3);
  if (k == remote_track_src_param(t)) s.src14[t] = (int16_t)(b[2] << 7 | b[3]);
  if (k == 12 && machine_of(track_vb[t]) == kWaveMachine) p13_written(t, value, lo);   // 2026-11-24 TBLB (see remote_param_write)
  else if (k == remote_track_src_param(t)) s.p13_last &= (uint16_t)~(1u << t);
  if (k == 0) s.wr_pat[t] = b[2];   // (its answer: kWrite)
  s.wr_ans[t][b[1]] = 1;
  return true;
}
CORE_COLD uint16_t remote_track_write_answers(int t, int k) { return t >= 0 && t < 12 && k >= 0 && k < 16 ? s.wr_cnt[t][k] : 0; }
CORE_COLD int remote_track_write_answer(int t, int k) {
  if (t < 0 || t >= 12 || k < 0 || k >= 16) return -2;
  const int a = s.wr_ans[t][k];
  return a == 0 ? -2 : a == 1 ? -1 : a - 2;
}
// ---- the touchscreen's VOICE knobs (vparam=T:K:V[:0]): a drag's values written to the Rytm as a knob turn writes them
// (remote_track_write: 48). The Rytm answers 48 with 0 written, 1 refused (shared SRC / page 2 records not set up for the machine),
// 2 no such parameter, 3 lock slots full, 4 locked on the steps held; there is no "busy". A full write queue on the Rytm drops a
// write with no answer, and builds without 48 never answer. Only the final value of a drag is followed (the line without ":0",
// sent at the lift): an answer to an earlier write of the knob must not clear it, so the answers are counted
// (remote_track_write_answers) and the last one's status is read once every write sent has been answered. Status 1 is sent again
// every kVpRetryMs, at most kVpRetries times within kVpRefusedMs of the first one (a new gesture's final line starts again),
// then it is refused; 3, 4 and 2 are refused at once (4 as "locked") and kept for the page (remote_vparam_failed: it puts the
// knob back and says so); no answer for kVpOweMs is nothing to follow. A new write of a knob forgets its earlier failure.
namespace {
struct VParam {
  uint16_t mask = 0;                    // the knobs (bit k: P(k + 1)) whose final write is followed
  uint8_t v[16] = {}, retries[16] = {};
  uint16_t sent[16] = {}, got0[16] = {};   // writes sent (live ones too) since the last time none was owed; the answer count then
  uint32_t t0[16] = {}, first1[16] = {}, next[16] = {};
};
VParam g_vp[12];
volatile uint16_t g_vp_failed[12];      // bit k: the last write of P(k + 1) was refused or locked
volatile uint8_t g_vp_kind[12];         // 1 locked on the steps held, 2 refused (the last failure's)
uint32_t g_vp_gen = 0;                  // written last (release), read first (acquire): core 3 -> the display's core 0
constexpr uint32_t kVpRetryMs = 100, kVpRefusedMs = 1000, kVpOweMs = 1500;
constexpr int kVpRetries = 10;
void vp_fail(int t, int k, int kind) {
  g_vp_failed[t] = (uint16_t)(g_vp_failed[t] | (1u << k));
  g_vp_kind[t] = (uint8_t)kind;
  __atomic_store_n(&g_vp_gen, g_vp_gen + 1, __ATOMIC_RELEASE);
}
uint16_t vp_owed(int t, int k) {   // (an answer nobody asked for, a stray one, owes nothing)
  const int16_t d = (int16_t)(g_vp[t].sent[k] - (uint16_t)(remote_track_write_answers(t, k) - g_vp[t].got0[k]));
  return d > 0 ? (uint16_t)d : 0;
}
bool vp_send(int t, int k, int v) {
  VParam& w = g_vp[t];
  if (!vp_owed(t, k)) w.got0[k] = remote_track_write_answers(t, k), w.sent[k] = 0;   // nothing owed: count from here
  if (!remote_track_write(t, k, v, 0, 0)) return false;
  ++w.sent[k];
  return true;
}
}  // namespace

CORE_COLD bool vparam_command(const char* p, char* o, int cap) {
  if (strncmp(p, "vparam", 6) != 0 || (p[6] && p[6] != '=')) return false;
  static const char kHelp[] = "vparam=T:K:V[:0] (track 0..11, the Rytm's P(K+1) with K 0..15, value 0..127; :0 a drag's live line, not followed: the touchscreen's VOICE knobs)";
  int a[3] = {-1, -1, -1};
  bool live = false;
  const char* q = p[6] ? p + 7 : p + 6;
  for (int i = 0; i < 3; ++i) {
    int n = 0, v = 0;
    for (; q[n] >= '0' && q[n] <= '9' && n < 4; ++n) v = v * 10 + (q[n] - '0');
    const bool tail = i < 2 || (q[n] == ':' && q[n + 1] == '0' && !q[n + 2]);
    if (!n || (i < 2 ? q[n] != ':' : q[n] != 0 && !tail)) return sniprintf(o, cap, "%s", kHelp), true;
    if (i == 2 && q[n]) live = true;
    a[i] = v, q += n + (i < 2 ? 1 : 0);
  }
  const int t = a[0], k = a[1], v = a[2];
  if (t > 11 || k > 15 || v > 127) return sniprintf(o, cap, "%s", kHelp), true;
  g_vp_failed[t] = (uint16_t)(g_vp_failed[t] & ~(1u << k));
  VParam& w = g_vp[t];
  if (!vp_send(t, k, v)) {   // not a Sidecar machine: nothing sent, the page puts the knob back
    w.mask = (uint16_t)(w.mask & ~(1u << k));
    vp_fail(t, k, 2);
    return sniprintf(o, cap, "vparam: track %d is not a Sidecar machine", t + 1), true;
  }
  if (!live) {
    w.mask = (uint16_t)(w.mask | (1u << k)), w.v[k] = (uint8_t)v, w.t0[k] = s.now_ms, w.first1[k] = 0, w.retries[k] = 0;
    w.next[k] = s.now_ms + kVpRetryMs;
  }
  return sniprintf(o, cap, "vparam: track %d P%d = %d%s", t + 1, k + 1, v, live ? " (live)" : ""), true;
}

CORE_COLD void vparam_tick(uint32_t ms) {
  for (int t = 0; t < 12; ++t) {
    VParam& w = g_vp[t];
    if (!w.mask) continue;
    for (int k = 0; k < 16; ++k) {
      if (!(w.mask >> k & 1)) continue;
      if (vp_owed(t, k)) {   // writes still unanswered (an earlier one's answer is not the last's)
        if (ms - w.t0[k] >= kVpOweMs) w.mask = (uint16_t)(w.mask & ~(1u << k)), w.got0[k] = (uint16_t)(remote_track_write_answers(t, k) - w.sent[k]);   // dropped, or no 48: nothing to follow
        continue;
      }
      const int a = remote_track_write_answer(t, k);
      if (a == 1) {   // refused: again, a few times
        if (!w.first1[k]) w.first1[k] = ms ? ms : 1;
        if (w.retries[k] >= kVpRetries || ms - w.first1[k] >= kVpRefusedMs) w.mask = (uint16_t)(w.mask & ~(1u << k)), vp_fail(t, k, 2);
        else if ((int32_t)(ms - w.next[k]) >= 0) vp_send(t, k, w.v[k]), ++w.retries[k], w.next[k] = ms + kVpRetryMs, w.t0[k] = ms;
      } else {
        w.mask = (uint16_t)(w.mask & ~(1u << k));   // written (0), none (-2), locked (4) or another refusal
        if (a > 0) vp_fail(t, k, a == 4 ? 1 : 2);
      }
    }
  }
}
CORE_COLD unsigned remote_vparam_failed(int t, int* kind) {
  if (t < 0 || t >= 12) return 0;
  if (kind) *kind = g_vp_kind[t];
  return g_vp_failed[t];
}
CORE_COLD uint32_t remote_vparam_gen() { return __atomic_load_n(&g_vp_gen, __ATOMIC_ACQUIRE); }

CORE_COLD int remote_track_src14(int t) { return t >= 0 && t < 12 ? s.src14[t] : -1; }
CORE_COLD int remote_track_src14_fresh(int t) {
  if (t < 0 || t >= 12 || s.src14[t] < 0) return -1;
  // a voice shared with another track of ours on the same machine: its report may be that track's, src14 is this one's
  for (int u = 0; u < 12; ++u)
    if (u != t && have_tracks && kTrackVoice[u] == kTrackVoice[t] && track_vb[u] != kNotOurs && track_vb[t] != kNotOurs &&
        machine_of(track_vb[u]) == machine_of(track_vb[t]))
      return s.src14[t];
  return s.src14_at[t] > s.rep_at[kTrackVoice[t]] ? s.src14[t] : -1;
}
CORE_COLD int remote_track_src_word(int t, int k) {
  return t >= 0 && t < 12 && k >= 0 && k < 16 && (s.src_known[t] >> k & 1) ? s.srcv[t][k] << 7 | s.srcl[t][k] : -1;
}
CORE_COLD bool remote_track_names_p13(int t) { return t >= 0 && t < 12 && (s.p13_last >> t & 1); }
CORE_COLD int remote_track_src_value(int t, int k) {
  return t >= 0 && t < 12 && k >= 0 && k < 16 && (s.src_known[t] >> k & 1) ? s.srcv[t][k] : -1;
}
static bool g_rytm_040 = false;
CORE_COLD void remote_rytm_040(bool on) { g_rytm_040 = on; }
CORE_COLD bool remote_rytm_040() { return g_rytm_040; }
CORE_COLD int remote_track_src_param(int t) {
  return t >= 0 && t < 12 && have_tracks && track_vb[t] != kNotOurs && (track_vb[t] & 0x78) == kDspSample ? 10 : 0;
}
CORE_COLD bool remote_track_src_list(int t, int k) {
  if (t < 0 || t >= 12 || !have_tracks || track_vb[t] == kNotOurs) return false;
  const int m = track_named(t);
  if (k == 0) return m == kGranMachine || m == kWaveMachine || m == kMultiMachine || m == kChopMachine || m == kJuno || m == kFm;
  return (k == 10 && sample_machine(m)) || (k == 12 && m == kWaveMachine);
}
CORE_COLD int remote_track_param(int t, int k) {
  if (t < 0 || t >= 12) return 0;
  tsrc.t = t;
  return tsrc.param(k & 15);
}
CORE_COLD const Page* remote_track_page(int t) { return t >= 0 && t < 12 ? track_page(t) : nullptr; }

CORE_COLD bool remote_key_repeating() { return s.rep_code >= 0; }

CORE_COLD void remote_reset() {
  s = State();
  g_fill_claimed = g_fill_keys_off = g_nohello_told = false;   // (tests: the claim statics too)
  g_going_down = false;
  for (auto& w : g_vp) w = VParam();
  for (int t = 0; t < 12; ++t) g_vp_failed[t] = 0, g_vp_kind[t] = 0;
  detent_reset();
  memset(reported, 0, sizeof reported);
  memset(track_smpl, 0, sizeof track_smpl);
  have_tracks = false, tracks_known = 0;
  src_capable = false;
  want_title = nullptr;
  disp.req_gen = 0, disp.req_page = 0, disp.sel_gen = 0, disp.sel = -1, disp.machine = -1, disp.info_gen = 0;
  disp.fx = false, disp.lfo_gen = 0;
  view.invalidate();
  work.clear();
  sent.clear();
}

}  // namespace ui
}  // namespace core
