// core/tests/test_library.cpp - the sample library (core/lib/library.h, docs/LIBRARY.md; the formats spec 6 and 7.2):
// sha256 against the NIST vectors and Python's hashlib, CRC-32 against the bitwise definition; the reference text form;
// the boot load -> walk -> hash on a fake card; the index on the card (index.bin round trip, the log's replay, the
// compaction, a damaged index rebuilt, a newer index never written over); the walk's diff (add, delete, change in place
// with the same or another time, rename, a move between folders, a duplicate made) with the right states and log
// lines; touched() with a writer's hash reads nothing; moved() keeps the hashes; every row of the resolution table;
// the scheduler's order (hash_now, the current project's references first) and its pause rules; the console; 100 000
// records (RAM, find and resolve time) and a reader thread resolving while core 0 publishes (the lock-free side).
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "timing.h"
#include "fmt/fmt.h"
#include "lib/library.h"
#include "lib/srclib.h"
#include "lib/sha256.h"
#include "platform.h"
#include "samp/out.h"

using namespace core;

static std::string g_card;
static void put(const std::string& p, const std::string& s) {
  CHECK(system(("mkdir -p \"$(dirname '" + g_card + p + "')\"").c_str()) == 0);
  FILE* f = fopen((g_card + p).c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static std::string card(const std::string& p) {
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "";
  std::string s;
  char b[4096];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static bool exists(const std::string& p) {
  struct stat st;
  return stat((g_card + p).c_str(), &st) == 0;
}
static void sh(const std::string& c) { CHECK(system(c.c_str()) == 0); }
static void touch_time(const std::string& p, const char* stamp) {   // a computer's edit: another mtime
  sh("touch -t " + std::string(stamp) + " '" + g_card + p + "'");
}
static std::string hx(const uint8_t* d) {
  char b[65];
  lib::hex(d, 32, b);
  return b;
}
static std::string sha_of(const std::string& s) {
  uint8_t d[32];
  lib::sha256(s.data(), s.size(), d);
  return hx(d);
}
static std::string blob(int n, int seed) {   // n bytes, a WAV header first when n is big enough
  std::string s((size_t)n, '\0');
  uint32_t x = (uint32_t)seed * 2654435761u + 1;
  for (int i = 0; i < n; ++i) x = x * 1103515245u + 12345u, s[(size_t)i] = (char)(x >> 16);
  if (n >= 44) {
    memcpy(&s[0], "RIFF", 4), memcpy(&s[8], "WAVEfmt ", 8);
    uint32_t sz = 16;
    memcpy(&s[16], &sz, 4);
    uint16_t pcm = 1, ch = (uint16_t)(1 + seed % 2);
    uint32_t rate = 48000;
    memcpy(&s[20], &pcm, 2), memcpy(&s[22], &ch, 2), memcpy(&s[24], &rate, 4);
  }
  return s;
}

static lib::IoRule g_rule = {lib::kPiece, lib::kPiece, 0, ""};
static bool g_busy = false, g_job = false;
static std::vector<std::string> g_refs;
static void rules(lib::IoRule* r) { *r = g_rule; }
static bool card_busy() { return g_busy; }
static bool other_job() { return g_job; }
static int current_refs(const char** out, int max) {
  int n = 0;
  for (const std::string& s : g_refs)
    if (n < max) out[n++] = s.c_str();
  return n;
}
static uint64_t g_us = 1000000;
static int run(int max = 200000) {   // the card core's loop until the library is idle
  int n = 0;
  for (; n < max; ++n) {
    g_us += 2500;
    host_fake_time(true, g_us);
    if (!lib::step() && !lib::busy()) break;
  }
  CHECK(n < max);
  return n;
}
static lib::Resolved res(const char* sha_hex, uint64_t size, const char* hint) {
  lib::Ref r;
  char line[300];
  snprintf(line, sizeof line, "%s %llu %s", sha_hex, (unsigned long long)size, hint);
  CHECK(lib::parse_ref(line, &r));
  lib::Resolved o;
  lib::resolve(r, &o);
  return o;
}
static std::string log_txt() { return card("/.rytm/library/log.txt"); }
static void boot() {   // a reboot: the RAM index gone, the card as it is
  lib::reset_for_tests();
  lib::Env e{};
  e.rules = rules, e.card_busy = card_busy, e.other_job = other_job, e.current_refs = current_refs;
  lib::set_env(e);
  lib::begin(true);
}

int main() {
  // ---- sha256: FIPS 180-4's examples; CRC-32: the check value and the bitwise definition
  CHECK(sha_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  {
    lib::Sha256 s;
    s.init();
    std::string a(1000, 'a');
    for (int i = 0; i < 1000; ++i) s.update(a.data(), a.size());
    uint8_t d[32];
    s.final(d);
    CHECK(hx(d) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  }
  CHECK(lib::crc32(0, "123456789", 9) == 0xCBF43926u);
  for (int n : {0, 1, 7, 8, 9, 63, 1000, 65537}) {
    std::string b = blob(n, n);
    uint32_t c = 0xFFFFFFFFu;
    for (char ch : b) {
      c ^= (uint8_t)ch;
      for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    CHECK(lib::crc32(0, b.data(), b.size()) == ~c);
    CHECK(lib::crc32(lib::crc32(0, b.data(), b.size() / 3), b.data() + b.size() / 3, b.size() - b.size() / 3) == ~c);
  }
  char tmpl[] = "/tmp/lib-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  {   // against Python's hashlib on random files
    for (int n : {1, 55, 56, 64, 100000}) {
      std::string b = blob(n, 7 * n);
      put("/py.bin", b);
      std::string cmd = "python3 -c \"import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())\" '" +
                        g_card + "/py.bin' > '" + g_card + "/py.txt'";
      sh(cmd);
      CHECK(card("/py.txt") == sha_of(b) + "\n");
    }
    sh("rm -f '" + g_card + "/py.bin' '" + g_card + "/py.txt'");
  }

  // ---- the reference's text form (spec 6.6)
  {
    lib::Ref r;
    std::string h = sha_of("x");
    CHECK(lib::parse_ref((h + " 412044 /Samples/Drums/kick 1.wav\tkick").c_str(), &r));
    CHECK(r.has_sha && r.size == 412044 && !strcmp(r.hint, "/Samples/Drums/kick 1.wav") && hx(r.sha) == h);
    char out[300];
    CHECK(lib::format_ref(r, out, sizeof out) > 0 && std::string(out) == h + " 412044 /Samples/Drums/kick 1.wav");
    CHECK(lib::parse_ref("- 0 /Samples/new.wav\r\n", &r) && !r.has_sha && r.size == 0 && !strcmp(r.hint, "/Samples/new.wav"));
    CHECK(lib::format_ref(r, out, sizeof out) > 0 && !strcmp(out, "- 0 /Samples/new.wav"));
    CHECK(lib::format_ref(r, out, 8) == 0 && !out[0]);   // does not fit: nothing (never cut)
    CHECK(!lib::parse_ref("zz 1 /x.wav", &r) && !lib::parse_ref("- x /x.wav", &r) && !lib::parse_ref("- 1 x.wav", &r));
    CHECK(!lib::parse_ref((h.substr(0, 63) + " 1 /x.wav").c_str(), &r) && !lib::parse_ref("", &r));
    std::string longp = "- 1 /" + std::string(200, 'a');
    CHECK(!lib::parse_ref(longp.c_str(), &r));   // longer than a hint holds: refused, never cut
  }

  // ---- a card: samples, an instrument, a recording, hidden folders, a dot file
  host_set_card(g_card.c_str());
  host_fake_time(true, g_us);
  std::string kick = blob(200000, 1), snare = blob(90000, 2), pad = blob(300000, 3), sfz = "<region> sample=pad.wav\n";
  put("/Samples/Drums/kick.wav", kick);
  put("/Samples/Drums/snare.wav", snare);
  put("/GRANULAR/pad.aif", pad);
  put("/MULTISAMPLE/Pad/pad.sfz", sfz);
  put("/Recordings/take1/01_MAIN.wav", blob(120000, 4));
  put("/Plugins/x.wav", blob(1000, 5));            // hidden (srclib::kHidden)
  put("/Bundles/b.wav", blob(1000, 6));            // hidden
  put("/Samples/.hidden.wav", blob(1000, 7));      // a dot file
  put("/Samples/readme.txt", "not a sample");      // not a library type
  put("/.rytm/x.wav", blob(1000, 8));              // the Sidecar's own folder
  boot();
  {   // before the first step: PENDING (played by path, as before)
    lib::Resolved o = res(sha_of(kick).c_str(), kick.size(), "/Samples/Drums/kick.wav");
    CHECK(o.st == lib::kPending && !strcmp(o.path, "/Samples/Drums/kick.wav"));
  }
  run();
  lib::Stats st = lib::stats();
  if (st.state != lib::kReady || st.files != 5 || st.hashed != 4 || st.unhashed != 0 || st.recordings != 1)
    fprintf(stderr, "state %d files %u hashed %u unhashed %u recordings %u\n", st.state, st.files, st.hashed, st.unhashed, st.recordings);
  CHECK(st.state == lib::kReady && st.files == 5 && st.hashed == 4 && st.unhashed == 0 && st.recordings == 1);
  CHECK(st.bytes == kick.size() + snare.size() + pad.size() + sfz.size() + 120000 && st.dups == 0 && st.gone == 0);
  CHECK(st.pct == 100);   // (the recording is indexed only: not counted as pending, open question 6)
  CHECK(exists("/.rytm/library/index.bin") && exists("/.rytm/library/state.txt") && exists("/.rytm/library/log.txt"));
  {
    std::string s = card("/.rytm/library/state.txt");
    CHECK(s.rfind("format=library-state/1\n", 0) == 0 && s.find("files=5\n") != std::string::npos && s.find("tree=") != std::string::npos);
    CHECK(log_txt().rfind("#sidecar library-log 1\n", 0) == 0);
    // the registry reads all three
    std::string ix = card("/.rytm/library/index.bin");
    fmt::Info info;
    fmt::Bytes b = {(const uint8_t*)ix.data(), (uint32_t)ix.size()};
    CHECK(fmt::load(*fmt::find("library"), b, nullptr, &info) == fmt::kOk);
    b = {(const uint8_t*)s.data(), (uint32_t)s.size()};
    CHECK(fmt::load(*fmt::find("library-state"), b, nullptr, &info) == fmt::kOk);
    std::string lg = log_txt();
    b = {(const uint8_t*)lg.data(), (uint32_t)lg.size()};
    CHECK(fmt::load(*fmt::find("library-log"), b, nullptr, &info) == fmt::kOk);
  }
  // resolution: EXACT; a ref without a sha by its path; find; ref_for_path
  CHECK(res(sha_of(kick).c_str(), kick.size(), "/Samples/Drums/kick.wav").st == lib::kExact);
  CHECK(res(sha_of(kick).c_str(), kick.size(), "/samples/drums/KICK.WAV").st == lib::kExact);   // FAT: case ignored
  CHECK(res("-", 0, "/Samples/Drums/snare.wav").st == lib::kExact);
  CHECK(res("-", 0, "/Recordings/take1/01_MAIN.wav").st == lib::kUnverified);   // indexed, not hashed
  CHECK(res("-", 0, "/Samples/none.wav").st == lib::kMissing);
  {
    uint8_t d[32];
    lib::sha256(pad.data(), pad.size(), d);
    const char* paths[4];
    CHECK(lib::find(d, paths, 4) == 1 && !strcmp(paths[0], "/GRANULAR/pad.aif"));
    char p[200];
    CHECK(lib::find_file(d, pad.size(), p, sizeof p) && !strcmp(p, "/GRANULAR/pad.aif"));
    CHECK(!lib::find_file(d, pad.size() + 1, p, sizeof p));
    lib::Ref r;
    CHECK(lib::ref_for_path("/GRANULAR/pad.aif", &r) && r.has_sha && !memcmp(r.sha, d, 32) && r.size == pad.size());
    CHECK(!lib::ref_for_path("/GRANULAR/none.aif", &r) && !r.has_sha && !strcmp(r.hint, "/GRANULAR/none.aif"));
  }
  uint32_t g0 = lib::gen();
  const lib::Work w0 = lib::work();
  CHECK(w0.files_hashed == 4 && w0.read >= kick.size() + snare.size() + pad.size() + sfz.size());
  CHECK(w0.max_read <= lib::kPiece && w0.max_write <= lib::kPiece);

  // ---- a reboot with nothing changed: the index read back, the walk finds nothing, nothing hashed or written
  uint64_t ch = host_changes();
  boot();
  run();
  if (lib::work().files_hashed || host_changes() != ch)
    fprintf(stderr, "hashed %u changes %llu\n%s\n", lib::work().files_hashed, (unsigned long long)(host_changes() - ch), card("/.rytm/library/state.txt").c_str());
  CHECK(lib::work().files_hashed == 0 && host_changes() == ch);
  CHECK(lib::stats().files == 5 && lib::stats().hashed == 4);
  CHECK(res(sha_of(kick).c_str(), kick.size(), "/Samples/Drums/kick.wav").st == lib::kExact);
  (void)g0;

  // ---- changes behind the Pi's back (a computer): add, delete, change in place (same size, new bytes, another time),
  // rename, a move between folders, a duplicate
  std::string kick2 = blob(200000, 11);   // same size, other bytes
  put("/Samples/Drums/kick.wav", kick2);
  touch_time("/Samples/Drums/kick.wav", "202601011200");
  sh("mv '" + g_card + "/Samples/Drums/snare.wav' '" + g_card + "/Samples/Drums/snare2.wav'");   // a rename
  sh("mkdir -p '" + g_card + "/Samples/Pads' && mv '" + g_card + "/GRANULAR/pad.aif' '" + g_card + "/Samples/Pads/pad.aif'");
  put("/Samples/new.wav", blob(5000, 12));
  put("/Samples/Copies/kick-copy.wav", kick2);   // a duplicate of the changed kick
  sh("rm '" + g_card + "/MULTISAMPLE/Pad/pad.sfz'");
  boot();
  run();
  st = lib::stats();
  CHECK(st.files == 6 && st.hashed == 5 && st.gone == 3 && st.dups == 1 && st.dup_bytes == kick2.size());
  CHECK(lib::work().files_hashed == 5);   // the changed kick, the renamed / moved files (new paths), new, the copy
  {
    std::string lg = log_txt();   // (compacted after the boot walk: the log starts again)
    CHECK(lg == "#sidecar library-log 1\n");
  }
  // the resolution table (spec 6.5), every row
  CHECK(res(sha_of(kick2).c_str(), kick2.size(), "/Samples/Drums/kick.wav").st == lib::kExact);   // 1
  {
    lib::Resolved o = res(sha_of(snare).c_str(), snare.size(), "/Samples/Drums/snare.wav");        // 2: renamed
    CHECK(o.st == lib::kMoved && !strcmp(o.path, "/Samples/Drums/snare2.wav"));
    o = res(sha_of(pad).c_str(), pad.size(), "/GRANULAR/pad.aif");                                   // 2: moved
    CHECK(o.st == lib::kMoved && !strcmp(o.path, "/Samples/Pads/pad.aif"));
    o = res(sha_of(kick).c_str(), kick.size(), "/Samples/Drums/kick.wav");                           // 4: changed
    CHECK(o.st == lib::kChanged && !strcmp(o.path, "/Samples/Drums/kick.wav"));
    o = res(sha_of(sfz).c_str(), sfz.size(), "/MULTISAMPLE/Pad/pad.sfz");                            // 6: missing
    CHECK(o.st == lib::kMissing && !o.path[0]);
    o = res(sha_of(kick2).c_str(), kick2.size(), "/Samples/Copies/other.wav");                       // 2: the hint's folder first
    CHECK(o.st == lib::kMoved && !strcmp(o.path, "/Samples/Copies/kick-copy.wav"));
    o = res(sha_of(kick2).c_str(), kick2.size(), "/Samples/Drums/Sub/kick.wav");                     // 2: the nearest path
    CHECK(o.st == lib::kMoved && !strcmp(o.path, "/Samples/Drums/kick.wav"));
    o = res("-", 0, "/Samples/new.wav");                                                             // 5: by path
    CHECK(o.st == lib::kExact);
  }

  // ---- the Pi's own writes (touched): with the writer's hash nothing is read; without, it is hashed; a delete is gone
  {
    std::string take = blob(400000, 21);
    put("/Samples/LIVE/SMP_001.wav", take);   // (as the sampler writes it, its FAT time fixed: no clock)
    uint8_t d[32];
    lib::sha256(take.data(), take.size(), d);
    uint64_t rd = lib::work().read;
    lib::touched("/Samples/LIVE/SMP_001.wav", d, lib::crc32(0, take.data(), take.size()));
    uint32_t fh = lib::work().files_hashed;
    run();
    if (lib::work().read != rd) fprintf(stderr, "read %llu, hashed %u\n", (unsigned long long)(lib::work().read - rd), lib::work().files_hashed - fh);
    CHECK(lib::work().read == rd);   // no second read
    CHECK(res(hx(d).c_str(), take.size(), "/Samples/LIVE/SMP_001.wav").st == lib::kExact);
    CHECK(log_txt().find("+ " + hx(d) + " 400000 ") != std::string::npos);
    // the same size, other bytes, the same FAT time (the Pi has no clock): only touched() can tell
    std::string take2 = blob(400000, 22);
    put("/Samples/LIVE/SMP_001.wav", take2);
    lib::touched("/Samples/LIVE/SMP_001.wav", nullptr, 0);
    run();
    CHECK(lib::work().read >= rd + take2.size());
    CHECK(res(sha_of(take2).c_str(), take2.size(), "/Samples/LIVE/SMP_001.wav").st == lib::kExact);
    CHECK(res(hx(d).c_str(), take.size(), "/Samples/LIVE/SMP_001.wav").st == lib::kChanged);
    // moved: the hash goes with it (a file, then a folder), no read
    rd = lib::work().read;
    sh("mv '" + g_card + "/Samples/LIVE/SMP_001.wav' '" + g_card + "/Samples/LIVE/take.wav'");
    lib::moved("/Samples/LIVE/SMP_001.wav", "/Samples/LIVE/take.wav");
    sh("mv '" + g_card + "/Samples/LIVE' '" + g_card + "/Samples/LIVE2'");
    lib::moved("/Samples/LIVE", "/Samples/LIVE2");
    run();
    CHECK(lib::work().read == rd);
    CHECK(res(sha_of(take2).c_str(), take2.size(), "/Samples/LIVE2/take.wav").st == lib::kExact);
    CHECK(log_txt().find("> /Samples/LIVE/SMP_001.wav\t/Samples/LIVE/take.wav\n") != std::string::npos);
    // deleted
    sh("rm '" + g_card + "/Samples/LIVE2/take.wav'");
    lib::touched("/Samples/LIVE2/take.wav", nullptr, 0);
    run();
    CHECK(res(sha_of(take2).c_str(), take2.size(), "/Samples/LIVE2/take.wav").st == lib::kMissing);
    CHECK(log_txt().find("- /Samples/LIVE2/take.wav\n") != std::string::npos);
    // the sampler's and the auto-sampler's writer: the take hashed while it is written (no read), its .sfz too
    {
      static float x[48000 * 2];
      for (int k = 0; k < 96000; ++k) x[k] = 0.3f * (float)((k * 37) % 200 - 100) / 100.0f;
      rd = lib::work().read;
      samp::mkdirs("/Samples/TAKES");
      samp::WavWriter w;
      CHECK(w.begin("/Samples/TAKES/SMP_001.wav", x, 2, 0, 48000, 1.0f, 48, 480));
      int r;
      while ((r = w.step()) == 1) {}
      CHECK(r == 0);
      CHECK(samp::write_text("/Samples/TAKES/SMP_001.sfz", "<region> sample=SMP_001.wav\n", 28));
      run();
      CHECK(lib::work().read == rd);
      std::string wav = card("/Samples/TAKES/SMP_001.wav");
      CHECK(res(sha_of(wav).c_str(), wav.size(), "/Samples/TAKES/SMP_001.wav").st == lib::kExact);
      CHECK(res(sha_of("<region> sample=SMP_001.wav\n").c_str(), 28, "/Samples/TAKES/SMP_001.sfz").st == lib::kExact);
    }
    lib::touched("/Samples/LIVE2/none.txt", nullptr, 0);   // not a library file: nothing
    lib::touched("USB:/x.wav", nullptr, 0);                 // a stick: not indexed
  }

  // ---- a writer's report while the index is still loading: kept and applied after the load (never replaced by the
  // index's older record of that path)
  {
    std::string k3 = blob(200000, 41);   // the kick's size again: only the report can tell
    put("/Samples/Drums/kick.wav", k3);
    touch_time("/Samples/Drums/kick.wav", "202601011200");   // (its old time: the walk sees nothing)
    boot();
    uint8_t d[32];
    lib::sha256(k3.data(), k3.size(), d);
    lib::touched("/Samples/Drums/kick.wav", d, 0);
    run();
    CHECK(res(hx(d).c_str(), k3.size(), "/Samples/Drums/kick.wav").st == lib::kExact);
    put("/Samples/Drums/kick.wav", kick2);   // (back as it was for the checks below)
    touch_time("/Samples/Drums/kick.wav", "202601011200");
    lib::sha256(kick2.data(), kick2.size(), d);
    lib::touched("/Samples/Drums/kick.wav", d, 0);
    run();
  }

  // ---- (review) a writer's file and a rename during the walk are not taken as gone at its end
  {
    boot();
    for (int i = 0; i < 1000 && lib::stats().state != lib::kWalking; ++i) lib::step();
    CHECK(lib::stats().state == lib::kWalking);
    std::string nw = blob(7000, 51);
    put("/Samples/Copies/during.wav", nw);
    uint8_t d[32];
    lib::sha256(nw.data(), nw.size(), d);
    lib::touched("/Samples/Copies/during.wav", d, 0);
    sh("mv '" + g_card + "/Samples/new.wav' '" + g_card + "/Samples/new2.wav'");
    lib::moved("/Samples/new.wav", "/Samples/new2.wav");
    run();
    CHECK(res(hx(d).c_str(), nw.size(), "/Samples/Copies/during.wav").st == lib::kExact);
    CHECK(res("-", 0, "/Samples/new2.wav").st == lib::kExact && res("-", 0, "/Samples/new.wav").st == lib::kMissing);
  }
  // ---- (review) a log whose last line was cut by a power cut: the next line still whole after a reboot
  {
    std::string lg = log_txt();
    put("/.rytm/library/log.txt", lg + "+ 0123456789abcdef");   // (no newline: cut mid-line)
    boot();
    run();
    std::string t2 = blob(9000, 61);
    put("/Samples/Copies/after-cut.wav", t2);
    uint8_t d[32];
    lib::sha256(t2.data(), t2.size(), d);
    lib::touched("/Samples/Copies/after-cut.wav", d, 0);
    run();
    // (its line in the log or, compacted, in index.bin: either way known after the reboot below, nothing hashed again)
    uint32_t hashed0 = lib::work().files_hashed;
    boot();
    run();
    CHECK(res(hx(d).c_str(), t2.size(), "/Samples/Copies/after-cut.wav").st == lib::kExact && lib::work().files_hashed == 0);
    (void)hashed0;
  }
  // ---- (review) a newer build's log is never written over (the index still v1)
  {
    std::string lg = log_txt();
    std::string newer = "#sidecar library-log 2\n? a line this build does not know\n";
    put("/.rytm/library/log.txt", newer);
    put("/Samples/Copies/x2.wav", blob(3000, 71));
    boot();
    run();
    CHECK(card("/.rytm/library/log.txt") == newer);
    char out[400];
    CHECK(lib::command("lib=status", out, sizeof out) && strstr(out, "newer"));
    put("/.rytm/library/log.txt", lg);
    boot();
    run();
  }

  // ---- the log replayed at boot (a reboot before any compaction), then a damaged index rebuilt
  {
    uint32_t files = lib::stats().files;
    boot();
    run();
    CHECK(lib::stats().files == files && lib::work().files_hashed == 0);
    std::string ix = card("/.rytm/library/index.bin");
    ix[ix.size() / 2] ^= 0x40;   // a damaged body (its CRC)
    put("/.rytm/library/index.bin", ix);
    boot();
    run();
    CHECK(lib::stats().files == files && lib::work().files_hashed == lib::stats().hashed);   // every file again
    fmt::Info info;
    std::string nx = card("/.rytm/library/index.bin");
    fmt::Bytes b = {(const uint8_t*)nx.data(), (uint32_t)nx.size()};
    CHECK(fmt::load(*fmt::find("library"), b, nullptr, &info) == fmt::kOk);
  }
  {   // a newer build's index: used read-only, never written over
    std::string ix = card("/.rytm/library/index.bin");
    std::string newer = ix;
    newer[4] = 2, newer[6] = 2;   // version 2, min_reader 2
    put("/.rytm/library/index.bin", newer);
    put("/Samples/newer-build.wav", blob(3000, 31));
    boot();
    run();
    CHECK(card("/.rytm/library/index.bin") == newer);
    char out[300];
    CHECK(lib::command("lib=status", out, sizeof out) && strstr(out, "newer"));
    put("/.rytm/library/index.bin", ix);
    boot();
    run();
  }

  // ---- the scheduler: hash_now and the current project's references first; the pause rules
  {
    for (int i = 0; i < 12; ++i) put("/Samples/Q/q" + std::to_string(i) + ".wav", blob(70000, 100 + i));
    g_refs = {"/Samples/Q/q9.wav"};
    lib::hash_now("/Samples/Q/q11.wav");
    lib::rescan();
    std::vector<std::string> order;
    for (int i = 0; i < 100000 && lib::busy(); ++i) {
      g_us += 2500;
      host_fake_time(true, g_us);
      uint32_t n0 = lib::work().files_hashed;
      lib::step();
      if (lib::work().files_hashed != n0) {
        for (int k = 0; k < 12; ++k) {   // (resolve, not ref_for_path: a pick would bump it)
          std::string p = "/Samples/Q/q" + std::to_string(k) + ".wav";
          bool seen = false;
          for (auto& o : order) seen |= o == p;
          if (!seen && res("-", 0, p.c_str()).st == lib::kExact) order.push_back(p);
        }
      }
    }
    if (order.size() != 12 || order[0] != "/Samples/Q/q11.wav" || order[1] != "/Samples/Q/q9.wav")
      for (auto& o : order) fprintf(stderr, "order: %s\n", o.c_str());
    CHECK(order.size() == 12 && order[0] == "/Samples/Q/q11.wav" && order[1] == "/Samples/Q/q9.wav");
    g_refs.clear();
    // pause rules: the streams first; a bundle job first; the recorder: nothing at all; playing: 16 KB reads, no write
    put("/Samples/P/big.wav", blob(600000, 200));
    lib::rescan();
    g_busy = true;
    lib::Work a = lib::work();
    for (int i = 0; i < 100; ++i) lib::step();
    CHECK(lib::work().steps == a.steps && lib::work().waits > a.waits && !strcmp(lib::stats().waiting, "STREAMS"));
    g_busy = false, g_job = true;
    for (int i = 0; i < 100; ++i) lib::step();
    CHECK(lib::work().steps == a.steps && !strcmp(lib::stats().waiting, "BUNDLE"));
    g_job = false;
    g_rule = {0, 0, 0, "RECORDING"};
    uint64_t c0 = host_changes();
    for (int i = 0; i < 100; ++i) lib::step();
    CHECK(lib::work().steps == a.steps && lib::work().read == a.read && !strcmp(lib::stats().waiting, "RECORDING"));
    g_rule = {lib::kSlowPiece, 0, lib::kSlowGapUs, "PLAYING"};
    for (int i = 0; i < 3000 && lib::busy(); ++i) {
      g_us += 1000;
      host_fake_time(true, g_us);
      uint64_t r0 = lib::work().read;
      lib::step();
      CHECK(lib::work().read - r0 <= lib::kSlowPiece);   // 16 KB pieces while the Rytm plays
    }
    CHECK(host_changes() == c0);   // no card write while the Rytm plays
    CHECK(lib::stats().pending_log > 0);   // the log lines wait in RAM
    g_rule = {lib::kPiece, lib::kPiece, 0, ""};
    run();
    CHECK(lib::stats().pending_log == 0 && host_changes() > c0);
    lib::Ref r;
    CHECK(lib::ref_for_path("/Samples/P/big.wav", &r) && r.has_sha);
  }

  // ---- the console
  {
    char out[400];
    CHECK(lib::command("lib=status", out, sizeof out) && strstr(out, "files") && strstr(out, "hashed"));
    CHECK(!lib::command("lib=gr:cd:/Samples", out, sizeof out) && !lib::command("lib", out, sizeof out));   // srclib's
    std::string fc = "lib=find:" + sha_of(kick2);
    CHECK(lib::command(fc.c_str(), out, sizeof out));
    CHECK(strstr(out, "/Samples/Drums/kick.wav") && strstr(out, "/Samples/Copies/kick-copy.wav"));
    CHECK(lib::command("lib=bench", out, sizeof out));
    run();
    lib::Bench bn = lib::bench();
    CHECK(!bn.running && bn.sha_mbs100 > 0 && bn.crc_mbs100 > 0 && bn.read_mbs100 > 0);
    printf("test_library: bench (Mac, card model off): sha256 %u.%02u MB/s, crc32 %u.%02u MB/s, read %u.%02u MB/s\n",
           bn.sha_mbs100 / 100, bn.sha_mbs100 % 100, bn.crc_mbs100 / 100, bn.crc_mbs100 % 100, bn.read_mbs100 / 100,
           bn.read_mbs100 % 100);
    CHECK(lib::command("lib=verify", out, sizeof out));
    uint32_t h0 = lib::work().files_hashed;
    run();
    CHECK(lib::work().files_hashed - h0 == lib::stats().hashed);
    CHECK(lib::command("lib=status", out, sizeof out) && strstr(out, "verify: 0 differ"));
    char js[600];
    CHECK(lib::status_json(js, sizeof js) > 0 && strstr(js, ",\"library\":{") && strstr(js, "\"files\":"));
  }

  // ---- the boot load is never half-published (Task 6b fix round 1): a '>' move line in the log (its rename makes
  // the index dirty) between 1 500 earlier and 1 000 later lines: while the load runs, lib::list / is_folder say "no
  // snapshot" (-1), the browser shows the later lines' folder as being read (never a short "done" listing) and a cd
  // into it is not refused; once loaded, the whole folder
  {
    lib::reset_for_tests();
    std::string lg = "#sidecar library-log 1\n";
    char l[300];
    for (int i = 0; i < 1500; ++i) {
      snprintf(l, sizeof l, "+ - %d 00000000 00000000 00000000 0000 /Early/E%05d.wav\n", 100 + i, i);
      lg += l;
    }
    lg += "> /Early/E00000.wav\t/Moved/E00000.wav\n";
    for (int i = 0; i < 1000; ++i) {   // (under srclib::kListing)
      snprintf(l, sizeof l, "+ - %d 00000000 00000000 00000000 0000 /Late/L%05d.wav\n", 100 + i, i);
      lg += l;
    }
    sh("rm -rf '" + g_card + "/.rytm/library' '" + g_card + "/Samples' '" + g_card + "/Big'");
    put("/.rytm/library/log.txt", lg);
    lib::Env e{};
    e.rules = rules;
    lib::set_env(e);
    lib::begin(true);
    srclib::begin(true);
    srclib::reset_for_tests();
    auto none = [](void*, const char*, bool, uint64_t) { return true; };
    int steps = 0, early_pub = 0, rejected = 0, short_done = 0;
    for (; steps < 100000 && (lib::stats().state == lib::kLoading || lib::stats().state == lib::kOff); ++steps) {
      g_us += 2500;
      host_fake_time(true, g_us);
      lib::step();
      if (lib::stats().state != lib::kLoading) break;
      if (lib::list("/Late", none, nullptr) >= 0 || lib::is_folder("/Late") != -1) ++early_pub;
      if (!srclib::cd(srclib::kGr, "/Late")) ++rejected;
      srclib::step();
      if (srclib::listing_done(srclib::kGr)) ++short_done;
    }
    printf("test_library: the boot load in %d steps: %d half snapshots seen, %d cd refused, %d short listings\n", steps,
           early_pub, rejected, short_done);
    CHECK(steps > 2 && early_pub == 0 && rejected == 0 && short_done == 0);
    for (int i = 0; i < 10 && !srclib::listing_done(srclib::kGr); ++i) srclib::step();
    CHECK(srclib::listing_done(srclib::kGr) && srclib::count(srclib::kGr) == 1000);
    CHECK(lib::is_folder("/Moved") == 1 && lib::is_folder("/Late") == 1);
  }

  // ---- a filtering browser on a flat folder (Task 6b fix round 1): MULTISAMPLE (.sfz) browsing 20 000 .wav and 4 .sfz:
  // the listing in steps of at most srclib::kListPer entries looked at (a step within ~1 ms on the Pi), all 4 found
  {
    lib::reset_for_tests();
    std::string lg = "#sidecar library-log 1\n";
    char l[300];
    for (int i = 0; i < 20000; ++i) {
      snprintf(l, sizeof l, "+ - %d 00000000 00000000 00000000 0000 /Flat/W%05d.wav\n", 100 + i, i);
      lg += l;
    }
    for (const char* n : {"a.sfz", "M.sfz", "zz1.sfz", "zz2.sfz"}) {
      snprintf(l, sizeof l, "+ - 7 00000000 00000000 00000000 0000 /Flat/%s\n", n);
      lg += l;
    }
    sh("rm -rf '" + g_card + "/.rytm/library' '" + g_card + "/Flat' '" + g_card + "/Late' '" + g_card + "/Early'");
    put("/.rytm/library/log.txt", lg);
    lib::Env e{};
    e.rules = rules;
    lib::set_env(e);
    lib::begin(true);
    for (int i = 0; i < 400000 && (lib::stats().state == lib::kLoading || lib::stats().state == lib::kOff); ++i) lib::step();
    srclib::reset_for_tests();
    uint32_t s0 = srclib::stats().steps;
    CHECK(srclib::cd(srclib::kMs, "/Flat"));
    double worst = 0;
    int calls = 0;
    for (; calls < 1000 && !srclib::listing_done(srclib::kMs); ++calls) {
      auto t0 = std::chrono::steady_clock::now();
      srclib::step();
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      worst = us > worst ? us : worst;
    }
    printf("test_library: MULTISAMPLE on a flat folder of 20 004: %u steps, the longest %.0f us (Mac)\n",
           srclib::stats().steps - s0, worst);
    CHECK(srclib::listing_done(srclib::kMs) && srclib::count(srclib::kMs) == 4);
    CHECK(srclib::stats().steps - s0 >= 20004 / srclib::kListPer);
    const char* n;
    CHECK(srclib::item(srclib::kMs, 0, &n, nullptr, nullptr) && !strcmp(n, "a.sfz"));
    CHECK(srclib::item(srclib::kMs, 3, &n, nullptr, nullptr) && !strcmp(n, "zz2.sfz"));
    srclib::cd(srclib::kMs, "");
  }

  // ---- the browser's folder view (lib::list, Task 6b): 20 000 records (20 folders of 1 000, a nested folder), one
  // folder of the snapshot listed - its subfolders once each, its files - from RAM, under 1 ms on the Pi (the Mac's
  // time x5 under 200 us); no snapshot yet: -1
  {
    lib::reset_for_tests();
    lib::begin(true);
    struct Got {
      std::vector<std::string> names;
      std::vector<bool> dirs;
      std::vector<uint64_t> sizes;
      int stop_at = -1;
    };
    auto fn = [](void* c, const char* name, bool dir, uint64_t size) {
      Got* g = (Got*)c;
      g->names.push_back(name), g->dirs.push_back(dir), g->sizes.push_back(size);
      return g->stop_at < 0 || (int)g->names.size() < g->stop_at;
    };
    Got none;
    CHECK(lib::list("", fn, &none) == -1 && lib::is_folder("/Big") == -1);   // (the boot load not done)
    lib::reset_for_tests();
    std::string lg = "#sidecar library-log 1\n";
    for (int i = 0; i < 20000; ++i) {
      char sha[65], l[300];
      uint8_t d[32];
      uint32_t v = (uint32_t)i + 500000;
      lib::sha256(&v, 4, d);
      lib::hex(d, 32, sha);
      snprintf(l, sizeof l, "+ %s %d 00000000 00000000 00000000 0000 /Big/F%02d/S%04d.wav\n", i % 3 ? sha : "-", 1000 + i,
               (i * 7) % 20, i / 20);
      lg += l;
    }
    lg += "+ - 5 00000000 00000000 00000000 0000 /Big/F07/sub/deep/x.wav\n";
    lg += "+ - 6 00000000 00000000 00000000 0000 /Big/F07 b.wav\n";   // (' ' before '/': between F07 and F07/...)
    lg += "+ - 7 00000000 00000000 00000000 0000 /Big/f07.aif\n";
    sh("rm -rf '" + g_card + "/.rytm/library' '" + g_card + "/Samples' '" + g_card + "/Big'");
    put("/.rytm/library/log.txt", lg);
    lib::Env e{};
    e.rules = rules;
    lib::set_env(e);
    lib::begin(true);
    for (int i = 0; i < 400000 && (lib::stats().state == lib::kLoading || lib::stats().state == lib::kOff); ++i) lib::step();
    CHECK(lib::stats().files == 20003);
    uint32_t pub_us = lib::work().max_us;   // the load's longest step: its publish
    Got root, big, f07, sub;
    CHECK(lib::list("", fn, &root) == 1 && root.names[0] == "Big" && root.dirs[0]);
    CHECK(lib::list("/Big", fn, &big) == 22);
    // (path order, case ignored - the caller sorts: ' ' and '.' come before '/', so two files sit before F07)
    CHECK(big.names[0] == "F00" && big.names[6] == "F06" && big.names[9] == "F07" && big.dirs[9] && big.names[21] == "F19");
    CHECK(big.names[7] == "F07 b.wav" && !big.dirs[7] && big.sizes[7] == 6 && big.names[8] == "f07.aif" && !big.dirs[8]);
    lib::ListPos lp{};
    CHECK(lib::list("/BIG/f07", fn, &f07, &lp) == 1001 && lp.gen == lib::gen() && lp.done);   // (case ignored: FAT)
    CHECK(f07.names[0] == "S0000.wav" && f07.sizes[0] == 1001 && f07.names[1000] == "sub" && f07.dirs[1000]);
    CHECK(lib::list("/Big/F07/sub", fn, &sub) == 1 && sub.names[0] == "deep" && sub.dirs[0]);
    Got cut;
    cut.stop_at = 10;
    CHECK(lib::list("/Big/F03", fn, &cut) == 10);   // (the caller stops it: a listing's cap)
    CHECK(lib::is_folder("") == 1 && lib::is_folder("/Big") == 1 && lib::is_folder("/big/f07/SUB/deep") == 1);
    CHECK(lib::is_folder("/Big/F07/S0001.wav") == 0 && lib::is_folder("/Nope") == 0 && lib::is_folder("/Big/F0") == 0);
    // the time of a listing (any core, RAM only): the biggest folder, the root of the folders, a deep one
    int reps = 200, nn = 0;
    CHECK(timing_retry("a listing's time", [&] {   // (wall-clock, loose: up to 3 tries - tests/timing.h)
      auto t0 = std::chrono::steady_clock::now();
      for (int k = 0; k < reps; ++k) {
        Got a, b2;
        nn += lib::list("/Big/F11", fn, &a);
        nn += lib::list("/Big", fn, &b2);
      }
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
      auto t1 = std::chrono::steady_clock::now();
      for (int k = 0; k < reps * 10; ++k) nn += lib::is_folder("/Big/F11");
      double fus = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / (reps * 10);
      printf("test_library: 20000 records: a folder of 1000 files + the folder of 20 folders listed in %.1f us, a folder "
             "check %.2f us, the publish %u us (Mac)\n", us, fus, pub_us);
      return us < 2000 && fus < 200;
    }));
    CHECK(nn > 0);
  }

  // ---- 100 000 records: RAM, publish, find and resolve time; a reader thread on the published snapshots
  {
    lib::reset_for_tests();
    std::vector<std::string> lines;
    lines.reserve(100000);
    std::string lg = "#sidecar library-log 1\n";
    for (int i = 0; i < 100000; ++i) {
      char sha[65], l[300];
      uint8_t d[32];
      uint32_t v = (uint32_t)i;
      lib::sha256(&v, 4, d);
      lib::hex(d, 32, sha);
      snprintf(l, sizeof l, "+ %s %d 00000000 00000000 00000000 0000 /Samples/Big/F%03d/file %06d.wav\n", sha, 1000 + i % 977, i / 1000, i);
      lg += l;
    }
    sh("rm -rf '" + g_card + "/.rytm/library' '" + g_card + "/Samples'");
    put("/.rytm/library/log.txt", lg);
    lib::Env e{};
    e.rules = rules;
    lib::set_env(e);
    lib::begin(true);
    std::atomic<bool> stop(false);
    std::atomic<uint64_t> resolved(0), bad(0);
    std::thread reader([&]() {   // the control core: resolves the whole time, never waits
      lib::Ref r;
      uint32_t v = 1234;
      uint8_t d[32];
      lib::sha256(&v, 4, d);
      memcpy(r.sha, d, 32), r.has_sha = true, r.size = 1000 + 1234 % 977;
      snprintf(r.hint, sizeof r.hint, "/Samples/Big/F001/file 001234.wav");
      while (!stop.load()) {
        lib::Resolved o;
        lib::State s = lib::resolve(r, &o);
        if (s != lib::kPending && s != lib::kExact && s != lib::kMissing) ++bad;
        if (s == lib::kExact && strcmp(o.path, r.hint)) ++bad;
        ++resolved;
      }
    });
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 400000 && (lib::stats().state == lib::kLoading || lib::stats().state == lib::kOff); ++i) lib::step();
    double load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(lib::stats().files >= 100000 || lib::stats().state == lib::kWalking);
    stop = true;
    reader.join();
    CHECK(bad == 0 && resolved > 0);
    // timings on the loaded snapshot
    uint32_t v = 77777;
    uint8_t d[32];
    lib::sha256(&v, 4, d);
    const char* paths[2];
    lib::Ref r;
    memcpy(r.sha, d, 32), r.has_sha = true, r.size = 1000 + 77777 % 977;
    snprintf(r.hint, sizeof r.hint, "/Samples/Big/F077/other.wav");
    lib::Resolved o;
    CHECK(timing_retry("find / resolve", [&] {   // (wall-clock: up to 3 tries - tests/timing.h)
      auto t1 = std::chrono::steady_clock::now();
      int found = 0;
      for (int k = 0; k < 10000; ++k) found += lib::find(d, paths, 2);
      double find_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / 10000;
      auto t2 = std::chrono::steady_clock::now();
      for (int k = 0; k < 10000; ++k) lib::resolve(r, &o);
      double res_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t2).count() / 10000;
      CHECK(found == 10000 && o.st == lib::kMoved && !strcmp(o.path, "/Samples/Big/F077/file 077777.wav"));
      printf("test_library: 100000 records: loaded (log replay + publish) in %.0f ms, %u publishes, find %.2f us, "
             "resolve %.2f us, %llu resolves on a reader thread meanwhile (all consistent)\n", load_ms,
             lib::work().publishes, find_us, res_us, (unsigned long long)resolved.load());
      return find_us < 50 && res_us < 50;
    }));
  }
  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);   // (the temp card)
  printf("test_library: ok\n");
  return 0;
}
