// The engine's index of the card's /Samples (docs/sampler-plan.md, "The card index"), on core::File / core::Fs
// (core/platform.h). At boot the board's fingerprint of /Samples (core::Fs::tree_hash: the Teensy hashes the raw
// FAT32 / exFAT entries, 0.7 s on a 449-folder FAT32 card, 81 ms on 75 exFAT folders; other boards their own
// listing) is compared with /.rytm/env; if it matches, the stored index is used, else the index is rebuilt (one
// full scan reading every .wav's header) and written to /.rytm/ - the only writes to the card. The index lives
// at the card's root, outside /Samples, so writing it does not change the
// listing it checks.
//
//   /.rytm/folders.bin  Folder records, breadth first (a folder's subfolders
//                       are consecutive), folder 0 = /Samples
//   /.rytm/files.bin    FileRec records, each folder's .wav files consecutive,
//                       in name order (sorted when built: version 3); integer PCM
//                       16/24/32-bit only (version 4: float WAVs, format tag 3 or
//                       extensible float, are left out - they decoded as noise)
//   /.rytm/env          "hash=<16 hex>\nversion=4\nfolders=N\nfiles=N\n",
//                       written last (a half-written index has no env; another
//                       version's index is rebuilt, never read)
//
// FAT32 and exFAT. Folders and files are found again by their entry's position
// in their folder (dir index; exFAT has no 8.3 names), opened down from
// /Samples. macOS "._" files and anything starting with "." are left out.
#pragma once
#include "../platform.h"

namespace card {

const int kMaxFolders = 1024;
const int kName = 40;

struct Folder {           // 68 bytes
  char name[kName];       // long name (cut), "" for the root
  uint32_t dir_index;     // its entry's position in its parent folder
  uint8_t pad[10];
  int16_t parent;
  uint16_t nsub, first_sub;
  uint16_t nfiles;
  uint32_t first_file;
};

struct FileRec {          // 84 bytes
  char name[kName];
  uint32_t dir_index;     // its entry's position in its folder
  uint8_t pad0[8];
  uint8_t format;         // the WAV format tag (1: integer PCM, the only one indexed)
  uint8_t channels, bits, contiguous;
  uint32_t size;
  uint32_t first_sector;  // of the file, when contiguous (raw reads)
  uint32_t data_offset, data_len, rate;
  uint32_t pad[2];
};
// folders.bin/files.bin hold these as they are: the on-card format must be the same on every board.
static_assert(sizeof(Folder) == 68 && sizeof(FileRec) == 84, "card index records changed size");

typedef void (*log_fn)(const char* fmt, ...);

class Index {
 public:
  // Mount the card and get an index (stored or rebuilt). false: no card, not
  // FAT32, or no /Samples. scratch: RAM for a rebuild's sorting (a folder's
  // records at a time; one with more files than fit is sorted in runs).
  bool begin(log_fn log, void* scratch, uint32_t scratch_bytes);

  int folders() const { return nfolders_; }
  const Folder& folder(int i) const { return folders_[i]; }
  // Subfolders of folder f holding .wav files below (wavs_below_ > 0), as
  // folder indices sorted by name; returns how many (up to max).
  int subfolders(int f, uint16_t* out, int max) const;
  // Open file record r of folder f (by dir index, down from /Samples).
  bool open(int f, const FileRec& r, core::File* out);
  // File i of folder f in name order (0..nfiles-1), from files.bin (sorted
  // when the index is built: no sorting while the voices play); CORE_MSAMP (the Pi, the Mac): from its copy in RAM,
  // read at begin() - no card time, so the control core may call it (loadstall, 0.3.0).
  bool file(int f, int i, FileRec* r);
  uint64_t hash() const { return hash_; }
  core::Fs& sd() { return sd_; }

 private:
  uint32_t walked_ = 0, hidden_ = 0;   // entries the last tree_hash hashed, and left out ('.')
  bool load(uint64_t h);
  bool build(uint64_t h);
  bool open_folder(int f, core::File* out);
  static bool parse_wav(core::File& f, FileRec* r);
  void flush_sorted(core::File& out, FileRec* recs, int n);
  FileRec* scratch_ = nullptr;
  uint32_t scratch_n_ = 0;   // FileRecs it holds

  core::Fs sd_;
  core::File files_;
  FileRec* ram_ = nullptr;   // CORE_MSAMP: files.bin's first nram_ records (begin; file() reads them, no card)
  uint32_t nram_ = 0;
  static constexpr uint32_t kRamFiles = 131072;   // 11 MB at most
  Folder* folders_ = nullptr;
  uint32_t* wavs_below_ = nullptr;
  int nfolders_ = 0;
  uint32_t nfiles_ = 0;
  uint64_t hash_ = 0;
  log_fn log_ = nullptr;
};

}  // namespace card
