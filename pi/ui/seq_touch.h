// pi/ui/seq_touch.h - the touch editor's model side (docs/TOUCH-EDITOR.md): the display's "seqed=..." command lines
// applied to a core/seq track, an undo history, and a track turned into the editor's view (sidecar_seqed.h). Pure: no
// clock, no card, no LVGL. On the Pi it runs on core 3 (pi/engine/seqed_pi.cpp: the console's command, then
// core::seq::touched () so the trig LEDs, the remote SEQ page and the save follow); the Mac's tests and the host
// render call it directly on a pattern of their own.
//
// Commands (integers; T the Rytm track 0..11, S a step 0..127, P a MIDI note, M micro -96..96, V 1..127, L 1/96 step):
//   seqed=add:T:S:P:M:V:L            a note (the same pitch on that step replaced)
//   seqed=del:T:S:P                  a note removed
//   seqed=move:T:S:P:S2:P2:M2        a note to step S2, pitch P2, micro M2 (velocity and length kept)
//   seqed=len:T:S:P:L                a note's length
//   seqed=vel:T:S:P:V                a note's velocity
//   seqed=chord:T:S:ROOT:CHD:INV:V:L:STRUM   the step's notes = core/poly's chord CHD/INV on ROOT (set_chord)
//   seqed=strum:T:S0:S1:A            every step S0..S1 with 2+ notes broken by A (1/96 a note; < 0 from the top)
//   seqed=human:T:S0:S1:TIME:VEL:SEED   every note's micro +- TIME, velocity +- VEL (a fixed pseudo-random sequence)
//   seqed=undo:T, seqed=redo:T
// Every edit is one undo level (a chord stamp, a strum over a page: one each).
#pragma once
#include "seq/seq.h"
#include "sidecar_seqed.h"

namespace seq_touch {

constexpr int kUndo = 24;   // levels, shared by the tracks (a copy of the track each: 24 x ~7 KB)

// The track of Rytm track rytm in the pattern being edited (create: a free slot taken), or null
typedef core::seq::Track* (*TrackFor)(int rytm, bool create, void* param);

void reset();   // the history dropped (a pattern change keeps it: each level names its pattern)
// One command line ("seqed=..."). Returns 1: the track changed; 0: nothing to do (undo with no history, a note that
// is not there); -1: not a seqed line or a bad one. out: a status line for the console (may be null).
int apply(const char* line, int pattern, TrackFor track_for, void* param, char* out, int cap);
int undo_levels(int rytm, int pattern);
// NOTE EDIT on the Rytm (core/seq UndoHooks, pi/engine/seqed_pi.cpp): its gestures' levels in this history, so the
// touchscreen's UNDO and the Rytm's FUNC + YES walk one history. save_level: t before a gesture (the redo dropped, as
// a touch edit drops it); undo_level: the newest level of that track into t (t onto the redo), false: none
void save_level(int rytm, int pattern, const core::seq::Track& t);
bool undo_level(int rytm, int pattern, core::seq::Track& t);
int redo_levels(int rytm, int pattern);
// The notes of t into v (step order, then pitch), its defaults, core/poly's chord names. The header (track, pattern,
// pages, transport) is the caller's.
void to_view(const core::seq::Track* t, sidecar_seq_view_t* v);

}  // namespace seq_touch
