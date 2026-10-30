// core/tests/test_pool.cpp - the project's pool on a folder standing in for the card: a project change loads its
// pool and sends 14 / 16 / 17; LOAD TO PROJ and ASSIGN go into the SAMPLES pool's bank A (core/lib/pools.h) and send
// 14 (inputs / built-ins there: empty slots; a file outside /Samples: the name only); it reads back;
// the TEENSY folder's listing (0E) puts JUNO first, then /Samples' folders by name; a JUNO patch goes to the list.
#include <cstring>
#include <string>
#include <unistd.h>   // mkdtemp
#include <vector>

#include "check.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "pool/pool.h"
#include "voices.h"
#include "wav.h"

static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }
static int g_pool_changed = 0, g_last_line = 99, g_project_changed = 0;

// the project's pool lists read, DSP SAMPLE's slots synced (the control core's steps)
static void ready() {
  char k[core::kKeyMax];
  core::project_key(core::project, k, sizeof k);
  for (int i = 0; i < 5000 && (!core::pools::ready() || strcmp(core::pools::project(), k) || core::pools::saving()); ++i)
    core::pools::step();
  CHECK(core::pools::ready());
  core::pool_sync();
}
static std::string file_text(const char* p) {
  FILE* f = fopen(core::host_path(p).c_str(), "rb");
  if (!f) return "";
  char b[512];
  size_t n = fread(b, 1, sizeof b, f);
  fclose(f);
  return std::string(b, n);
}

int main() {
  using namespace core;
  char tmpl[] = "/tmp/core-pool-XXXXXX";
  CHECK(mkdtemp(tmpl));
  host_set_card(tmpl);
  Fs fs;
  CHECK(fs.begin() && fs.mkdir("/Samples/Drums") && fs.mkdir("/Samples/Bass") && fs.mkdir("/JUNO"));
  auto z = [](int, int) { return (int32_t)0; };
  write_wav("/Samples/Drums/snare.wav", 10, 1, 16, 48000, z);
  write_wav("/Samples/Drums/kick.wav", 10, 1, 16, 48000, z);
  write_wav("/Samples/Bass/sub.wav", 10, 1, 16, 48000, z);
  std::vector<uint8_t> pad = {0xf0, 0x41, 0x30, 0, 5};
  for (int i = 0; i < 18; ++i) pad.push_back(i);
  pad.push_back(0xf7);
  FILE* f = fopen(host_path("/JUNO/Pad.syx").c_str(), "wb");
  fwrite(pad.data(), 1, pad.size(), f);
  fclose(f);
  hooks.to_rytm = out;
  hooks.pool_changed = [](int line) { ++g_pool_changed; g_last_line = line; };
  hooks.project_changed = [] { ++g_project_changed; };
  card_ok = cards.begin(log, shared, sizeof shared);
  CHECK(card_ok);
  srclib::begin(true);
  pools::begin(true);
  junolib.begin(&cards.sd(), log);
  dxlib.begin(&cards.sd(), log);
  voices_init();
  int drums = -1, bass = -1;
  for (int k = 1; k < cards.folders(); ++k) {
    if (!strcmp(cards.folder(k).name, "Drums")) drums = k;
    if (!strcmp(cards.folder(k).name, "Bass")) bass = k;
  }
  CHECK(drums > 0 && bass > 0);

  set_project((const uint8_t*)"PROJ\xf7", 5);
  CHECK(!strcmp(project, "PROJ") && project_dirty && g_project_changed == 1 && g_pool_changed == 1 && g_last_line == -1);
  // 14 (empty pool), 16 (no JUNO list), 17: Plaits' 96 ROM voices (no FM list), 8 a message
  CHECK(g_out.size() == 2 + 12 && g_out[0][4] == 0x14 && g_out[0][6] == 0 && g_out[1][4] == 0x16 && g_out[2][4] == 0x17);
  CHECK(g_out[2][5] == 1 && g_out[2][6] == 96 && g_out[13][4] == 0x17 && g_out[13][5] == 0);
  project_save();
  CHECK(file_text("/.rytm/project") == "PROJ" && !project_dirty);
  g_out.clear();
  set_project((const uint8_t*)"PROJ\xf7", 5);   // a repeat: nothing
  CHECK(g_out.empty() && g_project_changed == 1);

  ready();
  pool_add(drums, 0);   // kick.wav (name order): LOAD TO PROJ into the SAMPLES pool's bank A, its first free row
  CHECK(npool == 1 && !strcmp(pool[0].path, "Drums/kick.wav") && pool[0].folder == drums);
  CHECK(pools::at(pools::kSamples, 1).type == pools::kSample && !strcmp(pools::at(pools::kSamples, 1).path, "/Samples/Drums/kick.wav"));
  CHECK(g_out.back()[4] == 0x14 && g_out.back()[6] == 1 && g_out.back()[7] == 1 && !memcmp(&g_out.back()[12], "kick", 4));
  pool_add(drums, 0);   // already there: no second slot (one entry per hash / path)
  CHECK(npool == 1);
  pool_assign(5, bass, 0);   // ASSIGN: SMP 5 = A005
  CHECK(npool == 5 && pool[4].folder == bass && g_last_line == -1);
  // an input / built-in in bank A (not DSP SAMPLE's): an empty slot in the 14 table
  CHECK(pools::set_special(pools::kSamples, 3, pools::kBuiltin, 0, pools::kForced) == pools::kOk);
  pool_sync();
  CHECK(npool == 5 && pool[2].path[0] == 0 && pool[2].folder == -1);
  // a sample outside /Samples: the name only (folder -1: 0x3fff in the 14 table)
  CHECK(pools::set_file(pools::kSamples, 7, "/GRANULAR/x.wav", nullptr, pools::kForced) == pools::kOk);
  g_out.clear();
  pool_sync();
  CHECK(npool == 7 && pool[6].folder == -1 && !strcmp(pool[6].path, "/GRANULAR/x.wav") && !g_out.empty() && g_out.back()[4] == 0x14);
  CHECK(pools::unload(pools::kSamples, 7, pools::kForced) == pools::kOk);
  pool_sync();
  npool = 0;
  set_project((const uint8_t*)"OTHER\xf7", 6);   // another project and back: the SAMPLES pool read back from the card
  ready();
  CHECK(npool == 0);
  set_project((const uint8_t*)"PROJ\xf7", 5);
  ready();
  CHECK(npool == 5 && pool[4].folder == bass && pool[4].file == 0 && pool[1].path[0] == 0);
  CHECK(file_text("/.rytm/projects/PROJ/samples.pool.txt").find("A005 sample ") != std::string::npos);
  {   // a SAMPLES pool file this build cannot read (damaged): locked - an ASSIGN changes nothing, the file not written over
    std::string txt = "#sidecar samplepool 1\nA001 sample - 10 /Samples/Drums/kick.wav\nA001 frob\n";
    FILE* lf = fopen(host_path("/.rytm/projects/LONG/samples.pool.txt").c_str(), "wb");
    CHECK(lf || (fs.mkdir("/.rytm/projects/LONG") && (lf = fopen(host_path("/.rytm/projects/LONG/samples.pool.txt").c_str(), "wb"))));
    fwrite(txt.data(), 1, txt.size(), lf);
    fclose(lf);
    set_project((const uint8_t*)"LONG\xf7", 5);
    ready();
    pool_assign(3, bass, 0);
    CHECK(pools::locked(pools::kSamples) && npool == 0 && file_text("/.rytm/projects/LONG/samples.pool.txt") == txt);
    set_project((const uint8_t*)"PROJ\xf7", 5);
    ready();
    CHECK(npool == 5 && pool[4].folder == bass);
    printf("pool: a SAMPLES pool it cannot read is locked (not written over)\n");
  }

  g_out.clear();
  send_listing(0);   // /Samples: JUNO, then Bass, Drums (folders by name)
  CHECK(g_out.size() == 1 && g_out[0][4] == 0x0e);
  const std::vector<uint8_t>& m = g_out[0];
  CHECK(m[12] == 3 && m[13] == 1 && m[20] == 4 && !memcmp(&m[21], "JUNO", 4));
  CHECK(m[25] == 1 && m[32] == 4 && !memcmp(&m[33], "Bass", 4));

  {   // no card: an empty listing (total 0), not silence - the Rytm's UI waits for the 0E (rytm-mods 0023)
    bool ok = card_ok;
    card_ok = false;
    g_out.clear();
    send_listing(0);
    CHECK(g_out.size() == 1 && g_out[0][4] == 0x0e && g_out[0].size() == 14 && g_out[0][11] == 0 && g_out[0][12] == 0 && g_out[0][13] == 0xf7);
    g_out.clear();
    send_listing(-1);   // not a folder: still nothing
    CHECK(g_out.empty());
    card_ok = ok;
  }

  g_out.clear();
  pool_add(cards.folders() + 0, 0);   // TEENSY folder JUNO's first patch: into the project's JUNO list
  CHECK(junolib.listed() == 1 && !strcmp(junolib.slot(0).name, "Pad") && g_out[0][4] == 0x16);

  project[0] = 0;
  project_load();   // at boot: /.rytm/project names the project, its SAMPLES pool loads
  ready();
  CHECK(!strcmp(project, "PROJ") && npool == 5);
  printf("test_pool: PASS\n");
  return 0;
}
