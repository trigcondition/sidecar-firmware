// core/tests/test_project_key.cpp - bundles phase 0 (docs/BUNDLES.md "Project keys"): one project-name -> card-key
// function (core/project_key.h) used by every per-project file; the song folder's old rule migrated once; the project
// index /.rytm/projects.txt (core/bundle/projects.h); /Bundles hidden from the machines' library.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "check.h"
#include "bundle/projects.h"
#include "engine.h"
#include "hooks.h"
#include "lib/srclib.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "project_key.h"
#include "samp/out.h"
#include "song/song.h"

using namespace core;

static std::string g_card;
static std::string key(const char* n) {
  char b[kKeyMax];
  project_key(n, b, sizeof b);
  return b;
}
static bool is_dir(const std::string& p) {
  struct stat st;
  return stat((g_card + p).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
static bool is_file(const std::string& p) {
  struct stat st;
  return stat((g_card + p).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
static std::string slurp(const std::string& p) {
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "";
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
static void name_project(const char* n) {
  uint8_t d[32];
  int l = (int)strlen(n);
  memcpy(d, n, l);
  d[l] = 0xF7;
  set_project(d, l);
}

int main() {
  // ---- the rule (the six copies' rule, now the only one)
  CHECK(key("AR PERC") == "AR PERC");
  CHECK(key("A/B") == "A_B");
  CHECK(key("x:y*z?") == "x_y_z_");
  CHECK(key("TRAIL. . ") == "TRAIL");
  CHECK(key("") == "_default");
  CHECK(key(nullptr) == "_default");
  CHECK(key("...") == "_default");
  CHECK(key("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == "ABCDEFGHIJKLMNOPQRSTUVW");   // 23 at most
  CHECK(key("\xc3\xa9t\xc3\xa9") == "__t__");
  char small[6];
  project_key("ABCDEFGH", small, sizeof small);
  CHECK(!strcmp(small, "ABCDE"));
  // the song folder's pre-phase-0 rule (spaces and dots too, no trim, _none)
  char old[kKeyMax];
  project_key_song_v0("AR PERC.", old, sizeof old);
  CHECK(!strcmp(old, "AR_PERC_"));
  project_key_song_v0("", old, sizeof old);
  CHECK(!strcmp(old, "_none"));

  // the sampler's folder rule (samp::safe_name of the name, "Sidecar" without one)
  char sf[48], ss[48];
  for (const char* n : {"AR PERC", "A/B.c", ""}) {
    project_key_sampler(n, sf, sizeof sf);
    samp::safe_name(n[0] ? n : "Sidecar", ss, sizeof ss);
    CHECK(!strcmp(sf, ss));
  }
  CHECK(!strcmp(sf, "Sidecar"));

  char tmpl[] = "/tmp/pkey-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  host_set_card(g_card.c_str());
  card_ok = true;
  hooks.to_rytm = [](const uint8_t*, int) {};
  cardq::reset();

  // ---- every per-project file uses the key: the pool file, the song folder
  strcpy(project, "AR PERC");
  char fn[96];
  pool_file(fn, sizeof fn);
  CHECK(!strcmp(fn, "/.rytm/pools/AR PERC.txt"));
  song::folder(fn, sizeof fn);
  CHECK(!strcmp(fn, "/.rytm/songs/AR PERC"));

  // ---- the song folder's migration: the old rule's folder renamed once to the key's, nothing else touched
  put("/.rytm/songs/LIVE_SET/take-001.json", "{}");
  strcpy(project, "LIVE SET");
  song::folder(fn, sizeof fn);
  CHECK(!strcmp(fn, "/.rytm/songs/LIVE SET"));
  CHECK(is_file("/.rytm/songs/LIVE SET/take-001.json") && !is_dir("/.rytm/songs/LIVE_SET"));
  // both there: the key's stays, the old one is left alone (no merge)
  put("/.rytm/songs/JAM_2/take-001.json", "old");
  put("/.rytm/songs/JAM 2/take-001.json", "new");
  strcpy(project, "JAM 2");
  song::folder(fn, sizeof fn);
  CHECK(slurp("/.rytm/songs/JAM 2/take-001.json") == "new" && slurp("/.rytm/songs/JAM_2/take-001.json") == "old");
  // a name the two rules agree on: nothing to do
  put("/.rytm/songs/PLAIN/take-001.json", "p");
  strcpy(project, "PLAIN");
  song::folder(fn, sizeof fn);
  CHECK(slurp("/.rytm/songs/PLAIN/take-001.json") == "p");
  // no project: _none -> _default
  put("/.rytm/songs/_none/take-001.json", "n");
  project[0] = 0;
  song::folder(fn, sizeof fn);
  CHECK(!strcmp(fn, "/.rytm/songs/_default") && slurp("/.rytm/songs/_default/take-001.json") == "n");

  // ---- the project index: each 10 counts, the line written by the card queue (one write for back-to-back names)
  projects::reset_for_tests();
  project[0] = 0;
  project_seen = false;
  name_project("AR PERC");
  name_project("A/B");
  for (int i = 0; i < 8 && idle_save(); ++i) {}
  std::string ix = slurp("/.rytm/projects.txt");
  CHECK(ix.find("AR PERC\tAR PERC\t1\t-\t-\n") != std::string::npos);
  CHECK(ix.find("A_B\tA/B\t2\t-\t-\n") != std::string::npos);
  name_project("AR PERC");
  for (int i = 0; i < 8 && idle_save(); ++i) {}
  ix = slurp("/.rytm/projects.txt");
  CHECK(ix.find("AR PERC\tAR PERC\t3\t-\t-\n") != std::string::npos);
  // the clock known (the page's POST /time): the date goes in
  projects::set_clock(1792677780, projects::kClockBrowser);   // 2026-10-22 14:03:00 UTC
  name_project("A/B");
  for (int i = 0; i < 8 && idle_save(); ++i) {}
  ix = slurp("/.rytm/projects.txt");
  CHECK(ix.find("A_B\tA/B\t4\t2026-10-22T14:03:00Z\t-\n") != std::string::npos);
  // the imported flag, cleared when the Rytm names the project
  CHECK(projects::mark_imported("LIVE SET 2", "LIVE SET 2"));
  for (int i = 0; i < 8 && idle_save(); ++i) {}
  CHECK(slurp("/.rytm/projects.txt").find("LIVE SET 2\tLIVE SET 2\t0\t-\ti\n") != std::string::npos);
  name_project("LIVE SET 2");
  for (int i = 0; i < 8 && idle_save(); ++i) {}
  CHECK(slurp("/.rytm/projects.txt").find("LIVE SET 2\tLIVE SET 2\t5\t") != std::string::npos);
  CHECK(slurp("/.rytm/projects.txt").find("\ti\n") == std::string::npos);
  // read back after a restart: the same lines, the counter continues
  projects::reset_for_tests();
  projects::load();
  CHECK(projects::count() == 3);
  const projects::Info* a = projects::find("A_B");
  CHECK(a && !strcmp(a->rytm, "A/B") && a->used == 4);
  name_project("PLAIN");
  for (int i = 0; i < 8 && idle_save(); ++i) {}
  CHECK(slurp("/.rytm/projects.txt").find("PLAIN\tPLAIN\t6\t") != std::string::npos);
  // a damaged line is skipped, the rest read
  put("/.rytm/projects.txt", "GOOD\tGOOD\t7\t-\t-\nbroken line\n\t\t\n");
  projects::reset_for_tests();
  projects::load();
  CHECK(projects::count() == 1 && projects::find("GOOD"));
  // forget (a delete): the line goes
  CHECK(projects::forget("GOOD"));
  for (int i = 0; i < 8 && idle_save(); ++i) {}
  CHECK(slurp("/.rytm/projects.txt").find("GOOD") == std::string::npos);

  // ---- with a card core: the index write is a card-queue request (never a control-core card write)
  projects::reset_for_tests();
  cardq::reset();
  cardq::set_worker(true);
  host_card_model(true);
  projects::note("CARDQ");
  for (int i = 0; i < 8; ++i) projects::save();
  CHECK(host_card_us(3) == 0);   // posted, not written here
  CHECK(slurp("/.rytm/projects.txt").find("CARDQ") == std::string::npos);
  host_card_core(0);
  while (cardq::service()) {}
  host_card_core(3);
  CHECK(host_card_us(0) > 0);
  for (int i = 0; i < 8; ++i) projects::save();
  CHECK(host_card_us(3) == 0);
  CHECK(slurp("/.rytm/projects.txt").find("CARDQ\tCARDQ\t") != std::string::npos);
  host_card_model(false);
  cardq::reset();

  // ---- /Bundles is not a sample source
  CHECK(srclib::hidden_name("Bundles") && srclib::hidden_name("bundles"));
  CHECK(!srclib::hidden_name("Samples"));

  system(("rm -rf '" + g_card + "'").c_str());
  printf("test_project_key: PASS\n");
  return 0;
}
