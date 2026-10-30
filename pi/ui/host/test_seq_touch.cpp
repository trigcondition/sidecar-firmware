// pi/ui/host/test_seq_touch.cpp - the touch editor's model side (pi/ui/seq_touch.h) on core/seq's own pattern code:
// every command line, the undo / redo history per track and pattern, the view's order, bad lines refused.
// make -C pi/ui/host test
#include <stdio.h>
#include <string.h>

#include "../seq_touch.h"

using namespace core::seq;

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static Pattern g_p;
static Track* tf(int rytm, bool create, void*) { return track_of(g_p, rytm, create); }
static int run(const char* l) { char o[120]; return seq_touch::apply(l, 3, tf, nullptr, o, sizeof o); }
static const Note* at(int rytm, int step, int pitch) {
  Track* t = track_of(g_p, rytm, false);
  if (!t) return nullptr;
  for (int i = 0; i < t->step[step].n; ++i)
    if (t->step[step].note[i].pitch == pitch) return &t->step[step].note[i];
  return nullptr;
}

int main() {
  static sidecar_seq_view_t v;
  clear(g_p, 3);
  seq_touch::reset();
  // add, replace, delete
  CHECK(run("seqed=add:8:0:60:0:100:72") == 1);
  CHECK(at(8, 0, 60) && at(8, 0, 60)->vel == 100 && at(8, 0, 60)->len == 72);
  CHECK(run("seqed=add:8:0:60:12:90:48") == 1);   // the same pitch: replaced
  CHECK(track_of(g_p, 8, false)->step[0].n == 1 && at(8, 0, 60)->micro == 12 && at(8, 0, 60)->vel == 90);
  CHECK(run("seqed=del:8:0:61") == 0);            // not there: no change, no undo level
  CHECK(seq_touch::undo_levels(8, 3) == 2);
  CHECK(run("seqed=del:8:0:60") == 1 && track_of(g_p, 8, false)->step[0].n == 0);
  // undo / redo
  CHECK(run("seqed=undo:8") == 1 && at(8, 0, 60) && at(8, 0, 60)->micro == 12);
  CHECK(run("seqed=undo:8") == 1 && at(8, 0, 60) && at(8, 0, 60)->micro == 0);
  CHECK(seq_touch::redo_levels(8, 3) == 2);
  CHECK(run("seqed=redo:8") == 1 && at(8, 0, 60)->micro == 12);
  CHECK(run("seqed=vel:8:0:60:127") == 1 && at(8, 0, 60)->vel == 127);   // a new edit: redo gone
  CHECK(seq_touch::redo_levels(8, 3) == 0);
  // move (across steps and pitch, micro set), length
  CHECK(run("seqed=move:8:0:60:5:63:-20") == 1);
  CHECK(!at(8, 0, 60) && at(8, 5, 63) && at(8, 5, 63)->micro == -20 && at(8, 5, 63)->vel == 127);
  CHECK(run("seqed=len:8:5:63:300") == 1 && at(8, 5, 63)->len == 300);
  // a chord stamp (core/poly's table: MAJ on C4 = C E G), strummed, then re-strummed downwards
  CHECK(run("seqed=chord:8:16:60:1:0:100:72:0") == 1);
  CHECK(at(8, 16, 60) && at(8, 16, 64) && at(8, 16, 67) && track_of(g_p, 8, false)->step[16].n == 3);
  CHECK(run("seqed=strum:8:16:16:10") == 1);
  CHECK(at(8, 16, 60)->micro == 0 && at(8, 16, 64)->micro == 10 && at(8, 16, 67)->micro == 20);
  CHECK(run("seqed=strum:8:0:31:-10") == 1);       // a range: only steps with 2+ notes
  CHECK(at(8, 16, 67)->micro == 0 && at(8, 16, 60)->micro == 20 && at(8, 5, 63)->micro == -20);
  CHECK(run("seqed=chord:8:20:57:2:1:90:48:0") == 1);   // MIN, first inversion on A3: C4 E4 A4
  CHECK(at(8, 20, 60) && at(8, 20, 64) && at(8, 20, 69));
  // humanise: deterministic, bounded
  CHECK(run("seqed=human:8:16:20:6:10:7") == 1);
  for (int s = 16; s <= 20; ++s)
    for (int k = 0; k < track_of(g_p, 8, false)->step[s].n; ++k) {
      const Note& n = track_of(g_p, 8, false)->step[s].note[k];
      CHECK(n.micro >= -26 && n.micro <= 26 && n.vel >= 80 && n.vel <= 110);
    }
  Pattern copy = g_p;
  CHECK(run("seqed=undo:8") == 1 && run("seqed=redo:8") == 1 && memcmp(&copy, &g_p, sizeof g_p) == 0);
  // a track that is not a seq track: refused without creating one; another pattern's history is separate
  CHECK(run("seqed=del:2:0:60") == 0 && !track_of(g_p, 2, false));
  char o[100];
  CHECK(seq_touch::apply("seqed=undo:8", 4, tf, nullptr, o, sizeof o) == 0);
  // bad lines
  CHECK(run("seqed=add:8:0:60") == -1 && run("seqed=add:12:0:60:0:1:1") == -1 && run("seqed=foo:1") == -1);
  CHECK(run("seqed=add:8:128:60:0:100:72") == -1 && run("seq=add:8:0:60:0:100:72") == -1);
  // the history is bounded
  for (int i = 0; i < 40; ++i) {
    char l[64];
    snprintf(l, sizeof l, "seqed=add:8:%d:70:0:100:24", 64 + i);
    run(l);
  }
  CHECK(seq_touch::undo_levels(8, 3) == seq_touch::kUndo);
  // the view: step order, pitch order within a step, defaults, chord names
  seq_touch::to_view(track_of(g_p, 8, false), &v);
  CHECK(v.n == 1 + 3 + 3 + 40 && v.note[0].step == 5 && v.note[1].step == 16 && v.note[1].pitch == 60);
  CHECK(v.note[2].pitch == 64 && v.note[3].pitch == 67 && v.dvel == 100 && !strcmp(v.chord[1], "MAJ"));
  // NOTE EDIT on the Rytm (core/seq UndoHooks, review round 1): its levels in the same history - the touchscreen's UNDO
  // undoes them in order, a new one drops the redo
  {
    Track* t = track_of(g_p, 8, false);
    int lv = seq_touch::undo_levels(8, 3) - 1;
    run("seqed=undo:8");
    CHECK(seq_touch::redo_levels(8, 3) == 1);
    seq_touch::save_level(8, 3, *t);   // the Rytm's gesture starts: a level, the redo gone
    CHECK(seq_touch::undo_levels(8, 3) == lv + 1 && seq_touch::redo_levels(8, 3) == 0);
    t->step[5].note[0].vel = 7;        // its edit
    CHECK(run("seqed=undo:8") == 1 && at(8, 5, 63)->vel == 127);   // the touchscreen's UNDO takes the Rytm's edit back
    CHECK(run("seqed=redo:8") == 1 && at(8, 5, 63)->vel == 7);
    CHECK(seq_touch::undo_level(8, 3, *t) && at(8, 5, 63)->vel == 127 && seq_touch::redo_levels(8, 3) == 1);   // FUNC + YES
    CHECK(!seq_touch::undo_level(2, 3, *t));
  }
  printf(g_fail ? "test_seq_touch: %d FAILED\n" : "test_seq_touch: ok\n", g_fail);
  return g_fail != 0;
}
