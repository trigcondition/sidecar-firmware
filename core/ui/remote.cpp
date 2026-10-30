// core/ui/remote.cpp - see remote.h. Cold (program flash on the Teensy): a tick draws only while the page is open.
#include "md/md.h"   // CORE_MD
#include "ui/remote.h"

#include <string.h>

#include "ui/extra.h"
#include "ui/page_keys.h"
#include "lib/srclib.h"

#include "fx/send_fx.h"
#include "fx/insert.h"
#include "hooks.h"
#include "keys.h"
#include "lfo.h"
#include "machines.h"
#include "poly.h"
#include "pool/pool.h"
#include "seq/seq.h"
#include "song/song.h"
#include "version.h"
#include "voice_name.h"
#include "voices.h"
#if CORE_MSAMP
#include "granular/granular.h"
#include "msamp/msamp.h"
#include "msamp/sdtest.h"
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
// would change under the page), plus the encoders it uses and their pushes. TRK too: a second TRK within ~1 s opens a
// stock screen, which leaving by TRK + YES soon after a TRK on the page would do.
constexpr uint8_t kCatchKeys[] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x84,   // LEFT RIGHT UP DOWN YES NO PAGE
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35,         // TRIG SRC SMPL FLTR AMP LFO (the stock pages)
    0x10, 0xB0, 0xB4, 0xB5, 0xB1, 0x81, 0x82,   // FX GLOBAL SAMPLER PLAYMODE QPER SONG CHAIN
    0x86,                                       // TRK
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
// The voice each track plays on, as 0021's 03 / 04 carry it (the firmware's voice -> track owner table at 0x40290dd8,
// 00 04 01 05 08 06 0a 02 on rui1026's base [measured, rytm-emu dtpeek], and the Rytm's live 04s on the Pi [measured
// 2026-10-19: SD on JUNO trigs voice 2, CH on 6-OP FM voice 4, MT / HT voice 5]); a shared voice's second track (CP OH
// HT CB) takes it over at its trig. 0026's 33 puts its own table's voice in vb (BD 0, SD 1, RS/CP 2 ...), right only
// for BD and MT / HT, so its voice bits are replaced by this one's and only its machine flag is kept.
UI_ROM const uint8_t kTrackVoice[12] = {0, 2, 7, 7, 1, 3, 5, 5, 4, 4, 6, 6};
// 0x40 is MD (machines.h kMd): ours where the build has it (CORE_MD); on a build without, not ours (as analog)
CORE_COLD uint8_t track_vb_of(int t, uint8_t vb) {
  vb &= 0x7F;
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
CORE_COLD int machine_of_voice(int vi) {
  vi &= 7;
  if (have_tracks) {
    for (int t = 0; t < 12; ++t)   // the first track on this voice with a Sidecar machine (CP after RS, OH after CH...)
      if (track_vb[t] != kNotOurs && (track_vb[t] & 7) == vi) return track_vb[t] & 0x78;
    return kNoMachine;
  }
  return reported[vi] && (CORE_MD || !v[vi].md) ? machine_of_state(vi) : kNoMachine;   // (MD without CORE_MD: none)
}
CORE_COLD const char* short_name(int m) { return m == kNoMachine ? "-" : machine_short(m); }   // (core/machines.h)

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
    for (int n = 0; n < kVoices; ++n) {
      int vi = kShownOrder[n];
      int x = 32 * (n & 3), y = 13 + 11 * (n >> 2);
      c.fill(Rect{(int16_t)x, (int16_t)(y - 1), 10, 7}, kOn);
      sniprintf(b, sizeof b, "%.2s", voice_name(vi));
      c.put_text(kSlotFree + 2 * n, kRSmall, x + 1, y, b, kTextInvert);
      c.put_text(kSlotFree + 2 * n + 1, kRSmall, x + 21, y, short_name(machine_of_voice(vi)), kAlignCentre);
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
constexpr uint8_t kPagePoly = 0x20;  // rui1048: 0x20 + t (32..43) POLY CONFIG of track t (the Rytm's track menu, FUNC + CHRO; page_poly.cpp); an older host reads it as the SIDECAR page
constexpr int kExtra0 = -10;   // s.voice of extra page k (extra.h: SCOPE, REC): kExtra0 - k
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
  // rui1051 (the user's report on rui1050: NO on a page opened by its shortcut went to SIDECAR, a second NO out): where
  // the pages shown by a shortcut came from, newest last - kBackStock the stock screen (NO closes the page), else the
  // page (s.voice) it was opened over; the MENU counts as SIDECAR (a page opened from the MENU goes back to SIDECAR, as
  // before). LEFT / RIGHT and the page closing forget it (NO from a page reached by the ring goes to SIDECAR, as before).
  int back[2] = {};
  int nback = 0;
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
  int touched = -1;          // the encoder turned last (0..7): its cell's tiles go first, the bottom bar names it
  // the local echo (3F) as the Rytm has it: per encoder the slot (kEchoNone: none), the value, flags and prefix
  uint8_t echo_slot[8], echo_val[8], echo_flags[8];
  char echo_pre[8][12];
  bool echo_valid = false;
  bool legacy = false;       // the 1020 behaviour (tests: before / after): 50 ms frames, no echo, one commit
  uint8_t mask[kMaskBytes] = {};   // 1022: the catch mask the Rytm has (37), valid after the first since opening
  bool mask_valid = false;
  bool splash = false;       // 1023: the connect splash runs (the page closed)
  uint32_t now_ms = 0, open_ms = 0;   // the last remote_tick's clock; when the page was last opened (30 on)
  bool caps36 = false;       // the Rytm's hello said it understands 36 (the splash, the popups)
  int caps = 0;              // fix1049: the hello's caps byte whole (bit 1: 16 send FX label tables, rui1049)
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
  uint8_t srcl[12][16] = {};      // rui1038: the 2A's fraction bytes (P1 of GRANULAR / WAVETABLE / MULTISAMPLE: bank << 4)
  int16_t src14[12] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   // P1's last 14 bits by 2A or our 48 (song)
  uint16_t src_known[12] = {};
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
  for (int x = c0 * 8; x < (c0 + nc) * 8; ++x) raw[nr++] = work.fb[p + 8 * x];
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
  const TextItem& t = work.txt[i];
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
// ux1027 (review 5): the page shown goes from `from` to `to`: leaving the LFO page drops its design view (it opens on
// the main view again, T1..T16 not caught)
CORE_COLD void lfo_left(int from, int to) {
  const ExtraPage* x = extra_of(from);
  if (from != to && x && !strcmp(x->title, "LFO")) lfo_page_reset();
}

constexpr int kBackStock = -100;
CORE_COLD void back_push(int from) {   // a page shown by a shortcut over `from` (kBackStock: from the stock screen)
  const ExtraPage* x = extra_of(from);
  if (x && !strcmp(x->title, "MENU")) from = -1;
  if (s.nback && s.back[s.nback - 1] == from) return;
  if (s.nback == 2) s.back[0] = s.back[1], s.nback = 1;
  s.back[s.nback++] = from;
}

// The pages in order (the title bar's dots): SIDECAR, SYSTEM, the voices with a page, the extra pages shown.
CORE_COLD int page_ids(int* ids) {
  int n = 0;
  ids[n++] = -1, ids[n++] = -2;
  for (int vi = 0; vi < kVoices && !src_capable; ++vi)
    if (page_for_voice(machine_of_voice(vi), vi)) ids[n++] = vi;
  for (int k = 0; k < extra_pages() && n < kMaxIds; ++k) {
    // R3: MENU / KEYS only while shown; rui1051: the pages only their keys open too (page_keys.h sc_shortcut_only)
    if ((extra_page(k).off_ring || sc_shortcut_only(extra_page(k).title)) && s.voice != kExtra0 - k) continue;
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
  s.nback = 0;   // rui1051: the ring (or SIDECAR) forgets where a shortcut came from
  if (nv != s.voice) lfo_left(s.voice, nv), s.voice = nv, s.page_changed = true, s.touched = -1, s.via = -1;
}

// rui1051: NO back to where the shown page was opened from (true: done)
CORE_COLD bool go_back() {
  while (s.nback) {
    int b = s.back[--s.nback];
    if (b == s.voice) continue;
    if (b == kBackStock) { s.nback = 0, s.via = -1; remote_request(false); return true; }   // the stock screen: the page closes (3A 0 0)
    const ExtraPage* x = extra_of(b);
    if (x && x->shown && !x->shown()) continue;   // (gone from the list meanwhile)
    if (b >= 0 && (src_capable || !page_for_voice(machine_of_voice(b), b))) continue;
    lfo_left(s.voice, b), s.voice = b, s.page_changed = true, s.touched = -1, s.via = -1;
    return true;
  }
  return false;
}

CORE_COLD void set_param(int vi, int k, int nv) {
  V& x = v[vi];
  int m = machine_of_voice(vi);
  if (s.held_m[vi] != m) s.held[vi] = 0, s.held_m[vi] = (uint8_t)m;
  if (!(s.held[vi] & (1u << k))) s.rytm_v[vi][k] = (uint8_t)(int)lfo::base_of(vi, k, x.p[k]);   // the Rytm's value until now
  s.held[vi] |= (uint16_t)(1u << k);
  s.held_v[vi][k] = (uint8_t)nv;
  // a 0x03 PARAMS for the voice, its machine and every P (the edited one whole, the others as they are)
  uint8_t r[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)(m | vi)};
  int n = 6;
  for (int i = 0; i < 16; ++i) {
    float p = i == k ? (float)nv : lfo::base_of(vi, i, x.p[i]);   // an LFO's target: its base, not the modulated value
    int iv = (int)p;
    iv = iv < 0 ? 0 : iv > 127 ? 127 : iv;
    int fr = (int)((p - iv) * 128.0f);
    r[n++] = (uint8_t)iv;
    r[n++] = (uint8_t)(fr < 0 ? 0 : fr > 127 ? 127 : fr);
  }
  r[n++] = 0xF7;
  s.self = true;
  voice_report(0x03, r, n);
  s.self = false;
  s.stats.edits++;
}

CORE_COLD void on_enc(int e, int delta, bool func) {
  s.input = true;
  if (const ExtraPage* x = extra_of(s.voice)) {
    if (e >= 0 && e < 8 && (extra_encs(*x) >> e & 1)) x->enc(e, delta, func);
    return;
  }
  if (s.voice < 0 || src_capable || e < 0 || e > 7) return;
  s.touched = e;
  const Page* p = page_for_voice(machine_of_voice(s.voice), s.voice);
  if (!p || p->enc[e] == kNone) return;
  int k = p->enc[e] & 15, cur = (int)lfo::base_of(s.voice, k, v[s.voice].p[k]);
  int nv = step_value(*p, k, cur < 0 ? 0 : cur > 127 ? 127 : cur, delta, func);
  if (nv != cur) set_param(s.voice, k, nv);
}

CORE_COLD void on_key(int code, bool down) {
  if (!down) return;
  s.input = true;
  if (const ExtraPage* x = extra_of(s.voice))
    if (x->key && x->key(code)) return;
  if (code == kKeyUp) return;   // fix1049: UP is the Sidecar's modifier (page_keys.h), no page arrow: LEFT / RIGHT step
  if (standalone(extra_of(s.voice)) && (code == kKeyLeft || code == kKeyRight || code == kKeyDown || code == kKeyPage))
    return;   // ux1027: no siblings
  if (code == kKeyLeft) go(-1);
  else if (code == kKeyRight || code == kKeyDown || code == kKeyPage) go(+1);
  else if (code == kKeyNo && go_back()) {}                           // rui1051: back where a shortcut came from
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
    sniprintf(b, sizeof b, CORE_STR("%s %s:%+d"), name, p.label[e], v - 64);
    footer(work, b, v, true);
  } else {
    sniprintf(b, sizeof b, CORE_STR("%s %s:%d"), name, p.label[e], v);
    footer(work, b, v);
  }
}

// 3F for every encoder whose binding the Rytm does not have yet: numeric parameters echo into the bottom bar's slot
// ("BD HRM:" + the value), lists and the SIDECAR page none.
CORE_COLD void send_echoes(const Page* p, int vi) {
  for (int e = 0; e < 8; ++e) {
    uint8_t slot = kEchoNone, val = 0, fl = 0;
    char pre[12] = "";
    int ci = -1;
    const Cell* c = p && !s.legacy && p->enc[e] != kNone ? cell_of(*p, p->enc[e], &ci) : nullptr;
    if (c && c->kind != kList && !src_value_name(*p, p->enc[e], vsrc)) {   // (a source's name: the frame shows it)
      slot = kSlotFoot;
      val = (uint8_t)v_param(vi, p->enc[e]);
      fl = 1 | (c->kind == kKnobBi || c->kind == kPan ? 2 : 0);   // FUNC x 8 as step_value; bipolar shown signed
      sniprintf(pre, sizeof pre, CORE_STR("%s %s:"), voice_label(vi), p->label[e]);
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
// U1 NOTE EDIT: data - the SEQ page on a poly track also takes DATA (LEVEL, encoder 8) and its push (0x78): rytm-mods
// 0026 forwards any encoder 0..15 the mask names as 32 <e> (design.md 1022, "LEVEL = 8" [measured]), no mod change
// ux1027: yes false - the SEQ page leaves YES to the Rytm (seq::yes_wanted: its YES combos, the quick saves); rytm-mods
// 0026 catches per key (key_owner), so YES is named only while the page wants it (a question, FUNC or a trig held)
// updown1029 (the user, 2026-10-29): updown false - the SEQ page leaves UP / DOWN to the Rytm (seq::updown_wanted: CHRO's
// octave on the pads), named only under a question or with FUNC held (FUNC + DOWN: the poly track's menu)
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
  uint8_t c[16];
  bool want = !s.legacy && x && x->leds_now && x->leds_now(c);
  if (want && (!s.leds_on || memcmp(c, s.leds, 16))) send_leds(c);
  else if (!want && s.leds_on) send_leds(nullptr);
}

// One commit: the tiles in `tiles` and the text slots in `texts` (from `work`), then 39. Returns the tiles sent.
CORE_COLD int send_part(const uint32_t tiles[4], uint64_t texts, bool clear_first) {
  int n = 0;
  s.seq = (uint8_t)((s.seq + 1) & 0x7F);
  if (clear_first) {
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
    p = page_for_voice(machine_of_voice(s.voice), s.voice);
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
  int drawn = 1;
  if (p) {
    char right[8];
    vsrc.vi = s.voice;
    sniprintf(right, sizeof right, "%s", voice_label(s.voice));
    drawn = view.draw(work, *p, vsrc, right, at, n);
    voice_footer(*p, voice_label(s.voice), vsrc, s.touched >= 0 ? s.touched : 0);
  } else if (x) {
    view.invalidate();
    x->draw(work, at, n, ms);
  } else {
    view.invalidate();   // the status pages are drawn whole; a voice page after them starts clean
    ssrc.draw(work, s.voice == -2 ? 1 : 0, at, n);
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
  if (!s.legacy) send_mask(p, x ? extra_encs(*x) : 0, x && x->trigs_now ? x->trigs_now() : 0, sq, sq && seq_data(),
                            !sq || seq_yes(), !sq || seq_updown());   // before the frame: the keys and knobs the page shows are its own from now on
  update_leds(x);
  if (!s.sent_valid) s.echo_valid = false;
  send_echoes(p, s.voice);
  uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint64_t texts = 0;
  int tiles = s.sent_valid ? work.diff_tiles(sent, mask) : kTiles;
#if CORE_UI_TOOLS
  work.check_overlap();
#endif
  int ntext = s.sent_valid ? work.diff_text(sent, &texts) : 0;
  if (!s.sent_valid)
    for (int i = 0; i < kTextSlots; ++i)
      if (work.txt[i].s[0]) texts |= 1ull << i, ++ntext;
  if (tiles || ntext) {
    // the touched knob's cell and the bottom bar first, in a commit of their own, when there is more
    uint32_t first[4] = {}, rest[4];
    uint64_t tfirst = 0;
    int ci = -1;
    if (!s.legacy && s.sent_valid && p && s.touched >= 0 && p->enc[s.touched] != kNone &&
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
    sent = work;
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
  if (have_tracks) return track_vb[t] == kNotOurs ? kNoMachine : track_vb[t] & 0x78;
  return machine_of_voice(kTrackVoice[t]);
}
CORE_COLD const Page* track_page(int t) {   // DSP SAMPLE's GRAIN view by the Rytm's MOD (P12) once it came
  tsrc.t = t;   // test-1031: 6-OP FM / MULTISAMPLE with a chord on show it (the Rytm's CHD..VOI, poly::view)
  if (const Page* bp = src_bank_page(t, track_machine(t))) return bp;   // rui1038: the bank list open on this track
  return src_list_page(chord_view_page(src_view_page(page_for_voice(track_machine(t), kTrackVoice[t], s.src_known[t] >> 11 & 1 ? s.srcv[t][11] : -1)), tsrc), tsrc);
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
  wave_idle(ms);   // the sample overview of the Rytm's SMP being read (one column at most)
  if (!s.clock_set) s.clock_set = true, s.last_draw = ms - kTickMs, s.last_tx = ms;
  // a 2A at once (the Rytm sends one a 20 Hz tick at most; the 2As a USB read brought are taken before this tick, so a
  // burst is drawn once, with its latest values)
  if (s.input || ms - s.last_draw >= kTickMs) {
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
    int drawn = view.draw(work, *p, tsrc, right, 0, 0);
    int e = 0;
    for (int i = 0; i < 8; ++i)
      if (s.src_last >= 0 && p->enc[i] == s.src_last) e = i;
    voice_footer(*p, kTrackName[t], tsrc, e);
    s.stats.widgets += (uint32_t)drawn;
    uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint64_t texts = 0;
    int tiles = s.sent_valid ? work.diff_tiles(sent, mask) : kTiles;
#if CORE_UI_TOOLS
    work.check_overlap();
#endif
    if (s.sent_valid) work.diff_text(sent, &texts);
    else
      for (int i = 0; i < kTextSlots; ++i)
        if (work.txt[i].s[0]) texts |= 1ull << i;
    if (tiles || texts) {
      bool whole = !s.sent_valid;
      send_part(mask, texts, whole);
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
  sniprintf(b, sizeof b, CORE_STR("%d VOICES  %d FX"), kVoices, fx::kSlots);
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
  footer(c, f);
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
    return;
  }
  if (!first && ms - s.last_draw < (t < kSlideMs + kSlideStepMs ? kSlideStepMs : kSplashStepMs)) return;
  s.last_draw = ms;
  splash_draw(work, t);
  uint32_t mask[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  uint64_t texts = 0;
  int tiles = s.sent_valid ? work.diff_tiles(sent, mask) : kTiles;
  if (s.sent_valid) work.diff_text(sent, &texts);
  else
    for (int i = 0; i < kTextSlots; ++i)
      if (work.txt[i].s[0]) texts |= 1ull << i;
  if (tiles || texts) send_part(mask, texts, !s.sent_valid);
  else send_short(kAlive, nullptr, 0);
  s.stats.tiles += (uint32_t)tiles;
  sent = work;
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
    for (int i = 0; i < kTextSlots; ++i)
      if (work.txt[i].s[0]) texts |= 1ull << i;
    send_part(mask, texts, true);
    sent = work;
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
CORE_COLD bool extra_here(const char* title) {   // an ExtraPage of that title is in the list now (or none is: the status)
  for (int k = 0; k < extra_pages(); ++k)
    if (!strcmp(extra_page(k).title, title)) return !extra_page(k).shown || extra_page(k).shown();
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

}  // namespace

CORE_COLD void remote_sysex(const uint8_t* d, int len) {
  if (len < 7 || d[5] != 1 || d[len - 1] != 0xF7) { s.stats.bad++; return; }
  const uint8_t* b = d + 6;
  int n = len - 7;
  if (d[4] == kSrcView || d[4] == kSrcVals || d[4] == kSrcDiag) src_capable = true;
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
          if (!go_back()) go(0);   // back where it came from (the stock screen: the page asked closed), else SIDECAR
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
        if (!was_open || s.voice == -1) s.nback = 0;   // (SIDECAR: NO leaves, as before)
        if (was_open) lfo_left(was, s.voice);
        s.via = b[0], s.via_voice = s.voice;
        if (s.voice != -1 && !(was_open && was == s.voice))
          back_push(was_open ? was : kBackStock);   // rui1051: a shortcut's page: NO goes back there
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
        s.nback = 0, s.via = -1;
        keys_map_drop(), s.map_over = false;   // (UP's map goes with the page)
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
      if (!s.open && src_bank_key(b[0] << 7 | b[1], b[2] != 0)) {   // rui1038: the bank list's LEFT RIGHT UP DOWN
        s.input = true;
        return;
      }
      if (!s.open && s.src_track >= 0 && b[0] == 0 && b[1] >= 0x70 && b[1] <= 0x77) {   // rui1036: the SRC view's pushes
#if CORE_POLY && CORE_MSAMP
        if (chop_src_push(s.src_track, b[1], b[2] != 0)) {   // CHOP: a push of B..H opens the slice editor
          s.input = true;
          return;
        }
#endif
        if (b[1] == kKeyPushA) src_list_push(b[2] != 0);   // P1's: held - the list stays open; a click - the row acts
        s.input = true;
        return;
      }
#if CORE_POLY
      {   // the sequencer's trig keys and PAGE: in seq mode (the page open or not) and on its SEQ page
        const ExtraPage* x = s.open ? extra_of(s.voice) : nullptr;
        if (seq::ui_key(b[0] << 7 | b[1], b[2] != 0, x && !strcmp(x->title, "SEQ"))) {
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
      popup_key_closed();   // (a turn ends the Rytm's splash as a key does)
      if (!s.open && src_bank_enc(b[0], (int)b[1] - 64, b[2] != 0)) {   // rui1038: knob A in the bank list
        s.input = true;
        return;
      }
#if CORE_POLY
      {
        const ExtraPage* x = s.open ? extra_of(s.voice) : nullptr;
        // rui1048: POLY CONFIG's knobs are its own, also in seq mode (seq::ui_enc takes every turn then)
        if (!(x && !strcmp(x->title, "POLY")) && seq::ui_enc(b[0], (int)b[1] - 64, b[2] != 0, x && !strcmp(x->title, "SEQ"))) {
          s.input = true;
          return;
        }
      }
#endif
      if (s.open) on_enc(b[0], (int)b[1] - 64, b[2] != 0);
      return;
    case kMachines: {   // 1025: <t0> <n> {<machine 0..52> <vb | 7F>} x n
      if (n < 2 || b[0] >= 12 || b[1] < 1 || b[0] + b[1] > 12 || n != 2 + 2 * b[1]) break;
      if (!have_tracks) memset(track_vb, kNotOurs, sizeof track_vb);
      // One track turned JUNO / 6-OP FM (the SRC list, n 1): its PAT list (16 / 17) pushed now. The Rytm keeps one
      // list cache for both (and the U slots' lists); the 17 that follows every 16 at a project load evicts JUNO's,
      // and the Rytm asks again only once in 200 draws [measured, rytm-emu rui1025: ~38 s of factory names].
      if (b[1] == 1 && track_vb[b[0]] != track_vb_of(b[0], b[3])) {
        if ((b[3] & 0x78) == kJuno) send_juno_names();
        else if ((b[3] & 0x78) == kFm) send_fm_names();
      }
      for (int i = 0; i < b[1]; ++i)
        track_vb[b[0] + i] = track_vb_of(b[0] + i, b[3 + 2 * i]), tracks_known |= (uint16_t)(1u << (b[0] + i));
      have_tracks = true;
      s.stats.machines++;
      return;
    }
    case kBankList: {   // rui1038: <track> <on> <hi> <lo> <why>
      if (n < 5 || b[0] >= 12 || (b[1] > 1 && b[1] != 3)) break;   // rui1040: on 3 FUNC + YES (the files for the row)
      int t = b[0];
      src_bank_list(t, kTrackVoice[t], track_machine(t), srclib::code_of14(b[2] << 7 | b[3]), b[1], b[4]);
      s.input = true;
      return;
    }
    case kSrcView:   // 1027: <track> <on>
      if (n < 2 || b[0] >= 12 || b[1] > 1) break;
      s.stats.src_views++;
      if (src_log) core::log("rui src: 29 %lu ms, track %d, %s", (unsigned long)now_ms(), b[0], b[1] ? "on" : "off");
      if (b[1]) {
        s.src_track = (int8_t)b[0];
        src_list_quiet();
        s.src_told = -1;   // a whole frame at the next tick: the Rytm shows ours only after a 39 since this 29
        s.src_last = -1;   // a splash running (a hello just before) ends first, then the view's whole frame
      } else if (b[0] == s.src_track) {
        s.src_track = -1, s.src_told = -1;
        if (!s.open) s.sent_valid = false;   // the Rytm's own screen is back: its shadow is not ours
      }
      return;
    case kSrcDiag: {   // 1027d: logged when the state or the top view changes (or with srclog on)
      if (n < 8) break;
      static uint32_t last = 0xFFFFFFFFu;
      uint32_t vt = (uint32_t)b[2] << 28 | (uint32_t)b[3] << 21 | (uint32_t)b[4] << 14 | (uint32_t)b[5] << 7 | b[6];
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
      if (n < 5 || b[0] >= 12 || b[1] > 15 || b[2] < 1 || b[1] + b[2] > 16 || n != 3 + 2 * b[2]) break;
      int t = b[0];
      for (int i = 0; i < b[2]; ++i) {
        s.srcv[t][b[1] + i] = b[3 + 2 * i] & 0x7F;
        s.srcl[t][b[1] + i] = b[4 + 2 * i] & 0x7F;
        s.src_known[t] |= (uint16_t)(1u << (b[1] + i));
      }
      {   // rui1038: the banked knob's 14 bits (song): P1, or DSP SAMPLE's SMP P11 (rui1045)
        int kb = remote_track_src_param(t);
        if (b[1] <= kb && kb < b[1] + b[2]) s.src14[t] = (int16_t)((b[3 + 2 * (kb - b[1])] & 0x7F) << 7 | (b[4 + 2 * (kb - b[1])] & 0x7F));
      }
      if (b[2] == 1) s.src_last = (int8_t)b[1];
      else if (t == s.src_track) src_list_quiet();   // a kit's values at once (a load): no list for its SRC
      if (t == s.src_track) s.input = true;   // drawn at once (the input path), a burst coalesced to its latest values
      s.stats.src_vals++;
      if (src_log) core::log("rui src: 2A %lu ms, track %d, first %d, n %d, P%d = %d.%d", (unsigned long)now_ms(), t, b[1], b[2],
                       b[1] + 1, b[3], b[4]);
      return;
    }
    case kDisplay: {   // rui1036b
      if (n < 2) break;
      if (b[0] == 4 && n >= 2) {   // rui1044: UP held alone on the stock screen (1) / let go (0): the combo map
        keys_map(b[1] != 0);
        return;
      }
      if (b[0] == 5 && n >= 2) {   // rui1050: UP held alone over an open page (1): the map over it; let go (0): that page again
        if (b[1] && s.open && !s.map_over) {   // (a repeated 1: the page under it kept)
          s.map_back = s.voice, s.map_over = true;
          int nb = s.nback, bk[2] = {s.back[0], s.back[1]};
          keys_map_over(true);
          s.nback = nb, s.back[0] = bk[0], s.back[1] = bk[1];   // rui1051: the map is no page of its own (NO's way stays)
        } else if (!b[1] && s.map_over) {
          s.map_over = false;
          if (keys_map_over(false) && s.open && s.voice != s.map_back) s.voice = s.map_back, s.page_changed = true, s.input = true, s.via = -1;
        }
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
      if (b[0] == 2 && n >= 15 && b[1] < 12) {
        bool moved = disp.sel != b[1];
        for (int i = 0; i < 12; ++i) disp.name[i] = (char)(b[3 + i] >= 0x20 && b[3 + i] < 0x7F ? b[3 + i] : 0);
        disp.name[12] = 0;
        disp.machine = b[2], disp.sel = b[1];
        if (moved) disp.sel_gen = disp.sel_gen + 1;
#if CORE_POLY
        if (moved) seq::selected_track(b[1]), core::poly::selected_voice(remote_track_voice(b[1]));   // rui1040: the SEQ page
                                                  // follows the selection (or closes); the pads' binding too
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
      if (b[2]) core::log("rui: 48 write track %d P%d refused (status %d)", b[0] + 1, b[1] + 1, b[2]);
      return;
    case kKit:   // 1026: the Rytm's active kit: the LFOs are per kit (core/lfo.h)
      if (n < 1 || b[0] > 127) break;
      lfo::kit_changed(b[0]);
#if CORE_POLY
      ins::kit_changed(b[0]);   // the inserts are per kit too (core/fx/insert.h)
#endif
#if CORE_MSAMP
      gr::kit_changed(b[0]);   // GRANULAR's picks are per kit too
#endif
      return;
    case kHello: {   // 1023: <uptime >> 7> <uptime & 0x7F> <caps>: the splash when the Rytm was up before the link
      if (n < 3) break;
      s.stats.hellos++;
      int up = b[0] << 7 | b[1];
      // A new link (a replug, the Rytm's or our restart): whatever the Rytm showed before is gone - its page left by its
      // 1 s timeout, its SRC view says 29 t 1 again after this hello if it is on. Stale state here (the page open, a
      // view's track, a splash running when the cable went) blocked the splash and the view until a restart.
      bool was_open = s.open, was_view = s.src_track >= 0, was_splash = s.splash;
      if (s.open && s.now_ms - s.open_ms <= kHelloOpenMs) {
        // The user opened the page just before this (first) hello: it is theirs and the Rytm still shows it - no splash,
        // no close (a splash over it would be ignored by the Rytm, which holds the page, while we stopped serving it).
        // What the hello invalidates on the Rytm's side is sent again, as after an open.
        s.sent_valid = false, s.mask_valid = false, s.echo_valid = false, s.leds_on = false, s.clock_set = false;
        s.caps36 = (b[2] & 1) != 0, s.caps = b[2];
        core::log("rui: hello, Rytm up %d.%d s, caps %d, page opened %d ms ago: kept", up / 20, (up % 20) / 2, b[2],
                  (int)(s.now_ms - s.open_ms));
        src_names_resend();
        return;
      }
      if (s.open) s.stats.closes++;
      s.open = false, s.leds_on = false, s.mask_valid = false, s.echo_valid = false, s.sent_valid = false;
      s.src_track = -1, s.src_told = -1, s.splash = false, s.clock_set = false;
      tap_enable(false);
      s.caps36 = (b[2] & 1) != 0, s.caps = b[2];
      keys_map_drop(), s.map_over = false;   // (a release lost with the link: no map left up)
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

CORE_COLD void remote_tick(uint32_t ms) {
  s.now_ms = ms;
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

CORE_COLD void remote_request(bool on) {
  uint8_t b[2] = {0, (uint8_t)(on ? 1 : 0)};
  send_short(kOpen, b, 2);
}

CORE_COLD bool remote_open() { return s.open; }
CORE_COLD int remote_caps() { return s.caps; }

CORE_COLD void remote_show_page(const char* title) {
  if (!strcmp(title, "SYSTEM")) {   // (MENU's SYSTEM: the second status page)
    if (s.open && s.voice != -2) back_push(s.voice), lfo_left(s.voice, -2), s.voice = -2, s.page_changed = true, s.input = true, s.via = -1;
    return;
  }
  if (!strcmp(title, kStatusTitle)) {   // the status page (SIDECAR, the first): the page opens on it
    if (s.open && s.voice != -1) back_push(s.voice), lfo_left(s.voice, -1), s.voice = -1, s.page_changed = true, s.input = true, s.via = -1;
    else if (!s.open) want_title = nullptr, remote_request(true);
    return;
  }
  if (s.open) {
    for (int k = 0; k < extra_pages(); ++k)
      if (!strcmp(extra_page(k).title, title) && s.voice != kExtra0 - k) {
        back_push(s.voice);   // rui1051: NO goes back to the page it was opened over
        lfo_left(s.voice, kExtra0 - k), s.via = -1;
        s.voice = kExtra0 - k, s.page_changed = true, s.input = true;
      }
    return;
  }
  want_title = title;
  remote_request(true);
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
CORE_COLD uint32_t remote_status_popups() { return s.status_popups; }
CORE_COLD void remote_legacy(bool on) { s.legacy = on; }
CORE_COLD void remote_src_log(bool on) { src_log = on; }

CORE_COLD void remote_params(int vi, int m, float* p, int np) {
  vi &= 7;
  reported[vi] = true;
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
    return src_list_page(page_for_voice(machine_of_state(voice & 7), voice & 7), vsrc);
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
CORE_COLD uint32_t remote_track_info(int* track, int* machine, char name[13], uint8_t val[8], char label[8][5]) {
  if (track) *track = disp.sel;
  if (machine) *machine = disp.machine;
  if (name) memcpy(name, disp.name, 13);
  if (val) memcpy(val, disp.val, 8);
  if (label) memcpy(label, disp.label, sizeof disp.label);
  return disp.info_gen;
}
CORE_COLD int remote_param_write(int vi, int machine, int k, int value, int lo) {
  int n = 0;
  for (int t = 0; t < 12; ++t) {
    if (kTrackVoice[t] != (vi & 7) || !have_tracks || track_vb[t] == kNotOurs || (track_vb[t] & 0x78) != machine) continue;
    uint8_t b[4] = {(uint8_t)t, (uint8_t)(k & 15), (uint8_t)(value < 0 ? 0 : value > 127 ? 127 : value), (uint8_t)(lo & 0x7f)};
    send_short(kWrite, b, lo > 0 ? 4 : 3);   // rui1038: the fraction byte (a bank) only when there is one: older builds too
    if (k == remote_track_src_param(t)) s.src14[t] = (int16_t)(b[2] << 7 | b[3]);
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
  return true;
}
CORE_COLD int remote_track_src14(int t) { return t >= 0 && t < 12 ? s.src14[t] : -1; }
CORE_COLD int remote_track_src_param(int t) {
  return t >= 0 && t < 12 && have_tracks && track_vb[t] != kNotOurs && (track_vb[t] & 0x78) == kDspSample ? 10 : 0;
}
CORE_COLD int remote_track_param(int t, int k) {
  if (t < 0 || t >= 12) return 0;
  tsrc.t = t;
  return tsrc.param(k & 15);
}
CORE_COLD const Page* remote_track_page(int t) { return t >= 0 && t < 12 ? track_page(t) : nullptr; }

CORE_COLD void remote_reset() {
  s = State();
  memset(reported, 0, sizeof reported);
  have_tracks = false, tracks_known = 0;
  src_capable = false;
  want_title = nullptr;
  disp.req_gen = 0, disp.req_page = 0, disp.sel_gen = 0, disp.sel = -1, disp.machine = -1, disp.info_gen = 0;
  view.invalidate();
  work.clear();
  sent.clear();
}

}  // namespace ui
}  // namespace core
