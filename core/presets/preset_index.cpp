// core/presets/preset_index.cpp - see preset_index.h.
#include "preset_index.h"

#include <string.h>
#include <strings.h>

#include "../msamp/cardq.h"
#include "../pool/pool.h"
#include "../voices.h"

namespace core {
namespace presets {
namespace {

enum Type : uint8_t { kHidden, kDir, kBuiltin, kFile, kPatch, kFactory, kRom, kVoice };
struct Item {
  uint32_t text;   // its name (a file: until it is read, its file name)
  uint32_t data;   // kDir: its folder; kPatch: its 18 bytes; kFactory, kRom: the built-in's number; kVoice: 0..31
  uint8_t type;
};
struct Folder {
  uint32_t text, path, md5;   // md5: a DX7 bank's
  uint16_t first, n, offset;  // offset: of a DX7 bank's voices in its file
  int16_t up;
  uint8_t kind, depth, filled;
};
Item g_it[kMaxItems];
Folder g_f[kMaxFolders];
char g_text[kTextBytes];
int g_ni = 0, g_nf = 0;
uint32_t g_nt = 0;
int g_k = 0;                 // the kind being walked: what the shares below are charged to
int g_ni_k[2], g_nf_k[2];
uint32_t g_nt_k[2];
bool g_full[2];

volatile bool g_ready = false;
bool g_dirty = false, g_settle = false, g_walking = false;
uint32_t g_dirty_ms = 0, g_gen = 1;
constexpr uint32_t kSettleMs = 2000;   // an upload of many files: walked once, after the last
constexpr int kPiece = 8;              // a piece of the walk: so many folder entries, or one file

struct Walk {
  int f, phase, at;
  bool open, done;
  File dir;
} w;
cardq::Req g_wrq, g_arq;

uint32_t put(const void* s, uint32_t n) {   // 0: no room (offset 0 is never handed out)
  if (g_nt_k[g_k] + n > kTextOf[g_k]) {
    g_full[g_k] = true;
    return 0;
  }
  uint32_t at = g_nt;
  memcpy(g_text + at, s, n);
  g_nt += n, g_nt_k[g_k] += n;
  return at;
}
uint32_t put_s(const char* s) { return put(s, (uint32_t)strlen(s) + 1); }
int new_folder(const char* name, const char* path, int up, int kind) {
  if (g_nf_k[kind] >= kFoldersOf[kind]) {
    g_full[kind] = true;
    return -1;
  }
  uint32_t t = put_s(name), p = put_s(path);
  if (!t || !p) return -1;
  Folder& f = g_f[g_nf];
  memset(&f, 0, sizeof f);
  f.text = t, f.path = p, f.up = (int16_t)up, f.kind = (uint8_t)kind, f.depth = up < 0 ? 0 : g_f[up].depth + 1;
  ++g_nf_k[kind];
  return g_nf++;
}
Item* new_item(Type type, uint32_t text, uint32_t data) {
  if (g_ni_k[g_k] >= kItemsOf[g_k] || !text) {
    g_full[g_k] = true;
    return nullptr;
  }
  ++g_ni_k[g_k];
  Item& i = g_it[g_ni++];
  i.type = type, i.text = text, i.data = data;
  return &i;
}
bool before(const Item& a, const Item& b) {   // folders first, then by name
  bool da = a.type == kDir, db = b.type == kDir;
  if (da != db) return da;
  return strcasecmp(g_text + a.text, g_text + b.text) < 0;
}
void sort_items(Item* a, int n) {   // shell sort: a folder may hold thousands, no recursion, no scratch
  for (int gap = n / 2; gap > 0; gap /= 2)
    for (int i = gap; i < n; ++i) {
      Item t = a[i];
      int j = i;
      for (; j >= gap && before(t, a[j - gap]); j -= gap) a[j] = a[j - gap];
      a[j] = t;
    }
}
void strip_syx(char* s) {
  int l = (int)strlen(s);
  if (l > 4 && !strcasecmp(s + l - 4, ".syx")) s[l - 4] = 0;
}
bool is_syx(const char* s) {
  int l = (int)strlen(s);
  return l > 4 && !strcasecmp(s + l - 4, ".syx");
}
// The file behind item it of folder f: a JUNO patch, or a bank as a folder of its patches
void juno_file(Item& it, File& fl, int f, const char* path) {
  static uint8_t all[jlib::kMaxFiles][18], codes[jlib::kMaxFiles];
  it.type = kHidden;
  int n = jlib::Lib::scan(fl, -1, nullptr, codes, all);
  if (n > jlib::kMaxFiles) n = jlib::kMaxFiles;
  char nm[128];
  sniprintf(nm, sizeof nm, "%s", g_text + it.text);
  strip_syx(nm);
  if (n == 1) {
    char s[24];
    sniprintf(s, sizeof s, "%.23s", nm);
    uint32_t t = put_s(s), d = put(all[0], 18);
    if (t && d) it.type = kPatch, it.text = t, it.data = d;
  } else if (n > 1) {
    int k = new_folder(nm, path, f, kIxJuno);
    if (k < 0) return;
    g_f[k].filled = 1, g_f[k].first = (uint16_t)g_ni;
    for (int i = 0; i < n; ++i) {
      char code[8], s[24];
      uint8_t c = codes[i];
      if (c < 128) sniprintf(code, sizeof code, "%c%d%d", c < 64 ? 'A' : 'B', (c & 63) / 8 + 1, c % 8 + 1);
      else sniprintf(code, sizeof code, "#%d", i + 1);
      sniprintf(s, sizeof s, "%s %s", code, nm);
      uint32_t t = put_s(s), d = put(all[i], 18);
      if (!t || !d || !new_item(kPatch, t, d)) break;
      ++g_f[k].n;
    }
    it.type = kDir, it.data = (uint32_t)k, it.text = g_f[k].text;
  }
}
void dx7_file(Item& it, File& fl, int f, const char* path) {
  static dx7::Rec r;
  it.type = kHidden;
  if (!dx7::Lib::bank(fl, &r)) return;
  char nm[128];
  sniprintf(nm, sizeof nm, "%s", g_text + it.text);
  strip_syx(nm);
  int k = new_folder(nm, path, f, kIxFm);
  if (k < 0) return;
  uint32_t m = put(r.md5, 16);
  if (!m) return;
  g_f[k].filled = 1, g_f[k].first = (uint16_t)g_ni, g_f[k].md5 = m, g_f[k].offset = r.offset;
  for (int v = 0; v < 32; ++v) {
    char s[11];
    dx7::voice_name((const uint8_t*)r.names[v], s);
    if (!new_item(kVoice, put_s(s), (uint32_t)v)) break;
    ++g_f[k].n;
  }
  it.type = kDir, it.data = (uint32_t)k, it.text = g_f[k].text;
}
void built_in(Item& it, int f) {   // FACTORY, ROM: a folder with no file behind it
  int kind = g_f[f].kind, n = kind == kIxJuno ? 128 : dx7::kRomVoices;
  int k = new_folder(g_text + it.text, "", f, kind);
  it.type = kHidden;
  if (k < 0) return;
  g_f[k].filled = 1, g_f[k].first = (uint16_t)g_ni;
  for (int e = 0; e < n; ++e) {
    char s[11];
    if (kind == kIxFm) dx7::voice_name(dx7::rom_bank(e / 32) + 128 * (e % 32) + 118, s);
    if (!new_item(kind == kIxFm ? kRom : kFactory, put_s(kind == kIxFm ? s : juno::kPatches[e].name), (uint32_t)e)) break;
    ++g_f[k].n;
  }
  it.type = kDir, it.data = (uint32_t)k, it.text = g_f[k].text;
}
void piece(cardq::Req*) {
  int budget = kPiece;
  while (budget > 0 && !w.done) {
    if (w.f >= g_nf) {
      w.done = true;
      return;
    }
    Folder& F = g_f[w.f];
    g_k = F.kind;
    if (F.filled) {
      ++w.f;
      continue;
    }
    if (w.phase == 0) {
      F.first = (uint16_t)g_ni;
      if (w.f <= kIxFm) {   // the built-ins first in a root
        if (new_item(kBuiltin, put_s(w.f == kIxJuno ? "FACTORY" : "ROM"), 0)) ++F.n;
      }
      w.open = w.dir.open(g_text + F.path, kRead) && w.dir.isDir();
      if (!w.open) w.dir.close();
      w.phase = 1;
    }
    if (w.phase == 1) {
      File e;
      if (!w.open || !e.openNext(&w.dir, kRead)) {
        if (w.open) w.dir.close();
        w.open = false;
        w.phase = 2, w.at = F.first;
        continue;
      }
      char nm[128];
      e.getName(nm, sizeof nm);
      if (nm[0] != '.') {
        if (e.isDir() && F.depth < 8) {
          if (new_item(kDir, put_s(nm), 0xffffffffu)) ++F.n;
        } else if (!e.isDir() && is_syx(nm)) {
          if (new_item(kFile, put_s(nm), 0)) ++F.n;
        }
      }
      e.close();
      --budget;
      continue;
    }
    if (w.at >= F.first + F.n) {   // the folder's done: what was not a preset leaves its rows
      int n = 0;
      for (int i = 0; i < F.n; ++i)
        if (g_it[F.first + i].type != kHidden) g_it[F.first + n++] = g_it[F.first + i];
      F.n = (uint16_t)n;
      int skip = w.f <= kIxFm && n ? 1 : 0;   // (a bank is a folder only now; the built-ins stay on top)
      sort_items(g_it + F.first + skip, n - skip);
      ++w.f, w.phase = 0;
      continue;
    }
    Item& it = g_it[w.at++];
    char path[160];
    sniprintf(path, sizeof path, "%s/%s", g_text + F.path, g_text + it.text);
    if (it.type == kBuiltin) {
      built_in(it, w.f);
    } else if (it.type == kDir) {
      int k = new_folder(g_text + it.text, path, w.f, F.kind);
      if (k < 0) it.type = kHidden;
      else it.data = (uint32_t)k;
    } else {
      File fl;
      if (!fl.open(path, kRead)) it.type = kHidden;
      else if (F.kind == kIxJuno) juno_file(it, fl, w.f, path);
      else dx7_file(it, fl, w.f, path);
      fl.close();
      budget = 0;
    }
  }
}
void start() {
  g_ready = false;
  g_dirty = false;
  g_ni = g_nf = 0;
  g_nt = 1;   // (0: no room)
  for (int k = 0; k < 2; ++k) g_ni_k[k] = g_nf_k[k] = 0, g_nt_k[k] = 0, g_full[k] = false;
  g_k = kIxJuno;
  new_folder("JUNO", "/JUNO", -1, kIxJuno);
  g_k = kIxFm;
  new_folder("DX7", "/DX7", -1, kIxFm);
  w.f = 0, w.phase = 0, w.at = 0, w.open = false, w.done = false;
  g_walking = true;
}
// add: a voice on its way
struct Pending {
  bool on, ok;
  int folder, row, at;
  uint8_t data[128];
} g_p;
void read_voice(cardq::Req*) {
  const Folder& F = g_f[g_p.folder];
  File f;
  g_p.ok = f.open(g_text + F.path, kRead) && f.seekSet(F.offset + 128u * g_it[F.first + g_p.row].data) && f.read(g_p.data, 128) == 128;
  f.close();
}
int used_juno() {
  int n = 0;
  for (int s = 0; s < junolib.listed(); ++s) n += junolib.slot(s).used;
  return n;
}
int done_fm(int s, bool was, bool* there) {
  if (s < 0) return kAddFull;
  if (there) *there = was;
  send_fm_names();
  fm_relist();
  return s;
}
int done_juno(int s, bool was, bool* there) {
  if (s < 0) return kAddFull;
  if (there) *there = was;
  send_juno_names();
  juno_relist();
  return s;
}
int g_poll_slot = 0;
bool g_poll_there = false, g_poll_on = false;
Result g_res = {0, 0, 0, false, false};
void record(int kind, int slot, bool there, bool forced) {
  g_res.kind = kind, g_res.slot = slot, g_res.there = there, g_res.forced = forced;
  __atomic_store_n(&g_res.seq, g_res.seq + 1, __ATOMIC_RELEASE);
}
bool g_p_forced = false;
}  // namespace

uint32_t ram_bytes() { return sizeof g_it + sizeof g_f + sizeof g_text; }
Stats stats(int k) { return Stats{g_nf_k[k], g_ni_k[k], g_nt_k[k]}; }
bool full(int k) { return g_full[k]; }
void begin() {
  g_dirty = true, g_settle = false;
}
void changed(const char* path) {
  if (strncasecmp(path, "/JUNO", 5) && strncasecmp(path, "/DX7", 4) && path[0] && strcmp(path, "/")) return;
  g_dirty = true, g_settle = true, g_dirty_ms = now_ms();
}
void step() {
  if (g_p.on && !g_walking) {
    g_arq.fn = read_voice;
    if (cardq::async(&g_arq, cardq::kFnReq, nullptr, nullptr, 0, nullptr, 0)) {
      g_p.on = false;
      g_poll_on = true, g_poll_slot = kAddNo, g_poll_there = false;
      if (lists_busy()) g_poll_slot = kAddBusy;
      else if (g_p.ok) {
        const Folder& F = g_f[g_p.folder];
        int n0 = dxlib.listed();
        bool was = false;
        for (int s = 0; s < n0; ++s)
          was = was || (dxlib.slot(s).used && dxlib.slot(s).voice == g_it[F.first + g_p.row].data && !memcmp(dxlib.slot(s).md5, g_text + F.md5, 16));
        g_poll_slot = done_fm(dxlib.put_voice((const uint8_t*)g_text + F.md5, g_it[F.first + g_p.row].data, g_text + g_it[F.first + g_p.row].text, g_p.data, g_p.at),
                              was, &g_poll_there);
      }
      record(kIxFm, g_poll_slot, g_poll_there, g_p_forced);
    }
    return;
  }
  if (g_walking) {
    g_wrq.fn = piece;
    if (!cardq::async(&g_wrq, cardq::kFnReq, nullptr, nullptr, 0, nullptr, 0)) return;
    if (w.done) {
      g_walking = false;
      g_ready = true;
      ++g_gen;
      log(CORE_STR("presets: %d folders, %d rows, %lu bytes of text%s%s"), g_nf, g_ni, (unsigned long)g_nt, g_full[0] ? ", JUNO FULL" : "", g_full[1] ? ", FM FULL" : "");
    }
    return;
  }
  if (g_dirty && !g_p.on && !lists_busy() && (!g_settle || now_ms() - g_dirty_ms >= kSettleMs)) start();
}
bool ready() { return g_ready; }
uint32_t gen() { return g_gen; }
int count(int f) { return f < 0 || f >= g_nf ? 0 : g_f[f].n; }
const char* name(int f, int r) { return r < 0 || r >= count(f) ? "" : g_text + g_it[g_f[f].first + r].text; }
bool is_dir(int f, int r) { return r >= 0 && r < count(f) && g_it[g_f[f].first + r].type == kDir; }
int child(int f, int r) { return is_dir(f, r) ? (int)g_it[g_f[f].first + r].data : -1; }
int parent(int f) { return f < 0 || f >= g_nf ? -1 : g_f[f].up; }
int row_in_parent(int f) {
  int up = parent(f);
  for (int r = 0; r < count(up); ++r)
    if (child(up, r) == f) return r;
  return 0;
}
const char* folder_name(int f) { return f < 0 || f >= g_nf ? "" : g_text + g_f[f].text; }

int rows(int kind) {
  int n = kind == kIxJuno ? junolib.listed() : dxlib.listed();
  return n ? n : kind == kIxJuno ? 128 : dx7::kRomVoices;
}
bool built_ins(int kind) { return !(kind == kIxJuno ? junolib.listed() : dxlib.listed()); }
bool row_used(int kind, int r) {
  if (r < 0 || r >= rows(kind)) return false;
  if (built_ins(kind)) return true;
  return kind == kIxJuno ? junolib.slot(r).used : dxlib.slot(r).used;
}
const char* row_name(int kind, int r, char* b, int cap) {
  b[0] = 0;
  if (kind == kIxJuno) {
    if (junolib.listed()) sniprintf(b, cap, "%s", junolib.slot(r).used ? junolib.slot(r).name : "(empty)");
    else sniprintf(b, cap, "%s", juno::kPatches[r & 127].name);
  } else if (dxlib.listed()) {
    sniprintf(b, cap, "%s", dxlib.slot(r).used ? dxlib.slot(r).name : "(empty)");
  } else {
    char s[11];
    dx7::voice_name(dx7::rom_bank(r / 32) + 128 * (r % 32) + 118, s);
    sniprintf(b, cap, "%s", s);
  }
  return b;
}
void seed(int kind) {
  if (kind == kIxFm) {
    if (dxlib.listed()) return;
    dxlib.seed_rom();
    send_fm_names();
    fm_relist();
    return;
  }
  if (junolib.listed()) return;
  for (int e = 0; e < jlib::kMaxList; ++e) junolib.set_row(e, juno::kPatches[e].b, juno::kPatches[e].name, false);
  junolib.resave();
  send_juno_names();
  juno_relist();
}
int spare_juno() {
  for (int s = junolib.listed() - 1; s >= 0; --s)
    if (s < 128 && !memcmp(junolib.slot(s).b, juno::kPatches[s].b, 18)) return s;
  return -1;
}
namespace {
int add_(int kind, int f, int r, bool* there, int at, bool force) {
  if (!g_ready || g_p.on || lists_busy()) return kAddBusy;
  if (r < 0 || r >= count(f) || g_f[f].kind != kind) return kAddNo;
  if (at >= (kind == kIxJuno ? jlib::kMaxList : dx7::kMaxList)) return kAddNo;   // (a slot out of the list's range)
  const Item& it = g_it[g_f[f].first + r];
  seed(kind);
  int uj = used_juno();
  auto juno_in = [&](const uint8_t* b, const char* nm) -> int {
    if (force && at >= 0 && at < jlib::kMaxList) {
      junolib.set_row(at, b, nm);
      return done_juno(at, false, there);
    }
    int s = junolib.put(b, nm);
    if (s >= 0) return done_juno(s, used_juno() == uj, there);
    if (at < 0) return kAddFull;
    junolib.set_row(at, b, nm);
    return done_juno(at, false, there);
  };
  switch (it.type) {
    case kPatch: return juno_in((const uint8_t*)g_text + it.data, g_text + it.text);
    case kFactory: return juno_in(juno::kPatches[it.data].b, juno::kPatches[it.data].name);
    case kRom: {
      bool was = false;
      for (int s = 0; s < dxlib.listed(); ++s) was = was || (dxlib.slot(s).used && dxlib.slot(s).bank == -2 - (int)it.data / 32 && dxlib.slot(s).voice == it.data % 32);
      return done_fm(dxlib.add_rom((int)it.data, force || dxlib.listed() >= dx7::kMaxList ? at : -1), was, there);
    }
    case kVoice:
      if (!force && at < 0 && dxlib.listed() >= dx7::kMaxList) {
        bool room = false;
        for (int s = 0; s < dxlib.listed(); ++s) room = room || !dxlib.slot(s).used;
        if (!room) return kAddFull;   // (a voice in the list already: at the YES that replaces, its row)
      }
      g_p.on = true, g_p.folder = f, g_p.row = r, g_p.ok = false;
      g_p.at = force || dxlib.listed() >= dx7::kMaxList ? at : -1;
      g_p_forced = force;
      return kAddRead;
    default: return kAddNo;
  }
}
}  // namespace

int add(int kind, int f, int r, bool* there, int at, bool force) {
  bool was = false;
  int s = add_(kind, f, r, &was, at, force);
  if (there) *there = was;
  if (s != kAddRead) record(kind, s, was, force);
  return s;
}
bool add_poll(int* slot, bool* there) {
  if (!g_poll_on) return false;
  g_poll_on = false;
  *slot = g_poll_slot, *there = g_poll_there;
  return true;
}

Result result() {
  Result r = g_res;
  r.seq = __atomic_load_n(&g_res.seq, __ATOMIC_ACQUIRE);
  return r;
}
const char* result_text(char* b, int cap) {
  Result r = result();
  const char* who = r.kind == kIxJuno ? "JUNO" : "FM";
  if (!r.seq) b[0] = 0;
  else if (r.slot >= 0) sniprintf(b, cap, "%s %03d %s", who, r.slot, r.there ? "IN THE LIST ALREADY" : r.forced ? "SET" : "ADDED");
  else if (r.slot == kAddFull) sniprintf(b, cap, "LIST FULL");
  else if (r.slot == kAddBusy) sniprintf(b, cap, "PROJECT LOADING");
  else sniprintf(b, cap, "NOT ADDED");
  return b;
}

int view(int f, int kind, int first, int max, char (*names)[kViewName], uint8_t* flags, int16_t* kids, View* v) {
  memset(v, 0, sizeof *v);
  v->up = -1, v->kind = kind;
  if (kind < 0 || kind > 1 || first < 0 || max < 0) return -1;
  uint32_t g0 = __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE);
  if (f >= 0 && !g_ready) return -1;
  int n = 0;
  if (f < 0) {   // the project's list
    bool juno = kind == kIxJuno;
    int total = rows(kind);
    v->built_in = built_ins(kind), v->total = total, v->full = g_full[kind];
    sniprintf(v->title, sizeof v->title, "%s", v->built_in ? (juno ? "JUNO BUILT-IN" : "FM BUILT-IN") : (juno ? "JUNO LIST" : "FM LIST"));
    for (int r = first; r < total && n < max; ++r, ++n) {
      row_name(kind, r, names[n], kViewName);
      bool used = v->built_in || (juno ? junolib.slot(r).used : dxlib.slot(r).used);
      flags[n] = used ? kViewUsed : 0, kids[n] = -1;
    }
  } else {
    if (f >= g_nf) return -1;
    const Folder F = g_f[f];
    v->gen = g0, v->total = F.n, v->up = F.up, v->kind = F.kind, v->full = g_full[F.kind];
    if ((int)F.first + F.n > kMaxItems) return -1;
    char t[48] = "";
    int ids[10], d = 0;
    for (int k = f; k >= 0 && d < 10; k = g_f[k].up) ids[d++] = k;
    for (int i = d - 1; i >= 0; --i) {
      int l = (int)strlen(t);
      if (g_f[ids[i]].text >= kTextBytes) return -1;
      sniprintf(t + l, sizeof t - l, "%s%.20s", l ? "/" : "", g_text + g_f[ids[i]].text);
    }
    memcpy(v->title, t, sizeof v->title);
    for (int r = first; r < F.n && n < max; ++r, ++n) {
      const Item& it = g_it[F.first + r];
      if (it.text >= kTextBytes) return -1;
      sniprintf(names[n], kViewName, "%.*s", kViewName - 1, g_text + it.text);
      flags[n] = it.type == kDir ? kViewDir : 0;
      kids[n] = it.type == kDir && it.data < (uint32_t)kMaxFolders ? (int16_t)it.data : (int16_t)-1;
    }
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if ((f >= 0 && !g_ready) || __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE) != g0) return -1;
  v->gen = g0;
  return n;
}

}  // namespace presets
}  // namespace core
