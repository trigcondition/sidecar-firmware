// core/bundle/zip.h - the project bundle's container (docs/BUNDLES.md "Format"): a plain ZIP any Mac or PC opens,
// written and read here without third-party code. The rules (bundles design 3.1):
//   - stored entries only (method 0), no data descriptors (CRC-32 and sizes in each local header), no ZIP64: a bundle
//     is at most kZipMax bytes and 65535 entries;
//   - UTF-8 names (flag bit 11), checked by name_ok();
//   - every entry's data starts on a kAlign (4096) boundary, padded by an extra field with id 0xD935 (Android's
//     zipalign: 2 bytes of alignment, then zeros);
//   - the central directory holds no extra fields, made by UNIX (rw-r--r--), one DOS date and time for all entries.
// So the layout is fixed once the names and sizes are known: render() gives any byte range of the archive from the
// entries alone (headers made on the fly, entry data through a callback): the same function writes a bundle to the
// card and serves the browser's ranges. The reader checks the same rules and refuses anything else (a bundle is
// untrusted input: sizes cross-checked between the central directory and every local header, entries in order and
// not overlapping, names checked).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../lib/sha256.h"   // crc32 (zlib's convention: start 0, the result is the CRC)

namespace core {
namespace bundle {

constexpr uint32_t kAlign = 4096;
constexpr uint64_t kZipMax = 0x100000000ull - 65536;   // 4 GiB - 64 KiB (no ZIP64 in v1)
constexpr int kZipEntriesMax = 65535;
constexpr int kNameMax = 200;                          // an entry's name, bytes
constexpr uint16_t kAlignExtra = 0xD935;

// A bundle entry's name: 1..kNameMax bytes, no leading '/', no '\\' or ':', no control characters, no "." or ".."
// component, no empty component (no "//", no trailing '/'). Bytes >= 0x80 are allowed (UTF-8).
bool name_ok(const char* n);

struct ZEntry {
  char name[kNameMax + 1];
  uint32_t size, crc;
  uint64_t local;    // the local header's offset
  uint64_t data;     // the data's offset (a multiple of kAlign)
  uint16_t extra;    // the local header's extra field length (the padding)
};
struct Layout {
  uint64_t cd_off, cd_size, total;
  uint16_t dos_date, dos_time;
  int n;
};
// FAT / ZIP date and time (1980..2107); from seconds since 1970 (UTC); 0 seconds -> 1980-01-01 00:00
void dos_time(uint32_t epoch, uint16_t* date, uint16_t* time);
// The offsets of n entries (name, size, crc set). false: a bad name, more than kZipEntriesMax entries, or over kZipMax.
bool layout(ZEntry* e, int n, uint16_t date, uint16_t time, Layout* out);
// The archive's bytes [off, off + len): headers, padding, central directory and end record made here, entry data
// asked for through data(ctx, entry, offset in the entry, buf, n). false: past the end, or data() failed.
typedef bool (*DataFn)(void* ctx, int entry, uint32_t off, uint8_t* buf, uint32_t n);
bool render(const ZEntry* e, const Layout& L, uint64_t off, uint8_t* buf, uint32_t len, DataFn data, void* ctx);
// Which part of the archive off is in: the entry it belongs to (header or data), -1 the central directory / end
int entry_at(const ZEntry* e, const Layout& L, uint64_t off, bool* in_data);

// ---- the reader. Step by step (a bundle job's piece at a time): eocd() parses the end record from the archive's
// last bytes; the central directory is then read whole by the caller (cd_size bytes at cd_off) and parsed by
// central(); each entry's local header (30 + name + extra bytes at e.local) is checked by local(). Every error names
// what is wrong (err).
struct ZRead {
  uint64_t file_size, cd_off, cd_size;
  int n;   // entries the end record names
};
constexpr uint32_t kEocdTail = 22;   // the end record with no comment (a bundle has none; others are refused)
bool eocd(const uint8_t* tail, uint32_t len, uint64_t file_size, ZRead* r, char* err, int errcap);
bool central(const uint8_t* cd, const ZRead& r, ZEntry* out, int max, char* err, int errcap);
constexpr uint32_t kLocalMax = 30 + kNameMax + 0xFFFF;
bool local(const uint8_t* h, uint32_t len, ZEntry* e, uint64_t next_start, char* err, int errcap);

}  // namespace bundle
}  // namespace core
