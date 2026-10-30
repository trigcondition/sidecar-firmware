// core/ui/widgets.cpp - see widgets.h. Cold: a widget is drawn when one of its inputs changed.
#include "ui/widgets.h"

#include <math.h>
#include <string.h>

namespace core {
namespace ui {

static inline int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// The part of a cell above its label: rows 1..15 (the label at kLabelDy).
static inline Rect content(const Rect& r) { return Rect{r.x, (int16_t)(r.y + 1), r.w, (int16_t)(kLabelDy - 2)}; }

static CORE_COLD void begin(Canvas& c, const Rect& r) {
  c.unclip();
  c.fill(r, kOff);
  c.clip(r);
}

CORE_COLD void header(Canvas& c, const char* title, int page, int npages, const char* right) {
  Rect r = {0, 0, kW, kHeaderH};
  begin(c, r);
  c.fill(r, kOn);
  c.put_text(kSlotTitle, kRSmall, 2, 2, title, kTextInvert);
  int dots = npages > 1 ? (npages > 12 ? 12 : npages) : 0;
  int dw = dots * 4, x = kW - 2 - dw;   // 3 px a dot and a gap, a 2 px margin as the title's
  for (int i = 0; i < dots; ++i) {
    Rect d = {(int16_t)(x + 1 + i * 4), 3, 3, 3};
    if (i == page) c.fill(d, kOff);
    else c.pixel(d.x + 1, d.y + 1, kOff);
  }
  if (right && *right) {
    c.put_text(kSlotRight, kRSmall, x - 1, 2, right, kAlignRight | kTextInvert);
    x -= 1 + (int)strlen(right) * kRSmall.advance + 2;
  } else {
    c.put_text(kSlotRight, kRSmall, 0, 0, nullptr);
  }
  if (dots || (right && *right)) c.vline(x, r.y, r.y + r.h - 1, kOff);   // stock's separator
  c.unclip();
}

CORE_COLD void footer(Canvas& c, const char* text, int v, bool bipolar) {
  Rect r = {0, kFooterY, kW, kFooterH};
  begin(c, r);
  c.fill(r, kOn);
  c.put_text(kSlotFoot, kRSmall, 2, kFooterY + 2, text, kTextInvert);
#if CORE_UI_TOOLS
  if (v >= 0 && text && Canvas::fit_report && (int)strlen(text) * kRSmall.advance - 1 > 88 - 2 - 2)   // into the level box
    Canvas::fit_report(text, (int)strlen(text) * kRSmall.advance - 1, 84);
#endif
  if (v >= 0) {   // a level box in the bar's right part, as stock's LEV box: clear outline, clear fill
    v = clamp(v, 0, 127);
    Rect o = {88, (int16_t)(kFooterY + 2), 38, 5};
    c.rect(o, kOff);
    int inner = o.w - 4, x0 = o.x + 2;
    if (bipolar) {
      int mid = x0 + inner / 2, at = x0 + (v * inner + 63) / 127;
      c.fill(Rect{(int16_t)(at < mid ? at : mid), (int16_t)(o.y + 2), (int16_t)(at < mid ? mid - at : at - mid), 1}, kOff);
      c.vline(mid, o.y + 1, o.y + 3, kOff);
    } else {
      c.fill(Rect{(int16_t)x0, (int16_t)(o.y + 2), (int16_t)((v * inner + 63) / 127), 1}, kOff);
    }
  }
  c.unclip();
}

CORE_COLD int text_lines(Canvas& c, int slot, int n, int x, int y, int dy, const char* s, uint8_t flags) {
  int a = flags & 3;
  int room = a == kAlignLeft ? (kW - x + 1) / kRSmall.advance : a == kAlignRight ? (x + 2) / kRSmall.advance : kW / kRSmall.advance;
  if (room > kTextLen) room = kTextLen;
  int used = 0;
  while (s && *s == ' ') ++s;
  for (; used < n && s && *s; ++used) {
    char b[kTextLen + 1];
    int len = (int)strlen(s), take = len;
    if (len > room) {   // the last space that keeps the line in room (none: the word is cut)
      take = room;
      for (int k = room; k > 0; --k)
        if (s[k] == ' ') { take = k; break; }
    }
    if (used == n - 1) take = len;   // the last line: the rest (cut and reported by put_text when too long)
    int m = take < kTextLen ? take : kTextLen;
    memcpy(b, s, (size_t)m), b[m] = 0;
    if (take > kTextLen) {   // (put_text sees only kTextLen characters: report the whole rest here)
#if CORE_UI_TOOLS
      if (Canvas::fit_report) Canvas::fit_report(s, take, room);
#endif
    }
    c.put_text(slot + used, kRSmall, x, y + used * dy, b, flags);
    s += take;
    while (*s == ' ') ++s;
  }
  for (int i = used; i < n; ++i) c.put_text(slot + i, kRSmall, 0, 0, nullptr);
  return used;
}

CORE_COLD void name_fit(char* out, int cap, const char* s, int room) {
  if (!s) s = "";
  if (room > cap - 1) room = cap - 1;
  int len = (int)strlen(s);
  if (len <= room || room < 5) {
    int m = len < room ? len : room;
    memcpy(out, s, (size_t)m), out[m] = 0;
    return;
  }
  int head = (room - 2 + 1) / 2, tail = room - 2 - head;
  memcpy(out, s, (size_t)head);
  out[head] = '.', out[head + 1] = '.';
  memcpy(out + head + 2, s + len - tail, (size_t)tail);
  out[room] = 0;
}

CORE_COLD void labels(Canvas& c, const Rect& r, const char* const* t, int n, int enc0) {
  int cols = r.w / kCellW;
  if (cols < 1) cols = 1;
  for (int i = 0; i < n && i < cols; ++i) {
    int e = enc0 + i;
    if (e < 0 || e > 7) continue;
    c.put_text(kSlotLabels + e, kRSmall, r.x + i * kCellW + kCellW / 2, r.y + kLabelDy, t[i] && *t[i] ? t[i] : nullptr,
               kAlignCentre);
  }
}

// The stock dial: a midpoint circle of radius 7 centred at (cx, cy) without its points more than 4 rows below the
// centre [measured: the main screen's dials, rows 12..23 of a cell at row 9].
static CORE_COLD void dial_rim(Canvas& c, int cx, int cy) {
  int x = 7, y = 0, d = 1 - 7;
  while (x >= y) {
    const int px[8] = {x, y, -y, -x, -x, -y, y, x}, py[8] = {y, x, x, y, -y, -x, -x, -y};
    for (int i = 0; i < 8; ++i)
      if (py[i] <= 4) c.pixel(cx + px[i], cy + py[i], kOn);
    ++y;
    if (d < 0) d += 2 * y + 1;
    else --x, d += 2 * (y - x) + 1;
  }
}

static inline int rpt(int c, int r, int s) { return c + (r * s + (s >= 0 ? 63 : -63)) / 127; }

CORE_COLD void knob(Canvas& c, const Rect& r, int v, bool bipolar) {
  begin(c, r);
  v = clamp(v, 0, 127);
  int cx = r.x + kCellW / 2, cy = r.y + 10;
  dial_rim(c, cx, cy);
  int a = -96 + v * 192 / 127;   // 256 a turn, 0 up, clockwise: 270 degrees from down-left to down-right
  int from = bipolar ? 0 : -96, lo = a < from ? a : from, hi = a < from ? from : a;
  for (int t = lo; t <= hi; ++t) c.pixel(rpt(cx, 6, sin256(t)), rpt(cy, 6, -cos256(t)), kOn);   // the value: rim x 2
  c.line(cx, cy, rpt(cx, 5, sin256(a)), rpt(cy, 5, -cos256(a)), kOn);
  c.unclip();
}

CORE_COLD void bar(Canvas& c, const Rect& r, int v, bool pan) {
  begin(c, r);
  v = clamp(v, 0, 127);
  int cx = r.x + kCellW / 2, top = r.y + 3;
  if (pan) {   // a 17 x 7 box, filled from its centre mark
    Rect o = {(int16_t)(cx - 8), (int16_t)(top + 3), 17, 7};
    c.rect(o, kOn);
    int inner = o.w - 4, x0 = o.x + 2, mid = cx, at = x0 + (v * inner + 63) / 127;
    if (at > mid) c.fill(Rect{(int16_t)(mid + 1), (int16_t)(o.y + 2), (int16_t)(at - mid), 3}, kOn);
    else if (at < mid) c.fill(Rect{(int16_t)at, (int16_t)(o.y + 2), (int16_t)(mid - at), 3}, kOn);
    c.vline(mid, o.y - 1, o.y + o.h, kOn);
  } else {     // stock's LEV: a 9 x 13 box, its inside filled from the bottom
    Rect o = {(int16_t)(cx - 4), (int16_t)(top - 1), 9, 13};
    c.rect(o, kOn);
    int ih = o.h - 4, h = (v * ih + 63) / 127;
    if (h) c.fill(Rect{(int16_t)(o.x + 2), (int16_t)(o.y + 2 + ih - h), 5, (int16_t)h}, kOn);
    if (h < ih) c.fill(Rect{(int16_t)(o.x + 2), (int16_t)(o.y + 2), 5, (int16_t)(ih - h)}, kDither);
  }
  c.unclip();
}

CORE_COLD int list_count(const char* names) {
  int n = 0;
  while (*names) names += strlen(names) + 1, ++n;
  return n;
}

CORE_COLD const char* list_name(const char* names, int i) {
  while (*names && i-- > 0) names += strlen(names) + 1;
  return names;
}

CORE_COLD void list(Canvas& c, const Rect& r, const char* names, int sel, int slot) {
  begin(c, r);
  int n = list_count(names);
  sel = clamp(sel, 0, n > 0 ? n - 1 : 0);
  int cx = r.x + r.w / 2;
  c.put_text(slot, kRSmall, cx, r.y + 7, n ? list_name(names, sel) : "-", kAlignCentre);   // where the dial is
  if (n > 1) {   // the track: the whole list under the text, the selection's place solid
    int x0 = cx - 10, w = 21, y = r.y + 14;
    for (int x = x0; x < x0 + w; x += 2) c.pixel(x, y, kOn);
    int at = x0 + sel * (w - 3) / (n - 1);
    c.hline(at, at + 2, y, kOn);
    c.hline(at, at + 2, y - 1, kOn);
  }
  c.unclip();
}

// log2(x) for x > 0 to about 0.01 (a plot's precision): the float's exponent, and the mantissa on a parabola. No libm
// (log10f / exp2f would pull newlib's code and tables into the Teensy's RAM1).
static CORE_COLD float log2_approx(float x) {
  uint32_t b;
  memcpy(&b, &x, 4);
  float m = (float)(b & 0x7FFFFF) * (1.0f / 8388608.0f);   // 0..1
  return (float)((int)(b >> 23 & 0xFF) - 127) + m * (1.3465f - 0.3465f * m);
}

static CORE_COLD float exp2_approx(float x) {   // 2^x, |x| < 100, to about 0.5 %
  int i = (int)(x < 0 ? x - 1.0f : x);
  float f = x - (float)i;   // 0..1
  float m = 1.0f + f * (0.6565f + 0.3435f * f);
  uint32_t b;
  memcpy(&b, &m, 4);
  b += (uint32_t)i << 23;
  memcpy(&m, &b, 4);
  return m;
}

CORE_COLD void filter(Canvas& c, const Rect& r, int type, int cutoff, int reso) {
  begin(c, r);
  Rect k = content(r);
  type = clamp(type, 0, kFilterTypes - 1);
  const float top_db = 18.0f, bot_db = -30.0f;
  float q = 0.5f + clamp(reso, 0, 127) * (11.5f / 127.0f);
  float oct_c = clamp(cutoff, 0, 127) * (10.0f / 127.0f);   // 20 Hz .. 20 kHz: 10 octaves
  int w = k.w;
  float ratio = exp2_approx(10.0f / (w > 1 ? w - 1 : 1));
  float f = exp2_approx(-oct_c);   // column 0: f / fc
  int y0db = k.y + (int)((top_db - 0.0f) / (top_db - bot_db) * (k.h - 1) + 0.5f);
  c.hline(k.x, k.x + k.w - 1, y0db, kDither);
  int prev = -1;
  for (int i = 0; i < w; ++i, f *= ratio) {
    float f2 = f * f, re = 1.0f - f2, im = f / q;
    float den = sqrtf(re * re + im * im);
    float num2 = type == kLp ? 1.0f : type == kHp ? f2 : type == kBp ? im : (re < 0 ? -re : re);
    float mag = den > 1e-9f ? num2 / den : 1e3f;
    float db = mag > 1e-6f ? 6.0206f * log2_approx(mag) : bot_db;
    if (db > top_db) db = top_db;
    if (db < bot_db) db = bot_db;
    int y = k.y + (int)((top_db - db) / (top_db - bot_db) * (k.h - 1) + 0.5f);
    if (prev >= 0) c.vline(k.x + i, prev < y ? prev : y, prev < y ? y : prev, kOn);   // steep parts joined
    else c.pixel(k.x + i, y, kOn);
    if (y < k.y + k.h - 1) c.vline(k.x + i, y + 2, k.y + k.h - 1, kDither);   // the pass band's area, 50 %
    prev = y;
  }
  int xc = k.x + (int)(oct_c / 10.0f * (w - 1) + 0.5f);   // the cutoff
  c.vline(xc, k.y + k.h - 3, k.y + k.h - 1, kOn);
  c.unclip();
}

// An RC-like segment from (x0, y0) to (x1, y1): fast at the start, slow at the end (quadratic, integer). An
// envelope's attack and its decays alike.
static CORE_COLD void curve(Canvas& c, int x0, int y0, int x1, int y1) {
  int w = x1 - x0;
  if (w <= 0) { c.line(x0, y0, x1, y1, kOn); return; }
  int px = x0, py = y0;
  for (int i = 1; i <= w; ++i) {
    int t = w - i;   // remaining
    int y = y1 + (y0 - y1) * t * t / (w * w);
    c.line(px, py, x0 + i, y, kOn);
    px = x0 + i, py = y;
  }
}

CORE_COLD void envelope(Canvas& c, const Rect& r, int shape, int a, int d, int s, int rel, int h) {
  begin(c, r);
  Rect k = content(r);
  a = clamp(a, 0, 127), d = clamp(d, 0, 127), s = clamp(s, 0, 127), rel = clamp(rel, 0, 127), h = clamp(h, 0, 127);
  int x = k.x + 1, top = k.y + 1, bot = k.y + k.h - 1, W = k.w - 3;
  int ys = bot - s * (bot - top) / 127;
  c.hline(k.x, k.x + k.w - 1, bot, kDither);
  if (shape == kAd) {
    bool hold = d >= 127;
    int wa = a * W / 2 / 127, wd = hold ? W - wa : 2 + d * (W - wa - 2) / 127;
    curve(c, x, bot, x + wa, top);
    if (hold) c.hline(x + wa, x + W, top, kOn);
    else curve(c, x + wa, top, x + wa + wd, bot);
  } else if (shape == kAhd) {
    int wa = a * W / 3 / 127, wh = h * W / 3 / 127, wd = 2 + d * (W / 3) / 127;
    curve(c, x, bot, x + wa, top);
    c.hline(x + wa, x + wa + wh, top, kOn);
    curve(c, x + wa + wh, top, x + wa + wh + wd, bot);
  } else {   // ADSR: a d r a quarter of the width each at most, the sustain the rest
    int q = W / 4;
    int wa = a * q / 127, wd = 1 + d * q / 127, wr = 1 + rel * q / 127;
    int xs = x + wa + wd, xr = x + W - wr;
    curve(c, x, bot, x + wa, top);
    curve(c, x + wa, top, xs, ys);
    c.hline(xs, xr, ys, kOn);
    curve(c, xr, ys, x + W, bot);
    c.vline(xr, ys + 2, bot, kDither);   // the gate's end
  }
  // the area under the curve, 50 %: column by column below the first set pixel
  for (int i = k.x + 1; i < k.x + k.w - 1; ++i) {
    int y = top;
    while (y < bot && !c.get(i, y)) ++y;
    for (int yy = y + 2; yy < bot; ++yy)
      if (((i + yy) & 1) == 0) c.pixel(i, yy, kOn);
  }
  c.unclip();
}

static CORE_COLD uint32_t rnd(uint32_t x) {   // a fixed hash (noise / random shapes are static: the same picture each draw)
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

CORE_COLD int wave_at(int shape, int ph, int p) {
  ph &= 255;
  switch (shape) {
    case kSine: return sin256(ph);
    case kTri: return ph < 64 ? ph * 2 : ph < 192 ? 127 - (ph - 64) * 2 : (ph - 192) * 2 - 127;
    case kSaw: return 127 - ph;
    case kSquare: return ph < 128 ? 127 : -127;
    case kPulse: { int w = 8 + clamp(p, 0, 127) * 240 / 127; return ph < w ? 127 : -127; }
    default: return (int)(rnd((uint32_t)ph * 2654435761u) & 255) - 128;
  }
}

CORE_COLD int lfo_at(int shape, int ph) {
  ph &= 255;
  switch (shape) {
    case kLfoTri: return wave_at(kTri, ph, 0);
    case kLfoSin: return sin256(ph);
    case kLfoSqr: return ph < 128 ? 127 : -127;
    case kLfoSaw: return ph * 254 / 255 - 127;
    case kLfoExp: { int t = 255 - ph; return t * t / 256 - 127; }   // a fast fall, then slower
    case kLfoRmp: return 127 - ph * 254 / 255;
    default: return (int)(rnd((uint32_t)(ph >> 5) + 17) & 255) - 128;   // sample and hold: 8 steps a cycle
  }
}

static CORE_COLD void shape_plot(Canvas& c, const Rect& k, int (*at)(int, int, int), int shape, int p, int depth) {
  int mid = k.y + k.h / 2, amp = (k.h - 2) / 2;
  c.hline(k.x, k.x + k.w - 1, mid, kDither);
  int prev = 0;
  for (int i = 0; i < k.w; ++i) {
    int ph = i * 512 / k.w;   // two cycles
    int v = at(shape, ph, p) * depth / 127;
    int y = mid - v * amp / 127;
    if (i) {
      int a = prev, b = y;
      if (a > b) { int t = a; a = b; b = t; }
      c.vline(k.x + i, a, b, kOn);   // steps drawn as vertical edges
    }
    c.pixel(k.x + i, y, kOn);
    prev = y;
  }
}

static CORE_COLD int wave3(int s, int ph, int p) { return wave_at(s, ph, p); }
static CORE_COLD int lfo3(int s, int ph, int) { return lfo_at(s, ph); }

CORE_COLD void wave(Canvas& c, const Rect& r, int shape, int p) {
  begin(c, r);
  Rect k = content(r);
  k.x += 1, k.w -= 2;
  shape_plot(c, k, wave3, clamp(shape, 0, kWaves - 1), p, 127);
  c.unclip();
}

CORE_COLD void lfo(Canvas& c, const Rect& r, int shape, int depth) {
  begin(c, r);
  Rect k = content(r);
  k.x += 1, k.w -= 2;
  int dep = (clamp(depth, 0, 127) - 64) * 127 / 63;
  if (dep > 127) dep = 127;
  int mid = k.y + k.h / 2, amp = (k.h - 2) / 2, span = (dep < 0 ? -dep : dep) * amp / 127;
  if (span > 0) c.fill(Rect{k.x, (int16_t)(mid - span), k.w, (int16_t)(2 * span + 1)}, kDither);   // the range
  c.fill(Rect{k.x, (int16_t)(mid - span + 1), k.w, (int16_t)(span > 0 ? 2 * span - 1 : 0)}, kOff);
  shape_plot(c, k, lfo3, clamp(shape, 0, kLfoShapes - 1), 0, dep);
  c.unclip();
}

CORE_COLD void sample(Canvas& c, const Rect& r, const int8_t* peaks, int n, const uint8_t* slices, int nslices,
                      int start, int end) {
  begin(c, r);
  Rect k = content(r);
  int mid = k.y + k.h / 2, amp = (k.h - 1) / 2;
  int xs = k.x + clamp(start, 0, 127) * (k.w - 1) / 127, xe = k.x + clamp(end, 0, 127) * (k.w - 1) / 127;
  for (int i = 0; i < k.w && n > 0; ++i) {
    int j = i * n / k.w;
    int lo = peaks[2 * j], hi = peaks[2 * j + 1];
    int y0 = mid - hi * amp / 127, y1 = mid - lo * amp / 127;
    bool in = k.x + i >= xs && k.x + i <= xe;
    c.vline(k.x + i, y0, y1, in ? kOn : kDither);
  }
  if (n <= 0) c.hline(k.x, k.x + k.w - 1, mid, kDither);
  for (int s = 0; s < nslices; ++s) {
    int x = k.x + clamp(slices[s], 0, 127) * (k.w - 1) / 127;
    for (int y = k.y; y < k.y + k.h; y += 2) c.pixel(x, y, kXor);
  }
  c.vline(xs, k.y, k.y + k.h - 1, kOn);
  c.vline(xe, k.y, k.y + k.h - 1, kOn);
  c.unclip();
}

CORE_COLD int keys_window(int c0, int lo, int hi) {
  if (c0 < 0) c0 = 48;
  if (lo > hi) return c0;   // no notes: where it was
  int lc = lo / 12 * 12;   // (review: never above the lowest note's C, "too wide" measured from it)
  if (hi - lc > 35 || lo < c0) c0 = lc;
  else if (hi > c0 + 35) c0 = hi / 12 * 12 - 24 > lc ? lc : hi / 12 * 12 - 24;
  return c0 > 84 ? 84 : c0;
}

}  // namespace ui
}  // namespace core
