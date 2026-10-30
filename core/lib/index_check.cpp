// core/lib/index_check.cpp - lib::index_check (library.h): an index.bin image checked before it is used (the format
// registry's reader for "library", core/fmt/formats.cpp). Pure, no card, no globals: its own file so the format fuzzer
// (core/tests/fuzz_fmt.cpp) links it without the library's state.
#include <string.h>

#include "../platform.h"
#if CORE_MSAMP

#include "../fmt/fmt.h"
#include "library.h"
#include "sha256.h"

namespace core {
namespace lib {

namespace {
constexpr uint32_t kRec = 64, kRecPath = 60;   // a record's size and its path offset's place (library.cpp's Rec)
constexpr uint32_t kBody = 16;                 // u32 records, u32 path bytes, two reserved u32
uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
}  // namespace

const char* index_check(const uint8_t* p, uint32_t n) {
  fmt::Sniffed s;
  uint32_t crc;
  fmt::Bytes b = {p, n};
  if (!fmt::bin_header(b, "SLIB", &s, &crc)) return "not a library index";
  if (n - s.body < kBody) return "cut short";
  if (crc32(0, p + s.body, n - s.body) != crc) return "bad CRC";
  const uint8_t* body = p + s.body;
  uint32_t nr = rd32(body), pl = rd32(body + 4);
  if (nr > kMaxFiles || (uint64_t)kBody + (uint64_t)nr * kRec + pl != (uint64_t)(n - s.body)) return "sizes do not match";
  const char* pt = (const char*)(body + kBody + (size_t)nr * kRec);
  if (pl && pt[pl - 1]) return "a path not ended";
  for (uint32_t i = 0; i < nr; ++i) {
    uint32_t po = rd32(body + kBody + (size_t)i * kRec + kRecPath);
    if (po >= pl) return "a path outside its table";
  }
  return nullptr;
}

}  // namespace lib
}  // namespace core
#endif
