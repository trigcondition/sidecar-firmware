/* plugins/mdkit/tests/pi_window.c - the PI cymbals' delay pages stay inside patch_pi.py's window (0x135000..0x135FFF):
 * mdkit's own (unwindowed) PI-CC, PI-HH, PI-RC under random controls (all 0, all 127, random), random pre-rolls,
 * up to 4 trigs each with random renders and control changes mid-note; after every render the four page pointers
 * (each walked modulo 0x100 inside its 256-word block) are checked. After the review's fz.c (2026-10-27).
 * Usage: pi_window [iterations per voice, default 200]. MIT, Copyright 2026 trigcondition and contributors. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "mdkit.h"
#include "mdkit_pi_cc.h"
#include "mdkit_pi_hh.h"
#include "mdkit_pi_rc.h"
#define WIN0 0x135000u
#define WIN1 0x136000u
static uint32_t r = 12345;
static uint32_t rnd(void) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; }
static int bad = 0;
#define PAGES(T) { const T *s = (const T *)st; uint32_t p[4] = {s->storage.voice.page_a, s->storage.voice.page_b, \
  s->storage.voice.page_c, s->storage.voice.page_d}; for (int i = 0; i < 4; ++i) { \
  if ((p[i] & ~0xffu) < WIN0 || (p[i] | 0xffu) >= WIN1) { if (bad++ < 5) printf("FAIL %s page %d at %x\n", v->name, i, p[i]); } \
  if (p[i] < lo) lo = p[i]; if (p[i] > hi) hi = p[i]; } }
int main(int argc, char **argv) {
  const mdkit_voice *vs[3] = {&mdkit_voice_pi_cc, &mdkit_voice_pi_hh, &mdkit_voice_pi_rc};
  int iters = argc > 1 ? atoi(argv[1]) : 200;
  for (int vi = 0; vi < 3; ++vi) {
    const mdkit_voice *v = vs[vi];
    void *st = calloc(1, v->state_bytes);
    float out[512];
    uint32_t lo = ~0u, hi = 0;
    for (int it = 0; it < iters; ++it) {
      v->init(st);
      int mode = rnd() % 4;
      for (unsigned k = 0; k < v->control_count; ++k) v->set_control(st, k, mode == 0 ? 0 : mode == 1 ? 127 : rnd() % 128);
      for (int pre = rnd() % 3000; pre > 0;) {
        int n = 1 + rnd() % 300;
        if (n > pre) n = pre;
        v->render(st, out, (unsigned)n);
        pre -= n;
        if (rnd() % 4 == 0) v->set_control(st, rnd() % v->control_count, rnd() % 128);
      }
      for (int t = 1 + rnd() % 4; t > 0; --t) {
        v->trigger(st);
        for (int len = rnd() % 40000; len > 0;) {
          int n = 1 + rnd() % 500;
          if (n > len) n = len;
          v->render(st, out, (unsigned)n);
          len -= n;
          if (vi == 0) PAGES(mdkit_pi_cc) else if (vi == 1) PAGES(mdkit_pi_hh) else PAGES(mdkit_pi_rc)
          if (rnd() % 3 == 0) v->set_control(st, rnd() % v->control_count, rnd() % 2 ? (rnd() % 2 ? 0 : 127) : rnd() % 128);
        }
      }
    }
    printf("%s: pages %x..%x (+0xff) over %d runs, inside %x..%x\n", v->name, lo, hi, iters, WIN0, WIN1 - 1);
    free(st);
  }
  printf(bad ? "PI WINDOW: %d FAILED\n" : "PI WINDOW PASS\n", bad);
  return bad != 0;
}
