// core/fx/amp.cpp - see amp.h: INSERT TYPE 9 AMP. The loader (.namb / .nam), the engine pool, the audio side (offers,
// crossfade, GAIN / TONE / LEV), the control side (the /NAM/ list, the load job, the cap), texts, console, /status.
// TONE3000's A2-Lite engine and .namb reader are compiled in at the end of this file (third_party/nam/embedded, MIT).
#include "fx/amp.h"

#include <math.h>
#include <string.h>

#include "fx/insert.h"
#include "platform.h"
#if CORE_MSAMP
#include "msamp/cardq.h"
#endif

extern "C" {
#include "../../third_party/nam/embedded/namb/namb_reader.h"
}

namespace core {
namespace amp {

namespace {

// ---- atomics (one writer each side)
inline uint32_t acq(const volatile uint32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void rel(volatile uint32_t* p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

struct Inst {
  nam_a2lite_weights_t w;
  nam_a2lite_t e;
  nam_a2lite_state_t st;
  float dc0;   // the capture's output for silence after its prewarm (taken off: silence in, silence out)
};
Inst* g_pool = nullptr;
uint8_t* g_buf = nullptr;   // the file being loaded (kFileMax + NUL): placed with the pool
#ifndef CORE_HEAP_BUFFERS
alignas(64) uint8_t g_static[sizeof(Inst) * kInstances + ((kFileMax + 64) & ~63ul) + 64];
struct Placer {
  Placer() { heap_place(g_static); }
} g_placer;
#endif

// the control core's record of an instance
struct Meta {
  int model;       // the list entry its weights hold, -1 none
  uint32_t gen;    // the list generation it was loaded under
  bool busy;       // a slot's (playing, offered, fading out) or the job's
};
Meta g_meta[kInstances];

// a slot's exchange: the control core offers an instance (or -1: dry), the render core takes it and says when the old
// one is no longer used (done == offer)
struct Xchg {
  volatile int32_t inst;
  volatile uint32_t offer, done;
};
Xchg g_x[kVoices][kSlots];

// the render core's per slot (bus chains included: they run AMP's dry form)
struct Audio {
  int8_t cur = -1, from = -1;
  int fade = 0;
  uint32_t seen = 0;
  bool ran = false;
  float lp = 0, dcx = 0, dcy = 0;
};
Audio g_a[ins::kChains][kSlots];

// the control core's per slot
struct Ctl {
  int cur = -1, prev = -1;   // the instance the slot has (or is offered); the one to free once the offer is taken
  Status st = kIdle;
  int fail_model = -1;
  uint32_t fail_gen = 0;
  Status fail_st = kIdle;
  bool refused = false;      // the cap's refusal counted once
  char pending[kNameMax];    // the card's AMP line, resolved once listed
};
Ctl g_k[kVoices][kSlots];

// the /NAM/ list: two buffers, the readers (any core: texts) take the published one
struct List {
  int n;
  char file[kMaxModels][kNameMax];
  char shrt[kMaxModels][kShort];
};
List g_list[2];
volatile uint32_t g_pub = 0;   // the published buffer (0 / 1)
uint32_t g_gen = 0;            // bumped on every publish
bool g_listed = false;         // a list was published (an empty one too)
Stats g_stats;

const List& list() { return g_list[__atomic_load_n(&g_pub, __ATOMIC_ACQUIRE) & 1]; }

bool same_ci(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
    if (x != y) return false;
  }
  return *a == *b;
}
int cmp_ci(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
    if (x != y || !x) return (int)(unsigned char)x - (int)(unsigned char)y;
  }
}
bool ends_ci(const char* s, const char* ext) {
  size_t a = strlen(s), b = strlen(ext);
  return a > b && same_ci(s + a - b, ext);
}

// a JSON number (or NaN / Infinity: not finite) to a double; false: not a number
bool parse_num(const char* s, double* out, bool* finite) {
  *finite = true;
  bool neg = false;
  if (*s == '-' || *s == '+') neg = *s == '-', ++s;
  if (!strcmp(s, "NaN") || !strcmp(s, "Infinity") || !strcmp(s, "nan") || !strcmp(s, "inf")) {
    *finite = false;
    *out = 0;
    return true;
  }
  uint64_t m = 0;
  int ex = 0, nd = 0;
  bool any = false;
  for (; *s >= '0' && *s <= '9'; ++s, any = true)
    if (nd < 19) m = m * 10 + (uint64_t)(*s - '0'), nd += m != 0;
    else ++ex;
  if (*s == '.') {
    ++s;
    for (; *s >= '0' && *s <= '9'; ++s, any = true)
      if (nd < 19) m = m * 10 + (uint64_t)(*s - '0'), nd += m != 0, --ex;
  }
  if (!any) return false;
  if (*s == 'e' || *s == 'E') {
    ++s;
    bool en = false;
    if (*s == '-' || *s == '+') en = *s == '-', ++s;
    if (*s < '0' || *s > '9') return false;
    int e = 0;
    for (; *s >= '0' && *s <= '9'; ++s)
      if (e < 10000) e = e * 10 + (*s - '0');
    ex += en ? -e : e;
  }
  if (*s) return false;
  static const double p10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  double v = (double)m;
  if (m) {
    while (ex > 22) v *= 1e22, ex -= 22;
    while (ex < -22) v /= 1e22, ex += 22;
    v = ex >= 0 ? v * p10[ex] : v / p10[-ex];
  }
  if (!(v - v == 0.0)) *finite = false;
  *out = neg ? -v : v;
  return true;
}

inline float db_gain(float db) { return ins::fast_exp2(db * 0.16609640f); }   // 10^(db / 20)

}  // namespace

// ---------------------------------------------------------------------------------------------------- texts

const char* status_short(int s) {
  static const char* const t[kStatuses] = {"", "", "LOADING", "NO NAM", "NOTLITE", "BADFILE", "NOT 48K", "MAX 6", "NO RAM"};
  static_assert(kMaxRunning == 6, "the MAX text names the cap");
  return s >= 0 && s < kStatuses ? t[s] : "";
}
const char* status_long(int s) {
  static const char* const t[kStatuses] = {
      "not an AMP slot",
      "playing",
      "loading the capture",
      "no .nam / .namb file in /NAM/",
      "not an A2-Lite model: LITE MODELS ONLY",
      "not a NAM model this reads (damaged, cut short or not .nam / .namb)",
      "the capture is not 48 kHz (no resampler)",
      "6 AMP inserts play already (the CPU cap)",
      "no memory for the AMP engines"};
  return s >= 0 && s < kStatuses ? t[s] : "?";
}
Status parse_status(Parse p) {
  switch (p) {
    case kOk: return kPlaying;
    case kErrNotLite: return kNotLite;
    case kErrRate: return kNot48k;
    default: return kBadFile;
  }
}
bool has_ext(const char* n) { return ends_ci(n, ".namb") || ends_ci(n, ".nam"); }
void short_name(const char* f, char* o) {
  int k = 0;
  const char* dot = strrchr(f, '.');
  for (const char* p = f; *p && p != dot && k < kShort - 1; ++p) {
    char c = *p;
    if (c == ' ' || c == '_' || c == '-' || (unsigned char)c < 32 || c == '"' || c == '\\') continue;
    o[k++] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
  }
  if (!k) o[k++] = '?';
  o[k] = 0;
}

// ---------------------------------------------------------------------------------------------------- the loader

Parse parse_namb(const uint8_t* d, uint32_t n, float* w) {
  namb_model_t m;
  namb_status_t s = namb_parse(d, n, &m);
  if (s == NAMB_OK) s = namb_verify_crc(d, n);
  if (s == NAMB_ERR_TRUNCATED || s == NAMB_ERR_SIZE) return kErrTruncated;
  if (s == NAMB_ERR_CRC) return kErrCrc;
  if (s == NAMB_ERR_ARCH || s == NAMB_ERR_SHAPE) return kErrNotLite;
  if (s != NAMB_OK) return kErrFormat;
  if (namb_check_a2lite(&m) != NAMB_OK) return kErrNotLite;
  if (m.sample_rate > 0 && m.sample_rate != 48000.0) return kErrRate;
  for (int i = 0; i < kWeights; ++i) {
    float v = m.weights[i];
    if (!(v - v == 0.0f)) return kErrNotFinite;
    w[i] = v;
  }
  return kOk;
}

void JsonScan::begin(const char* text, uint32_t n, float* w) {
  *this = JsonScan();
  p_ = text, n_ = n, w_ = w;
}

// A model object: the top one (root 1), or a SlimmableContainer's submodel (top.config.submodels[i].model, root 5).
// Its fields are found at depths relative to it (o = root - 1).
bool JsonScan::open(char c) {
  if (depth_ >= (int)sizeof stack_) return false;
  if (depth_ == 0 && (c != '{' || top_)) return false;   // one object at the top
  const int o = root_ - 1;
  if (depth_ == 2 + o && !strcmp(key_[o], "config") && !strcmp(key_[o + 1], "condition_dsp")) cond_ = true;
  if (depth_ == 3 + o && stack_[2 + o] == '[' && !strcmp(key_[o], "config") && !strcmp(key_[o + 1], "layers") && c == '{') ++layers_;
  if (depth_ == 4 && c == '{' && stack_[2] == '[' && !strcmp(key_[0], "config") && !strcmp(key_[1], "submodels") &&
      !strcmp(key_[3], "model")) {   // a submodel: a candidate of its own
    root_ = 5;
    wavenet_ = other_arch_ = cond_ = rate_bad_ = false, finite_ = true, wn_ = 0, layers_ = 0, channels_ = -1;
  }
  stack_[depth_] = c;
  want_key_[depth_] = c == '{';
  key_[depth_][0] = 0;
  ++depth_;
  return true;
}

// the candidate's verdict (the top object's, or a submodel's when it closes)
Parse JsonScan::verdict() const {
  if (!wavenet_ && !other_arch_) return kErrFormat;   // no "architecture": not a NAM model
  if (other_arch_ || cond_ || layers_ != 1 || (channels_ != -1 && channels_ != 3) || wn_ != kWeights) return kErrNotLite;
  if (!finite_) return kErrNotFinite;
  if (rate_bad_) return kErrRate;
  return kOk;
}
void JsonScan::close_model() {   // a submodel's object closed (depth back to 4)
  Parse p = verdict();
  if (p == kOk) locked_ = true;   // the first A2-Lite submodel: its weights stay in w_
  else if (sub_res_ == kErrFormat || p == kErrNotLite) sub_res_ = p;
  ++subs_;
  root_ = 1;
}

void JsonScan::value(const char* t, int len, bool str) {
  (void)len;
  const int d = depth_;
  if (d < 1) {
    bad_ = true;
    return;
  }
  const char* k = stack_[d - 1] == '{' ? key_[d - 1] : "";
  bool lit_null = !str && !strcmp(t, "null"), lit = !str && (lit_null || !strcmp(t, "true") || !strcmp(t, "false"));
  double v = 0;
  bool fin = true;
  if (!str && !lit && !parse_num(t, &v, &fin)) {
    bad_ = true;
    return;
  }
  if (d == 1 && !strcmp(k, "architecture") && str) slim_ = !strcmp(t, "SlimmableContainer");
  const int o = root_ - 1, r = d - o;   // r: the depth within the model object
  if (r < 1) return;
  if (r == 1 && !strcmp(k, "architecture") && str) {
    if (!(o == 0 && slim_)) wavenet_ = !strcmp(t, "WaveNet"), other_arch_ = !wavenet_;
  } else if (r == 1 && !strcmp(k, "sample_rate") && !str && !lit) {
    if (v > 0 && v != 48000.0) rate_bad_ = true;
  } else if (r == 2 && stack_[1 + o] == '[' && !strcmp(key_[o], "weights")) {
    if (str || lit) {
      bad_ = true;
      return;
    }
    if (locked_) return;   // a Lite submodel is taken: the rest only checked for syntax
    if (!fin) finite_ = false;
    if (wn_ < kWeights) w_[wn_] = (float)v;
    if (wn_ < 1 << 30) ++wn_;
  } else if (r == 2 && !strcmp(key_[o], "config") && !strcmp(k, "condition_dsp") && !lit_null) {
    cond_ = true;
  } else if (r == 4 && stack_[2 + o] == '[' && stack_[3 + o] == '{' && !strcmp(key_[o], "config") &&
             !strcmp(key_[o + 1], "layers") && layers_ == 1 && !strcmp(k, "channels") && !str && !lit) {
    channels_ = (int)v;
  }
}

int JsonScan::step(uint32_t budget) {
  if (!p_) return res_ = kErrFormat, -1;
  uint32_t end = n_ - i_ < budget ? n_ : i_ + budget;
  if (failed_) return -1;
  auto fail = [&](Parse p) {
    res_ = p;
    failed_ = true;
    i_ = n_;
    return -1;
  };
  for (; i_ < end; ++i_) {
    char ch = p_[i_];
    if (in_str_) {
      if (esc_) {
        esc_ = false;
        if (tl_ < (int)sizeof tok_ - 1) tok_[tl_++] = ch;
      } else if (ch == '\\') {
        esc_ = true;
      } else if (ch == '"') {
        in_str_ = false;
        tok_[tl_] = 0;
        if (depth_ > 0 && stack_[depth_ - 1] == '{' && want_key_[depth_ - 1]) {
          sniprintf(key_[depth_ - 1], sizeof key_[0], "%s", tok_);
          want_key_[depth_ - 1] = false;
        } else {
          value(tok_, tl_, true);
          if (bad_) return fail(kErrFormat);
        }
      } else if (tl_ < (int)sizeof tok_ - 1) {
        tok_[tl_++] = ch;
      }
      continue;
    }
    if (in_tok_) {
      bool part = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '.' ||
                  ch == '-' || ch == '+';
      if (part) {
        if (tl_ >= (int)sizeof tok_ - 1) return fail(kErrFormat);
        tok_[tl_++] = ch;
        continue;
      }
      in_tok_ = false;
      tok_[tl_] = 0;
      value(tok_, tl_, false);
      if (bad_) return fail(kErrFormat);
    }
    switch (ch) {
      case ' ': case '\t': case '\n': case '\r': break;
      case '"':
        if (!depth_) return fail(kErrFormat);
        in_str_ = true, tl_ = 0;
        break;
      case '{': case '[':
        if (!open(ch)) return fail(kErrFormat);
        break;
      case '}': case ']':
        if (!depth_ || stack_[depth_ - 1] != (ch == '}' ? '{' : '[')) return fail(kErrFormat);
        if (--depth_ == 0) ++top_;
        if (root_ == 5 && depth_ == 4) close_model();
        break;
      case ':':
        if (!depth_ || stack_[depth_ - 1] != '{' || want_key_[depth_ - 1]) return fail(kErrFormat);
        break;
      case ',':
        if (!depth_) return fail(kErrFormat);
        if (stack_[depth_ - 1] == '{') want_key_[depth_ - 1] = true;
        break;
      case 0:
        if (depth_) return fail(kErrTruncated);
        i_ = n_ - 1;   // a NUL after the object: the end
        break;
      default:
        if (!depth_ || !((ch >= '0' && ch <= '9') || ch == '-' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
          return fail(kErrFormat);
        in_tok_ = true, tl_ = 0, tok_[tl_++] = ch;
        break;
    }
  }
  if (i_ < n_) return 1;
  failed_ = true;   // (the result below is final)
  if (in_str_ || depth_) return res_ = kErrTruncated, -1;
  if (in_tok_ || top_ != 1) return res_ = kErrFormat, -1;
  if (slim_) {   // TONE3000's A2 files: a SlimmableContainer of an A2-Lite and an A2-Full WaveNet
    if (!locked_) return res_ = subs_ ? sub_res_ : kErrFormat, -1;
  } else {
    Parse p = verdict();
    if (p != kOk) return res_ = p, -1;
  }
  failed_ = false;
  res_ = kOk;
  i_ = n_;
  return 0;
}

// ---------------------------------------------------------------------------------------------------- the pool

namespace {
constexpr unsigned long kBufBytes = (kFileMax + 64) & ~63ul;
}
unsigned long heap_bytes() { return (unsigned long)(sizeof(Inst) * kInstances + kBufBytes + 64); }
void heap_place(void* mem) {
  uintptr_t p = ((uintptr_t)mem + 63) & ~(uintptr_t)63;
  g_pool = (Inst*)p;
  g_buf = (uint8_t*)(p + sizeof(Inst) * kInstances);
  for (int i = 0; i < kInstances; ++i) {
    memset(&g_pool[i], 0, sizeof(Inst));
    g_meta[i] = Meta{-1, 0, false};
  }
}
bool placed() { return g_pool != nullptr; }

// ---------------------------------------------------------------------------------------------------- the audio side

namespace {
// take slot (c, s)'s offer, if one waits: from what plays to the offered instance over kXfadeFrames (fade: false, at once)
inline void take(int c, int s, Audio& a, bool fade) {
  Xchg& x = g_x[c][s];
  uint32_t q = acq(&x.offer);
  if (q == a.seen || a.fade > 0) return;   // nothing new, or a fade still running (the control core waits for done)
  int ni = __atomic_load_n(&x.inst, __ATOMIC_RELAXED);
  a.seen = q;
  ++g_stats.taken;
  if (fade && ni != a.cur) {
    a.from = a.cur, a.cur = (int8_t)ni, a.fade = kXfadeFrames;
    ++g_stats.xfades;
    return;
  }
  a.cur = (int8_t)ni, a.from = -1, a.fade = 0;
  rel(&x.done, q);
}
inline void model(int i, const float* u, float* o, int m) {
  Inst& I = g_pool[i];
  const float* in[1] = {u};
  float* out[1] = {o};
  nam_a2lite_process(&I.e, in, out, m);
  const float d = I.dc0;
  for (int k = 0; k < m; ++k) o[k] -= d;
}
}  // namespace

void fresh(int c, int s) {
  if (c < 0 || c >= ins::kChains || s < 0 || s >= kSlots) return;
  Audio& a = g_a[c][s];
  a.lp = a.dcx = a.dcy = 0;
}

void run(int c, int s, const float* p, const float* x, const float* r, float* y0, float* y1, int m) {
  if (m > NAM_A2LITE_MAX_BUFFER_SIZE) m = NAM_A2LITE_MAX_BUFFER_SIZE;
  float u[NAM_A2LITE_MAX_BUFFER_SIZE], o[NAM_A2LITE_MAX_BUFFER_SIZE];
  for (int i = 0; i < m; ++i) u[i] = ins::flush(r ? 0.5f * (x[i] + r[i]) : x[i]);   // AMP is mono
  Audio& a = g_a[c < 0 ? 0 : c >= ins::kChains ? ins::kChains - 1 : c][s & 1];
  if (c >= 0 && c < kVoices && g_pool) {
    a.ran = true;
    take(c, s, a, true);
  }
  if (a.cur >= 0 || a.from >= 0) {
    // the capture(s): GAIN in front, dry where a side has none
    float g = db_gain(-18.0f + 36.0f * p[1]), v[NAM_A2LITE_MAX_BUFFER_SIZE];
    for (int i = 0; i < m; ++i) v[i] = u[i] * g;
    if (a.cur >= 0) model(a.cur, v, o, m);
    else memcpy(o, u, sizeof(float) * m);
    if (a.fade > 0) {
      float q[NAM_A2LITE_MAX_BUFFER_SIZE];
      if (a.from >= 0) model(a.from, v, q, m);
      else memcpy(q, u, sizeof(float) * m);
      for (int i = 0; i < m; ++i) {
        float w = 1.0f - (float)(a.fade - i) * (1.0f / kXfadeFrames);   // the new side's share
        w = w < 0.0f ? 0.0f : w;
        o[i] = q[i] + w * (o[i] - q[i]);
      }
      a.fade -= m;
      if (a.fade <= 0) {
        a.fade = 0, a.from = -1;
        rel(&g_x[c][s].done, a.seen);
      }
    }
  } else {
    memcpy(o, u, sizeof(float) * m);
  }
  // after it: a 5 Hz DC block, TONE (a tilt at 650 Hz, +-6 dB), LEV
  const float t = ins::clampf((p[2] * 127.0f - 64.0f) * (1.0f / 63.0f), -1.0f, 1.0f);
  const float gl = ins::fast_exp2(-t), gh = ins::fast_exp2(t), lev = db_gain(-36.0f + 42.0f * p[3]);
  const float al = 0.0815f;            // 1 - exp(-2 pi 650 / 48000)
  const float R = 0.99934585f;         // 1 - 2 pi 5 / 48000
  float lp = a.lp, dx = a.dcx, dy = a.dcy;
  for (int i = 0; i < m; ++i) {
    float z = o[i] - dx + R * dy;
    dx = o[i], dy = z;
    lp += al * (z - lp);
    float out = (gl * lp + gh * (z - lp)) * lev;
    y0[i] = out;
    if (r) y1[i] = out;
  }
  a.lp = ins::flush(lp), a.dcx = ins::flush(dx), a.dcy = ins::flush(dy);
}

// voice_out's (insert.cpp) every block, before the chain: an offer to a slot that did not run AMP last block is taken
// at once (nothing plays it: no fade), a fade left half-way finishes
void service(int vi) {
  if (vi < 0 || vi >= kVoices || !g_pool) return;
  for (int s = 0; s < kSlots; ++s) {
    Audio& a = g_a[vi][s];
    if (!a.ran) {
      if (a.fade > 0) a.fade = 0, a.from = -1, rel(&g_x[vi][s].done, a.seen);
      take(vi, s, a, false);
    }
    a.ran = false;
  }
}

// ---------------------------------------------------------------------------------------------------- the list

int models() { return list().n; }
const char* model_file(int i) {
  const List& l = list();
  return i >= 0 && i < l.n ? l.file[i] : "";
}
const char* model_short(int i) {
  const List& l = list();
  return i >= 0 && i < l.n ? l.shrt[i] : "";
}
int model_for_knob(int v) {
  int n = models();
  if (n <= 0) return -1;
  v = v < 0 ? 0 : v > 127 ? 127 : v;
  return v * n / 128;
}
int knob_for_model(int i) {
  int n = models();
  if (n <= 0 || i < 0) return 0;
  int lo = (i * 128 + n - 1) / n, hi = ((i + 1) * 128 + n - 1) / n - 1;   // the values that pick i
  return (lo + hi) / 2;
}
int find_model(const char* f) {
  const List& l = list();
  for (int i = 0; i < l.n; ++i)
    if (same_ci(l.file[i], f)) return i;
  return -1;
}

// the list being built (control core) into the unpublished buffer
namespace {
List& building() { return g_list[(__atomic_load_n(&g_pub, __ATOMIC_RELAXED) & 1) ^ 1]; }
void publish() {
  List& b = building();
  for (int i = 1; i < b.n; ++i)   // sorted by name, case ignored (insertion: 64 at most)
    for (int j = i; j > 0 && cmp_ci(b.file[j - 1], b.file[j]) > 0; --j) {
      char t[kNameMax];
      memcpy(t, b.file[j], kNameMax), memcpy(b.file[j], b.file[j - 1], kNameMax), memcpy(b.file[j - 1], t, kNameMax);
    }
  for (int i = 0; i < b.n; ++i) short_name(b.file[i], b.shrt[i]);
  __atomic_store_n(&g_pub, g_pub ^ 1u, __ATOMIC_RELEASE);
  ++g_gen, g_listed = true, ++g_stats.lists;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------- the control side

Status status(int c, int s) { return c >= 0 && c < kVoices && s >= 0 && s < kSlots ? g_k[c][s].st : kIdle; }
void knob_text(int c, int s, int v, char* b, int cap) {
  Status st = status(c, s);
  if (c >= kVoices) {
    sniprintf(b, cap, "V ONLY");
    return;
  }
  int i = model_for_knob(v);
  if (st == kPlaying || st == kIdle) {
    if (i >= 0) sniprintf(b, cap, "%s", model_short(i));
    else sniprintf(b, cap, "%s", g_listed ? status_short(kNoFiles) : status_short(kLoading));
    return;
  }
  // loading a capture the knob points at: its name with a mark; a refusal: the reason
  if (st == kLoading && i >= 0) {
    sniprintf(b, cap, "%.6s~", model_short(i));
    return;
  }
  sniprintf(b, cap, "%s", status_short(st));
}
void want_file(int c, int s, const char* f) {
  if (c < 0 || c >= kVoices || s < 0 || s >= kSlots) return;
  sniprintf(g_k[c][s].pending, kNameMax, "%s", f ? f : "");
}
const char* slot_file(int c, int s) {
  if (c < 0 || c >= kVoices || s < 0 || s >= kSlots) return "";
  if (g_k[c][s].pending[0]) return g_k[c][s].pending;   // not listed yet: kept as the card has it
  return model_file(model_for_knob(ins::param(c, s, 0)));
}
int running() {
  int n = 0;
  for (int c = 0; c < kVoices; ++c)
    for (int s = 0; s < kSlots; ++s) n += g_k[c][s].cur >= 0;
  return n;
}

namespace {

void free_inst(int i) {
  if (i >= 0 && i < kInstances) g_meta[i].busy = false;
}
void offer(int c, int s, int inst) {
  Ctl& k = g_k[c][s];
  Xchg& x = g_x[c][s];
  k.prev = k.cur, k.cur = inst;
  __atomic_store_n(&x.inst, inst, __ATOMIC_RELAXED);
  rel(&x.offer, x.offer + 1);
  ++g_stats.offers;
}

// ---- the load job (one at a time)
enum Phase : uint8_t { kNone, kList, kOpen, kReading, kClose, kParse, kInit, kPrewarm };   // (kReading: core::kRead is a file mode)
struct Job {
  Phase ph = kNone;
  bool list_wanted = false;
  int c = -1, s = -1, model = -1, inst = -1;
  uint32_t gen = 0, size = 0, got = 0;
  int warm = 0;
  bool json = false;
  Parse res = kOk;
  Phase after = kNone;
  char path[96];
  JsonScan js;
#if CORE_MSAMP
  cardq::Req rq;
  File f;
#endif
};
Job g_job;
float g_w[kWeights];
bool g_card = false;

void job_fail(Parse p) {
  Job& j = g_job;
  Ctl& k = g_k[j.c][j.s];
  k.fail_model = j.model, k.fail_gen = j.gen, k.fail_st = parse_status(p), k.st = k.fail_st;
  free_inst(j.inst);
  log(CORE_STR("amp: %s %c: %s: %s"), ins::chain_name(j.c), 'A' + j.s, j.path, status_long(k.fail_st));
}

bool job_step() {
  Job& j = g_job;
  switch (j.ph) {
#if CORE_MSAMP
    case kList: {
      // final review (parked 6b concern 4): the listing through the card queue (cardq kListReq, run by the card core),
      // never f_opendir / f_readdir here on the control core (before: at boot and at every amp=rescan, 8 entries
      // a step). Its names land in g_buf, the load's buffer - free meanwhile: the job is one at a time
      if (g_buf && !cardq::async(&j.rq, cardq::kListReq, nullptr, kFolder, 0, g_buf, (uint32_t)kBufBytes)) return true;
      int32_t got = g_buf ? j.rq.got : cardq::kListUnread;
      j.rq.state = cardq::kFree;
      j.ph = kNone;
      List& b = building();
      b.n = 0;
      if (got == cardq::kListMissing) {
        publish();
        log(CORE_STR("amp: no %s folder on the card"), kFolder);
        return true;
      }
      if (got < 0) {   // kListUnread: a card error, or its names over the buffer - an empty list (amp=rescan again)
        publish();
        log(CORE_STR("amp: %s could not be listed (a card error, or names over %u bytes): no captures"), kFolder,
            (unsigned)kBufBytes);
        return true;
      }
      // (the listing has no file / folder flag: a folder named like a capture would be listed, and fail to open)
      for (uint32_t at = 0; at < (uint32_t)got;) {
        const char* nm = (const char*)g_buf + at;
        size_t len = strlen(nm);
        at += (uint32_t)len + 1;
        if (nm[0] == '.' || !has_ext(nm) || len >= (size_t)kNameMax || b.n >= kMaxModels) continue;
        sniprintf(b.file[b.n++], kNameMax, "%s", nm);
      }
      publish();
      log(CORE_STR("amp: %d captures in %s"), list().n, kFolder);
      return true;
    }
    case kOpen: {
      if (!cardq::async(&j.rq, cardq::kOpenReq, &j.f, j.path, 0, nullptr, 0)) return true;
      if (j.rq.got != 1) {
        j.ph = kNone;
        job_fail(kErrFormat);
        return true;
      }
      uint64_t sz = j.f.fileSize();
      j.res = kOk;
      if (sz > kFileMax) j.res = kErrNotLite;   // too big for an A2-Lite capture
      else if (sz < 80) j.res = kErrTruncated;
      j.size = (uint32_t)sz, j.got = 0;
      j.rq.state = cardq::kFree;
      j.after = j.res == kOk ? kParse : kNone;
      j.ph = j.res == kOk ? kReading : kClose;
      return true;
    }
    case kReading: {
      uint32_t n = j.size - j.got;
      if (n > kReadPiece) n = kReadPiece;
      if (!cardq::async(&j.rq, cardq::kReadReq, &j.f, nullptr, j.got, g_buf + j.got, n)) return true;
      j.rq.state = cardq::kFree;
      if (j.rq.got <= 0) {
        j.res = kErrTruncated, j.after = kNone, j.ph = kClose;
        return true;
      }
      j.got += (uint32_t)j.rq.got;
      if (j.got >= j.size) j.ph = kClose;
      return true;
    }
    case kClose: {
      if (!cardq::async(&j.rq, cardq::kCloseReq, &j.f, nullptr, 0, nullptr, 0)) return true;
      j.rq.state = cardq::kFree;
      if (j.after == kNone) {
        j.ph = kNone;
        job_fail(j.res);
        return true;
      }
      g_buf[j.size] = 0;
      j.json = !ends_ci(j.path, ".namb");
      if (j.json) j.js.begin((const char*)g_buf, j.size, g_w);
      j.ph = kParse;
      return true;
    }
#endif
    case kParse: {
      Parse p;
      if (!j.json) {
        p = parse_namb(g_buf, j.size, g_w);
      } else {
        int r = j.js.step(kParsePer);
        if (r == 1) return true;
        p = j.js.result();
      }
      if (p != kOk) {
        j.ph = kNone;
        job_fail(p);
        return true;
      }
      j.ph = kInit;
      return true;
    }
    case kInit: {
      Inst& I = g_pool[j.inst];
      if (j.model >= 0 && !(g_meta[j.inst].model == j.model && g_meta[j.inst].gen == j.gen)) {
        if (nam_a2lite_load_weights(&I.w, g_w, kWeights) != 0) {
          j.ph = kNone;
          job_fail(kErrNotLite);
          return true;
        }
        g_meta[j.inst].model = j.model, g_meta[j.inst].gen = j.gen;
        nam_a2lite_init(&I.e, &I.w, &I.st);
        ++g_stats.loads;
      } else {
        nam_a2lite_reset(&I.e);
        ++g_stats.reuses;
      }
      j.warm = 0;
      j.ph = kPrewarm;
      return true;
    }
    case kPrewarm: {
      Inst& I = g_pool[j.inst];
      float z[NAM_A2LITE_MAX_BUFFER_SIZE] = {0}, o[NAM_A2LITE_MAX_BUFFER_SIZE];
      const float* in[1] = {z};
      float* out[1] = {o};
      for (int k = 0; k < kPrewarmPer && j.warm < NAM_A2LITE_PREWARM_SAMPLES; k += NAM_A2LITE_MAX_BUFFER_SIZE) {
        int n = NAM_A2LITE_PREWARM_SAMPLES - j.warm;
        if (n > NAM_A2LITE_MAX_BUFFER_SIZE) n = NAM_A2LITE_MAX_BUFFER_SIZE;
        nam_a2lite_process(&I.e, in, out, n);
        j.warm += n;
      }
      if (j.warm < NAM_A2LITE_PREWARM_SAMPLES) return true;
      nam_a2lite_process(&I.e, in, out, 1);   // the steady output for silence: taken off in run()
      I.dc0 = o[0];
      j.ph = kNone;
      // still wanted? (the knob / a lock moved on, the TYPE changed: the instance is kept for a later reuse)
      Ctl& k = g_k[j.c][j.s];
      bool want = ins::control.type[j.c][j.s] == ins::kAmp && model_for_knob(ins::control.play[j.c][j.s][0]) == j.model &&
                  j.gen == g_gen;
      if (!want || acq(&g_x[j.c][j.s].done) != g_x[j.c][j.s].offer) {
        free_inst(j.inst);
        return true;
      }
      __atomic_thread_fence(__ATOMIC_RELEASE);
      offer(j.c, j.s, j.inst);
      k.st = kPlaying, k.refused = false;
      log(CORE_STR("amp: %s %c: %s"), ins::chain_name(j.c), 'A' + j.s, model_file(j.model));
      return true;
    }
    default: break;
  }
  return false;
}

int pick_inst(int model) {
  int any = -1, empty = -1;
  for (int i = 0; i < kInstances; ++i) {
    if (g_meta[i].busy) continue;
    if (g_meta[i].model == model && g_meta[i].gen == g_gen) return i;   // its weights already: a reset and a prewarm
    if (g_meta[i].model < 0 && empty < 0) empty = i;
    if (any < 0) any = i;
  }
  return empty >= 0 ? empty : any;
}

}  // namespace

void begin(bool card_ok) {
  g_card = card_ok;
  g_job.list_wanted = true;
}
void rescan() { g_job.list_wanted = true; }

bool step() {
  Job& j = g_job;
  if (j.ph != kNone) return job_step();
#if CORE_MSAMP
  if (j.list_wanted && g_card) {
    j.list_wanted = false;
    j.ph = kList;
    return job_step();
  }
#endif
  if (j.list_wanted && !g_card) {   // no card: an empty list
    j.list_wanted = false;
    building().n = 0;
    publish();
  }
  bool did = false;
  for (int c = 0; c < kVoices; ++c)
    for (int s = 0; s < kSlots; ++s) {
      Ctl& k = g_k[c][s];
      Xchg& x = g_x[c][s];
      const bool is_amp = ins::control.type[c][s] == ins::kAmp;
      if (acq(&x.done) != x.offer) {   // the render core has not taken the last offer yet
        if (!is_amp) k.st = kIdle;
        continue;
      }
      if (k.prev >= 0) free_inst(k.prev), k.prev = -1;
      if (k.pending[0] && g_listed) {   // the card's AMP line, now that /NAM/ is listed
        int i = find_model(k.pending);
        if (i >= 0) ins::set_param(c, s, 0, knob_for_model(i));
        else log(CORE_STR("amp: %s %c: %s is not in %s"), ins::chain_name(c), 'A' + s, k.pending, kFolder);
        k.pending[0] = 0;
      }
      if (!is_amp) {
        k.st = kIdle, k.refused = false;
        if (k.cur >= 0) offer(c, s, -1), did = true;
        continue;
      }
      if (!g_pool) {
        k.st = kNoMem;
        continue;
      }
      if (!g_listed) {
        k.st = kLoading;
        continue;
      }
      const int want = model_for_knob(ins::control.play[c][s][0]);
      if (want < 0) {
        k.st = kNoFiles;
        if (k.cur >= 0) offer(c, s, -1), did = true;
        continue;
      }
      if (k.cur >= 0 && g_meta[k.cur].model == want && g_meta[k.cur].gen == g_gen) {
        k.st = kPlaying;
        continue;
      }
      if (k.fail_model == want && k.fail_gen == g_gen) {
        k.st = k.fail_st;
        if (k.cur >= 0) offer(c, s, -1), did = true;
        continue;
      }
      if (k.cur < 0 && running() >= kMaxRunning) {
        if (!k.refused) ++g_stats.refused, k.refused = true;
        k.st = kCap;
        continue;
      }
      int i = pick_inst(want);
      if (i < 0) {   // every instance plays or fades: a little later
        k.st = kLoading;
        continue;
      }
      g_meta[i].busy = true;
      j = Job();
      j.c = c, j.s = s, j.model = want, j.inst = i, j.gen = g_gen;
      sniprintf(j.path, sizeof j.path, "%s/%s", kFolder, model_file(want));
      k.st = kLoading;
      j.ph = g_meta[i].model == want && g_meta[i].gen == g_gen ? kInit : kNone;
#if CORE_MSAMP
      if (j.ph == kNone) j.rq.state = cardq::kFree, j.ph = kOpen;
#endif
      if (j.ph == kNone) {   // no card port (the portable tests): only a reuse could play
        free_inst(i);
        k.st = kBadFile;
        continue;
      }
      return job_step();
    }
  return did;
}

// ---------------------------------------------------------------------------------------------------- console, json

bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "amp", 3) != 0 || (p[3] && p[3] != '=')) return false;
  if (p[3] == '=' && same_ci(p + 4, "rescan")) {
    rescan();
    sniprintf(o, cap, "amp: listing %s again", kFolder);
    return true;
  }
  if (p[3]) {
    sniprintf(o, cap, "amp (the AMP inserts), amp=rescan (list %s again)", kFolder);
    return true;
  }
  int n = sniprintf(o, cap, "amp: %d captures in %s, %d of %d playing, loads %u reuses %u refused %u",
                   models(), kFolder, running(), kMaxRunning, (unsigned)g_stats.loads, (unsigned)g_stats.reuses,
                   (unsigned)g_stats.refused);
  for (int c = 0; c < kVoices && n < cap; ++c)
    for (int s = 0; s < kSlots && n < cap; ++s) {
      if (g_k[c][s].st == kIdle) continue;
      int k = sniprintf(o + n, cap - n, "; %s %c %s: %s", ins::chain_name(c), 'A' + s, slot_file(c, s),
                       status_long(g_k[c][s].st));
      if (k < 0) break;
      n += k;
    }
  return true;
}

int json(char* o, int cap) {
  if (cap <= 0) return 0;
  int n = sniprintf(o, cap, "\"amp\":{\"models\":%d,\"running\":%d,\"cap\":%d,\"slots\":[", models(), running(), kMaxRunning);
  bool first = true;
  for (int c = 0; c < kVoices; ++c)
    for (int s = 0; s < kSlots; ++s) {
      if (n >= cap) return cap;
      if (g_k[c][s].st == kIdle) continue;
      char f[kNameMax];
      int q = 0;
      for (const char* p = slot_file(c, s); *p && q < kNameMax - 1; ++p)
        if (*p != '"' && *p != '\\' && (unsigned char)*p >= 32) f[q++] = *p;
      f[q] = 0;
      int k = sniprintf(o + n, cap - n, "%s{\"c\":\"%s\",\"s\":\"%c\",\"file\":\"%s\",\"st\":\"%s\"}", first ? "" : ",",
                        ins::chain_name(c), 'A' + s, f, g_k[c][s].st == kPlaying ? "PLAYING" : status_short(g_k[c][s].st));
      if (k < 0 || k >= cap - n) return cap;
      n += k, first = false;
    }
  int k = sniprintf(o + n, n < cap ? cap - n : 0, "]}");
  return n + (k > 0 ? k : 0);
}

const Stats& stats() { return g_stats; }

void reset() {
  for (int c = 0; c < kVoices; ++c)
    for (int s = 0; s < kSlots; ++s) {
      g_k[c][s] = Ctl();
      g_x[c][s].inst = -1, g_x[c][s].offer = 0, g_x[c][s].done = 0;
    }
  for (int c = 0; c < ins::kChains; ++c)
    for (int s = 0; s < kSlots; ++s) g_a[c][s] = Audio();
  for (int i = 0; i < kInstances; ++i) g_meta[i].busy = false;
  bool lw = g_job.list_wanted || (g_job.ph == kList);
  g_job = Job();
  g_job.list_wanted = lw;   // a listing asked for (or under way) is still made
  g_stats = Stats();
}

// ---------------------------------------------------------------------------------------------------- bench, tests

void synthetic_weights(float* w, uint32_t seed) {
  uint32_t r = seed * 2654435761u + 12345u;
  for (int i = 0; i < kWeights; ++i) {
    r = r * 1664525u + 1013904223u;
    w[i] = ((float)(r >> 8) * (1.0f / 8388608.0f) - 1.0f) * 0.35f;
  }
  w[kWeights - 1] = 0.6f;   // the head's scale
}

uint64_t bench(uint32_t* worst) {
  static Inst* I = nullptr;
  static float* w = nullptr;
  if (!I) I = new Inst, w = new float[kWeights];
  synthetic_weights(w, 1);
  nam_a2lite_load_weights(&I->w, w, kWeights);
  nam_a2lite_init(&I->e, &I->w, &I->st);
  nam_a2lite_prewarm(&I->e);
  constexpr int kB = 12;   // core/machines.h kBlock
  float x[kB], y[kB];
  const float* in[1] = {x};
  float* out[1] = {y};
  uint64_t sum = 0;
  uint32_t wb = 0;
  for (int n = 0; n < 4000; ++n) {
    for (int s = 0; s < kB; ++s) x[s] = 0.3f * sinf(0.05f * (float)(n * kB + s));
    uint32_t t0 = cycles();
    nam_a2lite_process(&I->e, in, out, kB);
    uint32_t t = cycles() - t0;
    sum += t;
    if (t > wb) wb = t;
  }
  if (worst) *worst = wb;
  return sum;
}

}  // namespace amp
}  // namespace core

// ---- TONE3000's A2-Lite engine (Cortex-A76 build: NEON assembly on AArch64, its scalar stand-in elsewhere) and .namb
// reader, compiled here (their C99 sources build as C++ unchanged with restrict spelled __restrict; the engine with C++
// linkage as amp.h declares it, the reader with C linkage as its header does)
#define restrict __restrict
#ifndef NAM_A2LITE_BLOCK_HINT
#define NAM_A2LITE_BLOCK_HINT 12   // core/machines.h kBlock: the voices' render block
#endif
#include "../../third_party/nam/embedded/engines/cortex-a76/a2lite/nam_a2lite.c"
#include "../../third_party/nam/embedded/namb/namb_reader.c"
#undef restrict
