// core/fx/send_fx.cpp - see send_fx.h: the values (fx::Control), the TYPE ids and names, 0x20 SENDFX in, 0x21 / 0x22
// out, the console commands. Per message or command, never per sample: cold. No card access here (send_fx_card.cpp),
// no log (the caller logs the reply), no libc beyond string.h (the Pi's hosted side has none).
#include "fx/send_fx.h"

#include "platform.h"

namespace core {
namespace fx {

Control control;
volatile bool card_dirty = false;
UserFx user[kUserSlots];
volatile uint16_t kit_tag[kSlots];

// The stable TYPE ids 1..63 (sim/protocol.py SENDFX_TYPES): implemented 1, 2, 16, 17, 18 (and 5, 6, 8, 15 with
// RYTM_SENDFX_LARGE); the rest reserved for planned effects.
struct Builtin {
  uint8_t id;
  const char* name;
};
static const Builtin kBuiltins[] = {
    {1, "CHORUS"},   {2, "COMB"},     {3, "DELAY"},    {4, "PINGPONG"}, {5, "SPACE"}, {6, "RINGS"},
    {7, "S4 RESO"},  {8, "CLOUDSEED"}, {9, "PLASMA"},  {10, "PLATE"},   {11, "HALL"},    {12, "SHIMMER"},
    {13, "FLANGER"}, {14, "PHASER"},  {15, "STUTTER"},  {16, "CRUSH"},    {17, "RINGMOD"}, {18, "HALO"},
};
static const int kNumBuiltins = sizeof kBuiltins / sizeof kBuiltins[0];

CORE_COLD bool builtin(int id) { return plays(id); }

CORE_COLD const char* type_name(int id, int slot) {
  if (id == kOff) return "OFF";
  for (int i = 0; i < kNumBuiltins; ++i)
    if (kBuiltins[i].id == id) return kBuiltins[i].name;
  static const char* const kUfx[kUserSlots] = {"UFX1", "UFX2", "UFX3", "UFX4"};
  int u = id - kFirstUser;
  if (u >= 0 && u < kUserSlots) return user[u].tag ? user[u].name : kUfx[u];
  (void)slot;
  return "?";
}

static CORE_COLD bool same_ci(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
    if (x != y) return false;
  }
  return *a == *b;
}

// a decimal number; -1 if none
static CORE_COLD int num(const char* p) {
  if (*p < '0' || *p > '9') return -1;
  int v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
  return v;
}

CORE_COLD int type_from_name(const char* s) {
  if (same_ci(s, "off")) return kOff;
  for (int i = 0; i < kNumBuiltins; ++i)
    if (same_ci(s, kBuiltins[i].name)) return kBuiltins[i].id;
  int v = num(s);
  return v >= 0 && v <= 127 ? v : -1;
}

CORE_COLD State state(int slot) {
  int id = control.knob[slot][0];
  if (id == kOff) return kStEmpty;
  if (id == kStutter && stutter_slot(control) != slot) return kStMissing;   // FX 1 has it (or this build lacks it)
  if (builtin(id)) return kStReady;
  // a user id: the slot map must hold an effect whose tag matches the kit's (an untagged kit takes the slot's);
  // even then no user-effect plugin runs yet, so MISSING either way (the name says which effect the slot holds)
  return kStMissing;
}

bool any_on() {
  for (int s = 0; s < kSlots; ++s)
    if (builtin(control.knob[s][0])) return true;
  return false;
}

bool all_sound_off() {
  static uint32_t last = 0;
  static bool seen = false;
  uint32_t now = now_ms();
  bool fresh = !seen || now - last >= 100;
  seen = true, last = now;   // a burst keeps itself going
  if (fresh) control.kill = (uint8_t)(control.kill + 1);
  return fresh;
}

uint8_t (*knob_filter)(int slot, int knob, uint8_t v) = nullptr;
void (*clock_listener)(uint8_t status, uint16_t spp) = nullptr;
void (*bus_insert)(int slot, float (*y)[2], int n) = nullptr;
static inline uint8_t knob(int s, int k, uint8_t v) { return knob_filter ? knob_filter(s, k, v) : v; }

CORE_COLD void clock_in(uint8_t status, uint16_t spp) {   // per MIDI realtime byte, control side: cold
  static int32_t pos = -1;   // the latest tick's song position (24 ppqn), -1 before the first after Start
  static uint32_t ticks = 0;
  static bool run = false;
  switch (status) {
    case 0xF8: ++ticks; if (run) ++pos; break;
    case 0xFA: pos = -1, run = true; break;
    case 0xFB: run = true; break;
    case 0xFC: run = false; break;
    case 0xF2: pos = (int32_t)spp * 6 - 1; break;
    default: return;
  }
  control.clock = ClockWord::pack(pos, ticks, run);
  if (clock_listener) clock_listener(status, spp);   // the Sidecar's LFOs' sync (core/lfo.h)
}

CORE_COLD uint16_t fx_tag(const char* name) {
  uint32_t h = 2166136261u;
  for (; *name; ++name) h = (h ^ (uint8_t)*name) * 16777619u;
  uint16_t t = (uint16_t)((h ^ (h >> 14) ^ (h >> 28)) & 0x3FFF);
  return t ? t : 1;
}

// Track 12's words in message order: 0, 29, 38, 39, 40, 41 (sim/protocol.py SENDFX_FX_WORDS). FX1's knobs: words
// 29, 38, 39 (pairs 1, 2, 3); FX2's: words 0, 40, 41 (pairs 0, 4, 5); each pair's hi then lo half.
static const uint8_t kKnobPairs[kSlots][3] = {{1, 2, 3}, {0, 4, 5}};
constexpr int kTagTrack = 13;

CORE_COLD bool take_sendfx(const uint8_t* d, int len) {
  // F0 7D 52 54 20 01 t ... F7
  if (len < 8 || d[0] != 0xF0 || d[1] != 0x7D || d[2] != 0x52 || d[3] != 0x54 || d[4] != 0x20 || d[5] != 0x01)
    return false;
  int t = d[6], nb = len - 8;   // the bytes between t and F7
  if (d[len - 1] != 0xF7 || t > kTagTrack || nb != (t == kTracks ? 12 : t == kTagTrack ? 4 : 2)) return false;
  const uint8_t* b = d + 7;
  for (int i = 0; i < nb; ++i)
    if (b[i] > 0x7F) return false;
  if (t < kTracks) {
    control.send[t][0] = b[0];
    control.send[t][1] = b[1];
  } else if (t == kTracks) {
    for (int s = 0; s < kSlots; ++s)
      for (int j = 0; j < 3; ++j) {
        control.knob[s][2 * j] = knob(s, 2 * j, b[2 * kKnobPairs[s][j]]);   // an LFO's on top (core/lfo.h)
        control.knob[s][2 * j + 1] = knob(s, 2 * j + 1, b[2 * kKnobPairs[s][j] + 1]);
      }
  } else {
    kit_tag[0] = (uint16_t)(b[0] << 7 | b[1]);
    kit_tag[1] = (uint16_t)(b[2] << 7 | b[3]);
  }
  control.messages = control.messages + 1;
  return true;
}

static CORE_COLD int head(uint8_t* m, uint8_t cmd) {
  m[0] = 0xF0, m[1] = 0x7D, m[2] = 0x52, m[3] = 0x54, m[4] = cmd, m[5] = 1;
  return 6;
}

static CORE_COLD int name_n(uint8_t* out, const char* nm, int w) {
  for (int i = 0; i < w; ++i) {
    char c = *nm ? *nm++ : 0;
    out[i] = !c ? 0 : (c >= 0x20 && c <= 0x7E) ? (uint8_t)c : '_';
  }
  return w;
}

// The TYPE knob's list: OFF, the built-in effects this build plays, the user-FX slots that hold an effect.
// In id order, so stepping TYPE from an id the list lacks lands on the next id up or down.
static const uint8_t kPlayable[] = {kOff, kChorus, kComb,
#if RYTM_SENDFX_LARGE
                                    kTape, kRings, kCloudSeed, kStutter,
#endif
                                    kCrush, kRingMod, kHalo};
static const int kNumPlayable = sizeof kPlayable / sizeof kPlayable[0];
static CORE_COLD int list_id(int k) {
  if (k < kNumPlayable) return kPlayable[k];
  for (int u = 0, n = kNumPlayable; u < kUserSlots; ++u)
    if (user[u].tag && n++ == k) return kFirstUser + u;
  return -1;
}

CORE_COLD int names_total() {
  int n = kNumPlayable;
  for (int u = 0; u < kUserSlots; ++u) n += user[u].tag != 0;
  return n;
}

CORE_COLD int sysex_names(uint8_t* m, bool replace, int total, int first, int count) {
  int n = head(m, 0x21);
  m[n++] = replace ? 1 : 0;
  m[n++] = (uint8_t)total;
  for (int k = first; k < first + count; ++k) {
    int id = list_id(k);
    m[n++] = (uint8_t)(id < 0 ? 0 : id);
    n += name_n(m + n, id < 0 ? "" : type_name(id), 13);
  }
  m[n++] = 0xF7;
  return n;
}

// 0x24 SENDFX LABELS (sim/protocol.py sendfx_labels): F0 7D 52 54 24 01 <id> <4 x 4 label bytes> then per zone
// <knob | show << 2 | step << 3> <lo> <base> <5 name bytes>, F7. Only for an id this build plays.
CORE_COLD const KnobLabels* labels(int id) {
  if (!plays(id)) return nullptr;
  switch (id) {
    case kChorus: return &kChorusLabels;
    case kComb: return &kCombLabels;
    case kCrush: return &kCrushLabels;
    case kRingMod: return &kRingModLabels;
    case kHalo: return &kHaloLabels;
#if RYTM_SENDFX_LARGE
    case kTape: return &kTapeLabels;
    case kRings: return &kRingsLabels;
    case kCloudSeed: return &kCloudSeedLabels;
    case kStutter: return &kStutterLabels;
#endif
    default: return nullptr;
  }
}

CORE_COLD int sysex_labels(uint8_t* m, int id) {
  const KnobLabels* l = labels(id);
  if (!l) return 0;
  int n = head(m, 0x24);
  m[n++] = (uint8_t)id;
  for (int j = 0; j < 4; ++j) n += name_n(m + n, l->label[j] ? l->label[j] : "", kLabelChars);
  for (int z = 0; z < l->nzones && z < kMaxZones; ++z) {
    const Zone& q = l->zones[z];
    m[n++] = (uint8_t)((q.knob & 3) | (q.show ? 4 : 0) | (q.step ? 8 : 0));
    m[n++] = (uint8_t)(q.lo & 0x7F);
    m[n++] = (uint8_t)(q.base & 0x7F);
    n += name_n(m + n, q.name, kZoneChars);
  }
  m[n++] = 0xF7;
  return n;
}

CORE_COLD int sysex_state(uint8_t* m, int slot) {
  int n = head(m, 0x22), id = control.knob[slot][0];
  m[n++] = (uint8_t)slot;
  m[n++] = (uint8_t)id;
  m[n++] = state(slot);
  n += name_n(m + n, type_name(id, slot), 10);
  m[n++] = 0xF7;
  return n;
}

static CORE_COLD int put(char* out, int max, int at, const char* s) {
  while (*s && at < max - 1) out[at++] = *s++;
  out[at < max ? at : max - 1] = 0;
  return at;
}

static CORE_COLD int put_u(char* out, int max, int at, unsigned v) {
  char b[12];
  int n = 0;
  do b[n++] = (char)('0' + v % 10), v /= 10; while (v && n < 11);
  while (n && at < max - 1) out[at++] = b[--n];
  out[at < max ? at : max - 1] = 0;
  return at;
}

// "1:..." -> FX slot 0 and the text after the ':' (null if malformed)
static CORE_COLD const char* slot_arg(const char* p, int* slot) {
  if ((p[0] != '1' && p[0] != '2') || p[1] != ':') return nullptr;
  *slot = p[0] - '1';
  return p + 2;
}

static CORE_COLD void show(char* r, int max) {
  static const char* const kSt[4] = {"", "", "", " MISSING"};
  int at = put(r, max, 0, "sendfx:");
  for (int s = 0; s < kSlots; ++s) {
    int id = control.knob[s][0];
    at = put(r, max, at, s ? "; fx2 " : " fx1 ");
    at = put(r, max, at, type_name(id, s));
    at = put(r, max, at, " (");
    at = put_u(r, max, at, (unsigned)id);
    at = put(r, max, at, ")");
    at = put(r, max, at, kSt[state(s) & 3]);
    at = put(r, max, at, " p");
    for (int j = 1; j < kKnobs; ++j) at = put(r, max, at, " "), at = put_u(r, max, at, control.knob[s][j]);
    at = put(r, max, at, " send");
    for (int t = 0; t < kTracks; ++t) at = put(r, max, at, " "), at = put_u(r, max, at, control.send[t][s]);
  }
  at = put(r, max, at, control.route == kRouteMain ? "; out forced main (0/1)" : control.route == kRouteFx ? "; out forced fx (2/3)" : "; out per slot (P1: 0..63 fx 2/3, 64..127 main 0/1)");
  if (control.stutter_lag) at = put(r, max, at, "; stutter lag "), at = put_u(r, max, at, control.stutter_lag);
  at = put(r, max, at, "; 0x20 msgs ");
  put_u(r, max, at, control.messages);
}

CORE_COLD bool command(const char* p, char* r, int max) {
  int s;
  const char* a;
  if (strcmp(p, "sendfx") == 0) {
    show(r, max);
    return true;
  }
  if (strcmp(p, "sendfx=off") == 0) {
    control.knob[0][0] = control.knob[1][0] = kOff;
    show(r, max);
    return true;
  }
  if (strncmp(p, "sendfx=", 7) == 0) {   // K:TYPE (a name or an id 0..127)
    int t = (a = slot_arg(p + 7, &s)) ? type_from_name(a) : -1;
    if (t < 0) {
      put(r, max, 0, "sendfx=K:TYPE (K 1|2; off chorus comb halo ..., or an id 0..127), sendfx=off");
      return true;
    }
    control.knob[s][0] = (uint8_t)t;
    show(r, max);
    return true;
  }
  if (strncmp(p, "sendfxout=", 10) == 0) {
    if (strcmp(p + 10, "fx") == 0) control.route = kRouteFx;
    else if (strcmp(p + 10, "main") == 0) control.route = kRouteMain;
    else if (strcmp(p + 10, "slot") == 0) control.route = kRouteSlot;
    else {
      put(r, max, 0, "sendfxout=slot|fx|main (slot: each FX's P1; fx, main: force all)");
      return true;
    }
    card_dirty = true;
    show(r, max);
    return true;
  }
  if (strncmp(p, "sendlvl=", 8) == 0) {   // K:T:V or K:all:V
    const char* c = (a = slot_arg(p + 8, &s)) ? strchr(a, ':') : nullptr;
    bool all = c && c == a + 3 && strncmp(a, "all", 3) == 0;
    int t = all ? -1 : c ? num(a) : 99, v = c ? num(c + 1) : -1;
    if (!c || (!all && (t < 0 || t >= kTracks)) || v < 0 || v > 127) {
      put(r, max, 0, "sendlvl=K:T:V (K 1|2, T 0..11 or all, V 0..127)");
      return true;
    }
    for (int i = 0; i < kTracks; ++i)
      if (all || i == t) control.send[i][s] = (uint8_t)v;
    show(r, max);
    return true;
  }
  if (strncmp(p, "stutterlag=", 11) == 0) {   // frames the stems trail the MIDI clock (STUTTER's grid)
    int v = num(p + 11);
    if (v < 0 || v > 4800) {
      put(r, max, 0, "stutterlag=FRAMES (0..4800, 48 kHz)");
      return true;
    }
    control.stutter_lag = (uint16_t)v;
    show(r, max);
    return true;
  }
  if (strncmp(p, "sendknob=", 9) == 0) {   // K:I:V, I 0 TYPE, 1 P1 (ROUTE), 2..5 P2..P5
    const char* c = (a = slot_arg(p + 9, &s)) ? strchr(a, ':') : nullptr;
    int i = c ? num(a) : -1, v = c ? num(c + 1) : -1;
    if (!c || i < 0 || i >= kKnobs || v < 0 || v > 127) {
      put(r, max, 0, "sendknob=K:I:V (K 1|2, I 0 TYPE or 1..5, V 0..127)");
      return true;
    }
    control.knob[s][i] = (uint8_t)v;
    show(r, max);
    return true;
  }
  return false;
}

}  // namespace fx
}  // namespace core
