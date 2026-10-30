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
extern PoolEntry pool[kPoolLines];     // RAM2 (26 KB a bank)
// Each slot's file, opened the first time a voice plays it and kept open (a copy per voice that plays it),
// with its index record: a DSP SAMPLE voice switching samples then only reads the new head (RAM2, 19 KB).
#ifdef CORE_HEAP_BUFFERS
extern File* pool_files;   // heap_buffers_place (core/heap_buffers.h)
#else
extern File pool_files[kPoolLines];
#endif
extern card::FileRec pool_recs[kPoolLines];
extern bool pool_open[kPoolLines];
extern int npool;                      // slots in use, empty ones included
// rui1045: DSP SAMPLE's SMP (P11 as the reports give it: the integer the row, the fraction's top 3 bits the bank) ->
// its slot 1..kPoolLines (bank * 127 + row), 0 OFF (row 0 of any bank); without CORE_MSAMP the row (bank A)
int pool_slot(float p11);
extern char project[24];
// 2026-10-27 (core/bundle/prjslot.h): the Rytm's name for the project - project itself in name mode, the name the Rytm
// reports in slot mode (project is then the slot key, "@005"). For display and the sampler's /Samples/<name>/ folder.
extern char project_name[24];
void pool_forget(int line);

// The project (10 <name>), its pool file and the card (moved from voiceloop.ino). Not real time: cold.
extern bool project_dirty;   // /.rytm/project to be written (between requests: idle_save)
extern bool project_seen;
void pool_file(char* out, int max);   // "/.rytm/pools/<PROJECT>.txt", the name made safe for the card
void folder_path(int f, char* out, int max);
bool find_path(const char* path, int* folder, int* file);
void pool_save();
void pool_load();
#if CORE_MSAMP
void pool_sync(bool force = false, bool send = true);   // DSP SAMPLE's slots = the SAMPLES pool's banks (pool.cpp), when it changed
#endif
void project_save();
void set_project(const uint8_t* d, int len);   // 10's name bytes (up to F7)
void project_switch(const char* name);         // the switch a 10 asked for once core/bundle/rename.h has its answer
void project_switch_named(const char* key, const char* name, bool reload);   // core/bundle/prjslot.h's switch
void project_load();
#if CORE_MSAMP
// 2026-10-26, the RAM store (core/store/store.h): a project load fills the whole store before the project counts as
// loaded. project_fill_begin: the store emptied, the project's files wanted (the LFOs', the inserts'), the fill begun
// (switch_to, the boot); project_fill_step: one step of it from the idle pass, and once it is full the LFOs and inserts
// switched to the project (from the store); true on that step. project_loading: a fill not yet applied.
void project_fill_begin();
bool project_fill_step();
bool project_loading();
#endif
void pool_set(int line, int folder, int file, const card::FileRec& r);
void pool_add(int folder, int file);           // 0F LOAD TO PROJ (a sample, a JUNO patch, a DX7 voice)
void pool_assign(int slot, int folder, int file);   // 12 ASSIGN
void send_pool();                              // 14
void send_juno_names();                        // 16
void send_fm_names();                          // 17
void send_listing(int f);                      // 0D -> 0E

}  // namespace core
