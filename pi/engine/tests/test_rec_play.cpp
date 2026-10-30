// pi/engine/tests/test_rec_play.cpp - the takes' playback (rec_play.h) on a temp folder of synthetic takes (the
// recorder's layout and headers, rec_wav.h): the takes newest first, a take's channels (MASTER first, else the first
// recorded), every sample where the file has it (24 / 16 bit, stereo / mono on both sides), the fades, a channel change
// resuming at the same frame, the take's end, the card falling behind (faded out, reported), a take cut short.
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.h"
#include "rec_play.h"
#include "rec/rec_wav.h"

static std::string g_root;
static FILE* g_f[4];
static std::vector<std::string> g_log;
static int g_reads_fail = 0;
// "USB:/Recordings/..." a drive: <root>/USB/Recordings/...
static std::string disk(const char* p) {
  const char* c = strchr(p, ':');
  if (c && p[0] != '/') return g_root + "/" + std::string(p, (size_t)(c - p)) + (c + 1);
  return g_root + p;
}
static bool g_usb = false;   // the drive mounted
static int BRoots(const char** out, int max) {
  if (!g_usb || max < 1) return 0;
  out[0] = "USB:";
  return 1;
}

static int BList(const char* d, char (*names)[64], uint64_t* sizes, bool* dirs, uint32_t* stamps, int max) {
  DIR* dp = opendir(disk(d).c_str());
  if (!dp) return -1;
  int n = 0;
  while (dirent* e = readdir(dp)) {
    if (e->d_name[0] == '.' || n == max) continue;
    struct stat st;
    std::string p = disk(d) + "/" + e->d_name;
    stat(p.c_str(), &st);
    snprintf(names[n], 64, "%s", e->d_name);
    sizes[n] = (uint64_t)st.st_size;
    dirs[n] = S_ISDIR(st.st_mode);
    stamps[n] = 0;   // the Pi without a clock: every stamp the same, the names decide
    ++n;
  }
  closedir(dp);
  return n;
}
static int BOpen(const char* p) {
  for (int i = 0; i < 4; ++i)
    if (!g_f[i]) return (g_f[i] = fopen(disk(p).c_str(), "rb")) ? i : -1;
  return -1;
}
static int BRead(int h, void* p, uint32_t n) {
  if (g_reads_fail) return --g_reads_fail, -1;
  return (int)fread(p, 1, n, g_f[h]);
}
static bool BSeek(int h, uint64_t off) { return fseek(g_f[h], (long)off, SEEK_SET) == 0; }
static void BClose(int h) { fclose(g_f[h]); g_f[h] = nullptr; }
static void BLog(const char* l) { g_log.push_back(l); }
static uint32_t BNow() { timeval tv; gettimeofday(&tv, nullptr); return (uint32_t)(tv.tv_sec * 1000000 + tv.tv_usec); }
static const recplay::Backend kB = {BList, BOpen, BRead, BSeek, BClose, BLog, BRoots};

// sample c of frame i of a file (source id src): distinct per file, frame and side, inside 16 bits
static int32_t val(int src, uint32_t i, int c) { return (int32_t)(((i * 7 + src * 1000 + c * 333) % 30000) - 15000); }

static void write_wav(const char* path, int src, unsigned ch, unsigned bits, uint32_t frames, bool cut = false) {
  FILE* f = fopen(disk(path).c_str(), "wb");
  uint8_t h[recwav::kHeader];
  uint32_t align = ch * bits / 8, data = frames * align;
  recwav::build(h, ch, bits, cut ? recwav::placeholder(align) : data);
  fwrite(h, 1, sizeof h, f);
  for (uint32_t i = 0; i < frames; ++i)
    for (unsigned c = 0; c < ch; ++c) {
      int32_t v = val(src, i, (int)c);
      if (bits == 24) {
        uint32_t u = (uint32_t)(v * 256);   // 24-bit: the 16-bit value with a low byte
        uint8_t b[3] = {(uint8_t)(u & 0xFF), (uint8_t)(u >> 8), (uint8_t)(u >> 16)};
        fwrite(b, 1, 3, f);
      } else {
        uint8_t b[2] = {(uint8_t)(v & 0xFF), (uint8_t)((uint32_t)v >> 8)};
        fwrite(b, 1, 2, f);
      }
    }
  fclose(f);
}
// what the player should send for sample c of frame i (int32 full scale)
static int32_t want(int src, uint32_t i, int c, unsigned bits) { return bits == 24 ? val(src, i, c) * 256 * 256 : val(src, i, c) * 65536; }

static recplay::Player P;   // 512 KB of ring: not on the stack

// n frames through the mix (polling the reader every block unless starve), the output's L / R appended
static void run(uint32_t n, std::vector<int32_t>* l, std::vector<int32_t>* r, bool starve = false) {
  for (uint32_t done = 0; done < n; done += 7) {
    int32_t out[7][12] = {};
    P.mix(out, 7);
    for (int f = 0; f < 7; ++f) {
      if (l) l->push_back(out[f][0]);
      if (r) r->push_back(out[f][1]);
    }
    if (!starve) P.poll();
  }
}

int main() {
  char tmpl[] = "/tmp/rec_play_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_root = tmpl;
  P.init(&kB, BNow);
  recplay::Status s;
  CHECK(P.scan() == 0);   // no /Recordings
  P.status(&s);
  CHECK(s.state == recplay::kNone && !P.play(nullptr, 0));
  mkdir(disk("/Recordings").c_str(), 0755);
  mkdir(disk("/Recordings/take_001").c_str(), 0755);
  mkdir(disk("/Recordings/take_002").c_str(), 0755);
  mkdir(disk("/Recordings/notes").c_str(), 0755);
  const uint32_t kLen = 100000;   // 2.08 s
  write_wav("/Recordings/take_001/00_MASTER.wav", 0, 2, 24, kLen);
  write_wav("/Recordings/take_001/03_BD.wav", 3, 1, 24, kLen);
  write_wav("/Recordings/take_001/13_VOICE1.wav", 13, 1, 16, kLen);
  write_wav("/Recordings/take_002/05_RS-CP.wav", 5, 1, 16, 30000);
  write_wav("/Recordings/take_002/03_BD.wav", 3, 1, 24, 30000, true);   // cut short: the placeholder sizes
  FILE* junk = fopen(disk("/Recordings/take_002/readme.txt").c_str(), "w");
  fputs("x", junk), fclose(junk);

  // the takes, newest first (the same stamps: by name), the newest selected; its channels in NN order, no MASTER: BD
  CHECK(P.scan() == 3);
  P.status(&s);
  CHECK(s.state == recplay::kStopped && !strcmp(s.take, "take_002") && s.take_i == 0 && s.takes == 3 && s.channels == 2 &&
        s.ch_i == 0 && !strcmp(s.channel, "BD") && s.len == 30000 && s.width == 1 && s.bits == 24);
  CHECK(P.step_take(1));
  P.status(&s);
  CHECK(!strcmp(s.take, "take_001") && s.channels == 3 && !strcmp(s.channel, "MASTER") && s.len == kLen && s.width == 2);
  CHECK(!P.step_take(1) || (P.status(&s), !strcmp(s.take, "notes")));   // "notes": a folder without WAVs
  P.status(&s);
  CHECK(s.channels == 0 && !P.play(nullptr, 0));
  CHECK(P.scan("take_001") == 3);
  P.status(&s);
  CHECK(s.take_i == 1 && !strcmp(s.channel, "MASTER"));

  // MASTER, 24-bit stereo, at 127 (as recorded): faded in over 10 ms, then every sample as the file has it
  P.set_level(127);
  char err[64];
  CHECK(P.play(err, sizeof err));
  CHECK(P.active());
  std::vector<int32_t> L, R;
  run(14000, &L, &R);
  bool same = true;
  for (uint32_t i = recplay::kFade; i < 14000; ++i) same &= L[i] == want(0, i, 0, 24) && R[i] == want(0, i, 1, 24);
  CHECK(same);
  CHECK(L[0] != want(0, 0, 0, 24) || want(0, 0, 0, 24) == 0);
  bool ramp = true;   // the fade-in: |out| <= |sample| and growing toward it
  for (uint32_t i = 0; i < recplay::kFade; ++i) ramp &= llabs(L[i]) <= llabs((long long)want(0, i, 0, 24));
  CHECK(ramp);
  P.status(&s);
  CHECK(s.state == recplay::kPlaying && s.pos == 14000);

  // B turned: BD, from the frame the fade ended on (the take's files line up); mono on both sides
  CHECK(P.step_channel(1));
  L.clear(), R.clear();
  run(7 * 200, &L, &R);
  P.status(&s);
  CHECK(!strcmp(s.channel, "BD") && s.state == recplay::kPlaying);
  uint64_t at = 14000 + recplay::kFade;               // the fade-out's frames played, then BD at that frame
  uint32_t b0 = (recplay::kFade + 6) / 7 * 7;          // from the next block on (the rest of the fade's block silent)
  bool bd = true;
  for (uint32_t k = b0 + recplay::kFade; k < L.size(); ++k) bd &= L[k] == want(3, (uint32_t)(at + k - b0), 0, 24) && R[k] == L[k];
  CHECK(bd && L[b0 - 1] == 0);
  CHECK(s.pos == at + 7 * 200 - b0);

  // 16-bit (VOICE1), level 100: about -4 dB
  CHECK(P.step_channel(1));
  P.set_level(100);
  L.clear();
  run(7 * 300, &L, nullptr);
  P.status(&s);
  int32_t w = want(13, (uint32_t)(s.pos - 1), 0, 16), got = L.back();
  float g = (100.0f / 127) * (100.0f / 127);
  CHECK(!strcmp(s.channel, "VOICE1") && s.bits == 16 && llabs((long long)got - (long long)(w * g)) <= 2);

  // stop: faded out over 10 ms, silence after, back to the start
  P.stop();
  L.clear();
  run(7 * 100, &L, nullptr);
  bool quiet = true;
  for (size_t i = recplay::kFade + 1; i < L.size(); ++i) quiet &= L[i] == 0;
  CHECK(quiet && !P.active());
  P.status(&s);
  CHECK(s.state == recplay::kStopped && s.pos == 0 && !s.error[0]);

  // the take's end: everything played, then stopped by itself
  P.set_level(127);
  CHECK(P.step_channel(-2));   // MASTER
  CHECK(P.play(err, sizeof err));
  L.clear();
  run(kLen + 700, &L, nullptr);
  P.status(&s);
  CHECK(!P.active() && s.state == recplay::kStopped && L[kLen - 1] == want(0, kLen - 1, 0, 24) && L[kLen] == 0 && !s.error[0]);

  // a pause keeps the frame: play() goes on from it (the same samples), a stop and another take start over
  CHECK(P.play(err, sizeof err));
  run(9000, nullptr, nullptr);
  P.pause();
  run(2000, nullptr, nullptr);
  P.status(&s);
  CHECK(!P.active() && s.state == recplay::kStopped && s.paused && s.pos > 9000 && s.pos < 9000 + 2 * recplay::kFade);
  uint64_t held = s.pos;
  CHECK(P.play(err, sizeof err));
  L.clear();
  run(3000, &L, nullptr);
  bool cont = true;
  for (uint32_t k = recplay::kFade; k < 2900; ++k) cont &= L[k] == want(0, (uint32_t)(held + k), 0, 24);
  CHECK(cont);
  P.status(&s);
  CHECK(s.state == recplay::kPlaying && !s.paused);
  P.pause();
  run(2000, nullptr, nullptr);
  P.stop();
  P.status(&s);
  CHECK(!s.paused);
  CHECK(P.play(err, sizeof err));
  P.status(&s);
  CHECK(s.pos == 0);
  P.stop();
  run(2000, nullptr, nullptr);

  // the card falls behind (no reads): faded out on what is held before it runs dry, reported once
  CHECK(P.play(err, sizeof err));
  L.clear();
  run(70000, &L, nullptr, true);
  CHECK(!P.active());
  size_t last = 0;
  for (size_t i = 0; i < L.size(); ++i)
    if (L[i]) last = i;
  CHECK(last > recplay::kStartFill - 1000 && last < recplay::kStartFill + 5120);   // the 0.5 s held (to a piece), then the fade
  {   // a fade, not a cut: halfway down 240 frames before the end
    uint32_t i = (uint32_t)last - 240;
    double q = (double)L[i] / want(0, i, 0, 24);
    CHECK(q > 0.4 && q < 0.6);
  }
  P.poll();
  P.status(&s);
  CHECK(s.underruns == 1 && strstr(s.error, "card too slow") && !g_log.empty() && strstr(g_log.back().c_str(), "take_001/MASTER"));

  // a read error mid-take: what is held plays out, the error reported
  CHECK(P.play(err, sizeof err));
  g_reads_fail = 1;
  run(40000, nullptr, nullptr);
  P.status(&s);
  CHECK(!P.active() && strstr(s.error, "read failed"));
  g_reads_fail = 0;

  // take_002's BD cut short (placeholder sizes): its length from the file's size; a pick by name and NN (the web page)
  CHECK(P.select_by_name("take_002", 3));
  P.status(&s);
  CHECK(!strcmp(s.take, "take_002") && !strcmp(s.channel, "BD") && s.len == 30000);
  CHECK(P.select_by_name("take_002", 5));
  P.status(&s);
  CHECK(!strcmp(s.channel, "RS-CP") && s.bits == 16);
  CHECK(!P.select_by_name("take_999", 0));

  // a new take (the recorder's next): scan() keeps the selection unless told the new one
  mkdir(disk("/Recordings/take_003").c_str(), 0755);
  write_wav("/Recordings/take_003/00_MASTER.wav", 0, 2, 16, 4800);
  CHECK(P.scan() == 4);
  P.status(&s);
  CHECK(!strcmp(s.take, "take_002"));
  CHECK(P.scan("take_003") == 4);
  P.status(&s);
  CHECK(!strcmp(s.take, "take_003") && s.take_i == 0 && !strcmp(s.channel, "MASTER") && s.len == 4800);

  // a USB drive's takes beside the SD's (ids "USB:<take>"), in one list by name; one plays from the drive
  {
    char f[40];
    recplay::take_folder(f, sizeof f, "USB:take_004");
    CHECK(!strcmp(f, "USB:/Recordings/take_004"));
    recplay::take_folder(f, sizeof f, "take_004");
    CHECK(!strcmp(f, "/Recordings/take_004"));
  }
  mkdir(disk("USB:/").c_str(), 0755);
  mkdir(disk("USB:/Recordings").c_str(), 0755);
  mkdir(disk("USB:/Recordings/take_004").c_str(), 0755);
  mkdir(disk("USB:/Recordings/take_002").c_str(), 0755);   // the same name as the SD's: told apart by the volume
  write_wav("USB:/Recordings/take_004/03_BD.wav", 3, 1, 24, 20000);
  write_wav("USB:/Recordings/take_002/00_MASTER.wav", 0, 2, 16, 4800);
  CHECK(P.scan() == 4);   // the drive not mounted: the SD's only
  g_usb = true;
  CHECK(P.scan() == 6);
  CHECK(P.select_take(0));
  P.status(&s);
  CHECK(!strcmp(s.take, "USB:take_004") && !strcmp(s.channel, "BD") && s.len == 20000);
  CHECK(P.step_take(1) && (P.status(&s), !strcmp(s.take, "take_003")));
  CHECK(P.step_take(1) && (P.status(&s), !strcmp(s.take, "take_002")));
  CHECK(P.step_take(1) && (P.status(&s), !strcmp(s.take, "USB:take_002")) && s.len == 4800 && s.bits == 16);
  CHECK(P.select_by_name("USB:take_004", 3) && P.play(err, sizeof err));
  {
    std::vector<int32_t> l, r;
    run(30000, &l, &r);
    // past the fade-in, a sample where the file has it (the mono BD on both sides)
    uint32_t i = 1000;
    CHECK(l.size() > 20000 && !P.active());
    bool found = false;
    for (size_t k = 0; k + 1 < l.size() && !found; ++k) found = l[k] == want(3, i, 0, 24) && r[k] == l[k];
    CHECK(found);
  }
  CHECK(P.scan("USB:take_004") == 6 && (P.status(&s), !strcmp(s.take, "USB:take_004")));
  g_usb = false;
  CHECK(P.scan() == 4 && (P.status(&s), !strchr(s.take, ':')));   // pulled: its takes gone from the list
  printf("test_rec_play: a USB drive's takes beside the SD's, one played from the drive: PASS\n");

  std::string rm = "rm -rf " + g_root;
  CHECK(system(rm.c_str()) == 0);
  printf("test_rec_play: PASS\n");
  return 0;
}
