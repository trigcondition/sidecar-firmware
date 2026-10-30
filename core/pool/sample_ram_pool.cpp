// The sample RAM (sample_ram_pool.h, docs/psram.md). Not real time: the
// loader runs between requests, from flash (CORE_COLD), as the card code.
#include "sample_ram_pool.h"

#include <new>

#if CORE_MSAMP
#include "../lib/library.h"
#include "pcache.h"
#include "pool.h"
#endif

namespace rytm {

#if CORE_MSAMP
// ---- the Pi / Mac: DSP SAMPLE's view of the pool RAM (core/pool/pcache.h)
namespace {
CORE_COLD bool path_of(card::Index* cards, int folder, int file, char* out, int cap) {
  if (folder <= SampleRamPool::kSlotKey) {
    int line = SampleRamPool::kSlotKey - folder;
    if (line < 0 || line >= core::kPool || core::pool[line].path[0] != '/') return false;
    return sniprintf(out, cap, "%s", core::pool[line].path) < cap;
  }
  card::FileRec r;
  if (folder < 0 || !cards || !cards->file(folder, file, &r)) return false;
  const char* parts[16];   // the folder's names up to /Samples (folder 0)
  int np = 0;
  for (int f = folder; f > 0 && np < 16; f = cards->folder(f).parent) parts[np++] = cards->folder(f).name;
  int n = sniprintf(out, cap, "/Samples");
  for (int i = np - 1; i >= 0 && n < cap; --i) n += sniprintf(out + n, cap - n, "/%s", parts[i]);
  return n < cap && sniprintf(out + n, cap - n, "/%s", r.name) < cap - n;
}
CORE_COLD bool ref_of(card::Index* cards, int folder, int file, core::lib::Ref* r, char* path, int cap) {
  if (!path_of(cards, folder, file, path, cap)) return false;
  core::lib::ref_for_path(path, r);   // (unknown to the library yet: by its path)
  return true;
}
}  // namespace

CORE_COLD SampleRamPool* SampleRamPool::create(card::Index* cards, card::log_fn log) {
  if (!core::sample_ram_total()) {   // sampleram=0: DSP SAMPLE streams only, as before
    log(CORE_STR("sample RAM: off (DSP SAMPLE streams from the card, MOD ignored)"));
    return nullptr;
  }
  core::pcache::begin(core::pool_ram_budget());   // (the first to start takes the pool RAM)
  SampleRamPool* p = new SampleRamPool();
  p->cards_ = cards, p->log_ = log, p->ok_ = true;
  p->pluck_ = new float[kVoices * SampleModes::kPluckLen];
  memset(p->pluck_, 0, sizeof(float) * kVoices * SampleModes::kPluckLen);
  for (int i = 0; i < 2 * kVoices; ++i) p->pins_[i].on = false;
  log(CORE_STR("sample RAM: DSP SAMPLE reads the pool RAM (the shared sample cache)"));
  return p;
}
bool SampleRamPool::get(int folder, int file, const int16_t** pcm, uint32_t* frames, float* rate, bool* cut, int* ch) {
  char path[core::kPath + 16];
  core::lib::Ref r;
  if (!core::pcache::ok() || !ref_of(cards_, folder, file, &r, path, sizeof path)) return false;
  int e = core::pcache::find(r, core::pcache::kWhole);
  core::pcache::Info in;
  if (e < 0 || !core::pcache::get(e, &in)) return false;
  core::pcache::touch(e);
  *pcm = in.pcm, *frames = in.frames, *rate = in.rate, *cut = in.cut;
  if (ch) *ch = in.ch;
  return true;
}
CORE_COLD void SampleRamPool::want(int folder, int file, bool prefetch) {
  char path[core::kPath + 16];
  core::lib::Ref r;
  if (!core::pcache::ok() || !ref_of(cards_, folder, file, &r, path, sizeof path)) return;
  if (core::pcache::want(r, path, prefetch) >= 0 && !prefetch) ++loads_;
}
CORE_COLD void SampleRamPool::clear_prefetch() {}
CORE_COLD void SampleRamPool::pin(int folder, int file) {
  char path[core::kPath + 16];
  core::lib::Ref r;
  if (!ref_of(cards_, folder, file, &r, path, sizeof path)) return;
  int e = core::pcache::find(r, core::pcache::kWhole);
  core::pcache::Info in;
  if (e < 0 || !core::pcache::get(e, &in)) return;
  for (Pin& p : pins_)
    if (!p.on) {
      p.on = true, p.key = key(folder, file), p.blk = in.blk;
      core::pcache::pin(in.blk);
      return;
    }
}
CORE_COLD void SampleRamPool::unpin(int folder, int file) {
  uint32_t k = key(folder, file);
  for (Pin& p : pins_)
    if (p.on && p.key == k) {
      p.on = false;
      core::pcache::unpin(p.blk);
      return;
    }
}
CORE_COLD bool SampleRamPool::step() { return core::pcache::step(); }
uint32_t SampleRamPool::arena_bytes() const {
  uint64_t b = core::pcache::stats().budget;
  return b > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)b;
}
uint32_t SampleRamPool::used_bytes() const {
  uint64_t b = core::pcache::stats().used;
  return b > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)b;
}

#else

CORE_COLD SampleRamPool* SampleRamPool::create(card::Index* cards, card::log_fn log) {
  uint32_t total = core::sample_ram_total();
  if (!total) {
    log(CORE_STR("PSRAM: none (DSP SAMPLE streams from the card, MOD ignored)"));
    return nullptr;
  }
  void* m = core::sample_ram_alloc(sizeof(SampleRamPool));   // itself in that memory (no RAM spent without it)
  if (!m) return nullptr;
  SampleRamPool* p = new (m) SampleRamPool();
  if (p->begin(cards, log, total)) return p;
  p->~SampleRamPool();
  core::sample_ram_free(m);
  return nullptr;
}

CORE_COLD bool SampleRamPool::begin(card::Index* cards, card::log_fn log, uint32_t total) {
  cards_ = cards;
  log_ = log;
  ok_ = false;
  busy_ = false;
  nq_ = 0;
  loads_ = max_step_us_ = 0;
  for (int i = 0; i < SampleRam::kMax; ++i) meta_[i].id = -1;
  uint32_t extra = kChunk + 64 + kVoices * SampleModes::kPluckLen * sizeof(float);
  if (total <= kReserve + extra + (1u << 20)) {
    log_(CORE_STR("PSRAM: %lu MB, too small for samples"), (unsigned long)(total >> 20));
    return false;
  }
  uint32_t arena = total - kReserve - extra - sizeof(SampleRamPool) - 8192;   // this object, the allocator's own
  uint8_t* base = (uint8_t*)core::sample_ram_alloc(arena + extra);
  if (!base) {
    log_(CORE_STR("PSRAM: %lu MB found, %lu KB not available"), (unsigned long)(total >> 20),
         (unsigned long)((arena + extra) >> 10));
    return false;
  }
  bounce_ = base;
  pluck_ = (float*)(base + kChunk + 64);
  memset(pluck_, 0, kVoices * SampleModes::kPluckLen * sizeof(float));
  // blocks on 32-byte cache lines, packed: samples start at all sorts of
  // cache sets (4 KB-aligned blocks would put the same stretch of eight
  // samples on the same sets of the 4-way, 8 KB-a-way D-cache; samplebench.py)
  uint8_t* a = base + extra;
  uint32_t skew = (32 - ((uintptr_t)a & 31)) & 31;
  ram_.Init(a + skew, arena - skew, 32);
  ok_ = true;
  log_(CORE_STR("PSRAM: %lu MB, %lu KB for samples, %lu KB kept for later"), (unsigned long)(total >> 20),
       (unsigned long)(ram_.size() >> 10), (unsigned long)(kReserve >> 10));
  return true;
}

SampleRamPool::Meta* SampleRamPool::meta(uint32_t k) {
  for (int i = 0; i < SampleRam::kMax; ++i)
    if (meta_[i].id >= 0 && meta_[i].key == k) {
      if (!ram_.Get(meta_[i].id)) {   // its block went (not expected: evictions are forgotten)
        meta_[i].id = -1;
        return nullptr;
      }
      return &meta_[i];
    }
  return nullptr;
}

CORE_COLD void SampleRamPool::fail(uint32_t k) {
  if (nfailed_ < kFailed) failed_[nfailed_++] = k;
}

bool SampleRamPool::failed(uint32_t k) const {
  for (int i = 0; i < nfailed_; ++i)
    if (failed_[i] == k) return true;
  return false;
}

// Drop the prefetch loading now (a want, or a new project, goes first).
CORE_COLD void SampleRamPool::abort_prefetch() {
  if (!busy_ || !job_pre_) return;
  Meta* m = meta(job_key_);
  if (m) {
    ram_.Unpin(m->id);
    ram_.Free(m->id);
    m->id = -1;
  }
  file_.close();
  busy_ = false;
}

CORE_COLD SampleRamPool::Meta* SampleRamPool::meta_new(uint32_t k) {
  for (int i = 0; i < SampleRam::kMax; ++i)
    if (meta_[i].id < 0) {
      meta_[i].key = k;
      meta_[i].ready = false;
      return &meta_[i];
    }
  return nullptr;
}

CORE_COLD void SampleRamPool::forget(uint32_t k) {
  Meta* m = meta(k);
  if (m) m->id = -1;
}

// In the render path's reach (a trig): RAM1 code, no card.
bool SampleRamPool::get(int folder, int file, const int16_t** pcm, uint32_t* frames, float* rate, bool* cut, int* ch) {
  if (!ok_) return false;
  if (ch) *ch = 1;
  Meta* m = meta(key(folder, file));
  if (!m || !m->ready) return false;
  ram_.Touch(m->id, core::now_ms());
  *pcm = (const int16_t*)ram_.Data(m->id);
  *frames = m->frames;
  *rate = m->rate;
  *cut = m->cut;
  return true;
}

CORE_COLD void SampleRamPool::want(int folder, int file, bool prefetch) {
  if (!ok_ || folder < 0) return;
  uint32_t k = key(folder, file);
  if (failed(k)) return;
  Meta* m = meta(k);
  if (m) {
    if (m->ready) ram_.Touch(m->id, core::now_ms());
    return;   // resident, or loading
  }
  if (!prefetch && busy_ && job_pre_) {   // a voice waits: it goes first, the prefetch later
    uint32_t again = job_key_;
    abort_prefetch();
    if (nq_ < kQueue) {
      queue_[nq_] = again;
      queue_pre_[nq_++] = true;
    }
  }
  for (int i = 0; i < nq_; ++i)
    if (queue_[i] == k) {
      if (prefetch || !queue_pre_[i]) return;
      memmove(&queue_[i], &queue_[i + 1], (nq_ - i - 1) * sizeof queue_[0]);   // a want now: to the front
      memmove(&queue_pre_[i], &queue_pre_[i + 1], (nq_ - i - 1) * sizeof queue_pre_[0]);
      --nq_;
      break;
    }
  if (prefetch) {
    if (nq_ < kQueue) {
      queue_[nq_] = k;
      queue_pre_[nq_++] = true;
    }
    return;
  }
  if (nq_ == kQueue) --nq_;   // full: the last (a prefetch, or the oldest want) goes
  memmove(&queue_[1], &queue_[0], nq_ * sizeof queue_[0]);
  memmove(&queue_pre_[1], &queue_pre_[0], nq_ * sizeof queue_pre_[0]);
  queue_[0] = k;
  queue_pre_[0] = false;
  ++nq_;
}

CORE_COLD void SampleRamPool::clear_prefetch() {
  abort_prefetch();
  nfailed_ = 0;
  int j = 0;
  for (int i = 0; i < nq_; ++i)
    if (!queue_pre_[i]) queue_[j] = queue_[i], queue_pre_[j++] = false;
  nq_ = j;
}

CORE_COLD void SampleRamPool::pin(int folder, int file) {
  Meta* m = ok_ ? meta(key(folder, file)) : nullptr;
  if (m) ram_.Pin(m->id);
}

CORE_COLD void SampleRamPool::unpin(int folder, int file) {
  Meta* m = ok_ ? meta(key(folder, file)) : nullptr;
  if (m) ram_.Unpin(m->id);
}

// Open the file and make room: one step.
CORE_COLD bool SampleRamPool::start(uint32_t k, bool prefetch) {
  if (meta(k)) return false;   // there already (or loading)
  int folder = k >> 16, file = k & 0xffff;
  if (!cards_->file(folder, file, &rec_) || !cards_->open(folder, rec_, &file_)) {
    log_(CORE_STR("PSRAM: cannot open folder %d file %d"), folder, file);
    fail(k);
    return true;
  }
  bpf_ = (uint32_t)rec_.channels * (rec_.bits / 8);
  uint32_t frames = bpf_ ? rec_.data_len / bpf_ : 0;
  uint32_t fit = ram_.size() / 2 - 1;   // a sample longer than the arena: its first part
  want_frames_ = frames < kMaxFrames ? frames : kMaxFrames;
  if (want_frames_ > fit) want_frames_ = fit;
  Meta* m = meta_new(k);
  // 16- and 24-bit PCM: 32-bit files may be float (the index keeps no format
  // tag), so they stream only, as before; at most 8 channels (the carry)
  if (want_frames_ < 2 || !(rec_.bits == 16 || rec_.bits == 24) || rec_.channels > 8 || !m) {
    if (m) fail(k);
    file_.close();
    return true;
  }
  uint32_t ev[SampleRam::kMax];
  int nev = 0;
  int id = ram_.Alloc(k, (want_frames_ + 1) * 2, core::now_ms(), ev, SampleRam::kMax, &nev, !prefetch);
  for (int i = 0; i < nev; ++i) forget(ev[i]);
  if (id < 0) {
    if (!prefetch) {   // not tried again before the next project
      log_(CORE_STR("PSRAM: no room for %s (%lu KB; samples playing hold the rest)"), rec_.name,
           (unsigned long)((want_frames_ * 2) >> 10));
      fail(k);
    }
    file_.close();
    return true;
  }
  m->id = id;
  m->frames = 0;
  m->rate = (float)rec_.rate;
  ram_.Pin(id);   // not evicted half loaded
  dst_ = (int16_t*)ram_.Data(id);
  job_key_ = k;
  job_pre_ = prefetch;
  src_off_ = out_frames_ = carry_ = 0;
  t0_ = core::now_ms();
  busy_ = true;
  if (nev) log_(CORE_STR("PSRAM: %d evicted for %s"), nev, rec_.name);
  return true;
}

CORE_COLD void SampleRamPool::finish() {
  Meta* m = meta(job_key_);
  busy_ = false;
  file_.close();
  if (!m) return;
  ram_.Unpin(m->id);
  if (out_frames_ < 2) {
    ram_.Free(m->id);
    m->id = -1;
    log_(CORE_STR("PSRAM: cannot read %s"), rec_.name);
    return;
  }
  dst_[out_frames_] = dst_[out_frames_ - 1];   // the guard frame
  m->frames = out_frames_;
  m->cut = out_frames_ < rec_.data_len / bpf_;
  m->ready = true;
  ++loads_;
  log_(CORE_STR("PSRAM: %s in (%lu frames%s, %lu KB, %lu ms; %lu of %lu KB used)"), rec_.name, (unsigned long)out_frames_,
       m->cut ? ", cut" : "", (unsigned long)((out_frames_ * 2) >> 10),
       (unsigned long)(core::now_ms() - t0_), (unsigned long)(ram_.used() >> 10), (unsigned long)(ram_.size() >> 10));
}

CORE_COLD bool SampleRamPool::step() {
  if (!ok_) return false;
  uint32_t t = core::now_us();
  if (!busy_) {
    bool did = false;
    while (nq_ > 0 && !did) {
      uint32_t k = queue_[0];
      bool pre = queue_pre_[0];
      memmove(&queue_[0], &queue_[1], (nq_ - 1) * sizeof queue_[0]);
      memmove(&queue_pre_[0], &queue_pre_[1], (nq_ - 1) * sizeof queue_pre_[0]);
      --nq_;
      did = start(k, pre);
    }
    if (!did) return false;
  } else {
    // one read, ending on a sector (the next read whole sectors), as the refills
    uint32_t need = (want_frames_ - out_frames_) * bpf_ - carry_;
    uint32_t left = rec_.data_len - src_off_;
    uint32_t n = kChunk < left ? kChunk : left;
    if (n > need) n = need;
    if (n == kChunk) n -= (rec_.data_offset + src_off_ + n) % 512;
    if (!file_.seekSet(rec_.data_offset + src_off_) || (uint32_t)file_.read(bounce_ + carry_, n) != n) {
      finish();
      return true;
    }
    uint32_t total = carry_ + n, fr = total / bpf_;
    if (fr > want_frames_ - out_frames_) fr = want_frames_ - out_frames_;
    // int16 mono: the first two channels averaged (as the streamed voice);
    // 24- and 32-bit cut to their top 16 bits
    const uint8_t* q = bounce_;
    int16_t* o = dst_ + out_frames_;
    int bytes = rec_.bits / 8, ch = rec_.channels > 1 ? 2 : 1;
    for (uint32_t i = 0; i < fr; ++i, q += bpf_) {
      int32_t a = (int16_t)(q[bytes - 2] | q[bytes - 1] << 8);
      if (ch == 2) a = (a + (int16_t)(q[2 * bytes - 2] | q[2 * bytes - 1] << 8)) >> 1;
      o[i] = (int16_t)a;
    }
    carry_ = total - fr * bpf_;
    if (carry_) memmove(bounce_, bounce_ + fr * bpf_, carry_);
    src_off_ += n;
    out_frames_ += fr;
    if (out_frames_ >= want_frames_ || src_off_ >= rec_.data_len) finish();
  }
  t = core::now_us() - t;
  if (t > max_step_us_) max_step_us_ = t;
  return true;
}

#endif
}  // namespace rytm
