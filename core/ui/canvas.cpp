// core/ui/canvas.cpp - see ui.h. Cold: drawn at most 20 times a second, and only what changed.
#include <string.h>

#include "ui/ui.h"
#include "ui/fonts/font3x5.h"
#include "ui/fonts/font5x7.h"

namespace core {
namespace ui {

static inline int idx(int x, int y) { return (7 - (y >> 3)) + 8 * x; }

CORE_COLD Canvas::Canvas() { clear(); }

CORE_COLD void Canvas::clear(bool on) {
  memset(fb, on ? 0xFF : 0x00, sizeof fb);
  clear_text();
}

CORE_COLD void Canvas::clip(const Rect& r) {
  int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
  int x1 = r.x + r.w > kW ? kW : r.x + r.w, y1 = r.y + r.h > kH ? kH : r.y + r.h;
  clip_ = Rect{(int16_t)x0, (int16_t)y0, (int16_t)(x1 > x0 ? x1 - x0 : 0), (int16_t)(y1 > y0 ? y1 - y0 : 0)};
}

CORE_COLD void Canvas::unclip() { clip_ = Rect{0, 0, kW, kH}; }

CORE_COLD void Canvas::pixel(int x, int y, Ink ink) {
  if (clipped(x, y)) return;
  uint8_t& b = fb[idx(x, y)];
  uint8_t m = (uint8_t)(1u << (y & 7));
  switch (ink) {
    case kOff: b &= (uint8_t)~m; break;
    case kOn: b |= m; break;
    case kXor: b ^= m; break;
    case kDither: if (((x + y) & 1) == 0) b |= m; else b &= (uint8_t)~m; break;
  }
}

CORE_COLD bool Canvas::get(int x, int y) const {
  if (x < 0 || y < 0 || x >= kW || y >= kH) return false;
  return fb[idx(x, y)] >> (y & 7) & 1;
}

CORE_COLD void Canvas::hline(int x0, int x1, int y, Ink ink) {
  if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
  for (int x = x0; x <= x1; ++x)
    if (ink != kDither || ((x + y) & 1) == 0) pixel(x, y, ink == kDither ? kOn : ink);
}

CORE_COLD void Canvas::vline(int x, int y0, int y1, Ink ink) {
  if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
  for (int y = y0; y <= y1; ++y)
    if (ink != kDither || ((x + y) & 1) == 0) pixel(x, y, ink == kDither ? kOn : ink);
}

CORE_COLD void Canvas::line(int x0, int y0, int x1, int y1, Ink ink) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, n = 0;
  for (;;) {
    if (ink != kDither || (n & 1) == 0) pixel(x0, y0, ink == kDither ? kOn : ink);
    ++n;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

CORE_COLD void Canvas::rect(const Rect& r, Ink ink) {
  if (r.w <= 0 || r.h <= 0) return;
  hline(r.x, r.x + r.w - 1, r.y, ink);
  hline(r.x, r.x + r.w - 1, r.y + r.h - 1, ink);
  vline(r.x, r.y, r.y + r.h - 1, ink);
  vline(r.x + r.w - 1, r.y, r.y + r.h - 1, ink);
}

CORE_COLD void Canvas::fill(const Rect& r, Ink ink) {
  // whole bytes where the rectangle covers a byte's 8 rows (the layout's pages), pixels at the edges
  int x0 = r.x < clip_.x ? clip_.x : r.x, x1 = r.x + r.w > clip_.x + clip_.w ? clip_.x + clip_.w : r.x + r.w;
  int y0 = r.y < clip_.y ? clip_.y : r.y, y1 = r.y + r.h > clip_.y + clip_.h ? clip_.y + clip_.h : r.y + r.h;
  for (int y = y0; y < y1;) {
    int page_end = (y | 7) + 1;
    int ye = page_end < y1 ? page_end : y1;
    uint8_t m = (uint8_t)(((1u << (ye - y)) - 1) << (y & 7));
    for (int x = x0; x < x1; ++x) {
      uint8_t& b = fb[idx(x, y)];
      switch (ink) {
        case kOff: b &= (uint8_t)~m; break;
        case kOn: b |= m; break;
        case kXor: b ^= m; break;
        case kDither: {
          uint8_t d = (x & 1) ? 0xAA : 0x55;   // bit k = row 8 p + k: set where (x + row) is even
          b = (uint8_t)((b & ~m) | (d & m));
          break;
        }
      }
    }
    y = ye;
  }
}

CORE_COLD void Canvas::blit(const uint8_t* cols, int w, int h, int x, int y, Ink ink, int scale) {
  for (int i = 0; i < w; ++i) {
    uint8_t c = cols[i];
    for (int j = 0; j < h; ++j) {
      if (!(c >> j & 1)) continue;
      for (int sx = 0; sx < scale; ++sx)
        for (int sy = 0; sy < scale; ++sy) pixel(x + i * scale + sx, y + j * scale + sy, ink);
    }
  }
}

CORE_COLD int Canvas::diff_tiles(const Canvas& o, uint32_t mask[4]) const {
  mask[0] = mask[1] = mask[2] = mask[3] = 0;
  int n = 0;
  for (int c = 0; c < kTileCols; ++c)
    for (int p = 0; p < kTilePages; ++p) {
      const uint8_t *a = fb + p + 64 * c, *b = o.fb + p + 64 * c;
      bool d = false;
      for (int k = 0; k < 64 && !d; k += 8) d = a[k] != b[k];
      if (!d) continue;
      int t = p * kTileCols + c;
      mask[t >> 5] |= 1u << (t & 31);
      ++n;
    }
  return n;
}

// ---- text
const Font kSmall = {3, 5, 4, kFont3x5Data};
const Font kMedium = {5, 7, 6, kFont5x7Data};

static inline int glyph_of(char ch) {
  int c = (unsigned char)ch;
  if (c >= 'a' && c <= 'z') c -= 32;
  if (c < 32 || c > 95) c = '?';
  return c - 32;
}

CORE_COLD int text(Canvas& cv, const Font& f, int x, int y, const char* s, Ink ink, int scale) {
  for (; *s; ++s) {
    cv.blit(f.data + glyph_of(*s) * f.w, f.w, f.h, x, y, ink, scale);
    x += f.advance * scale;
  }
  return x;
}

CORE_COLD int text_width(const Font& f, const char* s, int scale) {
  int n = (int)strlen(s);
  return n ? (n * f.advance - (f.advance - f.w)) * scale : 0;
}

CORE_COLD void text_in(Canvas& cv, const Font& f, const Rect& r, const char* s, int y, Ink ink, int scale) {
  Rect old = cv.clip_rect();
  cv.clip(r);
  int w = text_width(f, s, scale);
  int x = r.x + (r.w - w) / 2;
  if (x < r.x) x = r.x;
  if (y < 0) y = r.y + (r.h - f.h * scale) / 2;
  text(cv, f, x, y, s, ink, scale);
  cv.clip(old);
}

// ---- the Rytm-drawn text slots
#if CORE_UI_TOOLS
void (*Canvas::fit_report)(const char*, int, int) = nullptr;
#endif
CORE_COLD void Canvas::put_text(int slot, const RFont& f, int x, int y, const char* str, uint8_t flags) {
  if (slot < 0 || slot >= kTextSlots) return;
  TextItem& t = txt[slot];
  x = x < 0 ? 0 : x > kW - 1 ? kW - 1 : x;
  y = y < 0 ? 0 : y > kH - 1 ? kH - 1 : y;
  t.x = (uint8_t)x, t.y = (uint8_t)y, t.font = f.id, t.flags = flags;
  int room = kW / f.advance;   // a whole screen at most: the Rytm does not clip
  if ((flags & 3) == kAlignLeft) room = (kW - x + 1) / f.advance;
  if ((flags & 3) == kAlignRight) room = (x + 2) / f.advance;
  if (room > kTextLen) room = kTextLen;
  int n = 0;
#if CORE_UI_TOOLS
  if (fit_report && str) {
    int len = (int)strlen(str);
    if (len > room) fit_report(str, len, room);
    else if (len && y + f.h + f.desc > kH) fit_report(str, y + f.h + f.desc, kH - y);
  }
#endif
  for (; str && str[n] && n < room; ++n) {
    char ch = str[n];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);   // the stock screens are upper case
    t.s[n] = ch >= 0x20 && ch <= 0x7E ? ch : '?';
  }
  t.s[n] = 0;
}

#if CORE_UI_TOOLS
CORE_COLD void Canvas::check_overlap() const {
  if (!fit_report) return;
  auto box = [](const TextItem& t, int* x0, int* x1) {
    int w = (int)strlen(t.s) * 4 - 1;   // kRSmall's narrowest advance: only overlaps that are there whatever the glyphs
    *x0 = text_left(t, w), *x1 = *x0 + w - 1;
  };
  for (int i = 0; i < kTextSlots; ++i) {
    if (!txt[i].s[0]) continue;
    int a0, a1;
    box(txt[i], &a0, &a1);
    for (int j = i + 1; j < kTextSlots; ++j) {
      if (!txt[j].s[0]) continue;
      int b0, b1;
      box(txt[j], &b0, &b1);
      if (a1 < b0 || b1 < a0 || txt[i].y + 4 < txt[j].y || txt[j].y + 4 < txt[i].y) continue;
      char m[2 * kTextLen + 4];
      sniprintf(m, sizeof m, "%s / %s", txt[i].s, txt[j].s);
      fit_report(m, 0, 0);
    }
  }
}
#endif

CORE_COLD void Canvas::clear_text(int from, int n) {
  for (int i = from; i < from + n && i < kTextSlots; ++i)
    if (i >= 0) txt[i].s[0] = 0, txt[i].x = txt[i].y = txt[i].font = txt[i].flags = 0;
}

CORE_COLD int Canvas::diff_text(const Canvas& o, uint64_t* mask) const {
  int n = 0;
  *mask = 0;
  for (int i = 0; i < kTextSlots; ++i) {
    const TextItem &a = txt[i], &b = o.txt[i];
    bool same = !strcmp(a.s, b.s) && (!a.s[0] || (a.x == b.x && a.y == b.y && a.font == b.font && a.flags == b.flags));
    if (!same) *mask |= 1ull << i, ++n;
  }
  return n;
}

CORE_COLD int text_left(const TextItem& t, int w) {
  return (t.flags & 3) == kAlignCentre ? t.x - w / 2 : (t.flags & 3) == kAlignRight ? t.x - w : t.x;
}

CORE_COLD void preview(const Canvas& c, uint8_t out[kBytes]) {
#if CORE_UI_TOOLS
  c.check_overlap();
#endif
  static Canvas v;
  memcpy(v.fb, c.fb, kBytes);
  v.unclip();
  for (int i = 0; i < kTextSlots; ++i) {
    const TextItem& t = c.txt[i];
    if (!t.s[0]) continue;
    int w = text_width(kSmall, t.s);
    int x = text_left(t, w);
    Rect box = {(int16_t)x, (int16_t)t.y, (int16_t)w, (int16_t)kSmall.h};
    v.fill(box, kOff);
    text(v, kSmall, x, t.y, t.s, kOn);
    if (t.flags & kTextInvert) v.invert(box);
  }
  memcpy(out, v.fb, kBytes);
}

// sin over a quarter turn in 64 steps, Q7
static UI_ROM const int8_t kSinQ[65] = {
    0,   3,   6,   9,   12,  16,  19,  22,  25,  28,  31,  34,  37,  40,  43,  46,  49,  51,  54,  57,  60,  63,
    65,  68,  71,  73,  76,  78,  81,  83,  85,  88,  90,  92,  94,  96,  98,  100, 102, 104, 106, 107, 109, 111,
    112, 113, 115, 116, 117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127, 127};

CORE_COLD int sin256(int a) {
  a &= 255;
  int q = a >> 6, i = a & 63;
  switch (q) {
    case 0: return kSinQ[i];
    case 1: return kSinQ[64 - i];
    case 2: return -kSinQ[i];
    default: return -kSinQ[64 - i];
  }
}

}  // namespace ui
}  // namespace core
