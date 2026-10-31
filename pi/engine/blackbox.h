// pi/engine/blackbox.h - the black box (2026-10-30, BACKLOG 7z): the whole log, appended to the card as it is logged,
// so that the last seconds before a Rytm link drop or a reboot are still there afterwards. The crash breadcrumb's RAM
// (crumb.h) does not survive a reset on the Pi 4's sdboot path and /log keeps ~25 s; this keeps hours.
// The source is the crumb's log ring (every line any core logs, crumb::Log); core 0's loop takes the bytes logged since
// its last write (take, below) and blackbox_pi.cpp appends them to /.rytm/blackbox.log every ~2 s (sooner when 16 KB
// wait), the SD's streams first. Rotated to blackbox.1.log past kRotate bytes. Lines that fell out of the ring before
// they were written (the card busy for more than ~30 s) are replaced by one "[blackbox: N bytes not kept]" line.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "crumb.h"

namespace blackbox {

constexpr unsigned kRotate = 8u << 20;   // ~2 hours of the log a file; blackbox.1.log the one before
constexpr unsigned kChunk = 8192;        // at most this much a write (and: write now when this much waits)
constexpr unsigned kMargin = 1024;       // the ring's oldest bytes not trusted: a line being logged may be overwriting them

// The bytes logged since *done (a running count, crumb::Area::log_total), at most max of them, into out; *done
// advances past what was taken. Bytes the ring has overwritten since are skipped: their count goes to *lost and the
// copy starts kMargin past the ring's oldest byte (crumb::Log writes a line's bytes before it publishes the count, so
// the oldest kMargin may already be the next line's). A writer racing the copy (another core logging) can overwrite
// the oldest bytes taken; the count is read again after the copy (behind a fence) and that part counts as lost too.
inline unsigned take(const crumb::Area* a, uint64_t* done, char* out, unsigned max, uint64_t* lost) {
  uint64_t tot = __atomic_load_n(&a->log_total, __ATOMIC_ACQUIRE);
  uint64_t from = *done;
  if (from > tot) from = tot;   // (a new boot's ring: never ahead)
  constexpr uint64_t kSafe = crumb::kLog - kMargin;
  if (tot - from > kSafe) {
    *lost += tot - kSafe - from;
    from = tot - kSafe;
  }
  unsigned n = (unsigned)(tot - from < max ? tot - from : max);
  for (unsigned i = 0; i < n; ++i) out[i] = a->log[(from + i) & (crumb::kLog - 1)];
  __atomic_thread_fence(__ATOMIC_SEQ_CST);   // (the byte loads above complete before the count is read again)
  uint64_t tot2 = __atomic_load_n(&a->log_total, __ATOMIC_ACQUIRE);
  if (tot2 - from > kSafe) {   // the oldest part was (or may be being) overwritten while it was copied
    uint64_t bad = tot2 - kSafe - from;
    if (bad >= n) bad = n;
    *lost += bad;
    memmove(out, out + bad, n - (unsigned)bad);
    n -= (unsigned)bad;
    from += bad;
  }
  *done = from + n;
  return n;
}

// When to write: something waits and 2 s passed since the last write, or kChunk waits (the card busy with the SD's
// streams: wait up to 10 s, a full chunk or not, while the ring still holds it)
inline bool due(uint64_t pending, unsigned ms_since_write, bool card_busy) {
  if (pending == 0) return false;
  if (card_busy) return ms_since_write >= 10000 || pending >= crumb::kLog / 2;
  return ms_since_write >= 2000 || pending >= kChunk;
}

}   // namespace blackbox
