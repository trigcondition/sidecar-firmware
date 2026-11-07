// core/samp/sedit_io.cpp - see sedit_io.h.
#include "sedit_io.h"

#if CORE_MSAMP

#include <string.h>
#include <strings.h>

#include "../lib/library.h"
#include "../pool/pcache.h"
#include "../ui/ui.h"
#include "../wavetable/wav_read.h"
#include "dsp.h"

namespace core {
namespace sedit {

namespace {
uint8_t g_stage[cardq::kPiece + 8];                       // the Loader's piece, after at most 7 bytes of a split frame
uint8_t g_out[samp::kWavHeaderMax + cardq::kPiece + 1];   // the Saver's piece (the header first, a pad byte last)
char g_stem[256];                                         // edit_name's stem, for the listing on the card core

enum : uint8_t { kIdle, kOpen, kHead, kData, kClose, kDone };                       // the Loader's
enum : uint8_t { kExists = 1, kRmTmp, kWrite, kShut, kRename, kReport, kUndoShut, kUndoRm, kOver };   // the Saver's

// The length of stem without a trailing "-ed<digits>".
int base_len(const char* stem) {
  int n = (int)strlen(stem), i = n;
  while (i > 0 && stem[i - 1] >= '0' && stem[i - 1] <= '9') --i;
  if (i == n || i < 3 || strncasecmp(stem + i - 3, "-ed", 3)) return n;
  return i - 3;
}

// kFnReq on the card core: the highest k of "<g_stem>-ed<k>.wav" in the folder r->path (0 none, -1 no folder)
void list_fn(cardq::Req* r) {
  File d;
  if (!d.open(r->path, kRead) || !d.isDir()) {
    r->got = -1;
    return;
  }
  int bl = (int)strlen(g_stem), best = 0;
  char nm[256];
  bool dir;
  uint64_t size;
  uint16_t fd, ft;
  while (d.nextEntry(nm, sizeof nm, &dir, &size, &fd, &ft)) {
    int l = (int)strlen(nm);
    if (dir || l < bl + 8 || strncasecmp(nm, g_stem, bl) || strncasecmp(nm + bl, "-ed", 3)) continue;
    if (strcasecmp(nm + l - 4, ".wav")) continue;
    int v = 0, i = bl + 3;
    for (; i < l - 4 && nm[i] >= '0' && nm[i] <= '9' && v < 100000; ++i) v = v * 10 + (nm[i] - '0');
    if (i == bl + 3 || i != l - 4) continue;
    if (v > best) best = v;
  }
  d.close();
  r->got = best;
}

// kFnReq: a piece of the Saver's file (off 0: the first, the file opened for writing first); got len, or -1
void write_fn(cardq::Req* r) {
  if (r->off == 0 && !r->f->open(r->path, kWrite)) {
    r->got = -1;
    return;
  }
  r->got = r->f->write(r->buf, r->len) == r->len ? (int32_t)r->len : -1;
}
// kFnReq: the Saver's file closed (its last bytes on the card); got 1, or 0 the close failed
void close_fn(cardq::Req* r) { r->got = r->f->close() ? 1 : 0; }
// kFnReq: the library told of the saved file, its sha256 in buf and its CRC-32 in off (no second read)
void report_fn(cardq::Req* r) {
  lib::touched(r->path, r->buf, (uint32_t)r->off);
  r->got = 1;
}
}  // namespace

CORE_COLD const char* err_text(int err) {
  static UI_ROM const char* const t[] = {"CAN'T OPEN", "NOT 16/24/32-BIT PCM WAV", "TOO LONG (MAX 87 S)", "NO RAM",
                                         "WRITE FAILED", "NAME TAKEN", "NAME TOO LONG", "BUSY"};
  return err <= kErrOpen && err >= kErrBusy ? t[-err - 1] : "";
}

CORE_COLD void edit_name_from(const char* stem, const int* taken, int ntaken, char* out, int cap) {
  int best = 0;
  for (int i = 0; i < ntaken; ++i)
    if (taken[i] > best) best = taken[i];
  sniprintf(out, cap, "%.*s-ed%d.wav", base_len(stem), stem, best + 1);
}

// src_path's folder length (dl) and its stem (minus -ed<k>) into g_stem; 0, -1 too long, -2 no folder
CORE_COLD static int stem_of(const char* src_path, int* dl) {
  const char* sl = strrchr(src_path, '/');
  if (!sl) return -2;
  *dl = (int)(sl - src_path);
  const char* name = sl + 1;
  const char* dot = strrchr(name, '.');
  int sn = dot ? (int)(dot - name) : (int)strlen(name);
  if (*dl >= cardq::kPathMax || sn >= (int)sizeof g_stem) return -1;
  memcpy(g_stem, name, sn);
  g_stem[sn] = 0;
  g_stem[base_len(g_stem)] = 0;
  return 0;
}
// list_fn's request for the folder of src_path (dl bytes; "/" for the root)
CORE_COLD static void list_req(cardq::Req* r, const char* src_path, int dl) {
  r->fn = list_fn;
  sniprintf(r->path, sizeof r->path, "%.*s", dl ? dl : 1, dl ? src_path : "/");
}
// The name from the listing's highest k (top < 0: no folder)
CORE_COLD static int name_from(const char* src_path, int dl, int top, char* out, int cap) {
  if (top < 0) return -2;
  char nm[300];
  edit_name_from(g_stem, &top, 1, nm, sizeof nm);
  int n = sniprintf(out, cap, "%.*s/%s", dl, src_path, nm);
  return n < 0 || n >= cap ? -1 : 0;
}

CORE_COLD int edit_name(const char* src_path, char* out, int cap) {
  int dl = 0, e = stem_of(src_path, &dl);
  if (e) return e;
  static cardq::Req r;
  r.kind = cardq::kFnReq, r.f = nullptr, r.buf = nullptr, r.len = 0, r.off = 0, r.got = -1;
  list_req(&r, src_path, dl);
  // One listing, waited for: on the card core when it takes it in time, else taken back and run here
  if (!cardq::post(&r)) cardq::run_here(&r);
  uint32_t t0 = now_us();
  while (!cardq::idle(&r))
    if (now_us() - t0 > cardq::kTakeBackUs && cardq::take_back(&r)) break;
  r.state = cardq::kFree;
  return name_from(src_path, dl, r.got, out, cap);
}

// ---- Namer

CORE_COLD int Namer::begin(const char* src_path, char* out, int cap) {
  if (!cardq::idle(&r_)) return kErrBusy;
  on_ = false;
  if (strlen(src_path) >= sizeof src_) return -1;
  int e = stem_of(src_path, &dl_);
  if (e) return e;
  sniprintf(src_, sizeof src_, "%s", src_path);
  out_ = out, cap_ = cap;
  r_.state = cardq::kFree;
  list_req(&r_, src_, dl_);
  on_ = true;
  return 0;
}

CORE_COLD int Namer::step() {
  if (!on_) return -2;
  char path[cardq::kPathMax];   // (async copies its path argument into the request: kept from begin)
  sniprintf(path, sizeof path, "%s", r_.path);
  if (!cardq::async(&r_, cardq::kFnReq, nullptr, path, 0, nullptr, 0)) return 1;
  on_ = false;
  return name_from(src_, dl_, r_.got, out_, cap_);
}

// ---- Loader

CORE_COLD int Loader::begin(const char* path) {
  if (!cardq::idle(&r_)) return kErrBusy;
  if (strlen(path) >= sizeof path_) return kErrName;
  sniprintf(path_, sizeof path_, "%s", path);
  r_.state = cardq::kFree;
  x = nullptr, frames = rate = 0, ch = bits = 0, blk = -1;
  err_ = 0, pos_ = carry_ = frame_ = 0;
  st_ = kOpen;
  return 0;
}

CORE_COLD int Loader::fail(int err) {   // the file closed next, the block freed, then err
  err_ = err;
  st_ = kClose;
  return 1;
}

CORE_COLD int Loader::step() {
  switch (st_) {
    case kOpen:
      if (!cardq::async(&r_, cardq::kOpenReq, &f_, path_, 0, nullptr, 0)) return 1;
      if (r_.got != 1) return st_ = kDone, err_ = kErrOpen;
      size_ = f_.fileSize() > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)f_.fileSize();   // (known at the open: no card access)
      st_ = kHead;
      return 1;
    case kHead: {   // the first piece: the header (the data read again from data_off, piece by piece)
      ask_ = size_ < cardq::kPiece ? size_ : cardq::kPiece;
      if (!cardq::async(&r_, cardq::kReadReq, &f_, nullptr, 0, g_stage, ask_)) return 1;
      if (r_.got <= 0) return fail(kErrOpen);
      wavread::Info in;
      if (!wavread::parse(g_stage, (uint32_t)r_.got, &in, size_) || in.be || in.format != 1 ||
          (in.bits != 16 && in.bits != 24 && in.bits != 32) || in.ch < 1 || in.ch > 2 || !in.frames)
        return fail(kErrFormat);
      if (in.frames > pcache::kMaxFrames) return fail(kErrLong);
      void* p = nullptr;
      blk = pcache::alloc_private((uint64_t)in.frames * in.ch * 4, &p);
      if (blk < 0) return fail(kErrRam);
      x = (float*)p;
      frames = in.frames, rate = in.rate, ch = (uint8_t)in.ch, bits = (uint8_t)in.bits;
      fb_ = wavread::frame_bytes(in);
      data_off_ = in.data_off, data_len_ = in.frames * fb_;
      st_ = kData;
      return 1;
    }
    case kData: {   // a piece after the carried bytes of a split frame: its whole frames decoded, the rest carried
      ask_ = data_len_ - pos_ < cardq::kPiece ? data_len_ - pos_ : cardq::kPiece;
      if (!cardq::async(&r_, cardq::kReadReq, &f_, nullptr, (uint64_t)data_off_ + pos_, g_stage + carry_, ask_)) return 1;
      if (r_.got != (int32_t)ask_) return fail(kErrOpen);
      wavread::Info in = {};
      in.format = 1, in.ch = ch, in.bits = bits;
      uint32_t n = carry_ + ask_, nf = n / fb_;
      float* o = x + (size_t)frame_ * ch;
      const uint8_t* s = g_stage;
      for (uint32_t i = 0, e = nf * ch; i < e; ++i, s += bits / 8) o[i] = wavread::sample(in, s);
      carry_ = n - nf * fb_;
      memmove(g_stage, g_stage + nf * fb_, carry_);
      frame_ += nf, pos_ += ask_;
      if (pos_ >= data_len_) st_ = kClose;
      return 1;
    }
    case kClose:
      if (!cardq::async(&r_, cardq::kCloseReq, &f_, nullptr, 0, nullptr, 0)) return 1;
      st_ = kDone;
      if (err_ && blk >= 0) pcache::release(blk), blk = -1, x = nullptr;
      return err_;
    default:
      return err_;
  }
}

// ---- Saver

CORE_COLD int Saver::begin(const char* target, const float* x, int ch, int bits, uint32_t rate, uint32_t frames) {
  if (!cardq::idle(&r_)) return kErrBusy;
  st_ = kOver;
  if (strlen(target) + 4 >= sizeof tmp_) return kErrName;
  if ((ch != 1 && ch != 2) || (bits != 16 && bits != 24 && bits != 32)) return kErrFormat;
  sniprintf(target_, sizeof target_, "%s", target);
  sniprintf(tmp_, sizeof tmp_, "%s.tmp", target);
  x_ = x, ch_ = ch, bits_ = bits, rate_ = rate, frames_ = frames;
  pos_ = bytes_ = len_ = 0, err_ = 0, yields = 0;
  waiting_ = filled_ = false;
  r_.state = cardq::kFree;
  st_ = kExists;
  return 0;
}

CORE_COLD int Saver::fail(int err) {   // the .tmp closed and removed next, then err
  err_ = err;
  st_ = kUndoShut;
  return 1;
}

CORE_COLD int Saver::step() {
  switch (st_) {
    case kExists:
      if (!cardq::async(&r_, cardq::kExistsReq, nullptr, target_, 0, nullptr, 0)) return 1;
      if (r_.got != 0) return st_ = kOver, kErrExists;
      st_ = kRmTmp;
      return 1;
    case kRmTmp:   // a .tmp left by a save cut short
      if (!cardq::async(&r_, cardq::kRemoveReq, nullptr, tmp_, 0, nullptr, 0)) return 1;
      if (r_.got != 1) return st_ = kOver, kErrWrite;
      sh_.init();
      crc_ = 0;
      st_ = kWrite;
      return 1;
    case kWrite: {
      if (!filled_) {
        if (cardq::busy()) {   // a stream needs the card: wait (at most kUploadWaitMs a piece)
          uint32_t now = now_ms();
          if (!waiting_) waiting_ = true, wait0_ = now;
          if (now - wait0_ < cardq::kUploadWaitMs) return ++yields, 1;
        }
        waiting_ = false;
        uint8_t* o = g_out;
        if (bytes_ == 0) o += samp::wav_header(g_out, ch_, bits_, rate_, frames_);
        uint32_t fb = (uint32_t)ch_ * (bits_ / 8), nf = cardq::kPiece / fb;
        if (nf > frames_ - pos_) nf = frames_ - pos_;
        const float* s = x_ + (size_t)pos_ * ch_;
        for (uint32_t i = 0, e = nf * ch_; i < e; ++i, o += bits_ / 8) {
          if (bits_ == 16) samp::put16(o, s[i]);
          else if (bits_ == 24) samp::put24(o, s[i]);
          else samp::put32(o, s[i]);
        }
        pos_ += nf;
        if (pos_ >= frames_ && (samp::wav_data_bytes(ch_, bits_, frames_) & 1)) *o++ = 0;   // RIFF's pad byte
        len_ = (uint32_t)(o - g_out);
        sh_.update(g_out, len_);
        crc_ = lib::crc32(crc_, g_out, len_);
        filled_ = true;
        r_.fn = write_fn;
      }
      if (!cardq::async(&r_, cardq::kFnReq, &f_, tmp_, bytes_, g_out, len_)) return 1;
      filled_ = false;
      if (r_.got != (int32_t)len_) return fail(kErrWrite);
      bytes_ += len_;
      if (pos_ >= frames_) st_ = kShut;
      return 1;
    }
    case kShut:
      r_.fn = close_fn;
      if (!cardq::async(&r_, cardq::kFnReq, &f_, nullptr, 0, nullptr, 0)) return 1;
      if (r_.got != 1) return err_ = kErrWrite, st_ = kUndoRm, 1;
      st_ = kRename;
      return 1;
    case kRename:   // (refused when the target appeared meanwhile: never over a file that is there)
      if (!cardq::async(&r_, cardq::kRenameReq, nullptr, tmp_, 0, (uint8_t*)target_, 0)) return 1;
      if (r_.got != 1) return err_ = kErrWrite, st_ = kUndoRm, 1;
      sh_.final(sha_);
      st_ = kReport;
      return 1;
    case kReport:
      r_.fn = report_fn;
      if (!cardq::async(&r_, cardq::kFnReq, nullptr, target_, crc_, sha_, 32)) return 1;
      st_ = kOver;
      return 0;
    case kUndoShut:
      r_.fn = close_fn;
      if (!cardq::async(&r_, cardq::kFnReq, &f_, nullptr, 0, nullptr, 0)) return 1;
      st_ = kUndoRm;
      return 1;
    case kUndoRm:
      if (!cardq::async(&r_, cardq::kRemoveReq, nullptr, tmp_, 0, nullptr, 0)) return 1;
      st_ = kOver;
      return err_;
    default:
      return err_;
  }
}

}  // namespace sedit
}  // namespace core

#endif
