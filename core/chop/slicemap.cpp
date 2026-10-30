// core/chop/slicemap.cpp - see slicemap.h. Pure and cold: no card, no globals, no allocation.
#include "slicemap.h"

#include <math.h>
#include <string.h>

#include "../platform.h"
#include "../seq/rng.h"

namespace core {
namespace chop {

namespace {
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
bool say(char* why, int cap, const char* m) {
  if (why && cap > 0) sniprintf(why, cap, "%s", m);
  return false;
}
// ---- a small writer into the caller's buffer (no printf of floats on the Pi: integers only)
struct Out {
  char* p;
  int cap, n;
  bool ok;
  void put(const char* s) {
    while (*s) {
      if (n >= cap - 1) { ok = false; return; }
      p[n++] = *s++;
    }
    p[n] = 0;
  }
  void num(const char* name, int64_t v) {
    char b[48];
    sniprintf(b, sizeof b, " %s=%ld", name, (long)v);
    put(b);
  }
  void secs(const char* name, uint32_t ms) {   // ms as seconds, "0.005"
    char b[48];
    sniprintf(b, sizeof b, " %s=%lu.%03lu", name, (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    put(b);
  }
};
const char kHex[] = "0123456789abcdef";
int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
// ---- the reader: SFZ's syntax as the subset needs it (core/msamp/sfz.h: // and /* */ comments, <headers>,
// name=value; `sample` / sc_name run to the next opcode or the line's end)
bool is_name_ch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
// a decimal number (optional sign, digits, optional fraction) x scale, rounded: false when it is none
bool num_of(const char* s, int n, int64_t scale, int64_t* out) {
  int i = 0;
  bool neg = false;
  if (i < n && (s[i] == '-' || s[i] == '+')) neg = s[i] == '-', ++i;
  int64_t ip = 0, fp = 0, fd = 1;
  int d0 = i;
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    if (ip > 100000000000ll) return false;
    ip = ip * 10 + (s[i++] - '0');
  }
  bool digits = i > d0;
  if (i < n && s[i] == '.') {
    ++i;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
      if (fd < 100000000) fp = fp * 10 + (s[i] - '0'), fd *= 10;
      ++i, digits = true;
    }
  }
  if (!digits || i != n) return false;
  int64_t v = ip * scale + (fp * scale + fd / 2) / fd;
  *out = neg ? -v : v;
  return true;
}
bool sha_of(const char* s, int n, uint8_t* sha) {
  if (n != 64) return false;
  for (int i = 0; i < 32; ++i) {
    int a = hexv(s[2 * i]), b = hexv(s[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    sha[i] = (uint8_t)(a << 4 | b);
  }
  return true;
}
enum Hdr { kHNone, kHControl, kHGlobal, kHRegion, kHOther };
struct Parse {
  Map* m;
  const char* dir;
  int hdr;
  bool in_region;
  Slice cur;
  char cur_sample[kPathLen];
  bool cur_has_end, cur_has_off, cur_bad;
  int64_t cur_from, cur_to;   // an alt region's place in the map's sample (sc_from / sc_to; -1 not given)
  int key;
  char gsample[kPathLen];   // <global>'s sample (v2: the map's)
  char why[64];
  int regions;
  bool fail;
  bool fail_with(const char* w) {
    sniprintf(why, sizeof why, "%s", w);
    fail = true;
    return false;
  }
  void path_into(char* dst, const char* v, int vn) {   // relative to dir unless absolute; '\' as '/'
    char tmp[kPathLen];
    int k = 0;
    bool abs_p = vn > 0 && (v[0] == '/' || v[0] == '\\');
    if (!abs_p && dir && dir[0]) {
      for (const char* d = dir; *d && k < kPathLen - 2; ++d) tmp[k++] = *d;
      if (k > 0 && tmp[k - 1] != '/') tmp[k++] = '/';
    }
    for (int i = 0; i < vn && k < kPathLen - 1; ++i) tmp[k++] = v[i] == '\\' ? '/' : v[i];
    tmp[k] = 0;
    sniprintf(dst, kPathLen, "%s", tmp);
  }
  bool end_region() {
    if (!in_region) return true;
    in_region = false;
    ++regions;
    if (cur_bad) return true;   // (a region without offset / end is not a slice: skipped)
    if (!cur_has_off && !cur_has_end) return true;
    if (m->n >= kMaxSlices) return fail_with("more than 64 slices");
    const char* smp = cur_sample[0] ? cur_sample : gsample;
    if (!m->sample[0] && smp[0]) sniprintf(m->sample, sizeof m->sample, "%s", smp);
    cur.alt = -1;
    if (smp[0] && strcmp(smp, m->sample) && cur_from >= 0 && cur_to > cur_from) {   // its own sample (RESAMPLE)
      int a = -1;
      for (int i = 0; i < m->nalt && a < 0; ++i)
        if (!strcmp(m->alt[i], smp)) a = i;
      if (a < 0) {
        if (m->nalt >= kMaxAlts) return fail_with("too many resampled slices");
        a = m->nalt++;
        sniprintf(m->alt[a], kPathLen, "%s", smp);
      }
      cur.alt = (int8_t)a;
      cur.start = (uint32_t)cur_from, cur.end = (uint32_t)cur_to;   // (where it sat in the map's sample)
      cur_has_end = true;
    } else if (smp[0] && strcmp(smp, m->sample)) {
      return fail_with("a region of another sample");
    }
    if (!cur_has_end) cur.end = m->frames ? m->frames : cur.start + kMinSlice;
    if (cur.end <= cur.start) return fail_with("a slice ends before it starts");
    // in order, no overlap (a later region before an earlier one's end is refused: not a map this writes)
    if (m->n > 0 && cur.alt < 0 && m->s[m->n - 1].alt < 0 && cur.start < m->s[m->n - 1].end)
      return fail_with("slices overlap or are out of order");
    m->s[m->n++] = cur;
    return true;
  }
  void begin_region() {
    in_region = true;
    slice_defaults(&cur, 0, 0);
    cur_sample[0] = 0;
    cur_has_end = cur_has_off = cur_bad = false;
    cur_from = cur_to = -1;
  }
  bool opcode(const char* name, int nn, const char* v, int vn) {
    auto is = [&](const char* s) { return (int)strlen(s) == nn && !memcmp(name, s, nn); };
    int64_t x = 0;
    if (hdr == kHOther) return true;
    if (is("sample")) {
      if (hdr == kHRegion) path_into(cur_sample, v, vn);
      else if (hdr == kHGlobal || hdr == kHNone) path_into(gsample, v, vn);
      return true;
    }
    if (hdr == kHGlobal || hdr == kHControl || hdr == kHNone) {
      if (is("sc_sha")) {
        if (vn == 1 && v[0] == '-') m->has_sha = false;
        else if (sha_of(v, vn, m->sha)) m->has_sha = true;
        else return fail_with("a bad sc_sha");
      } else if (is("sc_size")) {
        if (!num_of(v, vn, 1, &x) || x < 0) return fail_with("a bad sc_size");
        m->size = (uint64_t)x;
      } else if (is("sc_frames")) {
        if (!num_of(v, vn, 1, &x) || x < 0 || x > 0x7fffffff) return fail_with("a bad sc_frames");
        m->frames = (uint32_t)x;
      } else if (is("sc_rate")) {
        if (!num_of(v, vn, 1, &x) || x < 0 || x > 1000000) return fail_with("a bad sc_rate");
        m->rate = (uint32_t)x;
      } else if (is("sc_ch")) {
        if (!num_of(v, vn, 1, &x) || x < 1 || x > 2) return fail_with("a bad sc_ch");
        m->ch = (uint8_t)x;
      } else if (is("sc_name")) {
        int k = vn < kNameLen - 1 ? vn : kNameLen - 1;
        memcpy(m->name, v, k);
        m->name[k] = 0;
      }
      return true;   // (other <control> / <global> opcodes: another player's)
    }
    // <region>
    bool num = num_of(v, vn, 1, &x);
    if (is("offset")) {
      if (!num || x < 0 || x > 0x7fffffff) return fail_with("a bad offset");
      cur.start = (uint32_t)x, cur_has_off = true;
    } else if (is("end")) {
      if (!num || x < -1 || x > 0x7ffffffe) return fail_with("a bad end");
      if (x < 0) cur_bad = true;   // (end=-1: SFZ's silent region)
      else cur.end = (uint32_t)x + 1, cur_has_end = true;
    } else if (is("key")) {
      if (num) key = (int)x;
    } else if (is("transpose")) {
      if (!num) return fail_with("a bad transpose");
      cur.pitch = (int8_t)clampi((int)x, -24, 24);
    } else if (is("tune")) {
      if (!num) return fail_with("a bad tune");
      cur.fine = (int8_t)clampi((int)x, -50, 50);
    } else if (is("volume")) {
      if (!num_of(v, vn, 1, &x)) return fail_with("a bad volume");
      cur.gain = (int8_t)clampi((int)x, -24, 12);
    } else if (is("direction")) {
      cur.rev = vn == 7 && !memcmp(v, "reverse", 7);
    } else if (is("ampeg_attack") || is("ampeg_decay")) {
      if (!num_of(v, vn, 1000, &x) || x < 0) return fail_with("a bad envelope time");
      uint16_t ms = (uint16_t)(x > 20000 ? 20000 : x);
      if (is("ampeg_attack")) cur.atk_ms = ms > 2000 ? 2000 : ms;
      else cur.dec_ms = ms;
    } else if (is("loop_mode")) {
      bool on = (vn == 15 && !memcmp(v, "loop_continuous", 15)) || (vn == 12 && !memcmp(v, "loop_sustain", 12));
      if (!on) cur.loop = kLoopOff;
      else if (cur.loop == kLoopOff) cur.loop = kLoopFwd;
    } else if (is("loop_type")) {
      if (vn == 9 && !memcmp(v, "alternate", 9)) cur.loop = kLoopPing;
    } else if (is("group")) {
      if (!num) return fail_with("a bad group");
      cur.choke = (uint8_t)(x >= 1 && x <= 4 ? x : 0);
    } else if (is("sc_nudge")) {
      if (!num) return fail_with("a bad sc_nudge");
      cur.nudge = (int16_t)clampi((int)x, -2400, 2400);
    } else if (is("sc_stut")) {
      if (!num) return fail_with("a bad sc_stut");
      cur.stut = (uint8_t)clampi((int)x, 1, 16);
    } else if (is("sc_stutlen")) {
      if (!num) return fail_with("a bad sc_stutlen");
      cur.stut_len = (uint8_t)clampi((int)x, 1, 100);
    } else if (is("sc_stutdec")) {
      if (!num) return fail_with("a bad sc_stutdec");
      cur.stut_dec = (uint8_t)clampi((int)x, 0, 90);
    } else if (is("sc_tape")) {
      cur.tape = vn == 4 && !memcmp(v, "stop", 4) ? kTapeStop : vn == 4 && !memcmp(v, "spin", 4) ? kTapeSpin : kTapeOff;
    } else if (is("sc_tapems")) {
      if (!num) return fail_with("a bad sc_tapems");
      cur.tape_ms = (uint16_t)clampi((int)x, 20, 4000);
    } else if (is("sc_from") || is("sc_to")) {
      if (!num || x < 0 || x > 0x7fffffff) return fail_with("a bad sc_from / sc_to");
      (is("sc_from") ? cur_from : cur_to) = x;
      cur_has_off = true;
    } else if (is("sc_nbr")) {
      if (!num) return fail_with("a bad sc_nbr");
      cur.nbr = (uint8_t)clampi((int)x, 0, 100);
    }
    return true;   // (other opcodes: SFZ's own, read by other players)
  }
};
// the end of a run-on value (`sample`, sc_name): the next " name=" or the line's end, trailing spaces cut
int runon_end(const char* t, int i, int n) {
  int e = i;
  while (e < n && t[e] != '\n' && t[e] != '\r') {
    if (t[e] == ' ' || t[e] == '\t') {
      int j = e;
      while (j < n && (t[j] == ' ' || t[j] == '\t')) ++j;
      int k = j;
      while (k < n && is_name_ch(t[k])) ++k;
      if (k > j && k < n && t[k] == '=') break;
      if (j < n && t[j] == '<') break;
      if (j + 1 < n && t[j] == '/' && t[j + 1] == '/') break;
    }
    ++e;
  }
  while (e > i && (t[e - 1] == ' ' || t[e - 1] == '\t')) --e;
  return e;
}
bool scan(const char* t, int n, Parse* p) {
  int i = 0;
  while (i < n) {
    char c = t[i];
    if (!c) return p->fail_with("a NUL byte");
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
    if (c == '/' && i + 1 < n && t[i + 1] == '/') {
      while (i < n && t[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && t[i + 1] == '*') {
      i += 2;
      while (i + 1 < n && !(t[i] == '*' && t[i + 1] == '/')) ++i;
      if (i + 1 >= n) return p->fail_with("an unclosed comment");
      i += 2;
      continue;
    }
    if (c == '#') return p->fail_with("a #define / #include (not a slice map)");
    if (c == '<') {
      int e = i + 1;
      while (e < n && t[e] != '>' && t[e] != '\n') ++e;
      if (e >= n || t[e] != '>') return p->fail_with("a broken header");
      int hn = e - i - 1;
      if (!p->end_region()) return false;
      const char* h = t + i + 1;
      p->hdr = hn == 7 && !memcmp(h, "control", 7) ? kHControl
             : hn == 6 && !memcmp(h, "global", 6) ? kHGlobal
             : hn == 6 && !memcmp(h, "region", 6) ? kHRegion
             : hn == 5 && !memcmp(h, "group", 5) ? kHGlobal   // (a <group>'s opcodes: as <global>'s, for the subset)
                                                  : kHOther;
      if (p->hdr == kHRegion) p->begin_region();
      i = e + 1;
      continue;
    }
    int ne = i;
    while (ne < n && is_name_ch(t[ne])) ++ne;
    if (ne == i || ne >= n || t[ne] != '=') return p->fail_with("not an opcode");
    int v0 = ne + 1, ve;
    bool runon = (ne - i == 6 && !memcmp(t + i, "sample", 6)) || (ne - i == 7 && !memcmp(t + i, "sc_name", 7));
    if (runon) ve = runon_end(t, v0, n);
    else {
      ve = v0;
      while (ve < n && t[ve] != ' ' && t[ve] != '\t' && t[ve] != '\r' && t[ve] != '\n') ++ve;
    }
    if (!p->opcode(t + i, ne - i, t + v0, ve - v0)) return false;
    i = ve;
  }
  return p->end_region();
}
}  // namespace

void slice_defaults(Slice* s, uint32_t start, uint32_t end) {
  memset(s, 0, sizeof *s);
  s->start = start, s->end = end;
  s->stut = 1, s->stut_len = 25, s->stut_dec = 20, s->tape_ms = 300, s->alt = -1;
}
void map_clear(Map* m) { memset(m, 0, sizeof *m); }

void slice_span(const Map& m, int k, uint32_t* a, uint32_t* b) {
  const Slice& s = m.s[k];
  int64_t st = (int64_t)s.start + s.nudge;
  uint32_t e = s.end;
  if (st < 0) st = 0;
  if (e < kMinSlice) e = kMinSlice;
  if (st > (int64_t)e - (int64_t)kMinSlice) st = (int64_t)e - kMinSlice;
  *a = (uint32_t)st, *b = e;
}

// ---- the file
int version_of(const char* t, uint32_t n, uint16_t* min_reader, uint32_t* body) {
  static const char kTag[] = "// sidecar slicemap ";
  const uint32_t tl = sizeof kTag - 1;
  if (n >= tl && !memcmp(t, kTag, tl)) {
    uint32_t i = tl, v = 0, mr = 0;
    while (i < n && t[i] >= '0' && t[i] <= '9' && v < 65535) v = v * 10 + (uint32_t)(t[i++] - '0');
    if (!v || v > 65535) return 0;
    while (i < n && t[i] == ' ') ++i;
    if (i + 4 <= n && !memcmp(t + i, "min=", 4)) {
      i += 4;
      while (i < n && t[i] >= '0' && t[i] <= '9' && mr < 65535) mr = mr * 10 + (uint32_t)(t[i++] - '0');
      if (mr > 65535) mr = 65535;   // review minor: an out-of-range min= is a newer reader's, never one that wraps
    }
    while (i < n && (t[i] == ' ' || t[i] == '\r')) ++i;
    if (i < n && t[i] != '\n') return 0;
    if (min_reader) *min_reader = (uint16_t)(mr ? mr : v);
    if (body) *body = i < n ? i + 1 : n;
    return (int)v;
  }
  // v1: an SFZ (no header of ours) with a region: the sampler's slice maps
  static const char kOther[] = "// sidecar ";
  if (n >= sizeof kOther - 1 && !memcmp(t, kOther, sizeof kOther - 1)) return 0;
  for (uint32_t i = 0; i + 8 <= n; ++i)
    if (t[i] == '<' && !memcmp(t + i, "<region>", 8)) {
      if (min_reader) *min_reader = 1;
      if (body) *body = 0;
      return 1;
    }
  return 0;
}

int write(const Map& m, char* out, int cap) {
  if (!out || cap < 2) return 0;
  Out o = {out, cap, 0, true};
  out[0] = 0;
  char b[kPathLen + 64];
  sniprintf(b, sizeof b, "// sidecar slicemap %u\n", (unsigned)kVersion);
  o.put(b);
  o.put("// CHOP's slices of one sample (docs/CHOP.md): a region a slice; sc_* opcodes are CHOP's, other players skip "
        "them\n<control> default_path=\n<global>");
  if (m.has_sha) {
    char h[65];
    for (int i = 0; i < 32; ++i) h[2 * i] = kHex[m.sha[i] >> 4], h[2 * i + 1] = kHex[m.sha[i] & 15];
    h[64] = 0;
    o.put(" sc_sha="), o.put(h);
  } else {
    o.put(" sc_sha=-");
  }
  o.num("sc_size", (int64_t)m.size);
  o.num("sc_frames", m.frames);
  o.num("sc_rate", m.rate);
  o.num("sc_ch", m.ch ? m.ch : 1);
  if (m.name[0]) o.put(" sc_name="), o.put(m.name);
  if (m.sample[0]) o.put(" sample="), o.put(m.sample);
  o.put("\n");
  for (int k = 0; k < m.n; ++k) {
    const Slice& s = m.s[k];
    bool alt = s.alt >= 0 && s.alt < m.nalt;
    o.put("<region>");
    o.num("key", kKeyBase + k);
    if (alt) o.num("offset", 0), o.num("sc_from", s.start), o.num("sc_to", s.end);   // (its own file, whole)
    else o.num("offset", s.start), o.num("end", (int64_t)s.end - 1);
    if (s.pitch) o.num("transpose", s.pitch);
    if (s.fine) o.num("tune", s.fine);
    if (s.gain) o.num("volume", s.gain);
    if (s.rev) o.put(" direction=reverse");
    if (s.atk_ms) o.secs("ampeg_attack", s.atk_ms);
    if (s.dec_ms) o.secs("ampeg_decay", s.dec_ms), o.put(" ampeg_sustain=0");
    if (s.loop == kLoopOff) {
      o.put(" loop_mode=one_shot");
    } else {
      o.put(" loop_mode=loop_continuous");
      o.put(s.loop == kLoopPing ? " loop_type=alternate" : " loop_type=forward");
      if (!alt) o.num("loop_start", s.start), o.num("loop_end", (int64_t)s.end - 1);
    }
    if (s.choke) o.num("group", s.choke), o.num("off_by", s.choke);
    if (s.nudge) o.num("sc_nudge", s.nudge);
    if (s.stut > 1) o.num("sc_stut", s.stut), o.num("sc_stutlen", s.stut_len), o.num("sc_stutdec", s.stut_dec);
    if (s.tape) o.put(s.tape == kTapeStop ? " sc_tape=stop" : " sc_tape=spin"), o.num("sc_tapems", s.tape_ms);
    if (s.nbr) o.num("sc_nbr", s.nbr);
    if (alt) o.put(" sample="), o.put(m.alt[(int)s.alt]);
    o.put("\n");
  }
  return o.ok ? o.n : 0;
}

bool read(const char* text, uint32_t n, Map* m, char* why, int cap, const char* dir) {
  map_clear(m);
  uint32_t body = 0;
  uint16_t mr = 0;
  int v = version_of(text, n, &mr, &body);
  if (v == 0) return say(why, cap, "not a slice map");
  if (v > kVersion && mr > kVersion) return say(why, cap, "made by a newer Sidecar");   // (min= this or lower: read)
  static Parse p;   // (one core at a time: the control core's load, or the registry's reader)
  memset(&p, 0, sizeof p);
  p.m = m, p.dir = dir ? dir : "";
  p.key = -1;
  if (!scan(text + body, (int)(n - body), &p)) return say(why, cap, p.why[0] ? p.why : "damaged");
  if (!m->n) return say(why, cap, "no slices");
  if (!m->sample[0] && p.gsample[0]) sniprintf(m->sample, sizeof m->sample, "%s", p.gsample);
  if (m->frames) {   // inside the sample (a map of a longer file: clamped)
    for (int k = 0; k < m->n; ++k) {
      Slice& s = m->s[k];
      if (s.alt >= 0) continue;
      if (s.start >= m->frames) return say(why, cap, "a slice past the sample's end");
      if (s.end > m->frames) s.end = m->frames;
    }
  }
  return true;
}

bool upgrade_v1(const char* text, uint32_t n, char* out, uint32_t cap, uint32_t* out_n) {
  static Map m;
  char why[64];
  if (version_of(text, n) != 1 || !read(text, n, &m, why, sizeof why, "")) return false;
  int w = write(m, out, (int)cap);
  if (w <= 0) return false;
  *out_n = (uint32_t)w;
  return true;
}

// ---- edits
uint32_t snap(const int16_t* pcm, int ch, uint32_t frames, uint32_t at, uint32_t reach) {
  if (!pcm || frames < 2 || at == 0 || at >= frames) return at;
  auto x = [&](uint32_t i) { return ch == 2 ? (int32_t)pcm[2 * i] + pcm[2 * i + 1] : (int32_t)pcm[i]; };
  uint32_t lo = at > reach ? at - reach : 1, hi = at + reach < frames ? at + reach : frames - 1;
  uint32_t best = at, bd = ~0u;
  for (uint32_t i = lo; i <= hi; ++i) {   // a crossing between i - 1 and i: the sample nearer zero is the start
    int32_t a = x(i - 1), b = x(i);
    if ((a < 0) == (b < 0) && a != 0) continue;
    uint32_t k = (a < 0 ? -a : a) < (b < 0 ? -b : b) ? i - 1 : i;
    if (k == 0) k = i;
    uint32_t d = k > at ? k - at : at - k;
    if (d < bd) bd = d, best = k;
  }
  return best;
}

int slice_at(const Map& m, uint32_t f) {
  for (int k = 0; k < m.n; ++k)
    if (m.s[k].alt < 0 && f >= m.s[k].start && f < m.s[k].end) return k;
  return -1;
}
namespace {
void insert_at(Map* m, int i, const Slice& s) {
  memmove(m->s + i + 1, m->s + i, sizeof(Slice) * (size_t)(m->n - i));
  m->s[i] = s;
  ++m->n;
}
void remove_at(Map* m, int i) {
  memmove(m->s + i, m->s + i + 1, sizeof(Slice) * (size_t)(m->n - i - 1));
  --m->n;
}
// an alt no slice uses any more: dropped (the indexes after it move down)
void drop_unused_alts(Map* m) {
  for (int a = m->nalt - 1; a >= 0; --a) {
    bool used = false;
    for (int k = 0; k < m->n && !used; ++k) used = m->s[k].alt == a;
    if (used) continue;
    memmove(m->alt[a], m->alt[a + 1], (size_t)(m->nalt - a - 1) * kPathLen);
    --m->nalt;
    for (int k = 0; k < m->n; ++k)
      if (m->s[k].alt > a) --m->s[k].alt;
  }
}
}  // namespace

bool split(Map* m, int k, uint32_t at) {
  if (k < 0 || k >= m->n || m->n >= kMaxSlices) return false;
  Slice& s = m->s[k];
  if (s.alt >= 0 || at < s.start + kMinSlice || at + kMinSlice > s.end) return false;
  Slice r = s;
  r.start = at, r.nudge = 0;
  s.end = at;
  insert_at(m, k + 1, r);
  return true;
}
bool add_marker(Map* m, uint32_t f) {
  if (m->n >= kMaxSlices) return false;
  int k = slice_at(*m, f);
  if (k >= 0) return split(m, k, f);
  int i = 0;
  while (i < m->n && m->s[i].start <= f) ++i;
  uint32_t end = i < m->n ? m->s[i].start : m->frames;
  if (!end || end < f + kMinSlice) return false;
  if (i > 0 && m->s[i - 1].end > f) return false;
  Slice s;
  slice_defaults(&s, f, end);
  insert_at(m, i, s);
  return true;
}
bool del(Map* m, int k) {
  if (k < 0 || k >= m->n) return false;
  remove_at(m, k);
  drop_unused_alts(m);
  return true;
}
bool merge(Map* m, int k) {
  if (k < 0 || k + 1 >= m->n || m->s[k].alt >= 0 || m->s[k + 1].alt >= 0) return false;
  m->s[k].end = m->s[k + 1].end;
  remove_at(m, k + 1);
  return true;
}
bool move_start(Map* m, int k, int32_t f) {
  if (k < 0 || k >= m->n || m->s[k].alt >= 0) return false;
  Slice& s = m->s[k];
  bool touch = k > 0 && m->s[k - 1].alt < 0 && m->s[k - 1].end == s.start;
  int64_t lo = k == 0 ? 0 : touch ? (int64_t)m->s[k - 1].start + kMinSlice : (int64_t)m->s[k - 1].end;
  int64_t hi = (int64_t)s.end - kMinSlice;
  if (hi < lo) return false;
  int64_t v = f < lo ? lo : f > hi ? hi : f;
  s.start = (uint32_t)v;
  if (touch) m->s[k - 1].end = s.start;
  return true;
}
bool move_end(Map* m, int k, int32_t f) {
  if (k < 0 || k >= m->n || m->s[k].alt >= 0) return false;
  Slice& s = m->s[k];
  bool touch = k + 1 < m->n && m->s[k + 1].alt < 0 && m->s[k + 1].start == s.end;
  int64_t lo = (int64_t)s.start + kMinSlice;
  int64_t hi = k + 1 >= m->n ? (int64_t)(m->frames ? m->frames : s.end)
             : touch ? (int64_t)m->s[k + 1].end - kMinSlice : (int64_t)m->s[k + 1].start;
  if (hi < lo) return false;
  int64_t v = f < lo ? lo : f > hi ? hi : f;
  s.end = (uint32_t)v;
  if (touch) m->s[k + 1].start = s.end;
  return true;
}
void grid(Map* m, int n, const int16_t* pcm, int ch) {
  if (!m->frames) return;
  n = clampi(n, 1, kMaxSlices);
  if ((uint32_t)n * kMinSlice > m->frames) n = (int)(m->frames / kMinSlice) > 0 ? (int)(m->frames / kMinSlice) : 1;
  uint32_t st[kMaxSlices + 1];
  for (int k = 0; k < n; ++k) {
    uint32_t at = (uint32_t)((uint64_t)m->frames * (uint32_t)k / (uint32_t)n);
    st[k] = k ? snap(pcm, ch, m->frames, at, kSnapFrames * (m->rate ? m->rate : 48000) / 48000) : 0;
  }
  st[n] = m->frames;
  m->n = 0;
  m->nalt = 0;
  for (int k = 0; k < n; ++k)
    if (st[k + 1] >= st[k] + kMinSlice || k == n - 1) slice_defaults(&m->s[m->n++], st[k], st[k + 1]);   // (review minor: never under kMinSlice)
  if (m->n > 1 && m->s[m->n - 1].end - m->s[m->n - 1].start < kMinSlice) m->n--;   // a short last one joins the one before
  for (int k = 0; k + 1 < m->n; ++k) m->s[k].end = m->s[k + 1].start;
  if (m->n) m->s[m->n - 1].end = m->frames;
}

// ---- onsets
void onsets_begin(Onsets* o, int sens) {
  memset(o, 0, sizeof *o);
  sens = clampi(sens, 0, 127);
  o->thr = 8.0f - 6.6f * (float)sens / 127.0f;   // 8.0 (few) .. 1.4 (many)
  const float fl = 32768.0f * 0.00178f;          // -55 dBFS
  o->floor_e = fl * fl * 256.0f;
  o->n = 1, o->at[0] = 0;                        // the first slice from the start
}
void onsets_feed(Onsets* o, const int16_t* pcm, int ch, uint32_t frames, uint32_t upto) {
  const uint32_t kHop = 256;
  uint32_t gap = 2400;   // 50 ms at 48 kHz
  if (upto > frames) upto = frames;
  while (o->pos + kHop <= upto) {
    uint32_t a = o->pos;
    float e = 0, pk = 0;
    for (uint32_t i = a; i < a + kHop; ++i) {   // the energy of the first difference: the attack's highs count most
      float x = ch == 2 ? 0.5f * ((float)pcm[2 * i] + (float)pcm[2 * i + 1]) : (float)pcm[i];
      float d = x - o->prev;
      o->prev = x;
      e += 2.0f * d * d;
      float ax = x < 0 ? -x : x;
      if (ax > pk) pk = ax;
    }
    float mean = 0;
    for (int h = 0; h < o->nh; ++h) mean += o->hist[h];
    mean = o->nh ? mean / (float)o->nh : 0;
    bool hit = o->nh >= 2 && e > o->floor_e && e > o->thr * (mean > o->floor_e * 0.05f ? mean : o->floor_e * 0.05f) &&
               a >= o->last + gap && o->n < kMaxSlices;
    if (hit) {
      uint32_t f = a;   // the hop's first frame over a quarter of its peak
      for (uint32_t i = a; i < a + kHop; ++i) {
        float x = ch == 2 ? 0.5f * ((float)pcm[2 * i] + (float)pcm[2 * i + 1]) : (float)pcm[i];
        if ((x < 0 ? -x : x) >= 0.25f * pk) { f = i; break; }
      }
      // back to the zero crossing before it (within 2 ms), so the slice starts on the onset's own rise
      uint32_t z = f;
      auto y = [&](uint32_t i) { return ch == 2 ? (int32_t)pcm[2 * i] + pcm[2 * i + 1] : (int32_t)pcm[i]; };
      for (uint32_t i = f; i > 0 && f - i < kSnapFrames; --i)
        if ((y(i - 1) < 0) != (y(i) < 0) || y(i) == 0) { z = i; break; }
      if (z < o->last + gap / 2) z = f;
      if (z > 0 && z >= o->at[o->n - 1] + kMinSlice) {
        if (z < gap && o->n == 1) o->at[0] = 0;   // (an onset in the first 50 ms: the first slice's own start)
        else o->at[o->n++] = z;
        o->last = z;
      }
    }
    if (o->nh < 8) o->hist[o->nh++] = e;
    else memmove(o->hist, o->hist + 1, sizeof(float) * 7), o->hist[7] = e;
    o->pos += kHop;
  }
}
void onsets_apply(const Onsets& o, Map* m, const int16_t*, int) {
  if (!m->frames || o.n < 1) return;
  m->n = 0, m->nalt = 0;
  for (int k = 0; k < o.n && m->n < kMaxSlices; ++k) {
    uint32_t a = o.at[k], b = k + 1 < o.n ? o.at[k + 1] : m->frames;
    if (b > a + kMinSlice / 2) slice_defaults(&m->s[m->n++], a, b);
  }
}

// ---- LAZY
void lazy_begin(Lazy* l) {
  memset(l, 0, sizeof *l);
  l->on = true;
}
int32_t lazy_tap(Lazy* l, uint32_t played, uint32_t lat, const int16_t* pcm, int ch, uint32_t frames) {
  if (!l->on) return -1;
  if (l->n >= kMaxSlices) {
    ++l->refused;
    return -1;
  }
  uint32_t at = played > lat ? played - lat : 0;
  if (frames && at >= frames) {
    ++l->refused;
    return -1;
  }
  at = snap(pcm, ch, frames, at);
  for (int i = 0; i < l->n; ++i) {   // too close to a marker already dropped (a double tap): refused
    uint32_t d = at > l->at[i] ? at - l->at[i] : l->at[i] - at;
    if (d < kMinSlice) {
      ++l->refused;
      return -1;
    }
  }
  l->at[l->n++] = at;
  return (int32_t)at;
}
bool lazy_undo(Lazy* l) {
  if (!l->n) return false;
  --l->n;
  return true;
}
void lazy_apply(const Lazy& l, Map* m) {
  if (!l.n || !m->frames) return;
  uint32_t a[kMaxSlices];
  int n = l.n;
  memcpy(a, l.at, sizeof(uint32_t) * (size_t)n);
  for (int i = 1; i < n; ++i)   // (taps come in order; sorted anyway)
    for (int j = i; j > 0 && a[j] < a[j - 1]; --j) {
      uint32_t t = a[j];
      a[j] = a[j - 1], a[j - 1] = t;
    }
  m->n = 0, m->nalt = 0;
  for (int k = 0; k < n; ++k) {
    uint32_t e = k + 1 < n ? a[k + 1] : m->frames;
    if (e > a[k] + kMinSlice / 2) slice_defaults(&m->s[m->n++], a[k], e);
  }
}

// ---- the keyed choices
void shuffle(uint32_t seed, int n, uint8_t* order) {
  n = clampi(n, 0, kMaxSlices);
  for (int k = 0; k < n; ++k) order[k] = (uint8_t)k;
  if (!seed) return;
  for (int k = n - 1; k > 0; --k) {   // Fisher-Yates, one keyed draw a place
    int j = seq::rng_below(seq::rng_key(seed, 0, k, -1, seq::kRngShuffle), k + 1);
    uint8_t t = order[k];
    order[k] = order[j], order[j] = t;
  }
}
int neighbour(uint32_t seed, int voice, int k, int n, uint32_t hit, int pct) {
  if (n < 2 || pct <= 0 || k < 0 || k >= n) return k;
  if (!seq::rng_chance(seq::rng_key(seed, voice, k, hit, seq::kRngNbr, 0), pct > 100 ? 100 : pct)) return k;
  if (k == 0) return 1;
  if (k == n - 1) return n - 2;
  return seq::rng_key(seed, voice, k, hit, seq::kRngNbr, 1) & 1 ? k + 1 : k - 1;
}

const char* loop_name(int l) { return l == kLoopFwd ? "FWD" : l == kLoopPing ? "PING" : "OFF"; }
const char* tape_name(int t) { return t == kTapeStop ? "STOP" : t == kTapeSpin ? "SPIN" : "OFF"; }

}  // namespace chop
}  // namespace core
