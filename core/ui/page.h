// core/ui/page.h - a page declaration: which widget where on the 4 x 2 grid, which parameters feed it, the encoders'
// parameters and labels (core/ui/README.md, "Page format"). Plain constant data (program flash on the Teensy): the
// built-in machine table's pages are in pages.cpp, a plugin declares its own the same way (or as the JSON the README
// describes, which maps 1:1 onto these structs). PageView draws a page and redraws only the widgets whose inputs
// changed since its last draw.
#pragma once
#include <stdint.h>

#include "ui/ui.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

constexpr uint8_t kNone = 0xFF;   // an unused input / encoder
constexpr int kMaxCells = 12;

enum Kind : uint8_t {
  kKnob,     // in[0] the value
  kKnobBi,   // in[0] the value, shown as -64..+63
  kBar,      // in[0] the level
  kPan,      // in[0] the position, 64 = centre
  kList,     // in[0] the value in list_count(names) equal zones (eng_zone: the curated ENG lists), names
  kFilter,   // in[0] cutoff, in[1] resonance, in[2] type in 4 zones (LP BP HP NOTCH) or kNone: arg = the type
  kEnv,      // arg = EnvShape; kAdsr in[0..3] = A D S R, kAd in[0..1] = A D, kAhd in[0..2] = A H D (kNone: 0)
  kWave,     // in[0] the shape in kWaves zones or kNone: arg = the shape; in[1] pulse width
  kLfo,      // in[0] the shape in kLfoShapes zones or kNone: arg = the shape; in[1] depth (64 = none)
  kSample,   // arg = the Source's peak set; in[0] start, in[1] end (kNone: 0 / 127)
  kText,     // arg = the Source's first text line; one line of the small font every 6 px
  kDraw,     // drawn by code (extra.h): arg = its DrawId, in[] its parameters
  kKinds
};

struct Cell {
  uint8_t kind;                  // Kind
  uint8_t col, row, cols, rows;  // the grid: col 0..3, row 0..1, span (cell() in widgets.h)
  uint8_t in[4];                 // parameter numbers 0..15 feeding it (the voice's P1..P16 = 0..15), kNone unused
  uint8_t arg;                   // per kind (above)
  const char* names;             // kList: '\0'-separated names ending in an empty one; else null
};

struct Page {
  char title[12];        // the header ("PLAITS"); the remote host adds the voice ("V3") on the right
  uint8_t machine;       // the voice byte's machine (machines.h: voice byte & 0x78) this page is for; kNone: none
  uint8_t ncells;
  const Cell* cells;
  uint8_t enc[8];        // encoder A..H (A..D the top row) -> parameter 0..15, kNone: none
  char label[8][5];      // each encoder's label, drawn under its grid column (row 0: A..D, row 1: E..H)
};

// What a page reads: parameter values (0..127, the integer part), and for kSample / kText their data.
class Source {
 public:
  virtual int param(int k) const = 0;
  // peak set `id`: n columns of (min, max) into *peaks, slice positions (0..127) into *slices / *nslices; 0: none
  virtual int peaks(int id, const int8_t** peaks, const uint8_t** slices, int* nslices) const;   // default: none
  virtual const char* text(int id) const;   // default: ""
  virtual int voice() const;                // the voice it reads (0..7), -1 none (default)
  // rui1038: P(k+1) as a source code (GRANULAR SRC / WAVETABLE TBL / MULTISAMPLE INST: bank * 128 + index, the bank
  // the fraction's top 3 bits; core/lib/srclib.h): default the voice's 14-bit report, else param(k) (bank A)
  virtual int code(int k) const;
};

class PageView {
 public:
  void invalidate() { valid_ = false; }
  // Draws `p` into c from s: the header (title, `right`, page dots) and every widget, but only those whose inputs
  // changed since the last draw of this page (all after invalidate() or another page). Returns the widgets drawn.
  int draw(Canvas& c, const Page& p, const Source& s, const char* right, int page, int npages);

 private:
  uint32_t key(const Page& p, const Cell& cell, const Source& s) const;
  const Page* page_ = nullptr;
  bool valid_ = false;
  uint32_t head_ = 0;
  uint32_t keys_[kMaxCells] = {};
};

// The encoder's new value: one zone a step on a kList cell's parameter (its zone's centre), else v + delta (x 8 with
// FUNC held), 0..127.
int step_value(const Page& p, int param, int v, int delta, bool func);
// The built-in machine table's pages (pages.cpp): the page for a voice byte's machine (null: none), the SIDECAR status
// page, and the widget gallery (demo 0 / 1: every widget kind, for core/ui's renderer and tests).
const Page* page_for_machine(int machine);
extern const Page kSidecarPage;
extern const Page kDemoPages[2];

}  // namespace ui
}  // namespace core
