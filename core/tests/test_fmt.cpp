// core/tests/test_fmt.cpp - the format registry (core/fmt/fmt.h, docs/LIBRARY.md "Formats"; the design: the formats
// spec sections 3 and 7.1): every format of the inventory registered with its version, its path rule as the modules
// name their files today (cross-checked against the bundle's project-file table), the golden files (core/tests/golden:
// load -> the expected bytes, idempotent, the writers' own round trip where the test can run it), the refusals (newer:
// locked; newer with min_reader <= current: read-only; damaged: locked; never a card write), an upgrade chain in memory,
// the header conventions, the settings' unknown keys carried to the next write (keys, fxin, sendfx, sdtest), the
// atomic write (read back, power cut after every change: the file old or new, never half; in pieces) and a quick
// in-process fuzz of every sniffer and reader (no crash, no card write). The libFuzzer / ASan run: tests/fuzz_fmt.cpp.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bundle/bundle_int.h"
#include "check.h"
#include "fmt/fmt.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "keys.h"
#include "msamp/cardq.h"
#include "msamp/sdtest.h"
#include "platform.h"
#include "seq/rng.h"
#include "seq/seq.h"

using namespace core;

static std::string g_card;
static std::string golden_dir() {
  std::string f = __FILE__;
  return f.substr(0, f.rfind('/')) + "/golden";
}
static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return v;
  uint8_t b[4096];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}
static std::string rd(const std::string& p) {
  std::vector<uint8_t> v = slurp(g_card + p);
  return std::string(v.begin(), v.end());
}
static void put(const std::string& p, const std::string& s) {
  CHECK(system(("mkdir -p \"$(dirname '" + g_card + p + "')\"").c_str()) == 0);
  FILE* f = fopen((g_card + p).c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static bool exists(const std::string& p) {
  struct stat st;
  return stat((g_card + p).c_str(), &st) == 0;
}
static std::vector<std::string> list(const std::string& dir) {
  std::vector<std::string> v;
  DIR* d = opendir(dir.c_str());
  if (!d) return v;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') v.push_back(e->d_name);
  closedir(d);
  return v;
}
static fmt::Status ld(const char* name, const std::string& s, fmt::Info* i, std::string* out = nullptr) {
  const fmt::Format* f = fmt::find(name);
  CHECK(f);
  static uint8_t buf[2 << 20];
  fmt::Buf b = {buf, 0, sizeof buf};
  fmt::Bytes in = {(const uint8_t*)s.data(), (uint32_t)s.size()};
  fmt::Status st = fmt::load(*f, in, &b, i);
  if (out) out->assign((const char*)buf, b.n);
  return st;
}

// ---- an upgrade chain (a test format: v1 "a=1" lines -> v2 with a header -> v3 upper-cased keys), pure
static bool up12(const fmt::Bytes& in, fmt::Buf* out, fmt::Ctx*) {
  int n = fmt::put_text_header((char*)out->p, (int)out->cap, "demo", 2, 2);
  if (n + in.n > out->cap) return false;
  memcpy(out->p + n, in.p, in.n);
  out->n = n + in.n;
  return true;
}
static bool up23(const fmt::Bytes& in, fmt::Buf* out, fmt::Ctx*) {
  fmt::Sniffed s;
  if (!fmt::text_header(in, "demo", &s) || s.version != 2) return false;
  int n = fmt::put_text_header((char*)out->p, (int)out->cap, "demo", 3, 2);
  for (uint32_t i = s.body; i < in.n; ++i) out->p[n++] = (uint8_t)(in.p[i] >= 'a' && in.p[i] <= 'z' ? in.p[i] - 32 : in.p[i]);
  out->n = (uint32_t)n;
  return true;
}
static bool demo_sniff(const fmt::Bytes& in, fmt::Sniffed* o) {
  if (fmt::text_header(in, "demo", o)) return true;
  o->version = o->min_reader = 1, o->body = 0;
  return true;
}
static bool demo_read(const fmt::Bytes& in, const fmt::Sniffed& s, char* why, int cap) {
  return s.version == 3 || (snprintf(why, cap, "not v3"), false);
}
static const fmt::Upgrade kDemoChain[] = {{1, up12}, {2, up23}};

int main() {
  char tmpl[] = "/tmp/fmt-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  host_set_card(g_card.c_str());
  card_ok = true;

  // ---- the inventory (spec section 1): every format registered, at today's version
  struct Want { const char* name; int cls; int current; };
  const Want inv[] = {{"pool", fmt::kProject, 1}, {"bankpool", fmt::kProject, 1}, {"picks", fmt::kProject, 1},
                      {"slots", fmt::kProject, 1}, {"slotnotes", fmt::kProject, 1}, {"fxslots", fmt::kProject, 1},
                      {"juno", fmt::kProject, 1}, {"fm", fmt::kProject, 1}, {"lfo", fmt::kProject, 1},
                      {"ins", fmt::kProject, 1}, {"sqp", fmt::kProject, 4}, {"tracks", fmt::kProject, 1},
                      {"song", fmt::kProject, 1}, {"display", fmt::kSetting, 1}, {"keys", fmt::kSetting, 1},
                      {"fxin", fmt::kSetting, 1}, {"sendfx", fmt::kSetting, 1}, {"wifi", fmt::kSetting, 1},
                      {"project", fmt::kSetting, 1}, {"projects", fmt::kSetting, 1}, {"sdtest", fmt::kSetting, 1},
                      {"cardindex", fmt::kDerived, 4}, {"dx7idx", fmt::kDerived, 1}, {"rmpa", fmt::kExchange, 4},
                      {"bundle", fmt::kExchange, 1}, {"library", fmt::kProject, 1}, {"library-log", fmt::kProject, 1},
                      {"library-state", fmt::kProject, 1}, {"samplepool", fmt::kProject, 1},
                      {"wtpool", fmt::kProject, 1}, {"instpool", fmt::kProject, 1}, {"poolused", fmt::kDerived, 1},
                      {"slicemap", fmt::kProject, 2}, {"kits", fmt::kProject, 1}};
  for (const Want& w : inv) {
    const fmt::Format* f = fmt::find(w.name);
    CHECK(f && f->cls == w.cls && f->current == w.current && f->sniff);
    CHECK(f->oldest <= f->native && f->native <= f->current);
  }
  CHECK(fmt::count() == (int)(sizeof inv / sizeof inv[0]));
  CHECK(!fmt::find("nope") && !fmt::find(nullptr));
  // seqx (E2): v4 natively, v1..v3 through the chain 1 -> 2 -> 3 -> 4 (seq::unpack still reads 1..4 itself, the card's path)
  CHECK(fmt::find("sqp")->native == 4 && fmt::find("sqp")->nchain == 3 && fmt::find("sqp")->oldest == 1);

  // ---- paths: generation 1 = where the modules write today, generation 2 = the project's own folder (the bundle's
  // table names the same files)
  char p[200];
  for (int i = 0; i < bundle::kNumProjectFiles; ++i) {
    const bundle::ProjectFile& r = bundle::kProjectFiles[i];
    char want[200];
    snprintf(want, sizeof want, r.card, "LIVE");
    const char* fm = r.fmt[0] ? r.fmt : "sqp";
    int kit = r.how == bundle::kDir ? 128 : -1;
    if (!strcmp(fm, "pool")) {   // the bank pools: their machine; the DSP pool: "pool"
      if (strstr(r.card, "granular")) fm = "bankpool", kit = 0;
      else if (strstr(r.card, "wavetable")) fm = "bankpool", kit = 1;
      else if (strstr(r.card, "multisample")) fm = "bankpool", kit = 2;
    }
    CHECK(fmt::path(fm, "LIVE", kit, p, sizeof p));
    if (strcmp(p, want)) fprintf(stderr, "path %s: %s, the bundle table %s\n", fm, p, want);
    CHECK(!strcmp(p, want));
    // generation 2 (the pools design 6): the lists and the used set are exported; the old pools / picks are not
    bool gen2 = !strcmp(fm, "samplepool") || !strcmp(fm, "wtpool") || !strcmp(fm, "instpool") || !strcmp(fm, "poolused");
    bool gen1_pool = !strcmp(r.fmt, "pool") || !strcmp(r.fmt, "picks");
    CHECK(gen2 ? r.kind == bundle::kKindPool && !r.old : gen1_pool ? r.old : !r.old);
  }
  for (const char* f : {"samplepool", "wtpool", "instpool", "poolused"}) {   // each in the table once
    int n = 0;
    for (int i = 0; i < bundle::kNumProjectFiles; ++i) n += !strcmp(bundle::kProjectFiles[i].fmt, f);
    CHECK(n == 1);
  }
  CHECK(!strcmp(fmt::path("lfo", "LIVE", 0, p, sizeof p), "/.rytm/lfo/LIVE/k001.txt"));
  CHECK(!strcmp(fmt::path("ins", "LIVE", 127, p, sizeof p), "/.rytm/ins/LIVE/k128.txt"));
  CHECK(!strcmp(fmt::path("sqp", "LIVE", 5, p, sizeof p), "/.rytm/seq/LIVE/p005.sqp"));
  CHECK(!strcmp(fmt::path("keys", nullptr, 0, p, sizeof p), "/.rytm/keys.txt"));
  CHECK(!strcmp(fmt::path("library", nullptr, 0, p, sizeof p), "/.rytm/library/index.bin"));
  // the pools (generation 2: the project's own folder)
  CHECK(!strcmp(fmt::path("samplepool", "LIVE", 0, p, sizeof p), "/.rytm/projects/LIVE/samples.pool.txt"));
  CHECK(!strcmp(fmt::path("wtpool", "LIVE", 0, p, sizeof p), "/.rytm/projects/LIVE/wavetables.pool.txt"));
  CHECK(!strcmp(fmt::path("instpool", "LIVE", 0, p, sizeof p), "/.rytm/projects/LIVE/instruments.pool.txt"));
  CHECK(!strcmp(fmt::path("poolused", "LIVE", 0, p, sizeof p), "/.rytm/projects/LIVE/pool-used.txt"));
  CHECK(!fmt::path("samplepool", "", 0, p, sizeof p));
  CHECK(!fmt::path("pool", "", 0, p, sizeof p) && !fmt::path("pool", "A/B", 0, p, sizeof p));
  CHECK(!fmt::path("bankpool", "LIVE", 3, p, sizeof p) && !fmt::path("pool", "LIVE", 0, p, 8));
  for (int i = 0; i < fmt::count(); ++i) {   // every registered format has a place
    const fmt::Format* f = fmt::at(i);
    CHECK(fmt::path(f->name, "LIVE", f == fmt::find("bankpool") ? 0 : 3, p, sizeof p) && p[0] == '/');
    CHECK(f->where && f->where[0] == '/');
  }

  // ---- golden files: every registered format has some; load -> kOk (the version the folder says), out == expected,
  // idempotent; nothing written on the card
  uint64_t ch0 = host_changes();
  int ngold = 0;
  for (int i = 0; i < fmt::count(); ++i) {
    const fmt::Format* f = fmt::at(i);
    std::string fd = golden_dir() + "/" + f->name;
    std::vector<std::string> vers = list(fd);
    bool any = false;
    for (const std::string& v : vers) {
      if (v[0] != 'v') continue;
      int ver = atoi(v.c_str() + 1);
      for (const std::string& nm : list(fd + "/" + v)) {
        std::vector<uint8_t> in = slurp(fd + "/" + v + "/" + nm);
        std::string s(in.begin(), in.end()), out;
        fmt::Info info;
        fmt::Status st = ld(f->name, s, &info, &out);
        if (st != fmt::kOk && st != fmt::kUpgraded)
          fprintf(stderr, "golden %s/%s/%s: %s (%s)\n", f->name, v.c_str(), nm.c_str(), fmt::status_name(st), info.why);
        CHECK(st == fmt::kOk || st == fmt::kUpgraded);
        CHECK(info.version == ver && !info.locked);
        std::vector<uint8_t> ex = slurp(fd + "/expected/" + nm + ".current");
        std::string want = ex.empty() ? s : std::string(ex.begin(), ex.end());
        CHECK(out == want);
        std::string again;
        CHECK(ld(f->name, out, &info, &again) == fmt::kOk && again == out);   // idempotent
        ++ngold;
        any = true;
      }
    }
    if (!any) fprintf(stderr, "no golden file for %s\n", f->name);
    CHECK(any);
  }
  CHECK(host_changes() == ch0);
  printf("test_fmt: %d formats, %d golden files\n", fmt::count(), ngold);
  {   // the writer's own round trip for .sqp: unpack -> pack is the writer's bytes (v1 -> v2: it writes the lowest
      // version that holds the data, spec 2.4)
    std::vector<uint8_t> v1 = slurp(golden_dir() + "/sqp/v1/p000.sqp"), v2 = slurp(golden_dir() + "/sqp/v2/p000.sqp"),
                         v3 = slurp(golden_dir() + "/sqp/v3/p001.sqp");
    static seq::Pattern pt;
    static uint8_t out[1 << 16];
    CHECK(seq::unpack(pt, 0, v1.data(), (int)v1.size()));
    int n = seq::pack(pt, out, sizeof out);
    CHECK(n == (int)v2.size() && !memcmp(out, v2.data(), n));
    CHECK(seq::unpack(pt, 1, v3.data(), (int)v3.size()));
    n = seq::pack(pt, out, sizeof out);
    CHECK(n == (int)v3.size() && !memcmp(out, v3.data(), n));
    CHECK(fmt::find("sqp")->min_reader_of(fmt::Bytes{out, (uint32_t)n}) == 3);
    // seqx (E2): a v4 file - its fixed seed, a track's seed and poly config, and a block this build does not know -
    // unpack -> pack gives the same bytes (the unknown block kept verbatim)
    std::vector<uint8_t> v4 = slurp(golden_dir() + "/sqp/v4/p002.sqp");
    CHECK(seq::unpack(pt, 2, v4.data(), (int)v4.size()) && pt.seed_fixed && pt.seed == 41 && pt.nkeep == 8);
    const seq::Track* t4 = seq::track_of(pt, 3);
    CHECK(t4 && t4->poly.voices == 4 && t4->poly.alloc == seq::kAllocRotate && t4->poly.unison == 2 &&
          t4->poly.detune == 30 && t4->poly.spread == 64 && t4->seed_own && t4->seed == 77 && t4->seed_same);
    n = seq::pack(pt, out, sizeof out);
    CHECK(n == (int)v4.size() && !memcmp(out, v4.data(), n) && seq::pack_dropped() == 0);
    // seqtop: the top-8 features' blocks (sim/protocol.py's writer) - read, and written back byte for byte
    std::vector<uint8_t> v5 = slurp(golden_dir() + "/sqp/v4/p003.sqp");
    CHECK(seq::unpack(pt, 3, v5.data(), (int)v5.size()) && pt.nkeep == 0);
    CHECK(pt.key == 2 && pt.scale == seq::kScaleDor && pt.qnt == seq::kQntPlay);
    CHECK(seq::track_of(pt, 1)->rnd_mode == seq::kRndSteps && seq::track_of(pt, 1)->rnd_low == 50 &&
          (seq::track_of(pt, 1)->step[3].xf & seq::kXfRnd) && !seq::track_of(pt, 1)->step[0].xf &&
          seq::track_of(pt, 1)->smod_src == 9 && seq::track_of(pt, 1)->smod_quant == seq::kSqBar);
    CHECK(seq::track_of(pt, 1)->step[0].nl == 2 && seq::track_of(pt, 1)->step[0].lock[0].cond == 11 && !seq::track_of(pt, 1)->step[0].lock[1].cond);
    CHECK(seq::track_of(pt, 1)->step[3].rt.cnt == 3 && seq::track_of(pt, 1)->step[3].rt.pcrv == -2 &&
          seq::track_of(pt, 1)->step[5].rt.mode == seq::kRtUpDn && seq::track_of(pt, 1)->step[5].rt.oct == 2);
    CHECK(seq::track_of(pt, 1)->step[0].slide == 1u << 5);
    CHECK((int8_t)seq::track_of(pt, 1)->step[5].fx[0].val == -1 && seq::track_of(pt, 1)->step[5].fx[1].cmd == seq::kFxChn &&
          seq::track_of(pt, 1)->step[9].fx[0].cmd == seq::kFxJmp && !seq::track_of(pt, 1)->step[9].n);
    const seq::Track* t5 = seq::track_of(pt, 1);
    CHECK(t5 && t5->speed == seq::kSpd2_3 && t5->dir == seq::kDirPend && t5->length == 12 && t5->step[3].note[0].micro == 6);
    n = seq::pack(pt, out, sizeof out);
    CHECK(n == (int)v5.size() && !memcmp(out, v5.data(), n));
    // an older Sidecar (its registry entry: sqp v3, read natively from v1) locks a v4 pattern: never played without
    // its blocks, never saved over
    fmt::Format old = *fmt::find("sqp");
    old.current = 3, old.native = 1, old.chain = nullptr, old.nchain = 0;
    fmt::Info oi;
    CHECK(fmt::load(old, fmt::Bytes{v4.data(), (uint32_t)v4.size()}, nullptr, &oi) == fmt::kNewer && oi.locked &&
          oi.version == 4);
    // the v3 -> v4 upgrade alone: the same steps, version 4, and this build's reader takes it as it is
    static uint8_t up[1 << 16];
    fmt::Buf ub = {up, 0, sizeof up};
    CHECK(fmt::load(*fmt::find("sqp"), fmt::Bytes{v3.data(), (uint32_t)v3.size()}, &ub, &oi) == fmt::kUpgraded &&
          oi.version == 3 && ub.n == v3.size() && up[4] == 4 && !memcmp(up + 5, v3.data() + 5, v3.size() - 5));
  }

  // ---- refusals (spec 3.3)
  fmt::Info info;
  std::string out;
  CHECK(ld("pool", "#sidecar pool 2\nA001 sample - 0 /Samples/x.wav\n", &info) == fmt::kNewer && info.locked && !info.read_only);
  CHECK(info.version == 2);
  CHECK(ld("pool", "#sidecar pool 2 min=1\nLive/s0.wav\n", &info, &out) == fmt::kForeignNewer && info.locked && info.read_only);
  CHECK(out == "#sidecar pool 2 min=1\nLive/s0.wav\n");
  CHECK(ld("pool", "#sidecar bankpool 1\n", &info) == fmt::kUnreadable && info.locked);   // another format's header
  CHECK(ld("pool", std::string("Live/s0.wav\n\0x\n", 15), &info) == fmt::kUnreadable && strstr(info.why, "NUL"));
  CHECK(ld("pool", std::string(170, 'a') + "\n", &info) == fmt::kUnreadable && strstr(info.why, "long"));   // pool_load locks it too
  // the pool lists (core/lib/pools_fmt.h): a header always; newer -> locked; lines it does not read -> locked
  CHECK(ld("samplepool", "A010 sample - 10 /Samples/x.wav\n", &info) == fmt::kUnreadable && info.locked);   // no header
  CHECK(ld("samplepool", "#sidecar samplepool 2\nA010 frob x\n", &info) == fmt::kNewer && info.locked);
  CHECK(ld("samplepool", "#sidecar samplepool 2 min=1\nA010 sample - 10 /Samples/x.wav\n", &info) == fmt::kForeignNewer &&
        info.read_only);
  CHECK(ld("samplepool", "#sidecar samplepool 1\nA010 sample - 10 /Samples/x.wav\tkick\t480 1 48000\nH001 input IN\n"
                         "H011 builtin PAD\n", &info) == fmt::kOk);
  CHECK(ld("samplepool", "#sidecar samplepool 1\nA000 sample - 10 /Samples/x.wav\n", &info) == fmt::kUnreadable &&
        strstr(info.why, "OFF"));
  CHECK(ld("samplepool", "#sidecar samplepool 1\nA002 input IN\nA001 input MAIN\n", &info) == fmt::kUnreadable);   // order
  CHECK(ld("samplepool", "#sidecar samplepool 1\nA001 input NOPE\n", &info) == fmt::kUnreadable);
  CHECK(ld("samplepool", "#sidecar samplepool 1\nI001 input IN\n", &info) == fmt::kUnreadable);
  CHECK(ld("samplepool", "#sidecar samplepool 1\nA001 instrument - 10 /M/p.sfz\n", &info) == fmt::kUnreadable);
  CHECK(ld("wtpool", "#sidecar wtpool 1\nA001 input IN\n", &info) == fmt::kUnreadable);   // no inputs in wavetables
  CHECK(ld("wtpool", "#sidecar wtpool 1\nA001 builtin BASIC\nA002 builtin PAD\n", &info) == fmt::kUnreadable);
  CHECK(ld("instpool", "#sidecar instpool 1\n m - 10 /M/a.wav\n", &info) == fmt::kUnreadable);   // a member first
  CHECK(ld("instpool", "#sidecar instpool 1\nB003 instrument - 900 /M/Piano/Piano.sfz\tPiano\n m - 10 /M/a.wav\n", &info) ==
        fmt::kOk);
  CHECK(ld("instpool", "#sidecar instpool 1\nB003 instrument - 900 /M/Piano/Piano.wav\n", &info) == fmt::kUnreadable);
  CHECK(ld("poolused", "#sidecar poolused 1\nsmp A010 3\ninst B003 1\n", &info) == fmt::kOk);
  CHECK(ld("poolused", "#sidecar poolused 1\nfoo A010 3\n", &info) == fmt::kUnreadable);
  CHECK(ld("samplepool", "#sidecar wtpool 1\n", &info) == fmt::kUnreadable);   // another list's header
  CHECK(ld("slots", "U1 juno60 1.2.0\nfuture line\n", &info) == fmt::kUnreadable);   // rmp_slots_parse: all or nothing
  CHECK(ld("song", "{\"format\":\"sidecar-song\",\"version\":2,\"events\":[]}", &info) == fmt::kNewer && info.version == 2);
  CHECK(ld("song", "{\"format\":\"sidecar-other\",\"version\":1}", &info) == fmt::kUnreadable);
  // (found by fuzz_fmt: a number past an int64 was a double -> int64 conversion out of range, undefined) refused
  CHECK(ld("song", "{\"format\":\"sidecar-song\",\"version\":1,\"length\":192000000000000000000000000,\"events\":[]}", &info) == fmt::kUnreadable);
  CHECK(ld("keys", "format=keys/2\nmode=channel\n", &info) == fmt::kNewer);
  {
    std::vector<uint8_t> v = slurp(golden_dir() + "/sqp/v3/p001.sqp");
    std::string s(v.begin(), v.end());
    s[4] = 5;   // a newer pattern file: its version byte is its lowest reader
    CHECK(ld("sqp", s, &info) == fmt::kNewer && info.version == 5 && info.min_reader == 5);
    s[4] = 3;
    s[20] ^= 0x55;   // a damaged body: the CRC
    CHECK(ld("sqp", s, &info) == fmt::kUnreadable && info.locked);
    char line[300];
    fmt::Info ni;
    ld("sqp", std::string(v.begin(), v.end()).replace(4, 1, 1, '\x05'), &ni);
    CHECK(fmt::refusal(*fmt::find("sqp"), ni, "/.rytm/seq/LIVE/p001.sqp", line, sizeof line) > 0);
    CHECK(strstr(line, "newer Sidecar") && strstr(line, "v5") && strstr(line, "up to v4") && strstr(line, "Not loaded, not changed."));
    CHECK(fmt::refusal(*fmt::find("sqp"), info, "/x.sqp", line, sizeof line) > 0 && strstr(line, "not a sqp file"));
  }
  CHECK(ld("cardindex", "hash=1\nversion=3\n", &info) == fmt::kUnreadable);   // class D: an older index is rebuilt, never upgraded
  CHECK(ld("project", std::string(40, 'A'), &info) == fmt::kUnreadable);
  CHECK(ld("library-log", "- /Samples/x.wav\n", &info) == fmt::kUnreadable);   // new formats: a header always
  CHECK(host_changes() == ch0);   // refusing never writes (the 2.1 regression: the registry's side)

  // ---- an upgrade chain in memory: v1 -> v2 -> v3, the reader sees only v3; out has the upgraded bytes
  {
    fmt::Format demo = {"demo", fmt::kProject, fmt::kText, 3, 1, 3, demo_sniff, demo_read, kDemoChain, 2, nullptr, "/demo", 0};
    static uint8_t buf[4096];
    fmt::Buf b = {buf, 0, sizeof buf};
    const char* v1 = "a=1\nb=2\n";
    fmt::Bytes in = {(const uint8_t*)v1, (uint32_t)strlen(v1)};
    CHECK(fmt::load(demo, in, &b, &info) == fmt::kUpgraded && info.version == 1 && !info.locked);
    CHECK(std::string((char*)buf, b.n) == "#sidecar demo 3 min=2\nA=1\nB=2\n");
    fmt::Bytes in3 = {buf, b.n};
    static uint8_t buf2[4096];
    fmt::Buf b2 = {buf2, 0, sizeof buf2};
    CHECK(fmt::load(demo, in3, &b2, &info) == fmt::kOk && b2.n == in3.n);   // idempotent
    fmt::Upgrade only12[] = {{1, up12}};
    demo.chain = only12, demo.nchain = 1;
    CHECK(fmt::load(demo, in, &b, &info) == fmt::kUnreadable && strstr(info.why, "no upgrade"));
    fmt::Buf tiny = {buf, 0, 6};
    demo.chain = kDemoChain, demo.nchain = 2;
    CHECK(fmt::load(demo, in, &tiny, &info) == fmt::kUnreadable);   // no room: refused, never cut
  }

  // ---- header conventions
  {
    char h[64];
    CHECK(fmt::put_text_header(h, sizeof h, "pool", 2, 1) > 0 && !strcmp(h, "#sidecar pool 2 min=1\n"));
    fmt::Sniffed s;
    fmt::Bytes b = {(const uint8_t*)h, (uint32_t)strlen(h)};
    CHECK(fmt::text_header(b, "pool", &s) && s.version == 2 && s.min_reader == 1 && s.body == strlen(h));
    CHECK(!fmt::text_header(b, "poo", &s) && !fmt::text_header(b, "pools", &s));
    CHECK(fmt::put_kv_header(h, sizeof h, "keys", 7) > 0 && !strcmp(h, "format=keys/7\n"));
    b = {(const uint8_t*)h, (uint32_t)strlen(h)};
    CHECK(fmt::kv_header(b, "keys", &s) && s.version == 7);
    const char* j = "{ \"format\": \"sidecar-project\", \"version\": 1, \"min_reader\": 1, \"formats\": {\"pool\": 2} }";
    b = {(const uint8_t*)j, (uint32_t)strlen(j)};
    CHECK(fmt::json_header(b, "project", &s) && s.version == 1 && s.min_reader == 1);
    CHECK(!fmt::json_header(b, "song", &s));
    uint8_t bin[32];
    fmt::put_bin_header(bin, "SLIB", 2, 1, 16, 0xDEADBEEF);
    uint32_t crc;
    b = {bin, sizeof bin};
    CHECK(fmt::bin_header(b, "SLIB", &s, &crc) && s.version == 2 && s.min_reader == 1 && s.body == 16 && crc == 0xDEADBEEF);
    CHECK(!fmt::bin_header(b, "SQP1", &s, &crc));
  }

  // ---- class S keeps what it does not understand (spec 3.4)
  {
    fmt::KvKeep k;
    const char* known[] = {"mode", "auto", nullptr};
    const char* t = "format=keys/1\n# a note\nmode=channel\nfuture=1\r\nauto=3\n\nlater=a=b\n";
    CHECK(fmt::kv_keep(t, (uint32_t)strlen(t), known, &k) == 2 && !strcmp(k.text, "future=1\nlater=a=b\n"));
    char b[64] = "mode=selected\n";
    uint32_t n = fmt::kv_append(k, b, (uint32_t)strlen(b), sizeof b);
    CHECK(std::string(b, n) == "mode=selected\nfuture=1\nlater=a=b\n");
    CHECK(fmt::kv_append(k, b, 60, sizeof b) == 60);   // no room: nothing (never half a line)
    // the settings writers: a newer build's keys survive this build's load + save
    put("/.rytm/keys.txt", "mode=channel\nauto=5\nmac=1\nfuture_keys=on\n");
    keys::load();
    char ko[160];
    CHECK(keys::command("keys=auto:6", ko, sizeof ko));
    CHECK(keys::save());
    CHECK(rd("/.rytm/keys.txt").find("future_keys=on\n") != std::string::npos && rd("/.rytm/keys.txt").find("auto=6\n") != std::string::npos);
    put("/.rytm/fxin.txt", "src=fx\nfuture_in=2\n");
    fxin::load();
    fxin::set(false);
    CHECK(fxin::save());
    CHECK(rd("/.rytm/fxin.txt") == "src=in\nfuture_in=2\n");
    put("/.rytm/sendfx.txt", "out=fx\nfuture_route=x\n");
    fx::card_load();
    fx::card_save();
    CHECK(rd("/.rytm/sendfx.txt") == "out=fx\nfuture_route=x\n");
  }

  // ---- write_atomic: read back, the three renames; a power cut after every change leaves the file old or new
  {
    CHECK(fmt::write_atomic("/.rytm/demo/a.txt", "one\n", 4) && rd("/.rytm/demo/a.txt") == "one\n");
    CHECK(!exists("/.rytm/demo/a.txt.tmp") && !exists("/.rytm/demo/a.txt.prev"));
    CHECK(fmt::write_atomic("/.rytm/demo/a.txt", "two two\n", 8) && rd("/.rytm/demo/a.txt") == "two two\n");
    int cuts = 0;
    for (int k = 0; k < 40; ++k) {
      put("/.rytm/demo/b.txt", "old\n");
      host_power_cut(k);
      bool ok = fmt::write_atomic("/.rytm/demo/b.txt", "new contents\n", 13);
      host_power_cut(-1);   // the reboot
      fmt::recover("/.rytm/demo/b.txt");
      std::string s = rd("/.rytm/demo/b.txt");
      CHECK(s == "old\n" || s == "new contents\n");
      CHECK(!ok || s == "new contents\n");
      CHECK(!exists("/.rytm/demo/b.txt.tmp") && !exists("/.rytm/demo/b.txt.prev"));
      if (!ok) ++cuts;
      if (ok) break;
    }
    CHECK(cuts > 3);
    // in pieces: each step at most its piece of card I/O
    std::string big(300000, 'x');
    for (size_t i = 0; i < big.size(); ++i) big[i] = (char)('a' + i % 23);
    fmt::AtomicWriter w;
    w.begin("/.rytm/demo/big.bin", (const uint8_t*)big.data(), (uint32_t)big.size(), 65536);
    int steps = 0;
    while (w.step()) {
      CHECK(w.bytes_io() <= 65536);
      ++steps;
    }
    CHECK(w.ok() && rd("/.rytm/demo/big.bin") == big && steps >= 10);
    w.begin("/.rytm/demo/big.bin", (const uint8_t*)"short", 5);
    CHECK(w.step() && w.step());
    w.cancel();   // stopped half way: the old file stays, no .tmp
    CHECK(rd("/.rytm/demo/big.bin") == big && !exists("/.rytm/demo/big.bin.tmp"));
    // through the card queue from the control core (no card core in this test: at once)
    cardq::Req rq;
    memset(&rq, 0, sizeof rq);
    File f;
    static uint8_t q[] = "queued\n";
    CHECK(cardq::async(&rq, cardq::kAtomicReq, &f, "/.rytm/demo/q.txt", 0, q, 7) && rq.got == 7);
    CHECK(rd("/.rytm/demo/q.txt") == "queued\n");
  }

  // ---- a quick fuzz (the full one: tests/fuzz_fmt.cpp under libFuzzer / ASan): every format's sniffer and reader on
  // mutated golden files and random bytes: no crash, never a card write
  {
    uint64_t c0 = host_changes();
    uint32_t seed = 12345;
    auto rnd = [&]() { return seed = seed * 1103515245u + 12345u, seed >> 8; };
    std::vector<std::string> seeds;
    for (int i = 0; i < fmt::count(); ++i)
      for (const std::string& v : list(golden_dir() + "/" + fmt::at(i)->name))
        for (const std::string& nm : list(golden_dir() + "/" + fmt::at(i)->name + "/" + v)) {
          std::vector<uint8_t> b = slurp(golden_dir() + "/" + fmt::at(i)->name + "/" + v + "/" + nm);
          seeds.push_back(std::string(b.begin(), b.end()));
        }
    int runs = 0;
    for (int it = 0; it < 4000; ++it) {
      std::string s = seeds[rnd() % seeds.size()];
      int m = (int)(rnd() % 6);
      if (m == 0 && !s.empty()) {
        s.resize(rnd() % s.size());   // cut short
      } else if (m == 1) {
        for (int k = 0; k < 4 && !s.empty(); ++k) s[rnd() % s.size()] = (char)rnd();   // bytes changed
      } else if (m == 2) {
        s.insert(s.empty() ? 0 : rnd() % s.size(), 1, '\0');   // a NUL
      } else if (m == 3) {
        for (char& c : s)
          if (c == '\n' && rnd() % 2) c = '\r';   // CR / LF mixed
      } else if (m == 4) {
        s = "";
        for (int k = (int)(rnd() % 200); k > 0; --k) s += (char)rnd();   // junk
      } else if (m == 5 && s.size() > 8) {
        s[4] = (char)0xFF, s[5] = (char)0xFF;   // huge counts / versions
      }
      for (int i = 0; i < fmt::count(); ++i) {
        fmt::Info fi;
        ld(fmt::at(i)->name, s, &fi);
        ++runs;
      }
    }
    CHECK(host_changes() == c0);
    printf("test_fmt: fuzz: %d loads, no crash, no card write\n", runs);
  }
  // ---- LineReader (2026-10-26): the per-kit LFO / insert files' lines through a read-ahead buffer must be exactly the
  // byte-at-a-time reader's (its rules: \r dropped, a line cut at max - 1, a last line without \n still a line, -1 only
  // at the end), across the buffer's refills
  {
    auto bytewise = [](File& f, char* out, int max) {   // the old line(): one File::read() a byte
      int n = 0, c;
      bool any = false;
      while ((c = f.read()) >= 0) {
        any = true;
        if (c == '\n') break;
        if (c != '\r' && n < max - 1) out[n++] = (char)c;
      }
      out[n] = 0;
      return any ? n : -1;
    };
    std::string longline(700, 'x');
    std::string mid(250, 'm');   // ends near the 256-byte refill, so the next line's \r\n straddles it
    const std::string cases[] = {
        "", "\n", "a", "a\n", "a\r\nb\r\n", "\n\n\nlast", "# head\r\n1 VOICE 1 2 3\r\n\r\n2 FX 4 5 6",
        mid + "\r\nnext\r\n" + longline + "\nshort\n" + longline,
        std::string("\0led\nok\n", 8),   // a NUL-led line: its count, not 0 (0.3.0: never taken for a blank line)
    };
    int ncase = 0, nlines = 0;
    for (const std::string& c : cases)
      for (int max : {2, 8, 96, 1024}) {
        std::string fn = g_card + "/lines.txt";
        FILE* w = fopen(fn.c_str(), "wb");
        CHECK(w);
        fwrite(c.data(), 1, c.size(), w);
        fclose(w);
        File a, b;
        CHECK(a.open("/lines.txt", kRead) && b.open("/lines.txt", kRead));
        fmt::LineReader rd(b);
        char x[1024], y[1024], z[1024];
        uint32_t at = 0;   // fmt::next_line (2026-10-26, the RAM store): the same lines from the file's bytes in RAM
        for (;;) {
          int ka = bytewise(a, x, max), kb = rd.line(y, max);
          int kc = fmt::next_line((const uint8_t*)c.data(), (uint32_t)c.size(), &at, z, max);
          CHECK(ka == kb && !strcmp(x, y));
          CHECK(ka == kc && (ka < 0 || !memcmp(x, z, (size_t)ka + 1)));
          if (ka < 0) break;
          ++nlines;
        }
        CHECK(at == c.size());
        a.close(), b.close();
        ++ncase;
      }
    printf("test_fmt: LineReader = next_line = the byte reader on %d files (%d lines)\n", ncase, nlines);
  }
  // ---- post_queue (2026-10-26, the RAM store): a kit change's save with the post slot busy is queued (a copy), never
  // waited for; a newer text of a queued file replaces it; post_begin is null while one waits (so no newer save of that
  // file lands before it); post_step posts the next when the slot frees; post_wait(path) writes up to that path's
  {
    auto card_turn = [] {
      host_card_core(0);
      while (cardq::service()) {
      }
      host_card_core(3);
    };
    cardq::reset();
    cardq::set_worker(true);
    fmt::reset_for_tests();
    char* t = fmt::post_begin();
    CHECK(t);
    memcpy(t, "A1\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3) && fmt::post_busy());
    CHECK(fmt::post_queue("/pq/b.txt", "B1\n", 3) && fmt::post_queue("/pq/c.txt", "C1\n", 3));
    CHECK(fmt::post_queue("/pq/b.txt", "B2\n", 3));   // newer: in its place
    CHECK(!fmt::post_begin() && fmt::post_busy());
    char big[8] = "x";
    CHECK(!fmt::post_queue("pq/rel.txt", big, 1) && !fmt::post_queue("/pq/big.txt", big, fmt::kQueueMax + 1));
    CHECK(!exists("/pq/a.txt") && !exists("/pq/b.txt"));   // nothing on the card before the card core's turn
    for (int i = 0; i < 16 && fmt::post_busy(); ++i) card_turn(), fmt::post_step();
    CHECK(!fmt::post_busy() && rd("/pq/a.txt") == "A1\n" && rd("/pq/b.txt") == "B2\n" && rd("/pq/c.txt") == "C1\n");
    CHECK(fmt::post_begin());
    // post_wait(path): the queue in its order up to that file (the rest left queued); no path: all
    t = fmt::post_begin();
    memcpy(t, "A2\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    CHECK(fmt::post_queue("/pq/b.txt", "B3\n", 3) && fmt::post_queue("/pq/c.txt", "C3\n", 3));
    fmt::post_wait("/pq/b.txt");
    CHECK(rd("/pq/a.txt") == "A2\n" && rd("/pq/b.txt") == "B3\n" && rd("/pq/c.txt") == "C1\n" && fmt::post_busy());
    fmt::post_wait("/pq/other.txt");   // not pending: no wait
    CHECK(rd("/pq/c.txt") == "C1\n");
    fmt::post_wait();
    CHECK(rd("/pq/c.txt") == "C3\n" && !fmt::post_busy());
    // no wait, no cap: 300 files queued behind one held-up write - every post_queue returns at once, the control core
    // charged no card time (a wait would write on it), nothing on the card before the card core's turns, none lost
    host_card_model(true);
    uint64_t ctl = host_card_us(3);
    t = fmt::post_begin();
    memcpy(t, "A4\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    std::vector<std::string> many;
    for (int i = 0; i < 300; ++i) many.push_back("/pq/m/q" + std::to_string(i) + ".txt");
    for (const std::string& p : many) CHECK(fmt::post_queue(p.c_str(), p.c_str(), (uint32_t)p.size()));
    for (const std::string& p : many) CHECK(fmt::post_queue(p.c_str(), p.c_str(), (uint32_t)p.size()));   // the same again: still 300
    CHECK(host_card_us(3) == ctl && !exists("/pq/m/q0.txt"));
    for (int i = 0; i < 700 && fmt::post_busy(); ++i) card_turn(), fmt::post_step();
    CHECK(!fmt::post_busy() && host_card_us(3) == ctl);
    for (const std::string& p : many) CHECK(rd(p) == p);
    host_card_model(false);
    // undo-replay review 2: a result per write (an id from post_queue), not one per path or "the last": two writes that
    // fail in one post_wait each keep their own failure, a good one its success, a replaced one is unknown
    {
      put("/pq/blocker", "a file where a folder should be\n");
      uint32_t ia = 0, ib = 0, ic = 0, id = 0, ix = 0;
      CHECK(fmt::post_queue("/pq/blocker/a.txt", "A\n", 2, &ia) && ia);
      CHECK(fmt::post_queue("/pq/blocker/b.txt", "B\n", 2, &ib) && ib && ib != ia);
      CHECK(fmt::post_queue("/pq/ok/c.txt", "C\n", 2, &ic) && ic);
      CHECK(fmt::post_queue("/pq/ok/d.txt", "D1\n", 3, &ix) && fmt::post_queue("/pq/ok/d.txt", "D2\n", 3, &id) && id != ix);
      CHECK(fmt::post_result(ia) == fmt::kPostPending && fmt::post_result(ic) == fmt::kPostPending);
      fmt::post_wait();   // (all of them in one call)
      CHECK(fmt::post_result(ia) == fmt::kPostFailed && fmt::post_result(ib) == fmt::kPostFailed);   // A not hidden by B
      CHECK(fmt::post_result(ic) == fmt::kPostOk && fmt::post_result(id) == fmt::kPostOk && rd("/pq/ok/d.txt") == "D2\n");
      CHECK(fmt::post_result(ix) == fmt::kPostUnknown && fmt::post_result(0) == fmt::kPostUnknown);   // replaced; none
      // the same path again, now writable: its new id succeeds, the old one's failure stays its own
      CHECK(system(("rm -f '" + g_card + "/pq/blocker'").c_str()) == 0);
      uint32_t ia2 = 0;
      CHECK(fmt::post_queue("/pq/blocker/a.txt", "A\n", 2, &ia2));
      fmt::post_wait();
      CHECK(fmt::post_result(ia2) == fmt::kPostOk && fmt::post_result(ia) == fmt::kPostFailed);
      printf("test_fmt: post_result per write: two failures in one post_wait each kept, a success, a replaced write unknown\n");
      // post_remove (undo-replay re-review): through the same queue, in its turn; a file not there is ok too
      uint32_t ir = 0, ir2 = 0, iw = 0;
      CHECK(fmt::post_queue("/pq/rm/x.txt", "X\n", 2, &iw));
      CHECK(fmt::post_remove("/pq/rm/x.txt", &ir) && ir && fmt::post_remove("/pq/rm/none.txt", &ir2));
      CHECK(fmt::post_result(ir) == fmt::kPostPending);
      fmt::post_wait();
      CHECK(fmt::post_result(ir) == fmt::kPostOk && fmt::post_result(ir2) == fmt::kPostOk && !exists("/pq/rm/x.txt"));
      CHECK(fmt::post_result(iw) == fmt::kPostOk);   // (the write in flight first, then the remove behind it)
      put("/pq/rm/y.txt", "Y\n");
      CHECK(fmt::post_remove("/pq/rm/y.txt", &ir));
      for (int i = 0; i < 8 && fmt::post_busy(); ++i) card_turn(), fmt::post_step();
      CHECK(fmt::post_result(ir) == fmt::kPostOk && !exists("/pq/rm/y.txt"));
      // re-review 2 finding 2: a remove the card refuses (a folder with a file in it), finished by post_wait: failed
      put("/pq/rm/full/a.txt", "A\n");
      CHECK(fmt::post_remove("/pq/rm/full", &ir));
      fmt::post_wait();
      CHECK(fmt::post_result(ir) == fmt::kPostFailed && exists("/pq/rm/full/a.txt"));
      printf("test_fmt: post_remove in the queue's order, a missing file ok\n");
    }
    // a newer text of the file in flight: queued, it lands after the one in flight (never before it)
    t = fmt::post_begin();
    memcpy(t, "A5\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    CHECK(fmt::post_queue("/pq/a.txt", "A6\n", 3));
    card_turn();
    CHECK(rd("/pq/a.txt") == "A5\n");
    for (int i = 0; i < 8 && fmt::post_busy(); ++i) fmt::post_step(), card_turn();
    CHECK(rd("/pq/a.txt") == "A6\n" && !fmt::post_busy());
    // fix round 1: a newer text of a queued file goes to the tail - it lands after the writes queued before it (a
    // RELOAD's .prv before the .sqp that replaces it, when an older .sqp save was queued ahead of both)
    {
      t = fmt::post_begin();
      memcpy(t, "S\n", 2);
      CHECK(fmt::post_end("/pq/o/slot.txt", 2));
      CHECK(fmt::post_queue("/pq/o/p000.sqp", "old\n", 4) && fmt::post_queue("/pq/o/p000.prv", "prv\n", 4));
      CHECK(fmt::post_queue("/pq/o/p000.sqp", "new\n", 4));   // newer: after the .prv now
      int sqp_at = -1, prv_at = -1;
      for (int i = 0; i < 16 && fmt::post_busy(); ++i) {
        card_turn(), fmt::post_step();
        if (prv_at < 0 && exists("/pq/o/p000.prv")) prv_at = i;
        if (sqp_at < 0 && exists("/pq/o/p000.sqp")) sqp_at = i;
      }
      card_turn(), fmt::post_step();
      if (sqp_at < 0 && exists("/pq/o/p000.sqp")) sqp_at = 99;
      CHECK(prv_at >= 0 && sqp_at > prv_at && rd("/pq/o/p000.sqp") == "new\n" && !fmt::post_busy());
      printf("test_fmt: a newer text of a queued file lands after the writes queued before it\n");
    }
    // a text over kPostMax (2026-10-26, the RAM store's phase 2: a sequencer pattern) is queued as its own copy, posted
    // at once when the slot is free (the slot writes from that copy), else in its turn behind the others
    {
      std::string b1(fmt::kPostMax + 1000, 'p'), b2(fmt::kPostMax * 3, 'q');
      b1[0] = '1', b2[0] = '2';
      CHECK(fmt::post_queue("/pq/big1.bin", b1.data(), (uint32_t)b1.size()) && fmt::pending("/pq/big1.bin"));
      CHECK(!exists("/pq/big1.bin"));   // posted: the card core's
      CHECK(fmt::post_queue("/pq/big2.bin", b2.data(), (uint32_t)b2.size()) && fmt::post_queue("/pq/a.txt", "A7\n", 3));
      b1[1] = b2[1] = 'x';   // the caller's buffers changed after: the copies are written
      for (int i = 0; i < 16 && fmt::post_busy(); ++i) card_turn(), fmt::post_step();
      CHECK(!fmt::post_busy() && rd("/pq/a.txt") == "A7\n");
      std::string r1 = rd("/pq/big1.bin"), r2 = rd("/pq/big2.bin");
      CHECK(r1.size() == b1.size() && r1[0] == '1' && r1[1] == 'p' && r2.size() == b2.size() && r2[0] == '2' && r2[1] == 'q');
      printf("test_fmt: post_queue over kPostMax: %u and %u bytes queued and written from their own copies\n",
             (unsigned)b1.size(), (unsigned)b2.size());
    }
    // the order between passes: one write per slot turn - after the first card turn only a is on the card, then a and b,
    // c still queued
    t = fmt::post_begin();
    memcpy(t, "A7\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    CHECK(fmt::post_queue("/pq/b.txt", "B7\n", 3) && fmt::post_queue("/pq/c.txt", "C7\n", 3));
    card_turn();
    CHECK(rd("/pq/a.txt") == "A7\n" && rd("/pq/b.txt") == "B3\n" && rd("/pq/c.txt") == "C3\n");
    CHECK(!fmt::post_step());   // a done here
    CHECK(fmt::post_step());    // b into the slot, posted
    card_turn();
    CHECK(rd("/pq/b.txt") == "B7\n" && rd("/pq/c.txt") == "C3\n" && fmt::post_busy());
    for (int i = 0; i < 8 && fmt::post_busy(); ++i) fmt::post_step(), card_turn();
    CHECK(rd("/pq/c.txt") == "C7\n");
    // no take-back (phase 3, 2026-10-26): a queued write in the slot the card core does not take within
    // cardq::kTakeBackUs stays posted (counted once in post_held), never run here; it lands at the next card turn
    t = fmt::post_begin();
    memcpy(t, "A8\n", 3);
    CHECK(fmt::post_end("/pq/a.txt", 3));
    CHECK(fmt::post_queue("/pq/b.txt", "B8\n", 3));
    card_turn();
    CHECK(!fmt::post_step() && fmt::post_step());   // a done; b posted, the card core not running
    uint64_t now = now_us();
    host_fake_time(true, now + cardq::kTakeBackUs + 1000);
    uint32_t held0 = fmt::post_held(), took0 = cardq::stats().taken_back;
    for (int i = 0; i < 4; ++i) CHECK(fmt::post_step() && fmt::post_busy());   // still posted
    CHECK(rd("/pq/b.txt") == "B7\n" && fmt::post_held() == held0 + 1 && cardq::stats().taken_back == took0);
    card_turn();
    CHECK(!fmt::post_step() && !fmt::post_busy() && rd("/pq/b.txt") == "B8\n");   // written by the card core
    host_fake_time(false, 0);
    cardq::set_worker(false);
    cardq::reset();
    fmt::reset_for_tests();
    // unwritten / the oldest age (/status "store"): one edit counts once at every stage with cardq::pending()
    {
      auto total = [&](uint32_t* o) { return (uint32_t)cardq::pending() + fmt::unwritten(o); };
      uint32_t o = 99;
      CHECK(total(&o) == 0 && o == 0);
      cardq::set_worker(true);
      t = fmt::post_begin();
      memcpy(t, "U1\n", 3);
      CHECK(fmt::post_end("/pq/u.txt", 3));   // posted into the ring
      CHECK(cardq::pending() == 1 && total(&o) == 1);
      CHECK(fmt::post_queue("/pq/u2.txt", "U2\n", 3) && total(&o) == 2);   // queued behind it
      uint64_t t0 = now_us();
      host_fake_time(true, t0 + 5000000);
      CHECK(total(&o) == 2 && o >= 4900 && o <= 5100);
      CHECK(fmt::post_queue("/pq/u2.txt", "U3\n", 3) && total(&o) == 2 && o >= 4900);   // replaced: keeps its age
      card_turn();   // the card core takes the slot's write
      CHECK(total(&o) == 2);
      while (fmt::post_busy()) fmt::post_step(), card_turn();
      CHECK(total(&o) == 0 && o == 0);
      // held: posted, the card core not running
      CHECK(fmt::post_queue("/pq/u4.txt", "U4\n", 3) && fmt::post_step());
      host_fake_time(true, t0 + 6000000);
      CHECK(total(&o) == 1);
      host_fake_time(true, t0 + 6000000 + cardq::kTakeBackUs + 1000);
      CHECK(fmt::post_step() && total(&o) == 1);   // held: still once
      card_turn();
      while (fmt::post_busy()) fmt::post_step(), card_turn();
      CHECK(total(&o) == 0 && o == 0);
      host_fake_time(false, 0);
    }
    printf("test_fmt: post_queue: queued behind the slot, never a wait, newest text wins, in order, none lost\n");
  }
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);   // (the temp card)
  printf("test_fmt: ok\n");
  return 0;
}
