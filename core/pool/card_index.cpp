// card_index.h's implementation (see there). None of it is real time: it
// runs from flash (CORE_COLD), leaving ITCM to the render path.
#include <stdlib.h>
#include "card_index.h"
#include "../heap_buffers.h"

#include <new>
#include <string.h>
#include <strings.h>

namespace card {

namespace {

#ifdef CORE_HEAP_BUFFERS
Folder* folder_mem = nullptr;   // heap_place (core/heap_buffers.h)
#else
CORE_RAM2 Folder folder_mem[kMaxFolders];       // 64 KB in RAM2
#endif
CORE_RAM2 uint32_t below_mem[kMaxFolders];

CORE_COLD bool hidden(const char* n) { return n[0] == '.'; }   // ".", "..", "._x", ".rytm"

CORE_COLD bool is_wav(const char* n) {
  size_t l = strlen(n);
  return l > 4 && !hidden(n) && strcasecmp(n + l - 4, ".wav") == 0;
}

CORE_COLD uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
CORE_COLD uint16_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }

}  // namespace

// Folder f, opened down from /Samples by its chain of dir indices.
CORE_COLD bool Index::open_folder(int f, core::File* out) {
  int chain[16], k = 0;
  for (int i = f; i > 0 && k < 16; i = folders_[i].parent) chain[k++] = i;
  core::File cur;
  if (!cur.open("/Samples", core::kRead)) return false;
  while (k-- > 0) {
    core::File next;
    if (!next.open(&cur, folders_[chain[k]].dir_index, core::kRead)) return false;
    cur.close();
    cur = next;
  }
  *out = cur;
  return true;
}

CORE_COLD bool Index::open(int f, const FileRec& r, core::File* out) {
  core::File dir;
  if (!open_folder(f, &dir)) return false;
  bool ok = out->open(&dir, r.dir_index, core::kRead);
  dir.close();
  return ok;
}

// RIFF/WAVE: "fmt " and "data", anywhere in the first 4 KB. r->format: the format tag (1 integer PCM, 3
// IEEE float), WAVE_FORMAT_EXTENSIBLE's (0xFFFE) taken from its SubFormat GUID.
CORE_COLD bool Index::parse_wav(core::File& f, FileRec* r) {
  uint8_t b[12];
  f.rewind();
  if (f.read(b, 12) != 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) return false;
  uint32_t pos = 12;
  bool fmt = false;
  while (pos < 4096) {
    uint8_t c[24];
    if (!f.seekSet(pos) || f.read(c, 8) != 8) return false;
    uint32_t sz = le32(c + 4);
    if (!memcmp(c, "fmt ", 4)) {
      if (f.read(c + 8, 16) != 16) return false;
      r->channels = (uint8_t)le16(c + 10);
      r->rate = le32(c + 12);
      r->bits = (uint8_t)le16(c + 22);
      uint16_t tag = le16(c + 8);
      if (tag == 0xFFFE && sz >= 26) {   // extensible: SubFormat's first two bytes at offset 24 of the chunk
        uint8_t x[10];
        if (f.read(x, 10) != 10) return false;
        tag = le16(x + 8);
      }
      r->format = tag > 255 ? 255 : (uint8_t)tag;
      fmt = true;
    } else if (!memcmp(c, "data", 4)) {
      r->data_offset = pos + 8;
      r->data_len = sz;
      return fmt;
    }
    pos += 8 + sz + (sz & 1);
  }
  return false;
}

CORE_COLD bool Index::load(uint64_t h) {
  core::File env;
  if (!env.open("/.rytm/env", core::kRead)) return false;
  char t[160] = {0};
  env.read(t, sizeof t - 1);
  env.close();
  char want[40];
  sniprintf(want, sizeof want, "hash=%08lx%08lx\n", (unsigned long)(h >> 32), (unsigned long)(uint32_t)h);
  if (strncmp(t, want, strlen(want)) || !strstr(t, "version=4\n")) return false;
  core::File fo;
  if (!fo.open("/.rytm/folders.bin", core::kRead)) return false;
  int n = fo.fileSize() / sizeof(Folder);
  if (n < 1 || n > kMaxFolders) return false;
  fo.read(folders_, n * sizeof(Folder));
  fo.close();
  if (!files_.open("/.rytm/files.bin", core::kRead)) return false;
  nfolders_ = n;
  nfiles_ = files_.fileSize() / sizeof(FileRec);
  return true;
}

// A run of a folder's records, sorted by name, to files.bin.
CORE_COLD void Index::flush_sorted(core::File& out, FileRec* recs, int n) {
  for (int gap = n / 2; gap > 0; gap /= 2)   // shell sort
    for (int i = gap; i < n; ++i)
      for (int j = i; j >= gap && strcasecmp(recs[j - gap].name, recs[j].name) > 0; j -= gap) {
        FileRec t = recs[j];
        recs[j] = recs[j - gap];
        recs[j - gap] = t;
      }
  out.write(recs, n * sizeof(FileRec));
}

// Breadth first from /Samples: each folder's subfolders appended together (so
// consecutive), its .wav files to files.bin together, by name.
CORE_COLD bool Index::build(uint64_t h) {
  uint32_t t0 = core::now_ms();
  sd_.mkdir("/.rytm");
  sd_.remove("/.rytm/env");               // no env until the index is whole
  core::File out, fo;
  if (!out.open("/.rytm/files.bin", core::kWrite)) return false;
  memset(&folders_[0], 0, sizeof(Folder));
  folders_[0].parent = -1;
  nfolders_ = 1;
  nfiles_ = 0;
  uint32_t skipped = 0;
  char name[256];
  for (int f = 0; f < nfolders_; ++f) {
    Folder& F = folders_[f];
    core::File dir, e;
    if (!open_folder(f, &dir)) continue;
    F.first_sub = nfolders_;
    F.first_file = nfiles_;
    uint32_t held = 0;   // this folder's records in scratch_
    while (e.openNext(&dir, core::kRead)) {
      e.getName(name, sizeof name);
      if (hidden(name)) {
        e.close();
        continue;
      }
      if (e.isDir()) {
        if (nfolders_ < kMaxFolders) {
          Folder& S = folders_[nfolders_];
          memset(&S, 0, sizeof S);
          strncpy(S.name, name, kName - 1);
          S.dir_index = e.dirIndex();
          S.parent = (int16_t)f;
          ++nfolders_;
          ++F.nsub;
        }
      } else if (is_wav(name)) {
        FileRec r;
        memset(&r, 0, sizeof r);
        strncpy(r.name, name, kName - 1);
        r.dir_index = e.dirIndex();
        r.size = e.fileSize();
        if (parse_wav(e, &r) && r.format == 1 && (r.bits == 16 || r.bits == 24 || r.bits == 32)) {   // integer PCM
          uint32_t a, b;
          r.contiguous = e.contiguousRange(&a, &b);
          r.first_sector = r.contiguous ? a : 0;
          if (held == scratch_n_) {   // more than scratch holds: sorted in runs
            flush_sorted(out, scratch_, held);
            held = 0;
          }
          scratch_[held++] = r;
          ++nfiles_;
          ++F.nfiles;
        } else {
          ++skipped;
        }
      }
      e.close();
    }
    dir.close();
    flush_sorted(out, scratch_, held);
    if ((f & 31) == 0 && log_) log_(CORE_STR("index: %d/%d folders, %lu files"), f, nfolders_, (unsigned long)nfiles_);
  }
  out.close();
  if (!fo.open("/.rytm/folders.bin", core::kWrite)) return false;
  fo.write(folders_, nfolders_ * sizeof(Folder));
  fo.close();
  core::File env;
  if (!env.open("/.rytm/env", core::kWrite)) return false;
  char t[160];
  int n = sniprintf(t, sizeof t, "hash=%08lx%08lx\nversion=4\nfolders=%d\nfiles=%lu\n",
                   (unsigned long)(h >> 32), (unsigned long)(uint32_t)h, nfolders_, (unsigned long)nfiles_);
  env.write(t, n);
  env.close();
  if (log_)
    log_(CORE_STR("index built: %d folders, %lu files (%lu .wav not usable), %lu ms"), nfolders_,
         (unsigned long)nfiles_, (unsigned long)skipped, (unsigned long)(core::now_ms() - t0));
  return files_.open("/.rytm/files.bin", core::kRead);
}

#ifdef CORE_HEAP_BUFFERS
size_t heap_bytes() { return sizeof(Folder[kMaxFolders]); }
CORE_COLD void heap_place(void* mem) { folder_mem = (Folder*)mem; }
#endif

CORE_COLD bool Index::begin(log_fn log, void* scratch, uint32_t scratch_bytes) {
  log_ = log;
  scratch_ = (FileRec*)scratch;
  scratch_n_ = scratch_bytes / sizeof(FileRec);
  folders_ = folder_mem;
  wavs_below_ = below_mem;
  uint32_t tb = core::now_ms();
  if (!sd_.begin()) {
    if (log_) log_(CORE_STR("card: none, or not FAT32/exFAT"));
    return false;
  }
  if (log_) log_(CORE_STR("card: %s, mounted in %lu ms"), sd_.type_name(),
                 (unsigned long)(core::now_ms() - tb));
  uint32_t t0 = core::now_ms();
  uint64_t h = sd_.tree_hash("/Samples", &walked_, &hidden_);
  if (!h) {
    if (log_) log_(CORE_STR("card: no /Samples"));
    return false;
  }
  hash_ = h;
  if (log_) log_(CORE_STR("card: listing hash %08lx%08lx in %lu ms (%lu entries, %lu hidden)"), (unsigned long)(h >> 32),
                 (unsigned long)(uint32_t)h, (unsigned long)(core::now_ms() - t0), (unsigned long)walked_, (unsigned long)hidden_);
  if (load(h)) {
    if (log_) log_(CORE_STR("index: unchanged, loaded (%d folders, %lu files)"), nfolders_, (unsigned long)nfiles_);
  } else {
    if (log_) log_(CORE_STR("index: card changed or no index - building"));
    if (!build(h)) {
      if (log_) log_(CORE_STR("index: build failed"));
      return false;
    }
  }
  if (log_) log_(CORE_STR("index: files.bin %s"), files_.isContiguous() ? "contiguous" : "fragmented");
#if CORE_MSAMP
  // loadstall (0.3.0): the records in RAM, read once here (the boot, the card's core) - file() is a copy then, never a
  // card read on the control core (the TEENSY listing 0D, LOAD TO PROJ 0F, DSP SAMPLE's reports and trigs, pool_sync's
  // lookups: one 84-byte seek + read each, 1-7 ms a message measured on the Pi). Up to kRamFiles (a card with more:
  // the rest from the card as before)
  free(ram_);
  ram_ = nullptr, nram_ = 0;
  uint32_t n = nfiles_ < kRamFiles ? nfiles_ : kRamFiles;
  if (n && (ram_ = (FileRec*)malloc(n * sizeof(FileRec)))) {   // (malloc: the Pi's runtime has no nothrow new[]; FileRec is POD)
    if (files_.seekSet(0) && files_.read(ram_, n * sizeof(FileRec)) == (int)(n * sizeof(FileRec))) nram_ = n;
    else free(ram_), ram_ = nullptr;
  }
  if (log_) log_(CORE_STR("index: %lu of %lu file records in RAM"), (unsigned long)nram_, (unsigned long)nfiles_);
#endif
  // .wav files at or below each folder, leaves first (breadth first: a
  // folder's subfolders come after it)
  for (int i = 0; i < nfolders_; ++i) wavs_below_[i] = folders_[i].nfiles;
  for (int i = nfolders_ - 1; i > 0; --i) wavs_below_[folders_[i].parent] += wavs_below_[i];
  return true;
}

CORE_COLD int Index::subfolders(int f, uint16_t* out, int max) const {
  int n = 0;
  const Folder& F = folders_[f];
  for (int i = 0; i < F.nsub && n < max; ++i) {
    int s = F.first_sub + i;
    if (s < nfolders_ && wavs_below_[s]) out[n++] = (uint16_t)s;
  }
  for (int i = 1; i < n; ++i)              // by name (insertion sort: few)
    for (int j = i; j > 0 && strcasecmp(folders_[out[j - 1]].name, folders_[out[j]].name) > 0; --j) {
      uint16_t t = out[j];
      out[j] = out[j - 1];
      out[j - 1] = t;
    }
  return n;
}

CORE_COLD bool Index::file(int f, int i, FileRec* r) {
  if (f < 0 || f >= nfolders_ || i < 0 || i >= folders_[f].nfiles) return false;
  uint32_t k = folders_[f].first_file + i;
  if (k < nram_) {   // (CORE_MSAMP: the RAM copy, no card)
    *r = ram_[k];
    return true;
  }
  if (k >= nfiles_ || !files_.seekSet(k * sizeof(FileRec))) return false;
  return files_.read(r, sizeof *r) == (int)sizeof *r;
}

}  // namespace card
