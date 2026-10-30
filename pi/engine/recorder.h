// pi/engine/recorder.h - multitrack recording to the card (docs/pi-engine.md "Recording"): the selected channels of
// the Overbridge IN stream (MAIN L/R, the 8 voice stems, AUDIO IN L/R), the master pair, the Sidecar's 8 voices and
// the two send FX returns, one WAV a source in /Recordings/<take>/<NN>_<NAME>.wav, every file starting on the same
// frame and the same length (sample-aligned: they drop into a DAW lined up).
//
// Three sides, no locks, no allocation after Init:
//   - producer (the IN completion, core 0's xHCI interrupt; the self-test's loop): in_block() copies the selected
//     samples of each IN block (with the voices/FX frames out_block() queued from the OUT fill) into a ring of
//     fixed slots in memory reserved once at boot. A full ring drops the block, counted; the consumer writes the
//     dropped frames as silence, so the files keep time (and stay aligned). Never blocks, never logs.
//   - control (core 0: the console, the web page; transport() from any core): configure, arm, start, stop.
//   - consumer (core 0's loop, poll()): one kPiece (64 KB) write a call at most, a file's 768 KB chunk in twelve, back
//     to back (sector-aligned: the headers are 512 bytes; long runs a file), so the loop (network, display) keeps
//     running between writes and the card queue's stream reads (core/msamp/cardq.h) wait one piece at most; a piece
//     is held back while the backend's card_busy() says a stream needs the card, unless the ring is half full
//     (counted: yields).
// Space reserved up front (Backend::reserve, FatFs f_expand): each file gets a contiguous area at its start (up to
// 64 MB, reserve_bytes), so its writes touch no FAT until it is used up - with 25 files open the FAT sectors no longer
// take turns in FatFs's one window. The close cuts the file to its data (Backend::truncate). While the take runs the
// marker (below) holds each such file's length as of its last sync: a take cut by a power loss is cut back to that
// (recover), never to the reserved area's leftovers.
// Portable: the card is a Backend (FatFs on the Pi: rec_pi.cpp; stdio in tests/test_recorder.cpp).
//
// The destination (set_root, docs/STORAGE.md "Recording to a USB drive"): the SD card (root "", the default) or another
// volume ("USB:", "USB2:", "USB3:", "NVME:" - FatFs's drive names): every path the recorder hands the backend is then
// <root>/Recordings/... ("USB:/Recordings/take_004/00_MASTER.wav"). A take's id is <root><take> ("USB:take_004"; on the
// SD just "take_004"). While a take is written, <root>/Recordings/.recording (kTakeMarker) holds its name: a take that
// never closed (power, a pulled drive) is fixed by recover() when that volume comes back.
//
// The take's name (rec_names.h): "<slug>_<YYYY-MM-DD_HHMM>" with a wall clock (Backend::clock; "_2", "_3" ... when that
// name is taken on the SD or the destination), "<slug>_take-NNN" without one; the slug the Rytm project's name
// (Backend::project), or the prefix rec=name: set (it replaces the project's), "sidecar" with neither.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "rec/rec_names.h"   // the sources, channel lists, take names and arming, shared with the Sidecar Mini's
#include "spsc.h"

namespace rec {

const unsigned kBlockFrames = 7;   // one Overbridge block
const uint32_t kChunk = 786432;     // a file's staging: 768 KB (whole frames for 2, 3, 4 and 6 bytes; 12 pieces)
const uint32_t kPiece = 65536;      // a write (the xHCI TRB rule's 64 KB, stor_vol.h; the card queue's wait)
// the meters (the display's REC page, meter_peaks): the 12 IN channels, the 8 voices, FX 1 L/R, FX 2 L/R, PRE L/R,
// POST L/R
const int kMeters = kInChannels + kSide;   // 28

struct Backend {
  bool (*mkdir)(const char* path);                      // true if made or there
  int (*open)(const char* path);                        // create (truncate) for writing: a handle 0..kSources-1, -1
  bool (*write)(int h, const void* p, uint32_t n);      // at the end
  bool (*pwrite)(int h, uint64_t off, const void* p, uint32_t n);   // the header, then back to the end
  bool (*sync)(int h);
  bool (*close)(int h);
  bool (*space)(uint64_t* free, uint64_t* total);
  uint32_t (*now_us)();
  // the boot scan and the take numbering
  // a folder's entries; -1 no such folder, -2 (any other negative) an error - never a partial count
  int (*list)(const char* dir, char (*names)[64], uint64_t* sizes, bool* dirs, int max);
  bool (*read_at)(const char* path, uint64_t off, void* p, uint32_t n);
  bool (*patch)(const char* path, uint64_t off, const void* p, uint32_t n);
  void (*log)(const char* line);
  bool (*card_busy)();   // optional: the card queue's streams need the card now (the Pi: core/msamp/cardq.h busy())
  // the destination volume (set_root); each optional
  bool (*ready)(const char* root);                                   // that volume is mounted now (null: always)
  // another volume's space (false: unknown; *free = ~0: the total known, the free space not counted)
  bool (*space_at)(const char* root, uint64_t* free, uint64_t* total);
  bool (*remove)(const char* path);                                  // the marker, at a take's clean close
  void (*abandon)(int h);   // forget a handle with no I/O (a drive that stopped answering: no header, no close)
  int (*mkdir_new)(const char* path);   // 1 made now, 0 there already, -1 an error - a take's folder is always new
  // the take's name (each optional): the wall clock's local time (false: none set), the Rytm project's name ("" none)
  bool (*clock)(uint32_t* local);
  void (*project)(char* out, unsigned cap);
  // space up front (each optional): reserve bytes for a file just opened (still empty; FatFs f_expand: false - no
  // contiguous room, the file left empty); truncate a file at its write position (the close); cut a closed file to
  // size bytes (recover)
  bool (*reserve)(int h, uint64_t bytes);
  bool (*truncate)(int h);
  bool (*cut)(const char* path, uint64_t size);
};

// The marker's layout (<root>/Recordings/.recording): the take's name, then each source's file length as of its last
// sync (by id: the bytes of data after the header; kNoLength: not tracked, the file's size counts). An old marker
// (40 bytes, the name only) still reads.
const uint64_t kNoLength = ~0ull;
struct Marker { char take[kTakeMax]; uint64_t data[kSources]; };

// The ring's memory (cmdline recbuf=MB, rec_pi.cpp): want the MB asked for (kRecbufAuto: not given); free_mb the low
// heap's free MB at boot (memalign's: the ring and its staging come from it, as new / malloc do), high_mb the high
// heap's (the pool and MULTISAMPLE take theirs from there; 0: a 1 GB board, where they fall back to the low heap).
// Asked: as asked, 1024 at most. Auto with a high heap: half of the low heap's free space past kRecbufKeep, 64..256 MB
// (what is there below 64); auto without one: 64 MB, as before. 0: recording off.
const unsigned kRecbufAuto = ~0u, kRecbufKeep = 192;
unsigned recbuf_mb(unsigned want, unsigned free_mb, unsigned high_mb);
// The space a file reserves at its start: 64 MB at most, a share of the destination's free space (half of it over the
// files), 0 below 4 MB (not worth it); a drive's free space unknown: 32 MB (f_expand refuses if there is no room)
uint64_t reserve_bytes(uint64_t free, bool known, unsigned files);
const uint64_t kReserveMin = 4ull << 20;   // smaller is not tried
const uint32_t kReserveUs = 1500000;       // a take's reservations stop after this (f_expand's FAT walks; logged)

// The warning before a take (the web page, the display): the selection needs more than kWarnPct % of the
// destination's write speed - measured by stor=bench (rec_pi.cpp keeps it, .rytm/recspeed.txt), else a conservative
// default for the kind of volume (the SD 4 MB/s, a USB drive 6 MB/s, the NVMe SSD 50 MB/s).
const unsigned kWarnPct = 70;
uint32_t default_write_kbps(const char* root);   // "" the SD, "USB:", "NVME:" ...
inline unsigned load_pct(uint32_t need_kbps, uint32_t speed_kbps) {
  return speed_kbps ? (unsigned)(((uint64_t)need_kbps * 100 + speed_kbps / 2) / speed_kbps) : 0;
}
inline bool rate_warning(uint32_t need_kbps, uint32_t speed_kbps) {
  return speed_kbps && (uint64_t)need_kbps * 100 > (uint64_t)speed_kbps * kWarnPct;
}
// .rytm/recspeed.txt: a line a volume, "<name> <KB/s>" (names[i] the volume i's: "SD", "USB" ...). parse: the
// volumes it names set in kbps (others untouched), the count; format: the volumes with a speed (non-zero)
int parse_speeds(const char* text, const char* const* names, int n, uint32_t* kbps);
int format_speeds(char* out, unsigned cap, const char* const* names, int n, const uint32_t* kbps);

const char kTakeMarker[] = ".recording";   // <root>/Recordings/.recording: the take being written (Marker, above)
const unsigned kRootMax = 8;           // "USB2:" and its NUL, with room

struct Status {
  int state;            // 0 idle, 1 armed (waiting for PLAY or start), 2 recording, 3 finishing (draining, closing)
  bool arm_mode, ready; // ready: memory reserved
  unsigned bits;
  uint32_t mask;
  char take[kTakeMax], prefix[25], error[64];   // take: its id (<root><name>: "USB:take_004", "take_004" on the SD);
                                                // prefix: rec=name:'s ("" the project's name)
  char root[kRootMax];  // the destination: "" the SD card, "USB:" ...
  bool abandoned;       // the last take's files were left without their headers (its drive stopped answering)
  uint64_t frames;      // recorded (this take, or the last one)
  uint32_t peak[kSources];   // 24-bit full scale = 8388607, since the last status (reset by it)
  uint32_t drops, dropped_frames, side_under;   // ring overruns (blocks), OUT side frames missing
  uint64_t bytes;       // written this take
  uint32_t write_kbps;  // the card's speed inside the writes (KB/s), this take
  uint32_t worst_write_us, writes;
  uint32_t yields;      // pieces held back for the streams
  uint32_t ring_slots, ring_max_used, ring_used;   // slots; the high-water mark this take
  uint32_t ring_ms;     // the ring's length at the selection's rate
  uint64_t free_bytes, total_bytes;
  bool free_known;      // false: a drive's free space not counted (FAT32 without FSINFO: stor=free:VOL)
  uint32_t need_kbps;   // the selection's data rate
};

class Recorder {
 public:
  // mem: the ring and the staging (kSources * kChunk), reserved once; bytes == 0: recording off (refused)
  void init(void* mem, size_t bytes, const Backend* b);
  // control (core 0)
  bool configure(uint32_t mask, unsigned bits, char* err, unsigned errlen);   // idle only
  bool set_prefix(const char* p);   // [A-Za-z0-9_-], 1..24; "": the project's name again
  // the destination volume: "" the SD card, else a FatFs drive ("USB:", up to 6 of A-Z 0-9 and the colon); idle only
  // (armed: refused). Not checked for being mounted here: start / arm refuse then ("USB drive not mounted").
  bool set_root(const char* root, char* err, unsigned errlen);
  const char* root() const { return root_; }
  bool start(char* err, unsigned errlen);    // now (manual)
  bool arm(char* err, unsigned errlen);      // start on the next PLAY (transport 0xFA/0xFB), stop on STOP; re-arms
  void stop();                               // a take in progress ends (its files closed by poll); armed: disarmed
  void transport(uint8_t status);            // MIDI realtime, any core: FA start, FB continue, FC stop
  void poll();                               // core 0's loop
  void status(Status* s, bool take_peaks);
  uint32_t mask() const { return mask_; }
  unsigned bits() const { return bits_; }
  // producer (the IN completion): n <= kBlockFrames frames of the 12 IN channels (int32 full scale)
  void in_block(const int32_t (*in)[kInChannels], unsigned n);
  // the OUT fill (before or after in_block, the same core): the voices as rendered (int16; mono, a stereo voice's
  // (L + R) / 2), the FX returns (float); lr (optional): the voices' L plane [0..7] and R plane [8..15] (a stereo
  // voice's file); pairs (optional): the PRE and POST pairs (L R L R) as sent
  void out_block(const int16_t (*voices)[kVoices], const float (*fx)[4], unsigned n, const int16_t (*lr)[2 * kVoices] = nullptr,
                 const float (*pairs)[4] = nullptr);
  // the OUT fill: the voices routed PRE / POST now (bit v); a take records those voices in stereo (latched at its start)
  void set_stereo_voices(uint8_t mask) { st_now_ = mask; }
  uint8_t stereo_voices() const { return st_take_; }   // the take's (or the last one's)
  bool wants_side() const { return (side_on_ && state_ != 0) || meters_; }
  bool wants_in() const { return state_ == 1 || state_ == 2 || meters_; }
  // the meters (the display's REC page, any state): while on, in_block / out_block keep each channel's peak (every
  // channel, selected or not: the levels show before a take); meter_peaks takes them (24-bit full scale, reset to 0).
  // Separate from Status::peak (the web page's), so the two readers never steal each other's peaks.
  void set_meters(bool on) { meters_ = on; }
  void meter_peaks(uint32_t out[kMeters]);
  // the boot scan: every WAV of ours under dir/<take>/ with the sizes its length says (a take cut by a power loss);
  // returns the files fixed
  int fix_all(const char* dir);
  int fix_take(const char* dir);   // one take's folder (dir: "<root>/Recordings/<take>"); the files fixed
  // a volume back (mounted again, or the boot): the take its marker names fixed and the marker removed; the files
  // fixed, -1 no marker (or that volume's take is being written now)
  int recover(const char* root);

  enum { kIdle = 0, kArmed = 1, kRecording = 2, kEnded = 3 };

 private:
  // done: bytes of buf written; reserved: the space f_expand gave it (0 none); synced: data at its last sync
  struct Out { int h; uint8_t* buf; uint32_t fill, done; uint64_t data, reserved, synced; uint8_t width, src; };
  bool begin(bool arm, char* err, unsigned errlen, bool rearm = false);
  bool open_take();
  bool pick_name(char* path, unsigned cap);   // the take's folder made (take_ named); false: failed (fail() said why)
  void marker_path(char* out, unsigned cap) const;
  // lens: by source id (kNoLength: by the file's size); *cut_failed: a file is still longer than its data
  int fix_take(const char* dir, const uint64_t* lens, bool* cut_failed);
  void marker_patch();
  void close_take();
  bool write_out(Out& o);    // one piece of o's staging; true when the staging is all written
  void fail(const char* why);
  void space_now();
  void slow(bool ok, uint32_t t0);   // a drive call that failed after > 1 s: the drive is gone (dead_)
  void rpath(char* out, unsigned cap, const char* rel) const;   // <root><rel>
  bool cas(int from, int to) { return __atomic_compare_exchange_n(&state_, &from, to, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
  int32_t* slot(uint32_t i) { return ring_ + (size_t)(i % nslots_) * slot_words_; }   // (every slot used: no power of 2)

  const Backend* b_ = nullptr;
  uint8_t* stage_ = nullptr;
  int32_t* ring_ = nullptr;
  size_t ring_bytes_ = 0;
  uint32_t nslots_ = 0, slot_words_ = 0, nsamp_ = 0;
  volatile int state_ = kIdle;
  volatile uint8_t go_ = 0;
  bool arm_mode_ = false, side_on_ = false, open_ = false, stopping_ = false;
  bool dead_ = false;   // a write failed after a long wait (a drive gone): no more I/O for this take
  bool abandoned_ = false;
  char root_[kRootMax] = "";
  uint32_t mask_ = 1u << kMaster;
  unsigned bits_ = 24;
  char prefix_[25] = "", take_[kTakeMax] = "", error_[64] = "";
  Marker mk_ = {};          // the take's marker as on the card (its lengths: after each sync on the SD, a round at a time on a drive)
  bool mk_dirty_ = false;
  uint32_t mk_fails_ = 0;
  // producer's
  uint32_t wr_ = 0, gap_ = 0;   // wr_: __atomic (the consumer reads it)
  volatile uint32_t drops_ = 0, dropped_frames_ = 0, side_under_ = 0;
  volatile uint32_t peak_[kSources] = {};
  volatile bool meters_ = false;
  volatile uint32_t mpeak_[kMeters] = {};
  struct Side { int32_t s[kSideFrame]; };
  volatile uint8_t st_now_ = 0;   // the OUT fill's: the voices on PRE / POST
  uint8_t st_take_ = 0;           // latched at begin: the voices this take records in stereo
  int width(int i) const {        // the file's channels for source i
    return source(i).width + (i >= kVoice && i < kVoice + kVoices && (st_take_ >> (i - kVoice) & 1) ? 1 : 0);
  }
  Spsc<Side, 512> side_;
  // consumer's
  volatile uint32_t rd_ = 0;
  uint32_t ring_max_ = 0, frame_in_slot_ = 0, zeros_ = 0;
  Out out_[kSources];
  int nout_ = 0, sync_next_ = 0, flush_next_ = 0;
  uint64_t frames_ = 0, bytes_ = 0, write_us_ = 0;
  uint32_t worst_us_ = 0, writes_ = 0, last_sync_us_ = 0, last_space_us_ = 0, yields_ = 0;
  uint64_t free_ = 0, total_ = 0;
  bool free_known_ = true;
  uint32_t last_drops_ = 0, last_drop_log_us_ = 0;
};

}  // namespace rec
