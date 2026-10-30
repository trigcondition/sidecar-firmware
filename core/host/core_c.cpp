// core/host/core_c.cpp - the core on the Mac for Python (sim/coreengine.py, sim/tests/test_core_*.py), ctypes.
// Names: core_c_<what>. Strings are '\n'-joined lists where a call takes several.
#include <string.h>

#include <string>
#include <vector>

#include "core.h"
#include "poly.h"
#include "inst.h"
#include "seq/seq.h"
#include "seq/rytm_dump.h"
#include "song/song.h"
#include "msamp/msamp.h"
#include "msamp/sdtest.h"
#include "nmach.h"
#include "livein/livein.h"
#include "fx/send_fx.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "um_host.h"
#include "user/lists.h"
#include "ui/extra.h"
#include "bundle/bundle.h"
#include "bundle/feed.h"
#include "bundle/projects.h"

namespace {
std::vector<std::string> split(const char* joined, int count) {
  std::vector<std::string> v;
  std::string s = joined ? joined : "";
  size_t a = 0;
  for (int k = 0; k < count; ++k) {
    size_t b = s.find('\n', a);
    v.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    a = b == std::string::npos ? s.size() : b + 1;
  }
  return v;
}
}  // namespace

extern "C" {

// 16 / 17: names '\n'-joined, an empty line an empty slot.
int core_c_names(uint8_t* m, int cmd, int replace, int total, int first, const char* joined, int count) {
  std::vector<std::string> v = split(joined, count);
  std::vector<const char*> p;
  for (auto& s : v) p.push_back(s.empty() ? nullptr : s.c_str());
  return core::sysex_names(m, (uint8_t)cmd, replace, total, first, p.data(), count);
}

// 15 from a patch's 18 bytes (the knobs are bytes 5 6 7 11 12 14, as voiceloop's juno_select).
int core_c_juno_knobs(uint8_t* m, int voice, const uint8_t* patch18) {
  const int at[6] = {5, 6, 7, 11, 12, 14};
  uint8_t k[6];
  for (int i = 0; i < 6; ++i) k[i] = patch18[at[i]];
  return core::sysex_juno_knobs(m, voice, k);
}

// 14: rows as parallel arrays; names '\n'-joined (whole: len = their length).
int core_c_pool(uint8_t* m, int replace, const uint8_t* slots, const uint16_t* folders, const uint16_t* files,
                const char* joined, int count) {
  std::vector<std::string> v = split(joined, count);
  std::vector<core::PoolRow> r;
  for (int k = 0; k < count; ++k) r.push_back({slots[k], folders[k], files[k], v[k].c_str(), (int)v[k].size()});
  return core::sysex_pool(m, replace, r.data(), count);
}

// 0E: entries as parallel arrays; names '\n'-joined.
int core_c_listing(uint8_t* m, int folder, int parent, int first, int total, const uint8_t* kinds,
                   const uint32_t* ids, const uint32_t* sizes, const char* joined, int count) {
  std::vector<std::string> v = split(joined, count);
  std::vector<core::ListEntry> e;
  for (int k = 0; k < count; ++k) e.push_back({kinds[k], ids[k], sizes[k], v[k].c_str()});
  return core::sysex_listing(m, folder, parent, first, total, e.data(), count);
}

// The empty user slots (core/slots.cpp's replies): 1A part 0 EMPTY, 1A unchanged, 1B 0x21 empty, 1D.
int core_c_slot_empty(uint8_t* m, int slot) { return core::sysex_slot_empty(m, slot); }
int core_c_slot_unchanged(uint8_t* m, int slot) { return core::sysex_slot_unchanged(m, slot); }
int core_c_installed_empty(uint8_t* m) { return core::sysex_installed_empty(m); }
int core_c_slots(uint8_t* m, int answer_slot, int result) { return core::sysex_slots(m, answer_slot, result); }

// One StreamVoice (core/engines/stream_voice.h) over the card `card_dir`: the file at card path `path`, STA
// `start` (0..1), triggered, n frames rendered in kBlock pieces with a refill before each; returns frames made.
int core_c_stream_render(const char* card_dir, const char* path, float semis, float start, float end, int loop,
                         float level, float* out, int n) {
  static uint8_t head[rytm::StreamVoice::kHead], ring[rytm::StreamVoice::kRing];
  core::host_set_card(card_dir);
  core::File f;
  if (!f.open(path, core::kRead)) return -1;
  card::FileRec r;   // the index's record of it, from the file itself
  memset(&r, 0, sizeof r);
  uint8_t h[44];
  if (f.read(h, 44) != 44) return -2;
  r.channels = h[22];
  r.rate = h[24] | h[25] << 8 | h[26] << 16 | (uint32_t)h[27] << 24;
  r.bits = h[34];
  r.format = 1;
  r.data_offset = 44;
  r.data_len = h[40] | h[41] << 8 | h[42] << 16 | (uint32_t)h[43] << 24;
  static rytm::StreamVoice v;
  v.attach(head, ring);
  if (!v.prepare(r, f, 0, 0, start)) return -3;
  v.trigger();
  int done = 0;
  while (done < n) {
    int k = n - done < core::kBlock ? n - done : core::kBlock;
    v.refill();
    v.render(semis, start, end, loop != 0, level, out + done, k);
    done += k;
  }
  return done;
}

// ---- the engine (core/engine.h) for sim/coreengine.py: one per process
static std::vector<std::vector<uint8_t>> g_sysex;
static void host_out(const uint8_t* m, int n) { g_sysex.emplace_back(m, m + n); }
static const int kRingFrames = 1024;   // as the Teensy's send_frames ring
static int16_t g_ring[kRingFrames][core::kVoices];
static int g_rd = 0, g_n = 0;
static uint64_t g_mach[core::kMachines];

int core_c_begin(const char* card_dir) {
  core::host_set_card(card_dir);
  core::hooks.to_rytm = host_out;
  return core::engine_begin() ? 1 : 0;
}

// The user machines (core/user/um.h: the card's /Plugins, RMPA packages run in place), after core_c_begin. Without
// it the user slots answer as a board without plugins (core/slots.cpp), as the older tests expect.
void core_c_um_begin() { um::begin(core::host_um_exec()); }

// The user machines' console (core/user/um.h command) and status JSON.
int core_c_um_command(const char* c, char* out, int cap) { return um::command(c, out, cap) ? 1 : 0; }
int core_c_um_json(char* out, int cap) { um::publish_now(); return um::json_copy(out, cap); }
void core_c_um_rescan() { um::rescan(); }
// The pool and JUNO / FM lists' page side (core/user/lists.h): console and status JSON.
int core_c_lists_command(const char* c, char* out, int cap) { return lists::command(c, out, cap) ? 1 : 0; }
int core_c_lists_json(char* out, int cap) { lists::publish_now(); return lists::json_copy(out, cap); }

void core_c_sysex(const uint8_t* d, int len) { core::engine_sysex(d, len); }

// The work between requests until there is none (at most `passes` jobs); returns the jobs run.
int core_c_idle(int passes) {
  int did = 0;
  while (did < passes && (core::idle_prepare() || core::idle_save())) ++did;
  core::idle_stream();
  while (did < passes && um::idle()) ++did;   // the user machines' steps (scan, loads, answers, 1D)
  return did;
}

// n frames x 8 voices (int16, voice order 0..7), as the Teensy answers 'N' (send_frames): blocks rendered into
// the ring as needed, a block's leftover kept for the next call, a stream topped up before each block.
void core_c_render(int16_t* out, int n) {
  while (n > 0) {
    int want = n < kRingFrames - core::kBlock ? n : kRingFrames - core::kBlock;
    while (g_n < want) {
      core::refill_neediest((uint32_t)(want - g_n) + 4 * core::kBlock);
      core::seq::pre_block();   // the sequencer's notes due in this block (core/seq/seq.h)
      core::render_voices(g_ring, kRingFrames, (g_rd + g_n) % kRingFrames, 0, core::kVoices, g_mach);
      g_n += core::kBlock;
    }
    for (int k = 0; k < want; ++k) {
      memcpy(out, g_ring[g_rd], sizeof g_ring[0]);
      out += core::kVoices;
      g_rd = (g_rd + 1) % kRingFrames;
    }
    g_n -= want;
    n -= want;
  }
}

// The oldest SysEx the engine sent, into buf (max bytes); its length, 0 when none is left.
int core_c_take_sysex(uint8_t* buf, int max) {
  if (g_sysex.empty()) return 0;
  std::vector<uint8_t> m = g_sysex.front();
  g_sysex.erase(g_sysex.begin());
  int n = (int)m.size() < max ? (int)m.size() : max;
  memcpy(buf, m.data(), n);
  return n;
}

// The send FX (core/fx/send_fx.h): 0x20 into the values, the values out (send 24, knob 12, route, tag1 hi lo,
// tag2 hi lo: 41 bytes), 0x21 / 0x22 as built, a console command's reply.
int core_c_sendfx_take(const uint8_t* d, int len) { return core::fx::take_sendfx(d, len) ? 1 : 0; }
int core_c_sendfx_values(uint8_t* out) {
  int n = 0;
  for (int t = 0; t < core::fx::kTracks; ++t) out[n++] = core::fx::control.send[t][0], out[n++] = core::fx::control.send[t][1];
  for (int s = 0; s < core::fx::kSlots; ++s)
    for (int j = 0; j < core::fx::kKnobs; ++j) out[n++] = core::fx::control.knob[s][j];
  out[n++] = core::fx::control.route;
  for (int s = 0; s < core::fx::kSlots; ++s) out[n++] = core::fx::kit_tag[s] >> 7, out[n++] = core::fx::kit_tag[s] & 0x7f;
  return n;
}
int core_c_sendfx_names(uint8_t* m, int replace, int total, int first, int count) {
  return core::fx::sysex_names(m, replace != 0, total, first, count);
}
int core_c_sendfx_names_total() { return core::fx::names_total(); }
int core_c_sendfx_state(uint8_t* m, int slot) { return core::fx::sysex_state(m, slot); }
int core_c_sendfx_labels(uint8_t* m, int id) { return core::fx::sysex_labels(m, id); }
int core_c_fxin_command(const char* p, char* reply, int max) { return core::fxin::command(p, reply, max) ? 1 : 0; }   // IN L/R source (host_link.h)
int core_c_fxin_json(char* out, int max) { return core::fxin::json(out, max); }
int core_c_sendfx_command(const char* p, char* reply, int max) { return core::fx::command(p, reply, max) ? 1 : 0; }

// MIDI realtime / SPP into the send FX's clock (STUTTER's grid): status F8 FA FB FC F2, spp in 16ths.
void core_c_sendfx_clock(int status, int spp) { core::fx::clock_in((uint8_t)status, (uint16_t)spp); }

// The send FX DSP (core::fx::SendFx, one per process, as the Pi's render core 2) on the live values: n frames of the
// 8 stems (float, full scale 1.0, interleaved n x 8) -> out (n x 2: L/R; STUTTER's insert added on), in kMaxFrames chunks. sr: the sample rate
// of the first call (later calls keep it). Returns the faults so far (NaN / runaway restarts).
int core_c_sendfx_process(const float* stems, int n, float* out, float sr) {
  static core::fx::SendFx* fx = nullptr;
  if (!fx) fx = new core::fx::SendFx(sr > 0 ? sr : 48000.0f);
  for (int at = 0; at < n; at += core::fx::SendFx::kMaxFrames) {
    int k = n - at < core::fx::SendFx::kMaxFrames ? n - at : core::fx::SendFx::kMaxFrames;
    fx->Process(reinterpret_cast<const float(*)[core::fx::kStems]>(stems + at * core::fx::kStems), k,
                reinterpret_cast<float(*)[2]>(out + at * 2), core::fx::control);
  }
  return (int)fx->Faults();
}

// ---- the remote UI (core/ui/remote.h) for sim/rui_link.py and sim/rui_render.py
void core_c_rui_tick(uint32_t ms) { core::ui::remote_tick(ms); }
// The LFOs (core/lfo.h): the control side's tick, the Rytm's channel messages, the console, the JSON, the page's view;
// core_c_fake_time: now_us / now_ms read us (on 0: the real clock), so the emulator link runs them on emulated time.
void core_c_fake_time(int on, uint64_t us) { core::host_fake_time(on != 0, us); }
void core_c_lfo_tick(uint32_t us) { core::lfo::tick(us); }
void core_c_lfo_midi(int st, int d1, int d2) { core::lfo::midi_in((uint8_t)st, (uint8_t)d1, (uint8_t)d2); }
int core_c_lfo_command(const char* p, char* out, int cap) { return core::lfo::command(p, out, cap) ? 1 : 0; }
// rui1038: a console command for the GRANULAR / WAVETABLE / MULTISAMPLE machines and their library (nm::command:
// "lib=gr:fill:B:/Folder", "gr=pick:V:E", ...): the e2e tests' bank set-ups
int core_c_nm_command(const char* p, char* out, int cap) { return core::nm::command(p, out, cap) ? 1 : 0; }
int core_c_lfo_json(char* out, int cap) { return core::lfo::json(out, cap); }
void core_c_lfo_page(int sel, int design, int step) { core::ui::lfo_page_set(sel, design != 0, step, 0); }
void core_c_lfo_stats(uint32_t* out) {
  const core::lfo::Stats& s = core::lfo::stats();
  out[0] = s.ticks, out[1] = 0, out[2] = 0, out[3] = s.voice_sets, out[4] = s.user_calls, out[5] = s.fx_sets,   // 1, 2: the
  out[6] = s.restores;                                                                                         // CCs, gone
}
float core_c_lfo_out(int i) { return core::lfo::out(i); }
int core_c_rui_open() { return core::ui::remote_open() ? 1 : 0; }
void core_c_rui_request(int on) { core::ui::remote_request(on != 0); }
static core::ui::RemoteStatus rui_st = {};
void core_c_rui_status(int ob, uint32_t under, int c1, int c2, int c3) {
  core::ui::RemoteStatus& s = rui_st;
  s.ob = (int8_t)ob, s.underruns = under, s.cpu[0] = (int8_t)c1, s.cpu[1] = (int8_t)c2, s.cpu[2] = (int8_t)c3;
  core::ui::remote_status(s);
}
// 1021: the rest of the SIDECAR page's numbers (net 0 unknown, 1 down, 2 up; user "A,B,C,D,E" names)
void core_c_rui_status2(int rec, uint32_t rec_secs, int eth, int wifi, uint32_t ip, const char* users) {
  core::ui::RemoteStatus& s = rui_st;
  s.rec = (uint8_t)rec, s.rec_secs = rec_secs, s.eth = (uint8_t)eth, s.wifi = (uint8_t)wifi;
  for (int i = 0; i < 4; ++i) s.ip[i] = (uint8_t)(ip >> (24 - 8 * i));
  for (int u = 0; u < 5 && users && *users; ++u) {
    int n = 0;
    while (*users && *users != ',') { if (n < 8) s.user[u][n++] = *users; ++users; }
    s.user[u][n] = 0;
    if (*users == ',') ++users;
  }
  core::ui::remote_status(s);
}
void core_c_rui_legacy(int on) { core::ui::remote_legacy(on != 0); }
void core_c_rui_frame(uint8_t* fb) { memcpy(fb, core::ui::remote_frame(), core::ui::kBytes); }
// the last frame with its text slots drawn in the host's font (what the Rytm shows, but for the glyphs), and the text
// items as rows of 36 B (x, y, font, flags, 32 characters NUL padded)
void core_c_rui_frame_preview(uint8_t* fb) { core::ui::preview_sent(fb); }
int core_c_rui_frame_texts(uint8_t* out, int max) { return core::ui::sent_texts(out, max); }
void core_c_rui_reset() { core::ui::remote_reset(); }
// RemoteStats as words: opens closes frames tiles rects bytes keepalives keys encs edits widgets bad draw_us_max
// draw_us_sum page voice texts echoes split masks hellos splashes machines
int core_c_rui_stats(uint32_t* out, int n) {
  const core::ui::RemoteStats& s = core::ui::remote_stats();
  uint32_t w[23] = {s.opens, s.closes, s.frames, s.tiles, s.rects, s.bytes, s.keepalives, s.keys, s.encs, s.edits,
                    s.widgets, s.bad, s.draw_us_max, s.draw_us_sum, s.page, s.voice, s.texts, s.echoes, s.split,
                    s.masks, s.hellos, s.splashes, s.machines};
  int k = n < 23 ? n : 23;
  memcpy(out, w, 4 * k);
  return k;
}
int core_c_rui_render(int which, int voice, const uint8_t* vals, uint8_t* fb) {
  static core::ui::Canvas c;
  if (!core::ui::remote_render(which, voice, vals, c)) return 0;
  core::ui::preview(c, fb);   // the text items in the host's font (the Rytm draws its own)
  return 1;
}
void core_c_rui_bench(int which, int voice, int n, float* out2) { core::ui::remote_bench(which, voice, n, out2, out2 + 1); }
float core_c_voice_param(int vi, int k) { return core::v[vi & 7].p[k & 15]; }
// The remote UI's input tap (core/ui/extra.h) as the platform's IN path feeds it: n frames of `stride` int32 channels
// (the Rytm's 12 first); tap_on: a page reads it now.
void core_c_tap_in(const int32_t* f, int n, int stride) { core::ui::tap_in(f, n, stride); }
int core_c_tap_on() { return core::ui::tap_on() ? 1 : 0; }
// The REC page's recorder (core/ui/extra.h RecInfo; the Pi's rec_pi.cpp): its state, and the page's request taken
void core_c_rui_rec(int state, uint32_t secs, uint32_t free_mb, uint32_t total_mb, uint32_t need_kbps, int in_mask,
                    const char* take, const char* error) {
  core::ui::RecInfo i = {};
  i.state = (uint8_t)state, i.secs = secs, i.free_mb = free_mb, i.total_mb = total_mb, i.need_kbps = need_kbps;
  i.in_mask = (uint16_t)in_mask;
  snprintf(i.take, sizeof i.take, "%s", take ? take : "");
  snprintf(i.error, sizeof i.error, "%s", error ? error : "");
  core::ui::remote_rec(i);
}
int core_c_rui_rec_take() { return core::ui::remote_rec_take(); }
// The REC page's playback (core/ui/extra.h PlayInfo; the Pi's rec_play.h): its state, and the page's input taken
// (out: toggle, channel, take, level deltas; returns 1 when any)
void core_c_rui_play(int state, int level, int ch_i, int channels, int take_i, int takes, uint32_t pos_ms, uint32_t len_ms,
                     const char* take, const char* channel, const char* error) {
  core::ui::PlayInfo i = {};
  i.state = (uint8_t)state, i.level = (uint8_t)level, i.ch_i = (uint8_t)ch_i, i.channels = (uint8_t)channels;
  i.take_i = (uint8_t)take_i, i.takes = (uint8_t)takes, i.pos_ms = pos_ms, i.len_ms = len_ms;
  snprintf(i.take, sizeof i.take, "%s", take ? take : "");
  snprintf(i.channel, sizeof i.channel, "%s", channel ? channel : "");
  snprintf(i.error, sizeof i.error, "%s", error ? error : "");
  core::ui::remote_play(i);
}
int core_c_rui_play_take(int* out) {
  core::ui::PlayInput in;
  bool any = core::ui::remote_play_take(&in);
  out[0] = in.toggle, out[1] = in.channel, out[2] = in.take, out[3] = in.level;
  return any ? 1 : 0;
}
void core_c_fm_alg_render(int alg, uint8_t* fb) {   // 6-OP FM's algorithm diagram alone (core/ui/page_gfx.cpp)
  static core::ui::Canvas c;
  core::ui::fm_algorithm_render(alg, c);
  memcpy(fb, c.fb, core::ui::kBytes);
}

// Poly FM's chords (core/poly.h) for sim/tests/test_core_poly.py: expand into out (6 bytes), detect into r (root CHD INV
// SPR VOI exact), the value texts
int core_c_poly_expand(int root, int chd, int inv, int spr, int voi, uint8_t* out) {
  return core::poly::expand(root, {chd, inv, spr, voi}, out);
}
void core_c_poly_detect(const uint8_t* notes, int n, int* r) {
  core::poly::Detected d = core::poly::detect(notes, n);
  r[0] = d.root, r[1] = d.c.chd, r[2] = d.c.inv, r[3] = d.c.spr, r[4] = d.c.voi, r[5] = d.exact;
}
int core_c_poly_text(int k, int v, char* out, int cap) {
  char b[16];
  const char* t = core::poly::value_text(k, v, b, sizeof b);
  return snprintf(out, cap, "%s", t);
}

// Poly FM's live side (core/poly.h) for sim/poly_live.py: the Rytm's channel messages (its pads' notes), the tick
// (a held trig's time, a live chord's window) and the console's "poly" line.
void core_c_poly_midi(int st, int d1, int d2) {
  core::poly::midi((uint8_t)st, (uint8_t)d1, (uint8_t)d2);   // (the sequencer through its note sink, as the Pi's bridge)
}
void core_c_poly_tick(void) {
  core::poly::tick(core::now_ms());
  core::seq::tick(core::now_ms());   // (rytm_dump's tick with it)
  core::song::tick(core::now_ms());
}
int core_c_poly_command(const char* c, char* out, int cap) {
  return core::poly::command(c, out, cap) || core::seq::command(c, out, cap) || core::rytm_dump::command(c, out, cap) ||
         core::song::command(c, out, cap) || core::inst::command(c, out, cap) ? 1 : 0;
}
// The Sidecar sequencer (core/seq/seq.h) for sim/seq_sim.py and poly_live.py: its console, its LEDs (16 trig + 4 page,
// palette), the render frames so far, the view set (track, page, held trig or -1, follow).
int core_c_seq_command(const char* c, char* out, int cap) { return core::seq::command(c, out, cap) ? 1 : 0; }
int core_c_seq_leds(uint8_t* out20) {
  int on = core::seq::leds(out20) ? 1 : 0;
  core::seq::page_leds(out20 + 16);
  return on;
}
uint64_t core_c_seq_frames() { return core::seq::frames(); }
void core_c_seq_view_set(int track, int page, int held, int follow) { core::seq::view_set(track, page, held, follow != 0); }
void core_c_seq_latency(int frames) { core::seq::set_latency(frames); }
int core_c_seq_pack(uint8_t* out, int cap) { return core::seq::pack(core::seq::edit_pattern(), out, cap); }
// seq-p2: the Rytm -> host Overbridge frames (12 int32 channels each: MAIN L/R, the 8 voices, AUDIO IN L/R; arrived
// just now) for the latency calibration, and the Rytm's MIDI bytes for its pattern dump (core/seq/rytm_dump.h).
void core_c_seq_rytm_audio(const int32_t* frames12, int n) {
  core::seq::cal_audio(frames12 + 2, n, 12, (int64_t)core::seq::frames());
}
int core_c_seq_cal_wanted(void) { return core::seq::cal_wanted() ? 1 : 0; }
void core_c_rytm_byte(int b) {
  core::rytm_dump::byte((uint8_t)b);
  core::song::byte((uint8_t)b);   // SONG RECORD (core/song/song.h): the whole stream
}
// The last pattern dump's record (13101 B) into out: its pattern, -1 none; gen: the count that moves with each.
int core_c_rytm_record(uint8_t* out, uint32_t* gen) {
  int p = -1;
  const uint8_t* r = core::rytm_dump::record(&p, gen);
  if (!r) return -1;
  memcpy(out, r, core::rytm_dump::kRecord);
  return p;
}

// MULTISAMPLE (core/msamp/msamp.h) and the SD card test (msamp/sdtest.h) for sim/poly_live.py: the console's msamp /
// sdtest commands (msamp=voice:V makes voice V play MULTISAMPLE), LIVE IN's (livein=...), the INSERT chains' (ins=...)
// and the OUT route's (out=...), the answer in out; 0 not one of them.
int core_c_msamp_command(const char* c, char* out, int cap) {
  return core::msamp::command(c, out, cap) || core::nm::command(c, out, cap) || core::sdtest::command(c, false, out, cap) ||
                 core::livein::command(c, out, cap) || core::ins::command(c, out, cap) || core::out::command(c, out, cap)
             ? 1
             : 0;
}
// LIVE IN (core/livein/livein.h) for sim/poly_live.py: the Rytm's frames (n x 12 int32, the device's channels: AUDIO IN
// among them) into its ring, as the Pi's IN completion does; the output FIFO the link holds (the latency estimate)
void core_c_livein_in(const int32_t* frames12, int n) { core::livein::in_block(frames12, n, 12); }
void core_c_livein_fifo(int frames) { core::livein::set_fifo(frames); }

// Project bundles (core/bundle) as the Pi's core 0 runs them, for sim/rui_link.py (the Rytm's PROJECT page end to
// end): the job's environment (full rate, the Rytm's current project from core::project), then per call `passes`
// rounds of the feed (the page's commands, the lists, the published status) and one bundle step; the control core's
// project index save. Returns the job's phase.
static void bundle_current(char* out, int cap) {
  char p[24];
  memcpy(p, (const char*)core::project, sizeof p);
  p[23] = 0;
  snprintf(out, cap, "%s", p);
}
void core_c_bundle_setup() {
  core::bundle::Env e{};
  e.current = bundle_current;
  e.version = "mac";
  e.board = "mac";
  e.flush_ms = 0;
  core::bundle::set_env(e);
  core::bundle::feed::set_available(true);
}
int core_c_bundle_tick(int passes) {
  for (int i = 0; i < passes; ++i) {
    core::bundle::feed::tick();
    core::bundle::step();
    core::projects::save();
  }
  return core::bundle::status().phase;
}
int core_c_bundle_command(const char* c, char* out, int cap) { return core::bundle::feed::command(c, out, cap) ? 1 : 0; }
const char* core_c_bundle_msg() { return core::bundle::status().msg; }

}  // extern "C"

