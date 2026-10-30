// core/msamp/sfz.cpp - see sfz.h. Control core only (an instrument's load), cold.
#include "../platform.h"

#if CORE_MSAMP
#include "sfz.h"

#include <string.h>

#include "text.h"

namespace core {
namespace msamp {

namespace {

constexpr int kDefines = 32;
struct Define {
  char name[32];
  char value[64];
};

// The opcodes one header level sets: a region's fields plus the strings (sample, default_path) not yet resolved.
struct Level {
  Region r;
  char sample[kPath];
  bool has_sample;
};

enum Header { kNone, kControl, kGlobal, kMaster, kGroup, kRegion, kSkip };

CORE_COLD Region default_region() {
  Region r;
  memset(&r, 0, sizeof r);
  r.sample = -1;
  r.lokey = 0, r.hikey = 127, r.keycenter = 60, r.lovel = 1, r.hivel = 127;
  r.trigger = kTrigAttack;
  r.loop_mode = kLoopAuto;
  r.pitch_keytrack = 100;
  r.amplitude = 100, r.amp_veltrack = 100;
  r.width = 100;
  r.end = kUnset, r.loop_start = kUnset, r.loop_end = kUnset;
  r.seq_length = 1, r.seq_position = 1;
  r.lorand = 0, r.hirand = 1;
  r.sustain = 100;
  r.release = 0.001f;   // sfz's ampeg_release default
  return r;
}

CORE_COLD bool num(const char* s, float* out) {
  const char* e;
  double x = to_double(s, &e);
  if (e == s || *e) return false;
  *out = (float)x;
  return true;
}

struct Parser {
  Desc* d;
  const char* dir;
  Define defs[kDefines];
  int ndefs = 0;
  char default_path[kPath] = "";
  int note_offset = 0, octave_offset = 0;
  Header at = kNone;
  Level global, master, group, region;
  bool region_open = false;
  bool has_master = false, has_group = false;   // since the last <global> / <master>
  bool disabled = false;   // end=-1 on the region (or inherited)

  void ignored(const char* name) {
    if (!d->ignored++ && name) {
      strncpy(d->first_ignored, name, sizeof d->first_ignored - 1);
      d->first_ignored[sizeof d->first_ignored - 1] = 0;
    }
  }
  int key(const char* v) {
    int n = note_value(v);
    if (n < 0) return -1;
    n += note_offset + 12 * octave_offset;
    return n < 0 ? 0 : n > 127 ? 127 : n;
  }
  Level* cur() {
    switch (at) {
      case kGlobal: return &global;
      case kMaster: return &master;
      case kGroup: return &group;
      case kRegion: return &region;
      default: return nullptr;
    }
  }
  int sample_index(const char* path) {
    for (int i = 0; i < d->nsamples; ++i)
      if (strcasecmp(d->samples[i], path) == 0) return i;
    if (d->nsamples >= d->max_samples) return -1;
    strcpy(d->samples[d->nsamples], path);
    return d->nsamples++;
  }
  // The open region into the table.
  void commit() {
    if (!region_open) return;
    region_open = false;
    Region r = region.r;
    bool drop = !region.has_sample || r.lokey > r.hikey || r.lovel > r.hivel || r.end == 0xFFFFFFFEu ||
                d->nregions >= d->max_regions;
    if (!drop) {
      char rel[kPath], full[kPath];
      // default_path + sample as text (a sample with a leading '/' or '\\' is the card's own path), then on dir
      bool abs_s = region.sample[0] == '/' || region.sample[0] == '\\';
      int dl = (int)strlen(default_path);
      bool ok = sniprintf(rel, sizeof rel, "%s%s%s", abs_s ? "" : default_path,
                          !abs_s && dl && default_path[dl - 1] != '/' && default_path[dl - 1] != '\\' ? "/" : "",
                          region.sample) < (int)sizeof rel &&
                join_path(dir, rel, full, sizeof full);
      int i = ok ? sample_index(full) : -1;
      if (i < 0) drop = true;
      else r.sample = (int16_t)i;
    }
    if (drop) {
      ++d->skipped;
      return;
    }
    d->regions[d->nregions++] = r;
  }
  void header(const char* h) {
    commit();
    if (!strcmp(h, "control")) at = kControl;
    else if (!strcmp(h, "global")) at = kGlobal, global = Level{default_region(), "", false}, has_master = has_group = false;
    else if (!strcmp(h, "master")) at = kMaster, master = global, has_master = true, has_group = false;
    else if (!strcmp(h, "group")) at = kGroup, group = has_master ? master : global, has_group = true;
    else if (!strcmp(h, "region")) at = kRegion, region = has_group ? group : has_master ? master : global, region_open = true;
    else {
      at = kSkip;
      if (strcmp(h, "curve") && strcmp(h, "effect") && strcmp(h, "midi") && strcmp(h, "sample")) ++d->malformed;
    }
  }
  void opcode(const char* name, const char* v) {
    if (at == kSkip) {
      ignored(name);
      return;
    }
    if (at == kControl || at == kNone) {
      if (!strcmp(name, "default_path")) {
        strncpy(default_path, v, kPath - 1);
        default_path[kPath - 1] = 0;
        return;
      }
      if (!strcmp(name, "note_offset")) { note_offset = to_int(v); return; }
      if (!strcmp(name, "octave_offset")) { octave_offset = to_int(v); return; }
      if (!strcmp(name, "sidecar_load")) {
        d->load = !strcmp(v, "ram") ? 1 : !strcmp(v, "stream") ? 2 : 0;
        return;
      }
      if (at == kControl) {
        ignored(name);
        return;
      }
      // opcodes before any header: as <global>'s (some files start with a bare <region>-less list)
      at = kGlobal;
    }
    Level* L = cur();
    Region& r = L->r;
    float x = 0;
    int k;
    if (!strcmp(name, "sample")) {
      strncpy(L->sample, v, kPath - 1);
      L->sample[kPath - 1] = 0;
      L->has_sample = v[0] != 0;
    } else if (!strcmp(name, "lokey") || !strcmp(name, "hikey") || !strcmp(name, "key") ||
               !strcmp(name, "pitch_keycenter")) {
      if ((k = key(v)) < 0) { ++d->malformed; return; }
      if (name[0] == 'l') r.lokey = (uint8_t)k;
      else if (name[0] == 'h') r.hikey = (uint8_t)k;
      else if (name[0] == 'p') r.keycenter = (uint8_t)k;
      else r.lokey = r.hikey = r.keycenter = (uint8_t)k;
    } else if (!strcmp(name, "lovel") || !strcmp(name, "hivel")) {
      k = to_int(v);
      k = k < 0 ? 0 : k > 127 ? 127 : k;
      if (name[0] == 'l') r.lovel = (uint8_t)k;
      else r.hivel = (uint8_t)k;
    } else if (!strcmp(name, "trigger")) {
      if (!strcmp(v, "release")) r.trigger = kTrigRelease;
      else if (!strcmp(v, "attack") || !strcmp(v, "first") || !strcmp(v, "legato")) r.trigger = kTrigAttack;
      else ++d->malformed;
    } else if (!strcmp(name, "loop_mode") || !strcmp(name, "loopmode")) {
      if (!strcmp(v, "no_loop")) r.loop_mode = kNoLoop;
      else if (!strcmp(v, "one_shot")) r.loop_mode = kOneShot;
      else if (!strcmp(v, "loop_continuous")) r.loop_mode = kLoopContinuous;
      else if (!strcmp(v, "loop_sustain")) r.loop_mode = kLoopSustain;
      else ++d->malformed;
    } else if (!known(name)) {
      ignored(name);
    } else if (!num(v, &x)) {
      ++d->malformed;   // every other opcode of the subset is a number
    } else if (!strcmp(name, "tune")) r.tune = x;
    else if (!strcmp(name, "transpose")) r.transpose = (int16_t)x;
    else if (!strcmp(name, "pitch_keytrack")) r.pitch_keytrack = (int16_t)x;
    else if (!strcmp(name, "volume")) r.volume = x;
    else if (!strcmp(name, "amplitude")) r.amplitude = x;
    else if (!strcmp(name, "pan")) r.pan = x < -100 ? -100 : x > 100 ? 100 : x;
    else if (!strcmp(name, "width")) r.width = x < -100 ? -100 : x > 100 ? 100 : x;
    else if (!strcmp(name, "position")) r.position = x < -100 ? -100 : x > 100 ? 100 : x;
    else if (!strcmp(name, "amp_veltrack")) r.amp_veltrack = x;
    else if (!strcmp(name, "offset")) r.offset = x < 0 ? 0 : (uint32_t)x;
    else if (!strcmp(name, "end")) r.end = x < 0 ? 0xFFFFFFFEu : (uint32_t)x;   // -1: the region is off
    else if (!strcmp(name, "loop_start") || !strcmp(name, "loopstart")) r.loop_start = x < 0 ? 0 : (uint32_t)x;
    else if (!strcmp(name, "loop_end") || !strcmp(name, "loopend")) r.loop_end = x < 0 ? 0 : (uint32_t)x;
    else if (!strcmp(name, "seq_length")) r.seq_length = (uint16_t)(x < 1 ? 1 : x > 100 ? 100 : x);
    else if (!strcmp(name, "seq_position")) r.seq_position = (uint16_t)(x < 1 ? 1 : x > 100 ? 100 : x);
    else if (!strcmp(name, "lorand")) r.lorand = x;
    else if (!strcmp(name, "hirand")) r.hirand = x;
    else if (!strcmp(name, "ampeg_attack")) r.attack = x < 0 ? 0 : x;
    else if (!strcmp(name, "ampeg_hold")) r.hold = x < 0 ? 0 : x;
    else if (!strcmp(name, "ampeg_decay")) r.decay = x < 0 ? 0 : x;
    else if (!strcmp(name, "ampeg_sustain")) r.sustain = x < 0 ? 0 : x > 100 ? 100 : x;
    else if (!strcmp(name, "ampeg_release")) r.release = x < 0 ? 0 : x;
  }
  static bool known(const char* n) {
    static const char* const k[] = {"tune", "transpose", "pitch_keytrack", "volume", "amplitude", "pan", "width", "position",
                                    "amp_veltrack", "offset", "end", "loop_start", "loopstart", "loop_end", "loopend",
                                    "seq_length", "seq_position", "lorand", "hirand", "ampeg_attack", "ampeg_hold",
                                    "ampeg_decay", "ampeg_sustain", "ampeg_release"};
    for (const char* x : k)
      if (!strcmp(n, x)) return true;
    return false;
  }
  // $NAME in v replaced by its #define (longest name first is not needed: names end at a non-word character)
  void substitute(const char* v, char* out, int cap) {
    int o = 0;
    for (int i = 0; v[i] && o < cap - 1;) {
      if (v[i] == '$') {
        int j = i + 1;
        while (is_word(v[j])) ++j;
        bool done = false;
        for (int k = 0; k < ndefs && !done; ++k)
          if ((int)strlen(defs[k].name) == j - i && !strncmp(defs[k].name, v + i, j - i)) {
            for (const char* p = defs[k].value; *p && o < cap - 1;) out[o++] = *p++;
            done = true;
          }
        if (done) {
          i = j;
          continue;
        }
      }
      out[o++] = v[i++];
    }
    out[o] = 0;
  }
};

CORE_COLD bool word(char c) { return is_word(c); }

}  // namespace

CORE_COLD int note_value(const char* s) {
  if (!s || !*s) return -1;
  if (is_digit(s[0]) || (s[0] == '-' && is_digit(s[1]))) {
    const char* e;
    long n = to_long(s, &e);
    return *e || n < 0 || n > 127 ? -1 : (int)n;
  }
  static const int8_t kPc[7] = {9, 11, 0, 2, 4, 5, 7};   // a..g
  char c = lower(s[0]);
  if (c < 'a' || c > 'g') return -1;
  int pc = kPc[c - 'a'], i = 1;
  if (s[i] == '#') ++pc, ++i;
  else if (s[i] == 'b' && (is_digit(s[i + 1]) || s[i + 1] == '-')) --pc, ++i;
  const char* e;
  long oct = to_long(s + i, &e);
  if (e == s + i || *e) return -1;
  long n = (oct + 1) * 12 + pc;
  return n < 0 || n > 127 ? -1 : (int)n;
}

CORE_COLD bool join_path(const char* a, const char* b, char* out, int cap) {
  // segments of a (unless b is absolute) then b, each split on '/' or '\'
  char buf[2 * kPath + 2];
  int n = 0;
  bool abs_b = b[0] == '/' || b[0] == '\\';
  const char* parts[2] = {abs_b ? "" : a, b};
  for (int p = 0; p < 2; ++p) {
    for (const char* s = parts[p]; *s && n < (int)sizeof buf - 2; ++s) buf[n++] = *s == '\\' ? '/' : *s;
    if (n < (int)sizeof buf - 1) buf[n++] = '/';
  }
  buf[n] = 0;
  int o = 0;
  out[0] = 0;
  for (char* seg = buf; *seg;) {
    char* e = strchr(seg, '/');
    *e = 0;
    if (!*seg || !strcmp(seg, ".")) {
    } else if (!strcmp(seg, "..")) {
      if (o == 0) return false;   // above the root
      while (o > 0 && out[o - 1] != '/') --o;
      if (o > 0) --o;
      out[o] = 0;
    } else {
      int l = (int)strlen(seg);
      if (o + 1 + l >= cap) return false;
      out[o++] = '/';
      memcpy(out + o, seg, l);
      o += l;
      out[o] = 0;
    }
    seg = e + 1;
  }
  if (o == 0) {
    if (cap < 2) return false;
    out[0] = '/', out[1] = 0;
  }
  return true;
}

CORE_COLD bool parse(const char* t, int len, const char* dir, Desc* d) {
  d->nregions = d->nsamples = d->ignored = d->skipped = d->malformed = d->load = 0;
  d->first_ignored[0] = d->err[0] = 0;
  Parser* P = new Parser;   // ~1 KB of levels and defines: off the control core's stack
  Parser& p = *P;
  p.d = d;
  p.dir = dir;
  p.global = Level{default_region(), "", false};
  p.master = p.group = p.region = p.global;
  int i = 0;
  auto line_end = [&](int j) {
    while (j < len && t[j] != '\n' && t[j] != '\r') ++j;
    return j;
  };
  while (i < len) {
    char c = t[i];
    if (c == 0) {
      ++d->malformed;   // binary: stop
      break;
    }
    if (is_space(c)) { ++i; continue; }
    if (c == '/' && i + 1 < len && t[i + 1] == '/') { i = line_end(i); continue; }
    if (c == '/' && i + 1 < len && t[i + 1] == '*') {
      i += 2;
      while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) ++i;
      i += 2;
      continue;
    }
    if (c == '<') {
      int j = i + 1;
      char h[24];
      int k = 0;
      while (j < len && t[j] != '>' && t[j] != '\n' && k < (int)sizeof h - 1) h[k++] = lower(t[j++]);
      h[k] = 0;
      if (j >= len || t[j] != '>') {
        ++d->malformed;
        i = j;
        continue;
      }
      p.header(h);
      i = j + 1;
      continue;
    }
    if (c == '#') {
      int e = line_end(i);
      char ln[160];
      int l = e - i < (int)sizeof ln - 1 ? e - i : (int)sizeof ln - 1;
      memcpy(ln, t + i, l);
      ln[l] = 0;
      char name[32], value[64];
      // "#define $NAME value": two words after it
      auto word_at = [](const char* q, char* o, int cap) {
        while (is_space(*q)) ++q;
        int k = 0;
        while (*q && !is_space(*q) && k < cap - 1) o[k++] = *q++;
        o[k] = 0;
        return q;
      };
      bool two = false;
      if (!strncmp(ln, "#define", 7)) {
        const char* q = word_at(ln + 7, name, sizeof name);
        word_at(q, value, sizeof value);
        two = name[0] && value[0];
      }
      if (two && name[0] == '$' && p.ndefs < kDefines) {
        strcpy(p.defs[p.ndefs].name, name);
        strcpy(p.defs[p.ndefs].value, value);
        ++p.ndefs;
      } else {
        ++d->malformed;   // #include, or a bad #define
      }
      i = e;
      continue;
    }
    // an opcode: name=value
    int j = i;
    while (j < len && word(t[j])) ++j;
    if (j == i || j >= len || t[j] != '=') {
      ++d->malformed;
      while (j < len && !is_space(t[j]) && t[j] != '<') ++j;
      i = j > i ? j : i + 1;
      continue;
    }
    char name[40];
    int nl = j - i < (int)sizeof name - 1 ? j - i : (int)sizeof name - 1;
    memcpy(name, t + i, nl);
    name[nl] = 0;
    int v0 = j + 1, v1 = v0;
    if (!strcmp(name, "sample") || !strcmp(name, "default_path")) {
      // to the line's end, a comment, a header or the next " name=" (spaces in file names)
      int e = line_end(v0);
      v1 = v0;
      while (v1 < e) {
        if (t[v1] == '<' || (t[v1] == '/' && v1 + 1 < e && t[v1 + 1] == '/')) break;
        if (is_space(t[v1])) {
          int q = v1;
          while (q < e && is_space(t[q])) ++q;
          int r = q;
          while (r < e && word(t[r])) ++r;
          if (r > q && r < e && t[r] == '=') break;
        }
        ++v1;
      }
      while (v1 > v0 && is_space(t[v1 - 1])) --v1;
    } else {
      while (v1 < len && !is_space(t[v1]) && t[v1] != '<') ++v1;
    }
    char raw[kPath], val[kPath];
    int vl = v1 - v0 < kPath - 1 ? v1 - v0 : kPath - 1;
    memcpy(raw, t + v0, vl);
    raw[vl] = 0;
    p.substitute(raw, val, sizeof val);
    p.opcode(name, val);
    i = v1 > j + 1 ? v1 : j + 1;
  }
  p.commit();
  delete P;
  if (!d->nregions) {
    sniprintf(d->err, sizeof d->err, "no playable region (%d skipped, %d malformed)", d->skipped, d->malformed);
    return false;
  }
  return true;
}

}  // namespace msamp
}  // namespace core
#endif
