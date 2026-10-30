// core/ui/page_scope.cpp - see extra.h: the Overbridge input tap, the meters, the SCOPE page and the list of pages drawn
// by code. Cold (program flash on the Teensy), the tap's buffers in RAM2.
//
// SCOPE: the selected source's waveform (MAIN, a Sidecar voice's track, AUDIO IN L / R) over the 128 columns, each
// column the min / max of its entries; short timebases (21..85 ms a screen) triggered on a rising crossing of the
// columns' middle (a steady picture of a tone), longer ones (171 ms..1.4 s) as a sweep (written left to right at a
// cursor: only the new columns' tiles change). Twelve peak meters under it (MAIN L R, voices 1..8, IN L R) with a
// 1 s peak hold. Encoders: A SRC, B TIME, C GAIN (AUTO: the largest power of two that keeps the window's peak on
// screen, x1..x64). The live part is updated at most every kScopeMs and only while the tiles sent stay within
// kTileRate a second (a token bucket over the remote host's tile count), so the Rytm's panel link and MIDI keep room.
#include <string.h>

#include "rec/rec_names.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

volatile bool tap_enabled = false;

namespace {

// Sidecar voice vi -> its IN channel (ob_block.h kVoiceFromRytm: 2 + the track index of physical voice vi)
UI_ROM const uint8_t kVoiceCh[8] = {2 + 0, 2 + 3, 2 + 1, 2 + 4, 2 + 6, 2 + 5, 2 + 7, 2 + 2};
// The IN channels 2..9 are the Rytm's voices in track order (BD SD RS/CP BT LT MT/HT CH/OH CY/CB), and so are the
// meters and the SCOPE's sources 1..8: each shows its track's channel (a Sidecar voice's level on the track that plays
// it, an analog track's its own), named by the track, never by the Sidecar's voice number (voice 1 is BT's).
UI_ROM const char* const kChName[8] = {"BD", "SD", "RS", "BT", "LT", "MT", "CH", "CY"};

volatile int t_src = 0;
CORE_RAM2 int16_t t_ring[kTapRing][2];
volatile uint32_t t_wr = 0;
volatile uint32_t t_peak[kTapCh];
int32_t t_lo = 0x7FFFFFFF, t_hi = -0x7FFFFFFF - 1;   // the IN path's own: the entry being filled
int t_n = 0;

CORE_COLD int src_ch(int src, int* b) {
  int a = src <= 0 ? 0 : src <= 8 ? 1 + src : src == 9 ? 10 : 11;
  *b = src <= 0 ? 1 : a;
  return a;
}

}  // namespace

CORE_COLD void tap_in(const int32_t* f, int n, int stride) {
  if (!tap_enabled || stride < kTapCh) return;
  int b, a = src_ch(t_src, &b);
  uint32_t m[kTapCh] = {};
  int32_t lo = t_lo, hi = t_hi;
  int k = t_n;
  uint32_t w = t_wr;
  for (int i = 0; i < n; ++i) {
    const int32_t* x = f + i * stride;
    for (int c = 0; c < kTapCh; ++c) {
      uint32_t v = (uint32_t)(x[c] < 0 ? ~x[c] : x[c]);
      if (v > m[c]) m[c] = v;
    }
    int32_t s = a == b ? x[a] : (x[a] >> 1) + (x[b] >> 1);
    if (s < lo) lo = s;
    if (s > hi) hi = s;
    if (++k == kTapFrames) {
      int16_t* e = t_ring[w & (kTapRing - 1)];
      e[0] = (int16_t)(lo >> 16), e[1] = (int16_t)(hi >> 16);
      ++w;
      k = 0, lo = 0x7FFFFFFF, hi = -0x7FFFFFFF - 1;
    }
  }
  t_lo = lo, t_hi = hi, t_n = k;
  for (int c = 0; c < kTapCh; ++c)
    if ((m[c] >> 16) > t_peak[c]) t_peak[c] = m[c] >> 16;
  __atomic_store_n(&t_wr, w, __ATOMIC_RELEASE);
}

CORE_COLD void tap_enable(bool on) { tap_enabled = on; }
CORE_COLD void tap_source(int src) { t_src = src < 0 ? 0 : src >= kTapSources ? kTapSources - 1 : src; }
CORE_COLD int tap_channel_of_voice(int vi) { return kVoiceCh[vi & 7]; }
CORE_COLD const char* tap_source_name(int src) {
  static const char* const n[kTapSources] = {"MAIN", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "IN L", "IN R"};
  if (rec::fxin() && (src == 9 || src == 10)) return src == 9 ? "FX L" : "FX R";
  return n[src < 0 || src >= kTapSources ? 0 : src];
}
CORE_COLD void tap_take_peaks(uint16_t out[kTapCh]) {
  for (int c = 0; c < kTapCh; ++c) {
    uint32_t p = __atomic_exchange_n(&t_peak[c], 0u, __ATOMIC_RELAXED);
    out[c] = (uint16_t)(p > 32767 ? 32767 : p);
  }
}
CORE_COLD uint32_t tap_write_pos() { return __atomic_load_n(&t_wr, __ATOMIC_ACQUIRE); }
CORE_COLD int tap_read(uint32_t* rd, int16_t (*out)[2], int max) {
  uint32_t w = tap_write_pos(), r = *rd;
  if (w - r > (uint32_t)(kTapRing - 64)) r = w - (kTapRing - 64);   // overrun: the oldest the writer is not near
  int n = 0;
  for (; r != w && n < max; ++r, ++n) out[n][0] = t_ring[r & (kTapRing - 1)][0], out[n][1] = t_ring[r & (kTapRing - 1)][1];
  *rd = r;
  return n;
}
CORE_COLD void tap_reset() {
  tap_enabled = false;
  t_src = 0;
  t_wr = 0;
  t_n = 0, t_lo = 0x7FFFFFFF, t_hi = -0x7FFFFFFF - 1;
  for (int c = 0; c < kTapCh; ++c) t_peak[c] = 0;
}

// dB x 2 below full scale: 12.04 x (log2 32767 - log2 p), log2 from the leading bit and the next 8 (within 0.1 dB)
CORE_COLD int db2_below(uint32_t p) {
  if (p == 0) return kMeterFloor;
  if (p > 32767) p = 32767;
  int e = 31 - __builtin_clz(p);
  int frac = e >= 8 ? (int)((p >> (e - 8)) & 0xFF) : (int)((p << (8 - e)) & 0xFF);
  // log2(1 + f) ~ f + 0.343 f (1 - f) (within 0.004)
  int l = e * 256 + frac + (frac * (256 - frac) * 88 >> 16);
  int d = ((15 * 256 - l) * 3082) >> 16;   // log2 32767 ~ 15
  return d < 0 ? 0 : d > kMeterFloor ? kMeterFloor : d;
}

CORE_COLD void Meters::reset() {
  for (int c = 0; c < kTapCh; ++c) level[c] = hold[c] = kMeterFloor, hold_ms[c] = 0;
  last_ms = 0;
}

CORE_COLD void Meters::update(const uint16_t peaks[kTapCh], uint32_t ms) {
  uint32_t dt = last_ms ? ms - last_ms : 0;
  last_ms = ms;
  int fall = (int)(dt * 48 / 1000);   // 24 dB a second
  for (int c = 0; c < kTapCh; ++c) {
    int d = db2_below(peaks[c]), l = level[c] + fall;
    level[c] = (uint8_t)(d < l ? d : l > kMeterFloor ? kMeterFloor : l);
    if (d <= hold[c]) hold[c] = (uint8_t)d, hold_ms[c] = ms;
    else if (ms - hold_ms[c] > 1000) {
      int h = hold[c] + fall;
      hold[c] = (uint8_t)(h > level[c] ? level[c] : h);
    }
  }
}

// MAIN L R | the 8 voice channels in track order | IN L R
CORE_COLD int Meters::x(int i) { return i < 2 ? 3 + 8 * i : i < 10 ? 24 + 11 * (i - 2) : 112 + 8 * (i - 10); }
// main_right (REC): the 8 voice channels | MAIN L R | IN L R
CORE_COLD int Meters::pos(int i) const {
  if (!main_right) return x(i);
  return i < 2 ? 90 + 8 * i : i < 10 ? 2 + 11 * (i - 2) : 112 + 8 * (i - 10);
}
static CORE_COLD int meter_ch(int i) { return i; }   // meter i: IN channel i (2..9 the tracks' voices, in track order)

CORE_COLD void Meters::draw(Canvas& c, int y, int h, int slot0) const {
  for (int i = 0; i < kTapCh; ++i) {
    int x = pos(i), ch = meter_ch(i);
    int fill = (kMeterFloor - level[ch]) * h / kMeterFloor, hv = (kMeterFloor - hold[ch]) * h / kMeterFloor;
    c.hline(x, x + 4, y + h - 1, kDither);   // the floor
    if (fill > 0) c.fill(Rect{(int16_t)x, (int16_t)(y + h - fill), 5, (int16_t)fill}, kOn);
    if (hv > 0 && hv > fill) c.hline(x, x + 4, y + h - hv, kOn);
  }
  char b[4];
  c.put_text(slot0, kRSmall, pos(0) + 7, y + h + 2, "MAIN", kAlignCentre);
  for (int v = 0; v < 8; ++v) {
    b[0] = kChName[v][0], b[1] = kChName[v][1], b[2] = 0;
    c.put_text(slot0 + 1 + v, kRSmall, pos(2 + v) + 2, y + h + 2, b, kAlignCentre);
  }
  c.put_text(slot0 + 9, kRSmall, 118, y + h + 2, rec::fxin() ? "FX" : "IN", kAlignCentre);
}

// ---- SCOPE
namespace {

constexpr uint32_t kScopeMs = 66;         // the live part at most 15 times a second
constexpr int kTileRate = 900;            // tiles a second at most (the panel link carries ~1560 [rytm-mods 0026])
constexpr int kTokensMax = 900 * 16 / 10;  // x16: a burst of 90 tiles
constexpr int kTbs = 7;
UI_ROM const uint8_t kTbEntries[kTbs] = {1, 2, 4, 8, 16, 32, 64};   // entries a column (21.3 ms .. 1.37 s a screen)
UI_ROM const char* const kTbName[kTbs] = {"21MS", "43MS", "85MS", "171MS", "341MS", "683MS", "1.4S"};
constexpr int kSweepFrom = 3;             // timebases from here sweep
constexpr int kTop = 11, kRows = 21, kMid = kTop + kRows / 2;   // rows 11..31
constexpr int kMeterY = 34, kMeterH = 14;
constexpr int kHist = 256;
constexpr int kAutoUp = 20;              // AUTO raises its gain after this many frames (~1.5 s at ~13 frames a second)

struct Scope {
  uint8_t src = 0, tb = 1, gain = 0;   // gain 0 AUTO, k: x 2^(k-1)
  bool fresh = true;                   // the source or timebase changed: start over
  uint32_t rd = 0;
  int16_t hist[kHist][2];              // the triggered timebases: the last kHist columns
  int hw = 0, hn = 0;
  int16_t sweep[128][2];               // the sweeping ones: the screen as written
  int cursor = 0, sn = 0;
  int32_t plo = 0, phi = 0;            // the column being gathered
  int pcnt = 0;
  uint32_t last_ms = 0, tiles_mark = 0, tok_ms = 0;
  int32_t tokens = kTokensMax;
  int shown_gain = 1;
  int auto_g = 1, auto_up = 0;          // AUTO: the gain shown, and how many frames a higher one has been wanted
  uint32_t updates = 0;
  Meters m;
};
CORE_RAM2 Scope sc;

CORE_COLD void column(int16_t lo, int16_t hi) {
  if (sc.tb >= kSweepFrom) {
    sc.sweep[sc.cursor][0] = lo, sc.sweep[sc.cursor][1] = hi;
    sc.cursor = (sc.cursor + 1) & 127;
    if (sc.sn < 128) ++sc.sn;
  } else {
    sc.hist[sc.hw][0] = lo, sc.hist[sc.hw][1] = hi;
    sc.hw = (sc.hw + 1) % kHist;
    if (sc.hn < kHist) ++sc.hn;
  }
}

CORE_COLD void scope_update(uint32_t ms) {
  if (sc.fresh) {
    tap_source(sc.src);
    sc.rd = tap_write_pos();   // from now: what the old source wrote is not this one's
    sc.hn = sc.hw = sc.sn = sc.cursor = sc.pcnt = 0;
    sc.auto_g = 64, sc.auto_up = 0;   // a new source: AUTO settles from the top (down at once)
    sc.fresh = false;
  }
  int k = kTbEntries[sc.tb];
  int16_t e[64][2];
  for (int n; (n = tap_read(&sc.rd, e, 64)) > 0;)
    for (int i = 0; i < n; ++i) {
      if (!sc.pcnt) sc.plo = e[i][0], sc.phi = e[i][1];
      if (e[i][0] < sc.plo) sc.plo = e[i][0];
      if (e[i][1] > sc.phi) sc.phi = e[i][1];
      if (++sc.pcnt == k) column((int16_t)sc.plo, (int16_t)sc.phi), sc.pcnt = 0;
    }
  uint16_t pk[kTapCh];
  tap_take_peaks(pk);
  sc.m.update(pk, ms);
  ++sc.updates;
}

// The 128 columns shown (lo, hi): the sweep as written, or the newest window of the history started on a rising
// crossing of the columns' middle (searched over the window before it); fewer than 128 known: from the left.
CORE_COLD int scope_window(int16_t (*out)[2]) {
  if (sc.tb >= kSweepFrom) {
    memcpy(out, sc.sweep, sizeof sc.sweep);
    return sc.sn;
  }
  int n = sc.hn < 128 ? sc.hn : 128;
  int newest = sc.hw - 1, start = newest - n + 1;   // in history positions (mod kHist)
  if (sc.hn >= 2 * 128) {
    int peak = 0;
    for (int i = 0; i < kHist; ++i) {
      int a = sc.hist[i][1] > -sc.hist[i][0] ? sc.hist[i][1] : -sc.hist[i][0];
      if (a > peak) peak = a;
    }
    int thr = peak / 8;
    for (int s = start; s > start - 127; --s) {
      const int16_t* p = sc.hist[(s - 1 + 2 * kHist) % kHist];
      const int16_t* q = sc.hist[(s + 2 * kHist) % kHist];
      if ((p[0] + p[1]) / 2 < -thr && (q[0] + q[1]) / 2 >= 0) { start = s; break; }
    }
  }
  for (int i = 0; i < n; ++i) {
    const int16_t* h = sc.hist[(start + i + 2 * kHist) % kHist];
    out[i][0] = h[0], out[i][1] = h[1];
  }
  return n;
}

CORE_COLD void scope_draw(Canvas& c, int page, int npages, uint32_t ms) {
  // the budget: tiles sent since the last look against kTileRate
  const RemoteStats& st = remote_stats();
  if (!sc.tok_ms) sc.tok_ms = ms, sc.tiles_mark = st.tiles;
  sc.tokens += (int32_t)((ms - sc.tok_ms) * kTileRate * 16 / 1000) - (int32_t)(st.tiles - sc.tiles_mark) * 16;
  sc.tok_ms = ms, sc.tiles_mark = st.tiles;
  if (sc.tokens > kTokensMax) sc.tokens = kTokensMax;
  if (sc.fresh || (ms - sc.last_ms >= kScopeMs && sc.tokens >= 0)) {
    scope_update(ms);
    sc.last_ms = ms;
  }
  c.clear();
  header(c, "SCOPE", page, npages, tap_source_name(sc.src));
  int16_t w[128][2];
  int n = scope_window(w);
  int peak = 0;
  for (int i = 0; i < n; ++i) {
    int a = w[i][1] > -w[i][0] ? w[i][1] : -w[i][0];
    if (a > peak) peak = a;
  }
  int g = 1;
  if (sc.gain) g = 1 << (sc.gain - 1);
  else {
    // AUTO: the peak between half and all of the height, without hunting: a lower gain at once (the trace would clip),
    // a higher one only after it has been wanted for ~1.5 s of frames (kAutoUp), one step at a time
    int want = 1;
    while (want < 64 && peak * 2 * want < 31000) want *= 2;
    if (want < sc.auto_g) { sc.auto_g = want; sc.auto_up = 0; }
    else if (want > sc.auto_g) {
      if (++sc.auto_up >= kAutoUp) { sc.auto_g *= 2; sc.auto_up = 0; }
    } else sc.auto_up = 0;
    g = sc.auto_g;
  }
  sc.shown_gain = g;
  int amp = kRows / 2;
  c.hline(0, kW - 1, kMid, kDither);
  for (int i = 0; i < n; ++i) {
    int y0 = kMid - (int)w[i][1] * g * amp / 32768, y1 = kMid - (int)w[i][0] * g * amp / 32768;
    y0 = y0 < kTop ? kTop : y0 > kTop + kRows - 1 ? kTop + kRows - 1 : y0;
    y1 = y1 < kTop ? kTop : y1 > kTop + kRows - 1 ? kTop + kRows - 1 : y1;
    c.vline(i, y0, y1, kOn);
  }
  if (sc.tb >= kSweepFrom)   // the cursor: two clear columns ahead of the newest
    c.fill(Rect{(int16_t)sc.cursor, (int16_t)kTop, 2, (int16_t)kRows}, kOff);
  sc.m.draw(c, kMeterY, kMeterH, kSlotFree + 8);
  char b[kTextLen + 1];
  if (sc.gain) sniprintf(b, sizeof b, CORE_STR("SRC %s  %s  X%d"), tap_source_name(sc.src), kTbName[sc.tb], g);
  else sniprintf(b, sizeof b, CORE_STR("SRC %s  %s  AUTO X%d"), tap_source_name(sc.src), kTbName[sc.tb], g);
  footer(c, b);
}

CORE_COLD int step(int v, int d, int n) {
  v += d;
  return v < 0 ? 0 : v >= n ? n - 1 : v;
}

CORE_COLD void scope_enc(int e, int delta, bool) {
  if (e == 0) {
    int s = step(sc.src, delta, kTapSources);
    if (s != sc.src) sc.src = (uint8_t)s, sc.fresh = true;
  } else if (e == 1) {
    int t = step(sc.tb, delta, kTbs);
    if (t != sc.tb) sc.tb = (uint8_t)t, sc.fresh = true;
  } else if (e == 2) {
    sc.gain = (uint8_t)step(sc.gain, delta, 8);
  }
}

CORE_COLD bool scope_key(int) { return false; }

UI_ROM const ExtraPage kScopePage = {"SCOPE", nullptr, scope_draw, scope_enc, scope_key, 0x07, true, nullptr, nullptr};
#if CORE_POLY && CORE_MSAMP
UI_ROM const ExtraPage* const kPages[] = {&kLfoPage, &kRecPage, &kScopePage, &kSampPage, &kAutoPage, &kPoolPage, &kSeqPage, &kInsPage, &kSongPage, &kProjectPage, &kMenuPage, &kKeysPage, &kWebUiPage, &kPolyPage, &kChopPage};   // SAMPLE (core/samp) where REC shows, POOL (core/lib/pools.h) after AUTO; INSERT (fx-ins), SONG (core/song), PROJECT (bundles) last
#elif CORE_POLY
UI_ROM const ExtraPage* const kPages[] = {&kLfoPage, &kRecPage, &kScopePage, &kSeqPage, &kInsPage, &kSongPage, &kMenuPage, &kKeysPage, &kWebUiPage, &kPolyPage};   // SEQ (core/seq), INSERT (fx-ins), SONG (core/song) last: LEFT on SIDECAR
#else
UI_ROM const ExtraPage* const kPages[] = {&kLfoPage, &kRecPage, &kScopePage, &kMenuPage, &kKeysPage, &kWebUiPage};   // the page order: LFO, REC (where shown), SCOPE last
#endif

}  // namespace

CORE_COLD int extra_pages() { return (int)(sizeof kPages / sizeof kPages[0]); }
CORE_COLD const ExtraPage& extra_page(int i) { return *kPages[i < 0 ? 0 : i >= extra_pages() ? extra_pages() - 1 : i]; }

#if CORE_UI_TOOLS
// tests and the renderer: the scope's state from the outside
int scope_updates() { return (int)sc.updates; }
void scope_reset() {
  sc = Scope();
  sc.m.reset();
}
#endif

}  // namespace ui
}  // namespace core
