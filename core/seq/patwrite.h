// core/seq/patwrite.h - FILL's pattern writer (0.5.0; notes/design sampler-lock-fill 1.3): the Rytm's active pattern
// read by number (the stock request 0x64 n, n = the active pattern: its working buffer as edited, saved or not; an
// answer is taken only when its byte 9 is n - near a queued switch a 0x6A answered the queued pattern [measured,
// rytm-emu 1.74]), patched in place by the caller's
// function (core/seq/lockfill.h: one target of one track, every other byte as it was), written back and read again to
// verify, byte for byte.
//
//   write    a 0x54 (pattern n) with n = the active pattern (2D 02; rytm_dump::active_pattern): stock takes it into both
//            of the pattern's copies, the working one it plays and the stored one its power-off save keeps. A 0x5A
//            (pattern X) reaches only the working copy: the fill played, then was gone after SAVE PATTERN and a
//            power-off [measured, rytm-emu OS 1.73 and 1.74: notes fill-persist-report]. No 2D 02 seen yet: PATTERN
//            UNKNOWN, nothing read or written (also after a Rytm hello, until the resync's 2D 02). The restore and the
//            undo are written the same way. A project load since the start (it keeps the pattern number): PROJECT
//            LOADED: STOPPED before the write, and no write-back after it.
//   guard    a DIRTY (2D 0B: a pattern edit) or a pattern change (2D 02) between the request and its answer: the answer
//            may or may not hold that edit, so it is dropped and the read starts again (a switch: at once, the new
//            pattern's number; 3 reads at most, then PATTERN
//            BUSY, nothing written). The pattern view's own fetches (rytm_dump) wait meanwhile (set_fill_hold); the
//            song's dumps and writes (rytm_dump::hold_level() 2) refuse a start: SONG BUSY.
//   playing  a write while the Rytm plays only when allowed (set_play_ok: emulator check E4 shows such a write clean:
//            the playhead goes on, no trig lost, the locks from the next pass). Until then a start while playing
//            answers STOP TO FILL, and PLAY pressed while reading drops the job before anything is written.
//   after    from the write on, a pattern change (2D 02) means another pattern is in the working buffer now: the job
//            ends CHANGED MEANWHILE: CHECK and nothing is written back. DIRTYs: a received 0x54 makes two of its own
//            [measured, rytm-emu fix round]; any other count than kOwnDirties (more: the user edited;
//            fewer: the write was not taken) means no write-back.
//   verify   the record read back must equal the one written (lockfill::same: the lock list may come back in the Rytm's
//            own order). The pattern as it was before the write (a late answer, or the write not taken yet): asked once
//            more; as before again: the Rytm did not take it, nothing written back (NOT TAKEN: UNCHANGED). A difference
//            writes the record as it was read back (VERIFY FAILED: RESTORED) - only while nothing
//            moved since the write. No verify answer at all: the same, else NOT CONFIRMED: CHECK IT, nothing written.
//   song     the song's own dumps or writes starting mid-job (rytm_dump::hold_level() 2): SONG BUSY, nothing written
//            back; the song refuses to arm, confirm a replay or undo while a job runs (song.cpp). A fill made while a
//            take is armed (hold 1, allowed) is undone by the take's Restore, as any edit then.
//   retries  a request not answered: again after 1.5 s, 5 tries in all (the song's dump client's rule), then NO ANSWER.
//
// The control core (the Pi's core 3): start / tick / byte from the platform's loop and its Rytm MIDI tap. One job at a
// time; nothing is stored on the Rytm's pattern slots (the user's SAVE PATTERN / RELOAD as for any edit). Pi / Mac.
#pragma once
#include <stdint.h>

namespace core {
namespace patwrite {

constexpr uint32_t kRetryMs = 1500;   // an unanswered request asked again
constexpr int kTries = 5;
constexpr int kReads = 3;             // reads a guard may restart
constexpr uint32_t kSettleMs = 120;   // after the write, before the verify read (the Rytm takes the 15 KB in)
constexpr uint32_t kOwnDirties = 2;   // the DIRTYs (2D 0B) a received 0x54 (as a 0x5A) makes [measured, rytm-emu: 2]

// The caller's patch: rec (13101 B, as read) changed in place; 0 done (written if it changed), else an error number the
// caller names (lockfill::Result), nothing written. ctx: the caller's.
using PatchFn = int (*)(uint8_t* rec, void* ctx);

enum State : uint8_t { kIdle = 0, kReading, kSettle, kVerifying, kDone, kFailed };
enum Why : int {
  kWhyNone = 0, kWhyPlaying = 1, kWhyBusy = 2, kWhySong = 3, kWhyNoAnswer = 4, kWhyVerify = 5, kWhyPatch = 6,
  kWhyPlayed = 7, kWhyRunning = 8, kWhyChanged = 9, kWhyUnconfirmed = 10, kWhyNotTaken = 11, kWhyNoPattern = 12, kWhyProject = 13,
};
const char* why_text(int why);   // "STOP TO FILL", "PATTERN BUSY", ...

#if CORE_POLY
// A job: false (and why()) when one runs, the song's dumps run, or the Rytm plays without set_play_ok.
bool start(PatchFn fn, void* ctx);
void tick(uint32_t ms);
void byte(uint8_t b);              // the Rytm -> host MIDI stream, every byte (beside rytm_dump::byte)
State state();
bool busy();                       // reading .. verifying
int why();                         // the last job's kWhy*
int patch_result();                // the patch's own number (kWhyPatch)
bool changed();                    // the last done job wrote something (the patch changed the record)
uint32_t jobs();                   // finished jobs (done or failed): a page's "seen" counter
uint32_t read_switch();            // rytm_dump::switch_count() when the last job's read was taken (its pattern)
const uint8_t* before();           // the last job's record as read (13101 B; null: none)
void set_play_ok(bool on);         // E4: writes while playing allowed
bool play_ok();
// console "fill=..." helpers live with the page; tests
void reset();
struct Stats { uint32_t reads, writes, verifies, restarts, timeouts, mismatches, restores; };
const Stats& stats();
#endif

}  // namespace patwrite
}  // namespace core
