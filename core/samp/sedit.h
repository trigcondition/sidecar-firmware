// core/samp/sedit.h - the sample editor's state (the design: docs/SAMPLE-EDIT.md): one
// SAMPLES pool sample, opened from a DSP SAMPLE / SAMPLER track or a pool row, loaded whole into a float edit buffer (sedit_io.h's
// Loader), edited with the range functions of sedit_dsp.h over the region [a, b), one level of undo (the spare buffer:
// the edit before the last apply), and saved as a new <stem>-ed<N>.wav next to the original (Namer, Saver) that goes
// into the SAMPLES pool. The original file is never written.
//
// Every pass over the samples (the load, an apply, the outline, the save) runs in bounded pieces from step() (called by
// nm::step on the control core): at most kEditPiece frames a call, the card work posted through cardq. The Rytm page,
// the touchscreen and the console (sedit=) all read view() and call the functions below.
//
// The edit and spare buffers are private pcache blocks (alloc_private at the open, released at close), each the
// loaded length: a crop shortens frames, never the blocks, so undo can give the old length back. Pi and Mac only.
//
// Live play (every open, sedit-050 #1): two int16 play buffers ((frames + 1) x ch each, from the same open) take turns.
// After each change the one not in the voices' override (voices.h dsps_override, set on every voice for the edited
// row's pool slot) is refilled in pieces, then becomes the override: every track on that row hears the edit from its
// next trig (a pool open too: it has no track of its own). A buffer a voice still reads is never written or freed: the
// refill waits for the note to end (or the voice's next trig), and a close leaves such a buffer on a retired list that
// step() frees once no voice reads it. preview() plays the region once on the track's voice (dsps_preview); opened from
// the pool, on the selected track's when it is DSP SAMPLE or SAMPLER, else the first such track's.
#pragma once
#include <stdint.h>

#include "../platform.h"

#if CORE_MSAMP

namespace core {
namespace sedit {

enum State : uint8_t { kClosed, kLoading, kReady, kBusy, kSaving, kError };
constexpr uint32_t kEditPiece = 65536;   // frames a step at most (= CHOP's kTransientPiece)
constexpr int kCols = 1024;              // the outline's columns

void begin(bool card);   // boot: closed
// A DSP SAMPLE or SAMPLER track's sample (its SMP row, P11), the region its STA..END / STRT..END (P13 / P14) (0..120 onto frames - 1, END inclusive). Refused
// ("NOT A SAMPLE TRACK") unless (remote_track_machine(track) & 0x78) == kDspSample (SAMPLER's vb too). 0 loading, -1 refused.
int open_track(int track);
// A SAMPLES pool row (code = bank * 128 + row), the region the whole sample, no track. 0 loading, -1 refused.
int open_code(int code);
void close();            // the buffers released, kClosed (a card job in flight is finished first, out of sight)
// The Rytm's project changed (pool.cpp's switch): closed, so the new project's sample in the same slot never plays the
// edit; a save in flight still writes its file, which stays out of the new project's pool
void project_changed();

// The region [a, b), always 0 <= a < b <= frames. set_start past b puts a at b - 1; set_end before a puts b at a + 1.
// sedit-050 #7: set_start / set_end / nudge / jump land on the nearest zero crossing within sedit_dsp.h kSnapReach
// (zero_near; none: the place asked for; the sample's ends are kept). #6: zoomed in, the window follows the marker
// when it leaves it (scrolled so the marker is an eighth inside).
void set_start(int64_t f);
void set_end(int64_t f);
// A marker (0 start, 1 end) moved by detents of max(1, (wb - wa) / 128) frames (coarse: 8 times that); the snap only
// past the marker in the turn's direction, so every detent moves it
void nudge(int which, int detents, bool coarse);
// sedit-050 #8 (FUNC + A / B): a marker to the next (detents > 0) or previous transient, one a detent (sedit_dsp.h
// Onsets, found after every change), landing on the zero crossing at or before it; at most 64 detents a call.
// -1: refused ("FINDING TRANSIENTS"
// while they are found, "NO MORE TRANSIENTS")
int jump(int which, int detents);
// sedit-050 #4 (knob D): the zoomed window moved by detents of max(1, span / 16) frames (coarse: 8 times that)
void pan(int detents, bool coarse);
// The window: frames >> level (0..8), centred on the marker moved last, within [0, frames]; window(): touch's pan and
// zoom (clamped the same way). The outline follows over the next steps (gen moves when it is whole).
void zoom(int level);
void window(uint32_t wa, uint32_t wb);

// The function apply() runs (sedit_dsp.h Fn). apply / undo / save / preview: 0 started, -1 refused ("BUSY" while not
// kReady; the view's msg says why).
void set_fn(int fn);
int apply();
int undo();
// The region (START..END, the functions applied) saved as <stem>-ed<N>.wav in the source's folder and added to the
// SAMPLES pool (the source's bank, else the next with room); the editor then edits that copy, cropped to the region
// (its source, undo 0, dirty 0). use: then use_saved(track, code).
int save(bool use);
int preview();
// SAVE + USE: the track's SMP written to the saved row on the Rytm (48), STA 0 / END 120 too when the saved length is
// not the one the track's STA / END were for; the live play follows the track to the new row. A write answered busy
// is sent again (every 20 ms, up to 1 s; then "USE: RYTM BUSY"), one refused says "USE: RYTM REFUSED", from step()
// (one retry per track; project_changed drops them).
int use_saved(int track, int code);

bool step();                    // one bounded piece of the job in hand; true: it did work
uint32_t last_step_frames();    // the frames the last step went over (the step-bound test)

// The outline: column c covers frames [wa + span * c / kCols, wa + span * (c + 1) / kCols) of the window (span = wb -
// wa; at least one frame), lo / hi its lowest / highest sample of any channel as int16; when the window's peak is over
// 1.0 (a gain the file will clip) all scaled by 1 / peak, and unity says where 1.0 is.
struct View {
  State st;
  char name[48];        // the source's file name
  char msg[32];         // the last refusal, error or note ("" none)
  uint32_t frames, rate, a, b, wa, wb;
  uint8_t ch, bits, fn, zoom;
  bool undo, dirty, has_track;
  int8_t track;         // -1 none
  uint8_t progress;     // the job's 0..100
  int16_t lo[kCols], hi[kCols];
  int32_t playhead;     // -1 none
  int16_t unity;        // sedit-050 #3: the outline's level of 1.0 (0 dBFS): 32767, less when the window's peak is over
  uint32_t gen;         // moves when a drawn field changes
};
void view(View* out);

// The console: "sedit" (the status line), "sedit=open:track:T|open:code:C|close|start:F|end:F|region:A:B|
// nudge:S|E:N[:c]|jump:S|E:N|zoom:L|pan:N[:c]|window:A:B|fn:N|NAME|apply[:NAME]|undo|save|save:use|preview"
bool command(const char* p, char* out, int cap);

}  // namespace sedit
}  // namespace core

#endif
