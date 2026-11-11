// pi/engine/update.cpp - see update.h (docs/web-update.md). Hosted (the core's flags): the web task and the update
// task on core 0 call it, one at a time (g_busy).
#include "update.h"

#include <string.h>

#ifdef FSWEB_HOST   // the Mac tests (pi/engine/tests/test_update.cpp)
#include <stdio.h>
#define sniprintf snprintf
#else
#include "core_port.h"   // sniprintf: platform_pi.cpp's integer printf
#endif
#include "lib/sha256.h"
#include "version.h"
#include "webnext_path.h"

namespace upd {
namespace {

#if SIDECAR_DIRTY
#define UPD_DIRTY "-dirty"
#else
#define UPD_DIRTY ""
#endif
// in every kernel with this code (the update file's kernels must carry their header's; "previous" looks for the prefix)
const char kStamp[] = "sidecar-webupdate-1 v=" SIDECAR_VERSION " c=" SIDECAR_COMMIT UPD_DIRTY;
const char kCap[] = "sidecar-webupdate-1 v=";
const char kEngine[] = "==== rytm-pi engine";
// The Pi 5 kernel's own boot lines (sdboot.py board()'s rule), kept here XOR 0x5a and decoded at run time: written
// out, they would be in every kernel with this code, the Pi 4's too, and sdboot.py (and this check) would take a
// Pi 4 kernel for a Pi 5 one. test_update.cpp checks that its own binary does not contain them.
uint8_t g_pi5a[] = {0x38, 0x35, 0x3b, 0x28, 0x3e, 0x60, 0x7a, 0x08, 0x3b, 0x29, 0x2a, 0x38, 0x3f, 0x28, 0x28, 0x23, 0x7a, 0x0a, 0x33, 0x7a, 0x6f, 0x7a, 0x38, 0x2f, 0x33, 0x36, 0x3e, 0x7a, 0x72, 0x31, 0x3f, 0x28, 0x34, 0x3f, 0x36, 0x05, 0x68, 0x6d, 0x6b, 0x68, 0x74, 0x33, 0x37, 0x3d, 0x73, 0};
uint8_t g_pi5b[] = {0x3d, 0x3b, 0x3e, 0x3d, 0x3f, 0x2e, 0x60, 0x7a, 0x34, 0x35, 0x34, 0x3f, 0x7a, 0x35, 0x34, 0x7a, 0x2e, 0x32, 0x33, 0x29, 0x7a, 0x38, 0x35, 0x3b, 0x28, 0x3e, 0x7a, 0x72, 0x0a, 0x33, 0x7a, 0x6f, 0x7a, 0x38, 0x2f, 0x33, 0x36, 0x3e, 0x73, 0};
const char* unx(uint8_t* p) {   // once each (check_kernel's statics)
  for (uint8_t* q = p; *q; ++q) *q ^= 0x5a;
  return (const char*)p;
}
const char kOurs[] = "# sidecar web update";   // tryboot.txt's first line: ours to delete

const char kDir[] = "/.rytm/update";
const char kFile[] = "/.rytm/update/update.scu";
const char kUpload[] = "/.rytm/update/upload.part";
const char kState[] = "/.rytm/update/state.txt";
const char kStatePart[] = "/.rytm/update/state.txt.part";
const char kPrevInfo[] = "/.rytm/update/prev.txt";
const char kWeb[] = "/.rytm/www-next";
const char kWebNew[] = "/.rytm/www-next.new";
const char kWebPrev[] = "/.rytm/www-next.prev";
const char kWebSwap[] = "/.rytm/www-next.swp";
const char kCmdTry[] = "/cmdline-try.txt";
const char kTryboot[] = "/tryboot.txt";
const char kWebMagic[] = "SCWEB001";

void say(fsweb::Result* r, int st, const char* m) {
  r->status = st;
  sniprintf(r->msg, sizeof r->msg, "%s", m);
}
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
bool zero(const uint8_t* p, int n) { for (int i = 0; i < n; ++i) if (p[i]) return false; return true; }

uint8_t g_buf[4096], g_back[4096];   // one job at a time (g_busy): the pieces and their read-back
bool g_busy;
struct Busy {
  bool ok;
  Busy() : ok(!g_busy) { g_busy = true; }
  ~Busy() { if (ok) g_busy = false; }
};

bool exists(const Env& e, const char* p, uint64_t* size = nullptr, bool* dir = nullptr) {
  uint64_t s;
  bool d;
  if (!e.b->stat(p, &s, &d)) return false;
  if (size) *size = s;
  if (dir) *dir = d;
  return true;
}
void rm(const Env& e, const char* p) { if (exists(e, p)) e.b->remove(p); }
void yield(const Env& e) { if (e.b->yield) e.b->yield(); }

// a small text file, whole (false: missing or bigger than cap - 1)
bool read_text(const Env& e, const char* p, char* out, int cap) {
  uint64_t n;
  bool d;
  if (!exists(e, p, &n, &d) || d || n >= (uint64_t)cap) return false;
  uint32_t got = 0;
  if (n && (!e.b->read(p, 0, out, (uint32_t)n, &got) || got != n)) return false;
  out[n] = 0;
  return true;
}
// <p>.part written, read back, then in place of <p>
bool write_text(const Env& e, const char* p, const char* text) {
  char part[fsweb::kPath + 8];
  sniprintf(part, sizeof part, "%s.part", p);
  uint32_t n = (uint32_t)strlen(text);
  if (n > sizeof g_back) return false;
  if (!e.b->write_at(part, 0, text, n, true)) { rm(e, part); return false; }
  uint32_t got = 0;
  if (!e.b->read(part, 0, g_back, n, &got) || got != n || memcmp(g_back, text, n)) { rm(e, part); return false; }
  if (exists(e, p) && !e.b->remove(p)) { rm(e, part); return false; }
  return e.b->rename(part, p);
}

// ---- the state (/.rytm/update/state.txt, key=value lines)
struct State {
  char state[16];   // idle staging trying committing swapping committed reverted failed
  char kind[12];    // update previous
  char tv[24], tc[24], fv[24], fc[24];   // the target's and the from's version and commit
  int web;          // the trial brings a web app (update: in the file; previous: www-next.prev)
  int ws;           // the web app's step: 0 not yet, 1 under way (unpacked / moved aside), 2 done
  char wsha[68];    // update: the web part's sha256 (hex) as install checked it; the commit unpacks only that
  char msg[160];
};
void field(const char* text, const char* key, char* out, int cap) {
  size_t k = strlen(key);
  out[0] = 0;
  for (const char* l = text; l && *l;) {
    if (strncmp(l, key, k) == 0 && l[k] == '=') {
      const char* v = l + k + 1;
      int n = 0;
      while (v[n] && v[n] != '\n' && n < cap - 1) { out[n] = v[n]; ++n; }
      out[n] = 0;
      return;
    }
    l = strchr(l, '\n');
    if (l) ++l;
  }
}
void load(const Env& e, State* s) {
  static char t[1024];
  memset(s, 0, sizeof *s);
  // state.txt, or (a cut between write_text's remove and rename) its .part: only ever removed once the .part is whole
  if (!read_text(e, kState, t, sizeof t) && !read_text(e, kStatePart, t, sizeof t)) { strcpy(s->state, "idle"); return; }
  char n[8];
  field(t, "state", s->state, sizeof s->state);
  field(t, "kind", s->kind, sizeof s->kind);
  field(t, "tv", s->tv, sizeof s->tv);
  field(t, "tc", s->tc, sizeof s->tc);
  field(t, "fv", s->fv, sizeof s->fv);
  field(t, "fc", s->fc, sizeof s->fc);
  field(t, "msg", s->msg, sizeof s->msg);
  field(t, "wsha", s->wsha, sizeof s->wsha);
  field(t, "web", n, sizeof n);
  s->web = n[0] == '1';
  field(t, "ws", n, sizeof n);
  s->ws = n[0] - '0' >= 0 && n[0] - '0' <= 2 ? n[0] - '0' : 0;
  if (!s->state[0]) strcpy(s->state, "idle");
}
bool save(const Env& e, const State& s) {
  static char t[1024];
  if (!exists(e, kDir) && !e.b->mkdir(kDir)) return false;
  sniprintf(t, sizeof t, "state=%s\nkind=%s\ntv=%s\ntc=%s\nfv=%s\nfc=%s\nweb=%d\nws=%d\nwsha=%s\nmsg=%s\n", s.state, s.kind,
            s.tv, s.tc, s.fv, s.fc, s.web, s.ws, s.wsha, s.msg);
  return write_text(e, kState, t);
}
void set(State* s, const char* state, const char* msg) {
  sniprintf(s->state, sizeof s->state, "%s", state);
  sniprintf(s->msg, sizeof s->msg, "%s", msg);
}
const char* is(const State& s, const char* st) { return strcmp(s.state, st) == 0 ? st : nullptr; }
// a job between install and the end of its commit: the uploaded file and the trial's files are not to be touched
bool in_flight(const Env& e, const State& s) { return e.trial || is(s, "staging") || is(s, "trying") || is(s, "committing"); }
void to_hex(const uint8_t* b, int n, char* out) {
  static const char d[] = "0123456789abcdef";
  for (int i = 0; i < n; ++i) out[2 * i] = d[b[i] >> 4], out[2 * i + 1] = d[b[i] & 15];
  out[2 * n] = 0;
}

// ---- searching a kernel for its strings, across the pieces' edges
struct Scan {
  static constexpr int kN = 5;
  const char* pat[kN];
  uint32_t len[kN];
  bool hit[kN];
  int n;
  uint8_t tail[96];
  uint32_t ntail;
  void init() { n = 0; ntail = 0; }
  int add(const char* p, uint32_t l) { pat[n] = p; len[n] = l; hit[n] = false; return n++; }
  static bool find(const uint8_t* d, uint32_t nd, const char* p, uint32_t l) {
    if (l == 0 || nd < l) return false;
    for (uint32_t i = 0; i + l <= nd; ++i)
      if (d[i] == (uint8_t)p[0] && memcmp(d + i, p, l) == 0) return true;
    return false;
  }
  void feed(const uint8_t* d, uint32_t nd) {
    static uint8_t w[96 + 4096];
    while (nd) {
      uint32_t k = nd < 4096 ? nd : 4096;
      memcpy(w, tail, ntail);
      memcpy(w + ntail, d, k);
      uint32_t nw = ntail + k;
      for (int i = 0; i < n; ++i) if (!hit[i]) hit[i] = find(w, nw, pat[i], len[i]);
      ntail = nw < 95 ? nw : 95;   // the longest pattern is shorter than 96
      memmove(tail, w + nw - ntail, ntail);
      d += k, nd -= k;
    }
  }
};

// sdboot's sums (sdboot.patch sdb_check, sdboot.py wrap): over the 32-bit words after the 32-byte header
struct Sums {
  uint32_t s1 = 0, s2 = 0, word = 0;
  int k = 0;
  void feed(const uint8_t* d, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
      word |= (uint32_t)d[i] << (8 * k);
      if (++k == 4) { s1 += word; s2 += s1; word = 0; k = 0; }
    }
  }
};

// A kernel at path[off, off + size): a Sidecar engine for the board, its sha256 (sha_out), the stamp if one is asked
// for, and whether it has this update code (*cap). nullptr: good.
const char* check_kernel(const Env& e, const char* path, uint64_t off, uint64_t size, int board, const char* want_stamp,
                         bool* cap, uint8_t sha_out[32]) {
  if (board == 4 ? size > kPi4Max || size < 32 + 1024 : size > kPi5Max || size < 1024) return board == 4 ? "the Pi 4 kernel's size is wrong" : "the Pi 5 kernel's size is wrong";
  uint32_t got = 0;
  uint32_t len = 0, w1 = 0, w2 = 0;
  if (board == 4) {
    uint8_t h[32];
    if (!e.b->read(path, off, h, 32, &got) || got != 32) return "read error";
    if (memcmp(h, "SDCRENG1", 8)) return "the Pi 4 kernel is not an engine.img (no SDCRENG1 header)";
    len = le32(h + 8), w1 = le32(h + 12), w2 = le32(h + 16);
    if (len != size - 32 || (len & 3) || !zero(h + 20, 12)) return "the Pi 4 kernel's engine.img header does not match its size";
  }
  static char stamp_pat[80];
  Scan sc;
  sc.init();
  static const char* pi5a = nullptr;
  static const char* pi5b = nullptr;
  if (!pi5a) pi5a = unx(g_pi5a), pi5b = unx(g_pi5b);
  int iEng = sc.add(kEngine, sizeof kEngine - 1), i5a = sc.add(pi5a, (uint32_t)strlen(pi5a)), i5b = sc.add(pi5b, (uint32_t)strlen(pi5b));
  int iCap = sc.add(kCap, sizeof kCap - 1), iStamp = -1;
  if (want_stamp) {
    sniprintf(stamp_pat, sizeof stamp_pat, "%s", want_stamp);
    iStamp = sc.add(stamp_pat, (uint32_t)strlen(stamp_pat) + 1);   // with its NUL: "c=abc1234" is not "c=abc1234-dirty"
  }
  Sums sums;
  core::lib::Sha256 sh;
  sh.init();
  for (uint64_t p = 0; p < size;) {
    uint32_t k = size - p < sizeof g_buf ? (uint32_t)(size - p) : (uint32_t)sizeof g_buf;
    if (!e.b->read(path, off + p, g_buf, k, &got) || got != k) return "read error while checking";
    sh.update(g_buf, k);
    uint32_t skip = board == 4 && p < 32 ? (uint32_t)(32 - p) : 0;   // the engine.img header: not the kernel's
    if (skip < k) {
      sc.feed(g_buf + skip, k - skip);
      if (board == 4) sums.feed(g_buf + skip, k - skip);
    }
    p += k;
    yield(e);
  }
  sh.final(sha_out);
  if (board == 4 && (sums.s1 != w1 || sums.s2 != w2)) return "the Pi 4 kernel's sums are wrong (sdboot would not start it)";
  if (!sc.hit[iEng]) return "not a Sidecar engine kernel";
  bool pi5 = sc.hit[i5a] || sc.hit[i5b];
  if (pi5 != (board == 5)) return board == 5 ? "the Pi 5 part holds a Pi 4 kernel" : "the Pi 4 part holds a Pi 5 kernel";
  if (iStamp >= 0 && !sc.hit[iStamp]) return "the kernel is not the version its header says";
  if (cap) *cap = sc.hit[iCap];
  return nullptr;
}

// the web app's archive: its sha256, then every entry (name as webnext_path.h serves it, sizes inside the part)
const char* check_web(const Env& e, const char* path, uint64_t off, uint64_t size, uint8_t sha_out[32]) {
  if (size > kWebMax || size < 12) return "the web app's size is wrong";
  core::lib::Sha256 sh;
  sh.init();
  uint32_t got = 0;
  for (uint64_t p = 0; p < size;) {
    uint32_t k = size - p < sizeof g_buf ? (uint32_t)(size - p) : (uint32_t)sizeof g_buf;
    if (!e.b->read(path, off + p, g_buf, k, &got) || got != k) return "read error while checking";
    sh.update(g_buf, k);
    p += k;
    yield(e);
  }
  sh.final(sha_out);
  uint8_t h[12];
  if (!e.b->read(path, off, h, 12, &got) || got != 12 || memcmp(h, kWebMagic, 8)) return "the web app part is not an SCWEB001 archive";
  uint32_t count = le32(h + 8);
  if (count == 0 || count > kWebFiles) return "the web app has too many (or no) files";
  uint64_t p = 12;
  bool index = false;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t eh[8];
    if (p + 8 > size || !e.b->read(path, off + p, eh, 8, &got) || got != 8) return "the web app archive is cut short";
    uint32_t nl = eh[0] | eh[1] << 8, fs = le32(eh + 4);
    if (nl == 0 || nl > 120 || eh[2] || eh[3]) return "a web app file has a bad name";
    char nm[128], url[136], sd[200];
    if (p + 8 + nl > size || !e.b->read(path, off + p + 8, nm, nl, &got) || got != nl) return "the web app archive is cut short";
    nm[nl] = 0;
    if (strlen(nm) != nl) return "a web app file has a bad name";
    sniprintf(url, sizeof url, "/next/%s", nm);
    if (!webnext::path(url, sd, sizeof sd)) return "a web app file has a name the Sidecar would not serve";
    if (strcmp(nm, "index.html") == 0 || strcmp(nm, "index.html.gz") == 0) index = true;
    p += 8 + nl + (uint64_t)fs;
    if (p > size) return "the web app archive is cut short";
  }
  if (p != size) return "the web app archive has bytes after its last file";
  if (!index) return "the web app has no index.html";
  return nullptr;
}

// path[off, off + size) -> dst (as dst.part, every piece read back, the whole sha256 compared, then in place)
const char* copy_to(const Env& e, const char* src, uint64_t off, uint64_t size, const char* dst, const uint8_t want[32]) {
  char part[fsweb::kPath + 8];
  sniprintf(part, sizeof part, "%s.part", dst);
  core::lib::Sha256 sh;
  sh.init();
  uint32_t got = 0;
  for (uint64_t p = 0; p < size || p == 0;) {
    uint32_t k = size - p < sizeof g_buf ? (uint32_t)(size - p) : (uint32_t)sizeof g_buf;
    if (k && (!e.b->read(src, off + p, g_buf, k, &got) || got != k)) { rm(e, part); return "read error while copying"; }
    if (!e.b->write_at(part, p, g_buf, k, p == 0)) { rm(e, part); return "write failed (the card full or removed?)"; }
    if (k && (!e.b->read(part, p, g_back, k, &got) || got != k || memcmp(g_back, g_buf, k))) { rm(e, part); return "read-back differs: the card did not keep the bytes"; }
    sh.update(g_buf, k);
    p += k;
    yield(e);
    if (size == 0) break;
  }
  uint8_t h[32];
  sh.final(h);
  if (want && memcmp(h, want, 32)) { rm(e, part); return "the copy's sha256 is wrong"; }
  if (exists(e, dst) && !e.b->remove(dst)) { rm(e, part); return "could not replace the old file"; }
  if (!e.b->rename(part, dst)) { rm(e, part); return "could not put the file in place"; }
  return nullptr;
}

// a folder and all in it (the web app's: a few levels)
bool rmtree(const Env& e, const char* p, int depth = 0) {
  static fsweb::Entry ent[32];
  bool d;
  uint64_t s;
  if (!exists(e, p, &s, &d)) return true;
  if (!d) return e.b->remove(p);
  if (depth > 8) return false;
  for (int guard = 0; guard < 100000; ++guard) {
    bool more;
    int n = e.b->list(p, ent, 32, &more);
    if (n <= 0) break;
    char sub[fsweb::kPath + 1];
    sub[0] = 0;
    for (int i = 0; i < n; ++i) {   // the files of this batch, then one folder (its own listing reuses ent[])
      char c[fsweb::kPath + 1];
      if (sniprintf(c, sizeof c, "%s/%s", p, ent[i].name) >= (int)sizeof c) return false;
      if (ent[i].dir) { if (!sub[0]) strcpy(sub, c); }
      else if (!e.b->remove(c)) return false;
    }
    if (sub[0] && !rmtree(e, sub, depth + 1)) return false;
    yield(e);
  }
  return e.b->remove(p);
}

// the web app from the file's part to /.rytm/www-next.new
const char* unpack_web(const Env& e, const Part& w) {
  if (!rmtree(e, kWebNew)) return "could not clear www-next.new";
  if (!e.b->mkdir(kWebNew)) return "could not make www-next.new";
  uint8_t h[12];
  uint32_t got = 0;
  if (!e.b->read(kFile, w.off, h, 12, &got) || got != 12) return "read error";
  uint32_t count = le32(h + 8);
  if (memcmp(h, kWebMagic, 8) || count == 0 || count > kWebFiles) return "not a web app archive";
  uint64_t p = 12;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t eh[8];
    char nm[128], dst[fsweb::kPath + 1], url[136], sd[200];
    if (p + 8 > w.size || !e.b->read(kFile, w.off + p, eh, 8, &got) || got != 8) return "the web app archive is cut short";
    uint32_t nl = eh[0] | eh[1] << 8, fs = le32(eh + 4);
    if (nl == 0 || nl > 120 || p + 8 + nl + (uint64_t)fs > w.size) return "the web app archive is malformed";
    if (!e.b->read(kFile, w.off + p + 8, nm, nl, &got) || got != nl) return "read error";
    nm[nl] = 0;
    sniprintf(url, sizeof url, "/next/%s", nm);
    if (strlen(nm) != nl || !webnext::path(url, sd, sizeof sd)) return "a web app file has a name the Sidecar would not serve";
    sniprintf(dst, sizeof dst, "%s/%s", kWebNew, nm);
    for (char* s = dst + sizeof kWebNew; *s; ++s)   // its folders
      if (*s == '/') {
        *s = 0;
        if (!exists(e, dst) && !e.b->mkdir(dst)) return "could not make a web app folder";
        *s = '/';
      }
    const char* why = copy_to(e, kFile, w.off + p + 8 + nl, fs, dst, nullptr);
    if (why) return why;
    p += 8 + nl + (uint64_t)fs;
  }
  return nullptr;
}

void clean_trial(const Env& e) {
  rm(e, trial_name(e.board));
  char t[fsweb::kPath + 8];
  sniprintf(t, sizeof t, "%s.part", trial_name(e.board));
  rm(e, t);
  rm(e, kCmdTry);
  static char tb[64];
  uint32_t got = 0;
  if (exists(e, kTryboot) && e.b->read(kTryboot, 0, tb, sizeof kOurs - 1, &got) && got == sizeof kOurs - 1 &&
      memcmp(tb, kOurs, sizeof kOurs - 1) == 0)
    e.b->remove(kTryboot);   // a tryboot.txt of sdboot.py's (a developer's pi5-try) stays
}

// the commit's web step and the end, resumable (boot() calls it again after a cut)
bool finish(const Env& e, State* s) {
  clean_trial(e);
  if (s->web && s->ws < 2 && strcmp(s->kind, "update") == 0) {
    if (s->ws == 0) {   // unpack (again): nothing in place has changed yet
      uint8_t h[kHeader];
      uint32_t got = 0;
      uint64_t fsz = 0;
      Header hd;
      const Part* w = nullptr;
      if (exists(e, kFile, &fsz) && e.b->read(kFile, 0, h, kHeader, &got) && got == kHeader && !parse_header(h, fsz, &hd))
        for (int i = 0; i < hd.n; ++i) if (hd.part[i].kind == kPartWeb) w = &hd.part[i];
      const char* why = w ? nullptr : "the update file is gone";
      if (!why) {   // the file could have changed since install: only the web app install checked is unpacked
        uint8_t sha[32];
        char hx[68];
        why = check_web(e, kFile, w->off, w->size, sha);
        to_hex(sha, 32, hx);
        if (!why && strcmp(hx, s->wsha)) why = "the update file changed after the install";
      }
      if (!why) why = unpack_web(e, *w);
      if (why) {   // the kernel is in place: the old web app stays (the page says when it does not match)
        rmtree(e, kWebNew);
        sniprintf(s->msg, sizeof s->msg, "updated, but the web app was not: %s", why);
        s->ws = 2;
        s->web = 0;
      } else {
        s->ws = 1;
        if (!save(e, *s)) return false;
      }
    }
    if (s->ws == 1) {   // the new app in place, the old one as www-next.prev
      if (exists(e, kWebNew)) {
        if (exists(e, kWeb)) {
          if (!rmtree(e, kWebPrev) || !e.b->rename(kWeb, kWebPrev)) return false;
        }
        if (!e.b->rename(kWebNew, kWeb)) return false;
      }
      s->ws = 2;
    }
  } else if (s->web && s->ws < 2) {   // previous: www-next and www-next.prev swap
    if (s->ws == 0) {   // the current app aside (unless a cut came right after that: www-next.swp, no www-next)
      if (exists(e, kWebPrev) && !(exists(e, kWebSwap) && !exists(e, kWeb))) {
        if (!rmtree(e, kWebSwap)) return false;
        if (exists(e, kWeb) && !e.b->rename(kWeb, kWebSwap)) return false;
      }
      s->ws = 1;
      if (!save(e, *s)) return false;
    }
    if (s->ws == 1) {
      if (exists(e, kWebPrev) && !exists(e, kWeb) && !e.b->rename(kWebPrev, kWeb)) return false;
      if (exists(e, kWebSwap) && !exists(e, kWebPrev) && !e.b->rename(kWebSwap, kWebPrev)) return false;
      s->ws = 2;
    }
  }
  static char t[96];
  sniprintf(t, sizeof t, "v=%s\nc=%s\nw=%d\n", s->fv, s->fc, s->web ? 1 : 0);
  write_text(e, kPrevInfo, t);
  rm(e, kFile);
  char m[160];
  sniprintf(m, sizeof m, "%s%s%s", strcmp(s->kind, "previous") == 0 ? "back to the previous version" : "updated",
            s->msg[0] ? ": " : "", s->msg);
  set(s, "committed", m);
  return save(e, *s);
}

const char* stage_trial(const Env& e, const char* src, uint64_t off, uint64_t size, const uint8_t sha[32], const char* stamp_want) {
  static char cfg[16384], cmd[2048], tb[17000], ct[2100];
  if (!read_text(e, "/config.txt", cfg, sizeof cfg)) return "no config.txt on the card (or it is too long)";
  if (!read_text(e, "/cmdline.txt", cmd, sizeof cmd)) cmd[0] = 0;
  const char* why = tryboot_txt(e.board, cfg, tb, sizeof tb);
  if (why) return why;
  if (!cmdline_try(cmd, ct, sizeof ct)) return "cmdline.txt is too long";
  why = copy_to(e, src, off, size, trial_name(e.board), sha);
  if (why) return why;
  uint8_t h[32];
  bool cap = false;
  why = check_kernel(e, trial_name(e.board), 0, size, e.board, stamp_want, &cap, h);   // what the firmware will read
  if (why) return why;
  if (memcmp(h, sha, 32)) return "the trial kernel on the card is not the one checked";
  if (!write_text(e, kCmdTry, ct)) return "could not write cmdline-try.txt";
  if (!write_text(e, kTryboot, tb)) return "could not write tryboot.txt";
  return nullptr;
}

// the stamp in a kernel file: v and c after "sidecar-webupdate-1 v=" up to its NUL (false: none). The prefix alone
// (kCap, followed by its NUL) is in every such kernel too: a match that does not read as "<v> c=<c>" is passed over.
bool stamp_in(const Env& e, const char* path, uint64_t size, char v[24], char c[24]) {
  static uint8_t w[96 + 4096];
  const uint32_t L = sizeof kCap - 1;
  uint32_t got = 0, keep = 0;
  for (uint64_t p = 0; p < size;) {
    uint32_t k = size - p < 4096 ? (uint32_t)(size - p) : 4096;
    if (!e.b->read(path, p, w + keep, k, &got) || got != k) return false;
    uint32_t n = keep + k;
    for (uint32_t i = 0; i + L <= n; ++i) {
      if (w[i] != 's' || memcmp(w + i, kCap, L)) continue;
      char t[64];
      if (!e.b->read(path, p - keep + i + L, t, sizeof t - 1, &got)) return false;   // it may run past this piece
      t[got] = 0;
      const char* cs = strstr(t, " c=");
      size_t nv = cs ? (size_t)(cs - t) : 0, nc = cs ? strlen(cs + 3) : 0;
      if (!cs || strlen(t) == got || nv < 1 || nv > 23 || nc < 7 || nc > 23) continue;
      memcpy(v, t, nv);
      v[nv] = 0;
      memcpy(c, cs + 3, nc + 1);
      return true;
    }
    keep = n < 95 ? n : 95;
    memmove(w, w + n - keep, keep);
    p += k;
    yield(e);
  }
  return false;
}

void stamp_of(char* out, int cap, const char* v, const char* c) { sniprintf(out, cap, "sidecar-webupdate-1 v=%s c=%s", v, c); }

void text_of(char* out, int cap, const char* v, const char* c) {
  if (v[0]) sniprintf(out, cap, "%s (%s)", v, c);
  else sniprintf(out, cap, "%s", "");
}

}  // namespace

const char* stamp() { return kStamp; }
const char* boot_name(int board) { return board == 5 ? "/kernel_2712.img" : "/engine.img"; }
const char* trial_name(int board) { return board == 5 ? "/kernel_2712-try.img" : "/engine-try.img"; }

const char* parse_header(const uint8_t* h, uint64_t file_size, Header* out) {
  if (file_size < kHeader || memcmp(h, "SCUPDAT1", 8)) return "not a Sidecar update file";
  if (le32(h + 8) != kHeader) return "not a Sidecar update file (its header size)";
  if (le32(h + 12) != 1) return "an update file of a newer format: update to an earlier release first, or by SD card";
  uint8_t sha[32];
  core::lib::sha256(h, 480, sha);
  if (memcmp(sha, h + 480, 32)) return "the update file's header is damaged (download it again)";
  memset(out, 0, sizeof *out);
  uint32_t n = le32(h + 16);
  if (n < 1 || n > (uint32_t)kMaxParts) return "the update file's part count is wrong";
  out->n = (int)n;
  out->flags = le32(h + 20);
  memcpy(out->version, h + 24, 24);
  memcpy(out->commit, h + 48, 24);
  if (out->version[23] || out->commit[23] || !out->version[0] || !out->commit[0]) return "the update file's version is malformed";
  for (int i = 0; i < 2; ++i) {   // the text fields: printable, no quote or backslash (they go into JSON)
    const char* t = i ? out->commit : out->version;
    for (const char* c = t; *c; ++c) if (*c < 0x21 || *c > 0x7e || *c == '"' || *c == '\\') return "the update file's version is malformed";
  }
  uint64_t at = kHeader;
  for (int i = 0; i < out->n; ++i) {
    const uint8_t* p = h + 80 + 64 * i;
    Part& q = out->part[i];
    q.kind = le32(p), q.flags = le32(p + 4), q.off = le64(p + 8), q.size = le64(p + 16);
    memcpy(q.sha, p + 24, 32);
    if (q.kind < kPartPi4 || q.kind > kPartWeb) return "the update file has a part of an unknown kind";
    for (int j = 0; j < i; ++j) if (out->part[j].kind == q.kind) return "the update file has a part twice";
    if (q.off != at || q.size == 0 || q.size > kFileMax || q.off + q.size > file_size) return "the update file is cut short or its parts overlap (download it again)";
    at = q.off + q.size;
  }
  for (int i = out->n; i < kMaxParts; ++i) if (!zero(h + 80 + 64 * i, 64)) return "the update file's header is malformed";
  if (at != file_size) return "the update file has bytes after its last part";
  return nullptr;
}

namespace {
bool vparse(const char* v, unsigned* m, const char** pre) {
  for (int i = 0; i < 3; ++i) {
    if (*v < '0' || *v > '9') return false;
    unsigned x = 0;
    while (*v >= '0' && *v <= '9') { x = x * 10 + (unsigned)(*v - '0'); if (x > 1000000) return false; ++v; }
    m[i] = x;
    if (i < 2) { if (*v != '.') return false; ++v; }
  }
  if (*v == 0) { *pre = nullptr; return true; }
  if (*v != '-' || !v[1]) return false;
  *pre = v + 1;
  return true;
}
}  // namespace

int cmp_version(const char* a, const char* b) {
  unsigned x[3], y[3];
  const char *pa, *pb;
  if (!vparse(a, x, &pa) || !vparse(b, y, &pb)) return strcmp(a, b) == 0 ? 0 : -2;
  for (int i = 0; i < 3; ++i) if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
  if (!pa || !pb) return !pa && !pb ? 0 : !pa ? 1 : -1;   // a release is above its pre-releases
  bool da = strcmp(pa, "dev") == 0, db = strcmp(pb, "dev") == 0;
  if (da != db) return da ? -1 : 1;
  int c = strcmp(pa, pb);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}

namespace {
// config.txt's sdboot block (pi/circle/sdboot.py block()): its line, exactly
const char kInitrd[] = "initramfs engine.img 0x02000000";
const char kBegin[] = "# sidecar sdboot begin";
const char kEnd[] = "# sidecar sdboot end";
bool app(char* out, int cap, int* k, const char* s, size_t n);
bool apps(char* out, int cap, int* k, const char* s) { return app(out, cap, k, s, strlen(s)); }
bool app(char* out, int cap, int* k, const char* s, size_t n) {
  if (*k + (int)n + 1 > cap) return false;
  memcpy(out + *k, s, n);
  *k += (int)n;
  out[*k] = 0;
  return true;
}
}  // namespace

const char* tryboot_txt(int board, const char* config, char* out, int cap) {
  int k = 0, found = 0;
  bool in = false;
  out[0] = 0;
  if (!apps(out, cap, &k, kOurs) || !apps(out, cap, &k, " (a trial boot; the Sidecar deletes this file)\n"))
    return "tryboot.txt too long";
  for (const char* l = config; *l;) {
    const char* nl = strchr(l, '\n');
    size_t n = nl ? (size_t)(nl - l + 1) : strlen(l);
    size_t t = n;   // the line without its end
    while (t && (l[t - 1] == '\n' || l[t - 1] == '\r')) --t;
    if (t >= sizeof kBegin - 1 && strncmp(l, kBegin, sizeof kBegin - 1) == 0) in = true;
    bool end = in && t >= sizeof kEnd - 1 && strncmp(l, kEnd, sizeof kEnd - 1) == 0;
    if (board == 5 && (in || end)) {   // the Pi 5: the block goes (as sdboot.py pi5-try)
    } else if (board == 4 && in && t == sizeof kInitrd - 1 && strncmp(l, kInitrd, t) == 0) {
      ++found;
      if (!apps(out, cap, &k, "initramfs engine-try.img 0x02000000") || !app(out, cap, &k, l + t, n - t)) return "tryboot.txt too long";
    } else if (!app(out, cap, &k, l, n))
      return "tryboot.txt too long";
    if (end) in = false;
    l += n;
  }
  if (k && out[k - 1] != '\n' && !app(out, cap, &k, "\n", 1)) return "tryboot.txt too long";
  if (board == 4) {
    if (found != 1) return "this card does not start the Sidecar through sdboot (config.txt): update it in a computer";
    if (!apps(out, cap, &k, "[pi4]\ncmdline=cmdline-try.txt\n")) return "tryboot.txt too long";
  } else if (!apps(out, cap, &k, "[pi5]\nkernel=kernel_2712-try.img\ncmdline=cmdline-try.txt\n"))
    return "tryboot.txt too long";
  return nullptr;
}

bool cmdline_try(const char* cmdline, char* out, int cap) {
  size_t n = 0;
  while (cmdline[n] && cmdline[n] != '\n' && cmdline[n] != '\r') ++n;
  while (n && cmdline[n - 1] == ' ') --n;
  return sniprintf(out, (size_t)cap, "%.*s%supdtrial=1\n", (int)n, cmdline, n ? " " : "") < cap;
}

void put(const Env& e, uint64_t off, uint64_t total, uint32_t crc, const uint8_t* data, uint32_t n, fsweb::Result* r) {
  static uint64_t next, want_total;
  static uint32_t run;
  static bool on;
  if (!e.enabled) return say(r, 403, e.off_why);
  Busy busy;
  if (!busy.ok) return say(r, 409, "an update is being installed");
  State st;
  load(e, &st);
  if (in_flight(e, st)) return say(r, 409, "an update is being tried: wait for it to finish");
  if (total < kHeader || total > kFileMax) return say(r, 400, "not a Sidecar update file (its size)");
  if (n > fsweb::kChunkMax || off + n > total) return say(r, 400, "chunk too big or past the total");
  if (off == 0) {
    uint64_t fr = 0, tot = 0;
    if (!exists(e, kDir) && !e.b->mkdir(kDir)) return say(r, 500, "could not make /.rytm/update");
    rm(e, kFile);   // an older upload: its space first
    if (e.b->space(&fr, &tot) && total + (1u << 20) > fr) return say(r, 507, "not enough free space on the card");
    next = 0, want_total = total, run = 0, on = true;
  } else if (!on || off != next || total != want_total) {
    sniprintf(r->msg, sizeof r->msg, "expected %llu", (unsigned long long)(on && total == want_total ? next : 0));
    r->status = 409;
    return;
  }
  for (uint32_t done = 0; done < n;) {   // 4 KB pieces, each read back
    uint32_t k = n - done < 4096 ? n - done : 4096;
    uint32_t got = 0;
    if (!e.b->write_at(kUpload, off + done, data + done, k, off + done == 0) ||
        !e.b->read(kUpload, off + done, g_back, k, &got) || got != k || memcmp(g_back, data + done, k)) {
      on = false;
      rm(e, kUpload);
      return say(r, 500, "write failed or the read-back differs (the card full or removed?)");
    }
    run = fsweb::crc32(run, data + done, k);
    done += k;
    yield(e);
  }
  next = off + n;
  if (next < total) {
    sniprintf(r->msg, sizeof r->msg, "ok %llu", (unsigned long long)next);
    r->status = 200;
    return;
  }
  on = false;
  if (run != crc) { rm(e, kUpload); return say(r, 400, "the upload's CRC is wrong: not kept (try again)"); }
  if (!e.b->rename(kUpload, kFile)) { rm(e, kUpload); return say(r, 500, "could not keep the upload"); }
  sniprintf(r->msg, sizeof r->msg, "done %llu", (unsigned long long)total);
  r->status = 200;
}

void discard(const Env& e, fsweb::Result* r) {
  Busy busy;
  if (!busy.ok) return say(r, 409, "an update is being installed");
  State st;
  load(e, &st);
  if (in_flight(e, st)) return say(r, 409, "an update is being tried: wait for it to finish");
  rm(e, kUpload);
  rm(e, kFile);
  say(r, 200, "discarded");
}

Reboot install(const Env& e, bool force, fsweb::Result* r) {
  if (!e.enabled) { say(r, 403, e.off_why); return kNone; }
  Busy busy;
  if (!busy.ok) { say(r, 409, "an update is being installed"); return kNone; }
  State s;
  load(e, &s);
  if (in_flight(e, s)) { say(r, 409, "a trial is running: wait for it to finish"); return kNone; }
  uint64_t fsz = 0;
  if (!exists(e, kFile, &fsz)) { say(r, 409, "no update file: upload it first"); return kNone; }
  uint8_t h[kHeader];
  uint32_t got = 0;
  Header hd;
  const char* why = fsz < kHeader || !e.b->read(kFile, 0, h, kHeader, &got) || got != kHeader ? "not a Sidecar update file" : parse_header(h, fsz, &hd);
  if (why) { say(r, 422, why); return kNone; }
  char want[80];
  stamp_of(want, sizeof want, hd.version, hd.commit);
  const Part* mine = nullptr;
  uint64_t unpack = 0;
  char wsha[68] = "";
  for (int i = 0; i < hd.n; ++i) {   // every part checked, the other board's too: a damaged file is refused whole
    const Part& p = hd.part[i];
    uint8_t sha[32];
    if (p.kind == kPartWeb) why = check_web(e, kFile, p.off, p.size, sha), unpack = p.size;
    else why = check_kernel(e, kFile, p.off, p.size, p.kind == kPartPi5 ? 5 : 4, want, nullptr, sha);
    if (!why && memcmp(sha, p.sha, 32)) why = "a part's sha256 is wrong: the file is damaged (download it again)";
    if (why) { say(r, 422, why); return kNone; }
    if (p.kind == (e.board == 5 ? kPartPi5 : kPartPi4)) mine = &p;
    if (p.kind == kPartWeb) to_hex(p.sha, 32, wsha);
  }
  if (!mine) { say(r, 422, e.board == 5 ? "this file has no kernel for the Pi 5" : "this file has no kernel for the Pi 4"); return kNone; }
  int c = cmp_version(hd.version, e.version);
  if (!force && c <= 0) {
    if (c == 0 && strcmp(hd.commit, e.commit) == 0) sniprintf(r->msg, sizeof r->msg, "the Sidecar already runs %s (%s)", hd.version, hd.commit);
    else if (c == 0) sniprintf(r->msg, sizeof r->msg, "the same version (%s) as the running one: install anyway?", hd.version);
    else sniprintf(r->msg, sizeof r->msg, "%s is older than the running %s: install anyway?", hd.version, e.version);
    r->status = 409;
    return kNone;
  }
  uint64_t fr = 0, tot = 0;
  if (e.b->space(&fr, &tot) && mine->size + 2 * unpack + (1u << 20) > fr) { say(r, 507, "not enough free space on the card"); return kNone; }
  memset(&s, 0, sizeof s);
  strcpy(s.kind, "update");
  sniprintf(s.tv, sizeof s.tv, "%s", hd.version);
  sniprintf(s.tc, sizeof s.tc, "%s", hd.commit);
  sniprintf(s.fv, sizeof s.fv, "%s", e.version);
  sniprintf(s.fc, sizeof s.fc, "%s", e.commit);
  s.web = unpack != 0;
  memcpy(s.wsha, wsha, sizeof wsha);
  set(&s, "staging", "");
  if (!save(e, s)) { say(r, 500, "could not write /.rytm/update/state.txt"); return kNone; }
  why = stage_trial(e, kFile, mine->off, mine->size, mine->sha, want);
  if (why) {
    clean_trial(e);
    set(&s, "failed", why);
    save(e, s);
    say(r, 500, why);
    return kNone;
  }
  set(&s, "trying", "");
  if (!save(e, s)) { clean_trial(e); say(r, 500, "could not write /.rytm/update/state.txt"); return kNone; }
  sniprintf(r->msg, sizeof r->msg, "restarting into %s (%s) as a trial", hd.version, hd.commit);
  r->status = 200;
  return kTry;
}

Reboot previous(const Env& e, bool force, fsweb::Result* r) {
  if (!e.enabled) { say(r, 403, e.off_why); return kNone; }
  Busy busy;
  if (!busy.ok) { say(r, 409, "an update is being installed"); return kNone; }
  State s;
  load(e, &s);
  if (in_flight(e, s)) { say(r, 409, "a trial is running: wait for it to finish"); return kNone; }
  char kp[fsweb::kPath + 8], info[160], pv[24], pc[24];
  sniprintf(kp, sizeof kp, "%s.prev", boot_name(e.board));
  uint64_t size = 0;
  if (!exists(e, kp, &size)) { say(r, 404, "no previous version on the card"); return kNone; }
  uint8_t sha[32];
  bool cap = false;
  const char* why = check_kernel(e, kp, 0, size, e.board, nullptr, &cap, sha);
  if (why) { sniprintf(r->msg, sizeof r->msg, "the previous kernel cannot be used: %s", why); r->status = 422; return kNone; }
  if (!read_text(e, kPrevInfo, info, sizeof info)) info[0] = 0;
  field(info, "v", pv, sizeof pv);
  field(info, "c", pc, sizeof pc);
  char pw[4];
  field(info, "w", pw, sizeof pw);
  // the kernel's own stamp names it (prev.txt can be stale: a card changed in a computer, sdboot.py); prev.txt's w=1
  // (www-next.prev is that version's app) counts only when prev.txt names the same build
  char sv[24], sc[24];
  bool same = false;
  if (cap && stamp_in(e, kp, size, sv, sc)) {
    same = strcmp(sv, pv) == 0 && strcmp(sc, pc) == 0;
    strcpy(pv, sv);
    strcpy(pc, sc);
  }
  memset(&s, 0, sizeof s);
  strcpy(s.kind, "previous");
  strcpy(s.tv, pv);
  strcpy(s.tc, pc);
  sniprintf(s.fv, sizeof s.fv, "%s", e.version);
  sniprintf(s.fc, sizeof s.fc, "%s", e.commit);
  if (cap) {   // it can commit itself: a trial, as an update
    s.web = exists(e, kWebPrev) && same && pw[0] == '1';   // www-next.prev only when it is this kernel's app
    uint64_t fr = 0, tot = 0;
    if (e.b->space(&fr, &tot) && size + (1u << 20) > fr) { say(r, 507, "not enough free space on the card"); return kNone; }
    set(&s, "staging", "");
    if (!save(e, s)) { say(r, 500, "could not write /.rytm/update/state.txt"); return kNone; }
    char want[80];
    stamp_of(want, sizeof want, pv, pc);
    why = stage_trial(e, kp, 0, size, sha, pv[0] ? want : nullptr);
    if (why) { clean_trial(e); set(&s, "failed", why); save(e, s); say(r, 500, why); return kNone; }
    set(&s, "trying", "");
    if (!save(e, s)) { clean_trial(e); say(r, 500, "could not write /.rytm/update/state.txt"); return kNone; }
    say(r, 200, "restarting into the previous version as a trial");
    return kTry;
  }
  if (!force) {
    say(r, 409, "the previous version has no web update: going back swaps it in directly, without a trial, and coming back needs the SD card. Go back anyway?");
    return kNone;
  }
  // a direct swap (sdboot.py previous): the kernel only - an engine without this code does not serve www-next's app
  char sw[fsweb::kPath + 8];
  sniprintf(sw, sizeof sw, "%s.swp", boot_name(e.board));
  set(&s, "swapping", "");
  if (!save(e, s)) { say(r, 500, "could not write /.rytm/update/state.txt"); return kNone; }
  rm(e, sw);
  if (!e.b->rename(boot_name(e.board), sw)) { say(r, 500, "could not move the kernel aside"); return kNone; }
  if (!e.b->rename(kp, boot_name(e.board))) { e.b->rename(sw, boot_name(e.board)); say(r, 500, "could not put the previous kernel in place"); return kNone; }
  e.b->rename(sw, kp);
  static char t[96];
  sniprintf(t, sizeof t, "v=%s\nc=%s\nw=0\n", s.fv, s.fc);
  write_text(e, kPrevInfo, t);
  set(&s, "committed", "back to the previous version (swapped directly)");
  save(e, s);
  say(r, 200, "restarting into the previous version");
  return kPlain;
}

bool boot(const Env& e) {
  if (!exists(e, kState) && exists(e, kStatePart)) e.b->rename(kStatePart, kState);   // a cut save's whole .part (load)
  State s;
  load(e, &s);
  char m[160], tx[48];
  text_of(tx, sizeof tx, s.tv, s.tc);
  if (is(s, "staging")) {   // a power cut (or a crash) while the old engine staged: nothing the boot reads changed
    set(&s, "failed", "the install was interrupted before it restarted: nothing changed (upload it again)");
    save(e, s);
    clean_trial(e);
  } else if (is(s, "trying")) {
    if (e.trial) return true;   // the trial: commit_due() / commit()
    sniprintf(m, sizeof m, "the new version%s%s did not start, or the power went off during its first minute: the "
              "Sidecar went back to %s (%s) by itself%s", tx[0] ? " " : "", tx, e.version, e.commit,
              e.board == 4 ? " (or this Pi 4's bootloader is too old for trial boots)" : "");
    set(&s, "reverted", m);
    save(e, s);
    clean_trial(e);
  } else if (is(s, "committing")) {   // the commit was cut: before the kernel's renames, or after
    if (exists(e, trial_name(e.board))) {
      sniprintf(m, sizeof m, "the commit was interrupted before the new kernel was in place: still %s (%s); install it again", e.version, e.commit);
      set(&s, "reverted", m);
      save(e, s);
      clean_trial(e);
    } else
      finish(e, &s);
  } else if (is(s, "swapping")) {   // a direct "previous" was cut (only this code's engine gets here)
    char kp[fsweb::kPath + 8], sw[fsweb::kPath + 8];
    sniprintf(kp, sizeof kp, "%s.prev", boot_name(e.board));
    sniprintf(sw, sizeof sw, "%s.swp", boot_name(e.board));
    if (exists(e, sw) && !exists(e, kp)) e.b->rename(sw, kp);
    set(&s, "failed", "going back was interrupted: the Sidecar runs the version it shows");
    save(e, s);
    clean_trial(e);
  } else if (!e.trial)
    clean_trial(e);   // leftovers of a cut install (the trial's names are this code's only)
  return false;
}

bool commit_due(const Env& e) {
  if (!e.trial || e.uptime_s < kCommitAfterS) return false;
  State s;
  load(e, &s);
  return is(s, "trying") != nullptr;
}

bool commit(const Env& e) {
  Busy busy;
  if (!busy.ok) return false;
  State s;
  load(e, &s);
  if (!e.trial || !is(s, "trying")) return false;
  if (s.tv[0] && (strcmp(s.tv, e.version) || strcmp(s.tc, e.commit))) {   // not what was staged: never made permanent
    clean_trial(e);
    char m[160];
    sniprintf(m, sizeof m, "the trial runs %s (%s), not the staged %s (%s): not kept", e.version, e.commit, s.tv, s.tc);
    set(&s, "failed", m);
    save(e, s);
    return false;
  }
  if (!exists(e, trial_name(e.board))) return false;   // (then "trying" stays: the next plain boot reverts)
  set(&s, "committing", "");
  s.ws = 0;
  if (!save(e, s)) return false;
  char kp[fsweb::kPath + 8];
  sniprintf(kp, sizeof kp, "%s.prev", boot_name(e.board));
  rm(e, kp);
  // THE WINDOW: between these two renames the card has no kernel (milliseconds; docs/web-update.md)
  if (exists(e, boot_name(e.board)) && !e.b->rename(boot_name(e.board), kp)) return false;
  if (!e.b->rename(trial_name(e.board), boot_name(e.board))) {
    for (int i = 0; i < 5 && !exists(e, boot_name(e.board)); ++i) e.b->rename(kp, boot_name(e.board));   // the old one back
    return false;
  }
  return finish(e, &s);
}

int state_json(const Env& e, char* out, int cap) {
  State s;
  load(e, &s);
  static char info[160];
  char pv[24], pc[24], prev[48], target[48], from[48], kp[fsweb::kPath + 8];
  if (!read_text(e, kPrevInfo, info, sizeof info)) info[0] = 0;
  field(info, "v", pv, sizeof pv);
  field(info, "c", pc, sizeof pc);
  text_of(prev, sizeof prev, pv, pc);
  text_of(target, sizeof target, s.tv, s.tc);
  text_of(from, sizeof from, s.fv, s.fc);
  sniprintf(kp, sizeof kp, "%s.prev", boot_name(e.board));
  uint64_t up = 0;
  bool have_up = exists(e, kFile, &up);
  bool have_prev = exists(e, kp);
  unsigned left = e.trial && is(s, "trying") && e.uptime_s < kCommitAfterS ? kCommitAfterS - e.uptime_s : 0;
  static char msg[330];   // JSON-escaped
  int q = 0;
  for (const char* c = s.msg; *c && q < (int)sizeof msg - 3; ++c) {
    if (*c == '"' || *c == '\\') msg[q++] = '\\';
    msg[q++] = *c >= 0x20 && *c <= 0x7e ? *c : ' ';
  }
  msg[q] = 0;
  int k = sniprintf(out, (size_t)cap,
                    "{\"api\":\"%s\",\"enabled\":%s,\"why\":\"%s\",\"board\":%d,\"version\":\"%s\",\"commit\":\"%s\","
                    "\"trial\":%s,\"state\":\"%s\",\"kind\":\"%s\",\"target\":\"%s\",\"from\":\"%s\",\"msg\":\"%s\","
                    "\"prev\":%s,\"prevVersion\":\"%s\",\"uploaded\":%llu,\"commitIn\":%u,\"uptime\":%u}",
                    kStamp, e.enabled ? "true" : "false", e.enabled ? "" : e.off_why, e.board, e.version, e.commit,
                    e.trial ? "true" : "false", s.state, s.kind, target, from, msg, have_prev ? "true" : "false", prev,
                    (unsigned long long)(have_up ? up : 0), left, e.uptime_s);
  return k < cap ? k : 0;
}

}  // namespace upd
