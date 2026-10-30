// core/tests/test_rename.cpp - a project renamed on the Rytm keeps its Sidecar files (UX review B1,
// core/bundle/rename.h): the Rytm naming a project with no files right after one with files asks first (the switch
// waits, the old project's state stays); YES moves every per-project file and folder to the new key (only names
// change) and then switches; NO switches to an empty project and moves nothing; a move that fails half way is undone
// (nothing lost); no question when the new name has files of its own, the old one has none, or both are one key; the
// Rytm naming yet another project, or the old one again, drops the question; with a card core every look and move is a
// card-queue request (none on the control core); the Rytm's PROJECT page asks it (its text fits).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "bundle/rename.h"
#include "check.h"
#include "rui_png.h"
#include "engine.h"
#include "fx/insert.h"
#include "hooks.h"
#include "lfo.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "seq/seq.h"
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
  system(("mkdir -p \"$(dirname '" + g_card + p + "')\"").c_str());
  FILE* f = fopen((g_card + p).c_str(), "wb");
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static void name_project(const char* n) {   // the Rytm's 10 <name>
  uint8_t m[40] = {0xF0, 0x7D, 0x52, 0x54, 0x10};
  int l = (int)strlen(n);
  memcpy(m + 5, n, (size_t)l);
  m[5 + l] = 0xF7;
  engine_sysex(m, 6 + l);
}
// a project's files: an LFO file, an insert folder, a sequence folder, the pool lists' folder, a song
static void fixture(const char* key) {
  std::string k = key;
  put("/.rytm/lfo/" + k + ".txt", "lfo " + k);
  put("/.rytm/ins/" + k + "/k001.txt", "ins " + k);
  put("/.rytm/seq/" + k + "/tracks.txt", "tracks " + k);
  put("/.rytm/projects/" + k + "/samples.pool.txt", "pool " + k);
  put("/.rytm/songs/" + k + "/take-001.json", "{\"song\":\"" + k + "\"}");
}
static bool has_all(const char* key, const char* content_of) {
  std::string k = key, c = content_of;
  return slurp("/.rytm/lfo/" + k + ".txt") == "lfo " + c && slurp("/.rytm/ins/" + k + "/k001.txt") == "ins " + c &&
         slurp("/.rytm/seq/" + k + "/tracks.txt") == "tracks " + c &&
         slurp("/.rytm/projects/" + k + "/samples.pool.txt") == "pool " + c &&
         slurp("/.rytm/songs/" + k + "/take-001.json") == "{\"song\":\"" + c + "\"}";
}
static bool has_none(const char* key) {
  std::string k = key;
  return !is_file("/.rytm/lfo/" + k + ".txt") && !is_dir("/.rytm/ins/" + k) && !is_dir("/.rytm/seq/" + k) &&
         !is_dir("/.rytm/projects/" + k) && !is_dir("/.rytm/songs/" + k);
}
static void settle() {   // (a project load's RAM store fill first: the LFOs and inserts switch once it is full)
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
  for (int i = 0; i < 64; ++i) idle_save();
}
static std::string screen_text() {   // the remote frame's text items, one line
  uint8_t t[36 * ui::kTextSlots];
  int n = ui::sent_texts(t, ui::kTextSlots);
  std::string s;
  for (int i = 0; i < n; ++i) s += std::string((const char*)t + 36 * i + 4) + "|";
  return s;
}

int main() {
  char tmpl[] = "/tmp/rename-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(system(("mkdir -p '" + g_card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = [](const uint8_t*, int) {};
  CHECK(engine_begin() && card_ok);
  cardq::reset();
  rename::reset();

  // ---- the first name: no question (nothing before it)
  fixture("SHARED SET");
  name_project("SHARED SET");
  CHECK(!strcmp(project, "SHARED SET") && rename::state() == rename::kIdle);

  // ---- renamed on the Rytm: asked; the switch waits (the old project stays)
  uint32_t asks = rename::asks();
  name_project("SHARED SET 2");
  CHECK(rename::state() == rename::kAsk && rename::asks() == asks + 1);
  CHECK(!strcmp(project, "SHARED SET") && !strcmp(rename::from(), "SHARED SET") && !strcmp(rename::to(), "SHARED SET 2"));
  CHECK(rename::files() == 5);
  name_project("SHARED SET 2");   // the Rytm's repeat: still the one question
  CHECK(rename::state() == rename::kAsk && rename::asks() == asks + 1 && !strcmp(project, "SHARED SET"));
  // YES: every file and folder under the new key, none left under the old; then the switch
  CHECK(rename::answer(true) == 5);
  CHECK(!strcmp(project, "SHARED SET 2") && rename::state() == rename::kIdle);
  CHECK(has_all("SHARED SET 2", "SHARED SET") && has_none("SHARED SET"));
  CHECK(!strcmp(rename::last(), "MOVED 5 TO SHARED SET 2"));
  settle();

  // ---- NO: a new project, nothing moved
  name_project("NEW ONE");
  CHECK(rename::state() == rename::kAsk && !strcmp(project, "SHARED SET 2"));
  CHECK(rename::answer(false) == 0);
  CHECK(!strcmp(project, "NEW ONE") && has_all("SHARED SET 2", "SHARED SET") && has_none("NEW ONE"));
  settle();

  // ---- no question: the new name has files of its own; the old one has none; both are one key
  fixture("JAM");
  name_project("JAM");   // NEW ONE (nothing) -> JAM
  CHECK(rename::state() == rename::kIdle && !strcmp(project, "JAM"));
  fixture("LIVE");
  name_project("LIVE");   // JAM (files) -> LIVE (files)
  CHECK(rename::state() == rename::kIdle && !strcmp(project, "LIVE"));
  name_project("LIVE.");  // the same key ("LIVE")
  CHECK(rename::state() == rename::kIdle && !strcmp(project, "LIVE."));
  settle();

  // ---- the Rytm names yet another project meanwhile, or the old one again: the question goes, nothing moved
  name_project("LIVE 2");
  CHECK(rename::state() == rename::kAsk);
  name_project("JAM");   // another (with files): switched to it, LIVE's files where they were
  CHECK(rename::state() == rename::kIdle && !strcmp(project, "JAM") && has_all("LIVE", "LIVE"));
  name_project("JAM 2");
  CHECK(rename::state() == rename::kAsk);
  name_project("JAM");   // back: no question, nothing switched
  CHECK(rename::state() == rename::kIdle && !strcmp(project, "JAM") && has_all("JAM", "JAM") && has_none("JAM 2"));
  settle();

  // ---- a move that fails half way is undone: nothing lost, the switch goes on as NO
  name_project("JAM 3");
  CHECK(rename::state() == rename::kAsk);
  host_power_cut(2);   // two more changes, then the card refuses
  CHECK(rename::answer(true) == 0);
  host_power_cut(-1);
  CHECK(!strcmp(project, "JAM 3") && !strcmp(rename::last(), "MOVE INCOMPLETE: 2 UNDER JAM 3"));   // (the undo cut too: said so)
  // the two that moved went back? (the card refused every change after the cut, the undo too): whatever moved is under
  // one of the two keys - every file is still on the card
  bool all = true;
  for (const char* p : {"/.rytm/lfo/%s.txt", "/.rytm/ins/%s/k001.txt", "/.rytm/seq/%s/tracks.txt",
                        "/.rytm/projects/%s/samples.pool.txt", "/.rytm/songs/%s/take-001.json"}) {
    char a[96], b[96];
    snprintf(a, sizeof a, p, "JAM"), snprintf(b, sizeof b, p, "JAM 3");
    all = all && (is_file(a) || is_file(b));
  }
  CHECK(all);
  settle();

  // ---- a failed move with the card working (the target there): undone completely
  name_project("JAM");      // back to JAM (its files: wherever the cut left them - put them back whole)
  settle();
  system(("rm -rf '" + g_card + "/.rytm/lfo/JAM 3.txt' '" + g_card + "/.rytm/ins/JAM 3' '" + g_card + "/.rytm/seq/JAM 3' '" +
          g_card + "/.rytm/projects/JAM 3' '" + g_card + "/.rytm/songs/JAM 3'").c_str());
  fixture("JAM");
  name_project("JAM 4");
  CHECK(rename::state() == rename::kAsk);
  put("/.rytm/songs/JAM 4/take-009.json", "{}");   // a file under the new key meanwhile: never written over
  CHECK(rename::answer(true) == 0);
  CHECK(!strcmp(project, "JAM 4") && has_all("JAM", "JAM") && slurp("/.rytm/songs/JAM 4/take-009.json") == "{}");
  settle();

  // ---- with a card core: the looks and the moves are card-queue requests, none on the control core
  system(("rm -rf '" + g_card + "/.rytm/songs/JAM 4'").c_str());
  name_project("JAM");
  settle();
  cardq::reset();
  cardq::set_worker(true);
  uint32_t posted0 = cardq::stats().posted;
  name_project("JAM 5");
  for (int i = 0; i < 8; ++i) idle_save();   // looks posted, none run here
  CHECK(rename::state() != rename::kAsk && !strcmp(project, "JAM"));
  for (int k = 0; k < 200 && rename::state() != rename::kAsk; ++k) {
    host_card_core(0);
    while (cardq::service()) {}
    host_card_core(3);
    idle_save();
  }
  CHECK(rename::state() == rename::kAsk && rename::files() == 5);
  rename::answer(true);
  for (int k = 0; k < 200 && strcmp(project, "JAM 5"); ++k) {
    host_card_core(0);
    while (cardq::service()) {}
    host_card_core(3);
    idle_save();
  }
  CHECK(!strcmp(project, "JAM 5") && has_all("JAM 5", "JAM") && has_none("JAM"));
  // every look (both keys' paths) and every move went through the queue: at least the 5 moves and 10 looks
  CHECK(cardq::stats().posted - posted0 >= 15);
  for (int k = 0; k < 100000 && project_loading(); ++k) {   // the new name's RAM store fill, through the card core too
    host_card_core(0);
    while (cardq::service()) {}
    host_card_core(3);
    idle_save();
  }
  cardq::set_worker(false);
  cardq::reset();
  settle();

  // ---- an edit not saved yet when YES comes: FLUSH writes it into the old key's file first, then it moves (never left
  // behind under the old name, never lost)
  {
    fixture("LFOP");
    system(("rm -f '" + g_card + "/.rytm/lfo/LFOP.txt'").c_str());   // (no LFO file yet: the edit makes it)
    name_project("LFOP");
    settle();
    char o[200];
    CHECK(lfo::command("lfo=2:dep:90", o, sizeof o) && lfo::dirty());
    name_project("LFOP 2");
    CHECK(rename::state() == rename::kAsk);
    rename::answer(true);
    for (int i = 0; i < 400 && strcmp(project, "LFOP 2"); ++i) { host_fake_time(true, (uint64_t)(i + 1) * 20000 + 9000000000ull); idle_save(); }
    host_fake_time(false, 0);
    CHECK(!strcmp(project, "LFOP 2") && !strncmp(rename::last(), "MOVED", 5));
    std::string l = slurp("/.rytm/lfo/LFOP 2.txt");
    CHECK(l != "<missing>" && l != "lfo LFOP" && !is_file("/.rytm/lfo/LFOP.txt"));   // the saved edit, under the new key
    settle();
  }

  // ---- an edit after YES, while the new name's store fills (Task 4 fix 1): the LFOs and inserts are the new name's
  // the moment the files have moved, so it is saved under the new name (and kept in its store) - never a file under
  // the old name the move just emptied
  {
    fixture("RNA");
    put("/.rytm/lfo/RNA.txt", "# Sidecar LFOs\n");   // (one this build reads: the fixture's text would lock it)
    name_project("RNA");
    settle();
    name_project("RNA 2");
    CHECK(rename::state() == rename::kAsk);
    rename::answer(true);
    uint64_t t = 9100000000ull;
    for (int i = 0; i < 400 && strcmp(project, "RNA 2"); ++i) host_fake_time(true, t += 20000), idle_save();
    CHECK(!strcmp(project, "RNA 2") && !strncmp(rename::last(), "MOVED", 5) && project_loading());
    char o[200];
    CHECK(lfo::command("lfo=2:dep:55", o, sizeof o) && lfo::dirty());
    ins::set_type(1, 0, ins::kEq);
    host_fake_time(true, t += 1100000);
    for (int i = 0; i < 6; ++i) idle_save();   // both saved, the fill still running
    CHECK(project_loading() && !lfo::dirty() && !ins::settings_dirty(nullptr));
    settle();
    host_fake_time(false, 0);
    CHECK(lfo::get(1).depth == 55 && ins::control.type[1][0] == ins::kEq);   // the load kept them (its store had them)
    CHECK(slurp("/.rytm/lfo/RNA 2.txt").find(" 55 ") != std::string::npos);
    CHECK(slurp("/.rytm/ins/RNA 2.txt").find("V2 A EQ") != std::string::npos);
    CHECK(has_none("RNA") && !is_file("/.rytm/ins/RNA.txt"));   // nothing under the old name
    ins::set_type(1, 0, 0);
    settle();
    name_project("LFOP 2");   // (back where the next part expects: both names have files, no question)
    settle();
    CHECK(!strcmp(project, "LFOP 2"));
  }

  // ---- final review (Minor): the Rytm changing kit while the files move (after FLUSH, before the re-key): the kit is
  // noted (as while a store fills) and taken at the switch - its old kit's edit is saved under the new name, never a
  // write queued under the old name that lands after the move (a stray file, the edit missing from the renamed project)
  {
    fixture("KMV");
    put("/.rytm/lfo/KMV.txt", "# Sidecar LFOs\n");   // (one this build reads: the fixture's text would lock it)
    system(("rm -rf '" + g_card + "/.rytm/ins/KMV'").c_str());
    put("/.rytm/ins/KMV.txt", "# Sidecar inserts\n");
    name_project("KMV");
    settle();
    int kit0 = lfo::kit();
    cardq::reset();
    cardq::set_worker(true);
    name_project("KMV 2");
    auto pump = [] {
      host_card_core(0);
      while (cardq::service()) {}
      host_card_core(3);
    };
    for (int k = 0; k < 200 && rename::state() != rename::kAsk; ++k) pump(), idle_save();
    CHECK(rename::state() == rename::kAsk);
    rename::answer(true);
    for (int k = 0; k < 400 && !rename::moving(); ++k) pump(), idle_save();
    CHECK(rename::moving() && !strcmp(project, "KMV"));
    char o[200];
    CHECK(lfo::command("lfo=3:dep:44", o, sizeof o) && lfo::dirty());
    ins::set_type(2, 0, ins::kEq);
    int kit1 = kit0 == 5 ? 6 : 5;
    lfo::kit_changed(kit1);   // (ui/remote.cpp's 2D kit message: both)
    ins::kit_changed(kit1);
    for (int k = 0; k < 400 && strcmp(project, "KMV 2"); ++k) pump(), idle_save();
    CHECK(!strcmp(project, "KMV 2") && !strncmp(rename::last(), "MOVED", 5));
    for (int k = 0; k < 100000 && project_loading(); ++k) pump(), idle_save();
    for (int k = 0; k < 200; ++k) pump(), idle_save();
    cardq::set_worker(false);
    cardq::reset();
    settle();
    CHECK(lfo::kit() == kit1);   // the kit the Rytm named meanwhile, loaded with the project
    CHECK(has_none("KMV") && !is_file("/.rytm/lfo/KMV.txt") && !is_file("/.rytm/ins/KMV.txt"));   // no stray file
    CHECK(slurp("/.rytm/lfo/KMV 2.txt").find(" 44 ") != std::string::npos);   // the old kit's edit, renamed project
    CHECK(slurp("/.rytm/ins/KMV 2.txt").find("V3 A EQ") != std::string::npos);
    printf("  rename: a kit change while the files move is taken at the switch (its edits under the new name)\n");
    name_project("LFOP 2");
    settle();
    CHECK(!strcmp(project, "LFOP 2"));
  }

  // ---- the Rytm's PROJECT page asks it: the page shown, its text (fits: check.h), YES / NO answer
  {
    ui::remote_reset();
    uint8_t open1[] = {0xF0, 0x7D, 0x52, 0x54, 0x30, 0x01, 0x00, 0x01, 0x00, 0xF7};
    ui::remote_sysex(open1, sizeof open1);
    name_project("JAM 6");
    CHECK(rename::state() == rename::kAsk);
    for (int t = 0; t < 40; ++t) ui::remote_tick(1000 + 30 * t);
    CHECK(ui::remote_page_title() && !strcmp(ui::remote_page_title(), "PROJECT"));
    std::string s = screen_text();
    CHECK(s.find("RENAMED ON THE RYTM?") != std::string::npos && s.find("FROM LFOP 2|") != std::string::npos &&
          s.find("TO   JAM 6") != std::string::npos && s.find("YES: MOVE  NO: NEW PROJ") != std::string::npos);
    uint8_t fb[ui::kBytes];   // rui1046: the docs' render of the prompt
    ui::preview_sent(fb); CHECK(rui_png(rui_png_dir("RUI_PAGES", "build/rui_pages"), "project_rename_prompt.png", fb));
    uint8_t no_key[] = {0xF0, 0x7D, 0x52, 0x54, 0x31, 0x01, 0x00, 0x45, 0x01, 0xF7};
    ui::remote_sysex(no_key, sizeof no_key);
    CHECK(rename::state() == rename::kIdle && !strcmp(project, "JAM 6") && has_all("JAM 5", "JAM"));
  }
  printf("test_rename: ok\n");
  return 0;
}
