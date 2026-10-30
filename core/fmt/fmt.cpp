// core/fmt/fmt.cpp - see fmt.h: load(), the header conventions, the settings' kept lines, the paths. The table of
// formats: formats.cpp; the atomic writes: atomic.cpp.
#include "fmt.h"

#include <string.h>
#include <strings.h>

namespace core {
namespace fmt {

const char* class_name(int c) {
  static const char* const n[] = {"project", "setting", "derived", "exchange"};
  return c >= 0 && c <= kExchange ? n[c] : "?";
}
const char* shape_name(int s) {
  static const char* const n[] = {"text", "key=value", "json", "binary"};
  return s >= 0 && s <= kBinary ? n[s] : "?";
}
const char* status_name(int s) {
  static const char* const n[] = {"ok", "upgraded", "foreign-newer", "newer", "unreadable"};
  return s >= 0 && s <= kUnreadable ? n[s] : "?";
}

const Format* find(const char* name) {
  if (!name) return nullptr;
  for (int i = 0; i < count(); ++i)
    if (!strcmp(at(i)->name, name)) return at(i);
  return nullptr;
}

namespace {
Status done(Info* info, Status st, uint16_t v, uint16_t m, const char* why) {
  if (info) {
    info->st = st;
    info->version = v, info->min_reader = m;
    info->locked = st == kNewer || st == kUnreadable || st == kForeignNewer;
    info->read_only = st == kForeignNewer;
    sniprintf(info->why, sizeof info->why, "%s", why ? why : "");
  }
  return st;
}
bool copy_out(Buf* out, const Bytes& b) {
  if (!out) return true;
  if (b.n > out->cap) return false;
  if (b.n) memmove(out->p, b.p, b.n);
  out->n = b.n;
  return true;
}
}  // namespace

Status load(const Format& f, const Bytes& in, Buf* out, Info* info, Ctx* c) {
  if (info) memset(info, 0, sizeof *info);
  if (out) out->n = 0;
  if (f.max_bytes && in.n > f.max_bytes) return done(info, kUnreadable, 0, 0, "too big");
  Sniffed s = {0, 0, 0};
  if (!f.sniff || !f.sniff(in, &s)) return done(info, kUnreadable, 0, 0, "not this format");
  if (!s.min_reader) s.min_reader = s.version;
  if (s.version > f.current) {
    if (s.min_reader > f.current) return done(info, kNewer, s.version, s.min_reader, "a newer version");
    char why[120] = "";
    if (f.read && !f.read(in, s, why, sizeof why)) return done(info, kNewer, s.version, s.min_reader, why);
    if (!copy_out(out, in)) return done(info, kUnreadable, s.version, s.min_reader, "too big");
    return done(info, kForeignNewer, s.version, s.min_reader, "a newer version this build may read (not written back)");
  }
  if (s.version < f.oldest || s.version == 0) return done(info, kUnreadable, s.version, s.min_reader, "an older version this build has no upgrade for");
  Status st = kOk;
  Bytes cur = in;
  if (s.version < f.native) {   // the chain, in memory: out holds each step's result (a step reads the previous)
    if (!out || !out->p) return done(info, kUnreadable, s.version, s.min_reader, "an upgrade needs a buffer");
    for (uint16_t v = s.version; v < f.native; ++v) {
      const Upgrade* u = nullptr;
      for (int k = 0; k < f.nchain; ++k)
        if (f.chain[k].from == v) u = &f.chain[k];
      if (!u) return done(info, kUnreadable, s.version, s.min_reader, "no upgrade from this version");
      Buf step = {out->p, 0, out->cap};
      Ctx none = {"", nullptr};
      if (cur.p == out->p) {   // the step reads its input from out: copied out of the way first (the input's end)
        if (cur.n * 2 > out->cap) return done(info, kUnreadable, s.version, s.min_reader, "too big to upgrade");
        memmove(out->p + out->cap - cur.n, cur.p, cur.n);
        cur.p = out->p + out->cap - cur.n;
        step.cap = out->cap - cur.n;
      }
      if (!u->up(cur, &step, c ? c : &none)) return done(info, kUnreadable, s.version, s.min_reader, "the upgrade refused it");
      cur.p = step.p, cur.n = step.n;
    }
    out->n = cur.n;
    Sniffed u;
    if (!f.sniff(cur, &u) || u.version != f.native) return done(info, kUnreadable, s.version, s.min_reader, "the upgrade's output");
    char why[120] = "";
    if (f.read && !f.read(cur, u, why, sizeof why)) return done(info, kUnreadable, s.version, s.min_reader, why[0] ? why : "damaged");
    return done(info, kUpgraded, s.version, s.min_reader, "");   // (info: the file's own version)
  }
  char why[120] = "";
  if (f.read && !f.read(cur, s, why, sizeof why)) return done(info, kUnreadable, s.version, s.min_reader, why[0] ? why : "damaged");
  if (!copy_out(out, in)) return done(info, kUnreadable, s.version, s.min_reader, "too big");
  return done(info, st, s.version, s.min_reader, "");
}

int refusal(const Format& f, const Info& i, const char* file, char* out, int cap) {
  if (i.st == kNewer)
    return sniprintf(out, cap, "%s: made by a newer Sidecar (format %s v%u); this Sidecar reads up to v%u. Not loaded, not changed.",
                     file, f.name, (unsigned)i.version, (unsigned)f.current);
  if (i.st == kForeignNewer)
    return sniprintf(out, cap, "%s: made by a newer Sidecar (format %s v%u): read, never written back here.", file, f.name,
                     (unsigned)i.version);
  if (i.st == kUnreadable)
    return sniprintf(out, cap, "%s: not a %s file this Sidecar can read (%s). Not loaded, not changed.", file, f.name,
                     i.why[0] ? i.why : "damaged");
  return out && cap ? (out[0] = 0, 0) : 0;
}

// ---- headers
namespace {
uint32_t line_end(const Bytes& in, uint32_t from) {
  uint32_t e = from;
  while (e < in.n && in.p[e] != '\n') ++e;
  return e;
}
bool num(const uint8_t*& p, const uint8_t* e, uint32_t* v) {
  if (p >= e || *p < '0' || *p > '9') return false;
  uint32_t x = 0;
  while (p < e && *p >= '0' && *p <= '9') {
    x = x * 10 + (uint32_t)(*p++ - '0');
    if (x > 65535) return false;
  }
  *v = x;
  return true;
}
}  // namespace

bool text_header(const Bytes& in, const char* name, Sniffed* out) {
  static const char kTag[] = "#sidecar ";
  size_t tl = sizeof kTag - 1, nl = strlen(name);
  uint32_t e = line_end(in, 0);
  if (e < tl + nl + 2 || memcmp(in.p, kTag, tl) || memcmp(in.p + tl, name, nl) || in.p[tl + nl] != ' ') return false;
  const uint8_t* p = in.p + tl + nl + 1;
  const uint8_t* end = in.p + e;
  if (end > p && end[-1] == '\r') --end;
  uint32_t v, m = 0;
  if (!num(p, end, &v)) return false;
  if (p < end) {
    if (end - p < 6 || memcmp(p, " min=", 5)) return false;
    p += 5;
    if (!num(p, end, &m) || p != end) return false;
  }
  out->version = (uint16_t)v;
  out->min_reader = (uint16_t)(m ? m : v);
  out->body = e < in.n ? e + 1 : e;
  return true;
}
int put_text_header(char* out, int cap, const char* name, uint16_t version, uint16_t min_reader) {
  if (min_reader && min_reader != version)
    return sniprintf(out, cap, "#sidecar %s %u min=%u\n", name, (unsigned)version, (unsigned)min_reader);
  return sniprintf(out, cap, "#sidecar %s %u\n", name, (unsigned)version);
}

bool kv_header(const Bytes& in, const char* name, Sniffed* out) {
  static const char kTag[] = "format=";
  size_t tl = sizeof kTag - 1, nl = strlen(name);
  uint32_t e = line_end(in, 0);
  if (e < tl + nl + 2 || memcmp(in.p, kTag, tl) || memcmp(in.p + tl, name, nl) || in.p[tl + nl] != '/') return false;
  const uint8_t* p = in.p + tl + nl + 1;
  const uint8_t* end = in.p + e;
  if (end > p && end[-1] == '\r') --end;
  uint32_t v;
  if (!num(p, end, &v) || p != end) return false;
  out->version = out->min_reader = (uint16_t)v;
  out->body = e < in.n ? e + 1 : e;
  return true;
}
int put_kv_header(char* out, int cap, const char* name, uint16_t version) {
  return sniprintf(out, cap, "format=%s/%u\n", name, (unsigned)version);
}

// The top object's first keys (up to 16, before any nested value): "format", "version", "min_reader". A small scanner:
// strings with escapes, numbers; anything nested ends the scan (the header keys come first by convention).
bool json_header(const Bytes& in, const char* name, Sniffed* out) {
  const uint8_t *p = in.p, *e = in.p + in.n;
  auto ws = [&]() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; };
  auto str = [&](const uint8_t** a, int* l) -> bool {
    if (p >= e || *p != '"') return false;
    *a = ++p;
    while (p < e && *p != '"') {
      if (*p == '\\') ++p;
      ++p;
    }
    if (p >= e) return false;
    *l = (int)(p - *a);
    ++p;
    return true;
  };
  ws();
  if (p >= e || *p != '{') return false;
  ++p;
  bool fmt_ok = false, have_v = false;
  uint32_t v = 0, m = 0;
  char want[48];
  int wl = sniprintf(want, sizeof want, "sidecar-%s", name);
  for (int k = 0; k < 16; ++k) {
    ws();
    const uint8_t* key;
    int kl;
    if (!str(&key, &kl)) break;
    ws();
    if (p >= e || *p != ':') return false;
    ++p;
    ws();
    if (p < e && *p == '"') {
      const uint8_t* a;
      int al;
      if (!str(&a, &al)) return false;
      if (kl == 6 && !memcmp(key, "format", 6)) {
        if (al != wl || memcmp(a, want, (size_t)wl)) return false;
        fmt_ok = true;
      }
    } else if (p < e && *p >= '0' && *p <= '9') {
      uint32_t x;
      if (!num(p, e, &x)) return false;
      if (kl == 7 && !memcmp(key, "version", 7)) v = x, have_v = true;
      else if (kl == 10 && !memcmp(key, "min_reader", 10)) m = x;
    } else {
      break;   // a nested value, true / false / null: the header keys are over
    }
    ws();
    if (p < e && *p == ',') ++p;
    else break;
  }
  if (!fmt_ok || !have_v) return false;
  out->version = (uint16_t)v;
  out->min_reader = (uint16_t)(m ? m : v);
  out->body = 0;
  return true;
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
bool bin_header(const Bytes& in, const char magic[4], Sniffed* out, uint32_t* body_crc) {
  if (in.n < kBinHeader || memcmp(in.p, magic, 4)) return false;
  uint32_t hb = rd32(in.p + 8);
  if (hb < kBinHeader || hb > in.n) return false;
  out->version = (uint16_t)(in.p[4] | in.p[5] << 8);
  out->min_reader = (uint16_t)(in.p[6] | in.p[7] << 8);
  out->body = hb;
  if (body_crc) *body_crc = rd32(in.p + 12);
  return out->version != 0;
}
void put_bin_header(uint8_t* out, const char magic[4], uint16_t version, uint16_t min_reader, uint32_t header_bytes,
                    uint32_t body_crc) {
  memcpy(out, magic, 4);
  out[4] = (uint8_t)version, out[5] = (uint8_t)(version >> 8);
  out[6] = (uint8_t)min_reader, out[7] = (uint8_t)(min_reader >> 8);
  wr32(out + 8, header_bytes);
  wr32(out + 12, body_crc);
}

// ---- class S
int kv_keep(const char* text, uint32_t len, const char* const* known, KvKeep* out) {
  out->n = 0;
  out->text[0] = 0;
  int kept = 0;
  for (uint32_t a = 0; a < len;) {
    uint32_t b = a;
    while (b < len && text[b] != '\n') ++b;
    uint32_t e = b;
    if (e > a && text[e - 1] == '\r') --e;
    const char* l = text + a;
    uint32_t ll = e - a;
    const char* eq = (const char*)memchr(l, '=', ll);
    bool skip = !ll || l[0] == '#' || !eq || (eq - l == 6 && !memcmp(l, "format", 6));
    for (uint32_t i = 0; !skip && i < ll; ++i) skip = l[i] == 0;
    for (const char* const* k = known; !skip && k && *k; ++k) {
      size_t kl = strlen(*k);
      if ((size_t)(eq - l) == kl && !memcmp(l, *k, kl)) skip = true;
    }
    if (!skip && out->n + ll + 1 < sizeof out->text) {
      memcpy(out->text + out->n, l, ll);
      out->n += ll;
      out->text[out->n++] = '\n';
      out->text[out->n] = 0;
      ++kept;
    }
    a = b + 1;
  }
  return kept;
}
uint32_t kv_append(const KvKeep& k, char* out, uint32_t n, uint32_t cap) {
  if (n >= cap) return n;
  uint32_t m = k.n < cap - n ? k.n : 0;   // whole or nothing: never half a line
  memcpy(out + n, k.text, m);
  return n + m;
}

// ---- paths (generation 1: today's places, as each module names them)
namespace {
struct Place {
  const char* format;
  const char* tmpl;   // %s the key; %K a kit / pattern (see path()); none: global
  uint8_t how;        // 0 global, 1 key, 2 key + kit (lfo / ins), 3 key + pattern, 4 key + machine
};
const Place kPlaces[] = {
    {"pool", "/.rytm/pools/%s.txt", 1},
    {"bankpool", "", 4},
    {"picks", "/.rytm/granular/%s.txt", 1},
    {"slots", "/.rytm/slots/%s.txt", 1},
    {"slotnotes", "/.rytm/slots/%s.inf", 1},
    {"fxslots", "/.rytm/fxslots/%s.txt", 1},
    {"juno", "/.rytm/juno/%s.txt", 1},
    {"fm", "/.rytm/fm/%s.txt", 1},
    {"lfo", "lfo", 2},
    {"ins", "ins", 2},
    {"kits", "kits", 2},   // core/bundle/kits.h: its folder (128); the records kNNN.sks / .prv, not path()'s .txt
    {"sqp", "/.rytm/seq/%s", 3},
    {"tracks", "/.rytm/seq/%s/tracks.txt", 1},
    {"song", "/.rytm/songs/%s", 1},
    {"display", "/.rytm/display.txt", 0},
    {"keys", "/.rytm/keys.txt", 0},
    {"fxin", "/.rytm/fxin.txt", 0},
    {"sendfx", "/.rytm/sendfx.txt", 0},
    {"wifi", "/.rytm/wifi.txt", 0},
    {"project", "/.rytm/project", 0},
    {"projects", "/.rytm/projects.txt", 0},
    {"sdtest", "/.rytm/sdtest.txt", 0},
    {"cardindex", "/.rytm/env", 0},
    {"dx7idx", "/.rytm/dx7.bin", 0},
    {"rmpa", "/Plugins", 0},
    {"bundle", "/Bundles", 0},
    {"library", "/.rytm/library/index.bin", 0},
    {"library-log", "/.rytm/library/log.txt", 0},
    {"library-state", "/.rytm/library/state.txt", 0},
    // generation 2 (the pools design 3.3): a project's own folder
    {"samplepool", "/.rytm/projects/%s/samples.pool.txt", 1},
    {"wtpool", "/.rytm/projects/%s/wavetables.pool.txt", 1},
    {"instpool", "/.rytm/projects/%s/instruments.pool.txt", 1},
    {"poolused", "/.rytm/projects/%s/pool-used.txt", 1},
    {"slicemap", "/CHOP/%s", 1},   // CHOP's slice maps (core/chop/slicemap.h): the project's folder of them
};
}  // namespace

const char* path(const char* format, const char* key, int kit, char* out, int cap) {
  if (!format || !out || cap <= 0) return nullptr;
  for (const Place& p : kPlaces) {
    if (strcmp(p.format, format)) continue;
    if (p.how && (!key || !key[0] || strchr(key, '/'))) return nullptr;
    int n;
    switch (p.how) {
      case 0: n = sniprintf(out, cap, "%s", p.tmpl); break;
      case 1: n = sniprintf(out, cap, p.tmpl, key); break;
      case 2:   // as lfo.cpp / insert_card.cpp file_name: -1 the project's file, 0..127 a kit's (k001..k128), 128 its folder
        if (kit < 0) n = sniprintf(out, cap, "/.rytm/%s/%s.txt", p.tmpl, key);
        else if (kit < 128) n = sniprintf(out, cap, "/.rytm/%s/%s/k%03d.txt", p.tmpl, key, kit + 1);
        else n = sniprintf(out, cap, "/.rytm/%s/%s", p.tmpl, key);
        break;
      case 3:   // seq.cpp file_of: pNNN.sqp (A01 = 000); a pattern outside 0..127: the folder
        if (kit >= 0 && kit < 128) n = sniprintf(out, cap, "/.rytm/seq/%s/p%03d.sqp", key, kit);
        else n = sniprintf(out, cap, p.tmpl, key);
        break;
      default: {   // srclib.cpp pool_path: /.rytm/<machine>/<key>.pool.txt
        static const char* const kDirs[] = {"granular", "wavetable", "multisample"};
        if (kit < 0 || kit > 2) return nullptr;
        n = sniprintf(out, cap, "/.rytm/%s/%s.pool.txt", kDirs[kit], key);
        break;
      }
    }
    return n > 0 && n < cap ? out : nullptr;
  }
  return nullptr;
}

CORE_COLD int LineReader::line(char* out, int max) {
  int k = 0;
  bool any = false;
  for (;;) {
    if (i == n) {
      if (end) break;
      int r = f.read(buf, sizeof buf);
      if (r <= 0) {
        end = true;
        break;
      }
      n = r, i = 0;
    }
    char c = buf[i++];
    any = true;
    if (c == '\n') break;
    if (c != '\r' && k < max - 1) out[k++] = c;
  }
  out[k] = 0;
  return any ? k : -1;
}

CORE_COLD int next_line(const uint8_t* d, uint32_t n, uint32_t* at, char* out, int max) {
  int k = 0;
  uint32_t i = *at;
  bool any = i < n;
  while (i < n) {
    char c = (char)d[i++];
    if (c == '\n') break;
    if (c != '\r' && k < max - 1) out[k++] = c;
  }
  *at = i;
  out[k] = 0;
  return any ? k : -1;
}

}  // namespace fmt
}  // namespace core
