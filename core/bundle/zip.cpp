// core/bundle/zip.cpp - see zip.h. Field layouts from PKWARE's APPNOTE.TXT (sections 4.3.7, 4.3.12, 4.3.16).
#include "zip.h"

#include <string.h>

#include "../platform.h"

namespace core {
namespace bundle {

namespace {
void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
uint32_t get16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }
void say(char* err, int cap, const char* what, const char* name = nullptr) {
  if (name) sniprintf(err, cap, "%s: %s", what, name);
  else sniprintf(err, cap, "%s", what);
}
constexpr uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;
constexpr uint16_t kFlagsUtf8 = 0x0800, kMadeBy = 0x0314 /* UNIX, 2.0 */, kNeed = 10;
constexpr uint32_t kAttr = 0100644u << 16;   // rw-r--r--, a regular file

// a local header (30 + name + extra) into out
uint32_t local_header(const ZEntry& e, const Layout& L, uint8_t* out) {
  uint32_t nl = (uint32_t)strlen(e.name);
  put32(out, kLocalSig);
  put16(out + 4, kNeed);
  put16(out + 6, kFlagsUtf8);
  put16(out + 8, 0);
  put16(out + 10, L.dos_time);
  put16(out + 12, L.dos_date);
  put32(out + 14, e.crc);
  put32(out + 18, e.size);
  put32(out + 22, e.size);
  put16(out + 26, nl);
  put16(out + 28, e.extra);
  memcpy(out + 30, e.name, nl);
  uint8_t* x = out + 30 + nl;
  memset(x, 0, e.extra);
  put16(x, kAlignExtra);
  put16(x + 2, e.extra - 4u);
  put16(x + 4, kAlign);
  return 30 + nl + e.extra;
}
uint32_t central_header(const ZEntry& e, const Layout& L, uint8_t* out) {
  uint32_t nl = (uint32_t)strlen(e.name);
  put32(out, kCentralSig);
  put16(out + 4, kMadeBy);
  put16(out + 6, kNeed);
  put16(out + 8, kFlagsUtf8);
  put16(out + 10, 0);
  put16(out + 12, L.dos_time);
  put16(out + 14, L.dos_date);
  put32(out + 16, e.crc);
  put32(out + 20, e.size);
  put32(out + 24, e.size);
  put16(out + 28, nl);
  put16(out + 30, 0);
  put16(out + 32, 0);
  put16(out + 34, 0);
  put16(out + 36, 0);
  put32(out + 38, kAttr);
  put32(out + 42, (uint32_t)e.local);
  memcpy(out + 46, e.name, nl);
  return 46 + nl;
}
// copy [a, b) of a piece that spans [p0, p0 + n) into the output window [off, off + len)
void window(const uint8_t* piece, uint64_t p0, uint32_t n, uint64_t off, uint8_t* buf, uint32_t len) {
  uint64_t a = p0 > off ? p0 : off, b = p0 + n < off + len ? p0 + n : off + len;
  if (a < b) memcpy(buf + (a - off), piece + (a - p0), (size_t)(b - a));
}
}  // namespace

bool name_ok(const char* n) {
  size_t l = strlen(n);
  if (l == 0 || l > (size_t)kNameMax || n[0] == '/' || n[l - 1] == '/') return false;
  const char* comp = n;
  for (size_t i = 0; i <= l; ++i) {
    unsigned char c = (unsigned char)n[i];
    if (c == '/' || c == 0) {
      size_t cl = (size_t)(n + i - comp);
      if (cl == 0 || (cl == 1 && comp[0] == '.') || (cl == 2 && comp[0] == '.' && comp[1] == '.')) return false;
      comp = n + i + 1;
      continue;
    }
    if (c < 0x20 || c == 0x7f || c == '\\' || c == ':') return false;
  }
  return true;
}

void dos_time(uint32_t epoch, uint16_t* date, uint16_t* time) {
  if (epoch < 315532800u) epoch = 315532800u;   // 1980-01-01
  uint32_t days = epoch / 86400, rem = epoch % 86400;
  int64_t z = (int64_t)days + 719468, era = z / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = (int64_t)yoe + era * 400;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  uint32_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  if (y > 2107) y = 2107;
  *date = (uint16_t)((y - 1980) << 9 | m << 5 | d);
  *time = (uint16_t)((rem / 3600) << 11 | (rem / 60 % 60) << 5 | (rem % 60) / 2);
}

bool layout(ZEntry* e, int n, uint16_t date, uint16_t time, Layout* L) {
  if (n < 0 || n > kZipEntriesMax) return false;
  uint64_t at = 0, cd = 0;
  for (int i = 0; i < n; ++i) {
    if (!name_ok(e[i].name)) return false;
    uint64_t base = at + 30 + strlen(e[i].name);
    uint32_t pad = (uint32_t)((kAlign - (base + 6) % kAlign) % kAlign) + 6;   // the 0xD935 field: 6 bytes at least
    e[i].local = at;
    e[i].extra = (uint16_t)pad;
    e[i].data = base + pad;
    at = e[i].data + e[i].size;
    cd += 46 + strlen(e[i].name);
    if (at > kZipMax) return false;
  }
  L->cd_off = at, L->cd_size = cd, L->total = at + cd + 22, L->dos_date = date, L->dos_time = time, L->n = n;
  return L->total <= kZipMax;
}

int entry_at(const ZEntry* e, const Layout& L, uint64_t off, bool* in_data) {
  int lo = 0, hi = L.n - 1, f = -1;
  *in_data = false;
  if (off >= L.cd_off) return -1;
  while (lo <= hi) {   // the last entry whose local header starts at or before off
    int m = (lo + hi) / 2;
    if (e[m].local <= off) f = m, lo = m + 1;
    else hi = m - 1;
  }
  if (f >= 0) *in_data = off >= e[f].data;
  return f;
}

bool render(const ZEntry* e, const Layout& L, uint64_t off, uint8_t* buf, uint32_t len, DataFn data, void* ctx) {
  if (off + len > L.total) return false;
  uint64_t end = off + len;
  static uint8_t hdr[30 + kNameMax + kAlign + 16];   // a local header (its padding < kAlign + 6) or a central one
  bool in;
  int i = entry_at(e, L, off, &in);
  if (i < 0 && off < L.cd_off) i = 0;
  for (; i >= 0 && i < L.n && off < end; ++i) {
    const ZEntry& z = e[i];
    if (z.local >= end) break;
    uint32_t hl = 30 + (uint32_t)strlen(z.name) + z.extra;
    if (off < z.data && z.local + hl > off) {
      if (hl > sizeof hdr) return false;
      local_header(z, L, hdr);
      window(hdr, z.local, hl, off, buf, len);
    }
    uint64_t a = off > z.data ? off : z.data, b = end < z.data + z.size ? end : z.data + z.size;
    if (a < b && !data(ctx, i, (uint32_t)(a - z.data), buf + (a - off), (uint32_t)(b - a))) return false;
  }
  if (end > L.cd_off) {   // the central directory, entry by entry, then the end record
    uint64_t at = L.cd_off;
    for (int k = 0; k < L.n && at < end; ++k) {
      uint32_t cl = 46 + (uint32_t)strlen(e[k].name);
      if (at + cl > off) {
        central_header(e[k], L, hdr);
        window(hdr, at, cl, off, buf, len);
      }
      at += cl;
    }
    uint8_t eo[22];
    put32(eo, kEndSig);
    put16(eo + 4, 0);
    put16(eo + 6, 0);
    put16(eo + 8, (uint32_t)L.n);
    put16(eo + 10, (uint32_t)L.n);
    put32(eo + 12, (uint32_t)L.cd_size);
    put32(eo + 16, (uint32_t)L.cd_off);
    put16(eo + 20, 0);
    window(eo, L.cd_off + L.cd_size, 22, off, buf, len);
  }
  return true;
}

bool eocd(const uint8_t* t, uint32_t len, uint64_t fs, ZRead* r, char* err, int cap) {
  if (len < kEocdTail || fs < kEocdTail) return say(err, cap, "not a bundle (too short)"), false;
  const uint8_t* p = t + len - kEocdTail;
  if (get32(p) != kEndSig) return say(err, cap, "not a bundle (no ZIP end record, or a ZIP comment)"), false;
  if (get16(p + 4) || get16(p + 6) || get16(p + 8) != get16(p + 10))
    return say(err, cap, "a split (multi-disk) ZIP: not a bundle"), false;
  r->file_size = fs, r->n = (int)get16(p + 10), r->cd_size = get32(p + 12), r->cd_off = get32(p + 16);
  if (get32(p + 16) == 0xFFFFFFFFu || get16(p + 10) == 0xFFFF) return say(err, cap, "a ZIP64 archive: not a v1 bundle"), false;
  if (r->cd_off + r->cd_size + kEocdTail != fs) return say(err, cap, "the central directory is not where the end record says"), false;
  if (r->n < 1) return say(err, cap, "an empty ZIP"), false;
  return true;
}

bool central(const uint8_t* cd, const ZRead& r, ZEntry* out, int max, char* err, int cap) {
  if (r.n > max) return say(err, cap, "too many entries for this Sidecar"), false;
  uint64_t at = 0;
  for (int i = 0; i < r.n; ++i) {
    if (at + 46 > r.cd_size) return say(err, cap, "the central directory is cut short"), false;
    const uint8_t* p = cd + at;
    uint32_t nl = get16(p + 28), xl = get16(p + 30), kl = get16(p + 32);
    if (get32(p) != kCentralSig || at + 46 + nl + xl + kl > r.cd_size) return say(err, cap, "a damaged central directory"), false;
    ZEntry& e = out[i];
    if (nl > (uint32_t)kNameMax) return say(err, cap, "an entry name is too long"), false;
    memcpy(e.name, p + 46, nl);
    e.name[nl] = 0;
    if (strlen(e.name) != nl || !name_ok(e.name)) return say(err, cap, "an unsafe entry name", e.name), false;
    uint32_t flags = get16(p + 8);
    if (flags & 0x0001) return say(err, cap, "an encrypted entry", e.name), false;
    if (flags & 0x0008) return say(err, cap, "an entry with a data descriptor (not a bundle's)", e.name), false;
    if (get16(p + 10) != 0) return say(err, cap, "a compressed entry (bundles store only)", e.name), false;
    e.crc = get32(p + 16);
    e.size = get32(p + 20);
    if (get32(p + 24) != e.size) return say(err, cap, "sizes differ in the central directory", e.name), false;
    e.local = get32(p + 42);
    e.data = 0, e.extra = 0;
    if (i > 0 && e.local <= out[i - 1].local) return say(err, cap, "entries out of order", e.name), false;
    if (e.local >= r.cd_off) return say(err, cap, "an entry starts past its data", e.name), false;
    at += 46 + nl + xl + kl;
  }
  if (at != r.cd_size) return say(err, cap, "the central directory has extra bytes"), false;
  return true;
}

bool local(const uint8_t* h, uint32_t len, ZEntry* e, uint64_t next, char* err, int cap) {
  if (len < 30 || get32(h) != kLocalSig) return say(err, cap, "a damaged local header", e->name), false;
  uint32_t nl = get16(h + 26), xl = get16(h + 28);
  if (30 + nl + xl > len) return say(err, cap, "a local header is cut short", e->name), false;
  if (nl != strlen(e->name) || memcmp(h + 30, e->name, nl)) return say(err, cap, "names differ (local / central)", e->name), false;
  if (get16(h + 6) & 0x0009 || get16(h + 8) != 0) return say(err, cap, "a local header's flags or method", e->name), false;
  if (get32(h + 14) != e->crc || get32(h + 18) != e->size || get32(h + 22) != e->size)
    return say(err, cap, "CRC or sizes differ (local / central)", e->name), false;
  e->extra = (uint16_t)xl;
  e->data = e->local + 30 + nl + xl;
  if (e->data + e->size > next) return say(err, cap, "an entry's data overlaps the next", e->name), false;
  return true;
}

}  // namespace bundle
}  // namespace core
