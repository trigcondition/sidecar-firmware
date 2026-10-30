// core/tests/fuzz_fmt.cpp - the format registry's fuzz target (core/fmt; the formats spec 7.1): every registered
// format's sniffer, upgrade chain and reader on arbitrary bytes - no crash, no read past the input (ASan), no undefined
// behaviour (UBSan), and fmt::load never touches the card. make -C core fuzz-fmt: libFuzzer (Homebrew's clang,
// FUZZ_CXX) seeded from core/tests/golden, FUZZ_SECS seconds; without libFuzzer the same target under ASan + UBSan with
// a deterministic driver (-DFUZZ_STANDALONE: the golden files mutated: cut short, bytes changed, NULs, CR / LF mixes,
// huge counts). The quick in-process run is in test_fmt.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fmt/fmt.h"
#include "poly.h"

#ifndef FUZZ_STANDALONE
// libFuzzer's coverage tables keep every function of the objects linked: the pattern file's chord editor
// (seq::set_chord) names poly::expand, which the readers never call. A stand-in here instead of the engine's poly.cpp.
namespace core {
namespace poly {
int expand(int, const Chord&, uint8_t*) { return 0; }
}  // namespace poly
}  // namespace core
#endif

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static uint8_t buf[1 << 21];
  if (size > (1u << 20)) return 0;
  core::fmt::Bytes in = {data, (uint32_t)size};
  for (int i = 0; i < core::fmt::count(); ++i) {
    core::fmt::Buf b = {buf, 0, sizeof buf};
    core::fmt::Info info;
    core::fmt::Status st = core::fmt::load(*core::fmt::at(i), in, &b, &info);
    if ((st == core::fmt::kOk || st == core::fmt::kForeignNewer) && (b.n != size || (size && memcmp(buf, data, size))))
      __builtin_trap();   // a file loaded as it is must come out as it is
    if ((st == core::fmt::kNewer || st == core::fmt::kUnreadable) && !info.locked) __builtin_trap();
    char line[300];
    core::fmt::refusal(*core::fmt::at(i), info, "/x", line, sizeof line);
  }
  return 0;
}

#ifdef FUZZ_STANDALONE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

static void seeds(const std::string& dir, std::vector<std::string>* out, int depth = 0) {
  DIR* d = opendir(dir.c_str());
  if (!d || depth > 3) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    std::string p = dir + "/" + e->d_name;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) continue;
    std::string s;
    char b[4096];
    size_t n;
    bool file = true;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    if (ferror(f)) file = false;
    fclose(f);
    if (file && e->d_type == DT_REG) out->push_back(s);
    else seeds(p, out, depth + 1);
  }
  closedir(d);
}
int main(int argc, char** argv) {
  std::vector<std::string> sd;
  seeds(argc > 1 ? argv[1] : "tests/golden", &sd);
  if (sd.empty()) sd.push_back("");
  long runs = argc > 2 ? atol(argv[2]) : 200000;
  uint32_t x = 2463534242u;
  auto rnd = [&]() { x ^= x << 13, x ^= x >> 17, x ^= x << 5; return x; };
  for (long it = 0; it < runs; ++it) {
    std::string s = sd[rnd() % sd.size()];
    for (int k = (int)(rnd() % 4); k >= 0; --k) {
      switch (rnd() % 7) {
        case 0: if (!s.empty()) s.resize(rnd() % s.size()); break;
        case 1: if (!s.empty()) s[rnd() % s.size()] = (char)rnd(); break;
        case 2: s.insert(s.empty() ? 0 : rnd() % s.size(), 1, '\0'); break;
        case 3: for (char& c : s) if (c == '\n' && rnd() % 2) c = '\r'; break;
        case 4: if (s.size() > 16) { size_t at = 4 + rnd() % 12; s[at] = (char)0xFF; } break;
        case 5: s += sd[rnd() % sd.size()]; break;
        default: { std::string j; for (int n = (int)(rnd() % 64); n > 0; --n) j += (char)rnd(); s.insert(s.empty() ? 0 : rnd() % s.size(), j); }
      }
    }
    LLVMFuzzerTestOneInput((const uint8_t*)s.data(), s.size());
  }
  printf("fuzz_fmt: %ld inputs x %d formats, %zu seeds: no crash\n", runs, core::fmt::count(), sd.size());
  return 0;
}
#endif
