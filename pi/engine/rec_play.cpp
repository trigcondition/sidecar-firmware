// pi/engine/rec_play.cpp - see rec_play.h.
#include "rec_play.h"

#include <string.h>

#include "rec/rec_wav.h"
#ifdef REC_HOST   // the Mac tests
#include <stdio.h>
#else
#include "core_port.h"   // platform_pi.cpp's integer printf
#define snprintf core_snprintf
#endif

namespace recplay {

void take_folder(char* out, unsigned cap, const char* id) {
  const char* c = strchr(id, ':');
  if (c) snprintf(out, cap, "%.*s/Recordings/%s", (int)(c + 1 - id), id, c + 1);
  else snprintf(out, cap, "/Recordings/%s", id);
}

void Player::init(const Backend* b, uint32_t (*now_us)()) {
  b_ = b;
  now_ = now_us;
  set_level(level_);
}

// ---- the takes and their channels

static bool newer(uint32_t sa, const char* a, uint32_t sb, const char* b) {   // a before b in the list
  if (sa != sb) return sa > sb;
  const char *ca = strchr(a, ':'), *cb = strchr(b, ':');   // the same stamp (no clock): take_010 before take_009,
  int c = strcmp(ca ? ca + 1 : a, cb ? cb + 1 : b);        // whichever volume it is on (USB:take_010 too)
  return c ? c > 0 : strcmp(a, b) > 0;
}

int Player::scan(const char* prefer) {
  static char names[256][64];
  static uint64_t sizes[256];
  static bool dirs[256];
  static uint32_t stamps[256];
  char was[sizeof takes_[0].name];
  strcpy(was, ntakes_ ? takes_[take_i_].name : "");
  const char* roots[8] = {""};
  int nroots = 1;
  if (b_ && b_->roots) nroots += b_->roots(roots + 1, 7);
  ntakes_ = 0;
  for (int r = 0; r < nroots; ++r) {
    char dir[48];
    snprintf(dir, sizeof dir, "%s/Recordings", roots[r]);
    int n = b_ ? b_->list(dir, names, sizes, dirs, stamps, 256) : -1;
    size_t rl = strlen(roots[r]);
    for (int i = 0; i < n; ++i) {
      if (!dirs[i] || names[i][0] == '.' || rl + strlen(names[i]) >= sizeof takes_[0].name) continue;
      char id[sizeof takes_[0].name];
      snprintf(id, sizeof id, "%s%s", roots[r], names[i]);
      int at = ntakes_ < kMaxTakes ? ntakes_++ : kMaxTakes - 1;   // full: the oldest falls out
      if (at == kMaxTakes - 1 && ntakes_ == kMaxTakes && !newer(stamps[i], id, takes_[at].stamp, takes_[at].name)) continue;
      while (at > 0 && newer(stamps[i], id, takes_[at - 1].stamp, takes_[at - 1].name)) takes_[at] = takes_[at - 1], --at;
      strcpy(takes_[at].name, id);
      takes_[at].stamp = stamps[i];
    }
  }
  int sel = 0;
  for (int i = 0; i < ntakes_; ++i)
    if ((prefer && *prefer && !strcmp(takes_[i].name, prefer)) || (!(prefer && *prefer) && *was && !strcmp(takes_[i].name, was))) sel = i;
  if (!ntakes_ || strcmp(takes_[sel].name, was)) resume_at_ = 0;   // a paused position belongs to its take
  if (active() && *was && !strcmp(takes_[sel].name, was)) take_i_ = sel;   // the one playing stays
  else if (ntakes_) select_take(sel);
  else {
    nch_ = 0, take_i_ = 0;
    if (!active()) state_ = kNone;
  }
  return ntakes_;
}

void Player::load_channels() {
  static char names[64][64];
  static uint64_t sizes[64];
  static bool dirs[64];
  static uint32_t stamps[64];
  nch_ = 0;
  char path[160];
  take_folder(path, sizeof path, takes_[take_i_].name);
  int n = b_->list(path, names, sizes, dirs, stamps, 64);
  for (int i = 0; i < n && nch_ < kMaxChannels; ++i) {
    const char* f = names[i];
    unsigned l = (unsigned)strlen(f);
    if (dirs[i] || l < 8 || l >= sizeof ch_[0].file || f[0] < '0' || f[0] > '9' || f[1] < '0' || f[1] > '9' || f[2] != '_') continue;
    if (strcmp(f + l - 4, ".wav") && strcmp(f + l - 4, ".WAV")) continue;
    if (sizes[i] < recwav::kHeader) continue;
    Chan c;
    strcpy(c.file, f);
    unsigned k = l - 7 < sizeof c.name - 1 ? l - 7 : sizeof c.name - 1;
    memcpy(c.name, f + 3, k);
    c.name[k] = 0;
    c.nn = (f[0] - '0') * 10 + f[1] - '0';
    c.size = sizes[i];
    int at = nch_++;
    while (at > 0 && ch_[at - 1].nn > c.nn) ch_[at] = ch_[at - 1], --at;
    ch_[at] = c;
  }
  ch_i_ = 0;   // MASTER (00) when recorded: it sorts first; else the first recorded after it
}

bool Player::select_take(int i) {
  if (!ntakes_) return false;
  i = i < 0 ? 0 : i >= ntakes_ ? ntakes_ - 1 : i;
  if (i == take_i_ && nch_) return false;
  if (active()) stop();
  resume_at_ = 0;
  take_i_ = i;
  load_channels();
  if (!active()) {
    state_ = kStopped;
    probe();
  }
  return true;
}

bool Player::select_channel(int i) {
  if (!nch_) return false;
  i = i < 0 ? 0 : i >= nch_ ? nch_ - 1 : i;
  if (i == ch_i_) return false;
  ch_i_ = i;
  if (active()) {
    pause_req_ = false;
    resume_ = true;   // poll() opens it at the frame the fade ended on
    stop_req_ = true;
  } else probe();
  return true;
}

bool Player::select_by_name(const char* take, int nn) {
  for (int t = 0; t < ntakes_; ++t)
    if (!strcmp(takes_[t].name, take)) {
      if (t != take_i_ || !nch_) {
        take_i_ = t == 0 ? 1 : 0;   // force a reload
        select_take(t);
      }
      for (int c = 0; c < nch_; ++c)
        if (ch_[c].nn == nn) select_channel(c);
      return true;
    }
  return false;
}

// ---- the file

static bool parse(const uint8_t* h, uint64_t size, uint32_t* align, uint32_t* width, uint32_t* bits, uint64_t* data) {
  uint32_t a = recwav::ours(h);
  if (!a) return false;
  *align = a;
  *width = recwav::get16(h + 22);
  *bits = recwav::get16(h + 34);
  uint64_t d = recwav::get32(h + recwav::kHeader - 4), have = size - recwav::kHeader;
  if (d > have) d = have;   // a take cut short (the placeholder): what the file holds
  *data = d / a * a;
  return true;
}

bool Player::probe() {
  len_ = 0, width_ = bits_ = 0;
  if (!nch_) return false;
  char path[200];
  take_folder(path, sizeof path, takes_[take_i_].name);
  snprintf(path + strlen(path), sizeof path - strlen(path), "/%s", ch_[ch_i_].file);
  int h = b_->open(path);
  if (h < 0) return false;
  uint8_t hd[recwav::kHeader];
  bool ok = b_->read(h, hd, sizeof hd) == (int)sizeof hd && parse(hd, ch_[ch_i_].size, &align_, &width_, &bits_, &data_);
  b_->close(h);
  if (ok) len_ = data_ / align_;
  return ok;
}

bool Player::open_channel(uint64_t at, char* err, unsigned errlen) {
  close_file();
  Frame x;
  while (ring_.pop(&x)) {   // the mix is idle here: the ring is the reader's alone
  }
  char path[200];
  take_folder(path, sizeof path, takes_[take_i_].name);
  snprintf(path + strlen(path), sizeof path - strlen(path), "/%s", ch_[ch_i_].file);
  h_ = b_->open(path);
  uint8_t hd[recwav::kHeader];
  if (h_ < 0 || b_->read(h_, hd, sizeof hd) != (int)sizeof hd ||
      !parse(hd, ch_[ch_i_].size, &align_, &width_, &bits_, &data_)) {
    if (err) snprintf(err, errlen, h_ < 0 ? "cannot open %s" : "not one of ours: %s", ch_[ch_i_].file);
    close_file();
    return false;
  }
  len_ = data_ / align_;
  if (at > len_) at = len_;
  read_ = at * align_;
  if (read_ && !b_->seek(h_, recwav::kHeader + read_)) {
    if (err) snprintf(err, errlen, "seek failed");
    close_file();
    return false;
  }
  eof_ = read_ >= data_;
  pos_ = at;
  error_[0] = 0;
  while (!eof_ && ring_.size() < kStartFill && read_piece()) {   // the sound starts on 0.5 s held
  }
  ramp_ = 0;
  under_ = stop_req_ = false;
  __atomic_store_n(&state_, (int)kPlaying, __ATOMIC_RELEASE);
  return true;
}

void Player::close_file() {
  if (h_ >= 0) b_->close(h_);
  h_ = -1;
}

bool Player::read_piece() {
  if (eof_ || h_ < 0) return false;
  uint32_t per = kPiece / align_;
  if (kRingFrames - ring_.size() < per) return false;
  uint64_t left = data_ - read_;
  uint32_t n = left < kPiece ? (uint32_t)left : kPiece;
  uint32_t t0 = now_ ? now_() : 0;
  int got = b_->read(h_, piece_, n);
  uint32_t us = now_ ? now_() - t0 : 0;
  reads_++;
  if (us > worst_read_us_) worst_read_us_ = us;
  if (got <= 0) {   // an error or an early end: what is held plays out
    if (got < 0) fail("read failed");
    eof_ = true;
    return false;
  }
  uint32_t frames = (uint32_t)got / align_;
  const uint8_t* p = piece_;
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t s[2];
    for (uint32_t c = 0; c < width_; ++c) {
      if (bits_ == 24) s[c] = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24), p += 3;
      else s[c] = (int32_t)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 24), p += 2;
    }
    ring_.push(Frame{s[0], width_ == 2 ? s[1] : s[0]});   // room checked above
  }
  read_ += (uint64_t)frames * align_;
  if (read_ >= data_ || (uint32_t)got < n) eof_ = true;
  return true;
}

void Player::fail(const char* why) {
  snprintf(error_, sizeof error_, "%s", why);
  if (b_ && b_->log) {
    char l[160];
    snprintf(l, sizeof l, "play: %s/%s: %s", ntakes_ ? takes_[take_i_].name : "-", nch_ ? ch_[ch_i_].name : "-", why);
    b_->log(l);
  }
}

// ---- control

bool Player::play(char* err, unsigned errlen) {
  if (active()) return true;
  if (!ntakes_ || !nch_) {
    if (err) snprintf(err, errlen, ntakes_ ? "no WAV in %s" : "no take in /Recordings", ntakes_ ? takes_[take_i_].name : "");
    return false;
  }
  resume_ = false;
  uint64_t at = resume_at_;   // paused: on from there
  resume_at_ = 0;
  return open_channel(at, err, errlen);
}

void Player::stop() {
  resume_ = false, pause_req_ = false, resume_at_ = 0;
  if (active()) stop_req_ = true;
}

void Player::pause() {
  if (state_ != kPlaying) return;
  resume_ = false, pause_req_ = true;
  stop_req_ = true;   // the fade out; poll() keeps the frame it ended on
}

void Player::set_level(int lev) {
  level_ = lev < 0 ? 0 : lev > 127 ? 127 : lev;
  float g = level_ / 127.0f;
  gain_ = g * g;
}

void Player::poll() {
  int st = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
  if (st == kPlaying) {
    read_piece();
    return;
  }
  if (st == kStopping || h_ < 0) return;
  // the mix ended (a stop's fade, the take's end, an underrun's fade): the file closes here
  close_file();
  if (under_) {
    underruns_++;
    under_ = false;
    fail("card too slow: faded out");
    resume_ = false;
  }
  if (resume_) {   // a channel change: the new one from the frame the fade ended on
    resume_ = false;
    char e[48];
    if (!open_channel(pos_, e, sizeof e)) fail(e);
    return;
  }
  if (pause_req_) {   // a pause: the stopped state keeps its frame (not at the end: that is a stop)
    pause_req_ = false;
    probe();
    resume_at_ = pos_ + kFade < len_ ? pos_ : 0;
    if (!resume_at_) pos_ = 0;
    return;
  }
  pos_ = 0;
  probe();
}

void Player::status(Status* s) const {
  memset(s, 0, sizeof *s);
  s->state = ntakes_ ? state_ : kNone;
  if (ntakes_) snprintf(s->take, sizeof s->take, "%s", takes_[take_i_].name);
  if (nch_) snprintf(s->channel, sizeof s->channel, "%s", ch_[ch_i_].name);
  snprintf(s->error, sizeof s->error, "%s", error_);
  s->take_i = take_i_, s->takes = ntakes_, s->ch_i = ch_i_, s->channels = nch_;
  s->pos = pos_, s->len = len_;
  s->paused = resume_at_ > 0 || (pause_req_ && active());
  s->level = level_;
  s->underruns = underruns_;
  s->reads = reads_, s->worst_read_us = worst_read_us_;
  s->width = (uint8_t)width_, s->bits = (uint8_t)bits_;
}

// ---- the mix (the OUT fill)

static inline int32_t add_sat(int32_t a, float b) {
  int64_t s = (int64_t)a + (int64_t)b;
  return s > 2147483647 ? 2147483647 : s < -2147483647 - 1 ? (int32_t)0x80000000 : (int32_t)s;
}

void Player::mix(int32_t (*out)[12], unsigned n) {
  int st = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
  if (st != kPlaying && st != kStopping) return;
  if (st == kPlaying && stop_req_) st = kStopping, stop_req_ = false;
  float g = gain_;
  uint64_t pos = pos_;
  for (unsigned f = 0; f < n; ++f) {
    if (st == kPlaying && !eof_ && ring_.size() < kFade + 64) {   // the card fell behind: fade out on what is held
      st = kStopping;
      under_ = true;
    }
    Frame x;
    if (!ring_.pop(&x)) {   // the take's end (or nothing held)
      st = kStopped;
      break;
    }
    if (st == kPlaying) {
      if (ramp_ < kFade) ramp_++;
    } else if (ramp_ > 0) ramp_--;
    float a = g * (float)ramp_ * (1.0f / kFade);
    out[f][0] = add_sat(out[f][0], (float)x.l * a);
    out[f][1] = add_sat(out[f][1], (float)x.r * a);
    pos++;
    if (st == kStopping && ramp_ == 0) {
      st = kStopped;
      break;
    }
  }
  pos_ = pos;
  __atomic_store_n(&state_, st, __ATOMIC_RELEASE);
}

}  // namespace recplay
