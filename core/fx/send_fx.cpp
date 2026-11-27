// core/fx/send_fx.cpp - see send_fx.h: the values (fx::Control), the TYPE ids and names, 0x20 SENDFX in, 0x21 / 0x22
// out, the console commands. Per message or command, never per sample: cold. No card access here (send_fx_card.cpp),
// no log (the caller logs the reply), no libc beyond string.h (the Pi's hosted side has none).
#include "fx/send_fx.h"

#include "mod/target.h"
#include "platform.h"

namespace core {
namespace fx {

Control control;
Control control1;   // slot 1's (send_fx.h control_of)
volatile uint8_t card_dirty = 0;
int (*slot_fn)() = nullptr;
CORE_COLD int slot_in_hand() {
  int sl = slot_fn ? slot_fn() : 0;
  return sl > 0 && sl < kVoiceSlots ? sl : 0;
}
CORE_COLD Control& cur() { return control_of(slot_in_hand()); }
UserFx user[kUserSlots];
volatile uint16_t kit_tag[kSlots];
UserFx user1[kUserSlots];            // box 1's (send_fx.h user_of)
volatile uint16_t kit_tag1[kSlots];

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
  (void)slot;
  return type_name_in(0, id);
}

CORE_COLD const char* type_name_in(int box, int id) {
  if (id == kOff) return "OFF";
  for (int i = 0; i < kNumBuiltins; ++i)
    if (kBuiltins[i].id == id) return kBuiltins[i].name;
  static const char* const kUfx[kUserSlots] = {"UFX1", "UFX2", "UFX3", "UFX4"};
  int u = id - kFirstUser;
  const UserFx* us = user_of(box);
  if (u >= 0 && u < kUserSlots) return us[u].tag ? us[u].name : kUfx[u];
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

CORE_COLD State state(int slot) { return state(control, slot); }

CORE_COLD State state(const Control& c, int slot) {
  int id = c.knob[slot][0];
  if (id == kOff) return kStEmpty;
  if (id == kStutter && stutter_slot(c) != slot) return kStMissing;   // FX 1 has it (or this build lacks it)
  if (builtin(id)) return kStReady;
  // a user id: the box's slot map must hold an effect whose tag matches the kit's (an untagged kit takes the slot's), and
  // (Plugin API 3) its plugin must be loaded: READY then, else MISSING (the name says which effect the slot holds)
  int u = id - kFirstUser;
  if (u >= 0 && u < kUserSlots && box_plays_at(box_of(c), id, slot)) return kStReady;
  return kStMissing;
}

bool any_on() { return any_on(control); }

bool any_on(const Control& c) {
  const int b = box_of(c);
  for (int s = 0; s < kSlots; ++s)
    if (builtin(c.knob[s][0]) || box_plays(b, c.knob[s][0])) return true;
  return false;
}

bool all_sound_off() { return all_sound_off(control); }

bool all_sound_off(Control& c) {
  static uint32_t last[2] = {0, 0};
  static bool seen[2] = {false, false};
  const int b = box_of(c);
  uint32_t now = now_ms();
  bool fresh = !seen[b] || now - last[b] >= 100;
  seen[b] = true, last[b] = now;   // a burst keeps itself going
  if (fresh) c.kill = (uint8_t)(c.kill + 1);
  return fresh;
}

uint8_t (*knob_filter)(int slot, int knob, uint8_t v) = nullptr;
uint8_t (*send_filter)(int track, int slot, uint8_t v) = nullptr;
void (*clock_listener)(int box, uint8_t status, uint16_t spp) = nullptr;
void (*bus_insert)(const Control& c, int slot, float (*y)[2], int n) = nullptr;
bool (*user_ready)(int u) = nullptr;
volatile uint8_t list_dirty = 0;
bool (*user_loaded)(int u) = nullptr;
const KnobLabels* (*user_labels)(int u) = nullptr;
void (*user_fresh_req)(int u, int slot) = nullptr;
bool (*user_fresh_done)(int u, int slot) = nullptr;
void (*user_run)(int u, int slot, const float* x, int n, float (*y)[2], const uint8_t* k) = nullptr;
static inline uint8_t knob(int s, int k, uint8_t v) { return knob_filter ? knob_filter(s, k, v) : v; }
static inline uint8_t send(int t, int s, uint8_t v) { return send_filter ? send_filter(t, s, v) : v; }
CORE_COLD void set_knob_base(int s, int k, uint8_t v) {
  if (s >= 0 && s < kSlots && k >= 0 && k < kKnobs) cur().knob[s][k] = knob(s, k, v);
}
CORE_COLD void set_send_base(int t, int s, uint8_t v) {
  if (t >= 0 && t < kTracks && s >= 0 && s < kSlots) cur().send[t][s] = send(t, s, v);
}

CORE_COLD void clock_in(uint8_t status, uint16_t spp) { clock_in(control, status, spp); }

CORE_COLD void clock_in(Control& c, uint8_t status, uint16_t spp) {   // per MIDI realtime byte, control side: cold
  static int32_t pos[2] = {-1, -1};   // the latest tick's song position (24 ppqn), -1 before the first after Start
  static uint32_t ticks[2] = {0, 0};
  static bool run[2] = {false, false};
  const int b = box_of(c);
  switch (status) {
    case 0xF8: ++ticks[b]; if (run[b]) ++pos[b]; break;
    case 0xFA: pos[b] = -1, run[b] = true; break;
    case 0xFB: run[b] = true; break;
    case 0xFC: run[b] = false; break;
    case 0xF2: pos[b] = (int32_t)spp * 6 - 1; break;
    default: return;
  }
  c.clock = ClockWord::pack(pos[b], ticks[b], run[b]);
  if (clock_listener) clock_listener(b, status, spp);   // the LFOs' sync (core/lfo.h): each box's clock its own slot's LFOs' (3(c) step 10)
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
  // The box in hand's (its slot's Scope: engine_sysex_link), never slot 0's whichever slot the Rytm holds (a Rytm in slot 1
  // beside an A4 in slot 0 overwrote the A4's FX and tags before)
  const int box = slot_in_hand();
  Control& c = control_of(box);
  if (t < kTracks) {
    c.send[t][0] = send(t, 0, b[0]);   // a modulation on top (core/mod kSnd)
    c.send[t][1] = send(t, 1, b[1]);
  } else if (t == kTracks) {
    for (int s = 0; s < kSlots; ++s)
      for (int j = 0; j < 3; ++j) {
        c.knob[s][2 * j] = knob(s, 2 * j, b[2 * kKnobPairs[s][j]]);   // an LFO's on top (core/lfo.h)
        c.knob[s][2 * j + 1] = knob(s, 2 * j + 1, b[2 * kKnobPairs[s][j] + 1]);
      }
  } else {
    volatile uint16_t* kt = kit_tag_of(box);
    kt[0] = (uint16_t)(b[0] << 7 | b[1]);
    kt[1] = (uint16_t)(b[2] << 7 | b[3]);
  }
  c.messages = c.messages + 1;
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
// The 0x21 list is the box in hand's: its own user effects (the Rytm it goes to, whichever slot it holds)
static CORE_COLD int list_id(int k) {
  if (k < kNumPlayable) return kPlayable[k];
  const UserFx* us = user_of(slot_in_hand());
  for (int u = 0, n = kNumPlayable; u < kUserSlots; ++u)
    if (us[u].tag && n++ == k) return kFirstUser + u;
  return -1;
}

CORE_COLD int names_total() {
  const UserFx* us = user_of(slot_in_hand());
  int n = kNumPlayable;
  for (int u = 0; u < kUserSlots; ++u) n += us[u].tag != 0;
  return n;
}

CORE_COLD int sysex_names(uint8_t* m, bool replace, int total, int first, int count) {
  int n = head(m, 0x21);
  m[n++] = replace ? 1 : 0;
  m[n++] = (uint8_t)total;
  for (int k = first; k < first + count; ++k) {
    int id = list_id(k);
    m[n++] = (uint8_t)(id < 0 ? 0 : id);
    n += name_n(m + n, id < 0 ? "" : type_name_in(slot_in_hand(), id), 13);
  }
  m[n++] = 0xF7;
  return n;
}

// 0x24 SENDFX LABELS (sim/protocol.py sendfx_labels): F0 7D 52 54 24 01 <id> <4 x 4 label bytes> then per zone
// <knob | show << 2 | step << 3> <lo> <base> <5 name bytes>, F7. Only for an id this build plays.
CORE_COLD const KnobLabels* labels(int id) {
  if (id >= kFirstUser && id < kFirstUser + kUserSlots)   // (the box in hand's own UFX: um's index box * kUserSlots + u)
    return user_labels ? user_labels(slot_in_hand() * kUserSlots + id - kFirstUser) : nullptr;
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

CORE_COLD int sysex_state(uint8_t* m, int slot) {   // the box in hand's FX `slot` (its Rytm's page)
  const int box = slot_in_hand();
  const Control& c = control_of(box);
  int n = head(m, 0x22), id = c.knob[slot][0];
  m[n++] = (uint8_t)slot;
  m[n++] = (uint8_t)id;
  m[n++] = state(c, slot);
  n += name_n(m + n, type_name_in(box, id), 10);
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
  Control& C = cur();
  static const char* const kSt[4] = {"", "", "", " MISSING"};
  int at = put(r, max, 0, "sendfx:");
  for (int s = 0; s < kSlots; ++s) {
    int id = C.knob[s][0];
    at = put(r, max, at, s ? "; fx2 " : " fx1 ");
    at = put(r, max, at, type_name_in(slot_in_hand(), id));
    at = put(r, max, at, " (");
    at = put_u(r, max, at, (unsigned)id);
    at = put(r, max, at, ")");
    at = put(r, max, at, kSt[state(C, s) & 3]);
    at = put(r, max, at, " p");
    for (int j = 1; j < kKnobs; ++j) at = put(r, max, at, " "), at = put_u(r, max, at, C.knob[s][j]);
    at = put(r, max, at, " send");
    for (int t = 0; t < kTracks; ++t) at = put(r, max, at, " "), at = put_u(r, max, at, C.send[t][s]);
  }
  at = put(r, max, at, C.route == kRouteMain ? "; out forced main (0/1)" : C.route == kRouteFx ? "; out forced fx (2/3)" : "; out per slot (P1: 0..63 fx 2/3, 64..127 main 0/1)");
  if (C.stutter_lag) at = put(r, max, at, "; stutter lag "), at = put_u(r, max, at, C.stutter_lag);
  at = put(r, max, at, "; 0x20 msgs ");
  put_u(r, max, at, C.messages);
}

CORE_COLD bool command(const char* p, char* r, int max) {
  Control& C = cur();   // the slot in hand's (3(c) step 8)
  int s;
  const char* a;
  if (strcmp(p, "sendfx") == 0) {
    show(r, max);
    return true;
  }
  if (strcmp(p, "sendfx=off") == 0) {
    C.knob[0][0] = C.knob[1][0] = kOff;
    if (settings_edit) settings_edit(0), settings_edit(1);
    show(r, max);
    return true;
  }
  if (strncmp(p, "sendfx=", 7) == 0) {   // K:TYPE (a name or an id 0..127)
    int t = (a = slot_arg(p + 7, &s)) ? type_from_name(a) : -1;
    if (t < 0) {
      put(r, max, 0, "sendfx=K:TYPE (K 1|2; off chorus comb halo ..., or an id 0..127), sendfx=off");
      return true;
    }
    C.knob[s][0] = (uint8_t)t;
    if (settings_edit) settings_edit(s);
    show(r, max);
    return true;
  }
  if (strncmp(p, "sendfxout=", 10) == 0) {
    if (strcmp(p + 10, "fx") == 0) C.route = kRouteFx;
    else if (strcmp(p + 10, "main") == 0) C.route = kRouteMain;
    else if (strcmp(p + 10, "slot") == 0) C.route = kRouteSlot;
    else {
      put(r, max, 0, "sendfxout=slot|fx|main (slot: each FX's P1; fx, main: force all)");
      return true;
    }
    card_dirty = (uint8_t)(card_dirty | 1u << slot_in_hand());
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
      if (all || i == t) C.send[i][s] = send(i, s, (uint8_t)v), edited(mod::tid(mod::kSnd, i, s), (float)v);
    show(r, max);
    return true;
  }
  if (strncmp(p, "stutterlag=", 11) == 0) {   // frames the stems trail the MIDI clock (STUTTER's grid)
    int v = num(p + 11);
    if (v < 0 || v > 4800) {
      put(r, max, 0, "stutterlag=FRAMES (0..4800, 48 kHz)");
      return true;
    }
    C.stutter_lag = (uint16_t)v;
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
    set_knob_base(s, i, (uint8_t)v);   // 0.5.0: through the knob filter (a live entry's base), a manual edit (spec-050 2.5)
    if (i >= 2) edited(mod::tid(mod::kFx, s, i - 2), (float)v);
    if (settings_edit) settings_edit(s);
    show(r, max);
    return true;
  }
  return false;
}

}  // namespace fx
}  // namespace core
