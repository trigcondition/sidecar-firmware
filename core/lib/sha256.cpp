// core/lib/sha256.cpp - see sha256.h.
#include "sha256.h"

#include <string.h>

namespace core {
namespace lib {

namespace {
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void block(uint32_t* h, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 64; ++i) {
    uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}
}  // namespace

void Sha256::init() {
  static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(h, H0, sizeof h);
  bytes = 0, nbuf = 0;
}
void Sha256::update(const void* v, size_t n) {
  const uint8_t* p = (const uint8_t*)v;
  bytes += n;
  if (nbuf) {
    size_t k = 64 - nbuf < n ? 64 - nbuf : n;
    memcpy(buf + nbuf, p, k);
    nbuf += (uint32_t)k, p += k, n -= k;
    if (nbuf < 64) return;
    block(h, buf);
    nbuf = 0;
  }
  for (; n >= 64; p += 64, n -= 64) block(h, p);
  memcpy(buf, p, n);
  nbuf = (uint32_t)n;
}
void Sha256::final(uint8_t out[32]) {
  uint64_t bits = bytes * 8;
  uint8_t pad = 0x80;
  update(&pad, 1);
  uint8_t z = 0;
  while (nbuf != 56) update(&z, 1);
  uint8_t len[8];
  for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
  update(len, 8);
  for (int i = 0; i < 8; ++i)
    out[4 * i] = (uint8_t)(h[i] >> 24), out[4 * i + 1] = (uint8_t)(h[i] >> 16), out[4 * i + 2] = (uint8_t)(h[i] >> 8),
    out[4 * i + 3] = (uint8_t)h[i];
}
void sha256(const void* p, size_t n, uint8_t out[32]) {
  Sha256 s;
  s.init();
  s.update(p, n);
  s.final(out);
}
void hex(const uint8_t* d, int n, char* out) {
  static const char D[] = "0123456789abcdef";
  for (int i = 0; i < n; ++i) out[2 * i] = D[d[i] >> 4], out[2 * i + 1] = D[d[i] & 15];
  out[2 * n] = 0;
}
bool unhex_prefix(const char* s, uint8_t* out, int n) {
  for (int i = 0; i < 2 * n; ++i) {
    char c = s[i];
    int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0) return false;
    if (i & 1) out[i / 2] |= (uint8_t)v;
    else out[i / 2] = (uint8_t)(v << 4);
  }
  return true;
}
bool unhex(const char* s, uint8_t* out, int n) { return unhex_prefix(s, out, n) && s[2 * n] == 0; }

namespace {
// CRC-32 (IEEE 802.3, reflected, zlib's): slice-by-8 tables made at compile time (nothing to initialise at run time,
// so any core may call it first)
struct CrcTables {
  uint32_t t[8][256];
  constexpr CrcTables() : t() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i)
      for (int s = 1; s < 8; ++s) t[s][i] = t[0][t[s - 1][i] & 0xff] ^ (t[s - 1][i] >> 8);
  }
};
constexpr CrcTables kCrc;
}  // namespace

uint32_t crc32(uint32_t crc, const void* v, size_t n) {
  const uint8_t* p = (const uint8_t*)v;
  uint32_t c = ~crc;
  for (; n >= 8; p += 8, n -= 8) {
    uint32_t a = c ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
    c = kCrc.t[7][a & 0xff] ^ kCrc.t[6][(a >> 8) & 0xff] ^ kCrc.t[5][(a >> 16) & 0xff] ^ kCrc.t[4][a >> 24] ^
        kCrc.t[3][b & 0xff] ^ kCrc.t[2][(b >> 8) & 0xff] ^ kCrc.t[1][(b >> 16) & 0xff] ^ kCrc.t[0][b >> 24];
  }
  for (; n; --n, ++p) c = kCrc.t[0][(c ^ *p) & 0xff] ^ (c >> 8);
  return ~c;
}

}  // namespace lib
}  // namespace core
