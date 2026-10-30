// core/ui/pages.cpp - see page.h: PageView, and the built-in machine table's pages (PLAITS; SIDECAR status; the widget
// gallery). Cold.
#include <string.h>

#include "lib/srclib.h"
#include "machines.h"
#include "ui/extra.h"
#include "ui/page.h"
#include "voices.h"

namespace core {
namespace ui {

CORE_COLD int Source::peaks(int, const int8_t**, const uint8_t**, int*) const { return 0; }
CORE_COLD const char* Source::text(int) const { return ""; }
CORE_COLD int Source::voice() const { return -1; }
CORE_COLD int Source::code(int k) const {
  int vi = voice();
  if (vi < 0) return param(k);
  return srclib::code_of(v[vi & 7].p[k & 15]);
}

static inline uint32_t mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static CORE_COLD uint32_t mix_str(uint32_t h, const char* s) {
  for (; s && *s; ++s) h = mix(h, (uint8_t)*s);
  return mix(h, 0);
}

static CORE_COLD int in_val(const Source& s, uint8_t k, int dflt) { return k == kNone ? dflt : s.param(k); }

CORE_COLD uint32_t PageView::key(const Page& p, const Cell& c, const Source& s) const {
  uint32_t h = mix(2166136261u, c.kind);
  h = mix(h, c.arg);
  switch (c.kind) {
    case kList: h = mix(h, (uint32_t)eng_zone((float)in_val(s, c.in[0], 0), list_count(c.names))); break;
    case kText:
      for (int i = 0; i < c.rows * kCellH / 6; ++i) h = mix_str(h, s.text(c.arg + i));
      break;
    case kSample: {
      const int8_t* pk = nullptr;
      const uint8_t* sl = nullptr;
      int ns = 0, n = s.peaks(c.arg, &pk, &sl, &ns);
      h = mix(h, (uint32_t)n);
      for (int i = 0; i < 2 * n; ++i) h = mix(h, (uint8_t)pk[i]);
      for (int i = 0; i < ns; ++i) h = mix(h, sl[i]);
      for (int i = 0; i < 2; ++i) h = mix(h, (uint32_t)in_val(s, c.in[i], 0x100));   // start / end
      break;
    }
    case kDraw:
      h = mix(h, draw_key(c, s));
      for (int i = 0; i < 4; ++i) h = mix(h, (uint32_t)in_val(s, c.in[i], 0x100));
      break;
    default:
      for (int i = 0; i < 4; ++i) h = mix(h, (uint32_t)in_val(s, c.in[i], 0x100));
  }
  for (int i = 0; i < c.cols; ++i) {   // its labels
    int e = (c.row + c.rows - 1) * 4 + c.col + i;
    h = mix_str(h, e < 8 ? p.label[e] : "");
  }
  return h;
}

static CORE_COLD void draw_cell(Canvas& cv, const Page& p, int ci, const Source& s) {
  const Cell& c = p.cells[ci];
  Rect r = cell(c.col, c.row, c.cols, c.rows);
  int v0 = in_val(s, c.in[0], 0);
  switch (c.kind) {
    case kKnob: knob(cv, r, v0); break;
    case kKnobBi: knob(cv, r, v0, true); break;
    case kBar: bar(cv, r, v0); break;
    case kPan: bar(cv, r, v0, true); break;
    case kList: list(cv, r, c.names, eng_zone((float)v0, list_count(c.names)), kSlotCells + ci); break;
    case kFilter:
      filter(cv, r, c.in[2] == kNone ? c.arg : eng_zone((float)s.param(c.in[2]), kFilterTypes), v0,
             in_val(s, c.in[1], 0));
      break;
    case kEnv:
      if (c.arg == kAhd)
        envelope(cv, r, kAhd, v0, in_val(s, c.in[2], 0), 0, 0, in_val(s, c.in[1], 0));
      else
        envelope(cv, r, c.arg, v0, in_val(s, c.in[1], 0), in_val(s, c.in[2], 0), in_val(s, c.in[3], 0));
      break;
    case kWave: wave(cv, r, c.in[0] == kNone ? c.arg : eng_zone((float)v0, kWaves), in_val(s, c.in[1], 64)); break;
    case kLfo: lfo(cv, r, c.in[0] == kNone ? c.arg : eng_zone((float)v0, kLfoShapes), in_val(s, c.in[1], 127)); break;
    case kSample: {
      const int8_t* pk = nullptr;
      const uint8_t* sl = nullptr;
      int ns = 0, n = s.peaks(c.arg, &pk, &sl, &ns);
      sample(cv, r, pk, n, sl, ns, in_val(s, c.in[0], 0), in_val(s, c.in[1], 127));
      break;
    }
    case kText: {   // a line every 6 rows (the Rytm's small font and a row), slots kSlotFree + arg + line
      cv.unclip();
      cv.fill(r, kOff);
      for (int i = 0; i < r.h / 6; ++i)
        cv.put_text(kSlotFree + c.arg + i, kRSmall, r.x + 2, r.y + 1 + 6 * i, s.text(c.arg + i));
      return;   // no labels
    }
    case kDraw:
      draw_custom(cv, r, c, s, kSlotCells + ci);
      if (c.arg == kDrawChord) return;   // the chord's keyboard takes the labels' rows (a turn: the bottom bar names it)
      if (c.arg == kDrawGrList || c.arg == kDrawWtList || c.arg == kDrawMsList || c.arg == kDrawBankList) return;   // the list's rows: no labels
      break;
    default: return;
  }
  const char* l[4];
  int n = c.cols > 4 ? 4 : c.cols;
  for (int i = 0; i < n; ++i) {
    int e = (c.row + c.rows - 1) * 4 + c.col + i;
    l[i] = e < 8 ? p.label[e] : "";
  }
  labels(cv, r, l, n, (c.row + c.rows - 1) * 4 + c.col);
}

CORE_COLD int PageView::draw(Canvas& c, const Page& p, const Source& s, const char* right, int page, int npages) {
  if (page_ != &p) valid_ = false, page_ = &p;
  if (!valid_) c.clear();
  int drawn = 0;
  uint32_t hk = mix(mix(mix_str(mix_str(2166136261u, p.title), right), (uint32_t)page), (uint32_t)npages);
  if (!valid_ || hk != head_) {
    header(c, p.title, page, npages, right);
    head_ = hk;
    ++drawn;
  }
  for (int i = 0; i < p.ncells && i < kMaxCells; ++i) {
    uint32_t k = key(p, p.cells[i], s);
    if (valid_ && k == keys_[i]) continue;
    draw_cell(c, p, i, s);
    keys_[i] = k;
    ++drawn;
  }
  valid_ = true;
  return drawn;
}

CORE_COLD int step_value(const Page& p, int param, int v, int delta, bool func) {
  for (int i = 0; i < p.ncells; ++i) {
    const Cell& c = p.cells[i];
    if (c.kind != kList || c.in[0] != param) continue;
    int n = list_count(c.names);
    int z = eng_zone((float)v, n) + delta;
    z = z < 0 ? 0 : z >= n ? n - 1 : z;
    return (z * 128 + 64) / n;   // the zone's centre: eng_zone() of it is z
  }
  v += delta * (func ? 8 : 1);
  return v < 0 ? 0 : v > 127 ? 127 : v;
}

// ---- the built-in machine table's pages

// PLAITS (machines.h kPlaits; sim/protocol.py: P1..P8 ENG HRM TIM MOR DEC LPG TUN LEV): the curated model list
// (kPlaitsModels' 17 zones, the names of Plaits' own models), HARMONICS / TIMBRE / MORPH, the low-pass gate's decay
// as its AD shape (Plaits has no filter of its own: the LPG is its VCF + VCA), the LPG's colour, TUNE and LEVEL.
static UI_ROM const char kPlaitsNames[] =
    "VA VCF\0PHASEDST\0WAVETERR\0STR MACH\0CHIPTUNE\0VA\0WAVESHAP\0"
    "2-OP FM\0GRANULAR\0HARMONIC\0WAVETABL\0CHORDS\0SPEECH\0SWARM\0PARTICLE\0STRING\0MODAL\0";
static UI_ROM const Cell kPlaitsCells[] = {
    {kList, 0, 0, 1, 1, {0, kNone, kNone, kNone}, 0, kPlaitsNames},
    {kKnob, 1, 0, 1, 1, {1, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 2, 0, 1, 1, {2, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 3, 0, 1, 1, {3, kNone, kNone, kNone}, 0, nullptr},
    {kEnv, 0, 1, 1, 1, {kNone, 4, kNone, kNone}, kAd, nullptr},
    {kKnob, 1, 1, 1, 1, {5, kNone, kNone, kNone}, 0, nullptr},
    {kKnobBi, 2, 1, 1, 1, {6, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 3, 1, 1, 1, {7, kNone, kNone, kNone}, 0, nullptr},
};
static UI_ROM const Page kPlaitsPage = {
    "PLAITS", kPlaits, sizeof kPlaitsCells / sizeof kPlaitsCells[0], kPlaitsCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"ENG", "HRM", "TIM", "MOR", "DEC", "LPG", "TUN", "LEV"},
};

// LIVE IN (machines.h kLiveMachine, core/livein/livein.h: P1..P9 INPT GAIN GATE HOLD MON ATK DEC SUS REL): the input
// as a list, GAIN bipolar around 0 dB, GATE's threshold as a level (fully down OFF), the gate's HOLD; MON, then MON
// TRIG's envelope (ATK DEC SUS on E..H, REL from P9 drawn in it).
static UI_ROM const char kLiveInNames[] = "L\0R\0L+R\0STR\0";
static UI_ROM const char kLiveMonNames[] = "ALW\0TRIG\0";
static UI_ROM const Cell kLiveCells[] = {
    {kList, 0, 0, 1, 1, {0, kNone, kNone, kNone}, 0, kLiveInNames},
    {kKnobBi, 1, 0, 1, 1, {1, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 2, 0, 1, 1, {2, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 3, 0, 1, 1, {3, kNone, kNone, kNone}, 0, nullptr},
    {kList, 0, 1, 1, 1, {4, kNone, kNone, kNone}, 0, kLiveMonNames},
    {kEnv, 1, 1, 3, 1, {5, 6, 7, 8}, kAdsr, nullptr},
};
static UI_ROM const Page kLivePage = {
    "LIVE IN", kLiveMachine, sizeof kLiveCells / sizeof kLiveCells[0], kLiveCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"INPT", "GAIN", "GATE", "HOLD", "MON", "ATK", "DEC", "SUS"},
};

// SIDECAR: text lines from the remote host's status source (remote.cpp): 0..1 the 8 voices' machines, 4..7 FX1 / FX2
// TYPE and ROUTE, 8..11 Overbridge, underruns, CPU.
static UI_ROM const Cell kSidecarCells[] = {
    {kText, 0, 0, 4, 1, {kNone, kNone, kNone, kNone}, 0, nullptr},
    {kText, 0, 1, 2, 1, {kNone, kNone, kNone, kNone}, 4, nullptr},
    {kText, 2, 1, 2, 1, {kNone, kNone, kNone, kNone}, 8, nullptr},
};
UI_ROM const Page kSidecarPage = {
    "SIDECAR", kNone, sizeof kSidecarCells / sizeof kSidecarCells[0], kSidecarCells,
    {kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone},
    {"", "", "", "", "", "", "", ""},
};

// The widget gallery: every kind on two pages (renderer and tests; not in the machine table).
static UI_ROM const char kDemoNames[] = "SINE\0TRI\0SAW\0SQUARE\0PULSE\0NOISE\0";
static UI_ROM const Cell kDemo0Cells[] = {
    {kFilter, 0, 0, 2, 1, {0, 1, 2, kNone}, 0, nullptr},
    {kEnv, 2, 0, 2, 1, {3, 4, 5, 6}, kAdsr, nullptr},
    {kWave, 0, 1, 1, 1, {8, 9, kNone, kNone}, 0, nullptr},
    {kLfo, 1, 1, 1, 1, {10, 11, kNone, kNone}, 0, nullptr},
    {kSample, 2, 1, 2, 1, {12, 13, kNone, kNone}, 0, nullptr},
};
static UI_ROM const Cell kDemo1Cells[] = {
    {kList, 0, 0, 1, 1, {0, kNone, kNone, kNone}, 0, kDemoNames},
    {kKnob, 1, 0, 1, 1, {1, kNone, kNone, kNone}, 0, nullptr},
    {kKnobBi, 2, 0, 1, 1, {2, kNone, kNone, kNone}, 0, nullptr},
    {kPan, 3, 0, 1, 1, {3, kNone, kNone, kNone}, 0, nullptr},
    {kEnv, 0, 1, 1, 1, {4, 5, kNone, kNone}, kAd, nullptr},
    {kEnv, 1, 1, 1, 1, {6, 7, 8, kNone}, kAhd, nullptr},
    {kFilter, 2, 1, 1, 1, {9, 10, kNone, kNone}, kHp, nullptr},
    {kBar, 3, 1, 1, 1, {11, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kDemoPages[2] = {
    {"FILTER ENV", kNone, sizeof kDemo0Cells / sizeof kDemo0Cells[0], kDemo0Cells,
     {0, 1, 3, 4, 8, 10, 12, 13},
     {"FREQ", "RESO", "ATK", "DEC", "WAVE", "LFO", "STA", "END"}},
    {"WIDGETS", kNone, sizeof kDemo1Cells / sizeof kDemo1Cells[0], kDemo1Cells,
     {0, 1, 2, 3, 5, 7, 9, 11},
     {"WAVE", "KNOB", "BIPO", "PAN", "AD", "AHD", "HP", "LEV"}},
};

struct MachinePage {
  uint8_t machine;
  const Page* page;
};
static UI_ROM const MachinePage kTable[] = {
    {kPlaits, &kPlaitsPage},
    {kLiveMachine, &kLivePage},
#if CORE_POLY && CORE_MSAMP
    {kChopMachine, &kChopSrcPage},   // CHOP (page_chop.cpp)
#endif
};

CORE_COLD const Page* page_for_machine(int machine) {
  for (const MachinePage& m : kTable)
    if (m.machine == machine) return m.page;
  return code_page_for_machine(machine);
}

}  // namespace ui
}  // namespace core
