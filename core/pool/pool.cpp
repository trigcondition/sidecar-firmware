// core/pool/pool.cpp - see pool.h.
#include "pool.h"

#include <string.h>
#include <strings.h>

#include "../fmt/fmt.h"
#include "../hooks.h"
#include "../sysex.h"
#include "../voices.h"
#include "../lfo.h"
#include "../seq/seq.h"
#include "../song/song.h"
#include "../fx/insert.h"
#if CORE_MSAMP
#include "../bundle/kits.h"
#include "../bundle/prjslot.h"
#include "../bundle/projects.h"
#include "../bundle/rename.h"
#include "../granular/granular.h"
#include "../lib/pools.h"
#include "../lib/srclib.h"
#include "../store/store.h"
#endif

namespace core {

card::Index cards;
bool card_ok = false;
jlib::Lib junolib;
dx7::Lib dxlib;
CORE_RAM2 PoolEntry pool[kPoolLines];
#ifdef CORE_HEAP_BUFFERS
File* pool_files = nullptr;   // heap_buffers_place (core/heap_buffers.h)
#else
CORE_RAM2 File pool_files[kPoolLines];
#endif
CORE_RAM2 card::FileRec pool_recs[kPoolLines];
bool pool_open[kPoolLines];
int npool = 0;
char project[24] = "";
char project_name[24] = "";   // 2026-10-27: the Rytm's name for it (slot mode: project is the slot key)
static char g_next_name[24] = "";   // project_switch_named's name for the coming switch_to

CORE_COLD int pool_slot(float p11) {
#if CORE_MSAMP
  int c = srclib::code_of(p11), row = srclib::index_of(c);
  return row ? srclib::bank_of(c) * kPool + row : 0;
#else
  return (int)p11;
#endif
}

CORE_COLD void pool_forget(int line) {
  if (pool_open[line]) pool_files[line].close();
  pool_open[line] = false;
}

// The project's pool file, its name made safe for the card.
CORE_COLD void pool_file(char* out, int max) {
  char n[kKeyMax];
  project_key(project, n, sizeof n);
  sniprintf(out, max, "/.rytm/pools/%s.txt", n);
}

// Folder f's path under /Samples ("" for /Samples itself).
CORE_COLD void folder_path(int f, char* out, int max) {
  if (f <= 0) {
    out[0] = 0;
    return;
  }
  folder_path(cards.folder(f).parent, out, max);
  size_t l = strlen(out);
  sniprintf(out + l, max - l, "%s%s", l ? "/" : "", cards.folder(f).name);
}

// A path under /Samples -> folder and file (name order); false if not there.
CORE_COLD bool find_path(const char* path, int* folder, int* file) {
  int f = 0;
  const char* p = path;
  for (;;) {
    const char* slash = strchr(p, '/');
    if (!slash) break;
    int len = slash - p, next = -1;
    const card::Folder& d = cards.folder(f);
    for (int i = 0; i < d.nsub; ++i) {
      const card::Folder& s = cards.folder(d.first_sub + i);
      if ((int)strlen(s.name) == len && strncmp(s.name, p, len) == 0) {
        next = d.first_sub + i;
        break;
      }
    }
    if (next < 0) return false;
    f = next;
    p = slash + 1;
  }
  card::FileRec r;
  for (int i = 0; i < cards.folder(f).nfiles; ++i)
    if (cards.file(f, i, &r) && strncmp(r.name, p, card::kName) == 0) {
      *folder = f;
      *file = i;
      return true;
    }
  return false;
}

#if CORE_MSAMP
// The pools design 3.6, rui1045: DSP SAMPLE's slots are the SAMPLES pool's eight banks (SMP's 14 bits: row N of bank B is
// slot B * 127 + N; bank A's rows are slots 1..127 as before). pool[] mirrors them - a
// sample row its resolved file (folder / file in the card index when it is under /Samples; else folder -1: the 14
// table sends the name only and the slot streams nothing), an input / built-in / empty / missing row an empty slot.
// pool_sync() rebuilds it when the pool list changes (nm::step); the old /.rytm/pools/<PROJECT>.txt is not read.
uint32_t pool_sig_seen = 0;
CORE_COLD void pool_save() {}
CORE_COLD const char* pools_key() {   // the project's key as the pool lists name it
  static char k[kKeyMax];
  project_key(project, k, sizeof k);
  return k;
}
CORE_COLD void pool_sync(bool force, bool send) {
  uint32_t sg = pools::sig(pools::kSamples);
  if (!force && sg == pool_sig_seen) return;
  pool_sig_seen = sg;
  bool rd = pools::ready() && !strcmp(pools::project(), pools_key());   // (the lists of this project: else empty)
  int n = 0, used = 0, outside = 0;
  bool changed = false;
  for (int i = 0; i < kPoolLines; ++i) {
    const pools::Entry& e = pools::at(pools::kSamples, pools::code(i / kPool, i % kPool + 1));
    char rel[kPath] = "";
    const char* path = rd && e.type == pools::kSample && pools::playable(e) ? e.path : "";
    if (path[0]) {
      if (!strncasecmp(path, "/Samples/", 9)) sniprintf(rel, sizeof rel, "%s", path + 9);
      else sniprintf(rel, sizeof rel, "%s", path), ++outside;
    }
    PoolEntry& pe = pool[i];
    if (strcmp(pe.path, rel)) {
      changed = true;
      pool_forget(i);
      sniprintf(pe.path, sizeof pe.path, "%s", rel);
      pe.folder = -1, pe.file = 0;
      int fo, fi;
      if (rel[0] && rel[0] != '/' && card_ok && cards.folders() > 0 && find_path(rel, &fo, &fi)) pe.folder = (int16_t)fo, pe.file = (uint16_t)fi;
      const char* base = strrchr(path, '/');
      strncpy(pe.name, base ? base + 1 : path, card::kName);
    }
    if (rel[0]) n = i + 1, ++used;
    else pe.folder = -1;
  }
  if (!force && !changed && npool == n) return;
  npool = n;
  if (rd) log(CORE_STR("pool %s: DSP SAMPLE: %d samples (%d outside /Samples: RAM only)"), project[0] ? project : "_default", used, outside);
  for (int i = 0; i < 8; ++i) prepared[i][0] = -2;   // voices: prepare again
  if (hooks.pool_changed) hooks.pool_changed(-1);   // plugin voices too
  if (psram) {
    psram->clear_prefetch();
    for (int i = 0; i < npool; ++i)
      if (pool[i].folder >= 0) psram->want(pool[i].folder, pool[i].file, true);
  }
  if (send) send_pool();
}
CORE_COLD void pool_load() {   // a project change: empty until its SAMPLES pool is read (set_project sends the 14)
  for (int i = 0; i < kPoolLines; ++i) pool_forget(i), pool[i].path[0] = 0, pool[i].folder = -1;
  npool = 0;
  pool_sync(true, false);
}
#else
// 1042 (data safety): the project's pool file is there but could not be opened, or has a line longer than a path this
// build keeps: never written over (before: a failed open loaded an empty pool and the next LOAD TO PROJ saved one slot
// over the whole pool; a long line was written back cut short). The slots still change in memory.
bool pool_locked = false;
CORE_COLD void pool_save() {
  char fn[48];
  pool_file(fn, sizeof fn);
  if (pool_locked) {
    log(CORE_STR("pool: %s not written: it could not be read whole (kept as it is)"), fn);
    return;
  }
  cards.sd().mkdir("/.rytm/pools");
  File f;
  if (!f.open(fn, kWrite)) {
    log(CORE_STR("pool: cannot write %s"), fn);
    return;
  }
  for (int i = 0; i < npool; ++i) {
    f.write(pool[i].path, strlen(pool[i].path));
    f.write('\n');
  }
  f.close();
}

CORE_COLD void pool_load() {
  char fn[48];
  pool_file(fn, sizeof fn);
  for (int i = 0; i < kPool; ++i) pool_forget(i);
  npool = 0;
  pool_locked = false;
  File f;
  if (f.open(fn, kRead)) {
    char line[kPath];
    int n = 0, missing = 0, used = 0, cut = 0;
    for (;;) {
      int c = f.read();
      if (c < 0 && n == 0) break;
      if (c < 0 || c == '\n') {
        if (npool >= kPool) break;
        while (n && line[n - 1] == '\r') --n;
        line[n] = 0;
        PoolEntry& e = pool[npool++];
        strncpy(e.path, line, kPath);
        e.folder = -1;
        e.file = 0;
        const char* base = strrchr(line, '/');
        strncpy(e.name, base ? base + 1 : line, card::kName);
        int fo, fi;
        if (line[0] && find_path(line, &fo, &fi)) {
          e.folder = fo;
          e.file = fi;
          ++used;
        } else if (line[0]) {
          ++missing;
        }
        n = 0;
        if (c < 0) break;
      } else if (n < kPath - 1) {
        line[n++] = c;
      } else {
        ++cut;
      }
    }
    f.close();
    if (cut) pool_locked = true;
    while (npool && !pool[npool - 1].path[0]) --npool;   // trailing empty slots
    if (pool_locked) log(CORE_STR("pool %s: a line longer than %d characters: the file kept as it is (not written)"), fn, kPath - 1);
    log(CORE_STR("pool %s: %d slots, %d samples, %d not on the card"), fn, npool, used, missing);
  } else if (cards.sd().exists(fn)) {
    pool_locked = true;
    log(CORE_STR("pool %s: cannot be read: empty for now, the file kept as it is"), fn);
  } else {
    log(CORE_STR("pool %s: empty"), fn);
  }
  for (int i = 0; i < 8; ++i) prepared[i][0] = -2;   // voices: prepare again
  if (hooks.pool_changed) hooks.pool_changed(-1);   // plugin voices too
  if (psram) {   // the new project's samples into free PSRAM, in slot order
    for (int i = 0; i < 8; ++i)
      if (!from_ram[i]) ram_release(i);   // a sounding note keeps its sample
    psram->clear_prefetch();
    for (int i = 0; i < npool; ++i) psram->want(pool[i].folder, pool[i].file, true);
  }
}

#endif

bool project_dirty = false;

// The project's name to /.rytm/project, so the Teensy starts with its pool. 0.3.0 power-cut safety: posted to the
// card core as one atomic write (fmt.h), so a cut never leaves half a name here - the Sidecar would boot into a
// project that does not exist.
CORE_COLD void project_save() {
  uint32_t n = (uint32_t)strlen(project);
  char* t = fmt::post_begin();
  if (!t || n > fmt::kPostMax) return;   // another settings write is in flight: at a later pass (still dirty)
  memcpy(t, project, n);
  if (fmt::post_end("/.rytm/project", n)) project_dirty = false;
}

// 10 <name>: the Rytm's project; its pool from now on. The table goes to the
// Rytm on the first 10 since start and when the project changes, not on the
// repeats the Rytm sends every few seconds.
bool project_seen = false;

// The switch itself: the old project's unsaved edits into its own files, then the new name's files loaded
CORE_COLD static void switch_to(const char* n) {
#if CORE_POLY
  seq::project_leaving();   // 1042: the old project's unsaved sequence edits, in its own folder (before the name moves)
#endif
  sniprintf(project, sizeof project, "%s", n);
  sniprintf(project_name, sizeof project_name, "%s", g_next_name[0] ? g_next_name : n);
  g_next_name[0] = 0;
  if (strcmp(project, project_name)) log(CORE_STR("project: %s (%s)"), project, project_name);
  else log(CORE_STR("project: %s"), project);
  pool_load();
  junolib.load_list(project);
  juno_relist();
  dxlib.load_list(project);
  fm_relist();
  if (hooks.project_changed) hooks.project_changed();   // the project's machines (the Teensy's plugins)
#if CORE_MSAMP
  // 2026-10-26, the RAM store: the project's LFO, INSERT and sequencer files read into RAM by the card core first; the
  // LFOs, inserts and sequences switch once it is full (project_fill_step, from the idle pass) - until then they are
  // the old project's, its edits saved into its own files
  project_fill_begin();
#else
  lfo::project_changed();   // the project's LFOs (/.rytm/lfo/<project>.txt)
#if CORE_POLY
  seq::project_changed();   // the project's sequences (/.rytm/seq/<project>/)
  ins::project_changed();   // the project's inserts (/.rytm/ins/<project>/, core/fx/insert.h)
#endif
#endif
  project_dirty = true;   // /.rytm/project written from loop() (a 2 ms card write)
}
CORE_COLD static void announce() {   // the Rytm's tables for the project (its first 10, a change)
#if CORE_MSAMP
  projects::note(project, project_name);   // the project index (/.rytm/projects.txt, core/bundle/projects.h): written by idle_save
#endif
  send_pool();
  send_juno_names();
  send_fm_names();
}

CORE_COLD void set_project(const uint8_t* d, int len) {
  char n[24];
  int k = 0;
  for (int i = 0; i < len && k < 23 && d[i] != 0xf7 && d[i]; ++i) n[k++] = d[i] & 0x7f;
  n[k] = 0;
  if (is_slot_key(n)) n[0] = '_';   // 2026-10-27: a Rytm name never names a slot key (today's key for it)
#if CORE_MSAMP
  if (prjslot::take_10(n)) return;   // slot mode (core/bundle/prjslot.h): the 4B reports switch, not the 10
  char rk[kKeyMax];
  if (prjslot::resolve(n, rk, sizeof rk)) {   // name mode on a card keyed by slot (older mods): the slot of that name
    bool first = !project_seen;
    project_seen = true;
    if (strcmp(rk, project) != 0) {
      rename::back();
      sniprintf(g_next_name, sizeof g_next_name, "%s", n);
      switch_to(rk);
    } else if (strcmp(project_name, n)) {
      sniprintf(project_name, sizeof project_name, "%s", n);
    } else if (!first) {
      return;
    }
    announce();
    return;
  }
#endif
  bool first = !project_seen;
  project_seen = true;
  if (strcmp(n, project) != 0) {
#if CORE_MSAMP
    if (rename::intercept(project, n)) {   // UX review B1: no files under the new name, some under the old: asked first
      if (first && rename::busy()) announce();   // (the old project's tables meanwhile)
      return;
    }
#endif
    switch_to(n);
  } else {
#if CORE_MSAMP
    rename::back();   // (a question about another name is moot: the Rytm is back on this one)
#endif
    if (!first) return;
  }
  announce();
}

// core/bundle/rename.cpp's answer: the switch the Rytm's 10 asked for, after the files moved (or not)
CORE_COLD void project_switch(const char* n) {
  if (strcmp(n, project) != 0) switch_to(n);
  announce();
}

// 2026-10-27 (core/bundle/prjslot.h): the switch to a slot key with the Rytm's name for it; reload: the same key again
// (its files changed under it: a cleared slot reloaded, a restore). A new name for the same key only renames it.
CORE_COLD void project_switch_named(const char* key, const char* name, bool reload) {
  project_seen = true;
  if (reload || strcmp(key, project) != 0) {
    sniprintf(g_next_name, sizeof g_next_name, "%s", name ? name : key);
    switch_to(key);
  } else {
    sniprintf(project_name, sizeof project_name, "%s", name ? name : key);
  }
  announce();
}

#if CORE_MSAMP
static bool g_loading = false;   // a fill begun whose project the LFOs and inserts are not switched to yet

// (a fill still running is abandoned by fill_begin: a switch while one runs reads only the new project's files)
CORE_COLD void project_fill_begin() {
  store::clear();
  lfo::want_files();
  kits::want_files();   // (every kit's saved and kept-aside record: core/bundle/kits.h)
#if CORE_POLY
  ins::want_files();
  seq::want_files();   // (phase 2: tracks.txt, the patterns, their saved states)
  song::want_files();  // (UNDO REPLAY's backup slots: .backups.txt)
  seq::loading_begin();   // (its edits refused until project_changed: they would be lost to the load)
#endif
  store::fill_begin();
  g_loading = true;
}

CORE_COLD bool project_fill_step() {
  if (!g_loading || !store::fill_step()) return false;
  g_loading = false;
  // the old project's unsaved edits into its own files (each project_changed saves before it loads), then the
  // project's, from the store; a kit the Rytm named while it filled (kit_changed: noted only) is the one loaded
  lfo::project_changed();
#if CORE_POLY
  ins::project_changed();
  seq::project_changed();   // (the boot's too: seq::init leaves its tracks and A01 to this)
#endif
  log(CORE_STR("store: %s loaded: %d files of %d paths, %u bytes"), project, store::present(), store::entries(),
      (unsigned)store::bytes());
  return true;
}

bool project_loading() { return g_loading; }
#endif

CORE_COLD void project_load() {
  File f;
  project[0] = 0;
  fmt::recover("/.rytm/project");   // 0.3.0: a cut between write_atomic's renames - the previous name back (fmt.h)
  if (f.open("/.rytm/project", kRead)) {
    int n = f.read(project, sizeof project - 1);
    project[n > 0 ? n : 0] = 0;
    f.close();
  }
  log(CORE_STR("project: %s"), project[0] ? project : "(none yet)");
  sniprintf(project_name, sizeof project_name, "%s", project);
#if CORE_MSAMP
  projects::load();
  prjslot::load();   // /.rytm/prjslot.txt (slot mode, the trash's counter)
  if (const projects::Info* e = projects::find(project))
    if (e->rytm[0]) sniprintf(project_name, sizeof project_name, "%s", e->rytm);   // (a slot key: the Rytm's name)
#endif
  pool_load();
  junolib.load_list(project);
  dxlib.load_list(project);
}

#if CORE_MSAMP
// 0F / 12 into the SAMPLES pool's bank A (the pool list decides: one entry per hash, a file there already keeps its row)
CORE_COLD void pool_add(int folder, int file) {
  if (card_ok && folder >= dx_base()) {
    if (dxlib.add(folder - dx_base(), file) >= 0) {
      send_fm_names();
      fm_relist();
    }
    return;
  }
  if (card_ok && folder >= cards.folders()) {
    if (junolib.add(folder - cards.folders(), file) >= 0) {
      send_juno_names();
      juno_relist();
    }
    return;
  }
  card::FileRec r;
  char path[kPath + 16];
  if (card_ok && cards.file(folder, file, &r)) {
    char rel[kPath];
    folder_path(folder, rel, sizeof rel);
    sniprintf(path, sizeof path, "/Samples/%s%s%s", rel, rel[0] ? "/" : "", r.name);
    int out = -1;
    int res = pools::add_file(pools::kSamples, 0, path, &out);
    log(CORE_STR("pool: LOAD TO PROJ %s: %s"), path, pools::result_text(res));
  }
  pool_sync(true);
}
CORE_COLD void pool_assign(int slot, int folder, int file) {
  card::FileRec r;
  if (card_ok && folder >= cards.folders()) {
    log(CORE_STR("pool: assign slot %d <- JUNO/DX7 entry %d/%d: not a sample, ignored"), slot, folder, file);
  } else if (slot < 1 || slot > kPool || !card_ok || !cards.file(folder, file, &r)) {
    log(CORE_STR("pool: assign slot %d <- folder %d file %d: ignored"), slot, folder, file);
  } else {
    char rel[kPath], path[kPath + 16];
    folder_path(folder, rel, sizeof rel);
    sniprintf(path, sizeof path, "/Samples/%s%s%s", rel, rel[0] ? "/" : "", r.name);
    int out = -1;   // the Rytm's own ASSIGN: an explicit choice on its screen (no prompt there): forced
    int res = pools::set_file(pools::kSamples, pools::code(0, slot), path, &out, pools::kForced);
    log(CORE_STR("pool: ASSIGN A%03d %s: %s"), slot, path, pools::result_text(res));
  }
  pool_sync(true);
}
#else
// Pool line `line` (slot line + 1) = folder's file r.
CORE_COLD void pool_set(int line, int folder, int file, const card::FileRec& r) {
  for (; npool <= line; ++npool) {   // the pool grows: empty slots up to it
    pool[npool].path[0] = 0;
    pool[npool].folder = -1;
  }
  PoolEntry& e = pool[line];
  pool_forget(line);
  folder_path(folder, e.path, kPath);
  size_t l = strlen(e.path);
  sniprintf(e.path + l, kPath - l, "%s%s", l ? "/" : "", r.name);
  e.folder = folder;
  e.file = file;
  strncpy(e.name, r.name, card::kName);
  pool_save();
  for (int i = 0; i < 8; ++i) prepared[i][0] = -2;   // voices: prepare again
  if (hooks.pool_changed) hooks.pool_changed(line);   // plugin voices reading this slot (or refused it) too
#if CORE_MSAMP
  gr::rescan();   // GRANULAR lists the pool's files (POOL)
#endif
  log(CORE_STR("pool %s: slot %d = %s"), project[0] ? project : "_default", line + 1, e.path);
}

// 0F <folder> <file>: LOAD TO PROJ - into the first empty slot (not again if
// it is in the pool already); a JUNO patch into the project's JUNO list, a
// DX7 voice into its FM list.
CORE_COLD void pool_add(int folder, int file) {
  if (card_ok && folder >= dx_base()) {
    if (dxlib.add(folder - dx_base(), file) >= 0) {
      send_fm_names();
      fm_relist();   // a voice's PAT may point at the new slot, or the list replaced the ROM's
    }
    return;
  }
  if (card_ok && folder >= cards.folders()) {
    if (junolib.add(folder - cards.folders(), file) >= 0) {
      send_juno_names();
      juno_relist();   // a voice's PAT may point at the new slot
    }
    return;
  }
  card::FileRec r;
  if (card_ok && cards.file(folder, file, &r)) {
    bool there = false;
    for (int i = 0; i < npool && !there; ++i) there = pool[i].folder == folder && pool[i].file == file;
    int line = 0;
    while (line < npool && pool[line].path[0]) ++line;   // the first empty slot
    if (!there && line < kPool) pool_set(line, folder, file, r);
  }
  send_pool();
}

// 12 <slot> <folder> <file>: ASSIGN - pool slot 1..127 = that file, whatever
// it held (the file may be in other slots too).
CORE_COLD void pool_assign(int slot, int folder, int file) {
  card::FileRec r;
  if (card_ok && folder >= cards.folders())
    log(CORE_STR("pool: assign slot %d <- JUNO/DX7 entry %d/%d: not a sample, ignored"), slot, folder, file);
  else if (slot < 1 || slot > kPool || !card_ok || !cards.file(folder, file, &r))
    log(CORE_STR("pool: assign slot %d <- folder %d file %d: ignored"), slot, folder, file);
  else
    pool_set(slot - 1, folder, file, r);
  send_pool();
}

#endif

// 14 <replace> <count> { <slot> <folder hi lo> <file hi lo> <13 name bytes> }:
// the project's pool table, non-empty slots in order, 6 a message; the first
// replaces the Rytm's table. Name: the file's without extension, printable
// ASCII (else '_'), NUL padded; folder/file 0x3fff when it is not on the card.
CORE_COLD void send_pool() {   // rui1045: bank A only (the Rytm builds before rui1045 show it on DSP SAMPLE's SMP)
  int line = 0, first = 1, end = npool < kPool ? npool : kPool;
  do {
    PoolRow rows[6];
    int count = 0;
    for (; line < end && count < 6; ++line) {
      const PoolEntry& e = pool[line];
      if (!e.path[0]) continue;
      int len = strnlen(e.name, card::kName);   // a long name may fill it, no NUL
      for (int i = len - 1; i > 0; --i)
        if (e.name[i] == '.') { len = i; break; }
      rows[count++] = {(uint8_t)(line + 1), (uint16_t)(e.folder < 0 ? 0x3fff : e.folder),
                       (uint16_t)(e.folder < 0 ? 0x3fff : e.file), e.name, len};
    }
    while (line < end && !pool[line].path[0]) ++line;   // so the last message is not empty
    uint8_t m[8 + 6 * 18];
    to_rytm(m, sysex_pool(m, first, rows, count));
    first = 0;
  } while (line < end);
}

// 16 <replace> <total> { <slot> <13 name bytes> }: the project's JUNO list's
// names, every slot 0..total-1, 8 a message; the first replaces the Rytm's
// list; total 0 (no list): one message, no entries. An empty slot: 13 NULs.
CORE_COLD void send_juno_names() {
  int total = junolib.listed(), s = 0, first = 1;
  do {
    const char* names[8];
    int k = 0, s0 = s;
    for (; k < 8 && s < total; ++k, ++s) {
      const jlib::Patch& p = junolib.slot(s);
      names[k] = p.used ? p.name : nullptr;
    }
    uint8_t m[8 + 8 * 14];
    to_rytm(m, sysex_names(m, 0x16, first, total, s0, names, k));
    first = 0;
  } while (s < total);
}

// 17 <replace> <total> { <slot> <13 name bytes> }: the effective FM list's
// names as 16 - the project's FM list, or Plaits' 96 ROM voices when it is
// empty.
CORE_COLD void send_fm_names() {
  int nl = dxlib.listed(), total = nl ? nl : dx7::kRomVoices, s = 0, first = 1;
  do {
    char rom[8][11];
    const char* names[8];
    int k = 0, s0 = s;
    for (; k < 8 && s < total; ++k, ++s) {
      names[k] = "";
      if (nl) {
        if (dxlib.slot(s).used) names[k] = dxlib.slot(s).name;
      } else {
        dx7::voice_name(dx7::rom_bank(s / 32) + 128 * (s % 32) + 118, rom[k]);
        names[k] = rom[k];
      }
    }
    uint8_t m[8 + 8 * 14];
    to_rytm(m, sysex_names(m, 0x17, first, total, s0, names, k));
    first = 0;
  } while (s < total);
}

// 0D <folder>: the folder's entries for rytm-mods 0023's TEENSY folder in the
// Rytm's file browser: 0E <folder hi lo> <parent hi lo> <first hi lo> <total hi
// lo>, per entry <kind> <id hi lo> <size 4 x 7 bits> <name length> <name>, 6 a
// message. Subfolders (kind 1, id: the index's folder number), then files
// (kind 0, id: position in name order, size in bytes). Folder 0 is /Samples.
// Entry i of TEENSY folder f for send_listing: kind (1 a folder), id, size,
// name. Folders below cards.folders() are /Samples' (the card index); the rest
// /JUNO's (juno_lib.h), shown as a folder JUNO first in /Samples' root.
namespace {

struct Ent { uint8_t kind; uint32_t id, size; char name[24]; };

uint16_t lsub[512];

int lns = 0, lparent = 0;

CORE_COLD int list_folder(int f) {
  int base = cards.folders(), dbase = dx_base();
  if (f >= dbase) {   // /DX7: a folder's folders and banks, or a bank's 32 voices
    int k = f - dbase;
    lns = dxlib.children(k, lsub, 512);
    lparent = k ? dbase + dxlib.node(k).parent : 0;
    return lns + (dxlib.node(k).bank ? 32 : 0);
  }
  if (f >= base) {
    int j = f - base;
    int total = junolib.count(j, lsub, 512, &lns);
    lparent = j ? base + junolib.folder(j).parent : 0;
    return total;
  }
  bool juno = f == 0 && junolib.folders() > 0;
  bool dx = f == 0 && dxlib.nodes() > 0;
  lns = cards.subfolders(f, lsub + juno + dx, 510) + juno + dx;
  if (juno) lsub[0] = 0xffff;   // JUNO
  if (dx) lsub[juno] = 0xfffe;  // DX7
  int nf = (int)cards.folder(f).nfiles < 512 - lns ? (int)cards.folder(f).nfiles : 512 - lns;
  lparent = f ? cards.folder(f).parent : 0;
  return lns + nf;
}

CORE_COLD bool list_entry(int f, int i, Ent* e) {
  int base = cards.folders(), dbase = dx_base();
  const char* name;
  char buf[24];
  e->size = 0;
  if (i < lns) {
    e->kind = 1;
    if (f >= dbase) {
      e->id = dbase + lsub[i];
      name = dxlib.node(lsub[i]).name;
    } else if (f >= base) {
      e->id = base + lsub[i];
      name = junolib.folder(lsub[i]).name;
    } else if (lsub[i] == 0xffff) {
      e->id = base;
      name = "JUNO";
    } else if (lsub[i] == 0xfffe) {
      e->id = dbase;
      name = "DX7";
    } else {
      e->id = lsub[i];
      name = cards.folder(lsub[i]).name;
    }
  } else if (f >= dbase) {   // a bank's voice: its 10-character name
    char raw[11];
    if (!dxlib.name(f - dbase, i - lns, raw)) return false;
    dx7::voice_name((const uint8_t*)raw, buf);
    if (!buf[0]) sniprintf(buf, sizeof buf, "VOICE %d", i - lns + 1);
    e->kind = 0;
    e->id = i - lns;
    e->size = 128;
    name = buf;
  } else if (f >= base) {
    if (!junolib.patch(f - base, i - lns, buf, nullptr)) return false;
    e->kind = 0;
    e->id = i - lns;
    e->size = 24;
    name = buf;
  } else {
    card::FileRec r;
    if (!cards.file(f, i - lns, &r)) return false;
    e->kind = 0;
    e->id = i - lns;
    e->size = r.size;
    name = r.name;
  }
  int len = 0;
  for (const char* q = name; *q && len < 23; ++q) e->name[len++] = (*q >= 32 && *q < 127) ? *q : '?';
  if (!e->kind && f < base && len > 4 && strncasecmp(e->name + len - 4, ".wav", 4) == 0) len -= 4;
  e->name[len] = 0;
  return true;
}

}  // namespace

// 0D <folder>: the folder's entries for rytm-mods 0023's TEENSY folder in the
// Rytm's file browser: 0E <folder hi lo> <parent hi lo> <first hi lo> <total hi
// lo>, per entry <kind> <id hi lo> <size 4 x 7 bits> <name length> <name>, 6 a
// message. Subfolders (kind 1, id: the folder number), then files or patches
// (kind 0, id: position in name order, size in bytes). Folder 0 is /Samples.
//
// With no card (the Pi without a Samples folder, or no card index yet) the answer is an empty folder (total 0),
// so the Rytm's UI task, which waits for this answer (0023), is never left waiting for one that does not come.
CORE_COLD void send_listing(int f) {
  if (!card_ok && f >= 0) {
    uint8_t m[32];
    to_rytm(m, sysex_listing(m, f, 0, 0, 0, nullptr, 0));
    return;
  }
  if (!card_ok || f < 0 || f >= dx_base() + dxlib.nodes()) return;
  int total = list_folder(f);
  int parent = lparent;
  for (int first = 0; first < (total > 1 ? total : 1); first += 6) {
    ListEntry ents[6];
    Ent e[6];
    int count = 0;
    for (int i = first; i < total && i < first + 6; ++i) {
      if (!list_entry(f, i, &e[count])) continue;
      ents[count] = {e[count].kind, e[count].id, e[count].size, e[count].name};
      ++count;
    }
    uint8_t m[256];
    to_rytm(m, sysex_listing(m, f, parent, first, total, ents, count));
  }
}

}  // namespace core
