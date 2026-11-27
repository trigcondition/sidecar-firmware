// core/pool/pool.h - the card and the project's pool (moved from teensy/voiceloop/voiceloop.ino). The pool
// ("static" samples, as the Octatrack's), one per Rytm project: files added with LOAD TO PROJ (0F) or ASSIGN (12)
// in the Rytm's TEENSY folder while that project is the Rytm's (10 <name>). Kept on the card as text,
// /.rytm/pools/<PROJECT>.txt: slot N (1..127) is line N-1, the file's path under /Samples, an empty line an empty
// slot. DSP SAMPLE's SMP picks slot SMP (0 off); the Rytm gets the table as 14.
#pragma once
#include "../platform.h"
#include "../presets/dx7_lib.h"
#include "../presets/juno_lib.h"
#include "card_index.h"
#include "../project_key.h"
#include "../machines.h"   // kVoiceSlots

namespace core {

extern card::Index cards;
extern bool card_ok;
extern jlib::Lib junolib;   // /JUNO's patches; folder j is TEENSY folder cards.folders() + j
extern dx7::Lib dxlib;      // /DX7's banks and the FM list; node k is TEENSY folder cards.folders() + junolib.folders() + k
inline int dx_base() { return cards.folders() + junolib.folders(); }

constexpr int kPool = 127;
#if CORE_MSAMP
// rui1045: DSP SAMPLE's SMP reaches every bank of the SAMPLES pool: slot = bank * 127 + row (1..127 bank A as before,
// so the console's pool=S:path, the user machines' slots and the 14 table keep meaning bank A); pool[] mirrors all 8
constexpr int kPoolLines = 8 * kPool;
#else
constexpr int kPoolLines = kPool;
#endif
constexpr int kPath = 160;
struct PoolEntry {
  int16_t folder;       // -1: empty, or not on the card
  uint16_t file;
  char name[card::kName];
  char path[kPath];     // under /Samples; "" empty
};
// Each slot's project has its own pool (docs/device-profiles.md "Several devices": nothing of one box's project is
// another's): slot s's DSP SAMPLE slot k (1..kPoolLines) is pool line pool_line(s, k), in [s * kPoolLines, (s + 1) * kPoolLines).
// The arrays below hold every slot's lines; a line is the same number everywhere (the open files, the sample RAM's keys).
constexpr int kPoolAll = kVoiceSlots * kPoolLines;
constexpr int pool_line(int s, int k) { return s * kPoolLines + k - 1; }
extern PoolEntry pool[kPoolAll];     // RAM2 (26 KB a bank)
// Each line's file, opened the first time a voice plays it and kept open (a copy per voice that plays it),
// with its index record: a DSP SAMPLE voice switching samples then only reads the new head (RAM2, 19 KB).
#ifdef CORE_HEAP_BUFFERS
extern File* pool_files;   // heap_buffers_place (core/heap_buffers.h)
#else
extern File pool_files[kPoolAll];
#endif
extern card::FileRec pool_recs[kPoolAll];
extern bool pool_open[kPoolAll];
// rui1045: DSP SAMPLE's SMP (P11 as the reports give it: the integer the row, the fraction's top 3 bits the bank) ->
// its slot 1..kPoolLines (bank * 127 + row), 0 OFF (row 0 of any bank); without CORE_MSAMP the row (bank A)
int pool_slot(float p11);

// One slot's project: slot 0 the Rytm's, slot 1 another box's (core/device/device.h), each named by its own box and kept in
// its own state folder. prj() is the slot in hand's (device::cur(): the box whose message, whose idle pass this is); prj(s)
// slot s's. An engine with one voice slot (the Teensy's) has one project, whichever link is in hand.
struct Project {
  char key[24];    // the project (10 <name>): its files' key; in slot mode the slot key ("@005", core/bundle/prjslot.h)
  // 2026-10-27: the box's name for it - key itself in name mode, the name the Rytm reports in slot mode. For display and the
  // sampler's /Samples/<name>/ folder.
  char name[24];
  char next[24];   // project_switch_named's name for the coming switch
  bool dirty;      // /.rytm/project to be written (between requests: idle_save)
  bool seen;       // its box named it since boot
  uint32_t reported;   // project_id(): a hash of the last project name its box reported (10, slot switch), even if held back
  uint32_t epoch;      // project_id(): its reloads (a cleared slot loaded again, a restore)
  int npool;       // its pool's slots in use, empty ones included
  uint32_t pool_sig;   // (CORE_MSAMP) the SAMPLES list's signature its pool was built from
  bool pool_locked;    // (no CORE_MSAMP) its pool file could not be read whole: never written over
};
extern Project g_projects[kVoiceSlots];
int prj_slot();   // device::cur()'s slot (core/device/device.h), 0 on an engine with one
inline Project& prj(int s) { return g_projects[s >= 0 && s < kVoiceSlots ? s : 0]; }
inline Project& prj() { return g_projects[prj_slot()]; }
void pool_forget(int line);

// The project (10 <name>), its pool file and the card (moved from voiceloop.ino). Not real time: cold.
void pool_file(char* out, int max);   // "/.rytm/pools/<PROJECT>.txt", the name made safe for the card
void folder_path(int f, char* out, int max);
bool find_path(const char* path, int* folder, int* file);
void pool_save();
void pool_load();
#if CORE_MSAMP
void pool_sync(bool force = false, bool send = true);   // DSP SAMPLE's slots = the SAMPLES pool's banks (pool.cpp), when it changed
// sedit-050: a WAV written after boot under /Samples made known to the card index (its row resolves, plays, draws).
// 1 added; 0 nothing to do (outside /Samples, no card); -1 not added (the index's table full, the folder unknown): the
// row plays after the next boot; -2 not added, the file name too long for the index (kName): the row never plays
int pool_card_add(const char* path, int ch, int bits, uint32_t rate, uint32_t frames, uint32_t data_offset);
#endif
void project_save();
void set_project(const uint8_t* d, int len);   // 10's name bytes (up to F7)
// 0.5.0 FILL, the pattern writer and the pattern seed (core/ui/page_fill.cpp, core/seq/patwrite.cpp, seedtake.cpp): which project the Rytm is
// in, as a number to remember and compare later ("the same project" or "another one": an undo, a job or a take made before is gone).
// It is the slot in hand's (prj()): a hash of its project's key, of the last name the Rytm REPORTED (a 10, a slot switch: it moves when the Rytm names
// another project even while the switch is held back, in slot mode or by the rename question) and of a reload count (a cleared slot
// loaded again, a restore: project_switch_named with reload). A repeat of the same report leaves it as it was.
uint32_t project_id();
void project_switch(const char* name);         // the switch a 10 asked for once core/bundle/rename.h has its answer
void project_switch_named(const char* key, const char* name, bool reload);   // core/bundle/prjslot.h's switch
void project_load();
// Slot s's project from its box's folder (slot 1 once a box says hello): one step from idle_save, true while it works
bool project_attach_step(int s);
#if CORE_MSAMP
// 2026-10-26, the RAM store (core/store/store.h): a project load fills the whole store before the project counts as
// loaded. project_fill_begin: the store emptied, the project's files wanted (the LFOs', the inserts'), the fill begun
// (switch_to, the boot); project_fill_step: one step of it from the idle pass, and once it is full the LFOs and inserts
// switched to the project (from the store); true on that step. project_loading: a fill not yet applied.
void project_fill_begin();
bool project_fill_step();
bool project_loading();
void project_lists_step();   // each pass: the JUNO / FM lists of a project just switched to (a card core: requests)
void pool_close_file(int line);   // the line's File closed (with a card core: by pool_closes_step)
bool pool_closing(int line);      // ... not yet: no open of that File before
void pool_closes_step();     // each pass: lines forgotten, their files closed by the card core (one request at a time)
bool lists_busy();           // ... under way: the old lists still there, an edit of them refused
void project_lists_rekey(const char* key);   // ... and the project moved to another key meanwhile
#endif
void pool_set(int line, int folder, int file, const card::FileRec& r);
void pool_add(int folder, int file);           // 0F LOAD TO PROJ (a sample, a JUNO patch, a DX7 voice)
void pool_assign(int slot, int folder, int file);   // 12 ASSIGN
void send_pool();                              // 14
void send_juno_names();                        // 16
void send_fm_names();                          // 17
void send_listing(int f);                      // 0D -> 0E

}  // namespace core
