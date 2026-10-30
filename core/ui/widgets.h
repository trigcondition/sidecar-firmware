// core/ui/widgets.h - the remote UI's widgets (core/ui/README.md): static Syntakt-style graphics drawn from parameter
// values, no animation. Each draws only inside its rectangle (it clears it first and clips to it), so a page redraws
// one widget when its inputs change and the frame diff (Canvas::diff_tiles) sends only the tiles that moved.
// Values are a parameter's integer part, 0..127, unless noted. The grid: the stock 9-row title bar, two rows of four
// 32x22 cells (the Rytm's encoders A..D over E..H, cell()), the stock 9-row bottom bar. Text is the Rytm's (ui.h).
#pragma once
#include <stdint.h>

#include "ui/ui.h"

namespace core {
namespace ui {

// The stock screens' geometry [measured, rytm-emu 1.73 main screen]: a 9-row title bar (0..8, inverted, its text at
// x 2, rows 2..6), two rows of four 32 px cells (dials rows 12..23 / 34..45, labels rows 26..30 / 48..52, centred in
// the column), a 9-row bottom bar (55..63, inverted, text at x 2, rows 57..61). Text in kRSmall, drawn by the Rytm.
constexpr int kHeaderH = 9, kCellW = 32, kCellH = 22, kGridY = 9, kFooterY = 55, kFooterH = 9;
constexpr int kLabelDy = 17;   // a label's top row in its cell
// Text slots (Canvas::put_text): the title bar, the bottom bar, the eight encoder labels, a value per cell.
constexpr int kSlotTitle = 0, kSlotRight = 1, kSlotFoot = 4, kSlotLabels = 8, kSlotCells = 16, kSlotFree = 32;
// The grid cell (col 0..3, row 0..1) spanning cols x rows cells.
inline Rect cell(int col, int row, int cols = 1, int rows = 1) {
  return Rect{(int16_t)(col * kCellW), (int16_t)(kGridY + row * kCellH), (int16_t)(cols * kCellW), (int16_t)(rows * kCellH)};
}

// The title bar: title on the left, `right` (may be null) and the page dots (page of npages) on the right, all in the
// inverted bar as stock's (a clear column between the two parts).
void header(Canvas& c, const char* title, int page, int npages, const char* right = nullptr);
// The bottom bar: text on the left; v >= 0: a level box on the right (bipolar: from its centre).
void footer(Canvas& c, const char* text, int v = -1, bool bipolar = false);
// Text in up to n lines (slots slot..slot + n - 1, rows y, y + dy ..), broken at spaces so each line fits the screen at
// kRSmall's widest advance from x (flags' alignment as put_text's). A word too long for a line is cut there (the
// text-fit check reports it). Returns the lines used; the slots after them are emptied.
int text_lines(Canvas& c, int slot, int n, int x, int y, int dy, const char* s, uint8_t flags = kAlignLeft);
// A name cut in the middle to room characters ("JUNO_A15..STRINGS"): its start and its end both stay readable.
void name_fit(char* out, int cap, const char* s, int room);
// A 3-octave keyboard's window (chrorange): its lowest C (0..84), kept while the notes lo..hi fit in it and else moved
// by the fewest octaves that bring them in (more than 3 octaves from the lowest note's C: from that C); c0 < 0, none yet:
// C3 (48). At most 84 (C6..B8: 120..127 not shown).
int keys_window(int c0, int lo, int hi);
// The labels under a cell's columns: label i in slot kSlotLabels + enc0 + i, centred in its column.
void labels(Canvas& c, const Rect& r, const char* const* text, int n, int enc0);
// A dial in the stock dial's size (a 7 px circle open at the bottom, rows 3..14 of the cell), the pointer, and the
// value as a thick part of the rim (bipolar: from the top, v 64 = centre).
void knob(Canvas& c, const Rect& r, int v, bool bipolar = false);
// A level: the stock LEV fader (a 9 x 13 box filled from the bottom); pan: a horizontal box filled from the centre.
void bar(Canvas& c, const Rect& r, int v, bool pan = false);
// An enum value in the stock enum style: the item `sel` of n ('\0'-separated names, "" ended) as text where the dial
// would be (slot), a short track under it with the selection's place.
void list(Canvas& c, const Rect& r, const char* names, int sel, int slot);
int list_count(const char* names);
const char* list_name(const char* names, int i);   // "" past the end
// A filter's response, 20 Hz..20 kHz over the width, -30..+18 dB: type 0 LP, 1 BP, 2 HP, 3 NOTCH (2-pole), cutoff and
// resonance 0..127 (Q 0.5..12), 0 dB dotted. A few floats a column at redraw time (sqrtf
// and an approximate log2, no libm).
enum FilterType { kLp, kBp, kHp, kNotch, kFilterTypes };
void filter(Canvas& c, const Rect& r, int type, int cutoff, int reso);
// An envelope: kAdsr (a d s r), kAd (a d; d 127 holds), kAhd (a h d); times 0..127 share the width, s the level.
enum EnvShape { kAdsr, kAd, kAhd };
void envelope(Canvas& c, const Rect& r, int shape, int a, int d, int s, int rel, int h = 0);
// An oscillator shape over two cycles: kSine, kTri, kSaw, kSquare, kPulse (width p 0..127), kNoise.
enum Wave { kSine, kTri, kSaw, kSquare, kPulse, kNoise, kWaves };
void wave(Canvas& c, const Rect& r, int shape, int p = 64);
// An LFO: shape (kLfoTri .. kLfoRnd) over two cycles, depth 0..127 bipolar (64 = none, below inverted), the centre
// dotted; the depth's range as a 50 % band.
enum LfoShape { kLfoTri, kLfoSin, kLfoSqr, kLfoSaw, kLfoExp, kLfoRmp, kLfoRnd, kLfoShapes };
void lfo(Canvas& c, const Rect& r, int shape, int depth);
// A sample: peaks[2 i] / peaks[2 i + 1] the min / max (-127..127) of column i of n (resampled to the width); slices
// 0..127 positions (dotted lines); start / end 0..127 the played region (outside it the wave is dithered).
void sample(Canvas& c, const Rect& r, const int8_t* peaks, int n, const uint8_t* slices, int nslices, int start,
            int end);

// shape value (-127..127) of the oscillator / LFO shapes at phase ph (0..255 a cycle), for tests and other widgets
int wave_at(int shape, int ph, int p);
int lfo_at(int shape, int ph);

}  // namespace ui
}  // namespace core
