// pi/engine/loop_keep.h - KEEP (spec-050 3.9): a grab written to the card as a take, from core 0's idle loop and never by
// core 3. /Recordings/<take>/00_LOOP.wav (stereo, 24 bit, 48 kHz, the recorder's 512-byte header with the true sizes) and
// loop.txt (bpm, bars, grabbed), the take named as the recorder names its own (rec_names.h: "<slug>_<date>_<time>" or
// "<slug>_take-NNN"), so the Record tab and the library list it. One piece (kPiece bytes of file) a poll, held back while
// the card queue's streams need the card (a few polls at most). Portable: the card is the recorder's Backend.
#pragma once
#include <stdint.h>

#include "loop/loop.h"
#include "recorder.h"

namespace loop_pi {

class Keep {
 public:
  static constexpr uint32_t kPiece = 30720;   // 5120 frames of 24-bit stereo
  // done: told when a job ends (the Pi: core::loop::keep_finished: the grab's kept flag and the pages' message)
  void init(const rec::Backend* b, void (*done)(uint32_t id, bool ok, const char* take)) { b_ = b, done_cb_ = done, state_ = 0; }
  bool busy() const { return state_ != 0; }
  bool begin(const core::loop::KeepJob& j);   // false: nothing written (the message says why)
  bool poll();                                // one piece; false: nothing more to do (done, failed or idle)
  const char* take() const { return take_; }
  const char* error() const { return err_; }
  uint32_t pieces() const { return pieces_; }   // written pieces (the test's count)
  uint32_t worst_bytes() const { return worst_; }

 private:
  bool fail(const char* why);
  void finish(bool ok);
  const rec::Backend* b_ = nullptr;
  void (*done_cb_)(uint32_t, bool, const char*) = nullptr;
  core::loop::KeepJob job_;
  int state_ = 0;   // 0 idle, 1 writing the audio, 2 closing
  int h_ = -1;
  bool own_dir_ = false;   // the take's folder was made by this job (a failure removes only that)
  uint32_t done_ = 0, pieces_ = 0, worst_ = 0, yields_ = 0;
  char take_[rec::kTakeMax] = "", path_[160] = "", err_[48] = "";
};

}  // namespace loop_pi
