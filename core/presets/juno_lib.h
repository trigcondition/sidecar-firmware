// Juno-106 patches on the SD card, for the JUNO voices (docs/PLAN.md 3a').
//
// /JUNO and its subfolders hold .syx files of Juno-106 patch dumps:
// F0 41 30|31 <ch> <patch no> <16 sliders> <2 switch bytes> F7, 24 bytes each
// (30: a patch with its number, 31: the manual settings; 32 control changes
// and anything else in a file are skipped). A file with one patch is one
// patch, named after the file ("001 A11 Accordion"); a file of several (a
// bank: e.g. 128 dumps) is a folder of its own, named after the file, whose
// entries are its patches in file order, named by the dump's patch number and
// the file ("A11 Factory", .. "B88 Factory"; a 31 dump "#5 Factory"). The
// folders and banks are found at boot (a walk of /JUNO in the card's directory
// order, breadth first, no index: the patches are tiny; a file counts as a
// bank when it holds more than one patch message); a folder's patches are
// listed when asked for (one folder at a time). A bank found after
// kMaxFolders is listed as one entry per dump ("Bank #1", ..) instead.
//
// The project's JUNO list - what LOAD TO PROJ on a patch adds to, and what the
// JUNO machine's PAT steps through - keeps each patch's own 18 bytes and name,
// so moving or renaming files on the card never breaks it:
//   /.rytm/juno/<PROJECT>.txt   a line per slot: 36 hex digits, a tab, the
//                               name; an empty line an empty slot
#pragma once
#include "../platform.h"
#include "../project_key.h"

#ifdef FLASHMEM   // the Teensy's: run from flash, a section per function (inline
                  // functions in one shared section would conflict)
#define JLIB_FLASH(f) __attribute__((section(".flashmem.jlib." #f)))
#else
#define JLIB_FLASH(f)
#endif

namespace jlib {

const int kMaxFolders = 64;    // folders and banks
const int kMaxFiles = 160;     // per folder
const int kMaxList = 127;      // the project's JUNO list (16's total is 7 bits)

struct Folder {
  char name[24];
  char path[96];
  int16_t parent;
  bool bank;        // a .syx file of several patches
  uint16_t names_at, nnames;   // its patch files in names_ (nnames kNotKept: walk the folder)
};

// The patch files' names and sizes, found by the boot walk and kept so that
// listing a folder never walks it on the card (128 patch files took ~10 ms:
// a glitch while the voices play). RAM2, 15 KB; folders past it are walked.
struct Name {
  char name[36];
  uint32_t size;
};
const int kNames = 384;
const uint16_t kNotKept = 0xffff;
extern Name names_[kNames];   // presets/libs.cpp: one copy (RAM2, 15 KB)

struct Patch {
  char name[24];
  uint8_t b[18];
  bool used;
};

typedef void (*log_fn)(const char* fmt, ...);

class Lib {
 public:
  // Walk /JUNO for its folders (folder 0 is /JUNO itself).
  JLIB_FLASH(begin) void begin(core::Fs* sd, log_fn log) {
    sd_ = sd;
    log_ = log;
    nfolders_ = 0;
    listed_ = -1;
    core::File root;
    if (!root.open("/JUNO", core::kRead) || !root.isDir()) {
      log_("juno: no /JUNO on the card");
      return;
    }
    root.close();
    add_folder("JUNO", "/JUNO", -1, false);
    int banks = 0;
    int nn = 0;
    for (int i = 0; i < nfolders_; ++i) {   // breadth first: the list grows
      if (folders_[i].bank) continue;
      core::File dir, e;
      folders_[i].names_at = nn;
      folders_[i].nnames = 0;
      if (!dir.open(folders_[i].path, core::kRead)) continue;
      char nm[64];
      while (e.openNext(&dir, core::kRead)) {
        e.getName(nm, sizeof nm);
        int len = strlen(nm);
        if (nm[0] != '.' && nfolders_ < kMaxFolders) {
          char path[96];
          sniprintf(path, sizeof path, "%s/%s", folders_[i].path, nm);
          if (e.isDir()) {
            add_folder(nm, path, i, false);
          } else if (len > 4 && strcasecmp(nm + len - 4, ".syx") == 0 && e.fileSize() > 24 &&
                     scan(e, -1, nullptr) > 1) {
            nm[len - 4] = 0;
            add_folder(nm, path, i, true);
            ++banks;
          } else if (len > 4 && strcasecmp(nm + len - 4, ".syx") == 0 && e.fileSize() >= 24 &&
                     folders_[i].nnames != kNotKept) {
            if (nn < kNames && folders_[i].nnames < kMaxFiles) {
              strncpy(names_[nn].name, nm, sizeof names_[0].name - 1);
              names_[nn].name[sizeof names_[0].name - 1] = 0;
              names_[nn].size = e.fileSize();
              ++nn;
              ++folders_[i].nnames;
            } else {   // no room: this folder is walked when listed
              nn = folders_[i].names_at;
              folders_[i].nnames = kNotKept;
            }
          }
        }
        e.close();
      }
      dir.close();
    }
    log_("juno: %d folders under /JUNO, %d of them banks", nfolders_, banks);
  }

  int folders() const { return nfolders_; }
  const Folder& folder(int j) const { return folders_[j]; }

  // Folder j's subfolders (indices, by name) then its patches: how many.
  int count(int j, uint16_t* sub, int max_sub, int* nsub) {
    int ns = 0;
    for (int k = 0; k < nfolders_ && ns < max_sub; ++k)
      if (folders_[k].parent == j) sub[ns++] = k;
    for (int a = 1; a < ns; ++a)   // by name
      for (int b = a; b > 0 && strcasecmp(folders_[sub[b - 1]].name, folders_[sub[b]].name) > 0; --b) {
        uint16_t t = sub[b];
        sub[b] = sub[b - 1];
        sub[b - 1] = t;
      }
    *nsub = ns;
    list(j);
    return ns + npatches_;
  }

  // Patch i of folder j (in listing order): name, and the bytes if b.
  JLIB_FLASH(patch) bool patch(int j, int i, char* name, uint8_t* b) {
    list(j);
    if (folders_[j].bank) {
      if (i < 0 || i >= npatches_) return false;
      uint8_t c = codes_[i];
      char code[8];
      if (c < 128) sniprintf(code, sizeof code, "%c%d%d", c < 64 ? 'A' : 'B', (c & 63) / 8 + 1, c % 8 + 1);
      else sniprintf(code, sizeof code, "#%d", i + 1);
      sniprintf(name, 24, "%s %s", code, folders_[j].name);
      if (!b) return true;
      core::File file;
      uint8_t m[24];
      if (!file.open(folders_[j].path, core::kRead)) return false;
      bool ok = scan(file, i, m) > i;
      file.close();
      if (ok) memcpy(b, m + 5, 18);
      return ok;
    }
    int k = 0;
    for (int f = 0; f < nfiles_; ++f) {
      int n = dumps_[f];
      if (i < k + n) {
        int d = i - k;
        const char* fn = files_[order_[f]];
        int len = strlen(fn);
        if (len > 4 && strcasecmp(fn + len - 4, ".syx") == 0) len -= 4;
        if (n > 1) sniprintf(name, 24, "%.*s #%d", len > 18 ? 18 : len, fn, d + 1);
        else sniprintf(name, 24, "%.*s", len > 23 ? 23 : len, fn);
        if (!b) return true;
        char path[160];
        sniprintf(path, sizeof path, "%s/%s", folders_[j].path, fn);
        core::File file;
        uint8_t m[24];
        if (!file.open(path, core::kRead)) return false;
        bool ok = file.seekSet(24u * d) && file.read(m, 24) == 24 && m[0] == 0xf0 && m[1] == 0x41 &&
                  (m[2] == 0x30 || m[2] == 0x31) && m[23] == 0xf7;
        file.close();
        if (ok) memcpy(b, m + 5, 18);
        return ok;
      }
      k += n;
    }
    return false;
  }

  // --- the project's JUNO list ---
  int listed() const { return nlist_; }
  const Patch& slot(int i) const { return list_[i]; }

  // 2026-10-27 (core/bundle/prjslot.h): the list's next writes under another project key (a SAVE AS copy placed)
  void rekey(const char* project) { project_file(project); }
  JLIB_FLASH(load_list) void load_list(const char* project) {
    project_file(project);
    nlist_ = 0;
    core::File f;
    if (f.open(file_, core::kRead)) {
      char line[96];
      int n = 0;
      for (;;) {
        int c = f.read();
        if (c < 0 && n == 0) break;
        if (c < 0 || c == '\n') {
          if (nlist_ >= kMaxList) break;
          line[n] = 0;
          Patch& p = list_[nlist_++];
          p.used = parse(line, &p);
          n = 0;
          if (c < 0) break;
        } else if (c != '\r' && n < (int)sizeof line - 1) {
          line[n++] = c;
        }
      }
      f.close();
      while (nlist_ && !list_[nlist_ - 1].used) --nlist_;
    }
    log_("juno list %s: %d slots", file_, nlist_);
  }

  // LOAD TO PROJ on patch i of folder j: into the first empty slot (none if
  // the same patch is there already). Returns the slot, or -1.
  JLIB_FLASH(add) int add(int j, int i) {
    Patch p;
    if (!patch(j, i, p.name, p.b)) return -1;
    p.used = true;
    for (int s = 0; s < nlist_; ++s)
      if (list_[s].used && memcmp(list_[s].b, p.b, 18) == 0) return s;
    int s = 0;
    while (s < nlist_ && list_[s].used) ++s;
    if (s >= kMaxList) return -1;
    list_[s] = p;
    if (s == nlist_) ++nlist_;
    save();
    log_("juno list: slot %d = %s", s, p.name);
    return s;
  }

  // The list's edits (the Pi's web page, core/user/lists.cpp): each saves the list. False: no such slot.
  // A patch's own 18 bytes and name into the first empty slot (none if those bytes are listed already): its slot, -1.
  JLIB_FLASH(put) int put(const uint8_t* b, const char* name) {
    for (int s = 0; s < nlist_; ++s)
      if (list_[s].used && memcmp(list_[s].b, b, 18) == 0) return s;
    int s = 0;
    while (s < nlist_ && list_[s].used) ++s;
    if (s >= kMaxList) return -1;
    Patch& p = list_[s];
    memcpy(p.b, b, 18);
    copy_name(p.name, name, sizeof p.name);
    p.used = true;
    if (s == nlist_) ++nlist_;
    save();
    return s;
  }
  JLIB_FLASH(clear) bool clear(int s) {
    if (s < 0 || s >= nlist_) return false;
    list_[s].used = false;
    while (nlist_ && !list_[nlist_ - 1].used) --nlist_;
    save();
    return true;
  }
  JLIB_FLASH(rename) bool rename(int s, const char* name) {
    if (s < 0 || s >= nlist_ || !list_[s].used) return false;
    copy_name(list_[s].name, name, sizeof list_[s].name);
    save();
    return true;
  }
  JLIB_FLASH(swap) bool swap(int a, int b) {   // two slots trade places (one may be empty, or past the end)
    if (a < 0 || b < 0 || a >= kMaxList || b >= kMaxList || a == b) return false;
    for (; nlist_ <= (a > b ? a : b); ++nlist_) list_[nlist_].used = false;
    Patch t = list_[a];
    list_[a] = list_[b];
    list_[b] = t;
    while (nlist_ && !list_[nlist_ - 1].used) --nlist_;
    save();
    return true;
  }

 private:
  JLIB_FLASH(add_folder) void add_folder(const char* name, const char* path, int parent, bool bank) {
    Folder& f = folders_[nfolders_++];
    f.bank = bank;
    f.names_at = 0;
    f.nnames = kNotKept;   // set by the walk of the folder itself
    strncpy(f.name, name, sizeof f.name - 1);
    f.name[sizeof f.name - 1] = 0;
    strncpy(f.path, path, sizeof f.path - 1);
    f.path[sizeof f.path - 1] = 0;
    f.parent = parent;
  }

  // Folder j's .syx files, by name, and how many dumps each holds.
  JLIB_FLASH(list) void list(int j) {
    if (listed_ == j) return;
    listed_ = j;
    nfiles_ = npatches_ = 0;
    core::File dir, e;
    if (j < 0 || j >= nfolders_) return;
    if (folders_[j].bank) {   // a bank: its patches' numbers, in file order
      core::File file;
      if (file.open(folders_[j].path, core::kRead)) {
        npatches_ = scan(file, -1, nullptr, codes_);
        if (npatches_ > kMaxFiles) npatches_ = kMaxFiles;
        file.close();
      }
      return;
    }
    if (folders_[j].nnames != kNotKept) {   // kept by the boot walk
      for (int k = 0; k < folders_[j].nnames && nfiles_ < kMaxFiles; ++k, ++nfiles_) {
        const Name& n = names_[folders_[j].names_at + k];
        memcpy(files_[nfiles_], n.name, sizeof files_[0]);
        sizes_[nfiles_] = n.size;
      }
    } else if (dir.open(folders_[j].path, core::kRead)) {
    char nm[64];
    while (nfiles_ < kMaxFiles && e.openNext(&dir, core::kRead)) {
      e.getName(nm, sizeof nm);
      int len = strlen(nm);
      uint32_t size = e.fileSize();
      if (!e.isDir() && nm[0] != '.' && len > 4 && strcasecmp(nm + len - 4, ".syx") == 0 && size >= 24 &&
          !is_bank(j, nm)) {
        strncpy(files_[nfiles_], nm, sizeof files_[0] - 1);
        files_[nfiles_][sizeof files_[0] - 1] = 0;
        sizes_[nfiles_] = size;
        ++nfiles_;
      }
      e.close();
    }
    dir.close();
    }
    for (int f = 0; f < nfiles_; ++f) order_[f] = f;
    for (int a = 1; a < nfiles_; ++a)
      for (int b = a; b > 0 && strcasecmp(files_[order_[b - 1]], files_[order_[b]]) > 0; --b) {
        uint8_t t = order_[b];
        order_[b] = order_[b - 1];
        order_[b - 1] = t;
      }
    for (int f = 0; f < nfiles_; ++f) {
      dumps_[f] = sizes_[order_[f]] / 24;
      npatches_ += dumps_[f];
    }
  }

  // Folder j's file nm is one of its banks (a folder of its own).
  bool is_bank(int j, const char* nm) const {
    char path[96];
    sniprintf(path, sizeof path, "%s/%s", folders_[j].path, nm);
    for (int k = 0; k < nfolders_; ++k)
      if (folders_[k].bank && folders_[k].parent == j && strcmp(folders_[k].path, path) == 0) return true;
    return false;
  }

 public:
  // The patch messages of an open .syx file, from its start: F0 41 30|31 <ch>
  // <patch no> <18 bytes> F7 (any other message skipped). With want >= 0
  // stops at patch want, copying its 24 bytes to m. codes (if given, up to
  // kMaxFiles): each patch's number, 0x80 for a 31 (manual) dump. Returns the
  // patches counted (want + 1 when found). At most 64 KB are read.
  JLIB_FLASH(scan) static int scan(core::File& f, int want, uint8_t* m, uint8_t* codes = nullptr) {
    uint8_t buf[512], msg[24];   // a sector a read
    int n = 0, len = -1;   // len -1: outside a message
    uint32_t total = 0;
    f.rewind();
    for (;;) {
      int got = f.read(buf, sizeof buf);
      if (got <= 0 || total > 65536) break;
      total += got;
      for (int k = 0; k < got; ++k) {
        uint8_t c = buf[k];
        if (c == 0xf0) {
          len = 0;
          continue;
        } else if (len < 0) {
          continue;
        } else if (c == 0xf7) {
          if (len == 22 && msg[0] == 0x41 && (msg[1] == 0x30 || msg[1] == 0x31)) {
            if (codes && n < kMaxFiles) codes[n] = msg[1] == 0x30 ? (msg[3] & 0x7f) : 0x80;
            if (n == want) {
              m[0] = 0xf0;
              memcpy(m + 1, msg, 22);
              m[23] = 0xf7;
              return n + 1;
            }
            ++n;
          }
          len = -1;
          continue;
        } else if (c & 0x80) {   // another status byte: not a patch
          len = -1;
          continue;
        }
        if (len < 22) msg[len] = c;
        ++len;
      }
    }
    return want >= 0 ? -1 : n;
  }

 private:
  void project_file(const char* project) {
    char n[core::kKeyMax];
    core::project_key(project, n, sizeof n);
    sniprintf(file_, sizeof file_, "/.rytm/juno/%s.txt", n);
  }

  static void copy_name(char* d, const char* n, int cap) {   // printable ASCII (the list file's tab and line safe)
    int k = 0;
    for (; n && n[k] && k < cap - 1; ++k) d[k] = n[k] >= 0x20 && n[k] <= 0x7e ? n[k] : '_';
    d[k] = 0;
  }
  static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  }

  static bool parse(const char* line, Patch* p) {
    p->name[0] = 0;
    for (int i = 0; i < 18; ++i) {
      int a = hex(line[2 * i]), b = a < 0 ? -1 : hex(line[2 * i + 1]);
      if (a < 0 || b < 0) return false;
      p->b[i] = (uint8_t)(a << 4 | b);
    }
    if (line[36] == '\t') {
      strncpy(p->name, line + 37, sizeof p->name - 1);
      p->name[sizeof p->name - 1] = 0;
    }
    return true;
  }

  JLIB_FLASH(save) void save() {
    sd_->mkdir("/.rytm/juno");
    core::File f;
    if (!f.open(file_, core::kWrite)) {
      log_("juno list: cannot write %s", file_);
      return;
    }
    char line[64];
    for (int s = 0; s < nlist_; ++s) {
      int n = 0;
      if (list_[s].used) {
        for (int i = 0; i < 18; ++i) n += sniprintf(line + n, sizeof line - n, "%02x", list_[s].b[i]);
        n += sniprintf(line + n, sizeof line - n, "\t%s", list_[s].name);
      }
      line[n++] = '\n';
      f.write(line, n);
    }
    f.close();
  }

  core::Fs* sd_ = nullptr;
  static void nolog(const char*, ...) {}   // until begin(): a core whose card did not mount never calls it
  log_fn log_ = nolog;
  Folder folders_[kMaxFolders];
  int nfolders_ = 0;
  // the folder listed last
  int listed_ = -1, nfiles_ = 0, npatches_ = 0;
  char files_[kMaxFiles][36];
  uint32_t sizes_[kMaxFiles];
  uint8_t order_[kMaxFiles];
  uint16_t dumps_[kMaxFiles];
  uint8_t codes_[kMaxFiles];   // a bank's patch numbers (0x80: none)
  // the project's list
  char file_[48] = "";
  Patch list_[kMaxList];
  int nlist_ = 0;
};

}  // namespace jlib
