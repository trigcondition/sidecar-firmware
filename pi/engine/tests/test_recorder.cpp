// pi/engine/tests/test_recorder.cpp - the recorder (recorder.h) on a temp folder: the WAV header and its boot-time fix,
// the channel lists, a take's files (headers, lengths, every sample where it belongs: the files line up), 16-bit,
// the ring's overrun (dropped blocks written as silence, the files still aligned), arming by PLAY/STOP.
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "check.h"
#include "rec_wav.h"
#include "recorder.h"

static std::string g_root;
static FILE* g_f[rec::kSources];
static uint64_t g_pos[rec::kSources];   // each file's write position (its end, unless space was reserved)
static std::vector<std::string> g_log;
// "/Recordings/..." the SD card, "USB:/Recordings/..." the drive: <root>/USB/Recordings/...
static std::string disk(const char* p) {
  const char* c = strchr(p, ':');
  if (c && p[0] != '/') return g_root + "/" + std::string(p, (size_t)(c - p)) + (c + 1);
  return g_root + p;
}

static bool BMkdir(const char* p) { return mkdir(disk(p).c_str(), 0755) == 0 || access(disk(p).c_str(), F_OK) == 0; }
static int BOpen(const char* p) {
  for (int i = 0; i < rec::kSources; ++i)
    if (!g_f[i]) return g_pos[i] = 0, (g_f[i] = fopen(disk(p).c_str(), "wb+")) ? i : -1;
  return -1;
}
static bool BWrite(int h, const void* p, uint32_t n) {
  bool ok = fseeko(g_f[h], (off_t)g_pos[h], SEEK_SET) == 0 && fwrite(p, 1, n, g_f[h]) == n;
  if (ok) g_pos[h] += n;
  return ok;
}
static bool BPwrite(int h, uint64_t off, const void* p, uint32_t n) {
  return fseeko(g_f[h], (off_t)off, SEEK_SET) == 0 && fwrite(p, 1, n, g_f[h]) == n;   // (the position kept: g_pos)
}
static bool BSync(int h) { return fflush(g_f[h]) == 0; }
static bool BClose(int h) { fclose(g_f[h]); g_f[h] = nullptr; return true; }
static bool BSpace(uint64_t* f, uint64_t* t) { *f = 1ull << 33, *t = 1ull << 34; return true; }
static uint32_t g_skew = 0;   // added to the clock (a write that "took" seconds: a drive's timeout)
static uint32_t BNow() { timeval tv; gettimeofday(&tv, nullptr); return (uint32_t)(tv.tv_sec * 1000000 + tv.tv_usec) + g_skew; }
static int BList(const char* d, char (*names)[64], uint64_t* sizes, bool* dirs, int max) {
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
    ++n;
  }
  closedir(dp);
  return n;
}
static bool BRead(const char* p, uint64_t off, void* b, uint32_t n) {
  FILE* f = fopen(disk(p).c_str(), "rb");
  if (!f) return false;
  bool ok = fseek(f, (long)off, SEEK_SET) == 0 && fread(b, 1, n, f) == n;
  fclose(f);
  return ok;
}
static bool BPatch(const char* p, uint64_t off, const void* b, uint32_t n) {
  FILE* f = fopen(disk(p).c_str(), "rb+");
  if (!f) return false;
  bool ok = fseek(f, (long)off, SEEK_SET) == 0 && fwrite(b, 1, n, f) == n;
  fclose(f);
  return ok;
}
static void BLog(const char* l) { g_log.push_back(l); printf("  log: %s\n", l); }
static const rec::Backend kB = {BMkdir, BOpen, BWrite, BPwrite, BSync, BClose, BSpace, BNow, BList, BRead, BPatch, BLog};
// the card queue's say (core/msamp/cardq.h): the streams need the card while g_busy; the writes' sizes
static bool g_busy = false;
static std::vector<uint32_t> g_writes;
static bool BWriteLog(int h, const void* p, uint32_t n) { g_writes.push_back(n); return BWrite(h, p, n); }
static bool BBusy() { return g_busy; }
static const rec::Backend kBQ = {BMkdir, BOpen, BWriteLog, BPwrite, BSync, BClose, BSpace, BNow, BList, BRead, BPatch, BLog, BBusy,
                                 nullptr, nullptr, nullptr, nullptr};
// a drive (set_root "USB:"): mounted or not, its space, the marker's removal, a pulled drive (g_fail 2: every call
// fails, the first after a "3 s" wait; 1: at once, as FatFs on an unmounted volume)
static bool g_mounted = true;
static int g_fail = 0, g_abandoned = 0, g_open_now = 0, g_syncs = 0;
static int g_list_mode = 0;          // 1: every listing comes back empty (short), 2: fails (a readdir error: -2)
static int g_pwrite_fail = 0;        // 2: the header writes time out ("3 s") and fail
static int g_sync_fail = 0, g_pwrites = 0, g_sync_fails = 0;
static bool g_free_unknown = false;  // a FAT32 without FSINFO: the drive's free space not counted
static int BOpenD(const char* p) {
  if (g_fail) return -1;
  int h = BOpen(p);
  if (h >= 0) g_open_now++;
  return h;
}
static bool BWriteD(int h, const void* p, uint32_t n) {
  g_writes.push_back(n);
  if (g_fail == 2) g_skew += 3000000;   // Circle's transfer timeout
  if (g_fail) return false;
  return BWrite(h, p, n);
}
static bool BPwriteD(int h, uint64_t off, const void* p, uint32_t n) {
  g_pwrites++;
  if (g_pwrite_fail == 2) g_skew += 3000000;
  return !g_fail && !g_pwrite_fail && BPwrite(h, off, p, n);
}
static bool BSyncD(int h) {
  g_syncs++;
  if (g_sync_fail) return g_sync_fails++, g_skew += 3000000, false;
  return !g_fail && BSync(h);
}
static int BListD(const char* d, char (*names)[64], uint64_t* sizes, bool* dirs, int max) {
  if (g_list_mode == 2) return -2;
  int n = BList(d, names, sizes, dirs, max);
  return g_list_mode == 1 && n > 0 ? 0 : n;
}
static int g_mkdir_fail = 0, g_mkdirs = 0;   // 2: the take folder's mkdir times out ("3 s") and fails
static int BMkdirNew(const char* p) {
  g_mkdirs++;
  if (g_mkdir_fail == 2) return g_skew += 3000000, -1;
  if (g_fail) return -1;
  return mkdir(disk(p).c_str(), 0755) == 0 ? 1 : errno == EEXIST ? 0 : -1;
}
static bool BCloseD(int h) { g_open_now--; BClose(h); return !g_fail; }
static bool BReady(const char* root) { return strcmp(root, "USB:") == 0 && g_mounted; }
static bool BSpaceAt(const char* root, uint64_t* f, uint64_t* t) {
  if (strcmp(root, "USB:") || !g_mounted) return false;
  *f = g_free_unknown ? ~0ull : 60ull << 30, *t = 64ull << 30;
  return true;
}
static bool BRemove(const char* p) { return !g_fail && unlink(disk(p).c_str()) == 0; }
static void BAbandon(int h) { g_abandoned++; g_open_now--; BClose(h); }   // (the test's FILE closed: no header written)
static const rec::Backend kBD = {BMkdir, BOpenD, BWriteD, BPwriteD, BSyncD, BCloseD, BSpace, BNow, BListD, BRead, BPatch, BLog,
                                 nullptr, BReady, BSpaceAt, BRemove, BAbandon, BMkdirNew, nullptr, nullptr, nullptr, nullptr, nullptr};
// 5d: the wall clock, the Rytm project's name, space reserved up front (f_expand: the file's size at once, sparse
// here), the close's trim and recover's cut
static bool g_clock_on = false, g_reserve_fail = false;
static uint32_t g_local = 0;
static std::string g_project;
static int g_reserves = 0, g_truncates = 0;
static bool BClock(uint32_t* l) { return g_clock_on && (*l = g_local, true); }
static void BProject(char* o, unsigned cap) { snprintf(o, cap, "%s", g_project.c_str()); }
static bool g_cut_fail = false, g_trunc_fail = false, g_patch_fail = false, g_reserve_slow = false;
static int g_reserve_ok = 99;   // reservations that succeed (then each refused: no contiguous room)
static int g_syncs_n = 0;
// f_expand does not clear the area: what the card held before. Here 0xA5 garbage, so an area that leaks into a
// file's data (a header grown over it) shows: no sample of ours ends in 0xA5 (in_val's low byte is the channel)
static bool BReserve(int h, uint64_t n) {
  g_reserves++;
  if (g_reserve_slow) g_skew += 1000000;
  fflush(g_f[h]);
  struct stat st;
  if (g_reserve_fail || g_reserve_ok <= 0 || fstat(fileno(g_f[h]), &st) || st.st_size != 0) return false;   // (an empty file only)
  g_reserve_ok--;
  static std::vector<uint8_t> junk(1u << 20, 0xA5);
  for (uint64_t o = 0; o < n; o += junk.size())
    if (pwrite(fileno(g_f[h]), junk.data(), (size_t)(n - o < junk.size() ? n - o : junk.size()), (off_t)o) <= 0) return false;
  return true;
}
static bool BTruncate(int h) {
  g_truncates++;
  fflush(g_f[h]);
  return !g_trunc_fail && ftruncate(fileno(g_f[h]), (off_t)g_pos[h]) == 0;
}
static bool BCut(const char* p, uint64_t n) { return !g_cut_fail && truncate(disk(p).c_str(), (off_t)n) == 0; }
static bool BSyncN(int h) { g_syncs_n++; return BSync(h); }
static bool BPatchN(const char* p, uint64_t off, const void* b, uint32_t n) { return !g_patch_fail && BPatch(p, off, b, n); }
static bool BSpaceN(uint64_t* f, uint64_t* t) { *f = 64ull << 20, *t = 1ull << 34; return true; }   // 16 MB a file (2)
static int BMkdirNewS(const char* p) { return mkdir(disk(p).c_str(), 0755) == 0 ? 1 : errno == EEXIST ? 0 : -1; }
static bool BRemoveS(const char* p) { return unlink(disk(p).c_str()) == 0; }
static const rec::Backend kBN = {BMkdir, BOpen, BWrite, BPwrite, BSyncN, BClose, BSpaceN, BNow, BList, BRead, BPatchN, BLog,
                                 nullptr, nullptr, nullptr, BRemoveS, nullptr, BMkdirNewS, BClock, BProject, BReserve, BTruncate, BCut};
// no 0xA5 garbage of a reserved area in a file's data (every 3-byte sample's low byte is a channel number: < 12)
static bool no_junk(const std::vector<uint8_t>& v) {
  if (v.size() < 512) return false;
  uint32_t d = recwav::get32(v.data() + 508);
  if (512ull + d > v.size()) return false;
  for (uint32_t i = 0; i + 3 <= d; i += 3)
    if (v[512 + i] == 0xA5) return false;
  return true;
}
static uint64_t fsize(const char* p) { struct stat st; return stat(disk(p).c_str(), &st) ? 0 : (uint64_t)st.st_size; }

static std::vector<uint8_t> slurp(const std::string& p) {
  std::vector<uint8_t> v;
  FILE* f = fopen(disk(p.c_str()).c_str(), "rb");
  if (!f) return v;
  fseek(f, 0, SEEK_END);
  v.resize((size_t)ftell(f));
  fseek(f, 0, SEEK_SET);
  if (fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
  fclose(f);
  return v;
}

// the synthetic stream: IN channel c of frame t = (t << 8 | c) << 8 (unique per frame and channel, 24 bits of it kept)
static int32_t in_val(uint32_t t, int c) { return (int32_t)(((t & 0xFFFF) << 8 | (uint32_t)c) << 8); }
static int16_t voice_val(uint32_t t, int v) { return (int16_t)((t * 7 + (uint32_t)v * 1000) & 0x7FFF); }

static void feed(rec::Recorder& r, uint32_t& t, unsigned blocks, bool poll) {
  for (unsigned b = 0; b < blocks; ++b) {
    int32_t in[rec::kBlockFrames][rec::kInChannels];
    int16_t v[rec::kBlockFrames][rec::kVoices];
    float fx[rec::kBlockFrames][4];
    for (unsigned f = 0; f < rec::kBlockFrames; ++f) {
      for (int c = 0; c < rec::kInChannels; ++c) in[f][c] = in_val(t + f, c);
      for (int i = 0; i < rec::kVoices; ++i) v[f][i] = voice_val(t + f, i);
      for (int i = 0; i < 4; ++i) fx[f][i] = (i & 1 ? -0.25f : 0.5f);
    }
    r.out_block(v, fx, rec::kBlockFrames);
    r.in_block(in, rec::kBlockFrames);
    t += rec::kBlockFrames;
    if (poll) r.poll();
  }
}
static void finish(rec::Recorder& r) {
  for (int i = 0; i < 100000; ++i) {
    r.poll();
    rec::Status s;
    r.status(&s, false);
    if (s.state == rec::Recorder::kIdle || s.state == rec::Recorder::kArmed) return;
  }
  CHECK(!"the take never closed");
}
static int32_t s24(const uint8_t* p) { return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8; }

int main() {
  char tmp[] = "/tmp/rec-test-XXXXXX";
  CHECK(mkdtemp(tmp));
  g_root = tmp;

  // ---- WAV header
  uint8_t h[recwav::kHeader];
  recwav::build(h, 2, 24, 600);
  CHECK(recwav::ours(h) == 6 && recwav::get32(h + 4) == 504 + 600 && recwav::get32(h + 508) == 600);
  CHECK(recwav::get32(h + 24) == 48000 && recwav::get32(h + 28) == 288000 && recwav::get16(h + 34) == 24);
  CHECK(recwav::placeholder(3) % 3 == 0 && 504ull + recwav::placeholder(3) <= 0xFFFFFFFFull);
  recwav::build(h, 1, 24, recwav::placeholder(3));
  CHECK(recwav::fix(h, 512 + 3000 + 2) && recwav::get32(h + 508) == 3000);   // a partial frame dropped
  CHECK(!recwav::fix(h, 512 + 3000));                                        // already right
  CHECK(!recwav::open_ended(h) && !recwav::fix(h, 512 + 6000));             // closed: never grown over a longer file
  CHECK(recwav::fix(h, 512 + 1500) && recwav::get32(h + 508) == 1500);       // ... only shrunk to a shorter one
  recwav::build(h, 1, 24, recwav::placeholder(3));
  CHECK(recwav::open_ended(h));
  uint8_t bad[512] = {0};
  CHECK(!recwav::ours(bad) && !recwav::fix(bad, 9999));

  // ---- channel lists
  uint32_t m = 0;
  char err[64];
  CHECK(rec::parse_list("all", &m, err, sizeof err) && m == (1u << rec::kSources) - 1);
  CHECK(rec::parse_list("none", &m, err, sizeof err) && m == 0);
  CHECK(rec::parse_list("master,bd,rs/cp,in_r,voice8,fx2", &m, err, sizeof err));
  CHECK(m == (1u << 0 | 1u << 3 | 1u << 5 | 1u << 12 | 1u << 20 | 1u << 22));
  CHECK(rec::parse_list("-bd,+1", &m, err, sizeof err) && m == (1u << 0 | 1u << 1 | 1u << 5 | 1u << 12 | 1u << 20 | 1u << 22));
  CHECK(rec::parse_list("0,in", &m, err, sizeof err) && m == (1u | 0xFFFu << 1));
  CHECK(!rec::parse_list("bd,xx", &m, err, sizeof err) && strstr(err, "xx"));
  CHECK(!rec::parse_list("25", &m, err, sizeof err));
  CHECK(rec::parse_list("pre,post,24", &m, err, sizeof err) && m == (1u << 23 | 1u << 24));   // the OUT route's pairs
  CHECK(rec::samples_per_frame(1u << 13 | 1u << 15, 0x05) == 4 && rec::samples_per_frame(1u << 13, 0x02) == 1);
  char ids[80];
  rec::list_ids(1u << 0 | 1u << 13 | 1u << 22, ids, sizeof ids);
  CHECK(strcmp(ids, "0,13,22") == 0);
  CHECK(rec::samples_per_frame(1u | 1u << 1 | 1u << 21) == 5 && rec::files(1u | 1u << 1 | 1u << 21) == 3);
  CHECK(rec::bytes_per_second(1u | 0xFFFu << 1, 24) == 14 * 3 * 48000);   // the 5-minute test's selection

  static rec::Recorder r;
  size_t memsz = (size_t)rec::kSources * rec::kChunk + (8u << 20);
  void* mem = aligned_alloc(64, memsz);
  r.init(mem, memsz, &kB);

  // ---- a take: master, MAIN_L, BD, IN_R, VOICE1, FX1; 24-bit; long enough for several chunks of each file
  CHECK(rec::parse_list("master,main_l,bd,in_r,voice1,fx1", &m, err, sizeof err));
  CHECK(r.configure(m, 24, err, sizeof err));
  CHECK(r.set_prefix("t") && !r.set_prefix("a/b") && r.set_prefix("") && r.set_prefix("t"));   // "": the project's name
  CHECK(r.start(err, sizeof err));
  uint32_t t = 0;
  feed(r, t, 1, true);   // the block that starts it is not recorded
  uint32_t t0 = t;
  feed(r, t, 30000, true);   // 210000 frames
  r.stop();
  finish(r);
  uint32_t frames = t - t0;
  rec::Status st;
  r.status(&st, true);
  CHECK(st.frames == frames && st.drops == 0 && st.side_under == 0 && strcmp(st.take, "t_take-001") == 0);
  CHECK(st.peak[3] > 0 && st.peak[3] <= 8388607);
  const char* names[] = {"00_MASTER", "01_MAIN_L", "03_BD", "12_IN_R", "13_VOICE1", "21_FX1"};
  int width[] = {2, 1, 1, 1, 1, 2};
  for (int k = 0; k < 6; ++k) {
    auto v = slurp(std::string("/Recordings/t_take-001/") + names[k] + ".wav");
    uint32_t align = 3 * width[k];
    CHECK(v.size() >= 512 && recwav::ours(v.data()) == align);
    uint32_t data = recwav::get32(v.data() + 508);
    CHECK(data == frames * align && v.size() == 512 + data + (data & 1));
    for (uint32_t f = 0; f < frames; f += 997) {   // every channel's sample at frame f is frame t0+f's
      const uint8_t* p = v.data() + 512 + f * align;
      uint32_t tt = t0 + f;
      if (k == 0) CHECK(s24(p) == in_val(tt, 0) >> 8 && s24(p + 3) == in_val(tt, 1) >> 8);
      if (k == 1) CHECK(s24(p) == in_val(tt, 0) >> 8);
      if (k == 2) CHECK(s24(p) == in_val(tt, 2) >> 8);
      if (k == 3) CHECK(s24(p) == in_val(tt, 11) >> 8);
      if (k == 4) CHECK(s24(p) == (int32_t)voice_val(tt - 7, 0) << 8);   // the OUT side's lead: the block before
      if (k == 5) CHECK(s24(p) == 0x400000 && s24(p + 3) == -0x200000);
    }
  }
  printf("test_recorder: a take of 6 files, %u frames each, aligned: PASS\n", frames);

  // ---- 16-bit, and the ring overrun: a small ring, nothing drained while 60000 frames come
  static rec::Recorder r2;
  size_t small = (size_t)rec::kSources * rec::kChunk + (1u << 20);
  void* mem2 = aligned_alloc(64, small);
  r2.init(mem2, small, &kB);
  CHECK(rec::parse_list("main_l,main_r,sd", &m, err, sizeof err));
  CHECK(r2.configure(m, 16, err, sizeof err) && r2.start(err, sizeof err));
  t = 0;
  feed(r2, t, 1, false);
  t0 = t;
  feed(r2, t, 120000 / 7, false);   // (every slot of the 1 MB used: 11397 of 23 words)
  rec::Status s2;
  r2.status(&s2, false);
  CHECK(s2.drops > 0 && s2.ring_used == s2.ring_slots);
  uint32_t dropped = s2.dropped_frames;
  feed(r2, t, 2000, true);   // drained again (its first block still dropped), then more
  r2.stop();
  finish(r2);
  frames = t - t0;
  r2.status(&s2, false);
  CHECK(s2.frames == frames && s2.dropped_frames == dropped + 7);
  dropped = s2.dropped_frames;
  {
    auto a = slurp("/Recordings/sidecar_take-001/01_MAIN_L.wav"), c = slurp("/Recordings/sidecar_take-001/04_SD.wav");   // r2: no prefix, no project
    CHECK(recwav::ours(a.data()) == 2 && recwav::get32(a.data() + 508) == frames * 2 && c.size() == a.size());
    uint32_t kept = s2.ring_slots * rec::kBlockFrames, zero = 0, ok = 0;
    for (uint32_t f = 0; f < frames; ++f) {
      int16_t x = (int16_t)recwav::get16(a.data() + 512 + 2 * f), y = (int16_t)recwav::get16(c.data() + 512 + 2 * f);
      if (f < kept) CHECK(x == (int16_t)(in_val(t0 + f, 0) >> 16) && y == (int16_t)(in_val(t0 + f, 3) >> 16));
      else if (f < kept + dropped) { CHECK(x == 0 && y == 0); zero++; }
      else { CHECK(x == (int16_t)(in_val(t0 + f, 0) >> 16) && y == (int16_t)(in_val(t0 + f, 3) >> 16)); ok++; }
    }
    CHECK(zero == dropped && ok > 0);
    printf("test_recorder: 16-bit, overrun: %u blocks dropped, %u frames of silence, files still in time: PASS\n", s2.drops, zero);
  }

  // ---- armed: PLAY starts a take, STOP ends it, armed again; the next PLAY makes the next take
  CHECK(rec::parse_list("bd", &m, err, sizeof err) && r.configure(m, 24, err, sizeof err));
  CHECK(r.arm(err, sizeof err));
  t = 0;
  feed(r, t, 100, true);
  r.status(&st, false);
  CHECK(st.state == rec::Recorder::kArmed && st.frames == 0);
  r.transport(0xFA);
  feed(r, t, 1001, true);
  r.transport(0xFC);
  finish(r);
  r.status(&st, false);
  CHECK(st.state == rec::Recorder::kArmed && st.frames == 7000 && strcmp(st.take, "t_take-002") == 0);
  r.transport(0xFA);
  feed(r, t, 11, true);
  r.stop();   // a manual stop: the take ends and the arming with it
  finish(r);
  r.status(&st, false);
  CHECK(st.state == rec::Recorder::kIdle && st.frames == 70 && strcmp(st.take, "t_take-003") == 0);
  CHECK(!r.configure(m, 20, err, sizeof err));

  // ---- the boot scan: a take cut by a power loss (placeholder sizes, a partial frame at the end)
  {
    FILE* f = fopen(disk("/Recordings/t_take-003/03_BD.wav").c_str(), "rb+");
    recwav::build(h, 1, 24, recwav::placeholder(3));
    fwrite(h, 1, 512, f);
    fclose(f);
    truncate(disk("/Recordings/t_take-003/03_BD.wav").c_str(), 512 + 199);   // 66 frames and a third
    CHECK(r.fix_all("/Recordings") == 1 && r.fix_all("/Recordings") == 0);
    auto v = slurp("/Recordings/t_take-003/03_BD.wav");
    CHECK(recwav::get32(v.data() + 508) == 198 && recwav::get32(v.data() + 4) == 504 + 198);
  }
  // ---- the card queue: every write a piece of at most 64 KB (a 192 KB chunk in three); held back while the streams
  // need the card, unless the ring is half full; the take whole and in time either way
  {
    static rec::Recorder rq;
    void* mem3 = aligned_alloc(64, memsz);
    rq.init(mem3, memsz, &kBQ);
    CHECK(rec::parse_list("master,bd", &m, err, sizeof err) && rq.configure(m, 24, err, sizeof err));
    CHECK(rq.set_prefix("q") && rq.start(err, sizeof err));
    t = 0;
    feed(rq, t, 1, true);
    t0 = t;
    g_writes.clear();
    g_busy = true;
    feed(rq, t, 20000, true);   // 140000 frames: 840 KB of MASTER, all held back (the ring far from half)
    rec::Status sq;
    rq.status(&sq, false);
    CHECK(sq.yields > 0 && sq.drops == 0);
    size_t held = g_writes.size();
    CHECK(held <= 2);   // (the headers only)
    g_busy = false;
    feed(rq, t, 20000, true);
    rq.stop();
    finish(rq);
    rq.status(&sq, false);
    uint32_t big = 0;
    for (size_t i = held; i < g_writes.size(); ++i) big = g_writes[i] > big ? g_writes[i] : big;
    CHECK(big == rec::kPiece && sq.frames == t - t0 && sq.drops == 0);
    auto v = slurp("/Recordings/q_take-001/00_MASTER.wav");
    CHECK(recwav::get32(v.data() + 508) == (t - t0) * 6);
    for (uint32_t f = 0; f < t - t0; f += 1009) CHECK(s24(v.data() + 512 + f * 6) == in_val(t0 + f, 0) >> 8);
    printf("test_recorder: pieces of %u KB, %u held back for the streams, the take whole: PASS\n", big >> 10, sq.yields);
    free(mem3);
  }
  {   // ---- the display's meters: every channel, idle or recording, apart from Status::peak; taken resets them
    static rec::Recorder rm;
    rm.init(mem, memsz, &kB);
    CHECK(!rm.wants_in() && !rm.wants_side());
    rm.set_meters(true);
    CHECK(rm.wants_in() && rm.wants_side());   // idle: the producer still calls it
    int32_t in[rec::kBlockFrames][rec::kInChannels] = {};
    in[3][0] = 0x40000000, in[5][0] = -0x7FFFFFFF - 1;   // MAIN L: half, then full scale negative
    in[1][9] = -0x01000000;                              // IN 9 (CY/CB)
    int16_t vo[rec::kBlockFrames][rec::kVoices] = {};
    vo[2][4] = -16384;                                   // voice 5
    float fx[rec::kBlockFrames][4] = {};
    fx[6][3] = 0.25f;                                    // FX 2 R
    rm.in_block(in, rec::kBlockFrames);
    rm.out_block(vo, fx, rec::kBlockFrames);
    uint32_t pk[rec::kMeters];
    rm.meter_peaks(pk);
    CHECK(pk[0] == 8388607 && pk[1] == 0 && pk[9] == 0x10000 - 1);
    CHECK(pk[rec::kInChannels + 4] == 16384 * 256 - 1);
    uint32_t f2r = pk[rec::kInChannels + rec::kVoices + 3];
    CHECK(f2r >= 0x200000 - 1 && f2r <= 0x200000);
    rec::Status sm;
    rm.status(&sm, true);
    CHECK(sm.peak[1] == 0);   // the web's peaks untouched (idle: none)
    rm.meter_peaks(pk);
    CHECK(pk[0] == 0 && pk[9] == 0);   // taken
    rm.set_meters(false);
    rm.in_block(in, rec::kBlockFrames);
    rm.meter_peaks(pk);
    CHECK(pk[0] == 0 && !rm.wants_in());
    printf("test_recorder: meters: PASS\n");
  }
  {   // ---- the OUT route (core/fx/out.h): voice 3 on POST when the take starts: 15_VOICE3 stereo (its L and R as
      // rendered), voice 1 (TRK) mono as before, 23_PRE / 24_POST the pairs as sent; voice 3 back on TRK: mono again
    static rec::Recorder rs;
    rs.init(mem, memsz, &kB);
    CHECK(rec::parse_list("voice1,voice3,pre,post", &m, err, sizeof err) && rs.configure(m, 24, err, sizeof err));
    CHECK(rs.set_prefix("st"));
    rs.set_stereo_voices(1u << 2);
    CHECK(rs.start(err, sizeof err) && rs.stereo_voices() == 1u << 2);
    uint32_t ts = 0, ts0 = 0;
    auto feed2 = [&](unsigned blocks) {
      for (unsigned b = 0; b < blocks; ++b) {
        int32_t in[rec::kBlockFrames][rec::kInChannels] = {};
        int16_t v[rec::kBlockFrames][rec::kVoices], lr[rec::kBlockFrames][2 * rec::kVoices];
        float fx[rec::kBlockFrames][4] = {}, pr[rec::kBlockFrames][4];
        for (unsigned f = 0; f < rec::kBlockFrames; ++f) {
          for (int i = 0; i < rec::kVoices; ++i) {
            lr[f][i] = voice_val(ts + f, i), lr[f][8 + i] = (int16_t)-voice_val(ts + f, i);
            v[f][i] = (int16_t)(((int)lr[f][i] + lr[f][8 + i]) / 2);
          }
          for (int i = 0; i < 4; ++i) pr[f][i] = (i + 1) * 0.125f * (i & 1 ? -1 : 1);
        }
        rs.out_block(v, fx, rec::kBlockFrames, lr, pr);
        rs.in_block(in, rec::kBlockFrames);
        ts += rec::kBlockFrames;
        rs.poll();
      }
    };
    feed2(1);
    ts0 = ts;
    feed2(2000);
    rs.stop();
    finish(rs);
    uint32_t n = ts - ts0;
    auto v1 = slurp("/Recordings/st_take-001/13_VOICE1.wav"), v3 = slurp("/Recordings/st_take-001/15_VOICE3.wav");
    auto pre = slurp("/Recordings/st_take-001/23_PRE.wav"), post = slurp("/Recordings/st_take-001/24_POST.wav");
    CHECK(recwav::ours(v1.data()) == 3 && recwav::ours(v3.data()) == 6 && recwav::ours(pre.data()) == 6 && recwav::ours(post.data()) == 6);
    CHECK(recwav::get32(v3.data() + 508) == n * 6 && recwav::get32(v1.data() + 508) == n * 3);
    for (uint32_t f = 0; f < n; f += 97) {
      uint32_t tt = ts0 + f - 7;   // (the OUT side's lead: the block before)
      const uint8_t* p3 = v3.data() + 512 + f * 6;
      CHECK(s24(p3) == (int32_t)voice_val(tt, 2) * 256 && s24(p3 + 3) == -(int32_t)voice_val(tt, 2) * 256);
      CHECK(s24(v1.data() + 512 + f * 3) == 0);   // TRK: the mono value
      const uint8_t* pp = pre.data() + 512 + f * 6, *pq = post.data() + 512 + f * 6;
      CHECK(s24(pp) == 0x100000 && s24(pp + 3) == -0x200000 && s24(pq) == 0x300000 && s24(pq + 3) == -0x400000);
    }
    rs.set_stereo_voices(0);
    CHECK(rs.start(err, sizeof err) && rs.stereo_voices() == 0);
    feed2(200);
    rs.stop();
    finish(rs);
    auto w3 = slurp("/Recordings/st_take-002/15_VOICE3.wav");
    CHECK(recwav::ours(w3.data()) == 3);
    printf("test_recorder: the OUT route: a POST voice's file stereo, the PRE / POST pairs: PASS\n");
  }
  {   // ---- the destination: a USB drive (docs/STORAGE.md "Recording to a USB drive")
    static rec::Recorder rd;
    rd.init(mem, memsz, &kBD);
    CHECK(rec::parse_list("master,bd", &m, err, sizeof err) && rd.configure(m, 24, err, sizeof err) && rd.set_prefix("t"));
    // the names it takes; idle only
    CHECK(rd.set_root("USB:", err, sizeof err) && !strcmp(rd.root(), "USB:"));
    CHECK(rd.set_root("", err, sizeof err) && !strcmp(rd.root(), ""));
    CHECK(!rd.set_root("usb:", err, sizeof err) && !rd.set_root("USB", err, sizeof err) && !rd.set_root("USBXXXX:", err, sizeof err));
    CHECK(!rd.set_root("../:", err, sizeof err) && !rd.set_root(":", err, sizeof err) && strstr(err, "vol:"));
    CHECK(rd.arm(err, sizeof err));
    CHECK(!rd.set_root("USB:", err, sizeof err) && strstr(err, "busy") && !strcmp(rd.root(), ""));   // armed: refused
    rd.stop();
    // not mounted: refused, never the SD instead
    g_mounted = false;
    CHECK(rd.set_root("USB:", err, sizeof err));
    CHECK(!rd.start(err, sizeof err) && !strcmp(err, "USB drive not mounted"));
    CHECK(!rd.arm(err, sizeof err) && !strcmp(err, "USB drive not mounted"));
    rec::Status sd;
    rd.status(&sd, false);
    CHECK(sd.state == rec::Recorder::kIdle && !strcmp(sd.root, "USB:"));
    // mounted: the take on the drive, named after the SD's takes too (t_take-003 is the SD's highest of "t")
    g_mounted = true;
    mkdir(disk("USB:/").c_str(), 0755);
    rd.poll();   // the drive's space (space_at)
    rd.status(&sd, false);
    CHECK(sd.total_bytes == 64ull << 30 && sd.free_bytes == 60ull << 30);
    CHECK(rd.start(err, sizeof err));
    uint32_t td = 0;
    feed(rd, td, 1, true);
    uint32_t td0 = td;
    for (int k = 0; k < 3; ++k) feed(rd, td, 1000, true), g_skew += 1100000;   // 3.3 s of the clock
    feed(rd, td, 10, true);
    rd.status(&sd, false);
    CHECK(sd.state == rec::Recorder::kRecording && !strcmp(sd.take, "USB:t_take-004"));
    CHECK(access(disk("USB:/Recordings/.recording").c_str(), F_OK) == 0);   // the marker while it records
    CHECK(slurp("USB:/Recordings/.recording").size() == sizeof(rec::Marker) && !strcmp((const char*)slurp("USB:/Recordings/.recording").data(), "t_take-004"));
    CHECK(g_syncs >= 3);   // a drive: every file's size reaches the directory every 2 s (2 files: a sync a second)
    rd.stop();
    finish(rd);
    rd.status(&sd, false);
    CHECK(sd.state == rec::Recorder::kIdle && !strcmp(sd.take, "USB:t_take-004") && !sd.abandoned && !sd.error[0]);
    CHECK(access(disk("USB:/Recordings/.recording").c_str(), F_OK) != 0);   // closed cleanly: gone
    CHECK(access(disk("/Recordings/t_take-004").c_str(), F_OK) != 0);            // nothing on the SD
    auto v = slurp("USB:/Recordings/t_take-004/03_BD.wav");
    CHECK(v.size() > 512 && recwav::get32(v.data() + 508) == (td - td0) * 3);
    for (uint32_t f = 0; f < td - td0; f += 101) CHECK(s24(v.data() + 512 + f * 3) == in_val(td0 + f, 2) >> 8);
    CHECK(rd.recover("USB:") == -1);   // no marker
    // the next take on the drive: t_take-005 (the drive's own t_take-004)
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 300, true);
    rd.stop();
    finish(rd);
    rd.status(&sd, false);
    CHECK(!strcmp(sd.take, "USB:t_take-005"));
    printf("test_recorder: a take to the USB drive (USB:t_take-004, numbered after the SD's), the marker while it records: PASS\n");

    // pulled while it records: the write waits a timeout (3 s in Circle) and fails - no more I/O, the files left as
    // they are (no header, no close), the error shown, the marker kept; recover() fixes them when the drive is back
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 1, true);
    feed(rd, td, 30000, true);   // 210000 frames: a whole chunk of MASTER written
    g_fail = 2;                 // pulled: every call fails from here, the first after "3 s"
    size_t calls = g_writes.size();
    feed(rd, td, 20000, true);
    finish(rd);
    rd.status(&sd, false);
    CHECK(sd.state == rec::Recorder::kIdle && sd.abandoned && !strcmp(sd.take, "USB:t_take-006"));
    CHECK(!strcmp(sd.error, "write failed (drive full or pulled?)"));
    CHECK(g_writes.size() == calls + 1);   // the one that timed out, nothing after it
    CHECK(g_abandoned == 2 && g_open_now == 0);   // both handles let go
    CHECK(access(disk("USB:/Recordings/.recording").c_str(), F_OK) == 0);
    int pulled_logs = 0;
    for (auto& l : g_log) pulled_logs += strstr(l.c_str(), "write failed") != nullptr;
    CHECK(pulled_logs == 1);   // one error, not one per file
    g_fail = 0;
    g_skew = 0;
    auto b0 = slurp("USB:/Recordings/t_take-006/00_MASTER.wav");
    CHECK(b0.size() > 512 && recwav::get32(b0.data() + 508) != b0.size() - 512);   // the placeholder sizes
    CHECK(rd.recover("USB:") == 2 && access(disk("USB:/Recordings/.recording").c_str(), F_OK) != 0);
    b0 = slurp("USB:/Recordings/t_take-006/00_MASTER.wav");
    CHECK(recwav::get32(b0.data() + 508) == (b0.size() - 512) / 6 * 6);
    printf("test_recorder: the drive pulled mid-take: one failed call, the files let go, recovered when back: PASS\n");

    // unmounted under it (FatFs refuses at once): the take ends with the error, the close tried (it fails), the
    // marker kept
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 1, true);
    feed(rd, td, 30000, true);
    g_fail = 1;
    feed(rd, td, 20000, true);   // (the next chunk's write: refused)
    finish(rd);
    rd.status(&sd, false);
    CHECK(sd.state == rec::Recorder::kIdle && !sd.abandoned && strstr(sd.error, "drive"));
    CHECK(access(disk("USB:/Recordings/.recording").c_str(), F_OK) == 0);
    g_fail = 0;
    CHECK(rd.recover("USB:") >= 0 && access(disk("USB:/Recordings/.recording").c_str(), F_OK) != 0);

    // pulled while armed: the PLAY that starts the take finds no drive - ended with the error, nothing on the SD
    CHECK(rd.arm(err, sizeof err));
    feed(rd, td, 10, true);
    g_mounted = false;
    rd.transport(0xFA);
    feed(rd, td, 50, true);
    finish(rd);
    rd.status(&sd, false);
    CHECK(sd.state == rec::Recorder::kIdle && !strcmp(sd.error, "USB drive not mounted"));
    g_mounted = true;
    // the SD again: a take there, its marker there and gone at the close
    CHECK(rd.set_root("", err, sizeof err) && rd.start(err, sizeof err));
    feed(rd, td, 100, true);
    CHECK(access(disk("/Recordings/.recording").c_str(), F_OK) == 0);
    rd.stop();
    finish(rd);
    rd.status(&sd, false);
    CHECK(!strcmp(sd.root, "") && !strncmp(sd.take, "t_take-00", 9) && !strchr(sd.take, ':'));
    CHECK(access(disk("/Recordings/.recording").c_str(), F_OK) != 0 && rd.recover("") == -1);
    // a marker naming something that is not a take of ours: removed, nothing touched
    FILE* mf = fopen(disk("/Recordings/.recording").c_str(), "wb");
    char junk[40] = "../../etc";
    fwrite(junk, 1, sizeof junk, mf);
    fclose(mf);
    CHECK(rd.recover("") == 0 && access(disk("/Recordings/.recording").c_str(), F_OK) != 0);
    printf("test_recorder: drive unmounted mid-take, pulled while armed, the SD again, a bad marker: PASS\n");

    // ---- the numbering never reuses a take: a short listing (n_take-001 not seen) makes n_take-002, a failed one no take at all;
    // the existing take's files untouched either way
    CHECK(rd.set_root("USB:", err, sizeof err) && rd.set_prefix("n"));
    mkdir(disk("USB:/Recordings/n_take-001").c_str(), 0755);
    FILE* kf = fopen(disk("USB:/Recordings/n_take-001/03_BD.wav").c_str(), "wb");
    fputs("KEEP", kf), fclose(kf);
    g_list_mode = 1;
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 50, true);
    rd.stop();
    finish(rd);
    rd.status(&sd, false);
    CHECK(!strcmp(sd.take, "USB:n_take-002") && !sd.error[0]);
    CHECK(slurp("USB:/Recordings/n_take-001/03_BD.wav").size() == 4);
    g_list_mode = 2;
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 50, true);
    finish(rd);
    rd.status(&sd, false);
    CHECK(sd.state == rec::Recorder::kIdle && strstr(sd.error, "cannot read") && access(disk("USB:/Recordings/n_take-003").c_str(), F_OK) != 0);
    CHECK(slurp("USB:/Recordings/n_take-001/03_BD.wav").size() == 4);
    g_list_mode = 0;
    // a drive that errors (hangs) at the take's folder: one mkdir, the take refused at once, the drive marked gone
    g_mkdir_fail = 2;
    g_mkdirs = 0;
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 50, true);
    finish(rd);
    rd.status(&sd, false);
    CHECK(g_mkdirs == 1 && sd.state == rec::Recorder::kIdle && strstr(sd.error, "folder"));
    g_mkdir_fail = 0;
    g_skew = 0;
    // an existing folder (n_take-002 there, the listing short): the next number, as before
    g_list_mode = 1;
    g_mkdirs = 0;
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 50, true);
    rd.stop();
    finish(rd);
    rd.status(&sd, false);
    CHECK(!strcmp(sd.take, "USB:n_take-003") && g_mkdirs == 3 && !sd.error[0]);   // n_take-001, n_take-002 there
    g_list_mode = 0;
    printf("test_recorder: a short listing takes the next free name, a failed one no take; nothing truncated: PASS\n");

    // ---- a drive that stops answering at the close (the headers): one timeout, the other files let go
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 200, true);
    g_pwrite_fail = 2;
    g_pwrites = g_abandoned = 0;
    rd.stop();
    finish(rd);
    rd.status(&sd, false);
    CHECK(g_pwrites == 1 && g_abandoned == 2 && sd.abandoned);   // MASTER's header timed out (let go), BD's never tried
    g_pwrite_fail = 0;
    g_skew = 0;
    CHECK(rd.recover("USB:") >= 0);
    // ... and at a sync while it records: the take ends, nothing more sent
    CHECK(rd.start(err, sizeof err));
    feed(rd, td, 100, true);
    g_sync_fail = 1;
    g_sync_fails = 0;
    g_skew += 2100000;   // the next poll syncs
    size_t w0 = g_writes.size();
    feed(rd, td, 3000, true);
    finish(rd);
    rd.status(&sd, false);
    CHECK(g_sync_fails == 1 && g_writes.size() == w0 && sd.abandoned && sd.state == rec::Recorder::kIdle && sd.error[0]);
    g_sync_fail = 0;
    g_skew = 0;
    CHECK(rd.recover("USB:") >= 0);
    printf("test_recorder: a drive that stops answering at a header or a sync: one wait, then nothing: PASS\n");

    // ---- a FAT32 without FSINFO: the free space unknown, not 0
    g_free_unknown = true;
    CHECK(rd.set_root("", err, sizeof err) && rd.set_root("USB:", err, sizeof err));
    rd.poll();
    rd.status(&sd, false);
    CHECK(!sd.free_known && sd.free_bytes == 0 && sd.total_bytes == 64ull << 30);
    g_free_unknown = false;
    CHECK(rd.set_root("", err, sizeof err) && rd.set_root("USB:", err, sizeof err));
    rd.poll();
    rd.status(&sd, false);
    CHECK(sd.free_known && sd.free_bytes == 60ull << 30);
    CHECK(rd.set_root("", err, sizeof err));
    rd.poll();
    rd.status(&sd, false);
    CHECK(sd.free_known);
    printf("test_recorder: a drive's free space not counted: unknown, not 0: PASS\n");
  }
  {   // ---- 5d: the sizes, the warning
    CHECK(rec::recbuf_mb(rec::kRecbufAuto, 800, 900) == 256);   // a Pi 4 (2 GB+): half the low heap past 192 MB, <= 256
    CHECK(rec::recbuf_mb(rec::kRecbufAuto, 400, 900) == 104);
    CHECK(rec::recbuf_mb(rec::kRecbufAuto, 300, 900) == 64 && rec::recbuf_mb(rec::kRecbufAuto, 220, 900) == 28);   // what there is
    CHECK(rec::recbuf_mb(rec::kRecbufAuto, 100, 900) == 0);
    CHECK(rec::recbuf_mb(rec::kRecbufAuto, 800, 0) == 64);       // a 1 GB board: one heap, the old 64 MB
    CHECK(rec::recbuf_mb(64, 800, 900) == 64 && rec::recbuf_mb(0, 800, 900) == 0 && rec::recbuf_mb(512, 800, 900) == 512);
    CHECK(rec::recbuf_mb(2000, 4000, 900) == 1024 && rec::recbuf_mb(700, 300, 0) == 700);   // asked: as asked (<= 1024)
    CHECK(rec::reserve_bytes(8ull << 30, true, 25) == 64ull << 20);    // plenty: 64 MB each
    CHECK(rec::reserve_bytes(1ull << 30, true, 25) == 20ull << 20);    // half the free space over the files
    CHECK(rec::reserve_bytes(100ull << 20, true, 25) == 0);            // under 4 MB: none
    CHECK(rec::reserve_bytes(0, false, 2) == 32ull << 20 && rec::reserve_bytes(1ull << 30, true, 0) == 0);
    CHECK(rec::default_write_kbps("") == 4096 && rec::default_write_kbps("USB:") == 6144 && rec::default_write_kbps("USB2:") == 6144);
    CHECK(rec::default_write_kbps("NVME:") == 51200);
    // 25 channels at 24 bit (4.2 MB/s) on the card that dropped blocks (5.0 MB/s): warned; 12 IN channels not
    uint32_t all = rec::bytes_per_second((1u << rec::kSources) - 1, 24) >> 10, in12 = rec::bytes_per_second(rec::kInMask, 24) >> 10;
    CHECK(all == 4218 && rec::rate_warning(all, 5120) && rec::load_pct(all, 5120) == 82);
    CHECK(!rec::rate_warning(in12, 5120) && rec::load_pct(in12, 5120) == 33);
    CHECK(!rec::rate_warning(3584, 5120) && rec::rate_warning(3585, 5120));   // the line: 70 %
    CHECK(!rec::rate_warning(all, 0) && rec::load_pct(all, 0) == 0);          // no speed: no warning
    CHECK(rec::rate_warning(all, rec::default_write_kbps("")) && !rec::rate_warning(all, rec::default_write_kbps("NVME:")));
    static const char* const vn[] = {"SD", "USB", "USB2", "USB3", "NVME"};
    uint32_t sp[5] = {0, 0, 0, 0, 0};
    CHECK(rec::parse_speeds("SD 23456\nUSB 40000\r\nNVME x\nUSB3 \nFOO 9\n\nUSB2 7", vn, 5, sp) == 3);
    CHECK(sp[0] == 23456 && sp[1] == 40000 && sp[2] == 7 && sp[3] == 0 && sp[4] == 0);
    char sb[128];
    CHECK(rec::format_speeds(sb, sizeof sb, vn, 5, sp) > 0 && !strcmp(sb, "SD 23456\nUSB 40000\nUSB2 7\n"));
    uint32_t sq[5] = {};
    CHECK(rec::parse_speeds(sb, vn, 5, sq) == 3 && !memcmp(sp, sq, sizeof sp));
    CHECK(rec::parse_speeds("", vn, 5, sq) == 0 && rec::parse_speeds(nullptr, vn, 5, sq) == 0);
    // the ring: every slot of the memory (no power of 2)
    static rec::Recorder rr;
    rr.init(mem, memsz, &kB);
    CHECK(rec::parse_list("all", &m, err, sizeof err) && rr.configure(m, 24, err, sizeof err));
    rec::Status s;
    rr.status(&s, false);
    uint32_t words = 2 + rec::kBlockFrames * rec::samples_per_frame(m);
    CHECK(s.ring_slots == (memsz - (size_t)rec::kSources * rec::kChunk) / (words * 4));
    CHECK(s.ring_ms == (uint32_t)((uint64_t)s.ring_slots * 7 * 1000 / 48000));
    printf("test_recorder: recbuf %u MB on a Pi 4, reservations, the 70 %% warning, the speeds file, the whole ring: PASS\n",
           rec::recbuf_mb(rec::kRecbufAuto, 800, 900));
  }
  {   // ---- 5d: the take's name from the project and the clock; a name already there; space reserved up front
    static rec::Recorder rn;
    rn.init(mem, memsz, &kBN);
    CHECK(rec::parse_list("master,bd", &m, err, sizeof err) && rn.configure(m, 24, err, sizeof err) && rn.set_prefix(""));
    rec::Status s;
    uint32_t tn = 0;
    auto take = [&](unsigned blocks) {
      CHECK(rn.start(err, sizeof err));
      feed(rn, tn, blocks, true);
      rn.stop();
      finish(rn);
      rn.status(&s, false);
      CHECK(s.state == rec::Recorder::kIdle && !s.error[0]);
      return std::string(s.take);
    };
    g_project = "My Jam: Night #2";
    CHECK(take(20) == "my-jam-night-2_take-001");   // no clock
    CHECK(take(20) == "my-jam-night-2_take-002");
    g_clock_on = true, g_local = 1793111579u;      // 2026-10-27 14:32 (local)
    CHECK(take(20) == "my-jam-night-2_2026-10-27_1432");
    CHECK(take(20) == "my-jam-night-2_2026-10-27_1432_2");   // the same minute: _2, _3
    CHECK(take(20) == "my-jam-night-2_2026-10-27_1432_3");
    mkdir(disk("/Recordings/my-jam-night-2_2026-10-27_1433_5").c_str(), 0755);   // a later name there: after it
    g_local += 60;
    CHECK(take(20) == "my-jam-night-2_2026-10-27_1433_6");
    CHECK(rn.set_prefix("Live"));                  // rec=name: replaces the project's name
    CHECK(take(20) == "Live_2026-10-27_1433");
    CHECK(rn.set_prefix(""));
    g_project = "";                                // no project yet
    CHECK(take(20) == "sidecar_2026-10-27_1433");
    g_project = "UNTITLED";
    g_clock_on = false;
    CHECK(take(20) == "untitled_take-001");
    // the reservation: 16 MB a file (64 MB free, 2 files) while it records, each file synced once at the start (its
    // directory entry owns the area), the file cut to its data at the close
    g_reserves = g_truncates = g_syncs_n = 0;
    CHECK(rn.start(err, sizeof err));
    feed(rn, tn, 1, true);
    CHECK(g_syncs_n == 2);
    feed(rn, tn, 2000, true);
    rn.status(&s, false);
    std::string dir = std::string("/Recordings/") + s.take;
    CHECK(fsize((dir + "/03_BD.wav").c_str()) == 16ull << 20 && g_reserves == 2);
    rn.stop();
    finish(rn);
    rn.status(&s, false);
    auto v = slurp(dir + "/03_BD.wav");
    CHECK(g_truncates == 2 && v.size() == 512 + s.frames * 3 + (s.frames * 3 & 1) && recwav::get32(v.data() + 508) == s.frames * 3);
    CHECK(no_junk(v));
    CHECK(access(disk("/Recordings/.recording").c_str(), F_OK) != 0);
    // no contiguous room (f_expand refuses): 16, 8, 4 MB tried for the first file, none for the rest; the files grow
    // as before, no trim
    g_reserve_fail = true;
    g_truncates = g_reserves = 0;
    std::string nm = take(300);
    CHECK(g_reserves == 3 && g_truncates == 0 && fsize(("/Recordings/" + nm + "/00_MASTER.wav").c_str()) == 512 + 300 * 7 * 6 - 7 * 6);
    g_reserve_fail = false;
    // f_expand slow (a fragmented card): the reserving stops after 1.5 s - the third file gets none; the time logged
    CHECK(rec::parse_list("master,bd,sd", &m, err, sizeof err) && rn.configure(m, 24, err, sizeof err));
    g_reserve_slow = true;
    g_reserves = 0;
    take(20);
    CHECK(g_reserves == 2 && strstr(g_log[g_log.size() - 2].c_str(), "20 MB reserved in 200"));
    g_reserve_slow = false;
    g_skew = 0;
    CHECK(rec::parse_list("master,bd", &m, err, sizeof err) && rn.configure(m, 24, err, sizeof err));
    printf("test_recorder: names from the project and the clock, _2 / _3, take-NNN without a clock; 64 MB reserved, trimmed: PASS\n");

    // a power loss mid-take: the files are the reserved size; the marker has each file's length at its last sync -
    // recover cuts them back to that and fixes the headers (never the reserved area's leftovers)
    CHECK(rn.start(err, sizeof err));
    feed(rn, tn, 1, true);
    feed(rn, tn, 40000, true);                // 280000 frames: whole chunks of both written
    g_skew += 2100000, feed(rn, tn, 10, true);   // a sync (MASTER)
    g_skew += 2100000, feed(rn, tn, 10, true);   // a sync (BD)
    feed(rn, tn, 3000, true);                 // more, not synced
    rn.status(&s, false);
    std::string cut = std::string("/Recordings/") + s.take;
    auto mk = slurp("/Recordings/.recording");
    CHECK(mk.size() == sizeof(rec::Marker));
    rec::Marker mm;
    memcpy(&mm, mk.data(), sizeof mm);
    CHECK(!strcmp(mm.take, s.take) && mm.data[0] > 0 && mm.data[3] > 0 && mm.data[1] == 0);
    CHECK(mm.data[0] % 6 == 0 && mm.data[3] % 3 == 0 && mm.data[3] < s.frames * 3);
    for (int i = 0; i < rec::kSources; ++i)   // the power goes: nothing more written, nothing closed by the recorder
      if (g_f[i]) fflush(g_f[i]), fclose(g_f[i]), g_f[i] = nullptr;
    CHECK(fsize((cut + "/03_BD.wav").c_str()) == 16ull << 20);
    static rec::Recorder rb;   // the next boot
    rb.init(mem2, small, &kBN);
    CHECK(rb.recover("") == 2 && access(disk("/Recordings/.recording").c_str(), F_OK) != 0);
    auto bd = slurp(cut + "/03_BD.wav"), ms = slurp(cut + "/00_MASTER.wav");
    CHECK(bd.size() == 512 + mm.data[3] + (mm.data[3] & 1) && recwav::get32(bd.data() + 508) == mm.data[3]);
    CHECK(ms.size() == 512 + mm.data[0] && recwav::get32(ms.data() + 508) == mm.data[0]);
    for (uint32_t f = 0; f < mm.data[3] / 3; f += 211) CHECK(s24(bd.data() + 512 + f * 3) != 0 || f == 0);   // the audio, not zeros
    CHECK(no_junk(bd) && no_junk(ms));
    CHECK(rb.fix_all("/Recordings") == 0);   // the boot scan after it: nothing more to fix
    // an old marker (the name alone, 40 bytes): the files' sizes count, as before
    FILE* of = fopen(disk("/Recordings/.recording").c_str(), "wb");
    char old[40] = {};
    snprintf(old, sizeof old, "%s", nm.c_str());
    fwrite(old, 1, sizeof old, of);
    fclose(of);
    CHECK(rb.recover("") == 0 && access(disk("/Recordings/.recording").c_str(), F_OK) != 0);
    printf("test_recorder: a power loss with space reserved: cut back to the synced lengths (%llu / %llu bytes): PASS\n",
           (unsigned long long)mm.data[0], (unsigned long long)mm.data[3]);

    // each scenario on a recorder of its own (a power cut leaves the last one recording, never polled again)
    static rec::Recorder rx[8];
    int nrx = 0;
    rec::Recorder* cur = nullptr;
    auto fresh = [&]() {
      CHECK(nrx < (int)(sizeof rx / sizeof rx[0]));
      cur = &rx[nrx++];
      cur->init(mem, memsz, &kBN);
      CHECK(cur->configure(m, 24, err, sizeof err) && cur->set_prefix(""));
      cur->poll();   // (the free space)
    };
    auto take2 = [&](unsigned blocks) {
      CHECK(cur->start(err, sizeof err));
      feed(*cur, tn, blocks, true);
      cur->stop();
      finish(*cur);
      cur->status(&s, false);
      return std::string(s.take);
    };
    auto power_cut = [&]() {   // nothing more written, nothing closed by the recorder
      for (int i = 0; i < rec::kSources; ++i)
        if (g_f[i]) fflush(g_f[i]), fclose(g_f[i]), g_f[i] = nullptr;
    };
    auto marker = [&]() {
      rec::Marker x;
      memset(&x, 0, sizeof x);
      auto b = slurp("/Recordings/.recording");
      if (b.size() == sizeof x) memcpy(&x, b.data(), sizeof x);
      return x;
    };
    auto synced_take = [&]() {   // a take with both files synced once mid-way, more after it
      CHECK(cur->start(err, sizeof err));
      feed(*cur, tn, 1, true);
      feed(*cur, tn, 40000, true);
      g_skew += 2100000, feed(*cur, tn, 10, true);
      g_skew += 2100000, feed(*cur, tn, 10, true);
      feed(*cur, tn, 3000, true);
      cur->status(&s, false);
      return std::string("/Recordings/") + s.take;
    };
    // path a: a power loss, then recover's cut fails - the marker is kept and the header holds the synced length; the
    // boot scan (fix_all) never grows it over the reserved tail; the next recover cuts the file
    {
      fresh();
      std::string a = synced_take();
      rec::Marker ma = marker();
      power_cut();
      g_cut_fail = true;
      rb.recover("");
      CHECK(access(disk("/Recordings/.recording").c_str(), F_OK) == 0);   // kept
      auto x = slurp(a + "/03_BD.wav");
      CHECK(x.size() == 16ull << 20 && recwav::get32(x.data() + 508) == ma.data[3]);
      CHECK(rb.fix_all("/Recordings") == 0);
      x = slurp(a + "/03_BD.wav");
      CHECK(recwav::get32(x.data() + 508) == ma.data[3] && no_junk(x));   // not grown over the tail
      g_cut_fail = false;
      rb.recover("");
      CHECK(access(disk("/Recordings/.recording").c_str(), F_OK) != 0);
      x = slurp(a + "/03_BD.wav");
      CHECK(x.size() == 512 + ma.data[3] + (ma.data[3] & 1) && no_junk(x));
    }
    // path b: the close's trim fails - the take ends with the error, the marker kept with the files' final lengths (not
    // their last sync); a new take overwrites the marker; the boot scan cuts the old files to their headers
    {
      fresh();
      g_trunc_fail = true;
      std::string b = synced_take();
      cur->stop();
      finish(*cur);
      cur->status(&s, false);
      uint64_t bd_len = s.frames * 3;
      CHECK(strstr(s.error, "trim") && access(disk("/Recordings/.recording").c_str(), F_OK) == 0);
      rec::Marker mb = marker();
      CHECK(mb.data[3] == bd_len && mb.data[0] == s.frames * 6);   // the final lengths
      auto x = slurp(b + "/03_BD.wav");
      CHECK(x.size() == 16ull << 20 && recwav::get32(x.data() + 508) == bd_len);
      g_trunc_fail = false;
      CHECK(take2(20) != "" && access(disk("/Recordings/.recording").c_str(), F_OK) != 0);   // the next take, its marker gone
      CHECK(rb.fix_all("/Recordings") == 0);
      x = slurp(b + "/03_BD.wav");
      CHECK(x.size() == 512 + bd_len + (bd_len & 1) && recwav::get32(x.data() + 508) == bd_len && no_junk(x));
    }
    // a power cut during the close (after the final lengths reached the marker): the files cut at their ends
    {
      fresh();
      g_trunc_fail = true;
      std::string c = synced_take();
      cur->stop();
      finish(*cur);
      cur->status(&s, false);
      uint64_t bd_len = s.frames * 3;
      g_trunc_fail = false;
      FILE* hf = fopen(disk((c + "/03_BD.wav").c_str()).c_str(), "rb+");   // as if the header never reached the card
      recwav::build(h, 1, 24, recwav::placeholder(3));
      fwrite(h, 1, 512, hf);
      fclose(hf);
      CHECK(rb.recover("") == 1);
      auto x = slurp(c + "/03_BD.wav");
      CHECK(x.size() == 512 + bd_len + (bd_len & 1) && recwav::get32(x.data() + 508) == bd_len && no_junk(x));
    }
    // on the SD the marker follows each sync, not each round: part-way through a round, the file just synced is in it
    {
      fresh();
      CHECK(cur->start(err, sizeof err));
      feed(*cur, tn, 1, true);
      feed(*cur, tn, 40000, true);
      g_skew += 2100000, feed(*cur, tn, 10, true);   // one sync (MASTER), the round not done
      rec::Marker mp = marker();
      CHECK(mp.data[0] > 0 && mp.data[3] == 0);
      g_skew += 2100000, feed(*cur, tn, 10, true);   // BD's
      CHECK(marker().data[3] > 0);
      cur->stop();
      finish(*cur);
    }
    // the marker's patch fails mid-take: logged once, retried each round; written again when it works
    {
      fresh();
      size_t logs = g_log.size();
      CHECK(cur->start(err, sizeof err));
      feed(*cur, tn, 1, true);
      feed(*cur, tn, 40000, true);
      g_patch_fail = true;
      for (int k = 0; k < 4; ++k) g_skew += 2100000, feed(*cur, tn, 10, true);   // two rounds
      CHECK(marker().data[3] == 0);
      int said = 0;
      for (size_t i = logs; i < g_log.size(); ++i) said += strstr(g_log[i].c_str(), "lengths not written") != nullptr;
      CHECK(said == 1);
      g_patch_fail = false;
      for (int k = 0; k < 2; ++k) g_skew += 2100000, feed(*cur, tn, 10, true);
      CHECK(marker().data[3] > 0);
      cur->stop();
      finish(*cur);
    }
    // one file reserved, one not: the marker says so (or the take does not start); a power loss cuts the reserved one to
    // its sync, the other to its size, as before
    {
      fresh();
      g_reserve_ok = 1, g_patch_fail = true;
      CHECK(cur->start(err, sizeof err));
      feed(*cur, tn, 50, true);
      finish(*cur);
      cur->status(&s, false);
      CHECK(s.state == rec::Recorder::kIdle && strstr(s.error, "cannot write"));
      g_patch_fail = false;
      g_reserve_ok = 1;
      std::string d = synced_take();
      rec::Marker md = marker();
      CHECK(md.data[0] > 0 && md.data[3] == rec::kNoLength);
      power_cut();
      CHECK(rb.recover("") == 2);
      auto x = slurp(d + "/03_BD.wav"), y = slurp(d + "/00_MASTER.wav");
      CHECK(recwav::get32(x.data() + 508) == (x.size() - 512) / 3 * 3 && x.size() < 16ull << 20 && no_junk(x));
      CHECK(y.size() == 512 + md.data[0] && no_junk(y));
      g_reserve_ok = 99;
      CHECK(rb.fix_all("/Recordings") == 0);
    }
    printf("test_recorder: a failed cut, a failed trim, a cut mid-close, a failed marker patch, a mixed take: never the tail: PASS\n");
  }
  free(mem);
  free(mem2);
  printf("test_recorder: PASS\n");
  return 0;
}
