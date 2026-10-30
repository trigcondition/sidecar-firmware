// pi/engine/rec_play.h - playback of the recorder's takes (recorder.h: /Recordings/<take>/<NN>_<NAME>.wav, our 512 B
// headers, 16 or 24 bit, mono or stereo, 48 kHz) into the Sidecar's output to the Rytm: one channel of one take at a
// time, mixed onto the Overbridge return's MAIN L / R (channels 0 / 1; a mono file on both) after the voices, the
// computer's OUT and the send FX returns, so the voices' own paths stay as they are.
//
// Two sides on one core (the Pi's core 0), no locks, no allocation after init:
//   - control and reader (core 0's loop): scan() the takes (newest first) and a take's channels, select, play / stop,
//     the level; poll() reads one 30 KB piece of the file a call into a ring of frames (the recorder's ring, reversed),
//     so the loop keeps running between reads.
//   - mix (the OUT fill, the xHCI interrupt): mix() adds the ring's frames to the outgoing block. It never blocks: when
//     the ring runs low before the file's end (the card fell behind) it fades out over what is left and stops, counted
//     and reported (underruns, error "card too slow").
// Fades: 10 ms in at the start, 10 ms out on a stop, a channel change (which resumes the new channel at the same frame:
// the take's files line up) or an underrun. Portable: the card is a Backend (FatFs on the Pi: rec_pi.cpp; stdio in
// tests/test_rec_play.cpp). WAV reading is recwav::ours / get32 (rec_wav.h, shared with the writer); the take and
// channel naming follows the recorder's. Could move to core/ with the portable recorder parts later (one WAV module
// for both directions, the take listing shared with the recorder's numbering).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "spsc.h"

namespace recplay {

const int kMaxTakes = 128, kMaxChannels = 23;   // the recorder's sources
const uint32_t kPiece = 30720;                   // one read: whole frames of 2, 3, 4 and 6 bytes, 60 sectors
const uint32_t kFade = 480;                      // 10 ms
const uint32_t kRingFrames = 65536;              // 1.37 s
const uint32_t kStartFill = 24000;               // frames read before the sound starts (0.5 s)

struct Backend {
  // dir's entries: names, sizes, directory flags, a time stamp each (larger = newer; 0 unknown); -1 no such dir
  int (*list)(const char* dir, char (*names)[64], uint64_t* sizes, bool* dirs, uint32_t* stamps, int max);
  int (*open)(const char* path);                       // for reading: a handle >= 0, -1
  int (*read)(int h, void* p, uint32_t n);             // bytes read (0 at the end), -1 an error
  bool (*seek)(int h, uint64_t off);
  void (*close)(int h);
  void (*log)(const char* line);
  // optional: the volumes with takes besides the SD card ("USB:" ... while mounted): scan() lists <root>/Recordings
  // of each; such a take's id is <root><name> ("USB:take_004"), the SD's its name. Null: the SD only.
  int (*roots)(const char** out, int max);
};
// a take's folder from its id: "/Recordings/take_004", "USB:/Recordings/take_004"
void take_folder(char* out, unsigned cap, const char* id);

enum State : int { kNone = 0, kStopped = 1, kPlaying = 2, kStopping = 3 };   // kNone: no take to play

struct Status {
  int state;
  char take[64], channel[16], error[32];   // take: an id as the recorder names them (rec_names.h kTakeMax)
  int take_i, takes, ch_i, channels;   // the selection (0 = the newest take)
  uint64_t pos, len;                   // frames played, the channel's length (frames)
  int level;                           // 0..127
  uint32_t underruns, reads, worst_read_us;
  bool paused;                         // stopped by pause(): play() goes on from pos (a stop, another take: from 0)
  uint8_t width, bits;                 // the selected file: 1 / 2 channels, 16 / 24 bit (0: not read yet)
};

struct Frame { int32_t l, r; };

class Player {
 public:
  void init(const Backend* b, uint32_t (*now_us)());
  // control (core 0's loop)
  int scan(const char* prefer = nullptr);   // /Recordings' takes (every root's), newest first; selects prefer (an id), else
                                            // the one selected (by name), else the newest; returns the takes found
  bool select_take(int i);                  // 0 the newest; stops playback; channel: MASTER if recorded, else the first
  bool step_take(int d) { return select_take(take_i_ + d); }
  bool select_channel(int i);               // playing: fades out, resumes channel i at the same frame
  bool step_channel(int d) { return select_channel(ch_i_ + d); }
  bool select_by_name(const char* take, int nn);   // the web page's pick: take's channel NN (-1: the default)
  bool play(char* err, unsigned errlen);    // from the start (or where a channel change left it)
  void stop();                              // fades out; forgets a paused position
  void pause();                             // fades out and keeps the frame: play() resumes there
  void set_level(int lev);                  // 0..127 (100: the default, -4 dB; 127: as recorded)
  int level() const { return level_; }
  void poll();
  void status(Status* s) const;
  bool active() const { int s = state_; return s == kPlaying || s == kStopping; }
  // the OUT fill (n frames of the host's 12 channels, int32): adds to MAIN L / R
  void mix(int32_t (*out)[12], unsigned n);

 private:
  struct Take { char name[64]; uint32_t stamp; };
  struct Chan { char file[64]; char name[16]; int nn; uint64_t size; };
  bool open_channel(uint64_t at_frame, char* err, unsigned errlen);
  void close_file();
  bool read_piece();
  void load_channels();
  void fail(const char* why);
  bool probe();

  const Backend* b_ = nullptr;
  uint32_t (*now_)() = nullptr;
  Take takes_[kMaxTakes];
  int ntakes_ = 0, take_i_ = 0;
  Chan ch_[kMaxChannels];
  int nch_ = 0, ch_i_ = 0;
  int level_ = 100;
  volatile float gain_ = 0.0f;
  // the file
  int h_ = -1;
  uint32_t align_ = 0, width_ = 0, bits_ = 0;
  uint64_t data_ = 0, read_ = 0;           // the data's bytes, read so far
  uint64_t len_ = 0;                       // frames
  uint8_t piece_[kPiece];
  char error_[32] = "";
  uint32_t reads_ = 0, worst_read_us_ = 0, underruns_ = 0;
  // shared with mix(): the reader's end mark, the requests, the mix's state
  Spsc<Frame, kRingFrames> ring_;
  volatile int state_ = kNone;
  volatile bool eof_ = false, stop_req_ = false, under_ = false;
  volatile uint64_t pos_ = 0;              // frames played (the mix's)
  uint32_t ramp_ = 0;                      // the mix's fade position (0..kFade)
  uint64_t resume_at_ = 0;                 // a pause's frame (control side's)
  bool pause_req_ = false;
  bool resume_ = false;                    // a channel change waiting for the fade: then ch_i_ from pos_
};

}  // namespace recplay
