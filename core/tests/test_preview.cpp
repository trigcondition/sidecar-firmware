// core/tests/test_preview.cpp - the sample browser's dry preview voice (core/samp/preview.h): a file streamed from the
// card through cardq into the static buffer and played once on the POST pair only (channels 0 / 1), mono on both
// sides, stereo kept, resampled to 48 kHz, at most 10 s; stop fades out in 5 ms; a new start fades the old one first;
// INFO reads the head only (no sound); a non-audio file fails quietly; mix() allocates nothing.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "host/core_port.h"
#include "fmt/fmt.h"
#include "msamp/cardq.h"
#include "samp/preview.h"
#include "wg_util.h"

using namespace core;

static int g_news = 0;
static bool g_count = false;
void* operator new(size_t n) {
  if (g_count) ++g_news;
  void* p = malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

static int32_t g_out[256][12];
// n frames mixed in blocks of 64 into L / R (the other channels checked silent)
static void run(unsigned n, std::vector<float>* l, std::vector<float>* r, bool load = true) {
  while (n) {
    unsigned k = n < 64 ? n : 64;
    memset(g_out, 0, sizeof g_out);
    g_count = true;
    preview::mix(g_out, k, 0, 1);
    g_count = false;
    for (unsigned f = 0; f < k; ++f) {
      for (int c = 2; c < 12; ++c) CHECK(g_out[f][c] == 0);
      if (l) l->push_back(g_out[f][0] / 2147483648.0f);
      if (r) r->push_back(g_out[f][1] / 2147483648.0f);
    }
    if (load) preview::step();
    n -= k;
  }
}
static uint64_t g_us = 1000000;
static void advance_ms(uint32_t ms) { host_fake_time(true, g_us += (uint64_t)ms * 1000); }
static void load_all() {   // (a mix of 0 frames between the steps: the audio side takes a new preview up)
  for (int i = 0; i < 10000; ++i) {
    preview::mix(g_out, 0, 0, 1);
    if (!preview::step()) break;
  }
}
static float peak(const std::vector<float>& x, size_t a = 0, size_t b = 0) {
  float p = 0;
  for (size_t i = a; i < (b ? b : x.size()); ++i) p = fabsf(x[i]) > p ? fabsf(x[i]) : p;
  return p;
}
static size_t last_sound(const std::vector<float>& x) {
  size_t k = 0;
  for (size_t i = 0; i < x.size(); ++i)
    if (x[i] != 0.0f) k = i + 1;
  return k;
}

int main() {
  char tmpl[] = "/tmp/core_preview_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  auto tone = [](double hz, double a) { return [hz, a](int i, int) { return a * std::sin(2 * M_PI * hz * i / 48000); }; };
  wg::write_file(card + "/Samples/mono.wav", wg::wav_bytes(48000, 1, 16, 48000, 1, "", tone(440, 0.5)));
  wg::write_file(card + "/Samples/dc.wav", wg::wav_bytes(48000, 1, 16, 48000, 1, "", [](int, int) { return 0.5; }));
  // 12 s stereo 24-bit 44.1 kHz: L a tone, R silent
  wg::write_file(card + "/Samples/long.wav", wg::wav_bytes(12 * 44100, 2, 24, 44100, 1, "",
                                                           [](int i, int c) { return c ? 0.0 : 0.5 * std::sin(2 * M_PI * 300 * i / 44100); }));
  wg::write_file(card + "/Samples/text.wav", std::vector<uint8_t>(5000, 'x'));
  // a WAV with 100 KB of JUNK before its fmt and data (the data past the head's 64 KB); 48 channels (refused)
  {
    std::vector<uint8_t> w = wg::wav_bytes(24000, 1, 16, 48000, 1, "", tone(440, 0.5));
    std::vector<uint8_t> junk = {'J', 'U', 'N', 'K', 0xA0, 0x86, 0x01, 0x00};   // 100000 bytes
    junk.resize(8 + 100000, 0);
    w.insert(w.begin() + 12, junk.begin(), junk.end());
    uint32_t riff = (uint32_t)w.size() - 8;
    for (int i = 0; i < 4; ++i) w[4 + i] = (uint8_t)(riff >> (8 * i));
    wg::write_file(card + "/Samples/junk.wav", w);
    // 2 s at 96 kHz (two frames a sample out): DC + 0.5, then DC - 0.5 (N1)
    wg::write_file(card + "/Samples/pos96.wav", wg::wav_bytes(192000, 1, 16, 96000, 1, "", [](int, int) { return 0.5; }));
    wg::write_file(card + "/Samples/neg96.wav", wg::wav_bytes(192000, 1, 16, 96000, 1, "", [](int, int) { return -0.5; }));
    wg::write_file(card + "/Samples/wide.wav", wg::wav_bytes(100, 48, 16, 48000, 1, "", [](int, int) { return 0.1; }));
  }
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  cardq::reset();
  preview::reset_for_tests();

  // nothing started: silence
  std::vector<float> l, r;
  run(256, &l, &r);
  CHECK(peak(l) == 0 && peak(r) == 0 && !preview::sounding());

  // a mono 1 s file at 48 kHz: both sides equal, -12 dB, ~1 s of sound, then idle
  CHECK(preview::start("/Samples/mono.wav") == 0 && preview::sounding());
  load_all();
  CHECK(preview::status().st == preview::kDone && preview::status().rate == 48000 && preview::status().ch == 1 &&
        preview::status().frames == 48000 && preview::status().loaded == 48000);
  l.clear(), r.clear();
  run(52000, &l, &r);
  CHECK(l == r);
  CHECK(fabsf(peak(l) - 0.5f * preview::level()) < 0.01f);
  size_t end = last_sound(l);
  CHECK(end > 47900 && end <= 48000);
  CHECK(!preview::sounding() && preview::stats().ends >= 1);
  printf("preview: mono 48 kHz on POST L = R, peak %.3f, %zu frames\n", peak(l), end);

  // stereo 24-bit 44.1 kHz, 12 s: R silent, L the tone, cut at 10 s of the file (resampled: 10 s at 48 kHz)
  CHECK(preview::start("/Samples/long.wav") == 0);
  load_all();
  CHECK(preview::status().ch == 2 && preview::status().bits == 24 && preview::status().rate == 44100 &&
        preview::status().loaded == 10 * 44100);
  l.clear(), r.clear();
  run(490000, &l, &r);
  end = last_sound(l);
  CHECK(peak(r) < 1e-4f && peak(l) > 0.03f);
  CHECK(end > 479000 && end <= 480100);
  printf("preview: stereo 44.1 kHz 12 s -> %zu frames at 48 kHz (10 s), R silent\n", end);

  // stop: faded out within 5 ms, then nothing
  CHECK(preview::start("/Samples/dc.wav") == 0);
  load_all();
  l.clear();
  run(1000, &l, nullptr);
  CHECK(l.back() > 0.03f);
  preview::stop();
  l.clear();
  run(2000, &l, nullptr);
  CHECK(last_sound(l) <= preview::kFade && !preview::sounding());
  for (size_t i = 1; i < preview::kFade && i < l.size(); ++i) CHECK(l[i] <= l[i - 1] + 1e-6f);   // a ramp down

  // a new start while one sounds: the old one fades, the new one plays from its start
  CHECK(preview::start("/Samples/mono.wav") == 0);
  load_all();
  run(4000, nullptr, nullptr);
  CHECK(preview::sounding());
  CHECK(preview::start("/Samples/dc.wav") == 0);
  l.clear();
  run(6000, &l, nullptr);   // (step() between the blocks loads the new file once the old one has faded)
  CHECK(preview::status().st == preview::kDone && strstr(preview::status().path, "dc.wav"));
  CHECK(fabsf(l.back() - 0.5f * preview::level()) < 0.01f);
  preview::stop();
  run(1000, nullptr, nullptr);

  // INFO: the head only, no sound
  CHECK(preview::info("/Samples/long.wav") == 0);
  load_all();
  CHECK(preview::status().st == preview::kDone && !preview::status().play && preview::status().rate == 44100 &&
        preview::status().frames == 12 * 44100 && preview::status().size > 3000000);
  l.clear();
  run(2000, &l, nullptr);
  CHECK(peak(l) == 0 && !preview::sounding());

  // not audio, not there, a virtual item: no sound
  CHECK(preview::start("/Samples/text.wav") == 0);
  load_all();
  CHECK(preview::status().st == preview::kFailed);
  CHECK(preview::start("/Samples/none.wav") == 0);
  load_all();
  CHECK(preview::status().st == preview::kFailed);
  CHECK(preview::start("@INPUTS/IN") < 0);
  l.clear();
  run(2000, &l, nullptr);
  CHECK(peak(l) == 0 && !preview::sounding());

  // the card behind: the sound holds where the buffer ends (an underrun counted), and goes on once more is there
  uint32_t u0 = preview::stats().underruns;
  CHECK(preview::start("/Samples/mono.wav") == 0);
  for (int i = 0; i < 10 && !preview::status().loaded; ++i) preview::mix(g_out, 0, 0, 1), preview::step();   // open, head, one piece
  CHECK(preview::status().loaded > 0 && preview::status().loaded < 48000);
  run(40000, nullptr, nullptr, false);
  CHECK(preview::stats().underruns > u0 && preview::sounding());
  load_all();
  l.clear();
  run(30000, &l, nullptr);
  CHECK(peak(l) > 0.03f);

  // above 48 kHz the hold never reads past the loaded frames (N1): the buffer past them still holds pos96's + 0.5
  CHECK(preview::start("/Samples/pos96.wav") == 0);
  load_all();
  run(100000, nullptr, nullptr);
  CHECK(!preview::sounding());
  CHECK(preview::start("/Samples/neg96.wav") == 0);
  for (int i = 0; i < 20 && (preview::status().st != preview::kLoading || !preview::status().loaded); ++i)
    preview::mix(g_out, 0, 0, 1), preview::step();
  CHECK(preview::status().loaded > 0 && preview::status().loaded < 192000);
  l.clear();
  run(40000, &l, nullptr, false);
  CHECK(peak(l) > 0.03f);
  for (float x : l) CHECK(x <= 0.0f);
  load_all();
  run(100000, nullptr, nullptr);

  // the head's chunks walked past 64 KB (L3); a 48-channel file refused (L4)
  CHECK(preview::start("/Samples/junk.wav") == 0);
  load_all();
  CHECK(preview::status().st == preview::kDone && preview::status().loaded == 24000 && preview::status().rate == 48000);
  l.clear(), r.clear();
  run(26000, &l, &r);
  CHECK(l == r && fabsf(peak(l) - 0.5f * preview::level()) < 0.01f && last_sound(l) > 23900);
  CHECK(preview::start("/Samples/wide.wav") == 0);
  load_all();
  CHECK(preview::status().st == preview::kFailed);
  // a device without the POST pair (kNoChannel): nothing written anywhere (L5)
  CHECK(preview::start("/Samples/dc.wav") == 0);
  load_all();
  {
    static int32_t big[64][12];
    memset(big, 0, sizeof big);
    preview::mix(big, 64, 0xFF, 0xFF);
    for (auto& f : big)
      for (int32_t x : f) CHECK(x == 0);
  }
  preview::stop();
  advance_ms(100);
  run(1000, nullptr, nullptr);

  // no audio side at all (the Overbridge stream down): sounding() ends kQuietMs after a stop, or past the file's length
  // unstopped, so the browser's toggle never sticks on PLAY (M1)
  CHECK(preview::start("/Samples/dc.wav") == 0);
  for (int i = 0; i < 100 && preview::status().st != preview::kDone; ++i) preview::step(), advance_ms(10);
  CHECK(preview::status().st == preview::kDone && preview::sounding());
  preview::stop();
  CHECK(preview::sounding());
  advance_ms(60);
  CHECK(!preview::sounding());
  CHECK(preview::start("/Samples/dc.wav") == 0);
  for (int i = 0; i < 100 && preview::status().st != preview::kDone; ++i) preview::step(), advance_ms(10);
  CHECK(preview::sounding());
  advance_ms(1000 + 1100);   // (dc.wav: 1 s)
  CHECK(!preview::sounding());
  // the fade's way out without audio: a new start goes on kQuietMs later
  CHECK(preview::start("/Samples/mono.wav") == 0);
  load_all();
  run(1000, nullptr, nullptr, false);
  CHECK(preview::sounding());
  CHECK(preview::start("/Samples/dc.wav") == 0);
  for (int i = 0; i < 10; ++i) preview::step();
  CHECK(preview::status().st == preview::kOpening);   // (waits for the fade)
  advance_ms(60);
  for (int i = 0; i < 200 && preview::status().st != preview::kDone; ++i) preview::step(), advance_ms(10);
  CHECK(preview::status().st == preview::kDone && strstr(preview::status().path, "dc.wav"));
  preview::stop();
  advance_ms(100);
  run(1000, nullptr, nullptr);

  // a card core (cardq worker): step() says false while its request waits (the idle chain goes on, H1); a start or a
  // stop while a read is in flight takes effect once that read is done
  cardq::set_worker(true);
  CHECK(preview::start("/Samples/mono.wav") == 0);
  for (int i = 0; i < 4 && preview::status().st == preview::kOpening && cardq::pending() == 0; ++i) preview::step();
  CHECK(cardq::pending() == 1);   // the open posted
  CHECK(!preview::step());        // waiting
  CHECK(!preview::step());
  CHECK(cardq::service());
  CHECK(preview::step() && preview::status().st == preview::kHeader);   // opened
  CHECK(preview::step());    // the head's read posted
  CHECK(!preview::step());
  CHECK(cardq::service());
  CHECK(preview::step() && preview::status().st == preview::kLoading);
  preview::mix(g_out, 0, 0, 1);
  CHECK(preview::step());    // a piece posted
  CHECK(!preview::step());
  CHECK(preview::start("/Samples/dc.wav") == 0);   // a new start while the read is in flight
  CHECK(!preview::step());   // (still the read)
  CHECK(cardq::service());
  for (int i = 0; i < 400 && preview::status().st != preview::kDone; ++i) {
    run(64, nullptr, nullptr, false);
    preview::step();
    while (cardq::service()) {
    }
    advance_ms(5);
  }
  CHECK(preview::status().st == preview::kDone && strstr(preview::status().path, "dc.wav") && preview::status().loaded == 48000);
  CHECK(preview::start("/Samples/long.wav") == 0);   // a stop while the head's read is in flight
  for (int i = 0; i < 400 && preview::status().st != preview::kHeader; ++i) {
    run(64, nullptr, nullptr, false);
    preview::step();
    if (preview::status().st != preview::kHeader)
      while (cardq::service()) {
      }
    advance_ms(5);
  }
  CHECK(preview::status().st == preview::kHeader);
  CHECK(preview::step() && !preview::step());   // the head's read posted, waiting
  preview::stop();
  CHECK(cardq::service());
  for (int i = 0; i < 20; ++i) {
    preview::step();
    while (cardq::service()) {
    }
  }
  CHECK(preview::status().st == preview::kIdle && preview::status().loaded == 0);
  advance_ms(100);
  CHECK(!preview::sounding());
  cardq::set_worker(false);

  // PREVIEW LEVEL (0.5.0): the default -9 dB (louder than the first build's -18), clamped to -40 .. -6 dB; a full-scale
  // file at the default keeps POST's sum (its voices and send FX return, each limited to 0.25) under full scale; a change mid-play ramps
  // over a block (no step); the setting saved to /.rytm/preview.txt (other lines kept) and read back at load()
  CHECK(preview::level_db() == preview::kLevelDefDb && preview::kLevelDefDb == -9 && fabsf(preview::level() - 0.3548f) < 0.001f &&
        0.25f + 0.25f + preview::level() < 1.0f);   // (arithmetic: POST's voices and send FX return at their limits only)
  preview::set_level_db(0);
  CHECK(preview::level_db() == -6 && fabsf(preview::level() - 0.5012f) < 0.001f);
  preview::set_level_db(-99);
  CHECK(preview::level_db() == -40);
  preview::set_level_db(-6);
  CHECK(preview::start("/Samples/dc.wav") == 0);
  load_all();
  l.clear();
  run(4800, &l, nullptr, false);
  CHECK(fabsf(l.back() - 0.5f * preview::level()) < 0.005f);
  preview::set_level_db(-20);
  run(64, &l, nullptr, false);
  run(64, &l, nullptr, false);
  for (size_t i = 4801; i < l.size(); ++i) CHECK(fabsf(l[i] - l[i - 1]) < 0.01f);   // ramped, not stepped
  CHECK(fabsf(l.back() - 0.5f * 0.1f) < 0.002f);
  preview::stop();
  run(1000, nullptr, nullptr, false);
  printf("preview: PREVIEW LEVEL default -9 dB, clamped -40 .. -6 dB, ramped\n");
  CHECK(system(("mkdir -p '" + card + "/.rytm' && printf 'later=1\\nlevel=-15\\n' > '" + card + "/.rytm/preview.txt'").c_str()) == 0);
  preview::load();
  CHECK(preview::level_db() == -15 && !preview::save());   // (read: nothing to write)
  preview::set_level_db(-14);
  CHECK(preview::save() && !preview::save());
  fmt::post_wait("/.rytm/preview.txt");
  preview::set_level_db(-30);
  preview::load();
  CHECK(preview::level_db() == -14);
  {
    FILE* f = fopen((card + "/.rytm/preview.txt").c_str(), "r");
    char b[128] = "";
    CHECK(f && fread(b, 1, sizeof b - 1, f) > 0);
    if (f) fclose(f);
    CHECK(strstr(b, "level=-14\n") && strstr(b, "later=1"));
  }
  printf("preview: PREVIEW LEVEL saved to /.rytm/preview.txt, read back, other lines kept\n");
  // review L3 / L5: a missing file, an empty or corrupt level=, or level= only inside a longer key keep the default
  // (-9 dB, not the loudest); a value out of range clamps; the key matches at a line's start only
  auto load_with = [&](const char* text) {
    std::string f = card + "/.rytm/preview.txt";
    if (text) {
      FILE* o = fopen(f.c_str(), "w");
      CHECK(o);
      fputs(text, o);
      fclose(o);
    } else {
      unlink(f.c_str());
    }
    preview::set_level_db(-30);
    preview::reset_for_tests();   // (the boot's default)
    preview::load();
    return preview::level_db();
  };
  CHECK(load_with(nullptr) == -9);
  CHECK(load_with("level=\n") == -9);
  CHECK(load_with("level=abc\n") == -9);
  CHECK(load_with("level=-12x\n") == -9);
  CHECK(load_with("maxlevel=-20\n") == -9);
  CHECK(load_with("maxlevel=-20\nlevel=-15\n") == -15);
  CHECK(load_with("level=-99\n") == -40);
  CHECK(load_with("level=3\n") == -6);
  CHECK(load_with("level=-13") == -13);   // (no newline at the end)
  CHECK(load_with("level=-11\r\n") == -11);
  printf("preview: preview.txt missing / corrupt keeps -9 dB, out of range clamps, exact key\n");

  CHECK(g_news == 0);   // mix() allocated nothing

  system(("rm -rf '" + card + "'").c_str());
  printf("test_preview: PASS\n");
  return 0;
}
