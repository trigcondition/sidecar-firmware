// pi/engine/bridge.cpp - see bridge.h. Hosted (the core's flags).
#include "bridge.h"

#include <math.h>
#include <string.h>

#include "engine.h"
#include "md/md.h"   // the built-in MD's cost (poly_json)
#include "lfo.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "heap_buffers.h"
#include "keys.h"
#include "notes.h"
#include "poly.h"
#include "inst.h"   // E0: the sequencer's instances ("inst" on the console, /status)
#include "seq/rytm_dump.h"
#include "seq/seq.h"
#include "song/song.h"
#include "song_pi.h"
#include "msamp/msamp.h"
#include "pool/pcache.h"
#include "pool/stream_budget.h"
#include "msamp/cardq.h"
#include "store/store.h"
#include "fmt/fmt.h"
#include "msamp/sdtest.h"
#include "nmach.h"   // WAVETABLE, GRANULAR (wt= / gr= on the console, the live input)
#include "livein/livein.h"   // LIVE IN (livein on the console, AUDIO IN from the IN completion)
#include "samp/autosamp.h"
#include "samp/sampler.h"
#include "slots.h"
#include "user/um.h"   // the guard key: a user plugin's load (machine_gen)
#include "ui/extra.h"
#include "ui/remote.h"
#include "spsc.h"   // the remote UI's hand-off from core 0 (rui_status, rui_popup)
#if SIDECAR_SEQED
#include "seqed_pi.h"
#include "rytmpat_pi.h"
#endif

namespace bridge {

static_assert(kVoices == core::kVoices && kBlock == core::kBlock && kMachines == core::kMachines, "bridge sizes");

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

// path written whole or not at all: a power loss mid-write leaves only path.tmp (removed on the next boot), never
// a short file that exists() would then keep. fill(f) writes the content and is false on a short write. Few, large
// writes: the Pi's File::write syncs on every call.
template <typename Fill>
static bool write_whole(core::Fs& fs, const char* path, Fill fill) {
  char tmp[64];
  sniprintf(tmp, sizeof tmp, "%s.tmp", path);
  core::File f;
  if (!f.open(tmp, core::kWrite)) return false;
  bool ok = fill(f);
  f.close();
  if (ok && fs.rename(tmp, path)) return true;
  fs.remove(tmp);
  return false;
}

bool selftest_content() {
  // (the pools design: DSP SAMPLE's slot 1 is the SAMPLES pool's A001 of the project _default)
  static const char kWav[] = "/Samples/pi-test/sine440.wav", kPool[] = "/.rytm/projects/_default/samples.pool.txt";
  core::Fs fs;
  if (!fs.begin()) return false;
  fs.remove("/Samples/pi-test/sine440.wav.tmp");   // a write cut short by a power loss
  fs.remove("/.rytm/projects/_default/samples.pool.txt.tmp");
  if (!fs.exists(kWav)) {
    fs.mkdir("/Samples/pi-test");
    bool ok = write_whole(fs, kWav, [](core::File& f) {
      const uint32_t frames = 96000;
      uint8_t h[44];
      memcpy(h, "RIFF", 4); put32(h + 4, 36 + frames * 2); memcpy(h + 8, "WAVEfmt ", 8);
      put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, 48000); put32(h + 28, 96000);
      put16(h + 32, 2); put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, frames * 2);
      if (f.write(h, sizeof h) != sizeof h) return false;
      static int16_t buf[9600];   // 0.2 s a write: 10 writes (static: once, at boot)
      for (uint32_t i = 0; i < frames; i += 9600) {
        for (int k = 0; k < 9600; ++k) buf[k] = (int16_t)(12000.0f * sinf(6.2831853f * 440.0f * (i + k) / 48000.0f));
        if (f.write(buf, sizeof buf) != sizeof buf) return false;
      }
      return true;
    });
    core::log(ok ? "selftest: wrote %s" : "selftest: could not write %s", kWav);
    if (!ok) return false;
  }
  if (!fs.exists("/.rytm/project") && !fs.exists(kPool)) {
    fs.mkdir("/.rytm/projects/_default");
    static const char kText[] = "#sidecar samplepool 1\nA001 sample - 192044 /Samples/pi-test/sine440.wav\tsine440\n";
    bool ok = write_whole(fs, kPool, [](core::File& f) { return f.write(kText, sizeof kText - 1) == sizeof kText - 1; });
    core::log(ok ? "selftest: SAMPLES pool _default A001 = pi-test/sine440.wav" : "selftest: could not write %s", kPool);
  }
  return true;
}

#ifdef CORE_HEAP_BUFFERS
unsigned long heap_bytes() { return core::heap_buffers_bytes(); }
void heap_place(void* mem) { core::heap_buffers_place(mem); }
#else
unsigned long heap_bytes() { return 0; }
void heap_place(void*) {}
#endif

bool begin(void (*to_host)(const uint8_t*, int)) {
  core::hooks.to_rytm = to_host;
  return core::engine_begin();
}
void sysex(const uint8_t* d, int len) { core::engine_sysex(d, len); }
uint32_t render(int16_t (*block)[kVoices], int v0, int v1, uint64_t* mach, int16_t (*block_r)[kVoices], uint8_t* st) {
  return core::render_voices(block, kBlock, 0, v0, v1, mach, block_r, st);
}
bool idle_prepare() { return core::idle_prepare(); }
bool idle_save() { return core::idle_save(); }
void idle_stream() { core::idle_stream(); }
// The remote UI is the control core's: remote_status and remote_popup write the state remote_tick draws from (the
// popup's lines among it). The stats tick on core 0 only queues them here; tick() hands them over on the control core.
// One producer (core 0's stats tick), one consumer (tick): a full queue drops the newest, which the next second sends
// again (the status) or offers again (the temperature popup, thermal::popup_due).
namespace {
struct RuiPopupMsg {
  char l1[core::ui::kTextLen + 1], l2[core::ui::kTextLen + 1];
};
Spsc<core::ui::RemoteStatus, 4> g_rui_status_q;
Spsc<RuiPopupMsg, 4> g_rui_popup_q;
void rui_take() {
  core::ui::RemoteStatus st;
  bool have = false;
  while (g_rui_status_q.pop(&st)) have = true;   // the newest counts
  if (have) core::ui::remote_status(st);
  RuiPopupMsg m;
  while (g_rui_popup_q.pop(&m)) core::ui::remote_popup(m.l1, m.l2[0] ? m.l2 : nullptr);
}
}  // namespace

void tick() {
  if (!core::hooks.slot_ask) core::slots_1d(5000);   // the user machines (core/user/um.cpp) send their own 1D
  rui_take();                              // core 0's status and popups for the remote UI, on this core
  core::ui::remote_tick(core::now_ms());   // nothing while the Rytm's remote page is closed
  core::lfo::tick(core::now_us());        // the Sidecar's LFOs (core/lfo.h): once a millisecond at most
  core::poly::tick(core::now_ms());       // poly FM: a held trig's time, a live chord's window (core/poly.h)
  core::seq::tick(core::now_ms());        // the Sidecar sequencer: its LEDs, its 2E (core/seq/seq.h)
  core::song::tick(core::now_ms());       // SONG RECORD (core/song/song.h)
  song_pi::Tick();                        // its view for the display's SONG page
#if SIDECAR_SEQED
  seqed_pi::Tick();                       // the touch editor's view for the display (docs/TOUCH-EDITOR.md)
  rytmpat_pi::Tick();                     // the Rytm pattern view: its dump fetched while the page is open
#endif
  core::samp::tick();                     // a written take's instruments listed (core/samp/sampler.h)
  core::samp::asamp_tick();               // the auto-sampler: a Rytm take's notes on time, its rescan (core/samp/autosamp.h)
}
void seq_block() { core::seq::pre_block(); }
void poly_midi(uint8_t st, uint8_t d1, uint8_t d2) {
  core::poly::midi(st, d1, d2);           // (and through its note sink the sequencer's recording and step entry)
}
// poly (poly FM) and msamp (MULTISAMPLE, core/msamp/msamp.h) share the console's branch
bool sdtest_command(const char* p, bool busy, char* out, int cap) { return core::sdtest::command(p, busy, out, cap); }
int sdtest_json(char* out, int cap) { return core::sdtest::json(out, cap); }
bool sdtest_running() { return core::sdtest::state() == core::sdtest::kRunning; }
void card_worker(bool on) { core::cardq::set_worker(on); }
int card_service(int n) {
  int k = 0;
  while (k < n && core::cardq::service()) ++k;
  return k;
}
bool card_busy() { return core::cardq::busy(); }
bool samp_command(const char* p, char* out, int cap) {
  return core::samp::command(p, out, cap) || core::samp::asamp_command(p, out, cap);
}
void samp_transport(uint8_t st) { core::samp::transport(st); }
bool samp_wants_in() { return core::samp::wants_in() || core::samp::asamp_wants_in(); }
void samp_in(const int32_t* f, int n, int stride) {
  core::samp::in_block(f, n, stride);
  core::samp::asamp_in_block(f, n, stride);
}
bool samp_wants_voices() { return core::samp::wants_voices(); }
void samp_voices(const int16_t (*v)[kVoices], int n, const int16_t (*lr)[2 * kVoices], const float (*fx)[4]) { core::samp::voice_block(v, n, lr, fx); }
void pool_mono(bool on) { core::pcache::set_mono(on); }
void stereo_trim(int pair, int db10) { core::out::set_trim(pair, db10); }
void stream_bench(int vol, unsigned mbps_x100, unsigned p99_us) {
  core::sbudget::set_bench(vol, mbps_x100 / 100.0f, p99_us / 1000.0f);
}
bool samp_poll() {
  bool a = core::samp::poll();
  return core::samp::asamp_poll() || a;
}
int samp_json(char* out, int cap) {
  int k = sniprintf(out, cap, ",\"samp\":");
  if (k > 0 && k < cap) k += core::samp::json(out + k, cap - k);
  if (k > 0 && k < cap) k += sniprintf(out + k, cap - k, ",\"asamp\":");
  if (k > 0 && k < cap) k += core::samp::asamp_json(out + k, cap - k);
  return k;
}
bool poly_command(const char* p, char* out, int cap) {
  return core::poly::command(p, out, cap) || core::msamp::command(p, out, cap) || core::nm::command(p, out, cap) ||   // wt / gr
         core::seq::command(p, out, cap) || core::keys::command(p, out, cap) || core::rytm_dump::command(p, out, cap) ||
         core::song::command(p, out, cap) || core::inst::command(p, out, cap) ||
         core::ins::command(p, out, cap) ||   // fx-ins: "ins", "ins=..." (core/fx/insert.h)
         core::amp::command(p, out, cap) ||   // INSERT AMP: "amp", "amp=rescan" (core/fx/amp.h)
         core::out::command(p, out, cap) ||   // the OUT route: "out=Vn:...", "stereo..." (core/fx/out.h)
         core::sbudget::command(p, out, cap) ||   // the streaming budget: "stream" (core/pool/stream_budget.h)
         core::livein::command(p, out, cap);    // LIVE IN: "livein", "livein=voice:V", "livein=measure" (core/livein/livein.h)
}
void keys_midi(int src, uint8_t st, uint8_t d1, uint8_t d2) { core::keys::midi(src, st, d1, d2); }
void keys_source(int src, bool present, const char* name) { core::keys::source(src, present, name); }
void keys_set_forward(void (*f)(uint8_t, uint8_t, uint8_t)) { core::keys::set_forward(f); }
void asamp_set_midi_out(bool (*f)(int, const uint8_t*, int)) { core::samp::asamp_set_midi_out(f); }
bool keys_computer_on() { return core::keys::computer_on(); }
const char* keys_line(char* b, int cap) { return core::keys::line(b, cap); }
void lfo_midi(uint8_t st, uint8_t d1, uint8_t d2) { core::lfo::midi_in(st, d1, d2); }
bool lfo_command(const char* p, char* out, int cap) { return core::lfo::command(p, out, cap); }
int lfo_json(char* out, int cap) { return core::lfo::json(out, cap); }
int poly_json(char* out, int cap) {
  int n = core::seq::poly_json(out, cap);
  if (n > 0 && n + 2 < cap) {
    out[n++] = ',';
    int k = core::inst::json(out + n, cap - n);
    n = k > 0 ? n + k : n - 1;
  }
  if (n > 0 && n + 2 < cap) {   // the built-in MD's control-core cost (core/md/md.h Cost: TYPE changes and their most)
    out[n++] = ',';
    int k = core::md::json(out + n, cap - n);
    n = k > 0 ? n + k : n - 1;
  }
  return n;
}
int seqx_json(char* out, int cap) { return core::seq::seqx_json(out, cap); }
void rui_status(int ob_running, uint32_t underruns, int c1, int c2, int c3, const RuiMore* more) {
  core::ui::RemoteStatus s = {};
  s.ob = (int8_t)ob_running, s.underruns = underruns, s.cpu[0] = (int8_t)c1, s.cpu[1] = (int8_t)c2, s.cpu[2] = (int8_t)c3;
  if (more) {
    s.rec = (uint8_t)more->rec, s.rec_secs = more->rec_secs, s.eth = (uint8_t)more->eth, s.wifi = (uint8_t)more->wifi;
    memcpy(s.ip, more->ip, sizeof s.ip);
    memcpy(s.user, more->user, sizeof s.user);
  }
  g_rui_status_q.push(s);   // the control core's tick() applies it (and the first-dropout popup it may raise)
}
void rui_popup(const char* line1, const char* line2) {
  RuiPopupMsg m;
  sniprintf(m.l1, sizeof m.l1, "%s", line1 ? line1 : "");
  sniprintf(m.l2, sizeof m.l2, "%s", line2 ? line2 : "");
  g_rui_popup_q.push(m);    // shown by the control core's tick(); the pairing code's popup wins there (remote_popup)
}
uint32_t rui_popups() { return core::ui::remote_status_popups(); }   // the pairing code's own are not counted
void rui_rec(const RuiRec& r) {
  core::ui::RecInfo i = {};
  i.state = (uint8_t)r.state, i.secs = r.secs, i.free_mb = r.free_mb, i.total_mb = r.total_mb, i.need_kbps = r.need_kbps;
  i.in_mask = (uint16_t)r.in_mask;
  i.ch_select = r.ch_select != 0;
  memcpy(i.take, r.take, sizeof i.take);
  memcpy(i.error, r.error, sizeof i.error);
  core::ui::remote_rec(i);
}
int rui_rec_take() { return core::ui::remote_rec_take(); }
unsigned rui_rec_toggles() { return core::ui::remote_rec_toggles(); }
void rui_play(const RuiPlay& p) {
  core::ui::PlayInfo i = {};
  i.state = (uint8_t)p.state, i.level = (uint8_t)p.level, i.ch_i = (uint8_t)p.ch_i, i.channels = (uint8_t)p.channels;
  i.take_i = (uint8_t)p.take_i, i.takes = (uint8_t)(p.takes > 255 ? 255 : p.takes), i.pos_ms = p.pos_ms, i.len_ms = p.len_ms;
  memcpy(i.take, p.take, sizeof i.take);
  memcpy(i.channel, p.channel, sizeof i.channel);
  memcpy(i.error, p.error, sizeof i.error);
  core::ui::remote_play(i);
}
bool rui_play_take(int* toggle, int* channel, int* take, int* level) {
  core::ui::PlayInput in;
  bool any = core::ui::remote_play_take(&in);
  *toggle = in.toggle, *channel = in.channel, *take = in.take, *level = in.level;
  return any;
}
bool tap_on() { return core::ui::tap_on(); }
// seq-p2: the sequencer's latency calibration (a click due or on its way back) and the Rytm pattern dump's reader
bool cal_on() { return core::seq::cal_wanted(); }
void cal_in(const int32_t* f, int n, int stride) { core::seq::cal_audio(f + 2, n, stride, (int64_t)core::seq::frames()); }
void rytm_byte(uint8_t b) {
  core::rytm_dump::byte(b);
  core::song::byte(b);   // SONG RECORD (core/song/song.h): the whole stream
}
void tap_in(const int32_t* f, int n, int stride) { core::ui::tap_in(f, n, stride); }
bool livein_on() { return core::livein::wanted(); }
void livein_in(const int32_t* f, int n, int stride) { core::livein::in_block(f, n, stride); }
void livein_fifo(unsigned frames) { core::livein::set_fifo((int)frames); }
int livein_status(int vi, char* b, int cap) {
  if (!core::v[vi & 7].li) return 0;
  return core::livein::status_short(vi, b, cap);
}
bool gr_live_on() { return core::gr::live_wanted(); }
void gr_live_in(const int32_t* f, int n, int stride) { core::gr::live_in(f, n, stride); }
bool fxin_command(const char* p, char* out, int cap) { return core::fxin::command(p, out, cap); }
int fxin_json(char* out, int cap) { return core::fxin::json(out, cap); }

// The pairing code on the Rytm's own screen (pi/engine/paircode.h): a popup that stays 30 s (kPopupMaxMs; a key ends
// it), the case being someone reading eight digits off a small screen and typing them into a browser on another
// device: too short is as bad as not showing it. The control core's, as every other remote UI call is. The code is
// passed in and never kept here. `over`: it goes on top of a Sidecar page rather than deferring to it.
bool pair_popup(const char* line) {
  return core::ui::remote_popup("SIDECAR PAIR CODE", line, core::ui::kPopupMaxMs, true);
}
void pair_code(const char* grouped) { core::ui::remote_pair_code(grouped); }
uint32_t rytm_hellos() { return core::ui::remote_stats().hellos; }
void webui_init(bool (*on)(), void (*set_on)(bool), int (*gate)(), int (*devices)(), bool (*new_code)(),
                void (*forget)()) {
  static core::ui::RemoteWebUi h;
  h = {on, set_on, gate, devices, new_code, forget};
  core::ui::remote_webui(&h);
}
void pair_popup_end() { core::ui::remote_popup_end(); }
bool rytm_greeted() { return core::ui::remote_stats().hellos != 0; }

bool rui_command(const char* p) {
  if (strcmp(p, "rui=on") == 0 || strcmp(p, "rui=off") == 0) {
    core::ui::remote_request(p[5] == 'n');
    core::log("rui: page %s requested", p[5] == 'n' ? "open" : "close");
    return true;
  }
  if (strncmp(p, "rui=srclog=", 11) == 0) {   // 1027: each 29 / 2A and SRC view commit logged
    core::ui::remote_src_log(p[11] == '1');
    core::log("rui: SRC view log %s", p[11] == '1' ? "on" : "off");
    return true;
  }
  if (strcmp(p, "rui") != 0) return false;
  const core::ui::RemoteStats& s = core::ui::remote_stats();
  core::log("rui: %s, page %s%u, opens %lu closes %lu, frames %lu tiles %lu rects %lu bytes %lu alive %lu, keys %lu encs %lu "
            "edits %lu, widgets %lu, bad %lu, draw max %lu us mean %lu us",
            core::ui::remote_open() ? "open" : "closed", s.page == 1 ? "voice " : s.page ? "SYSTEM" : "SIDECAR", s.page == 1 ? s.voice + 1u : 0u,
            (unsigned long)s.opens, (unsigned long)s.closes, (unsigned long)s.frames, (unsigned long)s.tiles,
            (unsigned long)s.rects, (unsigned long)s.bytes, (unsigned long)s.keepalives, (unsigned long)s.keys,
            (unsigned long)s.encs, (unsigned long)s.edits, (unsigned long)s.widgets, (unsigned long)s.bad,
            (unsigned long)s.draw_us_max, (unsigned long)(s.frames ? s.draw_us_sum / s.frames : 0));
  // the Rytm's 33s (rui1025+): how many came and each track's voice byte (7f not a Sidecar machine, -- none yet): once
  // one came the voice pages (JUNO, 6-OP FM graphics ...) follow these alone
  static const char kHex[] = "0123456789abcdef";
  char t[12 * 3 + 1];
  for (int i = 0; i < 12; ++i) {
    int vb = core::ui::remote_track_machine(i);
    t[3 * i] = vb < 0 ? '-' : kHex[(vb >> 4) & 15], t[3 * i + 1] = vb < 0 ? '-' : kHex[vb & 15], t[3 * i + 2] = ' ';
  }
  t[12 * 3] = 0;
  core::log("rui: machines (33) %lu, tracks %s", (unsigned long)s.machines, t);
  core::log("rui: SRC view 29s %lu, 2As %lu, frames %lu", (unsigned long)s.src_views, (unsigned long)s.src_vals,
            (unsigned long)s.src_frames);
  return true;
}
// core::voices_init resets daisy, fm and user but not the dsps and juno flags (zero at boot): the boot state
// back for every voice, so the bench's last machine does not stay on voice 0. Pi-side, the core is shared.
static void voices_boot_state() {
  for (int i = 0; i < kVoices; ++i) core::wt::silence(i), core::gr::silence(i), core::chop::silence(i);   // the bench's tails
  {   // review minor: the bench's CHOP notes faded out here (this core renders the bench): none left busy on voice 0
    static float scratch[kBlock];
    for (int b = 0; b < 64; ++b) core::chop::render_add(0, scratch, kBlock);
  }
  core::chop::use_buffer(0, nullptr, 0, 1, 48000);   // (CHOP's bench buffer let go)
  core::voices_init();
  for (int i = 0; i < kVoices; ++i) core::v[i].dsps = core::v[i].grain = core::v[i].juno = core::v[i].md = false;
  core::ui::remote_forget_voices();   // the bench's reports were ours, not the Rytm's: no voice page from them
}
void reinit_voices() { voices_boot_state(); }

static void bench_out(const uint8_t* d, int n) { core::engine_sysex(d, n); }

uint64_t bench(const char* machine, uint32_t* worst_block, int* peak) {
  core::NoteAdapter na(bench_out);
  na.SetMachine(0, machine);
  na.Channel(0x90, 48, 100);
  while (core::idle_prepare()) {}   // DSP SAMPLE: its head read before it plays
  static int16_t blk[kBlock][kVoices];
  uint64_t mach[kMachines] = {0}, sum = 0;
  uint32_t worst = 0;
  int pk = 0;
  const bool live = !strcmp(machine, "livein");   // LIVE IN: a synthetic AUDIO IN (a 220 Hz tone at -12 dBFS)
  if (!strcmp(machine, "chop")) {   // CHOP: 8 slice players (its VOICES' most) on 1 s of a synthetic break, looped, MORPH on
    static int16_t brk[48000];
    for (int i = 0; i < 48000; ++i) {
      int p = i % 6000;
      brk[i] = (int16_t)(20000.0f * expf(-(float)p / 1500.0f) * sinf(0.02f * (float)p * (1.0f + 3.0f * expf(-(float)p / 400.0f))));
    }
    core::chop::use_buffer(0, brk, 48000, 1, 48000);
    core::chop::edit_grid(0, 16);
    for (int k = 0; k < 8; ++k) core::chop::edit_field(0, k, core::chop::kFLoop, core::chop::kLoopFwd);
    uint8_t m[8 + 32 + 1] = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)core::kChopMachine};   // P4 MRPH 64, LEV 100
    const uint8_t p[16] = {10, 0, 64, 64, 0, 0, 64, 100, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 16; ++k) m[6 + 2 * k] = p[k], m[7 + 2 * k] = 0;
    m[38] = 0xF7;
    core::engine_sysex(m, 39);
    for (int k = 0; k < 8; ++k) core::chop::live_on(0, k, 100, core::poly::kSeqCh, k);
  }
  for (int b = 0; b < 4000; ++b) {
    core::refill_neediest(4 * kBlock);
    if (live) {
      float in[kBlock];
      for (int s = 0; s < kBlock; ++s) in[s] = 0.25f * sinf(0.0287979f * (float)(b * kBlock + s));
      core::livein::in_frames(in, in, kBlock);
    }
    uint32_t t = core::render_voices(blk, kBlock, 0, 0, 1, mach);
    sum += t;
    if (t > worst) worst = t;
    for (int s = 0; s < kBlock; ++s) pk = blk[s][0] > pk ? blk[s][0] : -blk[s][0] > pk ? -blk[s][0] : pk;
    if (b == 2000) na.Channel(0x90, 55, 100);   // a second note halfway (a retrigger, as a pattern would)
  }
  na.Channel(0x80, 55, 0);
  voices_boot_state();
  *worst_block = worst;
  if (peak) *peak = pk;
  return sum;
}
uint64_t ins_bench(int a, int b, uint32_t* worst_block) {
  using namespace core::ins;
  static Engine* e = nullptr;
  static float (*lines)[kLine] = nullptr;
  if (!e) e = new Engine(48000.0f), lines = new float[kLines][kLine];
  memset(lines, 0, sizeof(float) * kLines * kLine);
  e->Place(lines);
  e->Reset();
  static Control c;
  memset(&c, 0, sizeof c);
  for (int v = 0; v < kVoices; ++v) {
    for (int k = 0; k < 4; ++k) c.type[v][0] = (uint8_t)a, c.type[v][1] = (uint8_t)b, c.play[v][0][k] = c.play[v][1][k] = (uint8_t)(40 + 20 * k);
    // ins1112: a == kOff && b == kOff is the post EQ measured on its own (kernel.cpp's last Pairs row); otherwise the
    // pair is measured with the post EQ flat, i.e. skipped, so the existing numbers stay comparable
    for (int k = 0; k < kEqParams; ++k)
      c.peq[v][k] = (int8_t)(a == kOff && b == kOff ? eq_store(k, k < 2 ? 70 : 100) : eq_store(k, kEqFlat(k)));
  }
  float x[kBlock];
  uint64_t sum = 0;
  uint32_t worst = 0;
  for (int n = 0; n < 4000; ++n) {
    uint32_t t0 = core::cycles();
    for (int v = 0; v < kVoices; ++v) {
      for (int s = 0; s < kBlock; ++s) x[s] = 0.3f * sinf(0.05f * (float)(n * kBlock + s) * (float)(v + 1));
      e->ProcessMono(v, x, kBlock, c);
    }
    uint32_t t = core::cycles() - t0;
    sum += t;
    if (t > worst) worst = t;
  }
  *worst_block = worst;
  return sum;
}
const char* ins_bench_name(int t) { return core::ins::type_name(t); }
uint64_t amp_bench(uint32_t* worst_block) { return core::amp::bench(worst_block); }
int ins_json(char* out, int cap) {   // "ins":{... "out":[...]}, then "stream":{...} (the OUT route, the streaming budget)
  int n = core::ins::json(out, cap);
  if (n > 0 && n < cap - 2) {
    out[n++] = ',';
    out[n] = 0;
    int k = core::sbudget::json(out + n, cap - n);
    if (k > 0 && k < cap - n) n += k;
    else out[--n] = 0;
  }
  return n;
}
unsigned poly_voices() {
  unsigned m = 0;
#if CORE_POLY
  for (int t = 0; t < 12; ++t)
    if (core::seq::owns_track(t)) m |= 1u << (core::ui::remote_track_voice(t) & 7);   // (a plain read: drawing only)
#endif
  return m;
}
int rytm_pattern(int* transport) {
#if CORE_POLY
  int tr = core::seq::transport_now();   // (plain reads: drawing only)
  if (transport) *transport = tr == core::seq::kPlay ? 1 : tr == core::seq::kPause ? 2 : 0;
  return core::seq::pattern_now();
#else
  if (transport) *transport = 0;
  return -1;
#endif
}
uint32_t machine_gen(int vi) {
  const core::V& x = core::v[vi & 7];
  return x.user >= 0 ? (uint32_t) (x.user + 1) | um::code_epoch() << 3 : 0;
}
const char* machine_name(int vi) {
  const core::V& x = core::v[vi & 7];
  // the shared short names (core/machines.h machine_short: the Rytm's SIDECAR page says the same)
  return x.user >= 0 ? "USER" : core::machine_short(x.li ? core::kLiveMachine : x.cp ? core::kChopMachine : x.gr ? core::kGranMachine : x.wt ? core::kWaveMachine : x.ms ? core::kMultiMachine
                                                    : x.fm ? core::kFm : x.juno ? core::kJuno : x.grain ? core::kGrainMachine
                                                    : x.dsps ? core::kDspSample : x.daisy ? core::kDaisy : x.md ? core::kMd : core::kPlaits);
}
// The Rytm's own answer for every track, the one thing no other field here carries: machine_name is per voice and
// per what we play, track_machine folds in the wt=/gr=/msamp= forcings, and poly_json's tail comes from the host's
// per-voice flags (no analog case at all). A track the Rytm plays itself reports 0x7F, so "RYTM" is the discriminating
// value: without it a stock analog track and a PLAITS one look the same from outside.
int track_src_json(char* out, int cap) {
  int n = sniprintf(out, cap, "\"src\":[");
  if (n <= 0 || n >= cap) return 0;
  for (int t = 0; t < 12; ++t) {
    int vb = core::ui::remote_track_machine(t);
    const char* m = vb < 0 ? "?" : vb == 0x7F ? "RYTM" : core::machine_short(vb & 0x78);
    int k = sniprintf(out + n, cap - n, "%s{\"m\":\"%s\",\"vb\":%d}", t ? "," : "", m, vb);
    if (k <= 0 || k >= cap - n) return 0;
    n += k;
  }
  if (n + 2 > cap) return 0;
  out[n++] = ']';
  out[n] = 0;
  return n;
}
// The RAM store (core/store/store.h) and what is still on its way to the card: unwritten = the card queue's pending requests
// plus fmt's writes the queue does not hold (queued, and the slot's once the card core has taken it: running or held); oldestMs the oldest of those. An edit still inside
// its 1 s settle has not been posted yet and is not counted.
int store_json(char* out, int cap, unsigned long heap_free) {
  uint32_t fo = 0, fn = core::fmt::unwritten(&fo), co = core::cardq::oldest_ms();
  int cn = core::cardq::pending();
  int n = sniprintf(out, cap,
                    "\"store\":{\"files\":%d,\"bytes\":%lu,\"ready\":%s,\"unwritten\":%lu,\"oldestMs\":%lu,\"held\":%lu,"
                    "\"heapFree\":%lu}",
                    core::store::present(), (unsigned long)core::store::bytes(), core::store::ready() ? "true" : "false",
                    (unsigned long)((cn > 0 ? (uint32_t)cn : 0) + fn), (unsigned long)(fo > co ? fo : co),
                    (unsigned long)core::fmt::post_held(), heap_free);
  return n > 0 && n < cap ? n : 0;
}
const char* stat_name(int k) {
  static const char* const n[kMachines] = {"fm", "juno", "dsps", "daisy", "md", "plaits", "user"};
  return k >= 0 && k < kMachines ? n[k] : "?";
}
int card_folders() { return core::card_ok ? core::cards.folders() : 0; }
const char* project() { return core::project; }
int pool_slots() { return core::npool; }
uint32_t to_host_dropped() { return core::to_rytm_dropped(); }

}  // namespace bridge
