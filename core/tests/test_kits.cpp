// core/tests/test_kits.cpp - the Sidecar's per-kit data follows the Rytm's SAVE / RELOAD KIT (core/bundle/kits.h,
// rytm-mods 0026 rui1052d's 4C): SAVE keeps a record of every per-kit feature (the LFOs, the inserts) with the edits not
// yet saved; RELOAD keeps the working copy as .prv and puts the saved one back (the modules hold it at once); RESTORE
// brings .prv back; a reload with no saved record keeps the working copy; SAVE KIT to another kit gives that kit the
// copy; RELOAD ALL reloads every kit with a record; a report while the store fills waits for it; an unreadable working
// file refuses (nothing written); both copies in the RAM store and on the card; no card time on the control core.
#include <stdlib.h>
#include <string.h>
#include <string>

#include "check.h"
#include "../bundle/bundle.h"
#include "../bundle/bundle_int.h"
#include "../bundle/kits.h"
#include "../core.h"
#include "../fmt/fmt.h"
#include "../fx/insert.h"
#include "../host/core_port.h"
#include "../lfo.h"
#include "../msamp/cardq.h"
#include "../store/store.h"

namespace cardq = core::cardq;
namespace kits = core::kits;

static std::string g_card;
static uint64_t g_us = 1000000;
static void card_file(const char* path, const char* bytes) {
  std::string p = g_card + path;
  CHECK(system(("mkdir -p \"$(dirname '" + p + "')\"").c_str()) == 0);
  FILE* f = fopen(p.c_str(), "wb");
  CHECK(f);
  fwrite(bytes, 1, strlen(bytes), f);
  fclose(f);
}
static std::string rd(const std::string& path) {
  std::string s;
  FILE* f = fopen((g_card + path).c_str(), "rb");
  if (!f) return s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static bool has(const std::string& path, const char* what) { return rd(path).find(what) != std::string::npos; }
static bool held(const char* path, const char* what) {
  const uint8_t* d;
  uint32_t n;
  return core::store::get(path, &d, &n) == core::store::kPresent &&
         std::string((const char*)d, n).find(what) != std::string::npos;
}
static void card_turn() {
  core::host_card_core(0);
  while (cardq::service()) {
  }
  core::host_card_core(3);
}
static void pass() {
  core::host_fake_time(true, g_us += 20000);
  core::idle_save();
  card_turn();
}
static void settle() {   // the store filled, the kit reports handled, every write landed
  for (int i = 0; i < 20000 && core::project_loading(); ++i) pass();
  for (int i = 0; i < 400; ++i) pass();
  for (int i = 0; i < 4000 && (core::fmt::post_busy() || core::lfo::dirty() || core::ins::settings_dirty(nullptr)); ++i)
    pass();
  CHECK(!core::fmt::post_busy());
}
static void sx(const uint8_t* m, int n) { core::engine_sysex(m, n); }
static void project(const char* name) {
  uint8_t m[40] = {0xF0, 0x7D, 0x52, 0x54, 0x10};
  int k = 5;
  for (const char* c = name; *c; ++c) m[k++] = (uint8_t)*c;
  m[k++] = 0xF7;
  sx(m, k);
  for (int i = 0; i < 400 && strcmp(core::project, name); ++i) pass();
  CHECK(!strcmp(core::project, name));
}
static void kit(int k) {   // 3B: the Rytm's active kit (0-based)
  const uint8_t m[8] = {0xF0, 0x7D, 0x52, 0x54, 0x3B, 1, (uint8_t)k, 0xF7};
  sx(m, 8);
}
static void k4c(int op, int kit1, int src1) {   // 4C 01 <op> <kit> <src> (1-based)
  const uint8_t m[12] = {0xF0, 0x7D, 0x52, 0x54, 0x4C, 1, (uint8_t)op, (uint8_t)(kit1 >> 7), (uint8_t)(kit1 & 0x7F),
                         (uint8_t)(src1 >> 7), (uint8_t)(src1 & 0x7F), 0xF7};
  sx(m, 12);
}
static void lfo_set(const char* kv) {
  char o[160];
  CHECK(core::lfo::command(kv, o, sizeof o) && !strstr(o, "cannot"));
}
static void ins_set(const char* kv) {
  char o[200];
  CHECK(core::ins::command(kv, o, sizeof o));
}
static int depth() { return core::lfo::get(0).depth; }
static void noop_out(const uint8_t*, int) {}

int main() {
  char tmpl[] = "/tmp/kits-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  core::host_set_card(g_card.c_str());
  core::host_fake_time(true, g_us);
  core::hooks.to_rytm = noop_out;
  CHECK(system(("mkdir -p '" + g_card + "/Samples'").c_str()) == 0);
  card_file("/.rytm/project", "KP");
  card_file("/.rytm/lfo/KP/k001.txt", "# Sidecar LFOs\n1 V3 2 1 64 0 0 10 0\n");
  card_file("/.rytm/lfo/KQ/k001.txt", "# Sidecar LFOs\n1 V3 2 1 64 0 0 44 0\n");
  card_file("/.rytm/lfo/KL/k001.txt", "# Sidecar LFOs\n1 V3 2 1 64 0 0 55 0\n");
  CHECK(core::engine_begin());
  cardq::set_worker(true);
  kit(0);
  settle();
  CHECK(!core::project_loading() && core::lfo::kit() == 0 && depth() == 10);

  // the folder is one of the project's files: a slot's copy / move / trash takes it along (core/bundle/prjslot)
  bool listed = false;
  for (int i = 0; i < core::bundle::kNumProjectFiles; ++i)
    listed |= !strcmp(core::bundle::kProjectFiles[i].card, "/.rytm/kits/%s") && core::bundle::kProjectFiles[i].how == core::bundle::kDir;
  CHECK(listed);

  // ---- SAVE (YES + KIT) with edits not yet saved: the record holds them, every feature's section
  core::host_card_model(true);
  uint64_t ctl = core::host_card_us(3);
  lfo_set("lfo=1:dep:20");
  ins_set("ins=v1:send:1:40");
  k4c(kits::kSave, 1, 1);
  settle();
  const char* sks1 = "/.rytm/kits/KP/k001.sks";
  CHECK(has(sks1, "=lfo\n") && has(sks1, " 20 ") && has(sks1, "=ins\n") && has(sks1, "SEND V1 40"));
  CHECK(held(sks1, " 20 ") && kits::stats().saves == 1);
  printf("kits: SAVE KIT: kit 001's record (LFOs and inserts, the unsaved edits in it), in the store and on the card\n");

  // ---- RELOAD (NO + KIT): the working copy kept as .prv, the saved one back (the LFOs hold it at once)
  lfo_set("lfo=1:dep:30");
  k4c(kits::kReload, 1, 0);
  settle();
  CHECK(depth() == 20);
  CHECK(has("/.rytm/kits/KP/k001.prv", " 30 ") && has("/.rytm/lfo/KP/k001.txt", " 20 ") && !has("/.rytm/lfo/KP/k001.txt", " 30 "));
  CHECK(held("/.rytm/lfo/KP/k001.txt", " 20 ") && has("/.rytm/ins/KP/k001.txt", "SEND V1 40") && kits::stats().reloads == 1);
  printf("kits: RELOAD KIT: the saved copy back (LFO depth 20), the working one kept as k001.prv (depth 30)\n");

  // ---- RESTORE (0010's EDITS RESTORED): .prv back
  k4c(kits::kRestore, 1, 0);
  settle();
  CHECK(depth() == 30 && has("/.rytm/lfo/KP/k001.txt", " 30 ") && kits::stats().restores == 1);
  CHECK(has("/.rytm/kits/KP/k001.prv", " 20 "));   // (a swap: the copy it replaced kept aside in turn, nothing lost)
  printf("kits: RESTORE: the kept-aside copy back (depth 30), the one it replaced kept aside (depth 20)\n");

  // ---- RELOAD of a kit with no saved record: the working copy kept, nothing written
  kit(1);
  settle();
  lfo_set("lfo=1:dep:7");
  uint32_t kept = kits::stats().kept;
  k4c(kits::kReload, 2, 0);
  settle();
  CHECK(depth() == 7 && kits::stats().kept == kept + 1 && rd("/.rytm/kits/KP/k002.prv").empty());
  printf("kits: RELOAD of a kit never saved here: the working copy kept (no guess)\n");

  // ---- SAVE KIT to another kit (kit 2 saved as kit 3, the Rytm then on kit 3)
  k4c(kits::kSave, 3, 2);
  kit(2);
  settle();
  CHECK(core::lfo::kit() == 2 && depth() == 7);
  CHECK(has("/.rytm/kits/KP/k003.sks", " 7 ") && has("/.rytm/lfo/KP/k003.txt", " 7 "));
  printf("kits: SAVE KIT to kit 003 from kit 002: its record and its working copy, the LFOs on it (depth 7)\n");

  // ---- RELOAD ALL: every kit with a record back to it (kit 1 and kit 3), kit 2 (none) kept
  lfo_set("lfo=1:dep:9");   // kit 3
  kit(0);
  settle();
  lfo_set("lfo=1:dep:11");   // kit 1
  k4c(kits::kReloadAll, 1, 0);
  settle();
  CHECK(depth() == 20 && has("/.rytm/lfo/KP/k001.txt", " 20 ") && has("/.rytm/lfo/KP/k003.txt", " 7 "));
  CHECK(has("/.rytm/lfo/KP/k002.txt", " 7 ") && has("/.rytm/kits/KP/k003.prv", " 9 ") && kits::stats().reload_alls == 1);
  CHECK(core::host_card_us(3) == ctl);   // every write posted: no card time on the control core
  CHECK(kits::stats().scan_max > 0 && kits::stats().scan_max <= 16);   // (the store lookups one pass made: L1 / N5)
  core::host_card_model(false);
  printf("kits: RELOAD ALL: kits 001 and 003 back to their records, 002 kept; no card time on the control core\n");

  // ---- a report while the next project's store fills: it waits, then is the new project's
  project("KQ");
  CHECK(core::project_loading());
  k4c(kits::kSave, 1, 1);
  settle();
  CHECK(!core::project_loading() && has("/.rytm/kits/KQ/k001.sks", " 44 ") && !has("/.rytm/kits/KP/k001.sks", " 44 "));
  printf("kits: a SAVE while the project's store fills waits for it (KQ's record)\n");

  // ---- an unreadable working file: the SAVE refused, nothing written (never a guess)
  core::host_fail_open("/.rytm/lfo/KL/k001.txt", true);
  project("KL");
  settle();
  uint32_t refused = kits::stats().refused;
  k4c(kits::kSave, 1, 1);
  settle();
  CHECK(kits::stats().refused == refused + 1 && rd("/.rytm/kits/KL/k001.sks").empty());
  k4c(kits::kReload, 1, 0);   // (no record: kept)
  settle();
  CHECK(has("/.rytm/lfo/KL/k001.txt", " 55 "));
  core::host_fail_open("/.rytm/lfo/KL/k001.txt", false);
  printf("kits: an unreadable working file: no record (refused), the file as it was\n");

  // ---- round 5, H1: a report waiting (the store of KR filling) when the Rytm goes to another project (10 KS): stale,
  // dropped - never applied to KS (its kit 3 files untouched, no record)
  card_file("/.rytm/lfo/KS/k003.txt", "# Sidecar LFOs\n1 V3 2 1 64 0 0 66 0\n");
  card_file("/.rytm/lfo/KR/k002.txt", "# Sidecar LFOs\n1 V3 2 1 64 0 0 61 0\n");
  project("KR");
  CHECK(core::project_loading());
  uint32_t stale = kits::stats().stale;
  k4c(kits::kSave, 3, 2);   // (KR's SAVE KIT 2 -> 3, waiting for KR's store)
  project("KS");
  settle();
  CHECK(kits::stats().stale == stale + 1);
  CHECK(rd("/.rytm/kits/KS/k003.sks").empty() && rd("/.rytm/kits/KR/k003.sks").empty() &&
        has("/.rytm/lfo/KS/k003.txt", " 66 "));
  printf("kits: a report waiting when the Rytm changes projects: dropped, the next project's kit files untouched\n");

  // ---- round 5, L4: a kit with no file of its own is saved as "#none" and keeps inheriting the project's default
  card_file("/.rytm/lfo/KN.txt", "# Sidecar LFOs\n1 V3 2 1 64 0 0 12 0\n");
  project("KN");
  kit(1);
  settle();
  CHECK(depth() == 12 && rd("/.rytm/lfo/KN/k002.txt").empty());
  k4c(kits::kSave, 2, 2);
  settle();
  CHECK(has("/.rytm/kits/KN/k002.sks", "=lfo\n#none\n"));
  k4c(kits::kReload, 2, 0);
  settle();
  CHECK(rd("/.rytm/lfo/KN/k002.txt").empty() && depth() == 12);   // still no file of its own: inheriting
  lfo_set("lfo=1:dep:13");   // (an edit: kit 2 gets a file of its own)
  settle();
  CHECK(has("/.rytm/lfo/KN/k002.txt", " 13 "));
  k4c(kits::kReload, 2, 0);
  settle();
  CHECK(depth() == 12 && has("/.rytm/lfo/KN/k002.txt", " 12 ") && has("/.rytm/kits/KN/k002.prv", " 13 "));
  printf("kits: a kit without its own file: saved as #none, a reload keeps it inheriting (or, edited since, the default)\n");

  // ---- round 5, L2: a reload cut between its LFO and INSERT writes, then done again: .prv keeps the first reload's
  // working copy for the feature already reloaded (the LFO), never the half-reloaded state
  kit(0);
  settle();
  lfo_set("lfo=1:dep:21");
  ins_set("ins=v1:send:1:41");
  settle();
  k4c(kits::kSave, 1, 1);   // saved: 21 / 41
  settle();
  lfo_set("lfo=1:dep:22");
  ins_set("ins=v1:send:1:42");   // working: 22 / 42
  settle();
  k4c(kits::kReload, 1, 0);   // .prv 22 / 42, working 21 / 41
  settle();
  CHECK(depth() == 21 && has("/.rytm/kits/KN/k001.prv", " 22 ") && has("/.rytm/kits/KN/k001.prv", "SEND V1 42"));
  ins_set("ins=v1:send:1:42");   // the INSERT as a cut before its write leaves it: LFO reloaded, INSERT not
  settle();
  uint32_t ka = kits::stats().kept_aside;
  k4c(kits::kReload, 1, 0);   // done again
  settle();
  CHECK(kits::stats().kept_aside == ka + 1);
  CHECK(has("/.rytm/kits/KN/k001.prv", " 22 ") && !has("/.rytm/kits/KN/k001.prv", " 21 ") &&
        has("/.rytm/kits/KN/k001.prv", "SEND V1 42"));
  CHECK(depth() == 21 && has("/.rytm/ins/KN/k001.txt", "SEND V1 41"));
  printf("kits: a reload done again after a cut: .prv keeps the LFO the first one kept aside (22), not the half state\n");

  cardq::set_worker(false);
  cardq::reset();
  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_kits: ok\n");
  return 0;
}
