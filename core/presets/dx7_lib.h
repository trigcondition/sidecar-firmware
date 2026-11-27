// DX7 voices on the SD card, for the 6-OP FM machine (docs/PLAN.md 3b;
// sim/protocol.py, 17/18).
//
// /DX7 and its subfolders hold packed 32-voice DX7 banks: a 4104-byte .syx
// (F0 43 0n 09 20 00, 4096 bytes, checksum, F7) or a bare 4096-byte file. At
// boot a walk of /DX7 gives a hash of its listing (names, sizes, times); if it
// matches /.sidecar/dx7.bin's, that index is used, else it is rebuilt (each bank
// read once: an MD5 of its 4096 voice bytes and its 32 names). The index:
//   /.sidecar/dx7.bin   a Header, then a Rec per node: folders and banks, breadth
//                    first in the card's directory order (node 0 = /DX7); the
//                    header is written last, so a half-written index has none
// The nodes are TEENSY folders (core/pool/pool.cpp numbers them after /JUNO's): a
// folder's entries are its subfolders and banks by name, a bank's its 32
// voices (named by their 10-character names).
//
// The project's FM list - what LOAD TO PROJ on a voice adds to, and what the
// machine's PAT steps through - names each voice by its bank's MD5 and its
// position, so a moved or renamed bank is found again:
//   /.rytm/fm/<PROJECT>.txt   a line per slot: 32 hex digits, a tab, the voice
//                             0..31, a tab, its name, and since 0.4.0 a tab and the
//                             voice's own 128 bytes in hex (so a project's zip plays
//                             on a card without its banks); an empty line an empty slot
// Plaits' three ROM banks count as banks too (their MD5s), so a list can name
// their voices without the files on the card. When the list is empty the
// machine plays Plaits' 96 ROM voices instead (core/voices.cpp).
#pragma once
#include "../platform.h"
#include "../project_key.h"
#include "../machines.h"
#if CORE_MSAMP
#include "../msamp/cardq.h"
namespace core {
bool list_post(const char* path, const char* text, uint32_t n);   // fmt::post_queue (presets/libs.cpp)
bool list_replace(const char* path, const char* text, uint32_t n);   // fmt::write_atomic: the card core's, now
}
#endif

#include "md5.h"
#include "plaits/resources.h"

#ifdef FLASHMEM   // the Teensy's: run from flash, a section per function (inline
                  // functions in one shared section would conflict)
#define DX7_FLASH(f) __attribute__((section(".flashmem.dx7." #f)))
#else
#define DX7_FLASH(f)
#endif

namespace dx7 {

const int kMaxNodes = 128;   // folders and banks under /DX7
const int kMaxList = 127;    // the project's FM list (17's total is 7 bits)
const int kListSlots = core::kVoiceSlots;   // a project list per voice slot (core/pool/pool.h)
const int kRomVoices = 96;

struct Node {                // in RAM, 28 bytes
  char name[24];             // the folder's name, or the bank's file name without .syx
  int16_t parent;            // -1: /DX7 itself
  uint8_t bank;              // 1: a bank (32 voices), 0: a folder
  uint8_t pad;
};

struct Rec {                 // in /.sidecar/dx7.bin, 480 bytes
  Node node;
  uint16_t offset;           // of the voice bytes in the file (6: .syx, 0: bare)
  uint16_t pad;
  char path[112];
  uint8_t md5[16];           // of the 4096 voice bytes
  char names[32][10];
};

struct Header {
  char magic[8];             // "DX7IDX1"
  uint32_t hash_hi, hash_lo; // the listing's
  uint32_t count;
  uint32_t pad;
};

struct Slot {                // the project's FM list, in RAM, 32 bytes
  uint8_t md5[16];
  int16_t bank;              // node; -2 - k Plaits' ROM bank k; -1 not found
  uint8_t voice;             // 0..31
  bool used;
  char name[12];
};

typedef void (*log_fn)(const char* fmt, ...);

inline const uint8_t* rom_bank(int k) {
  return k == 0 ? plaits::syx_bank_0 : k == 1 ? plaits::syx_bank_1 : plaits::syx_bank_2;
}

// A packed voice's name (bytes 118..127): printable ASCII (else '_'), trailing
// spaces cut.
inline void voice_name(const uint8_t* v10, char* out) {
  int n = 0;
  for (int i = 0; i < 10; ++i) {
    char c = v10[i] & 0x7f;
    out[n++] = (c >= 0x20 && c <= 0x7e) ? c : '_';
  }
  while (n && out[n - 1] == ' ') --n;
  out[n] = 0;
}

class Lib {
 public:
  DX7_FLASH(begin) void begin(core::Fs* sd, log_fn log) {
    sd_ = sd;
    log_ = log;
    nnodes_ = 0;
    named_ = -1;
    for (int k = 0; k < 3; ++k) {
      md5::Md5 m;
      m.update(rom_bank(k), 4096);
      m.final(rom_md5_[k]);
    }
    core::File root;
    if (!root.open("/DX7", core::kRead) || !root.isDir()) {
      log_("dx7: no /DX7 on the card");
      return;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    walk(root, 0, &h);
    root.close();
    uint32_t t0 = core::now_ms();
    if (load(h)) {
      log_("dx7: index unchanged, loaded (%d folders and banks)", nnodes_);
    } else if (build(h)) {
      log_("dx7: index built, %d folders and banks, %lu ms", nnodes_, (unsigned long)(core::now_ms() - t0));
    } else {
      log_("dx7: cannot write the index");
      nnodes_ = 0;
    }
  }

  int nodes() const { return nnodes_; }
  const Node& node(int k) const { return nodes_[k]; }

  // Node k's children (folders and banks) by name; how many (up to max).
  DX7_FLASH(children) int children(int k, uint16_t* out, int max) const {
    int n = 0;
    for (int i = 0; i < nnodes_ && n < max; ++i)
      if (nodes_[i].parent == k) out[n++] = (uint16_t)i;
    for (int a = 1; a < n; ++a)
      for (int b = a; b > 0 && strcasecmp(nodes_[out[b - 1]].name, nodes_[out[b]].name) > 0; --b) {
        uint16_t t = out[b];
        out[b] = out[b - 1];
        out[b - 1] = t;
      }
    return n;
  }

  // Voice i of bank node k: its name ("" if none).
  DX7_FLASH(name) bool name(int k, int i, char* out) {
    out[0] = 0;
    if (k < 0 || k >= nnodes_ || !nodes_[k].bank || i < 0 || i >= 32) return false;
    if (named_ != k) {
      Rec r;
      if (!rec(k, &r)) return false;
      memcpy(names_, r.names, sizeof names_);
      named_ = k;
    }
    memcpy(out, names_[i], 10);
    out[10] = 0;
    return true;
  }

  // --- the project's FM list ---
  int listed() const { return L().n; }
  const Slot& slot(int i) const { return L().list[i]; }
  // voice slot s's list (a voice's own box's: slot_of_voice), whatever slot is in hand; the slot in hand's: sel
  int listed(int s) const { return lists_[pick(s)].n; }
  const Slot& slot(int s, int i) const { return lists_[pick(s)].list[i]; }
  bool voice(int s, int e, uint8_t* out) const {
    const List& x = lists_[pick(s)];
    if (e < 0 || e >= x.n || !x.cached[e]) return false;
    memcpy(out, x.vdata[e], 128);
    return true;
  }
  void selector(int (*sel)()) { sel_ = sel; }

  // 2026-10-27 (core/bundle/prjslot.h): the list's next writes under another project key (a SAVE AS copy placed)
  void rekey(const char* project) { project_file(project); }
  DX7_FLASH(load_list) void load_list(const char* project) {
    project_file(project);
    bool whole = read_list(L().file, L().list, &L().n, L().vdata, L().cached);
    resolve(L().list, L().n, 0, nnodes_);
    bool old = false;
    for (int s = 0; s < L().n; ++s)
      if (!L().cached[s] && cache(L().list, L().n, s, L().vdata[s], &L().cached[s])) old = true;
    if (old && whole) save();   // a list from before 0.4.0: its voices into the file
    log_list();
  }
  // load_list in parts (a card core, core/pool/pool.cpp): the file, the banks and the voices read into a copy on that core, a
  // bounded piece a request; the copy taken in here
  static void list_path(const char* project, char* out, int cap) {
    char n[core::kKeyMax];
    core::project_key(project, n, sizeof n);
    sniprintf(out, cap, "/.rytm/fm/%s.txt", n);
  }
  // false: a read error (the list as far as it came): such a list is never written back
  DX7_FLASH(read_list) static bool read_list(const char* file, Slot* list, int* count, uint8_t (*vd)[128], bool* have) {
    bool ok = true;
    *count = 0;
    memset(have, 0, sizeof(bool) * kMaxList);
    core::File f;
    if (f.open(file, core::kRead)) {
      char line[344];
      int n = 0;
      for (;;) {
        int c = f.read();
        if (c < 0 && f.curPosition() < f.fileSize()) ok = false;
        if (c < 0 && n == 0) break;
        if (c < 0 || c == '\n') {
          if (*count >= kMaxList) break;
          line[n] = 0;
          int at = (*count)++;
          Slot& s = list[at];
          s.used = parse(line, &s, vd[at], &have[at]);
          n = 0;
          if (c < 0) break;
        } else if (c != '\r' && n < (int)sizeof line - 1) {
          line[n++] = c;
        }
      }
      f.close();
      while (*count && !list[*count - 1].used) --*count;
    }
    return ok;
  }
  DX7_FLASH(take_list) void take_list(const char* file, const Slot* list, int count, const uint8_t (*v)[128], const bool* c) {
    sniprintf(L().file, sizeof L().file, "%s", file);
    memcpy(L().list, list, sizeof(Slot) * (size_t)count);
    memcpy(L().vdata, v, 128 * (size_t)count);
    memcpy(L().cached, c, sizeof(bool) * (size_t)count);
    L().n = count;
    log_list();
  }

  // Slot s's 128 voice bytes into out; false: empty, or its bank not found.
  // From RAM: the list's voices are read when the list is (load_list, add),
  // so a trig that changes the voice (6-OP FM's PAT) never waits for the card
  // (a card voice took 1-2 ms to open and read: eight on one step overran the
  // Overbridge buffer).
  bool has_voice(int s) const { return s >= 0 && s < L().n && L().cached[s]; }   // its 128 bytes in RAM, whatever the card holds
  bool voice(int s, uint8_t* out) const {
    if (s < 0 || s >= L().n || !L().cached[s]) return false;
    memcpy(out, L().vdata[s], 128);
    return true;
  }

 private:
  DX7_FLASH(log_list) void log_list() {
    int missing = 0;
    for (int s = 0; s < L().n; ++s) missing += L().list[s].used && L().list[s].bank == -1 && !L().cached[s];
    log_("fm list %s: %d slots, %d not on the card", L().file, L().n, missing);
  }

 public:
  // Slot s's voice into out (from the card for a card bank); ok: *cached. True when it went to the card.
  DX7_FLASH(cache) bool cache(const Slot* list, int n, int s, uint8_t* out, bool* cached) {
    *cached = false;
    if (s < 0 || s >= n || !list[s].used || list[s].bank == -1) return false;
    const Slot& e = list[s];
    if (e.bank <= -2) {
      memcpy(out, rom_bank(-2 - e.bank) + 128 * e.voice, 128);
      *cached = true;
      return false;
    }
    Rec r;
    core::File f;
    if (!rec(e.bank, &r) || !f.open(r.path, core::kRead)) return true;
    *cached = f.seekSet(r.offset + 128u * e.voice) && f.read(out, 128) == 128;
    f.close();
    return true;
  }

  // LOAD TO PROJ on voice i of bank node k: into the first empty slot (none
  // if it is in the list already). Returns its slot, or -1.
  DX7_FLASH(add) int add(int k, int i) {
    if (k < 0 || k >= nnodes_ || !nodes_[k].bank || i < 0 || i >= 32) return -1;
    Rec r;
    if (!rec(k, &r)) return -1;
    for (int s = 0; s < L().n; ++s)
      if (L().list[s].used && L().list[s].voice == i && memcmp(L().list[s].md5, r.md5, 16) == 0) return s;
    int s = 0;
    while (s < L().n && L().list[s].used) ++s;
    if (s >= kMaxList) return -1;
    Slot& e = L().list[s];
    memcpy(e.md5, r.md5, 16);
    e.voice = (uint8_t)i;
    e.used = true;
    e.bank = (int16_t)k;
    char nm[11];
    memcpy(nm, r.names[i], 10);
    nm[10] = 0;
    voice_name((const uint8_t*)nm, e.name);
    if (s == L().n) ++L().n;
    cache(L().list, L().n, s, L().vdata[s], &L().cached[s]);
    save();
    log_("fm list: slot %d = %s voice %d (%s)", s, r.node.name, i, e.name);
    return s;
  }

  // A voice read on the card core (read_voice) into the first empty slot, no card read here (none if listed): its slot, -1.
  // at >= 0: into that slot whatever it holds.
  DX7_FLASH(put_voice) int put_voice(const uint8_t* md5, int i, const char* name, const uint8_t* data, int at = -1) {
    for (int s = 0; s < L().n; ++s)
      if (L().list[s].used && L().list[s].voice == i && memcmp(L().list[s].md5, md5, 16) == 0) {
        if (!L().cached[s]) {   // listed, its bank was away: it plays from here on
          memcpy(L().vdata[s], data, 128);
          L().cached[s] = true;
          save();
        }
        return s;
      }
    int s = at;
    if (s < 0)
      for (s = 0; s < L().n && L().list[s].used;) ++s;
    if (s >= kMaxList) return -1;
    Slot& e = L().list[s];
    memcpy(e.md5, md5, 16);
    e.voice = (uint8_t)i, e.used = true, e.bank = -1;   // (found by its MD5 at the list's next load)
    sniprintf(e.name, sizeof e.name, "%s", name);
    memcpy(L().vdata[s], data, 128);
    L().cached[s] = true;
    if (s >= L().n) L().n = s + 1;
    save();
    return s;
  }
  // The built-ins into an empty list, row = PAT: what the list's first add does, so that a saved PAT keeps its voice.
  DX7_FLASH(seed_rom) void seed_rom() {
    if (L().n) return;
    for (int e = 0; e < kRomVoices; ++e) {
      Slot& d = L().list[e];
      memcpy(d.md5, rom_md5_[e / 32], 16);
      d.voice = (uint8_t)(e % 32), d.used = true, d.bank = (int16_t)(-2 - e / 32);
      voice_name(rom_bank(e / 32) + 128 * (e % 32) + 118, d.name);
      memcpy(L().vdata[e], rom_bank(e / 32) + 128 * (e % 32), 128);
      L().cached[e] = true;
    }
    L().n = kRomVoices;
    save();
  }
  void resave() { save(); }   // a list read whole at a project's switch from before 0.4.0: its voices into the file

  // The list's edits (the Pi's web page, core/user/lists.cpp): each saves the list. False: no such slot.
  // Plaits' ROM voice e (0..95: its three banks in order) into the first empty slot (none if listed): its slot, -1.
  DX7_FLASH(add_rom) int add_rom(int e, int at = -1) {
    if (e < 0 || e >= kRomVoices) return -1;
    int k = e / 32, i = e % 32;
    for (int s = 0; s < L().n; ++s)
      if (L().list[s].used && L().list[s].voice == i && memcmp(L().list[s].md5, rom_md5_[k], 16) == 0) return s;
    int s = at;
    if (s < 0)
      for (s = 0; s < L().n && L().list[s].used;) ++s;
    if (s >= kMaxList) return -1;
    Slot& d = L().list[s];
    memcpy(d.md5, rom_md5_[k], 16);
    d.voice = (uint8_t)i;
    d.used = true;
    d.bank = (int16_t)(-2 - k);
    voice_name(rom_bank(k) + 128 * i + 118, d.name);
    if (s >= L().n) L().n = s + 1;
    cache(L().list, L().n, s, L().vdata[s], &L().cached[s]);
    save();
    return s;
  }
  DX7_FLASH(clear) bool clear(int s) {
    if (s < 0 || s >= L().n) return false;
    L().list[s].used = false;
    L().cached[s] = false;
    while (L().n && !L().list[L().n - 1].used) --L().n;
    save();
    return true;
  }
  DX7_FLASH(rename) bool rename(int s, const char* name) {
    if (s < 0 || s >= L().n || !L().list[s].used) return false;
    int k = 0;
    for (; name && name[k] && k < (int)sizeof L().list[s].name - 1; ++k)
      L().list[s].name[k] = name[k] >= 0x20 && name[k] <= 0x7e ? name[k] : '_';
    L().list[s].name[k] = 0;
    save();
    return true;
  }
  DX7_FLASH(swap) bool swap(int a, int b) {   // two slots trade places (one may be empty, or past the end)
    if (a < 0 || b < 0 || a >= kMaxList || b >= kMaxList || a == b) return false;
    for (; L().n <= (a > b ? a : b); ++L().n) L().list[L().n].used = false, L().cached[L().n] = false;
    Slot t = L().list[a];
    L().list[a] = L().list[b];
    L().list[b] = t;
    uint8_t v[128];   // their cached voices with them (no card reads)
    memcpy(v, L().vdata[a], 128); memcpy(L().vdata[a], L().vdata[b], 128); memcpy(L().vdata[b], v, 128);
    bool c = L().cached[a]; L().cached[a] = L().cached[b]; L().cached[b] = c;
    while (L().n && !L().list[L().n - 1].used) --L().n;
    save();
    return true;
  }

 private:
  // The listing's fingerprint: every entry's name, size and time below /DX7.
  DX7_FLASH(walk) void walk(core::File& dir, int depth, uint64_t* h) {
    core::File e;
    char nm[128];
    while (e.openNext(&dir, core::kRead)) {
      size_t n = e.getName(nm, sizeof nm);
      if (nm[0] != '.') {
        fnv(h, nm, n);
        uint16_t d = 0, t = 0;
        e.getModifyDateTime(&d, &t);
        uint32_t sz = e.fileSize();
        fnv(h, &d, 2);
        fnv(h, &t, 2);
        fnv(h, &sz, 4);
        uint8_t mark = e.isDir() ? 1 : 0;
        fnv(h, &mark, 1);
        if (e.isDir() && depth < 8) walk(e, depth + 1, h);
        if (e.isDir()) fnv(h, "/", 1);   // where a folder ends
      }
      e.close();
    }
  }

  static void fnv(uint64_t* h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) {
      *h ^= b[i];
      *h *= 0x100000001b3ull;
    }
  }

  DX7_FLASH(rec) bool rec(int k, Rec* r) {
    core::File f;
    if (!f.open("/.sidecar/dx7.bin", core::kRead)) return false;
    bool ok = f.seekSet(sizeof(Header) + (uint32_t)k * sizeof(Rec)) && f.read(r, sizeof *r) == (int)sizeof *r;
    f.close();
    return ok;
  }

  DX7_FLASH(load) bool load(uint64_t h) {
    core::File f;
    Header hd;
    if (!f.open("/.sidecar/dx7.bin", core::kRead)) return false;
    bool ok = f.read(&hd, sizeof hd) == (int)sizeof hd && memcmp(hd.magic, "DX7IDX1", 8) == 0 &&
              hd.hash_hi == (uint32_t)(h >> 32) && hd.hash_lo == (uint32_t)h && hd.count >= 1 &&
              hd.count <= (uint32_t)kMaxNodes && f.fileSize() == sizeof hd + hd.count * sizeof(Rec);
    for (uint32_t k = 0; ok && k < hd.count; ++k) {
      Rec r;
      ok = f.read(&r, sizeof r) == (int)sizeof r;
      nodes_[k] = r.node;
    }
    f.close();
    nnodes_ = ok ? hd.count : 0;
    return ok;
  }

  // Breadth first from /DX7: each folder's entries in the card's order;
  // folders' paths read back from the records written so far.
  DX7_FLASH(build) bool build(uint64_t h) {
    sd_->mkdir("/.sidecar");
    core::File out;
    if (!out.open("/.sidecar/dx7.bin", core::kReadWrite)) return false;
    Header hd;
    memset(&hd, 0, sizeof hd);   // no magic until the index is whole
    out.write(&hd, sizeof hd);
    Rec r;
    memset(&r, 0, sizeof r);
    strcpy(r.node.name, "DX7");
    r.node.parent = -1;
    strcpy(r.path, "/DX7");
    out.write(&r, sizeof r);
    nodes_[0] = r.node;
    nnodes_ = 1;
    int banks = 0, skipped = 0;
    for (int i = 0; i < nnodes_; ++i) {
      if (nodes_[i].bank) continue;
      char path[112];
      if (!out.seekSet(sizeof hd + (uint32_t)i * sizeof(Rec)) || out.read(&r, sizeof r) != (int)sizeof r) break;
      strcpy(path, r.path);
      out.seekSet(sizeof hd + (uint32_t)nnodes_ * sizeof(Rec));
      core::File dir, e;
      if (!dir.open(path, core::kRead)) continue;
      char nm[128];
      while (e.openNext(&dir, core::kRead)) {
        size_t len = e.getName(nm, sizeof nm);
        memset(&r, 0, sizeof r);
        bool is_dir = e.isDir(), take = false;
        if (nm[0] == '.' || nnodes_ >= kMaxNodes || strlen(path) + 1 + len >= sizeof r.path) {
          if (nm[0] != '.') ++skipped;
        } else {
          take = is_dir || bank(e, &r);
        }
        e.close();
        if (!take) continue;
        sniprintf(r.path, sizeof r.path, "%s/%s", path, nm);
        if (!is_dir && len > 4 && strcasecmp(nm + len - 4, ".syx") == 0) nm[len - 4] = 0;
        strncpy(r.node.name, nm, sizeof r.node.name - 1);
        r.node.parent = (int16_t)i;
        r.node.bank = is_dir ? 0 : 1;
        banks += r.node.bank;
        nodes_[nnodes_++] = r.node;
        out.write(&r, sizeof r);
      }
      dir.close();
    }
    memcpy(hd.magic, "DX7IDX1", 8);
    hd.hash_hi = (uint32_t)(h >> 32);
    hd.hash_lo = (uint32_t)h;
    hd.count = nnodes_;
    out.seekSet(0);
    out.write(&hd, sizeof hd);
    out.close();
    if (skipped) log_("dx7: %d entries left out (too many, or the path too long)", skipped);
    log_("dx7: %d banks", banks);
    return true;
  }

  // An open file as a bank: r's offset, md5 and names; false if it is none.
 public:
  DX7_FLASH(bank) static bool bank(core::File& f, Rec* r) {
    uint32_t size = f.fileSize();
    uint8_t b[512];
    if (size == 4104) {
      if (f.read(b, 6) != 6 || b[0] != 0xf0 || b[1] != 0x43 || (b[2] & 0xf0) != 0 || b[3] != 0x09 ||
          b[4] != 0x20 || b[5] != 0x00)
        return false;
      r->offset = 6;
    } else if (size == 4096) {
      r->offset = 0;
    } else {
      return false;
    }
    if (!f.seekSet(r->offset)) return false;
    md5::Md5 m;
    for (int c = 0; c < 8; ++c) {   // 8 x 4 voices
      if (f.read(b, 512) != 512) return false;
      m.update(b, 512);
      for (int v = 0; v < 4; ++v) memcpy(r->names[c * 4 + v], b + v * 128 + 118, 10);
    }
    m.final(r->md5);
    return true;
  }

  // Each used slot's bank: Plaits' ROM banks (k0 0), then the card's by MD5, nodes k0..k1.
 public:
  DX7_FLASH(resolve) void resolve(Slot* list, int n, int k0, int k1) {
    for (int s = 0; k0 == 0 && s < n; ++s) {
      Slot& e = list[s];
      e.bank = -1;
      for (int k = 0; k < 3 && e.used; ++k)
        if (memcmp(e.md5, rom_md5_[k], 16) == 0) e.bank = -2 - k;
    }
    core::File f;
    if (!nnodes_ || !f.open("/.sidecar/dx7.bin", core::kRead)) return;
    for (int k = k0; k < k1 && k < nnodes_; ++k) {
      if (!nodes_[k].bank) continue;
      uint8_t m[16];
      if (!f.seekSet(sizeof(Header) + (uint32_t)k * sizeof(Rec) + offsetof(Rec, md5)) || f.read(m, 16) != 16) break;
      for (int s = 0; s < n; ++s)
        if (list[s].used && list[s].bank == -1 && memcmp(list[s].md5, m, 16) == 0) list[s].bank = (int16_t)k;
    }
    f.close();
  }

 private:
  DX7_FLASH(project_file) void project_file(const char* project) {
    char n[core::kKeyMax];
    core::project_key(project, n, sizeof n);
    sniprintf(L().file, sizeof L().file, "/.rytm/fm/%s.txt", n);
  }

  static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  }

  // "<32 hex>\t<voice>\t<name>"
  DX7_FLASH(parse) static bool parse(const char* line, Slot* s, uint8_t* vd, bool* have) {
    s->name[0] = 0;
    s->bank = -1;
    for (int i = 0; i < 16; ++i) {
      int a = hex(line[2 * i]), b = a < 0 ? -1 : hex(line[2 * i + 1]);
      if (a < 0 || b < 0) return false;
      s->md5[i] = (uint8_t)(a << 4 | b);
    }
    if (line[32] != '\t' || line[33] < '0' || line[33] > '9') return false;
    const char* p = line + 33;
    int v = 0;
    for (; *p >= '0' && *p <= '9'; ++p)
      if ((v = v * 10 + (*p - '0')) > 31) return false;
    s->voice = (uint8_t)v;
    if (*p == '\t') {
      strncpy(s->name, p + 1, sizeof s->name - 1);
      s->name[sizeof s->name - 1] = 0;
      if (char* t = strchr(s->name, '\t')) *t = 0;
      if (const char* q = strchr(p + 1, '\t')) {   // 0.4.0: the voice's own bytes
        int i = 0;
        for (; i < 128; ++i) {
          int a = hex(q[1 + 2 * i]), b = a < 0 ? -1 : hex(q[2 + 2 * i]);
          if (a < 0 || b < 0) break;
          vd[i] = (uint8_t)(a << 4 | b);
        }
        *have = i == 128;
      }
    }
    return true;
  }

  // The list file's text. A table, not sniprintf: 18,000 calls on the control core at 127 rows (core/tests/test_presets.cpp measures)
  static char* hex_out(char* t, const uint8_t* b, int n) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < n; ++i) *t++ = d[b[i] >> 4], *t++ = d[b[i] & 15];
    return t;
  }
 public:
  static uint32_t text(char* t, uint32_t cap, const Slot* list, int n, const uint8_t (*vd)[128], const bool* have) {
    char* e = t;
    for (int s = 0; s < n && (uint32_t)(e - t) + 330 <= cap; ++s) {
      if (list[s].used) {
        e = hex_out(e, list[s].md5, 16);
        e += sniprintf(e, 24, "\t%d\t%s", list[s].voice, list[s].name);
        if (have[s]) *e++ = '\t', e = hex_out(e, vd[s], 128);
      }
      *e++ = '\n';
    }
    return (uint32_t)(e - t);
  }
#if CORE_MSAMP
  // An export of a project's list not opened here since 0.4.0: its voices from their banks into the file. How many used slots
  // still have none (their bank is not on this card); -1 not read or not written (the file as it was). The card core's.
  DX7_FLASH(fill_file) int fill_file(const char* file) {
    static Slot l[kMaxList];
    static uint8_t vd[kMaxList][128];
    static bool have[kMaxList];
    static char t[kMaxList * 330];
    int n = 0, missing = 0;
    bool got = false;
    core::File f;
    if (!f.open(file, core::kRead)) return 0;   // (no list)
    f.close();
    if (!read_list(file, l, &n, vd, have)) return -1;
    resolve(l, n, 0, nnodes_);
    for (int s = 0; s < n; ++s) {
      if (!l[s].used || have[s]) continue;
      cache(l, n, s, vd[s], &have[s]);
      got = got || have[s];
      missing += !have[s];
    }
    if (got && !core::list_replace(file, t, text(t, sizeof t, l, n, vd, have))) return -1;
    return missing;
  }
#endif
 private:
  DX7_FLASH(save) void save() {
#if CORE_MSAMP
    if (core::cardq::worker()) {   // (the card core: fmt's posted queue, the folder made there)
      static char t[kMaxList * 330];
      uint32_t k = text(t, sizeof t, L().list, L().n, L().vdata, L().cached);
      if (!core::list_post(L().file, t, k)) log_("fm list: cannot write %s", L().file);
      return;
    }
#endif
    sd_->mkdir("/.rytm/fm");
    core::File f;
    if (!f.open(L().file, core::kWrite)) {
      log_("fm list: cannot write %s", L().file);
      return;
    }
    char line[344];
    for (int s = 0; s < L().n; ++s) {
      int n = 0;
      if (L().list[s].used) {
        for (int i = 0; i < 16; ++i) n += sniprintf(line + n, sizeof line - n, "%02x", L().list[s].md5[i]);
        n += sniprintf(line + n, sizeof line - n, "\t%d\t%s", L().list[s].voice, L().list[s].name);
        if (L().cached[s]) {
          line[n++] = '\t';
          for (int i = 0; i < 128; ++i) n += sniprintf(line + n, sizeof line - n, "%02x", L().vdata[s][i]);
        }
      }
      line[n++] = '\n';
      f.write(line, n);
    }
    f.close();
  }

  core::Fs* sd_ = nullptr;
  static void nolog(const char*, ...) {}   // until begin(): a core whose card did not mount never calls it
  log_fn log_ = nolog;
  uint8_t rom_md5_[3][16];
  Node nodes_[kMaxNodes];
  int nnodes_ = 0;
  int named_ = -1;           // the bank whose names are in names_
  char names_[32][10];
  // the project lists, one per voice slot (each box's project its own: core/pool/pool.h): the slot in hand's
  // through sel_ (set by the core: prj_slot), slot s's named through listed(s) / slot(s, i)
  struct List {
    char file[48] = "";
    Slot list[kMaxList];
    int n = 0;
    uint8_t vdata[kMaxList][128];   // the list's voices (16 KB)
    bool cached[kMaxList] = {};
  };
  List lists_[kListSlots];
  int (*sel_)() = nullptr;
  List& L() { return lists_[pick(sel_ ? sel_() : 0)]; }
  const List& L() const { return lists_[pick(sel_ ? sel_() : 0)]; }
  static int pick(int s) { return s >= 0 && s < kListSlots ? s : 0; }
};

}  // namespace dx7
