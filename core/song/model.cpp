// core/song/model.cpp - see song.h: events, the MIDI CONFIG, the dumps, base64, the buffer, the take, the clock and the
// recorder. Pure (no card, no MIDI out); cold.
#include "song/song.h"

#if CORE_POLY
#include <stdarg.h>
#include <string.h>

#include "platform.h"

namespace core {
namespace song {

namespace {
const char* const kTypeNames[kTypes] = {"", "pc", "sw", "p", "cc", "mute", "scene", "perf", "tempo", "play", "stop",
                                        "pause", "cont", "sc", "mach", "fill", "mark", "src"};
}

CORE_COLD const char* type_name(int type) { return type > 0 && type < kTypes ? kTypeNames[type] : "?"; }
CORE_COLD int type_of(const char* name, int n) {
  for (int k = 1; k < kTypes; ++k)
    if ((int)strlen(kTypeNames[k]) == n && !strncmp(kTypeNames[k], name, n)) return k;
  return kNone;
}

// ---- the MIDI CONFIG
CORE_COLD void config_default(MidiConfig* c) {
  memset(c, 0, sizeof *c);
  for (int t = 0; t < kTracks; ++t) c->track_ch[t] = (uint8_t)(t + 1);
  c->fx_ch = 13, c->auto_ch = 14;
  c->nrpn = 0, c->transport_send = 1, c->transport_receive = 1;
}

CORE_COLD bool parse_global(const uint8_t* u, int n, MidiConfig* c) {
  uint8_t auto_ch = c->auto_ch ? c->auto_ch : 14;
  config_default(c);
  c->auto_ch = auto_ch;
  if (n < 47) return false;
  c->nrpn = u[32] == 0;
  c->clock_send = u[34] != 0, c->transport_send = u[36] != 0, c->pc_receive = u[37] != 0, c->pc_send = u[38] != 0;
  c->transport_receive = u[35] != 0;   // fix1041 [inferred offset: song.h]
  c->encoder_ext = u[44] != 0, c->trk_ch = u[46] != 0;
  for (int t = 0; t < kTracks; ++t) c->track_ch[t] = u[12 + t] < 16 ? (uint8_t)(u[12 + t] + 1) : 0;
  c->fx_ch = u[24] < 16 ? (uint8_t)(u[24] + 1) : 0;
  c->valid = true;
  return true;
}

CORE_COLD uint32_t config_problems(const MidiConfig& c, bool replay) {
  if (!c.valid) return kCfgUnknown;
  uint32_t p = 0;
  if (!replay) {
    if (!c.encoder_ext) p |= kCfgEncoder;
    if (!c.trk_ch) p |= kCfgTrkCh;
    if (!c.nrpn) p |= kCfgNrpn;
  } else if (!c.pc_receive) {
    p |= kCfgPcReceive;
  }
  return p;
}

CORE_COLD int config_text(uint32_t p, char* out, int cap) {
  int k = 0;
  out[0] = 0;
  auto add = [&](const char* s) { k += sniprintf(out + k, cap - k > 0 ? cap - k : 0, "%s%s", k ? ", " : "", s); };
  if (p & kCfgUnknown) add("MIDI CONFIG NOT READ");
  if (p & kCfgEncoder) add("ENCODER DEST INT");
  if (p & kCfgTrkCh) add("OUTPUT CH AUTO");
  if (p & kCfgNrpn) add("PARAM OUTPUT CC");
  if (p & kCfgPcReceive) add("PROG CH RECEIVE OFF");
  return k < cap ? k : cap - 1;
}

CORE_COLD const char* config_row(int k) {
  static const char* const r[kCfgRows] = {"PORT: ENCODER DEST = INT+EXT", "PORT: OUTPUT CH = TRK CH", "PORT: PARAM OUTPUT = NRPN",
                                          "SYNC: PROG CH RECEIVE = ON",   "SYNC: PROG CH SEND = ON",  "SYNC: TRANSPORT SEND = ON"};
  return k >= 0 && k < kCfgRows ? r[k] : nullptr;
}

CORE_COLD int config_wrong(const MidiConfig& c) {
  if (!c.valid) return -1;
  return (c.encoder_ext ? 0 : 1) | (c.trk_ch ? 0 : 2) | (c.nrpn ? 0 : 4) | (c.pc_receive ? 0 : 8) | (c.pc_send ? 0 : 16) |
         (c.transport_send ? 0 : 32);
}

CORE_COLD const char* config_explain(int line) {
  static const char* const k[] = {   // (25 characters at most: the Rytm's screen)
      "SONG REC NEEDS MIDI OUT;",
      "SET ON THE RYTM (NOT US):",
      "GLOBAL>MIDI CFG>PORT CFG",
      " ENCODER DEST: INT+EXT",
      " OUTPUT CH: TRK CH",
      " PARAM OUTPUT: NRPN",
      "GLOBAL>MIDI CFG>SYNC",
      " PROG CH RECEIVE: ON",
      " PROG CH SEND: ON",
  };
  return line >= 0 && line < (int)(sizeof k / sizeof k[0]) ? k[line] : nullptr;
}

// ---- dumps
CORE_COLD int dump_request(uint8_t out[15], uint8_t id) {
  static const uint8_t k[15] = {0xF0, 0x00, 0x20, 0x3C, 0x07, 0x00, 0, 0x01, 0x01, 0, 0, 0, 0, 0x05, 0xF7};
  memcpy(out, k, 15);
  out[6] = id;
  return 15;
}

CORE_COLD bool dump_ok(const uint8_t* m, int n, uint8_t* id) {
  if (n < 15 || m[0] != 0xF0 || m[1] != 0 || m[2] != 0x20 || m[3] != 0x3C || m[4] != 0x07 || m[n - 1] != 0xF7) return false;
  uint32_t sum = 0;
  for (int i = 10; i < n - 5; ++i) sum += m[i];
  int ck = m[n - 5] << 7 | m[n - 4], len = m[n - 3] << 7 | m[n - 2];
  // the sum from byte 9 or from byte 10: the Rytm leaves its object number out of it (core/seq/rytm_dump.h [measured on
  // hardware]; with object 0 the two agree) - an answer after a switch to A02 carried 1 there [rytm-emu]
  if (((int)(sum & 0x3FFF) != ck && (int)((sum + m[9]) & 0x3FFF) != ck) || len != n - 10) return false;
  if (id) *id = m[6];
  return true;
}

CORE_COLD int unpack7(const uint8_t* p, int n, uint8_t* out, int cap) {
  int o = 0;
  for (int i = 0; i < n; i += 8) {
    uint8_t hi = p[i];
    for (int k = 1; k < 8 && i + k < n; ++k) {
      if (o >= cap) return o;
      out[o++] = (uint8_t)(p[i + k] | ((hi >> (7 - k)) & 1) << 7);
    }
  }
  return o;
}

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
int b64v(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
}  // namespace

CORE_COLD int b64_len(int n) { return (n + 2) / 3 * 4; }
CORE_COLD int b64_encode(const uint8_t* d, int n, char* out, int cap) {
  int o = 0;
  for (int i = 0; i < n; i += 3) {
    if (o + 4 > cap) return -1;
    uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out[o++] = kB64[v >> 18 & 63], out[o++] = kB64[v >> 12 & 63];
    out[o++] = i + 1 < n ? kB64[v >> 6 & 63] : '=';
    out[o++] = i + 2 < n ? kB64[v & 63] : '=';
  }
  return o;
}
CORE_COLD int b64_decode(const char* s, int n, uint8_t* out, int cap) {
  int o = 0;
  uint32_t acc = 0;
  int bits = 0;
  for (int i = 0; i < n; ++i) {
    if (s[i] == '=') break;
    int v = b64v(s[i]);
    if (v < 0) return -1;
    acc = acc << 6 | (uint32_t)v, bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o >= cap) return -1;
      out[o++] = (uint8_t)(acc >> bits);
    }
  }
  return o;
}

// ---- Buf
CORE_COLD bool Buf::reserve(int c) {
  if (c <= cap) return true;
  int nc = cap ? cap : 256;
  while (nc < c) nc *= 2;
  uint8_t* q = new uint8_t[nc];
  if (!q) return false;
  if (n) memcpy(q, p, n);
  delete[] p;
  p = q, cap = nc;
  return true;
}
CORE_COLD bool Buf::add(const void* d, int k) {
  if (k <= 0) return true;
  if (!reserve(n + k + 1)) return false;
  memcpy(p + n, d, k);
  n += k;
  p[n] = 0;
  return true;
}
CORE_COLD bool Buf::adds(const char* s) { return add(s, (int)strlen(s)); }
CORE_COLD bool Buf::addf(const char* fmt, ...) {
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  int k = vsniprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  return add(b, k < (int)sizeof b ? k : (int)sizeof b - 1);
}
CORE_COLD void Buf::free() {
  delete[] p;
  p = nullptr, n = cap = 0;
}

// ---- Take
CORE_COLD void Take::clear() {
  project[0] = name[0] = 0;
  config_default(&cfg);
  pattern = -1, tempo = 0, mutes = 0, scene = 0;
  kit.clear(), pat.clear(), settings.clear(), global.clear(), kit2.clear();
  has_fx = false;
  memset(fx_send, 0, sizeof fx_send), memset(fx_knob, 0, sizeof fx_knob), fx_route = 0;
  n = 0, end = 0, recovered = false;
}
CORE_COLD bool Take::push(const Ev& e) {
  if (n >= cap) {
    if (cap >= kMaxEvents) return false;
    int nc = cap ? cap * 2 : 1024;
    if (nc > kMaxEvents) nc = kMaxEvents;
    Ev* q = new Ev[nc];
    if (!q) return false;
    if (n) memcpy(q, ev, sizeof(Ev) * n);
    delete[] ev;
    ev = q, cap = nc;
  }
  ev[n++] = e;
  return true;
}
CORE_COLD void Take::free() {
  delete[] ev;
  ev = nullptr, n = cap = 0;
  kit.free(), pat.free(), settings.free(), global.free(), kit2.free();
}

// ---- Clock
CORE_COLD void Clock::start(double frame) {
  running = true, ticked = false;
  t = 0, frozen = 0, f = frame, last_step = -1, switched = false, f8s = 0;
}
CORE_COLD void Clock::cont(double frame) {
  running = true;
  f = frame;   // t goes on from the last tick (at() never below the frozen t)
}
CORE_COLD void Clock::pause(double frame) {
  frozen = at(frame);
  running = false;
}
CORE_COLD void Clock::stop() { running = false; }
CORE_COLD void Clock::tempo(double bpm) {
  if (bpm > 1) period = 48000.0 * 60.0 / bpm / 4.0;
}
CORE_COLD void Clock::tick(int step, int ln, int speed, double frame) {
  static const double kMult[7] = {2.0, 1.5, 1.0, 0.75, 0.5, 0.25, 0.125};
  double m = speed >= 0 && speed < 7 ? kMult[speed] : 1.0;
  double ninc = kPpq / m;
  if (!ticked) {   // the first tick after PLAY: t 0 (step 0 fires at PLAY)
    ticked = true;
  } else {
    int adv;
    if (switched && step == 0) adv = 1;
    else {
      int L = ln < 1 ? 1 : ln;
      adv = ((step - last_step) % L + L) % L;
      if (adv == 0) adv = L == 1 ? 1 : 0;
    }
    double measured = adv > 0 ? (frame - f) / adv : 0;
    if (adv > 0 && measured > 50 && measured < 200000 && !(period > 0))
      period = measured * m;   // no tempo reported yet: the ticks give it
    t += (uint32_t)(adv * inc + 0.5);
  }
  switched = false;
  inc = ninc, last_step = step, len = ln, f = frame;
  running = true;
}
CORE_COLD void Clock::f8(double frame) {
  if (ticked || !running) return;
  if (f8s > 0) {
    double d = frame - f;
    if (d > 10 && d < 48000) period = period > 0 ? period * 0.8 + d * 6 * 0.2 : d * 6;
    t += kPpq / 6;
  }
  ++f8s;
  f = frame;
}
CORE_COLD uint32_t Clock::at(double frame) const {
  if (!running) return frozen > t ? frozen : t;
  double step = ticked ? inc : kPpq / 6.0;
  uint32_t r = t;
  if (period > 0 && frame > f) {
    double dt = (frame - f) / period * kPpq;
    if (dt > step - 1) dt = step - 1;   // never past the next tick (it re-anchors)
    r = t + (uint32_t)dt;
  }
  return r > frozen ? r : frozen;
}

// ---- Recorder
CORE_COLD void Recorder::arm(Take* t) {
  take = t;
  armed = true, on = false;
  clk = Clock();
  for (Nrpn& x : nrpn) x = Nrpn();
  for (int i = 0; i < kThin; ++i) thin_key[i] = 0, thin_idx[i] = -1;
  last_pc = -1, last_pc_t = 0xFFFFFFFFu, sw_pending = -1;
  stats_dropped = stats_thinned = 0;
}

CORE_COLD int Recorder::thin_find(uint32_t key, bool make) {
  uint32_t h = (key * 2654435761u) >> 21;   // 11 bits
  for (int k = 0; k < kThin; ++k) {
    int i = (int)((h + k) & (kThin - 1));
    if (thin_idx[i] < 0) {
      if (!make) return -1;
      thin_key[i] = key;
      return i;
    }
    if (thin_key[i] == key) return i;
  }
  return -1;
}

// An event at the clock's t; thin: a parameter (kParam / kCc / kPerf): the same value again dropped, a second one on the
// same tick replaces the first
CORE_COLD bool Recorder::add(uint8_t type, uint8_t a, uint16_t b, uint32_t v, double f, uint32_t ms, bool thin) {
  if (!take || (!on && !armed)) return false;
  Ev e;
  e.t = on ? clk.at(f) : 0;
  e.type = type, e.a = a, e.b = b, e.v = v;
  e.ms = on ? ms - ms0 : 0;
  if (thin) {
    uint32_t key = (uint32_t)type << 24 | (uint32_t)a << 16 | b;
    int h = thin_find(key, true);
    if (h >= 0 && thin_idx[h] >= 0 && thin_idx[h] < take->n) {
      Ev& last = take->ev[thin_idx[h]];
      if (last.v == v) {
        ++stats_thinned;
        return true;
      }
      if (last.t == e.t) {
        last.v = v;
        ++stats_thinned;
        return true;
      }
    }
    if (!take->push(e)) {
      ++stats_dropped;
      return false;
    }
    if (h >= 0) thin_idx[h] = take->n - 1;
    return true;
  }
  if (!take->push(e)) {
    ++stats_dropped;
    return false;
  }
  return true;
}

CORE_COLD void Recorder::play(double f, uint32_t ms) {
  if (!take) return;
  clk.start(f);
  ms0 = ms;
  on = true, armed = false;
  take->pattern = pattern, take->mutes = mutes;
  add(kPlay, 0, 0, 0, f, ms);
}
CORE_COLD void Recorder::cont(double f, uint32_t ms) {
  if (!on) return;
  clk.cont(f);
  add(kCont, 0, 0, 0, f, ms);
}
CORE_COLD void Recorder::pause(double f, uint32_t ms) {
  if (!on) return;
  add(kPause, 0, 0, 0, f, ms);
  clk.pause(f);
}
CORE_COLD void Recorder::stop(double f, uint32_t ms) {
  if (!on) return;
  uint32_t t = clk.at(f);
  add(kStop, 0, 0, 0, f, ms);
  take->end = t;
  clk.stop();
  on = false;
}

// A channel message: NRPN (99 98 6 38) per channel as one kParam (on 38; a CC 6 with no 38 after it before the next
// NRPN number: on that), CC 94 mutes, CC 92 scenes, CC 35..47 on the auto channel PERF, other CCs as they are, program
// changes on the auto channel as queued patterns. Notes are not recorded (docs/SONG.md: the pads are the patterns').
CORE_COLD void Recorder::channel(uint8_t st, uint8_t d1, uint8_t d2, double f, uint32_t ms) {
  if (!take) return;
  int ch = st & 15, kind = st & 0xF0;
  const MidiConfig& c = take->cfg;
  int trk = -1;
  for (int t = 0; t < kTracks; ++t)
    if (c.track_ch[t] == ch + 1) { trk = t; break; }
  if (trk < 0 && c.fx_ch == ch + 1) trk = kFxTrack;
  if (kind == 0xC0) {
    if (ch + 1 == c.auto_ch || !c.valid) {
      if (!on) return;
      if (d1 == pattern && queued < 0) return;   // the PC at PLAY says the current pattern: not a change
      if (d1 == queued) return;                  // (2D PATTERN queued it already: the PC is the commit)
      uint32_t t = clk.at(f);
      if (d1 == last_pc && last_pc_t != 0xFFFFFFFFu && t - last_pc_t < kPpq) return;   // (2D PATTERN said it already)
      last_pc = d1, last_pc_t = t;
      queued = d1;
      add(kPc, d1, 0, 0, f, ms);
    }
    return;
  }
  if (kind != 0xB0) return;
  Nrpn& n = nrpn[ch];
  auto nrpn_out = [&](int v14) {
    if (n.msb < 0 || n.lsb < 0) return;
    int key = n.msb << 7 | n.lsb;
    if (n.msb == 1 && n.lsb == 101 && trk >= 0 && trk < kTracks) {   // mute (NRPN mode)
      uint32_t m = (v14 >> 7) ? mutes | 1u << trk : mutes & ~(1u << trk);
      mute_mask(m, f, ms);
      return;
    }
    if (n.msb == 1 && n.lsb == 104) {   // scene (NRPN mode)
      add(kScene, (uint8_t)(v14 >> 7), 0, 0, f, ms, false);
      return;
    }
    if (trk >= 0 && trk < kTracks && (banked >> trk & 1) && key == (1 << 7 | ((banked_smp >> trk & 1) ? 11 : 0))) return;   // rui1038: kSrc has it (rui1045: DSP SAMPLE's SMP 1:11)
    // (a channel no track has: 0x40 | channel, replayed on that channel)
    add(kParam, (uint8_t)(trk >= 0 ? trk : 0x40 | ch), (uint16_t)key, (uint32_t)v14, f, ms, true);
  };
  switch (d1) {
    case 99:
      if (n.vmsb >= 0) nrpn_out(n.vmsb << 7), n.vmsb = -1;
      n.msb = (int8_t)d2;
      return;
    case 98:
      if (n.vmsb >= 0) nrpn_out(n.vmsb << 7), n.vmsb = -1;
      n.lsb = (int8_t)d2;
      return;
    case 6:
      if (n.msb >= 0 && n.lsb >= 0) {
        if (n.vmsb >= 0) nrpn_out(n.vmsb << 7);
        n.vmsb = d2;
        return;
      }
      break;
    case 38:
      if (n.vmsb >= 0) {
        nrpn_out(n.vmsb << 7 | d2);
        n.vmsb = -1;
        return;
      }
      break;
    default: break;
  }
  if (d1 == 94 && trk >= 0 && trk < kTracks) {
    mute_mask(d2 ? mutes | 1u << trk : mutes & ~(1u << trk), f, ms);
    return;
  }
  if (d1 == 92) {
    add(kScene, d2, 0, 0, f, ms, false);
    return;
  }
  if (d1 >= 120) return;   // channel mode messages (the double STOP's CC 120)
  if (ch + 1 == c.auto_ch && d1 >= 35 && d1 <= 47 && d1 != 38) {
    add(kPerf, d1, 0, d2, f, ms, true);
    return;
  }
  if (trk >= 0 && trk < kTracks && (banked >> trk & 1) && d1 == ((banked_smp >> trk & 1) ? 27 : 16)) return;   // rui1038: SRC A's CC (rui1045: SMP's CC 27): kSrc has 14 bits
  add(kCc, (uint8_t)(ch + 1), d1, d2, f, ms, true);
}

CORE_COLD void Recorder::pattern_state(int now, int next, double f, uint32_t ms) {
  if (!on) pattern = now;
  if (next >= 0 && next != now && next != queued && on) {
    queued = next;
    uint32_t t = clk.at(f);
    if (!(next == last_pc && last_pc_t != 0xFFFFFFFFu && t - last_pc_t < kPpq)) {
      last_pc = next, last_pc_t = t;
      add(kPc, (uint8_t)next, 0, 0, f, ms);
    }
  }
  if (next < 0 || next == now) queued = -1;
}
CORE_COLD void Recorder::switched(int from, int to, double f, uint32_t ms) {
  clk.switch_seen();
  pattern = to, queued = -1;
  if (on && add(kSw, (uint8_t)from, (uint16_t)to, 0, f, ms)) sw_pending = take->n - 1;
}
// a clock tick: the switch reported just before it is on its step 0 (the SWITCH comes in the block before the tick)
CORE_COLD void Recorder::tick(int step, int len, int speed, double f) {
  clk.tick(step, len, speed, f);
  if (sw_pending >= 0 && sw_pending < take->n && take->ev[sw_pending].type == kSw && clk.t >= take->ev[sw_pending].t &&
      clk.t - take->ev[sw_pending].t < kPpq)
    take->ev[sw_pending].t = clk.t;
  sw_pending = -1;
}
CORE_COLD void Recorder::mute_mask(uint32_t mask, double f, uint32_t ms) {
  mask &= 0xFFF;
  if (mask == mutes) return;
  mutes = mask;
  if (on) add(kMute, 0, 0, mask, f, ms);
  else if (armed && take) take->mutes = mask;
}
CORE_COLD void Recorder::tempo(double bpm, double f, uint32_t ms) {
  clk.tempo(bpm);
  uint32_t x = (uint32_t)(bpm * 120 + 0.5);
  if (take && !on) take->tempo = x;
  if (on) add(kTempo, 0, 0, x, f, ms);
}
CORE_COLD void Recorder::fill(bool held, double f, uint32_t ms) {
  if (on) add(kFill, held ? 1 : 0, 0, 0, f, ms);
}
CORE_COLD void Recorder::sidecar(uint8_t kind, uint16_t index, uint32_t v, double f, uint32_t ms) {
  if (on) add(kSc, kind, index, v, f, ms, true);
}
CORE_COLD void Recorder::machine(int track, int vb, double f, uint32_t ms) {
  if (on) add(kMach, (uint8_t)track, (uint16_t)vb, 0, f, ms);
}
CORE_COLD void Recorder::mark(double f, uint32_t ms) {
  if (on) add(kMark, 0, 0, 0, f, ms);
}
CORE_COLD void Recorder::src(int track, int v14, double f, uint32_t ms, int param) {
  if (track >= 0 && track < kTracks) add(kSrc, (uint8_t)track, (uint16_t)(param & 15), (uint32_t)(v14 & 0x3FFF), f, ms, true);
}

}  // namespace song
}  // namespace core
#endif
