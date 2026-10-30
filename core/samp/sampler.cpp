// core/samp/sampler.cpp - see sampler.h.
#include "sampler.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../fx/out.h"
#include "../granular/granular.h"
#include "../lib/pools.h"
#include "../msamp/msamp.h"
#include "../platform.h"
#include "../pool/pool.h"
#include "dsp.h"
#include "out.h"

namespace core {
namespace samp {

namespace {

constexpr int kMaxSlices = 92;   // keys 36..127

Config cfg;                  // the console's
Config run;                  // the armed take's
float* ring = nullptr;       // cap frames x ch, a ring while armed
uint32_t cap = 0, ring_floats = 0;
int ch = 2;
volatile int state = kIdle;
volatile uint32_t wpos = 0, armed_n = 0, start_at = 0, rec_n = 0, want_n = 0, pre_got = 0;
volatile uint8_t go = 0, stop_req = 0;
volatile uint32_t play_gen = 0, bar_gen = 0, stop_gen = 0;
uint32_t seen_play = 0, seen_bar = 0, seen_stop = 0;
uint32_t pre_n = 0;
float thr_lin = 0.03f;
volatile float lvl = 0, pk = 0;
// the Rytm's clock (the control core's)
uint32_t ticks = 0, last_tick_us = 0;
volatile bool running = false;
uint32_t bar0 = 0;           // the bar count at the take's start
bool clocked = false;        // a bars take ends on the Rytm's bar (its clock runs), else on the frames at the tempo
float tick_us = 0;           // smoothed
volatile float clock_bpm = 0;
// processing (the card core's)
enum Phase : uint8_t { kPNone, kPLinear, kPAnalyse, kPWav, kPSfz };
Phase phase = kPNone;
uint32_t pa = 0, plen = 0;
float gain = 1, gain_db = 0;
uint32_t slices[kMaxSlices];
int nslices = 0;
char take[32] = "", dir[64] = "", wav[96] = "", error[64] = "";
WavWriter writer;
uint32_t t_write0 = 0, write_ms = 0;
volatile uint8_t rescan_req = 0;
char slices_path[160] = "";   // the take's slice map in the instruments' folder ("" none): into the INSTRUMENTS pool
char take_row[8] = "";        // the take's SAMPLES pool row (the control core's, tick)

inline uint32_t ld(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void sto(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline int st() { return __atomic_load_n(&state, __ATOMIC_ACQUIRE); }
inline void set_state(int s) { __atomic_store_n(&state, s, __ATOMIC_RELEASE); }

CORE_COLD void fail(const char* why) {
  strncpy(error, why, sizeof error - 1);
  error[sizeof error - 1] = 0;
  log(CORE_STR("samp: %s: %s"), take[0] ? take : "-", why);
  set_state(kFailed);
  phase = kPNone;
}

CORE_COLD float bpm_now() {
  float b = clock_bpm;
  return b >= 30 && b <= 300 ? b : run.bpm;
}

// The producer: one frame (ch samples) of the source
inline void feed(const float* s) {
  int stt = st();
  if (stt != kArmed && stt != kRecording) return;
  float a = 0;
  for (int c = 0; c < ch; ++c) {
    float v = s[c] < 0 ? -s[c] : s[c];
    if (v > a) a = v;
  }
  if (a > lvl) lvl = a;
  float* d = ring + (size_t)wpos * ch;
  for (int c = 0; c < ch; ++c) d[c] = s[c];
  uint32_t here = wpos;
  wpos = wpos + 1 >= cap ? 0 : wpos + 1;
  if (stt == kArmed) {
    armed_n = armed_n + 1;
    bool trig = false;
    switch (run.start) {
      case kManual: trig = go; break;
      case kThreshold: trig = a > thr_lin; break;
      case kOnPlay: trig = ld(&play_gen) != seen_play; break;
      case kOnBar: trig = ld(&bar_gen) != seen_bar; break;
    }
    if (!trig) return;
    uint32_t pre = armed_n - 1 < pre_n ? armed_n - 1 : pre_n;
    start_at = (here + cap - pre) % cap;
    pre_got = pre;
    rec_n = pre + 1;
    uint32_t len = run.bars ? (uint32_t)(run.bars * 4 * 60.0f / bpm_now() * kRate + 0.5f) : run.max_s * kRate;
    bar0 = ld(&bar_gen);
    clocked = run.bars && running;
    if (clocked) len += len / 4 + kRate / 10;   // the clock ends it (a tempo not measured yet: room for it)
    if (len > run.max_s * kRate) len = run.max_s * kRate;
    want_n = pre + len;
    seen_stop = ld(&stop_gen);
    pk = a;
    set_state(kRecording);
    return;
  }
  rec_n = rec_n + 1;
  if (a > pk) pk = a;
  bool end = rec_n >= want_n || stop_req || (run.start == kOnPlay && !run.bars && ld(&stop_gen) != seen_stop) ||
             (clocked && ld(&bar_gen) - bar0 >= run.bars);
  if (end) set_state(kCaptured);
}

// in place: reverse frames [i, j)
CORE_COLD void reverse(uint32_t i, uint32_t j) {
  while (j > i + 1) {
    --j;
    for (int c = 0; c < ch; ++c) {
      float t = ring[(size_t)i * ch + c];
      ring[(size_t)i * ch + c] = ring[(size_t)j * ch + c];
      ring[(size_t)j * ch + c] = t;
    }
    ++i;
  }
}

CORE_COLD int put(char* o, int cap, int k, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
CORE_COLD int put(char* o, int cap, int k, const char* fmt, ...) {
  if (k >= cap) return k;
  va_list ap;
  va_start(ap, fmt);
  int n = vsniprintf(o + k, cap - k, fmt, ap);
  va_end(ap);
  return n > 0 ? (k + n < cap ? k + n : cap - 1) : k;
}

// The slice map as SFZ: one region a slice on keys 36.., each played to its end once (one_shot); path the sample as
// the .sfz names it.
CORE_COLD int slices_sfz(char* o, int cap, const char* path) {
  int k = put(o, cap, 0, "// Sidecar sample %s: %d slices (%s), 48 kHz; key 36 (C1) is the first\n", take, nslices,
              run.slices == kGrid ? "grid" : "transients");
  k = put(o, cap, k, "<global> loop_mode=one_shot ampeg_attack=0.001 ampeg_release=0.02\n");
  for (int i = 0; i < nslices; ++i) {
    uint32_t a = slices[i], b = i + 1 < nslices ? slices[i + 1] : plen;
    k = put(o, cap, k, "<region> sample=%s key=%d pitch_keycenter=%d offset=%lu end=%lu\n", path, 36 + i, 36 + i,
            (unsigned long)a, (unsigned long)(b - 1));
  }
  return k;
}

CORE_COLD void process_step() {
  switch (phase) {
    case kPNone: break;
    case kPLinear: {   // the take from its start, linear in the ring: rotate the ring left by start_at
      uint32_t n = rec_n < cap ? rec_n : cap, s = start_at;
      if (s) {
        reverse(0, s), reverse(s, cap), reverse(0, cap);
      }
      plen = n, pa = 0;
      if (run.bars) {   // exactly the bars at the tempo measured over the take (the clock's last bar line can be a block late)
        uint32_t exact = pre_got + (uint32_t)(run.bars * 4 * 60.0f / bpm_now() * kRate + 0.5f);
        if (exact < plen) plen = exact;
      }
      phase = kPAnalyse;
      break;
    }
    case kPAnalyse: {
      uint32_t n = plen;
      if (run.trim && !run.bars) {
        Trim t;
        if (!trim(ring, ch, n, pre_got, &t, run.start == kThreshold ? run.thr_db - 12.0f : -50.0f)) {
          fail("no sound");
          return;
        }
        pa = t.start, plen = t.end - t.start;
      }
      float p = peak(ring + (size_t)pa * ch, ch, plen);
      gain = 1, gain_db = 0;
      if (run.normalise && p > 1e-5f) {
        gain = 0.891f / p;   // -1 dBFS
        if (gain > 100) gain = 100;
        gain_db = 20 * log10f(gain);
      }
      if (run.slices == kTransients)
        nslices = transients(ring + (size_t)pa * ch, ch, plen, slices, kMaxSlices, run.sens);
      else if (run.slices == kGrid)
        nslices = grid(ring + (size_t)pa * ch, ch, plen, run.grid_n > kMaxSlices ? kMaxSlices : run.grid_n, slices);
      else
        nslices = 0;
      char prj[24];
      safe_name(project_name[0] ? project_name : "Sidecar", prj, sizeof prj);   // (2026-10-27: the Rytm's name, not a slot key)
      sniprintf(dir, sizeof dir, "/Samples/%s", prj);
      if (!mkdirs(dir)) {
        fail("cannot make the folder");
        return;
      }
      if (run.name[0]) safe_name(run.name, take, sizeof take);
      else next_name(dir, "SMP", ".wav", take, sizeof take);
      sniprintf(wav, sizeof wav, "%s/%s.wav", dir, take);
      t_write0 = now_ms();
      bool trimmed = run.trim && !run.bars;
      if (!writer.begin(wav, ring, ch, pa, plen, gain, trimmed ? kRate / 1000 : 0, trimmed ? kRate / 100 : 0)) {
        fail("cannot create the WAV");
        return;
      }
      set_state(kWriting);
      phase = kPWav;
      break;
    }
    case kPWav: {
      int r = writer.step();
      if (r < 0) fail("write failed (card full?)");
      else if (r == 0) phase = kPSfz;
      break;
    }
    case kPSfz: {
      static char txt[8192];
      char path[160], prj[24];
      safe_name(project_name[0] ? project_name : "Sidecar", prj, sizeof prj);   // (2026-10-27: the Rytm's name, not a slot key)
      if (nslices > 0) {   // the slicing data next to the WAV, then the instruments
        sniprintf(path, sizeof path, "%s/%s.sfz", dir, take);
        char rel[48];
        sniprintf(rel, sizeof rel, "%s.wav", take);
        int n = slices_sfz(txt, sizeof txt, rel);
        if (!write_text(path, txt, n)) log(CORE_STR("samp: cannot write %s"), path);
      }
      char md[64];
      sniprintf(md, sizeof md, "%s/%s", msamp::kDir, prj);
      mkdirs(md);
      int k = put(txt, sizeof txt, 0, "// Sidecar sample %s (%s): one zone over every key, the sample's pitch at C4\n", take,
                  wav);
      k = put(txt, sizeof txt, k, "<region> sample=%s lokey=0 hikey=127 pitch_keycenter=60 loop_mode=no_loop "
              "ampeg_release=0.05\n", wav);
      sniprintf(path, sizeof path, "%s/%s.sfz", md, take);
      bool ok = write_text(path, txt, k);
      if (nslices > 1) {
        int n = slices_sfz(txt, sizeof txt, wav);
        sniprintf(path, sizeof path, "%s/%s-slices.sfz", md, take);
        ok = write_text(path, txt, n) && ok;
        if (ok) sniprintf(slices_path, sizeof slices_path, "%s", path);
      }
      if (!ok) log(CORE_STR("samp: cannot write the instruments in %s"), md);
      write_ms = now_ms() - t_write0;
      log(CORE_STR("samp: %s: %lu.%02lu s %s, %d slices, gain %d.%d dB, written in %lu ms (%lu waits for the streams)"),
          wav, (unsigned long)(plen / kRate), (unsigned long)(plen % kRate * 100 / kRate), ch == 2 ? "stereo" : "mono",
          nslices, (int)gain_db, (int)(fabsf(gain_db) * 10) % 10, (unsigned long)write_ms, (unsigned long)writer.yields);
      phase = kPNone;
      rescan_req = 1;
      set_state(kDone);
      break;
    }
  }
}

CORE_COLD const char* src_name(const Config& c, char* b, int cap) {
  static const char* const tr[8] = {"BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB"};
  if (c.src == kSrcIn) return "IN L/R";
  if (c.src == kSrcMain) return "MAIN";
  if (c.src == kSrcTrack) return tr[c.sub & 7];
  sniprintf(b, cap, "VOICE %d", c.sub + 1);
  return b;
}

CORE_COLD bool num(const char* s, int* v) {
  int sg = 1, x = 0;
  if (*s == '-') sg = -1, ++s;
  if (*s < '0' || *s > '9') return false;
  for (; *s >= '0' && *s <= '9'; ++s) x = x * 10 + (*s - '0');
  *v = sg * x;
  return true;
}

}  // namespace

CORE_COLD void configure(const Config& c) { cfg = c; }
CORE_COLD const Config& config() { return cfg; }

CORE_COLD bool arm(char* err, int cap_) {
  int s = st();
  if (s == kArmed || s == kRecording || s == kCaptured || s == kWriting) {
    sniprintf(err, cap_, "busy: a take is %s", s == kArmed ? "armed" : s == kRecording ? "recording" : "being written");
    return false;
  }
  run = cfg;
  if (run.max_s < 1) run.max_s = 1;
  if (run.max_s > 120) run.max_s = 120;
  // a Sidecar voice routed OUT PRE / POST (core/fx/out.h) is a stereo pair: its take stereo too
  ch = run.src == kSrcIn || run.src == kSrcMain || (run.src == kSrcVoice && (out::stereo(run.sub) || run.fx)) ? 2 : 1;
  pre_n = run.pre_ms > 1000 ? kRate : run.pre_ms * kRate / 1000;
  uint32_t need = (run.max_s + 1) * kRate + pre_n;
  if (!ring || ring_floats < need * 2) {
    delete[] ring;
    ring_floats = need * 2;
    ring = new float[ring_floats];
    if (!ring) {
      ring_floats = 0;
      sniprintf(err, cap_, "no memory for %lu s", (unsigned long)run.max_s);
      return false;
    }
  }
  cap = ring_floats / ch;
  thr_lin = db_to_lin(run.thr_db);
  wpos = armed_n = rec_n = start_at = pre_got = 0;
  go = run.start == kManual ? 1 : 0;
  stop_req = 0;
  seen_play = ld(&play_gen), seen_bar = ld(&bar_gen), seen_stop = ld(&stop_gen);
  take[0] = wav[0] = error[0] = 0;
  take_row[0] = 0;
  nslices = 0, pk = 0, gain_db = 0;
  phase = kPNone;
  set_state(kArmed);
  char b[12];
  log(CORE_STR("samp: armed: %s, start %s, pre-roll %lu ms, %s%d%s"), src_name(run, b, sizeof b),
      run.start == kManual ? "now" : run.start == kThreshold ? "threshold" : run.start == kOnPlay ? "PLAY" : "next bar",
      (unsigned long)run.pre_ms, run.bars ? "" : "free, at most ", run.bars ? run.bars : (int)run.max_s,
      run.bars ? " bars" : " s");
  return true;
}

CORE_COLD void stop() {
  int s = st();
  if (s == kArmed) set_state(kIdle);
  else if (s == kRecording) stop_req = 1;
}

CORE_COLD void cancel() {
  int s = st();
  if (s == kArmed || s == kRecording || s == kCaptured) set_state(kIdle);   // (writing: it finishes)
}

CORE_COLD void transport(uint8_t s) {
  uint32_t now = now_us();
  if (s == 0xFA) {
    running = true, ticks = 0;
    sto(&play_gen, play_gen + 1);
    sto(&bar_gen, bar_gen + 1);   // PLAY starts bar 1
  } else if (s == 0xFB) {
    running = true;
  } else if (s == 0xFC) {
    running = false;
    sto(&stop_gen, stop_gen + 1);
  } else if (s == 0xF8) {
    if (last_tick_us) {
      float dt = (float)(now - last_tick_us);
      if (dt > 500 && dt < 100000) {   // 25..5000 bpm: a real tick interval
        tick_us = tick_us > 0 ? tick_us + (dt - tick_us) * 0.05f : dt;
        clock_bpm = 60e6f / (tick_us * 24);
      }
    }
    last_tick_us = now;
    if (running && ++ticks % 96 == 0) sto(&bar_gen, bar_gen + 1);   // 4/4: 96 ticks a bar
  }
}

CORE_COLD void tick() {
  if (rescan_req) {   // the take into the SAMPLES pool, its slice map into the INSTRUMENTS pool (the bank last shown)
    rescan_req = 0;
    int out = -1;
    int r = pools::add_take(pools::kSamples, wav, &out);
    if ((r == pools::kOk || r == pools::kExisting) && out >= 0) pools::row_text(out, take_row);
    else take_row[0] = 0;
    log(CORE_STR("samp: TAKE -> %s (%s)"), take_row[0] ? take_row : "-", pools::result_text(r));
    if (slices_path[0]) {
      int o2 = -1;
      pools::add_take(pools::kInstr, slices_path, &o2);   // (read on the card core: set a moment later)
      slices_path[0] = 0;
    }
    msamp::rescan();
    gr::rescan();
  }
}

CORE_COLD void in_block(const int32_t* f, int n, int stride) {
  int s = st();
  if ((s != kArmed && s != kRecording) || run.src == kSrcVoice) return;
  int c0 = run.src == kSrcIn ? 10 : run.src == kSrcMain ? 0 : 2 + (run.sub & 7);
  for (int i = 0; i < n; ++i) {
    float x[2];
    for (int c = 0; c < ch; ++c) x[c] = (float)f[i * stride + c0 + c] * (1.0f / 2147483648.0f);
    feed(x);
  }
}

CORE_COLD void voice_block(const int16_t (*v)[8], int n, const int16_t (*lr)[16], const float (*fx)[4]) {
  int s = st();
  if ((s != kArmed && s != kRecording) || run.src != kSrcVoice) return;
  const int k = run.sub & 7;
  for (int i = 0; i < n; ++i) {
    float x[2] = {v[i][k] * (1.0f / 32768.0f), 0.0f};
    if (ch == 2) {   // a PRE / POST voice: its L and R (no planes given: its mono on both)
      if (lr && out::stereo(k)) x[0] = lr[i][k] * (1.0f / 32768.0f), x[1] = lr[i][8 + k] * (1.0f / 32768.0f);
      else x[1] = x[0];
    }
    if (run.fx && fx) x[0] += fx[i][0] + fx[i][2], x[1] += fx[i][1] + fx[i][3];   // CHOP's RESAMPLE with FX
    feed(x);
  }
}

CORE_COLD bool wants_in() {
  int s = st();
  return (s == kArmed || s == kRecording) && run.src != kSrcVoice;
}
CORE_COLD bool wants_voices() {
  int s = st();
  return (s == kArmed || s == kRecording) && run.src == kSrcVoice;
}

CORE_COLD bool poll() {
  int s = st();
  if (s == kCaptured && phase == kPNone) {
    phase = kPLinear;
    set_state(kWriting);
  }
  if (phase == kPNone) return false;
  process_step();
  return true;
}

CORE_COLD void status(Status* o) {
  o->state = st();
  o->cfg = st() == kIdle ? cfg : run;
  o->frames = rec_n;
  o->peak = pk;
  o->level = lvl;
  lvl = lvl * 0.5f;   // (a meter: decays between reads)
  o->bpm = clock_bpm;
  o->slices = nslices;
  o->gain_db = gain_db;
  memcpy(o->take, take, sizeof o->take);
  memcpy(o->wav, wav, sizeof o->wav);
  memcpy(o->row, take_row, sizeof o->row);
  memcpy(o->error, error, sizeof o->error);
  o->write_ms = write_ms;
}

CORE_COLD int json(char* out, int cap_) {
  Status s;
  status(&s);
  const Config& c = s.cfg;
  return sniprintf(out, cap_, "{\"state\":%d,\"src\":%d,\"sub\":%d,\"start\":%d,\"thr\":%d,\"preMs\":%lu,\"bars\":%d,"
                   "\"maxS\":%lu,\"slices\":%d,\"grid\":%d,\"ms\":%lu,\"level\":%d,\"peak\":%d,\"bpm\":%d,\"nSlices\":%d,"
                   "\"gainDb\":%d,\"wav\":\"%s\",\"row\":\"%s\",\"error\":\"%s\"}", s.state, c.src, c.sub, c.start, (int)c.thr_db,
                   (unsigned long)c.pre_ms, c.bars, (unsigned long)c.max_s, c.slices, c.grid_n,
                   (unsigned long)((uint64_t)s.frames * 1000 / kRate), (int)(s.level * 1000), (int)(s.peak * 1000),
                   (int)(s.bpm + 0.5f), s.slices, (int)s.gain_db, s.wav, s.row, s.error);
}

CORE_COLD const float* take_audio(uint32_t* frames, int* c) {
  *frames = plen, *c = ch;
  return ring ? ring + (size_t)pa * ch : nullptr;
}
CORE_COLD int take_slices(const uint32_t** s) {
  *s = slices;
  return nslices;
}

CORE_COLD void reset() {
  set_state(kIdle);
  cfg = Config();
  phase = kPNone;
  running = false, ticks = 0, last_tick_us = 0, tick_us = 0, clock_bpm = 0;
  rescan_req = 0;
}

// samp                       the state, the settings, the last take
// samp=src:in|main|track:T|voice:V    (T 1..8: BD SD RS/CP BT LT MT/HT CH/OH CY/CB; V 1..8)
// samp=start:now|thr:DB|play|bar      samp=pre:MS  samp=len:BARS (0 free)  samp=max:S  samp=bpm:N
// samp=slices:off|trans[:SENS]|grid:N  samp=norm:on|off  samp=trim:on|off  samp=name:NAME|auto
// samp=arm  samp=stop  samp=cancel
CORE_COLD bool command(const char* p, char* out, int cap_) {
  if (strncmp(p, "samp", 4) || (p[4] && p[4] != '=')) return false;
  char err[64] = "";
  bool ok = true;
  if (p[4] == '=') {
    const char* v = p + 5;
    int n = 0;
    if (!strncmp(v, "src:", 4)) {
      const char* a = v + 4;
      if (!strcmp(a, "in")) cfg.src = kSrcIn;
      else if (!strcmp(a, "main")) cfg.src = kSrcMain;
      else if (!strncmp(a, "track:", 6) && num(a + 6, &n) && n >= 1 && n <= 8) cfg.src = kSrcTrack, cfg.sub = (uint8_t)(n - 1);
      else if (!strncmp(a, "voice:", 6) && num(a + 6, &n) && n >= 1 && n <= 8) cfg.src = kSrcVoice, cfg.sub = (uint8_t)(n - 1);
      else ok = false, strcpy(err, "src:in|main|track:1..8|voice:1..8");
    } else if (!strncmp(v, "start:", 6)) {
      const char* a = v + 6;
      if (!strcmp(a, "now")) cfg.start = kManual;
      else if (!strncmp(a, "thr", 3)) {
        cfg.start = kThreshold;
        if (a[3] == ':' && num(a + 4, &n)) cfg.thr_db = (float)(n > 0 ? -n : n);
      } else if (!strcmp(a, "play")) cfg.start = kOnPlay;
      else if (!strcmp(a, "bar")) cfg.start = kOnBar;
      else ok = false, strcpy(err, "start:now|thr[:DB]|play|bar");
    } else if (!strncmp(v, "pre:", 4) && num(v + 4, &n)) cfg.pre_ms = n < 0 ? 0 : n > 1000 ? 1000 : n;
    else if (!strncmp(v, "len:", 4) && num(v + 4, &n)) cfg.bars = (uint8_t)(n < 0 ? 0 : n > 64 ? 64 : n);
    else if (!strncmp(v, "max:", 4) && num(v + 4, &n)) cfg.max_s = n < 1 ? 1 : n > 120 ? 120 : n;
    else if (!strncmp(v, "bpm:", 4) && num(v + 4, &n)) cfg.bpm = (float)(n < 30 ? 30 : n > 300 ? 300 : n);
    else if (!strncmp(v, "slices:", 7)) {
      const char* a = v + 7;
      if (!strcmp(a, "off")) cfg.slices = kNoSlices;
      else if (!strncmp(a, "trans", 5)) {
        cfg.slices = kTransients;
        if (a[5] == ':' && num(a + 6, &n)) cfg.sens = n < 15 ? 1.5f : n > 80 ? 8.0f : n / 10.0f;   // tenths
      } else if (!strncmp(a, "grid:", 5) && num(a + 5, &n) && n >= 1) cfg.slices = kGrid, cfg.grid_n = (uint8_t)(n > kMaxSlices ? kMaxSlices : n);
      else ok = false, strcpy(err, "slices:off|trans[:SENSx10]|grid:N");
    } else if (!strncmp(v, "norm:", 5)) cfg.normalise = !strcmp(v + 5, "on");
    else if (!strncmp(v, "trim:", 5)) cfg.trim = !strcmp(v + 5, "on");
    else if (!strncmp(v, "name:", 5)) {
      if (!strcmp(v + 5, "auto")) cfg.name[0] = 0;
      else safe_name(v + 5, cfg.name, sizeof cfg.name);
    } else if (!strcmp(v, "arm")) ok = arm(err, sizeof err);
    else if (!strcmp(v, "stop")) stop();
    else if (!strcmp(v, "cancel")) cancel();
    else ok = false, strcpy(err, "samp=src:|start:|len:|slices:|arm|stop|cancel ...");
  }
  Status s;
  status(&s);
  static const char* const names[] = {"idle", "armed", "recording", "captured", "writing", "done", "failed"};
  char b[12];
  const Config& c = s.cfg;
  int k = sniprintf(out, cap_, "samp%s%s: %s, src %s, start %s", ok ? "" : ": ", ok ? "" : err, names[s.state % 7],
                    src_name(c, b, sizeof b),
                    c.start == kManual ? "now" : c.start == kThreshold ? "thr" : c.start == kOnPlay ? "PLAY" : "bar");
  if (k > 0 && k < cap_ && c.start == kThreshold) k += sniprintf(out + k, cap_ - k, " %d dB", (int)c.thr_db);
  if (k > 0 && k < cap_)
    k += sniprintf(out + k, cap_ - k, ", pre %lu ms, %s%d%s, slices %s, clock %d bpm, %lu.%lu s", (unsigned long)c.pre_ms,
                   c.bars ? "" : "free <= ", c.bars ? c.bars : (int)c.max_s, c.bars ? " bars" : " s",
                   c.slices == kNoSlices ? "off" : c.slices == kGrid ? "grid" : "trans", (int)(s.bpm + 0.5f),
                   (unsigned long)(s.frames / kRate), (unsigned long)(s.frames % kRate * 10 / kRate));
  if (k > 0 && k < cap_ && s.wav[0])
    k += sniprintf(out + k, cap_ - k, ", last %s (%d slices, gain %d dB)", s.wav, s.slices, (int)s.gain_db);
  if (k > 0 && k < cap_ && s.error[0]) sniprintf(out + k, cap_ - k, ", error: %s", s.error);
  return true;
}

}  // namespace samp
}  // namespace core
