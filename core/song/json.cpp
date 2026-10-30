// core/song/json.cpp - see song.h: the song file (docs/SONG.md "Storage format"), written in pieces and read a step at a
// time, the journal's lines and the recovery from them. Our own writer's output is what the reader must take; it skips
// keys it does not know (a later version's) and stops at the first error. No floats in the printf (the Pi's is integer
// only): tempos as fixed point. Cold.
#include "song/song.h"

#if CORE_POLY
#include <string.h>

#include "platform.h"
#include "version.h"

namespace core {
namespace song {

namespace {

// bpm x 120 -> "127.25" (two decimals, rounded)
void bpm_text(uint32_t x120, char* b, int cap) {
  uint32_t h = (x120 * 100 + 60) / 120;
  sniprintf(b, cap, "%u.%02u", (unsigned)(h / 100), (unsigned)(h % 100));
}

bool add_b64(Buf& o, const Buf& d) {
  if (!o.reserve(o.n + b64_len(d.n) + 4)) return false;
  int k = b64_encode(d.p, d.n, (char*)o.p + o.n, o.cap - o.n);
  if (k < 0) return false;
  o.n += k;
  o.p[o.n] = 0;
  return true;
}

void add_str(Buf& o, const char* s) {
  o.addc('"');
  for (; *s; ++s) {
    char c = *s;
    if (c == '"' || c == '\\') o.addc('\\'), o.addc(c);
    else if ((unsigned char)c < 0x20) o.addc('_');
    else o.addc(c);
  }
  o.addc('"');
}

void header(const Take& t, Buf& o, bool journal) {
  o.adds("{\"format\":\"sidecar-song\",\"version\":1,");
  if (journal) o.adds("\"journal\":1,");
  o.adds("\"project\":"), add_str(o, t.project);
  o.adds(",\"name\":"), add_str(o, t.name);
  o.adds(",\"sidecar\":\"" SIDECAR_VERSION "\"");
  o.addf(",\"ppq_step\":%d,\"bar_steps\":%d", kPpq, kBarSteps);
  if (t.recovered) o.adds(",\"recovered\":1");
  const MidiConfig& c = t.cfg;
  o.addf(",\"midi_config\":{\"read\":%d,\"encoder\":\"%s\",\"out_ch\":\"%s\",\"param\":\"%s\",\"pc_receive\":%d,"
         "\"pc_send\":%d,\"transport_send\":%d,\"clock_send\":%d,\"track_ch\":[",
         c.valid ? 1 : 0, c.encoder_ext ? "int+ext" : "int", c.trk_ch ? "trk" : "auto", c.nrpn ? "nrpn" : "cc",
         c.pc_receive, c.pc_send, c.transport_send, c.clock_send);
  for (int i = 0; i < kTracks; ++i) o.addf("%s%d", i ? "," : "", c.track_ch[i]);
  o.addf("],\"fx_ch\":%d,\"auto_ch\":%d}", c.fx_ch, c.auto_ch);
  char bpm[16];
  bpm_text(t.tempo, bpm, sizeof bpm);
  o.addf(",\"start\":{\"pattern\":%d,\"tempo\":%s,\"mutes\":%u,\"scene\":%d,\"rytm_sysex\":{", t.pattern,
         t.tempo ? bpm : "0", (unsigned)t.mutes, t.scene);
}

// the dumps, base64, at most kB64Piece raw bytes a call (a multiple of 3: the pieces' text joins into one base64):
// true while there is more (UNDO REPLAY re-review N2: a 15 KB pattern X is not encoded in one control-core pass)
constexpr int kB64Piece = 3072;
bool sysex_piece(const Take& t, Buf& o, int& bi, int& off, bool& first) {
  const Buf* d[5] = {&t.kit, &t.pat, &t.settings, &t.global, &t.kit2};
  static const char* const kName[5] = {"kit_x", "pattern_x", "settings_x", "global_x", "kit2_x"};   // (kit2: a backup's)
  while (bi < 5 && !d[bi]->n) ++bi;
  if (bi >= 5) return false;
  const Buf& b = *d[bi];
  if (!off) o.addf("%s\"%s\":\"", first ? "" : ",", kName[bi]), first = false;
  int k = b.n - off < kB64Piece ? b.n - off : kB64Piece;
  Buf part;
  part.p = b.p + off, part.n = k;
  add_b64(o, part);
  part.p = nullptr;   // (not its memory)
  off += k;
  if (off >= b.n) o.addc('"'), ++bi, off = 0;
  return true;
}

void header_end(const Take& t, Buf& o) {
  o.adds("}");
  if (t.has_fx) {
    o.adds(",\"sidecar\":{\"send_fx\":{\"send\":[");
    for (int i = 0; i < kTracks; ++i) o.addf("%s[%d,%d]", i ? "," : "", t.fx_send[i][0], t.fx_send[i][1]);
    o.adds("],\"knob\":[");
    for (int s = 0; s < 2; ++s) {
      o.adds(s ? ",[" : "[");
      for (int j = 0; j < 6; ++j) o.addf("%s%d", j ? "," : "", t.fx_knob[s][j]);
      o.adds("]");
    }
    o.addf("],\"route\":%d}}", t.fx_route);
  }
  o.adds("}}");   // the start, the song
}

}  // namespace

CORE_COLD int event_json(const Ev& e, char* out, int cap) {
  char a[96];
  switch (e.type) {
    case kPc: sniprintf(a, sizeof a, "{\"to\":%d}", e.a); break;
    case kSw: sniprintf(a, sizeof a, "{\"from\":%d,\"to\":%d}", e.a, e.b); break;
    case kParam:
      sniprintf(a, sizeof a, "{\"trk\":%d,\"nrpn\":[%d,%d],\"v\":%u}", e.a, e.b >> 7, e.b & 127, (unsigned)e.v);
      break;
    case kCc: sniprintf(a, sizeof a, "{\"ch\":%d,\"n\":%d,\"v\":%u}", e.a, e.b, (unsigned)e.v); break;
    case kMute: sniprintf(a, sizeof a, "{\"mask\":%u}", (unsigned)e.v); break;
    case kScene: sniprintf(a, sizeof a, "{\"n\":%d}", e.a); break;
    case kPerf: sniprintf(a, sizeof a, "{\"n\":%d,\"v\":%u}", e.a, (unsigned)e.v); break;
    case kTempo: {
      char b[16];
      bpm_text(e.v, b, sizeof b);
      sniprintf(a, sizeof a, "{\"bpm\":%s}", b);
      break;
    }
    case kSc: {
      static const char* const k[3] = {"send", "knob", "route"};
      sniprintf(a, sizeof a, "{\"k\":\"%s\",\"i\":%d,\"v\":%u}", e.a < 3 ? k[e.a] : "?", e.b, (unsigned)e.v);
      break;
    }
    case kMach: sniprintf(a, sizeof a, "{\"trk\":%d,\"m\":%d}", e.a, e.b); break;
    case kSrc:   // rui1045: "p" the parameter when not P1 (10: DSP SAMPLE's SMP)
      if (e.b) sniprintf(a, sizeof a, "{\"trk\":%d,\"p\":%d,\"v\":%u}", e.a, e.b, (unsigned)e.v);
      else sniprintf(a, sizeof a, "{\"trk\":%d,\"v\":%u}", e.a, (unsigned)e.v);
      break;
    case kFill: sniprintf(a, sizeof a, "{\"on\":%d}", e.a); break;
    case kPlay: case kStop: case kPause: case kCont: sniprintf(a, sizeof a, "{\"ms\":%u}", (unsigned)e.ms); break;
    default: sniprintf(a, sizeof a, "{}"); break;
  }
  return sniprintf(out, cap, "[%u,\"%s\",%s]", (unsigned)e.t, type_name(e.type), a);
}

CORE_COLD void JsonGen::begin(const Take* take, bool journal_header) {
  t = take, stage = 0, i = 0, journal = journal_header, bi = 0, boff = 0, bfirst = true;
}

CORE_COLD bool JsonGen::next(Buf& o) {
  if (!t) return false;
  if (stage == 0) {   // the header to the dumps
    header(*t, o, journal);
    stage = 4;
    return true;
  }
  if (stage == 4) {   // the dumps, a piece a call
    if (sysex_piece(*t, o, bi, boff, bfirst)) return true;
    header_end(*t, o);
    if (journal) {
      o.n--;   // (the header line: its closing brace kept)
      o.adds("}\n");
      stage = 3;
      return true;
    }
    o.n--;   // reopen the object
    o.addf(",\"length\":%u,\"events\":[\n", (unsigned)t->end);
    stage = 1;
    return true;
  }
  if (stage == 1) {
    char b[160];
    int end = i + per < t->n ? i + per : t->n;
    for (; i < end; ++i) {
      int k = event_json(t->ev[i], b, sizeof b);
      o.add(b, k);
      o.adds(i + 1 < t->n ? ",\n" : "\n");
    }
    if (i >= t->n) stage = 2;
    return true;
  }
  if (stage == 2) {
    o.adds("]}\n");
    stage = 3;
    return true;
  }
  return false;
}

// ---- reading
namespace {

struct Lex {
  const char* s;
  int n, p;
  bool bad = false;
  void ws() {
    while (p < n && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p;
  }
  bool eat(char c) {
    ws();
    if (p < n && s[p] == c) { ++p; return true; }
    return false;
  }
  char peek() {
    ws();
    return p < n ? s[p] : 0;
  }
  // a string's span (no unescaping needed for our names: \" and \\ kept as the two characters' second)
  bool str(const char** a, int* len) {
    ws();
    if (p >= n || s[p] != '"') return bad = true, false;
    int q = ++p;
    while (p < n && s[p] != '"') p += s[p] == '\\' ? 2 : 1;
    if (p >= n) return bad = true, false;
    *a = s + q, *len = p - q;
    ++p;
    return true;
  }
  bool str_to(char* out, int cap) {
    const char* a;
    int len;
    if (!str(&a, &len)) return false;
    int o = 0;
    for (int i = 0; i < len && o < cap - 1; ++i) {
      if (a[i] == '\\' && i + 1 < len) ++i;
      out[o++] = a[i];
    }
    out[o] = 0;
    return true;
  }
  bool num(double* v) {
    ws();
    int q = p;
    bool neg = false;
    if (p < n && s[p] == '-') neg = true, ++p;
    double x = 0;
    int d = 0;
    while (p < n && s[p] >= '0' && s[p] <= '9') x = x * 10 + (s[p++] - '0'), ++d;
    if (p < n && s[p] == '.') {
      ++p;
      double f = 0.1;
      while (p < n && s[p] >= '0' && s[p] <= '9') x += (s[p++] - '0') * f, f *= 0.1, ++d;
    }
    if (p < n && (s[p] == 'e' || s[p] == 'E')) {   // (not written by us: skipped as a whole)
      ++p;
      if (p < n && (s[p] == '+' || s[p] == '-')) ++p;
      while (p < n && s[p] >= '0' && s[p] <= '9') ++p;
    }
    if (!d) { p = q; return bad = true, false; }
    *v = neg ? -x : x;
    return true;
  }
  bool num_i(int64_t* v) {
    double x;
    if (!num(&x)) return false;
    if (x > 9.0e18 || x < -9.0e18) return bad = true, false;   // (out of an int64's range: refused, not undefined)
    *v = (int64_t)(x < 0 ? x - 0.5 : x + 0.5);
    return true;
  }
  bool skip(int depth = 0) {
    if (depth > 32) return bad = true, false;
    char c = peek();
    if (c == '"') { const char* a; int l; return str(&a, &l); }
    if (c == '{') {
      ++p;
      if (eat('}')) return true;
      do {
        const char* a;
        int l;
        if (!str(&a, &l) || !eat(':') || !skip(depth + 1)) return bad = true, false;
      } while (eat(','));
      return eat('}') || (bad = true, false);
    }
    if (c == '[') {
      ++p;
      if (eat(']')) return true;
      do {
        if (!skip(depth + 1)) return false;
      } while (eat(','));
      return eat(']') || (bad = true, false);
    }
    if (c == 't' && p + 4 <= n && !strncmp(s + p, "true", 4)) return p += 4, true;
    if (c == 'f' && p + 5 <= n && !strncmp(s + p, "false", 5)) return p += 5, true;
    if (c == 'n' && p + 4 <= n && !strncmp(s + p, "null", 4)) return p += 4, true;
    double v;
    return num(&v);
  }
  // iterate an object's keys: call first with *started false
  bool key(bool* started, const char** k, int* kl) {
    if (!*started) {
      if (!eat('{')) return bad = true, false;
      *started = true;
      if (eat('}')) return false;
    } else {
      if (eat('}')) return false;
      if (!eat(',')) return bad = true, false;
    }
    return str(k, kl) && (eat(':') || (bad = true, false));
  }
};

bool is(const char* k, int kl, const char* name) { return (int)strlen(name) == kl && !strncmp(k, name, kl); }

bool read_b64(Lex& x, Buf& out) {
  const char* a;
  int l;
  if (!x.str(&a, &l)) return false;
  out.clear();
  if (!out.reserve(l / 4 * 3 + 4)) return false;
  int k = b64_decode(a, l, out.p, out.cap);
  if (k < 0) return x.bad = true, false;
  out.n = k;
  return true;
}

bool read_ints(Lex& x, int* v, int max, int* got) {
  if (!x.eat('[')) return x.bad = true, false;
  int k = 0;
  if (!x.eat(']')) {
    do {
      int64_t q;
      if (!x.num_i(&q)) return false;
      if (k < max) v[k] = (int)q;
      ++k;
    } while (x.eat(','));
    if (!x.eat(']')) return x.bad = true, false;
  }
  if (got) *got = k;
  return true;
}

bool read_config(Lex& x, MidiConfig* c) {
  bool st = false;
  const char *k, *a;
  int kl, al;
  config_default(c);
  while (x.key(&st, &k, &kl)) {
    int64_t v;
    if (is(k, kl, "read")) { if (!x.num_i(&v)) return false; c->valid = v != 0; }
    else if (is(k, kl, "encoder")) { if (!x.str(&a, &al)) return false; c->encoder_ext = is(a, al, "int+ext"); }
    else if (is(k, kl, "out_ch")) { if (!x.str(&a, &al)) return false; c->trk_ch = is(a, al, "trk"); }
    else if (is(k, kl, "param")) { if (!x.str(&a, &al)) return false; c->nrpn = is(a, al, "nrpn"); }
    else if (is(k, kl, "pc_receive")) { if (!x.num_i(&v)) return false; c->pc_receive = v != 0; }
    else if (is(k, kl, "pc_send")) { if (!x.num_i(&v)) return false; c->pc_send = v != 0; }
    else if (is(k, kl, "transport_send")) { if (!x.num_i(&v)) return false; c->transport_send = v != 0; }
    else if (is(k, kl, "clock_send")) { if (!x.num_i(&v)) return false; c->clock_send = v != 0; }
    else if (is(k, kl, "fx_ch")) { if (!x.num_i(&v)) return false; c->fx_ch = (uint8_t)v; }
    else if (is(k, kl, "auto_ch")) { if (!x.num_i(&v)) return false; c->auto_ch = (uint8_t)v; }
    else if (is(k, kl, "track_ch")) {
      int ch[kTracks];
      int got = 0;
      if (!read_ints(x, ch, kTracks, &got)) return false;
      for (int i = 0; i < kTracks && i < got; ++i) c->track_ch[i] = (uint8_t)ch[i];
    } else if (!x.skip()) return false;
  }
  return !x.bad;
}

bool read_start(Lex& x, Take* t) {
  bool st = false;
  const char* k;
  int kl;
  while (x.key(&st, &k, &kl)) {
    int64_t v;
    double d;
    if (is(k, kl, "pattern")) { if (!x.num_i(&v)) return false; t->pattern = (int)v; }
    else if (is(k, kl, "tempo")) { if (!x.num(&d)) return false; t->tempo = (uint32_t)(d * 120 + 0.5); }
    else if (is(k, kl, "mutes")) { if (!x.num_i(&v)) return false; t->mutes = (uint32_t)v; }
    else if (is(k, kl, "scene")) { if (!x.num_i(&v)) return false; t->scene = (int)v; }
    else if (is(k, kl, "rytm_sysex")) {
      bool s2 = false;
      const char* k2;
      int kl2;
      while (x.key(&s2, &k2, &kl2)) {
        Buf* b = is(k2, kl2, "kit_x") ? &t->kit : is(k2, kl2, "pattern_x") ? &t->pat : is(k2, kl2, "settings_x") ? &t->settings
                 : is(k2, kl2, "global_x") ? &t->global : is(k2, kl2, "kit2_x") ? &t->kit2 : nullptr;
        if (b ? !read_b64(x, *b) : !x.skip()) return false;
      }
      if (x.bad) return false;
    } else if (is(k, kl, "sidecar")) {
      bool s2 = false;
      const char* k2;
      int kl2;
      while (x.key(&s2, &k2, &kl2)) {
        if (!is(k2, kl2, "send_fx")) { if (!x.skip()) return false; continue; }
        bool s3 = false;
        const char* k3;
        int kl3;
        while (x.key(&s3, &k3, &kl3)) {
          if (is(k3, kl3, "send")) {
            if (!x.eat('[')) return false;
            int i = 0;
            if (!x.eat(']')) {
              do {
                int p[2] = {0, 0};
                if (!read_ints(x, p, 2, nullptr)) return false;
                if (i < kTracks) t->fx_send[i][0] = (uint8_t)p[0], t->fx_send[i][1] = (uint8_t)p[1];
                ++i;
              } while (x.eat(','));
              if (!x.eat(']')) return false;
            }
          } else if (is(k3, kl3, "knob")) {
            if (!x.eat('[')) return false;
            int s = 0;
            if (!x.eat(']')) {
              do {
                int p[6] = {0, 0, 0, 0, 0, 0};
                if (!read_ints(x, p, 6, nullptr)) return false;
                for (int j = 0; j < 6 && s < 2; ++j) t->fx_knob[s][j] = (uint8_t)p[j];
                ++s;
              } while (x.eat(','));
              if (!x.eat(']')) return false;
            }
          } else if (is(k3, kl3, "route")) {
            if (!x.num_i(&v)) return false;
            t->fx_route = (uint8_t)v;
          } else if (!x.skip()) return false;
        }
        t->has_fx = !x.bad;
      }
    } else if (!x.skip()) return false;
  }
  return !x.bad;
}

// [t,"type",{...}]
bool read_event(Lex& x, Ev* e) {
  memset(e, 0, sizeof *e);
  int64_t t;
  const char* ty;
  int tl;
  if (!x.eat('[') || !x.num_i(&t) || !x.eat(',') || !x.str(&ty, &tl) || !x.eat(',')) return x.bad = true, false;
  e->t = (uint32_t)t;
  e->type = (uint8_t)type_of(ty, tl);
  bool st = false;
  const char *k, *a;
  int kl, al;
  while (x.key(&st, &k, &kl)) {
    int64_t v;
    if (is(k, kl, "nrpn")) {
      int p[2] = {0, 0};
      if (!read_ints(x, p, 2, nullptr)) return false;
      e->b = (uint16_t)((p[0] & 127) << 7 | (p[1] & 127));
      continue;
    }
    if (is(k, kl, "k")) {
      if (!x.str(&a, &al)) return false;
      e->a = is(a, al, "send") ? kScSend : is(a, al, "knob") ? kScKnob : kScRoute;
      continue;
    }
    if (is(k, kl, "bpm")) {
      double d;
      if (!x.num(&d)) return false;
      e->v = (uint32_t)(d * 120 + 0.5);
      continue;
    }
    if (x.peek() != '-' && (x.peek() < '0' || x.peek() > '9')) {
      if (!x.skip()) return false;
      continue;
    }
    if (!x.num_i(&v)) return false;
    switch (e->type) {
      case kPc: if (is(k, kl, "to")) e->a = (uint8_t)v; break;
      case kSw: if (is(k, kl, "from")) e->a = (uint8_t)v; else if (is(k, kl, "to")) e->b = (uint16_t)v; break;
      case kParam: if (is(k, kl, "trk")) e->a = (uint8_t)v; else if (is(k, kl, "v")) e->v = (uint32_t)v; break;
      case kCc: if (is(k, kl, "ch")) e->a = (uint8_t)v; else if (is(k, kl, "n")) e->b = (uint16_t)v; else if (is(k, kl, "v")) e->v = (uint32_t)v; break;
      case kMute: if (is(k, kl, "mask")) e->v = (uint32_t)v; break;
      case kScene: if (is(k, kl, "n")) e->a = (uint8_t)v; break;
      case kPerf: if (is(k, kl, "n")) e->a = (uint8_t)v; else if (is(k, kl, "v")) e->v = (uint32_t)v; break;
      case kSc: if (is(k, kl, "i")) e->b = (uint16_t)v; else if (is(k, kl, "v")) e->v = (uint32_t)v; break;
      case kMach: if (is(k, kl, "trk")) e->a = (uint8_t)v; else if (is(k, kl, "m")) e->b = (uint16_t)v; break;
      case kSrc: if (is(k, kl, "trk")) e->a = (uint8_t)v; else if (is(k, kl, "p")) e->b = (uint16_t)(v & 15); else if (is(k, kl, "v")) e->v = (uint32_t)v; break;
      case kFill: if (is(k, kl, "on")) e->a = (uint8_t)v; break;
      default: if (is(k, kl, "ms")) e->ms = (uint32_t)v; break;
    }
  }
  if (x.bad || !x.eat(']')) return x.bad = true, false;
  return true;
}

// the top object's keys up to "events" (or its end); stage: 0 before '{', 1 in the object, 2 in "events"
bool read_head(Lex& x, Take* t, bool* at_events) {
  bool st = false;
  const char* k;
  int kl;
  *at_events = false;
  while (x.key(&st, &k, &kl)) {
    int64_t v;
    if (is(k, kl, "format")) {
      const char* a;
      int al;
      if (!x.str(&a, &al)) return false;
      if (!is(a, al, "sidecar-song")) return x.bad = true, false;
    } else if (is(k, kl, "version")) {
      if (!x.num_i(&v)) return false;
      if (v != 1) return x.bad = true, false;
    } else if (is(k, kl, "project")) { if (!x.str_to(t->project, sizeof t->project)) return false; }
    else if (is(k, kl, "name")) { if (!x.str_to(t->name, sizeof t->name)) return false; }
    else if (is(k, kl, "recovered")) { if (!x.num_i(&v)) return false; t->recovered = v != 0; }
    else if (is(k, kl, "length")) { if (!x.num_i(&v)) return false; t->end = (uint32_t)v; }
    else if (is(k, kl, "midi_config")) { if (!read_config(x, &t->cfg)) return false; }
    else if (is(k, kl, "start")) { if (!read_start(x, t)) return false; }
    else if (is(k, kl, "events")) {
      if (!x.eat('[')) return x.bad = true, false;
      *at_events = true;
      return true;
    } else if (!x.skip()) return false;
  }
  return !x.bad;
}

}  // namespace

CORE_COLD void JsonRead::begin(const char* text, int len, Take* take) {
  s = text, n = len, pos = 0, stage = 0, t = take, ok = false, done = false;
  err[0] = 0;
  t->clear();
}

CORE_COLD bool JsonRead::step(int max_events) {
  if (done) return false;
  Lex x{s, n, pos};
  auto fail = [&](const char* why) {
    sniprintf(err, sizeof err, "%s at byte %d", why, x.p);
    ok = false, done = true;
    return false;
  };
  if (stage == 0) {
    bool ev = false;
    if (!read_head(x, t, &ev)) return fail("bad header");
    if (!ev) {   // no events: a take with none
      ok = true, done = true;
      return false;
    }
    stage = 1;
    if (x.eat(']')) stage = 2;
    pos = x.p;
    return true;
  }
  if (stage == 1) {
    for (int k = 0; k < max_events; ++k) {
      Ev e;
      if (!read_event(x, &e)) return fail("bad event");
      if (!t->push(e)) return fail("too many events");
      if (x.eat(',')) continue;
      if (!x.eat(']')) return fail("bad event list");
      stage = 2;
      break;
    }
    pos = x.p;
    return true;
  }
  // after the events: the rest of the object (keys after "events" read, as read_head would)
  bool st = true;
  const char* k;
  int kl;
  while (x.key(&st, &k, &kl)) {
    int64_t v;
    if (is(k, kl, "length")) { if (!x.num_i(&v)) return fail("bad length"); t->end = (uint32_t)v; }
    else if (!x.skip()) return fail("bad trailer");
  }
  if (x.bad) return fail("bad trailer");
  if (!t->end && t->n) t->end = t->ev[t->n - 1].t;
  ok = true, done = true;
  return false;
}

CORE_COLD bool read_json(const char* s, int n, Take* t, char* err, int errcap) {
  JsonRead r;
  r.begin(s, n, t);
  while (r.step(1 << 20)) {}
  if (err) sniprintf(err, errcap, "%s", r.err);
  return r.ok;
}

CORE_COLD bool recover_journal(const char* s, int n, Take* t, char* err, int errcap) {
  t->clear();
  int line_end = 0;
  while (line_end < n && s[line_end] != '\n') ++line_end;
  Lex x{s, line_end, 0};
  bool ev = false;
  if (!read_head(x, t, &ev) || ev) {
    if (err) sniprintf(err, errcap, "bad journal header");
    return false;
  }
  int p = line_end + 1;
  uint32_t last = 0;
  while (p < n) {
    int e = p;
    while (e < n && s[e] != '\n') ++e;
    if (e >= n) break;   // a cut last line: dropped
    Lex y{s, e, p};
    if (y.peek() == '[') {
      Ev v;
      if (!read_event(y, &v)) break;
      if (!t->push(v)) break;
      last = v.t;
    } else if (y.peek() == '{') {   // {"end":t}
      bool st = false;
      const char* k;
      int kl;
      while (y.key(&st, &k, &kl)) {
        int64_t q;
        if (is(k, kl, "end") && y.num_i(&q)) last = (uint32_t)q;
        else if (!y.skip()) break;
      }
    }
    p = e + 1;
  }
  t->end = last;
  t->recovered = true;
  return true;
}

}  // namespace song
}  // namespace core
#endif
