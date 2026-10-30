// core/pool/pcache.h - the pool RAM: one decoded sample cache keyed by content hash (the pools design 4.1,
// docs/POOLS.md "RAM"), shared by GRANULAR (its card sources), DSP SAMPLE (its sample RAM) and MULTISAMPLE (its
// instruments' member samples). A file used as a GRANULAR source and as a MULTISAMPLE member is in RAM once.
//
//   entries  one per content: the lib::Ref's sha256 (a ref without a hash yet: its path and size), int16 at the file's
//            own rate in the file's channels (docs/STEREO.md): 1 mono, 2 stereo interleaved L R (a file with 3+
//            channels: its first two as L / R); poolmono=1 (set_mono) sums at load instead (one channel, as before
//            stereo samples). A mono reader of a stereo entry sums at read (mono_at: (L + R) / 2 truncated, the value
//            the summed load gave a 16-bit file, bit for bit). Either WHOLE (GRANULAR, DSP
//            SAMPLE, MULTISAMPLE's RAM mode; at most kMaxFrames, the rest cut) or a PREFIX (MULTISAMPLE's head; the
//            rest streams as before). A whole entry satisfies a prefix want; a whole want on a prefix grows it (a new
//            block, the old one kept while a pin holds it). One guard frame (ch samples) after the last.
//   memory   one arena of the budget (pool_ram_budget(): poolram=MB, default 1 GB on 2 / 4 GB boards, 3 GB on 8 GB;
//            the Mac CORE_POOL_RAM_MB, default 64), blocks 64-byte aligned in address order; a block that does not fit
//            evicts the least recently used window of unpinned entries next to a hole (never a compaction). A pin
//            (a voice / slot reading the block) keeps it; private blocks (a machine's own, e.g. MULTISAMPLE's loop
//            parts) are counted in the budget and never evicted.
//   loads    want() queues a whole load (wants before prefetches); step() does one card operation of it through the
//            card core (cardq: never on the control core's own time), converting a 32 KB read a step. fill_begin /
//            fill_end let a machine load an entry itself (MULTISAMPLE's heads).
//
// The control core's (core 3 on the Pi), except stats() / resident(): any core (racy, for drawing). Pi / Mac only.
#pragma once
#include <stdint.h>

#include "../lib/library.h"

namespace core {
namespace pcache {

constexpr int kEntries = 4096;
constexpr int kBlocks = 8192;
constexpr uint32_t kWhole = 0xFFFFFFFFu;
constexpr uint32_t kMaxFrames = 1u << 22;   // a whole entry's frames at most (87 s at 48 kHz; 8 MB mono, 16 MB stereo)
constexpr uint32_t kChunk = 32768;          // a load's card read

enum State : uint8_t { kNone = 0, kLoading, kReady, kFailed };
struct Info {
  const int16_t* pcm;   // (frames + 1) x ch int16 (a guard frame), interleaved L R when ch is 2
  uint32_t frames;      // resident
  uint32_t total;       // the file's frames (0 unknown)
  float rate;
  bool whole;           // every frame the entry will hold (cut: whole but shorter than the file)
  bool cut;
  int blk;              // the block (pin / unpin it)
  uint8_t ch;           // 1 mono, 2 stereo
  uint8_t fch;          // the file's channels (0 unknown: a machine's fill); a stereo file under poolmono: ch 1, fch 2
};
// Frame k of pcm (ch 1 or 2) as mono: a stereo frame's (L + R) / 2, truncated toward zero as the summed load was
inline int16_t mono_at(const int16_t* pcm, int ch, uint32_t k) {
  return ch == 2 ? (int16_t)(((int32_t)pcm[2 * k] + (int32_t)pcm[2 * k + 1]) / 2) : pcm[k];
}
// poolmono=1 (cmdline / console): every load from now on sums to one channel (the old layout, half the RAM for stereo
// files; a PRE/POST voice then plays them centred). Entries already loaded keep their channels.
void set_mono(bool on);
bool mono();

void begin(uint64_t budget_bytes);      // the arena (the first call; later calls change nothing)
bool ok();
// the entry for this content (-1 none); with min_frames, only when that much is resident (kWhole: whole)
int find(const lib::Ref& r, uint32_t min_frames = 0);
bool get(int e, Info* out);             // ready (prefix or whole)
State state(int e);
bool no_room(int e);                    // failed for want of room (else: a file it cannot read)
// A whole load of the file at path (the ref's resolved place) for this content: the entry (loading or ready), -1
// none (no room even after evicting what nothing pins, or failed before). prefetch: behind every want, free space only.
int want(const lib::Ref& r, const char* path, bool prefetch = false);
// A machine fills an entry itself (a prefix of frames frames, or the whole file when frames >= total): the block to
// write ((frames + 1) x ch int16, interleaved for ch 2); fill_end publishes what was written (frames). -1: no room. An
// entry with that much already: -1 and *have its id (use it instead; its Info::ch says its layout).
int fill_begin(const lib::Ref& r, uint32_t frames, uint32_t total, float rate, int16_t** dst, int* have, int ch = 1);
void fill_end(int e, uint32_t written, bool ok);
bool busy(int e);                       // a whole load waits or runs for the entry (queued, running, after a fill)
bool is(int e, const lib::Ref& r);      // the entry still holds this content (an id kept across steps: check it)
// pins: a reader holds the block (not evicted, not freed while it reads); the block id from Info::blk
void pin(int blk);
void unpin(int blk);
void touch(int e);                      // used now (LRU)
// a machine's own memory, counted in the budget, never evicted (-1 no room); freed by release
int alloc_private(uint64_t bytes, void** p);
void release(int blk);
void forget(const lib::Ref& r);         // an unloaded row: dropped now if nothing pins it (else when unpinned)
bool step();                            // one card operation of the load in progress (the control core's)

struct Stats {
  uint64_t budget, used, priv;          // bytes: the arena, its blocks in use (entries + private), private only
  uint32_t entries, ready, loads, fails, evictions, grows, shared_hits;
};
Stats stats();
// tests
void reset_for_tests();

}  // namespace pcache
}  // namespace core
