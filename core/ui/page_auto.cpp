// core/ui/page_auto.cpp - the remote UI's AUTO page (the Pi and the Mac: CORE_MSAMP), after SAMPLE: the auto-sampler
// (core/samp/autosamp.h) as three views by its state.
//   PLAN     A the source kind (6-OP FM, JUNO, VOICE copy, TRK, EXT), B its entry (the patch / voice / Rytm track BD..CB /
//            EXT's MIDI output and channel: USB 1..16, DEV 1..16, RYTM 1..16; B pushed: EXT's input L+R, L, R), C LO and D HI note,
//            E STEP (semitones), F velocity LAYERS, G round robins, H HOLD (the note's length; FUNC: x10); E pushed
//            LOOP on / off, H pushed the RELEASE capture (0.5 .. 8 s). The title bar the instrument's name (the source's:
//            the console's asamp=name: sets another), the bottom bar the take count. YES starts.
//            Every list knob steps one entry a detent and stops at its ends (no wrap): turned all the way left is always
//            6-OP FM, all the way right EXT.
//   RUNNING  the progress bar, the take now (D2 V100 RR1), takes done / total, elapsed and the time left (the title bar:
//            OFFLINE for the Sidecar's engines, rendered silently, faster than real time; RUNNING for TRK and EXT, heard
//            as they play). NO or A pushed cancels (the files written so far removed).
//   DONE     the instrument, zones x layers x robins, its size and RAM / STREAM, the loops found, IN THE POOL
//            and its INST number; YES auditions a note C4 on the first MULTISAMPLE track; NO or A pushed: the plan again.
// A Rytm-track or EXT source: its live level in the bottom bar's box (the tap, as SAMPLE's meter).
// Drawn and keyed on the control core, which owns the auto-sampler's plan: the page calls it directly. Cold.
#include "platform.h"

#if CORE_MSAMP && CORE_POLY
#include <string.h>

#include "keys.h"
#include "machines.h"
#include "msamp/msamp.h"
#include "samp/autosamp.h"
#include "samp/dsp.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "ui/widgets.h"

namespace core {
namespace ui {

namespace {

constexpr int kMaxTakes = 512;   // autosamp.cpp's
constexpr uint32_t kTails[] = {500, 1000, 1500, 2500, 4000, 8000};
constexpr int kTailN = (int)(sizeof kTails / sizeof kTails[0]);

bool g_edit = false;          // DONE: back to the plan
int g_aud_vi = -1;            // an audition's voice, its note off at g_aud_ms
uint32_t g_aud_ms = 0;
uint32_t g_start_ms = 0;      // when the running view saw the first take
uint32_t g_noaud_ms = 0;      // DONE: YES found no MULTISAMPLE track to audition on (shown until then)

CORE_COLD int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
// The SRC knob's step: the kind + delta, clamped to 6-OP FM .. EXT (no wrap: a fast turn's large delta lands on an end)
CORE_COLD int auto_src_step(int src, int delta) { return clampi(src + delta, 0, samp::kASources - 1); }
// The tap channel whose level the bottom bar shows for a real-time source (-1: none, offline)
CORE_COLD int live_channel(const samp::Plan& p) {
  if (p.src == samp::kATrack) return 2 + samp::track_stem(p.track);
  if (p.src == samp::kAExt) return p.in == samp::kInR ? 11 : 10;
  return -1;
}
CORE_COLD int live_level(const samp::Plan& p) {
  int ch = live_channel(p);
  if (ch < 0) return -1;
  int l = source_meter_level(ch);
  if (p.src == samp::kAExt && p.in == samp::kInLR) {
    int r = source_meter_level(11);
    if (r > l) l = r;
  }
  return l;
}
CORE_COLD int takes_of(const samp::Plan& p) {
  int notes = p.src == samp::kATrack && !p.chro ? 1 : (p.hi - p.lo) / p.step + 1;
  return notes * p.nvel * p.rr;
}
CORE_COLD void layers_of(samp::Plan* p, int n) {   // n velocities, evenly from 48 to 120 (one: 100)
  p->nvel = n;
  for (int i = 0; i < n; ++i) p->vels[i] = (uint8_t)(n == 1 ? 100 : 48 + 72 * i / (n - 1));
}
CORE_COLD void time_text(char* b, int n, uint32_t ms) {   // 1.5S, 12S
  if (ms < 10000) sniprintf(b, n, CORE_STR("%u.%uS"), (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
  else sniprintf(b, n, CORE_STR("%uS"), (unsigned)(ms / 1000));
}
CORE_COLD void clock_text(char* b, int n, uint32_t s) { sniprintf(b, n, CORE_STR("%u:%02u"), (unsigned)(s / 60), (unsigned)(s % 60)); }

CORE_COLD void bar_box(Canvas& c, int y, int num, int den) {   // the progress bar, rows y..y+7
  c.rect(Rect{4, (int16_t)y, 120, 8});
  int w = den > 0 ? clampi((int)((int64_t)116 * num / den), 0, 116) : 0;
  if (w > 0) c.fill(Rect{6, (int16_t)(y + 2), (int16_t)w, 4});
}

CORE_COLD void plan_draw(Canvas& c, int page, int npages) {
  const samp::Plan& p = samp::asamp_plan();
  samp::AStatus a;
  samp::asamp_status(&a);
  char sname[24], iname[40], nb[8], v[8][12], b[kTextLen + 1];
  samp::asamp_names(p, sname, sizeof sname, iname, sizeof iname);
  char hn[12];
  strncpy(hn, iname, 10), hn[10] = 0;   // (the title bar has room for about 10)
  header(c, "AUTO", page, npages, hn);
  static const char* const kKind[samp::kASources] = {"6-OP FM", "JUNO", "VOICE", "TRK", "EXT"};
  static const char* const kLab[8] = {"SRC", "PATCH", "LO", "HI", "STEP", "LAYER", "RR", "HOLD"};
  static const char* const kLabV[8] = {"SRC", "VOICE", "LO", "HI", "STEP", "LAYER", "RR", "HOLD"};
  static const char* const kLabT[8] = {"SRC", "TRACK", "LO", "HI", "STEP", "LAYER", "RR", "HOLD"};
  static const char* const kLabE[8] = {"SRC", "MIDI", "LO", "HI", "STEP", "LAYER", "RR", "HOLD"};
  strncpy(v[0], kKind[p.src < samp::kASources ? p.src : 0], sizeof v[0] - 1), v[0][sizeof v[0] - 1] = 0;
  if (p.src == samp::kAFm || p.src == samp::kAJuno) strncpy(v[1], sname, 6), v[1][6] = 0;
  else strncpy(v[1], sname, sizeof v[1] - 1), v[1][sizeof v[1] - 1] = 0;
  samp::note_name(p.lo, nb), strcpy(v[2], nb);
  samp::note_name(p.hi, nb), strcpy(v[3], nb);
  sniprintf(v[4], sizeof v[4], CORE_STR("%d"), p.step);
  sniprintf(v[5], sizeof v[5], CORE_STR("%d"), p.nvel);
  sniprintf(v[6], sizeof v[6], CORE_STR("%d"), p.rr);
  time_text(v[7], sizeof v[7], p.hold_ms);
  for (int i = 0; i < 8; ++i) c.put_text(kSlotCells + i, kRSmall, (i % 4) * kCellW + kCellW / 2, 14 + (i / 4) * kCellH, v[i], kAlignCentre);
  const char* const* lab = p.src == samp::kAVoice ? kLabV : p.src == samp::kATrack ? kLabT : p.src == samp::kAExt ? kLabE : kLab;
  labels(c, cell(0, 0, 4, 1), lab, 4, 0);
  labels(c, cell(0, 1, 4, 1), lab + 4, 4, 4);
  char rel[8];
  time_text(rel, sizeof rel, p.tail_ms);
  int n = takes_of(p);
  // spelled out (UX review R6); with the level box (a Rytm track, EXT) 17 characters, else 25
  int lv = live_level(p);
  if (a.state == samp::kAFailed && a.error[0]) {
    sniprintf(b, sizeof b, CORE_STR("ERR %.13s"), a.error);
  } else if (n > kMaxTakes) {
    sniprintf(b, sizeof b, CORE_STR("%d TAKES: %d MAX"), n, kMaxTakes);
  } else if (lv >= 0 && p.src == samp::kAExt) {
    sniprintf(b, sizeof b, CORE_STR("YES: %d TAKES %s"), n, p.in == samp::kInL ? "L" : p.in == samp::kInR ? "R" : "L+R");
  } else if (lv >= 0) {
    sniprintf(b, sizeof b, CORE_STR("YES: GO, %d TAKES"), n);
  } else {
    sniprintf(b, sizeof b, CORE_STR("YES: %d TAKES, R%s%s"), n, rel, p.loop ? " LOOP" : "");
  }
  footer(c, b, lv);   // a Rytm track or EXT: its live level, to see it sound
}

CORE_COLD void run_draw(Canvas& c, int page, int npages, uint32_t ms, const samp::AStatus& a) {
  header(c, "AUTO", page, npages, a.state == samp::kAWriting ? "WRITING" : a.offline ? "OFFLINE" : "RUNNING");
  char b[kTextLen + 1], t1[12], t2[12], note[8] = "", vel[8] = "";
  {   // the label "D2 v100 9/150": the note and the velocity
    int i = 0, k = 0;
    while (a.label[i] && a.label[i] != ' ' && k < 7) note[k++] = a.label[i++];
    note[k] = 0;
    while (a.label[i] == ' ') ++i;
    k = 0;
    while (a.label[i] && a.label[i] != ' ' && k < 7) vel[k++] = (char)(a.label[i] >= 'a' && a.label[i] <= 'z' ? a.label[i] - 32 : a.label[i]), ++i;
    vel[k] = 0;
  }
  int rr = a.rr > 0 ? a.done % a.rr + 1 : 1;
  sniprintf(b, sizeof b, CORE_STR("%s %s RR%d"), note, vel, rr);
  c.put_text(kSlotCells, kRSmall, 64, 14, b, kAlignCentre);
  name_fit(b, sizeof b, a.name, 25);
  c.put_text(kSlotCells + 1, kRSmall, 64, 24, b, kAlignCentre);
  bar_box(c, 32, a.done, a.total);
  sniprintf(b, sizeof b, CORE_STR("%d / %d TAKES"), a.done, a.total);
  c.put_text(kSlotCells + 2, kRSmall, 4, 44, b, kAlignLeft);
  uint32_t el = (ms - g_start_ms) / 1000;
  clock_text(t1, sizeof t1, el);
  if (a.done > 0 && a.state == samp::kATaking) clock_text(t2, sizeof t2, (uint32_t)((uint64_t)el * (a.total - a.done) / a.done));
  else strcpy(t2, "--:--");
  sniprintf(b, sizeof b, CORE_STR("%s  -%s"), t1, t2);
  c.put_text(kSlotCells + 3, kRSmall, 124, 44, b, kAlignRight);
  const samp::Plan& rp = samp::asamp_plan();
  footer(c, "NO / A: CANCEL", a.offline ? -1 : live_level(rp));
}

CORE_COLD void done_draw(Canvas& c, int page, int npages, const samp::AStatus& a) {
  header(c, "AUTO", page, npages, "DONE");
  char b[kTextLen + 1];
  name_fit(b, sizeof b, a.name, 24);
  c.put_text(kSlotCells, kRSmall, 4, 11, b, kAlignLeft);
  sniprintf(b, sizeof b, CORE_STR("%d ZONES, %d LAYER%s, %d RR"), a.zones, a.layers, a.layers == 1 ? "" : "S", a.rr);
  c.put_text(kSlotCells + 1, kRSmall, 4, 20, b, kAlignLeft);
  uint32_t mb10 = a.kb * 10 / 1024;
  bool ram = (uint64_t)a.kb * 1024 * 2 / 3 <= msamp::kAutoRam;   // held as int16 mono: 2/3 of the 24-bit WAVs [inferred from kAutoRam]
  sniprintf(b, sizeof b, CORE_STR("%u.%u MB  %s"), (unsigned)(mb10 / 10), (unsigned)(mb10 % 10), ram ? "RAM" : "STREAM");
  c.put_text(kSlotCells + 2, kRSmall, 4, 29, b, kAlignLeft);
  int inst = -1;
  char key[40];
  sniprintf(key, sizeof key, CORE_STR("/%s.sfz"), a.name);
  for (int k = 0; k < msamp::kList; ++k) {   // its INSTRUMENTS pool row (the auto-sampler put it there)
    const char* lp = msamp::list_path(k);
    if (!*lp) continue;
    if (strstr(lp, key)) {
      inst = k;
      break;
    }
  }
  if (inst >= 0) sniprintf(b, sizeof b, CORE_STR("LOOPS %d/%d  INST %c%03d"), a.loops, a.written, 'A' + (inst >> 7), inst & 127);
  else sniprintf(b, sizeof b, CORE_STR("LOOPS %d/%d"), a.loops, a.written);
  c.put_text(kSlotCells + 3, kRSmall, 4, 38, b, kAlignLeft);
  c.put_text(kSlotCells + 4, kRSmall, 4, 47, inst >= 0 ? "IN THE POOL" : "LOADING...", kAlignLeft);
  footer(c, g_aud_vi >= 0 ? "PLAYING..." : g_noaud_ms && (int32_t)(now_ms() - g_noaud_ms) < 0 ? "NO MULTISAMPLE TRACK" : "YES: AUDITION  NO: PLAN");
}

CORE_COLD void auto_draw(Canvas& c, int page, int npages, uint32_t ms) {
  samp::AStatus a;
  samp::asamp_status(&a);
  if (g_aud_vi >= 0 && (int32_t)(ms - g_aud_ms) > 0) msamp::note_off(g_aud_vi), g_aud_vi = -1;
  c.clear();
  source_meter_update(ms);
  bool running = a.state == samp::kATaking || a.state == samp::kAWriting;
  if (a.state != samp::kADone) g_edit = false;
  if (running) run_draw(c, page, npages, ms, a);
  else if (a.state == samp::kADone && !g_edit) done_draw(c, page, npages, a);
  else plan_draw(c, page, npages);
}

CORE_COLD void auto_enc(int e, int delta, bool func) {
  if (!delta) return;
  samp::AStatus a;
  samp::asamp_status(&a);
  if (a.state == samp::kATaking || a.state == samp::kAWriting) return;
  g_edit = true;
  samp::Plan p = samp::asamp_plan();
  int stp = func ? 8 : 1;
  switch (e) {
    case 0: {   // the source kind: an ordered list, clamped at its ends (one kind a detent)
      int s = auto_src_step((int)p.src, delta);
      if (s != (int)p.src) {
        p.src = (samp::ASrc)s;
        p.pat = p.voice = p.track = 0, p.chro = false;
        if (p.src == samp::kATrack) p.midi_ch = 0;
        if (p.src == samp::kAExt) {   // a synth on the Pi's USB-A when there is one, else the computer
          p.midi_ch = 0;
          p.port = (keys::view().present & 0x0F) ? samp::kPortDev : samp::kPortUsb;
        }
      }
      break;
    }
    case 1: {
      int n = samp::asamp_entries(p.src);
      if (p.src == samp::kAExt) {   // USB 1..16, DEV 1..16, RYTM 1..16
        int i = clampi(p.port * 16 + (p.midi_ch & 15) + delta * (func ? 16 : 1), 0, n - 1);
        p.port = (uint8_t)(i / 16), p.midi_ch = i % 16;
        break;
      }
      int* q = p.src == samp::kAVoice ? &p.voice : p.src == samp::kATrack ? &p.track : &p.pat;
      *q = clampi(*q + delta * (p.src == samp::kAFm || p.src == samp::kAJuno ? stp : 1), 0, n - 1);
      if (p.src == samp::kATrack) p.midi_ch = p.track;   // the Rytm's default: track N on channel N
      break;
    }
    case 2: p.lo = clampi(p.lo + delta * (func ? 12 : 1), 0, p.hi); break;
    case 3: p.hi = clampi(p.hi + delta * (func ? 12 : 1), p.lo, 127); break;
    case 4: p.step = clampi(p.step + delta, 1, 24); break;
    case 5: layers_of(&p, clampi(p.nvel + delta, 1, 8)); break;
    case 6: p.rr = clampi(p.rr + delta, 1, 4); break;
    case 7: p.hold_ms = (uint32_t)clampi((int)p.hold_ms + delta * (func ? 1000 : 100), 100, 20000); break;
  }
  samp::asamp_configure(p);
}

CORE_COLD bool auto_key(int code) {
  samp::AStatus a;
  samp::asamp_status(&a);
  bool running = a.state == samp::kATaking || a.state == samp::kAWriting;
  if (running) {
    if (code == 0x45 || code == 0x70) {   // NO, A pushed
      samp::asamp_cancel();
      return true;
    }
    return code == 0x44;
  }
  if (a.state == samp::kADone && !g_edit) {
    if (code == 0x44) {   // YES: a note C4 on the first MULTISAMPLE track, a second long
      for (int t = 0; t < 12 && g_aud_vi < 0; ++t) {
        int vb = remote_track_machine(t);
        if (vb >= 0 && (vb & 0x78) == kMultiMachine) {
          int vi = vb & 7;
          int inst = 0;
          char key[40];
          sniprintf(key, sizeof key, CORE_STR("/%s.sfz"), a.name);
          for (int k = 0; k < msamp::kList; ++k)
            if (*msamp::list_path(k) && strstr(msamp::list_path(k), key)) inst = k;
          float pr[16] = {(float)(inst & 127) + (float)((inst >> 7) << 4) / 128.0f, 127, 100, 0, 64, 64, 64, 64, 64, 64, 0, 0, 0, 0, 0, 0};
          msamp::report(vi, 60, 100, pr);
          g_aud_vi = vi, g_aud_ms = now_ms() + 1200;
        }
      }
      if (g_aud_vi < 0) g_noaud_ms = (now_ms() + 1500) | 1;
      return true;
    }
    if (code == 0x45 || code == 0x70) {
      g_edit = true;
      return true;
    }
    return false;
  }
  if (code == 0x44) {
    samp::Plan p = samp::asamp_plan();
    char err[64];
    if (!samp::asamp_start(p, err, sizeof err)) log(CORE_STR("asamp: remote page: %s"), err);
    g_edit = false;
    g_start_ms = now_ms() | 1;
    return true;
  }
  if (code == 0x71) {   // B pushed: EXT's input L+R, L, R
    samp::Plan p = samp::asamp_plan();
    if (p.src != samp::kAExt) return false;
    p.in = (uint8_t)((p.in + 1) % 3);
    samp::asamp_configure(p);
    return true;
  }
  if (code == 0x74) {   // E pushed: LOOP
    samp::Plan p = samp::asamp_plan();
    p.loop = !p.loop;
    samp::asamp_configure(p);
    return true;
  }
  if (code == 0x77) {   // H pushed: the release capture's next
    samp::Plan p = samp::asamp_plan();
    int i = 0;
    while (i < kTailN - 1 && kTails[i] < p.tail_ms) ++i;
    p.tail_ms = kTails[kTails[i] == p.tail_ms ? (i + 1) % kTailN : i];
    samp::asamp_configure(p);
    return true;
  }
  return false;
}

CORE_COLD bool auto_shown() { return kSampPage.shown && kSampPage.shown(); }

}  // namespace

UI_ROM const ExtraPage kAutoPage = {"AUTO", auto_shown, auto_draw, auto_enc, auto_key, 0xFF, true, nullptr, nullptr, nullptr};

}  // namespace ui
}  // namespace core
#endif
