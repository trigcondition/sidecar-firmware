// core/rec/rec_wav.h - the recorder's WAV header (pi/engine/recorder.h, core/rec/mini_rec.h): PCM, 48 kHz, 16 or 24 bit, 1 or 2 channels, always 512
// bytes (RIFF, fmt, a JUNK pad, data) so the samples start on a sector and every 192 KB write stays sector-aligned.
// While a take records, the sizes hold a placeholder (the largest whole frame count a 32-bit size allows): a file
// cut by a power loss still opens and reads to its end in most readers, and the next boot's scan (fix) writes the
// true sizes. Portable: no Circle, no allocation.
#pragma once
#include <stdint.h>
#include <string.h>

namespace recwav {

const uint32_t kHeader = 512, kRate = 48000;
const uint32_t kJunk = kHeader - 12 - 24 - 8 - 8;   // 460

inline void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
inline void put32(uint8_t* p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
inline uint32_t get16(const uint8_t* p) { return p[0] | p[1] << 8; }
inline uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

// the placeholder: as many whole frames as a 32-bit RIFF size allows
inline uint32_t placeholder(uint32_t align) { return (0xFFFFFFFFu - (kHeader - 8) - 1) / align * align; }

// h[kHeader]: channels 1|2, bits 16|24, data bytes (placeholder(align) while recording)
inline void build(uint8_t* h, unsigned channels, unsigned bits, uint32_t data) {
  uint32_t align = channels * (bits / 8);
  memset(h, 0, kHeader);
  memcpy(h, "RIFF", 4);
  put32(h + 4, (kHeader - 8) + data + (data & 1));
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 1);   // PCM
  put16(h + 22, channels);
  put32(h + 24, kRate);
  put32(h + 28, kRate * align);
  put16(h + 32, align);
  put16(h + 34, bits);
  memcpy(h + 36, "JUNK", 4);
  put32(h + 40, kJunk);
  memcpy(h + kHeader - 8, "data", 4);
  put32(h + kHeader - 4, data);
}

// one of ours (the layout above): its block align, else 0
inline uint32_t ours(const uint8_t* h) {
  if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVEfmt ", 8) || memcmp(h + 36, "JUNK", 4) || memcmp(h + kHeader - 8, "data", 4))
    return 0;
  uint32_t ch = get16(h + 22), bits = get16(h + 34);
  if ((ch != 1 && ch != 2) || (bits != 16 && bits != 24) || get16(h + 32) != ch * bits / 8) return 0;
  return ch * bits / 8;
}

// one of ours still holding the placeholder: never closed (its sizes come from the file)
inline bool open_ended(const uint8_t* h) {
  uint32_t align = ours(h);
  return align && get32(h + kHeader - 4) == placeholder(align);
}

// The sizes a file of `size` bytes should carry (the data cut to whole frames). True: h changed (write it back).
// A closed header (real sizes) is only ever shrunk to a shorter file, never grown: a file longer than its data holds
// space reserved after it (pi/engine/recorder.h), not audio.
inline bool fix(uint8_t* h, uint64_t size) {
  uint32_t align = ours(h);
  if (!align) return false;
  uint64_t d = size > kHeader ? size - kHeader : 0;
  if (d > placeholder(align)) d = placeholder(align);
  uint32_t data = (uint32_t)(d / align * align);
  if (!open_ended(h) && data > get32(h + kHeader - 4)) return false;
  if (get32(h + kHeader - 4) == data && get32(h + 4) == (kHeader - 8) + data + (data & 1)) return false;
  put32(h + 4, (kHeader - 8) + data + (data & 1));
  put32(h + kHeader - 4, data);
  return true;
}

}  // namespace recwav
