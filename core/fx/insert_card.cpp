// core/fx/insert_card.cpp - see insert.h: the INSERT settings on the Sidecar's card, per kit as the LFOs (core/lfo.h):
// /.rytm/ins/<project>/kNNN.txt for kit NNN (1..128), /.rytm/ins/<project>.txt the project's default (and what is used
// while the Rytm has not named a kit). One line per slot with a TYPE ("V1 A FILTER 80 20 0 0"), per voice with sends
// ("SEND V1 0 0 90"), "ROUTE B3 POST" when bus 3 returns POST. The card half of insert.cpp (as send_fx_card.cpp is
// send_fx.cpp's), so the DSP and the values build without the card (pi/engine/tests).
#include <string.h>

#include "fmt/fmt.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "platform.h"
#include "pool/pool.h"
#if CORE_MSAMP
#include "bundle/rename.h"
#include "store/store.h"
#endif

namespace core {
namespace ins {

namespace {

char g_proj[24] = "";
int g_kit = -1, g_rytm_kit = -1;

bool same_ci(const char* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
    if (x != y) return false;
  }
  return *a == *b;
}
int num(const char* p) {
  if (*p < '0' || *p > '9') return -1;
  int v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
  return v;
}

void name_of(const char* proj, char* fn, int cap, int kit) {
  char n[kKeyMax];
  project_key(proj, n, sizeof n);
  if (kit < 0) sniprintf(fn, cap, CORE_STR("/.rytm/ins/%s.txt"), n);
  else if (kit < 128) sniprintf(fn, cap, CORE_STR("/.rytm/ins/%s/k%03d.txt"), n, kit + 1);
  else sniprintf(fn, cap, CORE_STR("/.rytm/ins/%s"), n);
}
void file_name(char* fn, int cap, int kit) { name_of(g_proj, fn, cap, kit); }
// the line's words (at most 7) into w
int words(const char* b, char (*w)[12], int max) {
  int n = 0;
  const char* p = b;
  while (n < max) {
    while (*p == ' ') ++p;
    if (!*p) break;
    int k = 0;
    while (*p && *p != ' ') {
      if (k < 11) w[n][k++] = *p;
      ++p;
    }
    w[n++][k] = 0;
  }
  return n;
}
bool from_line(const char* b) {
  char w[7][12];
  int n = words(b, w, 7);
  if (n >= 3 && same_ci(w[0], "OUT")) {   // OUT V3 POST 100 64 64 0 (core/fx/out.h)
    const char* wp[7];
    for (int i = 0; i < n; ++i) wp[i] = w[i];
    return out::from_line(wp, n);
  }
  if (n >= 5 && same_ci(w[0], "SEND")) {
    int c = chain_from_name(w[1]);
    if (c < 0 || c >= kVoices) return false;
    for (int bus = 0; bus < kBuses; ++bus) {
      int v = num(w[2 + bus]);
      if (v < 0) return false;
      set_send(c, bus, v);
    }
    return true;
  }
  if (n >= 4 && same_ci(w[0], "AMP")) {   // AMP V1 A Plexi Crunch.namb (the rest of the line: the file, spaces kept)
    int c = chain_from_name(w[1]);
    int s = same_ci(w[2], "A") ? 0 : same_ci(w[2], "B") ? 1 : -1;
    if (c < 0 || c >= kVoices || s < 0) return false;
    const char* p = b;
    for (int k = 0; k < 3; ++k) {
      while (*p == ' ') ++p;
      while (*p && *p != ' ') ++p;
    }
    while (*p == ' ') ++p;
    amp::want_file(c, s, p);
    return true;
  }
  if (n >= 3 && same_ci(w[0], "ROUTE")) {
    set_route3(same_ci(w[2], "POST"));
    return true;
  }
  if (n >= 3 && same_ci(w[0], "PEQ")) {   // ins1112: PEQ V1 40 0 64 70 (HPF LPF LOW HIGH; a missing one stays flat)
    int c = chain_from_name(w[1]);
    if (c < 0) return false;
    for (int k = 0; k < kEqParams; ++k) {
      int v = 2 + k < n ? num(w[2 + k]) : -1;
      set_eq(c, k, v >= 0 ? v : kEqFlat(k));
    }
    return true;
  }
  if (n < 3) return false;
  int c = chain_from_name(w[0]);
  int s = same_ci(w[1], "A") ? 0 : same_ci(w[1], "B") ? 1 : -1;
  int t = type_from_name(w[2]);
  if (c < 0 || s < 0 || t < 0) return false;
  set_type(c, s, t);   // its defaults, then the line's params (a missing one keeps its default)
  for (int k = 0; k < kParams && 3 + k < n; ++k) {
    int v = num(w[3 + k]);
    if (v >= 0) set_param(c, s, k, v);
  }
  return true;
}

}  // namespace

void card_forget() { g_kit = g_rytm_kit = -1, set_kit(-1); }

// 1042 (data safety): the file save() writes (the kit's) had a line this build does not read, or is there but could not be
// opened: never written over (before: the next settings edit rewrote it without those lines)
bool g_locked = false;

#if CORE_MSAMP
void rekey(const char* name) { sniprintf(g_proj, sizeof g_proj, "%s", name ? name : ""); }
void want_files() {
  char fn[80];
  for (int k = -1; k < 128; ++k) name_of(project, fn, sizeof fn, k), store::want(fn);
}
#endif

void load() {
  g_locked = false;
  clear();
  strncpy(g_proj, project, sizeof g_proj - 1);
  g_proj[sizeof g_proj - 1] = 0;
  g_kit = g_rytm_kit;
  set_kit(g_kit);
  char fn[80], b[96];
  int n = 0, bad = 0;
  auto take = [&](int k) {
    if (k == 0 || b[0] == '#') return;   // a blank line or a comment (0.3.0: a NUL-led line is neither)
    if (from_line(b)) ++n;
    else ++bad;
  };
  auto done = [&](bool own) {
    if (bad && own) g_locked = true;
    log(CORE_STR("ins: %d lines from %s%s"), n, fn, bad ? (own ? " (lines it does not read: the file is kept, not written)" : " (lines it does not read)") : "");
  };
#if CORE_MSAMP
  if (card_ok) {   // 2026-10-26: the RAM store (a project load fills it before this runs): no card call on this core
    const uint8_t* d;
    uint32_t len, at = 0;
    bool own = true;   // the file read is the one save() writes
    file_name(fn, sizeof fn, g_kit);
    store::State st = store::get(fn, &d, &len);
    if (st == store::kAbsent && g_kit >= 0) {   // a kit without its own file: the project's
      file_name(fn, sizeof fn, -1), own = false;
      st = store::get(fn, &d, &len);
    }
    if (st == store::kPresent) {
      for (int k; (k = fmt::next_line(d, len, &at, b, sizeof b)) >= 0;) take(k);
      done(own);
    } else if (st != store::kAbsent) {   // there but unreadable (kLocked), or never filled (kUnknown): never absent
      // (as the card path: only the kit's own file is never written over; the project's, the default, is not save()'s)
      if (own) g_locked = true;
      log(CORE_STR("ins: %s %s: all off%s"), fn, st == store::kLocked ? "cannot be read" : "not in the store",
          own ? ", nothing written" : "");
    }
  }
#else
  if (card_ok) {
    File f;
    file_name(fn, sizeof fn, g_kit);
    fmt::post_wait(fn);   // 0.3.0: a posted (or queued) save of this very file lands first (another kit's is not in the way)
    fmt::recover(fn);     // a power cut between write_atomic's renames: the previous file back, a stray .tmp removed
    bool open = f.open(fn, kRead), own = open;   // own: the file read is the one save() writes
    if (!open && cards.sd().exists(fn)) g_locked = true;
    if (!open && g_kit >= 0)
      file_name(fn, sizeof fn, -1), fmt::post_wait(fn), fmt::recover(fn), open = f.open(fn, kRead);
    if (open) {
      fmt::LineReader rd(f);   // 2026-10-26: buffered, not a card read a byte (40 ms on core 3 at a kit change)
      for (int k; (k = rd.line(b, sizeof b)) >= 0;) take(k);
      f.close();
      done(own);
    }
  }
#endif
  settings_clean();
}

// 0.3.0 power-cut safety: as the LFOs' save, posted to the card core as one atomic write (fmt.h), so a cut leaves the
// kit's chains whole - old or new - and the control core does no card I/O for it (write_atomic makes the folders).
//
// 2026-10-26, the RAM store: the text is also put into the store (write-through), so a kit change back reads the edit
// from RAM before the card has it. queue (a kit change's save): no wait for the post slot - fmt::post_queue holds a copy
// behind the write in flight.
#if CORE_MSAMP
static char g_text[fmt::kPostMax];   // the text built (the post slot may be busy: post_queue copies it)
#endif
static bool save_text(bool queue) {
  uint32_t since;
  if (!settings_dirty(&since) || now_ms() - since < 1000) return false;
  if (!card_ok) return settings_clean(), false;
  if (g_locked) {   // 1042
    settings_clean();
    log(CORE_STR("ins: not saved: its file has lines this build does not read (kept as it is)"));
    return true;
  }
#if CORE_MSAMP
  char* t = g_text;
#else
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
#endif
  char fn[80], b[96];
  file_name(fn, sizeof fn, g_kit);
  static const char kHead[] = "# Sidecar inserts: CHAIN SLOT TYPE P1 P2 P3 P4 / PEQ CHAIN HPF LPF LOW HIGH / AMP CHAIN SLOT FILE / SEND Vn B1 B2 B3 / ROUTE B3 PRE|POST / OUT Vn TRK|PRE|POST LEV PAN WID DEC\n";
  uint32_t len = sizeof kHead - 1;
  memcpy(t, kHead, len);
  bool fits = true;
  auto add = [&](const char* s, int n) {   // never half a line: a text that does not fit leaves the file as it was
    if (n <= 0 || len + (uint32_t)n > fmt::kPostMax) return (void)(fits = false);
    memcpy(t + len, s, (size_t)n), len += (uint32_t)n;
  };
  for (int c = 0; c < kChains; ++c)
    for (int s = 0; s < kSlots; ++s) {
      int ty = control.type[c][s];
      if (!ty) continue;
      add(b, sniprintf(b, sizeof b, CORE_STR("%s %c %s %d %d %d %d\n"), chain_name(c), 'A' + s, type_name(ty),
                       param(c, s, 0), param(c, s, 1), param(c, s, 2), param(c, s, 3)));
      const char* af = ty == kAmp ? amp::slot_file(c, s) : "";
      if (af[0]) {   // AMP's capture by name: the knob's position follows /NAM/'s list when files come and go
        int n = sniprintf(b, sizeof b, CORE_STR("AMP %s %c %s\n"), chain_name(c), 'A' + s, af);
        if (n > 0 && n < (int)sizeof b) add(b, n);
      }
    }
  for (int c = 0; c < kChains; ++c) {   // ins1112: only a chain whose post EQ is off its flat defaults
    if (!eq_on(c)) continue;
    int n = sniprintf(b, sizeof b, CORE_STR("PEQ %s %d %d %d %d\n"), chain_name(c), eq_param(c, 0), eq_param(c, 1),
                      eq_param(c, 2), eq_param(c, 3));
    add(b, n);
  }
  for (int v = 0; v < kVoices; ++v) {
    if (!control.send[v][0] && !control.send[v][1] && !control.send[v][2]) continue;
    add(b, sniprintf(b, sizeof b, CORE_STR("SEND %s %d %d %d\n"), chain_name(v), control.send[v][0],
                     control.send[v][1], control.send[v][2]));
  }
  if (control.route3) {
    static const char kR[] = "ROUTE B3 POST\n";
    add(kR, (int)sizeof kR - 1);
  }
  for (int v = 0; v < kVoices; ++v) {
    int n = out::line(v, b, sizeof b);
    if (n > 0) add(b, n);
  }
  if (!fits) {
    settings_clean();
    log(CORE_STR("ins: %s not written: its text does not fit (kept as it is)"), fn);
    return true;
  }
#if CORE_MSAMP
  // the store is the project's now named (filled, or filling: the fill keeps this newer text); an old project's file
  // saved while the next one's store fills is not put in it
  if (!strcmp(g_proj, project)) store::put(fn, (const uint8_t*)t, len);
  // (a held slot, phase 3: queued too, so the edit is on its way rather than dirty for the whole hold)
  if (queue || fmt::post_stalled()) {
    if (!fmt::post_queue(fn, t, len)) log(CORE_STR("ins: %s not written to the card (refused or out of memory: in RAM only)"), fn);
  } else {
    char* p = fmt::post_begin();
    if (!p) return false;   // another settings write is in flight: at a later pass (still dirty)
    memcpy(p, t, len);
    if (!fmt::post_end(fn, len)) return false;   // not posted: still dirty, again at a later pass
  }
#else
  if (!fmt::post_end(fn, len)) return false;   // not posted: still dirty, again at a later pass
#endif
  settings_clean();
  return true;
}
bool save() { return save_text(false); }

void project_changed() {
  // the old kit's unsaved edits first, into its own file - never deferred past the load that clears the dirty flag
#if CORE_MSAMP
  // 2026-10-26: its write posted or queued (no wait on the card), its text into the store when the store is its
  if (settings_dirty(nullptr)) settings_due(), save_text(true);
#else
  if (settings_dirty(nullptr)) fmt::post_wait(), settings_due(), save();   // post_wait frees the slot for this save
#endif
  load();
}

void kit_changed(int kit) {
  if (kit < 0 || kit > 127) return;
  g_rytm_kit = kit;
#if CORE_MSAMP
  // a project load's store still filling: its load (project_fill_step) takes this kit. Likewise while a rename moves
  // the old key's files (final review): a save now would queue a write under the old name that lands after the
  // move (idle_save holds the writes meanwhile) - a stray file, the edit missing from the renamed project; the
  // switch that follows the move loads this kit, its project_changed saving the old kit's edits under the new name
  if (!store::ready() || rename::moving()) return;
#endif
  if (kit != g_kit) project_changed();
}

}  // namespace ins
}  // namespace core
