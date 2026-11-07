// core/samp/sedit.h - the sample editor's state (the design: docs/SAMPLE-EDIT.md): one
// SAMPLES pool sample, opened from a DSP SAMPLE track or a pool row, loaded whole into a float edit buffer (sedit_io.h's
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
// A DSP SAMPLE track's sample (its SMP row), the region its STA..END (0..120 onto frames - 1, END inclusive). Refused
// ("NOT A DSP SAMPLE TRACK") unless (remote_track_machine(track) & 0x78) == kDspSample. 0 loading, -1 refused.
int open_track(int track);
// A SAMPLES pool row (code = bank * 128 + row), the region the whole sample, no track. 0 loading, -1 refused.
int open_code(int code);
void close();            // the buffers released, kClosed (a card job in flight is finished first, out of sight)

// The region [a, b), always 0 <= a < b <= frames. set_start past b puts a at b - 1; set_end before a puts b at a + 1.
void set_start(int64_t f);
void set_end(int64_t f);
// A marker (0 start, 1 end) moved by detents of max(1, (wb - wa) / 128) frames (coarse: 8 times that)
void nudge(int which, int detents, bool coarse);
// The window: frames >> level (0..8), centred on the marker moved last, within [0, frames]; window(): touch's pan and
// zoom (clamped the same way). The outline follows over the next steps (gen moves when it is whole).
void zoom(int level);
void window(uint32_t wa, uint32_t wb);

// The function apply() runs (sedit_dsp.h Fn). apply / undo / save / preview: 0 started, -1 refused ("BUSY" while not
// kReady; the view's msg says why).
void set_fn(int fn);
int apply();
int undo();
// The edit saved as <stem>-ed<N>.wav in the source's folder and added to the SAMPLES pool (the source's bank, else the
// next with room); the editor then edits that copy (its source, undo 0, dirty 0). use: then use_saved(track, code).
int save(bool use);
int preview();
// SAVE + USE: the opening track moved to the saved row (Task 4 writes it to the Rytm; here the request is noted)
int use_saved(int track, int code);

bool step();                    // one bounded piece of the job in hand; true: it did work
uint32_t last_step_frames();    // the frames the last step went over (the step-bound test)

// The outline: column c covers frames [wa + span * c / kCols, wa + span * (c + 1) / kCols) of the window (span = wb -
// wa; at least one frame), lo / hi its lowest / highest sample of any channel as int16 (clipped as the play buffer).
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
  uint32_t gen;         // moves when a drawn field changes
};
void view(View* out);

// The console: "sedit" (the status line), "sedit=open:track:T|open:code:C|close|start:F|end:F|region:A:B|
// nudge:S|E:N[:c]|zoom:L|window:A:B|fn:N|NAME|apply[:NAME]|undo|save|save:use|preview"
bool command(const char* p, char* out, int cap);

}  // namespace sedit
}  // namespace core

#endif
