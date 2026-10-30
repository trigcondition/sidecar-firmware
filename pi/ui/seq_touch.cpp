// pi/ui/seq_touch.cpp - see seq_touch.h. Cold: one command per touch gesture.
#include "seq_touch.h"

#include <string.h>

#include "platform.h"   // sniprintf (newlib's integer printf on the Pi; snprintf on the Mac)
#include "poly.h"

namespace seq_touch {

using core::seq::kMaxSteps;
using core::seq::kNotes;
using core::seq::kSub;
using core::seq::Note;
using core::seq::Step;
using core::seq::Track;

namespace {

enum { kFree = 0, kUndoK = 1, kRedoK = 2 };
struct Entry {
  int kind, rytm, pattern;
  unsigned age;
  Track t;
};
Entry* g_e = nullptr;   // 2 x kUndo, allocated at the first edit
unsigned g_age = 0;

bool pool() {
  if (!g_e) {
    g_e = new Entry[2 * kUndo];
    if (!g_e) return false;
    for (int i = 0; i < 2 * kUndo; ++i) g_e[i].kind = kFree;
  }
  return true;
}
int count(int kind, int rytm, int pattern) {
  int n = 0;
  if (g_e)
    for (int i = 0; i < 2 * kUndo; ++i)
      n += g_e[i].kind == kind && (rytm < 0 || (g_e[i].rytm == rytm && g_e[i].pattern == pattern));
  return n;
}
Entry* newest(int kind, int rytm, int pattern) {
  Entry* b = nullptr;
  if (g_e)
    for (int i = 0; i < 2 * kUndo; ++i) {
      Entry& e = g_e[i];
      if (e.kind == kind && e.rytm == rytm && e.pattern == pattern && (!b || e.age > b->age)) b = &e;
    }
  return b;
}
// A copy of t as the newest entry of that kind (the oldest of that kind dropped when kUndo are kept)
void push(int kind, int rytm, int pattern, const Track& t) {
  if (!pool()) return;
  if (count(kind, -1, 0) >= kUndo) {
    Entry* o = nullptr;
    for (int i = 0; i < 2 * kUndo; ++i)
      if (g_e[i].kind == kind && (!o || g_e[i].age < o->age)) o = &g_e[i];
    o->kind = kFree;
  }
  for (int i = 0; i < 2 * kUndo; ++i)
    if (g_e[i].kind == kFree) {
      Entry& e = g_e[i];
      e.kind = kind, e.rytm = rytm, e.pattern = pattern, e.age = ++g_age;
      memcpy(&e.t, &t, sizeof t);
      return;
    }
}
void drop(int kind, int rytm, int pattern) {
  if (g_e)
    for (int i = 0; i < 2 * kUndo; ++i)
      if (g_e[i].kind == kind && g_e[i].rytm == rytm && g_e[i].pattern == pattern) g_e[i].kind = kFree;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
Note* find(Step& s, int pitch) {
  for (int i = 0; i < s.n; ++i)
    if (s.note[i].pitch == pitch) return &s.note[i];
  return nullptr;
}
// "a:b:c" -> up to n integers; the count read
int ints(const char* p, int* v, int n) {
  int k = 0;
  while (k < n && *p) {   // no strtol: the Pi's hosted side links no newlib libc
    int neg = *p == '-', x = 0;
    if (neg) ++p;
    if (*p < '0' || *p > '9') return -1;
    while (*p >= '0' && *p <= '9' && x < 100000) x = x * 10 + (*p++ - '0');
    v[k++] = neg ? -x : x;
    if (*p == 0) break;
    if (*p != ':') return -1;
    ++p;
  }
  return k;
}
uint32_t g_rnd;
int rnd(int a) {   // -a..a
  g_rnd = g_rnd * 1664525u + 1013904223u;
  return a ? (int)((g_rnd >> 8) % (uint32_t)(2 * a + 1)) - a : 0;
}

}  // namespace

void reset() {
  if (g_e)
    for (int i = 0; i < 2 * kUndo; ++i) g_e[i].kind = kFree;
}
int undo_levels(int rytm, int pattern) { return count(kUndoK, rytm, pattern); }
void save_level(int rytm, int pattern, const Track& t) {
  push(kUndoK, rytm, pattern, t);
  drop(kRedoK, rytm, pattern);
}
bool undo_level(int rytm, int pattern, Track& t) {
  Entry* e = newest(kUndoK, rytm, pattern);
  if (!e) return false;
  push(kRedoK, rytm, pattern, t);
  memcpy(&t, &e->t, sizeof t);
  e->kind = kFree;
  return true;
}
int redo_levels(int rytm, int pattern) { return count(kRedoK, rytm, pattern); }

int apply(const char* line, int pattern, TrackFor track_for, void* param, char* out, int cap) {
  char dummy[4];
  if (!out) out = dummy, cap = sizeof dummy;
  out[0] = 0;
  if (strncmp(line, "seqed=", 6)) return -1;
  const char* p = line + 6;
  static const char* const kOps[] = {"add", "del", "move", "len", "vel", "chord", "strum", "human", "undo", "redo"};
  static const int kArgs[] = {6, 3, 6, 4, 4, 8, 4, 6, 1, 1};
  int op = -1;
  for (int i = 0; i < 10; ++i) {
    size_t n = strlen(kOps[i]);
    if (!strncmp(p, kOps[i], n) && p[n] == ':') op = i, p += n + 1;
  }
  int a[8];
  if (op < 0 || ints(p, a, 8) != kArgs[op]) {
    sniprintf(out, cap, "seqed: bad line \"%s\"", line);
    return -1;
  }
  int rytm = a[0];
  if (rytm < 0 || rytm >= core::seq::kRytmTracks) {
    sniprintf(out, cap, "seqed: track %d", rytm);
    return -1;
  }
  bool adding = op == 0 || op == 5;
  Track* t = track_for(rytm, adding, param);
  if (op == 8 || op == 9) {   // undo / redo: the newest level of this track in this pattern
    Entry* e = newest(op == 8 ? kUndoK : kRedoK, rytm, pattern);
    if (!e) {
      sniprintf(out, cap, "seqed: nothing to %s", op == 8 ? "undo" : "redo");
      return 0;
    }
    if (!t) t = track_for(rytm, true, param);
    if (!t) return 0;
    push(op == 8 ? kRedoK : kUndoK, rytm, pattern, *t);   // the other kind's slots: e stays where it is
    memcpy(t, &e->t, sizeof *t);
    e->kind = kFree;
    sniprintf(out, cap, "seqed: %s T%d (%d left)", op == 8 ? "undo" : "redo", rytm + 1,
             count(op == 8 ? kUndoK : kRedoK, rytm, pattern));
    return 1;
  }
  if (!t) {
    sniprintf(out, cap, "seqed: T%d is not a seq track", rytm + 1);
    return 0;
  }
  int s0 = a[1];
  if (s0 < 0 || s0 >= kMaxSteps) {
    sniprintf(out, cap, "seqed: step %d", s0);
    return -1;
  }
  push(kUndoK, rytm, pattern, *t);   // taken back below when nothing changed
  Step& s = t->step[s0];
  int changed = 0;
  switch (op) {
    case 0: {   // add
      Note n = {(uint8_t)clampi(a[2], 0, 127), (uint8_t)clampi(a[4], 1, 127), (uint16_t)clampi(a[5], 1, kMaxSteps * kSub),
                (int8_t)clampi(a[3], -kSub, kSub), 0};
      changed = core::seq::add_note(s, n);
      break;
    }
    case 1:   // del
      changed = core::seq::remove_pitch(s, a[2]);
      if (changed && !s.n) s.flags = 0, s.chd = 0, s.inv = 0, s.strum = 0;
      break;
    case 2: {   // move
      Note* f = find(s, a[2]);
      int s2 = a[3];
      if (!f || s2 < 0 || s2 >= kMaxSteps) break;
      Note n = *f;
      n.pitch = (uint8_t)clampi(a[4], 0, 127), n.micro = (int8_t)clampi(a[5], -kSub, kSub), n.fresh = 0;
      Step& d = t->step[s2];
      if (s2 != s0 && d.n >= kNotes && !find(d, n.pitch)) break;   // the target is full
      core::seq::remove_pitch(s, a[2]);
      changed = core::seq::add_note(d, n);
      if (!s.n) s.flags = 0, s.chd = 0, s.inv = 0, s.strum = 0;
      break;
    }
    case 3:
    case 4: {   // len / vel
      Note* f = find(s, a[2]);
      if (!f) break;
      int v = op == 3 ? clampi(a[3], 1, kMaxSteps * kSub) : clampi(a[3], 1, 127);
      changed = op == 3 ? f->len != v : f->vel != v;
      if (op == 3) f->len = (uint16_t)v; else f->vel = (uint8_t)v;
      break;
    }
    case 5:   // chord
      core::seq::set_chord(s, clampi(a[2], 0, 127), a[3], a[4], clampi(a[5], 1, 127), clampi(a[6], 1, kMaxSteps * kSub),
                           clampi(a[7], -kSub, kSub));
      changed = s.n > 0;
      break;
    case 6:
    case 7: {   // strum / human over steps s0..a[2]
      int s1 = clampi(a[2], s0, kMaxSteps - 1);
      g_rnd = op == 7 ? (uint32_t)a[5] * 2654435761u + 1 : 0;
      for (int i = s0; i <= s1; ++i) {
        Step& x = t->step[i];
        if (op == 6) {
          if (x.n < 2) continue;
          int before[kNotes];
          for (int k = 0; k < x.n; ++k) before[k] = x.note[k].micro;
          core::seq::apply_strum(x, clampi(a[3], -kSub, kSub));
          for (int k = 0; k < x.n; ++k) changed |= before[k] != x.note[k].micro;
        } else
          for (int k = 0; k < x.n; ++k) {
            Note& n = x.note[k];
            int m = clampi(n.micro + rnd(clampi(a[3], 0, kSub)), -kSub, kSub);
            int v = clampi(n.vel + rnd(clampi(a[4], 0, 126)), 1, 127);
            changed |= m != n.micro || v != n.vel;
            n.micro = (int8_t)m, n.vel = (uint8_t)v;
          }
      }
      break;
    }
  }
  Entry* e = newest(kUndoK, rytm, pattern);
  if (!changed) {
    if (e) e->kind = kFree;
    sniprintf(out, cap, "seqed: %s T%d step %d: no change", kOps[op], rytm + 1, s0 + 1);
    return 0;
  }
  drop(kRedoK, rytm, pattern);   // a new edit: the redo history of this track is gone
  sniprintf(out, cap, "seqed: %s T%d step %d (%d notes there, undo %d)", kOps[op], rytm + 1, s0 + 1, s.n,
           count(kUndoK, rytm, pattern));
  return 1;
}

void to_view(const Track* t, sidecar_seq_view_t* v) {
  for (int i = 0; i < SEQED_CHORDS && i < core::poly::kChords; ++i) {
    strncpy(v->chord[i], core::poly::kChordNames[i], sizeof v->chord[i] - 1);
    v->chord[i][sizeof v->chord[i] - 1] = 0;
  }
  v->n = 0;
  memset(v->locks, 0, sizeof v->locks);
  memset(v->xstep, 0, sizeof v->xstep), memset(v->rt_cnt, 0, sizeof v->rt_cnt), memset(v->rt_mode, 0, sizeof v->rt_mode);
  memset(v->fx_cmd, 0, sizeof v->fx_cmd), memset(v->fx_val, 0, sizeof v->fx_val), memset(v->lcond, 0, sizeof v->lcond);
  v->speed = v->dir = v->rnd_mode = v->rnd_wght = 0, v->rnd_oct = 2;
  memset(v->cond, 0, sizeof v->cond);
  if (!t) return;
  for (int i = 0; i < kMaxSteps && i < SEQED_STEPS; ++i) v->locks[i] = t->step[i].nl, v->cond[i] = core::seq::cond_of(t->step[i]) != core::seq::kCondNone || (t->step[i].prob && t->step[i].prob < 100);   // (cond_knob's rule)
  v->dnote = t->dnote, v->dvel = t->dvel, v->dlen = t->dlen;
  v->poly_voices = t->poly.voices, v->poly_alloc = t->poly.alloc, v->poly_unison = t->poly.unison;   // E0 (the caller may
                                                                                                    // put the track's default)
  // seqtop: the track's speed / direction / random notes, each step's extras (the pattern's key and seed: the caller's)
  v->speed = t->speed, v->dir = t->dir, v->rnd_mode = t->rnd_mode, v->rnd_oct = t->rnd_oct, v->rnd_wght = t->rnd_wght;
  for (int i = 0; i < kMaxSteps && i < SEQED_STEPS; ++i) {
    const Step& s = t->step[i];
    int lc = 0;
    for (int k = 0; k < s.nl && !lc; ++k) lc = s.lock[k].cond;
    bool sl = false;
    for (int k = 0; k < s.nl; ++k) sl |= (s.slide >> (s.lock[k].param & 31) & 1) != 0;
    v->xstep[i] = (unsigned char)((s.xf & core::seq::kXfRnd ? 1 : 0) | (core::seq::rt_on(s.rt) ? 2 : 0) |
                                  (s.fx[0].cmd || s.fx[1].cmd ? 4 : 0) | (lc ? 8 : 0) | (sl ? 16 : 0));
    v->rt_cnt[i] = s.rt.cnt, v->rt_mode[i] = s.rt.mode;
    v->fx_cmd[i] = s.fx[0].cmd, v->fx_val[i] = (signed char)s.fx[0].val;
    v->lcond[i] = (unsigned char)lc;
  }
  for (int i = 0; i < kMaxSteps; ++i) {
    const Step& s = t->step[i];
    int order[kNotes];
    for (int k = 0; k < s.n; ++k) order[k] = k;
    for (int a = 1; a < s.n; ++a)   // by pitch
      for (int b = a; b > 0 && s.note[order[b]].pitch < s.note[order[b - 1]].pitch; --b) {
        int x = order[b];
        order[b] = order[b - 1], order[b - 1] = x;
      }
    for (int k = 0; k < s.n && v->n < SEQED_NOTES; ++k) {
      const Note& n = s.note[order[k]];
      sidecar_seq_note_t& o = v->note[v->n++];
      o.step = (unsigned short)i, o.micro = n.micro, o.pitch = n.pitch, o.vel = n.vel, o.len = n.len;
      o.flags = (s.flags & 2) ? 1 : 0;
    }
  }
}

}  // namespace seq_touch
