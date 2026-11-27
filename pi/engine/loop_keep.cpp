// pi/engine/loop_keep.cpp - see loop_keep.h.
#include "loop_keep.h"

#include <stdio.h>
#include <string.h>

#include "platform.h"   // sniprintf (newlib's integer printf; the Pi has no snprintf)
#include "rec/rec_wav.h"

namespace loop_pi {

bool Keep::fail(const char* why) {
  sniprintf(err_, sizeof err_, "%s", why);
  if (b_ && b_->log) {
    char l[96];
    sniprintf(l, sizeof l, "loop keep: %s", why);
    b_->log(l);
  }
  // no lying take: the partial file (a header with the full sizes over a short body) and its folder go; without a remove the
  // header is patched to what was written
  if (b_ && take_[0] && own_dir_) {   // (only a folder this job made: never an existing take's)
    char rel[100];
    rec::take_dir(rel, sizeof rel, take_);
    if (h_ >= 0) {
      if (!b_->remove) {
        uint8_t hdr[recwav::kHeader];
        recwav::build(hdr, 2, 24, done_ * 6);
        b_->pwrite(h_, 0, hdr, sizeof hdr);
      }
      b_->close(h_), h_ = -1;
    }
    if (b_->remove) {
      char f[160];
      sniprintf(f, sizeof f, "%s/00_LOOP.wav", rel);
      b_->remove(f);
      b_->remove(rel);
    }
  }
  finish(false);
  return false;
}

void Keep::finish(bool ok) {
  if (h_ >= 0) b_->close(h_), h_ = -1;
  state_ = 0;
  if (done_cb_) done_cb_(job_.id, ok, take_);
}

// the take's folder, named as the recorder's pick_name does (the project's slug, "sidecar" without one)
bool Keep::begin(const core::loop::KeepJob& j) {
  static char names[1024][64];
  static uint64_t sizes[1024];
  static bool dirs[1024];
  job_ = j;
  own_dir_ = false;
  err_[0] = 0, take_[0] = 0, done_ = pieces_ = worst_ = yields_ = 0;
  if (!b_ || !j.pcm || !j.frames) return fail("nothing to write"), false;
  state_ = 1;
  if (!b_->mkdir("/Recordings")) return fail("no /Recordings folder"), false;
  char sl[rec::kSlugMax + 1], base[rec::kTakeMax], rel[100];
  char pn[48] = "";
  if (b_->project) b_->project(pn, sizeof pn);
  rec::slug(pn, sl, sizeof sl);
  if (!sl[0]) sniprintf(sl, sizeof sl, "sidecar");
  uint32_t local = 0;
  bool clock = b_->clock && b_->clock(&local);
  if (clock) rec::stamp_name(base, sizeof base, sl, local);
  int n = b_->list("/Recordings", names, sizes, dirs, 1024);
  if (n < 0) return fail("cannot read /Recordings"), false;
  int next = rec::next_number(names, n, sl, clock, base);
  bool made = false;
  for (int tries = 0; tries < 16 && !made; ++tries, ++next) {
    if (clock) rec::variant(take_, sizeof take_, base, next);
    else rec::seq_name(take_, sizeof take_, sl, next);
    rec::take_dir(rel, sizeof rel, take_);
    int m = b_->mkdir_new ? b_->mkdir_new(rel) : b_->mkdir(rel) ? 1 : -1;
    if (m < 0) return fail("cannot make the take's folder"), false;
    made = m > 0;
  }
  if (!made) return take_[0] = 0, fail("cannot make the take's folder"), false;
  own_dir_ = true;
  sniprintf(path_, sizeof path_, "%s/00_LOOP.wav", rel);
  h_ = b_->open(path_);
  if (h_ < 0) return fail("cannot open the file"), false;
  uint8_t hdr[recwav::kHeader];
  recwav::build(hdr, 2, 24, j.frames * 6);
  if (!b_->write(h_, hdr, sizeof hdr)) return fail("cannot write the file"), false;
  return true;
}

bool Keep::poll() {
  if (state_ == 0) return false;
  if (state_ == 1) {
    if (b_->card_busy && b_->card_busy() && yields_ < 40) {   // the streams' reads first; after a few polls it goes on
      ++yields_;
      return true;
    }
    yields_ = 0;
    uint32_t n = job_.frames - done_;
    if (n > kPiece / 6) n = kPiece / 6;
    uint8_t buf[kPiece];
    const int32_t* s = job_.pcm + 2 * (size_t)done_;
    for (uint32_t i = 0; i < 2 * n; ++i) {   // 32-bit full scale -> 24-bit little endian
      int32_t v = s[i] >> 8;
      buf[3 * i] = (uint8_t)v, buf[3 * i + 1] = (uint8_t)(v >> 8), buf[3 * i + 2] = (uint8_t)(v >> 16);
    }
    if (!b_->write(h_, buf, n * 6)) return fail("write failed (card full?)"), false;
    ++pieces_, done_ += n;
    if (n * 6 > worst_) worst_ = n * 6;
    if (done_ >= job_.frames) state_ = 2;
    return true;
  }
  // closing: the sizes were true from the start; the sync, the close, then loop.txt
  if (!b_->sync(h_)) return fail("sync failed"), false;
  if (!b_->close(h_)) return h_ = -1, fail("close failed"), false;
  h_ = -1;
  char rel[100], txt[160];
  rec::take_dir(rel, sizeof rel, take_);
  sniprintf(path_, sizeof path_, "%s/loop.txt", rel);
  const int b10 = (int)(job_.bpm * 10.0f + 0.5f);   // (integer printf: the tempo in tenths)
  int n = sniprintf(txt, sizeof txt, "bpm=%d.%d\nbars=", b10 / 10, b10 % 10);
  if (job_.half_bars & 1) n += sniprintf(txt + n, sizeof txt - n, "%d.5\n", job_.half_bars / 2);
  else n += sniprintf(txt + n, sizeof txt - n, "%d\n", job_.half_bars / 2);
  uint32_t local = 0;
  if (b_->clock && b_->clock(&local)) {
    char dt[24];
    rec::date_time(local, dt, sizeof dt);
    n += sniprintf(txt + n, sizeof txt - n, "grabbed=%s\n", dt);
  }
  int t = b_->open(path_);
  if (t < 0 || !b_->write(t, txt, (uint32_t)n)) return (t >= 0 ? (void)b_->close(t) : (void)0), fail("cannot write loop.txt"), false;
  b_->close(t);
  finish(true);
  return false;
}

}  // namespace loop_pi
