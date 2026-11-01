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
// The Rytm's own hello 0x35 goes back to the Rytm (a box unplugged, a Rytm plugged in), unless the box's profile has
// the remote UI. All of it runs where engine_sysex runs (the Pi's core 3); nothing here runs per sample. kVoices,
// P1..P16 and the engine's own message layout stay the engine's: a profile fits a box into them.
//
// State: a box with a state_dir keeps its projects, settings and pools there, not in /.rytm, so two devices' state never
// mixes (shared content stays shared: /Samples, /Plugins, /JUNO, /DX7, ...). The engine's "/.rytm/..." literals stay as
// they are: the one path function of each port (platform_pi.cpp full, platform_host.cpp host_path) passes them through
// state_path. The folder is chosen once a boot, before the first file is read, and never changes while the engine runs:
// the card is touched long before any box can speak, and a stock Rytm never says hello. So the box of the last run is
// remembered in /.rytm/device. No file: the Rytm and /.rytm, as ever. A hello (or a Rytm's 35) that needs another folder
// is answered kRestart, the choice saved, the Pi restarted once after the saves drained (hooks.device_restart). The Pi's
// own files stay in /.rytm whatever the box: kMachine in device.cpp.
#pragma once
#include <stdint.h>

namespace core {
namespace out { struct Device; }
namespace device {

constexpr uint8_t kCmdHello = 0x4F;
constexpr uint8_t kProto = 1;
constexpr uint16_t kIdRytm = 1;
enum Status : uint8_t { kUnknown = 0, kOk = 1, kBadProto = 2, kRestart = 3, kWait = 4 };   // kRestart: say hello again after the Sidecar's reboot; kWait: a Rytm has the Sidecar, go on saying hello
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
  const char* state_dir;   // its state folder ("/.digitakt"); null: the Rytm's /.rytm
  // Engine commands it may send as they are, on the Sidecar's head (bit c of word c / 32): a box that reports like the Rytm
  // and names each voice's machine in the voice byte. For 03 / 04 / 11 the voice must be under `voices`.
  uint32_t in_allow[4];
  Bracket bracket;
  const out::Device* audio;   // its Overbridge stream (core/fx/out.h): channels each way, return channels, a stereo mix or per
                              // voice, what the Pi's driver opens. Null: no audio, the driver leaves the USB id alone
  const Controls* controls;   // null: no controls report
};

const Profile& rytm();                 // the default
const Profile& active();               // the profile in use
const Profile* by_usb(uint16_t vid, uint16_t pid);   // the known box with this USB id and an audio row (the Pi's driver); null none
bool add(const Profile* p);            // one more known box (false: id taken, table full, or it does not fit the engine)
// What one connected box has chosen: its profile, its state folder, its output, its counts. There is one link today, g_link
// (docs/device-profiles.md, "Several devices"): a second one is a second Link and a way to tell whose message it is.
struct Link {
  const Profile* box;                     // the box in use; null: the Rytm
  const char* state;                      // the state folder when it is not /.rytm; null: /.rytm
  void (*out)(const uint8_t*, int);       // the platform's to_rytm while a box is in use
  uint32_t dropped;
  const Profile* next;                    // a change of folder asked: the box it is for
  bool saving, posted;
  uint32_t post_id;                       // its write to /.rytm/device
};
extern Link g_link;
void hello(Link& l, const uint8_t* d, int len);   // a whole 4F
void reset();                          // back to the Rytm and /.rytm, as a fresh boot (tests)
uint32_t dropped();                    // messages from the box not translated, and messages to it filtered

void boot();                           // engine_begin, before anything else reads the card: the last box and its folder
bool leaving();                        // a change of folder waits: the idle saves flush, not settle
// The idle saves' tail: a change of folder once settled (all saved), and a restart only when the choice is on the card; true if it did work
bool step(bool settled = true);
// The Pi's Overbridge driver (core 0): a Rytm is on USB, or gone. A stock Rytm never says hello, so its USB id is how it asks for
// /.rytm: the same saved choice and one restart as its 35, unless a box said hello this boot. While it is there, a hello that
// would leave /.rytm waits (kWait), so two boxes on USB do not restart each other; so does one in the first kSettleMs of the boot
// a Rytm's id caused, when the Rytm may not have enumerated yet. Another box's USB id changes nothing: its hello decides.
void rytm_on_usb(bool on);
constexpr uint32_t kSettleMs = 10000;
const char* state_path_box(const char* path, char* buf, int cap);
// path as the card has it: path itself, or in buf. Every file call passes here, so the Rytm's cost is the one test.
// The file layer has no link to ask: this is g_link's folder.
inline const char* state_path(const char* path, char* buf, int cap) { return g_link.state ? state_path_box(path, buf, cap) : path; }
inline bool foreign(const Link& l = g_link) { return l.box != nullptr; }
// A whole message from a box: d and len are what host_sysex goes on with (the translation, in m; or the message itself
// when it is a 35 that gave the Rytm back); false: nothing more to do.
bool translate(Link& l, const uint8_t*& d, int& len, uint8_t* m);
// The control loop, every pass: a box with Controls runs its envelopes (strip.h). Nothing else: one test.
void strip_tick(uint32_t us);
inline void tick(uint32_t us) {
  if (g_link.box && g_link.box->controls) strip_tick(us);
}

}  // namespace device
}  // namespace core
