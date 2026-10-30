// core/pool/stream_budget.h - the streaming budget per volume (docs/STEREO.md "Streaming budget"; the stereo / POST
// design's addendum): how many sample streams of a format a volume carries with margin, from what was measured on it -
// the storage bench (`stor=bench`, pi/engine/stor_bench.h: sequential read MB/s, 64 KB random read p99) or, for the
// card, the SD card test (core/msamp/sdtest.h: its stereo 24-bit voice count) - else a cautious default per kind of
// drive. MULTISAMPLE asks it before it streams an instrument: a stereo instrument whose voices the volume cannot carry
// (fewer than kWantStreams of its format) is held whole in the pool RAM instead (forced RAM, when it fits: the
// instrument's RAM rule), shown as "RAM" in the pool lists and counted on /status. A pool row or a whole list marked
// "always in RAM" (pools.h set_ram) is held whole whatever the budget says.
//
// The model (the SD card test's, sdtest.cpp judge, per format): a stream of rate x channels x bytes reads kPiece bytes
// at a time; each read costs the volume's p99 latency (a cautious stand-in for its mean) plus the piece at its
// sequential rate; the streams may use kShare of the volume's time (the rest: the recorder, uploads, the library) and
// kShare of its throughput:
//   streams = min(kShare / (reads a second x latency), kShare x MB/s / (MB/s a stream))
// Pure and allocation-free; the control core's (set_bench from the Pi's bench, the console).
#pragma once
#include <stdint.h>

namespace core {
namespace sbudget {

constexpr int kVols = 5;                  // pi/engine/stor_vol.h: SD USB USB2 USB3 NVME
constexpr uint32_t kPiece = 32768;        // a streaming read (msamp kStreamChunk)
constexpr float kShare = 0.6f;            // of the volume's time and throughput (sdtest judge's)
constexpr int kWantStreams = 8;           // a stereo instrument streams when the volume carries this many of its voices

struct Bench {
  float read_mbps;                        // sequential reads, MB/s (2^20 bytes)
  float p99_ms;                           // 64 KB random reads, p99
  bool measured;                          // from a bench (false: the default for the kind of drive)
};
// The bench of volume v now: the measured one, else the SD card test's (v 0), else the default (cautious; inferred
// from docs/STORAGE.md's expectations and the SD card test's typical latencies: SD 12 MB/s 4 ms, a USB 2 stick 20 MB/s 6 ms,
// USB 3 80 MB/s 3 ms, NVMe 250 MB/s 1 ms)
Bench bench(int v);
void set_bench(int v, float read_mbps, float p99_ms);   // the Pi's stor=bench results (stor_pi.cpp)
void set_sd_voices(int voices);           // the SD card test's verdict (stereo 24-bit 48 kHz voices), 0 none
void clear();                             // tests
// Streams of rate / channels / bits volume v carries (0: none)
int streams(int v, uint32_t rate, int ch, int bits);
int streams_for(const Bench& b, uint32_t rate, int ch, int bits);   // the model alone (tests)
// The volume of a card path ("USB:/x.wav" -> 1, "/x.wav" or "SD:/x.wav" -> 0)
int vol_of(const char* path);
// A stream of this format from this path within the budget (kWantStreams of them)?
bool fits(const char* path, uint32_t rate, int ch, int bits);
// MULTISAMPLE's count: instruments forced into RAM by the budget (now)
void forced(int delta);
int forced_now();
// "stream" (the volumes' budgets for stereo / mono 24-bit 48 kHz), "stream=bench:VOL:MBPS:P99MS" (set by hand; tests)
bool command(const char* p, char* out, int cap);
int json(char* out, int cap);             // "\"stream\":{...}" (/status)

}  // namespace sbudget
}  // namespace core
