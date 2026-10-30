// core/ui/page_gfx.cpp - see extra.h: machine graphics drawn by code (original drawings, 1-bit) and the pages that
// use them. Cold.
//
//   kDrawEnv    JUNO's envelope: attack, decay to the patch's own sustain, the gate held for LEN (127: until the
//               note-off, the plateau's end dotted), release; curved segments (an RC-like attack, exponential-looking
//               decay and release), the sustain level dotted, every segment as wide as its slider is high.
//   kDrawFm     6-OP FM's algorithm (the voice PAT picks: its packed byte 110): the six operators as boxes, carriers
//               filled on the bottom row, each modulator above what it modulates, lines between, feedback as a loop
//               at the operator's side (or from one operator back up to another). The 32 topologies as data below.
//   kDrawCloud  GRAIN's cloud: grains as Hann-shaped bumps, as wide as SIZE, as many as DENS (1..12 shown), at
//               fixed pseudo-random places and heights (the picture changes only with the knobs).
//   kDrawPatch  JUNO's PAT: the patch's name (the factory table's or the project's JUNO list's), a track with its place.
//
// JUNO (machine 0x08, P1..P8 PAT CUT RES ENV ATK DEC REL LEN) and 6-OP FM (0x48, PAT TIM MOR DEC LPG TUN LEV LEN).
#include <string.h>

#include "poly.h"
#include "pool/pool.h"
#if CORE_POLY
#include "seq/seq.h"
#endif
#include "ui/extra.h"
#include "voices.h"
#if CORE_MSAMP
#include "msamp/msamp.h"
#endif

namespace core {
namespace ui {

namespace {

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- the envelope
// t 0..256 -> 0..256 of the way: 1 - (1 - t)^3, quick at first then easing in (an RC charge / discharge's look)
CORE_COLD int ease(int t) { int u = 256 - t; return 256 - ((u * u >> 8) * u >> 8); }

CORE_COLD void plot_seg(Canvas& c, int x0, int w, int ytop, int h, int from, int to, int* px, int* py) {
  for (int i = 1; i <= w; ++i) {
    int k = ease(i * 256 / w);   // 0..256 of the way from `from` to `to`
    int lv = from + (to - from) * k / 256;
    int y = ytop + h - 1 - lv * (h - 1) / 256;
    c.line(*px, *py, x0 + i, y, kOn);
    *px = x0 + i, *py = y;
  }
}

CORE_COLD int sustain_of(int vi) {
  return v[vi & 7].juno ? clampi(juno_kb[vi & 7][13], 0, 127) : 80;   // the Juno patch's sustain slider
}

CORE_COLD void draw_env(Canvas& c, const Rect& r, const Cell& cell, const Source& s) {
  int a = s.param(cell.in[0]), d = s.param(cell.in[1]), rl = s.param(cell.in[2]), len = s.param(cell.in[3]);
  int sus = sustain_of(s.voice() < 0 ? 0 : s.voice()) * 256 / 127;
  int x = r.x + 2, top = r.y + 2, h = kLabelDy - 3, wmax = r.w - 6;
  int wa = 2 + a * 20 / 127, wd = 2 + d * 20 / 127, wr = 2 + rl * 20 / 127;
  bool held = len >= 127;
  int ws = held ? 14 : 2 + len * 14 / 127;
  int tot = wa + wd + ws + wr;
  if (tot > wmax) wa = wa * wmax / tot, wd = wd * wmax / tot, ws = ws * wmax / tot, wr = wmax - wa - wd - ws;
  int y0 = top + h - 1;
  c.hline(x, x + wa + wd + ws + wr, y0, kDither);   // the zero line
  int ys = top + h - 1 - sus * (h - 1) / 256;
  c.hline(x + wa, x + wa + wd + ws, ys, kDither);    // the sustain level
  int px = x, py = y0;
  plot_seg(c, x, wa, top, h, 0, 256, &px, &py);
  plot_seg(c, x + wa, wd, top, h, 256, sus, &px, &py);
  int xe = x + wa + wd + ws;
  if (held) {   // until the note-off: the plateau's end dotted, then the release
    for (int i = px + 1; i <= xe; ++i) c.pixel(i, ys, (i & 1) || i < xe - 6 ? kOn : kOff);
  } else {
    c.hline(px, xe, ys, kOn);
  }
  c.vline(xe, ys - 1, y0, kDither);   // the gate's end
  px = xe, py = ys;
  plot_seg(c, xe, wr, top, h, sus, 0, &px, &py);
}

// ---- 6-OP FM: per algorithm the operators modulating op 1..6 (bit k: op k + 1), the carriers, feedback (src << 4 | dst)
struct Alg {
  uint8_t mod[6], car, fb;
};
UI_ROM const Alg kAlgs[32] = {
    {{2, 0, 8, 16, 32, 0}, 5, 0x66},  {{2, 0, 8, 16, 32, 0}, 5, 0x22},  {{2, 4, 0, 16, 32, 0}, 9, 0x66},
    {{2, 4, 0, 16, 32, 0}, 9, 0x46},  {{2, 0, 8, 0, 32, 0}, 21, 0x66},  {{2, 0, 8, 0, 32, 0}, 21, 0x56},
    {{2, 0, 24, 0, 32, 0}, 5, 0x66},  {{2, 0, 24, 0, 32, 0}, 5, 0x44},  {{2, 0, 24, 0, 32, 0}, 5, 0x22},
    {{2, 4, 0, 48, 0, 0}, 9, 0x33},   {{2, 4, 0, 48, 0, 0}, 9, 0x66},   {{2, 0, 56, 0, 0, 0}, 5, 0x22},
    {{2, 0, 56, 0, 0, 0}, 5, 0x66},   {{2, 0, 8, 48, 0, 0}, 5, 0x66},   {{2, 0, 8, 48, 0, 0}, 5, 0x22},
    {{22, 0, 8, 0, 32, 0}, 1, 0x66},  {{22, 0, 8, 0, 32, 0}, 1, 0x22},  {{14, 0, 0, 16, 32, 0}, 1, 0x33},
    {{2, 4, 0, 32, 32, 0}, 25, 0x66}, {{4, 4, 0, 48, 0, 0}, 11, 0x33},  {{4, 4, 0, 32, 32, 0}, 27, 0x33},
    {{2, 0, 32, 32, 32, 0}, 29, 0x66}, {{0, 4, 0, 32, 32, 0}, 27, 0x66}, {{0, 0, 32, 32, 32, 0}, 31, 0x66},
    {{0, 0, 0, 32, 32, 0}, 31, 0x66}, {{0, 4, 0, 48, 0, 0}, 11, 0x66},  {{0, 4, 0, 48, 0, 0}, 11, 0x33},
    {{2, 0, 8, 16, 0, 0}, 37, 0x55},  {{0, 0, 8, 0, 32, 0}, 23, 0x66},  {{0, 0, 8, 16, 0, 0}, 39, 0x55},
    {{0, 0, 0, 0, 32, 0}, 31, 0x66},  {{0, 0, 0, 0, 0, 0}, 63, 0x66},
};

// The voice PAT picks (fm_select's rule): its packed bytes; false: none (an empty slot)
CORE_COLD bool fm_bytes(int pat, uint8_t* b) {
  pat = clampi(pat, 0, 127);
  int nl = dxlib.listed(), total = nl ? nl : dx7::kRomVoices;
  int e = pat >= total ? total - 1 : pat;
  if (!nl) {
    memcpy(b, dx7::rom_bank(e / 32) + 128 * (e % 32), 128);
    return true;
  }
  return dxlib.voice(e, b);
}

CORE_COLD int fm_alg(int pat) {   // 0..31, -1 none
  uint8_t b[128];
  return fm_bytes(pat, b) ? (b[110] & 31) : -1;
}

// Positions: level = the longest chain above a carrier (carriers 0); columns: carriers in operator order, each
// modulator above the first operator it modulates, siblings to the right in the next free column.
struct Layout {
  int8_t col[6], lev[6];
  int ncols, nlev;
};
CORE_COLD int place(const Alg& a, int op, int col, int lev, Layout& L, int* next) {
  if (L.col[op] >= 0) return col;
  L.col[op] = (int8_t)col, L.lev[op] = (int8_t)lev;
  if (lev + 1 > L.nlev) L.nlev = lev + 1;
  int c = col;
  bool first = true;
  for (int m = 5; m >= 0; --m)   // the highest-numbered modulator straight above (the DX7's drawings' habit)
    if (a.mod[op] >> m & 1 && L.col[m] < 0) {
      int at = first ? col : (*next)++;
      place(a, m, at, lev + 1, L, next);
      if (at > c) c = at;
      first = false;
    }
  return c;
}
CORE_COLD void layout(const Alg& a, Layout& L) {
  memset(L.col, -1, sizeof L.col);
  L.ncols = 0, L.nlev = 1;
  int next = 0;
  for (int op = 0; op < 6; ++op)
    if (a.car >> op & 1) {
      int col = next++;
      place(a, op, col, 0, L, &next);
    }
  L.ncols = next;
}

CORE_COLD void draw_alg(Canvas& c, const Rect& r, int alg) {
  int top = r.y + 1, h = kLabelDy - 1;   // rows r.y + 1 .. r.y + 16
  if (alg < 0) {
    c.hline(r.x + 4, r.x + r.w - 5, top + h / 2, kDither);
    return;
  }
  const Alg& a = kAlgs[alg];
  Layout L;
  layout(a, L);
  // boxes 5 x 2 (carriers 5 x 3, standing on the output line in the cell's last row), a level every 4 rows
  const int bw = 5, pitch_y = 4;
  int pitch_x = L.ncols > 4 ? 10 : 12;
  int x0 = r.x + (r.w - (L.ncols - 1) * pitch_x - bw - 3) / 2;
  int bus = top + h - 1;
  int bx[6], by[6], bh[6];
  for (int op = 0; op < 6; ++op) {
    bool car = a.car >> op & 1;
    bx[op] = x0 + L.col[op] * pitch_x;
    bh[op] = car ? 3 : 2;
    by[op] = bus - 3 - L.lev[op] * pitch_y - (car ? 1 : 0);   // level 0 rows bus-4..bus-2, level 3 at the top
  }
  for (int t = 0; t < 6; ++t)   // the connections: a modulator's bottom to its target's top (slanted from aside)
    for (int m = 0; m < 6; ++m)
      if (a.mod[t] >> m & 1) {
        int xm = bx[m] + bw / 2, xt = bx[t] + bw / 2;
        if (xm == xt) c.vline(xm, by[m] + bh[m], by[t] - 1, kOn);
        else c.line(xm, by[m] + bh[m], xt + (xm > xt ? 1 : -1), by[t] - 1, kOn);
      }
  int c0 = 99, c1 = -1;
  for (int op = 0; op < 6; ++op) {
    c.fill(Rect{(int16_t)bx[op], (int16_t)by[op], (int16_t)bw, (int16_t)bh[op]}, kOn);
    if (a.car >> op & 1) {   // a carrier: a stub down to the output line
      c.vline(bx[op] + bw / 2, by[op] + bh[op], bus, kOn);
      c0 = bx[op] + bw / 2 < c0 ? bx[op] + bw / 2 : c0, c1 = bx[op] + bw / 2 > c1 ? bx[op] + bw / 2 : c1;
    }
  }
  c.hline(c0, c1 + 3, bus, kOn);   // the output, an arrow's stub past the last carrier
  c.pixel(c1 + 2, bus - 1, kOn);
  int src = (a.fb >> 4) - 1, dst = (a.fb & 15) - 1;   // feedback: a loop on the right of the box(es)
  if (src >= 0 && dst >= 0) {
    int xr = (bx[src] > bx[dst] ? bx[src] : bx[dst]) + bw + 1;
    int ya = by[dst] - 1 < top ? top : by[dst] - 1, yb = by[src] + bh[src];
    c.hline(bx[dst] + bw / 2, xr, ya, kOn);
    c.vline(xr, ya, yb, kOn);
    c.hline(bx[src] + bw - 1, xr, yb, kOn);
  }
}

CORE_COLD void draw_fm(Canvas& c, const Rect& r, const Cell& cell, const Source& s) {
  draw_alg(c, r, fm_alg(s.param(cell.in[0])));
}

// ---- GRAIN's cloud
CORE_COLD uint32_t hash(uint32_t x) {
  x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
  return x;
}
CORE_COLD void draw_cloud(Canvas& c, const Rect& r, const Cell& cell, const Source& s) {
  int size = s.param(cell.in[0]), dens = s.param(cell.in[1]);
  int x0 = r.x + 2, w = r.w - 4, top = r.y + 2, h = kLabelDy - 3, base = top + h - 1;
  int gw = 3 + size * 13 / 127;              // a grain's width: 3..16 px
  int n = 1 + dens * 11 / 127;               // grains shown: 1..12
  c.hline(x0, x0 + w - 1, base, kDither);
  for (int g = 0; g < n; ++g) {
    uint32_t k = hash((uint32_t)g * 2654435761u + 17);
    int gx = x0 + (int)(k % (uint32_t)(w - gw > 1 ? w - gw : 1));
    int gh = h * (55 + (int)(k >> 8) % 46) / 100;   // 55..100 % of the height
    int py = base;
    for (int i = 0; i <= gw; ++i) {   // a Hann bump: sin^2 over its width
      int sn = sin256(i * 128 / gw);
      int y = base - gh * sn * sn / (127 * 127);
      if (i) c.line(gx + i - 1, py, gx + i, y, kOn);
      py = y;
    }
  }
}

// ---- JUNO's patch name: juno_pick's rule (the factory table, or the project's JUNO list), here so the Teensy keeps
// that one inlined in its ITCM callers
CORE_COLD const char* juno_name(int pat) {
  int nl = junolib.listed();
  if (!nl) return juno::kPatches[pat & 127].name;
  const jlib::Patch& jp = junolib.slot(pat >= nl ? nl - 1 : pat);
  return jp.used ? jp.name : "(empty)";
}

CORE_COLD void draw_patch(Canvas& c, const Rect& r, const Cell& cell, const Source& s, int slot) {
  int pat = s.param(cell.in[0]);
  const char* name = juno_name(pat);
  char t[8];
  int n = 0;
  for (; name && name[n] && n < 6; ++n) t[n] = name[n];
  t[n] = 0;
  c.put_text(slot, kRSmall, r.x + r.w / 2, r.y + 6, t, kAlignCentre);
  int x = r.x + 4 + pat * (r.w - 9) / 127;   // the place in the list, as the stock enum's track
  c.hline(r.x + 4, r.x + r.w - 5, r.y + 14, kDither);
  c.fill(Rect{(int16_t)(x - 1), (int16_t)(r.y + 13), 3, 3}, kOn);
}

// ---- the chord (test-1031): in[0..3] CHD INV SPR VOI. A text line (the root's name with its octave and the chord, INV
// SPR VOI on the right), a 3-octave keyboard (a note sounding: its white key's lower part filled, its black key solid;
// unlit black keys dithered; a note past the keyboard: an arrow at its right end) with each octave's C named under it,
// the voice's notes sounding now beside it ("LIVE" under the count while the pads play it). chrorange: the keyboard's
// window is the voice's own, kept while the notes fit in it (keys_window: a lone pad note shows in its own octave).
// Slots kSlotChord.. (7 items).
constexpr int kSlotChord = kSlotFree + 24;
UI_ROM const char* const kPc[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
UI_ROM const int8_t kWhiteOf[12] = {0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6};   // white index in the octave, -1 black
UI_ROM const int8_t kBlackAfter[12] = {-1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1};   // a black key: the white before

struct ChordShown {
  int root;
  poly::Chord c;
  bool live;
  int sounding;
};
CORE_COLD ChordShown chord_shown(const Cell& cell, const Source& s) {
  float p[16] = {};
  for (int i = 0; i < 4; ++i) p[12 + i] = (float)s.param(cell.in[i]);
  ChordShown x = {60, poly::chord_of(p), false, 0};
#if CORE_POLY
  int vi = s.voice() < 0 ? 0 : s.voice();
  poly::View vw = poly::view(vi);
  x.root = vw.root, x.live = vw.live, x.sounding = poly::voice_notes(vi);
  if (vw.live) x.c = vw.c;
#endif
  return x;
}
CORE_COLD bool chord_on(const Cell& cell, const Source& s) {
  ChordShown x = chord_shown(cell, s);
  return x.c.chd != 0 || x.live;
}

CORE_COLD void draw_chord(Canvas& c, const Rect& r, const Cell& cell, const Source& s) {
  ChordShown x = chord_shown(cell, s);
  uint8_t ns[poly::kMaxNotes];
  int n = poly::expand(x.root, x.c, ns);
  char b[24], v3[8];
  sniprintf(b, sizeof b, CORE_STR("%s%d %s"), kPc[x.root % 12], x.root / 12 - 1, x.c.chd ? poly::kChordNames[x.c.chd] : "NOTE");
  c.put_text(kSlotChord, kRSmall, r.x + 2, r.y + 1, b);
  sniprintf(b, sizeof b, CORE_STR("INV%d SPR%d %s%s"), x.c.inv, x.c.spr, x.c.voi ? "VOI" : "",
            poly::value_text(3, x.c.voi, v3, sizeof v3));
  c.put_text(kSlotChord + 1, kRSmall, r.x + r.w - 2, r.y + 1, b, kAlignRight);
  // the keyboard: 21 white keys of 5 px (3 octaves) from the window's C, rows ky..ky+8 (black keys 5), the C names
  // under it in the cell's label rows (it has no labels)
  const int kx = r.x + 1, ky = r.y + 7, kh = 9, whites = 21, pitch = 5;
  // each voice's window (slot 8: no voice) and what it was for (the voice's machine, the pattern loads): a new machine,
  // pattern or project starts it again at C3
  static struct { int c0; uint32_t ctx; } win[9] = {};
  int lo = 127, hi = -1;
  for (int i = 0; i < n; ++i) lo = ns[i] < lo ? ns[i] : lo, hi = ns[i] > hi ? ns[i] : hi;
  int vi = s.voice();
  VoiceSound vs;
  uint32_t ctx = 1u | (uint32_t)((vi >= 0 && voice_sound(vi & 7, &vs) ? vs.machine : 0) & 0xFF) << 1;
#if CORE_POLY
  ctx |= seq::stats().loads << 9;
#endif
  auto& wv = win[vi < 0 ? 8 : vi & 7];
  if (wv.ctx != ctx) wv.ctx = ctx, wv.c0 = -1;
  int base = wv.c0 = keys_window(wv.c0, lo, hi);
  for (int o = 0; o < 3; ++o) {
    sniprintf(v3, sizeof v3, CORE_STR("C%d"), base / 12 - 1 + o);
    c.put_text(kSlotChord + 4 + o, kRSmall, kx + o * 7 * pitch + 1, ky + kh + 1, v3);
  }
  bool lit[36] = {};
  bool past = false;
  for (int i = 0; i < n; ++i) {
    int k = ns[i] - base;
    if (k >= 0 && k < 36) lit[k] = true;
    else past = true;
  }
  c.hline(kx, kx + whites * pitch, ky, kOn);
  c.hline(kx, kx + whites * pitch, ky + kh - 1, kOn);
  for (int w = 0; w <= whites; ++w) c.vline(kx + w * pitch, ky, ky + kh - 1, kOn);
  for (int k = 0; k < 36; ++k) {
    int oct = k / 12, pc = k % 12;
    if (kWhiteOf[pc] >= 0) {
      if (!lit[k]) continue;
      int w = oct * 7 + kWhiteOf[pc];
      c.fill(Rect{(int16_t)(kx + w * pitch + 1), (int16_t)(ky + 5), (int16_t)(pitch - 1), 3}, kOn);
    } else {
      int w = oct * 7 + kBlackAfter[pc] + 1;   // the separator it sits on
      Rect bk = {(int16_t)(kx + w * pitch - 1), (int16_t)ky, 3, 5};
      c.fill(bk, kOff);
      c.fill(bk, lit[k] ? kOn : kDither);
      if (!lit[k]) c.rect(bk, kOn);
    }
  }
  if (past) {   // a note outside the keyboard (3+ octaves of notes): an arrow at its right end
    int ax = kx + whites * pitch + 2, ay = ky + kh / 2;
    c.vline(ax, ay - 2, ay + 2, kOn);
    c.vline(ax + 1, ay - 1, ay + 1, kOn);
    c.pixel(ax + 2, ay, kOn);
  }
  sniprintf(b, sizeof b, "%d", x.sounding);
  c.put_text(kSlotChord + 2, kRSmall, r.x + r.w - 9, ky, b, kAlignCentre);
  c.put_text(kSlotChord + 3, kRSmall, r.x + r.w - 9, ky + 7, x.live ? "LIVE" : "ON", kAlignCentre);
}
CORE_COLD uint32_t chord_key(const Cell& cell, const Source& s) {
  ChordShown x = chord_shown(cell, s);
  return (uint32_t)x.root | (uint32_t)x.c.chd << 8 | (uint32_t)x.c.inv << 13 | (uint32_t)x.c.spr << 16 |
         (uint32_t)x.c.voi << 19 | (uint32_t)x.live << 23 | (uint32_t)(x.sounding & 63) << 24;
}

// ---- MULTISAMPLE's INST: the list entry's name (the card's /MULTISAMPLE, msamp.h), the place in the list under it
CORE_COLD const char* inst_name(int code, int vi) {   // rui1038: the source code (bank * 128 + index), the voice's pick
#if CORE_MSAMP
  const char* n = msamp::entry_name(msamp::src_entry(vi < 0 ? 0 : vi & 7, code));
  return n && *n ? n : "NO INST";
#else
  (void)code, (void)vi;
  return "";
#endif
}
CORE_COLD void draw_inst(Canvas& c, const Rect& r, const Cell& cell, const Source& s, int slot) {
  int inst = s.param(cell.in[0]);
  char t[13];
  const char* name = inst_name(s.code(cell.in[0]), s.voice());
  int k = 0;
  for (; name[k] && k < 12; ++k) t[k] = name[k];
  t[k] = 0;
  c.put_text(slot, kRSmall, r.x + r.w / 2, r.y + 5, t, kAlignCentre);
  int x = r.x + 4 + inst * (r.w - 9) / 127;
  c.hline(r.x + 4, r.x + r.w - 5, r.y + 14, kDither);
  c.fill(Rect{(int16_t)(x - 1), (int16_t)(r.y + 13), 3, 3}, kOn);
}

// ---- the pages
UI_ROM const Cell kJunoCells[] = {
    {kDraw, 0, 0, 1, 1, {0, kNone, kNone, kNone}, kDrawPatch, nullptr},
    {kFilter, 1, 0, 2, 1, {1, 2, kNone, kNone}, kLp, nullptr},
    {kKnob, 3, 0, 1, 1, {3, kNone, kNone, kNone}, 0, nullptr},
    {kDraw, 0, 1, 3, 1, {4, 5, 6, 7}, kDrawEnv, nullptr},
    {kKnob, 3, 1, 1, 1, {7, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kJunoPage = {
    "JUNO", kJuno, sizeof kJunoCells / sizeof kJunoCells[0], kJunoCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"PAT", "CUT", "RES", "ENV", "ATK", "DEC", "REL", "LEN"},
};
UI_ROM const Cell kFmCells[] = {
    {kDraw, 0, 0, 2, 1, {0, kNone, kNone, kNone}, kDrawFm, nullptr},
    {kKnob, 2, 0, 1, 1, {2, kNone, kNone, kNone}, 0, nullptr},
    {kEnv, 3, 0, 1, 1, {kNone, 3, kNone, kNone}, kAd, nullptr},
    {kKnob, 0, 1, 1, 1, {4, kNone, kNone, kNone}, 0, nullptr},
    {kKnobBi, 1, 1, 1, 1, {5, kNone, kNone, kNone}, 0, nullptr},
    {kBar, 2, 1, 1, 1, {6, kNone, kNone, kNone}, 0, nullptr},
    {kKnob, 3, 1, 1, 1, {7, kNone, kNone, kNone}, 0, nullptr},
};
UI_ROM const Page kFmPage = {
    "6-OP FM", kFm, sizeof kFmCells / sizeof kFmCells[0], kFmCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"PAT", "TIM", "MOR", "DEC", "LPG", "TUN", "LEV", "LEN"},
};
// test-1031: the chord views (the SRC view while a chord plays, chord_view_page): the top row as the page's, the chord
// below (P5..P8 still under the Rytm's encoders E..H: the bottom bar names a turn)
UI_ROM const Cell kFmChordCells[] = {
    {kDraw, 0, 0, 2, 1, {0, kNone, kNone, kNone}, kDrawFm, nullptr},
    {kKnob, 2, 0, 1, 1, {2, kNone, kNone, kNone}, 0, nullptr},
    {kEnv, 3, 0, 1, 1, {kNone, 3, kNone, kNone}, kAd, nullptr},
    {kDraw, 0, 1, 4, 1, {12, 13, 14, 15}, kDrawChord, nullptr},
};
UI_ROM const Page kFmChordPage = {
    "6-OP FM", kFm, sizeof kFmChordCells / sizeof kFmChordCells[0], kFmChordCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"PAT", "TIM", "MOR", "DEC", "LPG", "TUN", "LEV", "LEN"},
};
// MULTISAMPLE (machines.h kMultiMachine; msamp.h: P1..P8 INST VEL LEV RTRG ATK DEC SUS REL, P13..P16 the chord)
UI_ROM const char kRtrgNames[] = "OFF\0ON\0";
UI_ROM const Cell kMsCells[] = {
    {kDraw, 0, 0, 2, 1, {0, kNone, kNone, kNone}, kDrawInst, nullptr},
    {kBar, 2, 0, 1, 1, {2, kNone, kNone, kNone}, 0, nullptr},
    {kList, 3, 0, 1, 1, {3, kNone, kNone, kNone}, 0, kRtrgNames},
    {kEnv, 0, 1, 4, 1, {4, 5, 6, 7}, kAdsr, nullptr},
};
UI_ROM const Page kMsPage = {
    "MULTISAMPLE", kMultiMachine, sizeof kMsCells / sizeof kMsCells[0], kMsCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"INST", "VEL", "LEV", "RTRG", "ATK", "DEC", "SUS", "REL"},
};
UI_ROM const Cell kMsChordCells[] = {
    {kDraw, 0, 0, 2, 1, {0, kNone, kNone, kNone}, kDrawInst, nullptr},
    {kBar, 2, 0, 1, 1, {2, kNone, kNone, kNone}, 0, nullptr},
    {kList, 3, 0, 1, 1, {3, kNone, kNone, kNone}, 0, kRtrgNames},
    {kDraw, 0, 1, 4, 1, {12, 13, 14, 15}, kDrawChord, nullptr},
};
UI_ROM const Page kMsChordPage = {
    "MULTISAMPLE", kMultiMachine, sizeof kMsChordCells / sizeof kMsChordCells[0], kMsChordCells,
    {0, 1, 2, 3, 4, 5, 6, 7},
    {"INST", "VEL", "LEV", "RTRG", "ATK", "DEC", "SUS", "REL"},
};

}  // namespace

CORE_COLD const Page* gfx_page_for_machine(int machine) {
  return machine == kJuno ? &kJunoPage : machine == kFm ? &kFmPage : machine == kMultiMachine ? &kMsPage
                                                                   : wavegrain_page_for_machine(machine);
}

CORE_COLD const Page* chord_view_page(const Page* p, const Source& s) {
  const Page* to = p == &kFmPage ? &kFmChordPage : p == &kMsPage ? &kMsChordPage : nullptr;
  return to && chord_on(to->cells[to->ncells - 1], s) ? to : p;
}

CORE_COLD uint32_t gfx_key(const Cell& c, const Source& s) {
  int vi = s.voice() < 0 ? 0 : s.voice();
  switch (c.arg) {
    case kDrawEnv: return (uint32_t)sustain_of(vi) + 1;
    case kDrawFm: return (uint32_t)(fm_alg(s.param(c.in[0])) + 2);
    case kDrawChord: return chord_key(c, s);
    case kDrawInst: {
      const char* name = inst_name(s.code(c.in[0]), s.voice());
      uint32_t h = 2166136261u;
      for (const char* q = name; q && *q; ++q) h = (h ^ (uint8_t)*q) * 16777619u;
      return h;
    }
    case kDrawPatch: {
      const char* name = juno_name(s.param(c.in[0]));
      uint32_t h = 2166136261u;
      for (const char* q = name; q && *q; ++q) h = (h ^ (uint8_t)*q) * 16777619u;
      return h;
    }
    default: return 0;
  }
}

CORE_COLD void gfx_draw(Canvas& cv, const Rect& r, const Cell& c, const Source& s, int slot) {
  switch (c.arg) {
    case kDrawEnv: draw_env(cv, r, c, s); break;
    case kDrawFm: draw_fm(cv, r, c, s); break;
    case kDrawCloud: draw_cloud(cv, r, c, s); break;
    case kDrawPatch: draw_patch(cv, r, c, s, slot); break;
    case kDrawChord: draw_chord(cv, r, c, s); break;
    case kDrawInst: draw_inst(cv, r, c, s, slot); break;
    default: break;
  }
}

CORE_COLD bool fm_info(int pat, FmInfo* o) {
  uint8_t b[128];
  memset(o, 0, sizeof *o);
  o->alg = -1, o->fb_src = o->fb_dst = -1;
  if (!fm_bytes(pat, b)) return false;
  o->alg = b[110] & 31;
  o->fb = b[111] & 7;
  const Alg& a = kAlgs[o->alg];
  Layout L;
  layout(a, L);
  o->ncols = L.ncols, o->nlev = L.nlev;
  memcpy(o->col, L.col, 6), memcpy(o->lev, L.lev, 6), memcpy(o->mod, a.mod, 6);
  o->car = a.car;
  o->fb_src = (a.fb >> 4) - 1, o->fb_dst = (a.fb & 15) - 1;
  for (int k = 0; k < 6; ++k) {   // the packed voice: OP6's 17 bytes first .. OP1's last
    const uint8_t* op = b + (5 - k) * 17;
    o->out[k] = (uint8_t)clampi(op[14], 0, 99);
    int coarse = op[15] >> 1 & 31, fine = clampi(op[16], 0, 99);
    o->fixed[k] = op[15] & 1;
    if (o->fixed[k]) {   // 10^(coarse & 3) Hz x 10^(fine / 100)
      static const uint16_t kDec[4] = {1, 10, 100, 1000};
      float f = kDec[coarse & 3] * __builtin_powf(10.0f, fine / 100.0f);
      o->freq[k] = (uint16_t)(f > 65535.0f ? 65535.0f : f + 0.5f);
    } else {             // coarse 0 = 0.5, else coarse; x (1 + fine / 100)
      int r = coarse ? coarse * 100 : 50;
      o->freq[k] = (uint16_t)(r + r * fine / 100);
    }
  }
  for (int i = 0; i < 10; ++i) {
    char c = (char)(b[118 + i] & 0x7F);
    o->name[i] = c >= 0x20 && c < 0x7F ? c : ' ';
  }
  o->name[10] = 0;
  for (int i = 9; i >= 0 && o->name[i] == ' '; --i) o->name[i] = 0;
  return true;
}

#if CORE_UI_TOOLS
void fm_algorithm_render(int alg, Canvas& c) {   // the renderer: algorithm alg (0..31) in a two-column cell at (0, 0)
  c.clear();
  draw_alg(c, cell(0, 0, 2, 1), alg & 31);
}
int fm_algorithm_layout(int alg, int8_t* col, int8_t* lev) {   // tests: the layout of algorithm alg (0..31); columns
  Layout L;
  layout(kAlgs[alg & 31], L);
  memcpy(col, L.col, 6), memcpy(lev, L.lev, 6);
  return L.ncols;
}
#endif

}  // namespace ui
}  // namespace core
