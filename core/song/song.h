// core/song/song.h - SONG RECORD (docs/SONG.md, MVP): a Rytm performance captured on the Sidecar against the Rytm's step
// clock, kept as a JSON song per Rytm project, shown as a read-only timeline and replayed into the Rytm after an explicit
// confirmation. Pi and Mac (CORE_POLY builds).
//
// Recording (arm, then the Rytm's PLAY): every byte of the Rytm's USB MIDI reaches byte() (the platform's tap beside
// rytm_dump's). From it: stock MIDI (CC / NRPN per track and the FX track, CC 94 mutes, CC 92 scenes, CC 35..47 PERF on
// the auto channel, program changes, FA / FB / FC, F8 as a clock fallback) and the mod's 2C step ticks / 2D state
// (TRANSPORT, PATTERN, SWITCH, MUTES, TEMPO, FILL). The Sidecar's own changes (its send FX values, the machines the Rytm
// reports per track) are polled in tick(). Every event gets t, ticks since PLAY (kPpq a 1x step: 96, seq::kSub), from the
// clock track's 2C ticks (seq's 2E clock mask, set while armed) interpolated by the frames between them; F8 (16 ticks
// each) when no 2C comes (a stock Rytm). Pause freezes t.
// At ARM the start snapshot is requested (the stock dumps of the working kit / pattern / settings and the GLOBAL X:
// 0x68 0x6A 0x6C 0x6D, one at a time) and the MIDI CONFIG read from the GLOBAL X [measured offsets, docs/SONG.md]. While
// recording, /.rytm/songs/<project>/<name>.jsonl is appended (its first line the header with the snapshot; flushed every
// bar or 64 events, closed and reopened - the platform's sync - every 4 bars); at STOP it is folded into <name>.json and
// removed. A .jsonl left by a crash is folded into <name>.json ("recovered") when its project is next named.
//
// Replay: never automatic. replay() asks (the remote SONG page's YES / the touchscreen's CONFIRM); confirm() then backs
// up the current working kit / pattern / settings into .backup-<n>.json, selects the take's start pattern (PC), writes the
// take's pattern X and kit X (and settings X when asked) into the Rytm's WORKING buffers (never a stored slot, never a
// save), sets its mutes (CC 94), the Sidecar's send FX, and re-requests the kit and pattern to verify. Then the Rytm's
// PLAY starts the events against its own clock: program changes ahead of each recorded switch (the Rytm queues them to
// the end of the playing pattern), parameters / mutes / scenes / PERF at their tick, the Sidecar's own straight into the
// core. A switch the Rytm makes re-anchors the rest. Not replayed: tempo (stock MIDI has no input for it), FILL, edits,
// machine changes (informational; the kit carries the machines).
//
// UNDO REPLAY (UX review T14; docs/SONG.md step 7): everything the replay will overwrite is backed up at confirm,
// while the Rytm is stopped, before anything is written - a backup set: [0] the pattern selected (P0: its pattern X,
// kit X, settings X, the mutes, the Sidecar's send FX), then each other pattern the take plays (its start P1 first,
// then every pattern its switches go to): PC to it, its pattern X and kit X read. At most kUndoMax entries; a take
// that plays more: P0 and P1 only, and the undo ends when the replay leaves P1 ("covers the start only"). When the
// take's pattern X, once written, links P1 to another kit, that kit is read before kit X goes over it (kit2, Restore's
// kRelink). During the replay nothing is held or read: a switch to a pattern not in the set ends the undo at once.
// Each entry a file (.backup-S.json, .backup-S-K.json; S one of kBackupSlots in turn, .backups.txt - advanced and
// posted first - names the next and each slot's count: a reused slot's extra -K files removed; read from the RAM
// store, never a card call on the control core), formatted in idle() a file a pass, held in the RAM store
// (core/store) and written through fmt's posted queue (each write's own result: fmt::post_result). Nothing of the
// replay is written before the set's files are on the card.
// The undo (song=undo, the remote page's T5, the touchscreen's UNDO REPLAY) reads the set back from the RAM store
// only (a file an idle pass), checks every entry whole before a byte goes out, then puts the entries back newest
// first (PC, [kit2], [pattern X], kit X, each verified), P0 last with its settings X (when the replay wrote them),
// the mutes and the send FX: the Rytm ends on P0 as it was. Offered (Status::undo) while the Rytm is stopped, once
// the replay wrote something, until the next edit (a CC / NRPN from the Rytm - also while the replay plays -, a
// pattern change it reports, the Sidecar's send FX, ARM), 30 s stopped (REPLAY READY too), or a project change;
// refused while a project loads (its store filling, as seq's edits are: kLockLoading). Not offered when P0 was not
// known and the replay changes the pattern. Kept out of the touch editor's shared UNDO / REDO (that one is the
// Sidecar's own sequencer notes; this writes the Rytm's kits).
//
// Threads: everything on the control side (the Pi's core 3): byte() from the platform's MIDI tap, tick() from its loop,
// idle() among the card jobs, command() from the console. The display (core 0) reads lanes_publish's copy (lanes_read).
#pragma once
#include <stdint.h>

namespace core {
namespace song {

constexpr int kPpq = 96;                     // ticks a 1x step
constexpr int kBarSteps = 16;
constexpr int kBar = kPpq * kBarSteps;       // 1536
constexpr int kTracks = 12;
constexpr int kFxTrack = 12;                 // p events: trk 12 = the FX track
constexpr int kMaxEvents = 65536;            // a take (16 B each: 1 MB)

// ---- events (16 B in RAM; docs/SONG.md "Storage format")
enum Type : uint8_t {
  kNone = 0,
  kPc,      // a = the pattern queued (0..127)
  kSw,      // a = from, b = to: the switch happened
  kParam,   // a = track 0..11 / kFxTrack / 0x40 | channel (no track's), b = NRPN msb << 7 | lsb, v = 14-bit value
  kCc,      // a = channel 1..16, b = CC, v = 0..127
  kMute,    // v = the mute mask (bit t: track t muted)
  kScene,   // a = scene 0 (off) / 1..12
  kPerf,    // a = CC (35..47), v = 0..127
  kTempo,   // v = bpm x 120
  kPlay, kStop, kPause, kCont,
  kSc,      // a Sidecar parameter: a = kSc*, b = index, v = value
  kMach,    // a = Rytm track, b = its voice byte (0x7F not a Sidecar machine)
  kFill,    // a = 0 / 1
  kMark,
  kSrc,     // rui1038: a = Rytm track, b = the parameter (0 P1; rui1045: 10 DSP SAMPLE's SMP, P11), v = its 14 bits (index << 7 | bank << 4) of GRANULAR SRC / WAVETABLE TBL /
            // MULTISAMPLE INST, from the Sidecar's own reports (2A, its 48 writes) - never the 7-bit CC: the bank stays
  kTypes
};
enum : uint8_t { kScSend = 0, kScKnob = 1, kScRoute = 2 };   // send: b = track * 2 + slot; knob: b = slot * kKnobs + j
struct Ev {
  uint32_t t;
  uint8_t type, a;
  uint16_t b;
  uint32_t v;
  uint32_t ms;   // wall ms since PLAY (diagnostics)
};
const char* type_name(int type);   // "pc" "sw" "p" "cc" "mute" "scene" "perf" "tempo" "play" "stop" "pause" "cont" "sc" "mach" "fill" "mark" "src"
int type_of(const char* name, int n);   // kNone if unknown

// ---- the Rytm's MIDI CONFIG, from its GLOBAL X dump (0x6D, 80 bytes unpacked; offsets measured in rytm-emu on OS 1.73:
// 32 PARAM OUTPUT (1 CC, 0 NRPN), 34 CLOCK SEND, 36 TRANSPORT SEND, 37 PROG CH RECEIVE, 38 PROG CH SEND, 44 ENCODER DEST
// (1 INT+EXT), 46 OUTPUT CH (1 TRK CH); 12..23 the tracks' channels and 24 the FX channel, 0-based [inferred from the
// stock defaults 0..12])
constexpr int kGlobalBytes = 80;
struct MidiConfig {
  bool valid;
  uint8_t encoder_ext, trk_ch, nrpn, clock_send, transport_send, pc_receive, pc_send;
  uint8_t transport_receive;   // fix1041: GLOBAL X 35 [inferred: between CLOCK SEND 34 and TRANSPORT SEND 36, the SYNC
                               // menu's order]; on by default (the Rytm's)
  uint8_t track_ch[kTracks];   // 1..16
  uint8_t fx_ch, auto_ch;      // 1..16 (auto: 14, learned from the PC the Rytm sends at PLAY)
};
void config_default(MidiConfig* c);   // the Rytm's defaults (channels 1..12, FX 13, auto 14), not valid
bool parse_global(const uint8_t* u, int n, MidiConfig* c);
enum : uint32_t { kCfgEncoder = 1, kCfgTrkCh = 2, kCfgNrpn = 4, kCfgPcReceive = 8, kCfgUnknown = 16 };
// What stands in the way: recording (ENCODER DEST INT, AUTO CH; CC instead of NRPN: a hint only) or replay (PROG CH
// RECEIVE off). Never changed by the Sidecar: the user sets them on the Rytm (config_explain).
uint32_t config_problems(const MidiConfig& c, bool replay);
int config_text(uint32_t problems, char* out, int cap);   // "ENCODER DEST INT, OUTPUT CH AUTO" ("" none)
const char* config_explain(int line);                     // the how-to, a line at a time (null past the end)
// The how-to as a checklist (UX review T13): kCfgRows settings the user sets on the Rytm, row k's text, and which of
// them are wrong now (bit k), -1 while the MIDI CONFIG was not read
constexpr int kCfgRows = 6;
const char* config_row(int k);   // "PORT: ENCODER DEST = INT+EXT" .. (null past the end)
int config_wrong(const MidiConfig& c);

// ---- the stock SysEx dumps (F0 00 20 3C 07 00 <id> 01 01 <nr> ... <checksum 2> <length 2> F7; docs/SONG.md 2.1)
enum : uint8_t { kReqKit = 0x68, kReqPattern = 0x6A, kReqSettings = 0x6C, kReqGlobal = 0x6D };
int dump_request(uint8_t out[15], uint8_t id);   // nr 0: the working buffer
// A whole dump: its id (byte 6), checksum (sum of bytes 10 - or 9, the object number, which the Rytm leaves out - .. the
// payload's end & 0x3FFF) and length (bytes 5 .. the payload's end) right
bool dump_ok(const uint8_t* m, int n, uint8_t* id);
int unpack7(const uint8_t* p, int n, uint8_t* out, int cap);   // Elektron's 7-bit packing (8 -> 7)
int b64_len(int n);
int b64_encode(const uint8_t* d, int n, char* out, int cap);
int b64_decode(const char* s, int n, uint8_t* out, int cap);   // -1 bad

// ---- growable byte buffer (new[], never in the audio path)
struct Buf {
  uint8_t* p = nullptr;
  int n = 0, cap = 0;
  bool reserve(int c);
  bool add(const void* d, int k);
  bool addc(char c) { return add(&c, 1); }
  bool adds(const char* s);
  bool addf(const char* fmt, ...);
  void clear() { n = 0; }
  void free();
};

// ---- a take
struct Take {
  char project[24];
  char name[40];
  MidiConfig cfg;
  int pattern;          // at PLAY (-1 unknown)
  uint32_t tempo;       // bpm x 120 (0 unknown)
  uint32_t mutes;
  int scene;
  Buf kit, pat, settings, global;   // the start snapshot: the Rytm's replies (0x58 0x5A 0x5C 0x5D), whole SysEx
  Buf kit2;             // UNDO REPLAY's backup entry: the kit the take's pattern X linked, before the replay wrote it
  bool has_fx;          // the Sidecar's send FX at PLAY
  uint8_t fx_send[kTracks][2], fx_knob[2][6], fx_route;
  Ev* ev = nullptr;
  int n = 0, cap = 0;
  uint32_t end;         // the last tick (STOP)
  bool recovered;
  void clear();         // no events, no snapshot (the event memory kept)
  bool push(const Ev& e);
  void free();
};

// ---- the musical clock (pure)
struct Clock {
  bool running = false;   // PLAY (or continue) seen, not paused / stopped
  bool ticked = false;    // a 2C tick since PLAY (F8 then ignored)
  uint32_t t = 0;         // the last tick's t
  uint32_t frozen = 0;    // t at the pause (at() never below it)
  double f = 0;           // its host frame
  double period = 0;      // host frames a 1x step (from the tempo, else measured)
  double inc = kPpq;      // ticks one tick of the clock track advances
  int last_step = -1, len = 16;
  bool switched = false;  // a 2D SWITCH since the last tick: its step 0 is one step on
  uint32_t f8s = 0;
  void start(double frame);   // PLAY from STOP: t 0
  void cont(double frame);    // continue after pause
  void pause(double frame);   // t frozen
  void stop();
  void tempo(double bpm);
  void switch_seen() { switched = true; }
  void tick(int step, int len, int speed, double frame);
  void f8(double frame);      // MIDI clock (24 ppqn = 16 ticks), only while no 2C ticked
  uint32_t at(double frame) const;   // t at frame (between ticks: interpolated, never past the next tick)
};

// ---- the recorder (pure: frames and ms given)
struct Recorder {
  Take* take = nullptr;
  Clock clk;
  bool on = false;        // recording (PLAY seen while armed)
  bool armed = false;     // waiting for PLAY: CC / NRPN / mutes go to the start (t 0)
  uint32_t mutes = 0;
  int pattern = -1, queued = -1;
  uint32_t ms0 = 0;
  uint32_t stats_dropped = 0, stats_thinned = 0;
  void arm(Take* t);
  void play(double f, uint32_t ms);
  void cont(double f, uint32_t ms);
  void pause(double f, uint32_t ms);
  void stop(double f, uint32_t ms);
  void channel(uint8_t st, uint8_t d1, uint8_t d2, double f, uint32_t ms);
  void pattern_state(int now, int next, double f, uint32_t ms);   // 2D 02
  void switched(int from, int to, double f, uint32_t ms);         // 2D 03
  void tick(int step, int len, int speed, double f);              // the clock track's 2C tick
  int sw_pending = -1;
  void mute_mask(uint32_t mask, double f, uint32_t ms);           // 2D 05
  void tempo(double bpm, double f, uint32_t ms);                  // 2D 07
  void fill(bool held, double f, uint32_t ms);                    // 2D 0C
  void sidecar(uint8_t kind, uint16_t index, uint32_t v, double f, uint32_t ms);
  void machine(int track, int vb, double f, uint32_t ms);
  void mark(double f, uint32_t ms);
  // rui1038: P1 of a track on GRANULAR / WAVETABLE / MULTISAMPLE (banked: bit t) at 14 bits (kSrc); the Rytm's own NRPN
  // 1:0 / CC 16 (SRC A) on a banked track's channel are not recorded (the CC carries the index only)
  void src(int track, int v14, double f, uint32_t ms, int param = 0);   // rui1045: param 10 = DSP SAMPLE's SMP (P11)
  uint16_t banked = 0;
  uint16_t banked_smp = 0;   // rui1045: of those, the DSP SAMPLE tracks (SMP banked: its NRPN 1:11 / CC 27 dropped, not SRC A's)
  bool add(uint8_t type, uint8_t a, uint16_t b, uint32_t v, double f, uint32_t ms, bool thin = false);
 private:
  struct Nrpn { int8_t msb = -1, lsb = -1; int16_t vmsb = -1; };
  Nrpn nrpn[16];
  uint32_t last_pc_t = 0xFFFFFFFFu;
  int last_pc = -1;
  static constexpr int kThin = 2048;
  uint32_t thin_key[kThin];
  int32_t thin_idx[kThin];
  int thin_find(uint32_t key, bool make);
};

// ---- the file (json.cpp): the song JSON, the journal's lines, the recovery
// The whole JSON in pieces: next() appends the next piece (the header with the snapshot first, then events in groups
// of `per`), false when done.
struct JsonGen {
  const Take* t = nullptr;
  int stage = 0, i = 0, per = 256;
  int bi = 0, boff = 0;   // the dumps' base64: the dump, the raw offset (a piece a call)
  bool bfirst = true;
  bool journal = false;   // the journal's header line (no events, no closing brace)
  void begin(const Take* take, bool journal_header);
  bool next(Buf& out);
};
int event_json(const Ev& e, char* out, int cap);   // [t,"type",{...}]
// Reading (incremental: the events a step at a time, the Pi's control loop keeps rendering)
struct JsonRead {
  const char* s = nullptr;
  int n = 0, pos = 0, stage = 0;
  Take* t = nullptr;
  bool ok = false, done = false;
  char err[48];
  void begin(const char* text, int len, Take* take);
  bool step(int max_events);   // false: done (ok tells)
};
bool read_json(const char* s, int n, Take* t, char* err, int errcap);   // whole (tests)
// A journal (.jsonl) -> the take (its header line, then every whole event line; a cut last line dropped)
bool recover_journal(const char* s, int n, Take* t, char* err, int errcap);

// ---- replay (replay.cpp; pure: the messages go to a Port)
struct Port {
  virtual ~Port() {}
  virtual void send(const uint8_t* m, int n) = 0;   // a channel message (2 / 3 bytes) or a whole SysEx
  virtual void request(uint8_t id) = 0;              // a dump request (dump_request)
  // the backup of what the restore and the replay overwrite: p0 the state at confirm (pattern = the pattern then, -1
  // unknown), more[0..n) the other patterns the take plays (its start's first), partial: it plays more than kUndoMax
  // (more: its start's only). Async: backup_done later; false: cannot
  virtual bool backup(const Take& p0, const Take* const* more, int n, bool partial) = 0;
  // the take's pattern X linked P1 to another kit: that kit's state (before kit X is written) added to entry k's
  // backup (kit2). Async: relink_done later; false: cannot
  virtual bool relink(int k, const Buf& kit) { return false; }
  virtual bool playing() { return false; }           // the Rytm plays (a restore / an undo then stops: never into it)
  virtual void sidecar(const Ev& e) = 0;             // a kSc event into the core
  virtual void sidecar_start(const Take& t) = 0;     // the take's start send FX into the core
};
// The PC for each switch: at the recorded queue (a kPc to the same pattern after the switch before), else two steps
// before the switch, never before the previous switch
uint32_t pc_time(const Take& t, int sw_index);
struct Replayer {
  const Take* take = nullptr;
  int idx = 0;
  int64_t offset = 0;      // recorded t = Rytm t + offset (a switch re-anchors)
  uint32_t mutes = 0;
  int next_sw = -1;        // the next kSw to expect
  bool pc_sent = false;
  bool running = false;
  static constexpr int kSwWait = 4 * kPpq;
  uint32_t sent = 0, skipped = 0, reanchors = 0, pcs = 0;
  // fix1041: the take's end - its recorded STOP (else its last tick, Take::end, or its last event): reached, the
  // Rytm is stopped as the take was (a MIDI STOP 0xFC on the replay's port, when send_stop: TRANSPORT RECEIVE on) and
  // ended is set (the engine then ends the replay: song.cpp idle)
  uint32_t end_t = 0;
  bool send_stop = true, ended = false;
  void begin(const Take* t);
  void start(Port& p);                         // the Rytm's PLAY: t 0's events
  void advance(uint32_t t_now, Port& p);       // everything due up to t_now (the Rytm's t)
  void switched(int to, uint32_t t_now, Port& p);
  void stop();
  void send_event(const Ev& e, Port& p);
  void mute_to(uint32_t mask, Port& p);
};
constexpr int kUndoMax = 8;
// The confirmed restore (the order is the risk: docs/SONG.md section 7): backup -> PC start pattern -> pattern X ->
// [the kit it links read: relink] -> kit X [-> settings X] -> mutes -> the Sidecar's start -> verify (kit and pattern
// re-requested, compared). The backup: P0's dumps (the pattern selected: from), then for each other pattern the take
// plays (visit: its start first, then its switches') PC to it and its dumps (kBackupSel, kBackupReq1), then written;
// failed before any write: PC back to from.
struct Restore {
  enum St : uint8_t { kIdle, kBackupReq, kBackupSel, kBackupReq1, kBackupWrite, kSelect, kRelink, kRelinkWrite, kPattern,
                      kKit, kSettings, kMutes, kVerify, kDone, kFailed };
  St st = kIdle;
  const Take* take = nullptr;
  bool with_settings = false;
  uint32_t at = 0;             // when the state was entered (ms)
  int want = -1;               // the dump id awaited
  int tries = 0;               // requests of it so far (asked again after kRetryMs, kTries in all)
  Take backup;                 // the current working state, before anything is written (pattern = from)
  Take extra[kUndoMax - 1];    // the other patterns', after their PCs (visit's order)
  int visit[kUndoMax - 1];
  int nvisit = 0, vi = 0;      // the patterns to back up, the one being read
  int plays = 0;               // the patterns the take plays (P0 counted)
  bool partial = false;        // more than kUndoMax: only its start backed up
  Buf relinked;                // the kit the take's pattern X linked (kRelink)
  int from = -1;               // the pattern selected at confirm (-1 unknown)
  bool selected = false;       // a PC went out in the backup phase
  bool wrote = false;          // pattern X / kit X written (from here the Rytm is changed)
  Buf vkit, vpat;              // the verify answers
  bool verify_kit = false, verify_pat = false;
  char msg[64];
  void begin(const Take* t, bool settings, uint32_t ms, Port& p, int current = -1);
  void tick(uint32_t ms, Port& p);
  bool cancel(Port& p);        // before anything is written: stopped (PC back to from); false: writing
  void dump_in(uint8_t id, const uint8_t* m, int n, uint32_t ms, Port& p);
  void backup_done(bool ok, uint32_t ms, Port& p);
  void relink_done(bool ok, uint32_t ms, Port& p);
  bool busy() const { return st != kIdle && st != kDone && st != kFailed; }
  static constexpr uint32_t kTimeoutMs = 8000, kRetryMs = 1500, kGapMs = 60;
  static constexpr uint32_t kWriteTimeoutMs = 10000;   // the backup's files on the card (fmt's queue held up): else failed
  static constexpr int kTries = 5;
 private:
  void fail_unwritten(const char* why, Port& p);
  void to_write(uint32_t ms, Port& p);
  void next_visit(uint32_t ms, Port& p);
};
// UNDO REPLAY's run: a backup set's entries put back, list[n - 1] first, list[0] (P0) last. Every entry checked whole
// in begin (kit X; kit2 when it has one; pattern X when it has one - list[0] must -; list[0]'s settings X when
// settings): kFailed and nothing sent otherwise. Per entry: PC (pattern >= 0) -> [kit2 -> verified] -> [pattern X] ->
// kit X [-> list[0]: settings X, the mutes (mutes >= 0), the send FX] -> verify (kit, pattern). Stops when the Rytm
// plays (Port::playing).
struct UndoRun {
  enum St : uint8_t { kIdle, kSelect, kKit2, kVerify2, kPattern, kKit, kSettings, kMutes, kVerify, kDone, kFailed };
  St st = kIdle;
  const Take* list[kUndoMax];
  int n = 0, i = 0;
  bool with_settings = false;
  int32_t mutes = -1;
  uint32_t at = 0;
  int want = -1, tries = 0;
  bool wrote = false;          // a dump written (from here the Rytm is changed)
  Buf vkit, vpat;
  char msg[64];
  void begin(const Take* const* entries, int count, bool settings, int32_t mute_mask, uint32_t ms, Port& p);
  void tick(uint32_t ms, Port& p);
  void dump_in(uint8_t id, const uint8_t* m, int n, uint32_t ms, Port& p);
  bool busy() const { return st != kIdle && st != kDone && st != kFailed; }
 private:
  void select(uint32_t ms, Port& p);
};
// The replies payload-compared (the id's request form ignored: 0x58 against 0x58)
bool same_dump(const Buf& a, const Buf& b);

// ---- the timeline's summary (lanes.cpp; pure): what the display draws, built from a take
constexpr int kLaneBlocks = 96, kLaneMutes = 192, kLaneScenes = 64, kLanes = 10, kLanePoints = 160, kLaneTempo = 32;
struct Block { uint32_t t0, t1; uint8_t v; };
struct Point { uint32_t t; uint16_t v; };
struct Lane {
  char label[10];       // "BD TUN" / "FX 2.0" / "SC FX1 P2"
  uint8_t sidecar;      // 1: the Sidecar's own
  int n;
  Point p[kLanePoints];
};
struct Lanes {
  uint32_t length;      // ticks
  int nblocks, nmutes, nscenes, nperf, ntempo, nlanes, nmarks;
  Block blocks[kLaneBlocks];   // the patterns played (v = pattern)
  Block mutes[kLaneMutes];     // muted spans (v = track)
  Block scenes[kLaneScenes];   // v = scene 1..12
  Point perf[kLanePoints];
  Point tempo[kLaneTempo];     // v = bpm x 10
  uint32_t marks[16];
  Lane lane[kLanes];
};
void build_lanes(const Take& t, Lanes* out);
const char* param_label(int trk, int msb, int lsb, char* b, int cap);   // "BD TUN" (the NRPN chart, a few named)

#if CORE_POLY
// ---- the engine (song.cpp)
enum State : uint8_t { kOff = 0, kArming, kArmed, kRec, kSaving, kConfirm, kRestoring, kReplayReady, kReplaying, kLoading,
                      kUndoing };
const char* state_name(int s);
void byte(uint8_t b);                 // the Rytm -> host MIDI stream, every byte
void tick(uint32_t ms);               // the control loop
bool idle();                          // a card job: true if it did one
void folder(char* out, int cap);       // "/.rytm/songs/<key>" (core/project_key.h; the old rule's folder renamed once)
bool command(const char* p, char* out, int cap);   // "song", "song=arm|disarm|replay|confirm|cancel|stop|mark|check|
                                                   //  list|load:NAME|next|prev|settings:on|off|show|undo"
struct Status {
  uint8_t state;
  uint32_t t_now, length, events;
  uint32_t tempo;           // bpm x 120 now
  int pattern, take_i, takes;
  uint32_t problems;        // config_problems (recording / replay as the state needs)
  int8_t cfg_wrong;         // config_wrong of the Rytm's MIDI CONFIG now (-1 not read)
  bool cfg_valid, settings, recovered;
  bool undo;                // UNDO REPLAY is offered now (song=undo would run)
  bool undo_t5;             // the remote page takes T5: offered, or ended within the last second (a late press: said)
  char name[40], project[24], msg[64];
};
Status status();
// The display's lanes: built on the control side after a change (lanes_publish, from tick() when the display wants
// them), copied out on any core (lanes_read: true and *seen moved when newer).
void lanes_want(bool on);
bool lanes_read(Lanes* out, uint32_t* seen);
uint32_t show_requests();             // the Rytm's / the console's "show the SONG page" count (the display follows)
// The Rytm's SONG page in its page list: armed / recording / replaying, a take loaded, takes on the card, or asked for
// (song=show, the touchscreen's arm): not on a fresh card, so LEFT on SIDECAR still lands where it did
bool page_shown();
// The remote SONG page (core/ui/page_song.cpp): T1 ARM, T2 REPLAY, T3 MARK, T5 UNDO REPLAY (while offered, and 1 s
// after: "UNDO EXPIRED"); YES / NO answer a question
bool ui_key(int code);
int ui_question();                    // 2: the page shows the MIDI CONFIG how-to
// tests
void reset();
void test_frames(int64_t (*fn)());    // the frame clock (default seq::frames)
const Take& current();
void want_files();                    // the project's store fill: the song folder's .backups.txt (the backup slots' turn)
bool restore_busy();
#endif

}  // namespace song
}  // namespace core
