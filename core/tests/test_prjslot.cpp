// core/tests/test_prjslot.cpp - Sidecar projects keyed by the Rytm's project slot, and the project trash
// (core/bundle/prjslot.h; docs/superpowers/specs/2026-10-27-project-slot-keys-design.md). The Rytm's side is its
// messages as rytm-mods rui1052 sends them (tools/emu/pj_emu.py measured them on the emulator): the hello's caps (35),
// 4B 01 00 ACTIVE <slot> <name>, 4B 01 <op> <a> <b> (01 SAVE, 02 PASTE, 03 CLEAR, 04 DELETE, 05 RENAME); the 10 <name>
// of 0021 as before. Covered: old mods (name mode as today, the rename prompt), the migration (the active project by
// slot + name, files from before slots asked for when opened - YES / NO / declined / the timeout -, the two PRESETS
// never merged), SAVE AS / SAVE TO / PASTE copies, CLEAR / DELETE of another slot and of the active one (pending until
// it is left or reloaded, a SAVE keeps it), the no-slot project, the fallback question (a name changed without an
// event: keep / trash / the timeout keeps), the trash's cap (64, the oldest for good), restore (its key, another,
// refused over files, replace, refused for the current project), the web page's delete into the trash, emptying,
// the held 10 on a card keyed by slot, and every look / move / copy through the card queue with a card core.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bundle/bundle.h"
#include "bundle/kits.h"
#include "bundle/prjslot.h"
#include "bundle/projects.h"
#include "bundle/rename.h"
#include "check.h"
#include "engine.h"
#include "hooks.h"
#include "lfo.h"
#include "lib/pools.h"
#include "song/song.h"
#include "wg_util.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "rui_png.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;

static std::string g_card;
static bool is_file(const std::string& p) {
  struct stat st;
  return stat((g_card + p).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static bool is_dir(const std::string& p) {
  struct stat st;
  return stat((g_card + p).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static std::string slurp(const std::string& p) {
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "<missing>";
  std::string s;
  char b[512];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void put(const std::string& p, const std::string& s) {
  CHECK(system(("mkdir -p \"$(dirname '" + g_card + p + "')\"").c_str()) == 0);
  FILE* f = fopen((g_card + p).c_str(), "wb");
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
// a project's files: an LFO file, an insert folder, a sequence folder (two patterns, one past 64 KB: a copy in pieces),
// the pool lists' folder, a song, a kit's saved record (core/bundle/kits.h)
static void fixture(const std::string& k, const std::string& c) {
  put("/.rytm/lfo/" + k + ".txt", "lfo " + c);
  put("/.rytm/kits/" + k + "/k001.sks", "=lfo\nkits " + c + "\n");
  put("/.rytm/ins/" + k + "/k001.txt", "ins " + c);
  put("/.rytm/seq/" + k + "/tracks.txt", "tracks " + c);
  put("/.rytm/seq/" + k + "/A01.sqp", std::string(70000, 'p') + c);
  put("/.rytm/projects/" + k + "/samples.pool.txt", "pool " + c);
  put("/.rytm/songs/" + k + "/take-001.json", "{\"song\":\"" + c + "\"}");
}
static bool has_all(const std::string& k, const std::string& c) {
  return slurp("/.rytm/lfo/" + k + ".txt") == "lfo " + c && slurp("/.rytm/ins/" + k + "/k001.txt") == "ins " + c &&
         slurp("/.rytm/seq/" + k + "/tracks.txt") == "tracks " + c &&
         slurp("/.rytm/seq/" + k + "/A01.sqp") == std::string(70000, 'p') + c &&
         slurp("/.rytm/projects/" + k + "/samples.pool.txt") == "pool " + c &&
         slurp("/.rytm/songs/" + k + "/take-001.json") == "{\"song\":\"" + c + "\"}" &&
         slurp("/.rytm/kits/" + k + "/k001.sks") == "=lfo\nkits " + c + "\n";
}
static bool has_none(const std::string& k) {
  return !is_file("/.rytm/lfo/" + k + ".txt") && !is_dir("/.rytm/ins/" + k) && !is_dir("/.rytm/seq/" + k) &&
         !is_dir("/.rytm/projects/" + k) && !is_dir("/.rytm/songs/" + k) && !is_dir("/.rytm/kits/" + k);
}
// the trash's entries (their numbers, sorted) and an entry's info.txt line
static std::vector<int> entries() {
  std::vector<int> v;
  DIR* d = opendir((g_card + "/.rytm/trash").c_str());
  if (!d) return v;
  while (dirent* e = readdir(d))
    if (e->d_name[0] >= '0' && e->d_name[0] <= '9') v.push_back(atoi(e->d_name));
  closedir(d);
  std::sort(v.begin(), v.end());
  return v;
}
static std::string entry(int id) {
  char b[64];
  snprintf(b, sizeof b, "/.rytm/trash/%06d", id);
  return b;
}
static std::string info(int id, const char* field) {
  std::string t = slurp(entry(id) + "/info.txt"), f = std::string(field) + "=";
  size_t p = t.find("\n" + f);
  if (p == std::string::npos) return "";
  p += f.size() + 1;
  return t.substr(p, t.find('\n', p) - p);
}
// the newest entry with this key (0: none)
static int newest(const char* key) {
  int best = 0;
  for (int id : entries())
    if (info(id, "key") == key) best = id;
  return best;
}

// ---- the Rytm's messages
static void sx(std::vector<uint8_t> m) {
  m.insert(m.begin(), {0xF0, 0x7D, 0x52, 0x54});
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void hello(int caps) { sx({0x35, 0x01, 0x00, 0x05, (uint8_t)caps}); }
static void active(int slot, const char* name) {
  std::vector<uint8_t> m = {0x4B, 0x01, 0x00, (uint8_t)(slot >> 7), (uint8_t)(slot & 0x7F)};
  for (const char* c = name; *c; ++c) m.push_back((uint8_t)*c);
  sx(m);
}
enum { SAVE = 1, PASTE, CLEAR, DELETE, RENAME, LOST, WRITE, LIST, LIST_END, LOAD };
static void ev(int op, int a, int b) {
  sx({0x4B, 0x01, (uint8_t)op, (uint8_t)(a >> 7), (uint8_t)(a & 0x7F), (uint8_t)(b >> 7), (uint8_t)(b & 0x7F)});
}
static void name_project(const char* n) {   // 0021's 10 <name>
  std::vector<uint8_t> m = {0x10};
  for (const char* c = n; *c; ++c) m.push_back((uint8_t)*c);
  sx(m);
}
static uint64_t g_t = 9000000000ull;
static void pass() {
  host_fake_time(true, g_t += 20000);
  idle_save();
}
static void settle() {   // every job, the store's fill, then a few passes more
  for (int i = 0; i < 200000 && (prjslot::busy() || project_loading()) && !prjslot::ask(); ++i) pass();
  CHECK(!project_loading());
  for (int i = 0; i < 32; ++i) pass();
}
static bool is(const char* key) { return !strcmp(project, key); }
// the Rytm's slot list (4B 08 x 16, 4B 09): skip leaves one message out, count overrides the END's
static void send_list(const std::vector<std::pair<int, std::string>>& list, int skip = -1, int count = -1) {
  for (int first = 1, k = 0; first <= 128; first += 8, ++k) {
    if (k == skip) continue;
    std::vector<uint8_t> m = {0x4B, 0x01, 0x08, (uint8_t)(first >> 7), (uint8_t)(first & 0x7F), 8};
    for (int sl = first; sl < first + 8; ++sl) {
      std::string nm;
      for (auto& p : list) if (p.first == sl) nm = p.second;
      m.push_back(nm.empty() ? 0 : 1);
      for (int c = 0; c < 15; ++c) m.push_back(c < (int)nm.size() ? (uint8_t)nm[c] : 0);
    }
    sx(m);
  }
  int n = count >= 0 ? count : (int)list.size();
  sx({0x4B, 0x01, 0x09, (uint8_t)(n >> 7), (uint8_t)(n & 0x7F), 0x00, 0x00});
}
static std::string screen_text() {   // the remote frame's text items, one line
  uint8_t t[36 * ui::kTextSlots];
  int n = ui::sent_texts(t, ui::kTextSlots);
  std::string s;
  for (int i = 0; i < n; ++i) s += std::string((const char*)t + 36 * i + 4) + "|";
  return s;
}

int main() {
  char tmpl[] = "/tmp/prjslot-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = [](const uint8_t*, int) {};
  CHECK(engine_begin() && card_ok);
  cardq::reset();
  rename::reset();
  prjslot::reset();
  host_fake_time(true, g_t);

  // ---- the key rule: @NNN passes through, a Rytm name never makes one
  {
    char k[kKeyMax];
    project_key("@005", k, sizeof k);
    CHECK(!strcmp(k, "@005"));
    project_key("@129", k, sizeof k);
    CHECK(!strcmp(k, "_129"));
    project_key("@05", k, sizeof k);
    CHECK(!strcmp(k, "_05"));
    project_key("A@B", k, sizeof k);
    CHECK(!strcmp(k, "A_B"));
    project_key_song_v0("@007", k, sizeof k);
    CHECK(!strcmp(k, "@007"));
    CHECK(prjslot::slot_of("@128") == 128 && prjslot::slot_of("@000") == 0 && prjslot::slot_of("PRESETS") == -1);
  }

  // ---- old mods (rui1051: caps 3, no 4B): name mode, today's behaviour (the 10 switches by name, the rename prompt)
  fixture("PRESETS", "presets");
  fixture("OLDSONG", "oldsong");
  fixture("SHARED", "shared");
  hello(3);
  CHECK(prjslot::mode() == prjslot::kName);
  name_project("SHARED");
  settle();
  CHECK(is("SHARED") && !strcmp(project_name, "SHARED"));
  name_project("SHARED 2");   // a rename on the Rytm, as today: asked
  CHECK(rename::state() == rename::kAsk && is("SHARED"));
  rename::answer(false);
  CHECK(is("SHARED 2") && has_all("SHARED", "shared"));
  name_project("@001");   // a Rytm name of a slot key's form: today's key for it ("_001")
  settle();
  CHECK(is("_001"));
  name_project("PRESETS");
  settle();
  CHECK(is("PRESETS") && !prjslot::migrated());
  printf("  old mods: name mode as today (switch by name, the rename prompt)\n");

  // ---- rui1052, the first session: the migration of the active project by slot + name, without a question
  prjslot::reset();
  prjslot::load();
  hello(7);
  CHECK(prjslot::mode() == prjslot::kSlot);
  name_project("PRESETS");   // (0021's 10 goes on: not a switch now)
  active(1, "PRESETS");
  settle();
  CHECK(!prjslot::ask() && is("@001") && !strcmp(project_name, "PRESETS"));
  CHECK(has_all("@001", "presets") && has_none("PRESETS"));
  CHECK(prjslot::migrated() && slurp("/.rytm/prjslot.txt").find("mode=slot") != std::string::npos);
  CHECK(projects::find("@001") && !strcmp(projects::find("@001")->rytm, "PRESETS") && !projects::find("PRESETS"));
  printf("  migration: the active project by slot + name (PRESETS -> @001)\n");

  // ---- the user's second PRESETS (slot 2): never merged - no files of its own, none left before slots: empty
  active(2, "PRESETS");
  settle();
  CHECK(!prjslot::ask() && is("@002") && has_none("@002") && has_all("@001", "presets"));

  // ---- files from before slots, asked for when their slot is opened: NO (declined: not asked again for that slot)
  active(3, "OLDSONG");
  for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
  CHECK(prjslot::ask() == prjslot::kAskLegacy && is("@002"));   // (the switch waits for the answer)
  CHECK(std::string(prjslot::ask_line(1)) == "NAME OLDSONG" && std::string(prjslot::ask_line(2)) == "FOR SLOT 003?");
  prjslot::answer(false);
  settle();
  CHECK(is("@003") && has_none("@003") && has_all("OLDSONG", "oldsong"));
  active(1, "PRESETS");
  settle();
  active(3, "OLDSONG");   // declined for slot 3: no question
  settle();
  CHECK(!prjslot::ask() && is("@003") && has_all("OLDSONG", "oldsong"));
  // another slot of that name: asked, YES takes them
  active(4, "OLDSONG");
  for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
  CHECK(prjslot::ask() == prjslot::kAskLegacy);
  prjslot::answer(true);
  settle();
  CHECK(is("@004") && has_all("@004", "oldsong") && has_none("OLDSONG"));
  // the timeout: NO (nothing moved)
  fixture("LATE", "late");
  active(5, "LATE");
  for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
  CHECK(prjslot::ask() == prjslot::kAskLegacy);
  g_t += (uint64_t)prjslot::kAskMs * 1000 + 1000000;
  settle();
  CHECK(!prjslot::ask() && is("@005") && has_none("@005") && has_all("LATE", "late"));
  printf("  migration: files from before slots asked for (NO, declined, YES, the timeout)\n");

  // ---- SAVE AS (from slot 1 to the empty slot 6): copied, the original kept
  active(1, "PRESETS");
  settle();
  ev(SAVE, 1, 6);
  active(6, "PRESETS");
  settle();
  CHECK(!prjslot::ask() && is("@006") && has_all("@006", "presets") && has_all("@001", "presets"));
  CHECK(projects::find("@006") && !strcmp(projects::find("@006")->rytm, "PRESETS"));
  // SAVE TO an occupied slot 7 (a new name in the name editor): 7's files to the trash first, then the copy; no question
  fixture("@007", "seven");
  int n0 = (int)entries().size();
  ev(SAVE, 6, 7);
  active(7, "PRESETS X");
  settle();
  CHECK(!prjslot::ask() && is("@007") && !strcmp(project_name, "PRESETS X") && has_all("@007", "presets"));
  CHECK((int)entries().size() == n0 + 1);
  int e7 = newest("@007");
  CHECK(e7 && info(e7, "why") == "saveto" && slurp(entry(e7) + "/lfo!%.txt") == "lfo seven" &&
        slurp(entry(e7) + "/seq!%/A01.sqp") == std::string(70000, 'p') + "seven");
  printf("  SAVE AS / SAVE TO: the files copied (64 KB pieces), the overwritten slot's in the trash (%06d)\n", e7);

  // ---- PASTE onto another slot (8): copied; onto the active one: pending, a SAVE keeps it, leaving it replaces
  ev(PASTE, 1, 8);
  settle();
  CHECK(is("@007") && has_all("@008", "presets"));
  int n1 = (int)entries().size();
  ev(PASTE, 4, 7);   // onto the active slot 7: nothing moves yet (the Rytm's RAM is still 7's project)
  settle();
  CHECK(is("@007") && has_all("@007", "presets") && (int)entries().size() == n1);
  ev(SAVE, 7, 7);    // saved: the paste is undone on the card, the files stay
  active(1, "PRESETS");
  settle();
  CHECK(has_all("@007", "presets") && (int)entries().size() == n1);
  active(7, "PRESETS X");
  settle();
  ev(PASTE, 4, 7);
  active(1, "PRESETS");   // left: 7 is OLDSONG's copy now
  settle();
  CHECK(is("@001") && has_all("@007", "oldsong") && (int)entries().size() == n1 + 1 && info(newest("@007"), "why") == "paste");
  printf("  PASTE: another slot copied; the active one pending (a SAVE keeps it, leaving replaces it)\n");

  // ---- CLEAR / DELETE of another slot: to the trash
  ev(CLEAR, 0, 8);
  settle();
  CHECK(has_none("@008") && info(newest("@008"), "why") == "clear" && slurp(entry(newest("@008")) + "/seq!%/tracks.txt") == "tracks presets");
  fixture("@009", "nine");
  ev(DELETE, 0, 9);
  settle();
  CHECK(has_none("@009") && info(newest("@009"), "why") == "delete" && is("@001"));
  // CLEAR of the active one: kept until reloaded (the Rytm names it NEW PROJECT then); a SAVE cancels it
  active(6, "PRESETS");
  settle();
  int n2 = (int)entries().size();
  ev(CLEAR, 0, 6);
  settle();
  CHECK(has_all("@006", "presets") && (int)entries().size() == n2);
  ev(SAVE, 6, 6);
  ev(CLEAR, 0, 6);
  active(6, "NEW PROJECT");   // reloaded from the card
  settle();
  CHECK(is("@006") && !strcmp(project_name, "NEW PROJECT") && (int)entries().size() == n2 + 1);
  CHECK(slurp(entry(newest("@006")) + "/lfo!%.txt") == "lfo presets" && !is_file("/.rytm/lfo/@006.txt"));
  // DELETE of the active one, then another loaded: to the trash after the switch
  active(8, "PRESETS");   // (slot 8: its files were cleared above - an empty project)
  settle();
  fixture("@008", "eight");
  active(6, "NEW PROJECT");
  settle();
  active(8, "PRESETS");
  settle();
  CHECK(has_all("@008", "eight"));
  ev(DELETE, 0, 8);
  active(2, "PRESETS");
  settle();
  CHECK(is("@002") && has_none("@008") && info(newest("@008"), "why") == "delete");
  printf("  CLEAR / DELETE: another slot to the trash; the active one when reloaded or left, a SAVE keeps it\n");

  // ---- CREATE NEW (no slot), then SAVE AS 10: the no-slot project's files move to 10; a new one starts empty
  active(0, "NEW PROJECT");
  settle();
  CHECK(is("@000"));
  fixture("@000", "scratch");
  ev(SAVE, 0, 10);
  active(10, "SCRATCH");
  settle();
  CHECK(is("@010") && has_all("@010", "scratch") && has_none("@000"));
  fixture("@000", "left over");
  active(0, "NEW PROJECT");
  settle();
  CHECK(is("@000") && has_none("@000") && info(newest("@000"), "why") == "new");
  printf("  CREATE NEW: the no-slot project's files go with its SAVE AS; a new one starts empty (leftovers trashed)\n");

  // ---- the fallback question: a slot's name changed without an event
  fixture("@011", "eleven");
  active(11, "ELEVEN");   // (files, no index name yet: taken)
  settle();
  CHECK(is("@011") && !prjslot::ask());
  active(1, "PRESETS");
  settle();
  active(11, "OTHER");    // renamed while the Sidecar was off (or another card): asked
  for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
  CHECK(prjslot::ask() == prjslot::kAskMismatch && is("@001"));
  CHECK(std::string(prjslot::ask_line(1)) == "NOW OTHER" && std::string(prjslot::ask_line(2)) == "WAS ELEVEN");
  prjslot::answer(true);   // YES: kept
  settle();
  CHECK(is("@011") && has_all("@011", "eleven") && !strcmp(projects::find("@011")->rytm, "OTHER"));
  active(1, "PRESETS");
  settle();
  ev(RENAME, 0, 11);   // a RENAME seen: the next name taken without a question
  active(11, "RENAMED");
  settle();
  CHECK(!prjslot::ask() && is("@011") && has_all("@011", "eleven"));
  active(1, "PRESETS");
  settle();
  active(11, "THIRD");
  for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
  CHECK(prjslot::ask() == prjslot::kAskMismatch);
  g_t += (uint64_t)prjslot::kAskMs * 1000 + 1000000;   // the timeout keeps them
  settle();
  CHECK(!prjslot::ask() && is("@011") && has_all("@011", "eleven"));
  active(1, "PRESETS");
  settle();
  active(11, "FOURTH");
  for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
  CHECK(prjslot::ask() == prjslot::kAskMismatch);
  prjslot::answer(false);   // NO: to the trash, a new project
  settle();
  CHECK(is("@011") && has_none("@011") && info(newest("@011"), "why") == "mismatch");
  printf("  the fallback question: YES keeps, a RENAME is no question, the timeout keeps, NO trashes\n");

  // ---- restore: to its key; refused over files and for the current project; replace; to another key
  {
    int id = newest("@009");
    CHECK(id && prjslot::request_restore((uint32_t)id, "", false));
    settle();
    CHECK(has_all("@009", "nine") && !is_dir(entry(id)) && !strncmp(prjslot::last(), "RESTORED", 8));
    int id8 = newest("@008");   // @008's DELETE; the slot has files again now?
    fixture("@008", "new eight");
    CHECK(prjslot::request_restore((uint32_t)id8, "", false));
    settle();
    CHECK(has_all("@008", "new eight") && is_dir(entry(id8)) && std::string(prjslot::last()) == "RESTORE: @008 HAS FILES");
    CHECK(prjslot::request_restore((uint32_t)id8, "", true));   // replace: @008's files to the trash first
    settle();
    CHECK(has_all("@008", "eight") && !is_dir(entry(id8)) && info(newest("@008"), "why") == "restore");
    int r8 = newest("@008");
    CHECK(slurp(entry(r8) + "/lfo!%.txt") == "lfo new eight");
    CHECK(prjslot::request_restore((uint32_t)r8, "@012", false));   // to another slot
    settle();
    CHECK(has_all("@012", "new eight") && !is_dir(entry(r8)));
    int e11 = newest("@011");
    CHECK(prjslot::request_restore((uint32_t)e11, "@001", false));   // the current project: refused
    settle();
    CHECK(is_dir(entry(e11)) && has_all("@001", "presets"));
    printf("  restore: to its key, refused over files, replace (the replaced to the trash), to another key\n");
  }

  // ---- the web page's delete: into the trash, not deleted
  {
    CHECK(bundle::start_delete("@012", "@012"));
    settle();
    CHECK(has_none("@012") && info(newest("@012"), "why") == "web" && slurp(entry(newest("@012")) + "/lfo!%.txt") == "lfo new eight");
    char j[16384];
    CHECK(prjslot::trash_json(j, sizeof j) > 0 && strstr(j, "\"why\":\"web\"") && strstr(j, "\"key\":\"@012\""));
    printf("  the web page's delete: to the trash\n");
  }

  // ---- the cap: 64 entries, the oldest removed for good (logged), never more
  {
    std::vector<int> before = entries();
    CHECK(!before.empty());
    for (int k = 0; k < 70; ++k) {
      fixture("@100", "cap " + std::to_string(k));
      ev(CLEAR, 0, 100);
      settle();
    }
    std::vector<int> after = entries();
    CHECK((int)after.size() == prjslot::kTrashMax);
    CHECK(after.front() > before.back() - 1 || after.front() > before.front());   // the oldest went first
    CHECK(info(after.back(), "key") == "@100" && slurp(entry(after.back()) + "/lfo!%.txt") == "lfo cap 69");
    CHECK(!is_dir(entry(before.front())));
    printf("  the cap: %d entries kept, the oldest removed for good\n", prjslot::kTrashMax);
  }

  // ---- remove one, empty: for good (the web page, confirmed there)
  {
    std::vector<int> v = entries();
    CHECK(prjslot::request_remove((uint32_t)v.back()));
    settle();
    CHECK(!is_dir(entry(v.back())) && (int)entries().size() == (int)v.size() - 1);
    CHECK(prjslot::request_empty());
    settle();
    CHECK(entries().empty() && has_all("@001", "presets"));
    printf("  remove one, empty the trash\n");
  }

  // ---- a card core: every look, move and copy a card-queue request (none on the control core)
  {
    active(1, "PRESETS");
    settle();
    cardq::reset();
    cardq::set_worker(true);
    uint32_t posted0 = cardq::stats().posted;
    ev(SAVE, 1, 13);
    active(13, "PRESETS");
    for (int i = 0; i < 8; ++i) pass();
    CHECK(is("@001") && !is_dir("/.rytm/seq/@013"));   // posted, none run here
    for (int k = 0; k < 20000 && (prjslot::busy() || project_loading()); ++k) {
      host_card_core(0);
      while (cardq::service()) {}
      host_card_core(3);
      pass();
    }
    CHECK(is("@013") && has_all("@013", "presets") && has_all("@001", "presets"));
    CHECK(cardq::stats().posted - posted0 >= 30);   // looks of both keys, copies in pieces, listings
    cardq::set_worker(false);
    cardq::reset();
    settle();
    printf("  a card core: the copy all card-queue requests\n");
  }

  // ---- old mods again on this card (keyed by slot): the 10's name finds its slot (the most recently used)
  {
    prjslot::reset();
    prjslot::load();   // (a new session: the card's prjslot.txt)
    hello(3);
    name_project("SCRATCH");
    settle();
    CHECK(is("@010") && !strcmp(project_name, "SCRATCH") && has_all("@010", "scratch"));
    name_project("PRESETS");   // slots 1, 2, 6, 13 are PRESETS: 13 was the last used
    settle();
    CHECK(is("@013"));
    printf("  old mods on a card keyed by slot: a name finds its slot\n");
  }

  // ---- a card keyed by slot, a new session: the 10 held for a hello / a 4B; none in 3 s: name mode
  {
    prjslot::reset();
    prjslot::load();
    CHECK(prjslot::migrated() && prjslot::mode() == prjslot::kUndecided);
    name_project("SCRATCH");
    pass();
    CHECK(is("@013"));   // held
    g_t += 3100000;
    pass();
    CHECK(prjslot::mode() == prjslot::kName && is("@010"));
    prjslot::reset();
    prjslot::load();
    name_project("PRESETS");
    hello(7);            // a rui1052 hello: slot mode, the held 10 dropped
    active(2, "PRESETS");
    settle();
    CHECK(prjslot::mode() == prjslot::kSlot && is("@002"));
    printf("  a new session: the 10 held for the hello (3 s), then name mode\n");
  }

  // ---- power cuts (review HIGH 1, MEDIUM journal): each operation cut after k card changes, then a reboot (prjslot
  // from prjslot.txt, its journal completed forward): never a project split between two keys, never a half copy under
  // its slot, no staging left, the journal empty
  {
    auto reboot = [] {
      host_power_cut(-1);
      prjslot::reset();
      prjslot::load();
      hello(7);
      settle();
      CHECK(!prjslot::journal_line()[0]);
    };
    auto gone = [](const std::string& k) { CHECK(system(("rm -rf '" + g_card + "/.rytm/lfo/" + k + ".txt' '" + g_card + "/.rytm/ins/" + k + "' '" + g_card + "/.rytm/seq/" + k + "' '" + g_card + "/.rytm/projects/" + k + "' '" + g_card + "/.rytm/songs/" + k + "' '" + g_card + "/.rytm/kits/" + k + "'").c_str()) == 0); };
    auto entry_all = [](int id, const std::string& c) {   // every item of a fixture in trash entry id
      std::string e = entry(id);
      return id && slurp(e + "/lfo!%.txt") == "lfo " + c && slurp(e + "/ins!%/k001.txt") == "ins " + c &&
             slurp(e + "/seq!%/tracks.txt") == "tracks " + c && slurp(e + "/projects!%/samples.pool.txt") == "pool " + c &&
             slurp(e + "/songs!%/take-001.json") == "{\"song\":\"" + c + "\"}";
    };
    int copies = 0, trashes = 0, moves = 0, restores = 0;
    for (int k : {1, 2, 3, 5, 7, 9, 12, 16, 20, 26, 34, 60}) {
      // a copy (SAVE TO the empty slot 20): after the reboot @020 is the whole copy, or nothing
      active(1, "PRESETS");
      settle();
      gone("@020"), gone("@020~");
      ev(SAVE, 1, 20);
      host_power_cut(k);
      settle();
      reboot();
      CHECK(has_none("@020~"));
      CHECK(has_all("@020", "presets") || has_none("@020"));
      copies += has_all("@020", "presets");
      // a trash (CLEAR of slot 21, not the active one): all in the slot, or all in one entry
      fixture("@021", "t21");
      int before = newest("@021");
      ev(CLEAR, 0, 21);
      host_power_cut(k);
      settle();
      reboot();
      int e = newest("@021");
      bool in_entry = e != before && entry_all(e, "t21");
      CHECK(has_all("@021", "t21") || (has_none("@021") && in_entry));
      trashes += in_entry;
      gone("@021");
      // a move (a take from before slots, YES): all under the old name, or all under the slot
      fixture("CUTMOVE", "cm");
      gone("@022");
      active(22, "CUTMOVE");
      for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
      CHECK(prjslot::ask() == prjslot::kAskLegacy);
      host_power_cut(k);
      prjslot::answer(true);
      settle();
      reboot();
      CHECK(has_all("CUTMOVE", "cm") || (has_none("CUTMOVE") && has_all("@022", "cm")));
      moves += has_all("@022", "cm");
      gone("CUTMOVE"), gone("@022");
      // a restore: all in the entry, or all back under its key
      active(1, "PRESETS");
      settle();
      fixture("@023", "r23");
      ev(DELETE, 0, 23);
      settle();
      int r = newest("@023");
      CHECK(entry_all(r, "r23") && has_none("@023"));
      CHECK(prjslot::request_restore((uint32_t)r, "", false));
      host_power_cut(k);
      settle();
      reboot();
      CHECK((entry_all(r, "r23") && has_none("@023")) || (has_all("@023", "r23") && !is_file(entry(r) + "/lfo!%.txt")));
      restores += has_all("@023", "r23");
      gone("@023");
    }
    CHECK(copies > 0 && trashes > 0 && moves > 0 && restores > 0);   // (some cuts came late enough to complete)
    printf("  power cuts: copy %d, trash %d, move %d, restore %d of 12 completed by the journal; none split\n", copies,
           trashes, moves, restores);
  }

  // ---- review HIGH 2: a trash that fails leaves the slot's files; the copy then refuses, no mark: the switch asks
  {
    active(1, "PRESETS");
    settle();
    fixture("@024", "old24");
    CHECK(system(("mv '" + g_card + "/.rytm/trash' '" + g_card + "/.rytm/trash.keep' 2>/dev/null; true").c_str()) == 0);
    put("/.rytm/trash", "not a folder");   // (the trash cannot be made: the trash job fails)
    uint32_t f0 = prjslot::failures();
    ev(SAVE, 1, 24);
    active(24, "PRESETS");
    for (int i = 0; i < 400 && !prjslot::ask(); ++i) pass();
    CHECK(prjslot::failures() >= f0 + 2);   // the trash, then the copy that would have gone over the old files
    CHECK(prjslot::ask() == prjslot::kAskMismatch && has_all("@024", "old24"));
    prjslot::answer(true);   // YES: the files kept (the user's choice)
    settle();
    CHECK(is("@024") && has_all("@024", "old24"));
    CHECK(system(("rm -f '" + g_card + "/.rytm/trash'; mv '" + g_card + "/.rytm/trash.keep' '" + g_card + "/.rytm/trash' 2>/dev/null; true").c_str()) == 0);
    printf("  a failed trash: the copy refused, the old files kept, the name question asked\n");
  }

  // ---- review HIGH 3: the FLUSH never runs the card on the control core: with the card core held, the job waits
  {
    active(1, "PRESETS");
    settle();
    CHECK(system(("rm -rf '" + g_card + "/.rytm/seq/@025' '" + g_card + "/.rytm/lfo/@025.txt'").c_str()) == 0);
    put("/.rytm/lfo/@001.txt", "# Sidecar LFOs\n");
    active(2, "PRESETS"), settle(), active(1, "PRESETS"), settle();   // (the readable LFO file loaded)
    cardq::reset();
    cardq::set_worker(true);
    char o[200];
    CHECK(lfo::command("lfo=2:dep:50", o, sizeof o) && lfo::dirty());
    uint32_t back0 = cardq::stats().taken_back;
    ev(SAVE, 1, 25);
    active(25, "PRESETS");
    for (int i = 0; i < 2000; ++i) pass();   // the card core never runs
    CHECK(prjslot::busy() && !is_dir("/.rytm/seq/@025") && cardq::stats().taken_back == back0);
    for (int k = 0; k < 40000 && (prjslot::busy() || project_loading()); ++k) {
      host_card_core(0);
      while (cardq::service()) {}
      host_card_core(3);
      pass();
    }
    for (int k = 0; k < 400; ++k) host_card_core(0), (void)cardq::service(), host_card_core(3), pass();
    cardq::set_worker(false);
    cardq::reset();
    settle();
    CHECK(is("@025") && is_dir("/.rytm/seq/@025") && slurp("/.rytm/lfo/@025.txt").find(" 50 ") != std::string::npos);
    printf("  the FLUSH with the card core held: the job waits, nothing taken back on the control core\n");
  }

  // ---- review MEDIUM: edits made while a SAVE AS copies land in the new slot (the Rytm is on it), not the old one
  {
    active(1, "PRESETS");
    settle();
    CHECK(system(("rm -rf '" + g_card + "/.rytm/seq/@026' '" + g_card + "/.rytm/lfo/@026.txt'").c_str()) == 0);
    cardq::reset();
    cardq::set_worker(true);
    auto pump = [] { host_card_core(0); while (cardq::service()) {} host_card_core(3); pass(); };
    ev(SAVE, 1, 26);
    active(26, "PRESETS");
    for (int k = 0; k < 20000 && !prjslot::moving(); ++k) pump();
    CHECK(prjslot::moving());
    char o[200];
    CHECK(lfo::command("lfo=3:dep:44", o, sizeof o) && lfo::dirty());
    for (int k = 0; k < 40000 && (prjslot::busy() || project_loading()); ++k) pump();
    for (int k = 0; k < 200; ++k) pump();
    cardq::set_worker(false);
    cardq::reset();
    settle();
    CHECK(is("@026"));
    CHECK(slurp("/.rytm/lfo/@026.txt").find(" 44 ") != std::string::npos);
    CHECK(slurp("/.rytm/lfo/@001.txt").find(" 44 ") == std::string::npos);
    printf("  edits during a SAVE AS copy: in the new slot's files, not the old one's\n");
  }

  // ---- review MEDIUM: a restore never deletes what it does not know: kept in the entry
  {
    active(1, "PRESETS");
    settle();
    fixture("@027", "r27");
    ev(DELETE, 0, 27);
    settle();
    int r = newest("@027");
    put(entry(r) + "/future!%.dat", "a newer build's");
    CHECK(prjslot::request_restore((uint32_t)r, "", false));
    settle();
    CHECK(has_all("@027", "r27") && slurp(entry(r) + "/future!%.dat") == "a newer build's" && is_file(entry(r) + "/info.txt"));
    printf("  restore: items it does not know stay in the entry\n");
  }

  // ---- review MEDIUM: a cap purge that fails skips to the next oldest (never stops the trash)
  {
    active(1, "PRESETS");
    settle();
    while ((int)entries().size() < prjslot::kTrashMax) {
      fixture("@100", "fill");
      ev(CLEAR, 0, 100);
      settle();
    }
    std::vector<int> v = entries();
    std::string locked = g_card + entry(v[0]) + "/ins!%";
    put(entry(v[0]) + "/ins!%/k001.txt", "x");
    CHECK(system(("chmod 555 '" + locked + "'").c_str()) == 0);   // (its file cannot be removed)
    fixture("@100", "after");
    ev(CLEAR, 0, 100);
    settle();
    std::vector<int> w = entries();   // (this one past the cap, the stuck one remembered)
    CHECK(is_dir(entry(v[0])) && is_dir(entry(v[1])) && info(w.back(), "key") == "@100" && has_none("@100") &&
          (int)w.size() == prjslot::kTrashMax + 1);
    fixture("@100", "after2");
    ev(CLEAR, 0, 100);
    settle();
    CHECK(is_dir(entry(v[0])) && !is_dir(entry(v[1])) && has_none("@100"));   // (the next: the next oldest goes)
    CHECK(system(("chmod 755 '" + locked + "'").c_str()) == 0);
    printf("  a cap purge that fails: that trash goes past the cap, the next one removes the next oldest\n");
  }

  // ---- review MEDIUM: the web page's delete checked again when it runs (the Rytm switched to it meanwhile)
  {
    active(1, "PRESETS");
    settle();
    fixture("@028", "w28");
    projects::note("@028", "WEB28");
    uint32_t f0 = prjslot::failures();
    CHECK(bundle::start_delete("@028", "@028"));
    active(28, "WEB28");   // (before the control core's next tick)
    settle();
    CHECK(is("@028") && has_all("@028", "w28") && prjslot::failures() == f0 + 1 && projects::find("@028"));
    printf("  the web delete of a project the Rytm switched to meanwhile: refused\n");
  }

  // ---- review MEDIUM: the first ACTIVE of a boot announces the project's tables (as the first 10 did)
  {
    static std::vector<std::vector<uint8_t>> sent;
    hooks.to_rytm = [](const uint8_t* m, int n) { sent.emplace_back(m, m + n); };
    active(1, "PRESETS");
    settle();
    prjslot::reset();
    prjslot::load();
    project_seen = false;
    sent.clear();
    hello(7);
    active(1, "PRESETS");
    settle();
    bool pool14 = false;
    for (auto& m : sent) pool14 = pool14 || (m.size() > 5 && m[4] == 0x14);
    CHECK(pool14 && project_seen);
    hooks.to_rytm = [](const uint8_t*, int) {};
    printf("  a slot-mode boot: the first ACTIVE announces the tables\n");
  }

  // ---- 1052b: the Rytm's slot list - empty slots the index knows go to the trash; projects from before slots whose name
  // is in one slot move there silently; a name in two slots stays (asked when one is opened)
  {
    active(1, "PRESETS");
    settle();
    fixture("@029", "g29");
    projects::note("@029", "GONE29");
    fixture("SOLO", "solo");
    projects::note("SOLO");
    fixture("TWIN", "twin");
    projects::note("TWIN");
    std::vector<std::pair<int, std::string>> list = {{30, "SOLO"}, {31, "TWIN"}, {32, "TWIN"}};
    for (int i = 0; i < projects::count(); ++i) {   // (every slot the index knows is still there, but 29)
      const projects::Info* e = projects::at(i);
      int sl = prjslot::slot_of(e->key);
      if (sl >= 1 && sl != 29 && sl != 30 && sl != 31 && sl != 32) list.push_back({sl, e->rytm});
    }
    // review round 2, N2: a list is acted on only whole (16 messages), with its count, the active slot in it, and the
    // same twice in a row since the connect
    send_list(list, 5);   // a message lost
    send_list(list, 5);
    settle();
    CHECK(has_all("@029", "g29") && has_all("SOLO", "solo"));
    send_list(list, -1, (int)list.size() + 1);   // the count does not match
    send_list(list, -1, (int)list.size() + 1);
    settle();
    CHECK(has_all("@029", "g29") && has_all("SOLO", "solo"));
    send_list({}, -1, 0);   // an empty table (count 0)
    send_list({}, -1, 0);
    settle();
    CHECK(has_all("@029", "g29") && has_all("SOLO", "solo") && has_all("@001", "presets") == has_all("@001", "presets"));
    {
      auto noact = list;   // the active slot (1) not in it
      noact.erase(std::remove_if(noact.begin(), noact.end(), [](const std::pair<int, std::string>& p) { return p.first == 1; }), noact.end());
      send_list(noact);
      send_list(noact);
      settle();
      CHECK(has_all("@029", "g29") && has_all("SOLO", "solo"));
    }
    send_list(list);   // whole, but once: nothing yet
    settle();
    CHECK(has_all("@029", "g29") && has_all("SOLO", "solo"));
    hello(7);          // a connect between: the count starts again
    send_list(list);
    settle();
    CHECK(has_all("@029", "g29"));
    send_list(list);   // the second, the same: acted on
    settle();
    CHECK(has_none("@029") && info(newest("@029"), "why") == "gone");
    CHECK(has_none("SOLO") && has_all("@030", "solo") && projects::find("@030"));
    CHECK(has_all("TWIN", "twin") && has_none("@031") && has_none("@032"));
    active(31, "TWIN");
    for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
    CHECK(prjslot::ask() == prjslot::kAskLegacy);
    prjslot::answer(true);
    settle();
    CHECK(has_all("@031", "twin") && has_none("TWIN"));
    active(32, "TWIN");
    settle();
    CHECK(!prjslot::ask() && is("@032") && has_none("@032"));   // (never merged: the other TWIN starts empty)
    printf("  the slot list: gone to the trash, a unique old name moved, two of one name asked\n");
  }

  // ---- 1052b: LOAD of the active slot again resolves its pending paste at once (no wait for the slot to be left)
  {
    fixture("@033", "o33");
    projects::note("@033", "OLD33");
    active(33, "OLD33");
    settle();
    fixture("@036", "src36");
    projects::note("@036", "SRC36");
    ev(PASTE, 36, 33);
    settle();
    CHECK(has_all("@033", "o33"));
    ev(LOAD, 0, 33);
    active(33, "SRC36");
    settle();
    CHECK(is("@033") && has_all("@033", "src36") && info(newest("@033"), "why") == "paste");
    printf("  LOAD of the active slot: its pending paste resolved at once\n");
  }

  // ---- review MEDIUM: song takes are copied first, with idle_save running (not held), and only the takes
  {
    active(1, "PRESETS");
    settle();
    put("/.rytm/songs/@001/take-002.json", std::string(3u << 20, 't'));
    put("/.rytm/songs/@001/journal.jsonl", "an open journal");
    CHECK(system(("rm -rf '" + g_card + "/.rytm/seq/@034' '" + g_card + "/.rytm/songs/@034'").c_str()) == 0);
    cardq::reset();
    cardq::set_worker(true);
    auto pump = [] { host_card_core(0); while (cardq::service()) {} host_card_core(3); pass(); };
    ev(SAVE, 1, 34);
    bool seen_unheld = false;
    for (int k = 0; k < 40000 && prjslot::busy(); ++k) {
      pump();
      if (is_file("/.rytm/songs/@034~/take-002.json") && !prjslot::moving()) seen_unheld = true;
    }
    cardq::set_worker(false);
    cardq::reset();
    settle();
    CHECK(seen_unheld);
    CHECK(slurp("/.rytm/songs/@034/take-002.json") == std::string(3u << 20, 't') && !is_file("/.rytm/songs/@034/journal.jsonl"));
    CHECK(!is_dir("/.rytm/songs/@034~"));
    printf("  song takes: copied first in pieces with the saves running; the open journal left out\n");
  }

  // ---- review MEDIUM: slot mode cancels a name question from the 10s before it
  {
    prjslot::reset();
    prjslot::load();
    hello(3);
    fixture("@035", "q35");
    project_switch_named("@035", "Q35", false);
    settle();
    name_project("ZZNEW");   // (name mode: a name with no files after one with: asked)
    for (int i = 0; i < 50 && rename::state() != rename::kAsk; ++i) pass();
    CHECK(rename::state() == rename::kAsk);
    hello(7);
    CHECK(rename::state() == rename::kIdle && !rename::busy());
    active(1, "PRESETS");
    settle();
    CHECK(is("@001"));
    printf("  slot mode: a pending name question dropped\n");
  }

  // ---- review round 2: N1 (a name key with a space in the journal), N3 (the live place), N4 (the boot project reloaded
  // after a recovery), the pool lists rekeyed with the copy, SAVE TO cut between its trash and its copy, a restore's
  // recovery keeping its entry, SONG RECORD bounding the FLUSH
  {
    auto reboot = [] {
      host_power_cut(-1);
      prjslot::reset();
      prjslot::load();
      hello(7);
      settle();
      CHECK(!prjslot::journal_line()[0]);
    };
    auto gone = [](const std::string& k) { CHECK(system(("rm -rf '" + g_card + "/.rytm/lfo/" + k + ".txt' '" + g_card + "/.rytm/ins/" + k + "' '" + g_card + "/.rytm/seq/" + k + "' '" + g_card + "/.rytm/projects/" + k + "' '" + g_card + "/.rytm/songs/" + k + "' '" + g_card + "/.rytm/kits/" + k + "'").c_str()) == 0); };
    auto entry_all = [](int id, const std::string& c) {
      std::string e = entry(id);
      return id && slurp(e + "/lfo!%.txt") == "lfo " + c && slurp(e + "/ins!%/k001.txt") == "ins " + c &&
             slurp(e + "/seq!%/tracks.txt") == "tracks " + c && slurp(e + "/projects!%/samples.pool.txt") == "pool " + c &&
             slurp(e + "/songs!%/take-001.json") == "{\"song\":\"" + c + "\"}";
    };
    // N1: the legacy "MY SONG" taken by slot 46 and deleted on the web page, each cut after k changes, then a reboot
    int moved = 0, trashed = 0;
    for (int k : {1, 2, 3, 5, 7, 9, 12, 16, 20, 30, 60}) {
      active(1, "PRESETS");
      settle();
      fixture("MY SONG", "my");
      gone("@046");
      active(46, "MY SONG");
      for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
      CHECK(prjslot::ask() == prjslot::kAskLegacy);
      host_power_cut(k);
      prjslot::answer(true);
      settle();
      reboot();
      CHECK(has_all("MY SONG", "my") || (has_none("MY SONG") && has_all("@046", "my")));
      moved += has_all("@046", "my");
      gone("MY SONG"), gone("@046");
      active(1, "PRESETS");
      settle();
      fixture("MY SONG", "web");
      projects::note("MY SONG");
      int before = newest("MY SONG");
      CHECK(bundle::start_delete("MY SONG", "MY SONG"));
      host_power_cut(k);
      settle();
      reboot();
      int e = newest("MY SONG");
      bool in_entry = e != before && entry_all(e, "web");
      CHECK(has_all("MY SONG", "web") || (has_none("MY SONG") && in_entry));
      trashed += in_entry;
      gone("MY SONG");
    }
    CHECK(moved > 0 && trashed > 0);
    printf("  a spaced name key (MY SONG) in the journal: move %d, trash %d of 11 completed, none split\n", moved, trashed);

    // N3: a target that appears before the place: the placed ones back into the stage, the stage to the trash, the
    // slot suspect (its switch asks), nothing reported as copied
    active(1, "PRESETS");
    settle();
    gone("@042"), gone("@042~");
    cardq::reset();
    cardq::set_worker(true);
    auto pump = [] { host_card_core(0); while (cardq::service()) {} host_card_core(3); pass(); };
    uint32_t f0 = prjslot::failures();
    ev(SAVE, 1, 42);
    active(42, "PRESETS");
    for (int k = 0; k < 20000 && !is_dir("/.rytm/seq/@042~"); ++k) pump();
    CHECK(is_dir("/.rytm/seq/@042~"));
    put("/.rytm/lfo/@042.txt", "intruder");
    for (int k = 0; k < 40000 && !prjslot::ask(); ++k) pump();
    cardq::set_worker(false);
    cardq::reset();
    CHECK(prjslot::ask() == prjslot::kAskMismatch && prjslot::failures() > f0);
    CHECK(slurp("/.rytm/lfo/@042.txt") == "intruder" && !is_dir("/.rytm/seq/@042") && !is_dir("/.rytm/seq/@042~"));
    CHECK(info(newest("@042~"), "why") == "failed" && is_dir(entry(newest("@042~")) + "/seq!%"));
    prjslot::answer(false);   // NO: the intruder to the trash
    settle();
    CHECK(is("@042") && has_none("@042"));
    printf("  N3: a target there at the place: undone into the stage, the stage trashed, the slot asks\n");

    // N4: a place a cut stopped, of the project the Sidecar boots on: completed, then the project loaded again
    active(1, "PRESETS");
    settle();
    gone("@043"), gone("@043~");
    std::string lf = slurp("/.rytm/lfo/@025.txt");   // (an LFO file this build reads, LFO 2's depth 50)
    CHECK(lf.find(" 50 ") != std::string::npos);
    put("/.rytm/lfo/@043~.txt", lf);
    put("/.rytm/seq/@043~/tracks.txt", "tracks n4");
    project_switch_named("@043", "N43", false);
    settle();
    CHECK(is("@043") && lfo::get(1).depth != 50);
    {
      char t[200];
      snprintf(t, sizeof t, "mode=slot\ntrash_next=%u\njob=place\t@001\t@043\n", (unsigned)prjslot::trash_next());
      put("/.rytm/prjslot.txt", t);
    }
    prjslot::reset();
    prjslot::load();
    hello(7);
    settle();
    CHECK(!prjslot::journal_line()[0] && slurp("/.rytm/lfo/@043.txt") == lf && !is_file("/.rytm/lfo/@043~.txt"));
    CHECK(is("@043") && lfo::get(1).depth == 50);   // (loaded again: not the empty project of the boot)
    printf("  N4: a recovery that touched the boot project: it is loaded again\n");

    // the pool lists rekeyed with the copy: a pool edit held during a SAVE AS copy lands in the new slot
    gone("@044"), gone("@045");
    put("/.rytm/seq/@044/tracks.txt", "tracks p44");
    wg::write_file(g_card + "/Samples/pp.wav", wg::wav_bytes(480, 1, 16, 48000, 1, "", [](int, int) { return 0.1; }));
    active(44, "POOLS");
    settle();
    for (int i = 0; i < 400 && !(pools::ready() && !strcmp(pools::project(), "@044")); ++i) pools::step(), pass();
    CHECK(pools::ready() && !strcmp(pools::project(), "@044"));
    cardq::reset();
    cardq::set_worker(true);
    auto pump2 = [] { host_card_core(0); while (cardq::service()) {} host_card_core(3); pools::step(); pass(); };
    ev(SAVE, 44, 45);
    active(45, "POOLS");
    for (int k = 0; k < 20000 && !prjslot::moving(); ++k) pump2();
    CHECK(prjslot::moving());
    int row = -1;
    CHECK(pools::add_file(pools::kSamples, 0, "/Samples/pp.wav", &row) == pools::kOk);
    for (int k = 0; k < 40000 && (prjslot::busy() || project_loading() || pools::saving()); ++k) pump2();
    for (int k = 0; k < 400; ++k) pump2();
    cardq::set_worker(false);
    cardq::reset();
    settle();
    CHECK(is("@045"));
    CHECK(slurp("/.rytm/projects/@045/samples.pool.txt").find("pp.wav") != std::string::npos);
    CHECK(slurp("/.rytm/projects/@044/samples.pool.txt").find("pp.wav") == std::string::npos);
    printf("  a pool edit during a SAVE AS copy: in the new slot's pool list, not the old one's\n");

    // SAVE TO over an occupied slot, cut after k changes (its trash and its copy one journaled operation): after the
    // reboot the slot holds its old files or the whole copy - never nothing (the Rytm's slot is the copied project)
    int whole = 0, old = 0;
    for (int k : {1, 3, 6, 9, 12, 15, 18, 24, 30, 40, 60}) {
      active(1, "PRESETS");
      settle();
      gone("@047"), gone("@047~");
      fixture("@047", "old47");
      projects::note("@047", "OLD47");
      ev(SAVE, 1, 47);
      host_power_cut(k);
      settle();
      reboot();
      bool is_old = has_all("@047", "old47");
      bool is_copy = slurp("/.rytm/seq/@047/tracks.txt") == slurp("/.rytm/seq/@001/tracks.txt") &&
                     slurp("/.rytm/seq/@047/A01.sqp") == slurp("/.rytm/seq/@001/A01.sqp") &&
                     slurp("/.rytm/projects/@047/samples.pool.txt") == slurp("/.rytm/projects/@001/samples.pool.txt");
      CHECK(is_old || is_copy);
      CHECK(!has_none("@047"));
      whole += is_copy, old += is_old;
    }
    CHECK(whole > 0);
    printf("  SAVE TO cut after k changes: %d the whole copy, %d the old files, never nothing\n", whole, old);

    // a PASTE onto the active slot resolved when it is left, cut after k changes: the trash and the copy one journaled
    // operation (as on_event's PASTE): the slot holds its old files or the whole copy after the reboot - never nothing
    int pwhole = 0, pold = 0;
    for (int k : {1, 3, 6, 9, 12, 15, 18, 24, 30, 40, 60}) {
      active(1, "PRESETS");
      settle();
      gone("@050"), gone("@050~");
      fixture("@050", "old50");
      projects::note("@050", "OLD50");
      active(50, "OLD50");
      settle();
      CHECK(is("@050"));
      ev(PASTE, 1, 50);
      host_power_cut(k);
      active(1, "PRESETS");   // left: resolved now
      settle();
      reboot();
      bool is_old = has_all("@050", "old50");
      bool is_copy = slurp("/.rytm/seq/@050/tracks.txt") == slurp("/.rytm/seq/@001/tracks.txt") &&
                     slurp("/.rytm/seq/@050/A01.sqp") == slurp("/.rytm/seq/@001/A01.sqp") &&
                     slurp("/.rytm/projects/@050/samples.pool.txt") == slurp("/.rytm/projects/@001/samples.pool.txt");
      CHECK(is_old || is_copy);
      CHECK(!has_none("@050"));
      pwhole += is_copy, pold += is_old;
    }
    CHECK(pwhole > 0);
    printf("  a pending PASTE resolved, cut after k changes: %d the whole copy, %d the old files, never nothing\n", pwhole, pold);

    // a restore cut, then a reboot: an entry that keeps items keeps its info.txt
    active(1, "PRESETS");
    settle();
    fixture("@048", "r48");
    ev(DELETE, 0, 48);
    settle();
    int r = newest("@048");
    put(entry(r) + "/future!%.dat", "kept");
    CHECK(prjslot::request_restore((uint32_t)r, "", false));
    host_power_cut(6);
    settle();
    reboot();
    CHECK(is_file(entry(r) + "/info.txt") && slurp(entry(r) + "/future!%.dat") == "kept");
    CHECK(has_all("@048", "r48") || entry_all(r, "r48"));
    printf("  a restore's recovery: the entry with items it does not know keeps its info.txt\n");

    // SONG RECORD armed on the project: the copy waits at most kFlushMs, then goes on without the songs folder
    active(1, "PRESETS");
    settle();
    gone("@049"), gone("@049~");
    char o[200];
    song::command("song=arm", o, sizeof o);
    for (int i = 0; i < 20; ++i) pass();
    int st = song::status().state;
    if (st == song::kArming || st == song::kArmed) {
      ev(SAVE, 1, 49);
      settle();
      CHECK(is_dir("/.rytm/seq/@049") && !is_dir("/.rytm/songs/@049"));
      song::command("song=disarm", o, sizeof o);
      for (int i = 0; i < 20; ++i) pass();
      printf("  SONG RECORD armed: the copy went on after %u ms without the songs folder\n", (unsigned)prjslot::kFlushMs);
    } else {
      printf("  SONG RECORD did not arm here (%s): its bound not exercised\n", o);
    }

    // round 5, H1: a 4C SAVE KIT (kit 2 -> 3) while a SAVE AS copy keeps prjslot busy, then the Rytm loads another slot:
    // the kit report is stale (dropped), the loaded slot's kit files untouched; a LOST also says kits may be missed
    active(1, "PRESETS");
    settle();
    gone("@056"), gone("@056~"), gone("@057");
    put("/.rytm/lfo/@057/k003.txt", "# Sidecar LFOs\n1 V3 2 1 64 0 0 57 0\n");
    projects::note("@057", "OTHER57");
    uint32_t stale = kits::stats().stale;
    ev(SAVE, 1, 56);   // SAVE AS slot 56: a copy job (prjslot busy)
    CHECK(prjslot::busy());
    sx({0x4C, 0x01, 0x01, 0, 3, 0, 2});   // 4C SAVE kit 3 from kit 2, waiting
    ev(LOAD, 0, 57);
    active(57, "OTHER57");
    settle();
    CHECK(is("@057") && kits::stats().stale == stale + 1);
    CHECK(!is_dir("/.rytm/kits/@057") && slurp("/.rytm/lfo/@057/k003.txt") == "# Sidecar LFOs\n1 V3 2 1 64 0 0 57 0\n");
    uint32_t lost = kits::stats().lost;
    ev(LOST, 3, 0);
    settle();
    CHECK(kits::stats().lost == lost + 1);
    printf("  a kit report waiting behind a SAVE AS copy, then LOAD of another slot: dropped, that slot's kits untouched; "
           "a LOST is said for the kits too\n");
    // round 6, N3: in slot mode a 10 with another name (0021's, on its own path: a rename, or late) drops nothing
    active(1, "PRESETS");
    settle();
    gone("@058"), gone("@058~");
    stale = kits::stats().stale;
    uint32_t saves = kits::stats().saves;
    ev(SAVE, 1, 58);
    CHECK(prjslot::busy());
    sx({0x4C, 0x01, 0x01, 0, 1, 0, 1});   // 4C SAVE kit 1, waiting behind the copy
    name_project("SOMETHING ELSE");
    settle();
    CHECK(kits::stats().stale == stale && kits::stats().saves == saves + 1);
    printf("  slot mode: a 10 with another name while a kit report waits: the report kept (no drop)\n");
  }

  // ---- the Rytm's PROJECT page asks (its text fits: check.h), YES / NO on the Rytm's keys
  {
    active(1, "PRESETS");
    settle();
    fixture("@014", "page");
    projects::note("@014", "UNDO");
    ui::remote_reset();
    uint8_t open1[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 0x01, 0x00, 0x01, 0x00, 0xF7};
    ui::remote_sysex(open1, sizeof open1);
    active(14, "ANOTHER NAME 14");   // (@014 has files, its index name UNDO: a name changed without an event)
    for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
    CHECK(prjslot::ask() == prjslot::kAskMismatch);
    for (int t = 0; t < 40; ++t) ui::remote_tick(1000 + 30 * t);
    CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "PROJECT"));
    std::string sc = screen_text();
    CHECK(sc.find("SLOT 014: A NEW NAME") != std::string::npos && sc.find("NOW ANOTHER NAME 14") != std::string::npos &&
          sc.find("WAS UNDO") != std::string::npos && sc.find("YES: KEEP  NO: TRASH") != std::string::npos);
    uint8_t fb[ui::kBytes];
    ui::preview_sent(fb);
    CHECK(rui_png(rui_png_dir("RUI_PAGES", "build/rui_pages"), "project_slot_mismatch.png", fb));
    uint8_t yes_key[] = {0xF0, 0x7D, 0x52, 0x54, 0x31, 0x01, 0x00, 0x44, 0x01, 0xF7};
    ui::remote_sysex(yes_key, sizeof yes_key);
    settle();
    CHECK(!prjslot::ask() && is("@014") && !strcmp(project_name, "ANOTHER NAME 14"));
    fixture("LEGACY NAME", "legacy");
    active(15, "LEGACY NAME");
    for (int i = 0; i < 50 && !prjslot::ask(); ++i) pass();
    CHECK(prjslot::ask() == prjslot::kAskLegacy);
    for (int t = 0; t < 40; ++t) ui::remote_tick(3000 + 30 * t);
    sc = screen_text();
    CHECK(sc.find("FILES FROM BEFORE SLOTS") != std::string::npos && sc.find("NAME LEGACY NAME") != std::string::npos &&
          sc.find("FOR SLOT 015?") != std::string::npos && sc.find("YES: TAKE  NO: NOT THIS") != std::string::npos);
    ui::preview_sent(fb);
    CHECK(rui_png(rui_png_dir("RUI_PAGES", "build/rui_pages"), "project_slot_legacy.png", fb));
    uint8_t no_key[] = {0xF0, 0x7D, 0x52, 0x54, 0x31, 0x01, 0x00, 0x45, 0x01, 0xF7};
    ui::remote_sysex(no_key, sizeof no_key);
    settle();
    CHECK(!prjslot::ask() && is("@015") && has_none("@015") && has_all("LEGACY NAME", "legacy"));
    printf("  the PROJECT page asks (its text fits), the Rytm's YES / NO answer\n");
  }

  printf("test_prjslot: ok\n");
  return 0;
}
