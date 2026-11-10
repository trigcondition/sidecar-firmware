// core/fx/send_fx_card.cpp - send_fx.h's card files: /.rytm/sendfx.txt (global: the route) and
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

static fmt::KvKeep g_keep;   // the lines a newer build wrote that this one does not read (fmt.h, class S: kept)
CORE_COLD void card_load() {
  File f;
  control.route = kRouteSlot;
  g_keep.n = 0;
  fmt::post_wait(kGlobal);   // 0.3.0: a posted save of it lands before we read it back (fmt.h)
  fmt::recover(kGlobal);   // a power cut between write_atomic's renames: the previous file back, a stray .tmp gone
  if (!f.open(kGlobal, kRead)) return;
  char b[256];
  static const char* const kKnown[] = {"out", nullptr};
  while (line(f, b, sizeof b)) {
    if (strcmp(b, "out=main") == 0) control.route = kRouteMain;
    else if (strcmp(b, "out=fx") == 0) control.route = kRouteFx;
    else if (strncmp(b, "out=", 4)) {   // (one line at a time into the kept lines)
      fmt::KvKeep one;
      if (fmt::kv_keep(b, (uint32_t)strlen(b), kKnown, &one) && g_keep.n + one.n < sizeof g_keep.text)
        memcpy(g_keep.text + g_keep.n, one.text, one.n + 1), g_keep.n += one.n;
    }
  }
  f.close();
  log(CORE_STR("sendfx: out %s"), control.route == kRouteMain ? "forced main" : control.route == kRouteFx ? "forced fx" : "per slot (P1)");
}

// 0.3.0 power-cut safety: posted to the card core as one atomic write (fmt.h), a newer build's keys kept (class S)
CORE_COLD void card_save() {
  char* s = fmt::post_begin();
  if (!s) return;   // another settings write is in flight: at a later pass (card_dirty still set)
  uint32_t n = (uint32_t)sniprintf(s, 16, "%s", control.route == kRouteMain ? "out=main\n" : control.route == kRouteFx ? "out=fx\n" : "out=slot\n");
  n = fmt::kv_append(g_keep, s, n, fmt::kPostMax);
  if (fmt::post_end(kGlobal, n)) card_dirty = false;
}

static CORE_COLD int hexv(char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The map's text into user[]. n bytes of g_stext
static char g_stext[CORE_MSAMP ? 4096 : 512];   // (formats.cpp: 4 KB at most; only the first lines are read)
static CORE_COLD void slots_parse(uint32_t n) {
  uint32_t at = 0;
  char b[64];
  for (int u = 0; u < kUserSlots && at < n; ++u) {
    int k = 0;
    while (at < n) {
      char c = g_stext[at++];
      if (c == '\n') break;
      if (c != '\r' && k < (int)sizeof b - 1) b[k++] = c;
    }
    b[k] = 0;
    int t = 0, i = 0;
    for (; i < 4 && hexv(b[i]) >= 0; ++i) t = t << 4 | hexv(b[i]);
    if (i != 4 || b[4] != ' ' || (t & 0x3FFF) == 0) continue;
    if (user_loaded && user_loaded(u)) continue;   // (a loaded effect's own name stays)
    user[u].tag = (uint16_t)(t & 0x3FFF);
    strncpy(user[u].name, b + 5, sizeof user[u].name - 1);
    user[u].name[sizeof user[u].name - 1] = 0;
  }
}

static char g_spath[72];
static CORE_COLD void slots_name() {
  char safe[48];
  pool_file(safe, sizeof safe);   // "/.rytm/pools/<safe>.txt": the same safe name
  sniprintf(g_spath, sizeof g_spath, "/.rytm/fxslots/%s", safe + 13);
}

#if CORE_MSAMP
// A project change's read on the card core (core/msamp/cardq.h), never here: the 0x10 comes on the control core, and an open behind core 0's
// read or the recorder's write would hold it for that call, a card's pause too (100-137 ms measured). Another project while one is read: read again.
static cardq::Req g_sreq;
static bool g_sbusy, g_sagain;

CORE_COLD void slots_load(const char* project) {
  (void)project;
  // (a user effect loaded in its slot keeps its name and tag: the project's file only names a slot, the loaded effect
  // is what plays - Plugin API 3, core/user/um.cpp)
  for (int u = 0; u < kUserSlots; ++u)
    if (!(user_loaded && user_loaded(u))) user[u].tag = 0, user[u].name[0] = 0;
  if (g_sbusy) {
    g_sagain = true;
    return;
  }
  slots_name();
  g_sbusy = true;
}

CORE_COLD bool slots_step() {
  while (g_sbusy) {
    if (!cardq::async(&g_sreq, cardq::kSmallReq, nullptr, g_spath, 0, (uint8_t*)g_stext, sizeof g_stext)) return false;
    if (g_sagain) {
      g_sagain = false;
      slots_name();
      continue;
    }
    g_sbusy = false;
    if (g_sreq.got > 0) slots_parse((uint32_t)g_sreq.got);
  }
  return true;
}
#else
CORE_COLD void slots_load(const char* project) {
  (void)project;
  // (a user effect loaded in its slot keeps its name and tag: the project's file only names a slot, the loaded effect
  // is what plays - Plugin API 3, core/user/um.cpp)
  for (int u = 0; u < kUserSlots; ++u)
    if (!(user_loaded && user_loaded(u))) user[u].tag = 0, user[u].name[0] = 0;
  slots_name();
  File f;
  if (!f.open(g_spath, kRead)) return;
  int c;
  uint32_t n = 0;
  while (n < sizeof g_stext && (c = f.read()) >= 0) g_stext[n++] = (char)c;
  f.close();
  slots_parse(n);
}
CORE_COLD bool slots_step() { return true; }
#endif

}  // namespace fx
}  // namespace core
