// pi/engine/seqed_pi.cpp - see seqed_pi.h. Hosted (the core's flags): core 3 applies and publishes, core 0 copies.
#include "seqed_pi.h"

#include <stdlib.h>
#include <string.h>

#include "inst.h"
#include "platform.h"
#include "seq/seq.h"
#include "ui/remote.h"
#include "../ui/seq_touch.h"

namespace seqed_pi {

namespace {

namespace seq = core::seq;

int g_show = -1;                    // the Rytm track the display shows (-1: follow the SEQ page's / the first seq track)
sidecar_seq_view_t g_buf[2];        // published: core 3 writes g_buf[(n / 2 + 1) & 1], then n += 2
volatile uint32_t g_n = 0;          // the sequence counter (0: nothing yet)
volatile int g_play = -1;           // the shown track's play head, steps x 1000
struct Sig {
  uint32_t edits;
  int pattern, show, transport, bpm10, npages, length, undo, redo, tracks, swing, poly;
  uint32_t ne;   // U1: NOTE EDIT's counter (the highlight, the selection), | on
} g_sig = {~0u, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, ~0u};
bool g_force = true;
uint32_t g_req = 0;                 // seq-ux: TRK + TRIG's requests taken (seq::view_requests)

// fix1051b / per pattern poly (review round 1): the touch editor edits a seq track's slot only - one in the mask now
// (seq::edit_seq_track). An off slot (TURN OFF) or a released one (a Rytm machine) is nullptr here, and seq_touch.cpp
// says "T%d is not a seq track": before, an existing slot came back whatever `create` said, and seqed=add: wrote notes
// into it that were saved and never played
seq::Track* track_for(int rytm, bool create, void*) { return seq::edit_seq_track(rytm, create); }

int shown(const seq::View& v) {
  seq::Pattern& p = seq::edit_pattern();
  if (g_show >= 0 && seq::owns_track(g_show) && seq::track_of(p, g_show)) return g_show;
  if (v.seq) return v.track;
  for (const seq::Track& t : p.t)
    if (t.rytm >= 0 && seq::owns_track(t.rytm)) return t.rytm;
  return -1;
}

void publish(const seq::View& v, int show) {
  seq::Pattern& p = seq::edit_pattern();
  uint32_t n = g_n;
  sidecar_seq_view_t& o = g_buf[(n / 2 + 1) & 1];
  memset(&o, 0, offsetof(sidecar_seq_view_t, note));
  o.valid = 1;
  o.track = show;
  // the track chips: the pattern's seq tracks only (an off or released slot is no poly track here: -1, as a free one)
  for (int i = 0; i < SEQED_TRACKS && i < seq::kTracks; ++i) o.tracks[i] = seq::owns_track(p.t[i].rytm) ? p.t[i].rytm : -1;
  o.pattern = p.index;
  const seq::Track* t = show >= 0 ? seq::track_of(p, show) : nullptr;
  o.length = t && t->length ? t->length : v.length > 0 ? v.length : 16;
  o.npages = (o.length + seq::kStepsPerPage - 1) / seq::kStepsPerPage;
  o.edit_page = v.track == show ? v.page : -1;   // rui1051: the Rytm's PAGE (the grid's window holds it, stopped)
  o.transport = v.transport;
  o.bpm10 = (int)(v.bpm * 10 + 0.5);
  o.undo = seq_touch::undo_levels(show, p.index);
  o.redo = seq_touch::redo_levels(show, p.index);
  o.show_req = g_req;
  o.swing = t && t->swing > 50 ? t->swing : 50;   // touch2: the SEQ page's SWING (seq=swing:T:N)
  o.gen = seq::stats().edits + (uint32_t)show * 0x10000u + (uint32_t)p.index * 0x1000000u;
  seq_touch::to_view(t, &o);
  o.ne_step = o.ne_pitch = -1;
  if (show >= 0 && show == v.track) {   // U1: NOTE EDIT on the Rytm's SEQ page of this track - the editor follows it
    seq::NoteEdit ne = seq::note_edit();
    o.ne_on = ne.on, o.ne_step = ne.idx, o.ne_pitch = ne.pitch, o.ne_sel = ne.sel, o.ne_gen = ne.gen;
    for (int i = 0; ne.sel && i < o.n; ++i)
      if (seq::note_selected(o.note[i].step, o.note[i].pitch)) o.note[i].flags |= 2;
  }
  o.key = p.key, o.scale = p.scale, o.qnt = p.qnt, o.seed = p.seed, o.seed_fixed = p.seed_fixed;   // seqtop: the pattern's
  if (show >= 0) {   // E0: the config the track plays with (its pattern's, else its default) and what it resolves to
    seq::PolyCfg c = seq::poly_of(show);
    o.poly_voices = c.voices, o.poly_alloc = c.alloc, o.poly_unison = c.unison;
    core::inst::Resolved r;
    int m = -1, vi = core::ui::remote_track_voice(show);
    if (vi >= 0 && core::inst::resolve_for(vi, c, &r, &m)) core::inst::describe_short(r, m, o.poly_info, sizeof o.poly_info);
  }
  __atomic_store_n(&g_n, n + 2, __ATOMIC_RELEASE);   // the reader takes g_buf[(n / 2) & 1]
}

}  // namespace

bool Command(const char* p, char* out, int cap) {
  if (strncmp(p, "seqed", 5) || (p[5] && p[5] != '=')) return false;
  int pattern = seq::edit_pattern().index;
  if (!p[5]) {
    sniprintf(out, cap, "seqed: showing T%d, pattern %d, view %u, undo %d", g_show + 1, pattern, (unsigned)(g_n / 2),
              g_show >= 0 ? seq_touch::undo_levels(g_show, pattern) : 0);
    return true;
  }
  if (!strncmp(p, "seqed=show:", 11)) {   // the display's track chips
    int t = 0;
    for (const char* q = p + 11; *q >= '0' && *q <= '9'; ++q) t = t * 10 + (*q - '0');
    g_show = t < seq::kRytmTracks ? t : -1;
    g_force = true;
    sniprintf(out, cap, "seqed: showing T%d", g_show + 1);
    return true;
  }
  // review round 2 (N3): an UNDO of a track the Rytm's NOTE EDIT edited, then something outside it: that state first
  // becomes a level (core/seq undo_prepare), so the UNDO takes back the outside edit alone
  // round 3 (NB-1): a REDO too, and either ends the Rytm's snapshot (its state is history's now)
  bool undoing = !strncmp(p, "seqed=undo:", 11) || !strncmp(p, "seqed=redo:", 11);
  int urt = undoing ? atoi(p + 11) : -1;
  if (undoing && urt >= 0 && urt < seq::kRytmTracks) seq::undo_prepare(urt);
  if (seq_touch::apply(p, pattern, track_for, nullptr, out, cap) > 0) {
    if (!undoing) {   // a touch edit took its own level of the state before it
      const char* c = strchr(p, ':');   // seqed=<op>:<track>:...
      int rt = c ? atoi(c + 1) : -1;
      if (rt >= 0 && rt < seq::kRytmTracks) seq::level_taken(rt);
    }
    if (!strncmp(p, "seqed=move:", 11)) {   // review round 1: NOTE EDIT's highlight goes with a note the touchscreen moved
      int a[6] = {-1, -1, -1, -1, -1, -1}, k = 0;
      for (const char* q = p + 11; *q && k < 6; ++k) {
        bool neg = *q == '-';
        int x = 0;
        if (neg) ++q;
        while (*q >= '0' && *q <= '9') x = x * 10 + (*q++ - '0');
        a[k] = neg ? -x : x;
        if (*q == ':') ++q;
      }
      if (k == 6) seq::note_moved(a[0], a[1], a[2], a[3], a[4]);
    }
    seq::touched();   // the trig LEDs, the remote SEQ page and the save follow
    g_force = true;
  }
  return true;
}

// NOTE EDIT's undo levels (review round 1): the touchscreen's history (seq_touch), core 3 as the Rytm's keys
void undo_save(int rytm, int pattern, const seq::Track& t) { seq_touch::save_level(rytm, pattern, t); }
bool undo_take(int rytm, int pattern, seq::Track& t) { return seq_touch::undo_level(rytm, pattern, t); }
void undo_clear() { seq_touch::reset(); }   // review round 2 (N4): another project - the old one's history goes
const seq::UndoHooks kUndoHooks = {undo_save, undo_take, undo_clear};

void Tick() {
  static bool s_hooks = false;
  if (!s_hooks) seq::set_undo_hooks(&kUndoHooks), s_hooks = true;
  static uint32_t s_ms;   // every 5 ms at most: the play head at 200 Hz, the rest on a change
  uint32_t ms = core::now_ms();
  if (!g_force && ms - s_ms < 5) return;
  s_ms = ms;
  {   // rui1036b: the Rytm's selected track (49 2): the touchscreen's piano roll follows it
    static uint32_t s_tg = 0;
    uint32_t tg = 0;
    int st = core::ui::remote_selected_track(&tg);
    if (tg != s_tg) {
      s_tg = tg;
      if (st >= 0 && st < seq::kRytmTracks && seq::owns_track(st) && seq::track_of(seq::edit_pattern(), st)) g_show = st, g_force = true;
    }
  }
  int rt = -1;
  uint32_t rq = seq::view_requests(&rt);
  if (rq != g_req) {   // seq-ux: the Rytm's TRK + TRIG: its track on the touchscreen (the display shows the SEQ page)
    g_req = rq, g_force = true;
    if (rt >= 0 && rt < seq::kRytmTracks) g_show = rt;
  }
  seq::View v = seq::view();
  int show = shown(v);
  const seq::Pattern& p = seq::edit_pattern();
  int tracks = 0;
  for (const seq::Track& t : p.t) tracks = tracks * 13 + (seq::owns_track(t.rytm) ? t.rytm + 1 : 0);   // (the chips: the mask's)
  const seq::Track* st = show >= 0 ? seq::track_of(p, show) : nullptr;
  Sig s = {seq::stats().edits, p.index, show, v.transport, (int)(v.bpm * 10 + 0.5), v.npages, v.length,
           seq_touch::undo_levels(show, p.index), seq_touch::redo_levels(show, p.index), tracks, st ? st->swing : -1, 0, 0};
  if (show >= 0 && show == v.track) {   // U1: NOTE EDIT's highlight / selection moved
    seq::NoteEdit ne = seq::note_edit();
    s.ne = ne.gen << 1 | (ne.on ? 1u : 0u);
  }
  if (show >= 0) {   // E0: the poly config (and the voice's machine it resolves on) in the signature
    seq::PolyCfg c = seq::poly_of(show);
    int vi = core::ui::remote_track_voice(show);
    s.poly = c.voices | c.alloc << 4 | c.unison << 6 | (vi >= 0 ? core::ui::remote_track_machine(show) & 0x7F : 0x7F) << 10;
  }
  double pos = show >= 0 ? seq::play_pos(show) : -1;   // any seq track's, not only the Rytm's selected one
  g_play = pos >= 0 ? (int)(pos * 1000) : -1;
  if (!g_force && !memcmp(&s, &g_sig, sizeof s)) return;
  g_force = false;
  g_sig = s;
  publish(v, show);
}

bool View(sidecar_seq_view_t* out, unsigned* seen) {
  for (int tries = 0; tries < 4; ++tries) {
    uint32_t a = __atomic_load_n(&g_n, __ATOMIC_ACQUIRE);
    if (a == 0 || a == *seen) return false;
    memcpy(out, &g_buf[(a / 2) & 1], sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_n, __ATOMIC_ACQUIRE) - a < 2) {   // not overwritten meanwhile
      *seen = a;
      return true;
    }
  }
  return false;
}

int Playhead() { return g_play; }

}  // namespace seqed_pi
