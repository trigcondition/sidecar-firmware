// core/ui/extra.h - the remote UI's pages drawn by code rather than declared (page.h): SCOPE (core/ui/page_scope.cpp)
// and REC (page_rec.cpp), after the voice pages in the page order (remote.cpp: SIDECAR, SYSTEM, the voice pages, then
// these; LEFT on SIDECAR wraps to the last). Each draws its whole page every frame; the frame diff sends only the
// tiles that moved. And the Overbridge input tap they read: the Rytm's 12 IN channels as per-entry min / max of the
// selected source and per-channel peaks, written by the IN path (compares and stores only), reduced and drawn here on
// the control side (core/ui/README.md, "Live pages").
#pragma once
#include <stdint.h>

#include "fx/send_fx.h"   // RYTM_SENDFX_LARGE: 0 on the Teensy build
#include "ui/page.h"
#include "ui/tap_names.h"   // tap_source_name, tap_channel_of_voice (on their own for the Pi's rec_pi.cpp)
#include "ui/ui.h"

namespace core {
namespace ui {

struct ExtraPage {
  const char* title;
  bool (*shown)();                                              // in the page list now (null: always)
  void (*draw)(Canvas& c, int page, int npages, uint32_t ms);  // the whole page (header, body, bottom bar)
  void (*enc)(int e, int delta, bool func);                    // a turn of one of its encoders
  bool (*key)(int code);                                       // a key pressed; true: the page's own, not navigation
  uint8_t encs;                                                // the encoders it takes (bit e: A = bit 0)
  bool tap;                                                    // it reads the input tap (tap_in runs while it shows)
  uint8_t (*encs_now)();                                       // the encoders it takes now, when they vary (null: encs)
  uint16_t (*trigs_now)();                                     // the trig keys T1..T16 it takes now (bit k: T(k+1); null none)
  // 1026: the trig keys' LEDs while it shows (host -> Rytm 34, rui1026): true and c[k] for T(k+1) (palette colours,
  // kLed*), or false: stock (null: never)
  bool (*leds_now)(uint8_t c[16]);
  bool off_ring;   // UX review R3: not on the LEFT / RIGHT ring (in the page list only while it is the page shown)
  // 0.5.0 (spec-050 2.6): the release of a key whose press `key` took (true), to the page that took it, also after the
  // page changed; a hello or the page closing releases every key still held (the Rytm's releases are lost with the
  // link). Null: releases dropped, as before (aggregate initialisation leaves it null on the other pages)
  bool (*key_up)(int code);
  // 0.5.0 (spec-050 2.6, EDIT): the modulation target (core/mod/target.h Tid) encoder e edits now, 0 none: with a scene
  // key held and the scenes' EDIT on, a turn of it locks that target into the scene instead of editing it. Null: the
  // page locks nothing (its trig keys stay its own)
  uint16_t (*enc_tid)(int e);
  // updown044 (0.4.4, the owner: the Sidecar takes UP / DOWN only on a page that uses them): a page that stands alone
  // (off the ring, or only its keys open it: remote.cpp standalone) catches UP / DOWN only when this is set (its rows,
  // its banks); else they are the Rytm's (CHRO's octave on the pads). A ring page always takes them (DOWN: its next
  // page); the SEQ page decides per moment (seq::updown_wanted)
  bool updown;
};
#if CORE_UI_TOOLS
void extra_test_page(const ExtraPage* p);   // tests: one more page at the end of the order (null: none)
#endif
// UX review R3 / N3 (page_menu.cpp): MENU (YES on SIDECAR: every page with its shortcut) and KEYS (every combo; also
// while UP is held alone on the stock screen: rui1044's 49 1 4)
extern const ExtraPage kMenuPage, kKeysPage, kWebUiPage, kUsbPage;   // WEB UI, USB-C: the MENU's rows (remote_webui, remote_usb)
void menu_open();          // MENU shown (the page opened when closed)
void keys_map(bool on);    // UP held alone (on: KEYS shown, T1..T10 lit) / let go (off: the page closes again)
bool keys_map_on();        // the map shows (the touchscreen shows it too)
bool keys_map_over(bool on);   // rui1050: UP held alone over an open page (on: KEYS as its map); off: true when it showed
void lfo_page_reset();   // ux1027: the LFO page back on its main view (the page left)
// USER SLOTS (page_slots.cpp): U1..U5's machines (remote.h RemoteSlots), on the ring after SCOPE / AUTO and in the MENU
extern const ExtraPage kSlotsPage;
void slots_page_reset();   // tests: row U1, no pick, no note
int slots_page_row();      // tests: the row (0..4)
int slots_page_pick();     // tests: the pick (-1 none, 0 "---", 1.. the installed entry + 1)
void keys_map_drop();      // a hello or the page closed: the map (and its touchscreen overlay) gone
void menu_reset();         // tests
int menu_rows();                     // rui1051 (tests): the MENU's rows and the page title of row i (null: KEYS)
const char* menu_row_page(int i);
// 34's palette (rytm-mods 0026 rui1026): 0..111, ramps of 16 (red 0, green 16, blue 32, amber 48, cyan 64, magenta 80,
// white 96; each ramp's entry 0 is off). The REC page: recorded blue (47), not recorded orange (52), unused off.
enum : uint8_t { kLedOff = 0, kLedBlueDim = 43, kLedBlue = 47, kLedOrange = 52, kLedA = kLedBlue, kLedB = kLedOrange };
inline uint8_t extra_encs(const ExtraPage& x) { return x.encs_now ? x.encs_now() : x.encs; }
int extra_pages();                    // how many there are (shown or not)
const ExtraPage& extra_page(int i);   // in the page order
bool extra_for_box(const char* title);   // this page means something on the box in use (the Rytm: all do): device.h Feature

// ---- the Overbridge input tap. Channels as the Rytm sends them (ob_block.h): 0 / 1 MAIN L / R, 2 + t track t's voice
// (BD SD RS BT LT MT CH CY), 10 / 11 AUDIO IN L / R; int32 full scale.
constexpr int kTapCh = 12;
constexpr int kTapFrames = 8;   // frames an entry spans (6 kHz of min / max pairs)
#if RYTM_SENDFX_LARGE
constexpr int kTapRing = 2048;  // entries (8 KB: 341 ms)
#else
constexpr int kTapRing = 512;   // the Teensy: 2 KB of RAM2 (85 ms: more than a scope update's worth)
#endif
// The IN path (the Pi's IN completion on core 0, the Teensy's USB host interrupt): n frames, channel c of frame i at
// f[i * stride + c] (stride >= kTapCh). Nothing while no page reads it.
extern volatile bool tap_enabled;
inline bool tap_on() { return tap_enabled; }
void tap_in(const int32_t* f, int n, int stride);
void tap_enable(bool on);
// The scope's source: 0 MAIN (L + R) / 2, 1..8 Sidecar voice 1..8 (its track's channel), 9 / 10 AUDIO IN L / R.
constexpr int kTapSources = 11;
void tap_source(int src);
// tap_source_name, tap_channel_of_voice: ui/tap_names.h
// The peaks since the last call, |x| >> 16 (0..32767), channel order; the entries since *rd as (min, max) >> 16, at most
// max, oldest first (an overrun skips to the oldest still held); returns how many.
void tap_take_peaks(uint16_t out[kTapCh]);
int tap_read(uint32_t* rd, int16_t (*out)[2], int max);
uint32_t tap_write_pos();
void tap_reset();   // tests

// ---- meters (SCOPE and REC): 12 vertical bars (MAIN L R, voices 1..8, IN L R) in rows y..y+h-1, peak hold, labels
// under them in text slots from slot0 (4 used). Levels in dB x 2 below full scale (0 = 0 dBFS, kMeterFloor = none).
constexpr int kMeterFloor = 96;   // -48 dB
struct Meters {
  uint8_t level[kTapCh], hold[kTapCh];   // dB x 2 below full scale (smaller = louder)
  uint32_t hold_ms[kTapCh], last_ms;
  Meters() { reset(); }
  void reset();
  void update(const uint16_t peaks[kTapCh], uint32_t ms);   // a new peak set: rise at once, fall 24 dB/s; hold 1 s
  void draw(Canvas& c, int y, int h, int slot0) const;
  static int x(int i);   // meter i's left column (0..11: MAIN L R, voices 1..8, IN L R)
  bool main_right = false;   // 1026 (REC): MAIN drawn between voice 8 and IN, under T9
  int pos(int i) const;      // meter i's left column in this layout
};
int db2_below(uint32_t peak);   // |x| >> 16 -> dB x 2 below full scale, 0..kMeterFloor

// ---- the recorder (REC, page_rec.cpp): its state from the platform (the Pi's rec_pi.cpp, core 0, every 200 ms and
// after each request; any core), the page's requests to it (the platform takes them on the core that controls it)
enum RecState : uint8_t { kRecNone, kRecIdle, kRecArmed, kRecRecording, kRecFinishing };
enum RecRequest : int { kRecNoRequest, kRecStart, kRecStop, kRecArm, kRecDisarm };
struct RecInfo {
  uint8_t state;           // RecState (kRecNone: no recorder - the page is not shown)
  uint32_t secs;           // the take's length (this one, or the last)
  uint32_t free_mb, total_mb, need_kbps;   // the card; the selection's data rate
  uint16_t in_mask;        // the IN channels the take records (bit c: channel c, the tap's order)
  char take[24];           // the take's folder name ("" none yet)
  char error[32];          // the recorder's last error ("" none)
  bool usb_paused;         // the platform's USB audio paused for the take (the Mini's shared memory: "USB OFF")
  bool ch_select;          // 1026: the trig keys pick the take's channels (the Pi; the Mini not until stems with PSRAM)
};
void remote_rec(const RecInfo& i);
int remote_rec_take();     // the pending request (RecRequest), taken; kRecNoRequest none
// 1026: the trig keys pressed on the REC page since the last call (bit k: T(k+1); T1..T8 the tracks' channels (the
// tap's 2..9), T9 MAIN (the master L/R), T10 / T11 IN L / R (the FX return when it is on)); the platform toggles them
// in its channel selection (not while a take records: the page ignores them then). 0: none.
uint16_t remote_rec_toggles();
// Playback on the REC page (the Pi's rec_play.h): a take's channel into the Rytm's MAIN. The platform's state (as
// RecInfo's, any core), the page's input taken by the platform: B pushed (play / stop), B turned (the channel),
// C turned (the take, 0 = the newest), D turned (the level), summed since the last take.
enum PlayState : uint8_t { kPlayNone, kPlayStopped, kPlayPlaying, kPlayStopping };   // kPlayNone: no take to play
struct PlayInfo {
  uint8_t state;           // PlayState
  uint8_t level;           // 0..127
  uint8_t ch_i, channels, take_i, takes;
  uint32_t pos_ms, len_ms; // the playhead, the channel's length
  char take[24], channel[12];
  char error[32];          // "" none
};
struct PlayInput { int toggle, channel, take, level; };
void remote_play(const PlayInfo& i);   // the first call: a player exists (the page takes B / C / D from then on)
bool remote_play_take(PlayInput* in);   // false: nothing since the last call
extern const ExtraPage kRecPage;
// ---- the LFO page (page_lfo.cpp, core/lfo.h): first of the pages drawn by code (LFO, REC, SCOPE)
extern const ExtraPage kLfoPage;
// ---- the MORPH page (page_morph.cpp, core/mod/scene.h, spec-050 2.6): the scenes, A / B, the fader, a scene's locks
extern const ExtraPage kMorphPage;
// 0.5.0 (the owner): MIDI (page_midi.cpp, CORE_POLY), the SIDECAR MENU's row: the keyboards' route, auto channel and
// computer notes (core/keys.h), the MIDI-learn routes (core/mod/learn.h), the USB-A devices and the last key's way
extern const ExtraPage kMidiPage;
void midi_page_open();    // shown (the MENU's YES on its row)
void midi_page_reset();   // tests
// 0.5.0 FILL (page_fill.cpp, core/seq/lockfill.h, notes/design sampler-lock-fill): p-locks the selected track's trigs
// with a range of its machine's values (samples, engines, presets) or VELOCITY. Opened by UP + RIGHT (remote.h
// kPageFill: the 0.5.0 0026) and by the SIDECAR MENU's FILL row (older mods); off the LEFT / RIGHT ring (CORE_POLY).
extern const ExtraPage kFillPage;
void fill_page_open(int track);   // track -1: the Rytm's selected track
void fill_page_reset();           // tests
// The FILL page's state for the tests: its track, target, FROM / TO (indexes), options, the last note
struct FillView { bool on; int track, target, ntargets, from, to, order, trigs, every, offset, seed; bool undo; const char* note; };
FillView fill_page_view();
// the console's "fill": "fill" (state), "fill=yes|clear|undo", "fill=playing:on|off" (E4: writes while playing allowed)
bool fill_command(const char* p, char* out, int cap);
// page_wavegrain.cpp: the last quick-load batch (RIGHT in a list, one opening) of track t in list kind (0 samples,
// 1 wavetables, 2 instruments): its first and last code; false none
bool fill_batch(int t, int kind, int* first, int* last);
void morph_page_reset();   // the page's own state (holds, the question) cleared (tests)
int morph_page_held();     // the scene key held on it (0..11), -1 none
void morph_value_text(uint16_t tid, uint16_t v, char* b, int cap);   // a lock's value (x 128) as the page shows it
#if CORE_POLY
extern const ExtraPage kSeqPage;   // the Sidecar sequencer's page (page_seq.cpp, core/seq/seq.h)
// fix1028: NOTE EDIT's roll (page_seq.cpp) gives each pitch its lane and, when the page has more pitches than lanes
// fit, shows a window of them that follows the highlighted note. ne_window: the first lane shown (of n, lane 0 the
// lowest pitch) for `visible` lanes, from w0 (the frame before) - the least move that puts lane `focus` inside with
// `margin` lanes beside it where the ends allow; none while it is inside already (no jitter).
int ne_window(int w0, int visible, int n, int focus, int margin = 1);
struct NeRoll {   // the last NOTE EDIT frame's roll (tests)
  int lanes, visible;   // the page's pitches, the lanes shown
  int lo, hi;           // the lowest and the highest pitch shown (-1: none)
  int focus_y;          // the highlighted note's bar: its top row, -1 not drawn
  bool more_below, more_above;
};
NeRoll ne_roll_shown();
extern const ExtraPage kSongPage;  // SONG RECORD (page_song.cpp, core/song/song.h), last
extern const ExtraPage kInsPage;   // fx-ins: the INSERT page (page_ins.cpp, core/fx/insert.h), after SEQ
extern const ExtraPage kMasterPage;   // the master FX (page_mfx.cpp, core/fx/master.h), after INSERT
void ins_open_out();   // rui1047: the INSERT page on its OUT view of the Rytm's selected track (UP + AMP: 30 page 3)
// rui1048: POLY CONFIG (page_poly.cpp): a track's poly config (seq.h PolyCfg) on the Rytm's screen. poly_page_open: the
// device's page request for track t (the Rytm's track menu item: 30 page 0x20 + t, from_menu: NO goes back to the stock
// screen) or the SIDECAR MENU's entry (t -1: the Rytm's selected track); poly_page_closed: the page closed
extern const ExtraPage kPolyPage;
extern const ExtraPage kArpPage;   // the arpeggiator (core/arp/arp.h)
void poly_page_open(int track, bool from_menu);
// CHOP (page_chop.cpp, core/chop/chop.h): the SRC view's page for the machine, its picture, and the slice editor (page
// "CHOP", off the ring). chop_src_push: a push of encoder B..H on a CHOP track's SRC view (31 0x71..0x77) opens the editor
// for track t (true: taken); chop_page_open the same from elsewhere (t -1: the Rytm's selected track)
extern const Page kChopSrcPage;
extern const ExtraPage kChopPage;
uint32_t chop_key(const Cell& c, const Source& s);
void chop_draw(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot);
bool chop_src_push(int track, int code, bool down);
void chop_page_open(int track);
void chop_page_up_cancel();   // a UP combo reached the host (49 / 30 / 29): the tap in the making is no tap
// SAMPLE EDIT (page_sedit.cpp, core/samp/sedit.h): the sample editor on the Rytm's screen (off the ring; shown while an
// edit is open). sedit_src_push: a push of encoder B on a DSP SAMPLE track's SRC view (31 0x71) opens the track's sample
// (true: taken); sedit_page_open the same from elsewhere: track t's sample, else (t -1) SAMPLES pool code `code` (the
// POOL page's EDIT), else (both -1, the MENU) the Rytm's selected track's, or the edit open already
extern const ExtraPage kSeditPage;
bool sedit_src_push(int track, int code, bool down);
void sedit_page_open(int track, int code);
#if CORE_UI_TOOLS
void chop_page_set(int view, int sel, int zoom);   // tests and the renderer: the editor's view (0 EDIT 1 SLICE 1 2 SLICE 2 3 SLICE 3)
int chop_page_sel();
bool chop_page_all();   // the edit target is ALL slices (an UP tap toggles ONE <-> ALL)
void chop_page_reset();
void sedit_page_reset();   // tests
#endif
void poly_page_closed();
int poly_page_track();   // the track it shows (tests)
#if CORE_UI_TOOLS
// tests and the renderer: the view (track -1: the Rytm's; out: the OUT view; eq: ins1112's EQ view, RIGHT)
void ins_page_set(bool bus, int slot, int busel, int track, bool out = false, bool eq = false);
void mfx_page_set(int slot, bool eq);
#endif
#endif
#if CORE_POLY && CORE_MSAMP
extern const ExtraPage kSampPage;
void source_meter_update(uint32_t ms);   // the tap's peaks into the source meter (page_samp.cpp), at most every 66 ms
void source_meter_draw(Canvas& c, int y, int ch0, int n, int thr_db, int slot);   // n bars of tap channels ch0.., thr_db < 0: its tick
int source_meter_level(int ch);          // tap channel ch's level now, 0..127 (a footer's box)
extern const ExtraPage kPoolPage;      // POOL, after AUTO: the project's three POOL lists (page_pool.cpp, core/lib/pools.h)
void pool_page_set(int list, int bank, int row, int act);   // tests: the view (act: SELECT UNUSED ALL NONE UNLOAD REPLACE UNDO EDIT)
bool pool_page_browsing();
int pool_page_selected();
void pool_page_reset();
extern const ExtraPage kProjectPage;   // PROJECT, after SONG: project bundles (page_project.cpp, core/bundle/feed.h)
void project_page_reset();             // tests: the page's own state cleared
extern const ExtraPage kAutoPage;  // AUTO, after SAMPLE: the auto-sampler's plan, run and result (page_auto.cpp)  // SAMPLE, after SCOPE (shown where REC is): the sampler and the auto-sampler (page_samp.cpp, core/samp)
#endif
// voice vi's P(k+1) (k 0..15) as the Rytm labels it for the machine the voice plays now, into b ("" none: unused);
// returns b. machine_label: a machine's P(k+1) that no code page names (null none). (page_lfo.cpp)
const char* voice_label(int vi, int k, char* b, int cap);
const char* machine_label(int machine, int k);
#if CORE_UI_TOOLS
void lfo_page_set(int sel, bool design, int step, uint32_t ms);   // tests and the renderer: the view
#endif

// ---- cells drawn by code (page.h kDraw: arg one of these, in[] its parameters; page_wave.cpp dispatches)
enum DrawId : uint8_t {
  kDrawSample,   // DSP SAMPLE's waveform: in STA END LOP MOD
  kDrawGrain,    // GRAIN's waveform and grain window: in POS (STA) SIZE (END) DENS (LOP) MOD
  kDrawEnv,      // JUNO's envelope (page_gfx.cpp): in ATK DEC REL LEN, the sustain the patch's
  kDrawFm,       // 6-OP FM's algorithm: in PAT
  kDrawCloud,    // GRAIN's cloud: in SIZE DENS
  kDrawPatch,    // JUNO's patch name: in PAT
  kDrawChord,    // 6-OP FM's / MULTISAMPLE's chord (page_gfx.cpp): in CHD INV SPR VOI, the root and notes poly::view's
  kDrawInst,     // MULTISAMPLE's instrument name: in INST
  kDrawGran,     // GRANULAR's source outline, its grains (live, ~12 fps) and the head (page_wavegrain.cpp): in POS
  kDrawWt,       // WAVETABLE's frame at POS (morphed), the table's frames as ticks (page_wavegrain.cpp): in POS
  kDrawGrList,   // the source lists (page_wavegrain.cpp, src_list_page): GRANULAR's SRC, WAVETABLE's TBL, MULTISAMPLE's
  kDrawWtList,   //   INST as named rows around the value, groups at their first row, a scroll bar: in P1 (4 x 2 cells)
  kDrawMsList,
  kDrawBankList,   // rui1038: the Rytm's bank list (4A): tabs < A B C D E F G H >, rows 000 OFF / 001 name, the cursor
  kDrawChop,       // CHOP's sample with its slices, SLC's inverted, the playheads (page_chop.cpp): in SRC SLC MRPH
};
uint32_t draw_key(const Cell& c, const Source& s);   // what it shows besides its inputs (PageView redraws on a change)
// 0.4.3 (rytm-lag): a picture that moves by itself (WAVETABLE's POS, DSP SAMPLE's and CHOP's playheads) changes its key
// at most every kLiveMs: every new key is a redraw and a commit, and each commit costs the Rytm its whole stock redraw
// (rytm-mods 0026). `id` (what it shows: the table, the sample, the map) changes it at once; `live` (the moving part)
// at most every kLiveMs, the latest value always follows. A change held in the window gives a new key when the window
// ends even if the value came back meanwhile (`pend`): the cell may have been redrawn in between by something this key
// does not see (an input, an invalidate), with the value of that moment. One per picture and voice: it assumes one
// live view per voice (only the remote's PageView, remote.cpp `view`, draws these keys).
constexpr uint32_t kLiveMs = 100;
struct LiveKey {
  uint32_t ms = 0, id = 0, live = 0, gen = 0;
  bool set = false, pend = false;
  uint32_t get(uint32_t i, uint32_t l, uint32_t now) {
    if (!set || i != id) {
      set = true, id = i, live = l, ms = now, pend = false, ++gen;
    } else if (l != live || pend) {
      if (now - ms >= kLiveMs) live = l, ms = now, pend = false, ++gen;
      else pend = true;
    }
    return (id ^ gen * 2654435761u) | 1;
  }
};
void draw_custom(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot);   // slot: its text item
// The machine pages declared here (page_for_machine's fallback: DSP SAMPLE), and a voice's page: GRAIN for a DSP SAMPLE
// voice whose MOD is in GRAIN's zone (with the sample RAM), else page_for_machine (remote.cpp's page list).
const Page* code_page_for_machine(int machine);
const Page* gfx_page_for_machine(int machine);   // page_gfx.cpp: JUNO, 6-OP FM, MULTISAMPLE
const Page* wavegrain_page_for_machine(int machine);   // page_wavegrain.cpp: WAVETABLE, GRANULAR
uint32_t wavegrain_key(const Cell& c, const Source& s);
void wavegrain_draw(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot);
// The source list (GRANULAR SRC, WAVETABLE TBL, MULTISAMPLE INST): while P1 was turned in the last kListMs, the page
// that lists the entries with their names around the value (the value is the selection: it is the Rytm's own P1,
// kept in its kit), else p itself. A GRANULAR turn that rests kSettleMs previews the entry (gr::preview). The first
// sight of a voice / machine, and after src_list_quiet (a kit's values all at once), only takes the value in.
constexpr uint32_t kListMs = 2000, kSettleMs = 300;
const Page* src_list_page(const Page* p, const Source& s);
void src_list_quiet();
// rui1036: P1's push on the Rytm's SRC view (31 0x70): down - the list stays open while held; up within kClickMs, P1
// not turned since the down - a click: the row at P1 acts (a LIBRARY folder / ".." / page browses, a file goes into
// the machine's pool and the voice picks it there). Before any push came (no mod), a rest of kRestMs on a folder /
// page row opens it, and the list closing on a library file adds it (docs/GRANULAR.md "Library and pool").
void src_list_push(bool down);
bool src_list_pushes();
// The voice's P1 written to the Rytm (rui1036 48 via ui::remote_param_write on seq-ux; a stub until merged).
void src_list_write(int vi, int machine, int value);
void src_list_pin(bool on);   // rui1036: P1's push held on the Rytm's SRC view (31 0x70): the list stays open
// rui1036: the lists' names to the Rytm (47, its own value texts: the knob, a p-lock): each list when it changed, all
// after src_names_resend (a hello). Returns the messages sent.
int src_names_tick();
void src_names_resend();
// rui1038, the bank list (rytm-mods 0026 1038, docs/SRC-BANKS.md): the Rytm's 4A opened (on, code = P1 then: its bank and
// row the cursor) or closed it (why 1 YES: the cursor committed - 48 <track> 0 <index> <bank << 4> <flags 1>; else
// nothing) for track t on voice vi, machine m (kGranMachine / kWaveMachine / kMultiMachine). While open: src_bank_key
// takes LEFT / RIGHT (bank -/+) and UP / DOWN (row -/+), src_bank_enc knob A (row +delta, FUNC x 8), and
// src_bank_page(t) is the page the SRC view draws for t (else null). The Rytm keeps one bank's names a list (47 with a
// bank byte): src_names_tick sends the bank the list shows, else the bank of the SRC view's P1.
// rui1040: on 3 = FUNC + YES on the list's row: the card's files for that row (any bank, rows 1..127; row 000 and input / built-in rows refused with a note):
// the host answers 4A 01 <track> 1 (the Rytm's YES / NO then the browser's, 31) and 4A 01 <track> 0 when the browser
// ends (a file set into the row through the pool's rules - core/lib/pools.h set_file / set_special: one entry per hash,
// P3's ask on a row in use, refused while the Rytm plays - or NO / LEFT at its top).
void src_bank_list(int t, int vi, int machine, int code, int on, int why);
// 0.5.0 (the sample browser, docs/SRC-BANKS.md "0.5.0"): down 0 a release, 1 a press, 2 a press with FUNC held (the 0.5.0
// 0026 inside its open list / browser; an older 0026 never sends 2). The new keys (RIGHT quick load and re-entry, FUNC +
// LEFT / RIGHT banks, FUNC + YES preview, FUNC + RIGHT the file menu, FUNC + UP OFF) only with a Rytm that has them:
// its hello's kCapsBrowseFunc, or a 2 seen since the hello; else every key as before.
// kCapsBrowseFunc: hello caps bit 5, the 0.5.0 0026's FUNC-in-browser forwarding (no bit 64: SAMPLER is machine 43).
constexpr int kCapsBrowseFunc = 32;
bool src_bank_key(int code, int down);
bool src_bank_new_keys();   // the new keys are on (above)
bool src_bank_enc(int e, int delta, bool func);
const Page* src_bank_page(int t, int machine);
struct BankListState { bool on; int track, machine, bank, row, commits; bool browse; int cur, assigns, refusals, menu; };
BankListState src_bank_state();   // tests
// P1's value as a name on those machines' pages (the bottom bar, no echo): null on any other page / parameter.
const char* src_value_name(const Page& p, int param, const Source& s);
// test-1031: the SRC view's page for 6-OP FM / MULTISAMPLE while a chord plays (CHD on, or the pads played one): the
// bottom row the chord (its name, INV SPR VOI, a 3-octave keyboard with its notes from the last trig's root, the notes
// sounding now); any other page (or no chord) itself.
const Page* chord_view_page(const Page* p, const Source& s);
uint32_t gfx_key(const Cell& c, const Source& s);
void gfx_draw(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot);
// 6-OP FM's patch as the voice's PAT (0..127) picks it (fm_select's rule: the project's FM list, else the ROM banks), for
// the Pi display's VOICE page (pi/engine/voice_pi.cpp): its name, the algorithm (0..31) and its drawing's layout
// (operator k's column and level, carriers on level 0), who modulates whom (mod[k] bit m: op m + 1 modulates op k + 1),
// the carriers (bit k), the feedback loop (fb_src -> fb_dst, 0..5, -1 none) and its amount (0..7), each operator's
// output level (0..99) and frequency (ratio x 100, or a fixed frequency in Hz with fixed set). False: no patch (an
// empty list slot). Reads the list as the voices do (drawing only).
struct FmInfo {
  char name[11];
  int alg, fb, fb_src, fb_dst, ncols, nlev;
  int8_t col[6], lev[6];
  uint8_t mod[6], car;
  uint8_t out[6];
  uint16_t freq[6];
  bool fixed[6];
};
bool fm_info(int pat, FmInfo* o);
#if CORE_UI_TOOLS
void fm_algorithm_render(int alg, Canvas& c);                 // the renderer: algorithm alg (0..31) alone
int fm_algorithm_layout(int alg, int8_t* col, int8_t* lev);   // tests: its operators' columns and levels; columns
#endif
const Page* page_for_voice(int machine, int vi, int mod = -1);   // mod: MOD (P12) 0..127 as the page reads it, -1 the voice's
// The page the SRC view draws for a page_for_voice page: DSP SAMPLE / GRAIN (sample on P9..P16, the SMPL page) get the
// SRC page's own P1..P8 under the waveform, as the Rytm's encoders there turn those; any other page itself.
const Page* src_view_page(const Page* p);
// The sample overviews: one column of the file being read (remote_tick while the page is open); tests: cleared.
void wave_idle(uint32_t ms);
uint32_t wave_take_us();   // wave_idle's time since the last call
void wave_reset();
const void* wave_of_slot(int slot, int* state, int* progress);  // state 0 none, 1 loading (progress columns), 2 ready, 3 failed
const void* wave_of_voice(int vi, int* state, int* progress);   // the voice's SMP (P11)
void wave_column(const void* w, int c, int* lo, int* hi);      // column c (0..127) of an overview: min / max >> 16 bits

}  // namespace ui
}  // namespace core
