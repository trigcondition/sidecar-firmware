// pi/ui/host/seqdemo.cpp - the host render's sequencer: a core/seq pattern of its own (core/seq/pattern.cpp, the real
// model) edited only through seq_touch's command lines, the same path the Pi's display takes. seqdemo_build writes the
// demo track (a strummed C minor progression and a melody, humanised); every "seqed=" line the editor sends lands here.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../seq_touch.h"
#include "inst_alloc.h"
#include "machines.h"
#include "seq/rytm_dump.h"

using namespace core::seq;

static Pattern g_p;
static int g_show = 8;   // T9
static unsigned g_gen;
static Track* tf(int rytm, bool create, void*) { return track_of(g_p, rytm, create); }

extern "C" int seqdemo_command(const char* line) {
  if (!strncmp(line, "seqed=show:", 11)) {
    g_show = atoi(line + 11);
    ++g_gen;
    return 1;
  }
  if (!strncmp(line, "seq=swing:", 10)) {   // as core/seq's seq=swing:T:N (T 1..12, N 50..80; 50 straight)
    const char* q = strchr(line + 10, ':');
    Track* t = q ? track_of(g_p, atoi(line + 10) - 1, false) : nullptr;
    if (!t) return 0;
    int v = atoi(q + 1);
    v = v < 50 ? 50 : v > 80 ? 80 : v;
    t->swing = (uint8_t)(v > 50 ? v : 0);
    printf("  -> seq: track %d swing %d%%\n", atoi(line + 10), v);
    return 1;
  }
  if (!strncmp(line, "seq=poly:", 9)) {   // as core/seq's seq=poly:T:K=V,.. (E0; core/inst_alloc.h parse_cfg)
    const char* q = strchr(line + 9, ':');
    Track* t = q ? track_of(g_p, atoi(line + 9) - 1, false) : nullptr;
    char err[80];
    if (!t || !core::inst::parse_cfg(q + 1, &t->poly, err, sizeof err)) return 0;
    printf("  -> seq: track %d poly %d %d %d\n", atoi(line + 9), t->poly.voices, t->poly.alloc, t->poly.unison);
    ++g_gen;
    return 1;
  }
  if (!strncmp(line, "seq=seed:", 9)) {   // as core/seq's seq=seed:free|N (the pattern's seed)
    const char* q = line + 9;
    if (!strcmp(q, "free")) g_p.seed_fixed = 0, g_p.seed = 0;
    else if (*q >= '0' && *q <= '9') g_p.seed_fixed = 1, g_p.seed = (uint16_t)(atoi(q) > 9999 ? 9999 : atoi(q));
    else return 0;
    printf("  -> seq: pattern seed %s %d\n", g_p.seed_fixed ? "fixed" : "FREE", g_p.seed);
    ++g_gen;
    return 1;
  }
  {   // seqtop: the model's own commands, as core 3 applies them (core/seq/xcmd.cpp)
    int xt = -2;
    char xo[200];
    int xr = command_x(g_p, line, &xt, xo, sizeof xo);
    if (xr) {
      printf("  -> %s\n", xo);
      if (xr > 0) ++g_gen;
      return xr > 0;
    }
  }
  char out[120];
  int r = seq_touch::apply(line, g_p.index, tf, nullptr, out, sizeof out);
  printf("  -> %s\n", out);
  if (r > 0) ++g_gen;
  return r;
}

extern "C" void seqdemo_build(void) {
  clear(g_p, 3);   // A04
  seq_touch::reset();
  track_of(g_p, 8, true);
  track_of(g_p, 9, true);
  char l[96];
  // Cm7, Abmaj7 (1st inv), Fm7 (2nd inv), G7 (1st inv): a bar each, struck up on 1, struck down on the "and" of 3
  static const int kRoot[4] = {60, 56, 53, 55}, kChd[4] = {7, 6, 7, 5}, kInv[4] = {0, 1, 2, 1};
  for (int b = 0; b < 4; ++b) {
    snprintf(l, sizeof l, "seqed=chord:8:%d:%d:%d:%d:104:%d:24", b * 16, kRoot[b], kChd[b], kInv[b], 96 * 5);
    seqdemo_command(l);
    snprintf(l, sizeof l, "seqed=chord:8:%d:%d:%d:%d:84:%d:0", b * 16 + 10, kRoot[b], kChd[b], kInv[b], 96 * 3);
    seqdemo_command(l);
    snprintf(l, sizeof l, "seqed=strum:8:%d:%d:-16", b * 16 + 10, b * 16 + 10);
    seqdemo_command(l);
  }
  // the melody on top
  static const int kMel[4][4][3] = {{{2, 72, 96}, {6, 75, 80}, {13, 74, 110}, {15, 72, 64}},
                                    {{3, 75, 100}, {6, 74, 70}, {12, 72, 116}, {14, 70, 60}},
                                    {{2, 72, 90}, {7, 75, 84}, {12, 77, 120}, {14, 75, 70}},
                                    {{3, 74, 104}, {6, 71, 76}, {12, 74, 112}, {15, 75, 92}}};
  for (int b = 0; b < 4; ++b)
    for (int k = 0; k < 4; ++k) {
      snprintf(l, sizeof l, "seqed=add:8:%d:%d:%d:%d:%d", b * 16 + kMel[b][k][0], kMel[b][k][1], k % 2 ? 6 : 0,
               kMel[b][k][2], k == 2 ? 144 : 60);
      seqdemo_command(l);
    }
  seqdemo_command("seqed=human:8:0:63:3:8:11");
  // seq-ux: parameter locks on a few steps (the lock lane's heatmap): 1, 3 or 6 locks
  {
    Track* t = track_of(g_p, 8, false);
    static const int kL[6][2] = {{2, 1}, {10, 3}, {13, 6}, {26, 2}, {42, 6}, {50, 1}};
    for (auto& x : kL)
      for (int k = 0; k < x[1]; ++k) set_lock(t->step[x[0]], k, (40 + 10 * k) << 7);
    // T4: trig conditions on two steps (the ruler's marks): FILL on 7, 50 % on 23
    core::seq::cond_set(t->step[6], core::seq::kCondFill);
    t->step[22].prob = 50;
  }
  // T10: a few notes, so its chip is a track
  seqdemo_command("seqed=add:9:0:36:0:110:180");
}

extern "C" void seqdemo_view(sidecar_seq_view_t* v, int playing) {
  memset(v, 0, sizeof *v);
  v->valid = 1;
  v->track = g_show;
  for (int i = 0; i < SEQED_TRACKS; ++i) v->tracks[i] = g_p.t[i].rytm;
  v->pattern = g_p.index;
  strcpy(v->project, "SHARED SET");
  strcpy(v->machine, "6-OP FM");
  v->length = 64, v->npages = 4;
  v->transport = playing ? 1 : 0;
  v->bpm10 = 1215;
  v->undo = seq_touch::undo_levels(g_show, g_p.index);
  v->redo = seq_touch::redo_levels(g_show, g_p.index);
  v->gen = g_gen;
  seq_touch::to_view(track_of(g_p, g_show), v);
  v->key = g_p.key, v->scale = g_p.scale, v->qnt = g_p.qnt, v->seed = g_p.seed, v->seed_fixed = g_p.seed_fixed;   // seqtop
  const Track* st = track_of(g_p, g_show);
  v->swing = st && st->swing > 50 ? st->swing : 50;
  if (st) {   // E0: as core 3 publishes it - the shown track resolved on its machine (6-OP FM here: its own pool)
    core::inst::Resolved r = core::inst::resolve(st->poly, *core::inst::builtin(core::kFm));
    core::inst::describe_short(r, core::kFm, v->poly_info, sizeof v->poly_info);
  }
}
// tests: Rytm track rytm's poly config (voices, alloc, unison packed as seqed_pi's signature), -1 no such seq track
extern "C" int seqdemo_poly(int rytm) {
  const Track* t = track_of(g_p, rytm);
  return t ? t->poly.voices | t->poly.alloc << 4 | t->poly.unison << 6 : -1;
}
// tests (seqtop): Rytm track rytm's speed | direction << 4 | RND mode << 8, the pattern's key | scale << 4 | QNT << 8
// | fixed << 10 in *pat; step's ratchet count | mode << 8, FX 1 command | value << 8, lock condition (the first)
extern "C" int seqdemo_x(int rytm, int* pat) {
  const Track* t = track_of(g_p, rytm);
  if (pat) *pat = g_p.key | g_p.scale << 4 | g_p.qnt << 8 | g_p.seed_fixed << 10;
  return t ? t->speed | t->dir << 4 | t->rnd_mode << 8 : -1;
}
extern "C" int seqdemo_xstep(int rytm, int step, int* fx, int* lc, int* rnd) {
  const Track* t = track_of(g_p, rytm);
  if (!t || step < 0 || step >= kMaxSteps) return -1;
  const Step& s = t->step[step];
  if (fx) *fx = s.fx[0].cmd | (uint8_t)s.fx[0].val << 8;
  if (lc) *lc = s.nl ? s.lock[0].cond | (s.slide >> (s.lock[0].param & 31) & 1) << 8 : -1;
  if (rnd) *rnd = s.xf & kXfRnd ? 1 : 0;
  return s.rt.cnt | s.rt.mode << 8;
}
extern "C" int seqdemo_seed(void) { return g_p.seed; }
// tests: Rytm track rytm's swing (50 straight), -1 no such seq track
extern "C" int seqdemo_swing(int rytm) {
  const Track* t = track_of(g_p, rytm);
  return t ? (t->swing > 50 ? t->swing : 50) : -1;
}

// tests: the note of Rytm track rytm at step / pitch: its length (0: none), its velocity and micro
extern "C" int seqdemo_note(int rytm, int step, int pitch, int* vel, int* micro) {
  const Track* t = track_of(g_p, rytm);
  if (!t) return 0;
  for (int i = 0; i < t->step[step].n; ++i)
    if (t->step[step].note[i].pitch == pitch) {
      if (vel) *vel = t->step[step].note[i].vel;
      if (micro) *micro = t->step[step].note[i].micro;
      return t->step[step].note[i].len;
    }
  return 0;
}

// The Rytm view's data path on the Pi (pi/engine/rytmpat_pi.cpp): a stock pattern dump (14988 B) through core's own
// reader into the 13101-byte record. 1: good (pattern in *pattern); 0: refused.
extern "C" int seqdemo_rytm_parse(const unsigned char* m, int n, unsigned char* rec, int* pattern) {
  return core::rytm_dump::parse(m, n, rec, pattern) ? 1 : 0;
}
