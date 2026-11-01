// Overbridge 2 (Analog Rytm MKII) wire format and descriptor scan, with no
// Arduino or USBHost_t36 dependency, so it builds and is tested on the Mac
// (extras/ob_block_test.cpp).
//
// The format, from Overwitch's documentation (GPL-3, read for understanding
// only; nothing copied) and checked against the Rytm OS 1.73 firmware running
// in rytm-emu (src/usbhost.rs, docs/RYTM-PORT.md) [measured in the emulator]:
//
//   configuration 1; interface 1 alt 3: interrupt IN 0x83, interface 2 alt 3:
//   interrupt OUT 0x03, both wMaxPacketSize 368, bInterval 1 (one packet per
//   125 us microframe at most); the MIDI interface (class 1/3) is interface 5,
//   bulk 0x01 / 0x81, 512-byte packets.
//
//   A block (one packet) = a 32-byte header + 7 frames x N channels x int32
//   big-endian. Header: bytes 0-1 0x07FF host -> device, 0x0700 device -> host;
//   bytes 2-3 a big-endian frame counter (+7 a block); bytes 4-31 unknown (the
//   host sends zeros). Rytm MKII: N = 12 both ways.
//     device -> host: MAIN L, MAIN R, BD SD RS/CP BT LT MT/HT CH/OH CY/CB
//                     (channels 2..9, track order), AUDIO IN L, R
//     host -> device: MAIN L/R, MAIN FX L/R, then BD SD RS/CP BT LT MT/HT
//                     CH/OH CY/CB (4..11) - each voice's own input, mixed into
//                     the voice's digital layer before its analog chain
//                     (docs/overbridge-voice-inputs.md).
//   The sizes follow the descriptor and each direction has its own: a Digitakt
//   mk1 is 12 channels / 368 B to the host and 2 / 88 B from it, alt 3, same
//   endpoints (Overwitch's device table agrees; the numbers per box are in
//   their profiles, core/device).
#pragma once
#include <stdint.h>
#include <string.h>

namespace ob {

constexpr uint16_t kVid = 0x1935;          // Elektron
constexpr uint16_t kPidRytmMk2 = 0x0B48;   // Analog Rytm MKII, OVERBRIDGE mode
constexpr int kFramesPerBlock = 7;
constexpr int kHeaderBytes = 32;
constexpr uint8_t kInEp = 0x83, kOutEp = 0x03;
constexpr uint8_t kInIface = 1, kOutIface = 2, kAlt = 3;
constexpr int kChannels = 12;              // the Rytm MKII, both ways
constexpr int kVoices = 8;

// Physical voice v's host -> Rytm channel (the firmware's table at 0x401bea90,
// OS 1.73: BD BT SD LT CH/OH MT/HT CY/CB RS/CP), and its index among the
// Rytm -> host voice channels 2..9 (track order) - sim/obhost.py VOICE_CH,
// VOICE_OUT_IDX.
constexpr uint8_t kVoiceToRytm[kVoices] = {4, 7, 5, 8, 10, 9, 11, 6};
constexpr uint8_t kVoiceFromRytm[kVoices] = {2 + 0, 2 + 3, 2 + 1, 2 + 4, 2 + 6, 2 + 5, 2 + 7, 2 + 2};
// No voice channel (put_voice_frame skips a channel past the block's): with mirror 0, frame slot 0 alone goes out,
// on MAIN L/R - the stock Rytm's main mode (the application puts its MAIN mix in slot 0, core/main_out.h).
constexpr uint8_t kVoiceToNone[kVoices] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

constexpr int block_bytes(int channels) { return kHeaderBytes + kFramesPerBlock * channels * 4; }
static_assert(block_bytes(kChannels) == 368, "Rytm MKII block");

// Channels a block of wMaxPacketSize `mps` carries, 0 if it is not a block.
inline int channels_for(int mps) {
  int d = mps - kHeaderBytes;
  return (d >= kFramesPerBlock * 4 && d % (kFramesPerBlock * 4) == 0) ? d / (kFramesPerBlock * 4) : 0;
}

inline void put_be32(uint8_t* b, int32_t v) {
  uint32_t u = (uint32_t)v;
  b[0] = u >> 24;
  b[1] = u >> 16;
  b[2] = u >> 8;
  b[3] = u;
}
inline int32_t get_be32(const uint8_t* b) {
  return (int32_t)((uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
}

// Host -> device: the header (magic 0x07FF, the counter, zeros).
inline void write_header(uint8_t* blk, uint16_t counter) {
  memset(blk, 0, kHeaderBytes);
  blk[0] = 0x07;
  blk[1] = 0xFF;
  blk[2] = counter >> 8;
  blk[3] = counter & 0xFF;
}

// Host -> device: frame f (0..6) of a block of `channels`, all zero except the
// 8 voices (int16, full scale -> int32 full scale) on their channels, and
// voice `mirror` (if 0..7) also on MAIN L/R (channels 0, 1: no trig needed to
// hear it - for bring-up).
inline void put_voice_frame(uint8_t* blk, int channels, int f, const int16_t* v, const uint8_t map[kVoices],
                            int mirror) {
  uint8_t* fr = blk + kHeaderBytes + 4 * channels * f;
  memset(fr, 0, 4 * channels);
  if (!v) return;
  for (int i = 0; i < kVoices; ++i)
    if (map[i] < channels) put_be32(fr + 4 * map[i], (int32_t)v[i] << 16);
  if (mirror >= 0 && mirror < kVoices && channels >= 2) {
    put_be32(fr, (int32_t)v[mirror] << 16);
    put_be32(fr + 4, (int32_t)v[mirror] << 16);
  }
}

// Host -> device: a block's frames, the first `channels` of each row of F (a Digitakt takes 2 of the 12 the output stage makes).
inline void put_frames(uint8_t* blk, int channels, const int32_t (*F)[kChannels]) {
  for (int f = 0; f < kFramesPerBlock; ++f) {
    uint8_t* fr = blk + kHeaderBytes + 4 * channels * f;
    for (int c = 0; c < channels; ++c) put_be32(fr + 4 * c, F[f][c]);
  }
}

// Device -> host.
inline bool block_from_device(const uint8_t* blk) { return blk[0] == 0x07 && blk[1] == 0x00; }
inline uint16_t block_counter(const uint8_t* blk) { return (uint16_t)(blk[2] << 8 | blk[3]); }
inline int32_t block_sample(const uint8_t* blk, int channels, int f, int c) {
  return get_be32(blk + kHeaderBytes + 4 * (channels * f + c));
}

// What the configuration descriptor says about the Overbridge endpoints.
struct Endpoint {
  bool found;
  uint8_t addr, attr, interval;
  uint16_t mps;
};
struct Layout {
  Endpoint in, out;          // 0x83 in (kInIface, alt), 0x03 in (kOutIface, alt)
  int in_channels, out_channels;
  int midi_iface;            // the MIDIStreaming interface (class 1/3), -1 none
  int interfaces;            // interface descriptors with alt 0
};

// Scan descriptors (a whole configuration descriptor, or any tail of one, as
// USBHost_t36 hands them to claim()) for the endpoints of alternate setting
// `alt`. True when both are interrupt endpoints carrying whole blocks.
inline bool find_layout(const uint8_t* p, uint32_t len, uint8_t alt, Layout* l) {
  memset(l, 0, sizeof *l);
  l->midi_iface = -1;
  const uint8_t* end = p + len;
  int iface = -1, cur_alt = -1;
  while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
    if (p[1] == 4 && p[0] >= 9) {
      iface = p[2];
      cur_alt = p[3];
      if (cur_alt == 0) ++l->interfaces;
      if (p[5] == 1 && p[6] == 3 && l->midi_iface < 0) l->midi_iface = iface;
    } else if (p[1] == 5 && p[0] >= 7 && cur_alt == alt) {
      Endpoint e = {true, p[2], p[3], p[6], (uint16_t)(p[4] | (p[5] << 8))};
      if (iface == kInIface && p[2] == kInEp) l->in = e;
      if (iface == kOutIface && p[2] == kOutEp) l->out = e;
    }
    p += p[0];
  }
  l->in_channels = l->in.found ? channels_for(l->in.mps & 0x7FF) : 0;
  l->out_channels = l->out.found ? channels_for(l->out.mps & 0x7FF) : 0;
  return l->in.found && l->out.found && (l->in.attr & 3) == 3 && (l->out.attr & 3) == 3 && l->in_channels > 0 &&
         l->out_channels > 0 && (l->in.mps >> 11) == 0 && (l->out.mps >> 11) == 0;
}

}  // namespace ob
