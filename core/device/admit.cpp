// core/device/admit.cpp - see admit.h. Per attach, never per sample: cold.
#include "device/admit.h"

#include <string.h>
#include <strings.h>

#include "platform.h"

namespace core {
namespace admit {

static Costs s_costs = {};
static unsigned s_clock = 0;   // 0: the bench's own
static unsigned s_split = 4;
// voices= per box type: the Rytm, the A4, the Digitakt (-1: its profile's count)
static const char* const kTypes[] = {"rytm", "a4", "digitakt"};
static constexpr int kNTypes = 3;
static int8_t s_over[kNTypes] = {-1, -1, -1};

void set_costs(const Costs& c) { s_costs = c; }
const Costs& costs() { return s_costs; }
void set_clock(unsigned mhz) { s_clock = mhz; }
unsigned clock() { return s_clock ? s_clock : s_costs.mhz; }
void set_split(unsigned n) { s_split = n < 1 ? 1 : n > 7 ? 7 : n; }
unsigned split() { return s_split; }
bool on() { return s_costs.mhz != 0; }
static bool s_enforce = true;
void set_enforce(bool on) { s_enforce = on; }
bool enforce() { return s_enforce; }

static unsigned scaled(unsigned long long bp) {   // the bench's bp at the clock now
  const unsigned now = clock();
  return now ? (unsigned)((bp * s_costs.mhz + now - 1) / now) : (unsigned)bp;
}

static LoadPct s_pct = {kVoiceLoadPct, kAmpLoadPct, kChainLoadPct};
void set_load_pct(const LoadPct& p) { s_pct = p; }
const LoadPct& load_pct() { return s_pct; }

unsigned split_for(unsigned asked) {
  if (!asked) return s_split;
  const unsigned half = (asked + 1) / 2;
  return half < s_split ? half : s_split;
}

// box voices 0..n-1 at split sp: how many of them render on core c
static unsigned on_core(int c, unsigned n, unsigned sp) {
  const unsigned one = n < sp ? n : sp;
  return c == 1 ? one : n - one;
}

int base_of(const Load& l) { return l.held[0] ? 0 : 1; }

CORE_COLD unsigned core_bp(int c, const Load& l) {
  if (c != 1 && c != 2) return 0;
  unsigned v = 0;
  unsigned long long chains = 0;
  for (int s = 0; s < 2; ++s) {
    v += on_core(c, l.voices[s] > 8 ? 8 : l.voices[s], split_for(l.asked[s]));
    if (l.held[s] && chain_core(s) == c) chains += s_costs.fx_bp;   // its chains (dry) on the core that runs its send FX
  }
  const unsigned long long bp = (chains * s_pct.chain +
                                 ((unsigned long long)v * (s_costs.voice_bp + s_costs.ins_bp) + l.ins[c]) * s_pct.voice +
                                 (unsigned long long)l.amps[c] * s_costs.amp_bp * s_pct.amp + 99) / 100;
  return scaled(bp);
}

CORE_COLD unsigned core3_bp(const Load& l) {
  if (master_core(0) != 3) return 0;
  const unsigned long long bp = kC3BaseBp + (l.held[0] ? kC3MasterBp : 0) +
                                ((unsigned long long)l.ins[3] * s_pct.chain + 99) / 100 +
                                ((unsigned long long)l.amps[3] * s_costs.amp_bp * s_pct.amp + 99) / 100;
  return scaled(bp);
}
CORE_COLD bool ins3_fits(const Load& l, int t) {
  if (!on() || !s_enforce || master_core(0) != 3 || !master_slot_bp(t)) return true;
  return core3_bp(l) + scaled(((unsigned long long)master_slot_bp(t) * s_pct.chain + 99) / 100) <= kC3LimitBp;
}
unsigned limit_bp(int core) { return core == 3 ? kC3LimitBp : kLimitBp; }

unsigned master_slot_bp(int t) {
  if (t >= kSendType0 && t < kSendType0 + kSendTypes) {
    const unsigned s = s_costs.send_bp[t - kSendType0];
    return s ? s : 2 * s_costs.slot_bp;
  }
  return 2 * type_bp(t);
}
unsigned type_bp(int t) {
  if (t <= 0 || t >= kSendType0 || t == 9) return 0;   // (a send TYPE: master_slot_bp)   // OFF; AMP (fx/insert.h kAmp): amp_bp, admitted by amp.h
  return s_costs.type_bp[t] ? s_costs.type_bp[t] : s_costs.slot_bp;
}

int voice_core(int b, int c, const Load& l) { return (unsigned)c < split_for(l.asked[b & 1]) ? 1 : 2; }

CORE_COLD bool ins_fits(int c, const Load& l, int t, int mul) {
  if (!on() || !s_enforce || (c != 1 && c != 2)) return true;
  return core_bp(c, l) + scaled(((unsigned long long)mul * type_bp(t) * s_pct.voice + 99) / 100) <= kLimitBp;
}

int amp_core(int row, int master_row, const Load& l) {
  const int b = base_of(l);
  if (row >= master_row) return master_core(b);
  if (row < 0 || row >= 8) return 0;   // a bus: AMP plays dry there
  return (unsigned)row < split_for(l.asked[b]) ? 1 : 2;
}

int amp_core_of(int b, int c, int master_row, const Load& l) {
  if (b < 0 || b > 1) return 0;
  if (c >= master_row) return master_core(b);
  if (c < 0 || c >= 8) return 0;   // a bus: AMP plays dry there
  return voice_core(b, c, l);
}

CORE_COLD bool amp_fits(int c, const Load& l) {
  if (!on() || !s_enforce) return true;
  const unsigned one = scaled(((unsigned long long)s_costs.amp_bp * s_pct.amp + 99) / 100);
  if (c == 3) return master_core(0) != 3 || core3_bp(l) + one <= kC3LimitBp;
  if (c != 1 && c != 2) return true;
  return core_bp(c, l) + one <= kLimitBp;
}

CORE_COLD int fit(int s, int want, const Load& l) {
  if (want < 0) want = 0;
  if (want > 8) want = 8;
  if (!on() || s < 0 || s > 1) return want;
  Load t = l;
  t.held[s] = true;
  t.voices[s] = 0;
  t.asked[s] = (uint8_t)want;
  const unsigned sp = split_for((unsigned)want);
  const unsigned base[3] = {0, core_bp(1, t), core_bp(2, t)};   // without its voices (its chains counted)
  for (int n = want; n > 0; --n) {
    t.voices[s] = (uint8_t)n;
    bool ok = true;
    for (int c = 1; c <= 2 && ok; ++c)
      if (on_core(c, (unsigned)n, sp) && core_bp(c, t) > kLimitBp && core_bp(c, t) > base[c]) ok = false;
    if (ok) return n;
  }
  return 0;
}

CORE_COLD int admit(int s, int want, const Load& l) {
  if (s_enforce) return fit(s, want, l);
  return want < 0 ? 0 : want > 8 ? 8 : want;
}

static int type_index(const char* name, size_t len) {
  if (len == 2 && !strncasecmp(name, "dt", 2)) return 2;
  for (int k = 0; k < kNTypes; ++k)
    if (strlen(kTypes[k]) == len && !strncasecmp(name, kTypes[k], len)) return k;
  return -1;
}

CORE_COLD bool parse_override(const char* text) {
  if (!text) return false;
  int8_t next[kNTypes];
  memcpy(next, s_over, sizeof next);
  const char* p = text;
  while (*p) {
    const char* colon = strchr(p, ':');
    if (!colon) return false;
    const int k = type_index(p, (size_t)(colon - p));
    if (k < 0) return false;
    const char* end = colon + 1;   // (no strtol on the Pi)
    long n = 0;
    while (*end >= '0' && *end <= '9' && n <= 8) n = n * 10 + (*end++ - '0');
    if (end == colon + 1 || n > 8 || (*end && *end != ',')) return false;
    next[k] = (int8_t)n;
    p = *end ? end + 1 : end;
  }
  memcpy(s_over, next, sizeof s_over);
  return true;
}

int override_for(const char* type_short) {
  if (!type_short) return -1;
  const int k = type_index(type_short, strlen(type_short));
  return k < 0 ? -1 : s_over[k];
}

void reset() {
  s_costs = {};
  s_clock = 0;
  s_split = 4;
  s_enforce = true;
  s_pct = {kVoiceLoadPct, kAmpLoadPct, kChainLoadPct};
  for (int8_t& o : s_over) o = -1;
}

}  // namespace admit
}  // namespace core
