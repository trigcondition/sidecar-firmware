// core/lib/pools_fmt.cpp - see pools_fmt.h, and pools.h's pure helpers (names, rows, types). No card, no globals
// written: portable (the fuzz target builds it alone).
#include "pools_fmt.h"

#include <string.h>
#include <strings.h>

#include "../platform.h"
#include "pools.h"

namespace core {
namespace pools {

namespace {
const char* const kListNames[kLists] = {"SAMPLES", "WAVETABLES", "INSTRUMENTS"};
const char* const kTags[kLists] = {"smp", "wt", "inst"};
const char* const kFormats[kLists] = {"samplepool", "wtpool", "instpool"};
const char* const kInputNames[kInputs] = {"IN", "MAIN", "BD", "SD", "RS-CP", "BT", "LT", "MT-HT", "CH-OH", "CY-CB"};
const char* const kSmpBuiltinNames[kSmpBuiltins] = {"PAD", "BELLS"};
const char* const kWtBuiltinNames[kWtBuiltins] = {"BASIC", "PWM", "VOCAL", "ORGAN", "FM", "SYNC"};
const char* const kTypeWords[] = {"", "", "sample", "input", "builtin", "instrument"};

bool ends(const char* n, const char* ext) {
  int l = (int)strlen(n), e = (int)strlen(ext);
  return l > e && strcasecmp(n + l - e, ext) == 0;
}
bool say(char* why, int cap, const char* m) {
  if (why && cap) sniprintf(why, cap, "%s", m);
  return false;
}
}  // namespace

void row_text(int c, char* out) {
  int r = row_of(c);
  out[0] = (char)('A' + bank_of(c));
  out[1] = (char)('0' + r / 100), out[2] = (char)('0' + r / 10 % 10), out[3] = (char)('0' + r % 10), out[4] = 0;
}
bool parse_row(const char* s, int* c, const char** end) {
  if (!s) return false;
  char b = *s;
  int bank = b >= 'A' && b <= 'H' ? b - 'A' : b >= 'a' && b <= 'h' ? b - 'a' : -1;
  if (bank < 0) return false;
  const char* p = s + 1;
  int r = 0, d = 0;
  while (*p >= '0' && *p <= '9' && d < 4) r = r * 10 + (*p++ - '0'), ++d;
  if (!d || d > 3 || r > 127) return false;
  if (c) *c = code(bank, r);
  if (end) *end = p;
  return true;
}

const char* list_name(int l) { return l >= 0 && l < kLists ? kListNames[l] : "?"; }
const char* list_tag(int l) { return l >= 0 && l < kLists ? kTags[l] : "?"; }
const char* format_of(int l) { return l >= 0 && l < kLists ? kFormats[l] : ""; }
int list_of_tag(const char* t) {
  for (int l = 0; l < kLists; ++l)
    if (t && !strcmp(t, kTags[l])) return l;
  return -1;
}
const char* input_name(int i) { return i >= 0 && i < kInputs ? kInputNames[i] : ""; }
int builtins(int list) { return list == kSamples ? kSmpBuiltins : list == kTables ? kWtBuiltins : 0; }
const char* builtin_name(int list, int i) {
  if (i < 0 || i >= builtins(list)) return "";
  return list == kSamples ? kSmpBuiltinNames[i] : kWtBuiltinNames[i];
}
int builtin_of(int list, const char* name) {
  for (int i = 0; i < builtins(list); ++i)
    if (name && !strcmp(name, builtin_name(list, i))) return i;
  return -1;
}
int input_of(const char* name) {
  for (int i = 0; i < kInputs; ++i)
    if (name && !strcmp(name, kInputNames[i])) return i;
  return -1;
}
bool takes_type(int list, int type) {
  switch (list) {
    case kSamples: return type == kSample || type == kInput || type == kBuiltin;
    case kTables: return type == kSample || type == kBuiltin;
    case kInstr: return type == kInstrument;
    default: return false;
  }
}
bool accepts_file(int list, const char* path) {
  if (!path || path[0] != '/') return false;
  const char* b = strrchr(path, '/');
  b = b ? b + 1 : path;
  if (!b[0] || b[0] == '.') return false;
  if (list == kInstr) return ends(b, ".sfz");
  if (list == kSamples || list == kTables) return ends(b, ".wav") || ends(b, ".aif") || ends(b, ".aiff");
  return false;
}
bool playable(const Entry& e) {
  switch (e.type) {
    case kSample:
    case kInstrument: return e.st != lib::kMissing && e.path[0];
    case kInput:
    case kBuiltin: return true;
    default: return false;
  }
}

namespace text {

namespace {
// a decimal at p (no sign): false when none or past max
bool num(const char** p, uint32_t* v, uint32_t max) {
  const char* q = *p;
  if (*q < '0' || *q > '9') return false;
  uint64_t x = 0;
  while (*q >= '0' && *q <= '9') {
    x = x * 10 + (uint64_t)(*q++ - '0');
    if (x > max) return false;
  }
  *v = (uint32_t)x;
  *p = q;
  return true;
}
bool printable(const char* l, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if ((uint8_t)l[i] < 0x20 && l[i] != '\t') return false;
  return true;
}
// a sample / instrument line's tail after the ref: [TAB name [TAB frames ch rate]]
bool extras(const char* p, Line* o) {
  if (!*p) return true;
  if (*p++ != '\t') return false;
  const char* t = strchr(p, '\t');
  int l = t ? (int)(t - p) : (int)strlen(p);
  if (l >= kName) return false;
  memcpy(o->name, p, l);
  o->name[l] = 0;
  if (!t) return true;
  p = t + 1;
  uint32_t fr, ch, rate;
  if (!num(&p, &fr, 0xFFFFFFFFu) || *p++ != ' ' || !num(&p, &ch, 255) || *p++ != ' ' || !num(&p, &rate, 10000000))
    return false;
  if (*p) return false;
  o->frames = fr, o->ch = (uint8_t)ch, o->rate = rate;
  return true;
}
}  // namespace

bool parse(int list, const char* s, uint32_t n, LineFn fn, void* ctx, char* why, int cap) {
  Parser p;
  parser_init(&p);
  return parse_more(list, &p, s, n, fn, ctx, why, cap);
}
bool parse_more(int list, Parser* ps, const char* s, uint32_t n, LineFn fn, void* ctx, char* why, int cap) {
  if (list < 0 || list >= kLists) return say(why, cap, "no such list");
  char line[400];
  bool& inst = ps->inst;   // an instrument line came before (members follow it)
  int& last = ps->last;    // the last row (rows in order, each once)
  for (uint32_t a = 0; a < n;) {
    uint32_t b = a;
    while (b < n && s[b] != '\n') {
      if (!s[b]) return say(why, cap, "a NUL byte (binary data)");
      ++b;
    }
    uint32_t e = b;
    if (e > a && s[e - 1] == '\r') --e;
    uint32_t len = e - a;
    const char* l0 = s + a;
    a = b + 1;
    if (!len) continue;
    if (len >= sizeof line) return say(why, cap, "a line too long");
    if (!printable(l0, len)) return say(why, cap, "a control character");
    memcpy(line, l0, len);
    line[len] = 0;
    if (line[0] == '#') continue;   // (only the header today; comments are allowed)
    Line o;
    memset(&o, 0, sizeof o);
    const char* p = line;
    if (p[0] == ' ') {   // " m <ref>": a member of the instrument before
      if (list != kInstr || !inst || p[1] != 'm' || p[2] != ' ') return say(why, cap, "a member line without an instrument");
      if (!lib::parse_ref(p + 3, &o.ref)) return say(why, cap, "a member's reference it does not read");
      const char* t = strchr(p + 3, '\t');
      if (t) return say(why, cap, "a member line with extras");
      o.member = true, o.code = last, o.type = kSample;
    } else {
      int c;
      if (!parse_row(p, &c, &p) || *p++ != ' ') return say(why, cap, "not a row");
      if (row_of(c) == 0) return say(why, cap, "row 000 is OFF");
      if (c <= last) return say(why, cap, "rows out of order or twice");
      last = c;
      o.code = c;
      int t = -1;
      for (int k = kSample; k <= kInstrument; ++k) {
        int wl = (int)strlen(kTypeWords[k]);
        if (!strncmp(p, kTypeWords[k], wl) && p[wl] == ' ') t = k, p += wl + 1;
      }
      if (t < 0 || !takes_type(list, t)) return say(why, cap, "a type this list does not hold");
      o.type = (uint8_t)t;
      if (t == kInput || t == kBuiltin) {
        int id = t == kInput ? input_of(p) : builtin_of(list, p);
        if (id < 0) return say(why, cap, "an input / built-in it does not know");
        o.id = (uint8_t)id;
        sniprintf(o.name, sizeof o.name, "%s", p);
      } else {
        if (!lib::parse_ref(p, &o.ref)) return say(why, cap, "a reference it does not read");
        const char* tail = strchr(p, '\t');
        if (!extras(tail ? tail : "", &o)) return say(why, cap, "extras it does not read");
        if (t == kInstrument && !accepts_file(kInstr, o.ref.hint)) return say(why, cap, "an instrument that is not an .sfz");
      }
      inst = t == kInstrument;
    }
    if (fn && !fn(ctx, o)) return say(why, cap, "too many entries");
  }
  return true;
}

int put(int list, const Line& l, char* out, int cap) {
  int n;
  if (l.member) {
    char r[lib::kPath + 100];
    if (!lib::format_ref(l.ref, r, sizeof r)) return 0;
    n = sniprintf(out, cap, " m %s\n", r);
  } else {
    char row[5];
    row_text(l.code, row);
    if (l.type == kInput || l.type == kBuiltin) {
      const char* nm = l.type == kInput ? input_name(l.id) : builtin_name(list, l.id);
      n = sniprintf(out, cap, "%s %s %s\n", row, kTypeWords[l.type], nm);
    } else if (l.type == kSample || l.type == kInstrument) {
      char r[lib::kPath + 100];
      if (!lib::format_ref(l.ref, r, sizeof r)) return 0;
      char nm[kName];   // a name without TABs / control characters
      int k = 0;
      for (const char* q = l.name; *q && k < kName - 1; ++q) nm[k++] = (uint8_t)*q < 0x20 ? ' ' : *q;
      nm[k] = 0;
      if (l.frames && l.rate && l.type == kSample)
        n = sniprintf(out, cap, "%s %s %s\t%s\t%lu %u %lu\n", row, kTypeWords[l.type], r, nm, (unsigned long)l.frames,
                      (unsigned)l.ch, (unsigned long)l.rate);
      else n = sniprintf(out, cap, "%s %s %s\t%s\n", row, kTypeWords[l.type], r, nm);
    } else {
      return 0;
    }
  }
  return n > 0 && n < cap ? n : 0;
}

bool parse_used(const char* s, uint32_t n, UsedFn fn, void* ctx, char* why, int cap) {
  char line[64];
  for (uint32_t a = 0; a < n;) {
    uint32_t b = a;
    while (b < n && s[b] != '\n') {
      if (!s[b]) return say(why, cap, "a NUL byte (binary data)");
      ++b;
    }
    uint32_t e = b;
    if (e > a && s[e - 1] == '\r') --e;
    uint32_t len = e - a;
    const char* l0 = s + a;
    a = b + 1;
    if (!len) continue;
    if (len >= sizeof line) return say(why, cap, "a line too long");
    memcpy(line, l0, len);
    line[len] = 0;
    if (line[0] == '#') continue;
    const char* sp = strchr(line, ' ');
    if (!sp) return say(why, cap, "not a used line");
    char tag[8];
    int tl = (int)(sp - line);
    if (tl <= 0 || tl >= (int)sizeof tag) return say(why, cap, "not a list");
    memcpy(tag, line, tl);
    tag[tl] = 0;
    Used u;
    u.ram = 0;
    if (!strcmp(tag, "ram")) {   // "ram smp A001" / "ram inst *": always in RAM
      const char* q = sp + 1;
      const char* s2 = strchr(q, ' ');
      int ql = s2 ? (int)(s2 - q) : -1;
      if (ql <= 0 || ql >= (int)sizeof tag) return say(why, cap, "not a ram line");
      memcpy(tag, q, ql);
      tag[ql] = 0;
      u.list = list_of_tag(tag), u.count = 0;
      if (u.list < 0) return say(why, cap, "not a list");
      const char* p = s2 + 1;
      if (p[0] == '*' && !p[1]) u.ram = 2, u.code = -1;
      else if (parse_row(p, &u.code, &p) && !*p) u.ram = 1;
      else return say(why, cap, "not a row");
      if (fn && !fn(ctx, u)) return say(why, cap, "too many entries");
      continue;
    }
    u.list = list_of_tag(tag);
    if (u.list < 0) return say(why, cap, "not a list");
    const char* p = sp + 1;
    if (!parse_row(p, &u.code, &p) || *p++ != ' ') return say(why, cap, "not a row");
    if (!num(&p, &u.count, 0xFFFFFFFFu) || *p) return say(why, cap, "not a count");
    if (fn && !fn(ctx, u)) return say(why, cap, "too many entries");
  }
  return true;
}
int put_used(const Used& u, char* out, int cap) {
  char row[5];
  if (u.ram == 2) {
    int n = sniprintf(out, cap, "ram %s *\n", list_tag(u.list));
    return n > 0 && n < cap ? n : 0;
  }
  row_text(u.code, row);
  if (u.ram) {
    int n = sniprintf(out, cap, "ram %s %s\n", list_tag(u.list), row);
    return n > 0 && n < cap ? n : 0;
  }
  int n = sniprintf(out, cap, "%s %s %lu\n", list_tag(u.list), row, (unsigned long)u.count);
  return n > 0 && n < cap ? n : 0;
}

}  // namespace text
}  // namespace pools
}  // namespace core
