// pi/engine/rytmpat_pi.cpp - see rytmpat_pi.h. Hosted (the core's flags): core 3 fetches and decodes, core 0 copies.
#include "rytmpat_pi.h"

#include <string.h>

#include "platform.h"
#include "seq/rytm_dump.h"

namespace rytmpat_pi {

namespace {

namespace dump = core::rytm_dump;

rytm_pattern_t g_buf[2];          // published: core 3 writes g_buf[(n / 2 + 1) & 1], then n += 2
int g_index[2] = {-1, -1};
volatile uint32_t g_n = 0;        // the sequence counter (0: nothing yet)
volatile int g_want = 0;          // core 0 -> core 3: the page (rytm_dump kOff / kPagePattern / kPageVoice)
volatile uint32_t g_at = 0;       // core 3 -> core 0: now_ms of the last record
volatile bool g_pending = false, g_lost = false;
volatile int g_stale = 0;
uint32_t g_timeouts = 0;
bool g_hooked = false;

void on_record(const uint8_t* rec, int n, int pattern) {   // core 3, from rytm_dump::byte
  uint32_t k = g_n;
  int i = (int)((k / 2 + 1) & 1);
  if (rytm_pattern_decode(rec, n, &g_buf[i]) != 0) return;
  g_index[i] = pattern;
  g_at = core::now_ms();
  g_lost = false;
  __atomic_store_n(&g_n, k + 2, __ATOMIC_RELEASE);   // the reader takes g_buf[(n / 2) & 1]
}

}  // namespace

void Tick() {
  if (!g_hooked) g_hooked = true, dump::on_record(on_record);
  int w = g_want;
  if (w != dump::page()) dump::set_page(w);   // turned on: a fetch at once
  uint32_t t = dump::stats().timeouts;
  if (t != g_timeouts) g_timeouts = t, g_lost = true;
  g_pending = dump::pending();
  g_stale = dump::stale();
}

void Want(int page) { g_want = page; }

bool Pattern(rytm_pattern_t* out, int* index, unsigned* seen) {
  for (int tries = 0; tries < 4; ++tries) {
    uint32_t a = __atomic_load_n(&g_n, __ATOMIC_ACQUIRE);
    if (a == 0 || a == *seen) return false;
    memcpy(out, &g_buf[(a / 2) & 1], sizeof *out);
    *index = g_index[(a / 2) & 1];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_n, __ATOMIC_ACQUIRE) - a < 2) {   // not overwritten meanwhile
      *seen = a;
      return true;
    }
  }
  return false;
}

State Get() {
  State s;
  s.age_ms = g_n ? (int)(core::now_ms() - g_at) : -1;
  s.pending = g_pending;
  s.lost = g_lost;
  s.stale = g_stale;
  return s;
}

}  // namespace rytmpat_pi
