// core/device/device.h - the device profiles (docs/device-profiles.md): what the Sidecar assumes about the box it
// serves, in one table per box, and the box's choice of its table at its hello. The Analog Rytm MKII is the default: a
// Rytm never says hello here, nothing is translated or filtered for it and its messages take host_sysex's path as they
// always did (the only new work is one pointer test and one compare per SysEx, on the control core).
//
// Another box says hello, F0 7D 52 54 4F <ver 1> <id hi> <id lo> <proto 1> [<name>] F7, and gets one answer,
// F0 7D 52 54 4F <ver 1> <id hi> <id lo> <status> F7 (kOk / kUnknown / kBadProto). From an accepted hello on:
//   in   its trig, params and note-off messages (Profile's head and cmd_*) are turned into the engine's own 0x04 / 0x03
//        / 0x11; everything else from it is dropped, so a Rytm-only command never reaches a Rytm-only path.
//   out  what the engine sends goes through out_allow (default for a profile: nothing): a box with a small CPU is not
//        loaded with the Rytm's pages, pool tables and label lists, and there is no per-message answer.
// The Rytm's own hello 0x35 goes back to the Rytm (a box unplugged, a Rytm plugged in); a box with the remote UI says 0x36
// to its pages. All of it runs where engine_sysex runs (the Pi's core 3); nothing here runs per sample. kVoices,
// P1..P16 and the engine's own message layout stay the engine's: a profile fits a box into them.
//
// State: a box with a state_dir keeps its projects, settings and pools there, not in /.rytm, so two devices' state never
// mixes (shared content stays shared: /Samples, /Plugins, /JUNO, /DX7, ...). The engine's "/.rytm/..." literals stay as
// they are: the one path function of each port (platform_pi.cpp full, platform_host.cpp host_path) passes them through
// state_path. Two slots (docs/device-profiles.md "Several devices"; design decided 2026-11-21): slots by attach order, folders
// by box type. A box takes the lowest free slot at its hello (or its USB enumeration on the Pi, claim), on whichever
// transport it spoke (slot_of); the Rytm's hello is its 35, or its USB id. Any two different box types in either order; no
// file stores a slot number. At boot slot 0 is presumed the Rytm's (a lone stock Rytm says nothing first): its first message
// confirms it, and a box's hello before that takes slot 0 instead. A box that leaves (release) frees its slot, its voices
// stop and its edits are saved into its folder; the other box stays where it is; a box that comes back loads from its
// folder. Nothing restarts. Each file call is in the folder of the slot in hand (device::Scope), or of the card request's
// (CardScope). The Pi's own files are in /.sidecar whatever the box (core/device/sidecar_dir.h): no box's folder holds them.
#pragma once
#include <stdint.h>

#include "machines.h"   // kVoices, kBoxVoices

namespace core {
namespace out { struct Device; }
namespace admit { struct Load; }
namespace device {

constexpr uint8_t kCmdHello = 0x4F;
constexpr uint8_t kProto = 1;
constexpr uint16_t kIdRytm = 1;
enum Status : uint8_t { kUnknown = 0, kOk = 1, kBadProto = 2, kRestart = 3, kWait = 4 };   // kRestart: no longer sent (a restart for a folder, before slots); kWait: no slot free (two other boxes, or a slot whose box left still saving), go on saying hello
constexpr int kMaxParams = 16;
constexpr int kMaxMessage = 44;   // the engine's longest report: 0x04 with P1..P16 and the sound tag
constexpr uint8_t kCmdControls = 0x50;   // a box's controls report: head 50 <track> { <value> <fraction> } x Controls::n F7
constexpr int kMaxControls = 16;

// What the box has of the Rytm's mods' side of the protocol. Declarative for now, except kRemoteUi (see above): the
// Rytm-only paths are out of a box's reach because its messages are translated or dropped.
enum Feature : uint32_t {
  kRemoteUi = 1u << 0,     // 0x30..0x3F: pages on the box's screen
  kProject = 1u << 1,      // 0x10 the project's name: /.rytm files per project
  kKits = 1u << 2,         // 3B the active kit, 4C the kit reports
  kPatterns = 1u << 3,     // the Rytm pattern dumps (core/seq/rytm_dump.h), 2C..2E the step ticks
  kSendFx = 1u << 4,       // 0x20..0x24
  kUserSlots = 1u << 5,    // 19, 1A, 1B, 1D, 1E, 25: U1..U5 and their labels
  kSrcView = 1u << 6,      // 29, 2A, 2B: the stock SRC page's values
  kFxIn = 1u << 7,         // 26, 27
  kPools = 1u << 8,        // 0D..0F, 12..18: the card's lists on the box's screen
  kRytmKeys = 1u << 9,     // the page tables' keys (ui/page_keys.h): the Rytm's panel
};

// What a box can do, plainly. A box picks one and fills in the options; no new code for a box that fits.
//   kBracketFull  the voices go back one by one, into the box's own filter, amp and FX, which shape them there (the Rytm).
//                 The audio row has kPerVoice; no controls.
//   kBracketMix   one stereo mix on the main output: nothing of the box's voice path touches a Sidecar voice, so the box's
//                 filter and amp pages drive the Sidecar's instead. The audio row has kStereoMix, and Controls says which
//                 of the box's values do what (the Digitakt mk1).
enum Bracket : uint8_t { kBracketFull = 0, kBracketMix = 1 };

// A box's physical controls: which of the Sidecar's voice stages each value of its controls report reaches (strip.h).
enum Control : uint8_t {
  kCtlNone = 0,
  kCtlFreq, kCtlReso, kCtlType, kCtlFltEnv,        // the filter: cutoff, resonance (a bell's gain), type, envelope depth
  kCtlFltA, kCtlFltD, kCtlFltS, kCtlFltR,          // its envelope
  kCtlAmpA, kCtlAmpH, kCtlAmpD, kCtlVol, kCtlPan,  // the amp
  kControls
};
enum FilterKind : uint8_t { kFltOff = 0, kFltLp, kFltHp, kFltBell };
struct Controls {
  uint8_t n;                       // values in its report
  uint8_t target[kMaxControls];    // value k's Control
  uint8_t type[8];                 // kCtlType's value -> FilterKind
};

// A box with kRemoteUi: what its screen says that the Rytm's says in the Rytm's words. Pages that need a feature it lacks are hidden (extra.h extra_for_box).
struct Screen {
  const char* names[8];    // a voice's name for the user, for the voices it uses; SCOPE and the meters too, each on IN 2 + the voice; "-" a channel it does not fill
  const char* keys;        // the bottom bar of the connect splash: how the pages open
};

struct Profile {
  uint16_t id;             // 14 bits, in the hello. kIdRytm the Rytm; docs/device-profiles.md says how to pick one
  const char* name;        // for the log
  uint16_t usb_vid, usb_pid;   // the USB device the Pi's Overbridge driver claims for it, with an audio row: OVERBRIDGE mode's id
  uint8_t tracks;          // the box's tracks
  uint8_t voices;          // the Sidecar voices it may use, <= kVoices; a report's track selects one, >= voices dropped
  uint8_t params;          // parameters in its reports, <= kMaxParams
  uint8_t head[3];         // after F0: its SysEx identity (the hello always uses the Sidecar's 7D 52 54)
  uint8_t cmd_trig;        // head cmd <track> <note> <vel> { <value> <fraction> } x params F7
  uint8_t cmd_params;      // head cmd <track> { <value> <fraction> } x params F7
  uint8_t cmd_note_off;    // head cmd <track> F7
  uint32_t features;
  uint8_t machine;         // the machine its voices play: the voice byte's flag (machines.h)
  uint16_t sound_tag;      // the engine's sound tag on its reports, 0 none (md.h kLayoutTag: MD's own page)
  int8_t map[kMaxParams];  // P(k + 1) = its parameter map[k]; -1: fixed[k]
  uint8_t fixed[kMaxParams];
  uint32_t out_allow[4];   // bit c of word c / 32: engine command c may be sent to it
  // Optional, for a box whose reports do not fit the layout above: its message into a whole engine message in m
  // (kMaxMessage bytes), the length, 0 drop. Null: the table above.
  int (*translate)(const Profile& p, const uint8_t* d, int len, uint8_t* m);
  const char* state_dir;   // its state folder ("/.dt"); null: the Rytm's /.rytm
  // Engine commands it may send as they are, on the Sidecar's head (bit c of word c / 32): a box that reports like the Rytm
  // and names each voice's machine in the voice byte. For 03 / 04 / 11 the voice must be under `voices`.
  uint32_t in_allow[4];
  Bracket bracket;
  const out::Device* audio;   // its Overbridge stream (core/fx/out.h): channels each way, return channels, a stereo mix or per
                              // voice, what the Pi's driver opens. Null: no audio, the driver leaves the USB id alone
  const Controls* controls;   // null: no controls report
  bool note_out;              // the engine's own notes (the sequencer's) go to the box as MIDI, channel = voice: its voice's amp must open for the Sidecar's voice to sound
  bool audio_by_hello;        // (no longer read: by_usb claims every box's stream at enumeration, one stream a slot) its stream was claimed only once it was the box in use
  int16_t trk10;              // the voices' own channels' trim while it is the box in use, tenths of a dB (out::Control::trk10): a box whose insert point sits lower than the Rytm's; 0 for the Rytm
  bool chords;                // its 0x04s on a voice within kChordMs with different notes are one chord on the voice's machine, released whole by 0x11 (one report a note: the A4, on any track)
  const Screen* screen;       // with kRemoteUi; null: the Rytm's words
  const char* short_name;     // the box's word for the user where the Sidecar would say "Rytm" ("A4"); null: name
};
// The box's word for the user: its short name, else its name, else "box"
inline const char* short_name(const Profile& p) { return p.short_name ? p.short_name : p.name ? p.name : "box"; }

const Profile& rytm();                 // the default
const Profile& active();               // the profile in use
const Profile* by_id(uint16_t id);        // the known box with this id; null none
const Profile* by_usb(uint16_t vid, uint16_t pid, bool without_hello = false);   // the known box with this USB id and an audio row (the Pi's driver); null none.
                                       // without_hello: no longer changes the answer (every box's stream is claimed at enumeration)
bool add(const Profile* p);            // one more known box (false: id taken, table full, or it does not fit the engine)
// What one connected box has chosen: its profile, its state folder, its output, its counts. One Link per box on USB, kLinks
// at most (docs/device-profiles.md, "Several devices"). Link 0 is the one a single box has always had.
struct Link {
  const Profile* box;                     // the box in use; null: the Rytm, or no box (held null)
  const Profile* held;                    // who holds the slot (rytm() for the Rytm); null: free
  const Profile* was;                     // whose folder state is: the slot's last holder (kept after it leaves: its queued writes land there)
  bool presumed;                          // held by the Rytm presumed at boot, not yet heard from (a box's hello takes the slot)
  bool parting;                           // its holder left and its edits are still being saved: no other box takes it yet
  bool holding;                           // its holder left, its edits saved: kept for it a while yet (g_hold_ms), no other box takes it
  uint32_t left_ms;                       // when its holder left (now_ms)
  const char* state;                      // the state folder when it is not /.rytm; null: /.rytm
  void (*out)(const uint8_t*, int);       // the platform's to_rytm while a box is in use
  uint32_t dropped;
  const Profile* next;                    // a change of folder asked: the box it is for
  bool saving, posted;
  uint32_t post_id;                       // its write to /.rytm/device
  uint8_t index;                          // its place in g_links
  // Same type twice (design decided 2026-11-26, docs/device-profiles.md "Two boxes of one type"): a box is its profile and its
  // DeviceUID, the 32-bit answer to the Elektron MIDI RPC 0x03 (the same value the box's vendor control request 0 returns,
  // the one Overbridge keys devices by). 0: not known (a hello, a stock Rytm's 35, a box that did not answer in time).
  uint32_t uid;                           // the holder's (0 unknown)
  uint32_t was_uid;                       // whose folder state is, with was
  char dir[20];                           // its UID folder, "/.rytm/3fa2c001" (state points here); "" the type folder itself
  char label[24];                         // the box's name for the user (dir/name.txt; default "<type> <last 4 hex>"), "" none
  // CPU admission (admit.h): the voices its box asked for (its type's count, or voices=) and the ones admitted, 0 while free.
  // Its box voices 0..voices-1 render and play; a report for another is dropped. order: its attach (the first box keeps its
  // voices). tell: its box is to be told its count (a popup on its screen) until tell_until.
  uint8_t want, voices;
  uint32_t order;
  bool tell;
  uint32_t tell_until;
};
constexpr int kLinks = 2;
extern Link g_links[kLinks];
// The control core's link in hand: whose message is being handled, whose state is being saved. Outside any Scope it is g_base,
// the slot the engine's own work is for (its pages, its reports, its saves): slot 0 while it is held, else the slot that is
// (a box alone in slot 1: its partner in slot 0 left), so everything that never names a link is what it always was.
extern Link* g_cur;
extern Link* g_base;
extern int g_depth;   // Scopes open (the control core's)
inline Link& cur() { return *g_cur; }
struct Scope {   // the control core only; nests
  Link* was;
  explicit Scope(Link& l) : was(g_cur) { ++g_depth, g_cur = &l; }
  ~Scope() { g_cur = --g_depth ? was : g_base; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};
// Which link a file call is for (state_path). On the control core: g_cur. On the card core: the link its request was
// queued for (CardScope, set around each request it runs), else g_web_link (the web's own calls: the box its page serves).
// hooks.control_core says which core calls; null (the host, one thread): always the control core's answer.
// -1 while a RawPath is open on the control core: paths as they are (the moves into UID folders, /.rytm/last).
int path_link();
extern volatile int8_t g_web_link;
struct RawPath {   // the control core only; nests
  RawPath();
  ~RawPath();
  RawPath(const RawPath&) = delete;
  RawPath& operator=(const RawPath&) = delete;
};
struct CardScope {   // around one card request, on whichever core runs it
  int8_t was;
  Link* was_cur;
  explicit CardScope(int link);
  ~CardScope();
  CardScope(const CardScope&) = delete;
  CardScope& operator=(const CardScope&) = delete;
};
// The platform's sender for link 1.. (its box's USB MIDI), null when it goes. Link 0's is hooks.to_rytm as ever. While only link 0
// is up and it is a Rytm, hooks.to_rytm stays the platform's own; otherwise it sends to the link in hand (Scope).
void set_out(int link, void (*out)(const uint8_t*, int));
void hello(Link& l, const uint8_t* d, int len);   // a whole 4F, on the slot in hand (slot_of has claimed it): answered kOk
uint8_t hello_status(const uint8_t* d, int len);   // a refused hello's answer: kUnknown, kBadProto or kWait (no slot free)
// Attach order (decided 2026-11-21). claim: the slot box p holds (null or rytm(): the Rytm), else the lowest free one, which
// it now holds (its folder from now on; a presumed Rytm gives slot 0 up to the first box); -1: none free. Called at a hello
// (slot_of) and, on the Pi, when the box's USB device enumerates (ob_usb): the same box type is the same slot both ways.
// release: slot s's box left (its USB device gone, its transport down): its voices stop, its transports route nowhere, its
// edits are saved into its folder (parting until they are), the other slot does not move. Control core.
// Same type twice: uid the box's DeviceUID (0 unknown). A slot is box (p, uid)'s when its holder is p and the two UIDs
// match; a UID unknown on either side matches by type alone, and only while no other box of that type holds or keeps a
// slot (design 2026-11-26 point 4: with two boxes of a type an unknown one waits, -1). hint: the slot the transport the box
// spoke on is part of (the Pi's USB: set_midi_slot), taken first when that slot is p's.
int claim(const Profile* p, uint32_t uid = 0, int hint = -1);
void release(int s);
// How long a slot whose box left stays that box's after its edits are saved (ms; 0: none, freed at once). A box that drops off
// the bus for a moment and comes back (a plug on the hub's other port knocked it off, unit run AO2) keeps its slot and route.
// The Pi's cmdline slothold= (ms, default 5000); 0 in the host tests.
extern uint32_t g_hold_ms;
int slot_held_by(const Profile* p);    // the (first) slot a box of type p (null: the Rytm) holds, -1 none
int slot_of_box(const Profile* p, uint32_t uid);   // the slot box (p, uid) holds (claim's match), -1 none
int boxes_of(const Profile* p);        // slots a box of type p holds or keeps (saving, held for it)
// The Pi's USB, core 0 each pass: transport t's USB MIDI is part of the box whose stream is slot s (-1 none; CObUsb::MidiSlot).
// A hello or a Rytm's 35 on t takes that slot when it is that box type's: two boxes of one type speak the same words.
void set_midi_slot(int t, int s);
// The UID as its folder name: 8 lowercase hex digits, buf at least 9
void uid_text(uint32_t uid, char* buf);
bool uid_parse(const char* s, uint32_t* uid);   // exactly 8 hex digits
// The MIDI RPC DeviceUID (request 0x03, answer 0x83: the stock firmware's, the framing Elektroid uses: F0 00 20 3C 10 00, then
// the message 8-to-7 encoded: seq hi, seq lo, 0, 0, command; the answer's UID 4 bytes MSB first after its command). The fallback
// when a box's USB gave no UID (design 2026-11-26 attach step 2): asked on its own transport every kAskMs for kAskForMs, never
// while g_frozen says its stream is frozen. ask_uid: start asking on transport t for box p (a USB attach with no UID, a hello or
// a Rytm's 35 that waits beside a box of its type); an answer gives the slot its UID (learn), or claims one for it.
constexpr uint32_t kAskMs = 500, kAskForMs = 2000;
void ask_uid(int t, const Profile* p);
bool asking(int t);
extern bool (*g_frozen)(int slot);   // the Pi: slot's stream is frozen (ob_freeze.h); null none
int uid_request(uint16_t seq, uint8_t* m);   // the request into m (16 bytes), its length
bool uid_answer(const uint8_t* d, int len, uint32_t* uid);   // an 0x83 answer's UID
// The box's name (Link::label) set by the user: written to its folder's name.txt (control core). false: no box there, or a
// type folder without a UID folder (a box whose UID was never known has no name of its own)
bool set_label(int s, const char* text);
// The console's box selector (design 2026-11-21 point 8: commands touching box state name their box): "box=S:<command>", S a
// slot (0, 1) or a box type by its folder's name ("rytm", "a4", "dt": the same box whatever its attach order). The slot the
// command is for, *rest the command after the selector; -1: no selector (*rest = p: the base box's, as ever); -2: a selector
// that names no box attached now (*rest the reason). The platform runs *rest in that slot's Scope.
int box_prefix(const char* p, const char** rest);
const char* type_word(const Profile* held);   // a holder's folder name without "/.": "rytm", "a4", "dt"
inline bool held(int s) { return s >= 0 && s < kLinks && __atomic_load_n(&g_links[s].held, __ATOMIC_RELAXED) != nullptr; }
// slot s's modules run (tick, saves, its folder's work): held, or its box left and its edits are still being saved, or it is the
// base (the slot in hand outside any Scope, held or not). One slot (the Teensy): slot 0 always. Control core.
inline bool live(int s) {
  if (kVoiceSlots < 2) return s == 0;
  return s >= 0 && s < kLinks && (g_links[s].held || g_links[s].parting || &g_links[s] == g_base);
}
// slot s is held by the Rytm (its own words, kits, scenes, stems, calibration: its alone). One slot (the Teensy): always, as
// slot 0 always was
bool rytm_at(int s);
// slot s's folder state is a Rytm's: held by one, or its last holder (Link::was) a Rytm whose edits are still being saved.
// Its send FX are its kit's 0x20s, never its INSERT file's SFX lines, in either slot. One slot (the Teensy): always
bool rytm_state_at(int s);
int rytm_slot();                       // the (first) Rytm's slot, -1 none (one slot: 0)
// Where the Rytm's own state is read: the slot in hand when that is a Rytm (two Rytms: each its own; control core), else the
// first Rytm's slot, else 0
int rytm_home();
// The slot transport t's message is for: 0 the platform's host link (the USB-C box's MIDI, the emulator's TCP link), 1 the other
// box's (the Teensy relay's). A hello claims a slot for its transport, a Rytm's 35 the Rytm's (its first other message only
// confirms the slot 0 presumed the Rytm's at boot). -1: none (a hello refused: engine_sysex_link answers it; else the message is
// dropped: a transport goes nowhere until a hello or a 35).
// engine_sysex_link calls it for each message; with d null it only says where t goes now.
int slot_of(int t, const uint8_t* d, int len);
// The slot transport t carries now (slot_of's answer for a message that is not a hello or a 35): the platform's raw MIDI
// taps (core/song's byte) enter that slot's Scope. Two loads, any core.
int route_of(int t);
// The Rytm's slot when transport t carries it, else -1: a transport routed to its slot; transport 0 with no route yet (a stock
// Rytm on the host link, as ever): rytm_home(). The Pi's taps of its raw MIDI (its pattern dumps, FILL's answers) read only it.
int rytm_route(int t);
void (*tx(int t))(const uint8_t*, int);   // transport t's sender
void reset();                          // back to the Rytm and /.rytm, as a fresh boot (tests)
uint32_t dropped();                    // messages from the box not translated, and messages to it filtered

// engine_begin: a move into a UID folder cut short (its marker .moving still there) finished, then the presumed Rytm's folder:
// /.rytm/last's UID folder (the Rytm attached last), else /.rytm itself (a card no UID was ever read for)
void boot();
bool leaving();                        // a slot whose box left is saving its edits (idle_save: what waits to settle goes now)
bool step(bool settled = true);        // false: nothing to do (kept for the engine's idle saves)
// CPU admission's own pass (in step): shed what no longer fits at the clock now, admit refused INSERT types again. The
// load test (loadtest=on: idle_save returns before step) runs it too, so a lowered clock sheds there as it does in use.
void cpu_step();
// A Rytm is on USB (claim), or gone (release): the control core's (the tests'). The Pi's driver uses usb_attach / usb_detach.
void rytm_on_usb(bool on);
// The Pi's Overbridge driver, core 0, when box p's USB device enumerates: the slot its stream is (its slot already, else the
// lowest one a hello could take now), reserved for p at once; the control core makes it p's at its next step (claim). -1: none
// (two other boxes: it keeps its MIDI, no audio). usb_detach: slot s's device is gone; the control core releases it.
// uid: the box's DeviceUID read from its USB device (vendor request 0, CObUsb), 0 none: matched as claim's.
int usb_attach(const Profile* p, uint32_t uid = 0);
void usb_detach(int s);
constexpr uint32_t kSettleMs = 10000;
// CPU admission (admit.h; decided 2026-11-21). voices_asked: what box h (null or rytm(): the Rytm) asks for, its type's
// voices= or its profile's count. admitted: slot s's box voices that render and play now (0 free); any core, one byte load.
// readmit: every held slot again in attach order (the kernel's boot, once the bench and voices= are in: before a box can attach).
int voices_asked(const Profile* h);
inline int admitted(int s) {
  return s >= 0 && s < kLinks ? __atomic_load_n(&g_links[s].voices, __ATOMIC_RELAXED) : 0;
}
// engine voice vi plays: its box voice is under its slot's count (one slot: link 0's), or its slot is free (nothing of a box's
// plays there: the self-test's drone, the tests)
inline bool voice_admitted(int vi) {
  const int s = kVoiceSlots > 1 ? slot_of_voice(vi) : 0;
  return !held(s) || box_voice(vi) < admitted(s);
}
void readmit();
// The load admission counts now (admit.h Load): the held boxes' admitted voices, what they asked for, and the AMP slots playing
// on each render core (the first ask wins: amp_fits). /status's core1Bp / core2Bp
void load_now(admit::Load* out);
// what slot s's box asked for (0 free): the Pi's core 3 reads it for the render split (admit::split_for), one byte load
inline int wanted(int s) { return s >= 0 && s < kLinks ? __atomic_load_n(&g_links[s].want, __ATOMIC_RELAXED) : 0; }
constexpr uint32_t kTellMs = 30000;   // a box is told its count while its screen may take a popup, this long after its attach
// The sequencer played voice vi, or let it go (vel 0; note -1: every note held there, one off each): a box with note_out hears it as MIDI; its mod's report of that very note is dropped (kEchoMs): the voice plays once.
void box_note(int vi, int note, int vel);
constexpr uint32_t kEchoMs = 40;
constexpr uint32_t kChordMs = 20;   // from a chord's last note to its next; the A4's mod keeps a mono track's trigs apart by the same time (a4_report.c CHORD_GAP)
constexpr int kChordNotes = 4;      // the A4's most on a trig [read in OS 1.56]
const char* state_path_box(const char* state, const char* path, char* buf, int cap);
// path as the card has it: path itself, or in buf. Every file call passes here: with no box on any link (a lone Rytm) the cost
// is the one test it always was.
inline const char* state_path(const char* path, char* buf, int cap) {
  if (!g_links[0].state && !g_links[1].state) return path;
  const int k = path_link();
  if (k < 0) return path;
  const char* s = g_links[k].state;
  return s ? state_path_box(s, path, buf, cap) : path;
}
inline bool foreign(const Link& l = cur()) { return l.box != nullptr; }
// The engine voices of a link: its box's voice k (the wire's voice byte & 7, a project's track) is engine voice
// voice_base(l) + k. Slot 0's (the first attached) are 0..7 and slot 1's 8..15, whatever the boxes; on an 8-voice engine (the
// Teensy) every link's are 0..7.
inline int voice_base(const Link& l) { return kVoiceSlots > 1 ? l.index * kBoxVoices : 0; }
inline int voice_base() { return voice_base(cur()); }
inline int voice_in(uint8_t vb, const Link& l = cur()) { return voice_base(l) + (vb & (kBoxVoices - 1)); }   // a voice byte -> engine voice
// The first engine voice of the box the engine's own work is for (g_base): the voices the one output stage carries, the
// project's tracks, the pages (until each slot has its own). Any core reads it (one aligned byte load).
inline int base_voice_base() {
  const Link* b = __atomic_load_n(&g_base, __ATOMIC_RELAXED);
  return b ? voice_base(*b) : 0;
}
inline int base_voice(int k) { return base_voice_base() + (k & (kBoxVoices - 1)); }   // the base box's voice k -> engine voice
inline int base_box_voice(int vi) {   // engine voice vi -> its number in the base box, -1 another box's
  const int k = vi - base_voice_base();
  return k >= 0 && k < kBoxVoices ? k : -1;
}
// The slot whose voices sound beside the base box's, through an output stage of their own (on the Pi its own FIFO and
// render pace, paced by its own box's stream; its OUT and INSERT settings are slot 1's, core/fx/out.h control1 and
// core/fx/insert.h control1, alone or paired): 1 while both slots are held (the second box to attach, whatever its type), else -1
// (one box: the base box's stage carries it, as it always did). Any core reads it (two aligned loads).
inline int side_slot() {
  if (kVoiceSlots < 2) return -1;
  const Link* b = __atomic_load_n(&g_base, __ATOMIC_RELAXED);
  return b == &g_links[0] && __atomic_load_n(&g_links[1].held, __ATOMIC_RELAXED) ? 1 : -1;
}
inline bool on_side(int vi) { return slot_of_voice(vi) == side_slot(); }   // engine voice vi plays through the side stage
// The box's own words (Screen), from one read of l.box: the Pi's core 0 (rec_pi's view, the voices' names) asks while
// the control core may set the box. Null: the Rytm, or a box without a Screen.
inline const Screen* box_screen(const Link& l = cur()) {
  const Profile* p = __atomic_load_n(&l.box, __ATOMIC_RELAXED);
  return p ? p->screen : nullptr;
}
// A whole message from a box: d and len are what host_sysex goes on with (the translation, in m; or the message itself
// when it is a 35 that gave the Rytm back); false: nothing more to do.
bool translate(Link& l, const uint8_t*& d, int& len, uint8_t* m);
// The control loop, every pass: a box with Controls runs its envelopes (strip.h). Nothing else: one test.
void strip_tick(uint32_t us);
inline void tick(uint32_t us) {
  for (const Link& l : g_links)
    if (l.box && l.box->controls) return strip_tick(us);
}

}  // namespace device
}  // namespace core
