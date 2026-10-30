// core/bundle/import.cpp - see bundle.h: the import (VERIFY, PLAN, [BACKUP], STAGE, COMMIT, LINK), one piece a step.
#include "bundle_int.h"

#if CORE_MSAMP
#include <string.h>
#include <strings.h>

#include "../lib/library.h"
#include "../msamp/sfz.h"
#include "prjslot.h"
#include "projects.h"
#include "rmp_api.h"
#include "rmp_loader.h"

namespace core {
namespace bundle {

namespace {

enum Stage { V_OPEN = 0, V_CD, V_LOCAL, V_MANIFEST, V_CHECK, V_HASH, P_INDEX, P_NAME, P_GROUPS, P_CAND, P_HASH, P_DECIDE,
             P_PROJECT, P_PKG, P_PKGHASH, P_PKGFIN, P_SPACE, I_READY, I_BACKUP, S_BEGIN, S_COPY, S_BACK, S_NEXT, C_CHECK, C_WRITE, C_TEXT, C_OPS, C_CLEAN,
             I_LINK, I_DONE };
const char* const kPkgActs[] = {"install", "reuse", "conflict", "side-by-side", "left out", "not for the Pi", "refused", "checking"};

struct Write {
  int entry;            // the manifest's entry its bytes come from (-1: text, g.text + text_off)
  uint32_t text_off, size;
  bool must_be_free;    // planned at a path that was free: still free at COMMIT, or the import stops
  uint8_t sha[32];      // what the staged file must hash to
  char to[kCardPath];
  int pkg;              // a package's plan index (-1 none)
  uint8_t kind;         // the entry's kind
  bool dropped;         // a package check refused it
};
struct PkgPlan {
  int entry, slot;      // its slot (1..5), 0 none
  uint8_t act;
  char id[32], ver[12], name[12], to[kCardPath];
};
constexpr int kWrites = kMaxPaths + 512, kPkgPlans = 16;

struct Im {
  int st;
  bool go;                     // a start_import re-plan: on to STAGE after PLAN
  char zip[kCardPath];
  File zf;
  bool zopen;
  ZRead zr;
  int nz;
  int i, k;
  uint32_t off;
  Sha256 sh;
  uint32_t crc, pcrc;
  ImportOpts o, planned;
  char name[24], key[kKeyMax], prefix[96];
  bool clash;
  int ndel;                    // replace: the old project's files to delete (g.ops[0..ndel))
  Write* w;
  int nw;
  uint32_t text_used;
  PkgPlan pk[kPkgPlans];
  int npk;
  uint64_t new_bytes, new_sample_bytes, reused_bytes;
  int nreuse, nwrite, nreloc;
  char warn[4][120];
  int nwarn;
  // stage
  File out, in;
  bool out_open, in_open;
  char staged[80];
  char dir[64];
  bool committing;             // commit.txt complete: the staging folder stays for the boot's roll-forward
  bool made_bundles;           // S_BEGIN made /Bundles (a stopped import removes it again)
  bool backup_begun;
  char rytm_to[kCardPath], rytm_name[kNameMax + 1];
  uint32_t rytm_size;
  // slot mode (a card keyed by the Rytm's slots, prjslot::migrated()): the target slot (0: name mode), the slots as PLAN
  // saw them, the replaced files' trash entry (0: none) and its info.txt's write (-1 none)
  int slot;
  struct Slot {
    char name[24];     // the Rytm's name (its slot list), else the index's ("" none known)
    uint32_t files;    // the Sidecar's files under @NNN
    bool occupied;     // the Rytm has a project there (when its list is known; else: the index names one)
  } sl[prjslot::kSlots + 1];
  bool rytm_list;     // some names are the Rytm's (its slot list since the connect, its active slot)
  uint32_t trash_id;
  int info_w;
  bool held;          // COMMIT holds the control core's jobs (prjslot::hold_jobs; checked busy() the step after)
  bool pick;          // no default slot (every one taken): the user picks one (the web page); I.slot is a stand-in
};
Im I;

int g_hash_ht[kPathHash];

void im_ensure() {
  if (I.w) return;
  I.w = new Write[kWrites];
}
void warn(const char* fmt, const char* a) {
  if (I.nwarn < 4) sniprintf(I.warn[I.nwarn++], sizeof I.warn[0], fmt, a);
}
// manifest path -> index (case ignored)
// false: a card path named twice (by two entries, or in two spellings FAT takes as one)
bool path_table() {
  for (int i = 0; i < kPathHash; ++i) g_hash_ht[i] = -1;
  for (int k = 0; k < g.m.np; ++k) {
    uint32_t h = hash_ci(g.m.p[k].path) & (kPathHash - 1);
    while (g_hash_ht[h] >= 0) {
      if (!strcasecmp(g.m.p[g_hash_ht[h]].path, g.m.p[k].path)) return false;
      h = (h + 1) & (kPathHash - 1);
    }
    g_hash_ht[h] = k;
  }
  return true;
}
int path_index(const char* p) {
  uint32_t h = hash_ci(p) & (kPathHash - 1);
  for (int n = 0; n < kPathHash; ++n, h = (h + 1) & (kPathHash - 1)) {
    int k = g_hash_ht[h];
    if (k < 0) return -1;
    if (!strcasecmp(g.m.p[k].path, p)) return k;
  }
  return -1;
}
// an entry's bytes in the archive
bool zread(int e, uint32_t off, uint8_t* buf, uint32_t n) {
  const ZEntry& z = g.iz[g.zix[e]];
  return off + n <= z.size && read_at(I.zf, z.data + off, buf, n);
}
bool zread_text(int e, char* out, uint32_t cap, uint32_t* len) {
  uint32_t n = g.m.e[e].size;
  if (n > cap) return false;
  if (g.iram_off[e] != kRamFile) {   // kept by VERIFY
    memcpy(out, g.iram + g.iram_off[e], n);
    out[n] = 0;
    *len = n;
    return true;
  }
  for (uint32_t at = 0; at < n;) {
    uint32_t k = n - at < kPiece ? n - at : kPiece;
    if (!zread(e, at, (uint8_t*)out + at, k)) return false;
    at += k;
  }
  out[n] = 0;
  *len = n;
  return true;
}
bool is_file_kind(int k) { return k == kKindSample || k == kKindTake || k == kKindSfz; }
bool is_project_kind(int k) { return k == kKindPool || k == kKindProject || k == kKindSeq || k == kKindSong; }

// the card path a project entry goes to under key (the table); false: not one this Sidecar knows
bool project_target(const char* name, const char* key, char* out, int cap) {
  for (int i = 0; i < kNumProjectFiles; ++i) {
    const ProjectFile& r = kProjectFiles[i];
    size_t el = strlen(r.entry);
    if (r.how == kOne) {
      if (!strcmp(name, r.entry)) return sniprintf(out, cap, r.card, key) < cap;
      continue;
    }
    if (strncmp(name, r.entry, el) || !name[el] || strchr(name + el, '/')) continue;
    char d[kCardPath];
    sniprintf(d, sizeof d, r.card, key);
    return sniprintf(out, cap, "%s/%s", d, name + el) < cap;
  }
  return false;
}

// an .sfz's absolute values rewritten: sample=/X (a sample placed on its own) to where it went; default_path=/X (the
// instrument relocated with it) under the prefix
bool rewrite_sfz(int e, Write& w) {
  uint32_t len;
  char* src = g.text + I.text_used;
  uint32_t room = kTextMax - I.text_used;
  if (!zread_text(e, src, room / 2, &len)) return fail("%s is too big to rewrite", g.m.e[e].name);
  char* out = src + len + 1;
  uint32_t on = 0, cap = room - len - 2;
  bool moved = g.paths[g.m.e[e].first_path].relocated;
  bool changed = false;
  for (uint32_t i = 0; i < len;) {
    int op = 0;
    if (!strncmp(src + i, "sample=", 7)) op = 7;
    else if (!strncmp(src + i, "default_path=", 13)) op = 13;
    if (op && (i == 0 || src[i - 1] == ' ' || src[i - 1] == '\t' || src[i - 1] == '\n' || src[i - 1] == '>')) {
      uint32_t v = i + op;
      while (v < len && (src[v] == ' ' || src[v] == '\t')) ++v;
      if (v < len && src[v] == '/') {
        uint32_t end = v;   // the value: to the line's end, or the next opcode (as the parser: names may hold spaces)
        while (end < len && src[end] != '\n' && src[end] != '\r') {
          if (src[end] == ' ') {
            uint32_t t = end + 1;
            while (t < len && src[t] != ' ' && src[t] != '=' && src[t] != '\n') ++t;
            if (t < len && src[t] == '=') break;
          }
          ++end;
        }
        while (end > v && src[end - 1] == ' ') --end;
        char val[kCardPath], nv[kCardPath + 96];
        sniprintf(val, sizeof val, "%.*s", (int)(end - v), src + v);
        const char* to = nullptr;
        int k = op == 7 ? path_index(val) : -1;
        if (k >= 0) to = g.paths[k].to;
        else if (moved) sniprintf(nv, sizeof nv, "%s%s", I.prefix, val), to = nv;
        if (to && strcmp(to, val)) {
          if (on + (v - i) + strlen(to) + 1 > cap) return fail("%s is too big to rewrite", g.m.e[e].name);
          memcpy(out + on, src + i, v - i);
          on += v - i;
          memcpy(out + on, to, strlen(to));
          on += (uint32_t)strlen(to);
          i = end;
          changed = true;
          continue;
        }
      }
    }
    if (on + 1 > cap) return fail("%s is too big to rewrite", g.m.e[e].name);
    out[on++] = src[i++];
  }
  if (!changed) return true;
  memmove(src, out, on);
  w.entry = -1, w.text_off = I.text_used, w.size = on;
  sha256(src, on, w.sha);
  I.text_used += on;
  return true;
}

// the hash work of PLAN: (path k, which: 0 its own path, 1 the index's candidate, 2 the relocation's path)
bool hash_file_is(const char* path, const uint8_t* sha, uint32_t size, bool* equal) {
  // one piece a call: I.off the position; returns true when finished (*equal set)
  if (I.off == 0) {
    I.in.close();
    uint64_t sz;
    if (!I.in.open(path, kRead) || I.in.isDir() || (sz = I.in.fileSize()) != size) {
      I.in.close();
      *equal = false;
      return true;
    }
    I.sh.init();
  }
  uint32_t n = size - I.off < g.rp ? size - I.off : g.rp;
  if (n && !read_at(I.in, I.off, g.io, n)) {
    I.in.close();
    *equal = false;
    return true;
  }
  I.sh.update(g.io, n);
  if (I.off == 0) I.pcrc = 0xFFFFFFFFu;   // (a package's body CRC: the bytes after its 96-byte header)
  if (I.off + n > RMP_HEADER_BYTES) {
    uint32_t a = I.off >= RMP_HEADER_BYTES ? 0 : RMP_HEADER_BYTES - I.off;
    I.pcrc = rmp_crc32_update(I.pcrc, g.io + a, n - a);
  }
  I.off += n;
  if (I.off < size) return false;
  uint8_t h[32];
  I.sh.final(h);
  I.in.close();
  I.off = 0;
  *equal = !memcmp(h, sha, 32);
  return true;
}

// slot mode: the Rytm's name and the bundle's the same project? (the Rytm keeps 15 characters, upper case)
bool same_name(const char* rytm, const char* bundle) {
  if (!rytm[0] || !bundle[0]) return false;
  size_t a = strlen(rytm), b = strlen(bundle);
  while (a && rytm[a - 1] == ' ') --a;
  while (b && bundle[b - 1] == ' ') --b;
  if (b > 15) b = 15;
  while (b && bundle[b - 1] == ' ') --b;
  return a == b && !strncasecmp(rytm, bundle, a);
}
const char* source_name() { return g.m.rytm_name[0] ? g.m.rytm_name : g.m.key; }
// the slots now: the Rytm's names (prjslot), the Sidecar's files and the index's names (one scan)
void read_slots() {
  static uint32_t files[prjslot::kSlots + 1];
  static char names[prjslot::kSlots + 1][24];
  slot_files(files, names);
  I.rytm_list = false;
  for (int k = 1; k <= prjslot::kSlots; ++k) {
    Im::Slot& x = I.sl[k];
    x.files = files[k];
    bool occ = false;
    char rn[24];
    int from_rytm = prjslot::rytm_slot(k, rn, sizeof rn, &occ);
    if (from_rytm) sniprintf(x.name, sizeof x.name, "%s", rn), x.occupied = occ, I.rytm_list = true;
    else sniprintf(x.name, sizeof x.name, "%s", names[k]), x.occupied = names[k][0] != 0;
  }
}
// the default target: the bundle's own slot when the Rytm's name there is the bundle's, else the slot with the bundle's
// name, else the first empty one (nothing on the Rytm, no Sidecar files, not the current one)
int default_slot() {
  const char* bn = source_name();
  int own = prjslot::slot_of(g.m.key);
  if (own >= 1 && own <= prjslot::kSlots && same_name(I.sl[own].name, bn)) return own;
  for (int k = 1; k <= prjslot::kSlots; ++k)
    if (same_name(I.sl[k].name, bn)) return k;
  for (int k = 1; k <= prjslot::kSlots; ++k) {
    char key[kKeyMax];
    prjslot::slot_key(k, key, sizeof key);
    if (!I.sl[k].occupied && !I.sl[k].files && !is_current(key)) return k;
  }
  return 0;   // (every slot taken: never one by default - the user picks it)
}
void choose_slot() {
  read_slots();
  I.slot = I.o.slot >= 1 && I.o.slot <= prjslot::kSlots ? I.o.slot : default_slot();
  I.pick = I.slot == 0;
  if (I.pick) I.slot = 1;   // (planned as slot 1 until one is picked; never imported so)
  prjslot::slot_key(I.slot, I.key, sizeof I.key);
  // the name the index keeps: the Rytm's for that slot (known), else the page's, else the bundle's
  const Im::Slot& x = I.sl[I.slot];
  sniprintf(I.name, sizeof I.name, "%s", x.name[0] ? x.name : I.o.name[0] ? I.o.name : source_name());
  I.clash = x.files > 0;
}

void choose_name() {
  if (prjslot::migrated()) return choose_slot();
  I.slot = 0, I.pick = false;
  char base[24];
  sniprintf(base, sizeof base, "%s", I.o.name[0] ? I.o.name : g.m.rytm_name[0] ? g.m.rytm_name : g.m.key);
  sniprintf(I.name, sizeof I.name, "%s", base);
  project_key(I.name, I.key, sizeof I.key);
  I.clash = false;
  if (I.o.mode == kReplace) return;
  if (!project_exists(I.key)) return;
  I.clash = true;
  for (int n = 2; n < 1000; ++n) {
    char num[8];
    int nl = sniprintf(num, sizeof num, " %d", n);
    int bl = (int)strlen(base);
    if (bl + nl > 23) bl = 23 - nl;
    while (bl > 0 && base[bl - 1] == ' ') --bl;
    sniprintf(I.name, sizeof I.name, "%.*s%s", bl, base, num);
    project_key(I.name, I.key, sizeof I.key);
    if (!project_exists(I.key)) return;
  }
}

bool add_write(int entry, const char* to) {
  if (I.nw >= kWrites) return fail("too many files to import");
  Write& w = I.w[I.nw++];
  memset(&w, 0, sizeof w);
  w.entry = entry, w.size = g.m.e[entry].size, w.pkg = -1, w.kind = g.m.e[entry].kind;
  w.must_be_free = true;
  memcpy(w.sha, g.m.e[entry].sha, 32);
  sniprintf(w.to, sizeof w.to, "%s", to);
  return true;
}

void decide() {
  Manifest& m = g.m;
  // on its own: (a) the same content at its path, (b) the index's file, (c) its path free (under /Samples, not a
  // machine's scanned folder), (d) under /Samples/Projects/<key>/
  for (int k = 0; k < m.np; ++k) {
    PathPlan& p = g.paths[k];
    const MEntry& e = m.e[p.entry];
    if (!is_file_kind(e.kind) || p.group >= 0) continue;
    const char* path = m.p[k].path;
    Fs fs;
    if (p.ok_orig) p.act = kActReuse, sniprintf(p.to, kCardPath, "%s", path);
    else if (p.ok_cand) p.act = kActReuse, sniprintf(p.to, kCardPath, "%s", p.cand), p.relocated = true;
    else if (under(path, "/Samples") && !in_scanned(path) && !fs.exists(path)) p.act = kActWrite, sniprintf(p.to, kCardPath, "%s", path);
    else {
      p.relocated = true;
      sniprintf(p.to, kCardPath, "%s%s", I.prefix, path);
      p.act = p.ok_pre ? kActReuse : kActWrite;
    }
  }
  // an instrument and the samples it names relatively: placed together (its relative paths keep working)
  for (int e = 0; e < m.ne; ++e) {
    if (m.e[e].kind != kKindSfz) continue;
    bool all_same = true, orig_ok = true;
    for (int k = 0; k < m.np; ++k) {
      PathPlan& p = g.paths[k];
      if (p.group != e) continue;
      Fs fs;
      all_same = all_same && p.ok_orig;
      orig_ok = orig_ok && (p.ok_orig || (under(m.p[k].path, "/Samples") && !in_scanned(m.p[k].path) && !fs.exists(m.p[k].path)));
    }
    for (int k = 0; k < m.np; ++k) {
      PathPlan& p = g.paths[k];
      if (p.group != e) continue;
      if (all_same || orig_ok) {
        p.act = p.ok_orig ? kActReuse : kActWrite;
        sniprintf(p.to, kCardPath, "%s", m.p[k].path);
      } else {
        p.relocated = true;
        sniprintf(p.to, kCardPath, "%s%s", I.prefix, m.p[k].path);
        p.act = p.ok_pre ? kActReuse : kActWrite;
      }
    }
  }
}

void plan_writes() {
  Manifest& m = g.m;
  I.nw = 0, I.text_used = 0, I.new_bytes = I.new_sample_bytes = I.reused_bytes = 0, I.nreuse = I.nwrite = I.nreloc = 0;
  for (int k = 0; k < m.np; ++k) {
    PathPlan& p = g.paths[k];
    const MEntry& e = m.e[p.entry];
    if (!is_file_kind(e.kind)) continue;
    if (strlen(p.to) >= (size_t)kCardPath - 1) {
      fail("a path would be too long: %s", p.to);
      return;
    }
    I.nreloc += p.relocated;
    if (p.act == kActReuse) {
      ++I.nreuse, I.reused_bytes += e.size;
      continue;
    }
    ++I.nwrite;
    if (!add_write(p.entry, p.to)) return;
    if (e.kind == kKindSfz && e.abs_refs && !rewrite_sfz(p.entry, I.w[I.nw - 1])) return;
    I.new_bytes += I.w[I.nw - 1].size, I.new_sample_bytes += I.w[I.nw - 1].size;
  }
}

bool plan_project() {
  Manifest& m = g.m;
  for (int e = 0; e < m.ne; ++e) {
    const MEntry& x = m.e[e];
    if (!is_project_kind(x.kind)) continue;
    char to[kCardPath];
    if (!project_target(x.name, I.key, to, sizeof to)) {
      warn("%s: not a file this Sidecar knows (left out)", x.name);
      continue;
    }
    if (!add_write(e, to)) return false;
    I.w[I.nw - 1].must_be_free = I.o.mode != kReplace;   // (written as it is: the pool lists' refs resolve by hash)
    I.new_bytes += I.w[I.nw - 1].size;
  }
  return true;
}

bool plan_packages() {
  Manifest& m = g.m;
  I.npk = 0;
  for (int e = 0; e < m.ne && I.npk < kPkgPlans; ++e) {
    if (m.e[e].kind != kKindPackage) continue;
    PkgPlan& p = I.pk[I.npk++];
    memset(&p, 0, sizeof p);
    p.entry = e;
    for (int s = 0; s < m.nslots; ++s)
      if (!strcmp(m.slots[s].pkg, m.e[e].name)) p.slot = m.slots[s].slot, sniprintf(p.name, sizeof p.name, "%s", m.slots[s].name);
    uint8_t h[RMP_HEADER_BYTES];
    rmp_info info;
    if (m.e[e].size < RMP_HEADER_BYTES || !zread(e, 0, h, sizeof h) || rmp_verify_header_any(h, m.e[e].size, &info) != RMP_OK ||
        !rmp_id_ok(info.id)) {
      p.act = kPkgRefused;
      continue;
    }
    memcpy(p.id, info.id, 31);
    p.id[31] = 0;
    sniprintf(p.ver, sizeof p.ver, "%u.%u.%u", (unsigned)(info.version >> 16), (unsigned)((info.version >> 8) & 255),
              (unsigned)(info.version & 255));
    if (info.target != RMP_TARGET_A64) { p.act = kPkgNotPi; continue; }
    if (!I.o.packages || (p.slot && (I.o.skip_slots >> (p.slot - 1) & 1))) { p.act = kPkgLeftOut; continue; }
    sniprintf(p.to, sizeof p.to, "/Plugins/%s-%s.rmp", p.id, p.ver);
    // installed: the same id and version (any file name) -> reuse when the bytes are the same, else a conflict
    p.act = kPkgInstall;
    bool other_version = false;
    File d, f;
    if (d.open("/Plugins", kRead) && d.isDir())
      while (f.openNext(&d, kRead)) {
        char nm[128], path[kCardPath];
        f.getName(nm, sizeof nm);
        bool isd = f.isDir();
        uint64_t sz = f.fileSize();
        f.close();
        if (isd || !ends_ci(nm, ".rmp")) continue;
        sniprintf(path, sizeof path, "/Plugins/%s", nm);
        File pf;
        uint8_t ph[RMP_HEADER_BYTES];
        rmp_info pi;
        bool ok = pf.open(path, kRead) && pf.read(ph, sizeof ph) == (int)sizeof ph;
        pf.close();
        if (!ok || rmp_verify_header_any(ph, (uint32_t)sz, &pi) != RMP_OK || strcmp(pi.id, p.id) || pi.target != RMP_TARGET_A64) continue;
        if (pi.version != info.version) {
          other_version = true;
          continue;
        }
        p.act = kPkgCheck;   // its bytes compared in P_PKGHASH
        sniprintf(p.to, sizeof p.to, "%s", path);
        break;
      }
    if (p.act == kPkgInstall) {
      Fs fs;
      if (fs.exists(p.to)) p.act = kPkgConflict;
      else if (other_version) p.act = kPkgSide;
    }
  }
  return true;
}
bool package_writes() {
  for (int i = 0; i < I.npk; ++i) {
    PkgPlan& p = I.pk[i];
    if (p.act != kPkgInstall && p.act != kPkgSide) continue;
    if (!add_write(p.entry, p.to)) return false;
    I.w[I.nw - 1].pkg = i;
    I.new_bytes += g.m.e[p.entry].size;
  }
  return true;
}

// a folder with no file below it (empty folders and dot entries only: an empty per-project folder counts as no files)
bool empty_dir(const char* p, int depth = 0) {
  File d, f;
  if (depth > 4 || !d.open(p, kRead) || !d.isDir()) return false;
  bool empty = true;
  char nm[128], sub[kCardPath + 130];
  while (empty && f.openNext(&d, kRead)) {
    f.getName(nm, sizeof nm);
    bool isd = f.isDir();
    f.close();
    if (nm[0] == '.' && !isd) continue;   // (.DS_Store and the like)
    sniprintf(sub, sizeof sub, "%s/%s", p, nm);
    empty = isd && empty_dir(sub, depth + 1);
  }
  d.close();
  return empty;
}

// slot mode's replace: the old files' trash entry (its number: past every entry there and the control core's next) and
// its info.txt, staged as a write (a new entry's folder: must be free at COMMIT)
bool plan_trash() {
  uint32_t id = prjslot::trash_next();
  File d, f;
  if (d.open("/.rytm/trash", kRead) && d.isDir())
    while (f.openNext(&d, kRead)) {
      char nm[64];
      f.getName(nm, sizeof nm);
      f.close();
      uint32_t v = 0;
      bool num = nm[0] != 0;
      for (const char* c = nm; *c && num; ++c) num = *c >= '0' && *c <= '9' && v < 100000000u, v = v * 10 + (uint32_t)(*c - '0');
      if (num && v + 1 > id) id = v + 1;
    }
  d.close();
  char from[kCardPath], to[kCardPath], was[24];
  if (!prjslot::trash_item(0, I.key, id, from, to, sizeof to)) return fail("the project trash is not known to this build");
  char* sl = strrchr(to, '/');
  sniprintf(sl, sizeof to - (size_t)(sl - to), "/info.txt");
  rytm_name_of(I.key, was, sizeof was);
  if (I.nw >= kWrites) return fail("too many files to import");
  char* t = g.text + I.text_used;
  int n = prjslot::trash_info(I.key, was, "import", t, (int)(kTextMax - I.text_used));
  if (n <= 0 || (uint32_t)n >= kTextMax - I.text_used) return fail("too many files to import");
  Write& w = I.w[I.nw++];
  memset(&w, 0, sizeof w);
  w.entry = -1, w.text_off = I.text_used, w.size = (uint32_t)n, w.pkg = -1, w.must_be_free = true;
  sha256(t, (uint32_t)n, w.sha);
  sniprintf(w.to, sizeof w.to, "%s", to);
  I.text_used += (uint32_t)n, I.new_bytes += (uint32_t)n;
  I.trash_id = id, I.info_w = I.nw - 1;
  return true;
}
bool backup_first() { return I.o.mode == kReplace && !I.slot; }   // name mode's replace: the old project exported first

// the Rytm's step (the summary, the done screen); slot mode: the bundle holds the Sidecar's side, the Rytm's project
// goes into the slot (Transfer, or already there)
void step_text(char* out, int cap) {
  const char* rf = I.rytm_name[0] ? I.rytm_name + 5 : "";
  if (!I.slot) {
    if (rf[0]) sniprintf(out, cap, "Send %s to the Rytm with Elektron Transfer, then save it on the Rytm as %s.", rf, I.name);
    else sniprintf(out, cap, "This bundle has no Rytm project file: load the project on the Rytm yourself, then save it on the Rytm as %s.", I.name);
    return;
  }
  const Im::Slot& x = I.sl[I.slot];
  if (I.pick)
    sniprintf(out, cap, "Every slot holds a project: pick the slot this bundle's Sidecar side goes to (its files there go to the "
              "project trash only with replace).");
  else if (rf[0])
    sniprintf(out, cap, "Send %s to the Rytm's slot %03d with Elektron Transfer, then load slot %03d on the Rytm.", rf, I.slot, I.slot);
  else if (same_name(x.name, source_name()))
    sniprintf(out, cap, "This bundle holds only the Sidecar's side, no Rytm project file. Slot %03d on the Rytm holds %s: load it on the Rytm.",
              I.slot, x.name);
  else
    sniprintf(out, cap, "This bundle holds only the Sidecar's side, no Rytm project file: put the Rytm project into slot %03d yourself "
              "(Elektron Transfer, or SAVE TO %03d) before you import, then load it. A SAVE TO onto %03d after the import moves these files to the trash.",
              I.slot, I.slot, I.slot);
}

void stage_name(int i, char* out, int cap) { sniprintf(out, cap, "%s/f%04d", I.dir, i); }

// a staged package (its bytes already read back: I.pcrc is the CRC of its body): header, target, CRC, entry table
bool check_package(const char* staged, uint32_t size) {
  File f;
  uint8_t h[RMP_HEADER_BYTES];
  rmp_info info;
  if (!f.open(staged, kRead) || !read_at(f, 0, h, sizeof h)) return f.close(), false;
  if (rmp_verify_header_any(h, size, &info) != RMP_OK || info.target != RMP_TARGET_A64 || !rmp_id_ok(info.id)) return f.close(), false;
  uint8_t t[8];
  bool ok = ~I.pcrc == info.crc32 && read_at(f, RMP_HEADER_BYTES + info.entry_offset, t, 8);
  f.close();
  uint32_t magic, api;
  memcpy(&magic, t, 4);
  memcpy(&api, t + 4, 4);
  return ok && magic == RMP_PLUGIN_MAGIC && api >= RMP_API_MIN && api <= RMP_API;
}

void close_im() {
  if (I.zopen) I.zf.close(), I.zopen = false;
  if (I.out_open) I.out.close(), I.out_open = false;
  I.in.close();
  I.in_open = false;
}

}  // namespace

void im_cleanup() {
  prjslot::hold_jobs(false), I.held = false;   // (a stopped or failed import: the control core's jobs go on)
  if (!I.w) return;
  close_im();
  if (I.dir[0] && !I.committing) {
    remove_tree(I.dir);
    Fs fs;
    fs.remove("/Bundles/.staging");
    if (I.made_bundles) fs.remove("/Bundles");   // (only when empty: an import that stopped leaves the card as it was)
  }
  I.dir[0] = 0;
  ex_cleanup();
}

bool start_verify(const char* zip) {
  ensure();
  im_ensure();
  if (busy()) return say("busy: %s", phase_name(g.st.phase)), false;
  finish();
  close_im();
  memset(I.dir, 0, sizeof I.dir);
  I.committing = false, I.backup_begun = false;
  sniprintf(I.zip, sizeof I.zip, "%s", zip);
  drive_of(zip, g.st.vol);
  if (g.st.vol[0]) {
    uint64_t fr;
    if (!vol_space(g.st.vol, &fr)) {
      say("%s not mounted: plug in the USB stick", g.st.vol);
      g.st.vol[0] = 0;
      return false;
    }
  }
  I.st = V_OPEN, I.i = I.k = 0, I.off = 0, I.go = false;
  I.o = ImportOpts();
  I.nwarn = 0;
  g.job = kImport;
  g.st.job = kImport;
  g.st.key[0] = 0;
  sniprintf(g.st.file, sizeof g.st.file, "%s", zip);
  set_phase(kVerify, 0);
  say("checking %s", zip);
  return true;
}

ImportOpts import_opts() { return I.o; }

bool start_import(const ImportOpts& o) {
  if (g.job != kImport || g.st.phase != kReady) return say("nothing to import: check a bundle first"), false;
  bool same = !strcmp(o.name, I.o.name) && o.mode == I.o.mode && o.packages == I.o.packages && o.skip_slots == I.o.skip_slots;
  if (I.slot) {   // slot mode: the slot READY planned unless another is asked for; the name only for a slot without one
    if (I.pick && (o.slot < 1 || o.slot > prjslot::kSlots))
      return say("every slot is taken: pick the slot on the web page (Project tab)"), false;
    int sl = o.slot >= 1 && o.slot <= prjslot::kSlots ? o.slot : I.slot;
    char k[kKeyMax];
    prjslot::slot_key(sl, k, sizeof k);
    if (is_current(k)) return say("%s is the Rytm's current project: switch the Rytm to another project to replace it", k), false;
    if (I.sl[sl].files && o.mode != kReplace)
      return say("slot %03d has Sidecar files (%u): replace them (they go to the project trash) or pick another slot", sl,
                 (unsigned)I.sl[sl].files), false;
    same = sl == I.slot && o.mode == I.o.mode && o.packages == I.o.packages && o.skip_slots == I.o.skip_slots &&
           (!o.name[0] || !strcmp(o.name, I.name) || I.sl[sl].name[0]);
    I.pick = false;   // (only once a picked slot is accepted: a refused one leaves no stand-in to import into)
    I.o = o;
    I.o.slot = sl;
    if (!o.name[0] || I.sl[sl].name[0]) sniprintf(I.o.name, sizeof I.o.name, "%s", same ? I.name : "");
    if (same) I.st = S_BEGIN;
    else I.st = P_NAME, I.go = true;
    set_phase(same ? kStage : kPlan, 0);
    return true;
  }
  I.o = o;
  if (o.mode == kReplace) {
    char k[kKeyMax];
    project_key(o.name[0] ? o.name : g.m.rytm_name[0] ? g.m.rytm_name : g.m.key, k, sizeof k);
    if (is_current(k)) return say("%s is the Rytm's current project: switch the Rytm to another project to replace it", k), false;
  }
  if (same) I.st = o.mode == kReplace ? I_BACKUP : S_BEGIN;
  else I.st = P_NAME, I.go = true;
  set_phase(same && o.mode == kReplace ? kBackup : same ? kStage : kPlan, 0);
  return true;
}

// the archive is read in VERIFY, by PLAN's instruments (when VERIFY did not keep one) and packages, and by STAGE
bool import_stick_now() { return I.st <= V_HASH || I.st == P_GROUPS || I.st == P_PKG || I.st == S_COPY; }
bool import_stick_needed() { return I.st < C_CHECK; }
void import_step() {
  Manifest& m = g.m;
  char err[160];
  switch (I.st) {
    // ---- VERIFY: nothing written
    case V_OPEN: {
      if (!I.zf.open(I.zip, kRead) || I.zf.isDir()) return (void)fail("cannot open %s", I.zip);
      I.zopen = true;
      uint64_t fs = I.zf.fileSize();
      if (fs > kZipMax) return (void)fail("over 4 GB: not a v1 bundle");
      uint8_t tail[kEocdTail];
      if (fs < kEocdTail || !read_at(I.zf, fs - kEocdTail, tail, kEocdTail)) return (void)fail("not a bundle (too short)");
      if (!eocd(tail, kEocdTail, fs, &I.zr, err, sizeof err)) return (void)fail("%s", err);
      if (I.zr.n > kMaxEntries + 1 || I.zr.cd_size > (uint64_t)(kMaxEntries + 1) * (46 + kNameMax)) return (void)fail("too many entries for this Sidecar");
      g.st.bytes = fs;
      I.st = V_CD, I.off = 0;
      return;
    }
    case V_CD: {   // the central directory, a piece a step
      uint32_t n = (uint32_t)I.zr.cd_size - I.off < g.rp ? (uint32_t)I.zr.cd_size - I.off : g.rp;
      if (n && !read_at(I.zf, I.zr.cd_off + I.off, g.cd + I.off, n)) return (void)fail("cannot read %s", I.zip);
      I.off += n;
      if (I.off < I.zr.cd_size) return;
      if (!central(g.cd, I.zr, g.iz, kMaxEntries + 1, err, sizeof err)) return (void)fail("%s", err);
      I.nz = I.zr.n, I.i = 0, I.st = V_LOCAL;
      return;
    }
    case V_LOCAL: {   // 16 local headers a step
      static uint8_t h[30 + kNameMax + kAlign + 16];
      for (uint32_t got = 0; got < g.rp && I.i < I.nz; ++I.i) {   // (a piece's worth of headers)
        ZEntry& z = g.iz[I.i];
        uint64_t next = I.i + 1 < I.nz ? g.iz[I.i + 1].local : I.zr.cd_off;
        uint32_t want = (uint32_t)(next - z.local < sizeof h ? next - z.local : sizeof h);
        uint32_t nl = (uint32_t)strlen(z.name);
        if (want > 30 + nl + kAlign + 8) want = 30 + nl + kAlign + 8;   // the name and a bundle's padding at most
        if (want > g.rp) want = 30 + nl < g.rp ? g.rp : 30 + nl;
        if (got && got + want > g.rp) break;   // the next header: the next step
        got += want;
        if (!read_at(I.zf, z.local, h, want)) return (void)fail("cannot read %s", I.zip);
        if (!local(h, want, &z, next, err, sizeof err)) return (void)fail("%s", err);
      }
      if (I.i < I.nz) return;
      I.st = V_MANIFEST, I.off = 0;
      return;
    }
    case V_MANIFEST: {
      const ZEntry& z = g.iz[0];
      if (strcmp(z.name, "manifest.json")) return (void)fail("not a Sidecar bundle (manifest.json is not the first entry)");
      if (z.size > kManifestMax) return (void)fail("manifest: over 1 MB");
      uint32_t n = z.size - I.off < g.rp ? z.size - I.off : g.rp;   // a piece a step
      if (n && !read_at(I.zf, z.data + I.off, (uint8_t*)g.man + I.off, n)) return (void)fail("cannot read %s", I.zip);
      I.off += n;
      if (I.off < z.size) return;
      I.off = 0;
      if (crc32(0, g.man, z.size) != z.crc) return (void)fail("damaged: manifest.json (CRC)");
      if (!read_manifest(g.man, z.size, &m, err, sizeof err)) return (void)fail("%s", err);
      I.st = V_CHECK;
      return;
    }
    case V_CHECK: {   // every manifest entry in the archive, with its size; names in their own layout's prefix
      for (int e = 0; e < m.ne; ++e) {
        const MEntry& x = m.e[e];
        int zi = e + 1 < I.nz && !strcmp(g.iz[e + 1].name, x.name) ? e + 1 : -1;
        for (int k = 1; zi < 0 && k < I.nz; ++k)
          if (!strcmp(g.iz[k].name, x.name)) zi = k;
        if (zi < 0) return (void)fail("damaged: %s is in the manifest, not in the bundle", x.name);
        if (g.iz[zi].size != x.size) return (void)fail("damaged: %s (sizes differ)", x.name);
        const char* pre = is_project_kind(x.kind) ? "project/" : is_file_kind(x.kind) ? "files/" : x.kind == kKindPackage ? "plugins/" : "rytm/";
        if (strncmp(x.name, pre, strlen(pre))) return (void)fail("a %s entry outside %s: %s", kind_name(x.kind), pre, x.name);
        if (is_file_kind(x.kind) && x.npaths < 1) return (void)fail("manifest: %s has no card path", x.name);
        g.zix[e] = zi;
      }
      set_phase(kVerify, m.bytes());
      I.i = 0, I.off = 0, I.st = V_HASH;
      g.iram_used = 0;
      return;
    }
    case V_HASH: {   // a piece a step: every entry's sha256 (and its CRC) before anything is written
      if (I.i >= m.ne) {
        I.st = P_INDEX;
        set_phase(kPlan, 0);
        return;
      }
      const MEntry& x = m.e[I.i];
      if (I.off == 0) I.sh.init(), I.crc = 0;
      uint32_t n = x.size - I.off < g.rp ? x.size - I.off : g.rp;
      if (n && !zread(I.i, I.off, g.io, n)) return (void)fail("cannot read %s", I.zip);
      I.sh.update(g.io, n);
      I.crc = crc32(I.crc, g.io, n);
      if (I.off == 0) {   // the files PLAN reads (instruments): kept in RAM as they pass
        bool keep = x.kind == kKindSfz && x.size <= kImportRam - g.iram_used;
        g.iram_off[I.i] = keep ? g.iram_used : kRamFile;
        if (keep) g.iram_used += x.size;
      }
      if (g.iram_off[I.i] != kRamFile) memcpy(g.iram + g.iram_off[I.i] + I.off, g.io, n);
      I.off += n;
      g.st.done += n;
      bump();
      if (I.off < x.size) return;
      uint8_t h[32];
      I.sh.final(h);
      if (memcmp(h, x.sha, 32) || I.crc != g.iz[g.zix[I.i]].crc) return (void)fail("damaged: %s (its bytes are not what the manifest says)", x.name);
      ++I.i, I.off = 0;
      return;
    }
    // ---- PLAN: nothing written
    case P_INDEX:   // (was the dedupe index /.rytm/bundles/imported.txt: the library is it now, RAM only)
      if (!path_table()) return (void)fail("manifest: a card path is named twice");
      I.st = P_NAME;
      return;
    case P_NAME: {
      choose_name();
      sniprintf(I.prefix, sizeof I.prefix, "/Samples/Projects/%s", I.key);
      {   // paths under it too long for a path hint (lib::kPath: 159 characters): the folder's name shortened, a hash
          // added (5.3)
        size_t longest = 0;
        for (int k = 0; k < m.np; ++k) longest = strlen(m.p[k].path) > longest ? strlen(m.p[k].path) : longest;
        if (strlen(I.prefix) + longest > 159) sniprintf(I.prefix, sizeof I.prefix, "/Samples/Projects/%.8s~%04x", I.key, (unsigned)(hash_ci(I.key) & 0xFFFF));
      }
      sniprintf(g.st.key, sizeof g.st.key, "%s", I.key);
      if (I.slot && I.go && is_current(I.key))   // (READY shows it; the import is refused)
        return (void)fail("%s is the Rytm's current project: switch the Rytm to another project to replace it", I.key);
      if (I.slot && I.go && I.clash && I.o.mode != kReplace)
        return (void)fail("slot %03d has Sidecar files: replace them (they go to the project trash) or pick another slot", I.slot);
      if (!I.slot && I.o.mode == kReplace && is_current(I.key))
        return (void)fail("%s is the Rytm's current project: switch the Rytm to another project to replace it", I.key);
      for (int k = 0; k < m.np; ++k) {
        PathPlan& p = g.paths[k];
        memset(&p, 0, sizeof p);
        p.entry = m.p[k].entry, p.group = -1;
      }
      I.i = 0, I.st = P_GROUPS;
      return;
    }
    case P_GROUPS: {   // an instrument a step: the samples it names relatively move with it
      for (; I.i < m.ne && m.e[I.i].kind != kKindSfz; ++I.i) {}
      if (I.i >= m.ne) {
        I.k = 0, I.i = 0, I.st = P_CAND;
        return;
      }
      int e = I.i++;
      uint32_t len;
      if (!zread_text(e, g.text, kTextMax, &len)) return;
      const char* sp = m.p[m.e[e].first_path].path;
      for (int j = 0; j < m.e[e].npaths; ++j) g.paths[m.e[e].first_path + j].group = e;
      char dir[kCardPath];
      sniprintf(dir, sizeof dir, "%s", sp);
      char* sl = strrchr(dir, '/');
      if (sl) *sl = 0;
      static msamp::Region regions[2048];
      static char samples[1024][msamp::kPath];
      msamp::Desc d;
      memset(&d, 0, sizeof d);
      d.regions = regions, d.max_regions = 2048, d.samples = samples, d.max_samples = 1024;
      if (!msamp::parse(g.text, (int)len, dir, &d)) return;
      for (int s = 0; s < d.nsamples; ++s) {
        int k = path_index(samples[s]);
        if (k < 0 || g.paths[k].group >= 0) continue;
        // named literally (an absolute sample=): placed on its own, its value rewritten if it moves
        if (strstr(g.text, samples[s])) continue;
        g.paths[k].group = e;
      }
      return;
    }
    case P_CAND: {   // the candidates: the same size at its own path, the index's file, the relocation's path
      for (int n = 0; n < 32 && I.k < m.np; ++n, ++I.k) {
        PathPlan& p = g.paths[I.k];
        const MEntry& x = m.e[p.entry];
        if (!is_file_kind(x.kind)) continue;
        uint64_t sz;
        p.ok_orig = file_size(m.p[I.k].path, &sz) && sz == x.size;   // (hashed in P_HASH)
        p.ok_cand = false;
        if (p.group < 0 && !p.ok_orig) {
          // the library's file with that content (core/lib: RAM, hashed records), else the board's hook; either is a
          // hint: P_HASH reads it again before it is reused
          p.ok_cand = lib::find_file(x.sha, x.size, p.cand, kCardPath, m.p[I.k].path) ||
                      (env().find_by_hash && env().find_by_hash(x.sha, x.size, p.cand, kCardPath));
          if (p.ok_cand && !strcasecmp(p.cand, m.p[I.k].path)) p.ok_cand = false;
        }
        char pre[kCardPath + 96];
        sniprintf(pre, sizeof pre, "%s%s", I.prefix, m.p[I.k].path);
        p.ok_pre = strlen(pre) < (size_t)kCardPath && file_size(pre, &sz) && sz == x.size;
      }
      if (I.k < m.np) return;
      I.k = 0, I.i = 0, I.off = 0, I.st = P_HASH;
      return;
    }
    case P_HASH: {   // a piece a step: each candidate's content compared
      for (;;) {
        if (I.k >= m.np) {
          I.st = P_DECIDE;
          return;
        }
        PathPlan& p = g.paths[I.k];
        bool* flag = I.i == 0 ? &p.ok_orig : I.i == 1 ? &p.ok_cand : &p.ok_pre;
        if (!*flag) {
          if (++I.i > 2) I.i = 0, ++I.k;
          continue;
        }
        char pre[kCardPath + 96];
        const char* path = I.i == 0 ? m.p[I.k].path : I.i == 1 ? p.cand : (sniprintf(pre, sizeof pre, "%s%s", I.prefix, m.p[I.k].path), pre);
        bool eq;
        if (!hash_file_is(path, m.e[p.entry].sha, m.e[p.entry].size, &eq)) return;
        *flag = eq;
        if (++I.i > 2) I.i = 0, ++I.k;
        return;
      }
    }
    case P_DECIDE:
      decide();
      plan_writes();
      if (g.st.phase == kFailed) return;
      I.st = P_PROJECT;
      return;
    case P_PROJECT:
      I.ndel = backup_first() ? replace_deletes(I.key) : 0;
      I.trash_id = 0, I.info_w = -1;
      if (!plan_project()) return;
      if (I.slot && I.o.mode == kReplace && I.clash && !plan_trash()) return;
      I.st = P_PKG;
      return;
    case P_PKG:
      if (!plan_packages()) return;
      I.k = 0, I.off = 0, I.st = P_PKGHASH;
      return;
    case P_PKGHASH: {   // an installed package of the same id and version: the same bytes? (a piece a step)
      for (; I.k < I.npk && I.pk[I.k].act != kPkgCheck; ++I.k) {}
      if (I.k < I.npk) {
        PkgPlan& p = I.pk[I.k];
        bool same;
        if (!hash_file_is(p.to, m.e[p.entry].sha, m.e[p.entry].size, &same)) return;
        p.act = same ? kPkgReuse : kPkgConflict;
        return;
      }
      I.st = P_PKGFIN;
      return;
    }
    case P_PKGFIN: {
      if (!package_writes()) return;
      // the Rytm file: offered at /Bundles/Rytm/<new key>.<its extension>
      I.rytm_to[0] = I.rytm_name[0] = 0;
      for (int e = 0; e < m.ne; ++e) {
        if (m.e[e].kind != kKindRytm) continue;
        const char* dot = strrchr(m.e[e].name, '.');
        sniprintf(I.rytm_to, sizeof I.rytm_to, "/Bundles/Rytm/%s%s", I.key, dot && !strchr(dot, '/') ? dot : "");
        sniprintf(I.rytm_name, sizeof I.rytm_name, "%s", m.e[e].name);
        I.rytm_size = m.e[e].size;
        if (!add_write(e, I.rytm_to)) return;
        I.w[I.nw - 1].must_be_free = false;   // (our own folder: a new copy replaces an older one)
        break;
      }
      I.st = P_SPACE;
      return;
    }
    case P_SPACE: {
      uint64_t fr = card_free_bytes();
      if (fr && fr < I.new_bytes + kSpaceMargin)
        return (void)fail("card full: %u MB needed, %u MB free - delete recordings or old bundles", (unsigned)((I.new_bytes + kSpaceMargin) >> 20),
                          (unsigned)(fr >> 20));
      I.planned = I.o;
      if (I.go) {
        I.go = false;
        I.st = backup_first() ? I_BACKUP : S_BEGIN;
        set_phase(backup_first() ? kBackup : kStage, 0);
        return;
      }
      I.st = I_READY;
      set_phase(kReady, 0);
      if (I.slot)
        say("ready to import %s to slot %03d (%s): %u MB new, %u MB already on the card", g.m.key, I.slot, I.name,
            (unsigned)(I.new_bytes >> 20), (unsigned)(I.reused_bytes >> 20));
      else
        say("ready to import %s as %s: %u MB new, %u MB already on the card", g.m.key, I.name, (unsigned)(I.new_bytes >> 20),
            (unsigned)(I.reused_bytes >> 20));
      return;
    }
    case I_BACKUP: {   // replace: the project as it is now, exported first (never a silent overwrite)
      if (!I.backup_begun) {
        ExportOpts eo;
        export_begin(I.key, kToCard, eo, true, "/Bundles/.backup");
        I.backup_begun = true;
        return;
      }
      if (!export_sub_step()) return;
      I.backup_begun = false;
      if (g.st.phase == kFailed) return;
      I.st = S_BEGIN;
      return;
    }
    // ---- STAGE: every new file into /Bundles/.staging/<job>/, read back
    case S_BEGIN: {
      sniprintf(I.dir, sizeof I.dir, "/Bundles/.staging/%u", (unsigned)++g.job_n);
      Fs fs;
      I.made_bundles = !fs.exists("/Bundles");
      if (fs.exists(I.dir)) remove_tree(I.dir);
      if (!fs.mkdir(I.dir) || !is_dir(I.dir)) return (void)fail("cannot make %s", I.dir);
      uint64_t total = 0;
      for (int i = 0; i < I.nw; ++i) total += I.w[i].size;
      set_phase(kStage, total * 2);
      I.i = 0, I.off = 0, I.st = S_COPY;
      return;
    }
    case S_COPY: {
      if (I.i >= I.nw) {
        I.st = C_CHECK, I.off = 0, I.i = 0;
        return;
      }
      Write& w = I.w[I.i];
      if (I.off == 0) {
        stage_name(I.i, I.staged, sizeof I.staged);
        if (!I.out.open(I.staged, kWrite)) return (void)fail("cannot write %s", I.staged);
        I.out_open = true;
      }
      uint32_t n = w.size - I.off < g.wp ? w.size - I.off : g.wp;
      if (n) {
        if (w.entry >= 0) {
          if (!zread(w.entry, I.off, g.io, n)) return (void)fail("cannot read %s", I.zip);
        } else {
          memcpy(g.io, g.text + w.text_off + I.off, n);
        }
        if (!write_all(I.out, g.io, n)) return (void)fail("card full or removed while writing %s", w.to);
      }
      I.off += n;
      g.st.done += n;
      bump();
      if (I.off < w.size) return;
      bool ok = I.out.close();
      I.out_open = false;
      if (!ok) return (void)fail("card full or removed while writing %s", w.to);
      I.off = 0, I.st = S_BACK;
      return;
    }
    case S_BACK: {   // the staged file read back: its sha256 as planned
      Write& w = I.w[I.i];
      bool eq;
      stage_name(I.i, I.staged, sizeof I.staged);
      uint32_t before = I.off;
      if (!hash_file_is(I.staged, w.sha, w.size, &eq)) {
        g.st.done += I.off - before, bump();
        return;
      }
      g.st.done += w.size - before;
      if (!eq) return (void)fail("the card did not keep %s (read back differs)", w.to);
      if (w.pkg >= 0 && !check_package(I.staged, w.size)) {   // refused: that machine is left out, the rest imports
        w.dropped = true;
        I.pk[w.pkg].act = kPkgRefused;
        Fs fs;
        fs.remove(I.staged);
      }
      ++I.i, I.off = 0, I.st = S_COPY;
      return;
    }
    // ---- COMMIT: commit.txt (the renames, then END), then the renames
    case C_CHECK: {   // before anything is decided: nothing changed under the plan (32 paths a step)
      Fs fs;
      if (I.i == 0 && I.slot) {
        // the control core's jobs held until the renames are done (its trash jobs number their entries too, a job may
        // move this slot's files): held first, then none running - never two at once
        if (!I.held) return prjslot::hold_jobs(true), (void)(I.held = true);   // (the control core sees it by the next step)
        if (prjslot::busy()) return prjslot::hold_jobs(false), (void)(I.held = false);   // (its queue runs; tried again)
        if (is_current(I.key))
          return (void)fail("%s became the Rytm's current project: switch the Rytm to another project to replace it", I.key);
        // the slot's files now: replace moves every one there to the trash (one entry), new needs none
        char from[kCardPath], to[kCardPath];
        I.ndel = 0;
        for (int k = 0; prjslot::trash_item(k, I.key, I.trash_id, from, to, kCardPath); ++k) {
          if (!fs.exists(from) || (is_dir(from) && empty_dir(from))) continue;   // (an empty folder: the new files go in)
          if (I.o.mode != kReplace || !I.trash_id) return (void)fail("%s got Sidecar files during the import: import again", I.key);
          if (fs.exists(to)) return (void)fail("the project trash's entry %06lu appeared during the import: import again", (unsigned long)I.trash_id);
          Op& op = g.ops[I.ndel++];
          op.kind = 'M';
          sniprintf(op.a, sizeof op.a, "%s", from), sniprintf(op.b, sizeof op.b, "%s", to);
        }
        if (!I.ndel && I.info_w >= 0) I.w[I.info_w].dropped = true;   // (gone meanwhile: no empty trash entry)
      }
      if (I.i == 0 && !I.slot && I.o.mode == kReplace && is_current(I.key))
        return (void)fail("%s became the Rytm's current project: switch the Rytm to another project to replace it", I.key);
      for (int k = 0; k < 32 && I.i < I.nw; ++k, ++I.i) {
        const Write& w = I.w[I.i];
        if (!w.dropped && w.must_be_free && fs.exists(w.to)) return (void)fail("%s appeared on the card during the import: import again", w.to);
      }
      if (I.i < I.nw) return;
      // a replaced project's file that the import writes again is not deleted first: run again at boot, the delete
      // would remove the new file after its rename (the rename replaces it anyway)
      int keep = 0;
      for (int i = 0; i < I.ndel; ++i) {
        bool written = false;   // (a move to the trash is never left out: it runs only while its target is free)
        for (int j = 0; j < I.nw && !written && g.ops[i].kind != 'M'; ++j) written = !I.w[j].dropped && !strcasecmp(I.w[j].to, g.ops[i].a);
        if (!written) g.ops[keep++] = g.ops[i];
      }
      I.ndel = keep;
      I.st = C_WRITE;
      return;
    }
    case C_WRITE: {
      char* t = g.text;   // (the rewritten texts are staged: their room is free)
      uint32_t n = 0;
      set_phase(kCommit, (uint64_t)(I.ndel + I.nw));
      for (int i = 0; i < I.ndel; ++i)
        n += g.ops[i].kind == 'M' ? (uint32_t)sniprintf(t + n, kTextMax - n, "M %s %s\n", g.ops[i].a, g.ops[i].b)
                                  : (uint32_t)sniprintf(t + n, kTextMax - n, "%c %s\n", g.ops[i].kind, g.ops[i].a);
      for (int i = 0; i < I.nw; ++i) {
        if (I.w[i].dropped) continue;
        stage_name(i, I.staged, sizeof I.staged);
        if (n + 300 > kTextMax) return (void)fail("too many files to commit");
        n += (uint32_t)sniprintf(t + n, kTextMax - n, "R %s %s\n", I.staged, I.w[i].to);
      }
      n += (uint32_t)sniprintf(t + n, kTextMax - n, "END\n");
      I.text_used = n, I.off = 0;
      char cp[96];
      sniprintf(cp, sizeof cp, "%s/commit.txt", I.dir);
      if (!I.out.open(cp, kWrite)) return (void)fail("cannot write %s", cp);
      I.out_open = true;
      I.st = C_TEXT;
      return;
    }
    case C_TEXT: {   // commit.txt, a piece a step; complete (its END on the card) the import is decided
      char cp[96];
      sniprintf(cp, sizeof cp, "%s/commit.txt", I.dir);
      uint32_t n = I.text_used - I.off < g.wp ? I.text_used - I.off : g.wp;
      if (n && !write_all(I.out, (const uint8_t*)g.text + I.off, n)) return (void)fail("cannot write %s", cp);
      I.off += n;
      if (I.off < I.text_used) return;
      bool ok = I.out.close();
      I.out_open = false;
      uint64_t sz;
      if (!ok || !file_size(cp, &sz) || sz != I.text_used) return (void)fail("cannot write %s", cp);
      I.committing = true;   // from here the import completes: now, or at the next boot
      I.i = 0, I.st = C_OPS;
      return;
    }
    case C_OPS: {   // 16 a step (FatFs renames within a volume move directory entries only)
      for (int k = 0; k < 16 && I.i < I.ndel + I.nw; ++k, ++I.i) {
        if (I.i < I.ndel) {
          if (!run_op(g.ops[I.i].kind, g.ops[I.i].a, g.ops[I.i].kind == 'M' ? g.ops[I.i].b : ""))
            return (void)fail(g.ops[I.i].kind == 'M' ? "cannot move %s to the project trash: the slot is half moved - restart the Sidecar now (the boot finishes it)" : "cannot delete %s (the boot finishes it)",
                              g.ops[I.i].a);
          if (g.ops[I.i].kind == 'D') lib::touched(g.ops[I.i].a, nullptr, 0);   // (gone from the library)
          continue;
        }
        Write& w = I.w[I.i - I.ndel];
        if (w.dropped) continue;
        stage_name(I.i - I.ndel, I.staged, sizeof I.staged);
        if (!run_op('R', I.staged, w.to))
          return (void)fail(I.slot ? "cannot move %s into place: the slot is half moved - restart the Sidecar now (the boot finishes it)"
                                   : "cannot move %s into place (the boot finishes it)", w.to);
        // the library: the file in place with the sha256 STAGE read back (no second read); the CRC-32 the archive's
        // (a rewritten text's: not known here, 0)
        lib::touched(w.to, w.sha, w.entry >= 0 ? g.iz[g.zix[w.entry]].crc : 0);
      }
      g.st.done = (uint64_t)I.i;
      bump();
      if (I.i < I.ndel + I.nw) return;
      I.st = C_CLEAN;
      return;
    }
    case C_CLEAN: {
      char cp[96];
      sniprintf(cp, sizeof cp, "%s/commit.txt", I.dir);
      Fs fs;
      if (!fs.remove(cp)) return (void)fail("cannot remove %s (the boot finishes it)", cp);
      I.committing = false;
      prjslot::hold_jobs(false), I.held = false;
      remove_tree(I.dir);
      fs.remove("/Bundles/.staging");
      I.dir[0] = 0;
      I.st = I_LINK;
      set_phase(kLink, 0);
      return;
    }
    case I_LINK: {   // the index (the control core applies it), the rescans
      if (!projects::mark_imported(I.key, I.name)) return;
      bool pkg = false;
      for (int i = 0; i < I.npk; ++i) pkg |= I.pk[i].act == kPkgInstall || I.pk[i].act == kPkgSide;
      if (env().changed) {
        if (pkg) env().changed("/Plugins");
        env().changed("/Samples");
      }
      close_im();
      I.st = I_DONE;
      set_phase(kDone, 0);
      if (I.slot)
        say("imported to slot %03d (%s): %d files written, %d already on the card%s", I.slot, I.name, I.nwrite, I.nreuse,
            I.ndel ? "; its old Sidecar files are in the project trash" : "");
      else
        say("imported as %s: %d files written, %d already on the card", I.name, I.nwrite, I.nreuse);
      return;
    }
    default:
      return;
  }
}

const char* pkg_action_name(int a) { return a >= 0 && a <= kPkgCheck ? kPkgActs[a] : "?"; }

bool plan_info(PlanInfo* o) {
  memset(o, 0, sizeof *o);
  if (g.job != kImport || !I.w || (g.st.phase != kReady && g.st.phase != kDone)) return false;
  Manifest& m = g.m;
  sniprintf(o->name, sizeof o->name, "%s", I.name);
  sniprintf(o->from, sizeof o->from, "%s", m.rytm_name[0] ? m.rytm_name : m.key);
  o->clash = I.clash, o->replace = I.o.mode == kReplace;
  o->new_bytes = I.new_bytes, o->reused_bytes = I.reused_bytes;
  o->write = (uint32_t)I.nwrite, o->reuse = (uint32_t)I.nreuse, o->relocated = (uint32_t)I.nreloc;
  o->samples = (uint32_t)(m.count(kKindSample) + m.count(kKindTake));
  o->songs = (uint32_t)m.count(kKindSong), o->patterns = (uint32_t)m.count(kKindSeq), o->missing = (uint32_t)m.nmissing;
  for (int i = 0; i < I.npk && o->nmach < kPlanMachines; ++i) {
    const PkgPlan& p = I.pk[i];
    auto& x = o->mach[o->nmach++];
    x.slot = (uint8_t)p.slot, x.action = p.act;
    sniprintf(x.name, sizeof x.name, "%s", p.name[0] ? p.name : p.id);
    sniprintf(x.id, sizeof x.id, "%s", p.id);
    sniprintf(x.ver, sizeof x.ver, "%s", p.ver);
  }
  if (I.rytm_name[0]) sniprintf(o->rytm_file, sizeof o->rytm_file, "%s", I.rytm_name + 5);
  step_text(o->step, sizeof o->step);
  o->slot = (uint8_t)I.slot, o->pick = I.pick;
  return true;
}

int plan_json(char* out, int cap) {
  if (g.job != kImport || !I.w) return 0;
  Manifest& m = g.m;
  JOut o;
  o.begin(out, (uint32_t)cap);
  o.raw("{\"bundle\":"), o.str(I.zip), o.raw(",\"from\":{\"key\":"), o.str(m.key), o.raw(",\"rytm_name\":"), o.str(m.rytm_name);
  o.raw(",\"created\":"), o.str(m.created), o.raw(",\"sidecar\":"), o.str(m.sc_version), o.raw(",\"board\":"), o.str(m.board);
  o.raw("},\"name\":"), o.str(I.name), o.raw(",\"key\":"), o.str(I.key), o.raw(",\"clash\":"), o.raw(I.clash ? "true" : "false");
  o.raw(",\"mode\":"), o.str(I.o.mode == kReplace ? "replace" : "new");
  if (I.slot) {   // slot mode: the target, and every slot as PLAN saw it ([name, Sidecar files, the Rytm's current] by slot)
    o.raw(",\"slot\":"), o.num((uint64_t)(I.pick ? 0 : I.slot)), o.raw(",\"rytm_list\":"), o.raw(I.rytm_list ? "true" : "false");
    o.raw(",\"slots\":[");
    for (int k = 1; k <= prjslot::kSlots; ++k) {
      char key[kKeyMax];
      prjslot::slot_key(k, key, sizeof key);
      if (k > 1) o.raw(",");
      o.raw("["), o.str(I.sl[k].name), o.raw(","), o.num(I.sl[k].files), o.raw(is_current(key) ? ",1]" : ",0]");
    }
    o.raw("]");
  }
  o.raw(",\"new_bytes\":"), o.num(I.new_bytes), o.raw(",\"new_sample_bytes\":"), o.num(I.new_sample_bytes);
  o.raw(",\"reused_bytes\":"), o.num(I.reused_bytes), o.raw(",\"files\":{\"write\":"), o.num((uint64_t)I.nwrite);
  o.raw(",\"reuse\":"), o.num((uint64_t)I.nreuse), o.raw(",\"relocated\":"), o.num((uint64_t)I.nreloc), o.raw("}");
  o.raw(",\"totals\":{\"samples\":"), o.num((uint64_t)(m.count(kKindSample) + m.count(kKindTake))), o.raw(",\"songs\":");
  o.num((uint64_t)m.count(kKindSong)), o.raw(",\"patterns\":"), o.num((uint64_t)m.count(kKindSeq)), o.raw(",\"bytes\":");
  o.num(m.bytes()), o.raw("}");
  o.raw(",\"relocated\":[");
  int shown = 0;
  for (int k = 0; k < m.np && shown < 20; ++k)
    if (g.paths[k].relocated && is_file_kind(m.e[g.paths[k].entry].kind)) {
      if (shown++) o.raw(",");
      o.raw("{\"from\":"), o.str(m.p[k].path), o.raw(",\"to\":"), o.str(g.paths[k].to), o.raw("}");
    }
  o.raw("],\"machines\":[");
  for (int i = 0; i < I.npk; ++i) {
    const PkgPlan& p = I.pk[i];
    if (i) o.raw(",");
    o.raw("{\"slot\":"), o.num((uint64_t)p.slot), o.raw(",\"id\":"), o.str(p.id), o.raw(",\"ver\":"), o.str(p.ver);
    o.raw(",\"name\":"), o.str(p.name), o.raw(",\"action\":"), o.str(kPkgActs[p.act]), o.raw(",\"to\":"), o.str(p.to);
    o.raw(",\"code\":true}");
  }
  o.raw("],\"missing\":[");
  for (int i = 0; i < m.nmissing && i < 20; ++i) {
    if (i) o.raw(",");
    o.str(m.missing[i]);
  }
  o.raw("],\"warnings\":[");
  int nw = 0;
  for (int i = 0; i < I.nwarn; ++i) {
    if (nw++) o.raw(",");
    o.str(I.warn[i]);
  }
  if (m.f_sqp > 4) {   // (seqx: v4 = E2's blocks, read here; a newer pattern file locks, never played without its data)
    if (nw++) o.raw(",");
    o.str("patterns were saved by a newer sequencer (.sqp v5+): they are locked here (not played, not changed)");
  }
  o.raw("],\"rytm\":");
  char step[300];
  if (I.rytm_name[0]) o.raw("{\"file\":"), o.str(I.rytm_name), o.raw(",\"to\":"), o.str(I.rytm_to), o.raw(",\"size\":"), o.num(I.rytm_size), o.raw("}");
  else o.raw("null");
  step_text(step, sizeof step);
  o.raw(",\"step\":"), o.str(step);
  o.raw(",\"phase\":"), o.str(phase_name(g.st.phase)), o.raw(",\"msg\":"), o.str(g.st.msg), o.raw("}");
  return o.over ? 0 : (int)o.n;
}

}  // namespace bundle
}  // namespace core
#endif
