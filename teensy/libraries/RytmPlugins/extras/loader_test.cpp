// extras/loader_test.cpp - the loader on the Mac: verify, place, relocate.
// build: c++ -std=c++17 -O1 -I../src loader_test.cpp ../src/rmp_loader.cpp -o /tmp/loader_test
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cstddef>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "rmp_loader.h"
#include "rmp_api.h"

static std::vector<uint8_t> read(const char *p) {
  FILE *f = fopen(p, "rb"); CHECK(f);
  std::vector<uint8_t> v; uint8_t b[4096]; size_t n;
  while ((n = fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f); return v;
}
static uint32_t crc32(const uint8_t *p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) { c ^= *p++; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
  return ~c;
}
static void put32(std::vector<uint8_t> &v, size_t at, uint32_t x) { memcpy(&v[at], &x, 4); }
static uint16_t get16(const uint8_t *p) { uint16_t x; memcpy(&x, p, 2); return x; }
static void refresh_crc(std::vector<uint8_t> &p) {
  rmp_header h; memcpy(&h, p.data(), sizeof h);
  h.crc32 = crc32(p.data() + RMP_HEADER_BYTES, p.size() - RMP_HEADER_BYTES);
  memcpy(p.data(), &h, sizeof h);
}
// imm16 back out of a MOVW/MOVT T3 pair of halfwords
static uint16_t imm16_of(const uint8_t *p) {
  uint16_t a = get16(p), b = get16(p + 2);
  return (uint16_t)(((a & 0xFu) << 12) | (((a >> 10) & 1u) << 11) | (((b >> 12) & 7u) << 8) | (b & 0xFFu));
}

// hand-built package: 72-byte image (entry at 0 needs 72 bytes), MOVW r1 at 0, MOVT r1 at 4
static std::vector<uint8_t> tiny(uint32_t value) {
  std::vector<uint8_t> p(RMP_HEADER_BYTES + 72 + 16, 0);
  rmp_header h; memset(&h, 0, sizeof h);
  h.magic = RMP_FILE_MAGIC; h.format = RMP_FORMAT_RMP1; h.api = RMP_API;
  strcpy(h.id, "rytm.tiny"); h.version = 1; h.compatible_from = 1;
  h.image_size = 72; h.bss_size = 4; h.entry_offset = 0; h.reloc_count = 2; h.manifest_size = 0;
  memcpy(p.data(), &h, sizeof h);
  uint16_t movw[2] = {0xF240, 0x0100}, movt[2] = {0xF2C0, 0x0100};   // Rd = r1, imm 0
  memcpy(&p[RMP_HEADER_BYTES], movw, 4); memcpy(&p[RMP_HEADER_BYTES + 4], movt, 4);
  put32(p, RMP_HEADER_BYTES + 72, 0u | (uint32_t(RMP_REL_MOVW) << 28)); put32(p, RMP_HEADER_BYTES + 76, value);
  put32(p, RMP_HEADER_BYTES + 80, 4u | (uint32_t(RMP_REL_MOVT) << 28)); put32(p, RMP_HEADER_BYTES + 84, value);
  refresh_crc(p); return p;
}

// streaming placement: a read function over an in-memory package; counts calls
// and refuses anything outside the buffer (or everything when fail is set)
struct mem_rd { const std::vector<uint8_t> *p; int calls; bool fail; };
static bool mem_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t n) {
  mem_rd *m = (mem_rd *)ctx; ++m->calls;
  if (m->fail || (uint64_t)off + n > m->p->size()) return false;
  memcpy(buf, m->p->data() + off, n); return true;
}
// rmp_place and rmp_place_stream on the same package give the same bytes
static void same_as_place(const std::vector<uint8_t> &pkg, uint32_t base, int min_calls) {
  rmp_info a, b;
  CHECK(rmp_verify(pkg.data(), (uint32_t)pkg.size(), &a) == RMP_OK);
  CHECK(rmp_verify_header(pkg.data(), (uint32_t)pkg.size(), &b) == RMP_OK);   // no pointers into the package
  uint32_t sz = a.image_size + a.bss_size;
  std::vector<uint8_t> x(sz + 16, 0xEE), y(sz + 16, 0x55);
  CHECK(rmp_place(&a, x.data(), base) == RMP_OK);
  mem_rd m{&pkg, 0, false};
  CHECK(rmp_place_stream(&b, mem_read, &m, y.data(), base) == RMP_OK);
  CHECK(memcmp(x.data(), y.data(), sz) == 0);
  for (int i = 0; i < 16; ++i) CHECK(y[sz + i] == 0x55);   // no overrun
  CHECK(m.calls >= min_calls);
}
// tiny() with n ABS32 relocations (n not a multiple of the 32-per-chunk read)
static std::vector<uint8_t> tiny_abs(uint32_t n) {
  auto t = tiny(0);
  rmp_header h; memcpy(&h, t.data(), sizeof h); h.reloc_count = n; memcpy(t.data(), &h, sizeof h);
  t.resize(RMP_HEADER_BYTES + 72 + 8 * n);
  for (uint32_t k = 0; k < n; ++k) {
    put32(t, RMP_HEADER_BYTES + 8 + 4 * (k % 16), 0x100u * k);
    put32(t, RMP_HEADER_BYTES + 72 + 8 * k, (8u + 4u * (k % 16)) | (uint32_t(RMP_REL_ABS32) << 28));
    put32(t, RMP_HEADER_BYTES + 72 + 8 * k + 4, 0);
  }
  refresh_crc(t); return t;
}

int main(int argc, char **argv) {
  auto pkg = read(argc > 1 ? argv[1] : "../../../../plugins/sine_test/sine_test.rmp");
  rmp_info info;
  CHECK(rmp_verify(pkg.data(), (uint32_t)pkg.size(), &info) == RMP_OK);
  CHECK(strcmp(info.id, "rytm.sine_test") == 0);
  const uint32_t base = 0x20010000u;
  std::vector<uint8_t> arena(info.image_size + info.bss_size + 64, 0xEE);
  CHECK(rmp_place(&info, arena.data(), base) == RMP_OK);
  uint32_t w[10]; memcpy(w, arena.data() + info.entry_offset, sizeof w);
  CHECK(w[0] == 0x504D5252u && w[1] == 1u);
  for (int k = 4; k < 10; ++k) CHECK(w[k] >= base && w[k] < base + info.image_size);
  for (uint32_t i = 0; i < info.bss_size; ++i) CHECK(arena[info.image_size + i] == 0);
  for (int i = 0; i < 64; ++i) CHECK(arena[info.image_size + info.bss_size + i] == 0xEE);  // no overrun

  // rejects: truncated, wrong magic, wrong API, wrong format, header-only
  { auto t = pkg; CHECK(rmp_verify(t.data(), (uint32_t)t.size() - 1, &info) == RMP_ERR_SIZE); }
  { CHECK(rmp_verify(pkg.data(), 10, &info) == RMP_ERR_SIZE); }
  { auto t = pkg; t[0] ^= 0xFF; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_MAGIC); }
  { auto t = pkg; t[4] = 9; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_FORMAT); }
  // formats 1..3 load (they differ only in the manifest; the CRC covers the body, not the header); 0 and 4 (RMPA only) do not
  { auto t = pkg; t[4] = 1; rmp_info i1; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &i1) == RMP_OK && i1.format == 1); }
  { auto t = pkg; t[4] = 2; rmp_info i2; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &i2) == RMP_OK && i2.format == 2); }
  { auto t = pkg; t[4] = 3; rmp_info i3; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &i3) == RMP_OK && i3.format == 3); }
  { auto t = pkg; t[4] = 4; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_FORMAT); }
  { auto t = pkg; t[4] = 0; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_FORMAT); }
  // plugin API 1 and 2 load; 0, 3 and 99 do not
  { auto t = pkg; t[6] = 1; rmp_info a1; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &a1) == RMP_OK && a1.api == 1); }
  { auto t = pkg; t[6] = 2; rmp_info a2; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &a2) == RMP_OK && a2.api == 2); }
  { auto t = pkg; t[6] = 0; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_API); }
  { auto t = pkg; t[6] = 3; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_API); }
  { auto t = pkg; t[6] = 99; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_API); }
  { auto t = pkg; put32(t, offsetof(rmp_header, reloc_count), 0xFFFFFFFFu);
    CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_SIZE); }
  { auto t = pkg; put32(t, offsetof(rmp_header, manifest_size), 0xFFFFFFFFu);
    CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_SIZE); }
  { auto t = pkg; put32(t, offsetof(rmp_header, entry_offset), (info.image_size & ~3u) - 68);
    CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_SIZE); }

  { auto t = pkg; put32(t, offsetof(rmp_header, entry_offset), 2);   // misaligned entry
    CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_SIZE); }
  { auto t = pkg; t.push_back(0); t.push_back(0); t.push_back(0); t.push_back(0);   // trailing garbage
    CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_SIZE); }
  // MOVW/MOVT at a base that is not 64K aligned; value carries the full 32 bits
  {
    const uint32_t value = 0x00001234u, b2 = 0x2000F8C4u;
    auto t = tiny(value);
    rmp_info ti; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &ti) == RMP_OK);
    uint8_t dst[76];
    CHECK(rmp_place(&ti, dst, b2) == RMP_OK);
    uint32_t want = value + b2;
    CHECK(imm16_of(dst) == (want & 0xFFFFu));
    CHECK(imm16_of(dst + 4) == (want >> 16));
    CHECK(get16(dst) >> 11 == 0x1E && (get16(dst + 2) & 0x0F00) == 0x0100);   // opcode, Rd kept
    // carry from the low half into the high half: a base whose low 16 bits overflow
    const uint32_t b3 = 0x2000FFF0u; want = value + b3;
    CHECK(rmp_place(&ti, dst, b3) == RMP_OK);
    CHECK(imm16_of(dst) == (want & 0xFFFFu) && imm16_of(dst + 4) == (want >> 16));
  }
  // relocation outside the image is refused
  {
    auto t = tiny(0); put32(t, RMP_HEADER_BYTES + 72, 70u | (uint32_t(RMP_REL_ABS32) << 28)); refresh_crc(t);
    rmp_info ti; CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &ti) == RMP_OK);
    uint8_t dst[76]; CHECK(rmp_place(&ti, dst, 0x20000000u) == RMP_ERR_RELOC);
  }

  // id: no NUL in the 32 bytes, or empty -> RMP_ERR_FORMAT (CRC is not part of the header check)
  { auto t = pkg; memset(&t[offsetof(rmp_header, id)], 'x', 32);
    CHECK(rmp_verify_header(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_FORMAT);
    CHECK(rmp_verify(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_FORMAT); }
  { auto t = pkg; t[offsetof(rmp_header, id)] = 0;
    CHECK(rmp_verify_header(t.data(), (uint32_t)t.size(), &info) == RMP_ERR_FORMAT); }
  // header-only check + streaming CRC in odd chunks agree with rmp_verify
  {
    rmp_info hi; CHECK(rmp_verify_header(pkg.data(), (uint32_t)pkg.size(), &hi) == RMP_OK);
    CHECK(hi.image == nullptr && strcmp(hi.id, "rytm.sine_test") == 0);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = RMP_HEADER_BYTES; i < pkg.size(); i += 37) c = rmp_crc32_update(c, &pkg[i], (uint32_t)std::min<size_t>(37, pkg.size() - i));
    CHECK(~c == hi.crc32);
  }
  // streaming placement = rmp_place, byte for byte: sine_test, the MOVW/MOVT package,
  // 70 relocations (reads of 32 + 32 + 6), a failing read, a relocation outside the image
  same_as_place(pkg, 0x20010000u, 2);
  same_as_place(tiny(0x00001234u), 0x2000F8C4u, 2);
  same_as_place(tiny(0x00001234u), 0x2000FFF0u, 2);
  same_as_place(tiny_abs(70), 0x20000008u, 1 + 3);
  {
    auto t = tiny_abs(70); rmp_info ti; CHECK(rmp_verify_header(t.data(), (uint32_t)t.size(), &ti) == RMP_OK);
    std::vector<uint8_t> d(76); mem_rd m{&t, 0, true};
    CHECK(rmp_place_stream(&ti, mem_read, &m, d.data(), 0x20000000u) == RMP_ERR_SIZE);
    auto u = tiny(0); put32(u, RMP_HEADER_BYTES + 72, 70u | (uint32_t(RMP_REL_ABS32) << 28)); refresh_crc(u);
    CHECK(rmp_verify_header(u.data(), (uint32_t)u.size(), &ti) == RMP_OK);
    mem_rd m2{&u, 0, false};
    CHECK(rmp_place_stream(&ti, mem_read, &m2, d.data(), 0x20000000u) == RMP_ERR_RELOC);
  }
  // ids that are safe as file names (the store's rule, shared with the slot map)
  CHECK(rmp_id_ok("rytm.sine_test") && rmp_id_ok("a") && rmp_id_ok("x.y-z_1"));
  CHECK(rmp_id_ok("0123456789012345678901234567890"));     // 31 chars
  CHECK(!rmp_id_ok("01234567890123456789012345678901"));   // 32 chars
  CHECK(!rmp_id_ok(nullptr) && !rmp_id_ok("") && !rmp_id_ok(".hidden") && !rmp_id_ok("a/b") &&
        !rmp_id_ok("a\\b") && !rmp_id_ok("a..b"));
  // printable ASCII only, no spaces (0x21..0x7E)
  CHECK(!rmp_id_ok("a b") && !rmp_id_ok("a\tb") && !rmp_id_ok("a\x01") && !rmp_id_ok("a\x7f") &&
        !rmp_id_ok("caf\xc3\xa9") && rmp_id_ok("~!#$%&'()*+,-:;<=>?@[]^_`{|}"));
  // corrupt one byte (200 is past the 96-byte header): the CRC must refuse it
  pkg[200] ^= 1;
  CHECK(rmp_verify(pkg.data(), (uint32_t)pkg.size(), &info) == RMP_ERR_CRC);
  printf("ok\n");
  return 0;
}
