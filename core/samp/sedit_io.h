// core/samp/sedit_io.h - the sample editor's card side: a WAV loaded whole into float at its own depth (Loader), a
// copy saved as <stem>-ed<N>.wav next to it (edit_name, Saver). Every card operation is a cardq request run on the
// card core; the Loader and the Saver are stepped by the control core, one request or one 64 KB piece a step. The
// source file is never opened for writing: the Saver refuses a target that is there and writes <target>.tmp first,
// renamed into place when it is whole. Pi and Mac only, cold.
#pragma once
#include <stdint.h>

#include "../msamp/cardq.h"
#include "../platform.h"

#if CORE_MSAMP

#include "../lib/sha256.h"

namespace core {
namespace sedit {

enum : int { kErrOpen = -1, kErrFormat = -2, kErrLong = -3, kErrRam = -4, kErrWrite = -5, kErrExists = -6, kErrName = -7,
             kErrBusy = -8 };   // kErrBusy: begin() while the object's last card request still runs
// The message for an error code above ("" for anything else).
const char* err_text(int err);

// The save name for src_path: "<folder>/<stem>-ed<N>.wav", N one past the highest <stem>-ed<k>.wav in the folder
// (from 1). A trailing -ed<k> on the source's stem is dropped first (kick-ed1.wav saves as kick-ed2.wav). The folder
// is listed in one kFnReq on the card core (the caller waits for it). 0 ok, -1 too long, -2 the folder not there.
int edit_name(const char* src_path, char* out, int cap);
// edit_name without the wait (the control core's save): begin posts the folder's listing (a kFnReq), step takes its
// answer at a later call: 1 still listing, 0 done (out holds the name), -1 too long, -2 the folder not there. begin: 0,
// -1 too long, -2 no folder in src_path, kErrBusy (the last listing still runs). One at a time (edit_name's stem).
struct Namer {
  int begin(const char* src_path, char* out, int cap);
  int step();

 private:
  cardq::Req r_ = {};
  char* out_ = nullptr;
  int cap_ = 0, dl_ = 0;
  char src_[cardq::kPathMax] = "";
  bool on_ = false;
};
// The name rule alone: "<stem minus -ed<k>>-ed<N>.wav", N one past the highest of taken[0..ntaken) (1 when none).
void edit_name_from(const char* stem, const int* taken, int ntaken, char* out, int cap);

// A whole WAV into float (interleaved, -1..1, at its own depth): begin, then step until it is not 1. x is a private
// pcache block (blk) allocated after the header is read; the caller owns it once step returned 0 and frees it with
// pcache::release. An error frees it and closes the file.
struct Loader {
  int begin(const char* path);   // 0, kErrName, or kErrBusy (its last request still runs)
  int step();                    // 1 more, 0 done, < 0 a kErr* code
  uint32_t loaded() const { return frame_; }   // the frames decoded so far
  float* x = nullptr;
  uint32_t frames = 0, rate = 0;
  uint8_t ch = 0, bits = 0;
  int blk = -1;

 private:
  int fail(int err);
  cardq::Req r_ = {};
  File f_;
  char path_[cardq::kPathMax] = "";
  uint8_t st_ = 0;
  int err_ = 0;
  uint32_t size_ = 0, fb_ = 0, data_off_ = 0, data_len_ = 0, pos_ = 0, carry_ = 0, frame_ = 0, ask_ = 0;
};

// A copy of x (frames of ch channels) written as a ch / bits / rate PCM WAV (bits 16, 24 or 32, clipped): begin, then
// step until it is not 1. The target must not be there (kErrExists); <target>.tmp is written (a stale one removed
// first), closed and renamed to target, and the library told its hash (lib::touched, on the card core). Each data piece
// is held back while the card queue is busy (at most kUploadWaitMs), as the sampler's WavWriter is.
struct Saver {
  // 0, kErrName, kErrFormat, kErrBusy (its last request still runs)
  int begin(const char* target, const float* x, int ch, int bits, uint32_t rate, uint32_t frames);
  int step();   // 1 more, 0 done, < 0 a kErr* code
  uint32_t written() const { return pos_; }   // the frames encoded so far
  uint32_t yields = 0;

 private:
  int fail(int err);
  cardq::Req r_ = {};
  File f_;
  char target_[cardq::kPathMax] = "", tmp_[cardq::kPathMax] = "";
  lib::Sha256 sh_;
  uint32_t crc_ = 0;
  uint8_t sha_[32] = {};
  const float* x_ = nullptr;
  int ch_ = 1, bits_ = 16, err_ = 0;
  uint32_t rate_ = 0, frames_ = 0, pos_ = 0, bytes_ = 0, len_ = 0, wait0_ = 0;
  uint8_t st_ = 0;
  bool waiting_ = false, filled_ = false;
};

}  // namespace sedit
}  // namespace core

#endif
