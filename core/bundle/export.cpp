// core/bundle/export.cpp - see bundle.h: the export (FLUSH, COLLECT, HASH, WRITE / SERVE), one piece a step.
#include "bundle_int.h"

#if CORE_MSAMP
#include <string.h>
#include <strings.h>

#include "../fmt/fmt.h"
#include "../lib/library.h"
#include "../lib/pools.h"
#include "../lib/pools_fmt.h"
#include "../msamp/sfz.h"
#include "projects.h"
#include "rmp_loader.h"
#include "rmp_resolve.h"
#include "rmp_slots.h"

namespace core {
namespace bundle {

namespace {

enum Stage { E_FLUSH = 0, E_FILES, E_DIRS, E_PARSE, E_TAKES, E_SFZ, E_STAT, E_PKGSCAN, E_PKG, E_RYTM, E_SIZE, E_HASH,
             E_FINAL, E_WRITE, E_CLOSE, E_RENAME, E_SERVE, E_DONE };
constexpr uint32_t kPoolsFlushMs = 5000;   // FLUSH: the current project's pool lists' pending writes, at most this long

struct Ref {
  char path[kCardPath];
  uint8_t kind;      // kKindSample, kKindTake, kKindSfz
  bool ok, abs;
  int group;         // the .sfz ref it belongs to (-1 none)
  int entry;         // its entry, once added
  uint32_t size;
};
struct Pkg {
  char path[kCardPath];
  char id[32];
  uint32_t ver, size;
  uint8_t target;
};
constexpr int kPkgs = 64, kTakeDepth = 4;

struct Ex {
  int st;
  int dest;
  bool sub;                    // an import's backup of the project it replaces
  ExportOpts o;
  char key[kKeyMax], rytm[24], sampler[48], folder[48];
  uint32_t t0_ms, epoch;
  int clock;
  int i, j;
  File dir;
  bool dir_open;
  // the sampler's takes: a folder stack
  File tdir[kTakeDepth];
  char tpath[kTakeDepth][kCardPath];
  int tdepth;
  Ref* refs;
  int nrefs;
  Pkg pkg[kPkgs];
  int npkg;
  // HASH
  int he;
  uint32_t hoff;
  Sha256 sh;
  uint32_t crc;
  File hf;
  bool hopen;
  // WRITE / SERVE: the data callback's file, the running check
  File df;
  int dfe;
  File rf;                     // the snapshot's file being read
  int rd_e;
  uint32_t rd_off;
  int sfz_r;                   // the .sfz being read (-1 none)
  uint32_t sfz_off, sfz_len;
  int we;
  uint32_t wpos;
  Sha256 wsh;
  bool changed;
  char changed_path[kCardPath];
  uint64_t woff;
  File wf;
  bool wopen;
  char part[kCardPath + 8], final_[kCardPath], name[kCardPath];
  uint8_t* checked;            // serve: entry e's size checked against the plan
  uint8_t* known;              // entry e's sha256 and CRC-32 from the library (HASH reads nothing of it)
  MPath* tmp_paths;
  int* keep;
};
Ex E;

msamp::Region* g_regions = nullptr;
char (*g_samples)[msamp::kPath] = nullptr;
char* g_sfz = nullptr;   // an .sfz's text (not g.text: an import's rewritten files wait there during its backup)
constexpr int kRegions = 2048, kSamples = 1024;

void ex_ensure() {
  if (E.refs) return;
  E.refs = new Ref[kMaxPaths];
  E.checked = new uint8_t[kMaxEntries + 1];
  E.known = new uint8_t[kMaxEntries + 1];
  E.tmp_paths = new MPath[kMaxPaths];
  E.keep = new int[kMaxEntries];
  g_regions = new msamp::Region[kRegions];
  g_samples = new char[kSamples][msamp::kPath];
  g_sfz = new char[kTextMax + 1];
}
void ex_close() {
  if (E.dir_open) E.dir.close(), E.dir_open = false;
  for (int d = 0; d < kTakeDepth; ++d) E.tdir[d].close();
  E.tdepth = 0;
  if (E.hopen) E.hf.close(), E.hopen = false;
  E.df.close();
  E.dfe = -1;
  if (E.wopen) E.wf.close(), E.wopen = false;
  E.rf.close();
  E.rd_e = -1, E.sfz_r = -1;
}

// ---- refs: card files the project names (by path; case ignored, as FAT)
int ref_find(const char* path) {
  uint32_t h = hash_ci(path) & (kPathHash - 1);
  for (int k = 0; k < kPathHash; ++k, h = (h + 1) & (kPathHash - 1)) {
    int r = g.ht[h];
    if (r < 0) return -1;
    if (!strcasecmp(E.refs[r].path, path)) return r;
  }
  return -1;
}
int add_ref(const char* path, int kind, int group) {
  if (!path[0] || path[0] != '/' || strlen(path) >= (size_t)kCardPath) return -1;
  int r = ref_find(path);
  if (r >= 0) {
    if (E.refs[r].kind == kKindTake && kind != kKindTake) E.refs[r].kind = (uint8_t)kind;
    if (group >= 0 && E.refs[r].group < 0 && E.refs[r].kind != kKindSfz) E.refs[r].group = group;
    return r;
  }
  if (E.nrefs >= kMaxPaths) return -1;
  r = E.nrefs++;
  Ref& x = E.refs[r];
  memset(&x, 0, sizeof x);
  sniprintf(x.path, sizeof x.path, "%s", path);
  x.kind = (uint8_t)kind, x.group = group, x.entry = -1;
  uint32_t h = hash_ci(path) & (kPathHash - 1);
  while (g.ht[h] >= 0) h = (h + 1) & (kPathHash - 1);
  g.ht[h] = r;
  return r;
}

// a project file into the snapshot: opened here, read a piece a step (ram_step), its entry made now
int add_ram_entry(const char* name, int kind, const char* path, const char* fmt, int ver) {
  File& f = E.rf;
  if (!f.open(path, kRead) || f.isDir()) return f.close(), -1;
  uint64_t n = f.fileSize();
  if (n > kProjectRam - g.ram_used) return f.close(), fail("the project files are over %u MB (%s)", kProjectRam >> 20, path), -2;
  int e = g.x.add_entry(name, kind);
  if (e < 0) return f.close(), fail("too many files for one bundle"), -2;
  g.ram_off[e] = g.ram_used;
  g.x.e[e].size = (uint32_t)n;
  sniprintf(g.x.e[e].src, kCardPath, "%s", path);
  sniprintf(g.x.e[e].fmt, sizeof g.x.e[e].fmt, "%s", fmt);
  g.x.e[e].fmt_ver = ver;
  g.ram_used += (uint32_t)n;
  E.rd_e = e, E.rd_off = 0;
  return e;
}
// the snapshot's read in progress: one piece; true while it is not done (false: done, or failed)
bool ram_step() {
  if (E.rd_e < 0) return false;
  MEntry& x = g.x.e[E.rd_e];
  uint8_t* at = g.ram + g.ram_off[E.rd_e];
  uint32_t n = x.size - E.rd_off < g.rp ? x.size - E.rd_off : g.rp;
  if (n && !read_at(E.rf, E.rd_off, at + E.rd_off, n)) {
    E.rf.close();
    E.rd_e = -1;
    fail("cannot read %s", x.src);
    return false;
  }
  E.rd_off += n;
  if (E.rd_off < x.size) return true;
  E.rf.close();
  if (x.kind == kKindSeq && x.size >= 5 && !memcmp(at, "SQP1", 4)) x.fmt_ver = at[4];   // (.sqp / .sqs / .prv: one packing)
  E.rd_e = -1;
  return true;
}

void value_of(const char* path, char* out, int cap) {   // "out=fx\n" -> "fx"
  char b[64];
  int64_t n = read_small(path, (uint8_t*)b, sizeof b - 1);
  out[0] = 0;
  if (n <= 0) return;
  b[n] = 0;
  char* v = strchr(b, '=');
  v = v ? v + 1 : b;
  int k = 0;
  while (v[k] && v[k] != '\n' && v[k] != '\r' && k < cap - 1) out[k] = v[k], ++k;
  out[k] = 0;
}

// ---- the pool lists' references (core/lib/pools.h; the design's 6): each sample and instrument as the library resolves
// it now (where it plays from: a moved file where it is; a missing one listed by its hint), an instrument's members as
// its group. The lists' lines are read only by pools::refs() / pools::text::parse, never here.
struct PoolRefs {
  int inst;   // the instrument ref its members belong to (-1 none)
};
void pool_ref(PoolRefs* c, bool instrument, bool member, const lib::Ref& r) {
  lib::Resolved rs;
  lib::State st = lib::resolve(r, &rs);
  const char* path = st != lib::kMissing && rs.path[0] ? rs.path : r.hint;
  if (!member) {
    int x = add_ref(path, instrument ? kKindSfz : kKindSample, -1);
    c->inst = instrument ? x : -1;
  } else if (c->inst >= 0) {
    add_ref(path, kKindSample, c->inst);
  }
}
void refs_fn(void* ctx, int list, int, int member, const lib::Ref& r) {
  pool_ref((PoolRefs*)ctx, list == pools::kInstr, member >= 0, r);
}
bool line_fn(void* ctx, const pools::text::Line& l) {
  if (l.type == pools::kSample || l.type == pools::kInstrument)
    pool_ref((PoolRefs*)ctx, l.type == pools::kInstrument, l.member, l.ref);
  return true;
}
// the current project's lists from the pool module (RAM: FLUSH waited for their writes), another's from its files
void collect_pools() {
  Manifest& x = g.x;
  PoolRefs c = {-1};
  bool live = is_current(E.key) && pools::ready() && !strcmp(pools::project(), E.key);
  for (int l = 0; live && l < pools::kLists; ++l) {
    c.inst = -1;
    if (pools::refs(l, refs_fn, &c) < 0) live = false;   // a row kept changing: the files read too (add_ref merges)
  }
  for (int e = 0; e < x.ne; ++e) {
    MEntry& m = x.e[e];
    if (m.kind != kKindPool) continue;
    const uint8_t* t = g.ram + g.ram_off[e];
    fmt::Sniffed sn;
    m.fmt_ver = fmt::text_header(fmt::Bytes{t, m.size}, m.fmt, &sn) ? sn.version : 1;
    x.add_format(m.fmt, m.fmt_ver);
    for (int l = 0; !live && l < pools::kLists; ++l) {
      if (strcmp(m.fmt, pools::format_of(l))) continue;
      char why[80];
      c.inst = -1;
      if (!pools::text::parse(l, (const char*)t, m.size, line_fn, &c, why, sizeof why))
        log(CORE_STR("bundle: %s: %s (carried as it is; the refs before it collected)"), m.src, why);
    }
  }
}

void parse_project() {
  Manifest& x = g.x;
  collect_pools();
  int s = x.find_entry("project/slots.txt"), nf = x.find_entry("project/slots.inf");
  rmp_slot_map map;
  rmp_slot_notes notes;
  memset(&map, 0, sizeof map);
  memset(&notes, 0, sizeof notes);
  if (s >= 0) rmp_slots_parse((const char*)g.ram + g.ram_off[s], x.e[s].size, &map), x.f_slots = 1;
  if (nf >= 0) rmp_notes_parse((const char*)g.ram + g.ram_off[nf], x.e[nf].size, &notes);
  for (int u = 0; u < RMP_SLOTS; ++u) {
    const rmp_slot_note& no = notes.slot[u];
    if (map.slot[u].id[0] && x.nslots < kSlots) {
      Slot& o = x.slots[x.nslots++];
      memset(&o, 0, sizeof o);
      o.slot = u + 1;
      sniprintf(o.id, sizeof o.id, "%s", map.slot[u].id);
      uint32_t v = map.slot[u].version;
      sniprintf(o.ver, sizeof o.ver, "%u.%u.%u", (unsigned)(v >> 16), (unsigned)((v >> 8) & 255), (unsigned)(v & 255));
      sniprintf(o.name, sizeof o.name, "%s", no.name);
      o.fp = no.fp;
    }
    if (no.fp && x.nprints < kSlots) x.prints[x.nprints++] = {u + 1, no.fp, no.nfp, ""};
  }
  int fx = x.find_entry("project/fxslots.txt");
  if (fx >= 0) {
    const char* t = (const char*)g.ram + g.ram_off[fx];
    uint32_t n = x.e[fx].size;
    int line = 0;
    for (uint32_t a = 0; a < n && line < kFxSlots; ++line) {
      uint32_t b = a;
      while (b < n && t[b] != '\n') ++b;
      int v = 0, k = 0;
      for (; k < 4 && a + k < b; ++k) {
        char c = t[a + k];
        int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (h < 0) break;
        v = v << 4 | h;
      }
      if (k == 4 && a + 4 < b && t[a + 4] == ' ' && (v & 0x3FFF)) {
        Print& p = x.fxprints[x.nfxprints++];
        memset(&p, 0, sizeof p);
        p.slot = line + 1, p.fp = v & 0x3FFF;
        sniprintf(p.name, sizeof p.name, "%.*s", (int)(b - a - 5), t + a + 5);
        int l = (int)strlen(p.name);
        while (l && (p.name[l - 1] == '\r' || p.name[l - 1] == ' ')) p.name[--l] = 0;
      }
      a = b + 1;
    }
  }
  for (int e = 0; e < x.ne; ++e) {
    if (x.e[e].kind == kKindSeq && x.e[e].fmt_ver > x.f_sqp) x.f_sqp = x.e[e].fmt_ver;   // (every pattern file's packing)
    if (x.e[e].kind == kKindSong) x.f_song = 1;
  }
  value_of("/.rytm/sendfx.txt", x.sendfx_route, sizeof x.sendfx_route);
  value_of("/.rytm/fxin.txt", x.fx_in, sizeof x.fx_in);
}

// an .sfz's samples (core/msamp/sfz: the parser's own path rules), into the refs as its group
bool sfz_refs(int r, int64_t n);
// an .sfz's text, a piece a step (E.sfz_r: the ref being read); true while reading
bool sfz_read_step() {
  if (E.sfz_r < 0) return false;
  uint32_t n = E.sfz_len - E.sfz_off < g.rp ? E.sfz_len - E.sfz_off : g.rp;
  if (n && !read_at(E.rf, E.sfz_off, (uint8_t*)g_sfz + E.sfz_off, n)) {
    E.rf.close();
    E.sfz_r = -1;   // unreadable: STAT lists it if it is gone
    return true;
  }
  E.sfz_off += n;
  if (E.sfz_off < E.sfz_len) return true;
  E.rf.close();
  int r = E.sfz_r;
  E.sfz_r = -1;
  sfz_refs(r, (int64_t)E.sfz_len);
  return true;
}
bool sfz_refs(int r, int64_t n) {
  Ref& s = E.refs[r];
  char dir[kCardPath];
  sniprintf(dir, sizeof dir, "%s", s.path);
  char* sl = strrchr(dir, '/');
  if (sl) *sl = 0;
  if (!dir[0]) strcpy(dir, "/");
  msamp::Desc d;
  memset(&d, 0, sizeof d);
  d.regions = g_regions, d.max_regions = kRegions, d.samples = g_samples, d.max_samples = kSamples;
  bool ok = msamp::parse(g_sfz, (int)n, dir, &d);
  if (ok)
    for (int i = 0; i < d.nsamples; ++i) add_ref(g_samples[i], kKindSample, r);
  // absolute sample= / default_path= values (a leading '/' after the '=')
  const char* t = g_sfz;
  for (int64_t i = 0; i < n; ++i) {
    const char* op = nullptr;
    if (!strncmp(t + i, "sample=", 7)) op = t + i + 7;
    else if (!strncmp(t + i, "default_path=", 13)) op = t + i + 13;
    if (!op || (i > 0 && t[i - 1] != ' ' && t[i - 1] != '\t' && t[i - 1] != '\n' && t[i - 1] != '>')) continue;
    while (*op == ' ' || *op == '\t') ++op;
    if (*op == '/') E.refs[r].abs = true;
  }
  return true;
}

bool pkg_header(const char* path, Pkg* p) {
  File f;
  uint8_t h[RMP_HEADER_BYTES];
  if (!f.open(path, kRead) || f.isDir()) return false;
  uint64_t sz = f.fileSize();
  bool ok = sz >= RMP_HEADER_BYTES && f.read(h, sizeof h) == (int)sizeof h;
  f.close();
  rmp_info info;
  if (!ok || rmp_verify_header_any(h, (uint32_t)sz, &info) != RMP_OK) return false;
  sniprintf(p->path, sizeof p->path, "%s", path);
  memcpy(p->id, info.id, 32);
  p->id[31] = 0;
  p->ver = info.version, p->size = (uint32_t)sz, p->target = info.target;
  return true;
}

// the too-big refusal: what takes the space
void too_big(uint64_t total) {
  uint64_t by[kKinds] = {0};
  int big[3] = {-1, -1, -1};
  for (int e = 0; e < g.x.ne; ++e) {
    by[g.x.e[e].kind] += g.x.e[e].size;
    for (int k = 0; k < 3; ++k)
      if (big[k] < 0 || g.x.e[e].size > g.x.e[big[k]].size) {
        for (int m = 2; m > k; --m) big[m] = big[m - 1];
        big[k] = e;
        break;
      }
  }
  char parts[120] = "", files[200] = "";
  int pl = 0, fl = 0;
  for (int k = 0; k < kKinds; ++k)
    if (by[k] >= (1u << 20))
      pl += sniprintf(parts + pl, sizeof parts - pl, "%s%s %u MB", pl ? ", " : "", kind_name(k), (unsigned)(by[k] >> 20));
  for (int k = 0; k < 3; ++k)
    if (big[k] >= 0 && g.x.e[big[k]].size >= (1u << 20)) {
      const MEntry& x = g.x.e[big[k]];
      fl += sniprintf(files + fl, sizeof files - fl, "%s%s %u MB", fl ? ", " : "", x.npaths ? g.x.p[x.first_path].path : x.name,
                      (unsigned)(x.size >> 20));
    }
  fail("too big for one bundle: %u MB, the limit is 4 GB (FAT32) - %s; largest: %s", (unsigned)(total >> 20), parts, files);
}

// ---- the archive's data: the manifest, the snapshot, the card's files (WRITE checks each file's sha256 again as it
// goes; SERVE each file's size before its first byte)
bool data_fn(void*, int zi, uint32_t off, uint8_t* buf, uint32_t n) {
  if (zi == 0) {
    if (off + n > g.man_len) return false;
    memcpy(buf, g.man + off, n);
    return true;
  }
  int e = zi - 1;
  const MEntry& x = g.x.e[e];
  if (g.ram_off[e] != kRamFile) {
    if (off + n > x.size) return false;
    memcpy(buf, g.ram + g.ram_off[e] + off, n);
  } else {
    if (E.dfe != e) {
      E.df.close();
      E.dfe = -1;
      if (!E.df.open(x.src, kRead) || E.df.fileSize() != x.size) {
        E.changed = true;
        sniprintf(E.changed_path, sizeof E.changed_path, "%s", x.src);
        return false;
      }
      E.dfe = e;
    }
    if (!read_at(E.df, off, buf, n)) return false;
  }
  if (E.st == E_WRITE) {   // in order: the running sha256 of the entry against the plan's
    if (E.we != e) E.we = e, E.wpos = 0, E.wsh.init();
    if (off == E.wpos) {
      E.wsh.update(buf, n);
      E.wpos += n;
      if (E.wpos == x.size) {
        uint8_t h[32];
        E.wsh.final(h);
        if (memcmp(h, x.sha, 32)) {
          E.changed = true;
          sniprintf(E.changed_path, sizeof E.changed_path, "%s", x.src);
          return false;
        }
      }
    }
  }
  return true;
}

// the breadcrumb (bundle_int.h mark()): this export's own on a stick, never the import's backup (E.sub: on the card)
void xmark(const char* step) {
  if (!E.sub && E.dest == kToVolume) mark(step);
}

void ex_fail_changed() {
  lib::touched(E.changed_path, nullptr, 0);   // (the library hashes it again: a stale record is not used twice)
  fail("%s changed during export - export again", E.changed_path);
}

void finalize() {
  Manifest& x = g.x;
  // the same content at two paths: stored once (samples and takes outside an instrument)
  for (int i = 0; i < x.ne; ++i) E.keep[i] = i;
  for (int i = 0; i < x.ne; ++i) {
    MEntry& a = x.e[i];
    if ((a.kind != kKindSample && a.kind != kKindTake) || a.group >= 0 || E.keep[i] != i) continue;
    for (int j = i + 1; j < x.ne; ++j) {
      MEntry& b = x.e[j];
      if ((b.kind == kKindSample || b.kind == kKindTake) && b.group < 0 && E.keep[j] == j && b.size == a.size &&
          !memcmp(a.sha, b.sha, 32)) {
        E.keep[j] = i;
        if (b.kind == kKindSample) a.kind = kKindSample;
      }
    }
  }
  // compact: kept entries in order, each with its own paths then its duplicates'
  int np = 0, ne = 0;
  static int newi[kMaxEntries];
  for (int i = 0; i < x.ne; ++i) {
    if (E.keep[i] != i) continue;
    int first = np;
    for (int j = i; j < x.ne; ++j) {
      if (E.keep[j] != i) continue;
      for (int k = 0; k < x.e[j].npaths && np < kMaxPaths; ++k) E.tmp_paths[np] = x.p[x.e[j].first_path + k], E.tmp_paths[np++].entry = ne;
    }
    newi[i] = ne;
    MEntry t = x.e[i];
    t.first_path = first, t.npaths = np - first;
    uint32_t ro = g.ram_off[i];
    x.e[ne] = t;
    g.ram_off[ne] = ro;
    ++ne;
  }
  for (int i = 0; i < ne; ++i)
    if (x.e[i].group >= 0) x.e[i].group = newi[x.e[i].group];
  x.ne = ne;
  memcpy(x.p, E.tmp_paths, sizeof(MPath) * np);
  x.np = np;
  // the head
  if (E.epoch) {
    projects::iso_date(E.epoch, x.created, sizeof x.created);
    sniprintf(x.clock, sizeof x.clock, "%s", projects::clock_name(E.clock));
  } else {
    strcpy(x.created, "");
    strcpy(x.clock, "none");
  }
  sniprintf(x.sc_version, sizeof x.sc_version, "%s", env().version);
  sniprintf(x.board, sizeof x.board, "%s", env().board);
  sniprintf(x.rytm_name, sizeof x.rytm_name, "%s", E.rytm);
  sniprintf(x.key, sizeof x.key, "%s", E.key);
  x.version = kFormat, x.min_reader = 1;
  for (int i = 0; i < x.ne; ++i) {
    int r = reader_for(x.e[i].fmt, x.e[i].fmt_ver);
    if (r > x.min_reader) x.min_reader = r;
  }
}

bool begin_layout() {
  JOut o;
  o.begin(g.man, kManifestMax);
  if (!write_manifest(g.x, o)) return fail("too many files for one manifest (1 MB)");
  g.man_len = o.n;
  memset(&g.z[0], 0, sizeof(ZEntry));
  strcpy(g.z[0].name, "manifest.json");
  g.z[0].size = g.man_len;
  g.z[0].crc = crc32(0, g.man, g.man_len);
  for (int i = 0; i < g.x.ne; ++i) {
    ZEntry& z = g.z[i + 1];
    memset(&z, 0, sizeof z);
    sniprintf(z.name, sizeof z.name, "%s", g.x.e[i].name);
    z.size = g.x.e[i].size, z.crc = g.x.e[i].crc;
  }
  uint16_t dd, dt;
  dos_time(E.epoch, &dd, &dt);
  if (!layout(g.z, g.x.ne + 1, dd, dt, &g.L)) {
    too_big(g.x.bytes());
    return false;
  }
  if (!E.sub) g.st.bytes = g.L.total;
  return true;
}

void name_bundle() {
  Fs fs;
  char stamp[20];
  if (E.epoch) {
    projects::stamp(E.epoch, stamp, sizeof stamp);
    sniprintf(E.name, sizeof E.name, "%s %s.sidecar.zip", E.key, stamp);
    for (int k = 2; k < 100; ++k) {
      sniprintf(E.final_, sizeof E.final_, "%s/%s", E.folder, E.name);
      if (!fs.exists(E.final_)) break;
      sniprintf(E.name, sizeof E.name, "%s %s %d.sidecar.zip", E.key, stamp, k);
    }
  } else {
    for (int k = 1; k < 1000; ++k) {
      sniprintf(E.name, sizeof E.name, "%s %03d.sidecar.zip", E.key, k);
      sniprintf(E.final_, sizeof E.final_, "%s/%s", E.folder, E.name);
      if (!fs.exists(E.final_)) break;
    }
  }
  sniprintf(E.final_, sizeof E.final_, "%s/%s", E.folder, E.name);
  sniprintf(E.part, sizeof E.part, "%s.part", E.final_);
}

}  // namespace

void ex_cleanup() {
  if (!E.refs) return;
  ex_close();
  if (E.part[0]) {
    Fs fs;
    fs.remove(E.part);
    E.part[0] = 0;
  }
}

// start an export (also an import's backup: sub)
bool export_begin(const char* key, int dest, const ExportOpts& o, bool sub, const char* folder) {
  ensure();
  ex_ensure();
  ex_close();
  memset(E.part, 0, sizeof E.part);
  g.x.clear();
  E.st = E_FLUSH;
  E.dest = dest, E.sub = sub, E.o = o;
  sniprintf(E.key, sizeof E.key, "%s", key);
  sniprintf(E.folder, sizeof E.folder, "%s", folder);
  rytm_name_of(key, E.rytm, sizeof E.rytm);
  project_key_sampler(E.rytm, E.sampler, sizeof E.sampler);
  E.t0_ms = now_ms();
  E.i = E.j = 0, E.nrefs = 0, E.npkg = 0, E.changed = false, E.we = -1, E.dfe = -1, E.rd_e = -1, E.sfz_r = -1;
  E.epoch = o.epoch;
  E.clock = projects::kClockBrowser;
  if (!E.epoch) projects::clock_now(&E.epoch, &E.clock);
  g.ram_used = 0;
  memset(E.known, 0, kMaxEntries + 1);
  for (int i = 0; i < kPathHash; ++i) g.ht[i] = -1;
  return true;
}

// 0.3.0 (the user, 2026-10-29): export to a stick hung core 0 at its end on the device (cause not yet confirmed; the
// end's breadcrumb names it): off - exports go to the card, a stick is read only. The host tests turn it on.
static bool g_usb_export = false;
void set_usb_export(bool on) { g_usb_export = on; }

bool start_export(const char* key, int dest, const ExportOpts& o) {
  ensure();
  if (busy()) return say("busy: %s", phase_name(g.st.phase)), false;
  finish();
  if (!key || !key[0] || !key_ok(key) || !project_exists(key)) return say("no project %s on the card", key ? key : ""), false;
  char folder[48] = "/Bundles";
  g.st.vol[0] = 0;
  if (dest == kToVolume && !g_usb_export)
    return say("export to a USB stick is off in this version: export to the card, then copy the bundle"), false;
  if (dest == kToVolume) {
    uint64_t fr;
    if (drive_of(o.vol, nullptr) != (int)strlen(o.vol) || !o.vol[0] || !vol_space(o.vol, &fr))
      return say("%s not mounted: plug in a USB stick (FAT32)", o.vol[0] ? o.vol : "USB:"), false;
    sniprintf(folder, sizeof folder, "%s%s", o.vol, kStickFolder);
    sniprintf(g.st.vol, sizeof g.st.vol, "%s", o.vol);
  }
  export_begin(key, dest, o, false, folder);
  g.job = kExport;
  g.st.job = kExport;
  sniprintf(g.st.key, sizeof g.st.key, "%s", key);
  g.st.file[0] = 0, g.st.bytes = 0;
  set_phase(kFlush, 0);
  say("exporting %s", key);
  return true;
}

// one step of the export (an import's backup too). Returns true when it reached its end (done or failed).
bool export_sub_step() {
  Manifest& x = g.x;
  if (ram_step() || sfz_read_step()) return false;
  if (g.st.phase == kFailed) return true;
  switch (E.st) {
    case E_FLUSH: {
      if (is_current(E.key)) {
        if (env().song_recording && env().song_recording()) return fail("a song take is recording: stop the song recording first"), true;
        if (now_ms() - E.t0_ms < env().flush_ms) return false;   // the idle saves (1 s after the last edit) first
        if (pools::saving() && now_ms() - E.t0_ms < env().flush_ms + kPoolsFlushMs) return false;   // the lists' too
      }
      E.st = E_FILES, E.i = 0;
      if (!E.sub) set_phase(kCollect, 0);
      return false;
    }
    case E_FILES: {   // one project file a step, into the snapshot
      for (; E.i < kNumProjectFiles && (kProjectFiles[E.i].how != kOne || kProjectFiles[E.i].old); ++E.i) {}
      if (E.i >= kNumProjectFiles) {
        E.st = E_DIRS, E.i = 0;
        return false;
      }
      const ProjectFile& r = kProjectFiles[E.i++];
      char p[kCardPath];
      sniprintf(p, sizeof p, r.card, E.key);
      if (add_ram_entry(r.entry, r.kind, p, r.fmt, 1) == -2) return true;
      ++g.st.done, bump();
      return false;
    }
    case E_DIRS: {   // a folder's files, one a step
      if (!E.dir_open) {
        for (; E.i < kNumProjectFiles && kProjectFiles[E.i].how == kOne; ++E.i) {}
        if (E.i >= kNumProjectFiles) {
          E.st = E_PARSE;
          return false;
        }
        char p[kCardPath];
        sniprintf(p, sizeof p, kProjectFiles[E.i].card, E.key);
        if (!E.dir.open(p, kRead) || !E.dir.isDir()) {
          E.dir.close();
          ++E.i;
          return false;
        }
        E.dir_open = true;
        return false;
      }
      const ProjectFile& r = kProjectFiles[E.i];
      File f;
      if (!f.openNext(&E.dir, kRead)) {
        E.dir.close(), E.dir_open = false, ++E.i;
        return false;
      }
      char nm[128];
      f.getName(nm, sizeof nm);
      bool isd = f.isDir();
      f.close();
      if (isd || nm[0] == '.' || ends_ci(nm, ".tmp") || ends_ci(nm, ".part")) return false;
      if (r.how == kSongs && (!ends_ci(nm, ".json") || !strncmp(nm, ".backup", 7))) return false;
      char p[kCardPath], name[kNameMax + 1];
      sniprintf(p, sizeof p, r.card, E.key);
      int pl = (int)strlen(p);
      sniprintf(p + pl, sizeof p - pl, "/%s", nm);
      sniprintf(name, sizeof name, "%s%s", r.entry, nm);
      if (!name_ok(name)) return false;
      int kind = r.kind;
      const char* fmt = r.fmt;
      if (r.kind == kKindSeq) {   // pNNN.sqp the working copy, .sqs the saved state, .prv the previous one (1042)
        const char* ext = strrchr(nm, '.');
        bool pat = ext && (!strcasecmp(ext, ".sqp") || !strcasecmp(ext, ".sqs") || !strcasecmp(ext, ".prv"));
        kind = pat ? kKindSeq : kKindProject;
        fmt = !pat ? "seqtracks" : !strcasecmp(ext, ".sqp") ? "sqp" : !strcasecmp(ext, ".sqs") ? "sqs" : "prv";
      }
      if (add_ram_entry(name, kind, p, fmt, 1) == -2) return true;
      ++g.st.done, bump();
      return false;
    }
    case E_PARSE: {
      parse_project();
      E.st = E_TAKES;
      E.tdepth = 0;
      if (E.o.unused_takes) {
        sniprintf(E.tpath[0], kCardPath, "/Samples/%s", E.sampler);
        if (E.tdir[0].open(E.tpath[0], kRead) && E.tdir[0].isDir()) E.tdepth = 1;
        else E.tdir[0].close();
      }
      return false;
    }
    case E_TAKES: {   // the sampler's folder, an entry a step (subfolders a few levels down)
      if (E.tdepth == 0) {
        E.st = E_SFZ, E.j = 0;
        return false;
      }
      int d = E.tdepth - 1;
      File f;
      if (!f.openNext(&E.tdir[d], kRead)) {
        E.tdir[d].close();
        --E.tdepth;
        return false;
      }
      char nm[128], p[kCardPath];
      f.getName(nm, sizeof nm);
      bool isd = f.isDir();
      f.close();
      if (nm[0] == '.') return false;
      if (sniprintf(p, sizeof p, "%s/%s", E.tpath[d], nm) >= (int)sizeof p) return false;
      if (isd) {
        if (E.tdepth < kTakeDepth) {
          sniprintf(E.tpath[E.tdepth], kCardPath, "%s", p);
          if (E.tdir[E.tdepth].open(p, kRead)) ++E.tdepth;
        }
      } else {
        add_ref(p, kKindTake, -1);
      }
      return false;
    }
    case E_SFZ: {   // an instrument a step: its samples
      for (; E.j < E.nrefs && E.refs[E.j].kind != kKindSfz; ++E.j) {}
      if (E.j >= E.nrefs) {
        E.st = E_STAT, E.j = 0;
        if (!E.sub) set_phase(kCollect, (uint64_t)E.nrefs);
        return false;
      }
      {
        int r = E.j++;
        if (!E.rf.open(E.refs[r].path, kRead) || E.rf.isDir() || E.rf.fileSize() > kTextMax) return E.rf.close(), false;
        E.sfz_r = r, E.sfz_off = 0, E.sfz_len = (uint32_t)E.rf.fileSize();
      }
      return false;
    }
    case E_STAT: {   // 16 files a step: there, and their sizes
      for (int k = 0; k < 16 && E.j < E.nrefs; ++k, ++E.j) {
        Ref& r = E.refs[E.j];
        uint64_t sz;
        r.ok = file_size(r.path, &sz) && sz <= 0xFFFFFFFFull;
        r.size = (uint32_t)sz;
        if (!r.ok && x.nmissing < x.cap_missing) sniprintf(x.missing[x.nmissing++], kCardPath, "%s", r.path);
      }
      if (!E.sub) g.st.done = (uint64_t)E.j, bump();
      if (E.j < E.nrefs) return false;
      for (int i = 0; i < E.nrefs; ++i) {   // the entries, in the refs' order (an instrument before its samples)
        Ref& r = E.refs[i];
        if (!r.ok) continue;
        char name[kNameMax + 8];
        sniprintf(name, sizeof name, "files%s", r.path);
        if (!name_ok(name)) {
          if (x.nmissing < x.cap_missing) sniprintf(x.missing[x.nmissing++], kCardPath, "%s", r.path);
          continue;
        }
        int e = x.add_entry(name, r.kind);
        if (e < 0 || !x.add_path(e, r.path)) return fail("too many files for one bundle"), true;
        r.entry = e;
        x.e[e].size = r.size;
        x.e[e].abs_refs = r.abs;
        sniprintf(x.e[e].src, kCardPath, "%s", r.path);
        g.ram_off[e] = kRamFile;
        if (r.kind == kKindSfz) sniprintf(x.e[e].fmt, sizeof x.e[e].fmt, "sfz");
        E.known[e] = lib::hashes_of(r.path, r.size, x.e[e].sha, &x.e[e].crc);   // (WRITE checks the sha256 again)
      }
      for (int i = 0; i < E.nrefs; ++i) {
        Ref& r = E.refs[i];
        if (r.entry < 0) continue;
        if (r.kind == kKindSfz) x.e[r.entry].group = r.entry;
        else if (r.group >= 0 && E.refs[r.group].entry >= 0) x.e[r.entry].group = E.refs[r.group].entry;
      }
      E.st = E_PKGSCAN, E.npkg = 0;
      E.dir_open = E.dir.open("/Plugins", kRead) && E.dir.isDir();
      if (!E.dir_open) E.dir.close();
      return false;
    }
    case E_PKGSCAN: {   // a package's header a step
      File f;
      if (!E.dir_open || !f.openNext(&E.dir, kRead)) {
        if (E.dir_open) E.dir.close(), E.dir_open = false;
        E.st = E_PKG;
        return false;
      }
      char nm[128], p[kCardPath];
      f.getName(nm, sizeof nm);
      bool isd = f.isDir();
      f.close();
      if (isd || !ends_ci(nm, ".rmp") || E.npkg >= kPkgs) return false;
      sniprintf(p, sizeof p, "/Plugins/%s", nm);
      if (pkg_header(p, &E.pkg[E.npkg])) ++E.npkg;
      return false;
    }
    case E_PKG: {
      for (int s = 0; s < x.nslots; ++s) {
        Slot& sl = x.slots[s];
        for (int t = 0; t < 2; ++t) {   // the Pi's target first; another target's package of that version carried too
          uint8_t want = t == 0 ? RMP_TARGET_A64 : RMP_TARGET_ARM32;
          for (int k = 0; k < E.npkg; ++k) {
            const Pkg& p = E.pkg[k];
            char ver[12];
            sniprintf(ver, sizeof ver, "%u.%u.%u", (unsigned)(p.ver >> 16), (unsigned)((p.ver >> 8) & 255), (unsigned)(p.ver & 255));
            if (p.target != want || strcmp(p.id, sl.id) || strcmp(ver, sl.ver)) continue;
            char name[kNameMax + 1];
            const char* tn = want == RMP_TARGET_A64 ? "a64" : "arm32";
            sniprintf(name, sizeof name, "plugins/%s-%s.%s.rmp", p.id, ver, tn);
            if (x.find_entry(name) >= 0) break;
            int e = x.add_entry(name, kKindPackage);
            if (e < 0) return fail("too many files for one bundle"), true;
            x.e[e].size = p.size;
            sniprintf(x.e[e].src, kCardPath, "%s", p.path);
            sniprintf(x.e[e].fmt, sizeof x.e[e].fmt, "rmp");
            x.e[e].fmt_ver = 1;
            g.ram_off[e] = kRamFile;
            if (!sl.pkg[0] || want == RMP_TARGET_A64) sniprintf(sl.pkg, sizeof sl.pkg, "%s", name), sniprintf(sl.target, sizeof sl.target, "%s", tn);
            break;
          }
        }
      }
      E.st = E_RYTM;
      return false;
    }
    case E_RYTM: {   // the Rytm project file: /Bundles/Rytm/<key>.<any extension>
      File d, f;
      if (d.open("/Bundles/Rytm", kRead) && d.isDir())
        while (f.openNext(&d, kRead)) {
          char nm[128];
          f.getName(nm, sizeof nm);
          bool isd = f.isDir();
          uint64_t sz = f.fileSize();
          f.close();
          const char* dot = strrchr(nm, '.');
          int stem = dot ? (int)(dot - nm) : (int)strlen(nm);
          if (isd || nm[0] == '.' || stem != (int)strlen(E.key) || strncasecmp(nm, E.key, stem)) continue;
          char name[kNameMax + 1], p[kCardPath];
          sniprintf(name, sizeof name, "rytm/%s", nm);
          sniprintf(p, sizeof p, "/Bundles/Rytm/%s", nm);
          if (!name_ok(name)) continue;
          int e = x.add_entry(name, kKindRytm);
          if (e < 0) return fail("too many files for one bundle"), true;
          x.e[e].size = (uint32_t)sz;
          sniprintf(x.e[e].src, kCardPath, "%s", p);
          g.ram_off[e] = kRamFile;
          break;
        }
      E.st = E_SIZE;
      return false;
    }
    case E_SIZE: {   // the 4 GiB limit, from the sizes (before anything is read)
      uint64_t total = (uint64_t)x.ne * (30 + 46 + 2 * kNameMax) + kAlign + kManifestMax;
      for (int i = 0; i < x.ne; ++i) total += x.e[i].size + kAlign;
      if (total > kZipMax) return too_big(x.bytes()), true;
      E.st = E_HASH, E.he = 0, E.hoff = 0, E.hopen = false;
      if (!E.sub) set_phase(kHash, x.bytes());
      return false;
    }
    case E_HASH: {   // a piece a step: the sha256 and CRC-32 of every entry
      if (E.he >= x.ne) {
        E.st = E_FINAL;
        return false;
      }
      MEntry& e = x.e[E.he];
      if (E.known[E.he]) {   // the library's hash: nothing read
        if (!E.sub) g.st.done += e.size, bump();
        ++E.he, E.hoff = 0;
        return false;
      }
      if (E.hoff == 0) E.sh.init(), E.crc = 0;
      uint32_t n = e.size - E.hoff < g.rp ? e.size - E.hoff : g.rp;
      if (g.ram_off[E.he] != kRamFile) {
        n = e.size;
        E.sh.update(g.ram + g.ram_off[E.he], n);
        E.crc = crc32(E.crc, g.ram + g.ram_off[E.he], n);
      } else if (n) {
        if (!E.hopen) {
          if (!E.hf.open(e.src, kRead) || E.hf.fileSize() != e.size) {
            sniprintf(E.changed_path, sizeof E.changed_path, "%s", e.src);
            return ex_fail_changed(), true;
          }
          E.hopen = true;
        }
        if (!read_at(E.hf, E.hoff, g.io, n)) return fail("cannot read %s", e.src), true;
        E.sh.update(g.io, n);
        E.crc = crc32(E.crc, g.io, n);
      }
      E.hoff += n;
      if (!E.sub) g.st.done += n, bump();
      if (E.hoff >= e.size) {
        E.sh.final(e.sha);
        e.crc = E.crc;
        if (E.hopen) E.hf.close(), E.hopen = false;
        ++E.he, E.hoff = 0;
      }
      return false;
    }
    case E_FINAL: {
      finalize();
      if (!begin_layout()) return true;
      if (E.dest == kToBrowser) {
        sniprintf(E.name, sizeof E.name, "%s.sidecar.zip", E.key);
        if (E.epoch) {
          char stamp[20];
          projects::stamp(E.epoch, stamp, sizeof stamp);
          sniprintf(E.name, sizeof E.name, "%s %s.sidecar.zip", E.key, stamp);
        }
        memset(E.checked, 0, (size_t)x.ne + 1);
        E.st = E_SERVE;
        sniprintf(g.st.file, sizeof g.st.file, "%s", E.name);
        set_phase(kServe, g.L.total);
        say("%s ready to download (%u MB)", E.name, (unsigned)(g.L.total >> 20));
        return true;
      }
      if (E.dest == kToVolume && !E.sub) {   // (the stick's first calls: the free names' f_stat, f_mkdir, f_open)
        sniprintf(g.st.file, sizeof g.st.file, "%s", E.folder);
        xmark("f_stat of free names, f_mkdir, f_open of the .part");
      }
      name_bundle();
      if (E.dest == kToVolume) {
        uint64_t fr;
        if (!vol_space(E.o.vol, &fr)) return fail("USB stick removed: the export stopped"), true;
        if (fr && fr < g.L.total + (1u << 20))
          return fail("USB stick full: %u MB needed, %u MB free", (unsigned)(g.L.total >> 20), (unsigned)(fr >> 20)), true;
      } else {
        uint64_t fr = card_free_bytes();
        if (fr && fr < g.L.total + (1u << 20))
          return fail("card full: %u MB needed, %u MB free", (unsigned)(g.L.total >> 20), (unsigned)(fr >> 20)), true;
      }
      Fs fs;
      fs.mkdir(E.folder);
      if (!E.wf.open(E.part, kWrite)) {
        E.part[0] = 0;
        return fail("cannot write %s", E.final_), true;
      }
      E.wopen = true, E.woff = 0, E.we = -1;
      E.st = E_WRITE;
      if (!E.sub) sniprintf(g.st.file, sizeof g.st.file, "%s", E.final_), set_phase(kWriting, g.L.total);
      xmark("the write (32 KB pieces)");
      return false;
    }
    case E_WRITE: {   // a piece a step, front to back, then renamed
      uint32_t n = g.L.total - E.woff < g.wp ? (uint32_t)(g.L.total - E.woff) : g.wp;
      if (n && !render(g.z, g.L, E.woff, g.io, n, data_fn, nullptr)) {
        if (E.changed) return ex_fail_changed(), true;
        return fail("cannot read a file of the project"), true;
      }
      if (n && !write_all(E.wf, g.io, n))
        return fail(E.dest == kToVolume ? "USB stick full or removed while writing %s" : "card full or removed while writing %s", E.final_), true;
      E.woff += n;
      if (!E.sub) g.st.done = E.woff, bump();
      if (E.woff < g.L.total) return false;
      E.st = E_CLOSE;   // the end: one file-system call a pass (2026-10-29: a stick export hung core 0 here)
      return false;
    }
    case E_CLOSE: {   // the size FatFs kept (the open handle's: no second open), then the closes
      uint64_t sz = E.wf.fileSize();
      xmark("f_close of the .part");
      bool ok = E.wf.close();
      E.wopen = false;
      xmark("f_close of the last source file");
      E.df.close(), E.dfe = -1;
      if (!ok || sz != g.L.total) return fail("the card did not keep %s", E.final_), true;
      E.st = E_RENAME;
      return false;
    }
    case E_RENAME: {   // (the next pass: one more loop of core 0 between the close and the rename)
      Fs fs;
      xmark("f_stat of the final name");
      if (fs.exists(E.final_)) xmark("f_unlink of the final name"), fs.remove(E.final_);   // (name_bundle picked a free name)
      xmark("f_rename of the .part");
      if (!fs.rename(E.part, E.final_)) return fail("cannot rename %s", E.part), true;
      E.part[0] = 0;
      E.st = E_DONE;
      if (!E.sub && E.dest == kToVolume) unmark();
      if (!E.sub) {
        set_phase(kDone, 0);
        say("exported %s to %s (%u MB, %d files)", E.key, E.final_, (unsigned)(g.L.total >> 20), x.ne);
      }
      return true;
    }
    default:
      return true;
  }
}

bool export_stick_now() { return E.dest == kToVolume && !E.sub && (E.st == E_FINAL || E.st == E_WRITE || E.st == E_CLOSE || E.st == E_RENAME); }
void export_step() {
  if (export_sub_step() && E.st == E_DONE) ex_close();
}
bool export_done() { return E.st == E_DONE; }
const char* export_path() { return E.final_; }

bool serve(uint64_t off, uint8_t* buf, uint32_t len, uint32_t* got) {
  *got = 0;
  if (g.st.phase != kServe || g.job != kExport) return false;
  if (off >= g.L.total) return false;
  IoRule r = {kPiece, kPiece, 0, ""};
  if (env().rules) env().rules(&r);
  if (!r.read_piece) {   // paused (the recorder records): the browser asks again
    sniprintf(g.st.waiting, sizeof g.st.waiting, "%s", r.why && r.why[0] ? r.why : "PAUSED");
    return true;
  }
  if (len > r.read_piece) len = r.read_piece;
  if (len > kPiece) len = kPiece;
  if (off + len > g.L.total) len = (uint32_t)(g.L.total - off);
  // every entry this range touches: its size checked once against the plan
  bool in;
  int a = entry_at(g.z, g.L, off, &in), b = entry_at(g.z, g.L, off + len - 1, &in);
  if (a < 0) a = b;
  for (int zi = a < 1 ? 1 : a; zi >= 1 && zi <= b; ++zi) {
    int e = zi - 1;
    if (E.checked[e] || g.ram_off[e] != kRamFile) continue;
    uint64_t sz;
    if (!file_size(g.x.e[e].src, &sz) || sz != g.x.e[e].size) {
      sniprintf(E.changed_path, sizeof E.changed_path, "%s", g.x.e[e].src);
      ex_fail_changed();
      return false;
    }
    E.checked[e] = 1;
  }
  if (!render(g.z, g.L, off, buf, len, data_fn, nullptr)) {
    if (E.changed) ex_fail_changed();
    else fail("cannot read a file of the project");
    return false;
  }
  *got = len;
  g.st.done = off + len > g.st.done ? off + len : g.st.done;
  return true;
}
const char* bundle_name() { return E.name; }

}  // namespace bundle
}  // namespace core
#endif
