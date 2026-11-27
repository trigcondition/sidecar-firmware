// pi/engine/tests/test_loop_pi.cpp - the master looper's Pi side (spec-050 section 3.5, 3.9, 3.10; plan Tasks 23, 24): the
// real ob_usb.cpp on its fake Circle hands MAIN L / R of each IN block to the looper stamped with the render frame, and the
// loop's frames are added to MAIN after the master FX and before the take player; the RAM by board; KEEP's take on a temp
// folder, written in pieces and never by core 3.
#define main test_ob_usb_main   // (test_ob_usb.cpp's Bus, descriptors and Circle stand-ins; its own main stays unused)
#include "test_ob_usb.cpp"
#undef main

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "bridge.h"        // (shim_ob: the stand-ins ob_usb.cpp is built against, recording what the looper is handed)
#include "loop_pi.h"
#include "masterfx_pi.h"
#include "rec_pi.h"
#include "loop_keep.h"
#include "loop_plan.h"
#include "rec/rec_wav.h"

using namespace loop_pi;

// an IN block of the Rytm's 12 channels: MAIN L = base + f, MAIN R = -(base + f), the other ten at full scale (a wrong channel shows)
static void in_block(Bus& b, uint16_t& ctr, int32_t base, uint16_t skip_blocks = 0) {
  ctr += 7 * skip_blocks;   // the device's counter went on: those blocks never came
  CUSBRequest* u = b.host.in.front();
  b.host.in.pop_front();
  memset(u->buf, 0, 368);
  u->buf[0] = 0x07, u->buf[2] = ctr >> 8, u->buf[3] = (uint8_t)ctr;
  ctr += 7;
  for (int f = 0; f < 7; ++f)
    for (int c = 0; c < 12; ++c) ob::put_be32(u->buf + 32 + 4 * (12 * f + c), c == 0 ? base + f : c == 1 ? -(base + f) : 0x7FFFFFFF);
  u->SetStatus(1), u->SetResultLen(368);
  u->fn(u, u->param, u->ctx);
}

static void master_marker(int32_t (*F)[12], unsigned n) { for (unsigned f = 0; f < n; ++f) F[f][0] = 5; }
static void loop_adds(int32_t (*F)[12], unsigned n) { for (unsigned f = 0; f < n; ++f) F[f][0] += 1000, F[f][1] += 1000; }
static void play_adds(int32_t (*F)[12], unsigned n) { for (unsigned f = 0; f < n; ++f) F[f][0] += 100000, F[f][1] += 100000; }

static void test_hooks() {
  g_Output.SetMode(Output::kMain);
  loop_pi::g_wants = true;
  Bus b(g_rytm, 0x0b48);
  CHECK(b.ob && b.ob->Configure());
  uint16_t c = 0;
  // capture_aligned_to_render_frames: channels 0 / 1 of every block, the arrival stamped with the render frame counter
  loop_pi::g_in.clear();
  bridge::g_render_frames = 1000;
  in_block(b, c, 100);
  bridge::g_render_frames = 1007;
  in_block(b, c, 107);
  bridge::g_render_frames = 1014;
  in_block(b, c, 114, 3);   // three blocks lost before this one (21 frames): the ring writes them as silence
  CHECK(loop_pi::g_in.size() == 3);
  const auto& a = loop_pi::g_in[0];
  CHECK(a.lr.size() == 14 && a.arrival == 1000 && a.lost == 0);
  for (int f = 0; f < 7; ++f) CHECK(a.lr[2 * f] == 100 + f && a.lr[2 * f + 1] == -(100 + f));
  CHECK(loop_pi::g_in[1].arrival == 1007 && loop_pi::g_in[1].lr[0] == 107);
  CHECK(loop_pi::g_in[2].lost == 21 && loop_pi::g_in[2].arrival == 1014 && loop_pi::g_in[2].lr[0] == 114);
  loop_pi::g_wants = false;   // the looper off: nothing handed over
  loop_pi::g_in.clear();
  in_block(b, c, 200);
  CHECK(loop_pi::g_in.empty());
  loop_pi::g_wants = true;

  // playback_after_masterfx_on_0_1: the loop's frames after the master chain (which sets 5) and before the take player
  // (which adds 100000), on MAIN L / R (channels 0 / 1) only; the render frame is the frames pulled so far
  masterfx_pi::g_process_hook = master_marker;
  loop_pi::g_out_hook = loop_adds;
  g_play_hook = play_adds;
  int16_t fr[7][8] = {};
  for (int k = 0; k < 8; ++k) g_Output.Push(fr, 7);   // 56 render frames in the FIFO
  loop_pi::g_out_frames.clear();
  uint64_t first = g_Output.PulledFrame();
  while (!b.host.out.empty()) b.out_done();
  for (int k = 0; k < 4; ++k) {
    in_block(b, c, 300 + 7 * k);
    const uint8_t* o = b.host.out.back()->buf;
    for (int f = 0; f < 7; ++f) {
      CHECK(ob::block_sample(o, 12, f, 0) == 101005 && ob::block_sample(o, 12, f, 1) == 101000);   // master 5, loop +1000, take +100000
      for (int ch = 2; ch < 12; ++ch) CHECK(ob::block_sample(o, 12, f, ch) == 0);
    }
  }
  CHECK(loop_pi::g_out_frames.size() == 4);
  for (int k = 0; k < 4; ++k) CHECK(loop_pi::g_out_frames[k] == first + 7 * (uint64_t)k);
  masterfx_pi::g_process_hook = nullptr;
  loop_pi::g_out_hook = nullptr;
  g_play_hook = nullptr;
  printf("  hooks: IN stamped with the render frame, the loop after the master FX on 0 / 1 ok\n");
}

static void test_plan() {
  // defaults_by_board: spec 3.10's store (96 / 192 / 256 MB), the ring 32 MB on all; a 1 GB board: off (owner decision 23)
  CHECK(kRingFrames == 4194304 && kRingBytes == (32u << 20));
  CHECK(StoreMB(948, -1) == 0 && StoreMB(1024, -1) == 0);                       // 1 GB (the heap sees less than 1024)
  CHECK(StoreMB(1900, -1) == 96 && StoreMB(2048, -1) == 96);                    // 2 GB
  CHECK(StoreMB(3900, -1) == 192 && StoreMB(4096, -1) == 192);                  // 4 GB
  CHECK(StoreMB(8000, -1) == 256 && StoreMB(16000, -1) == 256);                 // 8 / 16 GB
  // loopram=MB: asked for, 0..1024; 0 is off whatever the board has; a 1 GB board turned on by asking
  CHECK(StoreMB(8000, 0) == 0 && StoreMB(2048, 0) == 0);                        // loopram_zero_off
  CHECK(StoreMB(1024, 48) == 48 && StoreMB(8000, 2000) == 1024 && StoreMB(2048, 1024) == 512 && StoreMB(1024, 1024) == 256);   // (at most a quarter of the RAM)
  printf("  plan: defaults by board, loopram=0 off, 1 GB off by default ok\n");
}

// ---- Task 24: KEEP on a temp folder
static std::string g_root;
static FILE* g_fh[8];
static bool g_in_poll = false;   // a write outside Keep::poll / begin would be another thread's: none counted
static int g_writes = 0, g_bad_writes = 0;
static int g_fail_after = -1;   // KWrite fails from this many writes on (a full card)
static uint32_t g_largest = 0;
static std::string disk(const char* p) { return g_root + p; }
static bool KMkdir(const char* p) { return mkdir(disk(p).c_str(), 0755) == 0 || access(disk(p).c_str(), F_OK) == 0; }
static bool g_mkdir_fail = false, g_list_empty = false;   // N3: a folder that cannot be made; a listing that misses the takes
static int KMkdirNew(const char* p) { if (g_mkdir_fail) return -1; return mkdir(disk(p).c_str(), 0755) == 0 ? 1 : errno == EEXIST ? 0 : -1; }
static int KOpen(const char* p) {
  for (int i = 0; i < 8; ++i)
    if (!g_fh[i]) return (g_fh[i] = fopen(disk(p).c_str(), "wb")) ? i : -1;
  return -1;
}
static bool KWrite(int h, const void* p, uint32_t n) {
  ++g_writes;
  if (g_fail_after >= 0 && g_writes > g_fail_after) return false;
  if (!g_in_poll) ++g_bad_writes;
  if (n > g_largest) g_largest = n;
  return fwrite(p, 1, n, g_fh[h]) == n;
}
static bool KPwrite(int, uint64_t, const void*, uint32_t) { return false; }
static bool KRemove(const char* p) { return remove(disk(p).c_str()) == 0; }
static bool KSync(int h) { return fflush(g_fh[h]) == 0; }
static bool KClose(int h) { fclose(g_fh[h]); g_fh[h] = nullptr; return true; }
static bool KSpace(uint64_t* f, uint64_t* t) { *f = 1ull << 33, *t = 1ull << 34; return true; }
static uint32_t KNow() { return 0; }
static int KList(const char* d, char (*names)[64], uint64_t* sizes, bool* dirs, int max) {
  DIR* dp = opendir(disk(d).c_str());
  if (!dp) return -1;
  if (g_list_empty) return closedir(dp), 0;
  int n = 0;
  while (dirent* e = readdir(dp)) {
    if (e->d_name[0] == '.' || n == max) continue;
    snprintf(names[n], 64, "%s", e->d_name);
    sizes[n] = 0, dirs[n] = true;
    ++n;
  }
  closedir(dp);
  return n;
}
static bool KRead(const char*, uint64_t, void*, uint32_t) { return false; }
static bool KPatch(const char*, uint64_t, const void*, uint32_t) { return false; }
static void KLog(const char*) {}
static bool KClock(uint32_t* t) { *t = 1794571200; return true; }   // 2026-11-13 12:00
static void KProject(char* o, unsigned cap) { snprintf(o, cap, "Demo Project"); }
static const rec::Backend kKB = {KMkdir, KOpen, KWrite, KPwrite, KSync, KClose, KSpace, KNow, KList, KRead, KPatch, KLog,
                                 nullptr, nullptr, nullptr, KRemove, nullptr, KMkdirNew, KClock, KProject, nullptr, nullptr, nullptr};
static uint32_t g_done_id = 0;
static bool g_done_ok = false;
static std::string g_done_take;
static void KDone(uint32_t id, bool ok, const char* take) { g_done_id = id, g_done_ok = ok, g_done_take = take; }
static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return v;
  uint8_t b[4096];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}

static void test_keep() {
  char tmpl[] = "/tmp/loop_keep_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_root = tmpl;
  // a 2-bar grab at 120 BPM would be 192000 frames; 40000 keeps the test short: 8 pieces of 5120 frames
  const uint32_t frames = 40000;
  std::vector<int32_t> pcm(2 * (size_t)frames);
  for (uint32_t i = 0; i < frames; ++i) pcm[2 * i] = (int32_t)((i % 1000) << 12) - (1 << 20), pcm[2 * i + 1] = -pcm[2 * i];
  Keep k;
  k.init(&kKB, KDone);
  core::loop::KeepJob job{7, frames, pcm.data(), 120.0f, 2, 5000};
  CHECK(!k.busy());
  g_in_poll = true;   // (begin writes the header: core 0's idle loop)
  CHECK(k.begin(job) && k.busy());
  int polls = 0;
  while (k.poll()) ++polls;
  g_in_poll = false;
  CHECK(!k.busy() && g_done_ok && g_done_id == 7);
  CHECK(g_bad_writes == 0 && g_largest <= Keep::kPiece && k.pieces() == (frames + 5119) / 5120 && k.worst_bytes() == Keep::kPiece);
  // the take: "<slug>_<date>_<time>" in /Recordings, the audio file and loop.txt inside
  CHECK(g_done_take == "demo-project_2026-11-13_1200");
  std::string dir = g_root + "/Recordings/" + g_done_take;
  std::vector<uint8_t> w = slurp(dir + "/00_LOOP.wav");
  CHECK(w.size() == recwav::kHeader + (size_t)frames * 6);
  CHECK(recwav::ours(w.data()) == 6 && recwav::get32(w.data() + recwav::kHeader - 4) == frames * 6);   // 24-bit stereo, the true size
  for (uint32_t i : {0u, 1u, 999u, 5120u, 39999u})
    for (int ch = 0; ch < 2; ++ch) {
      const uint8_t* s = w.data() + recwav::kHeader + 6 * (size_t)i + 3 * ch;
      int32_t v = (int32_t)(s[0] | s[1] << 8 | s[2] << 16);
      if (v & 0x800000) v -= 0x1000000;
      CHECK(v == pcm[2 * (size_t)i + ch] >> 8);
    }
  std::vector<uint8_t> t = slurp(dir + "/loop.txt");
  std::string txt(t.begin(), t.end());
  CHECK(txt == "bpm=120.0\nbars=1\ngrabbed=2026-11-13 12:00\n");
  // a second keep the same minute: the next name, never the same take
  g_done_ok = false;
  g_in_poll = true;
  core::loop::KeepJob half{8, 5120, pcm.data(), 90.0f, 1, 0};
  CHECK(k.begin(half));
  while (k.poll()) {
  }
  g_in_poll = false;
  CHECK(g_done_ok && g_done_take == "demo-project_2026-11-13_1200_2");
  {
    std::vector<uint8_t> t2 = slurp(g_root + "/Recordings/" + g_done_take + "/loop.txt");
    CHECK(std::string(t2.begin(), t2.end()).find("bars=0.5") != std::string::npos);
  }
  // M3: a card that fills mid-take: told, and no take left behind (no short file under a header with the full sizes)
  {
    int taken = 0;
    {
      DIR* dp = opendir((g_root + "/Recordings").c_str());
      while (dirent* e = readdir(dp)) taken += e->d_name[0] != '.';
      closedir(dp);
    }
    g_done_ok = true;
    g_fail_after = g_writes + 3;
    Keep f;
    f.init(&kKB, KDone);
    g_in_poll = true;
    core::loop::KeepJob job2{11, frames, pcm.data(), 120.0f, 2, 0};
    CHECK(f.begin(job2));
    while (f.poll()) {
    }
    g_in_poll = false;
    g_fail_after = -1;
    CHECK(!g_done_ok && g_done_id == 11);
    int now_n = 0;
    {
      DIR* dp = opendir((g_root + "/Recordings").c_str());
      while (dirent* e = readdir(dp)) now_n += e->d_name[0] != '.';
      closedir(dp);
    }
    CHECK(now_n == taken);
  }
  // N3: a Keep that cannot even make its folder never removes another take's file
  {
    std::string old = g_root + "/Recordings/demo-project_2026-11-13_1200/00_LOOP.wav";
    CHECK(access(old.c_str(), F_OK) == 0);
    g_list_empty = g_mkdir_fail = true;   // the first candidate name is the existing take's
    Keep n3;
    n3.init(&kKB, KDone);
    g_done_ok = true;
    core::loop::KeepJob job3{12, frames, pcm.data(), 120.0f, 2, 0};
    CHECK(!n3.begin(job3) && !g_done_ok);
    g_list_empty = g_mkdir_fail = false;
    CHECK(access(old.c_str(), F_OK) == 0);
  }
  // no backend room: refused, told
  g_done_ok = true;
  Keep dead;
  dead.init(&kKB, KDone);
  core::loop::KeepJob none{9, 0, nullptr, 120.0f, 2, 0};
  CHECK(!dead.begin(none) && !g_done_ok && g_done_id == 9);
  system(("rm -rf " + g_root).c_str());
  printf("  keep: a take of %u pieces, 24-bit WAV, loop.txt, none from core 3 ok\n", (frames + 5119) / 5120);
}

int main() {
  test_hooks();
  test_plan();
  test_keep();
  printf("test_loop_pi: PASS\n");
  return 0;
}
