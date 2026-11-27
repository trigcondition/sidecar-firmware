// tools/sidecar_emu.cpp - the Sidecar emulator: the engine on a Mac, as the other end of analog-emu's Overbridge link (--ob-link, the link the Pi's
// link=tcp speaks, pi/engine/tcp_link.h, emuhost= on a Pi), so a box in the emulator runs against the real engine with no Pi. The real core, its device profiles
// and the Pi's real output stage (pi/engine/output.cpp); one thread instead of the Pi's cores, on the emulated box's clock: each N the emulator asks with is
// answered from the voices, and time moves by the frames sent, so a run does not depend on how fast the Mac is. What Circle and the driver do is not here.
//   make -C core sidecar-emu, then: core/build/sidecar_emu --link 127.0.0.1:7301 --card DIR [--out-wav F] [--in-wav F] [--seconds S]
// --out-wav: what went to the box, its channels; --in-wav: what the box sent, its channels, both 32-bit WAV. A hello that asks for another state folder restarts
// this program, once, as it restarts a Pi. Stats on stdout; `quit` on the link's close.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "core.h"
#include "device/a4.h"
#include "device/device.h"
#include "fx/out.h"
#include "inst.h"
#include "keys.h"
#include "output.h"
#include "poly.h"
#include "seq/seq.h"
#include "ui/remote.h"

using namespace core;

namespace {

constexpr int kRate = 48000;
enum { HelloMidi = 1 };

struct Opts {
  std::string link = "127.0.0.1:7301", card, out_wav, in_wav;
  double seconds = 0;
  struct Trig { double at, len; int voice, note; bool on, off; };
  std::vector<Trig> trigs;
  double all_off = -1;
  double pair_at = -1;     // --pair-at: the pairing popup at this time, a made-up test code, never a real one
  bool flood = false;
  std::string frame;       // --frame-out: the last frame the screen was sent, 1024 B, at the end
} o;
char** g_argv;
int g_fd = -1;
Output g_out;
uint64_t g_frames, g_mach[kMachines];
uint32_t g_hellos, g_ok, g_trigs, g_params, g_offs, g_notes, g_seq, g_in_ch;
double g_peak_out[Output::kChannels], g_peak_in[12];
bool g_restart;

// ---- what the box said
void take(const uint8_t* m, int n) {
  if (n >= 7 && m[1] == 0x7D && m[2] == 0x52 && m[3] == 0x54) {
    int c = m[4];
    if (c == device::kCmdHello) ++g_hellos;
    else if (c == 0x04) ++g_trigs;
    else if (c == 0x03) ++g_params;
    else if (c == 0x11) ++g_offs;
  }
  engine_sysex(m, n);
}

std::vector<uint8_t> g_msg;
void midi_in(const uint8_t* d, int n) {
  for (int k = 0; k < n; ++k) {
    uint8_t b = d[k];
    if (b >= 0xF8) continue;
    if (b == 0xF0) g_msg.assign(1, b);
    else if (g_msg.empty()) {
      if (b >= 0x80 && b < 0xF0 && (b & 0xF0) == 0x90) ++g_notes;
    } else if (b == 0xF7) g_msg.push_back(b), take(g_msg.data(), (int)g_msg.size()), g_msg.clear();
    else if (b & 0x80) g_msg.clear();
    else g_msg.push_back(b);
  }
}

uint64_t g_screen_bytes, g_screen_msgs, g_screen_commits;
std::vector<uint8_t> g_tx;
void engine_out(const uint8_t* m, int n) {   // through the profile's filter
  if (n >= 9 && m[4] == device::kCmdHello && m[8] == device::kOk) ++g_ok;
  else g_screen_bytes += (uint64_t)n, ++g_screen_msgs, g_screen_commits += n > 4 && m[4] == 0x39;
  g_tx.push_back('M'), g_tx.push_back((uint8_t)n), g_tx.push_back((uint8_t)(n >> 8));
  g_tx.insert(g_tx.end(), m, m + n);
}
// After the block it answered: a page's frames do not hold up the audio on the one socket, as they do not on the Pi, where each has its own endpoint
void tx_flush() {
  const uint8_t* p = g_tx.data();
  size_t left = g_tx.size();
  while (left) {
    ssize_t w = write(g_fd, p, left);
    if (w <= 0) break;
    p += w, left -= (size_t)w;
  }
  g_tx.clear();
}

// ---- WAV, 32 bit, the header written at the end
struct Wav {
  FILE* f = nullptr;
  uint32_t bytes = 0;
  int ch = 0;
  void open(const std::string& p, int c) {
    if (p.empty()) return;
    f = fopen(p.c_str(), "wb");
    ch = c;
    if (f) fseek(f, 44, SEEK_SET);
  }
  void put(const int32_t* s, int n) {
    if (f) fwrite(s, 4, n, f), bytes += 4 * n;
  }
  void close() {
    if (!f) return;
    uint32_t v;
    uint16_t h;
    auto u32 = [&](uint32_t x) { v = x, fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t x) { h = x, fwrite(&h, 2, 1, f); };
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f), u32(36 + bytes), fwrite("WAVEfmt ", 1, 8, f), u32(16), u16(1), u16(ch), u32(kRate), u32(kRate * 4 * ch), u16(4 * ch), u16(32);
    fwrite("data", 1, 4, f), u32(bytes);
    fclose(f);
    f = nullptr;
  }
} g_wo, g_wi;

// ---- the voices: one block into the output stage, as the Pi's control and render cores do it
// A MIDI message for the box, as the Pi's keys forward queue sends it on the host link
void box_midi(uint8_t st, uint8_t a, uint8_t b) {
  printf("sidecar_emu: to the box %02x %02x %02x at %.3f\n", st, a, b, (double)g_frames / kRate);
  uint8_t m[6] = {'M', 3, 0, st, a, b};
  if (write(g_fd, m, 6) != 6) return;
}

// --trig: the sequencer's way into a voice (core/inst.h seq_on), at a time on the box's clock
void trigs_due() {
  double t = (double)g_frames / kRate;
  if (o.all_off >= 0 && t >= o.all_off) {   // the sequencer's stop: seq.cpp release() on every voice
    o.all_off = -1;
    for (int v = 0; v < kVoices; ++v) inst::seq_off(v, -1);
    for (auto& x : o.trigs) x.off = true;
  }
  for (auto& x : o.trigs) {
    seq::PolyCfg cfg = {};
    if (!x.on && t >= x.at) x.on = true, inst::seq_on(x.voice, x.note, 100, cfg), ++g_seq;
    if (!x.off && t >= x.at + x.len) x.off = true, inst::seq_off(x.voice, x.note);
  }
}

void render_block() {
  static int16_t l[kBlock][kVoices], r[kBlock][kVoices];
  uint8_t st[kVoices] = {}, bits = 0;
  refill_neediest(4 * kBlock);
  seq::pre_block_all();   // (one pass for every voice: every slot's sequencer)
  render_voices(l, kBlock, 0, 0, kVoices, g_mach, r, st);
  for (int v = 0; v < kVoices; ++v) bits |= st[v] ? 1 << v : 0;
  g_out.Push(l, kBlock, r, bits);
}

void control_tick() {
  host_fake_time(true, 1000000 + g_frames * 1000000 / kRate);
  trigs_due();
  lfo::tick(now_us());
  device::tick(now_us());
  poly::tick(now_ms());
  seq::tick(now_ms());
  if (!idle_prepare() && !idle_save()) idle_stream();
}

// What the Pi's control core does for the screen: the status once a second, the tick each pass. The numbers are a made-up unit's.
void screen_tick() {
  static uint32_t said = 0;
  uint32_t ms = (uint32_t)(g_frames * 1000 / kRate);
  if (ms - said >= 1000 || !said) {
    said = ms | 1;
    ui::RemoteStatus st = {};
    st.ob = 1, st.rec = 1, st.eth = 2, st.ip[0] = 10, st.ip[3] = 2;
    for (int k = 0; k < 3; ++k) st.cpu[k] = 10 + k;
    st.underruns = g_out.Underflows();
    ui::remote_status(st);
  }
  if (o.pair_at >= 0 && (double)g_frames / kRate >= o.pair_at) {
    o.pair_at = -1;
    ui::remote_pair_code("9173 2046");
    ui::remote_popup("SIDECAR PAIR CODE", "9173 2046", ui::kPopupMaxMs, true);
  }
  ui::remote_tick(ms);
  static uint32_t forced = 0;   // --flood: a whole frame each 25 ms while a page is open, as many bytes as the screen can ever ask of the link
  if (o.flood && ui::remote_open() && ms - forced >= 25) forced = ms, ui::remote_resync();
}

void pull(uint32_t n) {
  static std::vector<int32_t> buf;
  buf.resize(1 + n * Output::kChannels);
  int32_t(*f)[Output::kChannels] = (int32_t(*)[Output::kChannels])buf.data();
  for (uint32_t done = 0; done < n;) {
    uint32_t k = n - done > 32 ? 32 : n - done;
    control_tick();
    while (g_out.Level() < k + 2 * kBlock) render_block();
    g_out.Pull(f + done, k);
    done += k, g_frames += k;
    screen_tick();
  }
  const int ch = out::device().channels;
  for (uint32_t i = 0; i < n; ++i) {
    g_wo.put(f[i], ch);
    for (int c = 0; c < ch; ++c) {
      double a = abs((double)f[i][c]) / 2147483648.0;
      if (a > g_peak_out[c]) g_peak_out[c] = a;
    }
  }
  uint8_t h[5] = {'A', (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
  if (write(g_fd, h, 5) != 5) return;
  const uint8_t* p = (const uint8_t*)f;
  size_t left = n * Output::kChannels * 4;
  while (left) {
    ssize_t w = write(g_fd, p, left);
    if (w <= 0) return;
    p += w, left -= (size_t)w;
  }
  tx_flush();
}

// ---- the link, rytm-emu src/oblink.rs: host side
int connect_link() {
  auto c = o.link.rfind(':');
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)atoi(o.link.c_str() + c + 1));
  inet_pton(AF_INET, o.link.substr(0, c).c_str(), &a.sin_addr);
  for (int k = 0; k < 600; ++k) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, (sockaddr*)&a, sizeof a) == 0) {
      int one = 1;
      setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      return s;
    }
    close(s);
    usleep(100000);
  }
  return -1;
}

void stats(const char* why) {
  const out::Device& d = out::device();
  printf("sidecar_emu: %s: %s, %.2f s, %u hellos (%u accepted), %u trigs, %u params, %u note-offs, %u notes, under %u, dropped %u\n", why, device::active().name,
         (double)g_frames / kRate, g_hellos, g_ok, g_trigs, g_params, g_offs, g_notes, g_out.Underflows(), device::dropped());
  printf("sidecar_emu: OUT peak by channel:");
  for (int c = 0; c < d.channels; ++c) printf(" %.3f", g_peak_out[c]);
  printf("\nsidecar_emu: IN peak by channel:");
  for (uint32_t c = 0; c < g_in_ch; ++c) printf(" %.3f", g_peak_in[c]);
  printf("\n");
  fflush(stdout);
}

void run() {
  uint8_t hello[3] = {'H', 1, HelloMidi};
  if (write(g_fd, hello, 3) != 3) return;
  std::vector<uint8_t> b;
  size_t at = 0;
  uint8_t chunk[1 << 16];
  uint64_t shown = 0;
  for (;;) {
    ssize_t got = read(g_fd, chunk, sizeof chunk);
    if (got <= 0) return;
    b.insert(b.end(), chunk, chunk + got);
    for (;;) {
      size_t have = b.size() - at;
      if (have < 1) break;
      const uint8_t* p = b.data() + at;
      uint32_t need;
      switch (p[0]) {
        case 'N': need = 5; break;
        case 'H': need = 3; break;
        case 'T': need = 4; break;
        case 'P': need = 10; break;
        case 'M': if (have < 3) { need = ~0u; break; } need = 3 + (p[1] | p[2] << 8); break;
        case 'R': if (have < 5) { need = ~0u; break; } need = 5 + 48 * (p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24); break;
        default: fprintf(stderr, "sidecar_emu: a byte 0x%02x starts no message\n", p[0]); return;
      }
      if (need == ~0u || have < need) break;
      if (p[0] == 'N') pull(p[1] | p[2] << 8 | p[3] << 16 | (uint32_t)p[4] << 24);
      else if (p[0] == 'M') midi_in(p + 3, need - 3), tx_flush();
      else if (p[0] == 'R') {
        uint32_t n = (need - 5) / 48;
        const int32_t* s = (const int32_t*)(p + 5);
        int ic = out::device().in_channels;
        g_in_ch = (uint32_t)ic;
        for (uint32_t i = 0; i < n; ++i, s += 12) {
          g_wi.put(s, ic);
          for (int c = 0; c < ic; ++c) {
            double a = abs((double)s[c]) / 2147483648.0;
            if (a > g_peak_in[c]) g_peak_in[c] = a;
          }
        }
      }
      at += need;
      if (g_restart) return;
    }
    if (at > (1u << 20)) b.erase(b.begin(), b.begin() + (long)at), at = 0;
    if (g_frames - shown >= 5 * kRate) shown = g_frames, stats("run");
    if (o.seconds && (double)g_frames / kRate >= o.seconds) return;
  }
}

void on_restart() { g_restart = true; }

}  // namespace

int main(int argc, char** argv) {
  g_argv = argv;
  for (int k = 1; k < argc; ++k) {
    std::string a = argv[k];
    auto next = [&]() -> const char* {
      if (++k >= argc) exit(2);
      return argv[k];
    };
    if (a == "--link") o.link = next();
    else if (a == "--card") o.card = next();
    else if (a == "--out-wav") o.out_wav = next();
    else if (a == "--in-wav") o.in_wav = next();
    else if (a == "--seconds") o.seconds = atof(next());
    else if (a == "--all-off") o.all_off = atof(next());   // AT: every voice's notes off, as a stop
    else if (a == "--pair-at") o.pair_at = atof(next());
    else if (a == "--frame-out") o.frame = next();
    else if (a == "--flood") o.flood = true;
    else if (a == "--trig") {   // AT:LEN:VOICE:NOTE, seconds of emulated time (after the restart: it starts at 0 again)
      Opts::Trig t = {};
      if (sscanf(next(), "%lf:%lf:%d:%d", &t.at, &t.len, &t.voice, &t.note) != 4) return 2;
      o.trigs.push_back(t);
    }
    else return fprintf(stderr, "sidecar_emu --link HOST:PORT --card DIR [--out-wav F] [--in-wav F] [--seconds S] [--trig AT:LEN:VOICE:NOTE] [--all-off AT]\n"), 2;
  }
  if (o.card.empty()) return fprintf(stderr, "sidecar_emu: --card DIR\n"), 2;
  signal(SIGPIPE, SIG_IGN);
  for (const char* d : {"", "/Samples", "/.rytm", "/.sidecar"}) mkdir((o.card + d).c_str(), 0755);
  host_set_card(o.card.c_str());
  host_fake_time(true, 1000000);
  hooks.to_rytm = engine_out;
  keys::set_forward(box_midi);
  hooks.device_restart = on_restart;
  if (!engine_begin()) return fprintf(stderr, "sidecar_emu: the engine did not start on %s\n", o.card.c_str()), 1;
  for (int k = 0; k < 2000 && (idle_prepare() || idle_save()); ++k) {
  }
  const device::Profile* box = device::by_usb(device::kA4Vid, device::kA4Pid);   // as the Pi's driver at a USB claim (core/device/device.cpp: after its hello)
  if (box) out::set_device(box->audio);
  g_out.SetOutControl(&out::control);
  g_out.SetMode(Output::kTracks);
  g_out.SetFifoTarget(256);
  g_out.Reset();
  g_wo.open(o.out_wav, out::device().channels);
  g_wi.open(o.in_wav, out::device().in_channels);
  printf("sidecar_emu: %s, state in %s, link %s\n", device::active().name, device::g_link.state ? device::g_link.state : "/.rytm", o.link.c_str());
  fflush(stdout);
  g_fd = connect_link();
  if (g_fd < 0) return fprintf(stderr, "sidecar_emu: no link at %s\n", o.link.c_str()), 1;
  run();
  stats(g_restart ? "restart" : "end");
  printf("sidecar_emu: screen %llu B in %llu messages, %llu commits, opens %lu, keys %lu, turns %lu, bad %lu\n", (unsigned long long)g_screen_bytes, (unsigned long long)g_screen_msgs,
         (unsigned long long)g_screen_commits, (unsigned long)ui::remote_stats().opens, (unsigned long)ui::remote_stats().keys, (unsigned long)ui::remote_stats().encs, (unsigned long)ui::remote_stats().bad);
  if (!o.frame.empty()) {
    if (FILE* f = fopen(o.frame.c_str(), "wb")) fwrite(ui::remote_frame(), 1, 1024, f), fclose(f);
  }
  g_wo.close(), g_wi.close();
  close(g_fd);
  if (g_restart) {
    for (int k = 0; k < 200; ++k) {   // the choice is on the card by now (device::step), the rest of the saves too
      idle_prepare() || idle_save();
    }
    execv(argv[0], argv);
    perror("sidecar_emu: restart");
    return 1;
  }
  return 0;
}
