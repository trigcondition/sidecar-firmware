// core/samp/preview.cpp - see preview.h.
#include "preview.h"

#if CORE_MSAMP

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include "../fmt/fmt.h"
#include "../msamp/cardq.h"
#include "../wavetable/wav_read.h"

namespace core {
namespace preview {

namespace {
int16_t* g_buf = nullptr;           // L R int16 at the file's rate, kBufFrames frames (the board's high heap, begin())
uint8_t g_stage[cardq::kPiece];     // a read's bytes (the head, then the data's pieces)

// shared: written by the control core, read by the audio side (g_done / g_seen the other way)
uint32_t g_gen = 0;      // the preview to play (0 none); a new one: the audio starts it from its first frame
uint32_t g_filled = 0;   // frames of g_gen in g_buf
uint32_t g_end = 0;      // g_gen's last frame + 1 once loaded whole (0: still loading)
uint32_t g_inc = 0;      // the file's rate / kOutRate, 16.16
uint32_t g_stop = 0;     // g_gen to fade out
uint32_t g_done = 0;     // the audio side's: the gen it finished (faded out or ended)
uint32_t g_seen = 0;     // the audio side's: the gen it has taken up (it reads no older gen's frames from then on)
Stats g_stats{};
float g_level = 0.0f;    // PREVIEW LEVEL's gain (set_level_db; the audio side ramps to it, g_lvl)

// the audio side's own
struct Voice {
  uint32_t gen = 0, pos = 0, frac = 0, ramp = 0;
  bool started = false, held = false;
};
Voice g_v;
float g_lvl = -1.0f;     // the gain it last played at (-1: none yet, the level as set)

// the control side's
enum Phase : uint8_t { kPIdle, kPClose, kPFade, kPOpen, kPHead, kPScan, kPLoad };
constexpr uint32_t kQuietMs = 50;    // no answer from the audio side this long: none is running (the stream down)
constexpr uint32_t kScanMax = 64;    // the head's windows walked for a data chunk at most (4 MB of chunks)
struct Job {
  Phase ph = kPIdle;
  File f;
  bool f_open = false;
  cardq::Req rq{};
  bool abort = false;
  bool has_next = false, next_play = false;
  char next[sizeof(Status::path)] = "";
  wavread::Info a{};
  uint32_t want = 0, gen_pub = 0, fade_ms = 0, pub_ms = 0, stop_ms = 0, dur_ms = 0;
  bool stopped = false;              // gen_pub asked to fade out (at stop_ms)
  uint32_t counter = 0;              // the last gen given out
  // a WAV whose chunks before 'data' are longer than the head (bext, iXML, JUNK): walked a window at a time
  uint8_t fmt[48];
  uint32_t fmt_len = 0, scan = 0, scans = 0;
};
Job j;
Status g_st{};
// PREVIEW LEVEL, a Sidecar setting (the control side's)
const char kFile[] = "/.rytm/preview.txt";
int g_level_db = kLevelDefDb;
bool g_dirty = false;
fmt::KvKeep g_keep;   // the lines a newer build wrote that this one does not read (fmt.h, class S: kept)

inline uint32_t ld(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void st(uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

inline int32_t add_sat(int32_t a, float x) {
  float s = (float)a + x;
  return s > 2147483520.0f ? 2147483520 : s < -2147483648.0f ? INT32_MIN : (int32_t)s;
}

// One card request of the phase: true with its result; false while it is not done - *did then says whether this call
// posted it (work done) or only found it waiting (none: the idle chain goes on, core/engine.cpp idle_stream)
CORE_COLD bool io(cardq::Kind kind, const char* path, uint64_t off, uint8_t* buf, uint32_t len, bool* did) {
  bool was = cardq::idle(&j.rq);
  if (cardq::async(&j.rq, kind, &j.f, path, off, buf, len)) {
    j.rq.state = cardq::kFree;
    *did = true;
    return true;
  }
  *did = was && !cardq::idle(&j.rq);
  return false;
}
CORE_COLD void fail() {
  g_st.st = kFailed;
  j.ph = kPIdle;   // (closes the file)
}
CORE_COLD void take_next() {
  j.has_next = false, j.abort = false;
  memset(&g_st, 0, sizeof g_st);
  g_st.st = kOpening, g_st.play = j.next_play;
  memcpy(g_st.path, j.next, sizeof g_st.path);
  j.gen_pub = 0, j.stopped = false, j.fmt_len = 0, j.scans = 0;
  j.ph = kPOpen;
}
// gen_pub still sounds: the audio side has not finished it, and it has not timed out without one - asked to stop
// kQuietMs ago, or past its own length (no audio side running: the Overbridge stream down, a port without mix())
CORE_COLD bool sounding_gen() {
  if (!j.gen_pub || ld(&g_done) == j.gen_pub) return false;
  uint32_t now = now_ms();
  if (j.stopped && now - j.stop_ms >= kQuietMs) return false;
  if (g_st.st == kDone && now - j.pub_ms >= j.dur_ms + 1000) return false;
  return true;
}
CORE_COLD void stop_gen() {
  if (!j.gen_pub) return;
  st(&g_stop, j.gen_pub);
  if (!j.stopped) j.stopped = true, j.stop_ms = now_ms();
}
CORE_COLD int begin_job(const char* path, bool play) {
  if (!g_buf || !path || path[0] != '/' || strlen(path) >= sizeof j.next) return -1;
  memcpy(j.next, path, strlen(path) + 1);
  j.has_next = true, j.next_play = play;
  if (j.ph != kPIdle && j.ph != kPClose && j.ph != kPFade) j.abort = true;   // (the request in flight finished first)
  stop_gen();
  g_st.st = kOpening, g_st.play = play;   // (the status is the new one's from here)
  memcpy(g_st.path, j.next, sizeof g_st.path);
  return 0;
}
// a WAV's chunks walked from j.scan in the window b (file offset base, n bytes): the 'fmt ' chunk kept, true at 'data'
// (*data its header's offset, *len its length); false: read on from j.scan (or nothing more: j.scan >= the size)
CORE_COLD bool walk(const uint8_t* b, uint32_t base, uint32_t n, uint32_t* data, uint32_t* len) {
  while (j.scan >= base && j.scan + 8 <= base + n) {
    const uint8_t* c = b + (j.scan - base);
    uint32_t l = wavread::le32(c + 4);
    if (!memcmp(c, "fmt ", 4)) {
      if (j.scan + 8 + 16 > base + n) return false;   // (its body in the next window)
      j.fmt_len = l < sizeof j.fmt ? l : (uint32_t)sizeof j.fmt;
      if (j.scan + 8 + j.fmt_len > base + n) j.fmt_len = base + n - j.scan - 8;
      memcpy(j.fmt, c + 8, j.fmt_len);
    } else if (!memcmp(c, "data", 4)) {
      *data = j.scan, *len = l;
      return true;
    }
    uint64_t next = (uint64_t)j.scan + 8 + l + (l & 1);
    if (next > 0xFFFFFFFFu) {
      j.scan = 0xFFFFFFFFu;
      return false;
    }
    j.scan = (uint32_t)next;
  }
  return false;
}
// the head parsed (the file's first window, or a synthetic RIFF of the walked fmt + data headers); false: not audio
CORE_COLD bool headed() {
  g_st.rate = j.a.rate, g_st.frames = j.a.frames, g_st.ch = j.a.ch, g_st.bits = j.a.bits;
  uint32_t fb = wavread::frame_bytes(j.a);
  if (!j.a.frames || !fb || fb > 4096 || j.a.ch > 32) return false;   // (a frame over the read piece: refused)
  if (!g_st.play) {
    g_st.st = kDone;
    j.ph = kPIdle;
    return true;
  }
  uint32_t want = j.a.frames, cap = kMaxSeconds * j.a.rate;
  if (want > cap) want = cap;
  if (want > kBufFrames) want = kBufFrames;
  j.want = want;
  j.dur_ms = (uint32_t)((uint64_t)want * 1000 / j.a.rate);
  st(&g_filled, 0), st(&g_end, 0);
  st(&g_inc, (uint32_t)(((uint64_t)j.a.rate << 16) / kOutRate));
  j.gen_pub = ++j.counter ? j.counter : ++j.counter;
  j.pub_ms = now_ms(), j.stopped = false;
  st(&g_gen, j.gen_pub);
  ++g_stats.starts;
  g_st.st = kLoading;
  j.ph = kPLoad;
  return true;
}
}  // namespace

CORE_COLD void set_level_db(int db) {
  db = db < kLevelMinDb ? kLevelMinDb : db > kLevelMaxDb ? kLevelMaxDb : db;
  if (db != g_level_db) g_dirty = true;
  g_level_db = db;
  float g = powf(10.0f, (float)db / 20.0f);
  __atomic_store(&g_level, &g, __ATOMIC_RELAXED);
}
CORE_COLD int level_db() { return g_level_db; }
CORE_COLD float level() {
  float g;
  __atomic_load(&g_level, &g, __ATOMIC_RELAXED);
  return g;
}
CORE_COLD void load() {
  g_keep.n = 0;
  fmt::post_wait(kFile);   // a posted save of it lands before we read it back (fmt.h)
  fmt::recover(kFile);     // a power cut between write_atomic's renames: the previous file back
  File f;
  if (!f.open(kFile, kRead)) return;
  static char b[512];
  memset(b, 0, sizeof b);
  int got = f.read(b, sizeof b - 1);
  f.close();
  static const char* const kKnown[] = {"level", nullptr};
  if (got > 0) fmt::kv_keep(b, (uint32_t)got, kKnown, &g_keep);
  // "level=<dB>" at a line's start (not the tail of a longer key); an empty or corrupt value keeps the default
  for (const char* l = b; l && *l; l = strchr(l, '\n'), l = l ? l + 1 : nullptr) {
    if (strncmp(l, "level=", 6)) continue;
    // parsed by hand: the Pi's bare-metal build has no strtol
    const char* e = l + 6;
    bool neg = *e == '-';
    if (neg || *e == '+') e++;
    const char* d = e;
    long v = 0;
    while (*e >= '0' && *e <= '9' && v < 100000) v = v * 10 + (*e++ - '0');
    if (neg) v = -v;
    if (e != d && (*e == 0 || *e == '\n' || *e == '\r')) set_level_db(v < -999 ? -999 : v > 999 ? 999 : (int)v);
    break;
  }
  g_dirty = false;
}
CORE_COLD bool save() {
  if (!g_dirty) return false;
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
  uint32_t n = (uint32_t)sniprintf(t, 32, CORE_STR("level=%d\n"), g_level_db);
  n = fmt::kv_append(g_keep, t, n, fmt::kPostMax);
  if (!fmt::post_end(kFile, n)) return false;
  g_dirty = false;
  return true;
}

CORE_COLD void begin() {
  set_level_db(g_level_db);
  if (!g_buf) g_buf = (int16_t*)msamp_ram_alloc((uint64_t)kBufFrames * 2 * sizeof(int16_t));
  if (!g_buf) log(CORE_STR("preview: no buffer (sample RAM arena full): previews off"));
}
CORE_COLD int start(const char* path) { return begin_job(path, true); }
CORE_COLD int info(const char* path) { return begin_job(path, false); }

CORE_COLD void stop() {
  j.has_next = false;
  if (j.ph == kPOpen || j.ph == kPHead || j.ph == kPScan || j.ph == kPLoad) j.abort = true;
  stop_gen();
  if (g_st.st != kFailed) g_st.st = kIdle;
}

CORE_COLD bool sounding() {
  if (!g_st.play || g_st.st == kFailed) return false;
  if (!j.gen_pub) return g_st.st != kIdle && g_st.st != kDone;
  return sounding_gen();
}

CORE_COLD bool step() {
  bool did = false;
  switch (j.ph) {
    case kPIdle:
      if (j.f_open) {
        j.ph = kPClose;
        return true;
      }
      if (!j.has_next) return false;
      if (sounding_gen()) {   // the one sounding fades out before its buffer is filled again
        stop_gen();
        j.ph = kPFade, j.fade_ms = now_ms();
        return true;
      }
      take_next();
      return true;
    case kPClose:
      if (!io(cardq::kCloseReq, nullptr, 0, nullptr, 0, &did)) return did;
      j.f_open = false;
      j.ph = kPIdle;
      return true;
    case kPFade:
      // the audio's answer, or no audio running (the Rytm's stream down): a few blocks at most
      if (sounding_gen() && now_ms() - j.fade_ms < kQuietMs) return false;
      if (j.has_next) take_next();
      else j.ph = kPIdle;
      return true;
    case kPOpen: {
      if (!io(cardq::kOpenReq, g_st.path, 0, nullptr, 0, &did)) return did;
      if (j.rq.got == 1) j.f_open = true;
      if (j.abort) return j.ph = kPIdle, true;
      if (j.rq.got != 1) return fail(), true;
      g_st.size = (uint32_t)j.f.fileSize();
      g_st.st = kHeader;
      j.ph = kPHead;
      return true;
    }
    case kPHead: {
      uint32_t n = g_st.size < sizeof g_stage ? g_st.size : (uint32_t)sizeof g_stage;
      if (!io(cardq::kReadReq, nullptr, 0, g_stage, n, &did)) return did;
      if (j.abort) return j.ph = kPIdle, true;
      uint32_t got = j.rq.got > 0 ? (uint32_t)j.rq.got : 0;
      if (got && wavread::parse(g_stage, got, &j.a, g_st.size)) {
        if (!headed()) fail();
        return true;
      }
      // a RIFF WAVE whose data chunk lies past the head: its chunks walked on, a window at a time
      if (got < 12 || memcmp(g_stage, "RIFF", 4) || memcmp(g_stage + 8, "WAVE", 4) || g_st.size <= got) return fail(), true;
      j.scan = 12;
      uint32_t d, l;
      if (walk(g_stage, 0, got, &d, &l)) return fail(), true;   // (a data chunk there and still no parse: not one it reads)
      j.ph = kPScan;
      return true;
    }
    case kPScan: {
      if (j.scan + 8 > g_st.size || ++j.scans > kScanMax) return fail(), true;
      uint32_t n = g_st.size - j.scan < sizeof g_stage ? g_st.size - j.scan : (uint32_t)sizeof g_stage;
      if (!io(cardq::kReadReq, nullptr, j.scan, g_stage, n, &did)) return did;
      if (j.abort) return j.ph = kPIdle, true;
      uint32_t got = j.rq.got > 0 ? (uint32_t)j.rq.got : 0, base = j.scan, d = 0, l = 0;
      if (got < 8) return fail(), true;
      if (!walk(g_stage, base, got, &d, &l)) return true;   // (on from j.scan at the next step)
      if (j.fmt_len < 16) return fail(), true;
      // a RIFF of the fmt and the data header alone, parsed as the file's head; the data's place the file's own
      uint8_t* h = g_stage;   // (the window is done with)
      uint32_t k = 0, have = g_st.size - (d + 8);
      if (l > have) l = have;
      memcpy(h + k, "RIFF\0\0\0\0WAVEfmt ", 16), k += 16;
      h[k++] = (uint8_t)j.fmt_len, h[k++] = 0, h[k++] = 0, h[k++] = 0;
      memcpy(h + k, j.fmt, j.fmt_len), k += j.fmt_len;
      memcpy(h + k, "data", 4), k += 4;
      for (int i = 0; i < 4; ++i) h[k++] = (uint8_t)(l >> (8 * i));
      if (!wavread::parse(h, k, &j.a, k + l)) return fail(), true;
      j.a.data_off = d + 8;
      if (!headed()) fail();
      return true;
    }
    case kPLoad: {
      // L2: nothing written over the buffer until the audio side has taken this gen up (it reads the old one's
      // frames until then), or no audio side runs
      if (!g_st.loaded && ld(&g_seen) != j.gen_pub && now_ms() - j.pub_ms < kQuietMs) return false;
      uint32_t fb = wavread::frame_bytes(j.a), per = (uint32_t)sizeof g_stage / fb, left = j.want - g_st.loaded;
      uint32_t n = (left < per ? left : per) * fb;
      if (!io(cardq::kReadReq, nullptr, (uint64_t)j.a.data_off + (uint64_t)g_st.loaded * fb, g_stage, n, &did)) return did;
      if (j.abort) return j.ph = kPIdle, true;
      uint32_t got = j.rq.got > 0 ? (uint32_t)j.rq.got / fb : 0;
      if (got > left) got = left;
      wavread::to_stereo16(j.a, g_stage, got, g_buf + 2 * g_st.loaded);
      g_st.loaded += got;
      st(&g_filled, g_st.loaded);
      if (!got || g_st.loaded >= j.want) {   // the end (or the card's: what is there plays)
        st(&g_end, g_st.loaded ? g_st.loaded : 1);
        g_st.st = kDone;
        j.ph = kPIdle;
      }
      return true;
    }
  }
  return false;
}

const Status& status() { return g_st; }
const Stats& stats() { return g_stats; }

void mix(int32_t (*out)[12], unsigned n, int left, int right) {
  if (left < 0 || left >= 12 || right < 0 || right >= 12 || !g_buf) return;   // (a device without the pair: kNoChannel)
  uint32_t gen = ld(&g_gen);
  if (!gen) return;
  Voice& v = g_v;
  if (gen != v.gen) {
    v = Voice(), v.gen = gen;
    st(&g_seen, gen);
  }
  if (ld(&g_done) == gen) return;
  bool stopping = ld(&g_stop) == gen;
  uint32_t filled = ld(&g_filled), end = ld(&g_end), inc = ld(&g_inc);
  if (!v.started) {
    if (stopping) {   // stopped before a frame sounded
      st(&g_done, gen), ++g_stats.ends;
      return;
    }
    if (!end && filled < kStartFrames) return;
    v.started = true;
  }
  uint32_t have = end ? end : filled;
  float lv, to;   // PREVIEW LEVEL: a change ramped over this block
  __atomic_load(&g_level, &to, __ATOMIC_RELAXED);
  lv = g_lvl < 0.0f ? to : g_lvl;
  float dl = n ? (to - lv) / (float)n : 0.0f;
  g_lvl = to;
  for (unsigned f = 0; f < n; ++f) {
    lv += dl;
    uint32_t i = v.pos;
    if (end && i >= end) {
      st(&g_done, gen), ++g_stats.ends;
      return;
    }
    bool hold = !end && i + 1 >= have;   // the card behind: the last frame held, faded out, until more is there
    if (hold && !v.held) v.held = true, ++g_stats.underruns;
    if (!hold) v.held = false;
    if (hold && i >= have) i = have - 1;   // (above 48 kHz a step can land past the last loaded frame)
    if (stopping || hold) {
      if (!v.ramp) {
        if (stopping) st(&g_done, gen), ++g_stats.ends;
        return;
      }
      --v.ramp;
    } else if (v.ramp < kFade) {
      ++v.ramp;
    }
    uint32_t k = i + 1 < have ? i + 1 : i;
    float t = hold ? 0.0f : (float)v.frac * (1.0f / 65536.0f), g = lv * 65536.0f * (float)v.ramp * (1.0f / kFade);
    float l = (float)g_buf[2 * i] + ((float)g_buf[2 * k] - (float)g_buf[2 * i]) * t;
    float r = (float)g_buf[2 * i + 1] + ((float)g_buf[2 * k + 1] - (float)g_buf[2 * i + 1]) * t;
    out[f][left] = add_sat(out[f][left], l * g);
    out[f][right] = add_sat(out[f][right], r * g);
    ++g_stats.played;
    if (hold) continue;
    v.frac += inc;
    v.pos += v.frac >> 16;
    v.frac &= 0xFFFF;
  }
}

CORE_COLD void reset_for_tests() {
  if (j.f_open) j.f.close();
  j.~Job();
  new (&j) Job();
  g_st = Status{};
  g_v = Voice();
  g_gen = g_filled = g_end = g_inc = g_stop = g_done = g_seen = 0;
  g_stats = Stats{};
  g_lvl = -1.0f, g_level_db = kLevelDefDb, g_dirty = false, g_keep.n = 0;
  begin();
}

}  // namespace preview
}  // namespace core

#endif
