// pi/engine/stor_bench.h - the storage bench, `stor=bench[:VOL][:MB][:n=N][!]` (docs/STORAGE.md). Portable (no
// Circle; pi/engine/tests/test_stor_bench.cpp runs it on a temp folder): the phases, their math and the table; the
// Circle side (stor_pi.cpp) gives it the volume's files, the raw device and core 0's probes through Io.
//
// Phases, on core 0's loop, each step bounded (kStepUs of I/O, or one operation in the "busy" phases):
//   1. sequential: a test file of MB (default 256) written, synced, closed and read back in 64 KB, 256 KB and 1 MB
//      pieces (a piece is one f_write / f_read, or with split - stor_pi always - calls of <= 64 KB that never cross a
//      64 KB boundary: the xHCI TRB rule, stor_vol.h); every piece read is checked against the pattern written
//   2. random reads at QD1 in that file: N x 4 KB, N x 64 KB (aligned): p50 / p99 / max, IOPS
//   3. the raw device without FatFs (Circle's CDevice Seek/Read on emmc1 / umsdN / nvme1, under FatFs's volume lock):
//      64 KB sequential over the test file's size from the device's start, N x 4 KB random (read only)
//   4. "busy": N x 64 KB random reads one a pass while the Overbridge stream runs (if it does), then again with 64 KB
//      writes to a second file paced at kRecBytesPerSec (the recorder's rate): core 0's loop wake latency
//      (sleep1msMaxUs, kernel.cpp) and the output's underruns over each
// Then both files are removed and the table logged. Never run automatically; refused while the Rytm plays or a
// recording runs unless forced (refuse()).
#pragma once
#include <stdint.h>

namespace storbench {

constexpr int kSizes = 3;
extern const uint32_t kPiece[kSizes];			// 64 KB, 256 KB, 1 MB
constexpr uint32_t kBufBytes = 1u << 20;		// each of the two buffers (the largest piece), 64 KB-aligned
constexpr uint32_t kDefaultMB = 256, kMinMB = 8, kMaxMB = 8192;
constexpr int kDefaultN = 1000, kMinN = 20, kMaxN = 4000;
constexpr uint32_t kStepUs = 8000;			// a step's I/O on core 0 (phases 1-3), at least one operation
constexpr uint32_t kRecBytesPerSec = 2u << 20;		// the busy phase's writes (the recorder: 12 ch x 24-bit x 48 kHz = 1.7 MB/s)
constexpr uint64_t kSpare = 24ull << 20;		// free space left beside the test file (the busy phase's writes, margin)
constexpr uint64_t kBusyWriteMax = 16ull << 20;		// the busy phase's second file at most (inside kSpare)

struct Args
{
	int vol = 0;		// stor::Vol (default SD)
	uint32_t mb = kDefaultMB;
	int n = kDefaultN;
	bool force = false;	// a trailing '!'
};
// a: what follows "stor=bench" ("", ":USB", ":USB:64", ":NVME:1024:n=500!", ...). false: *pErr says why
bool parse (const char *a, Args *pArgs, const char **pErr);

struct Cond
{
	bool mounted = false, playing = false, recording = false, running = false, sdtest = false;
};
const char *refuse (const Args &a, const Cond &c);	// 0: it may start; else why not

struct Lat { uint32_t n = 0, p50 = 0, p99 = 0, max = 0, mean = 0, iops = 0; };	// microseconds; IOPS over the busy time
uint32_t pct (const uint32_t *pSorted, int n, int q);	// nearest rank: the q-th percentile of n sorted values
Lat lat_of (uint32_t *pUs, int n, uint64_t nBusyUs);	// sorts pUs
uint32_t mbps_x100 (uint64_t nBytes, uint64_t nUs);	// MB/s (1 MB = 2^20 bytes) x 100

struct Seq { uint32_t piece = 0, calls = 0, wmax = 0, rmax = 0, sync = 0; uint64_t wbytes = 0, wus = 0, rbytes = 0, rus = 0; };
struct Busy
{
	bool ran = false, streaming = false;
	Lat lat;
	uint32_t loop_max_us = 0, loop_avg_us = 0, underruns = 0;
	uint64_t wbytes = 0, wus = 0;
};
struct Results
{
	int vol = 0;
	bool split = false;
	uint64_t size = 0;
	uint32_t total_ms = 0;
	bool verify_ok = true;
	uint64_t verify_off = 0;	// the first mismatch
	Seq seq[kSizes];
	Lat rnd4, rnd64;
	bool raw = false;
	uint64_t raw_bytes = 0, raw_us = 0;
	Lat raw4;
	Busy busy[2];			// reads only; reads with the paced writes
	char link[40] = "", fs[8] = "";
};

struct Io
{
	bool (*open) (int nVol, int f, bool bWrite);	// f 0: the test file, 1: the busy phase's writes (created, truncated)
	int (*write) (int f, const void *p, uint32_t n);	// bytes written, -1 error
	int (*read_at) (int f, uint64_t nOff, void *p, uint32_t n);
	bool (*sync) (int f);
	void (*close) (int f);
	void (*remove) (int nVol, int f);
	int (*raw_read) (int nVol, uint64_t nOff, void *p, uint32_t n);	// 0: no raw access
	uint64_t (*raw_size) (int nVol);
	uint32_t (*now_us) (void);
	bool (*streaming) (void);			// the Overbridge stream runs
	uint32_t (*underruns) (void);			// the output's, since boot
	void (*loop_reset) (void);			// core 0's loop latency: from now
	void (*loop_read) (uint32_t *pMaxUs, uint32_t *pAvgUs);
	void (*log) (const char *pLine);		// one line of the table
	bool (*hold) (void);				// 0 or true: end this step now (the SD's streams need the card)
};
void set_io (const Io *pIo);
void set_buffers (uint8_t *pWrite, uint8_t *pRead);	// kBufBytes each, 64 KB-aligned (the caller's, once)

enum State : int { kIdle, kRunning, kDone, kFailed, kCancelled };
// 0: started; else why not (no buffers, too little free space). nFree 0: unknown. pLink / pFs: the table's header.
const char *start (const Args &a, bool bSplit, uint64_t nFree, const char *pLink, const char *pFs);
bool step (void);		// core 0's loop: true while it runs
void cancel (void);
State state (void);
int progress (void);		// 0..100
const char *phase (void);	// what runs, or the last failure
int vol (void);
bool have (void);		// a result to show
const Results &results (void);
int table (const Results &r, char *pOut, int nCap);	// the compact table, lines ended by '\n'
int json (char *pOut, int nCap);			// "bench":{...} (no leading comma)

}	// namespace storbench
