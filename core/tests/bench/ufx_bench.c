// core/tests/bench/ufx_bench.c - one user effect's render (plugins/ufx_*: built in with -DUFX_SRC) as the send FX calls it
// on the Pi: 12-frame blocks, wet only, the knobs in b->p, noise in. Built by ufx_bench.sh in the rytm-circle image with
// the Pi's flags (-mcpu=cortex-a72 -O2, as plugin.mk's a64 build); prints the wall time a block on this machine and is
// traced by trace_steps for a72_model.py (the function ufx_call, one call a block).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include UFX_SRC

static rmp3_transport g_tr = {0, 120.0f, 20833.333f, 0.0f, -1, 0.0f, 0, 385};
static float g_in[16], g_l[16], g_r[16], g_p[16];
static rmp3_block g_b;
__attribute__((noinline)) void ufx_call(void *st) {
  const rmp3_plugin *p3 = (const rmp3_plugin *)rmp_entry.reserved[RMP_P_V3];
  p3->render(st, &g_b);
}
int main(int ac, char **av) {
  int blocks = ac > 1 ? atoi(av[1]) : 200000;
  for (int k = 0; k < 4; ++k) g_p[k] = ac > 2 + k ? (float)atof(av[2 + k]) : 64.0f;
  const rmp3_plugin *p3 = (const rmp3_plugin *)rmp_entry.reserved[RMP_P_V3];
  rmp_host h = {3u, 48000.0f, 16};
  if (rmp_entry.init(&h)) return 1;
  void *st = calloc(1, rmp_entry.state_bytes + 64);
  rmp_entry.voice_init(st);
  uint32_t ab = p3->voice_want ? p3->voice_want : p3->voice_min;
  void *arena = ab ? calloc(1, ab) : NULL;
  rmp3_voice v = {NULL, arena, 0, ab, 64, -1, 0, RMP3_AT_SEND};
  if (p3->voice_attach) p3->voice_attach(st, &v);
  g_b.size = sizeof g_b, g_b.n = 12, g_b.flags = 2u, g_b.place = RMP3_AT_SEND;
  g_b.out[0] = g_l, g_b.out[1] = g_r, g_b.in[0] = g_in, g_b.p = g_p, g_b.transport = &g_tr;
  uint32_t r = 1;
  struct timespec t0, t1;
  float sink = 0.0f;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = 0; i < blocks; ++i) {
    for (int j = 0; j < 12; ++j) r ^= r << 13, r ^= r >> 17, r ^= r << 5, g_in[j] = (float)(int32_t)r * (0.3f / 2147483648.0f);
    ufx_call(st);
    sink += g_l[3] + g_r[7];
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / blocks;
  printf("%s: %.3f us a 12-frame block here (P %g %g %g %g; %g)\n", UFX_SRC, us, g_p[0], g_p[1], g_p[2], g_p[3], (double)sink * 0.0);
  return 0;
}
