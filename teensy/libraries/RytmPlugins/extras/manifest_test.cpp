// extras/manifest_test.cpp - manifests (formats 1..3) as slot descriptions, on the Mac.
// build: c++ -std=c++17 -O1 -Wall -I../src manifest_test.cpp ../src/rmp_manifest.cpp ../src/rmp_loader.cpp -o /tmp/manifest_test && /tmp/manifest_test
// (run from extras/; argv[1] overrides the path of sine_test.rmp)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_manifest.h"
#include "rmp_format.h"

static std::vector<uint8_t> pkg;   // a fake package: 96 header bytes, an 8-byte image, the manifest
static int reads;
static int fail_at = -1;           // >= 0: the read with this count (0 the first) fails
static bool rd(void *, uint32_t off, uint8_t *buf, uint32_t n) {
  if ((uint64_t)off + n > pkg.size() || (fail_at >= 0 && reads >= fail_at)) return false;
  ++reads; memcpy(buf, pkg.data() + off, n); return true;
}
static void put(std::vector<uint8_t> &v, const char *s, size_t n) {
  for (size_t i = 0; i < n; ++i) v.push_back(i < strlen(s) ? (uint8_t)s[i] : 0);
}
struct K { int p, look, text, first, count, role, lo, hi, def; const char *lng, *sht; };
// A manifest in format fmt with these knobs and names (8 bytes each).
static rmp_info make(int fmt, const char *name, const char *shrt, int page2, const char *title, const char *ext,
                     const std::vector<K> &ks, const std::vector<std::string> &names) {
  std::vector<uint8_t> m;
  put(m, name, 10); put(m, shrt, 4); put(m, "MIT", 48); m.push_back((uint8_t)page2); m.push_back((uint8_t)ks.size());
  if (fmt >= 3) { put(m, title, 13); put(m, ext, 7); }
  for (const K &k : ks) {
    m.push_back((uint8_t)k.p); m.push_back((uint8_t)k.look); m.push_back((uint8_t)k.text);
    if (fmt == 1) m.push_back(0);
    else { m.push_back((uint8_t)k.first); m.push_back((uint8_t)k.count); }
    if (fmt == 2) m.push_back(0);
    if (fmt >= 3) { m.push_back((uint8_t)k.role); m.push_back((uint8_t)k.lo); m.push_back((uint8_t)k.hi); m.push_back((uint8_t)k.def); m.push_back(0); }
    put(m, k.lng, 12); put(m, k.sht, 4);
  }
  if (fmt >= 2) for (const std::string &n : names) put(m, n.c_str(), 8);
  pkg.assign(96 + 8, 0);
  pkg.insert(pkg.end(), m.begin(), m.end());
  rmp_info info; memset(&info, 0, sizeof info);
  info.format = (uint16_t)fmt; info.image_size = 8; info.reloc_count = 0; info.manifest_size = (uint32_t)m.size();
  return info;
}
static bool str_is(const char *f, size_t n, const char *want) {   // NUL-padded field == want
  char b[32] = {0}; memcpy(b, f, n); return !strcmp(b, want);
}

int main(int argc, char **argv) {
  rmp_desc d;
  {   // format 3: everything, knobs out of p order, a preset knob, ranges, roles, names
    rmp_info i = make(3, "NP CLUST1", "NPC1", 1, "NP PAGE 2", "np",
                      {{9, 0, 0, 0, 0, 0, 0, 127, 64, "Gain", "GAN"},
                       {1, 3, 2, 0, 2, 1, 0, 127, 0, "Algorithm", "ALG"},
                       {2, 0, 6, 0, 0, 0, 0, 127, 0, "Preset", "PRE"},
                       {3, 2, 0, 0, 0, 2, 10, 120, 100, "Level", "LEV"},
                       {5, 0, 7, 0, 0, 0, 0, 127, 0, "Sample", "SMP"}},
                      {"PrimeCl", "PrimeCn"});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(str_is(d.name, 10, "NP CLUST1") && str_is(d.short_name, 4, "NPC1") && str_is(d.p2_title, 13, "NP PAGE 2"));
    CHECK(!strcmp(d.preset_ext, "np") && d.page2 == 1 && d.nknobs == 5 && d.format == 3);
    CHECK(d.knob[0].p == 1 && d.knob[1].p == 2 && d.knob[2].p == 3 && d.knob[3].p == 5 && d.knob[4].p == 9);   // p order
    CHECK(d.pre == 1 && d.knob[1].kind == RMP_TEXT_PRESET);
    CHECK(d.knob[0].kind == RMP_TEXT_ENUM && d.knob[0].look == RMP_LOOK_LIST && d.knob[0].role == RMP_ROLE_PRESET);
    CHECK(d.knob[2].min == 10 && d.knob[2].max == 120 && d.knob[2].def == 100 && d.knob[2].role == RMP_ROLE_OFFSET);
    CHECK(d.knob[3].kind == RMP_TEXT_POOL && str_is(d.knob[3].long_name, 12, "Sample") && str_is(d.knob[3].short_name, 4, "SMP"));
    CHECK(rmp_desc_find(&d, 5) == 3 && rmp_desc_find(&d, 4) == -1);
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 0, rd, nullptr, nm) == 2 && !strcmp(nm[0], "PrimeCl") && !strcmp(nm[1], "PrimeCn"));
    CHECK(rmp_manifest_enum(&d, 2, rd, nullptr, nm) == 0);   // no names
  }
  {   // format 2: format 3's defaults, the name table
    rmp_info i = make(2, "SINE TEST", "SINE", 0, "", "", {{1, 3, 2, 0, 3, 0, 0, 0, 0, "Mode", "MOD"}}, {"A", "B", "C"});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(d.nknobs == 1 && d.knob[0].min == 0 && d.knob[0].max == 127 && d.knob[0].def == 64 && d.knob[0].role == 0);
    CHECK(d.p2_title[0] == 0 && d.preset_ext[0] == 0 && d.pre == -1);
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 0, rd, nullptr, nm) == 3 && !strcmp(nm[2], "C"));
  }
  {   // format 1: no names
    rmp_info i = make(1, "OLD", "OLD", 0, "", "", {{2, 1, 1, 0, 0, 0, 0, 0, 0, "Tune", "TUN"}}, {});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(d.nknobs == 1 && d.knob[0].p == 2 && d.knob[0].look == RMP_LOOK_CENTRED && d.knob[0].kind == RMP_TEXT_BIPOLAR);
    CHECK(d.names_at == 0);
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 0, rd, nullptr, nm) == 0);
  }
  {   // skipped knobs: p 0, p 17, a P seen before; unprintable bytes made '_'; a bad range repaired
    rmp_info i = make(3, "BAD\x01NAME", "B\x7f", 0, "T", "n.p",
                      {{0, 0, 0, 0, 0, 0, 0, 127, 64, "Zero", "Z"}, {17, 0, 0, 0, 0, 0, 0, 127, 64, "Big", "B"},
                       {4, 0, 0, 0, 0, 0, 90, 20, 5, "Four", "F"}, {4, 0, 0, 0, 0, 0, 0, 127, 64, "Again", "A"}}, {});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(str_is(d.name, 10, "BAD_NAME") && str_is(d.short_name, 4, "B_"));
    CHECK(d.preset_ext[0] == 0);                                   // "n.p": not letters and digits
    CHECK(d.nknobs == 1 && d.knob[0].p == 4 && str_is(d.knob[0].long_name, 12, "Four"));
    CHECK(d.knob[0].min == 0 && d.knob[0].max == 127 && d.knob[0].def == 64);   // min > max: the full range
  }
  {   // full-width strings: every field NUL-terminated (one spare byte each), even with no padding left
    rmp_info i = make(3, "ABCDEFGHIJ", "WXYZ", 1, "THIRTEENCHARS", "abcdefg",
                      {{1, 0, 2, 0, 1, 0, 0, 127, 64, "TwelveLetter", "FOUR"}}, {"EIGHTCHR"});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(strlen(d.name) == 10 && !strcmp(d.name, "ABCDEFGHIJ"));
    CHECK(strlen(d.short_name) == 4 && !strcmp(d.short_name, "WXYZ"));
    CHECK(strlen(d.p2_title) == 13 && !strcmp(d.p2_title, "THIRTEENCHARS"));
    CHECK(strlen(d.preset_ext) == 7 && !strcmp(d.preset_ext, "abcdefg"));
    CHECK(strlen(d.knob[0].long_name) == 12 && !strcmp(d.knob[0].long_name, "TwelveLetter"));
    CHECK(strlen(d.knob[0].short_name) == 4 && !strcmp(d.knob[0].short_name, "FOUR"));
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 0, rd, nullptr, nm) == 1 && !strcmp(nm[0], "EIGHTCH"));   // at most 7
    rmp_desc_from_id("x.ABCDEFGHIJKLMN", &d);
    CHECK(strlen(d.name) == 10 && !strcmp(d.name, "ABCDEFGHIJ") && strlen(d.short_name) == 4);
  }
  {   // range bytes 128..255 saturate to 127 (not masked)
    rmp_info i = make(3, "SAT", "SAT", 0, "", "",
                      {{1, 0, 0, 0, 0, 0, 100, 200, 255, "Hi", "HI"},      // 100..127, default 127
                       {2, 0, 0, 0, 0, 0, 0, 130, 129, "Top", "TOP"},      // 0..127, default 127 (& 127 would give 0..2, 1)
                       {3, 0, 0, 0, 0, 0, 200, 50, 30, "Bad", "BAD"}},     // 127 > 50: the full range, default 64
                      {});
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(d.knob[0].min == 100 && d.knob[0].max == 127 && d.knob[0].def == 127);
    CHECK(d.knob[1].min == 0 && d.knob[1].max == 127 && d.knob[1].def == 127);
    CHECK(d.knob[2].min == 0 && d.knob[2].max == 127 && d.knob[2].def == 64);
  }
  {   // malformed: the knob count does not fit the manifest; another format; a failing read
    rmp_info i = make(3, "X", "X", 0, "", "", {{1, 0, 0, 0, 0, 0, 0, 127, 64, "A", "A"}}, {});
    rmp_info t = i; t.manifest_size -= 1; CHECK(!rmp_manifest_read(&t, rd, nullptr, &d));
    t = i; t.format = 4; CHECK(rmp_manifest_read(&t, rd, nullptr, &d) && d.format == 4 && d.nknobs == 1);   // 4: as 3 (RLB2)
    t = i; t.format = 5; CHECK(!rmp_manifest_read(&t, rd, nullptr, &d));
    t = i; t.image_size = 100000; CHECK(!rmp_manifest_read(&t, rd, nullptr, &d));
  }
  {   // labels that follow a list knob (RMP_LABELS_*): the block at the manifest's end, the static labels before it
    rmp_info i = make(3, "MD-PI", "MDPI", 1, "", "",
                      {{1, 0, 0, 0, 0, 0, 0, 127, 64, "Pitch", "PTCH"}, {2, 0, 0, 0, 0, 0, 0, 127, 64, "Decay", "DEC"},
                       {9, 3, 2, 0, 3, 0, 0, 127, 0, "Type", "TYPE"}, {10, 2, 0, 0, 0, 0, 0, 127, 100, "Level", "LEV"}},
                      {"BD", "SD", "MA"});
    std::vector<uint8_t> blk = {9, 3, 2, 0, 1, 2};
    const char *lb[3][2][2] = {{{"Pitch", "PTCH"}, {"Decay", "DEC"}}, {{"Snappy", "SNAP"}, {"Tension", "TENS"}},
                               {{"Grains", "GRNS"}, {nullptr, nullptr}}};
    for (auto &row : lb)
      for (auto &e : row) { put(blk, e[0] ? e[0] : "", 12); put(blk, e[1] ? e[1] : "", 4); }
    uint16_t len = (uint16_t)blk.size();
    blk.push_back((uint8_t)len); blk.push_back((uint8_t)(len >> 8));
    put(blk, "RLB1", 4);
    std::vector<uint8_t> plain = pkg;
    pkg.insert(pkg.end(), blk.begin(), blk.end());
    rmp_info li = i; li.manifest_size += (uint32_t)blk.size();
    CHECK(rmp_manifest_read(&li, rd, nullptr, &d));   // the old reading: the same knobs, the names reachable
    CHECK(d.nknobs == 4 && str_is(d.knob[0].long_name, 12, "Pitch"));
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 2, rd, nullptr, nm) == 3 && !strcmp(nm[2], "MA"));
    rmp_labels L;
    reads = 0;
    CHECK(rmp_manifest_labels(&li, rd, nullptr, &d, &L) && reads == 2);   // the footer, then the block whole
    CHECK(L.knob == 9 && L.nsets == 3 && L.nparams == 2 && L.p[0] == 1 && L.p[1] == 2);
    CHECK(rmp_labels_set(&L, 0) == 0 && rmp_labels_set(&L, 42) == 0 && rmp_labels_set(&L, 43) == 1 && rmp_labels_set(&L, 127) == 2);
    CHECK(rmp_labels_set(&L, -5) == 0 && rmp_labels_set(&L, 1e9f) == 2 && rmp_labels_set(&L, __builtin_nanf("")) == 0);
    rmp_desc e = d;
    CHECK(rmp_labels_apply(&L, 1, &e) && str_is(e.knob[0].long_name, 12, "Snappy") && str_is(e.knob[0].short_name, 4, "SNAP"));
    CHECK(str_is(e.knob[1].long_name, 12, "Tension") && str_is(e.knob[3].long_name, 12, "Level"));   // P10 untouched
    CHECK(rmp_labels_apply(&L, 2, &e) && e.knob[1].long_name[0] == 0 && e.knob[1].short_name[0] == 0);   // inert: ""
    CHECK(!rmp_labels_apply(&L, 3, &e) && !rmp_labels_apply(&L, -1, &e));
    // no block, a wrong magic, a length that does not fit, a set count other than the knob's names: none
    pkg = plain;
    CHECK(!rmp_manifest_labels(&i, rd, nullptr, &d, &L) && L.nsets == 0);
    pkg.insert(pkg.end(), blk.begin(), blk.end());
    pkg[pkg.size() - 1] = 'X';
    CHECK(!rmp_manifest_labels(&li, rd, nullptr, &d, &L));
    pkg[pkg.size() - 1] = '1';
    pkg[pkg.size() - 6] = (uint8_t)(len + 1);
    CHECK(!rmp_manifest_labels(&li, rd, nullptr, &d, &L));
    pkg[pkg.size() - 6] = (uint8_t)len;
    pkg[pkg.size() - 6 - len + 1] = 2;
    CHECK(!rmp_manifest_labels(&li, rd, nullptr, &d, &L));
    pkg[pkg.size() - 6 - len + 1] = 3;
    pkg[pkg.size() - 6 - len + 4] = 9;   // a P that is the list knob itself
    CHECK(!rmp_manifest_labels(&li, rd, nullptr, &d, &L));
    pkg[pkg.size() - 6 - len + 4] = 1;
    CHECK(rmp_manifest_labels(&li, rd, nullptr, &d, &L));
  }
  {   // a list whose names follow another list knob (RLB2, MD's ENGINE and TYPE): P9 ENGINE (2), P10 TYPE (static: 2)
    rmp_info i = make(3, "MD", "MD", 1, "", "",
                      {{1, 0, 0, 0, 0, 0, 0, 127, 64, "Pitch", "PTCH"}, {2, 0, 0, 0, 0, 0, 0, 127, 64, "Decay", "DEC"},
                       {9, 3, 2, 0, 2, 0, 0, 127, 0, "Engine", "ENG"}, {10, 3, 2, 2, 2, 0, 0, 127, 0, "Type", "TYPE"},
                       {11, 2, 0, 0, 0, 0, 0, 127, 100, "Level", "LEV"}},
                      {"FM", "PI", "BD", "SD"});
    // knob 10, 5 sets, 1 param, dep 9, 2 values: 2 + 3 names; P1; the names; the labels
    std::vector<uint8_t> blk = {10, 5, 1, 9, 2, 2, 3, 1};
    const char *nms[5] = {"BD", "SD", "BD", "MT", "HHLONG7"};
    for (const char *n : nms) put(blk, n, 8);
    const char *lb[5] = {"Pitch", "Snap", "Tune", "Hammer", nullptr};
    for (const char *e : lb) { put(blk, e ? e : "", 12); put(blk, e ? "LB" : "", 4); }
    uint16_t len = (uint16_t)blk.size();
    blk.push_back((uint8_t)len); blk.push_back((uint8_t)(len >> 8));
    put(blk, "RLB2", 4);
    std::vector<uint8_t> plain = pkg;
    pkg.insert(pkg.end(), blk.begin(), blk.end());
    rmp_info li = i; li.manifest_size += (uint32_t)blk.size();
    CHECK(rmp_manifest_read(&li, rd, nullptr, &d));   // the old reading: TYPE's own (static) names
    char nm[16][8];
    CHECK(rmp_manifest_enum(&d, 3, rd, nullptr, nm) == 2 && !strcmp(nm[1], "SD"));
    rmp_labels L;
    reads = 0;
    CHECK(rmp_manifest_labels(&li, rd, nullptr, &d, &L) && reads == 2);
    CHECK(L.knob == 10 && L.dep == 9 && L.ndep == 2 && L.nsets == 5 && L.count[0] == 2 && L.count[1] == 3 &&
          L.base[1] == 2 && L.nparams == 1 && L.p[0] == 1);
    CHECK(rmp_labels_set(&L, 0) == -1);   // (an RLB2 set needs ENGINE's value)
    float pv[16] = {};
    pv[8] = 0, pv[9] = 127; CHECK(rmp_labels_pick(&L, pv) == 1);    // FM, SD
    pv[8] = 64, pv[9] = 0; CHECK(rmp_labels_pick(&L, pv) == 2);     // PI, BD
    pv[8] = 127, pv[9] = 64; CHECK(rmp_labels_pick(&L, pv) == 3);   // PI, (64 * 3) >> 7 = 1: MT
    pv[8] = 127, pv[9] = 127; CHECK(rmp_labels_pick(&L, pv) == 4);
    pv[8] = __builtin_nanf(""), pv[9] = 1e9f; CHECK(rmp_labels_pick(&L, pv) == 1);
    rmp_desc e = d;
    CHECK(rmp_labels_apply(&L, 3, &e) && str_is(e.knob[0].long_name, 12, "Hammer") && e.knob[3].enum_count == 3);
    CHECK(str_is(e.knob[1].long_name, 12, "Decay") && e.knob[2].enum_count == 2);   // P2 and ENGINE untouched
    CHECK(rmp_labels_apply(&L, 4, &e) && e.knob[0].long_name[0] == 0);              // inert
    CHECK(rmp_labels_apply(&L, 1, &e) && e.knob[3].enum_count == 2);
    CHECK(rmp_labels_enum(&L, 4, 10, nm) == 3 && !strcmp(nm[0], "BD") && !strcmp(nm[1], "MT") && !strcmp(nm[2], "HHLONG7"));
    CHECK(rmp_labels_enum(&L, 0, 10, nm) == 2 && !strcmp(nm[1], "SD") && nm[2][0] == 0);
    CHECK(rmp_labels_enum(&L, 0, 9, nm) == -1 && rmp_labels_enum(&L, -1, 10, nm) == -1 && rmp_labels_enum(&L, 5, 10, nm) == -1);
    rmp_labels L1;   // an RLB1 block has no lists of its own
    memset(&L1, 0, sizeof L1); L1.knob = 9; L1.nsets = 2; L1.nparams = 1; L1.p[0] = 1;
    CHECK(rmp_labels_enum(&L1, 0, 9, nm) == -1);
    pv[8] = 127; CHECK(rmp_labels_pick(&L1, pv) == 1);
    // refused: counts not adding up to nsets, ndep other than ENGINE's names, dep the knob itself, a count of 0, a P
    // that is dep, a dep that is not a list knob; each restored after
    size_t at = pkg.size() - 6 - len;
    auto refused = [&](size_t off, uint8_t v) {
      uint8_t was = pkg[at + off]; pkg[at + off] = v;
      bool r = rmp_manifest_labels(&li, rd, nullptr, &d, &L);
      pkg[at + off] = was;
      return !r && L.nsets == 0;
    };
    CHECK(refused(6, 2));    // 2 + 2 != 5
    CHECK(refused(4, 3));    // ndep 3 (also the length)
    CHECK(refused(3, 10));   // dep = knob
    CHECK(refused(3, 1));    // dep P1: not a list knob
    CHECK(refused(5, 0));    // a count of 0
    CHECK(refused(7, 9));    // a P that is dep
    CHECK(rmp_manifest_labels(&li, rd, nullptr, &d, &L) && L.nsets == 5);
    pkg = plain;
  }
  {   // hostile RLB2 blocks (review finding 9): a consistent block from its counts, then each kind of damage; the
      // reader answers false with out all zero, or a block within its bounds
    std::vector<std::string> nm16;
    auto mk = [&](const std::vector<int> &counts, std::vector<uint8_t> *blk_out, size_t *blk_at) {
      std::vector<std::string> en;
      for (size_t v = 0; v < counts.size(); ++v) en.push_back("E" + std::to_string(v));
      en.push_back("BD"); en.push_back("SD");
      rmp_info i = make(3, "MD", "MD", 1, "", "",
                        {{1, 0, 0, 0, 0, 0, 0, 127, 64, "Pitch", "PTCH"},
                         {9, 3, 2, 0, (int)counts.size(), 0, 0, 127, 0, "Engine", "ENG"},
                         {10, 3, 2, (int)counts.size(), 2, 0, 0, 127, 0, "Type", "TYPE"}}, en);
      int ns = 0;
      for (int c : counts) ns += c;
      std::vector<uint8_t> blk = {10, (uint8_t)ns, 1, 9, (uint8_t)counts.size()};
      for (int c : counts) blk.push_back((uint8_t)c);
      blk.push_back(1);
      for (int k = 0; k < ns; ++k) put(blk, ("T" + std::to_string(k)).c_str(), 8);
      for (int k = 0; k < ns; ++k) { put(blk, "Lbl", 12); put(blk, "LB", 4); }
      uint16_t len = (uint16_t)blk.size();
      blk.push_back((uint8_t)len); blk.push_back((uint8_t)(len >> 8));
      put(blk, "RLB2", 4);
      *blk_at = pkg.size();
      pkg.insert(pkg.end(), blk.begin(), blk.end());
      if (blk_out) *blk_out = blk;
      i.manifest_size += (uint32_t)blk.size();
      return i;
    };
    auto zero = [](const rmp_labels &L) {
      const uint8_t *b = (const uint8_t *)&L;
      for (size_t k = 0; k < sizeof L; ++k) if (b[k]) return false;
      return true;
    };
    auto sane = [](const rmp_labels &L) {   // a block the reader took: every index the consumers use is in bounds
      if (L.nsets < 1 || L.nsets > RMP_LABELS_SETS || L.nparams < 1 || L.nparams > RMP_LABELS_PARAMS) return false;
      if (L.dep) {
        if (L.ndep < 1 || L.ndep > RMP_LABELS_DEPS) return false;
        int sum = 0;
        for (int v = 0; v < L.ndep; ++v) {
          if (L.count[v] < 1 || L.count[v] > 16 || L.base[v] != sum) return false;
          sum += L.count[v];
        }
        if (sum != L.nsets) return false;
      }
      float pv[16];
      char nm[16][8];
      for (float a : {0.0f, 63.0f, 127.0f, 1e9f, -1e9f, __builtin_nanf("")})
        for (float b : {0.0f, 64.0f, 127.0f, 1e9f, __builtin_nanf("")}) {
          for (float &x : pv) x = b;
          if (L.dep) pv[L.dep - 1] = a;
          int set = rmp_labels_pick(&L, pv);
          if (set < 0 || set >= L.nsets) return false;
          if (L.dep && rmp_labels_enum(&L, set, L.knob, nm) > 16) return false;
        }
      return true;
    };
    rmp_desc dd;
    rmp_labels L;
    size_t at = 0;
    std::vector<uint8_t> blk;
    // the largest: 32 sets (16 + 16) taken; 33 (16 + 16 + 1) refused; a count of 17 refused
    rmp_info i = mk({16, 16}, &blk, &at);
    CHECK(rmp_manifest_read(&i, rd, nullptr, &dd) && rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && L.nsets == 32 && sane(L));
    char nm[16][8];
    CHECK(rmp_labels_enum(&L, 31, 10, nm) == 16 && !strcmp(nm[15], "T31"));
    i = mk({16, 16, 1}, &blk, &at);
    CHECK(rmp_manifest_read(&i, rd, nullptr, &dd) && !rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));
    i = mk({17, 1}, &blk, &at);
    CHECK(rmp_manifest_read(&i, rd, nullptr, &dd) && !rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));
    // a good one to damage
    i = mk({2, 3}, &blk, &at);
    CHECK(rmp_manifest_read(&i, rd, nullptr, &dd) && rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && sane(L));
    std::vector<uint8_t> good = pkg;
    size_t foot = pkg.size() - 6;
    uint16_t len = (uint16_t)(blk.size() - 6);
    auto set_len = [&](uint32_t n) { pkg[foot] = (uint8_t)n; pkg[foot + 1] = (uint8_t)(n >> 8); };
    set_len(len - 1u); CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));      // one short
    set_len(len + 1u); CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));      // one long
    set_len(60000u); CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));        // past the manifest
    set_len(5u); CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));            // only the head's 5
    pkg = good;
    pkg[at + 4] = 16; CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));       // ndep 16: len < head
    pkg = good;
    pkg[at + 3] = 0; CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));        // dep 0
    pkg = good;
    pkg[at + 1] = 0; CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));        // nsets 0
    pkg = good;
    pkg[at + 8] = 0x01; pkg[at + 9] = 0xff;                                                     // a name not printable
    CHECK(rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && sane(L) && !strcmp(L.names[0], "__"));
    pkg = good;
    pkg.resize(pkg.size() - 3);   // truncated: the footer read fails
    CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));
    pkg = good;
    reads = 0, fail_at = 1;   // the footer read works, the block's read fails
    CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L) && reads == 1);
    reads = 0, fail_at = 0;   // the footer's read fails
    CHECK(!rmp_manifest_labels(&i, rd, nullptr, &dd, &L) && zero(L));
    fail_at = -1;
    // every byte of the block and footer at each of these values: refused (all zero) or in bounds
    int taken = 0, refused = 0;
    for (size_t k = at; k < pkg.size(); ++k)
      for (int x : {0, 1, 2, 5, 9, 10, 16, 17, 32, 33, 0x7f, 0x80, 0xff, good[k] ^ 1}) {
        pkg[k] = (uint8_t)x;
        bool ok = rmp_manifest_labels(&i, rd, nullptr, &dd, &L);
        CHECK(ok ? sane(L) : zero(L));
        ok ? ++taken : ++refused;
        pkg[k] = good[k];
      }
    CHECK(taken > 0 && refused > 0);
    pkg = good;
  }
  {   // a missing machine's description from its id
    rmp_desc_from_id("befaco.np.atari", &d);
    CHECK(str_is(d.name, 10, "ATARI") && str_is(d.short_name, 4, "ATAR") && d.nknobs == 0 && d.pre == -1);
    rmp_desc_from_id("plainidentifier1", &d);
    CHECK(str_is(d.name, 10, "PLAINIDENT") && str_is(d.short_name, 4, "PLAI"));
  }
  {   // the real sine_test package (format 3 since Task 3), read in pieces
    const char *path = argc > 1 ? argv[1] : "../../../../plugins/sine_test/sine_test.rmp";
    FILE *f = fopen(path, "rb"); CHECK(f);
    pkg.clear(); uint8_t b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) pkg.insert(pkg.end(), b, b + n);
    fclose(f);
    rmp_info i; CHECK(rmp_verify_header(pkg.data(), (uint32_t)pkg.size(), &i) == RMP_OK);
    reads = 0;
    CHECK(rmp_manifest_read(&i, rd, nullptr, &d));
    CHECK(str_is(d.name, 10, "SINE TEST") && d.nknobs == 3 && d.knob[0].kind == RMP_TEXT_BIPOLAR && d.format == 3);
    CHECK(reads == 1 + 3);   // the head, then a knob a read: never the package whole
  }
  printf("ok\n");
  return 0;
}
