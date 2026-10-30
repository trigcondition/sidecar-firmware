// core/md/md.cpp - see md.h. Control-core code is cold; render is the render core's.
#include "md/md.h"

#if CORE_MD
#include <string.h>

#include "glue/md_tables.h"   // plugins/mdkit/md_tables.h (gen_tables.py from md.json), copied by setup_engines.sh
#include "hooks.h"
#include "rmp_api.h"   // (teensy/libraries/RytmPlugins/src: the host's copy, as core/user/um.cpp)
#include "machines.h"
#include "platform.h"
#include "rmp_proto.h"
#include "sysex.h"

extern "C" {
extern const rmp_plugin mdk_builtin_entry;   // plugins/mdkit/mdk.c (MDK_FAM_MD), compiled as MDKit/mdk_builtin.c
int mdk_builtin_types(void);
int mdk_builtin_controls(int t);
int mdk_builtin_default(int t, int k);
}

namespace core {
namespace md {

namespace {
uint8_t* g_state[kVoices];   // each voice's state (8-aligned, the plugin's state_bytes), on the heap at init
bool g_md_layout[kVoices];   // the voice's last report had kLayoutTag (else DIGIDRUM's page)
bool g_ok = false;
float g_q[kVoices][16];      // the knobs as the voice last got them (voice_set)
int8_t g_pending[kVoices];   // a set change params() made, waiting for its trig (its tail + set-up), -1 none

// DIGIDRUM's 11 engines (its ENG zones) -> MD's set, and how many of DIGIDRUM's controls 1..7 (P2..P8) carry over as
// MD's P1..: the same control names in the same order (DIGIDRUM's labels against mdkit's: TRX-CB from its 4th on
// differs, DIGIDRUM TONE BUMP .. DIST against mdkit DAMP TONE BUMP); the rest at MD's defaults
struct Legacy { uint8_t set, keep; };
const Legacy kLegacy[11] = {{0, 7}, {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}, {6, 7}, {7, 7},   // EFM BD..CY: FM BD..CY
                            {21, 3}, {22, 3}, {27, 6}};                                      // TRX-RS CB CL: TRX RS CB CL

int zone(float f, int n) {   // mdk.c's zone: the integer part, clamped, in n zones
  int z = f >= 0.0f && f <= 127.0f ? (int)f : f > 127.0f ? 127 : 0;
  return (z * n) >> 7;
}
int lo(int z, int n) { return (z * 128 + n - 1) / n; }   // a zone's first value
int engine_of(int set) {
  int e = 0;
  while (e + 1 < MDT_ENGINES && set >= mdt_base[e + 1]) ++e;
  return e;
}
int gen_of(int set) { return 1 + set + 32 * kTableVersion; }
void copy_text(char* d, const char* s, int n) {   // n chars at most, NUL-padded (d holds n + 1)
  int i = 0;
  for (; s[i] && i < n; ++i) d[i] = s[i];
  for (; i <= n; ++i) d[i] = 0;
}
}  // namespace

CORE_COLD uint32_t state_bytes() { return mdk_builtin_entry.state_bytes; }

CORE_COLD void enter(int vi) {
  if (!g_ok || vi < 0 || vi >= kVoices) return;
  memset(g_state[vi], 0, mdk_builtin_entry.state_bytes);
  mdk_builtin_entry.voice_init(g_state[vi]);   // (FM BD: the knobs' set 0)
  memset(g_q[vi], 0, sizeof g_q[vi]);
  g_pending[vi] = -1;
}

CORE_COLD void init() {
  if (!g_ok) {
    static_assert(MDT_SETS == kSets, "md.h kSets is md.json's TYPE count");
    rmp_host h = {};
    h.api = RMP_API, h.sample_rate = 48000.0f, h.max_block = kBlock;
    if (mdk_builtin_types() != kSets || mdk_builtin_entry.init(&h) != 0) {
      log(CORE_STR("md: the built-in MD refused to start: 0x40 stays silent"));
      return;
    }
    uint32_t n = (mdk_builtin_entry.state_bytes + 7u) / 8u;
    // plain new as the rest of core (the Pi's kernel has no std::nothrow; core/store/store.cpp): a null is MD off
    for (int vi = 0; vi < kVoices; ++vi) {
      g_state[vi] = (uint8_t*)new uint64_t[n];
      if (!g_state[vi]) {
        log(CORE_STR("md: no memory for the voices' states (%lu B each): 0x40 stays silent"), (unsigned long)(n * 8u));
        for (int k = 0; k < vi; ++k) delete[] (uint64_t*)g_state[k], g_state[k] = nullptr;
        return;
      }
    }
    g_ok = true;
  }
  for (int vi = 0; vi < kVoices; ++vi) g_md_layout[vi] = false, g_pending[vi] = -1, enter(vi);
}

CORE_COLD void knobs_of(const float* p, int np, bool md_layout, float q[16]) {
  for (int k = 0; k < 16; ++k) q[k] = 0;
  if (md_layout) {
    for (int k = 0; k < 16 && k < np; ++k) q[k] = p[k];
    if (np < 11) q[MDT_P_LEVEL - 1] = MDT_LEVEL_DEFAULT;   // an 8-param report: FM BD, LEV at its default
    return;
  }
  const Legacy& g = kLegacy[eng_zone(np > 0 ? p[0] : 0.0f, 11)];
  int e = engine_of(g.set), t = g.set - mdt_base[e];
  for (int k = 0; k < 8; ++k) q[k] = k < g.keep && k + 1 < np ? p[k + 1] : (float)mdk_builtin_default(g.set, k);
  q[MDT_P_ENGINE - 1] = (float)lo(e, MDT_ENGINES);
  q[MDT_P_TYPE - 1] = (float)lo(t, mdt_count[e]);
  q[MDT_P_LEVEL - 1] = MDT_LEVEL_DEFAULT;
}
CORE_COLD int set_of(const float q[16]) {
  int e = zone(q[MDT_P_ENGINE - 1], MDT_ENGINES);
  return mdt_base[e] + zone(q[MDT_P_TYPE - 1], mdt_count[e]);
}
CORE_COLD const char* set_name(int set) {   // (a static buffer: the control core's only)
  static char b[12];
  if (set < 0 || set >= kSets) return "?";
  int e = engine_of(set);
  sniprintf(b, sizeof b, "%s %s", mdt_engine_names[e], mdt_type_names[set]);
  return b;
}

// the control core's cost (/status "md", review M4): a call's us; a TYPE change's set-up (params) and its trig's tail
// render + set-up (note_on) are the expensive ones, counted apart from the plain knob and trig calls
static Cost g_cost;
static void count(uint32_t t0, bool change) {
  uint32_t us = now_us() - t0;
  if (change) {
    ++g_cost.changes;
    if (us > g_cost.change_max_us) g_cost.change_max_us = us;
  } else if (us > g_cost.call_max_us) {
    g_cost.call_max_us = us;
  }
}
CORE_COLD Cost cost() { return g_cost; }
CORE_COLD void cost_reset() { g_cost = Cost{}; }
static void set_knobs(int vi, int before) {
  uint32_t t0 = now_us();
  mdk_builtin_entry.params(g_state[vi], g_q[vi]);
  bool change = set_of(g_q[vi]) != before;
  if (change) g_pending[vi] = (int8_t)set_of(g_q[vi]);
  count(t0, change);
}
CORE_COLD void report(int vi, const float* p, int np, int tag) {
  if (!g_ok || vi < 0 || vi >= kVoices) return;
  int before = set_of(g_q[vi]);
  g_md_layout[vi] = tag == kLayoutTag;
  knobs_of(p, np, g_md_layout[vi], g_q[vi]);
  set_knobs(vi, before);
}
CORE_COLD void params(int vi, const float* p) {
  if (!g_ok || vi < 0 || vi >= kVoices) return;
  int before = set_of(g_q[vi]);
  knobs_of(p, 16, g_md_layout[vi], g_q[vi]);
  set_knobs(vi, before);
}
CORE_COLD int voice_set(int vi) { return vi >= 0 && vi < kVoices ? set_of(g_q[vi]) : -1; }
CORE_COLD void trig(int vi, int vel) {
  if (!g_ok || vi < 0 || vi >= kVoices) return;
  uint32_t t0 = now_us();
  mdk_builtin_entry.note_on(g_state[vi], 60.0f, (vel < 1 ? 1 : vel > 127 ? 127 : vel) / 127.0f);
  count(t0, g_pending[vi] >= 0);   // a trig after a TYPE change: the old voice's tail rendered, the new one set up
  g_pending[vi] = -1;
}
void render(int vi, float* out, int n) {
  if (!g_ok) {
    for (int s = 0; s < n; ++s) out[s] = 0;
    return;
  }
  mdk_builtin_entry.render(g_state[vi], out, n);
  for (int s = 0; s < n; ++s) {   // as the plugin guard does for a user machine: a NaN is 0, the rest within +-1
    float y = out[s];
    out[s] = y == y ? (y > 1.0f ? 1.0f : y < -1.0f ? -1.0f : y) : 0.0f;
  }
}
CORE_COLD int json(char* out, int cap) {
  int n = sniprintf(out, cap, "\"md\":{\"ok\":%s,\"stateBytes\":%lu,\"changes\":%lu,\"changeMaxUs\":%lu,\"callMaxUs\":%lu}",
                    g_ok ? "true" : "false", (unsigned long)state_bytes(), (unsigned long)g_cost.changes,
                    (unsigned long)g_cost.change_max_us, (unsigned long)g_cost.call_max_us);
  return n > 0 && n < cap ? n : 0;
}

// ---- the Rytm's labels (docs/MD-BUILTIN.md section 3)
CORE_COLD bool on_ask(const uint8_t* d, int len) {
  if (len != 11 || d[4] != RMP_CMD_ASK || d[5] != RMP_PROTO_VER || d[6] < kAskSlot || d[6] >= kAskSlot + MDT_ENGINES ||
      d[10] != 0xF7)
    return false;
  int e = d[6] - kAskSlot, gen = d[7] & 0x7F, what = d[8], arg = d[9] & 0x7F;
  if (!g_ok) return true;   // (ours, but no MD on this build: no answer)
  uint8_t m[RMP_MSG_MAX];
  if (what == RMP_ASK_META) {
    int set = mdt_base[e] + zone((float)arg, mdt_count[e]), g = gen_of(set);
    if (gen == g) {
      to_rytm(m, rmp_build_meta_unchanged(m, d[6], g));
      return true;
    }
    static rmp_desc ds;
    memset(&ds, 0, sizeof ds);
    copy_text(ds.name, "MD", 10), copy_text(ds.short_name, "MD", 4), copy_text(ds.p2_title, "MD ENGINE", 13);
    ds.page2 = 1, ds.nknobs = 16, ds.pre = -1, ds.format = 4;
    for (int k = 0; k < 16; ++k) {
      rmp_desc_knob& q = ds.knob[k];
      q.p = (uint8_t)(k + 1);
      if (k < 8 && k < mdk_builtin_controls(set)) {
        q.look = RMP_LOOK_DIAL, q.kind = RMP_TEXT_NUMBER, q.max = 127, q.def = (uint8_t)mdk_builtin_default(set, k);
        copy_text(q.long_name, mdt_label[set][k][0], 12), copy_text(q.short_name, mdt_label[set][k][1], 4);
      } else if (k >= 8 && k <= 10) {   // P9 ENGINE, P10 TYPE, P11 LEV
        int j = k - 8;
        q.look = j == 2 ? RMP_LOOK_FADER : RMP_LOOK_LIST, q.kind = j == 2 ? RMP_TEXT_NUMBER : RMP_TEXT_ENUM, q.max = 127;
        q.def = j == 2 ? MDT_LEVEL_DEFAULT : 0;
        q.enum_count = j == 0 ? MDT_ENGINES : j == 1 ? mdt_count[e] : 0;
        copy_text(q.long_name, mdt_knob_long[j], 12), copy_text(q.short_name, mdt_knob_short[j], 4);
      }   // else unused: all 0, empty labels
    }
    // fp 0: a built-in has no plugin fingerprint (review M3; the Rytm must not compare or store one for MD)
    to_rytm(m, rmp_build_meta_head(m, d[6], g, RMP_PST_READY, RMP_FLAG_PARAMS, &ds, 0));
    for (int part = 1; part < rmp_meta_nparts(&ds); ++part) to_rytm(m, rmp_build_meta_part(m, d[6], g, part, &ds));
    return true;
  }
  if (what == RMP_ASK_ENUM && (arg == MDT_P_ENGINE - 1 || arg == MDT_P_TYPE - 1)) {
    char names[13][13];
    bool eng = arg == MDT_P_ENGINE - 1;
    int total = eng ? MDT_ENGINES : mdt_count[e];
    for (int i = 0; i < total; ++i) copy_text(names[i], eng ? mdt_engine_names[i] : mdt_type_names[mdt_base[e] + i], 12);
    int g = gen ? gen : gen_of(mdt_base[e]);   // the ASK's own generation echoed (the page's 1A gen: review M1)
    for (int first = 0; first < total; first += RMP_LIST_PER_MSG) {
      int n = total - first < RMP_LIST_PER_MSG ? total - first : RMP_LIST_PER_MSG;
      to_rytm(m, rmp_build_list(m, d[6], arg, g, first == 0, total, first, n, names[first], 13));
    }
    return true;
  }
  return true;   // another ask on MD's slots: nothing to say
}

}  // namespace md
}  // namespace core
#endif
