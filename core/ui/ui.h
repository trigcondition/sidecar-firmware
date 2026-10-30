// core/ui/ui.h - the remote UI's 1-bit drawing (docs/superpowers/specs/2026-10-16-remote-ui-research.md; core/ui/README.md):
// a 128x64 canvas in the Rytm's framebuffer layout, so a tile of it is the 8 bytes the Rytm's panel takes and a frame
// is sent as-is (rytm-mods 0026-remote-ui). Glass pixel (x, y from the top-left) = byte (7 - y / 8) + 8 x, bit y % 8.
// No allocation, no floats here (the curve widgets use a few at redraw time, widgets.cpp); every function is cold
// (program flash on the Teensy). Fonts: core/ui/fonts (original glyphs, CC0).
#pragma once
#include <stdint.h>

#include "platform.h"

#ifdef PROGMEM
#define UI_ROM PROGMEM   // the Teensy: tables in program flash, read in place
#else
#define UI_ROM
#endif

namespace core {
namespace ui {

constexpr int kW = 128, kH = 64, kBytes = 1024;
constexpr int kTileCols = 16, kTilePages = 8, kTiles = kTileCols * kTilePages;

struct Rect {
  int16_t x, y, w, h;
};

// ---- text the Rytm draws (rytm-mods 0026 1021, 3D): the Sidecar never draws the Rytm's glyphs and has none of them.
// A frame is pixels + up to kTextSlots text items; the Rytm draws each item with its own text routine in one of its
// own fonts over the committed pixels. The host keeps the items in the canvas beside the pixels and sends the slots
// that changed with the frame (remote.cpp). kRSmall: the stock screens' font (title bar, knob labels, the bottom
// bar, enum values) [measured in rytm-emu 1.73: font 16 draws the stock main screen's "TUN" and title bar pixel for
// pixel]. For previews and tests without a Rytm, preview() draws the items with kSmall (core/ui/fonts, CC0) in the
// same boxes, which has the same height and advance.
constexpr int kTextSlots = 64, kTextLen = 32;
enum TextFlag : uint8_t { kAlignLeft = 0, kAlignCentre = 1, kAlignRight = 2, kTextInvert = 4 };
struct RFont {
  uint8_t id;        // the Rytm's font object (0..16, boot order: 0x417e381c + 8 id in 1.73)
  uint8_t h, desc;   // rows from the top of the item, rows under them (descenders) [measured]
  uint8_t advance;   // a character's widest advance (glyph and gap), for bounding a string's length [measured]
};
constexpr RFont kRSmall = {16, 5, 0, 5};   // 4 to 5 px a character ("T" 4, "U" 5) [measured, rytm-emu 1.73]
struct TextItem {
  uint8_t x, y, font, flags;   // glass x (where the alignment puts it), the top row, RFont::id, TextFlag bits
  char s[kTextLen + 1];        // "" empty slot
};

enum Ink : uint8_t {
  kOff,      // clear
  kOn,       // set
  kXor,      // invert
  kDither,   // 50 %: set where (x + y) is even, clear elsewhere (fills); every other pixel (lines)
};

class Canvas {
 public:
  uint8_t fb[kBytes];   // the Rytm's layout (above)

  Canvas();
  void clear(bool on = false);
  void clip(const Rect& r);   // drawing outside r is dropped (intersected with the screen)
  void unclip();
  const Rect& clip_rect() const { return clip_; }

  void pixel(int x, int y, Ink ink = kOn);
  bool get(int x, int y) const;
  void hline(int x0, int x1, int y, Ink ink = kOn);
  void vline(int x, int y0, int y1, Ink ink = kOn);
  void line(int x0, int y0, int x1, int y1, Ink ink = kOn);   // Bresenham; kDither: every other pixel
  void rect(const Rect& r, Ink ink = kOn);                     // outline
  void fill(const Rect& r, Ink ink = kOn);
  void invert(const Rect& r) { fill(r, kXor); }
  void dither(const Rect& r) { fill(r, kDither); }
  // A 1-bit glyph: w columns of one byte each (bit 0 the top row, h <= 8), at (x, y); set bits drawn with ink (kOn,
  // kOff, kXor), clear bits untouched. scale 1 or 2.
  void blit(const uint8_t* cols, int w, int h, int x, int y, Ink ink = kOn, int scale = 1);

  // The tiles (column c 0..15, page p 0..7: glass rows 56 - 8 p .. 63 - 8 p) that differ from `other`: bit (p * 16 + c)
  // of mask[0..3]. Returns how many.
  int diff_tiles(const Canvas& other, uint32_t mask[4]) const;

  // The Rytm-drawn text (above). put_text: slot 0..kTextSlots-1, cut to fit the screen at f's advance; s null or "":
  // the slot empty. diff_text: the slots that differ from `other` as a bit mask, how many.
  TextItem txt[kTextSlots];
  void put_text(int slot, const RFont& f, int x, int y, const char* s, uint8_t flags = kAlignLeft);
#if CORE_UI_TOOLS
  // The text-fit check (UX review R1; core/tests/check.h installs it): called for every item put_text cuts (more
  // characters than fit at f's widest advance) or that runs past the bottom row, with the whole string.
  static void (*fit_report)(const char* s, int need, int room);
  // Items whose boxes (at the narrowest advance, 4 px) overlap each other: reported as "A / B" (remote.cpp, each frame sent).
  void check_overlap() const;
#endif
  void clear_text(int from = 0, int n = kTextSlots);
  int diff_text(const Canvas& other, uint64_t* mask) const;

 private:
  bool clipped(int x, int y) const {
    return x < clip_.x || y < clip_.y || x >= clip_.x + clip_.w || y >= clip_.y + clip_.h;
  }
  Rect clip_ = {0, 0, kW, kH};
};

// ---- text (font.cpp)
struct Font {
  uint8_t w, h, advance;   // glyph size, pixels to the next glyph
  const uint8_t* data;     // ASCII 32..95, w bytes a glyph
};
extern const Font kSmall;    // 3x5, advance 4: labels
extern const Font kMedium;   // 5x7, advance 6: values, titles
// kMedium at scale 2 (10x14) is the large font (headers, big values).

// Text from (x, y) = its top-left; lower case drawn as upper case, other characters as '?'. Returns the x after it.
int text(Canvas& c, const Font& f, int x, int y, const char* s, Ink ink = kOn, int scale = 1);
int text_width(const Font& f, const char* s, int scale = 1);   // without the trailing gap
// Centred in r horizontally (and vertically when y < 0), clipped to r.
void text_in(Canvas& c, const Font& f, const Rect& r, const char* s, int y = -1, Ink ink = kOn, int scale = 1);

// fb with c's text items drawn in the CC0 small font, each in the box the Rytm clears for it (its width, h + desc
// rows), inverted ones as clear text on a set box: what the Rytm shows, but for the glyphs.
void preview(const Canvas& c, uint8_t out[kBytes]);
// The left x of an item's box as the Rytm aligns it (with a nominal width: the preview's).
int text_left(const TextItem& t, int w);

// Integer helpers for the widgets: sin of a turn in 256 steps, Q7 (-127..127).
int sin256(int a);
inline int cos256(int a) { return sin256(a + 64); }

}  // namespace ui
}  // namespace core
