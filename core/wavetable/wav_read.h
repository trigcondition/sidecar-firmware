// core/wavetable/wav_read.h - a WAV (or AIFF) file parsed from memory, for WAVETABLE's tables and GRANULAR's samples
// (docs/WAVETABLE.md, docs/GRANULAR.md): RIFF/WAVE with fmt / data / 'clm ' anywhere; PCM 8/16/24/32-bit, IEEE float
// 32/64-bit, WAVE_FORMAT_EXTENSIBLE with either; FORM/AIFF and AIFC (NONE / twos big-endian, sowt little-endian PCM
// 8..32-bit, fl32 / fl64 float) with COMM / SSND; any channel count (the first two summed: the voice is mono), any
// rate. Header-only and allocation-free; the Pi's hosted objects link no C library besides strings and libm.
#pragma once
#include <stdint.h>
#include <string.h>

namespace core {
namespace wavread {

struct Info {
  uint16_t format;      // 1 PCM, 3 float
  uint16_t ch, bits;
  uint32_t rate;
  uint32_t data_off;    // the data chunk's first byte in the file
  uint32_t data_len;    // its bytes (cut to the file when a writer left it longer)
  uint32_t frames;
  uint32_t clm_frame;   // a 'clm ' chunk's "<!>N" (Serum's wavetable frame size), 0 none
  uint8_t be;           // AIFF: big-endian samples
  uint8_t s8;           // AIFF: 8-bit samples are signed
};

inline uint16_t le16(const uint8_t* b) { return (uint16_t)(b[0] | b[1] << 8); }
inline uint32_t le32(const uint8_t* b) { return (uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24); }

// The 'clm ' text: "<!>2048 ..." (Serum); the number after "<!>", 0 none.
inline uint32_t clm_size(const uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i + 3 < n; ++i) {
    if (p[i] != '<' || p[i + 1] != '!' || p[i + 2] != '>') continue;
    uint32_t v = 0, k = i + 3;
    while (k < n && p[k] >= '0' && p[k] <= '9' && v < 100000) v = v * 10 + (p[k++] - '0');
    return v;
  }
  return 0;
}

inline uint16_t be16(const uint8_t* b) { return (uint16_t)(b[0] << 8 | b[1]); }
inline uint32_t be32(const uint8_t* b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }
// AIFF's 80-bit extended sample rate as an integer
inline uint32_t ext80(const uint8_t* b) {
  int e = ((b[0] & 0x7F) << 8 | b[1]) - 16383;
  uint32_t m = be32(b + 2);
  if (e < 0 || e > 31) return 0;
  return m >> (31 - e);
}
// FORM AIFF / AIFC (big-endian chunks)
inline bool parse_aiff(const uint8_t* b, uint32_t n, Info* a, uint32_t size) {
  bool aifc = !memcmp(b + 8, "AIFC", 4), comm = false, data = false;
  a->be = 1, a->s8 = 1, a->format = 1;
  uint32_t p = 12;
  while (p + 8 <= n) {
    uint32_t len = be32(b + p + 4);
    const uint8_t* c = b + p + 8;
    if (!memcmp(b + p, "COMM", 4) && p + 8 + 18 <= n) {
      a->ch = be16(c);
      a->bits = be16(c + 6);
      a->rate = ext80(c + 8);
      if (aifc && p + 8 + 22 <= n) {
        if (!memcmp(c + 18, "sowt", 4)) a->be = 0;
        else if (!memcmp(c + 18, "fl32", 4) || !memcmp(c + 18, "FL32", 4)) a->format = 3, a->bits = 32;
        else if (!memcmp(c + 18, "fl64", 4) || !memcmp(c + 18, "FL64", 4)) a->format = 3, a->bits = 64;
        else if (memcmp(c + 18, "NONE", 4) && memcmp(c + 18, "twos", 4)) return false;   // compressed
      }
      a->bits = (uint16_t)((a->bits + 7) / 8 * 8);
      comm = true;
    } else if (!memcmp(b + p, "SSND", 4) && p + 16 <= n) {
      uint32_t off = be32(c);
      a->data_off = p + 16 + off;
      a->data_len = len >= 8 + off ? len - 8 - off : 0;
      if ((uint64_t)a->data_off + a->data_len > size) a->data_len = size > a->data_off ? size - a->data_off : 0;
      data = true;
    }
    uint64_t next = (uint64_t)p + 8 + len + (len & 1);
    if (next > 0xFFFFFFFFu) break;
    p = (uint32_t)next;
  }
  if (!comm || !data || !a->ch || !a->rate) return false;
  bool ok = (a->format == 1 && (a->bits == 8 || a->bits == 16 || a->bits == 24 || a->bits == 32)) ||
            (a->format == 3 && (a->bits == 32 || a->bits == 64));
  if (!ok) return false;
  a->frames = a->data_len / (a->ch * (a->bits / 8));
  return true;
}

// The header of the file in b (n bytes: all of it, or at least everything before the data chunk). False: not a WAV
// this reads. size: the whole file's length (data_len is cut to it), 0 = n.
inline bool parse(const uint8_t* b, uint32_t n, Info* a, uint32_t size = 0) {
  if (!size) size = n;
  memset(a, 0, sizeof *a);
  if (n >= 12 && !memcmp(b, "FORM", 4) && (!memcmp(b + 8, "AIFF", 4) || !memcmp(b + 8, "AIFC", 4)))
    return parse_aiff(b, n, a, size);
  if (n < 12 || memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0) return false;
  bool fmt = false, data = false;
  uint32_t p = 12;
  while (p + 8 <= n) {
    uint32_t len = le32(b + p + 4);
    const uint8_t* c = b + p + 8;
    if (!memcmp(b + p, "fmt ", 4) && len >= 16 && p + 8 + 16 <= n) {   // (a shorter fmt chunk is no format: the next chunk is not its bytes)
      a->format = le16(c);
      a->ch = le16(c + 2);
      a->rate = le32(c + 4);
      a->bits = le16(c + 14);
      if (a->format == 0xFFFE && len >= 40 && p + 8 + 26 <= n) a->format = le16(c + 24);   // EXTENSIBLE: the GUID's first word
      fmt = true;
    } else if (!memcmp(b + p, "clm ", 4)) {
      uint32_t m = p + 8 + len <= n ? len : n - (p + 8);
      a->clm_frame = clm_size(c, m);
    } else if (!memcmp(b + p, "data", 4)) {
      a->data_off = p + 8;
      a->data_len = len;
      if ((uint64_t)a->data_off + a->data_len > size) a->data_len = size > a->data_off ? size - a->data_off : 0;
      data = true;
    }
    uint64_t next = (uint64_t)p + 8 + len + (len & 1);
    if (next > 0xFFFFFFFFu) break;
    p = (uint32_t)next;
  }
  if (!fmt || !data || !a->ch || !a->rate) return false;
  bool ok = (a->format == 1 && (a->bits == 8 || a->bits == 16 || a->bits == 24 || a->bits == 32)) ||
            (a->format == 3 && (a->bits == 32 || a->bits == 64));
  if (!ok) return false;
  a->frames = a->data_len / (a->ch * (a->bits / 8));
  return true;
}

inline uint32_t frame_bytes(const Info& a) { return a.ch * (a.bits / 8); }

// One channel's sample at p, in -1..1.
inline float sample(const Info& a, const uint8_t* p) {
  if (a.be) {
    switch (a.bits) {
      case 8: return (int8_t)p[0] * (1.0f / 128.0f);
      case 16: return (int16_t)be16(p) * (1.0f / 32768.0f);
      case 24: return (int32_t)((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8) * (1.0f / 2147483648.0f);
      case 32:
        if (a.format == 3) {
          uint32_t u = be32(p);
          float f;
          memcpy(&f, &u, 4);
          return f;
        }
        return (int32_t)be32(p) * (1.0f / 2147483648.0f);
      default: {
        uint64_t u = (uint64_t)be32(p) << 32 | be32(p + 4);
        double d;
        memcpy(&d, &u, 8);
        return (float)d;
      }
    }
  }
  if (a.bits == 8 && a.s8) return (int8_t)p[0] * (1.0f / 128.0f);
  switch (a.bits) {
    case 8: return (p[0] - 128) * (1.0f / 128.0f);
    case 16: return (int16_t)le16(p) * (1.0f / 32768.0f);
    case 24: return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) * (1.0f / 2147483648.0f);
    case 32:
      if (a.format == 3) {
        float f;
        memcpy(&f, p, 4);
        return f;
      }
      return (int32_t)le32(p) * (1.0f / 2147483648.0f);
    default: {   // 64-bit float
      double d;
      memcpy(&d, p, 8);
      return (float)d;
    }
  }
}

// frames whole frames from in into out, mono (the first two channels summed and halved)
inline void to_mono(const Info& a, const uint8_t* in, uint32_t frames, float* out) {
  uint32_t fb = frame_bytes(a), bps = a.bits / 8;
  for (uint32_t i = 0; i < frames; ++i) {
    const uint8_t* f = in + i * fb;
    float x = sample(a, f);
    if (a.ch > 1) x = 0.5f * (x + sample(a, f + bps));
    out[i] = x;
  }
}
// frames of a 2+ channel file into int16 L R interleaved (out: 2 x frames), the first two channels as they are (16-bit
// exactly; others rounded as to_mono's callers do: x 32768, clamped, truncated)
inline void to_stereo16(const Info& a, const uint8_t* in, uint32_t frames, int16_t* out) {
  uint32_t fb = frame_bytes(a), bps = a.bits / 8;
  for (uint32_t i = 0; i < frames; ++i) {
    const uint8_t* f = in + i * fb;
    for (int c = 0; c < 2; ++c) {
      float s = sample(a, f + (a.ch > 1 ? c : 0) * bps) * 32768.0f;
      out[2 * i + c] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
  }
}

}  // namespace wavread
}  // namespace core
