// core/inst_alloc.cpp - see inst_alloc.h. Pure, cold (per note, per config change).
#include "inst_alloc.h"

#include <math.h>
#include <string.h>

#include "machines.h"
#include "platform.h"
#include "rmp_format.h"

namespace core {
namespace inst {

namespace {
// [measured: docs/pi-engine.md task7-bench, the mean % of a Pi 4 core a voice x 100]; the peak for the budget: the
// worst warm block over every machine (cores 1-3: 0.6..1.8 %) [measured]. MULTISAMPLE / WAVETABLE / GRANULAR were not
// in that bench: DSP SAMPLE's figure for MULTISAMPLE (a streamed sample a note), FM's for the others [inferred].
constexpr uint16_t kPeak = 180;
const Decl kBuiltins[] = {
    // machine, AUTO, max, pitch, own pool, mean, peak
    {kPlaits, 4, 8, kPitchNote, false, 73, kPeak},
    {kJuno, 6, 8, kPitchNote, false, 54, kPeak},          // the Juno-106's six
    {kDaisy, 4, 8, kPitchNote, false, 97, kPeak},
    {kDspSample, 1, 1, kPitchNote, false, 47, kPeak},     // one card stream a voice: mono
    {kGrainMachine, 1, 1, kPitchNote, false, 47, kPeak},  // DSP SAMPLE's GRAIN: the same voice
    {kMd, 1, 1, kPitchNone, false, 47, kPeak},            // MD (core/md/md.h): mono drums, the note ignored (PTCH a knob);
                                                          // cpu: placeholders (DSP SAMPLE's mean, the shared peak) until
                                                          // the Pi's boot bench "md" row measures it (mono: no budget use)
    {kFm, 6, 8, kPitchNote, true, 122, kPeak},            // poly.h kPerVoice
    {kMultiMachine, 8, 8, kPitchNote, true, 47, kPeak},   // (its pool holds 16 a voice; 8 the most VOICES says)
    {kWaveMachine, 8, 8, kPitchNote, true, 122, kPeak},   // wavetable.h kPerVoice
    {kGranMachine, 6, 8, kPitchNote, true, 122, kPeak},   // granular.h kPerVoice
    {kLiveMachine, 1, 1, kPitchNone, false, 4, kPeak},   // LIVE IN: one input a voice, mono only (livein.h)
    {kChopMachine, 8, 8, kPitchNote, true, 30, kPeak},    // CHOP: its own pool of slice players (chop.h kPerVoice; the note is the slice)
};
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

CORE_COLD const Decl* builtin(int m) {
  if (user_slot_of(m) >= 0) return nullptr;
  for (const Decl& d : kBuiltins)
    if (d.machine == m) return &d;
  return &kBuiltins[0];   // anything else plays as PLAITS (machines.h)
}

CORE_COLD Decl user_decl(int machine, uint32_t flags, uint32_t cpu_permille, uint32_t state_bytes) {
  (void)state_bytes;
  Decl d = {};
  d.machine = machine;
  d.max_voices = (uint8_t)clampi(rmp_hf_max_voices(flags), 1, kMaxInst);
  d.pitch = rmp_hf_pitch(flags) == RMP_PITCH_NONE ? kPitchNone : kPitchNote;
  d.def_voices = (uint8_t)(d.pitch == kPitchNone ? 1 : d.max_voices < 4 ? d.max_voices : 4);
  d.own_pool = false;
  if (cpu_permille > 100000u) cpu_permille = 100000u;   // (a bogus header: no wrap past the budget)
  uint32_t mean = (cpu_permille * 1582u + 5000u) / 10000u;   // x 100: permille x 12.5 / 7.9 / 10
  d.cpu_mean = (uint16_t)(mean < 1 ? 1 : mean > 2000 ? 2000 : mean);
  d.cpu_peak = (uint16_t)(d.cpu_mean * 5 / 2);
  return d;
}

CORE_COLD Resolved resolve(const seq::PolyCfg& cfg, const Decl& d) {
  Resolved r = {};
  int voices = cfg.voices ? cfg.voices : d.def_voices;
  int uni = cfg.unison > 1 && !d.own_pool ? cfg.unison : 1;   // (UNISON stacks the instance pool's machines only)
  int maxi = d.max_voices < kMaxInst ? d.max_voices : kMaxInst;
  // the budget: the voice's own machine and n instances, each at its peak
  int fit = d.cpu_peak ? kBudgetPct100 / d.cpu_peak - 1 : kMaxInst;
  if (fit < 1) fit = 1;
  if (fit < maxi && !d.own_pool) maxi = fit;
  if (uni > maxi) uni = maxi;
  int want = voices * uni;
  r.unison = uni;
  r.lanes = clampi(want <= maxi ? voices : maxi / uni, 1, kMaxInst);
  r.instances = r.lanes * uni;
  r.clamped = want > r.instances;
  r.cpu100 = r.instances * d.cpu_mean;
  return r;
}

CORE_COLD void reset(Alloc& a, int lanes) {
  memset(&a, 0, sizeof a);
  a.lanes = clampi(lanes, 1, kMaxInst);
  for (Lane& l : a.lane) l.note = -1;
}

namespace {
int oldest_held(const Alloc& a) {
  int b = -1;
  for (int k = 0; k < a.lanes; ++k)
    if (a.lane[k].held && (b < 0 || (int32_t)(a.lane[k].order - a.lane[b].order) < 0)) b = k;
  return b;
}
int quietest_released(const Alloc& a) {
  int b = -1;
  for (int k = 0; k < a.lanes; ++k) {
    const Lane& l = a.lane[k];
    if (l.held || !l.sounding) continue;
    if (b < 0 || l.level < a.lane[b].level || (l.level == a.lane[b].level && (int32_t)(l.order - a.lane[b].order) < 0)) b = k;
  }
  return b;
}
int idle_from(const Alloc& a, int from) {
  for (int i = 0; i < a.lanes; ++i) {
    int k = (from + i) % a.lanes;
    if (!a.lane[k].held && !a.lane[k].sounding) return k;
  }
  return -1;
}
}  // namespace

CORE_COLD int note_on(Alloc& a, const seq::PolyCfg& cfg, int note, int* stole) {
  if (a.lanes < 1) reset(a, 1);
  int k = -1;
  bool same = false;
  for (int i = 0; i < a.lanes && k < 0; ++i)
    if (a.lane[i].held && a.lane[i].note == note) k = i, same = true;   // retrigger, never a second copy
  if (k < 0 && a.lanes == 1) k = 0;                                    // mono: the new note chokes the old
  if (k < 0) {
    switch (cfg.alloc) {
      case seq::kAllocRotate:
        for (int i = 0; i < a.lanes && k < 0; ++i) {
          int j = (a.rot + i) % a.lanes;
          if (!a.lane[j].held) k = j;
        }
        break;
      case seq::kAllocReset:
        for (int i = 0; i < a.lanes && k < 0; ++i)
          if (!a.lane[i].held) k = i;
        break;
      default:   // REASSIGN
        for (int i = 0; i < a.lanes && k < 0; ++i)
          if (!a.lane[i].held && a.lane[i].note == note && a.lane[i].sounding) k = i;
        if (k < 0) k = idle_from(a, a.rot);
        if (k < 0) k = quietest_released(a);
        break;
    }
    if (k < 0) k = oldest_held(a);
  }
  Lane& l = a.lane[k];
  int s = same ? 0 : l.held ? 1 : l.sounding ? 2 : 0;
  if (s == 1) ++a.steals;
  if (stole) *stole = s;
  l.note = (int8_t)note, l.held = true, l.sounding = true, l.order = ++a.clock, l.level = 0;
  a.rot = (k + 1) % a.lanes;
  return k;
}

CORE_COLD int note_off(Alloc& a, int note) {
  for (int k = 0; k < a.lanes; ++k)
    if (a.lane[k].held && a.lane[k].note == note) {
      a.lane[k].held = false;
      return k;
    }
  return -1;
}

CORE_COLD const char* alloc_name(int a) {
  static const char* const n[seq::kAllocs] = {"REASSIGN", "ROTATE", "RESET"};
  return a >= 0 && a < seq::kAllocs ? n[a] : n[0];
}

CORE_COLD bool parse_cfg(const char* kv, seq::PolyCfg* c, char* err, int cap) {
  if (!strcmp(kv, "auto")) {
    *c = seq::PolyCfg{};
    return true;
  }
  for (const char* k = kv; *k;) {
    const char* e = strchr(k, '=');
    const char* next = strchr(k, ',');
    if (!e || (next && next < e)) {
      if (err) sniprintf(err, cap, "KEY=VALUE pairs (voices alloc unison detune spread group)");
      return false;
    }
    const char* v = e + 1;
    int x = 0;
    bool num = *v >= '0' && *v <= '9';
    for (const char* q = v; *q >= '0' && *q <= '9' && x < 1000; ++q) x = x * 10 + (*q - '0');
    switch (*k) {
      case 'v': c->voices = (uint8_t)clampi(x, 0, seq::kMaxVoicesCfg); break;
      case 'u': c->unison = (uint8_t)clampi(x, 0, seq::kMaxUnison); break;
      case 'd': c->detune = (uint8_t)clampi(x, 0, 127); break;
      case 's': c->spread = (uint8_t)clampi(x, 0, 127); break;
      case 'g': c->group = (uint8_t)clampi(x, 0, seq::kPolyGroups); break;
      case 'a': {
        int a = -1;
        static const char* const n[seq::kAllocs] = {"reassign", "rotate", "reset"};
        for (int i = 0; i < seq::kAllocs; ++i)
          if (!strncmp(v, n[i], strlen(n[i]))) a = i;
        if (a < 0 && num && x < seq::kAllocs) a = x;
        if (a < 0) {
          if (err) sniprintf(err, cap, "alloc=reassign|rotate|reset");
          return false;
        }
        c->alloc = (uint8_t)a;
        break;
      }
      default:
        if (err) sniprintf(err, cap, "unknown key %.*s", (int)(e - k), k);
        return false;
    }
    if ((*k != 'a' && !num)) {
      if (err) sniprintf(err, cap, "%.*s: a number", (int)(e - k), k);
      return false;
    }
    k = next ? next + 1 : v + strlen(v);
  }
  return true;
}

CORE_COLD int describe(const seq::PolyCfg& c, const Resolved* r, int machine, char* out, int cap) {
  int n = c.voices ? sniprintf(out, cap, "VOICES %d", c.voices) : sniprintf(out, cap, "VOICES AUTO");
  if (n < cap)
    n += sniprintf(out + n, cap - n, " %s UNISON %d DETUNE %d SPREAD %d GROUP %d", alloc_name(c.alloc),
                   c.unison > 1 ? c.unison : 1, c.detune, c.spread, c.group);
  if (r && n < cap)
    n += sniprintf(out + n, cap - n, " -> %s: %d instance%s, %d note%s at once%s (%d.%02d %% a core)", machine_short(machine),
                   r->instances, r->instances == 1 ? "" : "s", r->lanes, r->lanes == 1 ? "" : "s",
                   r->clamped ? ", CLAMPED" : "", r->cpu100 / 100, r->cpu100 % 100);
  return n;
}

CORE_COLD int describe_short(const Resolved& r, int machine, char* out, int cap) {
  return sniprintf(out, cap, "%s %d INST %d NOTE%s %d.%02d%%%s", machine_short(machine), r.instances, r.lanes,
                   r.lanes == 1 ? "" : "S", r.cpu100 / 100, r.cpu100 % 100, r.clamped ? " CUT" : "");
}

CORE_COLD void unison_offset(const seq::PolyCfg& cfg, int u, int k, float* cents, float* pan) {
  float x = u > 1 ? 2.0f * (float)k / (float)(u - 1) - 1.0f : 0.0f;   // -1 .. 1 across the stack
  *cents = x * 50.0f * (float)cfg.detune / 127.0f;
  *pan = x * (float)cfg.spread / 127.0f;
}

}  // namespace inst
}  // namespace core
