// core/bundle/bundle.cpp - see bundle.h. The job's state machine: the common parts, the project-file table, the
// project list, delete and the boot recovery. The export: export.cpp; the import: import.cpp (bundle_int.h).
#include "bundle_int.h"

#if CORE_MSAMP
#include <stdarg.h>
#include <string.h>
#include <strings.h>

#include "prjslot.h"
#include "projects.h"

namespace core {
namespace bundle {

// ---- the per-project files: card path (%s = the key) <-> bundle name. kDir: a folder whose files go under the
// entry's name; kSongs: the same, only take JSONs (not an open journal, a backup or a temporary file). The pool lists
// (core/lib/pools.h) are opaque kind "pool" files: the collector takes their references from pools::refs() / the
// lists' own parser, never from their lines here. old: the per-machine pools and picks before the pools redesign -
// no longer read by anything, so not exported; an old bundle's are imported where they were (and deleted with the
// project), never read. A new per-project file whose module holds its key in RAM also needs a rekey hook in
// prjslot.cpp's rekey_all (a SAVE AS copy's place and a move of the current project rekey every module there).
const ProjectFile kProjectFiles[] = {
    {"/.rytm/projects/%s/samples.pool.txt", "project/samples.pool.txt", kKindPool, "samplepool", kOne, false},
    {"/.rytm/projects/%s/wavetables.pool.txt", "project/wavetables.pool.txt", kKindPool, "wtpool", kOne, false},
    {"/.rytm/projects/%s/instruments.pool.txt", "project/instruments.pool.txt", kKindPool, "instpool", kOne, false},
    {"/.rytm/projects/%s/pool-used.txt", "project/pool-used.txt", kKindPool, "poolused", kOne, false},
    {"/.rytm/pools/%s.txt", "project/pool.txt", kKindPool, "pool", kOne, true},
    {"/.rytm/granular/%s.pool.txt", "project/granular.pool.txt", kKindPool, "pool", kOne, true},
    {"/.rytm/wavetable/%s.pool.txt", "project/wavetable.pool.txt", kKindPool, "pool", kOne, true},
    {"/.rytm/multisample/%s.pool.txt", "project/multisample.pool.txt", kKindPool, "pool", kOne, true},
    {"/.rytm/granular/%s.txt", "project/granular.picks.txt", kKindPool, "picks", kOne, true},
    {"/.rytm/slots/%s.txt", "project/slots.txt", kKindProject, "slots", kOne, false},
    {"/.rytm/slots/%s.inf", "project/slots.inf", kKindProject, "slotnotes", kOne, false},
    {"/.rytm/fxslots/%s.txt", "project/fxslots.txt", kKindProject, "fxslots", kOne, false},
    {"/.rytm/juno/%s.txt", "project/juno.txt", kKindProject, "juno", kOne, false},
    {"/.rytm/fm/%s.txt", "project/fm.txt", kKindProject, "fm", kOne, false},
    {"/.rytm/lfo/%s.txt", "project/lfo.txt", kKindProject, "lfo", kOne, false},
    {"/.rytm/ins/%s.txt", "project/ins.txt", kKindProject, "ins", kOne, false},
    {"/.rytm/lfo/%s", "project/lfo/", kKindProject, "lfo", kDir, false},
    {"/.rytm/ins/%s", "project/ins/", kKindProject, "ins", kDir, false},
    {"/.rytm/kits/%s", "project/kits/", kKindProject, "kits", kDir, false},   // the kits' saved copies (kits.h)
    {"/.rytm/seq/%s", "project/seq/", kKindSeq, "", kDir, false},
    {"/.rytm/songs/%s", "project/songs/", kKindSong, "song", kSongs, false},
};
const int kNumProjectFiles = sizeof kProjectFiles / sizeof kProjectFiles[0];
const char* const kScanned[] = {"/GRANULAR", "/WAVETABLES", "/MULTISAMPLE"};   // the machines' own folder scans

Shared g;   // the job's state (one job at a time)

namespace {
Env g_env = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "?", "?", 1500};
}

void set_env(const Env& e) {
  g_env = e;
  if (!g_env.version) g_env.version = "?";
  if (!g_env.board) g_env.board = "?";
}
const Env& env() { return g_env; }
const Status& status() { return g.st; }

const char* phase_name(int p) {
  static const char* const n[] = {"IDLE", "FLUSH", "COLLECT", "HASH", "WRITE", "SERVE", "VERIFY", "PLAN", "READY",
                                  "BACKUP", "STAGE", "COMMIT", "LINK", "DELETE", "DONE", "FAILED", "CANCELLED"};
  return p >= 0 && p <= kCancelled ? n[p] : "?";
}

// The job's memory, taken at the first job and kept (no fragmentation): about 90 MB with the 32 MB snapshot (the
// Pi's heap; the Mac's malloc). recover() needs only the text buffer (ensure_text).
static void ensure_text() {
  if (!g.text) g.text = new char[kTextMax + 1];
}
void ensure() {
  ensure_text();
  if (g.io) return;
  g.io = new uint8_t[kPiece + 16];
  g.x.alloc(kMaxEntries, kMaxPaths, kMaxMissing);
  g.m.alloc(kMaxEntries, kMaxPaths, kMaxMissing);
  g.z = new ZEntry[kMaxEntries + 1];
  g.iz = new ZEntry[kMaxEntries + 1];
  g.man = new char[kManifestMax + 1];
  g.ram = new uint8_t[kProjectRam];
  g.ram_off = new uint32_t[kMaxEntries];
  g.cd = new uint8_t[(size_t)(kMaxEntries + 1) * (46 + kNameMax) + 64];   // (V_OPEN's limit)
  g.iram = new uint8_t[kImportRam];
  g.iram_off = new uint32_t[kMaxEntries];
  g.ops = new Op[kMaxOps];
  g.paths = new PathPlan[kMaxPaths];
  g.zix = new int[kMaxEntries];
  g.ht = new int[kPathHash];
}

void bump() { __atomic_store_n(&g.st.seq, g.st.seq + 1, __ATOMIC_RELEASE); }
void say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsniprintf(g.st.msg, sizeof g.st.msg, fmt, ap);
  va_end(ap);
  bump();
}
bool fail(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsniprintf(g.st.msg, sizeof g.st.msg, fmt, ap);
  va_end(ap);
  log(CORE_STR("bundle: %s failed: %s"), phase_name(g.st.phase), g.st.msg);
  cleanup_failed();
  g.st.phase = kFailed;
  g.st.waiting[0] = 0;
  bump();
  return false;
}
void set_phase(int p, uint64_t total) {
  g.st.phase = (uint8_t)p;
  g.st.done = 0, g.st.total = total;
  g.st.waiting[0] = 0;
  bump();
}

bool file_size(const char* path, uint64_t* size) {
  File f;
  if (!f.open(path, kRead) || f.isDir()) return false;
  *size = f.fileSize();
  f.close();
  return true;
}
bool is_dir(const char* path) {
  File f;
  bool d = f.open(path, kRead) && f.isDir();
  f.close();
  return d;
}
bool mkdirs_for(const char* file) {   // the folders a file goes into
  char d[kCardPath + 64];
  sniprintf(d, sizeof d, "%s", file);
  char* sl = strrchr(d, '/');
  if (!sl || sl == d) return true;
  *sl = 0;
  Fs fs;
  if (is_dir(d)) return true;
  return fs.mkdir(d) && is_dir(d);
}
static Stats g_stats;
void mark(const char* step) {
  if (!g.st.vol[0]) return;   // a stick job's only (the card's own steps are the card's)
  char b[kCardPath + 160];
  int n = sniprintf(b, sizeof b, "%s %s %s: %s (step %lu)\n", g.job == kExport ? "export" : g.job == kImport ? "import" : "job",
                    g.st.vol, g.st.file, step, (unsigned long)g_stats.steps);
  if (n < 0 || !is_dir("/.rytm")) return;
  File f;
  if (!f.open(kMarkPath, kWrite)) return;
  f.write(b, (size_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
  f.close();
}
void unmark() {
  Fs fs;
  if (fs.exists(kMarkPath)) fs.remove(kMarkPath);
}
static uint32_t g_step_rd, g_step_wr;
bool read_at(File& f, uint64_t off, uint8_t* buf, uint32_t n) {
  g_step_rd += n;
  return f.seekSet(off) && f.read(buf, n) == (int)n;
}
bool write_all(File& f, const uint8_t* buf, uint32_t n) {
  g_step_wr += n;
  return f.write(buf, n) == n;
}
const Stats& stats() { return g_stats; }
void stats_reset() { memset(&g_stats, 0, sizeof g_stats); }
// a small file whole into buf (cap bytes at most): its length, -1 missing, -2 too big
int64_t read_small(const char* path, uint8_t* buf, uint32_t cap) {
  File f;
  if (!f.open(path, kRead) || f.isDir()) return -1;
  uint64_t n = f.fileSize();
  if (n > cap) return f.close(), -2;
  bool ok = !n || read_at(f, 0, buf, (uint32_t)n);
  f.close();
  return ok ? (int64_t)n : -1;
}
bool ends_ci(const char* s, const char* e) {
  size_t a = strlen(s), b = strlen(e);
  return a >= b && !strcasecmp(s + a - b, e);
}
bool under(const char* path, const char* dir) {   // path inside dir (case ignored)
  size_t l = strlen(dir);
  return !strncasecmp(path, dir, l) && path[l] == '/';
}
bool in_scanned(const char* path) {
  for (const char* s : kScanned)
    if (under(path, s)) return true;
  return false;
}
uint32_t hash_ci(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) h = (h ^ (uint8_t)((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s)) * 16777619u;
  return h;
}
void key_of_rytm(const char* rytm, char* key) { project_key(rytm, key, kKeyMax); }

// the Rytm's name for a key: the index's, else the key itself
void rytm_name_of(const char* key, char* out, int cap) {
  static projects::Info ix[projects::kMax];
  int n = projects::copy(ix, projects::kMax);
  for (int i = 0; i < n; ++i)
    if (!strcmp(ix[i].key, key)) {
      sniprintf(out, cap, "%s", ix[i].rytm[0] ? ix[i].rytm : key);
      return;
    }
  char cur[24] = "";
  if (g_env.current) g_env.current(cur, sizeof cur);
  char ck[kKeyMax];
  key_of_rytm(cur, ck);
  sniprintf(out, cap, "%s", cur[0] && !strcmp(ck, key) ? cur : key);
}
bool is_current(const char* key) {
  char cur[24] = "";
  if (g_env.current) g_env.current(cur, sizeof cur);
  if (!cur[0]) return false;
  char ck[kKeyMax];
  key_of_rytm(cur, ck);
  return !strcmp(ck, key);
}

// ---- the projects
bool key_ok(const char* key) {   // a key the rule makes (never "..", '/', or anything else a path could use)
  char k[kKeyMax];
  project_key(key, k, sizeof k);
  return key && key[0] && !strcmp(k, key) && strcmp(key, ".") && strcmp(key, "..");
}
bool project_exists(const char* key) {
  char p[kCardPath];
  for (int i = 0; i < kNumProjectFiles; ++i) {
    sniprintf(p, sizeof p, kProjectFiles[i].card, key);
    Fs fs;
    if (fs.exists(p)) return true;
  }
  static projects::Info ix[projects::kMax];
  int n = projects::copy(ix, projects::kMax);
  if (n < 0) return true;   // (the index not readable now: taken as there, never a silent clash)
  for (int i = 0; i < n; ++i)
    if (!strcmp(ix[i].key, key)) return true;
  return false;
}

namespace {
struct PEnt {
  char key[kKeyMax];
  uint32_t files;
  uint64_t bytes;
};
PEnt* g_pl = nullptr;
int g_npl = 0;
PEnt* pl_find(const char* key, bool make) {
  for (int i = 0; i < g_npl; ++i)
    if (!strcmp(g_pl[i].key, key)) return &g_pl[i];
  if (!make || g_npl >= projects::kMax) return nullptr;
  PEnt& e = g_pl[g_npl++];
  memset(&e, 0, sizeof e);
  sniprintf(e.key, sizeof e.key, "%s", key);
  return &e;
}
// the folder of a table row ("/.rytm/pools/%s.txt" -> "/.rytm/pools", suffix ".txt")
void row_folder(const ProjectFile& r, char* dir, char* suffix) {
  const char* s = strstr(r.card, "%s");
  int l = (int)(s - r.card) - 1;
  sniprintf(dir, kCardPath, "%.*s", l, r.card);
  sniprintf(suffix, 32, "%s", s + 2);
}
void add_files(PEnt* p, const char* dir) {   // a project's own folder: its files counted
  File d, f;
  if (!d.open(dir, kRead) || !d.isDir()) return;
  char nm[128];
  while (f.openNext(&d, kRead)) {
    f.getName(nm, sizeof nm);   // (a dot file - .DS_Store - is not the project's: the import's empty_dir agrees)
    if (!f.isDir() && nm[0] != '.') ++p->files, p->bytes += f.fileSize();
    f.close();
  }
}
}  // namespace

static projects::Info* g_ix = nullptr;
static int g_nix = 0;
// the projects the card knows: g_pl (keys, files, bytes) and g_ix (the index)
static void scan_projects() {
  if (!g_pl) g_pl = new PEnt[projects::kMax];
  if (!g_ix) g_ix = new projects::Info[projects::kMax];
  g_npl = 0;
  char dir[kCardPath], suf[32], nm[128], key[kKeyMax];
  for (int i = 0; i < kNumProjectFiles; ++i) {
    const ProjectFile& r = kProjectFiles[i];
    row_folder(r, dir, suf);
    File d, f;
    if (!d.open(dir, kRead) || !d.isDir()) continue;
    while (f.openNext(&d, kRead)) {
      f.getName(nm, sizeof nm);
      bool isd = f.isDir();
      uint64_t sz = f.fileSize();
      f.close();
      if (nm[0] == '.') continue;
      int l = (int)strlen(nm), sl = (int)strlen(suf);
      bool in_dir = suf[0] == '/';   // the project's own folder: /.rytm/projects/<key>/samples.pool.txt
      if (r.how == kOne && in_dir) {
        char fp[kCardPath];
        if (!isd || sniprintf(fp, sizeof fp, r.card, nm) >= (int)sizeof fp || !file_size(fp, &sz)) continue;
      } else if (r.how == kOne) {
        if (isd || l <= sl || strcasecmp(nm + l - sl, suf)) continue;
        nm[l - sl] = 0;
        if (r.kind == kKindPool && !strcmp(r.entry, "project/granular.picks.txt") && ends_ci(nm, ".pool")) continue;
      } else if (!isd) {
        continue;
      }
      project_key(nm, key, sizeof key);
      if (strcmp(key, nm)) continue;   // not a key (another tool's file)
      PEnt* p = pl_find(key, true);
      if (!p) continue;
      if (r.how == kOne) ++p->files, p->bytes += sz;
      else {
        char sub[kCardPath];
        sniprintf(sub, sizeof sub, "%s/%s", dir, nm);
        add_files(p, sub);
      }
    }
  }
  g_nix = projects::copy(g_ix, projects::kMax);
  if (g_nix < 0) g_nix = 0;
  for (int i = 0; i < g_nix; ++i) pl_find(g_ix[i].key, true);
}
static const projects::Info* ix_of(const char* key) {
  for (int j = 0; j < g_nix; ++j)
    if (!strcmp(g_ix[j].key, key)) return &g_ix[j];
  return nullptr;
}

void slot_files(uint32_t* files, char (*names)[24]) {
  scan_projects();
  for (int sl = 0; sl <= prjslot::kSlots; ++sl) files[sl] = 0, names[sl][0] = 0;
  for (int i = 0; i < g_npl; ++i) {
    int sl = prjslot::slot_of(g_pl[i].key);
    if (sl < 1 || sl > prjslot::kSlots) continue;
    const projects::Info* inf = ix_of(g_pl[i].key);
    files[sl] = g_pl[i].files;
    sniprintf(names[sl], 24, "%s", inf ? inf->rytm : "");
  }
}

int projects_json(char* out, int cap) {
  scan_projects();
  JOut o;
  o.begin(out, (uint32_t)cap);
  o.raw("[");
  for (int i = 0; i < g_npl; ++i) {
    const PEnt& p = g_pl[i];
    const projects::Info* inf = ix_of(p.key);
    if (i) o.raw(",");
    o.raw("{\"key\":"), o.str(p.key), o.raw(",\"rytm\":"), o.str(inf && inf->rytm[0] ? inf->rytm : p.key);
    o.raw(",\"used\":"), o.num(inf ? inf->used : 0), o.raw(",\"date\":"), o.str(inf ? inf->date : "");
    o.raw(",\"imported\":"), o.raw(inf && inf->imported ? "true" : "false");
    o.raw(",\"current\":"), o.raw(is_current(p.key) ? "true" : "false");
    o.raw(",\"files\":"), o.num(p.files), o.raw(",\"bytes\":"), o.num(p.bytes), o.raw("}");
  }
  o.raw("]");
  return o.over ? 0 : (int)o.n;
}

int project_rows(ProjectRow* out, int max) {
  scan_projects();
  int n = 0;
  for (int i = 0; i < g_npl && n < max; ++i) {
    const PEnt& p = g_pl[i];
    const projects::Info* inf = ix_of(p.key);
    ProjectRow& r = out[n++];
    memset(&r, 0, sizeof r);
    sniprintf(r.key, sizeof r.key, "%s", p.key);
    sniprintf(r.rytm, sizeof r.rytm, "%s", inf && inf->rytm[0] ? inf->rytm : p.key);
    sniprintf(r.date, sizeof r.date, "%s", inf ? inf->date : "");
    r.files = p.files, r.bytes = p.bytes, r.used = inf ? inf->used : 0;
    r.imported = inf && inf->imported, r.current = is_current(p.key);
  }
  // the current one first, then the last used, then by name (the touchscreen's and the Rytm's list order)
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0; --j) {
      ProjectRow& a = out[j - 1];
      ProjectRow& b = out[j];
      bool sw = b.current != a.current ? b.current : b.used != a.used ? b.used > a.used : strcasecmp(b.key, a.key) < 0;
      if (!sw) break;
      ProjectRow t = a;
      a = b, b = t;
    }
  return n;
}

int bundle_rows(const char* folder, BundleRow* out, int max) {
  int n = 0;
  File d;
  char nm[160];
  bool isd;
  uint64_t sz;
  uint16_t dd, dt;
  // the folder's entries as its listing has them (one f_readdir each): no open or f_stat by path, which walks the
  // folder from its start every time (O(n^2) sector reads: seconds over USB in a stick's full root)
  if (d.open(folder, kRead) && d.isDir())
    while (n < max && d.nextEntry(nm, sizeof nm, &isd, &sz, &dd, &dt)) {
      bool part = ends_ci(nm, ".sidecar.zip.part");
      if (isd || nm[0] == '.' || (!ends_ci(nm, ".sidecar.zip") && !part) || strlen(nm) >= sizeof out[0].name) continue;
      BundleRow& r = out[n++];
      memset(&r, 0, sizeof r);
      sniprintf(r.name, sizeof r.name, "%s", nm);
      sniprintf(r.path, sizeof r.path, "%s%s%s", folder, folder[strlen(folder) - 1] == '/' ? "" : "/", nm);
      sniprintf(r.date, sizeof r.date, "%04d-%02d-%02d %02d:%02d", 1980 + (dd >> 9), (dd >> 5) & 15, dd & 31, dt >> 11, (dt >> 5) & 63);
      r.size = sz, r.incomplete = part;
    }
  for (int i = 1; i < n; ++i)   // the newest first (the date, then the name: stamps sort by time)
    for (int j = i; j > 0; --j) {
      int c = strcmp(out[j].date, out[j - 1].date);
      if (c < 0 || (c == 0 && strcmp(out[j].name, out[j - 1].name) <= 0)) break;
      BundleRow t = out[j - 1];
      out[j - 1] = out[j], out[j] = t;
    }
  return n;
}

int bundles_json(const char* folder, char* out, int cap) {
  JOut o;
  o.begin(out, (uint32_t)cap);
  o.raw("[");
  File d;
  bool first = true;
  char nm[160];
  bool isd;
  uint64_t sz;
  uint16_t dd, dt;
  if (d.open(folder, kRead) && d.isDir())   // (one f_readdir an entry, as bundle_rows)
    while (d.nextEntry(nm, sizeof nm, &isd, &sz, &dd, &dt)) {
      bool part = ends_ci(nm, ".sidecar.zip.part");
      if (isd || nm[0] == '.' || (!ends_ci(nm, ".sidecar.zip") && !part)) continue;
      char date[24];
      sniprintf(date, sizeof date, "%04d-%02d-%02d %02d:%02d", 1980 + (dd >> 9), (dd >> 5) & 15, dd & 31, dt >> 11, (dt >> 5) & 63);
      char path[kCardPath + 160];
      sniprintf(path, sizeof path, "%s%s%s", folder, folder[strlen(folder) - 1] == '/' ? "" : "/", nm);
      o.comma(&first);
      o.raw("{\"name\":"), o.str(nm), o.raw(",\"path\":"), o.str(path), o.raw(",\"size\":"), o.num(sz), o.raw(",\"date\":"), o.str(date);
      o.raw(",\"incomplete\":"), o.raw(part ? "true" : "false"), o.raw("}");
    }
  o.raw("]");
  return o.over ? 0 : (int)o.n;
}

// ---- a bundle file deleted
static bool bundle_file_path(const char* p) {
  char d[kDriveMax];
  int dl = drive_of(p, d);
  if (dl == 3 && !strncmp(p, "SD:", 3)) return false;   // (the card's paths have no drive)
  const char* name;
  if (dl) {
    if (p[dl] != '/') return false;
    name = p + dl + 1;
    if (!strncmp(name, "SIDECAR/", 8)) name += 8;
  } else {
    if (strncmp(p, "/Bundles/", 9)) return false;
    name = p + 9;
  }
  if (!name[0] || strchr(name, '/') || strstr(name, "..") || !name_ok(name)) return false;
  if (name[0] == '.' || (!ends_ci(name, ".sidecar.zip") && !ends_ci(name, ".sidecar.zip.part"))) return false;
  for (const char* c = p; *c; ++c)
    if ((unsigned char)*c < 0x20 || *c == '\\') return false;
  return true;
}
const char* remove_bundle(const char* path) {
  if (!path || !bundle_file_path(path)) return "not a bundle's path (/Bundles/<name>.sidecar.zip[.part], USB:/SIDECAR/...)";
  uint8_t p = g.st.phase;
  bool job = p != kIdle && p != kDone && p != kFailed && p != kCancelled;
  if (job && g.st.file[0]) {   // the job's own file: its archive, an export's .part beside it
    size_t l = strlen(g.st.file);
    if (!strcasecmp(path, g.st.file) || (!strncasecmp(path, g.st.file, l) && !strcasecmp(path + l, ".part")))
      return "in use by the job that runs (cancel it first)";
  }
  char d[kDriveMax];
  if (drive_of(path, d) && !(g_env.vol_ok && g_env.vol_ok(d))) return "that USB stick is not mounted";
  if (is_dir(path)) return "not a file";
  uint64_t sz;
  if (!file_size(path, &sz)) return "no such file";
  Fs fs;
  if (!fs.remove(path)) return "the delete failed";
  log(CORE_STR("bundle: deleted %s (%llu bytes)"), path, (unsigned long long)sz);
  return nullptr;
}

// ---- delete: the project files of a key (folders' files, then the folders)
// bundled_only (replace): only what an export carries (its backup holds them): a folder's dot-files, temporary files,
// an open song journal stay
static bool list_project_files(const char* key, bool with_dirs, bool bundled_only = false) {
  g.nops = 0;
  char p[kCardPath];
  for (int i = 0; i < kNumProjectFiles; ++i) {
    const ProjectFile& r = kProjectFiles[i];
    sniprintf(p, sizeof p, r.card, key);
    if (r.how == kOne) {
      uint64_t sz;
      if (bundled_only && r.old) continue;   // (replace: an export does not carry it - it stays, as it is never read)
      if (file_size(p, &sz) && g.nops < kMaxOps) g.ops[g.nops].kind = 'D', sniprintf(g.ops[g.nops++].a, kCardPath, "%s", p);
      continue;
    }
    File d, f;
    if (!d.open(p, kRead) || !d.isDir()) continue;
    while (f.openNext(&d, kRead)) {
      char nm[128];
      f.getName(nm, sizeof nm);
      bool isd = f.isDir();
      f.close();
      if (isd || g.nops >= kMaxOps) continue;
      if (bundled_only && (nm[0] == '.' || ends_ci(nm, ".tmp") || ends_ci(nm, ".part") ||
                           (r.how == kSongs && !ends_ci(nm, ".json")))) continue;
      g.ops[g.nops].kind = 'D';
      sniprintf(g.ops[g.nops++].a, kCardPath, "%s/%s", p, nm);
    }
    d.close();
    if (with_dirs && g.nops < kMaxOps) g.ops[g.nops].kind = 'd', sniprintf(g.ops[g.nops++].a, kCardPath, "%s", p);
  }
  // the project's own folder (the pool lists' /.rytm/projects/<key>), once its files are gone; delete only (a replace
  // writes the new lists into it)
  if (with_dirs && !bundled_only && g.nops < kMaxOps) {
    sniprintf(p, sizeof p, "/.rytm/projects/%s", key);
    if (is_dir(p)) g.ops[g.nops].kind = 'd', sniprintf(g.ops[g.nops++].a, kCardPath, "%s", p);
  }
  return true;
}
int replace_deletes(const char* key) {   // import's replace mode: the old project's files, for commit.txt
  list_project_files(key, true, true);
  return g.nops;
}

bool start_delete(const char* key, const char* confirm) {
  ensure();
  if (busy()) return say("busy: %s", phase_name(g.st.phase)), false;
  finish();
  if (!key || !key[0] || !confirm || strcmp(key, confirm)) return say("delete: the confirmation does not match"), false;
  if (!key_ok(key)) return say("not a project key: %s", key), false;
  if (is_current(key)) return say("%s is the Rytm's current project: switch the Rytm to another project first", key), false;
  if (!project_exists(key)) return say("no project %s on the card", key), false;
  // 2026-10-27 (UX review B3, core/bundle/prjslot.h): never deleted outright - the project's files move to the project
  // trash (the control core's job, a card-queue step a tick), restored or emptied from the web page
  char nm[projects::kRytm];
  rytm_name_of(key, nm, sizeof nm);
  if (!prjslot::request_trash(key, "web", nm)) return say("busy: try again in a moment"), false;
  // (its index entry goes when the trash has taken the files: checked again then - never the current project)
  g.job = kDeleteJob;
  g.st.job = kDeleteJob;
  sniprintf(g.st.key, sizeof g.st.key, "%s", key);
  set_phase(kDone, 0);
  say("%s moved to the project trash (its samples are kept; restore it from the trash)", key);
  return true;
}

// ---- removable volumes
int drive_of(const char* path, char* drive) {
  int i = 0;
  while (path && i < kDriveMax - 2 && ((path[i] >= 'A' && path[i] <= 'Z') || (path[i] >= '0' && path[i] <= '9'))) ++i;
  int n = path && i > 0 && path[i] == ':' ? i + 1 : 0;
  if (drive) {
    memcpy(drive, path, (size_t)n);
    drive[n] = 0;
  }
  return n;
}
bool vol_space(const char* drive, uint64_t* free) {
  *free = 0;
  if (!g_env.volumes || !g_env.vol_ok || !g_env.vol_ok(drive)) return false;
  Volume v[4];
  int n = g_env.volumes(v, 4);
  for (int i = 0; i < n; ++i)
    if (!strcmp(v[i].drive, drive)) {
      *free = v[i].free_known ? v[i].free : 0;
      return true;
    }
  return false;
}
static bool stick_now() {
  if (!g.st.vol[0]) return false;
  return g.job == kExport ? export_stick_now() : g.job == kImport ? import_stick_now() : false;
}
static bool stick_needed() {
  if (!g.st.vol[0]) return false;
  return g.job == kExport || (g.job == kImport && import_stick_needed());
}
const char* eject(const char* drive) {
  if (g.st.vol[0] && !strcmp(g.st.vol, drive)) {
    uint8_t p = g.st.phase;
    if (p == kCommit || p == kLink) return "busy: an import is finishing (a moment)";
    if (busy() || p == kReady || p == kServe) cancel(), say("cancelled: the USB stick was ejected");
  }
  if (!g_env.vol_eject) return "no USB sticks on this Sidecar";
  return g_env.vol_eject(drive);
}

// ---- the job
bool busy() {
  uint8_t p = g.st.phase;
  return !(p == kIdle || p == kDone || p == kFailed || p == kCancelled || p == kServe || p == kReady);
}
void finish() {
  if (g.st.phase == kServe || g.st.phase == kReady) cleanup_failed();
  g.job = kNoJob;
  g.st.job = kNoJob;
  g.st.phase = kIdle;
  g.st.waiting[0] = 0;
  g.st.vol[0] = 0;
  bump();
}
void cancel() {
  uint8_t p = g.st.phase;
  if (p == kIdle || p == kDone || p == kFailed || p == kCancelled) return;
  if (p == kCommit || p == kLink) return;   // milliseconds: it finishes
  cleanup_failed();
  g.st.phase = kCancelled;
  say("cancelled");
}
static bool needs_write(int p) { return p == kWriting || p == kStage || p == kCommit || p == kDelete || p == kBackup; }
bool step() {
  if (!g.io || !busy()) return false;
  IoRule r = {kPiece, kPiece, 0, ""};
  if (g_env.rules) g_env.rules(&r);
  const char* why = nullptr;
  int ph = g.st.phase;
  if (stick_needed() && !(g_env.vol_ok && g_env.vol_ok(g.st.vol))) {   // pulled (or ejected elsewhere): stop now
    if (g.job == kExport) fail("USB stick removed: the export stopped (its .part on the stick is incomplete)");
    else fail("USB stick removed: nothing was imported (the card is unchanged)");
    return true;
  }
  bool stick = stick_now();
  if (g_env.card_busy && g_env.card_busy()) why = "STREAMS";
  else if (stick && !r.write_piece) why = r.why && r.why[0] ? r.why : "PAUSED";   // the stick: full rate only
  else if (needs_write(ph) && !r.write_piece) why = r.why && r.why[0] ? r.why : "PAUSED";
  else if (ph != kFlush && ph != kLink && !r.read_piece) why = r.why && r.why[0] ? r.why : "PAUSED";
  else if (r.read_gap_us && now_us() - g.last_us < r.read_gap_us) return false;
  if (why) {
    ++g_stats.waits;
    if (strcmp(g.st.waiting, why)) sniprintf(g.st.waiting, sizeof g.st.waiting, "%s", why), bump();
    return false;
  }
  if (g.st.waiting[0]) g.st.waiting[0] = 0, bump();
  g.rp = r.read_piece < kPiece ? r.read_piece : kPiece;
  g.wp = r.write_piece < kPiece ? r.write_piece : kPiece;
  if (!g.rp) g.rp = kPiece;
  if (!g.wp) g.wp = kPiece;
  if (stick) g.rp = g.rp < kStickPiece ? g.rp : kStickPiece, g.wp = g.wp < kStickPiece ? g.wp : kStickPiece;
  g.last_us = now_us();
  g_step_rd = g_step_wr = 0;
  uint32_t c0 = cycles();
  if (g.job == kExport) export_step();
  else if (g.job == kImport) import_step();
  uint32_t us = (uint32_t)((uint64_t)(cycles() - c0) * 1000000ull / cycles_per_second());
  ++g_stats.steps;
  if (g_step_rd > g_stats.max_read) g_stats.max_read = g_step_rd;
  if (g_step_wr > g_stats.max_write) g_stats.max_write = g_step_wr;
  if (us > g_stats.max_us) g_stats.max_us = us;
  g_stats.read += g_step_rd, g_stats.written += g_step_wr;
  return true;
}

void cleanup_failed() {
  Fs fs;
  bool crumb = fs.exists(kMarkPath);   // (only a marked stick step has one: no card write otherwise)
  if (crumb) mark("the cleanup (f_close, f_unlink of the .part)");
  if (g.job == kExport) ex_cleanup();
  else if (g.job == kImport) im_cleanup();
  if (crumb) unmark();
}
void close_files() {
  ex_cleanup();
  im_cleanup();
}

// ---- boot recovery
static void remove_tree(const char* path, int depth) {
  Fs fs;
  File d, f;
  if (depth > 16) return;
  if (d.open(path, kRead) && d.isDir()) {
    char nm[128], sub[kCardPath + 130];
    for (;;) {   // listed again after each removal (the index moves)
      bool any = false;
      d.rewind();
      while (f.openNext(&d, kRead)) {
        f.getName(nm, sizeof nm);
        bool isd = f.isDir();
        f.close();
        sniprintf(sub, sizeof sub, "%s/%s", path, nm);
        if (isd) remove_tree(sub, depth + 1);
        else fs.remove(sub);
        any = !fs.exists(sub);   // a removal that failed (a card error, a cut): stop, never loop on it
        break;
      }
      if (!any) break;
      d.close();
      if (!d.open(path, kRead)) break;
    }
    d.close();
  }
  fs.remove(path);
}
void remove_tree(const char* path) { remove_tree(path, 0); }

// one commit.txt line run (again): idempotent, so a cut at any point is rolled forward by running every line again
bool run_op(char kind, const char* a, const char* b) {
  Fs fs;
  if (kind == 'D' || kind == 'd') return !fs.exists(a) || fs.remove(a) || (kind == 'd' && is_dir(a));
  if (kind == 'M') {   // a file / folder to the project trash: only while its target is free (run again after the new
                       // files are in place, the new one stays where it is and the trashed one is kept)
    if (!fs.exists(a) || fs.exists(b)) return true;
    return mkdirs_for(b) && fs.rename(a, b);
  }
  if (kind != 'R') return true;
  if (!fs.exists(a)) return true;   // done before the cut
  if (!mkdirs_for(b)) return false;
  if (fs.exists(b) && !fs.remove(b)) return false;
  return fs.rename(a, b);
}
// commit.txt: "D <path>" / "d <folder>" / "M <path> <trash path>" / "R <staged> <final>" lines, then "END": complete ->
// every line run
// An M that did not complete (its source still there, its target not: the rename refused) blocks every R into its path
// or below it: an old file is never replaced unless it is safely in the trash (its staged file goes with the folder).
static bool roll_forward(const char* commit) {
  static char blocked[32][kCardPath];
  int nblocked = 0;
  Fs fs;
  uint8_t* buf = (uint8_t*)g.text;
  int64_t n = read_small(commit, buf, kTextMax);
  if (n < 4) return false;
  buf[n] = 0;
  char* s = (char*)buf;
  if (n < 4 || strcmp(s + n - 4, "END\n")) return false;
  for (char* line = s; *line;) {
    char* nl = strchr(line, '\n');
    if (!nl) break;
    *nl = 0;
    if (!strcmp(line, "END")) break;
    char kind = line[0];
    if (line[1] == ' ') {
      char* a = line + 2;
      char* b = kind == 'R' || kind == 'M' ? strstr(a, " /") : nullptr;
      if (b) *b++ = 0;
      bool skip = false;
      for (int k = 0; kind == 'R' && b && k < nblocked && !skip; ++k) {
        size_t l = strlen(blocked[k]);
        skip = !strncasecmp(b, blocked[k], l) && (b[l] == 0 || b[l] == '/');
      }
      if (skip) log(CORE_STR("bundle: boot: %s kept (its move to the trash did not complete): not replaced"), b);
      else run_op(kind, a, b ? b : "");
      if (kind == 'M' && b && fs.exists(a) && !fs.exists(b)) {
        if (nblocked < 32) sniprintf(blocked[nblocked++], kCardPath, "%s", a);
        log(CORE_STR("bundle: boot: %s could not go to the trash: kept, the import's files for it left out"), a);
      }
    }
    line = nl + 1;
  }
  return true;
}
void recover() {
  {   // a stick job's breadcrumb left by the run before (a reset in that step: mark())
    char b[kCardPath + 160];
    int64_t n = read_small(kMarkPath, (uint8_t*)b, sizeof b - 1);
    if (n > 0) {
      b[n] = 0;
      if (b[n - 1] == '\n') b[n - 1] = 0;
      log(CORE_STR("bundle: boot: the last run stopped in a stick job's step (a hang or a reset there): %s"), b);
      sniprintf(g.st.msg, sizeof g.st.msg, "the last run stopped in: %s", b);
      bump();
    }
    if (n != -1) unmark();   // (a broken one too)
  }
  File d, f;
  if (!d.open("/Bundles/.staging", kRead) || !d.isDir()) return;
  ensure_text();
  char names[32][32];
  int n = 0;
  while (n < 32 && f.openNext(&d, kRead)) {
    f.getName(names[n], sizeof names[n]);
    if (f.isDir()) ++n;
    f.close();
  }
  d.close();
  for (int i = 0; i < n; ++i) {
    char dir[96], commit[128];
    sniprintf(dir, sizeof dir, "/Bundles/.staging/%s", names[i]);
    sniprintf(commit, sizeof commit, "%s/commit.txt", dir);
    bool rolled = roll_forward(commit);
    log(CORE_STR("bundle: boot: %s %s"), dir, rolled ? "rolled forward (an import's commit finished)" : "removed (an import that did not commit)");
    remove_tree(dir);
  }
  Fs fs;
  fs.remove("/Bundles/.staging");   // (only when empty)
}

void reset_for_tests() {
  ensure();
  g.job = kNoJob;
  memset(&g.st, 0, sizeof g.st);
  g.job_n = 0;
  close_files();
}

}  // namespace bundle
}  // namespace core
#endif
