// core/ui/page_wave.cpp - see extra.h: DSP SAMPLE's and GRAIN's pages with the pool slot's sample as a waveform, the
// per-file overview cache that feeds them, and the dispatch of the cells drawn by code (Kind kDraw). Cold.
//
// The overview: 128 columns of (min, max) over the file's data, found once per file and kept (kWaveSlots files: the
// last used goes first). Read on the control side in remote_tick, one column a millisecond at most: a column of up
// to 256 frames is read whole, a longer one as four evenly spaced runs of a quarter of that (2 KB a read at most; ~130 ms a
// file), from the card through its own handle (core/pool: cards, pool[]). 16 / 24 / 32-bit integer PCM, mono or
// stereo (the channels' extremes). Drawn normalised to the file's peak.
//
// DSP SAMPLE (machine 0x20), the SD SAMPLE page P9..P16: TUN FIN SMP MOD over the waveform, STA END LOP LEV under it.
// The waveform shows STA..END solid, the rest dithered (END before STA: reversed, an arrow), the mode (MOD, the sample
// RAM's playback modes; without it always the streamed one-shot) in its corner, LOOP's / a looping ONE's loop as
// brackets over the region, SLICE's slices as dotted lines. GRAIN (MOD in its zone, with the sample RAM): the grain
// window as a box over the whole waveform - centred at POS (STA), SIZE (END) wide (10 ms..0.5 s of the file), the
// spray (a quarter of a grain either way) dotted on from its edges.
#include <string.h>

#include "pool/pool.h"
#include "ui/extra.h"
#include "voices.h"

namespace core {
namespace ui {

namespace {

constexpr int kCols = 128;
constexpr int kSlotStatus = kSlotFree + 31;   // the text item saying why there is no waveform
#if RYTM_SENDFX_LARGE
constexpr int kWaveSlots = 8;   // a file a voice
#else
constexpr int kWaveSlots = 1;   // the Teensy: the page shows one voice (another one's file is read again, ~0.15 s)
#endif

struct Overview {
  int16_t folder = -1;
  uint16_t file = 0;
  uint8_t state = 0;    // 0 empty, 1 loading, 2 ready, 3 failed (not readable)
  uint8_t col = 0;      // the next column to read
  uint32_t frames = 0, rate = 0, used = 0;
  int16_t pk[kCols][2];
};
CORE_RAM2 Overview cache[kWaveSlots];
File wfile;
int wfile_of = -1;   // the cache entry wfile is open for
card::FileRec wrec;
uint32_t wave_clock = 0, last_col_ms = 0;
bool col_clock = false;

CORE_COLD int bytes_per_frame(const card::FileRec& r) { return (r.bits / 8) * (r.channels ? r.channels : 1); }

CORE_COLD int sample_at(const uint8_t* p, int bits) {   // one sample, >> to 16 bits
  if (bits == 16) return (int16_t)(p[0] | p[1] << 8);
  if (bits == 24) return (int16_t)(p[1] | p[2] << 8);
  return (int16_t)(p[2] | p[3] << 8);
}

// Column c of entry w from wfile: false when the card failed
CORE_COLD bool read_column(Overview& w, int c) {
  uint32_t f0 = (uint32_t)((uint64_t)w.frames * c / kCols), f1 = (uint32_t)((uint64_t)w.frames * (c + 1) / kCols);
  if (f1 <= f0) f1 = f0 + 1;
  if (f1 > w.frames) f1 = w.frames;
  int bpf = bytes_per_frame(wrec), ch = wrec.channels ? wrec.channels : 1;
  uint32_t span = f1 - f0;
  uint8_t buf[2048];
  uint32_t most = sizeof buf / bpf;   // frames a read (256 up to 8 bytes a frame)
  int runs = span <= most ? 1 : 4;
  uint32_t len = span <= most ? span : most / 4;
  int lo = 32767, hi = -32768;
  for (int k = 0; k < runs; ++k) {
    uint32_t at = runs == 1 ? f0 : f0 + (uint32_t)((uint64_t)(span - len) * k / (runs - 1));
    if (!wfile.seekSet((uint64_t)wrec.data_offset + (uint64_t)at * bpf)) return false;
    int n = wfile.read(buf, len * bpf);
    if (n <= 0) return false;
    for (int i = 0; i + bpf <= n; i += bpf)
      for (int j = 0; j < ch && j < 2; ++j) {
        int s = sample_at(buf + i + j * (wrec.bits / 8), wrec.bits);
        if (s < lo) lo = s;
        if (s > hi) hi = s;
      }
  }
  if (lo > hi) lo = hi = 0;
  w.pk[c][0] = (int16_t)lo, w.pk[c][1] = (int16_t)hi;
  return true;
}

CORE_COLD void wave_fail(Overview& w) {
  w.state = 3;
  if (wfile.isOpen()) wfile.close();
  wfile_of = -1;
}

}  // namespace

// The overview of a pool slot (SMP, word 19: 1..), loading if new; null: no slot, an empty one, or no card.
CORE_COLD const void* wave_of_slot(int slot, int* state, int* progress) {
  *state = 0, *progress = 0;
  if (!card_ok || slot < 1 || slot > npool || pool[slot - 1].folder < 0) return nullptr;
  int f = pool[slot - 1].folder, fi = pool[slot - 1].file;
  int pick = -1;
  for (int i = 0; i < kWaveSlots; ++i)
    if (cache[i].state && cache[i].folder == f && cache[i].file == fi) pick = i;
  if (pick < 0) {   // the least recently used entry (not the one being read, when there is a choice)
    for (int i = 0; i < kWaveSlots; ++i)
      if (pick < 0 || (i != wfile_of && (pick == wfile_of || cache[i].used < cache[pick].used))) pick = i;
    if (pick == wfile_of) wave_fail(cache[pick]);
    Overview& w = cache[pick];
    w.folder = (int16_t)f, w.file = (uint16_t)fi, w.state = 1, w.col = 0, w.frames = w.rate = 0;
  }
  Overview& w = cache[pick];
  w.used = ++wave_clock;
  *state = w.state, *progress = w.col;
  return &w;
}
CORE_COLD const void* wave_of_voice(int vi, int* state, int* progress) { return wave_of_slot(pool_slot(v[vi & 7].p[10]), state, progress); }
// rui1045: SMP's slot as the page reads it (the SRC view's 2A at 14 bits, or the voice's report): bank * 127 + row, 0 OFF
CORE_COLD int smp_slot(const Source& s) {
  int c = s.code(10), row = c & 127;
  return row ? (c >> 7 & 7) * kPool + row : 0;
}

// One column of the file being read, at most one a millisecond (remote_tick, while the page is open).
CORE_COLD void wave_idle(uint32_t ms) {
  if (col_clock && ms == last_col_ms) return;
  col_clock = true, last_col_ms = ms;
  int k = -1;
  for (int i = 0; i < kWaveSlots; ++i)
    if (cache[i].state == 1 && (k < 0 || cache[i].used > cache[k].used)) k = i;
  if (k < 0) return;
  Overview& w = cache[k];
  if (wfile_of != k) {
    if (wfile.isOpen()) wfile.close();
    wfile_of = -1;
    if (!cards.file(w.folder, w.file, &wrec) || !cards.open(w.folder, wrec, &wfile)) { wave_fail(w); return; }
    wfile_of = k;
    int bpf = bytes_per_frame(wrec);
    if (wrec.format != 1 || (wrec.bits != 16 && wrec.bits != 24 && wrec.bits != 32) || bpf <= 0 || bpf > 64) {
      wave_fail(w);
      return;
    }
    w.frames = wrec.data_len / bpf, w.rate = wrec.rate;
    if (!w.frames) { wave_fail(w); return; }
  }
  if (!read_column(w, w.col)) { wave_fail(w); return; }
  if (++w.col == kCols) {
    w.state = 2;
    wfile.close();
    wfile_of = -1;
  }
}

CORE_COLD void wave_column(const void* w, int c, int* lo, int* hi) {
  const Overview* o = (const Overview*)w;
  *lo = o ? o->pk[c & (kCols - 1)][0] : 0, *hi = o ? o->pk[c & (kCols - 1)][1] : 0;
}

CORE_COLD void wave_reset() {   // tests
  for (Overview& w : cache) w = Overview();
  if (wfile.isOpen()) wfile.close();
  wfile_of = -1;
  col_clock = false;
}

namespace {

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
// MOD's mode (voices.cpp dsp_mode's rule, here so the Teensy keeps that one inlined in its ITCM callers); -1: streamed.
// mod: MOD's value as the page reads it (the Source's P12: the SRC view's is the Rytm's own, not the voice's yet)
CORE_COLD int mode_of(int vi, float mod) {
  if (!psram) return -1;
  return v[vi & 7].grain ? (int)rytm::SampleModes::kGrain : rytm::SampleModes::Zone(mod, rytm::SampleModes::kModes);
}

// The waveform in rows k.y..k.y+k.h-1, x0..x0+w-1, normalised; columns outside [a, b] dithered (a > b: none)
CORE_COLD void plot(Canvas& c, const Overview* w, int x0, int y0, int wd, int h, int a, int b) {
  int mid = y0 + h / 2, amp = (h - 1) / 2;
  if (!w || w->state != 2) {
    c.hline(x0, x0 + wd - 1, mid, kDither);
    if (!w || w->state == 3) return;
  }
  int n = w->state == 2 ? kCols : w->col, peak = 1;
  for (int i = 0; i < n; ++i) {
    int m = w->pk[i][1] > -w->pk[i][0] ? w->pk[i][1] : -w->pk[i][0];
    if (m > peak) peak = m;
  }
  for (int x = 0; x < wd; ++x) {
    int j = x * kCols / wd;
    if (j >= n) break;
    int y0_ = mid - w->pk[j][1] * amp / peak, y1 = mid - w->pk[j][0] * amp / peak;
    bool in = a <= b && x0 + x >= a && x0 + x <= b;
    c.vline(x0 + x, y0_, y1, in || a > b ? kOn : kDither);
  }
}

// No waveform (yet): why, centred over where it would be (slot: a text item; empty when there is a waveform)
CORE_COLD void status(Canvas& c, const Overview* w, const Source& s, int x0, int wd, int y0, int h, int slot) {
  int vi = s.voice() < 0 ? 0 : s.voice() & 7;
  const char* why = v[vi].grain && !psram ? "!RAM: GRAIN NEEDS PSRAM"
                    : !w ? (smp_slot(s) < 1 ? "SMP OFF" : "EMPTY SLOT") : w->state == 1 ? "LOADING" : w->state == 3 ? "UNREADABLE"
                                                                                                                  : nullptr;
  c.put_text(slot, kRSmall, x0 + wd / 2, y0 + h / 2 - 2, why, kAlignCentre);
}

CORE_COLD int xpos(int x0, int wd, int p120) { return x0 + clampi(p120, 0, 120) * (wd - 1) / 120; }

CORE_COLD void bracket(Canvas& c, int x, int y, int h, int dir) {   // a loop end: a vertical line with feet inward
  c.vline(x, y, y + h - 1, kOn);
  c.pixel(x + dir, y, kOn), c.pixel(x + dir, y + h - 1, kOn);
}

// DSP SAMPLE: in[0] STA, in[1] END, in[2] LOP, in[3] MOD
CORE_COLD void draw_sample(Canvas& c, const Rect& r, const Cell& cell, const Source& s, int slot) {
  int vi = s.voice() < 0 ? 0 : s.voice();
  int st = 0, prog = 0;
  const Overview* w = (const Overview*)wave_of_slot(smp_slot(s), &st, &prog);
  // rows 11..24 of the top row; the waveform in x 1..100, the mode's name in the column right of it
  int x0 = r.x + 1, wd = r.w - 28, y0 = r.y + 2, h = kLabelDy - 3;
  int sta = s.param(cell.in[0]), end = s.param(cell.in[1]), lop = s.param(cell.in[2]);
  int xs = xpos(x0, wd, sta), xe = xpos(x0, wd, end);
  bool rev = end < sta;
  int a = rev ? xe : xs, b = rev ? xs : xe;
  plot(c, w, x0, y0, wd, h, a, b);
  int mode = mode_of(vi, (float)s.param(11));
  bool loop = mode == rytm::SampleModes::kLoop || (mode <= 0 && lop >= 1);
  c.vline(a, y0, y0 + h - 1, kOn);
  c.vline(b, y0, y0 + h - 1, kOn);
  if (loop) {   // the loop: brackets at both ends, a dotted line over the region (LOOP's ping-pong: both ways)
    bracket(c, a, y0 - 1, h + 2, 1);
    bracket(c, b, y0 - 1, h + 2, -1);
    c.hline(a + 2, b - 2, y0 - 1, kDither);
  }
  if (mode == rytm::SampleModes::kSlice) {
    int n = rytm::SampleModes::SliceCount((float)lop);
    for (int k = 1; k < n; ++k) {
      int x = a + (b - a) * k / n;
      for (int y = y0; y < y0 + h; y += 2) c.pixel(x, y, kXor);
    }
  }
  if (rev) {   // reversed: an arrow pointing left at the region's top
    int y = y0 + 1, x = a + 3;
    c.hline(x, x + 4, y, kXor);
    c.pixel(x + 1, y - 1, kXor), c.pixel(x + 1, y + 1, kXor);
  }
  const char* name = mode < 0 ? (lop >= 1 ? "LOOP" : "ONE") : rytm::SampleModes::Name(mode);
  c.put_text(slot, kRSmall, r.x + r.w - 13, y0 + h / 2 - 2, name, kAlignCentre);
  status(c, w, s, x0, wd, y0, h, kSlotStatus);
}

// GRAIN: in[0] POS (STA), in[1] SIZE (END), in[2] DENS (LOP)
CORE_COLD void draw_grain(Canvas& c, const Rect& r, const Cell& cell, const Source& s, int slot) {
  int vi = s.voice() < 0 ? 0 : s.voice();
  int st = 0, prog = 0;
  const Overview* w = (const Overview*)wave_of_slot(smp_slot(s), &st, &prog);
  int x0 = r.x + 1, wd = r.w - 2, y0 = r.y + 2, h = kLabelDy - 3;
  plot(c, w, x0, y0, wd, h, 1, 0);
  int pos = s.param(cell.in[0]), size = s.param(cell.in[1]);
  int xc = xpos(x0, wd, pos);
  // the grain's length: 10 ms x 50^(SIZE / 127) of the file (its rate), as a share of the file's columns
  int span_px = 2;
  if (w && w->frames && w->rate) {
    float secs = 0.01f, k = 1.0f;
    for (int i = 0; i < size; ++i) k *= 1.0313f;   // 50^(1/127)
    secs *= k;
    span_px = (int)(secs * (float)w->rate / (float)w->frames * (float)wd + 0.5f);
  }
  span_px = clampi(span_px, 2, wd);
  int g0 = clampi(xc - span_px / 2, x0, x0 + wd - 1), g1 = clampi(xc + (span_px - 1) / 2, x0, x0 + wd - 1);
  // the window: a box from a row over the waveform to its last row, ticks at its centre, the spray dotted on from its
  // top and bottom edges
  c.rect(Rect{(int16_t)g0, (int16_t)(y0 - 1), (int16_t)(g1 - g0 + 1), (int16_t)(h + 1)}, kOn);
  c.vline(xc, y0 - 1, y0 + 1, kOn);
  c.vline(xc, y0 + h - 3, y0 + h - 1, kOn);
  int spray = span_px / 4;
  if (spray > 0) {
    c.hline(clampi(g0 - spray, x0, x0 + wd - 1), g0, y0 - 1, kDither);
    c.hline(clampi(g0 - spray, x0, x0 + wd - 1), g0, y0 + h - 1, kDither);
    c.hline(g1, clampi(g1 + spray, x0, x0 + wd - 1), y0 - 1, kDither);
    c.hline(g1, clampi(g1 + spray, x0, x0 + wd - 1), y0 + h - 1, kDither);
  }
  status(c, w, s, x0, wd, y0, h, kSlotStatus);
  (void)slot;
}

// ---- the pages (machines.h kDspSample; P9..P16 = 8..15: TUN FIN SMP MOD STA END LOP LEV)
UI_ROM const Cell kDspsCells[] = {
    {kDraw, 0, 0, 4, 1, {12, 13, 14, 11}, kDrawSample, nullptr},
    {kKnob, 0, 1, 1, 1, {12, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 1, 1, 1, 1, {13, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 2, 1, 1, 1, {14, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 3, 1, 1, 1, {15, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kDspsPage = {
    "DSP SAMPLE", kDspSample, sizeof kDspsCells / sizeof kDspsCells[0], kDspsCells,
    {8, 9, 10, 11, 12, 13, 14, 15},
    {"TUN", "FIN", "SMP", "MOD", "STA", "END", "LOP", "LEV"},
};
UI_ROM const Cell kGrainCells[] = {
    {kDraw, 0, 0, 4, 1, {12, 13, 14, 11}, kDrawGrain, nullptr},
    {kKnob, 0, 1, 1, 1, {12, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 1, 1, 1, 1, {13, kNone, kNone, kNone}, 0, nullptr},
    {kDraw, 2, 1, 1, 1, {13, 14, kNone, kNone}, kDrawCloud, nullptr},   // the cloud: SIZE wide grains, DENS many
    {kBar, 3, 1, 1, 1, {15, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kGrainPage = {
    "GRAIN", kDspSample, sizeof kGrainCells / sizeof kGrainCells[0], kGrainCells,
    {8, 9, 10, 11, 12, 13, 14, 15},
    {"TUN", "FIN", "SMP", "MOD", "POS", "SIZE", "DENS", "LEV"},
};
// The GRAIN machine (machines.h kGrainMachine): the same view, MOD's word its SPRY
UI_ROM const Page kGrainMachinePage = {
    "GRAIN", kGrainMachine, sizeof kGrainCells / sizeof kGrainCells[0], kGrainCells,
    {8, 9, 10, 11, 12, 13, 14, 15},
    {"TUN", "FIN", "SMP", "SPRY", "POS", "SIZE", "DENS", "LEV"},
};

// The SRC view (remote.cpp): the Rytm's encoders there are the SRC page's own, P1..P8 (for DSP SAMPLE the resonator:
// ENG STR BRI DMP MIX FB TUN LEV; the sample's P9..P16 are the SMPL page's, turned there). The same picture, the
// waveform above, the knobs of P5..P8 below, so a turn on the Rytm shows.
UI_ROM const Cell kDspsSrcCells[] = {
    {kDraw, 0, 0, 4, 1, {12, 13, 14, 11}, kDrawSample, nullptr},
    {kKnob, 0, 1, 1, 1, {4, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 1, 1, 1, 1, {5, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 2, 1, 1, 1, {6, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 3, 1, 1, 1, {7, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kDspsSrcPage = {
    "DSP SAMPLE", kDspSample, sizeof kDspsSrcCells / sizeof kDspsSrcCells[0], kDspsSrcCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"ENG", "STR", "BRI", "DMP", "MIX", "SEED", "TUN", "LEV"},   // rui1050: P6 (never read: FB fixed) SEED MOD's slot
};
UI_ROM const Cell kGrainSrcCells[] = {
    {kDraw, 0, 0, 4, 1, {12, 13, 14, 11}, kDrawGrain, nullptr},
    {kKnob, 0, 1, 1, 1, {4, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 1, 1, 1, 1, {5, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 2, 1, 1, 1, {6, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 3, 1, 1, 1, {7, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kGrainSrcPage = {
    "GRAIN", kDspSample, sizeof kGrainSrcCells / sizeof kGrainSrcCells[0], kGrainSrcCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"ENG", "STR", "BRI", "DMP", "MIX", "FB", "TUN", "LEV"},
};

}  // namespace

CORE_COLD const Page* src_view_page(const Page* p) {
  return p == &kDspsPage ? &kDspsSrcPage : p == &kGrainPage ? &kGrainSrcPage : p;
}

CORE_COLD const Page* code_page_for_machine(int machine) {
  return machine == kDspSample ? &kDspsPage : gfx_page_for_machine(machine);
}

CORE_COLD const Page* page_for_voice(int machine, int vi, int mod) {
  if (machine == kDspSample && mode_of(vi, mod < 0 ? v[vi & 7].p[11] : (float)mod) == rytm::SampleModes::kGrain) return &kGrainPage;
  if (machine == kGrainMachine) return &kGrainMachinePage;
  return page_for_machine(machine);
}

CORE_COLD uint32_t draw_key(const Cell& c, const Source& s) {
  uint32_t h = 0;
  if (c.arg == kDrawSample || c.arg == kDrawGrain) {
    int vi = s.voice() < 0 ? 0 : s.voice(), st = 0, prog = 0;
    const void* w = wave_of_slot(smp_slot(s), &st, &prog);
    h = (uint32_t)(w ? (uintptr_t)w : 1) ^ (uint32_t)st << 8 ^ (uint32_t)(prog / 16) << 12 ^ (uint32_t)(mode_of(vi, (float)s.param(11)) + 1) << 20 ^
        (uint32_t)(smp_slot(s) < 1) << 28;
  } else if (c.arg == kDrawGran || c.arg == kDrawWt || c.arg == kDrawGrList || c.arg == kDrawWtList || c.arg == kDrawMsList ||
             c.arg == kDrawBankList) {
    h = wavegrain_key(c, s);
#if CORE_POLY && CORE_MSAMP
  } else if (c.arg == kDrawChop) {
    h = chop_key(c, s);
#endif
  } else {
    h = gfx_key(c, s);
  }
  return h;
}

CORE_COLD void draw_custom(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot) {
  cv.unclip();
  cv.fill(r, kOff);
  cv.clip(r);
  switch (c.arg) {
    case kDrawSample: draw_sample(cv, r, c, s, slot); break;
    case kDrawGrain: draw_grain(cv, r, c, s, slot); break;
    case kDrawGran:
    case kDrawWt:
    case kDrawGrList:
    case kDrawWtList:
    case kDrawMsList:
    case kDrawBankList: wavegrain_draw(cv, r, c, s, slot); break;
#if CORE_POLY && CORE_MSAMP
    case kDrawChop: chop_draw(cv, r, c, s, slot); break;
#endif
    default: gfx_draw(cv, r, c, s, slot); break;
  }
  cv.unclip();
}

}  // namespace ui
}  // namespace core
