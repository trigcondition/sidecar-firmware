// core/samp/autosamp.cpp - see autosamp.h.
#include "../voices.h"   // first (platform.h, the engines)

#include "autosamp.h"

#include <math.h>
#include <stdarg.h>
#include <string.h>

#include "../granular/granular.h"
#include "../lib/pools.h"
#include "../keys.h"
#include "../msamp/msamp.h"
#include "../presets/dx7_lib.h"
#include "dsp.h"
#include "out.h"

namespace core {
namespace samp {

namespace {

constexpr int kMaxTakes = 512;
constexpr uint64_t kArenaMax = 96ull << 20;   // the takes' audio (float mono) at most: 96 MB, ~8.7 minutes
constexpr uint32_t kRenderChunk = 4800;       // offline: 100 ms of audio a poll

struct Take {
  uint8_t note, vel, layer, rr;
  uint32_t off, cap, len;   // in the arena: its first float, room (frames), recorded (frames); stereo: L then R planes
  uint32_t a, n;            // trimmed: first frame, count
  uint32_t note_off;        // the frame the note was released (from off)
  bool loop, silent;
  uint32_t ls, le;          // the loop (relative to a), inclusive end
};

Plan plan, cfg;
Take takes[kMaxTakes];
int ntakes = 0;
float* arena = nullptr;
uint64_t arena_frames = 0;
int nch = 1;                 // the takes' channels: 2 for EXT L+R (each take two planes of cap frames, L then R)
AMidiOut midi_out = nullptr;
const char* const kTrackNames[kATracks] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
const char* const kPortNames[kAPorts] = {"USB", "DEV", "RYTM"};
const char* const kInNames[3] = {"L+R", "L", "R"};
volatile int state = kAIdle;
volatile uint8_t cancel_req = 0;
volatile int cur = 0;            // the take being taken
char label[24] = "", name[32] = "", sfz[112] = "", error[64] = "", dir[96] = "", srcdesc[80] = "";
float gain = 1, gain_db = 0, lat_ms_used = 0, speed = 0;
int loops = 0, written = 0;
uint32_t out_kb = 0;
// the offline engine (the card core's)
plaits::Voice* ov = nullptr;
char* oscratch = nullptr;
uint8_t* ostage = nullptr;
plaits::Patch opatch;
plaits::Modulations omod;
rytm::FmMachine ofm;
juno::Voice* oj = nullptr;
uint8_t dx7[128];
bool has_dx7 = false;
uint8_t jb[18];
bool jext = false;
bool use_juno = false;
uint32_t r_pos = 0, r_quiet = 0;   // the take's rendered frames, frames in a row under -80 dBFS after the note-off
uint32_t t_start_ms = 0;
uint64_t rendered = 0;
// a Rytm take (the control core's timing, core 0's frames)
enum TPhase : uint8_t { kTNext, kTHold, kTTail, kTGap };
TPhase tph = kTNext;
uint32_t t0 = 0;
volatile uint8_t capturing = 0;
volatile uint32_t cap_w = 0;
// processing (the card core's)
enum PPhase : uint8_t { kPNone, kPGain, kPTake, kPWav, kPSfz, kPCancel };
PPhase pph = kPNone;
int pi = 0;
WavWriter wr;
volatile uint8_t rescan_req = 0;
char paths[kMaxTakes][64];   // each take's file name (no folder)

inline int st() { return __atomic_load_n(&state, __ATOMIC_ACQUIRE); }
inline void set_state(int s) { __atomic_store_n(&state, s, __ATOMIC_RELEASE); }

CORE_COLD int put(char* o, int cap, int k, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
CORE_COLD int put(char* o, int cap, int k, const char* fmt, ...) {
  if (k >= cap) return k;
  va_list ap;
  va_start(ap, fmt);
  int n = vsniprintf(o + k, cap - k, fmt, ap);
  va_end(ap);
  return n > 0 ? (k + n < cap ? k + n : cap - 1) : k;
}

CORE_COLD void fail(const char* why) {
  strncpy(error, why, sizeof error - 1);
  error[sizeof error - 1] = 0;
  log(CORE_STR("asamp: %s: %s"), name[0] ? name : "-", why);
  set_state(kAFailed);
  pph = kPNone;
  capturing = 0;
}

CORE_COLD void set_label(int i) {
  char nn[8];
  note_name(takes[i].note, nn);
  sniprintf(label, sizeof label, "%s v%d %d/%d", nn, takes[i].vel, i + 1, ntakes);
}

CORE_COLD bool offline() { return plan.src != kATrack && plan.src != kAExt; }
// take t's plane c (0 L / mono, 1 R)
inline float* plane(const Take& t, int c) { return arena + t.off + (size_t)c * t.cap; }

// A fresh offline voice for take i (the card core's), its note on
CORE_COLD void voice_on(const Take& t) {
  float k[8];
  for (int j = 0; j < 8; ++j) k[j] = plan.knobs[j];
  k[7] = 127;   // LEN: the gate held until the note-off
  if (use_juno) {
    oj->Init(48000.0f);
    oj->UsePatch(jext ? jb : nullptr, name);
    oj->SetKnobs(k);
    oj->NoteOn((float)t.note, (t.vel < 1 ? 1 : t.vel) / 127.0f);
  } else {
    stmlib::BufferAllocator a(oscratch, 16384);
    ov->Init(&a);
    memset(&opatch, 0, sizeof opatch);
    memset(&omod, 0, sizeof omod);
    omod.trigger_patched = true;
    ofm.Init();
    ofm.SetVoice(has_dx7 ? dx7 : nullptr);
    ofm.SetKnobs(k);
    ofm.NoteOn((float)t.note, (t.vel < 1 ? 1 : t.vel) / 127.0f);
  }
}
CORE_COLD void voice_off() {
  if (use_juno) oj->NoteOff();
  else ofm.NoteOff();
}
CORE_COLD void voice_render(float* o, int n) {
  if (use_juno) oj->Render(o, n);
  else ofm.Render(ov, &opatch, &omod, ostage, o, n);
}

// One offline step: up to kRenderChunk frames of take cur; the next take when it is done
CORE_COLD void render_step() {
  Take& t = takes[cur];
  if (r_pos == 0) {
    set_label(cur);
    voice_on(t);
  }
  uint32_t hold = plan.hold_ms * kRate / 1000;
  float* x = arena + t.off;
  uint32_t todo = kRenderChunk;
  bool end = false;
  while (todo && !end) {
    uint32_t n = todo < 480 ? todo : 480;
    if (r_pos < hold && r_pos + n > hold) n = hold - r_pos;
    if (r_pos + n > t.cap) n = t.cap - r_pos;
    voice_render(x + r_pos, (int)n);
    for (uint32_t i = 0; i < n; ++i) {
      float v = x[r_pos + i] < 0 ? -x[r_pos + i] : x[r_pos + i];
      r_quiet = r_pos + i >= hold && v < 1e-4f ? r_quiet + 1 : 0;
    }
    r_pos += n, todo -= n, rendered += n;
    if (r_pos == hold) voice_off(), t.note_off = hold;
    if (r_pos >= t.cap || (r_pos > hold && r_quiet >= kRate / 10)) end = true;
  }
  if (!end) return;
  t.len = r_pos;
  r_pos = r_quiet = 0;
  if (++cur >= ntakes || cancel_req) {
    uint32_t ms = now_ms() - t_start_ms;
    speed = ms ? (float)rendered / kRate / (ms / 1000.0f) : 0;
    pph = cancel_req ? kPCancel : kPGain;
    set_state(cancel_req ? kACancelled : kAWriting);
  }
}

// The global gain: one normalisation for the instrument (peak -1 dBFS), so velocity layers keep their relation
CORE_COLD void gain_step() {
  float p = 0;
  for (int i = 0; i < ntakes; ++i)
    for (int c = 0; c < nch; ++c) {
      float q = peak(plane(takes[i], c), 1, takes[i].len);
      if (q > p) p = q;
    }
  if (p < 1e-6f) {
    fail("no sound in any take");
    return;
  }
  gain = 0.891f / p;
  if (gain > 1000) gain = 1000;
  gain_db = 20 * log10f(gain);
  // the latency (a Rytm track): the first take at the top velocity's onset (above a tenth of its peak), less 2 ms
  uint32_t lat = 0;
  if (!offline()) {
    if (plan.lat_ms >= 0) lat = (uint32_t)plan.lat_ms * kRate / 1000;
    else {
      int best = 0;
      for (int i = 0; i < ntakes; ++i)
        if (takes[i].note == takes[0].note && takes[i].vel > takes[best].vel) best = i;
      const Take& t = takes[best];
      uint32_t on = t.len;
      for (int c = 0; c < nch; ++c) {
        float q = peak(plane(t, c), 1, t.len);
        uint32_t o = q > 0 ? first_above(plane(t, c), 1, t.len, q * 0.1f) : t.len;
        if (o < on) on = o;
      }
      lat = on < t.len ? on : 0;
    }
    lat_ms_used = lat * 1000.0f / kRate;
    log(CORE_STR("asamp: latency %lu.%lu ms (%s)"), (unsigned long)(lat * 1000 / kRate),
        (unsigned long)(lat * 10000 / kRate % 10), plan.lat_ms >= 0 ? "given" : "measured on the first take");
  }
  uint32_t cut = lat > kRate / 500 ? lat - kRate / 500 : 0;
  float floor = db_to_lin(-60.0f) / gain;   // -60 dBFS after the gain
  loops = 0;
  for (int i = 0; i < ntakes; ++i) {
    Take& t = takes[i];
    const float* x = plane(t, 0);   // the loop is found on L (mono: the take), the same loop cut into R
    t.a = cut < t.len ? cut : 0;
    uint32_t e = 0;
    for (int c = 0; c < nch; ++c) {
      uint32_t ec = last_above(plane(t, c), 1, t.len, floor);
      if (ec > e) e = ec;
    }
    t.silent = e <= t.a;
    e = e + kRate / 100 < t.len ? e + kRate / 100 : t.len;   // + 10 ms (faded)
    t.n = e > t.a ? e - t.a : 0;
    t.loop = false;
    uint32_t hold = t.note_off > t.a ? t.note_off - t.a : 0;
    if (plan.loop && !t.silent && hold > kRate / 4) {
      uint32_t s0 = t.a + hold / 4, s1 = t.a + hold - kRate / 50;
      if (sustained(x, s0, s1)) {
        // the best seam; a crossfade over the loop's last third (at most 50 ms) when it is not clean already
        Loop L = find_loop(x, s0, s1, 100, 0.3f);
        if (L.ok) {
          if (L.score > 0.003f) {
            uint32_t xf = (L.e - L.s) / 3 < kRate / 20 ? (L.e - L.s) / 3 : kRate / 20;
            for (int c = 0; c < nch; ++c) crossfade_loop(plane(t, c), L.s, L.e, xf);
          }
          t.loop = true, t.ls = L.s - t.a, t.le = L.e - 1 - t.a, ++loops;
        }
      }
    }
  }
  pi = 0;
  pph = kPTake;
}

CORE_COLD void take_file(int i, char* out, int cap) {
  char nn[8];
  note_name(takes[i].note, nn);
  for (char* c = nn; *c; ++c) if (*c == '#') *c = 's';   // C#4 -> Cs4 (a safe file name)
  sniprintf(out, cap, "%s_%s_v%d_rr%d.wav", name, nn, takes[i].vel, takes[i].rr + 1);
}

CORE_COLD void take_step() {
  if (cancel_req) {
    pph = kPCancel;
    return;
  }
  while (pi < ntakes && takes[pi].silent) ++pi;   // "no sound": left out of the instrument
  if (pi >= ntakes) {
    pph = kPSfz;
    return;
  }
  Take& t = takes[pi];
  set_label(pi);
  take_file(pi, paths[pi], sizeof paths[pi]);
  char path[160];
  sniprintf(path, sizeof path, "%s/samples/%s", dir, paths[pi]);
  if (nch == 2) {   // the planes interleaved in place for the writer (R through the arena's spare take, then from the end)
    float* x = arena + t.off;
    float* r = arena + (size_t)ntakes * t.cap * 2;
    memcpy(r, x + t.cap, t.len * sizeof(float));
    for (uint32_t i = t.len; i-- > 0;) {
      float l = x[i];
      x[2 * i + 1] = r[i], x[2 * i] = l;
    }
  }
  if (!wr.begin(path, arena + t.off, nch, t.a, t.n, gain, 0, kRate / 100, t.loop, t.ls, t.le, t.note)) {
    fail("cannot create a sample file");
    return;
  }
  pph = kPWav;
}

CORE_COLD void sfz_step() {
  static char txt[65536];
  int notes[128], nn = 0;
  uint64_t wavb = 0;
  for (int i = 0; i < ntakes; ++i)
    if (!takes[i].silent) wavb += 44 + (uint64_t)takes[i].n * 3 * nch;   // 24-bit
  out_kb = (uint32_t)(wavb / 1024);
  for (int i = 0; i < ntakes; ++i) {
    bool seen = false;
    for (int j = 0; j < nn; ++j) seen = seen || notes[j] == takes[i].note;
    if (!seen && !takes[i].silent) notes[nn++] = takes[i].note;
  }
  for (int i = 1; i < nn; ++i)   // ascending
    for (int j = i; j > 0 && notes[j] < notes[j - 1]; --j) { int t = notes[j]; notes[j] = notes[j - 1], notes[j - 1] = t; }
  // the release: the loudest take's time from its note-off down to -60 dBFS (after the gain)
  float rel = 0.25f;
  for (int i = 0; i < ntakes; ++i)
    if (!takes[i].silent && takes[i].a + takes[i].n > takes[i].note_off) {
      float r = (float)(takes[i].a + takes[i].n - takes[i].note_off) / kRate;
      if (r > rel) rel = r;
    }
  if (rel > 10) rel = 10;
  int k = put(txt, sizeof txt, 0, "// Auto-sampled by the Sidecar: %s\n// notes %d..%d every %d, %d velocity layer%s, %d round "
              "robin%s, hold %lu ms; normalised %d dB (one gain), %d loops\n", srcdesc, plan.lo, plan.hi, plan.step,
              plan.nvel, plan.nvel > 1 ? "s" : "", plan.rr, plan.rr > 1 ? "s" : "", (unsigned long)plan.hold_ms,
              (int)gain_db, loops);
  k = put(txt, sizeof txt, k, "<control> default_path=samples/\n<global> ampeg_release=%d.%02d\n", (int)rel,
          (int)(rel * 100) % 100);
  for (int ni = 0; ni < nn; ++ni) {
    int key = notes[ni];
    int lo = ni == 0 ? 0 : (notes[ni - 1] + key) / 2 + 1, hi = ni == nn - 1 ? 127 : (key + notes[ni + 1]) / 2;
    for (int i = 0; i < ntakes; ++i) {
      const Take& t = takes[i];
      if (t.note != key || t.silent) continue;
      int L = t.layer;
      int lv = L == 0 ? 1 : (plan.vels[L - 1] + plan.vels[L]) / 2 + 1;
      int hv = L == plan.nvel - 1 ? 127 : (plan.vels[L] + plan.vels[L + 1]) / 2;
      k = put(txt, sizeof txt, k, "<region> sample=%s lokey=%d hikey=%d pitch_keycenter=%d lovel=%d hivel=%d", paths[i], lo, hi,
              key, lv, hv);
      if (plan.rr > 1) k = put(txt, sizeof txt, k, " seq_length=%d seq_position=%d", plan.rr, t.rr + 1);
      if (t.loop) k = put(txt, sizeof txt, k, " loop_mode=loop_sustain loop_start=%lu loop_end=%lu", (unsigned long)t.ls,
                          (unsigned long)t.le);
      else k = put(txt, sizeof txt, k, " loop_mode=no_loop");
      k = put(txt, sizeof txt, k, "\n");
    }
  }
  sniprintf(sfz, sizeof sfz, "%s/%s.sfz", dir, name);
  if (!write_text(sfz, txt, k)) {
    fail("cannot write the .sfz");
    return;
  }
  log(CORE_STR("asamp: %s: %d samples (%d silent left out), %d loops, gain %d dB%s"), sfz, written, ntakes - written,
      loops, (int)gain_db, offline() ? "" : "");
  rescan_req = 1;
  pph = kPNone;
  set_state(kADone);
}

CORE_COLD void cancel_step() {   // the files written so far removed, no .sfz
  char path[160];
  for (int i = 0; i < pi && i < ntakes; ++i) {
    if (takes[i].silent || !paths[i][0]) continue;
    sniprintf(path, sizeof path, "%s/samples/%s", dir, paths[i]);
    cards.sd().remove(path);
  }
  log(CORE_STR("asamp: %s cancelled (%d takes, nothing kept)"), name, cur);
  pph = kPNone;
  set_state(kACancelled);
  capturing = 0;
}

CORE_COLD void send_note(bool on, const Take& t) {
  uint8_t ch = (uint8_t)(plan.midi_ch & 15);
  bool pitched = plan.src == kAExt || plan.chro;
  uint8_t m[3] = {(uint8_t)((on ? 0x90 : 0x80) | ch), (uint8_t)(pitched ? t.note : 60), (uint8_t)(on ? t.vel : 0)};
  if (plan.src == kAExt && plan.port != kPortRytm) {
    if (midi_out) midi_out(plan.port, m, 3);
  } else {
    to_rytm(m, 3);
  }
}

// The Overbridge IN channel a real-time take records (plane c): a track's stem, AUDIO IN L / R
CORE_COLD int in_channel(int c) {
  if (plan.src == kATrack) return 2 + track_stem(plan.track);
  if (plan.in == kInR) return 11;
  return plan.in == kInL ? 10 : 10 + c;
}

CORE_COLD bool parse_note(const char* s, int* v) {
  int n = msamp::note_value(s);
  if (n < 0) return false;
  *v = n;
  return true;
}

CORE_COLD int scan_int(const char*& s) {
  int v = 0, sg = 1;
  if (*s == '-') sg = -1, ++s;
  while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
  return v * sg;
}

}  // namespace

CORE_COLD int track_stem(int track) {
  static const uint8_t kStem[kATracks] = {0, 1, 2, 2, 3, 4, 5, 5, 6, 6, 7, 7};
  return kStem[track < 0 ? 0 : track >= kATracks ? kATracks - 1 : track];
}
CORE_COLD void asamp_set_midi_out(AMidiOut f) { midi_out = f; }
CORE_COLD const char* asamp_port_name(int port) { return kPortNames[port < 0 || port >= kAPorts ? 0 : port]; }

CORE_COLD const Plan& asamp_plan() { return cfg; }
CORE_COLD void asamp_configure(const Plan& p) { cfg = p; }

CORE_COLD int asamp_entries(ASrc s) {
  if (s == kAFm) {
    int nl = dxlib.listed();
    return nl ? nl : dx7::kRomVoices;
  }
  return s == kAJuno ? 128 : s == kATrack ? kATracks : s == kAExt ? 16 * kAPorts : 8;
}

CORE_COLD void asamp_names(const Plan& p, char* srcn, int scap, char* inst, int icap) {
  char nm[32];
  if (p.src == kAFm) {
    uint8_t d[128];
    int e = p.pat < 0 ? 0 : p.pat;
    int total = asamp_entries(kAFm);
    e = e >= total ? total - 1 : e;
    bool ok = true;
    if (!dxlib.listed()) memcpy(d, dx7::rom_bank(e / 32) + 128 * (e % 32), 128);
    else ok = dxlib.voice(e, d);
    if (ok) sniprintf(nm, sizeof nm, "%.10s", (const char*)d + 118);
    else strcpy(nm, "(EMPTY)");
    if (srcn) sniprintf(srcn, scap, "%s", nm);
    sniprintf(inst, icap, "FM_%s", nm);
  } else if (p.src == kAJuno) {
    int e = p.pat < 0 ? 0 : p.pat > 127 ? 127 : p.pat;
    if (srcn) sniprintf(srcn, scap, "%.12s", juno::kPatches[e].name);
    sniprintf(inst, icap, "JUNO_%.20s", juno::kPatches[e].name);
  } else if (p.src == kAVoice) {
    if (srcn) sniprintf(srcn, scap, "VOICE %d", p.voice + 1);
    sniprintf(inst, icap, "VOICE%d", p.voice + 1);
  } else if (p.src == kATrack) {
    int t = p.track < 0 ? 0 : p.track >= kATracks ? kATracks - 1 : p.track;
    if (srcn) sniprintf(srcn, scap, "%s", kTrackNames[t]);
    sniprintf(inst, icap, "RYTM_%s", kTrackNames[t]);
  } else {
    if (srcn) sniprintf(srcn, scap, "%s %d", asamp_port_name(p.port), (p.midi_ch & 15) + 1);
    sniprintf(inst, icap, "EXT_CH%d", (p.midi_ch & 15) + 1);
  }
  if (p.name[0]) sniprintf(inst, icap, "%s", p.name);
}

CORE_COLD bool asamp_start(const Plan& p0, char* err, int cap) {
  int s = st();
  if (s == kATaking || s == kAWriting) {
    sniprintf(err, cap, "busy: %s", label);
    return false;
  }
  plan = p0;
  if (plan.step < 1) plan.step = 1;
  if (plan.lo < 0) plan.lo = 0;
  if (plan.hi > 127) plan.hi = 127;
  if (plan.hi < plan.lo) plan.hi = plan.lo;
  if (plan.nvel < 1) plan.nvel = 1, plan.vels[0] = 100;
  if (plan.nvel > 8) plan.nvel = 8;
  for (int i = 1; i < plan.nvel; ++i)   // ascending velocities
    for (int j = i; j > 0 && plan.vels[j] < plan.vels[j - 1]; --j) { uint8_t t = plan.vels[j]; plan.vels[j] = plan.vels[j - 1], plan.vels[j - 1] = t; }
  if (plan.rr < 1) plan.rr = 1;
  if (plan.rr > 4) plan.rr = 4;
  if (plan.hold_ms < 50) plan.hold_ms = 50;
  if (plan.hold_ms > 20000) plan.hold_ms = 20000;
  if (plan.tail_ms > 20000) plan.tail_ms = 20000;
  // the source
  use_juno = false, has_dx7 = false, jext = false;
  char nm[32] = "";
  if (plan.src == kAVoice) {
    VoiceSound vs;
    if (!voice_sound(plan.voice, &vs)) {
      sniprintf(err, cap, "voice %d plays neither 6-OP FM nor JUNO", plan.voice + 1);
      return false;
    }
    for (int j = 0; j < 8; ++j) plan.knobs[j] = vs.p[j];
    if (vs.machine == kJuno) {
      use_juno = true, memcpy(jb, vs.juno, 18), jext = vs.juno_ext;
      sniprintf(srcdesc, sizeof srcdesc, "Sidecar voice %d, JUNO \"%s\"", plan.voice + 1, vs.name);
      sniprintf(nm, sizeof nm, "JUNO_%s", vs.name);
    } else {
      has_dx7 = vs.has_dx7, memcpy(dx7, vs.dx7, 128);
      sniprintf(srcdesc, sizeof srcdesc, "Sidecar voice %d, 6-OP FM \"%.10s\"", plan.voice + 1, (const char*)dx7 + 118);
      sniprintf(nm, sizeof nm, "FM_%.10s", (const char*)dx7 + 118);
    }
  } else if (plan.src == kAFm) {
    int e = plan.pat < 0 ? 0 : plan.pat > 127 ? 127 : plan.pat;
    int nl = dxlib.listed(), total = nl ? nl : dx7::kRomVoices;
    e = e >= total ? total - 1 : e;
    if (!nl) memcpy(dx7, dx7::rom_bank(e / 32) + 128 * (e % 32), 128), has_dx7 = true;
    else has_dx7 = dxlib.voice(e, dx7);
    if (!has_dx7) {
      sniprintf(err, cap, "FM entry %d: empty or not on the card", e);
      return false;
    }
    plan.knobs[0] = (float)e;
    sniprintf(srcdesc, sizeof srcdesc, "6-OP FM entry %d \"%.10s\"", e, (const char*)dx7 + 118);
    sniprintf(nm, sizeof nm, "FM_%.10s", (const char*)dx7 + 118);
  } else if (plan.src == kAJuno) {
    int e = plan.pat < 0 ? 0 : plan.pat > 127 ? 127 : plan.pat;
    use_juno = true;
    memcpy(jb, juno::kPatches[e].b, 18);
    jext = false;
    plan.knobs[0] = (float)e;
    static const int kb[6] = {5, 6, 7, 11, 12, 14};   // CUT RES ENV ATK DEC REL: the patch's own sliders
    for (int j = 0; j < 6; ++j) plan.knobs[1 + j] = jb[kb[j]];
    sniprintf(srcdesc, sizeof srcdesc, "JUNO factory patch \"%s\"", juno::kPatches[e].name);
    sniprintf(nm, sizeof nm, "JUNO_%.20s", juno::kPatches[e].name);
  } else if (plan.src == kATrack) {
    if (plan.track < 0 || plan.track >= kATracks) plan.track = 0;
    sniprintf(srcdesc, sizeof srcdesc, "the Rytm's track %s (Overbridge IN %d), MIDI channel %d%s", kTrackNames[plan.track],
              2 + track_stem(plan.track), plan.midi_ch + 1, plan.chro ? ", chromatic" : "");
    sniprintf(nm, sizeof nm, "RYTM_%s", kTrackNames[plan.track]);
  } else if (plan.src == kAExt) {
    if (plan.port >= kAPorts) plan.port = kPortUsb;
    if (plan.in > kInR) plan.in = kInLR;
    // the output must be there: the Rytm's always (the host link), the others the platform's
    if (plan.port != kPortRytm && (!midi_out || !midi_out(plan.port, nullptr, 0))) {
      sniprintf(err, cap, "no MIDI out %s on this build", kPortNames[plan.port]);
      return false;
    }
#if CORE_POLY
    if (plan.port == kPortDev && !(keys::view().present & 0x0F)) {
      sniprintf(err, cap, "no USB MIDI device on the USB-A ports");
      return false;
    }
#endif
    sniprintf(srcdesc, sizeof srcdesc, "an external synth: MIDI %s channel %d, AUDIO IN %s (Overbridge IN %s)",
              kPortNames[plan.port], plan.midi_ch + 1, kInNames[plan.in],
              plan.in == kInLR ? "11/12" : plan.in == kInL ? "11" : "12");
    sniprintf(nm, sizeof nm, "EXT_CH%d", plan.midi_ch + 1);
  } else {
    sniprintf(err, cap, "unknown source %d", (int)plan.src);
    return false;
  }
  nch = plan.src == kAExt && plan.in == kInLR ? 2 : 1;
  safe_name(plan.name[0] ? plan.name : nm, name, sizeof name);
  // the takes
  ntakes = 0;
  uint32_t per = (plan.hold_ms + plan.tail_ms) * kRate / 1000 + kRate / 10;   // + 100 ms (the latency's room)
  int notes = plan.src == kATrack && !plan.chro ? 1 : (plan.hi - plan.lo) / plan.step + 1;
  for (int ni = 0; ni < notes; ++ni)
    for (int L = 0; L < plan.nvel; ++L)
      for (int r = 0; r < plan.rr; ++r) {
        if (ntakes >= kMaxTakes) {
          sniprintf(err, cap, "more than %d takes", kMaxTakes);
          return false;
        }
        Take& t = takes[ntakes];
        memset(&t, 0, sizeof t);
        t.note = (uint8_t)(plan.src == kATrack && !plan.chro ? 60 : plan.lo + ni * plan.step);
        t.vel = plan.vels[L], t.layer = (uint8_t)L, t.rr = (uint8_t)r;
        t.off = (uint32_t)(ntakes * (uint64_t)per * nch), t.cap = per;
        paths[ntakes][0] = 0;
        ++ntakes;
      }
  uint64_t need = (uint64_t)ntakes * per * nch + (nch == 2 ? per : 0);   // stereo: a spare plane (the interleave)
  if (need * 4 > kArenaMax) {
    sniprintf(err, cap, "%d takes need %lu MB (at most %lu): fewer notes, layers or a shorter hold / tail", ntakes,
              (unsigned long)(need * 4 >> 20), (unsigned long)(kArenaMax >> 20));
    return false;
  }
  if (!arena || arena_frames < need) {
    delete[] arena;
    arena = new float[need];
    arena_frames = arena ? need : 0;
    if (!arena) {
      sniprintf(err, cap, "no memory for %lu MB", (unsigned long)(need * 4 >> 20));
      return false;
    }
  }
  if (offline() && !ov) {
    ov = new plaits::Voice;
    oscratch = new char[16384 + 64];
    ostage = new uint8_t[rytm::FmMachine::kStageBytes];
    oj = new juno::Voice;
  }
  sniprintf(dir, sizeof dir, "%s/%s", msamp::kDir, name);
  char sd[110];
  sniprintf(sd, sizeof sd, "%s/samples", dir);
  if (!mkdirs(sd)) {
    sniprintf(err, cap, "cannot make %s", sd);
    return false;
  }
  cur = 0, pi = 0, written = 0, loops = 0, out_kb = 0;
  r_pos = r_quiet = 0, rendered = 0;
  cancel_req = 0, capturing = 0, cap_w = 0;
  tph = kTNext;
  error[0] = sfz[0] = 0;
  gain = 1, gain_db = 0, lat_ms_used = 0, speed = 0;
  pph = kPNone;
  t_start_ms = now_ms();
  set_label(0);
  set_state(kATaking);
  log(CORE_STR("asamp: %s from %s: %d takes (%d notes x %d velocities x %d round robins), %lu MB"), name, srcdesc, ntakes,
      notes, plan.nvel, plan.rr, (unsigned long)(need * 4 >> 20));
  return true;
}

CORE_COLD void asamp_cancel() {
  if (st() == kATaking || st() == kAWriting) cancel_req = 1;
}

CORE_COLD bool asamp_poll() {
  int s = st();
  if (s == kATaking && offline()) {
    render_step();
    return true;
  }
  if (s == kATaking && !offline() && cur >= ntakes && pph == kPNone) {
    pph = cancel_req ? kPCancel : kPGain;
    set_state(cancel_req ? kACancelled : kAWriting);
  }
  switch (pph) {
    case kPNone: return false;
    case kPGain: gain_step(); return true;
    case kPTake: take_step(); return true;
    case kPWav: {
      int r = wr.step();
      if (r < 0) fail("write failed (card full?)");
      else if (r == 0) ++written, ++pi, pph = kPTake;
      return true;
    }
    case kPSfz: sfz_step(); return true;
    case kPCancel: cancel_step(); return true;
  }
  return false;
}

// A Rytm take's timeline (the control core): note on (capture on), hold, note off, tail, capture off, gap
CORE_COLD void asamp_tick() {
  if (rescan_req) {   // the instrument into the INSTRUMENTS pool (the bank last shown; its .sfz read on the card core)
    rescan_req = 0;
    int out = -1;
    int r = sfz[0] ? pools::add_take(pools::kInstr, sfz, &out) : pools::kErrPath;
    log(CORE_STR("asamp: %s -> the INSTRUMENTS pool: %s"), sfz, pools::result_text(r));
    msamp::rescan();
    gr::rescan();
  }
  if (st() != kATaking || offline() || cur >= ntakes) return;
  uint32_t now = now_ms();
  Take& t = takes[cur];
  switch (tph) {
    case kTNext:
      if (cancel_req) {
        cur = ntakes;
        return;
      }
      set_label(cur);
      __atomic_store_n(&cap_w, 0u, __ATOMIC_RELEASE);
      __atomic_store_n(&capturing, (uint8_t)1, __ATOMIC_RELEASE);
      send_note(true, t);
      t0 = now, tph = kTHold;
      break;
    case kTHold:
      if (now - t0 < plan.hold_ms) break;
      send_note(false, t);
      t.note_off = __atomic_load_n(&cap_w, __ATOMIC_ACQUIRE);
      t0 = now, tph = kTTail;
      break;
    case kTTail:
      if (now - t0 < plan.tail_ms && __atomic_load_n(&cap_w, __ATOMIC_ACQUIRE) < t.cap) break;
      __atomic_store_n(&capturing, (uint8_t)0, __ATOMIC_RELEASE);
      t.len = __atomic_load_n(&cap_w, __ATOMIC_ACQUIRE);
      t0 = now, tph = kTGap;
      break;
    case kTGap:
      if (now - t0 < plan.gap_ms) break;
      ++cur, tph = kTNext;
      break;
  }
}

CORE_COLD void asamp_in_block(const int32_t* f, int n, int stride) {
  if (!__atomic_load_n(&capturing, __ATOMIC_ACQUIRE)) return;
  int c = cur;
  if (c >= ntakes) return;
  Take& t = takes[c];
  uint32_t w = cap_w;
  int c0 = in_channel(0), c1 = nch == 2 ? in_channel(1) : -1;
  float* x0 = plane(t, 0);
  float* x1 = nch == 2 ? plane(t, 1) : nullptr;
  for (int i = 0; i < n && w < t.cap; ++i, ++w) {
    x0[w] = (float)f[i * stride + c0] * (1.0f / 2147483648.0f);
    if (x1) x1[w] = (float)f[i * stride + c1] * (1.0f / 2147483648.0f);
  }
  __atomic_store_n(&cap_w, w, __ATOMIC_RELEASE);
}

CORE_COLD bool asamp_wants_in() { return __atomic_load_n(&capturing, __ATOMIC_ACQUIRE) != 0; }

CORE_COLD void asamp_status(AStatus* o) {
  memset(o, 0, sizeof *o);
  o->state = st();
  o->done = st() == kATaking ? cur : ntakes;
  o->total = ntakes;
  o->written = written;
  memcpy(o->label, label, sizeof o->label);
  memcpy(o->name, name, sizeof o->name);
  memcpy(o->sfz, sfz, sizeof o->sfz);
  memcpy(o->error, error, sizeof o->error);
  o->lat_ms = lat_ms_used;
  o->speed = speed;
  o->loops = loops;
  o->gain_db = gain_db;
  o->kb = out_kb;
  o->zones = plan.src == kATrack && !plan.chro ? 1 : (plan.hi - plan.lo) / plan.step + 1;
  o->offline = offline();
  o->layers = plan.nvel;
  o->rr = plan.rr;
}

CORE_COLD int asamp_json(char* out, int cap) {
  AStatus s;
  asamp_status(&s);
  return sniprintf(out, cap, "{\"state\":%d,\"done\":%d,\"total\":%d,\"written\":%d,\"label\":\"%s\",\"name\":\"%s\","
                   "\"sfz\":\"%s\",\"loops\":%d,\"latMs\":%d,\"error\":\"%s\"}", s.state, s.done, s.total, s.written,
                   s.label, s.name, s.sfz, s.loops, (int)(s.lat_ms + 0.5f), s.error);
}

CORE_COLD void asamp_reset() {
  set_state(kAIdle);
  cfg = Plan();
  pph = kPNone;
  capturing = 0;
  cancel_req = 0;
  rescan_req = 0;
}

// asamp                                  the state and the plan
// asamp=src:fm:PAT|juno:PAT|voice:V|track:T[:CH[:chro]]|ext[:usb|dev|rytm][:CH][:lr|l|r]   (V 1..8, T 1..12, CH 1..16)
// asamp=notes:LO:HI[:STEP]  (numbers or names: c2:c6:3)   asamp=vel:V[,V..]   asamp=rr:N
// asamp=hold:MS  asamp=tail:MS  asamp=gap:MS  asamp=loop:on|off  asamp=lat:MS|auto  asamp=knob:K:V (P1..P8)
// asamp=name:NAME|auto   asamp=start   asamp=cancel
CORE_COLD bool asamp_command(const char* p, char* out, int cap) {
  if (strncmp(p, "asamp", 5) || (p[5] && p[5] != '=')) return false;
  char err[100] = "";
  bool ok = true;
  if (p[5] == '=') {
    const char* v = p + 6;
    if (!strncmp(v, "src:", 4)) {
      const char* a = v + 4;
      if (!strncmp(a, "fm:", 3)) a += 3, cfg.src = kAFm, cfg.pat = scan_int(a);
      else if (!strncmp(a, "juno:", 5)) a += 5, cfg.src = kAJuno, cfg.pat = scan_int(a);
      else if (!strncmp(a, "voice:", 6)) a += 6, cfg.src = kAVoice, cfg.voice = (scan_int(a) - 1) & 7;
      else if (!strncmp(a, "track:", 6)) {
        a += 6;
        int t = scan_int(a) - 1;
        cfg.src = kATrack, cfg.track = t < 0 ? 0 : t >= kATracks ? kATracks - 1 : t, cfg.midi_ch = cfg.track;
        if (*a == ':') ++a, cfg.midi_ch = (scan_int(a) - 1) & 15;
        cfg.chro = !strcmp(a, ":chro");
      } else if (!strcmp(a, "ext") || !strncmp(a, "ext:", 4)) {   // ext[:usb|dev|rytm][:CH][:lr|l|r]
        a += 3;
        cfg.src = kAExt;
        while (*a == ':') {
          ++a;
          if (!strncmp(a, "usb", 3)) cfg.port = kPortUsb, a += 3;
          else if (!strncmp(a, "dev", 3)) cfg.port = kPortDev, a += 3;
          else if (!strncmp(a, "rytm", 4)) cfg.port = kPortRytm, a += 4;
          else if (!strncmp(a, "lr", 2)) cfg.in = kInLR, a += 2;
          else if (*a == 'l') cfg.in = kInL, ++a;
          else if (*a == 'r') cfg.in = kInR, ++a;
          else if (*a >= '0' && *a <= '9') cfg.midi_ch = (scan_int(a) - 1) & 15;
          else break;
        }
        if (*a) ok = false, strcpy(err, "src:ext[:usb|dev|rytm][:CH][:lr|l|r]");
      } else ok = false, strcpy(err, "src:fm:PAT|juno:PAT|voice:V|track:T[:CH[:chro]]|ext[:PORT][:CH][:IN]");
    } else if (!strncmp(v, "notes:", 6)) {
      char b[3][8] = {"", "", ""};
      int k = 0, j = 0;
      for (const char* c = v + 6; *c && k < 3; ++c) {
        if (*c == ':') { ++k, j = 0; continue; }
        if (j < 7) b[k][j++] = *c, b[k][j] = 0;
      }
      int lo, hi;
      if (parse_note(b[0], &lo) && parse_note(b[1], &hi)) {
        cfg.lo = lo, cfg.hi = hi;
        const char* s3 = b[2];
        if (*s3) cfg.step = scan_int(s3);
      } else ok = false, strcpy(err, "notes:LO:HI[:STEP] (c2:c6:3 or 36:84:3)");
    } else if (!strncmp(v, "vel:", 4)) {
      const char* a = v + 4;
      int n = 0;
      while (*a && n < 8) {
        int x = scan_int(a);
        if (x >= 1 && x <= 127) cfg.vels[n++] = (uint8_t)x;
        if (*a == ',') ++a;
        else break;
      }
      if (n) cfg.nvel = n;
      else ok = false, strcpy(err, "vel:V[,V..] (1..127, up to 8)");
    } else if (!strncmp(v, "rr:", 3)) { const char* a = v + 3; cfg.rr = scan_int(a); }
    else if (!strncmp(v, "hold:", 5)) { const char* a = v + 5; cfg.hold_ms = (uint32_t)scan_int(a); }
    else if (!strncmp(v, "tail:", 5)) { const char* a = v + 5; cfg.tail_ms = (uint32_t)scan_int(a); }
    else if (!strncmp(v, "gap:", 4)) { const char* a = v + 4; cfg.gap_ms = (uint32_t)scan_int(a); }
    else if (!strncmp(v, "loop:", 5)) cfg.loop = !strcmp(v + 5, "on");
    else if (!strncmp(v, "lat:", 4)) { const char* a = v + 4; cfg.lat_ms = !strcmp(a, "auto") ? -1 : scan_int(a); }
    else if (!strncmp(v, "knob:", 5)) {
      const char* a = v + 5;
      int kk = scan_int(a);
      if (*a == ':' && kk >= 1 && kk <= 8) ++a, cfg.knobs[kk - 1] = (float)scan_int(a);
      else ok = false, strcpy(err, "knob:K:V (K 1..8)");
    } else if (!strncmp(v, "name:", 5)) {
      if (!strcmp(v + 5, "auto")) cfg.name[0] = 0;
      else safe_name(v + 5, cfg.name, sizeof cfg.name);
    } else if (!strcmp(v, "start")) ok = asamp_start(cfg, err, sizeof err);
    else if (!strcmp(v, "cancel")) asamp_cancel();
    else ok = false, strcpy(err, "asamp=src:|notes:|vel:|rr:|hold:|tail:|loop:|lat:|name:|start|cancel");
  }
  AStatus s;
  asamp_status(&s);
  static const char* const names[] = {"idle", "taking", "writing", "done", "failed", "cancelled"};
  static const char* const srcs[] = {"fm", "juno", "voice", "track", "ext"};
  const Plan& c = cfg;
  int k = sniprintf(out, cap, "asamp%s%s: %s %s, src %s:%d, notes %d..%d step %d, %d vel, %d rr, hold %lu ms, tail %lu ms, "
                    "loop %s", ok ? "" : ": ", ok ? "" : err, names[s.state % 6], s.state == kATaking || s.state == kAWriting ? s.label : "",
                    srcs[c.src < kASources ? c.src : 0], c.src == kAFm || c.src == kAJuno ? c.pat : c.src == kAVoice ? c.voice + 1 :
                    c.src == kATrack ? c.track + 1 : c.midi_ch + 1,
                    c.lo, c.hi, c.step, c.nvel, c.rr, (unsigned long)c.hold_ms, (unsigned long)c.tail_ms, c.loop ? "on" : "off");
  if (k > 0 && k < cap && c.src == kAExt)
    k += sniprintf(out + k, cap - k, ", midi %s ch %d, in %s", kPortNames[c.port % kAPorts], c.midi_ch + 1, kInNames[c.in % 3]);
  if (k > 0 && k < cap && s.sfz[0])
    k += sniprintf(out + k, cap - k, ", last %s (%d samples, %d loops, gain %d dB, x%d real time)", s.sfz, s.written,
                   s.loops, (int)s.gain_db, (int)s.speed);
  if (k > 0 && k < cap && s.error[0]) sniprintf(out + k, cap - k, ", error: %s", s.error);
  return true;
}

}  // namespace samp
}  // namespace core
