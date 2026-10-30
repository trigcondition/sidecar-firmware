// core/user/lists.cpp - see lists.h. Control core only, but json_copy (any core).
#include "lists.h"

#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "pool/pool.h"
#if CORE_MSAMP
#include "lib/pools.h"
#endif
#include "sysex.h"
#include "voices.h"

extern "C" void* um_heap(uint32_t bytes);   // the board's heap at boot (core/user/um.cpp's weak one, the Pi's)

namespace core {
int juno_pick(int pat, const uint8_t** b, const char** name);   // voices.cpp: the patch PAT picks
}

namespace lists {
namespace {

constexpr int kJson = 49152;            // a full pool of long paths and both lists fit
constexpr uint32_t kEveryMs = 250;
const int kJunoByte[6] = {5, 6, 7, 11, 12, 14};   // CUT RES ENV ATK DEC REL (voices.cpp's)

char* g_js[2] = {nullptr, nullptr};
int g_jlen[2];
volatile uint32_t g_jseq = 0;
uint32_t g_hash = 0, g_ms = 0;

struct Js {
  char* b;
  int cap, n;
  void put(const char* s) { while (*s && n < cap - 1) b[n++] = *s++; b[n] = 0; }
  void str(const char* s, int max = 1 << 30) {   // a JSON string, printable ASCII
    put("\"");
    for (int i = 0; s[i] && i < max && n < cap - 8; ++i) {
      char c = s[i];
      if (c == '"' || c == '\\') { b[n++] = '\\'; b[n++] = c; }
      else if (c >= 0x20 && c <= 0x7e) b[n++] = c;
      else b[n++] = '_';
    }
    b[n] = 0;
    put("\"");
  }
  void num(const char* k, long v) { char t[40]; sniprintf(t, sizeof t, "\"%s\":%ld", k, v); put(t); }
};

const char* machine(const core::V& x, char* b) {
  if (x.user >= 0) { sniprintf(b, 4, "U%d", x.user + 1); return b; }
  return x.fm ? "FM" : x.juno ? "JUNO" : x.dsps ? "DSPS" : x.daisy ? "DAISY" : x.md ? "MD" : "PLAITS";
}

uint32_t fnv(uint32_t h, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 16777619u;
  return h;
}
// What the JSON says, as a number: it is published again when this moves.
uint32_t state_hash() {
  uint32_t h = fnv(2166136261u, core::project, strlen(core::project));
  h = fnv(h, &core::npool, sizeof core::npool);
  for (int i = 0; i < core::npool && i < core::kPool; ++i) {   // (rui1045: bank A, the slots this JSON has always meant)
    h = fnv(h, core::pool[i].path, strlen(core::pool[i].path));
    h = fnv(h, &core::pool[i].folder, sizeof core::pool[i].folder);
  }
  int nj = core::junolib.listed(), nf = core::dxlib.listed();
  h = fnv(h, &nj, sizeof nj);
  for (int i = 0; i < nj; ++i) h = fnv(h, &core::junolib.slot(i), sizeof(jlib::Patch));
  h = fnv(h, &nf, sizeof nf);
  for (int i = 0; i < nf; ++i) h = fnv(h, &core::dxlib.slot(i), sizeof(dx7::Slot));
  for (int vi = 0; vi < core::kVoices; ++vi) {
    const core::V& x = core::v[vi];
    int m[5] = {x.user, x.fm, x.juno, x.dsps * 2 + x.daisy, x.md};
    h = fnv(h, m, sizeof m);
    h = fnv(h, &core::juno_pat[vi], sizeof(int));
    h = fnv(h, &core::fm_pat[vi], sizeof(int));
  }
  return h;
}

void publish() {
  if (!g_js[0]) {
    g_js[0] = (char*)um_heap(kJson);
    g_js[1] = (char*)um_heap(kJson);
    if (!g_js[0] || !g_js[1]) return;
  }
  uint32_t seq = g_jseq;
  int w = (int)((seq / 2 + 1) & 1);
  Js j{g_js[w], kJson, 0};
  j.put("{\"project\":");
  j.str(core::project[0] ? core::project : "_default");
  j.put(",\"card\":");
  j.put(core::card_ok ? "true" : "false");
  char t[64];
  sniprintf(t, sizeof t, ",\"sram\":[%u,%u]", (unsigned)(core::psram ? core::psram->arena_bytes() : 0),
            (unsigned)(core::psram ? core::psram->used_bytes() : 0));
  j.put(t);
  j.put(",\"pool\":[");
  bool first = true;
  for (int i = 0; i < core::npool && i < core::kPool; ++i) {   // rui1045: bank A only (the page's slots 1..127; kJson's size)
    const core::PoolEntry& e = core::pool[i];
    if (!e.path[0]) continue;
    j.put(first ? "{" : ",{");
    first = false;
    j.num("slot", i + 1);
    j.put(",\"path\":"); j.str(e.path, core::kPath);
    j.put(",\"name\":"); j.str(e.name, card::kName);
    j.put(",\"ok\":"); j.put(e.folder >= 0 ? "true" : "false");
    j.put("}");
  }
  j.put("],"); j.num("npool", core::npool < core::kPool ? core::npool : core::kPool);
  int nj = core::junolib.listed();
  j.put(",\"juno\":{"); j.num("total", nj); j.put(",\"list\":[");
  for (int i = 0; i < nj; ++i) {
    const jlib::Patch& p = core::junolib.slot(i);
    j.put(i ? ",{" : "{"); j.num("slot", i);
    j.put(",\"name\":"); j.str(p.used ? p.name : "", sizeof p.name);
    j.put(",\"used\":"); j.put(p.used ? "true" : "false"); j.put("}");
  }
  int nf = core::dxlib.listed();
  j.put("]},\"fm\":{"); j.num("total", nf ? nf : dx7::kRomVoices);
  j.put(",\"rom\":"); j.put(nf ? "false" : "true"); j.put(",\"list\":[");
  for (int i = 0; i < nf; ++i) {
    const dx7::Slot& s = core::dxlib.slot(i);
    j.put(i ? ",{" : "{"); j.num("slot", i);
    j.put(",\"name\":"); j.str(s.used ? s.name : "", sizeof s.name);
    j.put(",\"used\":"); j.put(s.used ? "true" : "false");
    j.put(",\"found\":"); j.put(s.used && s.bank != -1 ? "true" : "false"); j.put("}");
  }
  j.put("]},\"voices\":[");
  for (int vi = 0; vi < core::kVoices; ++vi) {
    const core::V& x = core::v[vi];
    char mb[4];
    j.put(vi ? ",{" : "{"); j.num("v", vi);
    j.put(",\"m\":"); j.str(machine(x, mb));
    j.put(","); j.num("pat", x.juno ? core::juno_pat[vi] : x.fm ? core::fm_pat[vi] : -1);
    j.put("}");
  }
  j.put("]}");
  g_jlen[w] = j.n;
  __atomic_store_n(&g_jseq, seq + 2, __ATOMIC_RELEASE);   // the reader takes buffer (seq/2) & 1
}

int num(const char* p, const char** end) {
  if (*p < '0' || *p > '9') { *end = p; return -1; }
  int v = 0;
  while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (*p++ - '0');
  *end = p;
  return v;
}

#if CORE_MSAMP
// the pools design 3.6: slot N (1..127) is the SAMPLES pool's A00N (the old pool=<slot>:<path> kept for the page; rui1045:
// DSP SAMPLE's SMP reaches every bank, slot = bank * 127 + row, this line bank A)
bool cmd_pool(const char* a, char* out, int cap) {
  const char* e;
  int slot = num(a, &e);
  if (slot < 1 || slot > core::kPool || *e != ':') { sniprintf(out, cap, "usage: pool=<1..127>:<path under /Samples> | pool=<slot>:"); return true; }
  const char* path = e + 1;
  int r;
  if (!path[0] || !strcmp(path, "-")) {
    r = core::pools::unload(core::pools::kSamples, core::pools::code(0, slot), core::pools::kForced);
    sniprintf(out, cap, "pool slot %d (A%03d) empty: %s", slot, slot, core::pools::result_text(r));
  } else {
    char full[200];
    sniprintf(full, sizeof full, "/Samples/%s", path);
    int o = -1;
    r = core::pools::set_file(core::pools::kSamples, core::pools::code(0, slot), full, &o, core::pools::kForced);
    sniprintf(out, cap, "pool slot %d (A%03d) = %s: %s", slot, slot, path, core::pools::result_text(r));
  }
  core::pool_sync(true);
  return true;
}
#else
bool pool_clear(int slot) {
  int line = slot - 1;
  if (line < 0 || line >= core::kPool) return false;
  if (line < core::npool) {
    core::pool_forget(line);
    core::pool[line].path[0] = 0;
    core::pool[line].name[0] = 0;
    core::pool[line].folder = -1;
    while (core::npool && !core::pool[core::npool - 1].path[0]) --core::npool;
    core::pool_save();
    for (int i = 0; i < core::kVoices; ++i) core::prepared[i][0] = -2;   // voices: prepare again
    if (core::hooks.pool_changed) core::hooks.pool_changed(line);
    core::log("pool %s: slot %d emptied", core::project[0] ? core::project : "_default", slot);
  }
  core::send_pool();
  return true;
}

bool cmd_pool(const char* a, char* out, int cap) {
  const char* e;
  int slot = num(a, &e);
  if (slot < 1 || slot > core::kPool || *e != ':') { sniprintf(out, cap, "usage: pool=<1..127>:<path under /Samples> | pool=<slot>:"); return true; }
  const char* path = e + 1;
  if (!path[0] || !strcmp(path, "-")) {
    pool_clear(slot);
    sniprintf(out, cap, "pool slot %d empty", slot);
    return true;
  }
  int f, i;
  if (!core::card_ok || !core::find_path(path, &f, &i)) { sniprintf(out, cap, "pool slot %d = %s: not on the card", slot, path); return true; }
  core::pool_assign(slot, f, i);
  sniprintf(out, cap, "pool slot %d = %s: ok", slot, path);
  return true;
}

#endif

// juno=... / fm=...: the list's edit, then the names to the Rytm and the voices on their PAT again.
bool cmd_list(bool juno, const char* a, char* out, int cap) {
  const char* what = juno ? "juno" : "fm";
  const char* e;
  bool ok = false;
  if (!strncmp(a, "del:", 4)) {
    int s = num(a + 4, &e);
    ok = *e == 0 && (juno ? core::junolib.clear(s) : core::dxlib.clear(s));
    sniprintf(out, cap, ok ? "%s slot %d emptied" : "%s: no slot %d", what, s);
  } else if (!strncmp(a, "ren:", 4)) {
    int s = num(a + 4, &e);
    ok = *e == ':' && e[1] && (juno ? core::junolib.rename(s, e + 1) : core::dxlib.rename(s, e + 1));
    sniprintf(out, cap, ok ? "%s slot %d renamed" : "%s: slot %d not renamed (empty, or no name)", what, s);
  } else if (!strncmp(a, "swap:", 5)) {
    int x = num(a + 5, &e), y = *e == ':' ? num(e + 1, &e) : -1;
    ok = *e == 0 && (juno ? core::junolib.swap(x, y) : core::dxlib.swap(x, y));
    sniprintf(out, cap, ok ? "%s slots %d and %d swapped" : "usage: %s=swap:<a>:<b>", what, x, y);
  } else if (!strncmp(a, "save:", 5)) {
    int vi = num(a + 5, &e);
    const char* name = *e == ':' ? e + 1 : nullptr;
    if (vi < 0 || vi >= core::kVoices || (*e && *e != ':')) {
      sniprintf(out, cap, "usage: %s=save:<voice 0..7>%s", what, juno ? "[:<name>]" : "");
      return true;
    }
    const core::V& x = core::v[vi];
    if (juno) {
      if (!x.juno) { sniprintf(out, cap, "voice %d does not play JUNO", vi); return true; }
      const uint8_t* b;
      const char* nm;
      core::juno_pick(core::juno_pat[vi] < 0 ? 0 : core::juno_pat[vi], &b, &nm);
      uint8_t pb[18];
      memcpy(pb, b, 18);
      for (int k = 0; k < 6; ++k) {
        float p = x.p[1 + k];
        pb[kJunoByte[k]] = (uint8_t)(!(p > 0.0f) ? 0 : p >= 127.0f ? 127 : (int)(p + 0.5f));
      }
      int s = core::junolib.put(pb, name && name[0] ? name : nm);
      ok = s >= 0;
      sniprintf(out, cap, ok ? "juno slot %d = voice %d's patch" : "juno: the list is full", s, vi);
    } else {
      if (!x.fm) { sniprintf(out, cap, "voice %d does not play FM", vi); return true; }
      if (core::dxlib.listed()) {
        sniprintf(out, cap, "voice %d plays the project's FM list already (slot %d)", vi, core::fm_pat[vi]);
        return true;
      }
      int r = core::fm_pat[vi] < 0 ? 0 : core::fm_pat[vi] >= dx7::kRomVoices ? dx7::kRomVoices - 1 : core::fm_pat[vi];
      int s = core::dxlib.add_rom(r);
      ok = s >= 0;
      sniprintf(out, cap, ok ? "fm slot %d = ROM voice %d (the list now replaces the ROM's for PAT)" : "fm: not added", s, r);
    }
  } else {
    sniprintf(out, cap, "usage: %s=ren:<slot>:<name> | %s=del:<slot> | %s=swap:<a>:<b> | %s=save:<voice>", what, what, what, what);
    return true;
  }
  if (ok) {
    if (juno) { core::send_juno_names(); core::juno_relist(); }
    else { core::send_fm_names(); core::fm_relist(); }
    publish_now();
  }
  return true;
}

}  // namespace

bool command(const char* c, char* out, int cap) {
  out[0] = 0;
  bool r;
#if CORE_MSAMP
  if (!strncmp(c, "pool=", 5) && !(c[5] >= '0' && c[5] <= '9')) return core::pools::command(c, out, cap);   // smp|wt|inst
#endif
  if (!strncmp(c, "pool=", 5)) r = cmd_pool(c + 5, out, cap);
  else if (!strncmp(c, "juno=", 5)) r = cmd_list(true, c + 5, out, cap);
  else if (!strncmp(c, "fm=", 3)) r = cmd_list(false, c + 3, out, cap);
  else return false;
  publish_now();
  return r;
}

void idle() {
  if (core::now_ms() - g_ms < kEveryMs) return;
  g_ms = core::now_ms();
  uint32_t h = state_hash();
  if (h != g_hash || !g_jseq) {
    g_hash = h;
    publish();
  }
}

void publish_now() {
  g_hash = state_hash();
  g_ms = core::now_ms();
  publish();
}

int json_copy(char* out, int cap) {
  for (int tries = 0; tries < 8; ++tries) {
    uint32_t a = __atomic_load_n(&g_jseq, __ATOMIC_ACQUIRE);
    if (a == 0) return 0;
    int r = (int)((a / 2) & 1);
    int n = g_jlen[r];
    if (n >= cap) return 0;
    memcpy(out, g_js[r], (size_t)n);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_jseq, __ATOMIC_ACQUIRE) - a < 2) { out[n] = 0; return n; }
  }
  return 0;
}

}  // namespace lists
