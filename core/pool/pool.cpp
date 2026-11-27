// core/pool/pool.cpp - see pool.h.
#include "pool.h"

#include <string.h>
#include <strings.h>

#include "../device/device.h"
#include "../fmt/fmt.h"
#include "../hooks.h"
#include "../sysex.h"
#include "../voices.h"
#include "../lfo.h"
#include "../seq/seq.h"
#include "../song/song.h"
#include "../fx/insert.h"
#include "../fx/master.h"
#include "../fx/out.h"
#include "../fx/send_fx.h"
#include "../host_link.h"
#include "../keys.h"
#include "../loop/loop.h"
#include "../mod/learn.h"
#include "../samp/preview.h"
#include "../mod/scene.h"
#include "../mod/xypad.h"
#if CORE_MSAMP
#include "../bundle/kits.h"
#include "../bundle/prjslot.h"
#include "../bundle/projects.h"
#include "../bundle/rename.h"
#include "../granular/granular.h"
#include "../lib/pools.h"
#include "../lib/srclib.h"
#include "../store/store.h"
#include "../msamp/cardq.h"
#include "../samp/sedit.h"
#include "../ui/remote.h"
#include "../presets/preset_index.h"
#endif

namespace core {

card::Index cards;
bool card_ok = false;
jlib::Lib junolib;
dx7::Lib dxlib;
CORE_RAM2 PoolEntry pool[kPoolAll];
#ifdef CORE_HEAP_BUFFERS
File* pool_files = nullptr;   // heap_buffers_place (core/heap_buffers.h)
#else
CORE_RAM2 File pool_files[kPoolAll];
#endif
CORE_RAM2 card::FileRec pool_recs[kPoolAll];
bool pool_open[kPoolAll];
Project g_projects[kVoiceSlots];
CORE_COLD int prj_slot() {
  int s = device::cur().index;
  return s < kVoiceSlots ? s : 0;
}
// 10b: the FX settings' slot. On the control core the slot in hand's (prj_slot); on any other core (the Pi's touchscreen
// views, core 0: pi/engine/insfx_pi.cpp) the slot the display follows (ui::remote_display_slot), not whatever the
// control core has in hand at that moment.
static int fx_slot() {
  if (!hooks.control_core || hooks.control_core()) return prj_slot();
  return ui::remote_display_slot();
}
#if CORE_POLY
// 3(c): the INSERT and OUT settings are the slot in hand's (core/fx/insert.h, out.h: their control side builds without
// core/device, so it asks through these)
static struct SlotFns {
  SlotFns() { ins::slot_fn = fx_slot, out::slot_fn = fx_slot, ins::rytm_fn = device::rytm_state_at, loop::set_slot_fn(fx_slot); }   // (the looper: core/loop)
} g_slot_fns;
#endif
// 3(c) step 8: the send FX settings too (core/fx/send_fx.h; its control side builds without core/device)
static struct FxSlotFn {
  FxSlotFn() { fx::slot_fn = fx_slot; }
} g_fx_slot_fn;
#if CORE_POLY
// the Rytm's report 50 (core/fx/master.h rytm()): its slot's master control, whichever slot it holds
static int rytm_home_slot() { const int s = device::rytm_home(); return s < kVoiceSlots ? s : 0; }
static struct MasterRytmFn {
  MasterRytmFn() { master::rytm_slot_fn = rytm_home_slot; }
} g_master_rytm_fn;
#endif

CORE_COLD int pool_slot(float p11) {
#if CORE_MSAMP
  int c = srclib::code_of(p11), row = srclib::index_of(c);
  return row ? srclib::bank_of(c) * kPool + row : 0;
#else
  return (int)p11;
#endif
}

// A line's file closed. A close takes FatFs's lock, so with a card core it is a request: the line goes at once, its File is
// closed a little later (pool_closes_step), and open_on_card waits for that.
#if CORE_MSAMP
static bool g_closing[kPoolAll];
static int g_nclosing = 0;
static int g_close_line = -1;
static cardq::Req g_close_rq;
CORE_COLD void pool_close_file(int line) {
  if (!cardq::worker()) {
    pool_files[line].close();
    return;
  }
  if (g_closing[line]) return;
  g_closing[line] = true;
  ++g_nclosing;
}
CORE_COLD bool pool_closing(int line) { return g_closing[line]; }
CORE_COLD void pool_closes_step() {
  if (!g_nclosing) return;
  if (g_close_line < 0) {
    for (int i = 0; i < kPoolAll && g_close_line < 0; ++i)
      if (g_closing[i]) g_close_line = i;
  }
  if (!cardq::async(&g_close_rq, cardq::kCloseReq, &pool_files[g_close_line], nullptr, 0, nullptr, 0)) return;
  g_closing[g_close_line] = false;
  --g_nclosing;
  g_close_line = -1;
}
#else
CORE_COLD void pool_close_file(int line) { pool_files[line].close(); }
CORE_COLD bool pool_closing(int) { return false; }
CORE_COLD void pool_closes_step() {}
#endif
CORE_COLD void pool_forget(int line) {
  if (pool_open[line]) pool_close_file(line);
  pool_open[line] = false;
}

// The project's pool file, its name made safe for the card.
CORE_COLD void pool_file(char* out, int max) {
  char n[kKeyMax];
  project_key(prj().key, n, sizeof n);
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

// A path under /Samples -> its folder and the name's place in path (false: a folder not there)
static CORE_COLD bool find_folder(const char* path, int* folder, const char** leaf) {
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
  *folder = f, *leaf = p;
  return true;
}

// A path under /Samples -> folder and file (name order); false if not there.
CORE_COLD bool find_path(const char* path, int* folder, int* file) {
  int f;
  const char* p;
  if (!find_folder(path, &f, &p)) return false;
  card::FileRec r;
  const int n = cards.folder(f).nfiles;
#if CORE_MSAMP
  // the files added since boot first (cards.add_file): one there is newer than a scanned file of the same name (a file
  // written again after boot)
  for (int i = n, all = cards.files_in(f); i < all; ++i)
    if (cards.file(f, i, &r) && strncmp(r.name, p, card::kName) == 0) {
      *folder = f;
      *file = i;
      return true;
    }
#endif
  for (int i = 0; i < n; ++i)
    if (cards.file(f, i, &r) && strncmp(r.name, p, card::kName) == 0) {
      *folder = f;
      *file = i;
      return true;
    }
  return false;
}

#if CORE_MSAMP
// sedit-050 #5: a WAV written after boot (/Samples/...: the sample editor's copy) made known to the card index, so a
// pool row on it resolves (pool_sync) and plays, and the WAVE page reads it. A path outside /Samples: nothing to do.
CORE_COLD int pool_card_add(const char* path, int ch, int bits, uint32_t rate, uint32_t frames, uint32_t data_offset) {
  if (!card_ok || cards.folders() <= 0 || strncasecmp(path, "/Samples/", 9)) return 0;
  int f;
  const char* leaf;
  if (!find_folder(path + 9, &f, &leaf)) return -1;
  if (strlen(leaf) >= (size_t)card::kName) return -2;   // (the index cuts names: never found by its path)
  card::FileRec r;
  memset(&r, 0, sizeof r);
  sniprintf(r.name, sizeof r.name, "%s", leaf);
  r.dir_index = 0xFFFFFFFFu;   // (unknown: opened by its path, on the card core)
  r.format = 1, r.channels = (uint8_t)ch, r.bits = (uint8_t)bits, r.contiguous = 0;
  r.data_offset = data_offset, r.data_len = frames * (uint32_t)ch * (uint32_t)(bits / 8), r.rate = rate;
  r.size = r.data_offset + r.data_len;
  return cards.add_file(f, r) >= 0 ? 1 : -1;
}
#endif

#if CORE_MSAMP
// The pools design 3.6, rui1045: DSP SAMPLE's slots are the SAMPLES pool's eight banks (SMP's 14 bits: row N of bank B is
// slot B * 127 + N; bank A's rows are slots 1..127 as before). pool[] mirrors them - a
// sample row its resolved file (folder / file in the card index when it is under /Samples; else folder -1: the 14
// table sends the name only and the slot streams nothing), an input / built-in / empty / missing row an empty slot.
// pool_sync() rebuilds it when the pool list changes (nm::step); the old /.rytm/pools/<PROJECT>.txt is not read.
CORE_COLD void pool_save() {}
CORE_COLD const char* pools_key() {   // the project's key as the pool lists name it
  static char k[kKeyMax];
  project_key(prj().key, k, sizeof k);
  return k;
}
CORE_COLD void pool_sync(bool force, bool send) {
  Project& j = prj();
  PoolEntry* const lines = pool + pool_line(prj_slot(), 1);   // (this slot's)
  const int base = pool_line(prj_slot(), 1);
  uint32_t sg = pools::sig(pools::kSamples);
  if (!force && sg == j.pool_sig) return;
  j.pool_sig = sg;
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
    PoolEntry& pe = lines[i];
    if (strcmp(pe.path, rel)) {
      changed = true;
      pool_forget(base + i);
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
  if (!force && !changed && j.npool == n) return;
  j.npool = n;
  if (rd) log(CORE_STR("pool %s: DSP SAMPLE: %d samples (%d outside /Samples: RAM only)"), j.key[0] ? j.key : "_default", used, outside);
  for (int i = 0; i < kVoices; ++i) prepared[i][0] = -2;   // voices: prepare again
  if (hooks.pool_changed) hooks.pool_changed(-1);   // plugin voices too
  if (send) send_pool();
}
CORE_COLD void pool_load() {   // a project change: empty until its SAMPLES pool is read (set_project sends the 14)
  const int b = pool_line(prj_slot(), 1);
  for (int i = b; i < b + kPoolLines; ++i) pool_forget(i), pool[i].path[0] = 0, pool[i].folder = -1;
  prj().npool = 0;
  pool_sync(true, false);
}
#else
static_assert(kVoiceSlots == 1, "a pool file per project: one voice slot (the Teensy's)");
// 1042 (data safety): the project's pool file is there but could not be opened, or has a line longer than a path this
// build keeps: never written over (before: a failed open loaded an empty pool and the next LOAD TO PROJ saved one slot
// over the whole pool; a long line was written back cut short). The slots still change in memory.
CORE_COLD void pool_save() {
  char fn[48];
  pool_file(fn, sizeof fn);
  if (prj().pool_locked) {
    log(CORE_STR("pool: %s not written: it could not be read whole (kept as it is)"), fn);
    return;
  }
  cards.sd().mkdir("/.rytm/pools");
  File f;
  if (!f.open(fn, kWrite)) {
    log(CORE_STR("pool: cannot write %s"), fn);
    return;
  }
  for (int i = 0; i < prj().npool; ++i) {
    f.write(pool[i].path, strlen(pool[i].path));
    f.write('\n');
  }
  f.close();
}

CORE_COLD void pool_load() {
  char fn[48];
  pool_file(fn, sizeof fn);
  for (int i = 0; i < kPool; ++i) pool_forget(i);
  prj().npool = 0;
  prj().pool_locked = false;
  File f;
  if (f.open(fn, kRead)) {
    char line[kPath];
    int n = 0, missing = 0, used = 0, cut = 0;
    for (;;) {
      int c = f.read();
      if (c < 0 && n == 0) break;
      if (c < 0 || c == '\n') {
        if (prj().npool >= kPool) break;
        while (n && line[n - 1] == '\r') --n;
        line[n] = 0;
        PoolEntry& e = pool[prj().npool++];
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
    if (cut) prj().pool_locked = true;
    while (prj().npool && !pool[prj().npool - 1].path[0]) --prj().npool;   // trailing empty slots
    if (prj().pool_locked) log(CORE_STR("pool %s: a line longer than %d characters: the file kept as it is (not written)"), fn, kPath - 1);
    log(CORE_STR("pool %s: %d slots, %d samples, %d not on the card"), fn, prj().npool, used, missing);
  } else if (cards.sd().exists(fn)) {
    prj().pool_locked = true;
    log(CORE_STR("pool %s: cannot be read: empty for now, the file kept as it is"), fn);
  } else {
    log(CORE_STR("pool %s: empty"), fn);
  }
  for (int i = 0; i < kVoices; ++i) prepared[i][0] = -2;   // voices: prepare again
  if (hooks.pool_changed) hooks.pool_changed(-1);   // plugin voices too
  if (psram) {   // the new project's samples into free PSRAM, in slot order
    for (int i = 0; i < 8; ++i)
      if (!from_ram[i]) ram_release(i);   // a sounding note keeps its sample
    psram->clear_prefetch();
    for (int i = 0; i < prj().npool; ++i) psram->want(pool[i].folder, pool[i].file, true);
  }
}

#endif


// The project's name to /.rytm/project, so the Teensy starts with its pool. 0.3.0 power-cut safety: posted to the
// card core as one atomic write (fmt.h), so a cut never leaves half a name here - the Sidecar would boot into a
// project that does not exist.
CORE_COLD void project_save() {
  uint32_t n = (uint32_t)strlen(prj().key);
  char* t = fmt::post_begin();
  if (!t || n > fmt::kPostMax) return;   // another settings write is in flight: at a later pass (still dirty)
  memcpy(t, prj().key, n);
  if (fmt::post_end("/.rytm/project", n)) prj().dirty = false;
}

// 10 <name>: the Rytm's project; its pool from now on. The table goes to the
// Rytm on the first 10 since start and when the project changes, not on the
// repeats the Rytm sends every few seconds.

#if CORE_MSAMP
// 7z (2026-10-31): the project's JUNO and FM lists read by the card core, a bounded piece a request, into a copy: the list file,
// 16 banks' MD5s, a slot's voice from the card. Until the copy is whole the old project's lists stay (and their file names: an
// edit made then is held or refused, lists_busy); taken in at once with the voices on their PAT and the names to the Rytm.
namespace {
enum : uint8_t { kLsIdle, kLsJuno, kLsFm, kLsBanks, kLsVoices };
uint8_t g_ls = kLsIdle;
bool g_ls_again = false;
// One reader for every voice slot's lists (each box's project its own: junolib / dxlib hold a list per slot): the slot
// being read (its Scope for the card requests and the take), the others' reads waiting their turn
int g_ls_slot = 0;
bool g_ls_want[kVoiceSlots];
int g_ls_k = 0;
cardq::Req g_lrq;
char g_lj_file[48], g_lf_file[48];
jlib::Patch g_lj[jlib::kMaxList];
dx7::Slot g_lf[dx7::kMaxList];
uint8_t g_lv[dx7::kMaxList][128];
bool g_lc[dx7::kMaxList];
int g_lj_n = 0, g_lf_n = 0;
bool g_lf_old = false;   // voices read from their banks: a list from before 0.4.0, written again with them
bool g_lf_whole = true;  // ... unless its read failed: a part of a list is never written over the file
struct HeldAdd { int folder, file, slot; };
HeldAdd g_held_add[4];
int g_nheld_add = 0;
constexpr int kBanksAPiece = 16;
void lists_card(cardq::Req*) {   // the card core
  if (g_ls == kLsJuno) jlib::Lib::read_list(g_lj_file, g_lj, &g_lj_n);
  else if (g_ls == kLsFm) g_lf_whole = dx7::Lib::read_list(g_lf_file, g_lf, &g_lf_n, g_lv, g_lc);
  else if (g_ls == kLsBanks) dxlib.resolve(g_lf, g_lf_n, g_ls_k, g_ls_k + kBanksAPiece);
  else
    for (; g_ls_k < g_lf_n; ++g_ls_k)   // (voices the file holds, ROM voices: no card time; up to the next card read)
      if (!g_lc[g_ls_k] && dxlib.cache(g_lf, g_lf_n, g_ls_k, g_lv[g_ls_k], &g_lc[g_ls_k])) {
        g_lf_old = true;
        break;
      }
}
void lists_start(int s) {   // slot s's project's lists from now on
  g_ls_slot = s, g_ls_want[s] = false;
  jlib::Lib::list_path(prj(s).key, g_lj_file, sizeof g_lj_file);
  dx7::Lib::list_path(prj(s).key, g_lf_file, sizeof g_lf_file);
  g_ls = kLsJuno, g_ls_again = false;
}
void lists_begin() {   // the slot in hand's
  const int s = prj_slot();
  if (!g_ls) {
    lists_start(s);
  } else if (g_ls_slot == s) {
    jlib::Lib::list_path(prj(s).key, g_lj_file, sizeof g_lj_file);
    dx7::Lib::list_path(prj(s).key, g_lf_file, sizeof g_lf_file);
    g_ls_again = true;
  } else {
    g_ls_want[s] = true;   // (after the other slot's read: its key then)
  }
}
void lists_done() {
  g_ls = kLsIdle;
  {
    device::Scope in(device::g_links[g_ls_slot]);   // (the slot's own lists, its voices, its box's held adds)
    junolib.take_list(g_lj_file, g_lj, g_lj_n);
    juno_relist();
    dxlib.take_list(g_lf_file, g_lf, g_lf_n, g_lv, g_lc);
    if (g_lf_old && g_lf_whole) dxlib.resave();
    g_lf_old = false;
    fm_relist();
    if (device::rytm_at(g_ls_slot)) {   // the names to the Rytm (its JUNO / FM lists' 16 / 17)
      send_juno_names();
      send_fm_names();
    }
    int k = 0;
    for (int i = 0; i < g_nheld_add; ++i)
      if (g_held_add[i].slot == g_ls_slot) pool_add(g_held_add[i].folder, g_held_add[i].file);
      else g_held_add[k++] = g_held_add[i];
    g_nheld_add = k;
  }
  for (int s = 0; s < kVoiceSlots; ++s)
    if (g_ls_want[s]) {
      lists_start(s);
      break;
    }
}
}  // namespace
bool lists_busy() {   // the slot in hand's lists are being read (or wait for the reader)
  const int s = prj_slot();
  return (g_ls != kLsIdle && g_ls_slot == s) || g_ls_want[s];
}
// the project moved to another key during the read (core/bundle/prjslot.cpp rekey_all): read again there, take_list then keeps it
CORE_COLD void project_lists_rekey(const char* key) {
  if (!g_ls || g_ls_slot != prj_slot()) return;   // (a waiting read takes the slot's key when it starts)
  jlib::Lib::list_path(key, g_lj_file, sizeof g_lj_file);
  dx7::Lib::list_path(key, g_lf_file, sizeof g_lf_file);
  g_ls_again = true;
}
CORE_COLD void project_lists_step() {
  if (!g_ls) return;
  device::Scope in(device::g_links[g_ls_slot]);   // (the card requests: the slot's own folder)
  if (g_lrq.state == cardq::kFree) {   // (the list's save on its way: it lands first)
    if (g_ls == kLsJuno && fmt::pending(g_lj_file)) return;
    if (g_ls == kLsFm && fmt::pending(g_lf_file)) return;
  }
  g_lrq.fn = lists_card;
  if (!cardq::async(&g_lrq, cardq::kFnReq, nullptr, nullptr, 0, nullptr, 0)) return;
  if (g_ls_again) {
    g_ls_again = false;
    g_ls = kLsJuno;
  } else if (g_ls == kLsJuno) {
    g_ls = kLsFm;
  } else if (g_ls == kLsFm) {
    g_ls = kLsBanks, g_ls_k = 0;
  } else if (g_ls == kLsBanks) {
    g_ls_k += kBanksAPiece;
    if (g_ls_k >= dxlib.nodes()) g_ls = kLsVoices, g_ls_k = 0;
  } else if (++g_ls_k >= g_lf_n) {
    lists_done();
  }
}
#else
bool lists_busy() { return false; }
void project_lists_rekey(const char*) {}
void project_lists_step() {}
#endif

// The switch itself: the old project's unsaved edits into its own files, then the new name's files loaded
CORE_COLD static void switch_to(const char* to) {
#if CORE_POLY
  seq::project_leaving();   // 1042: the old project's unsaved sequence edits, in its own folder (before the name moves)
#endif
  Project& j = prj();
  char n[sizeof j.key];
  sniprintf(n, sizeof n, "%s", to);   // (to may be j.key itself: a reload)
  sniprintf(j.name, sizeof j.name, "%s", j.next[0] ? j.next : n);
  sniprintf(j.key, sizeof j.key, "%s", n);
  j.next[0] = 0;
  if (strcmp(j.key, j.name)) log(CORE_STR("project: %s (%s)"), j.key, j.name);
  else log(CORE_STR("project: %s"), j.key);
#if CORE_MSAMP
  sedit::project_changed();   // the sample editor closed first: its edit is the old project's sample (samp/sedit.h)
#endif
  pool_load();
#if CORE_MSAMP
  if (cardq::worker()) {
    lists_begin();
  } else
#endif
  {
    junolib.load_list(prj().key);
    juno_relist();
    dxlib.load_list(prj().key);
    fm_relist();
  }
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
  prj().dirty = true;   // /.rytm/project written from loop() (a 2 ms card write)
}
CORE_COLD static void announce() {   // the Rytm's tables for the project (its first 10, a change)
#if CORE_MSAMP
  projects::note(prj().key, prj().name);   // the project index (/.rytm/projects.txt, core/bundle/projects.h): written by idle_save
#endif
  send_pool();
  send_juno_names();
  send_fm_names();
}

static uint32_t name_hash(const char* c) {
  uint32_t h = 2166136261u;   // FNV-1a
  for (; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
  return h;
}
CORE_COLD uint32_t project_id() { return (name_hash(prj().key) * 16777619u + prj().reported) * 16777619u + prj().epoch; }

CORE_COLD void set_project(const uint8_t* d, int len) {
  char n[24];
  int k = 0;
  for (int i = 0; i < len && k < 23 && d[i] != 0xf7 && d[i]; ++i) n[k++] = d[i] & 0x7f;
  n[k] = 0;
  if (is_slot_key(n)) n[0] = '_';   // 2026-10-27: a Rytm name never names a slot key (today's key for it)
  prj().reported = name_hash(n);   // before any early return below (slot mode, the rename question): the Rytm HAS named this project
#if CORE_MSAMP
  // The Rytm's project slots (its 4B reports, prjslot) and the rename question on its PROJECT page are the Rytm's alone: a
  // box's 10 (an A4's, a Digitakt's) is its project's name and nothing more, in its own folder (design 2026-11-21)
  const bool rytm = device::rytm_at(prj_slot());
  if (rytm && prjslot::take_10(n)) return;   // slot mode (core/bundle/prjslot.h): the 4B reports switch, not the 10
  char rk[kKeyMax];
  if (rytm && prjslot::resolve(n, rk, sizeof rk)) {   // name mode on a card keyed by slot (older mods): the slot of that name
    bool first = !prj().seen;
    prj().seen = true;
    if (strcmp(rk, prj().key) != 0) {
      rename::back();
      sniprintf(prj().next, sizeof prj().next, "%s", n);
      switch_to(rk);
    } else if (strcmp(prj().name, n)) {
      sniprintf(prj().name, sizeof prj().name, "%s", n);
    } else if (!first) {
      return;
    }
    announce();
    return;
  }
#endif
  bool first = !prj().seen;
  prj().seen = true;
  if (strcmp(n, prj().key) != 0) {
#if CORE_MSAMP
    if (rytm && rename::intercept(prj().key, n)) {   // UX review B1: no files under the new name, some under the old: asked first
      if (first && rename::busy()) announce();   // (the old project's tables meanwhile)
      return;
    }
#endif
    switch_to(n);
  } else {
#if CORE_MSAMP
    if (rytm) rename::back();   // (a question about another name is moot: the Rytm is back on this one)
#endif
    if (!first) return;
  }
  announce();
}

// core/bundle/rename.cpp's answer: the switch the Rytm's 10 asked for, after the files moved (or not)
CORE_COLD void project_switch(const char* n) {
  if (strcmp(n, prj().key) != 0) switch_to(n);
  announce();
}

// 2026-10-27 (core/bundle/prjslot.h): the switch to a slot key with the Rytm's name for it; reload: the same key again
// (its files changed under it: a cleared slot reloaded, a restore). A new name for the same key only renames it.
CORE_COLD void project_switch_named(const char* key, const char* name, bool reload) {
  prj().reported = name_hash(name ? name : key);
  if (reload) ++prj().epoch;
  prj().seen = true;
  if (reload || strcmp(key, prj().key) != 0) {
    sniprintf(prj().next, sizeof prj().next, "%s", name ? name : key);
    switch_to(key);
  } else {
    sniprintf(prj().name, sizeof prj().name, "%s", name ? name : key);
  }
  announce();
}

#if CORE_MSAMP
// One RAM store per voice slot (core/store/store.h): each box's project's files in its own, filled from its own folder
// in its Scope. A fill begun whose project the modules are not switched to yet: g_loading of its slot.
static bool g_loading[kVoiceSlots];

// The slot in hand's project's files (a fill of that slot still running is abandoned by fill_begin: a switch while one
// runs reads only the new project's files). Per slot so far: the LFOs, the inserts and OUT, the XY pad, the scenes and the sequencer
// (each slot's fill switches its own). The kits' records (core/bundle/kits.h) are each slot's too: read from the store of the
// slot whose kit report needs them (3(c) step 9), nothing to switch. song is the Rytm's still (10c): its files are wanted
// for every slot (the paths of the slot in hand's key), but only slot 0's song uses them.
CORE_COLD void project_fill_begin() {
  const int s = prj_slot();
  store::clear();
  lfo::want_files();
  kits::want_files();   // (every kit's saved and kept-aside record: core/bundle/kits.h)
#if CORE_POLY
  ins::want_files();
  scene::want_files();   // (the scenes: core/mod/scene.h)
  xypad::want_files();   // (the XY pad: core/mod/xypad.h)
  seq::want_files();   // (phase 2: tracks.txt, the patterns, their saved states)
  song::want_files();  // (UNDO REPLAY's backup slots: .backups.txt)
  seq::loading_begin();   // (its edits refused until project_changed: they would be lost to the load; each slot's own, 3(c))
#endif
  store::fill_begin();
  g_loading[s] = true;
}

CORE_COLD bool project_fill_step() {
  for (int s = 0; s < kVoiceSlots; ++s) {
    if (!g_loading[s]) continue;
    device::Scope in(device::g_links[s]);   // (its requests: the slot's own folder; its store)
    if (!store::fill_step()) {
      if (s == 0) return false;   // (one slot's step a pass; slot 0's first)
      continue;
    }
    g_loading[s] = false;
    if (s == 0) {
      // the old project's unsaved edits into its own files (each project_changed saves before it loads), then the
      // project's, from the store; a kit the Rytm named while it filled (kit_changed: noted only) is the one loaded
      lfo::project_changed();
#if CORE_POLY
      ins::project_changed();
      xypad::project_changed();   // (before the scenes: its release puts the fader back first)
      scene::project_changed();
      seq::project_changed();   // (the boot's too: seq::init leaves its tracks and A01 to this)
#endif
    } else {   // 3(c): slot s's own LFOs, inserts and OUT, XY pad, scenes and sequencer, from its store (in its Scope)
      lfo::project_changed();
#if CORE_POLY
      ins::project_changed();
      xypad::project_changed();   // (before the scenes: its release puts the fader back first)
      scene::project_changed();
      seq::project_changed();   // (its own sequencer: tracks.txt and the patterns from its folder, 3(c) step 7)
#endif
    }
    log(CORE_STR("store: slot %d: %s loaded: %d files of %d paths, %u bytes"), s, prj().key, store::present(),
        store::entries(), (unsigned)store::bytes());
    return true;
  }
  return false;
}

bool project_loading() { return g_loading[prj_slot()]; }   // the slot in hand's
#endif

#if CORE_MSAMP
// Slot s's project, from its box's folder once a box holds it (attach order: either slot; the presumed Rytm's slot 0 is the
// boot's project_load): its name read by the card core (kSmallReq: no card time here), then its pool. Read again when another
// box (another folder) takes the slot; a box gone leaves it as it was until the next one, and the same box back finds it so.
namespace {
struct Attach {
  const device::Profile* from;   // the holder whose folder its project was read from, null none yet
  char from_dir[sizeof device::Link::dir];   // and its UID folder ("" its type folder): two boxes of one type are two folders
  bool reading;
  cardq::Req rq;
  char buf[24];
  int set;            // 3(c) step 8: the next of kSettings to read (kNSettings: all read)
  bool set_reading;
  char text[1536];    // a settings file's text
};
Attach g_attach[kVoiceSlots];
// 3(c) step 8: the box settings slot 0 loads at boot (engine.cpp), read for another slot from its own folder at its
// attach, each by the card core (kSmallReq) and taken in the slot's Scope into its own instance
struct SettingsFile {
  const char* path;
  void (*take)(const char* b, int n);
};
const SettingsFile kSettings[] = {
    {"/.rytm/sendfx.txt", fx::card_text},
    {"/.rytm/fxin.txt", fxin::take_text},
#if CORE_POLY
    {"/.rytm/keys.txt", keys::take_text},
    {"/.rytm/learn.txt", learn::take_text},
    {"/.rytm/loop.txt", loop::take_text},   // its looper's settings (core/loop)
#endif
    {"/.rytm/preview.txt", preview::take_text},
};
constexpr int kNSettings = (int)(sizeof kSettings / sizeof kSettings[0]);
CORE_COLD bool settings_attach_step(int s, Attach& a) {
  if (a.set >= kNSettings) return false;
  const SettingsFile& f = kSettings[a.set];
  if (!a.set_reading) a.set_reading = true, memset(a.text, 0, sizeof a.text);
  if (!cardq::async(&a.rq, cardq::kSmallReq, nullptr, f.path, 0, (uint8_t*)a.text, sizeof a.text - 1, s)) return true;
  const int n = a.rq.got > 0 ? (int)a.rq.got : 0;
  a.text[n] = 0;
  a.set_reading = false, ++a.set;
  device::Scope in(device::g_links[s]);
  f.take(a.text, n);   // (none there: n 0, the defaults)
  return true;
}
}  // namespace
CORE_COLD bool project_attach_step(int s) {
  if (s < 0 || s >= kVoiceSlots || s >= device::kLinks) return false;
  device::Link& l = device::g_links[s];
  Attach& a = g_attach[s];
  if (!l.held) return false;
  if (!a.from && !a.reading && s == 0 && l.held == &device::rytm())
    a.from = l.held, a.set = kNSettings, strcpy(a.from_dir, l.dir);   // (the boot's Rytm: project_load's)
  if (a.from == l.held && !strcmp(a.from_dir, l.dir) && !a.reading) return settings_attach_step(s, a);   // then its box settings, one file a step
  if (!a.reading) a.reading = true, memset(a.buf, 0, sizeof a.buf);
  if (!cardq::async(&a.rq, cardq::kSmallReq, nullptr, "/.rytm/project", 0, (uint8_t*)a.buf, sizeof a.buf - 1, s)) return true;
  a.buf[a.rq.got > 0 ? a.rq.got : 0] = 0;
  a.reading = false, a.from = l.held, a.set = 0, a.set_reading = false;
  strcpy(a.from_dir, l.dir);
  device::Scope in(l);
  Project& j = prj(s);
  sniprintf(j.key, sizeof j.key, "%s", a.buf);
  sniprintf(j.name, sizeof j.name, "%s", a.buf);
  j.next[0] = 0, j.dirty = false, j.seen = false, ++j.epoch;
  log(CORE_STR("project: slot %d (%s): %s"), s, l.state ? l.state : "/.rytm", j.key[0] ? j.key : "(none yet)");
  pool_load();
  if (cardq::worker()) lists_begin();   // its JUNO / FM lists, read from its folder (one reader: in turn)
  if (hooks.project_changed) hooks.project_changed();   // its machines: its own user-slot map (core/user/um.h, 3(c))
  project_fill_begin();                 // its RAM store, from its folder
  return true;
}
#else
CORE_COLD bool project_attach_step(int) { return false; }
#endif

CORE_COLD void project_load() {
  File f;
  prj().key[0] = 0;
  fmt::recover("/.rytm/project");   // 0.3.0: a cut between write_atomic's renames - the previous name back (fmt.h)
  if (f.open("/.rytm/project", kRead)) {
    int n = f.read(prj().key, sizeof prj().key - 1);
    prj().key[n > 0 ? n : 0] = 0;
    f.close();
  }
  log(CORE_STR("project: %s"), prj().key[0] ? prj().key : "(none yet)");
  sniprintf(prj().name, sizeof prj().name, "%s", prj().key);
#if CORE_MSAMP
  projects::load();
  prjslot::load();   // /.rytm/prjslot.txt (slot mode, the trash's counter)
  if (const projects::Info* e = projects::find(prj().key))
    if (e->rytm[0]) sniprintf(prj().name, sizeof prj().name, "%s", e->rytm);   // (a slot key: the Rytm's name)
#endif
  pool_load();
  junolib.load_list(prj().key);
  dxlib.load_list(prj().key);
#if CORE_MSAMP
  Attach& a = g_attach[prj_slot()];   // (the boot's: its holder's project and settings read here and by engine_begin)
  a.from = device::cur().held, a.set = kNSettings, a.reading = a.set_reading = false;
  strcpy(a.from_dir, device::cur().dir);
#endif
}

#if CORE_MSAMP
// 0F / 12 into the SAMPLES pool's bank A (the pool list decides: one entry per hash, a file there already keeps its row)
CORE_COLD void pool_add(int folder, int file) {
  if (lists_busy() && card_ok && folder >= cards.folders()) {   // a JUNO patch or a DX7 voice, the new project's lists on their way
    if (g_nheld_add < (int)(sizeof g_held_add / sizeof g_held_add[0])) g_held_add[g_nheld_add++] = HeldAdd{folder, file, prj_slot()};
    return;
  }
  if (card_ok && folder >= dx_base()) {
    presets::seed(presets::kIxFm);   // (an empty list: a saved PAT keeps its preset)
    if (dxlib.add(folder - dx_base(), file) >= 0) {
      send_fm_names();
      fm_relist();
    }
    return;
  }
  if (card_ok && folder >= cards.folders()) {
    presets::seed(presets::kIxJuno);
    int j = folder - cards.folders(), s = junolib.add(j, file);
    if (s < 0) {   // no empty row: the highest still its factory patch (the browser asks which)
      jlib::Patch p;
      if ((s = presets::spare_juno()) >= 0 && junolib.patch(j, file, p.name, p.b)) junolib.set_row(s, p.b, p.name);
      else s = -1;
    }
    if (s >= 0) {
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
  for (; prj().npool <= line; ++prj().npool) {   // the pool grows: empty slots up to it
    pool[prj().npool].path[0] = 0;
    pool[prj().npool].folder = -1;
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
  for (int i = 0; i < kVoices; ++i) prepared[i][0] = -2;   // voices: prepare again
  if (hooks.pool_changed) hooks.pool_changed(line);   // plugin voices reading this slot (or refused it) too
#if CORE_MSAMP
  gr::rescan();   // GRANULAR lists the pool's files (POOL)
#endif
  log(CORE_STR("pool %s: slot %d = %s"), prj().key[0] ? prj().key : "_default", line + 1, e.path);
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
    for (int i = 0; i < prj().npool && !there; ++i) there = pool[i].folder == folder && pool[i].file == file;
    int line = 0;
    while (line < prj().npool && pool[line].path[0]) ++line;   // the first empty slot
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
  const PoolEntry* lines = pool + pool_line(prj_slot(), 1);   // (the slot in hand's)
  int line = 0, first = 1, end = prj().npool < kPool ? prj().npool : kPool;
  do {
    PoolRow rows[6];
    int count = 0;
    for (; line < end && count < 6; ++line) {
      const PoolEntry& e = lines[line];
      if (!e.path[0]) continue;
      int len = strnlen(e.name, card::kName);   // a long name may fill it, no NUL
      for (int i = len - 1; i > 0; --i)
        if (e.name[i] == '.') { len = i; break; }
      rows[count++] = {(uint8_t)(line + 1), (uint16_t)(e.folder < 0 ? 0x3fff : e.folder),
                       (uint16_t)(e.folder < 0 ? 0x3fff : e.file), e.name, len};
    }
    while (line < end && !lines[line].path[0]) ++line;   // so the last message is not empty
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
#if CORE_MSAMP
  const int all = cards.files_in(f);   // (the files added since boot too, after the scanned ones)
#else
  const int all = cards.folder(f).nfiles;
#endif
  int nf = all < 512 - lns ? all : 512 - lns;
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
