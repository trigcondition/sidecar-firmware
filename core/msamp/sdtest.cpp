// core/msamp/sdtest.cpp - see sdtest.h. The control core's, cold.
#include "../platform.h"

#if CORE_MSAMP
#include "sdtest.h"

#include <string.h>

#include "msamp.h"
#include "../pool/stream_budget.h"
#include "text.h"

namespace core {
namespace sdtest {

using msamp::to_double;
using msamp::to_int;

namespace {

const char* const kPaths[2] = {"/.rytm/sdtest.bin", "/.rytm/sdtest2.bin"};
const char* const kResult = "/.rytm/sdtest.txt";

// ---- the board's card
File files[2];
bool d_open(int w, bool write) {
  if (write) Fs().mkdir("/.rytm");
  return files[w].open(kPaths[w], write ? kWrite : kRead);
}
int d_read(int w, uint64_t off, void* b, uint32_t n) {
  if (!files[w].seekSet(off)) return -1;
  return files[w].read(b, n);
}
int d_write(int w, const void* b, uint32_t n) { return (int)files[w].write(b, n); }
void d_close(int w) { files[w].close(); }
void d_remove(int w) {
  Fs fs;
  if (fs.exists(kPaths[w])) fs.remove(kPaths[w]);
}
uint64_t d_free() { return card_free_bytes(); }
uint32_t d_now() { return now_us(); }
const Io kBoard = {d_open, d_read, d_write, d_close, d_remove, d_free, d_now};
const Io* io = &kBoard;

enum Phase : uint8_t { pWrite, pRead, pRand, pMixed, pClean };
struct Run {
  State state;
  Phase phase;
  uint64_t size, pos;
  uint32_t t0, busy_us;     // the phase's start; time inside its operations
  int sz, n, writes;
  uint64_t wbytes;
  uint32_t wus;
  float lat[kRandN + kMixedN];
  uint32_t rnd;
  bool open[2];
  char why[96];
};
Run run;
Measured meas;
Verdict ver;
bool have = false;
uint8_t* buf = nullptr;     // kRecChunk

void close_all() {
  for (int w = 0; w < 2; ++w)
    if (run.open[w]) io->close(w), run.open[w] = false;
}
void clean() {
  close_all();
  io->remove(0);
  io->remove(1);
}
void fail(const char* why) {
  clean();
  run.state = kFailed;
  strncpy(run.why, why, sizeof run.why - 1);
  log(CORE_STR("sdtest: failed: %s"), why);
}
uint32_t rnd() {
  run.rnd = run.rnd * 1664525u + 1013904223u;
  return run.rnd >> 4;
}
Lat stats_of(float* v, int n, uint32_t busy_us, uint32_t bytes_each) {
  for (int i = 1; i < n; ++i)
    for (int j = i; j > 0 && v[j - 1] > v[j]; --j) {
      float t = v[j];
      v[j] = v[j - 1], v[j - 1] = t;
    }
  Lat l = {};
  if (!n) return l;
  double s = 0;
  for (int i = 0; i < n; ++i) s += v[i];
  l.mean_ms = (float)(s / n);
  l.p50_ms = v[n / 2], l.p99_ms = v[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1], l.max_ms = v[n - 1];
  l.iops = busy_us ? n * 1e6f / busy_us : 0;
  (void)bytes_each;
  return l;
}
// timed operation: microseconds
uint32_t timed_read(int w, uint64_t off, uint32_t n, bool* ok) {
  uint32_t t = io->now_us();
  int got = io->read(w, off, buf, n);
  uint32_t dt = io->now_us() - t;
  *ok = got == (int)n;
  return dt;
}
uint32_t timed_write(int w, uint32_t n, bool* ok) {
  uint32_t t = io->now_us();
  int got = io->write(w, buf, n);
  uint32_t dt = io->now_us() - t;
  *ok = got == (int)n;
  return dt;
}

void save() {
  File f;
  Fs().mkdir("/.rytm");
  if (!f.open(kResult, kWrite)) return;
  char t[700];
  int n = sniprintf(t, sizeof t,
                    "# the SD card test (core/msamp/sdtest.h)\nwrite_mbps=%d.%02d\nread_mbps=%d.%02d\n"
                    "rand4k_iops=%d\nrand32k_iops=%d\nrand32k_p99_ms=%d\nrand32k_max_ms=%d\nrand64k_max_ms=%d\n"
                    "mixed32k_p99_ms=%d\nmixed32k_max_ms=%d\nwrite_max_ms=%d\nvoices=%d\nvoices_rec=%d\nlevel=%d\n"
                    "ahead_ms=%lu\nstreams=%d\nverdict=%s\n",
                    (int)meas.write_mbps, (int)(meas.write_mbps * 100) % 100, (int)meas.read_mbps,
                    (int)(meas.read_mbps * 100) % 100, (int)meas.rand[0].iops, (int)meas.rand[1].iops,
                    (int)meas.rand[1].p99_ms, (int)meas.rand[1].max_ms, (int)meas.rand[2].max_ms, (int)meas.mixed.p99_ms,
                    (int)meas.mixed.max_ms, (int)meas.write_max_ms, ver.voices, ver.voices_rec, ver.level,
                    (unsigned long)ver.ahead_ms, ver.streams, ver.text);
  f.write(t, n);
  f.close();
}

const char* kv(const char* t, const char* k) {
  const char* p = strstr(t, k);
  return p ? p + strlen(k) : nullptr;
}

}  // namespace

Verdict judge(const Measured& m) {
  Verdict v = {};
  const float kVoiceMBps = 0.288f, kReadsPerVoice = 9.0f, kShare = 0.6f, kRecMBps = 1.97f;
  float mean32 = m.rand[1].mean_ms > 0 ? m.rand[1].mean_ms : 1000.0f;
  float n_lat = kShare / (kReadsPerVoice * mean32 / 1000.0f), n_tp = kShare * m.read_mbps / kVoiceMBps;
  float n = n_lat < n_tp ? n_lat : n_tp;
  float rec_frac = m.write_mbps > 0 ? kRecMBps / m.write_mbps : 1.0f;
  float mm = m.mixed.mean_ms > 0 ? m.mixed.mean_ms : mean32;
  float r_lat = (kShare - rec_frac) / (kReadsPerVoice * mm / 1000.0f), r_tp = (kShare - rec_frac) * m.read_mbps / kVoiceMBps;
  float nr = r_lat < r_tp ? r_lat : r_tp;
  v.voices = n < 0 ? 0 : n > 999 ? 999 : (int)n;
  v.voices_rec = nr < 0 ? 0 : nr > 999 ? 999 : (int)nr;
  float worst = m.rand[1].max_ms > m.rand[2].max_ms ? m.rand[1].max_ms : m.rand[2].max_ms;
  float worst_rec = m.mixed.max_ms > worst ? m.mixed.max_ms : worst;
  bool cover = worst * 1.5f + 30 <= 600, cover_rec = worst_rec * 1.5f + 30 <= 600;
  v.level = v.voices_rec >= kMinVoices && cover_rec ? 2 : v.voices >= kMinVoices && cover ? 1 : 0;
  float a = worst_rec * 1.5f + 30;   // the recorder's stalls covered whenever they can be (it may start any time)
  v.ahead_ms = (uint32_t)(a < 100 ? 100 : a > 600 ? 600 : a);
  int s = v.level == 2 ? v.voices_rec : v.voices;
  v.streams = s < 1 ? 1 : s > msamp::kTotal ? msamp::kTotal : s;
  if (v.level == 2)
    sniprintf(v.text, sizeof v.text, "OK for %d stereo streaming voices + recorder (%d without), read-ahead %lu ms",
              v.voices_rec, v.voices, (unsigned long)v.ahead_ms);
  else if (v.level == 1)
    sniprintf(v.text, sizeof v.text, "OK for %d stereo streaming voices without the recorder (%d with it), read-ahead %lu ms",
              v.voices, v.voices_rec, (unsigned long)v.ahead_ms);
  else
    sniprintf(v.text, sizeof v.text, "too slow for streaming (%d voices): use RAM mode or a faster card (A1/A2, U3/V30)",
              v.voices);
  return v;
}

void set_io(const Io* x) { io = x ? x : &kBoard; }

CORE_COLD bool start(uint32_t max_mb) {
  if (run.state == kRunning) return false;
  if (!buf) {
    buf = new uint8_t[kRecChunk];
    uint32_t r = 12345;
    for (uint32_t i = 0; i < kRecChunk; ++i) r = r * 1664525u + 1013904223u, buf[i] = (uint8_t)(r >> 24);
  }
  memset(&run, 0, sizeof run);
  memset(&meas, 0, sizeof meas);
  run.rnd = io->now_us() | 1;
  io->remove(0);
  io->remove(1);
  uint64_t fr = io->free_bytes();
  if (fr && fr < (64ull << 20)) {
    run.state = kFailed;
    sniprintf(run.why, sizeof run.why, "only %lu MB free (64 MB needed)", (unsigned long)(fr >> 20));
    log(CORE_STR("sdtest: %s"), run.why);
    return false;
  }
  uint64_t size = (uint64_t)max_mb << 20;
  if (fr && size > fr / 4) size = fr / 4;
  if (!fr) size = size < (64ull << 20) ? size : (64ull << 20);
  if (size < (32ull << 20)) size = 32ull << 20;
  run.size = size & ~(uint64_t)(kSeqChunk - 1);
  if (!io->open(0, true)) {
    fail("cannot create /.rytm/sdtest.bin");
    return false;
  }
  run.open[0] = true;
  run.state = kRunning;
  run.phase = pWrite;
  run.t0 = io->now_us();
  strcpy(run.why, "sequential write");
  log(CORE_STR("sdtest: started: %lu MB test file (%lu MB free)"), (unsigned long)(run.size >> 20),
      (unsigned long)(fr >> 20));
  return true;
}

CORE_COLD void cancel() {
  if (run.state != kRunning) return;
  clean();
  run.state = kCancelled;
  strcpy(run.why, "cancelled");
  log(CORE_STR("sdtest: cancelled"));
}

CORE_COLD bool step() {
  if (run.state != kRunning) return false;
  bool ok = true;
  switch (run.phase) {
    case pWrite: {
      uint32_t dt = timed_write(0, kSeqChunk, &ok);
      if (!ok) return fail("a write failed (card full?)"), false;
      run.busy_us += dt;
      float ms = dt / 1000.0f;
      if (ms > meas.write_max_ms) meas.write_max_ms = ms;
      run.pos += kSeqChunk;
      if (run.pos < run.size) return true;
      uint32_t t = io->now_us();
      io->close(0);   // its flush counts
      run.open[0] = false;
      run.busy_us += io->now_us() - t;
      meas.write_mbps = run.busy_us ? (float)(run.size / 1048576.0 * 1e6 / run.busy_us) : 0;
      if (!io->open(0, false)) return fail("cannot read the test file back"), false;
      run.open[0] = true;
      run.phase = pRead, run.pos = 0, run.busy_us = 0;
      strcpy(run.why, "sequential read");
      return true;
    }
    case pRead: {
      run.busy_us += timed_read(0, run.pos, kSeqChunk, &ok);
      if (!ok) return fail("a read failed"), false;
      run.pos += kSeqChunk;
      if (run.pos < run.size) return true;
      meas.read_mbps = run.busy_us ? (float)(run.size / 1048576.0 * 1e6 / run.busy_us) : 0;
      run.phase = pRand, run.sz = 0, run.n = 0, run.busy_us = 0;
      strcpy(run.why, "random reads");
      return true;
    }
    case pRand: {
      uint32_t c = kRandSize[run.sz];
      uint64_t blocks = (run.size - c) / 4096;
      uint64_t off = (rnd() % (uint32_t)(blocks ? blocks : 1)) * 4096;
      uint32_t dt = timed_read(0, off, c, &ok);
      if (!ok) return fail("a random read failed"), false;
      run.busy_us += dt;
      run.lat[run.n++] = dt / 1000.0f;
      if (run.n < kRandN) return true;
      meas.rand[run.sz] = stats_of(run.lat, run.n, run.busy_us, c);
      run.n = 0, run.busy_us = 0;
      if (++run.sz < kSizes) return true;
      if (!io->open(1, true)) return fail("cannot create /.rytm/sdtest2.bin"), false;
      run.open[1] = true;
      run.phase = pMixed, run.writes = 0, run.wbytes = 0, run.wus = 0;
      strcpy(run.why, "reads while writing");
      return true;
    }
    case pMixed: {
      if (run.n % 4 == 0 && run.writes <= run.n / 4) {   // the recorder's write
        uint32_t dt = timed_write(1, kRecChunk, &ok);
        if (!ok) return fail("a write failed (card full?)"), false;
        ++run.writes, run.wbytes += kRecChunk, run.wus += dt;
        return true;
      }
      uint64_t blocks = (run.size - 32768) / 4096;
      uint64_t off = (rnd() % (uint32_t)(blocks ? blocks : 1)) * 4096;
      uint32_t dt = timed_read(0, off, 32768, &ok);
      if (!ok) return fail("a random read failed"), false;
      run.busy_us += dt;
      run.lat[run.n++] = dt / 1000.0f;
      if (run.n < kMixedN) return true;
      meas.mixed = stats_of(run.lat, run.n, run.busy_us, 32768);
      meas.mixed_write_mbps = run.wus ? (float)(run.wbytes / 1048576.0 * 1e6 / run.wus) : 0;
      run.phase = pClean;
      strcpy(run.why, "cleaning up");
      return true;
    }
    case pClean: {
      clean();
      ver = judge(meas);
      have = true;
      sbudget::set_sd_voices(ver.voices);   // the card's streaming budget (core/pool/stream_budget.h)
      save();
      msamp::set_stream_limits(ver.ahead_ms, ver.streams);
      run.state = kDone;
      strcpy(run.why, "done");
      log(CORE_STR("sdtest: write %d.%d MB/s, read %d.%d MB/s, 32 KB reads %d IOPS p99 %d ms worst %d ms, with writes "
                   "worst %d ms: %s"),
          (int)meas.write_mbps, (int)(meas.write_mbps * 10) % 10, (int)meas.read_mbps, (int)(meas.read_mbps * 10) % 10,
          (int)meas.rand[1].iops, (int)meas.rand[1].p99_ms, (int)meas.rand[1].max_ms, (int)meas.mixed.max_ms, ver.text);
      return false;
    }
  }
  return false;
}

State state() { return run.state; }
int progress() {
  if (run.state == kDone) return 100;
  if (run.state != kRunning) return 0;
  switch (run.phase) {
    case pWrite: return (int)(40 * run.pos / (run.size ? run.size : 1));
    case pRead: return 40 + (int)(25 * run.pos / (run.size ? run.size : 1));
    case pRand: return 65 + (15 * (run.sz * kRandN + run.n)) / (kSizes * kRandN);
    case pMixed: return 80 + 15 * run.n / kMixedN;
    default: return 95;
  }
}
const char* why() { return run.why; }
const Measured& measured() { return meas; }
const Verdict& verdict() { return ver; }

CORE_COLD void load() {
  File f;
  if (!f.open(kResult, kRead)) return;
  char t[700];
  int n = f.read(t, sizeof t - 1);
  f.close();
  if (n <= 0) return;
  t[n] = 0;
  const char* p;
  const char* e_;
  if ((p = kv(t, "voices="))) ver.voices = to_int(p);
  if ((p = kv(t, "voices_rec="))) ver.voices_rec = to_int(p);
  if ((p = kv(t, "level="))) ver.level = to_int(p);
  if ((p = kv(t, "ahead_ms="))) ver.ahead_ms = (uint32_t)to_int(p);
  if ((p = kv(t, "streams="))) ver.streams = to_int(p);
  if ((p = kv(t, "read_mbps="))) meas.read_mbps = (float)to_double(p, &e_);
  if ((p = kv(t, "write_mbps="))) meas.write_mbps = (float)to_double(p, &e_);
  if ((p = kv(t, "verdict="))) {
    int i = 0;
    while (p[i] && p[i] != '\n' && i < (int)sizeof ver.text - 1) ver.text[i] = p[i], ++i;
    ver.text[i] = 0;
  }
  have = ver.ahead_ms > 0;
  if (have) sbudget::set_sd_voices(ver.voices);
}

int json(char* o, int cap) {
  static const char* const kState[] = {"idle", "running", "done", "failed", "cancelled"};
  return sniprintf(o, cap,
                   "{\"state\":\"%s\",\"progress\":%d,\"phase\":\"%s\",\"have\":%d,\"level\":%d,\"voices\":%d,"
                   "\"voicesRec\":%d,\"aheadMs\":%lu,\"streams\":%d,\"writeMBps\":%d.%02d,\"readMBps\":%d.%02d,"
                   "\"rand32Iops\":%d,\"rand32P99Ms\":%d,\"rand32MaxMs\":%d,\"mixedMaxMs\":%d,\"verdict\":\"%s\"}",
                   kState[run.state], progress(), run.why, have ? 1 : 0, ver.level, ver.voices, ver.voices_rec,
                   (unsigned long)ver.ahead_ms, ver.streams, (int)meas.write_mbps, (int)(meas.write_mbps * 100) % 100,
                   (int)meas.read_mbps, (int)(meas.read_mbps * 100) % 100, (int)meas.rand[1].iops,
                   (int)meas.rand[1].p99_ms, (int)meas.rand[1].max_ms, (int)meas.mixed.max_ms, have ? ver.text : "");
}

const char* line() {
  static char b[16];   // 8 characters at most (40 px on the SYSTEM page)
  int n = ver.level == 2 ? ver.voices_rec : ver.voices;
  if (run.state == kRunning) sniprintf(b, sizeof b, "SD %d%%", progress());
  else if (!have) sniprintf(b, sizeof b, "SD -");
  else if (ver.level == 2) sniprintf(b, sizeof b, "SD OK %d", n > 99 ? 99 : n);
  else if (ver.level == 1) sniprintf(b, sizeof b, "SD %d NR", n > 99 ? 99 : n);
  else sniprintf(b, sizeof b, "SD SLOW");
  return b;
}

CORE_COLD bool command(const char* p, bool busy, char* out, int cap) {
  if (strncmp(p, "sdtest", 6) != 0 || (p[6] && p[6] != '=')) return false;
  const char* a = p[6] ? p + 7 : "";
  if (!strcmp(a, "start") || !strcmp(a, "start!")) {
    msamp::Stats s = msamp::stats();
    if ((busy || s.active) && a[5] != '!') {
      sniprintf(out, cap, "sdtest: notes are playing (or a take records): sdtest=start! runs it anyway (audio may drop)");
      return true;
    }
    if (start()) sniprintf(out, cap, "sdtest: running (sdtest=cancel stops it)");
    else sniprintf(out, cap, "sdtest: %s", run.state == kRunning ? "already running" : run.why);
    return true;
  }
  if (!strcmp(a, "cancel")) {
    cancel();
    sniprintf(out, cap, "sdtest: %s", run.why[0] ? run.why : "not running");
    return true;
  }
  if (*a) return false;
  if (run.state == kRunning) sniprintf(out, cap, "sdtest: %s, %d %%", run.why, progress());
  else if (have)
    sniprintf(out, cap, "sdtest: write %d.%d MB/s, read %d.%d MB/s: %s", (int)meas.write_mbps,
              (int)(meas.write_mbps * 10) % 10, (int)meas.read_mbps, (int)(meas.read_mbps * 10) % 10, ver.text);
  else sniprintf(out, cap, "sdtest: not run yet (sdtest=start)%s%s", run.why[0] ? "; last: " : "", run.why);
  return true;
}

}  // namespace sdtest
}  // namespace core
#endif
