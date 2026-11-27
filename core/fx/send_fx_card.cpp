// core/fx/send_fx_card.cpp - send_fx.h's card files: /.rytm/sendfx.txt (each box's route, in its own folder) and
// /.rytm/fxslots/<project>.txt (the project's user-FX slot map). Cold: boot, a project change, the idle step.
#include "fmt/fmt.h"
#include "fx/send_fx.h"
#include "msamp/cardq.h"
#include "pool/pool.h"

namespace core {
namespace fx {

static const char kGlobal[] = "/.rytm/sendfx.txt";

// one line (without the '\n') from f; false at the end
static CORE_COLD bool line(File& f, char* out, int max) {
  int n = 0, c;
  bool any = false;
  while ((c = f.read()) >= 0) {
    any = true;
    if (c == '\n') break;
    if (c != '\r' && n < max - 1) out[n++] = (char)c;
  }
  out[n] = 0;
  return any;
}

static fmt::KvKeep g_keep[kVoiceSlots];   // each slot's lines a newer build wrote that this one does not read (class S: kept)

// a line of sendfx.txt into the slot in hand's route (or its kept lines)
static CORE_COLD void take(const char* b) {
  static const char* const kKnown[] = {"out", nullptr};
  fmt::KvKeep& keep = g_keep[slot_in_hand()];
  if (strcmp(b, "out=main") == 0) cur().route = kRouteMain;
  else if (strcmp(b, "out=fx") == 0) cur().route = kRouteFx;
  else if (strncmp(b, "out=", 4)) {   // (one line at a time into the kept lines)
    fmt::KvKeep one;
    if (fmt::kv_keep(b, (uint32_t)strlen(b), kKnown, &one) && keep.n + one.n < sizeof keep.text)
      memcpy(keep.text + keep.n, one.text, one.n + 1), keep.n += one.n;
  }
}
static CORE_COLD void said() {
  const int r = cur().route;
  log(CORE_STR("sendfx: slot %d: out %s"), slot_in_hand(), r == kRouteMain ? "forced main" : r == kRouteFx ? "forced fx" : "per slot (P1)");
}

CORE_COLD void card_load() {
  File f;
  cur().route = kRouteSlot;
  g_keep[slot_in_hand()].n = 0;
  fmt::post_wait(kGlobal);   // 0.3.0: a posted save of it lands before we read it back (fmt.h)
  fmt::recover(kGlobal);   // a power cut between write_atomic's renames: the previous file back, a stray .tmp gone
  if (!f.open(kGlobal, kRead)) return;
  char b[256];
  while (line(f, b, sizeof b)) take(b);
  f.close();
  said();
}

// 3(c) step 8: another slot's file, read by the card core at its attach (core/pool/pool.cpp), taken in its Scope
CORE_COLD void card_text(const char* t, int n) {
  cur().route = kRouteSlot;
  g_keep[slot_in_hand()].n = 0;
  char b[256];
  for (int i = 0; i < n;) {
    int k = 0;
    while (i < n && t[i] != '\n') {
      if (t[i] != '\r' && k < (int)sizeof b - 1) b[k++] = t[i];
      ++i;
    }
    ++i;
    b[k] = 0;
    take(b);
  }
  said();
}

// 0.3.0 power-cut safety: posted to the card core as one atomic write (fmt.h), a newer build's keys kept (class S)
CORE_COLD void card_save() {
  char* s = fmt::post_begin();
  if (!s) return;   // another settings write is in flight: at a later pass (card_dirty still set)
  const int r = cur().route, sl = slot_in_hand();
  uint32_t n = (uint32_t)sniprintf(s, 16, "%s", r == kRouteMain ? "out=main\n" : r == kRouteFx ? "out=fx\n" : "out=slot\n");
  n = fmt::kv_append(g_keep[sl], s, n, fmt::kPostMax);
  if (fmt::post_end(kGlobal, n)) card_dirty = (uint8_t)(card_dirty & ~(1u << sl));
}

static CORE_COLD int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The map's text txt[0..n) into box's user names (box 0: user[], 1: user1[])
static char g_stext[kVoiceSlots][CORE_MSAMP ? 4096 : 512];   // (formats.cpp: 4 KB at most; only the first lines are read)
static CORE_COLD void slots_parse(int box, const char* txt, uint32_t n) {
  UserFx* user = user_of(box);
  uint32_t at = 0;
  char b[64];
  for (int u = 0; u < kUserSlots && at < n; ++u) {
    int k = 0;
    while (at < n) {
      char c = txt[at++];
      if (c == '\n') break;
      if (c != '\r' && k < (int)sizeof b - 1) b[k++] = c;
    }
    b[k] = 0;
    if (char* tab = strchr(b, '\t')) *tab = 0;   // (um.cpp adds "\t<id> <version>" to the line: the name ends at the tab)
    int t = 0, i = 0;
    for (; i < 4 && hexv(b[i]) >= 0; ++i) t = t << 4 | hexv(b[i]);
    if (i != 4 || b[4] != ' ' || (t & 0x3FFF) == 0) continue;
    if (user_loaded && user_loaded(box * kUserSlots + u)) continue;   // (a loaded effect's own name stays)
    user[u].tag = (uint16_t)(t & 0x3FFF);
    strncpy(user[u].name, b + 5, sizeof user[u].name - 1);
    user[u].name[sizeof user[u].name - 1] = 0;
  }
}

// a box's names start over: each slot empty but a loaded effect's (its own name and tag stay: the project's file only
// names a slot, the loaded effect is what plays - Plugin API 3, core/user/um.cpp)
static CORE_COLD void slots_clear(int box) {
  UserFx* user = user_of(box);
  for (int u = 0; u < kUserSlots; ++u)
    if (!(user_loaded && user_loaded(box * kUserSlots + u))) user[u].tag = 0, user[u].name[0] = 0;
}

// 3(c): another box's map (core/user/um.cpp reads it with its own slot map, from the box's folder): its names
CORE_COLD void slots_text(int box, const char* t, int n) {
  if (box < 0 || box >= kVoiceSlots) return;
  slots_clear(box);
  if (n > 0) slots_parse(box, t, (uint32_t)n);
}

// Each box's own map (two links, decided 2026-11-26: slot 0 and slot 1 the same): the slot in hand's project, read in its
// folder (the card core's request carries the link it was queued in), into that slot's user names. The Rytm's 0x10 comes in
// its own slot's Scope, whichever slot it holds.
static char g_spath[kVoiceSlots][72];
static CORE_COLD void slots_name(int box) {
  char safe[48];
  pool_file(safe, sizeof safe);   // "/.rytm/pools/<safe>.txt": the same safe name
  sniprintf(g_spath[box], sizeof g_spath[box], "/.rytm/fxslots/%s", safe + 13);
}

#if CORE_MSAMP
// A project change's read on the card core (core/msamp/cardq.h), never here: the 0x10 comes on the control core, and an open behind core 0's
// read or the recorder's write would hold it for that call, a card's pause too (100-137 ms measured). Another project while one is read: read again.
static cardq::Req g_sreq[kVoiceSlots];
static bool g_sbusy[kVoiceSlots], g_sagain[kVoiceSlots];

CORE_COLD void slots_load(const char* project) {
  (void)project;
  const int box = slot_in_hand();
  slots_clear(box);   // (the slot in hand's)
  if (g_sbusy[box]) {
    g_sagain[box] = true;
    return;
  }
  slots_name(box);
  g_sbusy[box] = true;
}

CORE_COLD bool slots_step() {
  const int box = slot_in_hand();
  while (g_sbusy[box]) {
    if (!cardq::async(&g_sreq[box], cardq::kSmallReq, nullptr, g_spath[box], 0, (uint8_t*)g_stext[box], sizeof g_stext[box]))
      return false;
    if (g_sagain[box]) {
      g_sagain[box] = false;
      slots_name(box);
      continue;
    }
    g_sbusy[box] = false;
    if (g_sreq[box].got > 0) slots_parse(box, g_stext[box], (uint32_t)g_sreq[box].got);
  }
  return true;
}
#else
CORE_COLD void slots_load(const char* project) {
  (void)project;
  const int box = slot_in_hand();
  slots_clear(box);   // (the slot in hand's)
  slots_name(box);
  File f;
  if (!f.open(g_spath[box], kRead)) return;
  int c;
  uint32_t n = 0;
  while (n < sizeof g_stext[box] && (c = f.read()) >= 0) g_stext[box][n++] = (char)c;
  f.close();
  slots_parse(box, g_stext[box], n);
}
CORE_COLD bool slots_step() { return true; }
#endif

}  // namespace fx
}  // namespace core
