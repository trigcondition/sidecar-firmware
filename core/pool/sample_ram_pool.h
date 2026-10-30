// The sample RAM (docs/psram.md): the project's pool samples held whole in memory the board gives
// (core::sample_ram_total / sample_ram_alloc: the Teensy's PSRAM, the Pi's heap), so DSP SAMPLE's
// playback modes (sample_modes.h, libraries/Sampler) can read them anywhere -
// slices, grains, a stretch, a wavetable scan. Loaded from the card between
// requests, one 8 KB read a call (step(), as the streams' refills; never
// inside a render), converted to int16 mono at the file's rate. A sample is
// loaded when a voice wants it (its SMP slot in a report) and, at a project
// change, every pool slot that fits in free space (no eviction); when full,
// the least recently used sample not playing makes room (sample_ram.h).
//
// Without memory from the board nothing here runs and DSP SAMPLE streams from the card.
//
// On the Pi and the Mac (CORE_MSAMP: the pools design 4.1) the samples live in the pool RAM's shared cache
// (core/pool/pcache.h): one copy a content, shared with GRANULAR and MULTISAMPLE; this class is DSP SAMPLE's view of it
// (keys -> the file's path -> its lib::Ref), the same interface; a pool line's file outside /Samples has the key
// kSlotKey - line (RAM only: no stream).
#pragma once
#include <Sampler.h>

#include "../platform.h"
#include "card_index.h"

namespace rytm {

class SampleRamPool {
 public:
  static const int kSlotKey = -100;               // (Pi / Mac) folder kSlotKey - line: pool line's file outside /Samples
  static const uint32_t kReserve = 4u << 20;      // PSRAM left for later uses (recording, the looper)
  static const uint32_t kMaxFrames = 4u << 20;    // a sample's frames in RAM at most (8 MB, 87 s
                                                  // at 48 kHz): a longer one, its first kMaxFrames
  static const uint32_t kChunk = 8192;            // a card read, as the streams' refills
  static const int kQueue = 32;
  static const int kVoices = 8;

  // At boot, after the card: the pool, itself in PSRAM (no RAM spent without
  // PSRAM), taking all of it but kReserve; null without PSRAM - and then
  // nothing of this runs, ever.
  static SampleRamPool* create(card::Index* cards, card::log_fn log);
  bool ok() const { return ok_; }
  float* pluck_line(int voice) { return ok_ ? pluck_ + voice * SampleModes::kPluckLen : nullptr; }

  // Folder/file's sample, if resident (touched: recently used); else false.
  // *cut: only its first kMaxFrames are in RAM. ch (optional): its channels
  // (2: interleaved L R, the Pi's pool RAM; always 1 elsewhere).
  bool get(int folder, int file, const int16_t** pcm, uint32_t* frames, float* rate, bool* cut, int* ch = nullptr);
  // Load folder/file: a voice wants it (first in line, may evict; a
  // prefetch loading meanwhile gives way), or a prefetch (behind the wants,
  // free space only).
  void want(int folder, int file, bool prefetch);
  // A new project: its prefetches replace the old (one loading is dropped),
  // failed samples may be tried again.
  void clear_prefetch();
  // One step of loading (an open, or one card read): false if nothing to do.
  bool step();
  // A voice plays this sample: it is not evicted while pinned.
  void pin(int folder, int file);
  void unpin(int folder, int file);

#if CORE_MSAMP
  uint32_t arena_bytes() const;   // the pool RAM's (MB-capped to 32 bits)
  uint32_t used_bytes() const;
#else
  uint32_t arena_bytes() const { return ram_.size(); }
  uint32_t used_bytes() const { return ram_.used(); }
#endif
  uint32_t loads() const { return loads_; }
  uint32_t evictions() const { return ram_.evictions(); }
  uint32_t max_step_us() const { return max_step_us_; }
  void reset_max() { max_step_us_ = 0; }

 private:
  bool begin(card::Index* cards, card::log_fn log, uint32_t total);
  static uint32_t key(int folder, int file) { return (uint32_t)folder << 16 | (uint16_t)file; }
  struct Meta {
    uint32_t key;
    int32_t id;   // SampleRam block, -1: free entry
    uint32_t frames;
    float rate;
    bool ready, cut;
  };
  Meta* meta(uint32_t k);
  Meta* meta_new(uint32_t k);
  void forget(uint32_t k);
  bool start(uint32_t k, bool prefetch);
  void finish();
  void abort_prefetch();
  void fail(uint32_t k);
  bool failed(uint32_t k) const;

  card::Index* cards_ = nullptr;
  card::log_fn log_ = nullptr;
  bool ok_ = false;
  SampleRam ram_;
  Meta meta_[SampleRam::kMax];
  uint32_t queue_[kQueue];
  bool queue_pre_[kQueue];
  int nq_ = 0;
  uint8_t* bounce_ = nullptr;   // a card read (in PSRAM: no RAM spent without it)
  float* pluck_ = nullptr;      // the voices' PLUCK delay lines
  // the load in progress
  bool busy_ = false, job_pre_ = false;
  uint32_t job_key_ = 0;
  static const int kFailed = 16;   // keys that cannot load (format, open, size):
  uint32_t failed_[kFailed];       // not retried until the next project
  int nfailed_ = 0;
  core::File file_;
  card::FileRec rec_;
  uint32_t bpf_ = 2, src_off_ = 0, out_frames_ = 0, want_frames_ = 0, carry_ = 0, t0_ = 0;
  int16_t* dst_ = nullptr;
  uint32_t loads_ = 0, max_step_us_ = 0;
  // CORE_MSAMP: the pins held (a key and the cache block it pinned)
  struct Pin {
    uint32_t key;
    int blk;
    bool on;
  };
  Pin pins_[2 * kVoices];
};

}  // namespace rytm
