// pi/engine/recorder.cpp - see recorder.h.
#include "recorder.h"

#include <stddef.h>
#include <string.h>

#include "rec/rec_wav.h"
#ifdef REC_HOST   // the Mac tests
#include <stdio.h>
#else
#include "core_port.h"   // platform_pi.cpp's integer printf
#define snprintf core_snprintf
#endif

namespace rec {

// ---- the sizes and the warning

unsigned recbuf_mb(unsigned want, unsigned free_mb, unsigned high_mb) {
  if (want != kRecbufAuto) return want > 1024 ? 1024 : want;   // asked: tried as asked (the allocation says if it fits)
  if (!high_mb) return 64;   // one heap for everything (a 1 GB board: the pool and MULTISAMPLE fall back to it): as before
  unsigned room = free_mb > kRecbufKeep ? free_mb - kRecbufKeep : 0;
  want = room / 2;
  if (want < 64) want = 64;
  if (want > 256) want = 256;
  return want < room ? want : room;
}

uint64_t reserve_bytes(uint64_t free, bool known, unsigned files) {
  const uint64_t kMax = 64ull << 20, kMin = 4ull << 20;
  if (!files) return 0;
  uint64_t r = known ? free / 2 / files : 32ull << 20;
  if (r > kMax) r = kMax;
  r &= ~((1ull << 20) - 1);   // whole MB
  return r < kMin ? 0 : r;
}

uint32_t default_write_kbps(const char* root) {
  if (!root || !root[0] || !strncmp(root, "SD", 2)) return 4096;
  if (!strncmp(root, "NVME", 4)) return 51200;
  return 6144;
}

int parse_speeds(const char* t, const char* const* names, int n, uint32_t* kbps) {
  int got = 0;
  while (t && *t) {
    const char* e = t;
    while (*e && *e != '\n') ++e;
    const char* sp = t;
    while (sp < e && *sp != ' ') ++sp;
    for (int i = 0; i < n && sp < e; ++i)
      if ((size_t)(sp - t) == strlen(names[i]) && !strncmp(t, names[i], (size_t)(sp - t))) {
        uint32_t v = 0;
        const char* d = sp + 1;
        bool ok = d < e;
        for (; d < e && *d != '\r'; ++d) {
          if (*d < '0' || *d > '9' || v > 10000000) { ok = false; break; }
          v = v * 10 + (uint32_t)(*d - '0');
        }
        if (ok && v) kbps[i] = v, got++;
      }
    t = *e ? e + 1 : e;
  }
  return got;
}

int format_speeds(char* out, unsigned cap, const char* const* names, int n, const uint32_t* kbps) {
  int k = 0;
  if (cap) out[0] = 0;
  for (int i = 0; i < n; ++i)
    if (kbps[i]) {
      int m = snprintf(out + k, cap - (unsigned)k, "%s %u\n", names[i], (unsigned)kbps[i]);
      if (m < 0 || (unsigned)(k + m) >= cap) break;
      k += m;
    }
  return k;
}

// ---- control

void Recorder::init(void* mem, size_t bytes, const Backend* b) {
  b_ = b;
  size_t st = (size_t)kSources * kChunk;
  if (!mem || bytes < st + (1u << 20)) return;   // off: nothing reserved (or too little)
  stage_ = (uint8_t*)mem;
  ring_ = (int32_t*)(stage_ + st);
  ring_bytes_ = bytes - st;
  configure(mask_, bits_, nullptr, 0);
}

bool Recorder::configure(uint32_t mask, unsigned bits, char* err, unsigned errlen) {
  if (state_ != kIdle) {
    if (err) snprintf(err, errlen, "busy: stop first");
    return false;
  }
  if (bits != 16 && bits != 24) {
    if (err) snprintf(err, errlen, "bits: 16 or 24");
    return false;
  }
  mask_ = mask & ((1u << kSources) - 1);
  bits_ = bits;
  nsamp_ = samples_per_frame(mask_, st_take_);
  side_on_ = false;
  for (int i = kVoice; i < kSources; ++i)
    if (mask_ & (1u << i)) side_on_ = true;
  if (ring_ && nsamp_) {
    slot_words_ = 2 + kBlockFrames * nsamp_;
    nslots_ = (uint32_t)(ring_bytes_ / (slot_words_ * 4));
  } else nslots_ = 0;
  return true;
}

bool Recorder::set_prefix(const char* p) {
  if (p[0] && !valid_prefix(p)) return false;
  memcpy(prefix_, p, strlen(p) + 1);
  return true;
}

// "USB:" -> "USB" (the messages)
static unsigned drive_len(const char* root) {
  unsigned n = (unsigned)strlen(root);
  return n && root[n - 1] == ':' ? n - 1 : n;
}

bool Recorder::set_root(const char* root, char* err, unsigned errlen) {
  if (state_ != kIdle) {
    if (err) snprintf(err, errlen, "busy: stop first");
    return false;
  }
  unsigned n = (unsigned)strlen(root);
  bool ok = n == 0 || (n >= 2 && n < kRootMax - 1 && root[n - 1] == ':');
  for (unsigned i = 0; ok && i + 1 < n; ++i) ok = (root[i] >= 'A' && root[i] <= 'Z') || (root[i] >= '0' && root[i] <= '9');
  if (!ok) {
    if (err) snprintf(err, errlen, "vol: SD or a drive (USB, USB2, USB3, NVME)");
    return false;
  }
  if (strcmp(root_, root) != 0) {
    memcpy(root_, root, n + 1);
    free_ = total_ = 0;   // that volume's, at the next poll
    last_space_us_ = 0;
  }
  return true;
}

void Recorder::rpath(char* out, unsigned cap, const char* rel) const { snprintf(out, cap, "%s%s", root_, rel); }
void Recorder::marker_path(char* out, unsigned cap) const { snprintf(out, cap, "%s/Recordings/%s", root_, kTakeMarker); }

// the lengths into the marker (one write). A failure leaves the card's lengths where they were - shorter, never longer
// than the files' synced data - and is tried again next round; logged once a take
void Recorder::marker_patch() {
  char mk[120];
  marker_path(mk, sizeof mk);
  if (b_->patch(mk, offsetof(Marker, data), mk_.data, sizeof mk_.data)) { mk_dirty_ = false; return; }
  mk_dirty_ = true;
  if (!mk_fails_++ && b_->log) {
    char l[160];
    snprintf(l, sizeof l, "rec: %s%s: the marker's lengths not written (a power cut now cuts the files to an earlier sync)", root_, take_);
    b_->log(l);
  }
}

void Recorder::space_now() {
  free_known_ = true;
  if (!root_[0]) b_->space(&free_, &total_);
  else if (!b_->space_at || !b_->space_at(root_, &free_, &total_)) free_ = total_ = 0;
  else if (free_ == ~0ull) free_ = 0, free_known_ = false;
}

void Recorder::slow(bool ok, uint32_t t0) {
  if (!ok && root_[0] && b_->now_us() - t0 > 1000000u) dead_ = true;   // a timeout (Circle's 3 s a transfer)
}

bool Recorder::begin(bool arm, char* err, unsigned errlen, bool rearm) {
  if (!ring_) {
    if (err) snprintf(err, errlen, "off: no memory reserved (cmdline recbuf=MB)");
    return false;
  }
  if (!mask_) {
    if (err) snprintf(err, errlen, "no channel selected");
    return false;
  }
  if (state_ == kRecording || state_ == kEnded) {
    if (err) snprintf(err, errlen, "a take is running");
    return false;
  }
  if (root_[0] && b_ && b_->ready && !b_->ready(root_)) {   // never the SD instead
    if (err) snprintf(err, errlen, "%.*s drive not mounted", (int)drive_len(root_), root_);
    return false;
  }
  if (state_ == kArmed) {   // armed already: start now (manual), or stay armed
    arm_mode_ = arm;
    if (!arm) go_ = 1;
    return true;
  }
  st_take_ = st_now_;   // the voices on PRE / POST now: recorded in stereo
  configure(mask_, bits_, nullptr, 0);
  wr_ = rd_ = gap_ = 0;
  frame_in_slot_ = zeros_ = 0;
  if (!rearm) {   // re-armed (arm mode, after a take): the counters run on, the last take's figures stay shown
    drops_ = dropped_frames_ = side_under_ = 0;
    last_drops_ = 0;
    frames_ = bytes_ = write_us_ = 0;
    worst_us_ = writes_ = ring_max_ = yields_ = 0;
    take_[0] = 0;
  }
  error_[0] = 0;
  stopping_ = dead_ = false;
  Side x;
  while (side_.pop(&x)) {
  }
  arm_mode_ = arm;
  go_ = arm ? 0 : 1;
  __atomic_store_n(&state_, (int)kArmed, __ATOMIC_RELEASE);   // the producer starts at its next block once go_
  return true;
}

bool Recorder::start(char* err, unsigned errlen) { return begin(false, err, errlen); }
bool Recorder::arm(char* err, unsigned errlen) { return begin(true, err, errlen); }

void Recorder::stop() {
  arm_mode_ = false;
  go_ = 0;
  if (!cas(kArmed, kIdle)) cas(kRecording, kEnded);
}

void Recorder::transport(uint8_t s) { transport_gate(s, arm_mode_, &go_); }

void Recorder::fail(const char* why) {
  if (stopping_) return;   // the first error stays (a dead drive fails every call after it)
  snprintf(error_, sizeof error_, "%s", why);
  if (b_ && b_->log) {
    char l[120];
    snprintf(l, sizeof l, "rec: %s: %s - stopped", take_[0] ? take_ : "-", why);
    b_->log(l);
  }
  arm_mode_ = false;
  go_ = 0;
  if (!cas(kArmed, kIdle)) cas(kRecording, kEnded);
  stopping_ = true;   // what is left in the ring is dropped
}

// ---- producer

static inline uint32_t mag24(int32_t v) { return (uint32_t)(v < 0 ? -(v + 1) : v) >> 8; }   // |v| at 24-bit full scale

void Recorder::meter_peaks(uint32_t out[kMeters]) {
  for (int i = 0; i < kMeters; ++i) out[i] = __atomic_exchange_n(&mpeak_[i], 0u, __ATOMIC_ACQ_REL);
}

void Recorder::in_block(const int32_t (*in)[kInChannels], unsigned n) {
  if (meters_)   // the display's meters: every IN channel, any state (a compare a sample)
    for (int c = 0; c < kInChannels; ++c) {
      uint32_t m = mpeak_[c];
      for (unsigned f = 0; f < n; ++f) {
        uint32_t a = mag24(in[f][c]);
        if (a > m) m = a;
      }
      mpeak_[c] = m;
    }
  int st = __atomic_load_n(&state_, __ATOMIC_ACQUIRE);
  if (st == kArmed) {
    Side x;
    while (side_.size() > 16 * kBlockFrames && side_.pop(&x)) {   // armed: the OUT side kept current, 16 blocks of lead
    }                                                           // (the OUT fill may run just after this IN block)
    for (unsigned f = 0; f < n; ++f)   // the meters of the selected IN channels while waiting
      for (int i = 0; i < kVoice; ++i)
        if (mask_ & (1u << i))
          for (int k = 0; k < source(i).width; ++k) {
            int32_t v = in[f][source(i).ch + k];
            uint32_t a = (uint32_t)(v < 0 ? -(v + 1) : v) >> 8;
            if (a > peak_[i]) peak_[i] = a;
          }
    if (!go_ || !cas(kArmed, kRecording)) return;
    return;   // the take starts with the next block: its OUT side frames are queued after this
  }
  if (st != kRecording || n == 0) return;
  if (n > kBlockFrames) n = kBlockFrames;
  Side side[kBlockFrames];
  if (side_on_)
    for (unsigned f = 0; f < n; ++f)
      if (!side_.pop(&side[f])) {
        memset(&side[f], 0, sizeof side[f]);
        side_under_ = side_under_ + 1;
      }
  uint32_t w = wr_, r = __atomic_load_n(&rd_, __ATOMIC_ACQUIRE);
  if (w - r >= nslots_) {   // the card is behind: this block is dropped (written as silence later)
    gap_ += n;
    drops_ = drops_ + 1;
    dropped_frames_ = dropped_frames_ + n;
    return;
  }
  int32_t* s = slot(w);
  s[0] = (int32_t)gap_;
  s[1] = (int32_t)n;
  gap_ = 0;
  int32_t* o = s + 2;
  for (unsigned f = 0; f < n; ++f)
    for (int i = 0; i < kSources; ++i) {
      if (!(mask_ & (1u << i))) continue;
      const Source& src = source(i);
      const int wd = width(i);
      for (int k = 0; k < wd; ++k) {
        // (a stereo voice's R: the side's R plane)
        int32_t v = src.side ? side[f].s[k && wd > src.width ? kSideR + (i - kVoice) : src.ch + k] : in[f][src.ch + k];
        *o++ = v;
        uint32_t a = (uint32_t)(v < 0 ? -(v + 1) : v) >> 8;
        if (a > peak_[i]) peak_[i] = a;
      }
    }
  __atomic_store_n(&wr_, w + 1, __ATOMIC_RELEASE);
}

static int32_t sat(float x) {
  float y = x * 2147483648.0f;
  return y >= 2147483647.0f ? 2147483647 : y <= -2147483648.0f ? (int32_t)0x80000000 : (int32_t)y;
}

void Recorder::out_block(const int16_t (*v)[kVoices], const float (*fx)[4], unsigned n, const int16_t (*lr)[2 * kVoices],
                         const float (*pairs)[4]) {
  if (meters_)   // the display's meters: the voices as rendered, the FX returns, the PRE / POST pairs
    for (unsigned f = 0; f < n; ++f) {
      for (int i = 0; v && i < kVoices; ++i) {
        uint32_t a = mag24((int32_t)v[f][i] * 65536);
        if (lr && (st_now_ >> i & 1)) {   // a stereo voice: the louder side
          uint32_t b = mag24((int32_t)lr[f][i] * 65536), c = mag24((int32_t)lr[f][kVoices + i] * 65536);
          a = b > c ? b : c;
        }
        if (a > mpeak_[kInChannels + i]) mpeak_[kInChannels + i] = a;
      }
      for (int i = 0; fx && i < 4; ++i) {
        uint32_t a = mag24(sat(fx[f][i]));
        if (a > mpeak_[kInChannels + kVoices + i]) mpeak_[kInChannels + kVoices + i] = a;
      }
      for (int i = 0; pairs && i < 4; ++i) {
        uint32_t a = mag24(sat(pairs[f][i]));
        if (a > mpeak_[kInChannels + kVoices + 4 + i]) mpeak_[kInChannels + kVoices + 4 + i] = a;
      }
    }
  int st = state_;
  if (!side_on_ || (st != kArmed && st != kRecording)) return;
  for (unsigned f = 0; f < n; ++f) {
    Side x;
    for (int i = 0; i < kVoices; ++i) {
      bool two = lr && (st_take_ >> i & 1);   // a stereo voice's file: its L and R as rendered
      x.s[i] = two ? (int32_t)lr[f][i] * 65536 : v ? (int32_t)v[f][i] * 65536 : 0;
      x.s[kSideR + i] = two ? (int32_t)lr[f][kVoices + i] * 65536 : 0;
    }
    for (int i = 0; i < 4; ++i) x.s[kVoices + i] = fx ? sat(fx[f][i]) : 0;
    for (int i = 0; i < 4; ++i) x.s[kVoices + 4 + i] = pairs ? sat(pairs[f][i]) : 0;
    side_.push(x);   // full: only while armed with no IN stream (in_block keeps it drained)
  }
}

// ---- consumer

// the take's folder: "<slug>_<date>[_N]" or "<slug>_take-NNN" (recorder.h), the next free one on the SD and the
// destination (a listing that missed a take never reuses it: mkdir_new says "there already", the next is tried)
bool Recorder::pick_name(char* path, unsigned cap) {
  static char names[2][1024][64];
  static uint64_t sizes[1024];
  static bool dirs[1024];
  char sl[kSlugMax + 1], base[kTakeMax], rel[100];
  if (prefix_[0]) snprintf(sl, sizeof sl, "%s", prefix_);
  else {
    char pn[48] = "";
    if (b_->project) b_->project(pn, sizeof pn);
    slug(pn, sl, sizeof sl);
    if (!sl[0]) snprintf(sl, sizeof sl, "sidecar");
  }
  uint32_t local = 0;
  bool clock = b_->clock && b_->clock(&local);
  if (clock) stamp_name(base, sizeof base, sl, local);
  int next = 1;
  for (int k = 0; k < (root_[0] ? 2 : 1); ++k) {   // on a drive: the SD's takes too (one name on either)
    rpath(path, cap, "/Recordings");
    int n = b_->list(k ? "/Recordings" : path, names[k], sizes, dirs, 1024);
    if (n < (k ? -1 : 0)) return fail("cannot read /Recordings (no take: a name could repeat)"), false;   // (the SD's
    int v = next_number(names[k], n, sl, clock, base);                                                   // may be missing)
    if (v > next) next = v;
  }
  for (int tries = 0;; ++tries, ++next) {
    if (clock) variant(take_, sizeof take_, base, next);
    else seq_name(take_, sizeof take_, sl, next);
    take_dir(rel, sizeof rel, take_);
    rpath(path, cap, rel);
    uint32_t t0 = b_->now_us();
    int m = b_->mkdir_new ? b_->mkdir_new(path) : b_->mkdir(path) ? 1 : -1;
    slow(m >= 0, t0);   // (a drive that hangs here: gone, one wait)
    if (m > 0) return true;
    if (m < 0 || tries == 15) return fail("cannot make the take's folder"), false;   // only "there already" moves on
  }
}

bool Recorder::open_take() {
  char path[160], rel[120];
  abandoned_ = false;
  if (root_[0] && b_->ready && !b_->ready(root_)) {   // pulled while armed
    snprintf(path, sizeof path, "%.*s drive not mounted", (int)drive_len(root_), root_);
    return fail(path), false;
  }
  rpath(path, sizeof path, "/Recordings");
  if (!b_->mkdir(path)) return fail("no /Recordings folder"), false;
  if (!pick_name(path, sizeof path)) return false;
  frames_ = bytes_ = write_us_ = 0;
  worst_us_ = writes_ = ring_max_ = yields_ = 0;
  // the marker: this take until it closes cleanly (recover() reads it); the lengths tracked (0 so far) where space
  // is reserved, else none (the file's size counts)
  Marker& mk = mk_;
  memset(&mk, 0, sizeof mk);
  snprintf(mk.take, sizeof mk.take, "%s", take_);
  bool resv = b_->reserve && b_->truncate;
  for (int i = 0; i < kSources; ++i) mk.data[i] = resv ? 0 : kNoLength;
  mk_fails_ = 0, mk_dirty_ = false;
  marker_path(path, sizeof path);
  {
    int h = b_->open(path);
    bool ok = h >= 0 && b_->write(h, &mk, sizeof mk);
    if (h >= 0) ok = b_->close(h) && ok;
    if (!ok) return fail(root_[0] ? "cannot write to the drive" : "cannot write to the card"), false;
  }
  nout_ = 0;
  open_ = true;   // from here close_take() closes what was opened
  uint8_t h[recwav::kHeader];
  unsigned bps = bits_ / 8;
  uint64_t want = resv ? reserve_bytes(free_, free_known_ && total_ != 0, files(mask_)) : 0;
  uint64_t got = 0;
  int none = 0;
  uint32_t r0 = b_->now_us(), rus = 0;
  for (int i = 0; i < kSources; ++i) {
    if (!(mask_ & (1u << i))) continue;
    Out& o = out_[nout_];
    file_path(rel, sizeof rel, take_, i);
    rpath(path, sizeof path, rel);
    o.h = b_->open(path);
    o.buf = stage_ + (size_t)nout_ * kChunk;
    o.fill = o.done = 0;
    o.data = o.reserved = o.synced = 0;
    o.width = (uint8_t)width(i);
    o.src = (uint8_t)i;
    if (o.h < 0) return fail("cannot create a file"), false;
    nout_++;
    // f_expand walks the FAT (all of it when there is no room): a refusal halves the size for this file and the
    // ones after it, a refusal at the smallest - or kReserveUs spent - ends the reserving for this take
    while (want >= kReserveMin && !o.reserved) {
      if (b_->reserve(o.h, want)) o.reserved = want;
      else want = want / 2 >= kReserveMin ? want / 2 : 0;
      if (b_->now_us() - r0 > kReserveUs) want = 0;
    }
    got += o.reserved;
    if (resv && !o.reserved) mk.data[i] = kNoLength, none++;
    recwav::build(h, o.width, bits_, recwav::placeholder(o.width * bps));
    if (!b_->write(o.h, h, sizeof h)) return fail("write failed"), false;
    // its directory entry now: the reserved chain owned by the file (a power cut before its first round's sync would
    // leave the area's clusters lost), the header on the card
    if (o.reserved && !b_->sync(o.h)) return fail(root_[0] ? "cannot write to the drive" : "cannot write to the card"), false;
  }
  rus = b_->now_us() - r0;
  if (none && resv) {   // the files with no reservation: their size counts (as before) - or the take does not start
    marker_path(path, sizeof path);
    if (!b_->patch(path, 0, &mk, sizeof mk)) return fail(root_[0] ? "cannot write to the drive" : "cannot write to the card"), false;
  }
  last_sync_us_ = b_->now_us();
  char l[240], ids[80];
  list_ids(mask_, ids, sizeof ids);
  snprintf(l, sizeof l, "rec: %s%s started: %d files, %u-bit, channels %s, %u KB/s, ring %u slots (%u ms), %u MB reserved in %u ms%s", root_,
           take_, nout_, bits_, ids, (unsigned)(bytes_per_second(mask_, bits_, st_take_) >> 10), nslots_,
           (unsigned)((uint64_t)nslots_ * kBlockFrames * 1000 / recwav::kRate), (unsigned)(got >> 20), (unsigned)(rus / 1000),
           arm_mode_ ? ", armed by PLAY" : "");
  if (b_->log) b_->log(l);
  return true;
}

bool Recorder::write_out(Out& o) {
  uint32_t n = o.fill - o.done < kPiece ? o.fill - o.done : kPiece;
  uint32_t t = b_->now_us();
  bool ok = !dead_ && b_->write(o.h, o.buf + o.done, n);
  uint32_t us = b_->now_us() - t;
  slow(ok || dead_, t);   // a timeout (Circle's 3 s a transfer): the drive is gone
  if (us > worst_us_) worst_us_ = us;
  write_us_ += us;
  writes_++;
  if (ok) bytes_ += n, o.data += n;   // (a failed write's bytes are not the file's)
  o.done += n;
  if (!ok) fail(root_[0] ? "write failed (drive full or pulled?)" : "write failed (card full?)");
  if (o.done < o.fill && ok) return false;
  o.fill = o.done = 0;
  return true;
}

void Recorder::close_take() {
  uint8_t h[recwav::kHeader];
  bool clean = !dead_;
  // 1. the staging's rest and RIFF's pad byte, each file (a reserved one synced: its tail on the card)
  for (int i = 0; i < nout_; ++i) {
    Out& o = out_[i];
    if (o.h < 0 || dead_) continue;
    while (o.fill && !write_out(o) && !stopping_ && !dead_) {
    }
    uint32_t t0 = b_->now_us();
    if ((o.data & 1) && !dead_) {
      uint8_t z = 0;
      slow(b_->write(o.h, &z, 1), t0);
    }
    if (o.reserved && !dead_) {
      t0 = b_->now_us();
      bool ok = b_->sync(o.h);
      slow(ok, t0);
      if (ok) mk_.data[o.src] = o.data, mk_dirty_ = true;
    }
  }
  // 2. the final lengths into the marker (one write) before any trim: a power cut during the trims and headers cuts
  // each file at its end, not at its last round's sync
  if (mk_dirty_ && !dead_) marker_patch();
  // 3. each file trimmed, its header, closed
  for (int i = 0; i < nout_; ++i) {
    Out& o = out_[i];
    if (o.h < 0) continue;
    if (dead_) {   // nothing more to that drive: its headers are fixed by recover() when it is back
      if (b_->abandon) b_->abandon(o.h);
      else b_->close(o.h);
      o.h = -1;
      o.fill = o.done = 0;
      continue;
    }
    uint32_t t0;
    if (o.reserved && !dead_) {   // the reserved area's rest given back: the file ends with its data
      t0 = b_->now_us();
      bool ok = b_->truncate(o.h);
      slow(ok, t0);
      if (!ok) fail("could not trim a file"), clean = false;   // (the marker stays: recover cuts it)
    }
    recwav::build(h, o.width, bits_, (uint32_t)o.data);
    t0 = b_->now_us();
    bool ok = !dead_ && b_->pwrite(o.h, 0, h, sizeof h);
    slow(ok || dead_, t0);
    if (!ok) fail("header write failed"), clean = false;
    if (dead_) {   // it stopped answering just now: this one let go too
      if (b_->abandon) b_->abandon(o.h);
      else b_->close(o.h);
      o.h = -1;
      continue;
    }
    t0 = b_->now_us();
    ok = b_->close(o.h);
    slow(ok, t0);
    if (!ok) clean = false;
    o.h = -1;
  }
  abandoned_ = dead_;
  clean = clean && !dead_;
  if (clean && b_->remove) {   // closed with its sizes: no recovery needed
    char mk[120];
    marker_path(mk, sizeof mk);
    b_->remove(mk);
  }
  open_ = false;
  uint32_t kbps = write_us_ ? (uint32_t)(bytes_ * 1000000 / write_us_ >> 10) : 0;
  char l[200];
  snprintf(l, sizeof l, "rec: %s%s stopped: %llu frames (%llu.%01llu s), %llu KB in %u writes, %s %u KB/s, worst write %u ms, "
           "ring max %u%%, dropped %u blocks, OUT side short %u%s", root_, take_[0] ? take_ : "-", (unsigned long long)frames_,
           (unsigned long long)(frames_ / recwav::kRate), (unsigned long long)(frames_ % recwav::kRate * 10 / recwav::kRate),
           (unsigned long long)(bytes_ >> 10), writes_, root_[0] ? "drive" : "card", kbps, worst_us_ / 1000,
           nslots_ ? (unsigned)((uint64_t)ring_max_ * 100 / nslots_) : 0, (unsigned)drops_, (unsigned)side_under_,
           dead_ ? ", the drive stopped answering: files left as they are (fixed when it is back)" : "");
  if (b_->log) b_->log(l);
}

void Recorder::poll() {
  if (!b_) return;
  uint32_t now = b_->now_us();
  if (now - last_space_us_ > 10000000u || last_space_us_ == 0) {
    last_space_us_ = now | 1;
    if (state_ != kRecording) space_now();   // f_getfree: not between a take's writes (FatFs keeps
  }                                                          // the count in step itself; ours is estimated below)
  int st = state_;
  if (st == kRecording && !go_) cas(kRecording, kEnded), st = state_;
  if (st != kRecording && st != kEnded) return;
  if (!open_ && !stopping_ && !open_take()) return;
  if (drops_ != last_drops_ && now - last_drop_log_us_ > 1000000u && b_->log) {
    char l[120];
    snprintf(l, sizeof l, "rec: %s%s: the %s is behind: %u blocks dropped (%u frames written as silence)", root_, take_,
             root_[0] ? "drive" : "card", (unsigned)drops_, (unsigned)dropped_frames_);
    b_->log(l);
    last_drops_ = drops_;
    last_drop_log_us_ = now;
  }
  if (dead_) {   // the drive is gone (a write, a sync or a header waited a timeout): the take ends, the staging is
    if (!stopping_) fail("the drive stopped answering (pulled?)");   // dropped (no call waits on it again)
    for (int i = 0; i < nout_; ++i) out_[i].fill = out_[i].done = 0;
  }
  for (int i = 0; i < nout_; ++i)   // one piece of a whole chunk a call
    if (out_[i].fill >= kChunk) {
      uint32_t used = __atomic_load_n(&wr_, __ATOMIC_ACQUIRE) - rd_;
      // the SD's streams first, while the ring has room (another volume has its own bus and FatFs lock)
      if (!root_[0] && b_->card_busy && b_->card_busy() && used < nslots_ / 2) {
        yields_++;
        return;
      }
      write_out(out_[i]);
      return;
    }
  if (stopping_) {   // failed: drop the rest
    __atomic_store_n(&rd_, __atomic_load_n(&wr_, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    zeros_ = 0;
  }
  // into the staging, up to a chunk
  unsigned bps = bits_ / 8;
  bool full = false;
  for (unsigned budget = 8192; budget && !full; --budget) {
    const int32_t* fr = nullptr;
    int32_t* s = nullptr;
    if (zeros_ == 0) {
      uint32_t r = rd_, w = __atomic_load_n(&wr_, __ATOMIC_ACQUIRE);
      if (r == w) break;
      if (w - r > ring_max_) ring_max_ = w - r;
      s = slot(r);
      if (frame_in_slot_ == 0 && s[0]) {
        zeros_ = (uint32_t)s[0];
        s[0] = 0;
      }
    }
    if (zeros_ == 0) fr = s + 2 + frame_in_slot_ * nsamp_;
    if (frames_ * 3 * 2 + kChunk > kMaxData) {   // the next chunk would pass FAT32's 4 GB (the widest file)
      fail("4 GB file limit");
      break;
    }
    unsigned k = 0;
    for (int i = 0; i < nout_; ++i) {
      Out& o = out_[i];
      uint8_t* d = o.buf + o.fill;
      for (int c = 0; c < o.width; ++c) {
        int32_t v = fr ? fr[k++] : 0;
        if (bps == 3) d[0] = (uint8_t)(v >> 8), d[1] = (uint8_t)(v >> 16), d[2] = (uint8_t)(v >> 24), d += 3;
        else d[0] = (uint8_t)(v >> 16), d[1] = (uint8_t)(v >> 24), d += 2;
      }
      o.fill += o.width * bps;
      if (o.fill >= kChunk) full = true;
    }
    frames_++;
    if (zeros_) zeros_--;
    else if (++frame_in_slot_ >= (uint32_t)s[1]) {
      frame_in_slot_ = 0;
      __atomic_store_n(&rd_, rd_ + 1, __ATOMIC_RELEASE);
    }
  }
  if (full) return;
  if (state_ == kEnded && zeros_ == 0 && rd_ == __atomic_load_n(&wr_, __ATOMIC_ACQUIRE)) {
    for (; flush_next_ < nout_; ++flush_next_)   // the partial chunks, one a call
      if (out_[flush_next_].fill) {
        if (write_out(out_[flush_next_])) flush_next_++;
        return;
      }
    flush_next_ = 0;
    if (open_) close_take();
    open_ = false;
    stopping_ = false;
    if (!dead_) space_now();
    last_space_us_ = b_->now_us() | 1;
    if (arm_mode_) {   // the next PLAY makes the next take
      char e[64];
      __atomic_store_n(&state_, (int)kIdle, __ATOMIC_RELEASE);
      uint8_t g = go_;
      if (!begin(true, e, sizeof e, true)) {   // (its drive pulled meanwhile): not armed, and why
        arm_mode_ = false;
        snprintf(error_, sizeof error_, "%s", e);
        return;
      }
      go_ = g;   // a PLAY that came while this take was closing starts the next at once
    } else __atomic_store_n(&state_, (int)kIdle, __ATOMIC_RELEASE);
    return;
  }
  // every 2 s one file's size reaches the directory (readable to there); on a drive every file's every 2 s (a pulled
  // drive loses ~2 s at most: its own bus, and no card queue to wait for)
  uint32_t every = root_[0] && nout_ ? 2000000u / (uint32_t)nout_ : 2000000u;
  if ((int32_t)(now - last_sync_us_) > (int32_t)every && nout_ && !dead_) {   // (signed: the take opened after now)
    last_sync_us_ = now;
    Out& o = out_[sync_next_ % nout_];
    uint64_t d = o.data;
    bool ok = b_->sync(o.h);
    slow(ok, now);
    if (ok && o.reserved && d != o.synced) o.synced = d, mk_.data[o.src] = d, mk_dirty_ = true;   // its length at this sync
    sync_next_++;
    // the lengths into the marker: on the SD after each sync (a file synced every 2 s: a round of 25 files is 50 s), on
    // a drive once a round (every file every 2 s there: one patch, not one each 80 ms)
    if (mk_dirty_ && !dead_ && (!root_[0] || sync_next_ % nout_ == 0)) marker_patch();
  }
}

void Recorder::status(Status* s, bool take_peaks) {
  memset(s, 0, sizeof *s);
  s->state = state_;
  s->arm_mode = arm_mode_;
  s->ready = ring_ != nullptr;
  s->bits = bits_;
  s->mask = mask_;
  if (take_[0]) snprintf(s->take, sizeof s->take, "%s%s", root_, take_);
  memcpy(s->root, root_, sizeof s->root);
  s->abandoned = abandoned_;
  memcpy(s->prefix, prefix_, sizeof s->prefix);
  memcpy(s->error, error_, sizeof s->error);
  s->frames = frames_;
  for (int i = 0; i < kSources; ++i) {
    s->peak[i] = peak_[i];
    if (take_peaks) peak_[i] = 0;
  }
  s->drops = drops_;
  s->dropped_frames = dropped_frames_;
  s->side_under = side_under_;
  s->bytes = bytes_;
  s->write_kbps = write_us_ ? (uint32_t)(bytes_ * 1000000 / write_us_ >> 10) : 0;
  s->worst_write_us = worst_us_;
  s->writes = writes_;
  s->yields = yields_;
  s->ring_slots = nslots_;
  s->ring_max_used = ring_max_;
  s->ring_used = __atomic_load_n(&wr_, __ATOMIC_ACQUIRE) - rd_;
  s->need_kbps = bytes_per_second(mask_, bits_) >> 10;
  s->ring_ms = (uint32_t)((uint64_t)nslots_ * kBlockFrames * 1000 / recwav::kRate);
  uint64_t used = state_ == kRecording || state_ == kEnded ? bytes_ : 0;   // since the last f_getfree
  s->free_bytes = free_ > used ? free_ - used : 0;
  s->free_known = free_known_;
  s->total_bytes = total_;
}

int Recorder::fix_take(const char* path) { return fix_take(path, nullptr, nullptr); }

int Recorder::fix_take(const char* path, const uint64_t* lens, bool* cut_failed) {
  static char names[64][64];
  static uint64_t sizes[64];
  static bool dirs[64];
  int nf = b_->list(path, names, sizes, dirs, 64), fixed = 0;
  for (int f = 0; f < nf; ++f) {
    if (dirs[f] || !is_wav_name(names[f])) continue;
    char fp[280];
    snprintf(fp, sizeof fp, "%s/%s", path, names[f]);
    uint8_t h[recwav::kHeader];
    if (sizes[f] < recwav::kHeader || !b_->read_at(fp, 0, h, sizeof h)) continue;
    uint64_t size = sizes[f];
    const char* n = names[f];
    int id = n[0] >= '0' && n[0] <= '9' && n[1] >= '0' && n[1] <= '9' && n[2] == '_' ? (n[0] - '0') * 10 + n[1] - '0' : -1;
    uint32_t align = recwav::ours(h);
    if (align && !recwav::open_ended(h)) {   // closed with its sizes: they are the truth - a longer file (a trim that
      uint64_t end = recwav::kHeader + recwav::get32(h + recwav::kHeader - 4);   // failed) is cut to them, never
      end += end & 1;                                                             // the header grown over the tail
      if (size > end && !(b_->cut && b_->cut(fp, end)) && cut_failed) *cut_failed = true;
      if (size >= end) continue;
    } else if (lens && align && id >= 0 && id < kSources && lens[id] != kNoLength) {   // its length at the last sync:
      uint64_t d = lens[id] < size - recwav::kHeader ? lens[id] : size - recwav::kHeader;   // the rest is the reserved
      d = d / align * align;                                                                // area's
      uint64_t end = recwav::kHeader + d + (d & 1);
      if (size > end && !(b_->cut && b_->cut(fp, end)) && cut_failed) *cut_failed = true;   // (the marker kept)
      size = recwav::kHeader + d;
    }
    if (recwav::fix(h, size) && b_->patch(fp, 0, h, sizeof h)) {
      fixed++;
      if (b_->log) {
        char m[300];
        snprintf(m, sizeof m, "rec: %s: sizes fixed (%llu bytes: a take cut short)", fp, (unsigned long long)sizes[f]);
        b_->log(m);
      }
    }
  }
  return fixed;
}

int Recorder::fix_all(const char* dir) {
  static char takes[256][64];
  static uint64_t tsz[256];
  static bool tdir[256];
  int nt = b_->list(dir, takes, tsz, tdir, 256), fixed = 0;
  for (int t = 0; t < nt; ++t) {
    if (!tdir[t]) continue;
    char path[200];
    snprintf(path, sizeof path, "%s/%s", dir, takes[t]);
    fixed += fix_take(path);
  }
  return fixed;
}

int Recorder::recover(const char* root) {
  if (!b_) return -1;
  int st = state_;
  if ((st == kRecording || st == kEnded) && strcmp(root, root_) == 0) return -1;   // its marker is the live take's
  char mk[60];
  static Marker m;
  memset(&m, 0, sizeof m);
  snprintf(mk, sizeof mk, "%s/Recordings/%s", root, kTakeMarker);
  bool lens = b_->read_at(mk, 0, &m, sizeof m);
  if (!lens && !b_->read_at(mk, 0, m.take, 40)) return -1;   // an older marker: the name alone
  char* name = m.take;
  name[lens ? sizeof m.take - 1 : 39] = 0;
  bool ok = name[0] != 0;   // a take's name as we make them: no path in it
  for (const char* c = name; *c && ok; ++c)
    ok = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-';
  int n = 0;
  if (ok) {
    char dir[100];
    snprintf(dir, sizeof dir, "%s/Recordings/%s", root, name);
    bool cut_failed = false;
    n = fix_take(dir, lens ? m.data : nullptr, &cut_failed);
    if (cut_failed) {   // a file still longer than its data: the marker stays (the next mount or boot tries again)
      if (b_->log) {
        char l[160];
        snprintf(l, sizeof l, "rec: %s%s: a file could not be cut to its data: the marker kept", root, name);
        b_->log(l);
      }
      return n;
    }
    if (b_->log) {
      char l[160];
      snprintf(l, sizeof l, "rec: %s%s: recovered (it never closed: %d files' sizes fixed)", root, name, n);
      b_->log(l);
    }
  }
  if (b_->remove) b_->remove(mk);
  return n;
}

}  // namespace rec
