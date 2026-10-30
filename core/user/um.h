// core/user/um.h - user machines (the .rmp machine plugins of docs/plugins.md) on a board whose packages are files
// on its card: the Pi (pi/engine) and the Mac's core lib (core/host, sim/um_link.py). The Teensy keeps its own glue
// (teensy/voiceloop, a flash store); both speak the same slot protocol (sim/protocol.py 19..1E) and load the same
// plugin ABI (sdk/rmp/rmp_api.h). docs/user-machines-pi.md has the design.
//
//   card     /Plugins/*.rmp - the installed machines (any file name; the package's own id names it). Packages
//            for the other target (an RMP1 Thumb image on the Pi) are listed, marked, never loaded.
//            /.rytm/slots/<PROJECT>.txt - which machine each user slot U1..U5 plays (rmp_slots_parse's format,
//            as the Teensy's).
//   code     each package's image is placed into the board's executable memory (Exec below) at a 4096-aligned
//            address, relocated (rmp_loader: RMP_REL_ABS64), the caches synchronised, init() called once.
//   cores    everything here runs on the control core (the Pi's core 3, the Mac's caller) between blocks, except
//            user_render (a render core) and json_copy (any core, the web on the Pi's core 0).
//            No card access in a call longer than one 8 KB read; writes go out through Exec::write_file.
#pragma once
#include <stdint.h>

namespace um {

struct Exec {
  // Executable memory: `bytes` at a 4096-aligned address, writable between write_begin and sync; nullptr when
  // full. reset: every block free again (called only with no user voice rendering and no plugin code in use).
  uint8_t* (*alloc)(uint32_t bytes);
  void (*reset)();
  void (*write_begin)();                 // may be null (the Mac's MAP_JIT write toggle)
  void (*sync)(void* p, uint32_t bytes); // after writing code: D-cache clean to PoU, I-cache invalidate, barriers
  void (*barrier)();                     // a render core, before it first runs code placed since its last call (isb); may be null
  uint32_t total;                        // the arena's bytes (for the resources)
  // A small card file written whole (tmp + rename) away from the audio path: the Pi hands it to core 0, the Mac
  // writes it at once. Null: written here.
  bool (*write_file)(const char* path, const void* data, uint32_t n);
  // RW memory for the images' writable parts (data + bss), 4096-aligned, within +-4 GB of the code (ADRP's reach);
  // reset together with the code. Null: a 4 MB block of um_heap (the Pi: code and data both in its low heap).
  uint8_t* (*data_alloc)(uint32_t bytes);
};

constexpr int kSlots = 5;
constexpr int kStore = 32;               // packages listed (the Rytm's list holds 32)
constexpr uint32_t kStateMax = 65536;    // a voice's plugin state at most (the Teensy's is 16 KB without PSRAM)

// Installs core::hooks' user and slot hooks, the arena, and queues the first scan and the current project's map.
// dir: the packages' folder ("/Plugins").
void begin(const Exec* ex, const char* dir = "/Plugins");
bool idle();                // one bounded step of the work below; true when it did something
void rescan();              // the card's packages changed (the web's upload / delete)
// Console: "um" (the list in the log), "um=rescan", "um=assign:<slot 1..5>:<id>" (the installed version, current
// project), "um=clear:<slot>" (explicitly empty), "um=empty" (start this project empty: all five),
// "um=put:<slot>:<id|->:<a.b.c>[:<name>]" (an imported slot list's entry; the package need not be installed).
// "um=save:<slot>:<name>" (a generic preset /PRESETS/<id>/<name>.txt of what the slot's last voice plays now: its
// preset- and offset-role knobs, every knob but PRE when it has neither), "um=presets" (the card's /PRESETS changed:
// every slot's list scanned again).
// Presets (rmp_presets.h, as the Teensy's): /PRESETS/<id>/<name>.txt and <name>.<preset_ext>; 1A's presets flag,
// 1B 0x20 names, the PRE knob's loads between requests with a 1C write-back. API-2 plugins get the project's pool
// (pool_info / pool_read) from the sample RAM; without one, RMP_POOL_EMPTY.
// True when it was one of these; out gets a one-line answer.
// Which machine a slot plays when a project loads: rmp_resolve.h (fingerprints from 0x25 or lazily from trigs, the
// project's map, inheritance, explicit empty); /.rytm/slots/<PROJECT>.inf notes where each assignment came from.
bool command(const char* c, char* out, int cap);
// The status as a JSON array (docs/user-machines-pi.md, "JSON"): published by the control core after each change;
// json_copy gives the last one from any core. Returns its length (0: none yet or cap too small).
int json_copy(char* out, int cap);
void publish_now();         // the control core: the JSON again at once (the Mac's tests; idle() does it at most every 100 ms)
const char* dir();
// The tests' view.
struct SlotView { int state; const char* id; uint32_t version; uint16_t fp; const char* name; int src; int flags; uint16_t want_fp; };
SlotView slot_view(int s);  // s 0..4; state an rmp_slot_state
// The display's view (any core: published with the JSON, the same seqlock): the project and U1..U5 as the Rytm shows
// them - the machine's name, "---" empty, "!NAME" missing / bad / no room, "..." loading. False before the first publish.
struct UiView {
  char project[24];
  struct { char name[12]; uint8_t state; } slot[kSlots];   // state: an rmp_slot_state
};
bool ui_view(UiView* out);
// E0 (core/inst.h): a user machine's instances on voice vi. i: the instance's index in core::inst's pool (0..31),
// each its own state of the package voice vi's slot plays (voice_init / voice_enter on its first note there,
// voice_leave when the slot's image changes or inst_stop), the voice's params as the voice's own state gets them (its
// preset's hold, the offsets). Only for a package that opts in (its header's RMP_HF_INSTANCES; core/inst.cpp asks
// inst_decl first). Control core, but inst_render (a render core).
bool inst_decl(int vi, uint32_t* header_flags, uint32_t* cpu_permille, uint32_t* state_bytes);   // false: no ready slot
bool inst_note_on(int vi, int i, float note, float vel);   // false: not played (no ready slot, a sound-tag mismatch)
void inst_note_off(int vi, int i);
void inst_params(int vi);   // the voice's params changed: its instances take them
void inst_stop(int vi);     // every instance of voice vi leaves its state
void inst_render(int vi, int i, float* out);
int installed();            // packages in the Rytm's list
uint8_t store_gen();
uint32_t code_epoch();      // bumped whenever plugin code is placed (any slot's load): any core may read it

// 2026-10-27 (core/bundle/prjslot.h): the slot map's next writes under another project key (a SAVE AS copy placed)
void rekey(const char* key);
}  // namespace um
