// core/tests/test_msamp.cpp - MULTISAMPLE (core/msamp): the SFZ subset's parser (zones, defaults, inheritance, note
// names, round robin, loops, paths, #define, comments, malformed input), then the machine through the engine's own
// entry (host_sysex, a voice forced to MULTISAMPLE): the instrument list, a trig waiting for its load, zone and
// velocity-layer choice, pitch, chords (P13..P16 as poly FM's), round robin, release regions, the memory cap and
// RAM refused; streaming: a streamed instrument opens reading only its heads and plays bit-exact what the RAM one
// plays; a simulated card (throughput, jittered latency, recorder-like stalls) streams 16 stereo 24-bit notes with
// no underrun and still bit-exact, and a card too slow for them shows underruns or bandwidth steals.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include "check.h"
#include "pool_util.h"
#include "pool/pcache.h"
#include "lib/library.h"
#include "pool/pool.h"
#include "host_link.h"
#include "msamp/cardq.h"
#include "msamp/msamp.h"
#include "fx/out.h"
#include "pool/stream_budget.h"
#include "poly.h"
#include "voices.h"

using namespace core;
using namespace core::msamp;


// ---- the parser
struct D {
  std::vector<Region> regions = std::vector<Region>(256);
  std::vector<char> samples = std::vector<char>(64 * msamp::kPath);
  Desc d{};
  D() {
    d.regions = regions.data(), d.max_regions = 256;
    d.samples = (char(*)[msamp::kPath])samples.data(), d.max_samples = 64;
  }
  bool parse(const char* t, const char* dir = "/MULTISAMPLE/X") { return msamp::parse(t, (int)strlen(t), dir, &d); }
  const char* path(int i) { return d.samples[d.regions[i].sample]; }
};

static void test_notes_paths() {
  CHECK(note_value("c4") == 60 && note_value("C4") == 60 && note_value("c#4") == 61 && note_value("db4") == 61);
  CHECK(note_value("a4") == 69 && note_value("c-1") == 0 && note_value("g9") == 127 && note_value("b3") == 59);
  CHECK(note_value("60") == 60 && note_value("x4") == -1 && note_value("c") == -1 && note_value("128") == -1);
  char o[msamp::kPath];
  CHECK(join_path("/MULTISAMPLE/P", "s/a.wav", o, sizeof o) && !strcmp(o, "/MULTISAMPLE/P/s/a.wav"));
  CHECK(join_path("/MULTISAMPLE/P", "..\\Q\\.\\b.wav", o, sizeof o) && !strcmp(o, "/MULTISAMPLE/Q/b.wav"));
  CHECK(join_path("/MULTISAMPLE/P", "/Samples//c.wav", o, sizeof o) && !strcmp(o, "/Samples/c.wav"));
  CHECK(!join_path("/A", "../../x.wav", o, sizeof o));   // above the root
  char small[8];
  CHECK(!join_path("/A", "abcdefghij.wav", small, sizeof small));
}

static void test_parse() {
  {  // defaults and key=
    D p;
    CHECK(p.parse("<region> sample=a.wav key=c4"));
    const Region& r = p.d.regions[0];
    CHECK(p.d.nregions == 1 && r.lokey == 60 && r.hikey == 60 && r.keycenter == 60 && r.lovel == 1 && r.hivel == 127);
    CHECK(r.loop_mode == kLoopAuto && r.end == kUnset && r.loop_start == kUnset && r.seq_length == 1 && r.amplitude == 100);
    CHECK(r.sustain == 100 && r.pitch_keytrack == 100 && r.trigger == kTrigAttack && !strcmp(p.path(0), "/MULTISAMPLE/X/a.wav"));
  }
  {  // inheritance: control > global > master > group > region; default_path; a region overrides
    D p;
    CHECK(p.parse("<control> default_path=smp/ <global> volume=-6 ampeg_release=1.5 <master> tune=10 "
                  "<group> lovel=64 hivel=127 lokey=c3 hikey=b3 <region> sample=lo.wav pitch_keycenter=c3 "
                  "<region> sample=hi.wav volume=0 <group> <region> sample=x.wav"));
    CHECK(p.d.nregions == 3 && p.d.nsamples == 3);
    const Region &a = p.d.regions[0], &b = p.d.regions[1], &c = p.d.regions[2];
    CHECK(a.volume == -6 && a.release == 1.5f && a.tune == 10 && a.lovel == 64 && a.lokey == 48 && a.hikey == 59);
    CHECK(a.keycenter == 48 && b.keycenter == 60 && b.volume == 0 && b.tune == 10);
    CHECK(c.lovel == 1 && c.lokey == 0 && c.volume == -6 && c.tune == 10);   // a new group: the master's again
    CHECK(!strcmp(p.path(0), "/MULTISAMPLE/X/smp/lo.wav"));
  }
  {  // a <global> after a group resets everything below it
    D p;
    CHECK(p.parse("<global> volume=-3 <group> tune=5 <region> sample=a.wav <global> <region> sample=b.wav"));
    CHECK(p.d.regions[0].volume == -3 && p.d.regions[0].tune == 5 && p.d.regions[1].volume == 0 && p.d.regions[1].tune == 0);
  }
  {  // round robin, random, loops, offsets, release trigger, aliases
    D p;
    CHECK(p.parse("<group> key=60 seq_length=3 <region> sample=r1.wav seq_position=1 <region> sample=r2.wav seq_position=2 "
                  "<region> sample=r3.wav seq_position=3 lorand=0.5 hirand=1\n"
                  "<region> sample=l.wav loop_mode=loop_continuous loop_start=100 loop_end=999 offset=10 end=2000\n"
                  "<region> sample=l.wav loopmode=loop_sustain loopstart=5 loopend=50 trigger=release\n"
                  "<region> sample=l.wav loop_mode=one_shot\n"));
    CHECK(p.d.nregions == 6 && p.d.nsamples == 4);   // l.wav once
    CHECK(p.d.regions[1].seq_length == 3 && p.d.regions[1].seq_position == 2 && p.d.regions[2].lorand == 0.5f);
    const Region& l = p.d.regions[3];
    CHECK(l.loop_mode == kLoopContinuous && l.loop_start == 100 && l.loop_end == 999 && l.offset == 10 && l.end == 2000);
    CHECK(p.d.regions[4].loop_mode == kLoopSustain && p.d.regions[4].loop_start == 5 && p.d.regions[4].trigger == kTrigRelease);
    CHECK(p.d.regions[5].loop_mode == kOneShot && p.d.regions[3].sample == p.d.regions[4].sample);
  }
  {  // comments, spaces in file names, #define, the next opcode on the same line, CRLF
    D p;
    CHECK(p.parse("// a comment\r\n#define $KEY 62\r\n/* block\n <region> sample=no.wav */\r\n"
                  "<region> sample=My Piano C4.wav key=$KEY // tail\r\n<region>sample=b.wav lokey=d4 hikey=e4\r\n"));
    CHECK(p.d.nregions == 2 && !strcmp(p.path(0), "/MULTISAMPLE/X/My Piano C4.wav") && p.d.regions[0].lokey == 62);
    CHECK(p.d.regions[1].lokey == 62 && p.d.regions[1].hikey == 64);
  }
  {  // note_offset / octave_offset; sidecar_load
    D p;
    CHECK(p.parse("<control> note_offset=2 octave_offset=-1 sidecar_load=ram <region> sample=a.wav key=c4"));
    CHECK(p.d.regions[0].lokey == 50 && p.d.load == 1);
    D q;
    CHECK(q.parse("<control> sidecar_load=stream <region> sample=a.wav") && q.d.load == 2);
  }
  {  // ignored and malformed input: counted, the rest still parses
    D p;
    CHECK(p.parse("<region> sample=a.wav fil_type=lpf_2p cutoff=500 lokey=zz garbage <curve> v000=0 <effect> type=x "
                  "<bogus> <region> sample=b.wav end=-1 <region> lokey=70 hikey=60 sample=c.wav <region> key=60 "
                  "<region> sample=d.wav tune=abc #include \"other.sfz\"\n"));
    CHECK(p.d.nregions == 2 && p.d.ignored >= 4 && !strcmp(p.d.first_ignored, "fil_type"));
    CHECK(p.d.skipped == 3 && p.d.malformed >= 4);   // end=-1, an empty key range, no sample
  }
  {  // nothing playable, binary, empty, too many regions
    D p;
    CHECK(!p.parse("") && strstr(p.d.err, "no playable region"));
    CHECK(!p.parse("<group> key=60"));
    const char bin[] = {'<', 'r', 'e', 'g', 'i', 'o', 'n', '>', 0, 's', 0};
    CHECK(!msamp::parse(bin, sizeof bin, "/X", &p.d));
    std::string many;
    for (int i = 0; i < 300; ++i) many += "<region> sample=a" + std::to_string(i % 80) + ".wav\n";
    CHECK(p.parse(many.c_str()) && p.d.nsamples == 64 && p.d.nregions == 252 && p.d.skipped == 48);   // 64 samples here
    many += std::string(4000, ' ') + many;
    CHECK(p.parse(many.c_str()) && p.d.nregions == 256);   // a full table: the rest skipped
  }
}

// ---- the machine
static std::vector<std::vector<uint8_t>> g_out;
static void out(const uint8_t* m, int n) { g_out.emplace_back(m, m + n); }

static std::vector<uint8_t> report(int cmd, int vb, int note, int vel, std::vector<float> p) {
  std::vector<uint8_t> m = {0xf0, 0x7d, 0x52, 0x54, (uint8_t)cmd, (uint8_t)vb};
  if (cmd == 0x04) { m.push_back(note); m.push_back(vel); }
  p.resize(16, 0);
  for (float x : p) { int i = (int)x; m.push_back(i); m.push_back((int)((x - i) * 128) & 0x7f); }
  m.push_back(0xf7);
  return m;
}
static void send(const std::vector<uint8_t>& m) { host_sysex(m.data(), (int)m.size()); }
static void off(int vi) { send({0xf0, 0x7d, 0x52, 0x54, 0x11, (uint8_t)vi, 0xf7}); }
// A MULTISAMPLE sound: INST, VEL 127, LEV 100, RTRG, ATK DEC SUS REL 64 (the instrument's), TUNE FINE 64, STRT 0,
// LOOP AUTO, CHD INV SPR VOI
static std::vector<float> ms(int inst, int rtrg = 0, int chd = 0, int voi = 0, int loop = 0) {
  return {(float)inst, 127, 100, (float)rtrg, 64, 64, 64, 64, 64, 64, 0, (float)loop, (float)chd, 0, 0, (float)voi};
}

constexpr int kOut = 48000 * 12;
static int16_t ring[kOut][kVoices];
static uint64_t g_us = 1000000;
static int g_w = 0;
// the card: none (every step at once), or a model (MB/s, a latency per read, stalls)
struct Card {
  double mbps = 0;
  double lat_lo_ms = 0, lat_hi_ms = 0;
  double stall_ms = 0, stall_every_s = 0;
  uint64_t busy_until = 0, next_stall = 0;
  uint32_t rnd = 1;
} g_card;

// The card queue (msamp/cardq.h) on a simulated card core: the streams' requests and a recorder's writes share one
// card timeline. queue: the requests first, the recorder in 64 KB pieces held back while cardq::busy() (unless its
// backlog passed 4 MB); old: the recorder first, whole 192 KB chunks (the FatFs spin lock's worst order).
struct Rec {
  bool on = false, queue = true;
  double mbps = 1.97;           // the recorder's data (13 files)
  double write_mbps = 9.2;      // the card inside the writes
  double stall_ms = 100, stall_every_s = 2;   // a write that takes this long, every so often
  double backlog = 0, max_backlog = 0;
  uint64_t next_stall = 0;
  uint32_t yields = 0, pieces = 0;
} g_rec;
static void card_core() {
  g_rec.backlog += g_rec.mbps * 1e6 * 250e-6;   // a block's worth
  if (g_rec.backlog > g_rec.max_backlog) g_rec.max_backlog = g_rec.backlog;
  for (int i = 0; i < 64 && g_card.busy_until <= g_us; ++i) {
    double ms = 0;
    bool rec_ready = g_rec.backlog >= (g_rec.queue ? cardq::kPiece : 196608);
    bool rec_first = !g_rec.queue && rec_ready;
    bool rec_now = rec_first || (rec_ready && (!cardq::busy() || g_rec.backlog > 4e6));
    if (g_rec.queue && rec_ready && !rec_now) ++g_rec.yields;
    cardq::Req* r = rec_first ? nullptr : cardq::service();
    g_card.rnd = g_card.rnd * 1664525u + 1013904223u;
    double lat = g_card.lat_lo_ms + (g_card.lat_hi_ms - g_card.lat_lo_ms) * (g_card.rnd >> 8) / 16777216.0;
    if (r) {
      ms = lat + (r->kind == cardq::kReadReq ? r->len / (g_card.mbps * 1e3) : 0.2);
    } else if (rec_now) {
      double n = g_rec.queue ? cardq::kPiece : 196608;
      g_rec.backlog -= n;
      ms = n / (g_rec.write_mbps * 1e3);
      if (g_us >= g_rec.next_stall) {   // the card's own pause inside a write (its erase / allocation)
        ms += g_rec.stall_ms;
        g_rec.next_stall = g_us + (uint64_t)(g_rec.stall_every_s * 1e6);
      }
      ++g_rec.pieces;
    } else break;
    g_card.busy_until = (g_card.busy_until > g_us ? g_card.busy_until : g_us) + (uint64_t)(ms * 1000);
  }
}

static void idle() {
  if (g_rec.on) {   // the control core posts and takes results (never blocks); the card core runs them
    for (int i = 0; i < 64 && msamp::step(); ++i) {}
    card_core();
    for (int i = 0; i < 64 && msamp::step(); ++i) {}
    return;
  }
  if (g_card.mbps <= 0) {
    for (int i = 0; i < 2000 && msamp::step(); ++i) {}
    return;
  }
  for (int i = 0; i < 64 && g_card.busy_until <= g_us; ++i) {
    uint64_t b0 = msamp::stats().stream_bytes;
    if (!msamp::step()) break;
    uint64_t b = msamp::stats().stream_bytes - b0;
    g_card.rnd = g_card.rnd * 1664525u + 1013904223u;
    double lat = g_card.lat_lo_ms + (g_card.lat_hi_ms - g_card.lat_lo_ms) * (g_card.rnd >> 8) / 16777216.0;
    double ms = lat + (b ? b / (g_card.mbps * 1e3) : 0.2);
    if (g_card.stall_every_s > 0 && g_us >= g_card.next_stall) {   // the recorder's write holds the card
      ms += g_card.stall_ms;
      g_card.next_stall = g_us + (uint64_t)(g_card.stall_every_s * 1e6);
    }
    g_card.busy_until = g_us + (uint64_t)(ms * 1000);
  }
}
static void render(int blocks) {
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) {
    idle();
    core::render_voices(ring, kOut, g_w, 0, core::kVoices, mach);
    g_w = (g_w + core::kBlock) % kOut;
    g_us += 250;
    host_fake_time(true, g_us);
  }
}
static void restart() {   // the ring from 0
  g_w = 0;
  memset(ring, 0, sizeof ring);
}
static double rms(int voice, int a, int b) {
  double s = 0;
  for (int i = a; i < b; ++i) s += (double)ring[i][voice] * ring[i][voice];
  return std::sqrt(s / (b - a));
}
static double freq(int voice, int a, int b) {   // rising zero crossings a second
  int n = 0, first = -1, last = -1;
  for (int i = a + 1; i < b; ++i)
    if (ring[i - 1][voice] < 0 && ring[i][voice] >= 0) {
      if (first < 0) first = i;
      last = i, ++n;
    }
  return n > 1 ? (n - 1) * 48000.0 / (last - first) : 0;
}
static double brightness(int voice, int a, int b) {   // mean |diff| / mean |x|: a saw is far brighter than a sine
  double d = 0, x = 0;
  for (int i = a + 1; i < b; ++i) d += std::abs(ring[i][voice] - ring[i - 1][voice]), x += std::abs(ring[i][voice]);
  return x > 0 ? d / x : 0;
}
static int entry(const char* name) {
  for (int k = 0; k < kList; ++k) {
    const char* p = list_path(k);
    if (*p && strstr(p, name)) return k;
  }
  return -1;
}

// ---- WAV writing for the generated test files
static void wav(const std::string& path, int rate, int ch, int bits, int frames, double f, double amp, int loop_s = -1) {
  FILE* fp = fopen(path.c_str(), "wb");
  CHECK(fp);
  int bps = bits / 8;
  uint32_t data = frames * ch * bps;
  bool smpl = loop_s >= 0;
  uint32_t riff = 4 + 8 + 16 + 8 + data + (smpl ? 8 + 60 : 0);
  auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, fp); };
  auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, fp); };
  fwrite("RIFF", 1, 4, fp), u32(riff), fwrite("WAVEfmt ", 1, 8, fp), u32(16);
  u16(1), u16(ch), u32(rate), u32(rate * ch * bps), u16(ch * bps), u16(bits);
  fwrite("data", 1, 4, fp), u32(data);
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < ch; ++c) {
      double x = amp * std::sin(2 * M_PI * f * i / rate + c) + 0.05 * std::sin(2 * M_PI * 3.1 * f * i / rate);
      int32_t v = (int32_t)std::lround(x * ((1 << (bits - 1)) - 1));
      fwrite(&v, bps, 1, fp);
    }
  if (smpl) {
    fwrite("smpl", 1, 4, fp), u32(60);
    uint32_t h[9] = {0, 0, 0, 60, 0, 0, 0, 1, 0}, l[6] = {0, 0, (uint32_t)loop_s, (uint32_t)frames - 1, 0, 0};
    fwrite(h, 4, 9, fp), fwrite(l, 4, 6, fp);
  }
  fclose(fp);
}
static void text(const std::string& path, const std::string& t) {
  FILE* fp = fopen(path.c_str(), "w");
  CHECK(fp);
  fputs(t.c_str(), fp);
  fclose(fp);
}

static std::string g_card_dir;
static constexpr int kFmB = 0x48;

static void setup_card() {
  char tmpl[] = "/tmp/msamp-test-XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card_dir = tmpl;
  std::string M = g_card_dir + "/MULTISAMPLE";
  CHECK(system(("mkdir -p '" + M + "' && cp -R ../content/MULTISAMPLE/SidecarTest '" + M + "/'").c_str()) == 0);
  // round robin: two takes of key 60, the second 20 dB down
  system(("mkdir -p '" + M + "/rr' '" + M + "/sim' '" + M + "/big' '" + M + "/stream' '" + M + "/.hidden'").c_str());
  text(M + "/rr/rr.sfz", "<group> key=60 seq_length=2 <region> sample=../SidecarTest/samples/C4_soft.wav seq_position=1 "
                          "<region> sample=../SidecarTest/samples/C4_soft.wav seq_position=2 volume=-20\n");
  // the test instrument streamed: its own text with sidecar_load=stream and the samples' folder
  {
    FILE* fp = fopen((M + "/SidecarTest/SidecarTest.sfz").c_str(), "r");
    CHECK(fp);
    std::string t;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, fp)) > 0) t.append(b, n);
    fclose(fp);
    size_t at = t.find("default_path=samples/");
    CHECK(at != std::string::npos);
    t.replace(at, strlen("default_path=samples/"), "sidecar_load=stream default_path=../SidecarTest/samples/");
    text(M + "/stream/stream.sfz", t);
  }
  // the streaming simulation's instrument: four 10 s stereo 24-bit samples at 48 kHz (2.9 MB each), once RAM, once STREAM
  std::string zones;
  const int keys[4] = {48, 55, 60, 67};
  for (int i = 0; i < 4; ++i) {
    char n[32];
    snprintf(n, sizeof n, "s%d.wav", i);
    wav(M + "/sim/" + n, 48000, 2, 24, 480000, 440.0 * std::pow(2, (keys[i] - 69) / 12.0), 0.4);
    zones += "<region> sample=" + std::string(n) + " pitch_keycenter=" + std::to_string(keys[i]) + " lokey=" +
             std::to_string(i ? keys[i] : 0) + " hikey=" + std::to_string(i < 3 ? keys[i + 1] - 1 : 127) + "\n";
  }
  text(M + "/sim/simram.sfz", "<control> sidecar_load=ram <global> ampeg_release=0.3 loop_mode=no_loop\n" + zones);
  text(M + "/sim/simstream.sfz", "<control> sidecar_load=stream <global> ampeg_release=0.3 loop_mode=no_loop\n" + zones);
  // over the cap: three 40 s mono samples (3.8 MB each as int16)
  for (int i = 0; i < 3; ++i) wav(M + "/big/b" + std::to_string(i) + ".wav", 48000, 1, 16, 1920000, 220.0 * (i + 1), 0.3);
  text(M + "/big/big.sfz", "<control> sidecar_load=ram <region> sample=b0.wav key=48 <region> sample=b1.wav key=60 "
                           "<region> sample=b2.wav key=72\n");
  text(M + "/.hidden/no.sfz", "<region> sample=x.wav\n");
  // gain staging: a hot looped sine (0.95 of full scale) on every key
  system(("mkdir -p '" + M + "/hot'").c_str());
  wav(M + "/hot/h.wav", 48000, 1, 16, 96000, 261.63, 0.95, 0);
  text(M + "/hot/hot.sfz", "<control> sidecar_load=ram <region> sample=h.wav pitch_keycenter=60 loop_mode=loop_continuous\n");
  text(M + "/broken.sfz", "<group> nothing here\n");
  host_set_card(g_card_dir.c_str());
}

static void test_list_and_load() {
  // 7 instruments in the INSTRUMENTS pool (pool_layout): big, hot, rr, SidecarTest, simram, simstream, stream; an
  // .sfz with no region is refused by the pool (its .sfz read when it goes in)
  CHECK(stats().listed == 7);
  CHECK(entry("broken.sfz") < 0 && entry("SidecarTest.sfz") >= 0 && entry("no.sfz") < 0);
  char o[600];
  CHECK(msamp::command("msamp=voice:0", o, sizeof o) && strstr(o, "voice 0 plays MULTISAMPLE"));
  int e = entry("SidecarTest.sfz");
  send(report(0x03, kFmB | 0, -1, 0, ms(e)));   // the sound selected: the load starts
  CHECK(voice_slot(0) < 0);
  uint32_t loads = stats().loads;
  send(report(0x04, kFmB | 0, 60, 100, ms(e)));   // a trig before it is loaded: waits
  CHECK(stats().waits == 1);
  restart();
  render(40);   // the load (no card model: at once), then the held trig
  CHECK(stats().loads == loads + 1 && voice_slot(0) >= 0 && stats().trigs == 1);
  int s = voice_slot(0);
  const Desc* d = slot_desc(s);
  CHECK(d && d->nregions == 11 && d->nsamples == 11 && slot_ram(s));   // 1.3 MB: RAM automatically
  CHECK(slot_bytes(s) > 1000000 && slot_bytes(s) < 1400000);
  // an empty row: no instrument, a trig sounds nothing
  char o2[600];
  msamp::command("msamp=voice:1", o2, sizeof o2);
  send(report(0x04, kFmB | 1, 60, 100, ms(20)));
  render(20);
  CHECK(voice_slot(1) < 0 && voice_notes(1) == 0);
  for (int v = 0; v < kVoices; ++v) msamp::silence(v);
  render(400);
}

// A sequenced chord (core/seq: poly::seq_on per note -> msamp::live_on) as the very first trig on a voice whose
// instrument is not loaded yet: its notes wait and start when it is (kLateMs); a note-off before that drops its note.
static void test_seq_chord_before_load() {
  char o[200];
  msamp::command("msamp=rescan", o, sizeof o);   // nothing resident
  render(40);
  int e = entry("SidecarTest.sfz");
  msamp::command("msamp=voice:2", o, sizeof o);
  send(report(0x03, kFmB | 2, -1, 0, ms(e)));   // the machine selected: the load queued, not done
  CHECK(voice_slot(2) < 0);
  uint32_t waits = stats().waits;
  const int keys[4] = {60, 64, 67, 71};
  for (int k : keys) CHECK(poly::seq_on(2, k, 100));
  poly::seq_off(2, 71);   // released before the load: dropped
  CHECK(stats().waits == waits + 4 && voice_notes(2) == 0);
  restart();
  render(40);   // loaded: the three held notes start
  CHECK(voice_slot(2) >= 0 && voice_notes(2) >= 3);
  render(400);
  CHECK(rms(2, 4800, 9600) > 300);
  poly::seq_off(2, -1);
  render(8000);
  CHECK(voice_notes(2) == 0);
  msamp::command("msamp=voice:2:off", o, sizeof o);
  printf("test_msamp: sequenced chord before the load PASS\n");
}

static void test_play() {
  int e = entry("SidecarTest.sfz");
  // key 60, soft layer: C4 (261.6 Hz) as a sine; the loud layer the same pitch, brighter
  restart();
  send(report(0x04, kFmB | 0, 60, 60, ms(e)));
  render(1200);   // 0.3 s
  double fs = freq(0, 4800, 14400), bs = brightness(0, 4800, 14400);
  CHECK(std::abs(fs - 261.63) < 1.0);
  off(0);
  render(4000);
  restart();
  send(report(0x04, kFmB | 0, 60, 120, ms(e)));
  render(1200);
  double fl = freq(0, 4800, 14400), bl = brightness(0, 4800, 14400);
  CHECK(std::abs(fl - 261.63) < 1.5 && bl > bs * 1.3);   // the saw layer
  // key 67 plays the C4 zone a fifth up; key 72 the C5 zone (523.3 Hz)
  restart();
  send(report(0x04, kFmB | 0, 67, 60, ms(e)));
  render(1200);
  CHECK(std::abs(freq(0, 4800, 14400) - 392.0) < 1.5);
  restart();
  send(report(0x04, kFmB | 0, 72, 60, ms(e)));
  render(1200);
  CHECK(std::abs(freq(0, 4800, 14400) - 523.25) < 2.0);
  // TUNE +12: an octave up
  restart();
  std::vector<float> p = ms(e);
  p[8] = 76;
  send(report(0x04, kFmB | 0, 60, 60, p));
  render(1200);
  CHECK(std::abs(freq(0, 4800, 14400) - 523.25) < 2.0);
  // held past the sample's 1.25 s: loop_sustain keeps it sounding
  restart();
  send(report(0x04, kFmB | 0, 60, 60, ms(e)));
  render(8000);   // 2 s
  CHECK(rms(0, 48000 * 1 + 24000, 48000 * 2 - 1) > 0.8 * rms(0, 14400, 24000) && rms(0, 14400, 24000) > 300);
  off(0);   // released: the loop runs out (the region's loop_sustain), the release fades
  render(8000);
  CHECK(rms(0, 48000 * 3 + 24000, 48000 * 4 - 1) < 5);
  CHECK(stats().active == 0);
}

// 0.3.0 (a debugger's find): a note whose note-on and note-off both come before its first render (a very short Rytm
// trig: its 0x04 and 0x11 in one control pass) was silent - the render saw it released at level 0. It plays its attack
// for a block and then releases, as FM / PLAITS sound such a trig; a normal note is unchanged (test_play above).
static void test_short_trig() {
  int e = entry("SidecarTest.sfz");
  restart();
  send(report(0x04, kFmB | 0, 60, 120, ms(e)));
  off(0);   // before any render
  render(400);   // 0.1 s
  double r = rms(0, 0, 4800);
  printf("test_msamp: a trig released before its first render: rms %.1f\n", r);
  CHECK(r > 0);
  render(8000);   // its release runs out
  CHECK(stats().active == 0 && rms(0, 48000 * 2 - 4800, 48000 * 2) < 1);
}

static void test_chords_rr_release() {
  int e = entry("SidecarTest.sfz");
  restart();
  uint32_t n0 = stats().notes;
  send(report(0x04, kFmB | 0, 60, 60, ms(e, 0, 6, 0)));   // CHD 6 = MAJ7: C E G B
  render(200);
  CHECK(stats().notes - n0 == 4 && stats().active == 4);
  off(0);
  render(4000);
  CHECK(stats().active == 0);
  // VOI 6: six notes on one voice
  n0 = stats().notes;
  send(report(0x04, kFmB | 0, 48, 60, ms(e, 0, 1, 6)));
  render(50);
  CHECK(stats().notes - n0 == 6);
  off(0);
  render(4000);
  // release regions: RTRG on, a note-off starts the breath; off, nothing
  n0 = stats().notes;
  send(report(0x04, kFmB | 0, 60, 90, ms(e, 100)));
  render(100);
  off(0);
  render(10);
  CHECK(stats().notes - n0 == 2);
  render(4000);
  n0 = stats().notes;
  send(report(0x04, kFmB | 0, 60, 90, ms(e, 0)));
  render(100);
  off(0);
  render(4000);
  CHECK(stats().notes - n0 == 1);
  // round robin: the takes alternate (the second 20 dB down)
  int rr = entry("rr.sfz");
  send(report(0x03, kFmB | 0, -1, 0, ms(rr)));
  render(40);
  double lv[4];
  for (int i = 0; i < 4; ++i) {
    restart();
    send(report(0x04, kFmB | 0, 60, 127, ms(rr)));
    render(800);
    lv[i] = rms(0, 2400, 9600);
    off(0);
    render(6000);
  }
  CHECK(lv[0] > lv[1] * 5 && lv[2] > lv[3] * 5 && std::abs(lv[0] - lv[2]) < lv[0] * 0.01);
}

static void test_cap_and_ram() {
  // big.sfz asks for RAM: 11.5 MB, more than half the 10 MB pool RAM: streamed instead, its heads only
  int e = entry("big.sfz");
  send(report(0x03, kFmB | 0, -1, 0, ms(e)));
  render(200);
  int s = voice_slot(0);
  CHECK(s >= 0 && !slot_ram(s) && slot_bytes(s) < 400000);
  // heads of 60 s: each sample whole (3.8 MB), the third past the pool RAM (the first two pinned): left out, its key silent
  for (int v = 0; v < kVoices; ++v) msamp::silence(v);
  render(100);
  msamp::set_head_ms(60000);
  char o[600];
  msamp::command("msamp=rescan", o, sizeof o);
  render(50);
  uint32_t left = stats().left_out;
  send(report(0x03, kFmB | 0, -1, 0, ms(entry("big.sfz"))));
  render(400);
  CHECK(voice_slot(0) >= 0 && stats().left_out == left + 1);
  restart();
  send(report(0x04, kFmB | 0, 72, 100, ms(entry("big.sfz"))));
  render(400);
  CHECK(rms(0, 0, 4800) < 1);
  send(report(0x04, kFmB | 0, 48, 100, ms(entry("big.sfz"))));
  render(400);
  CHECK(rms(0, 4800, 9600) > 500);
  for (int v = 0; v < kVoices; ++v) msamp::silence(v);
  render(100);
  msamp::set_head_ms(kHeadMs);
  msamp::command("msamp=rescan", o, sizeof o);
  render(50);
}

// The same notes from the RAM instrument and the streamed one: the voice's output equal sample for sample.
static std::vector<int16_t> play_sim(const char* name, int voices, bool chords, int secs) {
  int e = entry(name);
  for (int v = 0; v < voices; ++v) {
    char o[100];
    msamp::command(("msamp=voice:" + std::to_string(v)).c_str(), o, sizeof o);
    send(report(0x03, kFmB | v, -1, 0, ms(e)));
  }
  g_card.mbps = 0;
  render(4000);   // loaded (no card model)
  for (int v = 0; v < voices; ++v) CHECK(voice_slot(v) >= 0);
  restart();
  std::vector<int16_t> o;
  g_card.busy_until = g_us;
  return o;
}
static std::vector<int16_t> run_sim(const char* name, int voices, int secs, const Card& card) {
  play_sim(name, voices, true, secs);
  g_card = card;
  g_card.busy_until = g_us, g_card.next_stall = g_us + 500000;
  int e = entry(name);
  std::vector<int16_t> o;
  const int keys[4] = {48, 50, 53, 55};
  for (int t = 0; t < secs * 2; ++t) {   // a chord a voice every 0.5 s, the previous one released (its tail streams on)
    if (t % 4 == 0)
      for (int v = 0; v < voices; ++v) send(report(0x04, kFmB | v, keys[v & 3] + 2 * (t / 4 % 3), 127, ms(e, 0, 6)));
    render(2000);
    for (int i = 0; i < 24000; ++i) o.push_back(ring[(g_w - 24000 + i + kOut) % kOut][0]), o.push_back(ring[(g_w - 24000 + i + kOut) % kOut][3]);
    if (t % 4 == 3)
      for (int v = 0; v < voices; ++v) off(v);
  }
  for (int v = 0; v < kVoices; ++v) msamp::silence(v);
  g_card.mbps = 0;
  render(2000);
  return o;
}

// Gain staging (as poly FM's): one key exactly as before (its gain 1: the headroom stays 1), chords of 3 / 6 keys at
// full velocity never at full scale (the soft limiter takes the overs, counted), the headroom smooth
static void test_headroom() {
  int e = entry("hot.sfz");
  send(report(0x03, kFmB | 0, -1, 0, ms(e)));
  render(100);
  CHECK(voice_slot(0) >= 0);
  struct Run {
    int16_t peak;
    int full;
    float dmax, g;
    double rms;
  };
  auto run = [&](std::vector<float> p, bool live, int nlive) {
    restart();
    float g = msamp::headroom(0), dmax = 0;
    if (live) {
      const int keys[6] = {60, 64, 67, 72, 76, 79};
      for (int k = 0; k < nlive; ++k) msamp::live_on(0, keys[k], 127, 3, keys[k]);
    } else {
      send(report(0x04, kFmB | 0, 60, 127, p));
    }
    for (int b = 0; b < 1600; ++b) {   // 0.4 s
      render(1);
      float h = msamp::headroom(0);
      dmax = std::max(dmax, std::fabs(h - g));
      g = h;
    }
    Run r{0, 0, dmax, g, rms(0, 4800, 19200)};
    for (int i = 0; i < 19200; ++i) {
      int x = std::abs((int)ring[i][0]);
      r.peak = (int16_t)std::max<int>(r.peak, std::min(x, 32767));
      r.full += x >= 32767;
    }
    if (live) msamp::live_off(0, 3, -1);
    else off(0);
    render(4000);
    CHECK(stats().active == 0);
    render(400);
    CHECK(msamp::headroom(0) == 1.0f);
    return r;
  };
  uint32_t lim0 = voice_limited(0);
  Run one = run(ms(e), false, 0);
  // the sine 0.95 + its 0.05 partial, times the voices' 0.7: as before the headroom
  double want = 0.7 * 32767 * std::sqrt((0.95 * 0.95 + 0.05 * 0.05) / 2);
  printf("test_msamp: one key rms %.0f (want %.0f, %.2f dB), headroom %.3f\n", one.rms, want, 20 * std::log10(one.rms / want), one.g);
  CHECK(std::fabs(20 * std::log10(one.rms / want)) < 0.5 && one.dmax == 0 && one.g == 1.0f && voice_limited(0) == lim0);
  for (int voi : {3, 6}) {
    Run c = run(ms(e, 0, 1, voi), false, 0);   // MAJ, VOI 3 / 6
    printf("test_msamp: %d keys: peak %d (%.2f dBFS), headroom %.3f, step %.4f, limited %lu\n", voi, c.peak,
           20 * std::log10(c.peak / 32767.0), c.g, c.dmax, (unsigned long)voice_limited(0));
    CHECK(c.full == 0 && c.peak <= (int)(kSoftCeil * 32767) + 1 && c.dmax < 0.06f);
    CHECK(std::fabs(c.g - 1 / std::sqrt((float)voi)) < 0.02f);
  }
  CHECK(voice_limited(0) > lim0);   // the 6 keys' overs: limited, not clipped
  Run l = run({}, true, 6);   // six live pads at 0.6 each
  CHECK(l.full == 0 && l.peak <= (int)(kSoftCeil * 32767) + 1 && std::fabs(l.g - 1 / std::sqrt(6.0f)) < 0.02f);
  // the Rytm's mono pads on MULTISAMPLE (core/poly.h kStealMs): channel 14 bound to voice 0 by two note-on / 0x04
  // pairs; three pads 20 ms apart, each pad's note-on followed by the previous pad's note-off: all three keep
  // sounding (the hot sine loops while held), the last pad's note-off releases them all
  poly::reset();
  for (int k = 0; k < 2; ++k) {
    poly::midi(0x9D, 30 + k, 100);
    render(4);   // 1 ms
    send(report(0x04, kFmB | 0, 54 + k, 100, ms(e)));
    render(4);
  }
  CHECK(poly::stats().bound[13] == 0);
  poly::midi(0x8D, 30, 0), poly::midi(0x8D, 31, 0);
  render(4000);
  CHECK(stats().active == 0);
  const int pads[3] = {36, 40, 43};
  uint32_t steals = poly::stats().mono_steals;
  for (int k = 0; k < 3; ++k) {
    poly::midi(0x9D, pads[k], 110);
    render(4);
    if (k) poly::midi(0x8D, pads[k - 1], 0);   // the Rytm's mono note-off
    send(report(0x04, kFmB | 0, pads[k] + 24, 110, ms(e)));
    render(76);
  }
  render(1200);   // 0.3 s
  CHECK(stats().active == 3 && poly::stats().mono_steals == steals + 2);
  poly::midi(0x8D, pads[2], 0);
  render(4000);
  CHECK(stats().active == 0);
  poly::reset();   // (no binding left for the next tests)
  printf("test_msamp: headroom PASS\n");
}

static void test_streaming() {
  // the streamed SidecarTest opens reading only its heads: plays the same as the RAM one
  int es = entry("/stream.sfz");
  send(report(0x03, kFmB | 0, -1, 0, ms(es)));
  render(100);
  int s = voice_slot(0);
  CHECK(s >= 0 && !slot_ram(s));
  // 0.5 s heads, the loop (0.25..1.25 s, to the end) preloaded past them: as many bytes as RAM's, nothing streams, the
  // same output (the head + loop part path)
  std::vector<int16_t> outs[2];
  for (int k = 0; k < 2; ++k) {
    int e = k ? es : entry("SidecarTest.sfz");
    send(report(0x03, kFmB | 0, -1, 0, ms(e)));
    render(100);
    restart();
    send(report(0x04, kFmB | 0, 55, 100, ms(e, 100)));   // RTRG on: the breath at the end
    render(8000);
    off(0);
    render(8000);
    for (int i = 0; i < 192000; ++i) outs[k].push_back(ring[i][0]);
  }
  CHECK(outs[0] == outs[1] && rms(0, 0, 96000) > 300);
  for (int v = 0; v < kVoices; ++v) msamp::silence(v);
  render(100);

  // the simulation instrument: 2.9 MB a sample on the card, 0.5 s heads (24000 frames as int16 mono) in RAM
  Card none;
  std::vector<int16_t> a = run_sim("simram.sfz", 4, 6, none);
  int sr = voice_slot(0);
  CHECK(slot_ram(sr) && slot_bytes(sr) > 3800000);
  // the pools design 4.1: the same files in another instrument share simram's whole copies (no head read, no stream);
  // for the streaming check below they are dropped from the pool RAM first
  {
    uint32_t sh = pcache::stats().shared_hits;
    std::vector<int16_t> c = run_sim("simstream.sfz", 4, 2, none);
    CHECK(pcache::stats().shared_hits >= sh + 4 && slot_bytes(voice_slot(0)) > 3800000);
    for (int v = 0; v < kVoices; ++v) msamp::silence(v);
    render(100);
    char o[200];
    msamp::command("msamp=rescan", o, sizeof o);   // (the slots let go of their samples)
    for (int i = 0; i < 4; ++i) {
      lib::Ref r;
      lib::ref_for_path(("/MULTISAMPLE/sim/s" + std::to_string(i) + ".wav").c_str(), &r);
      pcache::forget(r);
    }
  }
  uint32_t u0 = stats().underruns;
  std::vector<int16_t> b = run_sim("simstream.sfz", 4, 6, none);
  int ss = voice_slot(0);
  CHECK(!slot_ram(ss) && slot_bytes(ss) < 4 * 24000 * 2 + 4096);
  CHECK(a.size() == b.size() && a == b);   // bit-exact
  CHECK(stats().underruns == u0 && stats().streams_peak >= 16);
  uint64_t rd0 = stats().stream_bytes;
  CHECK(rd0 > 0);
  printf("test_msamp: streaming %d notes at most, %llu KB read\n", stats().streams_peak, (unsigned long long)(rd0 >> 10));

  // a card of 12 MB/s, 0.3..3 ms a read, a 100 ms stall every 2 s (the recorder's worst write): no underrun, the same
  Card ok;
  ok.mbps = 12, ok.lat_lo_ms = 0.3, ok.lat_hi_ms = 3, ok.stall_ms = 100, ok.stall_every_s = 2;
  uint32_t steals0 = stats().bw_steals;
  std::vector<int16_t> c = run_sim("simstream.sfz", 4, 6, ok);
  CHECK(stats().underruns == u0 && stats().bw_steals == steals0);
  CHECK(c == a);
  // a card of 2 MB/s with 20 ms reads and 250 ms stalls: too slow for 16 stereo 24-bit notes, and it says so
  Card slow;
  slow.mbps = 2, slow.lat_lo_ms = 5, slow.lat_hi_ms = 20, slow.stall_ms = 250, slow.stall_every_s = 1;
  run_sim("simstream.sfz", 4, 6, slow);
  CHECK(stats().underruns > u0 || stats().bw_steals > steals0);
  printf("test_msamp: slow card: %u underruns, %u bandwidth steals\n", stats().underruns - u0, stats().bw_steals - steals0);
}

// The card queue with a recorder writing 13 files (1.97 MB/s) on the Pi card's figures (reads 12 MB/s with 0.3..3 ms a
// read; writes 9.2 MB/s with a 100 ms pause every 2 s: docs/MULTISAMPLE.md): MAJ7 chords on 4 voices (16 held + 16
// releasing stereo 24-bit streams) play bit-exact what the RAM instrument plays, no underrun, the recorder kept up
// (its backlog bounded). The old order (the recorder's whole chunks first) on the same card underruns.
static const int g_cq_voices = 4;
static void test_card_queue() {
  Card none;
  std::vector<int16_t> a = run_sim("simram.sfz", g_cq_voices, 6, none);
  Card pi;
  pi.mbps = 12, pi.lat_lo_ms = 0.3, pi.lat_hi_ms = 3;
  cardq::reset();
  cardq::set_worker(true);
  g_rec = Rec();
  g_rec.on = true, g_rec.next_stall = g_us + 300000;
  uint32_t u0 = stats().underruns, s0 = stats().bw_steals;
  std::vector<int16_t> b = run_sim("simstream.sfz", g_cq_voices, 6, pi);
  uint32_t uq = stats().underruns - u0, sq = stats().bw_steals - s0;
  printf("test_msamp: card queue: %u underruns, %u steals, %u requests (max %u waiting, longest wait %u ms), recorder "
         "%u pieces, held back %u times, backlog max %.0f KB\n", uq, sq, cardq::stats().served, cardq::stats().max_pending,
         cardq::stats().max_wait_us / 1000, g_rec.pieces, g_rec.yields, g_rec.max_backlog / 1024);
  CHECK(uq == 0 && sq == 0 && a == b);
  CHECK(g_rec.max_backlog < 16e6 && g_rec.pieces > 150);   // the recorder kept up: its ring (the Pi's 64 MB: 30 s) never near full
  CHECK(cardq::stats().full == 0);   // (8 in flight at most; cancelled ones stay in the ring until passed)
  // the old order on the same card
  cardq::reset();
  cardq::set_worker(true);
  g_rec = Rec();
  g_rec.on = true, g_rec.queue = false, g_rec.next_stall = g_us + 300000;
  u0 = stats().underruns, s0 = stats().bw_steals;
  run_sim("simstream.sfz", g_cq_voices, 6, pi);
  uint32_t uo = stats().underruns - u0, so = stats().bw_steals - s0;
  printf("test_msamp: the recorder first (no queue): %u underruns, %u steals\n", uo, so);
  CHECK(uo > 0 || so > 0);
  g_rec = Rec();
  cardq::reset();
  printf("test_msamp: card queue PASS\n");
}

// A cancelled request reposted (core/store's refill does this): its first ring slot stays behind as a ghost, until the
// card core passes it. The card core must run the repost once, with its new fields (the new buffer, never the old
// one), and skip the other slot; pending() drains to 0.
static void test_card_queue_repost() {
  cardq::reset();
  File f;
  CHECK(f.open("/MULTISAMPLE/rr/rr.sfz", core::kRead));   // "<group> key=60 ..."
  cardq::set_worker(true);
  static cardq::Req r;
  memset(&r, 0, sizeof r);
  uint8_t a[6], b[6];
  memset(a, 0xee, sizeof a);
  memset(b, 0xee, sizeof b);
  CHECK(!cardq::async(&r, cardq::kReadReq, &f, nullptr, 0, a, 4));   // posted (slot 0)
  CHECK(cardq::cancel(&r) && cardq::idle(&r));
  CHECK(!cardq::async(&r, cardq::kReadReq, &f, nullptr, 1, b, 5));    // reposted (slot 1): new off, buf, len
  CHECK(cardq::pending() == 2);
  CHECK(cardq::service() == &r);    // through the ghost slot: the repost, once
  CHECK(cardq::service() == nullptr && cardq::pending() == 0);   // the live slot skipped (done already)
  CHECK(cardq::stats().served == 1);
  CHECK(cardq::async(&r, cardq::kReadReq, &f, nullptr, 1, b, 5) && r.got == 5 && !memcmp(b, "group", 5) && b[5] == 0xee);
  for (uint8_t c : a) CHECK(c == 0xee);   // the cancelled request's buffer never written
  // the caller reposts again before the card core reaches the second slot: still one run per posting
  CHECK(!cardq::async(&r, cardq::kReadReq, &f, nullptr, 0, a, 4));
  CHECK(cardq::cancel(&r));
  CHECK(!cardq::async(&r, cardq::kReadReq, &f, nullptr, 0, b, 1));
  CHECK(cardq::service() == &r && cardq::async(&r, cardq::kReadReq, &f, nullptr, 0, b, 1) && r.got == 1 && b[0] == '<');
  CHECK(!cardq::async(&r, cardq::kReadReq, &f, nullptr, 1, b, 1));   // a third posting while the second's slot waits
  CHECK(cardq::service() == &r && cardq::service() == nullptr && cardq::pending() == 0);
  CHECK(cardq::async(&r, cardq::kReadReq, &f, nullptr, 1, b, 1) && r.got == 1 && b[0] == 'g');
  for (uint8_t c : a) CHECK(c == 0xee);
  CHECK(cardq::stats().served == 3 && cardq::stats().cancelled == 2);
  f.close();
  cardq::reset();
  printf("test_msamp: card queue repost PASS\n");
}

// Stereo (docs/STEREO.md): a stereo sample kept stereo in the pool RAM (poolmono=0); on a voice routed OUT POST its L
// and R exactly as the file (unity pitch, gain 1), from RAM and streamed (the ring interleaved); on TRK the mono sum
// exactly as the summed load gave it (poolmono=1); an sfz pan / width moves the pair
static int16_t ring_r[kOut][kVoices];
static void render2(int blocks) {
  uint64_t mach[core::kMachines] = {0};
  for (int b = 0; b < blocks; ++b) {
    idle();
    core::render_voices(ring, kOut, g_w, 0, core::kVoices, mach, ring_r);
    g_w = (g_w + core::kBlock) % kOut;
    g_us += 250;
    host_fake_time(true, g_us);
  }
}
static void test_stereo() {
  pcache::set_mono(false);
  std::string M = g_card_dir + "/MULTISAMPLE/st";
  CHECK(system(("mkdir -p '" + M + "'").c_str()) == 0);
  const int frames = 96000;
  wav(M + "/lr.wav", 48000, 2, 16, frames, 440, 0.3);   // L and R a radian apart, the 3.1x partial on both
  text(M + "/st.sfz", "<control> sidecar_load=ram <region> sample=lr.wav pitch_keycenter=60 loop_mode=no_loop\n");
  text(M + "/sts.sfz", "<control> sidecar_load=stream <region> sample=lr.wav pitch_keycenter=60 loop_mode=no_loop\n");
  text(M + "/stp.sfz", "<control> sidecar_load=ram <region> sample=lr.wav pitch_keycenter=60 loop_mode=no_loop pan=-100\n");
  text(M + "/stw.sfz", "<control> sidecar_load=ram <region> sample=lr.wav pitch_keycenter=60 loop_mode=no_loop width=0\n");
  CHECK(pu::put(pools::kInstr, "A010", "/MULTISAMPLE/st/st.sfz") == pools::kOk);
  CHECK(pu::put(pools::kInstr, "A011", "/MULTISAMPLE/st/sts.sfz") == pools::kOk);
  CHECK(pu::put(pools::kInstr, "A012", "/MULTISAMPLE/st/stp.sfz") == pools::kOk);
  CHECK(pu::put(pools::kInstr, "A013", "/MULTISAMPLE/st/stw.sfz") == pools::kOk);
  render(400);
  // the file's values
  std::vector<int16_t> L(frames), R(frames);
  {
    FILE* fp = fopen((M + "/lr.wav").c_str(), "rb");
    CHECK(fp && fseek(fp, 44, SEEK_SET) == 0);
    std::vector<int16_t> d(2 * frames);
    CHECK(fread(d.data(), 2, d.size(), fp) == d.size());
    fclose(fp);
    for (int i = 0; i < frames; ++i) L[i] = d[2 * i], R[i] = d[2 * i + 1];
  }
  auto want = [](int16_t x) {   // the voice's soft_out at unity: x / 32768 x 0.7 (linear below the knee)
    float y = (float)x * (1.0f / 32768.0f) * 0.7f * 32767.0f;
    return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
  };
  char o[200];
  const int vi = 4;
  msamp::command("msamp=voice:4", o, sizeof o);
  msamp::set_head_ms(50);
  std::vector<int16_t> mono_native;
  for (int k = 0; k < 2; ++k) {   // RAM, then STREAM
    int e = entry(k ? "/st/sts.sfz" : "/st/st.sfz");
    send(report(0x03, kFmB | vi, -1, 0, ms(e)));
    render(400);
    CHECK(voice_slot(vi) >= 0 && slot_ram(voice_slot(vi)) == (k == 0));
    out::control.route[vi] = out::kPost;
    restart();
    send(report(0x04, kFmB | vi, 60, 127, ms(e)));
    auto c0 = std::chrono::steady_clock::now();
    render2(4000);   // 1 s
    double t_st = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    int start = -1;
    for (int i = 0; i < 4000 && start < 0; ++i)
      if (ring[i][vi] || ring_r[i][vi]) start = i;
    CHECK(start >= 0);
    int bad = 0;
    for (int i = 0; i < 40000; ++i) bad += ring[start + i][vi] != want(L[i]) || ring_r[start + i][vi] != want(R[i]);
    printf("test_msamp: stereo %s on POST: %d of 40000 frames off the file\n", k ? "STREAM" : "RAM", bad);
    if (bad)
      for (int i : {0, 1, 2, 100, 1000, 1001, 5000})
        printf("  %d: got %d %d want %d %d (file %d %d) slot %d ram %d bytes %llu\n", i, ring[start + i][vi], ring_r[start + i][vi], want(L[i]), want(R[i]), L[i], R[i], voice_slot(vi), slot_ram(voice_slot(vi)), (unsigned long long)slot_bytes(voice_slot(vi)));
    CHECK(bad == 0);
    msamp::silence(vi);
    render2(100);
    // TRK: mono, the summed value; with the R plane given (the Pi's render) R = L and L as without it
    out::control.route[vi] = out::kTrk;
    restart();
    send(report(0x04, kFmB | vi, 60, 127, ms(e)));
    c0 = std::chrono::steady_clock::now();
    if (k) render(4000);
    else render2(4000);
    double t_mono = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    printf("test_msamp: one %s note, 1 s of all 8 voices' render: POST (stereo) %.2f ms, TRK (mono) %.2f ms (Mac)\n",
           k ? "STREAM" : "RAM", t_st * 1000, t_mono * 1000);
    std::vector<int16_t> m;
    for (int i = 0; i < 48000; ++i) m.push_back(ring[i][vi]);
    if (!k)
      for (int i = 0; i < 48000; ++i)
        for (int v = 0; v < kVoices; ++v) CHECK(ring_r[i][v] == ring[i][v]);
    if (!k) mono_native = m;
    else CHECK(m == mono_native);   // the stream: the same as RAM
    msamp::silence(vi);
    render(100);
  }
  // TRK: exactly the old layout's value (poolmono=1 / before stereo: summed at load, (L + R) >> 1 as convert() sums)
  {
    int start = -1;
    for (int i = 0; i < 4000 && start < 0; ++i)
      if (mono_native[i]) start = i;
    CHECK(start >= 0);
    int bad = 0;
    for (int i = 0; i < 40000; ++i) bad += mono_native[start + i] != want((int16_t)(((int32_t)L[i] + R[i]) >> 1));
    printf("test_msamp: stereo on TRK: %d of 40000 frames off the summed load\n", bad);
    CHECK(bad == 0);
  }
  // pan=-100 (a stereo sample: the balance hard left, R silent), width=0 (L = R = the mid)
  for (int k = 0; k < 2; ++k) {
    int e = entry(k ? "/st/stw.sfz" : "/st/stp.sfz");
    send(report(0x03, kFmB | vi, -1, 0, ms(e)));
    render(400);
    out::control.route[vi] = out::kPost;
    restart();
    send(report(0x04, kFmB | vi, 60, 127, ms(e)));
    render2(2000);
    double el = 0, er = 0, d = 0;
    for (int i = 4800; i < 19200; ++i) el += std::abs(ring[i][vi]), er += std::abs(ring_r[i][vi]), d += std::abs(ring[i][vi] - ring_r[i][vi]);
    if (!k) CHECK(el > 1e6 && er == 0);
    else CHECK(el > 1e6 && d <= 14400);   // (rounding: within 1 LSB a frame)
    msamp::silence(vi);
    render2(100);
  }
  out::control.route[vi] = out::kTrk;
  // the streaming budget (core/pool/stream_budget.h): the stereo STREAM instrument on a card too slow for 8 of its
  // voices is held in RAM (forced, counted); within the budget it streams; "always in RAM" holds it whole regardless
  {
    msamp::command("msamp=rescan", o, sizeof o);
    render(40);
    int e = entry("/st/sts.sfz");
    sbudget::clear();
    sbudget::set_bench(0, 12.0f, 25.0f);
    send(report(0x03, kFmB | vi, -1, 0, ms(e)));
    render(400);
    CHECK(voice_slot(vi) >= 0 && slot_ram(voice_slot(vi)) && sbudget::forced_now() == 1);
    CHECK(!strcmp(msamp::entry_note(e, o, sizeof o), "RAM (BUDGET)"));
    msamp::command("msamp=rescan", o, sizeof o);   // (unused now: its slot freed, the count back)
    render(40);
    sbudget::set_bench(0, 20.0f, 2.0f);
    send(report(0x03, kFmB | vi, -1, 0, ms(e)));
    render(400);
    CHECK(voice_slot(vi) >= 0 && !slot_ram(voice_slot(vi)) && sbudget::forced_now() == 0);
    msamp::command("msamp=rescan", o, sizeof o);
    render(40);
    CHECK(pools::command("pool=inst:ram:A011:on", o, sizeof o) && pools::always_ram(pools::kInstr, 11) && strstr(o, "always in RAM"));
    send(report(0x03, kFmB | vi, -1, 0, ms(e)));
    render(400);
    CHECK(voice_slot(vi) >= 0 && slot_ram(voice_slot(vi)) && sbudget::forced_now() == 0);
    CHECK(pools::command("pool=inst:ram:A011:off", o, sizeof o) && !pools::always_ram(pools::kInstr, 11));
    sbudget::clear();
    printf("test_msamp: stereo over the streaming budget: RAM; within it: STREAM; always in RAM: RAM\n");
  }
  msamp::command("msamp=voice:4:off", o, sizeof o);
  msamp::set_head_ms(kHeadMs);
  printf("test_msamp: stereo PASS\n");
}

int main() {
  test_notes_paths();
  test_parse();
  setenv("CORE_MSAMP_RAM_MB", "64", 1);
  setenv("CORE_POOL_RAM_MB", "10", 1);    // the pool RAM: 10 MB (the cap below: big.sfz's three 3.8 MB samples)
  // poolmono=1 (docs/STEREO.md): the sizes below are int16 mono (stereo files summed at load, as before stereo
  // samples); native stereo storage, its sizes and its render are test_stereo's
  pcache::set_mono(true);
  setup_card();
  hooks.to_rytm = ::out;
  host_fake_time(true, g_us);
  voices_init();
  card_ok = true;
  srclib::begin(true);
  pools::begin(true);
  msamp::begin(true);
  pu::steps();
  {
    const char* const insts[] = {"/MULTISAMPLE/big/big.sfz", "/MULTISAMPLE/hot/hot.sfz", "/MULTISAMPLE/rr/rr.sfz",
                                 "/MULTISAMPLE/SidecarTest/SidecarTest.sfz", "/MULTISAMPLE/sim/simram.sfz",
                                 "/MULTISAMPLE/sim/simstream.sfz", "/MULTISAMPLE/stream/stream.sfz"};
    for (int i = 0; i < 7; ++i) {
      char r[8];
      snprintf(r, sizeof r, "A%03d", i + 1);
      CHECK(pu::put(pools::kInstr, r, insts[i]) == pools::kOk);
    }
    CHECK(pu::put(pools::kInstr, "A008", "/MULTISAMPLE/broken.sfz") == pools::kErrType);
  }
  render(400);   // the list
  test_list_and_load();
  test_seq_chord_before_load();
  test_play();
  test_short_trig();
  test_chords_rr_release();
  test_cap_and_ram();
  test_headroom();
  test_streaming();
  test_card_queue();
  test_card_queue_repost();
  test_stereo();
  system(("rm -rf '" + g_card_dir + "'").c_str());
  printf("test_msamp: PASS\n");
  return 0;
}
