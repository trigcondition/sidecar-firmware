// core/seq/filltargets.cpp - see filltargets.h. Control side; cold.
#include "seq/filltargets.h"

#if CORE_POLY
#include <string.h>

#include "machines.h"
#include "md/md.h"
#include "platform.h"
#include "presets/preset_index.h"
#include "ui/page.h"
#include "ui/remote.h"
#include "ui/widgets.h"
#include "user/um.h"
#if CORE_MSAMP
#include "lib/pools.h"
#endif

namespace core {
namespace filltargets {

namespace {
UI_ROM const char kDaisyNames[] = "MODAL\0STRING\0RESON\0";
// a machine page's list names for P(param + 1) (PLAITS' ENG: the curated models)
const char* page_names(int machine, int param) {
  const ui::Page* p = ui::page_for_machine(machine);
  for (int i = 0; p && i < p->ncells; ++i)
    if (p->cells[i].kind == ui::kList && p->cells[i].in[0] == param) return p->cells[i].names;
  return nullptr;
}
int zone_lo(int z, int n) { return n > 0 ? (z * 128 + n - 1) / n : 0; }
int zone_of(int v, int n) { return n > 0 ? eng_zone((float)v, n) : 0; }
void set_label(Def& d, const char* s) {
  memset(d.label, 0, sizeof d.label);
  for (int i = 0; s && s[i] && i < (int)sizeof d.label - 1; ++i) d.label[i] = s[i];
}
Def lock_def(const char* label, int p, bool fine, uint8_t values, int arg, int n) {
  Def d = {};
  set_label(d, label);
  d.g = lockfill::Target{lockfill::kLock, (uint8_t)p, fine};
  d.values = values, d.arg = (int8_t)arg, d.n = (int16_t)n, d.order = lockfill::kUp;
  return d;
}
int velocity(Def* out, int max) {
  if (max < 1) return 0;
  Def v = {};
  set_label(v, "VELOCITY");
  v.g = lockfill::Target{lockfill::kVelocity, 0, false};
  v.values = kRange, v.n = 127, v.lo = 1, v.order = lockfill::kRnd;
  out[0] = v;
  return 1;
}
}  // namespace

CORE_COLD int for_machine(int m, int md_engine, Def* out, int max) {
  int n = 0;
  auto add = [&](const Def& d) {
    if (n < max) out[n++] = d;
  };
#if CORE_MSAMP
  if (m == kDspSample || m == kSmplMachine) add(lock_def("SAMPLE", 10, true, kPool, pools::kSamples, pools::kCodes));
  if (m == kGranMachine) add(lock_def("SOURCE", 0, true, kPool, pools::kSamples, pools::kCodes));
  if (m == kWaveMachine) add(lock_def("TABLE", 0, true, kPool, pools::kTables, pools::kCodes));
  if (m == kMultiMachine) add(lock_def("INST", 0, true, kPool, pools::kInstr, pools::kCodes));
#endif
#if CORE_MD
  if (m == kMd) {
    add(lock_def("ENGINE", 8, false, kMdEngines, 0, md::engines()));
    int e = md_engine < 0 ? 0 : md_engine >= md::engines() ? md::engines() - 1 : md_engine;
    add(lock_def("TYPE", 9, false, kMdTypes, e, md::types(e)));
  }
#endif
  if (m == kPlaits) {
    Def d = lock_def("ENGINE", 0, false, kZones, 0, kPlaitsEngs);
    d.names = page_names(kPlaits, 0);
    add(d);
  }
  if (m == kDaisy) {
    Def d = lock_def("ENGINE", 0, false, kZones, 0, ui::list_count(kDaisyNames));
    d.names = kDaisyNames;
    add(d);
  }
  if (m == kJuno) add(lock_def("PRESET", 0, false, kPresets, presets::kIxJuno, presets::rows(presets::kIxJuno)));
  if (m == kFm) add(lock_def("PRESET", 0, false, kPresets, presets::kIxFm, presets::rows(presets::kIxFm)));
  int us = user_slot_of(m);
  if (us >= 0) {
    um::FillKnob k[8];
    int nk = um::fill_knobs(us, k, 8);
    return n + for_knobs(us, k, nk, um::presets(us), out + n, max - n);
  }
  return n + velocity(out + n, max - n);
}

CORE_COLD int for_knobs(int us, const um::FillKnob* k, int nk, int np, Def* out, int max) {
  int n = 0;
  for (int i = 0; i < nk && n < max - 1; ++i) {   // (room for VELOCITY kept)
    if (k[i].kind == um::kFillPreset) {
      if (np > 0) out[n++] = lock_def("PRESET", k[i].p - 1, false, kUserPresets, us, np);
    } else if (k[i].kind == um::kFillPool) {
#if CORE_MSAMP
      out[n++] = lock_def("SAMPLE", k[i].p - 1, false, kPool, pools::kSamples, pools::kRows);   // a pool slot: bank A's rows
#endif
    } else if (k[i].count > 1) {
      Def d = lock_def(k[i].label, k[i].p - 1, false, kUserZones, us, k[i].count);
      d.p1 = k[i].p;
      out[n++] = d;
    }
  }
  return n + velocity(out + n, max - n);
}

CORE_COLD int track_machine(int t) {
  int vb = ui::remote_track_machine(t);
  if (vb < 0 || vb == 0x7F) return -1;
  int m = machine_of((uint8_t)vb);
  return m == kDspSample && ui::remote_track_smpl(t) ? kSmplMachine : m;
}

CORE_COLD int for_track(int t, Def* out, int max) {
  int m = track_machine(t), e = 0;
#if CORE_MD
  if (m == kMd) e = zone_of(ui::remote_track_param(t, 8), md::engines());
#endif
  return for_machine(m, e, out, max);
}

CORE_COLD uint16_t encode(const Def& d, int i) {
  switch (d.values) {
#if CORE_MSAMP
    case kPool: return lockfill::value(i & 127, ((i >> 7) & 7) << 4);   // coarse the row, fine bank << 4 (srclib::lo_of)
#endif
    case kZones:
    case kUserZones:
    case kMdEngines:
    case kMdTypes: return lockfill::value(zone_lo(i, d.n));
    case kRange: return lockfill::value(d.lo + i);
    default: return lockfill::value(i);   // a preset row / entry
  }
}

CORE_COLD int index_of(const Def& d, uint16_t v) {
  if ((v & 0xFF) == lockfill::kNoValue) return -1;
  int c = lockfill::coarse_of(v), i = c;
  switch (d.values) {
    case kPool: i = d.n > 128 ? ((lockfill::fine_of(v) >> 4) & 7) * 128 + c : c; break;
    case kZones:
    case kUserZones:
    case kMdEngines:
    case kMdTypes: i = zone_of(c, d.n); break;
    case kRange: i = c - d.lo; break;
    default: break;
  }
  return i >= 0 && i < d.n ? i : -1;
}

CORE_COLD bool skip(const Def& d, int i) {
  if (i < 0 || i >= d.n) return true;
  switch (d.values) {
#if CORE_MSAMP
    case kPool: {
      if (!(i & 127)) return true;   // row 000 of each bank: OFF
      uint8_t ty = pools::at(d.arg, i).type;
      return ty == pools::kEmpty || ty == pools::kOff;
    }
#endif
    case kPresets: return !presets::row_used(d.arg, i);
    default: return false;
  }
}

CORE_COLD const char* name(const Def& d, int i, char* b, int cap) {
  b[0] = 0;
  if (i < 0 || i >= d.n) return b;
  switch (d.values) {
#if CORE_MSAMP
    case kPool: {
      char r[6];
      pools::row_text(i, r);
      const pools::Entry& e = pools::at(d.arg, i);
      sniprintf(b, cap, "%s %s", r, e.type == pools::kEmpty ? "---" : e.name);
      break;
    }
#endif
    case kZones: sniprintf(b, cap, "%s", d.names ? ui::list_name(d.names, i) : ""); if (!b[0]) sniprintf(b, cap, "%d", i + 1); break;
#if CORE_MD
    case kMdEngines: sniprintf(b, cap, "%s", md::engine_name(i)); break;
    case kMdTypes: sniprintf(b, cap, "%s", md::type_name(d.arg, i)); break;
#endif
    case kPresets: {
      char nm[24];
      sniprintf(b, cap, "%03d %s", i, presets::row_name(d.arg, i, nm, sizeof nm));
      break;
    }
    case kUserPresets: {
      const char* nm = um::preset_name(d.arg, i);
      sniprintf(b, cap, "%03d %s", i, nm);
      break;
    }
    case kUserZones: {
      char nm[36][8];
      int k = um::knob_names(d.arg, d.p1, nm);
      if (i < k) sniprintf(b, cap, "%s", nm[i]);
      else sniprintf(b, cap, "%d", i + 1);
      break;
    }
    case kRange: sniprintf(b, cap, "%d", d.lo + i); break;
    default: sniprintf(b, cap, "%d", i); break;
  }
  return b;
}

CORE_COLD int step(const Def& d, int i, int dir) {
  for (int k = i + dir; k >= 0 && k < d.n; k += dir)
    if (!skip(d, k)) return k;
  return i;
}

CORE_COLD int value_set(const Def& d, int from, int to, uint16_t* set, int max) {
  if (d.n <= 0) return 0;
  from = from < 0 ? 0 : from >= d.n ? d.n - 1 : from;
  to = to < 0 ? 0 : to >= d.n ? d.n - 1 : to;
  int n = 0, dir = to >= from ? 1 : -1;
  for (int i = from;; i += dir) {
    if (i >= 0 && i < d.n && !skip(d, i) && n < max) set[n++] = encode(d, i);
    if (i == to || n >= max) break;
  }
  return n;
}

}  // namespace filltargets
}  // namespace core
#endif
