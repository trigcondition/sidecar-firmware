// core/samp/out.h - the sampler's and the auto-sampler's files on the card (docs/SAMPLING.md): folders, small text
// files (.sfz) at once, WAVs in kPiece writes, each held back while the card queue is busy (cardq.h: a stream needs
// the card; at most kUploadWaitMs a piece, as an upload). The card core's (core 0 on the Pi). Cold.
#pragma once
#include <stdint.h>

#include "../lib/sha256.h"
#include "../msamp/cardq.h"
#include "../platform.h"

namespace core {
namespace samp {

bool mkdirs(const char* path);   // every level of path ("/A/B/C"), true if they are there now
bool write_text(const char* path, const char* text, int len);   // (a library file: reported with its hash, core/lib)
// A file name safe on the card and in SFZ: [A-Za-z0-9_-], others '_', at most cap - 1
void safe_name(const char* in, char* out, int cap);
// The next free "<prefix>_NNN" in dir (NNN from 001: one more than the highest there; ext: ".wav" or "" for folders)
void next_name(const char* dir, const char* prefix, const char* ext, char* out, int cap);

// One WAV from frames [a, a + n) of x (ch 1 or 2, interleaved float), times gain, 24-bit; fade_in / fade_out frames
// at its ends (linear); an optional loop (its frames relative to a, inclusive end) in a smpl chunk at root. Its sha256
// and CRC-32 are made from the bytes as they are written and handed to the library when the file is closed
// (lib::touched: the take is known by its hash without a second read, docs/LIBRARY.md).
struct WavWriter {
  bool begin(const char* path, const float* x, int ch, uint32_t a, uint32_t n, float gain, uint32_t fade_in,
             uint32_t fade_out, bool loop = false, uint32_t loop_s = 0, uint32_t loop_e = 0, int root = 60);
  int step();       // 1 more to write (or held back), 0 done (closed), -1 failed (closed)
  bool busy() const { return open_; }
  uint32_t yields = 0, bytes = 0;

 private:
  File f_;
  char path_[160] = "";
  lib::Sha256 sh_;
  uint32_t crc_ = 0;
  const float* x_ = nullptr;
  int ch_ = 1;
  uint32_t a_ = 0, n_ = 0, pos_ = 0, fin_ = 0, fout_ = 0, wait0_ = 0;
  float gain_ = 1;
  bool open_ = false, waiting_ = false, odd_ = false;
};

}  // namespace samp
}  // namespace core
