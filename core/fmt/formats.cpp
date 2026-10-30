// core/fmt/formats.cpp - the registry's table (fmt.h): every format of the spec's inventory (section 1) at the version
// its files have today, with a sniffer (bytes -> name and version) and the current version's reader (bytes -> usable
// or why not). v1 of a format that has no header today is "today's file": its sniffer accepts the bytes as they are
// and reports version 1; a header (spec 3.2) says a later version.
//
// The readers check what the module's own reader needs (the same limits: a pool line of kPath - 1 characters at
// most, rmp_slots_parse for the slot map, seq::unpack for a pattern, song::read_json for a take ...): F1 does not
// change any module's loading (F3 moves the modules onto load()), so a file a reader here refuses is one its module
// refuses too, and the registry's verdict is what a later load() through it will say.
#include <string.h>

#include "fmt.h"

#if CORE_MSAMP   // (the Pi and the Mac; the frozen Teensy build copies core/ whole: an empty table there)
#include "../lib/library.h"
#include "../lib/pools.h"
#include "../lib/pools_fmt.h"
#include "../chop/slicemap.h"
#include "../seq/seq.h"
#include "../song/song.h"
#include "fmt.h"
#include "rmp_format.h"
#include "rmp_resolve.h"
#include "rmp_slots.h"

namespace core {
namespace fmt {

namespace {
// ---- helpers
bool say(char* why, int cap, const char* m) {
  if (why && cap) sniprintf(why, cap, "%s", m);
  return false;
}
// the bytes as text lines from s.body: no NUL, no line over max_line characters (the '\r' of CRLF not counted);
// each line handed to ok() (null: any)
bool lines(const Bytes& in, const Sniffed& s, uint32_t max_line, bool (*ok)(const char* l, uint32_t n), char* why,
           int cap, uint32_t max_lines = 0) {
  uint32_t nl = 0;
  for (uint32_t a = s.body; a < in.n;) {
    uint32_t b = a;
    while (b < in.n && in.p[b] != '\n') {
      if (!in.p[b]) return say(why, cap, "a NUL byte (binary data)");
      ++b;
    }
    uint32_t e = b;
    if (e > a && in.p[e - 1] == '\r') --e;
    if (e - a > max_line) return say(why, cap, "a line too long");
    if (ok && !ok((const char*)in.p + a, e - a)) return say(why, cap, "a line it does not read");
    if (max_lines && ++nl > max_lines) return say(why, cap, "too many lines");
    a = b + 1;
  }
  return true;
}
bool no_nul(const Bytes& in, uint32_t from) {
  return !memchr(in.p + from, 0, in.n - from);
}
// text formats: "#sidecar <name> N" says N, else today's v1 (any text: the NUL check is the reader's)
bool sniff_text(const Bytes& in, const char* name, Sniffed* o) {
  if (text_header(in, name, o)) return true;
  static const char kTag[] = "#sidecar ";
  if (in.n >= sizeof kTag - 1 && !memcmp(in.p, kTag, sizeof kTag - 1)) return false;   // another format's header
  o->version = o->min_reader = 1, o->body = 0;
  return true;
}
// key=value settings: "format=<name>/N" says N, else today's v1
bool sniff_kv(const Bytes& in, const char* name, Sniffed* o) {
  if (kv_header(in, name, o)) return true;
  if (in.n >= 7 && !memcmp(in.p, "format=", 7)) return false;
  o->version = o->min_reader = 1, o->body = 0;
  return true;
}
bool printable(const char* l, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if ((uint8_t)l[i] < 0x20 && l[i] != '\t') return false;
  return true;
}
bool kv_line(const char* l, uint32_t n) {   // blank, '#', or key=value
  if (!n || l[0] == '#') return true;
  return memchr(l, '=', n) && printable(l, n);
}
int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool hexrun(const char* l, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i)
    if (hexv(l[i]) < 0) return false;
  return true;
}
bool path_line(const char* l, uint32_t n) { return printable(l, n); }

// ---- per-project content (class P)
bool sn_pool(const Bytes& in, Sniffed* o) { return sniff_text(in, "pool", o); }
bool rd_pool(const Bytes& in, const Sniffed& s, char* why, int cap) {   // pool.cpp pool_load: a line of kPath - 1 at most
  return lines(in, s, 159, path_line, why, cap);
}
bool sn_bankpool(const Bytes& in, Sniffed* o) { return sniff_text(in, "bankpool", o); }
bool rd_bankpool(const Bytes& in, const Sniffed& s, char* why, int cap) {   // srclib: 1024 rows (bank A..H x 128)
  return lines(in, s, 159, path_line, why, cap, 1024 + 1);
}
bool sn_picks(const Bytes& in, Sniffed* o) { return sniff_text(in, "picks", o); }
bool pick_line(const char* l, uint32_t n) {   // granular.cpp picks_save: "<kit> <voice> <base> <key>"
  if (!n) return true;
  uint32_t i = 0;
  for (int f = 0; f < 3; ++f) {
    if (i < n && l[i] == '-') ++i;
    uint32_t d = i;
    while (i < n && l[i] >= '0' && l[i] <= '9') ++i;
    if (i == d || i >= n || l[i] != ' ') return false;
    ++i;
  }
  return printable(l, n);
}
bool rd_picks(const Bytes& in, const Sniffed& s, char* why, int cap) { return lines(in, s, 200, pick_line, why, cap); }
bool sn_slots(const Bytes& in, Sniffed* o) { return sniff_text(in, "slots", o); }
bool rd_slots(const Bytes& in, const Sniffed& s, char* why, int cap) {   // um.cpp map_read: the whole parse or nothing
  static rmp_slot_map m;
  if (!no_nul(in, s.body)) return say(why, cap, "a NUL byte (binary data)");
  if (!rmp_slots_parse((const char*)in.p + s.body, in.n - s.body, &m)) return say(why, cap, "not a slot map");
  return true;
}
bool sn_slotnotes(const Bytes& in, Sniffed* o) { return sniff_text(in, "slotnotes", o); }
bool rd_slotnotes(const Bytes& in, const Sniffed& s, char* why, int cap) {
  static rmp_slot_notes m;
  if (!no_nul(in, s.body)) return say(why, cap, "a NUL byte (binary data)");
  if (in.n > s.body && !rmp_notes_parse((const char*)in.p + s.body, in.n - s.body, &m)) return say(why, cap, "not a notes file");
  return true;
}
bool sn_fxslots(const Bytes& in, Sniffed* o) { return sniff_text(in, "fxslots", o); }
bool fxslot_line(const char* l, uint32_t n) {   // send_fx_card.cpp slots_load: "XXXX name" (others are empty slots)
  return printable(l, n);
}
bool rd_fxslots(const Bytes& in, const Sniffed& s, char* why, int cap) { return lines(in, s, 63, fxslot_line, why, cap); }
bool sn_juno(const Bytes& in, Sniffed* o) { return sniff_text(in, "juno", o); }
bool juno_line(const char* l, uint32_t n) {   // juno_lib.h parse: 36 hex [TAB name], empty = an empty slot
  if (!n) return true;
  return n >= 36 && hexrun(l, 36) && (n == 36 || l[36] == '\t') && printable(l, n);
}
bool rd_juno(const Bytes& in, const Sniffed& s, char* why, int cap) { return lines(in, s, 63, juno_line, why, cap); }
bool sn_fm(const Bytes& in, Sniffed* o) { return sniff_text(in, "fm", o); }
bool fm_line(const char* l, uint32_t n) {   // dx7_lib.h parse: 32 hex TAB voice [TAB name]
  if (!n) return true;
  if (n < 34 || !hexrun(l, 32) || l[32] != '\t' || l[33] < '0' || l[33] > '9') return false;
  return printable(l, n);
}
bool rd_fm(const Bytes& in, const Sniffed& s, char* why, int cap) { return lines(in, s, 80, fm_line, why, cap); }
bool sn_lfo(const Bytes& in, Sniffed* o) { return sniff_text(in, "lfo", o); }
bool sn_ins(const Bytes& in, Sniffed* o) { return sniff_text(in, "ins", o); }
bool sn_kits(const Bytes& in, Sniffed* o) { return sniff_text(in, "kits", o); }   // core/bundle/kits.h: kNNN.sks / .prv
bool rd_kitlines(const Bytes& in, const Sniffed& s, char* why, int cap) {   // lfo.cpp / insert_card.cpp: '#' skipped
  return lines(in, s, 255, printable, why, cap);
}
bool sn_sqp(const Bytes& in, Sniffed* o) {   // SQP1: the version byte is "the lowest reader" (spec 3.2)
  if (in.n < 12 || memcmp(in.p, "SQP1", 4) || !in.p[4]) return false;
  o->version = o->min_reader = in.p[4], o->body = 12;
  return true;
}
bool rd_sqp(const Bytes& in, const Sniffed& s, char* why, int cap) {
  static seq::Pattern p;
  if (!seq::unpack(p, 0, in.p, (int)in.n)) return say(why, cap, "bad CRC, cut short, or damaged");
  return true;
}
uint16_t mr_sqp(const Bytes& in) { return in.n > 4 ? in.p[4] : 1; }
// E2 (seqx): the chain v1 -> v2 -> v3 -> v4, pure on bytes (the pattern model in a static, as rd_sqp's). v1 -> v2: a
// lock's 2-byte value becomes 0021's 14 bits (seq::unpack reads v1, seq::pack writes the lowest version: 2); v2 -> v3:
// the version byte (the condition bits are clear in a v2 file as its reader takes it: unpack + pack clears them);
// v3 -> v4: the version byte (a v3 body is a v4 body without the block trailer).
bool up_sqp_repack(const Bytes& in, Buf* out, int to) {
  static seq::Pattern p;
  if (in.n < 12 || in.p[4] != to - 1 || !seq::unpack(p, 0, in.p, (int)in.n)) return false;
  if (to < 4) {   // a v1 / v2 body through the writer (it writes v2 for both: no conditions survive a v2 reader)
    int n = seq::pack(p, out->p, (int)out->cap);
    if (n <= 0 || out->p[4] != 2) return false;
    out->p[4] = (uint8_t)to, out->n = (uint32_t)n;   // (the CRC is the body's: the header byte is outside it)
    return true;
  }
  if (in.n > out->cap) return false;
  memmove(out->p, in.p, in.n);
  out->p[4] = 4, out->n = in.n;
  return true;
}
bool up_sqp_1(const Bytes& in, Buf* out, Ctx*) { return up_sqp_repack(in, out, 2); }
bool up_sqp_2(const Bytes& in, Buf* out, Ctx*) { return up_sqp_repack(in, out, 3); }
bool up_sqp_3(const Bytes& in, Buf* out, Ctx*) { return up_sqp_repack(in, out, 4); }
const Upgrade kSqpChain[] = {{1, up_sqp_1}, {2, up_sqp_2}, {3, up_sqp_3}};
bool sn_tracks(const Bytes& in, Sniffed* o) { return sniff_kv(in, "tracks", o); }
bool rd_kv(const Bytes& in, const Sniffed& s, char* why, int cap) { return lines(in, s, 255, kv_line, why, cap); }
bool sn_song(const Bytes& in, Sniffed* o) { return json_header(in, "song", o); }
bool rd_song(const Bytes& in, const Sniffed& s, char* why, int cap) {   // json.cpp read_json: the whole take
  static song::Take t;
  char err[64] = "";
  if (in.n > (64u << 20)) return say(why, cap, "too big");
  t.clear();
  bool ok = song::read_json((const char*)in.p, (int)in.n, &t, err, sizeof err);
  if (!ok) return say(why, cap, err[0] ? err : "not a song take");
  return true;
}

// ---- settings (class S): key=value, '#' lines skipped; unknown keys carried (kv_keep)
bool sn_display(const Bytes& in, Sniffed* o) { return sniff_kv(in, "display", o); }
bool sn_keys(const Bytes& in, Sniffed* o) { return sniff_kv(in, "keys", o); }
bool sn_fxin(const Bytes& in, Sniffed* o) { return sniff_kv(in, "fxin", o); }
bool sn_sendfx(const Bytes& in, Sniffed* o) { return sniff_kv(in, "sendfx", o); }
bool sn_wifi(const Bytes& in, Sniffed* o) { return sniff_kv(in, "wifi", o); }
bool sn_sdtest(const Bytes& in, Sniffed* o) { return sniff_kv(in, "sdtest", o); }
bool sn_project(const Bytes& in, Sniffed* o) {   // pool.cpp project_save: the Rytm's name, raw (no newline)
  o->version = o->min_reader = 1, o->body = 0;
  return in.n <= 32;
}
bool rd_project(const Bytes& in, const Sniffed& s, char* why, int cap) {
  for (uint32_t i = 0; i < in.n; ++i)
    if (in.p[i] < 0x20 || in.p[i] > 0x7e) return say(why, cap, "not a project name");
  return true;
}
bool sn_projects(const Bytes& in, Sniffed* o) { return sniff_text(in, "projects", o); }
bool projects_line(const char* l, uint32_t n) {   // projects.cpp format: key TAB rytm TAB used TAB date TAB flags
  if (!n) return true;
  int tabs = 0;
  for (uint32_t i = 0; i < n; ++i) tabs += l[i] == '\t';
  return tabs >= 4 && printable(l, n);
}
bool rd_projects(const Bytes& in, const Sniffed& s, char* why, int cap) { return lines(in, s, 255, projects_line, why, cap); }

// ---- derived (class D): rebuilt on any mismatch, never upgraded
bool sn_cardindex(const Bytes& in, Sniffed* o) {   // card_index.cpp: "hash=...\nversion=4\n..."
  const char* p = (const char*)in.p;
  for (uint32_t a = 0; a + 8 < in.n;) {
    if (!memcmp(p + a, "version=", 8)) {
      uint32_t v = 0, i = a + 8;
      while (i < in.n && p[i] >= '0' && p[i] <= '9' && v < 65535) v = v * 10 + (uint32_t)(p[i++] - '0');
      if (i == a + 8 || !v || v > 65535) return false;
      o->version = o->min_reader = (uint16_t)v, o->body = 0;
      return true;
    }
    const char* nl = (const char*)memchr(p + a, '\n', in.n - a);
    if (!nl) break;
    a = (uint32_t)(nl - p) + 1;
  }
  return false;
}
bool sn_dx7idx(const Bytes& in, Sniffed* o) {   // dx7_lib.h Header: "DX7IDX1\0"
  if (in.n < 24 || memcmp(in.p, "DX7IDX1", 8)) return false;
  o->version = o->min_reader = 1, o->body = 24;
  return true;
}

// ---- exchange (class X): their own range fields; sniffed and reported only
bool sn_rmpa(const Bytes& in, Sniffed* o) {   // rmp_format.h: magic RMPA (Pi) / RMP1 (Teensy), u16 format
  if (in.n < RMP_HEADER_BYTES || (memcmp(in.p, "RMPA", 4) && memcmp(in.p, "RMP1", 4))) return false;
  uint16_t f = (uint16_t)(in.p[4] | in.p[5] << 8);
  if (!f) return false;
  o->version = o->min_reader = f;   // (its own range: api, compatible_from; the loader checks them)
  o->body = RMP_HEADER_BYTES;
  return true;
}
bool sn_bundle(const Bytes& in, Sniffed* o) {   // the ZIP's first entry is manifest.json (bundles design 3.1, 3.3)
  if (in.n < 30 || memcmp(in.p, "PK\x03\x04", 4)) return false;
  uint32_t nl = (uint32_t)(in.p[26] | in.p[27] << 8), xl = (uint32_t)(in.p[28] | in.p[29] << 8);
  if (nl != 13 || 30 + nl > in.n || memcmp(in.p + 30, "manifest.json", 13)) return false;
  uint32_t d = 30 + nl + xl;
  if (d >= in.n) return false;
  Bytes m = {in.p + d, in.n - d};
  if (!json_header(m, "bundle", o)) return false;
  o->body = d;
  return true;
}

// ---- the library (core/lib, docs/LIBRARY.md): class P for upgrades, class D for corruption (a bad index is rebuilt)
bool sn_library(const Bytes& in, Sniffed* o) { return bin_header(in, "SLIB", o, nullptr); }
bool rd_library(const Bytes& in, const Sniffed& s, char* why, int cap) {
  const char* w = lib::index_check(in.p, in.n);
  return w ? say(why, cap, w) : true;
}
bool sn_liblog(const Bytes& in, Sniffed* o) { return text_header(in, "library-log", o); }   // a header always
bool liblog_line(const char* l, uint32_t n) {
  if (!n) return true;
  return (l[0] == '+' || l[0] == '-' || l[0] == '>' || l[0] == '#') && printable(l, n);
}
bool rd_liblog(const Bytes& in, const Sniffed& s, char* why, int cap) { return lines(in, s, 600, liblog_line, why, cap); }
bool sn_libstate(const Bytes& in, Sniffed* o) { return kv_header(in, "library-state", o); }   // a header always

// ---- the pool lists (core/lib/pools.h, pools_fmt.h): a header always (new formats at the new place)
bool sn_samplepool(const Bytes& in, Sniffed* o) { return text_header(in, "samplepool", o); }
bool sn_wtpool(const Bytes& in, Sniffed* o) { return text_header(in, "wtpool", o); }
bool sn_instpool(const Bytes& in, Sniffed* o) { return text_header(in, "instpool", o); }
bool sn_poolused(const Bytes& in, Sniffed* o) { return text_header(in, "poolused", o); }
bool rd_list(int list, const Bytes& in, const Sniffed& s, char* why, int cap) {
  int nm = 0, ne = 0;
  struct Count { int* ne; int* nm; } c = {&ne, &nm};
  return pools::text::parse(list, (const char*)in.p + s.body, in.n - s.body,
                            [](void* x, const pools::text::Line& l) {
                              Count* k = (Count*)x;
                              return l.member ? ++*k->nm <= pools::kMembers : ++*k->ne <= pools::kCodes;
                            },
                            &c, why, cap);
}
bool rd_samplepool(const Bytes& in, const Sniffed& s, char* why, int cap) { return rd_list(pools::kSamples, in, s, why, cap); }
bool rd_wtpool(const Bytes& in, const Sniffed& s, char* why, int cap) { return rd_list(pools::kTables, in, s, why, cap); }
bool rd_instpool(const Bytes& in, const Sniffed& s, char* why, int cap) { return rd_list(pools::kInstr, in, s, why, cap); }
bool rd_poolused(const Bytes& in, const Sniffed& s, char* why, int cap) {
  int n = 0;
  return pools::text::parse_used((const char*)in.p + s.body, in.n - s.body,
                                 [](void* x, const pools::text::Used&) { return ++*(int*)x <= 3 * pools::kCodes; }, &n,
                                 why, cap);
}

// ---- CHOP's slice maps (core/chop/slicemap.h): an SFZ; v2 "// sidecar slicemap 2", v1 the sampler's slice maps (no
// header, regions with offset / end), upgraded by the chain
bool sn_slicemap(const Bytes& in, Sniffed* o) {
  uint16_t mr = 0;
  uint32_t body = 0;
  int v = chop::version_of((const char*)in.p, in.n, &mr, &body);
  if (!v) return false;
  o->version = (uint16_t)v, o->min_reader = mr ? mr : (uint16_t)v, o->body = body;
  return true;
}
bool rd_slicemap(const Bytes& in, const Sniffed&, char* why, int cap) {
  static chop::Map m;
  if (memchr(in.p, 0, in.n)) return say(why, cap, "a NUL byte (binary data)");
  return chop::read((const char*)in.p, in.n, &m, why, cap, "");
}
bool up_slicemap_1(const Bytes& in, Buf* out, Ctx*) {
  if (memchr(in.p, 0, in.n)) return false;
  return chop::upgrade_v1((const char*)in.p, in.n, (char*)out->p, out->cap, &out->n);
}
const Upgrade kSlicemapChain[] = {{1, up_slicemap_1}};

constexpr uint32_t K = 1024, M = 1024 * 1024;
const Format kFormats[] = {
    // name, class, shape, current, oldest, native, sniff, read, chain, nchain, min_reader_of, where, max bytes
    {"pool", kProject, kText, 1, 1, 1, sn_pool, rd_pool, nullptr, 0, nullptr, "/.rytm/pools/K.txt", 256 * K},
    {"bankpool", kProject, kText, 1, 1, 1, sn_bankpool, rd_bankpool, nullptr, 0, nullptr, "/.rytm/{granular,wavetable,multisample}/K.pool.txt", 1 * M},
    {"picks", kProject, kText, 1, 1, 1, sn_picks, rd_picks, nullptr, 0, nullptr, "/.rytm/granular/K.txt", 256 * K},
    {"slots", kProject, kText, 1, 1, 1, sn_slots, rd_slots, nullptr, 0, nullptr, "/.rytm/slots/K.txt", 4 * K},
    {"slotnotes", kProject, kText, 1, 1, 1, sn_slotnotes, rd_slotnotes, nullptr, 0, nullptr, "/.rytm/slots/K.inf", 4 * K},
    {"fxslots", kProject, kText, 1, 1, 1, sn_fxslots, rd_fxslots, nullptr, 0, nullptr, "/.rytm/fxslots/K.txt", 4 * K},
    {"juno", kProject, kText, 1, 1, 1, sn_juno, rd_juno, nullptr, 0, nullptr, "/.rytm/juno/K.txt", 64 * K},
    {"fm", kProject, kText, 1, 1, 1, sn_fm, rd_fm, nullptr, 0, nullptr, "/.rytm/fm/K.txt", 64 * K},
    {"lfo", kProject, kText, 1, 1, 1, sn_lfo, rd_kitlines, nullptr, 0, nullptr, "/.rytm/lfo/K.txt, /.rytm/lfo/K/kNNN.txt", 64 * K},
    {"ins", kProject, kText, 1, 1, 1, sn_ins, rd_kitlines, nullptr, 0, nullptr, "/.rytm/ins/K.txt, /.rytm/ins/K/kNNN.txt", 64 * K},
    {"kits", kProject, kText, 1, 1, 1, sn_kits, rd_kitlines, nullptr, 0, nullptr, "/.rytm/kits/K/kNNN.sks, /.rytm/kits/K/kNNN.prv", 64 * K},
    {"sqp", kProject, kBinary, 4, 1, 4, sn_sqp, rd_sqp, kSqpChain, 3, mr_sqp, "/.rytm/seq/K/pNNN.sqp", 1 * M},
    {"tracks", kProject, kKeyValue, 1, 1, 1, sn_tracks, rd_kv, nullptr, 0, nullptr, "/.rytm/seq/K/tracks.txt", 64 * K},
    {"song", kProject, kJson, 1, 1, 1, sn_song, rd_song, nullptr, 0, nullptr, "/.rytm/songs/K/take-*.json", 64 * M},
    {"display", kSetting, kKeyValue, 1, 1, 1, sn_display, rd_kv, nullptr, 0, nullptr, "/.rytm/display.txt", 16 * K},
    {"keys", kSetting, kKeyValue, 1, 1, 1, sn_keys, rd_kv, nullptr, 0, nullptr, "/.rytm/keys.txt", 16 * K},
    {"fxin", kSetting, kKeyValue, 1, 1, 1, sn_fxin, rd_kv, nullptr, 0, nullptr, "/.rytm/fxin.txt", 16 * K},
    {"sendfx", kSetting, kKeyValue, 1, 1, 1, sn_sendfx, rd_kv, nullptr, 0, nullptr, "/.rytm/sendfx.txt", 16 * K},
    {"wifi", kSetting, kKeyValue, 1, 1, 1, sn_wifi, rd_kv, nullptr, 0, nullptr, "/.rytm/wifi.txt", 16 * K},
    {"project", kSetting, kText, 1, 1, 1, sn_project, rd_project, nullptr, 0, nullptr, "/.rytm/project", 32},
    {"projects", kSetting, kText, 1, 1, 1, sn_projects, rd_projects, nullptr, 0, nullptr, "/.rytm/projects.txt", 1 * M},
    {"sdtest", kSetting, kKeyValue, 1, 1, 1, sn_sdtest, rd_kv, nullptr, 0, nullptr, "/.rytm/sdtest.txt", 16 * K},
    {"cardindex", kDerived, kKeyValue, 4, 4, 4, sn_cardindex, rd_kv, nullptr, 0, nullptr, "/.rytm/env (+ folders.bin, files.bin)", 4 * K},
    {"dx7idx", kDerived, kBinary, 1, 1, 1, sn_dx7idx, nullptr, nullptr, 0, nullptr, "/.rytm/dx7.bin", 64 * M},
    {"rmpa", kExchange, kBinary, RMP_FORMAT, RMP_FORMAT_MIN, RMP_FORMAT_MIN, sn_rmpa, nullptr, nullptr, 0, nullptr, "/Plugins/*.rmp", 0},
    {"bundle", kExchange, kBinary, 1, 1, 1, sn_bundle, nullptr, nullptr, 0, nullptr, "/Bundles/*.sidecar.zip", 0},
    {"library", kProject, kBinary, 1, 1, 1, sn_library, rd_library, nullptr, 0, nullptr, "/.rytm/library/index.bin", 0},
    {"library-log", kProject, kText, 1, 1, 1, sn_liblog, rd_liblog, nullptr, 0, nullptr, "/.rytm/library/log.txt", 0},
    {"library-state", kProject, kKeyValue, 1, 1, 1, sn_libstate, rd_kv, nullptr, 0, nullptr, "/.rytm/library/state.txt", 16 * K},
    {"samplepool", kProject, kText, 1, 1, 1, sn_samplepool, rd_samplepool, nullptr, 0, nullptr, "/.rytm/projects/K/samples.pool.txt", pools::kFileMax},
    {"wtpool", kProject, kText, 1, 1, 1, sn_wtpool, rd_wtpool, nullptr, 0, nullptr, "/.rytm/projects/K/wavetables.pool.txt", pools::kFileMax},
    {"instpool", kProject, kText, 1, 1, 1, sn_instpool, rd_instpool, nullptr, 0, nullptr, "/.rytm/projects/K/instruments.pool.txt", pools::kFileMax},
    {"poolused", kDerived, kText, 1, 1, 1, sn_poolused, rd_poolused, nullptr, 0, nullptr, "/.rytm/projects/K/pool-used.txt", 1 * M},
    {"slicemap", kProject, kText, chop::kVersion, 1, chop::kVersion, sn_slicemap, rd_slicemap, kSlicemapChain, 1, nullptr,
     "/CHOP/K/<name>-<sha8>.chop.sfz (v1: the sampler's /Samples/K/<name>.sfz, -slices.sfz)", 64 * K},
};
}  // namespace

int count() { return (int)(sizeof kFormats / sizeof kFormats[0]); }
const Format* at(int i) { return i >= 0 && i < count() ? &kFormats[i] : nullptr; }

}  // namespace fmt
}  // namespace core
#else
namespace core {
namespace fmt {
int count() { return 0; }
const Format* at(int) { return nullptr; }
}  // namespace fmt
}  // namespace core
#endif
