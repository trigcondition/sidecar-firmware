// core/samp/out.cpp - see out.h.
#include "out.h"

#include <string.h>
#include <strings.h>

#include "../fmt/fmt.h"
#include "../lib/library.h"
#include "../pool/pool.h"
#include "dsp.h"

namespace core {
namespace samp {

namespace {
uint8_t g_buf[cardq::kPiece + 16];   // one writer's piece at a time (the card core's)
}

CORE_COLD bool mkdirs(const char* path) {
  char p[200];
  int n = (int)strlen(path);
  if (n >= (int)sizeof p) return false;
  memcpy(p, path, n + 1);
  for (int i = 1; i <= n; ++i)
    if (p[i] == '/' || p[i] == 0) {
      char c = p[i];
      p[i] = 0;
      if (!cards.sd().exists(p)) cards.sd().mkdir(p);
      p[i] = c;
    }
  return cards.sd().exists(path);
}

// 0.3.0 power-cut safety: fmt::write_atomic, not a plain truncating write. These are the .sfz files, and an .sfz is
// the only record of how a sampled instrument's WAVs map to keys and velocities - after a long auto-sampling run the
// WAVs are the expensive part but the .sfz is the part that cannot be worked out again. A cut now leaves the previous
// .sfz, never half of one, and the few extra ms land in a save that is already writing megabytes of WAV. What this
// does not do: .sfz files are opened by path in several places (msamp/sfz's callers, lib/pools, bundle/export,
// page_auto), so there is no one reader to call fmt::recover from. A cut inside write_atomic's rename window - about
// 4 ms of the ~24 ms it spends on the card - therefore leaves <name>.sfz.prev with no <name>.sfz, which is visible
// and recoverable by hand, where the plain write left a truncated .sfz that parsed into a quietly wrong instrument.
// Closed 2026-10-26 by the boot sweep (fmt::sweep_boot, from engine_begin): the .prev is put back before anything
// reads the card.
CORE_COLD bool write_text(const char* path, const char* text, int len) {
  if (len < 0 || !fmt::write_atomic(path, text, (uint32_t)len)) return false;
  uint8_t h[32];   // the library: an .sfz with its hash (a text it does not index: ignored there)
  lib::sha256(text, (size_t)len, h);
  lib::touched(path, h, lib::crc32(0, text, (size_t)len));
  return true;
}

CORE_COLD void safe_name(const char* in, char* out, int cap) {
  int k = 0;
  for (; *in && k < cap - 1; ++in) {
    char c = *in;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    out[k++] = ok ? c : '_';
  }
  if (!k && cap > 1) out[k++] = 'X';
  out[k] = 0;
}

CORE_COLD void next_name(const char* dir, const char* prefix, const char* ext, char* out, int cap) {
  int best = 0, pl = (int)strlen(prefix), el = (int)strlen(ext);
  File d, e;
  if (d.open(dir, kRead) && d.isDir()) {
    while (e.openNext(&d, kRead)) {
      char nm[80];
      e.getName(nm, sizeof nm);
      e.close();
      int l = (int)strlen(nm);
      if (l < pl + 2 || strncmp(nm, prefix, pl) || nm[pl] != '_') continue;
      if (el && (l <= el || strcasecmp(nm + l - el, ext))) continue;
      int v = 0, i = pl + 1;
      for (; i < l && nm[i] >= '0' && nm[i] <= '9'; ++i) v = v * 10 + (nm[i] - '0');
      if (i == pl + 1) continue;
      if (v > best) best = v;
    }
  }
  sniprintf(out, cap, "%s_%03d", prefix, best + 1);
}

CORE_COLD bool WavWriter::begin(const char* path, const float* x, int ch, uint32_t a, uint32_t n, float gain,
                                uint32_t fade_in, uint32_t fade_out, bool loop, uint32_t loop_s, uint32_t loop_e,
                                int root) {
  if (open_) f_.close();
  open_ = false;
  if (!f_.open(path, kWrite)) return false;
  uint8_t h[kWavHeaderMax];
  int hn = wav_header(h, ch, 24, kRate, n, loop, loop_s, loop_e, root);
  if ((int)f_.write(h, hn) != hn) {
    f_.close();
    return false;
  }
  sniprintf(path_, sizeof path_, "%s", path);
  sh_.init();
  sh_.update(h, (size_t)hn);
  crc_ = lib::crc32(0, h, (size_t)hn);
  x_ = x, ch_ = ch, a_ = a, n_ = n, pos_ = 0, gain_ = gain;
  fin_ = fade_in < n / 2 ? fade_in : n / 2, fout_ = fade_out < n / 2 ? fade_out : n / 2;
  odd_ = wav_data_bytes(ch, 24, n) & 1;
  bytes = (uint32_t)hn, yields = 0, waiting_ = false;
  open_ = true;
  return true;
}

CORE_COLD int WavWriter::step() {
  if (!open_) return 0;
  if (cardq::busy()) {   // a stream needs the card: wait (at most kUploadWaitMs a piece)
    uint32_t now = now_ms();
    if (!waiting_) waiting_ = true, wait0_ = now;
    if (now - wait0_ < cardq::kUploadWaitMs) {
      ++yields;
      return 1;
    }
  }
  waiting_ = false;
  uint32_t fb = (uint32_t)ch_ * 3, nf = cardq::kPiece / fb;
  if (nf > n_ - pos_) nf = n_ - pos_;
  uint8_t* o = g_buf;
  for (uint32_t i = 0; i < nf; ++i) {
    uint32_t t = pos_ + i;
    float g = gain_;
    if (t < fin_) g *= (float)t / fin_;
    if (t >= n_ - fout_) g *= (float)(n_ - t) / fout_;
    for (int c = 0; c < ch_; ++c, o += 3) put24(o, x_[(size_t)(a_ + t) * ch_ + c] * g);
  }
  pos_ += nf;
  uint32_t len = (uint32_t)(o - g_buf);
  if (pos_ >= n_ && odd_) *o++ = 0, ++len;   // RIFF's pad byte
  bool ok = f_.write(g_buf, len) == len;
  bytes += len;
  sh_.update(g_buf, len);
  crc_ = lib::crc32(crc_, g_buf, len);
  if (!ok || pos_ >= n_) {
    ok = f_.close() && ok;
    open_ = false;
    if (ok) {   // the library: the take by its hash, made while it was written (no second read)
      uint8_t d[32];
      sh_.final(d);
      lib::touched(path_, d, crc_);
    }
    return ok ? 0 : -1;
  }
  return 1;
}

}  // namespace samp
}  // namespace core
