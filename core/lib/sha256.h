// core/lib/sha256.h - the Sidecar's one SHA-256 (FIPS 180-4) and one CRC-32 (zlib's), written for this project from
// the standards' descriptions (no third-party code). The identity of every sample in the library (core/lib/library.h,
// docs/LIBRARY.md) and of every file in a project bundle (docs/BUNDLES.md); the CRC-32 the bundles' ZIP needs. Portable,
// no allocation, no globals written: safe on any core.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace core {
namespace lib {

struct Sha256 {
  uint32_t h[8];
  uint64_t bytes;
  uint8_t buf[64];
  uint32_t nbuf;
  void init();
  void update(const void* p, size_t n);
  void final(uint8_t out[32]);
};
void sha256(const void* p, size_t n, uint8_t out[32]);
void hex(const uint8_t* d, int n, char* out);           // 2n lowercase digits + NUL
bool unhex(const char* s, uint8_t* out, int n);         // exactly 2n hex digits (then the end of s)
bool unhex_prefix(const char* s, uint8_t* out, int n);  // 2n hex digits at s (anything may follow)
// zlib's convention: start 0, the result is the CRC; crc32(crc32(0, a), b) == crc32(0, a + b). Slice-by-8.
uint32_t crc32(uint32_t crc, const void* p, size_t n);

}  // namespace lib

namespace bundle {   // (the bundle code's names: one implementation)
using lib::crc32;
using lib::hex;
using lib::Sha256;
using lib::sha256;
using lib::unhex;
}  // namespace bundle
}  // namespace core
