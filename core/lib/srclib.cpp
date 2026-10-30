// core/lib/srclib.cpp - see srclib.h. The control core's and cold (the displays read the listings racily). The card's
// folders are listed from the library's RAM index (lib::list): no card I/O here.
#include "../platform.h"

#if CORE_MSAMP
#include "srclib.h"

#include <string.h>
#include <strings.h>

#include "library.h"
#include "pools.h"

namespace core {
namespace srclib {

namespace {

constexpr int kItemName = 64;
struct Item {
  char name[kItemName];
  uint8_t dir;
  uint32_t size;
};
struct Listing {
  char path[kPath];            // "" the root
  uint8_t cls;                 // 0 audio (.wav .aif .aiff), 1 .sfz
  bool used, done, stale;      // stale: listed again at the next step (srclib::changed)
  uint32_t used_ms;
  uint32_t lgen;               // the library's gen() it is listed from (another: listed again)
  lib::ListPos pos;            // a listing in pieces (kListPer entries looked at a step): where the next goes on
  uint32_t sig;                // its entries' hash (listed again the same: the browsers' views not moved)
  volatile int n;
  Item* items;                 // [kListing]
  uint16_t* order;             // [kListing] sorted: folders, then files, by name
};
Listing cache[kCache];
struct Browser {
  char cwd[kPath];
  int li;                      // its listing (-1: not yet / a virtual folder)
  uint32_t gen;                // a cd
};
Browser br[kKinds + 1];   // (+ kFill)
bool card = false, did_begin = false;
Stats st;

const char* const kDirs[kKinds] = {"granular", "wavetable", "multisample"};
// the card's own folders the library leaves out at its root (the Sidecar's and the boot partition's)
const char* const kHidden[] = {"Plugins", "PRESETS", "JUNO", "DX7", "System Volume Information", "overlays", "LOST.DIR",
                               "$RECYCLE.BIN", "Recordings.tmp", "Bundles"};   // Bundles: project bundles, not sources
const char* const kVInputs = "@INPUTS";
const char* const kVBuiltin = "@BUILT-IN";

CORE_COLD int cls_of(int k) { return k == kMs ? 1 : 0; }
CORE_COLD bool ends(const char* n, const char* ext) {
  int l = (int)strlen(n), e = (int)strlen(ext);
  return l > e && strcasecmp(n + l - e, ext) == 0;
}
CORE_COLD bool accepts_cls(int c, const char* n) {
  if (!n[0] || n[0] == '.') return false;
  return c == 1 ? ends(n, ".sfz") : ends(n, ".wav") || ends(n, ".aif") || ends(n, ".aiff");
}
// the machine's virtual folders at the root (GRANULAR: @INPUTS @BUILT-IN; WAVETABLE: @BUILT-IN)
CORE_COLD int nvirt(int k) { return k == kGr ? 2 : k == kWt ? 1 : 0; }
CORE_COLD const char* virt(int k, int i) { return k == kGr && i == 0 ? kVInputs : kVBuiltin; }
// the browser is in a virtual folder: its pools type (0 none)
CORE_COLD int in_virt(int k) {
  const char* c = br[k].cwd;
  if (k == kGr && !strcmp(c, kVInputs)) return pools::kInput;
  if ((k == kGr || k == kWt) && !strcmp(c, kVBuiltin)) return pools::kBuiltin;
  return 0;
}
CORE_COLD int virt_count(int k, int type) { return type == pools::kInput ? pools::kInputs : pools::builtins(k); }
CORE_COLD const char* virt_name(int k, int type, int i) {
  return type == pools::kInput ? pools::input_name(i) : pools::builtin_name(k, i);
}
CORE_COLD void ensure() {
  if (did_begin) return;
  did_begin = true;
  for (int i = 0; i < kCache; ++i) {
    cache[i].items = new Item[kListing];
    cache[i].order = new uint16_t[kListing];
    cache[i].used = false, cache[i].n = 0;
  }
  for (int k = 0; k <= kFill; ++k) br[k].cwd[0] = 0, br[k].li = -1, br[k].gen = 1;
}
CORE_COLD bool before(const Item& a, const Item& b) {
  if (a.dir != b.dir) return a.dir > b.dir;
  return strcasecmp(a.name, b.name) < 0;
}
CORE_COLD bool in_use(int li) {
  for (int k = 0; k <= kFill; ++k)
    if (br[k].li == li) return true;
  return false;
}
CORE_COLD void start_read(int li) {   // listing li listed anew at the next step (shown as being read until then)
  Listing& L = cache[li];
  L.n = 0, L.done = false, L.stale = false;
  L.pos = lib::ListPos{0, 0, false};
}
CORE_COLD int find_listing(const char* path, int c) {
  for (int i = 0; i < kCache; ++i)
    if (cache[i].used && cache[i].cls == c && !strcmp(cache[i].path, path)) return i;
  return -1;
}
// the listing of browser k's folder: found in the cache, or a slot for it (least recently used, not in use) read anew
CORE_COLD void attach(int k) {
  Browser& b = br[k];
  if (in_virt(k)) {
    b.li = -1;
    return;
  }
  int c = cls_of(k);
  int li = find_listing(b.cwd, c);
  if (li < 0) {
    for (int i = 0; i < kCache && li < 0; ++i)
      if (!cache[i].used) li = i;
    if (li < 0) {
      for (int i = 0; i < kCache; ++i) {
        if (in_use(i) && !(b.li == i)) continue;
        if (li < 0 || (int32_t)(cache[i].used_ms - cache[li].used_ms) < 0) li = i;
      }
      if (li < 0) li = 0;
      ++st.evicted;
    }
    Listing& L = cache[li];
    sniprintf(L.path, sizeof L.path, "%s", b.cwd);
    L.cls = (uint8_t)c, L.used = true;
    start_read(li);
  }
  cache[li].used_ms = now_ms();
  b.li = li;
}
// one entry of the library's folder into the listing, sorted as it comes (folders first, then by name)
CORE_COLD bool add_item(void* ctx, const char* name, bool dir, uint64_t size) {
  Listing& L = *(Listing*)ctx;
  ++st.entries_read;
  if (name[0] == '.' || strlen(name) >= kItemName) return true;
  if (dir ? (!L.path[0] && hidden_name(name)) : !accepts_cls(L.cls, name)) return true;
  int n = L.n;
  if (n >= kListing) return false;
  Item& it = L.items[n];
  memcpy(it.name, name, strlen(name) + 1);
  it.dir = dir, it.size = size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)size;
  int lo = 0, hi = n;   // where it goes in the order
  while (lo < hi) {
    int m = (lo + hi) / 2;
    if (before(L.items[L.order[m]], it)) lo = m + 1;
    else hi = m;
  }
  memmove(L.order + lo + 1, L.order + lo, sizeof(uint16_t) * (n - lo));
  L.order[lo] = (uint16_t)n;
  L.n = n + 1;
  return true;
}
CORE_COLD uint32_t sig_of(const Listing& L) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < L.n; ++i) {
    const Item& it = L.items[L.order[i]];
    for (const char* c = it.name; *c; ++c) h = (h ^ (uint8_t)*c) * 16777619u;
    h = (h ^ it.dir ^ it.size * 2654435761u) * 16777619u;
  }
  return h;
}
// one step: a browser's listing not listed yet, or listed from an older snapshot (the library saw the card change),
// listed from the library's RAM index (lib::list: no card I/O), kListPer of its entries looked at a step (a filtering
// browser on a big flat folder: a few steps, each within ~1 ms on the Pi); until the boot load has a whole snapshot the
// folder stays "being read". A listing listed again keeps showing as done while it fills (racily, as before).
CORE_COLD bool read_step() {
  uint32_t g = lib::gen();
  int li = -1;
  for (int i = 0; i < kCache && li < 0; ++i)
    if (cache[i].used && !cache[i].done && in_use(i)) li = i;
  for (int i = 0; i < kCache && li < 0; ++i)
    if (cache[i].used && cache[i].done && (cache[i].stale || cache[i].lgen != g || cache[i].pos.at) && in_use(i)) li = i;
  if (li < 0) return false;
  Listing& L = cache[li];
  if (!card) {
    L.n = 0, L.done = true, L.lgen = g, L.stale = false, L.pos = lib::ListPos{0, 0, false};
    for (int k = 0; k <= kFill; ++k)
      if (br[k].li == li) ++br[k].gen;
    return true;
  }
  bool fresh = L.pos.at == 0;
  int n0 = L.n;
  if (fresh) L.n = 0;
  int r = lib::list(L.path, add_item, &L, &L.pos, kListPer);
  if (r == -1) {   // no whole snapshot yet
    if (fresh) L.n = n0;
    return false;
  }
  ++st.steps;
  if (r == -2) {   // the snapshot changed under a listing in pieces: from the start at the next step
    L.pos = lib::ListPos{0, 0, false};
    L.n = 0;
    return true;
  }
  if (!L.pos.done) {   // more at the next step
    if (!L.done)
      for (int k = 0; k <= kFill; ++k)
        if (br[k].li == li) ++br[k].gen;
    return true;
  }
  bool was = L.done;
  uint32_t old = L.sig;
  L.done = true, L.lgen = L.pos.gen, L.stale = false;
  L.pos = lib::ListPos{0, 0, false};
  ++st.dirs_read;
  L.sig = sig_of(L);
  if (!was || L.sig != old)
    for (int k = 0; k <= kFill; ++k)
      if (br[k].li == li) ++br[k].gen;
  return true;
}
CORE_COLD const char* base_of(const char* p) {
  const char* b = strrchr(p, '/');
  return b ? b + 1 : p;
}
CORE_COLD int kind_of(const char* a, const char** rest) {
  for (int k = 0; k < kKinds; ++k) {
    const char* t = k == kGr ? "gr" : k == kWt ? "wt" : "ms";
    if (!strncmp(a, t, 2) && (a[2] == ':' || !a[2])) {
      *rest = a[2] ? a + 3 : "";
      return k;
    }
  }
  return -1;
}
}  // namespace

// ---- the machines' list entries from the pool lists (1024 a compose: plain copies, no printf - the control core's
// pass stays short)
namespace {
inline void cpy(char* d, int cap, const char* s) {
  int i = 0;
  for (; s[i] && i < cap - 1; ++i) d[i] = s[i];
  d[i] = 0;
}
}  // namespace
CORE_COLD int compose(int k, Entry* out) {
  static const char* const kGrp[kBanks] = {"BANK A", "BANK B", "BANK C", "BANK D", "BANK E", "BANK F", "BANK G", "BANK H"};
  for (int c = 0; c < kEntries; ++c) {
    const pools::Entry& p = pools::at(k, c);
    Entry& e = out[c];
    e.path[0] = 0, e.fixed = -1, e.type = p.type, e.st = p.st, e.size = 0;
    cpy(e.grp, sizeof e.grp, kGrp[bank_of(c)]);
    switch (p.type) {
      case pools::kOff: e.lib = kLibOff, cpy(e.name, sizeof e.name, "OFF"); break;
      case pools::kEmpty: e.lib = kLibHole, cpy(e.name, sizeof e.name, "---"); break;
      case pools::kInput:
      case pools::kBuiltin:
        e.lib = kLibNone, e.fixed = (int8_t)p.id;
        cpy(e.name, sizeof e.name, p.name);
        cpy(e.grp, sizeof e.grp, p.type == pools::kInput ? "LIVE" : "BUILT-IN");
        break;
      default:
        cpy(e.name, sizeof e.name, p.name);
        e.size = (uint32_t)(p.ref.size > 0xFFFFFFF0u ? 0xFFFFFFF0u : p.ref.size);
        if (pools::playable(p)) e.lib = kLibNone, cpy(e.path, sizeof e.path, p.path);
        else e.lib = kLibMissing;
        break;
    }
  }
  return kEntries;
}
CORE_COLD uint32_t pool_sig(int k) { return pools::sig(k); }

CORE_COLD bool hidden_name(const char* n) {
  for (const char* h : kHidden)
    if (!strcasecmp(n, h)) return true;
  return false;
}
CORE_COLD bool begun() { return did_begin; }
CORE_COLD void begin(bool c) {
  ensure();
  card = c;
}
CORE_COLD bool step() {
  ensure();
  return read_step();
}
CORE_COLD void changed(const char* path) {
  ensure();
  for (int i = 0; i < kCache; ++i) {
    Listing& L = cache[i];
    if (!L.used) continue;
    int l = (int)strlen(L.path);
    bool under = !strncasecmp(path, L.path, l) && (path[l] == '/' || !path[l]);   // a change in it (or below)
    bool above = !strncasecmp(L.path, path, strlen(path));                      // it moved / went
    if (!under && !above) continue;
    if (in_use(i)) L.stale = true, L.pos.at = 0, L.pos.done = false;   // listed again (from the start; shown meanwhile)
    else L.used = false;
  }
}

CORE_COLD const char* machine_dir(int k) { return k >= 0 && k < kKinds ? kDirs[k] : ""; }
CORE_COLD bool accepts(int k, const char* name) { return accepts_cls(cls_of(k), base_of(name)); }

CORE_COLD const char* cwd(int k) { ensure(); return br[k].cwd; }
CORE_COLD bool cd(int k, const char* path) {
  ensure();
  char p[kPath];
  sniprintf(p, sizeof p, "%s", path && strcmp(path, "/") ? path : "");
  int l = (int)strlen(p);
  while (l > 0 && p[l - 1] == '/') p[--l] = 0;
  bool v = (k == kGr && !strcmp(p, kVInputs)) || ((k == kGr || k == kWt) && !strcmp(p, kVBuiltin));
  if (p[0] && p[0] != '/' && !v) return false;
  if (p[0] && card && !v && k != kFill && lib::is_folder(p) == 0) return false;   // (RAM: no snapshot yet, taken)
  Browser& b = br[k];
  sniprintf(b.cwd, sizeof b.cwd, "%s", p);
  ++b.gen;
  attach(k);
  return true;
}
CORE_COLD void open(int k) {
  ensure();
  if (k < 0 || k > kFill) return;
  char p[kPath];
  sniprintf(p, sizeof p, "%s", br[k].cwd);
  if (!cd(k, p)) cd(k, "");
}
CORE_COLD void up(int k) {
  ensure();
  char p[kPath];
  sniprintf(p, sizeof p, "%s", br[k].cwd);
  char* s = strrchr(p, '/');
  if (s) *s = 0;
  else p[0] = 0;
  cd(k, p);
}
CORE_COLD bool listing_done(int k) {   // (any core: no listing attached here)
  if (!did_begin) return false;
  if (in_virt(k)) return true;
  int li = br[k].li;
  return li >= 0 && cache[li].done;
}
CORE_COLD int count(int k) {
  if (!did_begin) return 0;
  int t = in_virt(k);
  if (t) return virt_count(k, t);
  int li = br[k].li;
  return (br[k].cwd[0] ? 0 : nvirt(k)) + (li >= 0 ? cache[li].n : 0);
}
CORE_COLD bool item(int k, int i, const char** name, bool* dir, uint32_t* size) {
  ensure();
  int t = in_virt(k);
  if (t) {
    if (i < 0 || i >= virt_count(k, t)) return false;
    if (name) *name = virt_name(k, t, i);
    if (dir) *dir = false;
    if (size) *size = 0;
    return true;
  }
  if (!br[k].cwd[0]) {
    if (i >= 0 && i < nvirt(k)) {
      if (name) *name = virt(k, i);
      if (dir) *dir = true;
      if (size) *size = 0;
      return true;
    }
    i -= nvirt(k);
  }
  int li = br[k].li;
  if (li < 0) return false;
  Listing& L = cache[li];
  if (i < 0 || i >= L.n) return false;
  const Item& it = L.items[L.order[i]];
  if (name) *name = it.name;
  if (dir) *dir = it.dir;
  if (size) *size = it.size;
  return true;
}
CORE_COLD bool item_path(int k, int i, char* out, int cap) {
  const char* n;
  if (!item(k, i, &n, nullptr, nullptr)) return false;
  if (!br[k].cwd[0] && i < nvirt(k)) return sniprintf(out, cap, "%s", n) < cap;   // a virtual folder
  if (in_virt(k)) return sniprintf(out, cap, "%s/%s", br[k].cwd, n) < cap;
  return sniprintf(out, cap, "%s/%s", br[k].cwd, n) < cap;
}
CORE_COLD bool special(int k, const char* path, int* type, int* id) {
  if (!path || path[0] != '@') return false;
  const char* sl = strchr(path, '/');
  if (!sl) return false;
  int t = (k == kGr && !strncmp(path, kVInputs, sl - path) && (int)strlen(kVInputs) == sl - path) ? pools::kInput
        : ((k == kGr || k == kWt) && !strncmp(path, kVBuiltin, sl - path) && (int)strlen(kVBuiltin) == sl - path)
            ? pools::kBuiltin : 0;
  if (!t) return false;
  int i = t == pools::kInput ? pools::input_of(sl + 1) : pools::builtin_of(k, sl + 1);
  if (i < 0) return false;
  if (type) *type = t;
  if (id) *id = i;
  return true;
}
CORE_COLD uint32_t view_gen(int k) {
  if (!did_begin) return 0;
  int li = br[k].li;
  return br[k].gen * 2654435761u ^ (uint32_t)(li >= 0 ? cache[li].n : 0) * 40503u ^ (uint32_t)(li >= 0 && cache[li].done) ^
         (uint32_t)(li + 1) << 28;
}

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (!strncmp(p, "lib=changed:", 12)) {   // (the older name: web.cpp, bundles)
    ensure();
    changed(p + 12);
    sniprintf(out, cap, "browse: %s listed again", p + 12);
    return true;
  }
  if (strncmp(p, "browse", 6) != 0 || (p[6] && p[6] != '=')) return false;
  ensure();
  const char* a = p[6] ? p + 7 : "";
  if (!*a) {
    sniprintf(out, cap, "browse: gr '%s' wt '%s' ms '%s'; %lu folders read, %lu entries, %lu steps, %lu listings dropped",
              br[0].cwd, br[1].cwd, br[2].cwd, (unsigned long)st.dirs_read, (unsigned long)st.entries_read,
              (unsigned long)st.steps, (unsigned long)st.evicted);
    return true;
  }
  if (!strncmp(a, "changed:", 8)) {
    changed(a + 8);
    sniprintf(out, cap, "browse: %s listed again", a + 8);
    return true;
  }
  const char* r;
  int k = kind_of(a, &r);
  if (k < 0) {
    sniprintf(out, cap, "browse: browse=gr|wt|ms:cd:PATH|up|root|open:I");
    return true;
  }
  if (!strncmp(r, "cd:", 3)) {
    bool ok = cd(k, r + 3);
    sniprintf(out, cap, ok ? "browse: %s browses '%s'" : "browse: %s: no folder '%s'", kDirs[k], r + 3);
    return true;
  }
  if (!strcmp(r, "up") || !strcmp(r, "root")) {
    if (r[0] == 'u') up(k);
    else cd(k, "");
    sniprintf(out, cap, "browse: %s browses '%s'", kDirs[k], br[k].cwd);
    return true;
  }
  if (!strncmp(r, "open:", 5)) {   // a folder item: into it (a file: nothing; the pools take files)
    int i = 0;
    for (const char* q = r + 5; *q >= '0' && *q <= '9'; ++q) i = i * 10 + (*q - '0');
    bool dir = false;
    char path[kPath];
    if (!item(k, i, nullptr, &dir, nullptr) || !dir || !item_path(k, i, path, sizeof path) || !cd(k, path))
      sniprintf(out, cap, "browse: %s: item %d is not a folder", kDirs[k], i);
    else sniprintf(out, cap, "browse: %s browses '%s'", kDirs[k], br[k].cwd);
    return true;
  }
  sniprintf(out, cap, "browse: browse=%s:cd:PATH|up|root|open:I", k == kGr ? "gr" : k == kWt ? "wt" : "ms");
  return true;
}

const Stats& stats() { return st; }
CORE_COLD void reset_for_tests() {
  ensure();
  for (int i = 0; i < kCache; ++i) cache[i].used = false, cache[i].n = 0;
  for (int k = 0; k <= kFill; ++k) br[k].cwd[0] = 0, br[k].li = -1, ++br[k].gen;
}

}  // namespace srclib
}  // namespace core
#endif
