// core/device/device.cpp - see device.h. Per message, never per sample: cold.
#include "device/device.h"
#include "device/a4.h"
#include "device/admit.h"
#include "device/digitakt.h"
#include "device/strip.h"

#include <string.h>

#include "fmt/fmt.h"
#include "fx/amp.h"
#include "fx/insert.h"
#include "fx/fx_guard.h"
#include "fx/out.h"
#include "host_link.h"
#if CORE_POLY
#include "inst.h"
#endif
#include "keys.h"
#if CORE_MSAMP
#include "msamp/cardq.h"
#endif
#include "device/sidecar_dir.h"
#include "platform.h"
#include "ui/remote.h"

namespace core {
namespace device {

// The Rytm's table: what the rest of the engine and rytm-mods 0021 / 0026 assume. Its reports are the engine's own, so
// map / fixed / out_allow are never read for it.
static constexpr Profile kRytm = {
    kIdRytm, "Analog Rytm MKII", 0x1935, 0x0b48,
    12, 8, 16,
    {0x7D, 0x52, 0x54},
    0x04, 0x03, 0x11,
    kRemoteUi | kProject | kKits | kPatterns | kSendFx | kUserSlots | kSrcView | kFxIn | kPools,
    0, 0, {}, {}, {}, nullptr, nullptr, {},
    kBracketFull, &out::kRytmMk2, nullptr, false, false, 0, false, nullptr, "Rytm"};
static_assert(kRytm.voices == kBoxVoices, "the Rytm's voices are one box's");
static_assert(kRytm.params == kMaxParams, "the Rytm's reports carry P1..P16");

static void select(Link& l, const Profile* h, uint32_t uid = 0);
static void resolve(Link& l, const Profile* h, uint32_t uid);
static void finish_moves();
static void usb_take();
static void tell_tick();
static void admit_alone();
static constexpr int kMax = 8;
static const Profile* s_list[kMax] = {&kRytm, &kDigitakt, &kA4};
static int s_n = 3;
Link g_links[kLinks] = {{}, {}};   // (slot 0 presumed the Rytm's: s_presume below)
Link* g_cur = &g_links[0];
Link* g_base = &g_links[0];
int g_depth = 0;
uint32_t g_hold_ms = 0;
volatile int8_t g_web_link = 0;
static volatile int8_t s_card_link = -1;   // the card core's request in hand: its link (CardScope)
static int s_raw = 0;                       // RawPaths open on the control core
static volatile bool s_card_raw = false;    // one open on another core (the card core's move into a UID folder)
static struct LinkIndex {
  LinkIndex() {
    for (int k = 0; k < kLinks; ++k) g_links[k].index = (uint8_t)k;
    g_links[0].held = g_links[0].was = &kRytm, g_links[0].presumed = true;   // boot: slot 0 the Rytm's until a box says otherwise
    g_links[0].want = g_links[0].voices = kRytm.voices, g_links[0].order = 1;   // (readmit: voices= once the cmdline is read)
  }
} s_link_index;

static inline bool on_control() { return !hooks.control_core || hooks.control_core(); }

int path_link() {
  if (on_control()) return s_raw ? -1 : g_cur->index;
  if (__atomic_load_n(&s_card_raw, __ATOMIC_RELAXED)) return -1;
  int c = __atomic_load_n(&s_card_link, __ATOMIC_RELAXED);
  return c >= 0 && c < kLinks ? c : g_web_link >= 0 && g_web_link < kLinks ? g_web_link : 0;
}

CardScope::CardScope(int link) : was(s_card_link), was_cur(g_cur) {
  if (link < 0 || link >= kLinks) link = 0;
  if (on_control()) ++g_depth, g_cur = &g_links[link];
  else __atomic_store_n(&s_card_link, (int8_t)link, __ATOMIC_RELAXED);
}
CardScope::~CardScope() {
  if (on_control()) g_cur = --g_depth ? was_cur : g_base;
  else __atomic_store_n(&s_card_link, was, __ATOMIC_RELAXED);
}

RawPath::RawPath() {
  if (on_control()) ++s_raw;
  else __atomic_store_n(&s_card_raw, true, __ATOMIC_RELAXED);
}
RawPath::~RawPath() {
  if (on_control()) --s_raw;
  else __atomic_store_n(&s_card_raw, false, __ATOMIC_RELAXED);
}

const Profile& rytm() { return kRytm; }
const Profile& active() { return cur().box ? *cur().box : kRytm; }
uint32_t dropped() { return cur().dropped; }
bool rytm_at(int s) { return kVoiceSlots < 2 || (s >= 0 && s < kLinks && g_links[s].held == &kRytm); }
bool rytm_state_at(int s) {
  if (kVoiceSlots < 2) return true;
  if (s < 0 || s >= kLinks) return false;
  const Profile* h = __atomic_load_n(&g_links[s].held, __ATOMIC_RELAXED);
  return (h ? h : g_links[s].was) == &kRytm;
}
int rytm_slot() { return kVoiceSlots < 2 ? 0 : slot_held_by(&kRytm); }
int rytm_home() {
  if (kVoiceSlots < 2) return 0;
  if (on_control() && rytm_at(g_cur->index)) return g_cur->index;   // two Rytms: the one in hand
  const int r = rytm_slot();
  return r < 0 ? 0 : r;
}
CORE_COLD const char* type_word(const Profile* h) {
  if (!h || h == &kRytm || !h->state_dir) return "rytm";
  return h->state_dir[0] == '/' && h->state_dir[1] == '.' ? h->state_dir + 2 : h->state_dir;
}
CORE_COLD int box_prefix(const char* p, const char** rest) {
  *rest = p;
  if (strncmp(p, "box=", 4)) return -1;
  const char* a = p + 4;
  const char* c = strchr(a, ':');
  if (!c || c == a || !c[1]) return *rest = "box=S:<command>, S 0..1 or rytm / a4 / dt", -2;
  const int n = (int)(c - a);
  int s = -2;
  if (n == 1 && a[0] >= '0' && a[0] < '0' + kLinks) {
    s = a[0] - '0';
    if (!held(s)) return *rest = "no box in that slot", -2;
  } else {
    // its type's word ("rytm": the first of its type, "rytm2" the second), or its UID (8 hex digits, or the last 4 as its name shows)
    int seen = 0;
    for (int k = 0; k < kLinks && s < 0; ++k) {
      const Profile* h = __atomic_load_n(&g_links[k].held, __ATOMIC_RELAXED);
      const char* w = h ? type_word(h) : nullptr;
      if (!w) continue;
      const int wn = (int)strlen(w);
      if (wn <= n && !strncmp(w, a, wn)) {
        ++seen;
        if (wn == n ? seen == 1 : n == wn + 1 && a[wn] == '0' + seen) s = k;
      }
      char u[9];
      uid_text(g_links[k].uid, u);
      if (g_links[k].uid && ((n == 8 && !strncmp(a, u, 8)) || (n == 4 && !strncmp(a, u + 4, 4)))) s = k;
    }
    if (s < 0) return *rest = "no such box attached (rytm / a4 / dt, rytm2 the second of a type, or its UID)", -2;
  }
  *rest = c + 1;
  return s;
}
CORE_COLD int slot_held_by(const Profile* p) {
  if (!p) p = &kRytm;
  for (int k = 0; k < kLinks; ++k)
    if (g_links[k].held == p) return k;
  return -1;
}

static inline bool kept(const Link& l) { return l.parting || l.holding; }   // (its last holder's still: saving, or held for it)
CORE_COLD int boxes_of(const Profile* p) {
  if (!p) p = &kRytm;
  int n = 0;
  for (const Link& l : g_links)
    if (l.held == p || (!l.held && kept(l) && l.was == p)) ++n;
  return n;
}
// Box (p, uid) against a holder (h, hu): the same type, and the same UID. A known UID for a holder that had none (the presumed
// Rytm, a box by its hello) is that holder while it is the one box of its type; two unknowns, the same. A box whose UID is not
// known is not a holder whose UID is: that holder's own messages and USB come with its route and its UID, so this is another box
// of its type, and it waits (decided 2026-11-26 point 4)
static CORE_COLD bool same_box(const Profile* h, uint32_t hu, const Profile* p, uint32_t uid) {
  if (!h || h != p) return false;
  if (hu && uid) return hu == uid;
  if (hu && !uid) return false;
  return boxes_of(p) <= 1;
}
CORE_COLD int slot_of_box(const Profile* p, uint32_t uid) {
  if (!p) p = &kRytm;
  for (int k = 0; k < kLinks; ++k)
    if (same_box(g_links[k].held, g_links[k].uid, p, uid)) return k;
  return -1;
}

CORE_COLD void uid_text(uint32_t uid, char* b) {
  static const char kHex[] = "0123456789abcdef";
  for (int k = 0; k < 8; ++k) b[k] = kHex[uid >> (28 - 4 * k) & 15];
  b[8] = 0;
}
CORE_COLD bool uid_parse(const char* s, uint32_t* uid) {
  uint32_t u = 0;
  for (int k = 0; k < 8; ++k) {
    const char c = s[k];
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0) return false;
    u = u << 4 | (uint32_t)d;
  }
  if (s[8]) return false;
  *uid = u;
  return true;
}

// g_base: slot 0 while it is held, else the first slot that is (device.h). The control core's; the web follows it.
static CORE_COLD void rebase() {
  Link* b = &g_links[0];
  for (Link& l : g_links)
    if (l.held) { b = &l; break; }
  if (b == g_base) return;
  g_base = b, g_web_link = (int8_t)b->index;
  if (!g_depth) g_cur = b;
  out::g_base_slot = (int8_t)slot_of_voice(voice_base(*b));   // the base box's settings: its stage, the console's trim, AMP
}

CORE_COLD const Profile* by_id(uint16_t id) {
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->id == id) return s_list[k];
  return nullptr;
}

// A box's stream is claimed when it enumerates, its hello or not: each slot has its own stream (pi/engine/ob_usb.h), so a
// stock A4 beside a Rytm no longer stands in the Rytm's way, and its hello always comes after enumeration now that no restart
// brings it back already chosen (audio_by_hello used to wait for that restart). Its MIDI still waits for the hello (hello_gate.h).
CORE_COLD const Profile* by_usb(uint16_t vid, uint16_t pid, bool) {
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->usb_vid == vid && s_list[k]->usb_pid == pid && s_list[k]->audio) return s_list[k];
  return nullptr;
}

void strip_tick(uint32_t us) {   // (one box with controls at a time: the first slot's that has them)
  for (const Link& l : g_links)
    if (l.box && l.box->controls) return strip::tick(*l.box->controls, us);
}

static inline bool is_sidecar(const uint8_t* d, int len) { return len >= 6 && d[1] == 0x7D && d[2] == 0x52 && d[3] == 0x54; }

// The engine's output while a box is in use or a second link is up: to the link in hand (Scope), the box whose message this
// answers, else link 0. A box gets only what its out_allow lists; a Rytm everything.
static CORE_COLD void filtered_out(const uint8_t* m, int n) {
  int c = n > 4 ? m[4] & 0x7f : 0;
  Link& l = cur();
  if (!l.box) {
    if (l.out) l.out(m, n);
    else ++l.dropped;
  } else if (l.box->out_allow[c >> 5] >> (c & 31) & 1) {
    if (l.out) l.out(m, n);
  } else {
    ++l.dropped;
  }
}

// hooks.to_rytm from the links: the Rytm's own sender while it is the one slot held and nothing else is linked (the Rytm's
// path as it always was: no test, no filter), else filtered_out. Transport 0's sender is the platform's to_rytm, taken the
// first time it is not already ours, and given to the slot transport 0 goes to.
static int8_t s_route[kLinks] = {0, -1};
static void (*s_tx[kLinks])(const uint8_t*, int);   // each transport's sender: 0 the platform's to_rytm, 1 set_out(1)
static CORE_COLD void rewire() {
  if (!s_tx[0] && hooks.to_rytm && hooks.to_rytm != filtered_out) s_tx[0] = hooks.to_rytm;
  if (s_route[0] >= 0 && !g_links[s_route[0]].out) g_links[s_route[0]].out = s_tx[0];
  const Link* only = nullptr;
  int n = 0;
  for (const Link& l : g_links)
    if (l.held || l.out) ++n, only = &l;
  hooks.to_rytm = n == 0 ? s_tx[0] : n == 1 && !only->box && only->out ? only->out : filtered_out;
}


CORE_COLD const char* state_path_box(const char* state, const char* path, char* buf, int cap) {
  // only /.rytm's paths are a box's: the Pi's own files are in /.sidecar (core/device/sidecar_dir.h), the libraries shared
  if (strncmp(path, "/.rytm", 6) || (path[6] && path[6] != '/')) return path;
  sniprintf(buf, cap, "%s%s", state, path + 6);
  return buf;
}

// Boot: nothing to choose. Slot 0 is presumed the Rytm's, in /.rytm, from the first file on; boxes take slots by attach order
// (docs/device-profiles.md "Several devices"). An older card's /.rytm/device (the last box, read here before two slots) is
// left as it is and never read.
static bool amp_room(int row);
static bool ins_room(int b, int c, int s, int t);
CORE_COLD void boot() {
  amp::set_cpu_check(amp_room), ins::set_cpu_check(ins_room);
  finish_moves();
  Link& l = g_links[0];
  if (l.held == &kRytm && l.presumed) {   // (the presumed Rytm: /.rytm/last's folder, else /.rytm)
    resolve(l, &kRytm, 0);
    l.was_uid = l.uid = 0;
    if (l.dir[0]) log(CORE_STR("device: slot 0 presumed the Rytm's, %s%s%s"), l.dir, l.label[0] ? ", " : "", l.label);
  }
}

CORE_COLD bool leaving() {   // a box left and its edits are not all saved yet
  for (const Link& l : g_links)
    if (l.parting) return true;
  return false;
}
static void ask_step();
static void midi_late();
static void shed_over();
CORE_COLD void cpu_step() {
  fx::guard::poll();   // the measured guard's mutes worded and logged, and released once their slot is set again (fx/fx_guard.h)
  shed_over();
  if (ins::regrant()) log(CORE_STR("device: INSERT types admitted again (CPU room)"));
}
CORE_COLD bool step(bool settled) {   // (each idle pass: once every edit is saved, a slot whose box left is free for any box)
  usb_take();   // the USB devices that came or went (core 0's)
  midi_late();  // a box's MIDI that enumerated after its USB stream took the slot
  ask_step();   // the MIDI RPC's DeviceUID asked where no USB gave it
  tell_tick();
  if (settled)
    for (Link& l : g_links)
      if (l.parting) {
        l.parting = false, l.holding = g_hold_ms > 0;
        if (l.holding) log(CORE_STR("device: slot %d saved, kept %u ms for its box"), l.index, (unsigned)g_hold_ms);
        else log(CORE_STR("device: slot %d saved, free"), l.index);
      }
  for (Link& l : g_links)
    if (l.holding && !l.parting && now_ms() - l.left_ms >= g_hold_ms) l.holding = false, log(CORE_STR("device: slot %d free"), l.index);
  admit_alone();
  cpu_step();
  rebase();
  return false;
}


// ---- UID folders (design decided 2026-11-26): every box's state in <type folder>/<uid>/, /.rytm/<uid>/ the Rytm's. The type
// folder keeps `last` (the UID attached last: a box whose UID is not known, alone of its type, uses that folder) and,
// on a card from before this build, the old layout's files: the first box of the type to attach with a known UID inherits
// them (move_old). Paths here are the card's own (RawPath): /.rytm is not any box's folder while they run.
static const char* type_dir(const Profile* h) { return h && h != &kRytm && h->state_dir ? h->state_dir : "/.rytm"; }
static CORE_COLD void uid_dir(const char* t, uint32_t uid, char* b, int cap) {
  char u[9];
  uid_text(uid, u);
  sniprintf(b, cap, "%s/%s", t, u);
}
static CORE_COLD int read_small(const char* path, char* b, int cap) {   // a small file whole, "" none; its length
  File f;
  b[0] = 0;
  if (!f.open(path, kRead)) return 0;
  int n = (int)f.read(b, (size_t)(cap - 1));
  f.close();
  n = n < 0 ? 0 : n;
  b[n] = 0;
  while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r' || b[n - 1] == ' ')) b[--n] = 0;
  return n;
}
static CORE_COLD bool write_small(const char* path, const char* text) {
  File f;
  if (!f.open(path, kWrite)) return false;
  const size_t n = strlen(text);
  const bool ok = f.write(text, n) == n;
  f.close();
  return ok;
}
static CORE_COLD uint32_t read_last(const char* t) {
  char p[32], b[16];
  sniprintf(p, sizeof p, "%s/last", t);
  uint32_t u = 0;
  return read_small(p, b, sizeof b) == 8 && uid_parse(b, &u) ? u : 0;
}
// An entry of a type folder that is the old layout's (moved into a UID folder): not a UID folder, not `last`, and none of the
// Pi-wide names sidecar_dir::migrate keeps in /.rytm when /.sidecar already has one (they stay for the user to sort out)
static CORE_COLD bool old_entry(const char* name, bool dir) {
  uint32_t u;
  if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, "last")) return false;
  if (dir && uid_parse(name, &u)) return false;
  return !sidecar_dir::pi_wide(name);
}
static constexpr int kOldMax = 64;
static CORE_COLD int old_entries(const char* t, char (*names)[64], int max) {   // collected first: FatFs skips entries renamed mid-walk
  File d;
  if (!d.open(t, kRead) || !d.isDir()) return 0;
  int n = 0;
  char name[256];
  bool dir;
#if CORE_MSAMP
  uint64_t size;
  uint16_t fd, ft;
  while (n < max && d.nextEntry(name, sizeof name, &dir, &size, &fd, &ft))
    if (strlen(name) < 64 && old_entry(name, dir)) strcpy(names[n++], name);
#else
  File e;
  while (n < max && e.openNext(&d, kRead)) {
    dir = e.isDir();
    e.getName(name, sizeof name);
    e.close();
    if (strlen(name) < 64 && old_entry(name, dir)) strcpy(names[n++], name);
  }
#endif
  d.close();
  return n;
}
// Type folder t's old-layout entries renamed into its UID folder (FAT: a rename, nothing copied). Its marker .moving is written
// before the first rename and removed after the last, so a power cut leaves it there and boot (finish_moves) completes the
// move. An entry whose name the UID folder already has stays where it is (logged): nothing is overwritten, nothing deleted.
static CORE_COLD int move_old(const char* t, uint32_t uid) {
  RawPath raw;
  Fs fs;
  char dir[24], mark[40];
  uid_dir(t, uid, dir, sizeof dir);
  sniprintf(mark, sizeof mark, "%s/.moving", dir);
  static char names[kOldMax][64];
  const int n = old_entries(t, names, kOldMax);
  if (!n) {
    if (fs.exists(mark)) fs.remove(mark);
    return 0;
  }
  fs.mkdir(dir);
  if (!write_small(mark, "moving the old layout's files here\n")) {
    log(CORE_STR("device: %s: marker not written, nothing moved"), dir);
    return 0;
  }
  int moved = 0;
  for (int i = 0; i < n; ++i) {
    char from[96], to[96];
    sniprintf(from, sizeof from, "%s/%s", t, names[i]);
    sniprintf(to, sizeof to, "%s/%s", dir, names[i]);
    if (fs.exists(to)) {
      log(CORE_STR("device: %s kept where it is: %s is already there"), from, to);
      continue;
    }
    if (fs.rename(from, to)) ++moved;
    else log(CORE_STR("device: %s could not be moved to %s"), from, to);
  }
  fs.remove(mark);   // (what stayed was logged: kept, or a card error; the next attach with a UID tries again)
  log(CORE_STR("device: %d of %d entries of %s moved into %s"), moved, n, t, dir);
  return moved;
}
// Boot: a move cut short by a power cut (a UID folder with .moving in it) finished, in every known type's folder
static CORE_COLD void finish_moves() {
  RawPath raw;
  Fs fs;
  for (int k = 0; k < s_n; ++k) {
    const char* t = type_dir(s_list[k]);
    bool seen = false;   // (the Rytm's folder is every profile without a state_dir's: once)
    for (int j = 0; j < k; ++j) seen = seen || !strcmp(type_dir(s_list[j]), t);
    if (seen) continue;
    File d;
    if (!d.open(t, kRead) || !d.isDir()) continue;
    static char found[4][9];
    int nf = 0;
    char name[256];
    File e;
    while (nf < 4 && e.openNext(&d, kRead)) {
      const bool dir = e.isDir();
      e.getName(name, sizeof name);
      e.close();
      uint32_t u;
      if (!dir || !uid_parse(name, &u)) continue;
      char mark[40];
      sniprintf(mark, sizeof mark, "%s/%s/.moving", t, name);
      if (fs.exists(mark)) strcpy(found[nf++], name);
    }
    d.close();
    for (int i = 0; i < nf; ++i) {
      uint32_t u;
      uid_parse(found[i], &u);
      log(CORE_STR("device: %s/%s: a move cut short, finished now"), t, found[i]);
      move_old(t, u);
    }
  }
}
#if CORE_MSAMP
// The move on the card core (a kFnReq: after the card requests queued before it, before the attach's reads of the folder)
static struct Move { cardq::Req rq; const char* t; uint32_t uid; } s_move[kLinks];
static void move_fn(cardq::Req* r) {
  for (Move& m : s_move)
    if (&m.rq == r) move_old(m.t, m.uid);
}
#endif
// Slot l's folder for holder h with UID uid (select). A known UID: its UID folder (the type's old layout moved in first, the
// type's `last` written); none: `last`'s folder, else the type folder itself (a card no UID was ever read for: as before).
static CORE_COLD void resolve(Link& l, const Profile* h, uint32_t uid) {
  const char* t = type_dir(h);
  RawPath raw;
  Fs fs;
  if (uid) {
    static char names[1][64];
    if (old_entries(t, names, 1)) {
#if CORE_MSAMP
      Move& m = s_move[l.index];
      if (cardq::worker() && cardq::idle(&m.rq)) {
        memset(&m.rq, 0, sizeof m.rq);
        m.t = t, m.uid = uid, m.rq.kind = cardq::kFnReq, m.rq.fn = move_fn, m.rq.link = (int8_t)l.index;
        if (!cardq::post(&m.rq)) move_old(t, uid);
      } else {
        move_old(t, uid);
      }
#else
      move_old(t, uid);
#endif
    }
    char u[9];
    uid_text(uid, u);
    if (read_last(t) != uid) {
      char p[32];
      sniprintf(p, sizeof p, "%s/last", t);
      fs.mkdir(t);
      write_small(p, u);
    }
  } else {
    uid = read_last(t);
    char d[24];
    if (uid) uid_dir(t, uid, d, sizeof d);
    if (uid && !fs.exists(d)) uid = 0;
  }
  if (!uid) {
    l.dir[0] = 0, l.label[0] = 0;
    l.state = h == &kRytm ? nullptr : h->state_dir;
    return;
  }
  uid_dir(t, uid, l.dir, sizeof l.dir);
  fs.mkdir(t), fs.mkdir(l.dir);
  char p[48];
  sniprintf(p, sizeof p, "%s/name.txt", l.dir);
  if (!read_small(p, l.label, sizeof l.label)) {   // its default name: the type and the UID's last 4 hex digits ("Rytm 3fa2")
    char u[9];
    uid_text(uid, u);
    sniprintf(l.label, sizeof l.label, "%s %s", short_name(*h), u + 4);
    write_small(p, l.label);
  }
  l.state = l.dir;
}

CORE_COLD bool set_label(int s, const char* text) {
  if (s < 0 || s >= kLinks || !g_links[s].held || !g_links[s].dir[0]) return false;
  Link& l = g_links[s];
  char b[sizeof l.label];
  int n = 0;
  for (const char* c = text; *c && n < (int)sizeof b - 1; ++c)
    if ((uint8_t)*c >= 0x20 && *c != 0x7f) b[n++] = *c;   // (one line: no control characters)
  while (n > 0 && b[n - 1] == ' ') --n;
  b[n] = 0;
  if (!n) return false;
  RawPath raw;
  char p[48];
  sniprintf(p, sizeof p, "%s/name.txt", l.dir);
  if (!write_small(p, b)) return false;
  memcpy(l.label, b, sizeof b);
  log(CORE_STR("device: slot %d is named %s"), s, l.label);
  return true;
}

// CPU admission (admit.h). s_order: attach order, so the first box (the lower order) keeps its voices.
static uint32_t s_order = 1;
int voices_asked(const Profile* h) {
  if (!h) h = &kRytm;
  const int o = admit::override_for(h == &kRytm ? "rytm" : short_name(*h));
  const int n = o >= 0 && o < h->voices ? o : h->voices;   // (voices= lowers a count: a box's reports past its profile's are dropped)
  return n > kBoxVoices ? kBoxVoices : n;
}
void load_now(admit::Load* out);
// A voice chain's INSERT cost: twice its mono bench row while its voice is on OUT PRE / POST (the render cores run it in
// stereo: voices.cpp stv, insert.h ProcessVoiceStereo); a stereo-mix box (out::kStereoMix) has every voice on POST
static int chain_mul(int b, int c) { return out::control_of(b).route[c & 7] != out::kTrk ? 2 : 1; }
static admit::Load load_now() {
  admit::Load ld;
  load_now(&ld);
  return ld;
}
void load_now(admit::Load* out) {
  admit::Load& ld = *out;
  ld = {};
  for (int k = 0; k < kLinks && k < 2; ++k)
    ld.voices[k] = g_links[k].held ? g_links[k].voices : 0, ld.held[k] = g_links[k].held != nullptr, ld.asked[k] = g_links[k].want;
  const int m = amp::master_row();   // every box's AMPs playing, each on its render core (admit.h amp_core_of): the first ask wins
  for (int r = 0; r < amp::rows(); ++r)
    for (int s = 0; s < amp::kSlots; ++s)
      if (amp::slot_playing(r, s)) {
        const int c = admit::amp_core_of(amp::row_box(r), amp::row_chain(r), m, ld);
        if (c >= 1 && c <= 3) ++ld.amps[c];
      }
  static_assert(ins::kAmp == 9 && ins::kTypes == admit::kSendType0 && ins::kMChorus == admit::kSendType0 &&
                    ins::kAllTypes == admit::kSendType0 + admit::kSendTypes,
                "admit.cpp type_bp / master_slot_bp: AMP is 9, the voice types under kSendType0, then the send TYPEs");
  for (int b = 0; b < kVoiceSlots && b < 2; ++b) {   // the voice INSERT slots admitted (insert.h refused: not counted)
    if (!ld.held[b]) continue;
    const ins::Control& k = ins::control_of(b);
    const int n = ld.asked[b] < ins::kVoices ? ld.asked[b] : ins::kVoices;   // (its chains past what it asked never render)
    for (int c = 0; c < n; ++c)
      for (int s = 0; s < ins::kSlots; ++s)
        if (!k.refused[c][s]) ld.ins[admit::voice_core(b, c, ld)] += chain_mul(b, c) * admit::type_bp(k.type[c][s]);
  }
  if (ld.held[0] && admit::master_core(0) == 3) {   // slot 0's master chain on core 3: its INSERT types (stereo: twice)
    const ins::Control& k = ins::control_of(0);
    for (int c = ins::kM0; c < ins::kChains; ++c)
      for (int s = 0; s < ins::kSlots; ++s)
        if (!k.refused[c][s]) ld.ins[3] += admit::master_slot_bp(k.type[c][s]);
  }
}
// INSERT's CPU check (ins::set_cpu_check): box slot b's voice chain c, slot s may play type t, beside everything admitted
// now but what that slot plays (it is replaced). A chain its box does not render (not held, past what it asked): yes.
static CORE_COLD bool ins_room(int b, int c, int s, int t) {
  admit::Load ld = load_now();
  if (c >= ins::kM0) {   // a master chain: slot 0's on core 3 (counted there); slot 1's on core 1 is in its box's chains row
    if (b != 0 || !ld.held[0] || admit::master_core(0) != 3) return true;
    const ins::Control& k = ins::control_of(0);
    if (!k.refused[c][s]) ld.ins[3] -= admit::master_slot_bp(k.type[c][s]);
    return admit::ins3_fits(ld, t);
  }
  if (b < 0 || b > 1 || !ld.held[b] || c >= ld.asked[b]) return true;
  const int core = admit::voice_core(b, c, ld);
  const ins::Control& k = ins::control_of(b);
  const int mul = chain_mul(b, c);
  if (!k.refused[c][s]) ld.ins[core] -= mul * admit::type_bp(k.type[c][s]);
  return admit::ins_fits(core, ld, t, mul);
}
// AMP's CPU check (amp::set_cpu_check): one more AMP slot on row's render core, beside what renders now
static CORE_COLD bool amp_room(int row) {
  const admit::Load ld = load_now();
  return admit::amp_fits(admit::amp_core_of(amp::row_box(row), amp::row_chain(row), amp::master_row(), ld), ld);
}
// The clock lowered (admit::set_clock: the engine's step-down for heat, decided 2026-11-22 for a Pi without cooling, or
// cpumhz=) leaves a render core over kLimitBp with what is admitted: shed one item a step until each is within it. AMPs
// first (the costliest: the highest voice row first), then the voice INSERT slots of the box attached last (the first ask
// wins), its highest voice and slot first. Each plays dry and shows NO CPU, and is admitted again when it fits (amp_fits,
// ins::regrant: the same limit with its own cost added, so nothing flickers). Voices are never shed here.
static CORE_COLD void shed_over() {
  if (!admit::on() || !admit::enforce()) return;
  admit::Load ld;
  load_now(&ld);
  {   // core 3 (slot 0's master chain: its master AMPs, the highest row first)
    const int m = amp::master_row();
    if (admit::core3_bp(ld) > admit::kC3LimitBp)
      for (int r = amp::rows() - 1; r >= 0; --r)
        for (int s = amp::kSlots - 1; s >= 0; --s)
          if (amp::row_chain(r) >= m && amp::slot_playing(r, s) && admit::amp_core_of(amp::row_box(r), amp::row_chain(r), m, ld) == 3 &&
              amp::shed(r, s)) {
            log(CORE_STR("device: core 3 over 70 %% at %u MHz: master AMP row %d %c shed (NO CPU)"), admit::clock(), r, 'A' + s);
            return;
          }
    if (admit::core3_bp(ld) > admit::kC3LimitBp && ld.held[0])   // then its master INSERT types, the last slot first
      for (int c = ins::kChains - 1; c >= ins::kM0; --c)
        for (int s = ins::kSlots - 1; s >= 0; --s)
          if (admit::master_slot_bp(ins::control_of(0).type[c][s]) && ins::shed(0, c, s)) {
            log(CORE_STR("device: core 3 over 70 %% at %u MHz: master %d %c shed (NO CPU)"), admit::clock(), c - ins::kM0 + 1,
                'A' + s);
            return;
          }
  }
  for (int core = 1; core <= 2; ++core) {
    if (admit::core_bp(core, ld) <= admit::kLimitBp) continue;
    const int m = amp::master_row();
    for (int r = amp::rows() - 1; r >= 0; --r)   // (every box's voice and master AMPs on this core, the highest row first)
      for (int s = amp::kSlots - 1; s >= 0; --s)
        if (amp::slot_playing(r, s) && admit::amp_core_of(amp::row_box(r), amp::row_chain(r), m, ld) == core && amp::shed(r, s)) {
          log(CORE_STR("device: core %d over 80 %% at %u MHz: AMP row %d %c shed (NO CPU)"), core, admit::clock(), r, 'A' + s);
          return;
        }
    int order[2] = {0, 1};
    if (g_links[0].order < g_links[1].order) order[0] = 1, order[1] = 0;   // the box attached last first
    for (int b : order) {
      if (b >= kVoiceSlots || !ld.held[b]) continue;
      const int n = ld.asked[b] < ins::kVoices ? ld.asked[b] : ins::kVoices;
      for (int c = n - 1; c >= 0; --c)
        for (int s = ins::kSlots - 1; s >= 0; --s)
          if (admit::voice_core(b, c, ld) == core && ins::shed(b, c, s)) {
            log(CORE_STR("device: core %d over 80 %% at %u MHz: slot %d V%d %c shed (NO CPU)"), core, admit::clock(), b, c + 1,
                'A' + s);
            return;
          }
    }
  }
}
static CORE_COLD void set_voices(Link& l, int n, bool tell) {
  __atomic_store_n(&l.voices, (uint8_t)n, __ATOMIC_RELAXED);
  l.tell = tell, l.tell_until = now_ms() + kTellMs;
}
// Slot l's box has just attached: its voices, beside the other slot's box if one is held (the first keeps its own).
// back: the box came back to the slot still kept for it (saving, or held g_hold_ms: a plug or unplug on one of the Pi's hub
// ports knocks the box on the other port off and back, seen on a unit 2026-11-21, 8 of 8). It keeps its place in the attach
// order, so a first box stays first and keeps its voices; the box that stayed is not raised meanwhile (admit_alone).
static CORE_COLD void admit_slot(Link& l, bool back) {
  l.want = (uint8_t)voices_asked(l.held);
  if (!back || !l.order) l.order = ++s_order;
  bool other = false;
  for (const Link& o : g_links)
    if (&o != &l && o.held && (!back || o.order < l.order)) other = true;
  admit::Load ld = load_now();
  ld.voices[l.index] = 0;
  const int n = other || kVoiceSlots < 2 ? admit::admit(l.index, l.want, ld) : l.want;
  set_voices(l, kVoiceSlots < 2 || !other ? l.want : n, n < l.want);
  if (l.voices < l.want)
    log(CORE_STR("device: slot %d: %d of %d voices admitted (CPU: each render core under 80 %%)"), l.index, l.voices, l.want);
  ld = load_now();
  if (admit::on() && (admit::core_bp(1, ld) > admit::kLimitBp || admit::core_bp(2, ld) > admit::kLimitBp))
    log(CORE_STR("device: render cores at worst %u / %u bp (limit %u)"), admit::core_bp(1, ld), admit::core_bp(2, ld), admit::kLimitBp);
}
// Slot l's box left: its voices stop (its place in the attach order is kept while its edits are saved: it may come back).
static CORE_COLD void admit_left(Link& l) {
  set_voices(l, 0, false);
  l.want = 0;
}
// Each idle pass: a box alone (no other slot held, or kept for a box that left: it may come back) gets back what it asked for.
static CORE_COLD void admit_alone() {
  for (Link& o : g_links) {
    if (!o.held || o.voices >= o.want) continue;
    bool other = false;
    for (const Link& x : g_links)
      if (&x != &o && (x.held || x.parting || x.holding)) other = true;
    if (other) continue;
    log(CORE_STR("device: slot %d: alone, %d voices again"), o.index, o.want);
    set_voices(o, o.want, true);
  }
}
CORE_COLD void readmit() {
  Link* first = nullptr;
  for (Link& l : g_links)
    if (l.held && (!first || l.order < first->order)) first = &l;
  if (!first) return;
  first->want = (uint8_t)voices_asked(first->held);
  set_voices(*first, first->want, false);
  for (Link& l : g_links) {
    if (&l == first || !l.held) continue;
    l.want = (uint8_t)voices_asked(l.held);
    admit::Load ld = load_now();
    ld.voices[l.index] = 0;
    const int n = admit::admit(l.index, l.want, ld);
    set_voices(l, n, n < l.want);
  }
}
// Its box told its count while it may take a popup (its screen: the remote UI's own rule), in its Scope; a box with no screen
// for it reads /status.
static CORE_COLD void tell_tick() {
  for (Link& l : g_links) {
    if (!l.tell) continue;
    if (!l.held || (int32_t)(now_ms() - l.tell_until) > 0) { l.tell = false; continue; }
    char a[24], b[24];
    sniprintf(a, sizeof a, "SIDECAR %d/%d VOICES", l.voices, l.want);
    sniprintf(b, sizeof b, l.voices < l.want ? "CPU FULL" : "ALL VOICES");
    Scope in(l);
    if (ui::remote_popup(a, b, 3000)) l.tell = false;
  }
}

static uint32_t s_held[kBoxVoices][4];   // box_note's
static struct { uint8_t n, vb, notes[kChordNotes]; uint32_t at; } s_chord[kBoxVoices];   // chord_note's: the notes so far, the machine, the newest note's time
// Slot l's holder now (h: a profile, rytm() the Rytm; null: free). Its box is h unless h is the Rytm. A holder takes its folder
// (its type's; the Rytm's /.rytm). A slot freed keeps its folder until its next holder: a write queued for the box that left (a
// card job carries its slot, not the folder) still lands in that box's own folder.
static CORE_COLD void select(Link& l, const Profile* h, uint32_t uid) {
  const Profile* p = h == &kRytm ? nullptr : h;
  if (h == l.held && p == l.box && (!h || uid == l.uid)) return;
  memset(s_held, 0, sizeof s_held);   // the box before it: what it held is not the next one's
  memset(s_chord, 0, sizeof s_chord);
  if (!l.box) rewire();   // (link 0's sender taken while it is still the platform's)
  __atomic_store_n(&l.held, h, __ATOMIC_RELAXED);
  __atomic_store_n(&l.box, p, __ATOMIC_RELAXED);
  l.presumed = false;
  // (CPU admission: its place in the attach order kept, for the same box: same type and, where both are known, the same UID)
  const bool back = h && h == l.was && (!uid || !l.was_uid || l.was_uid == uid) && (l.parting || l.holding);
  if (h) {
    l.uid = l.was_uid = uid;
    resolve(l, h, uid);   // its folder: its UID's (design 2026-11-26), else its type's last, else the type folder
    l.was = h, l.parting = l.holding = false;
    if (l.state && !l.dir[0]) Fs().mkdir(l.state);
  } else {
    l.uid = 0;
  }
  rewire();
  rebase();
  // the slot's TRK trim, its box's (the Rytm's: 0). On 8 voices every link plays slot 0's settings: the holder's there
  if (h || kVoiceSlots > 1) out::control_of(slot_of_voice(voice_base(l))).trk10 = p ? p->trk10 : 0;
  log(CORE_STR("device: slot %d: %s%s%s, state in %s"), l.index, h ? h->name : "free", l.label[0] && h ? " " : "",
      l.label[0] && h ? l.label : "", l.state ? l.state : "/.rytm");
  link_reset("device", l.index);   // the box before it: its notes and transport unknown (its slot's voices)
  if (h) fx::guard::clear_box(l.index);   // its box attached (again): the measured guard's mutes of its slot released
  if (h) admit_slot(l, back);
  else admit_left(l);
}

// Which slot each transport's messages are (in), -1 none: transport 0 is the platform's host link (the USB-C box's MIDI, or the
// emulator's TCP link), transport 1 the other box's (pi/engine/link1.h, the Teensy relay). Transport 0 goes to the presumed
// Rytm's slot 0 from boot; a hello on either claims its box's slot (attach order), a Rytm's first message the Rytm's.
static CORE_COLD void route(int t, int slot) {
  for (int k = 0; k < kLinks; ++k)
    if (k != t && s_route[k] == slot) s_route[k] = -1;   // (a box speaks on one transport: the other one's old route goes)
  s_route[t] = (int8_t)slot;
  for (Link& l : g_links) l.out = nullptr;
  for (int k = 0; k < kLinks; ++k)
    if (s_route[k] >= 0) g_links[s_route[k]].out = s_tx[k];
  rewire();
}

// usb_attach's reservations (core 0 writes them, the control core takes them in step / claim) and usb_detach's
static const Profile* volatile s_usb_want[kLinks];
static volatile uint32_t s_usb_uid[kLinks];   // with s_usb_want: the box's UID (written before it)
static volatile bool s_usb_gone[kLinks];
static volatile int8_t s_midi_slot[kLinks] = {-1, -1};   // set_midi_slot's
static inline bool own_kept(const Link& l, const Profile* p, uint32_t uid) {   // kept for box (p, uid): its own slot
  return kept(l) && l.was == p && (!uid || !l.was_uid || l.was_uid == uid);
}
static CORE_COLD bool open_for(const Link& l, const Profile* p, uint32_t uid) {   // a slot (p, uid) may take now (no other's reservation)
  const Profile* w = __atomic_load_n(&s_usb_want[l.index], __ATOMIC_ACQUIRE);
  const Profile* h = __atomic_load_n(&l.held, __ATOMIC_RELAXED);
  const bool mine = w == p && (!uid || !s_usb_uid[l.index] || s_usb_uid[l.index] == uid);
  return (!w || mine) && (!h || (l.presumed && p != &kRytm)) && (!kept(l) || own_kept(l, p, uid));
}
void set_midi_slot(int t, int s) {
  if (t >= 0 && t < kLinks) __atomic_store_n(&s_midi_slot[t], (int8_t)(s >= 0 && s < kLinks ? s : -1), __ATOMIC_RELAXED);
}
// A known UID for the box holding slot l (it had none: the presumed Rytm, a box by its hello or past its USB's timeout): its
// folder stays when it is the same (the folder `last` named), else the slot reads its own UID's (select)
static CORE_COLD void learn_uid(Link& l, uint32_t uid) {
  if (!uid || l.uid == uid || !l.held) return;
  char d[24];
  uid_dir(type_dir(l.held), uid, d, sizeof d);
  if (!strcmp(d, l.dir)) {
    l.uid = l.was_uid = uid;
    resolve(l, l.held, uid);   // (the same folder: its `last` kept, an old layout still there moved in)
    log(CORE_STR("device: slot %d: its UID %s"), l.index, d);
    return;
  }
  const Profile* h = l.held;
  const bool presumed = l.presumed;
  select(l, h, uid);
  l.presumed = presumed;
}

CORE_COLD int usb_attach(const Profile* p, uint32_t uid) {
  if (!p) p = &kRytm;
  // a slot already reserved: this box's when the reservation is (p, uid), another box's (skipped) when not
  bool other[kLinks] = {};
  for (const Link& l : g_links) {
    const Profile* w = __atomic_load_n(&s_usb_want[l.index], __ATOMIC_ACQUIRE);
    if (!w) continue;
    if (w == p && (!uid || !s_usb_uid[l.index] || s_usb_uid[l.index] == uid)) return l.index;
    other[l.index] = true;
  }
  for (const Link& l : g_links) {
    if (other[l.index]) continue;
    const Profile* h = __atomic_load_n(&l.held, __ATOMIC_RELAXED);
    if (!(l.presumed && h == p) && !same_box(h, l.uid, p, uid)) continue;
    // its slot already (the Rytm's presumed at boot): reserved for it all the same, so usb_take confirms it (a stock Rytm
    // says no 35: its USB is its hello) and routes its MIDI
    s_usb_uid[l.index] = uid;
    const Profile* none = nullptr;
    __atomic_compare_exchange_n(&s_usb_want[l.index], &none, p, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return l.index;
  }
  // a box of a type already here, its UID not known: it waits (decided 2026-11-26 point 4); the driver claims no stream for it
  if (!uid && slot_held_by(p) >= 0) return -1;
  // its own slot first while that is still saving (a box that dropped off the bus a moment and came back: it stays where it
  // was, the RA1 re-enumeration), then the lowest free one
  for (int own = 1; own >= 0; --own)
    for (const Link& l : g_links) {
      if (own && !own_kept(l, p, uid)) continue;
      if (!open_for(l, p, uid)) continue;
      s_usb_uid[l.index] = uid;
      const Profile* none = nullptr;
      if (__atomic_compare_exchange_n(&s_usb_want[l.index], &none, p, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return l.index;
    }
  return -1;
}
void usb_detach(int s) {
  if (s >= 0 && s < kLinks) __atomic_store_n(&s_usb_gone[s], true, __ATOMIC_RELEASE);
}
static CORE_COLD void usb_take() {   // (the control core: step, claim)
  for (Link& l : g_links) {
    if (__atomic_exchange_n(&s_usb_gone[l.index], false, __ATOMIC_ACQ_REL)) {
      __atomic_store_n(&s_usb_want[l.index], (const Profile*)nullptr, __ATOMIC_RELEASE);
      release(l.index);
    }
    const Profile* w = __atomic_load_n(&s_usb_want[l.index], __ATOMIC_ACQUIRE);
    if (!w) continue;
    const uint32_t uid = s_usb_uid[l.index];
    if (l.held != w) {
      if (l.held && l.presumed) release(l.index);   // (the presumed Rytm never spoke: the box's USB came first)
      if (!l.held) select(l, w, uid), log(CORE_STR("device: %s attached (USB): slot %d"), w->name, l.index);
    } else {
      learn_uid(l, uid);   // (the presumed Rytm, or a box that said hello first: its UID now)
    }
    // its MIDI: the transport the Pi's USB says is part of it (set_midi_slot); none said, a Rytm's is the host link's as ever
    // (a stock Rytm says no 35)
    if (l.held == w) {
      int t = -1;
      for (int k = 0; k < kLinks; ++k)
        if (s_midi_slot[k] == l.index && s_route[k] < 0) t = k;
      if (t < 0 && w == &kRytm && s_route[0] < 0 && s_midi_slot[0] < 0) t = 0;
      if (t >= 0) route(t, l.index);
      // no UID from its USB: asked by MIDI on its transport (design attach step 2's fallback; the presumed Rytm's too)
      for (int k = 0; k < kLinks && !l.uid; ++k)
        if (s_route[k] == l.index) ask_uid(k, w);
    }
    l.presumed = false;
    __atomic_store_n(&s_usb_want[l.index], (const Profile*)nullptr, __ATOMIC_RELEASE);
  }
}

// A box held by its USB whose MIDI interface the Pi matched to it only after usb_take ran (set_midi_slot came later: the
// link1 scan, every 250 ms, after the stream's attach) got no route, so a stock box (no hello, no 35) never had its
// transport routed: its realtime bytes (its own clock) were dropped (unit run CK2, T521, 2026-11-26: the A4 on link1). Each
// pass, a transport the Pi says is a held slot's box, routed nowhere, while no other transport carries that slot: routed.
static CORE_COLD void midi_late() {
  for (int t = 0; t < kLinks; ++t) {
    const int s = s_midi_slot[t];
    if (s < 0 || s_route[t] >= 0 || !g_links[s].held || g_links[s].presumed) continue;
    bool carried = false;
    for (int k = 0; k < kLinks; ++k) carried = carried || s_route[k] == s;
    if (carried) continue;
    route(t, s);
    log(CORE_STR("device: transport %d is slot %d's box (its USB MIDI): routed"), t, s);
    if (!g_links[s].uid) ask_uid(t, g_links[s].held);
  }
}

CORE_COLD int claim(const Profile* p, uint32_t uid, int hint) {
  if (!p) p = &kRytm;
  usb_take();
  if (hint >= 0 && hint < kLinks && g_links[hint].held == p) {   // the slot its transport's USB device is (two of a type: theirs)
    learn_uid(g_links[hint], uid);
    return g_links[hint].presumed = false, hint;
  }
  const int had = slot_of_box(p, uid);
  if (had >= 0) return learn_uid(g_links[had], uid), g_links[had].presumed = false, had;
  // a box of a type already here, its UID not known: it waits (decided 2026-11-26 point 4)
  if (!uid && slot_held_by(p) >= 0) {
    log(CORE_STR("device: %s: another of its type is here and its UID is not known: it waits"), p->name);
    return -1;
  }
  // the presumed Rytm never spoke: the first box to say hello is the first attached, slot 0 is its
  for (Link& l : g_links)
    if (l.presumed && p != &kRytm) release(l.index);
  // the lowest free slot; one whose box left and is still saving only for that same box (its own folder); none another
  // box's USB has just reserved
  for (int own = 1; own >= 0; --own)   // (its own slot first while that is still saving: usb_attach's)
  for (Link& l : g_links)
    if (!l.held && (!own || own_kept(l, p, uid)) && open_for(l, p, uid)) {
      select(l, p, uid);
      log(CORE_STR("device: %s attached: slot %d"), p->name, l.index);
      return l.index;
    }
  log(CORE_STR("device: %s: no slot free"), p->name);
  return -1;
}

CORE_COLD void release(int s) {
  if (s < 0 || s >= kLinks || !g_links[s].held) return;
  Link& l = g_links[s];
  const Profile* h = l.held;
  const bool quiet = l.presumed;   // (a Rytm presumed at boot never spoke: nothing of its own to save beyond what idle_save does anyway)
  slot_left(s);   // its transport, sequencer, arpeggiator and MIDI stop, in its Scope (host_link.h)
  select(l, nullptr);   // its voices silenced (link_reset); its folder kept for its saves
  l.parting = !quiet, l.holding = false, l.left_ms = now_ms();
  for (int t = 0; t < kLinks; ++t)
    if (s_route[t] == s) s_route[t] = -1;
  route(0, s_route[0]);   // (the senders again)
  log(CORE_STR("device: %s left slot %d%s"), h->name, s, quiet ? "" : ": its edits saved into its folder");
}

void rytm_on_usb(bool on) {
  if (on) {
    const int s = claim(&kRytm);
    if (s >= 0 && s_route[0] < 0) route(0, s);   // its MIDI on the host link (a stock Rytm says no 35), unless a box has it
  }
  else release(slot_held_by(&kRytm));   // (its link: rytm_slot() is 0 on one slot)
}

CORE_COLD void set_out(int link, void (*out)(const uint8_t*, int)) {
  if (link <= 0 || link >= kLinks) return;   // (transport 0's is the platform's to_rytm)
  rewire();   // transport 0's sender taken first, while hooks.to_rytm is still the platform's
  s_tx[link] = out;
  if (s_route[link] >= 0) g_links[s_route[link]].out = out;
  if (!out && s_route[link] >= 0) release(s_route[link]);   // its box went: its slot free again (the other box stays)
  rewire();
}

// The slot the bytes of transport t are for, -1 none.
int route_of(int t) { return t >= 0 && t < kLinks ? s_route[t] : -1; }
int rytm_route(int t) {
  const int s = route_of(t);
  if (s < 0) return t == 0 ? rytm_home() : -1;
  return rytm_at(s) ? s : -1;
}

// A hello's profile when the box may have a slot (known, this protocol); null: its answer is kUnknown / kBadProto
static CORE_COLD const Profile* hello_profile(const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1 || d[8] != kProto) return nullptr;
  return by_id((uint16_t)(d[6] << 7 | d[7]));
}

CORE_COLD int slot_of(int t, const uint8_t* d, int len) {
  if (t < 0 || t >= kLinks) t = 0;
  const int r = s_route[t];
  if (!d || len < 6) return r;
  if (r >= 0 && d[4] != kCmdHello && d[4] != 0x35) {   // (every other message of a slot's box: two compares, and the presumed test)
    if (g_links[r].presumed && is_sidecar(d, len)) g_links[r].presumed = false;   // the presumed Rytm spoke: slot 0 is the Rytm's
    return r;
  }
  if (!s_tx[0] && hooks.to_rytm && hooks.to_rytm != filtered_out) s_tx[0] = hooks.to_rytm;
  if (!is_sidecar(d, len)) return r;
  if (d[4] == kCmdHello) {
    const Profile* p = hello_profile(d, len);
    if (!p) return -1;   // (engine_sysex_link answers kUnknown / kBadProto)
    if (r >= 0 && g_links[r].held == p) return r;   // (its hello again)
    if (r >= 0 && g_links[r].held && !g_links[r].presumed) release(r);   // another box on this transport now: the one before left
    const int s = claim(p, 0, s_midi_slot[t]);
    if (s >= 0) route(t, s);
    else if (slot_held_by(p) >= 0) ask_uid(t, p);   // (one of its type here: its UID says whether it is another, or it waits)
    return s;
  }
  if (d[4] != 0x35) return r;   // (a transport that goes nowhere: only a hello or a Rytm's 35 gives it a slot)
  // a Rytm's 35, its hello (a remote-UI box's slot keeps its own: translate drops a 35 from it)
  if (r >= 0 && (g_links[r].held == &kRytm || (g_links[r].box && (g_links[r].box->features & kRemoteUi)))) {
    if (g_links[r].presumed) g_links[r].presumed = false;
    return r;
  }
  if (r >= 0) release(r);   // a Rytm on this transport now: the box before it left
  const int s = claim(&kRytm, 0, s_midi_slot[t]);
  if (s >= 0) route(t, s);
  else if (slot_held_by(&kRytm) >= 0) ask_uid(t, &kRytm);
  return s;
}


// ---- the MIDI RPC DeviceUID (device.h): the fallback when a box's USB did not give its UID
bool (*g_frozen)(int slot) = nullptr;
static struct Ask { const Profile* p; uint32_t since, last; bool on; } s_ask[kLinks];
static uint16_t s_seq = 0x0100;
static const uint8_t kRpcHead[6] = {0xF0, 0x00, 0x20, 0x3C, 0x10, 0x00};
CORE_COLD int uid_request(uint16_t seq, uint8_t* m) {
  const uint8_t msg[5] = {(uint8_t)(seq >> 8), (uint8_t)seq, 0, 0, 0x03};
  int n = 0;
  for (uint8_t b : kRpcHead) m[n++] = b;
  uint8_t top = 0;
  for (int k = 0; k < 5; ++k) top |= (uint8_t)((msg[k] >> 7) << (6 - k));
  m[n++] = top;
  for (int k = 0; k < 5; ++k) m[n++] = msg[k] & 0x7f;
  m[n++] = 0xF7;
  return n;
}
CORE_COLD bool uid_answer(const uint8_t* d, int len, uint32_t* uid) {
  if (len < 8 || d[0] != 0xF0 || d[1] != 0x00 || d[2] != 0x20 || d[3] != 0x3C || d[5] != 0x00 || d[len - 1] != 0xF7) return false;
  if (d[4] == 0x7D || d[4] == 0x07) return false;   // (analoghealth's channel, the Rytm's own dumps)
  uint8_t m[16];
  int n = 0;
  for (int i = 6; i < len - 1 && n < (int)sizeof m; i += 8)   // 8-to-7: a group's top bits first, then up to 7 bytes
    for (int k = 0; k < 7 && i + 1 + k < len - 1 && n < (int)sizeof m; ++k)
      m[n++] = (uint8_t)(d[i + 1 + k] | (d[i] & (0x40 >> k) ? 0x80 : 0));
  if (n < 9 || m[4] != 0x83) return false;
  *uid = (uint32_t)m[5] << 24 | (uint32_t)m[6] << 16 | (uint32_t)m[7] << 8 | m[8];
  return *uid != 0;
}
CORE_COLD void ask_uid(int t, const Profile* p) {
  if (kVoiceSlots < 2 || t < 0 || t >= kLinks || s_ask[t].on) return;   // (the Teensy, one slot, never asks)
  s_ask[t] = Ask{p ? p : &kRytm, now_ms(), 0, true};
  s_ask[t].last = s_ask[t].since - kAskMs;   // (the first ask at the next pass)
  log(CORE_STR("device: transport %d: %s's UID asked by MIDI (its USB gave none)"), t, s_ask[t].p->name);
}
bool asking(int t) { return t >= 0 && t < kLinks && s_ask[t].on; }
static CORE_COLD void ask_step() {
  for (int t = 0; t < kLinks; ++t) {
    Ask& a = s_ask[t];
    if (!a.on) continue;
    const uint32_t now = now_ms();
    const int s = s_route[t] >= 0 ? s_route[t] : s_midi_slot[t];
    if (now - a.since >= kAskForMs) {
      a.on = false;
      log(CORE_STR("device: transport %d: no UID answer in %u ms: %s"), t, (unsigned)kAskForMs,
          s >= 0 && g_links[s].held ? "its type's last folder" : "it waits");
      continue;
    }
    if (now - a.last < kAskMs) continue;
    a.last = now;
    if (s >= 0 && g_frozen && g_frozen(s)) continue;   // never MIDI to a frozen box
    void (*out)(const uint8_t*, int) = tx(t);
    if (!out) continue;
    uint8_t m[16];
    out(m, uid_request(s_seq++ & 0x3fff, m));
  }
}
// An answer on transport t: the slot it goes to learns its UID, or (a box that waited) one is claimed for it now
static CORE_COLD bool take_answer(int t, const uint8_t* d, int len) {
  uint32_t uid;
  if (!uid_answer(d, len, &uid)) return false;
  Ask& a = s_ask[t];
  const Profile* p = a.on ? a.p : nullptr;
  a.on = false;
  const int r = s_route[t];
  if (r >= 0 && g_links[r].held) {
    log(CORE_STR("device: transport %d: its box's UID by MIDI"), t);
    learn_uid(g_links[r], uid);
  } else if (p) {
    const int s = claim(p, uid, s_midi_slot[t]);
    if (s >= 0) route(t, s);
  }
  return true;
}

CORE_COLD void reset() {
  rewire();   // (the platform's sender taken, if it is not yet)
  void (*plat)(const uint8_t*, int) = s_tx[0];
  g_cur = g_base = &g_links[0], g_depth = 0, s_card_link = -1, g_web_link = 0, out::g_base_slot = 0;
  s_route[0] = 0, s_route[1] = -1;
  for (Link& l : g_links)
    select(l, nullptr), l.state = nullptr, l.was = nullptr, l.parting = l.holding = false, l.out = nullptr, l.uid = l.was_uid = 0,
                        l.dir[0] = l.label[0] = 0;
  select(g_links[0], &kRytm), g_links[0].presumed = true;   // (as at boot)
  for (Link& l : g_links) l.out = nullptr;
  s_tx[0] = s_tx[1] = nullptr;
  for (int k = 0; k < kLinks; ++k) s_usb_want[k] = nullptr, s_usb_uid[k] = 0, s_usb_gone[k] = false, s_midi_slot[k] = -1, s_ask[k].on = false;
  hooks.to_rytm = plat;          // back to the platform's sender (taken again: a test may set another)
  strip::reset();
  amp::set_cpu_check(amp_room), ins::set_cpu_check(ins_room);
}

CORE_COLD bool add(const Profile* p) {
  if (!p || p->voices < 1 || p->voices > kBoxVoices || p->params > kMaxParams || s_n >= kMax) return false;
  for (int k = 0; k < kMaxParams; ++k)
    if (p->map[k] >= p->params) return false;
  for (int k = 0; k < s_n; ++k)
    if (s_list[k]->id == p->id) return false;
  s_list[s_n++] = p;
  return true;
}

void (*tx(int t))(const uint8_t*, int) { return t >= 0 && t < kLinks ? (t == 0 && !s_tx[0] ? hooks.to_rytm : s_tx[t]) : nullptr; }

// A hello's answer: kOk when slot_of claimed this slot for it (else engine_sysex_link answered it, with hello_status)
uint8_t hello_status(const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1) return kUnknown;
  const Profile* p = by_id((uint16_t)(d[6] << 7 | d[7]));
  return !p ? kUnknown : d[8] != kProto ? kBadProto : kWait;
}

CORE_COLD void hello(Link& l, const uint8_t* d, int len) {
  if (len < 10 || d[5] != 1) return;
  int id = d[6] << 7 | d[7];
  const Profile* p = by_id((uint16_t)id);
  uint8_t st = p && d[8] == kProto && l.held == p ? kOk : hello_status(d, len);
  if (st != kOk) log(CORE_STR("device: hello from id %d proto %d on slot %d: %d"), id, d[8], l.index, st);
  const uint8_t a[10] = {0xF0, 0x7D, 0x52, 0x54, kCmdHello, 1, d[6], d[7], st, 0xF7};
  void (*out)(const uint8_t*, int) = l.out ? l.out : l.index == 0 ? hooks.to_rytm : nullptr;   // the answer is never filtered
  if (out) out(a, (int)sizeof a);
}

// The table's translation: the box's message into the engine's 0x04 / 0x03 (P1..P16, the sound tag) or 0x11.
static CORE_COLD int table(const Profile& p, const uint8_t* d, int len, uint8_t* m) {
  if (len < 7 || d[1] != p.head[0] || d[2] != p.head[1] || d[3] != p.head[2]) return 0;
  int cmd = d[4], track = d[5];
  if (track >= p.voices) return 0;
  if (cmd == p.cmd_note_off) {
    if (len != 7) return 0;
    const uint8_t off[7] = {0xF0, 0x7D, 0x52, 0x54, 0x11, (uint8_t)track, 0xF7};
    memcpy(m, off, 7);
    return 7;
  }
  bool trig = cmd == p.cmd_trig;
  if (!trig && cmd != p.cmd_params) return 0;
  int from = trig ? 8 : 6;
  if (len != from + 2 * p.params + 1) return 0;
  int n = 0;
  m[n++] = 0xF0, m[n++] = 0x7D, m[n++] = 0x52, m[n++] = 0x54;
  m[n++] = trig ? 0x04 : 0x03;
  m[n++] = (uint8_t)(p.machine | track);
  if (trig) m[n++] = d[6], m[n++] = d[7];
  for (int k = 0; k < kMaxParams; ++k) {
    int s = p.map[k];
    m[n++] = s >= 0 ? d[from + 2 * s] : p.fixed[k];
    m[n++] = s >= 0 ? d[from + 2 * s + 1] : 0;
  }
  m[n++] = (p.sound_tag >> 7) & 0x7f;
  m[n++] = p.sound_tag & 0x7f;
  m[n++] = 0xF7;
  return n;
}

// What the sequencer sent the box: its echoes, the mod's report of each note it plays, a chord's as well; and what is held there, so that an all-off is an off for each
static struct { uint8_t note; bool on; uint32_t at; } s_sent[kBoxVoices][kChordNotes];
static uint8_t s_sent_w[kBoxVoices];

static CORE_COLD bool is_echo(int vi, uint8_t note) {
  uint32_t ms = now_ms();
  for (auto& s : s_sent[vi])
    if (s.on && s.note == note && ms - s.at < kEchoMs) return s.on = false, true;
  return false;
}

CORE_COLD void box_note(int ev, int note, int vel) {
  // ev: the engine voice; the box that owns it (its slot's, on 16 voices; the link in hand on 8) and its own voice vi
  Link& l = kVoiceSlots > 1 ? g_links[slot_of_voice(voice_wrap(ev))] : cur();
  const int vi = box_voice(ev);
  const Profile* p = l.box;
  if (!p || !p->note_out || ev < 0 || vi >= p->voices || note > 127) return;
  Scope sc(l);   // keys::forward_note sends to the link in hand
  const uint8_t off = (uint8_t)(0x80 | vi);
  if (note < 0) {   // inst::seq_off's -1: every note of the voice
    for (int n = 0; n < 128; ++n)
      if (s_held[vi][n >> 5] >> (n & 31) & 1) keys::forward_note(off, (uint8_t)n, 0);
    memset(s_held[vi], 0, sizeof s_held[vi]);
    return;
  }
  uint32_t bit = 1u << (note & 31);
  if (vel) {
    auto& s = s_sent[vi][s_sent_w[vi]++ % kChordNotes];
    s.note = (uint8_t)note, s.on = true, s.at = now_ms();
    s_held[vi][note >> 5] |= bit;
    keys::forward_note((uint8_t)(0x90 | vi), (uint8_t)note, (uint8_t)vel);
  } else {
    s_held[vi][note >> 5] &= ~bit;
    keys::forward_note(off, (uint8_t)note, 0);
  }
}

// A box's trig on a voice, with Profile::chords: true when it joined the voice's chord, so the message goes no further. The first
// note is the 0x04 it always was, on the voice's own machine: nothing waits, and a lone trig never comes here for more.
// vi: the engine voice (inst's); the chord state by the box's own voice k (box_voice)
static CORE_COLD bool chord_note(int vi, uint8_t vb, uint8_t note, uint8_t vel) {
#if CORE_POLY
  auto& c = s_chord[box_voice(vi)];
  uint32_t ms = now_ms();
  bool near = c.n && c.vb == vb && ms - c.at < kChordMs;
  if (near)
    for (int k = 0; k < c.n; ++k)
      if (c.notes[k] == note) return c.n > 1;   // a note of the chord twice is one; alone, a retrig as ever
  if (near && c.n < kChordNotes && inst::chord_on(vi, note, vel)) {
    c.notes[c.n++] = note, c.at = ms;
    return true;
  }
  if (near && inst::chord_single(vi)) return c.at = ms, true;   // one hit, as the box's own step: 04s this close are a flam
  if (c.n > 1) inst::chord_off(vi);   // a new note, no off before it: the amp's tail, as a mono retrig
  c.n = 1, c.vb = vb, c.notes[0] = note, c.at = ms;
#else
  (void)vi, (void)vb, (void)note, (void)vel;
#endif
  return false;
}

static CORE_COLD void chord_end(int vi) {
#if CORE_POLY
  if (s_chord[box_voice(vi)].n > 1) inst::chord_off(vi);
#endif
  s_chord[box_voice(vi)].n = 0;
}

CORE_COLD bool translate(Link& l, const uint8_t*& d, int& len, uint8_t* m) {
  if (len < 5 || d[len - 1] != 0xF7) return ++l.dropped, false;
  if (is_sidecar(d, len) && d[4] == kCmdHello) return hello(l, d, len), false;
  if (is_sidecar(d, len) && d[4] == 0x36 && (l.box->features & kRemoteUi) && len <= kMaxMessage) {   // a box's screen says hello: 35 is the Rytm's, and asks for /.rytm
    memcpy(m, d, (size_t)len);
    m[4] = 0x35;
    d = m;
    return true;
  }
  if (is_sidecar(d, len) && d[4] == 0x35) return ++l.dropped, false;   // (a Rytm's: slot_of gave its transport slot 0 first)
  if (is_sidecar(d, len) && d[4] == kCmdControls && l.box->controls) {
    const Controls& c = *l.box->controls;
    if (len != 7 + 2 * c.n || d[5] >= l.box->voices) return ++l.dropped, false;
    strip::report(c, d[5], d + 6);
    return false;
  }
  if (is_sidecar(d, len) && (l.box->in_allow[(d[4] & 0x7f) >> 5] >> (d[4] & 31) & 1)) {
    bool voiced = d[4] == 0x03 || d[4] == 0x04 || d[4] == 0x11;
    if (voiced && (d[5] & 7) >= l.box->voices) return ++l.dropped, false;
    if (d[4] == 0x04 && len > 7 && is_echo(d[5] & 7, d[6])) return false;
    if (l.box->chords && d[4] == 0x04 && len > 7 && chord_note(voice_in(d[5], l), d[5] & 0x78, d[6], d[7])) return false;
    if (l.box->chords && d[4] == 0x11) chord_end(voice_in(d[5], l));
    if (l.box->controls && d[4] == 0x04) strip::trig(d[5] & 7);
    if (l.box->controls && d[4] == 0x11) strip::off(d[5] & 7);
    return true;
  }
  int n = l.box->translate ? l.box->translate(*l.box, d, len, m) : table(*l.box, d, len, m);
  if (n <= 0) return ++l.dropped, false;
  d = m, len = n;
  return true;
}

}  // namespace device

CORE_COLD void engine_sysex_link(int t, const uint8_t* d, int len) {
  if (len >= 8 && d[1] == 0x00 && d[2] == 0x20 && device::take_answer(t < 0 || t >= device::kLinks ? 0 : t, d, len)) return;
  int slot = device::slot_of(t, d, len);
  if (slot < 0) {   // a hello refused (an unknown box or protocol, or no slot free: it waits); anything else goes nowhere
    if (len >= 10 && d[4] == device::kCmdHello) {
      const uint8_t a[10] = {0xF0, 0x7D, 0x52, 0x54, device::kCmdHello, 1, d[6], d[7], device::hello_status(d, len), 0xF7};
      if (auto* out = device::tx(t)) out(a, (int)sizeof a);
    }
    return;
  }
  device::Scope s(device::g_links[slot]);
  host_sysex_slot(d, len);
}

CORE_COLD void engine_sysex_self(const uint8_t* d, int len) {
  device::Scope s(device::g_links[0]);
  host_sysex_slot(d, len);
}

CORE_COLD void engine_sysex_self_slot(int slot, const uint8_t* d, int len) {   // the load test's notes (the Pi's own, slot's box)
  if (slot < 0 || slot >= device::kLinks) return;
  device::Scope s(device::g_links[slot]);
  host_sysex_slot(d, len);
}

}  // namespace core
