// core/song/song.cpp - see song.h: the engine side of SONG RECORD (the MIDI tap, the states, the card jobs, the console,
// the display's lanes). Control side only; cold.
#include "song/song.h"

#if CORE_POLY
#include <stdarg.h>
#include <string.h>

#include "fmt/fmt.h"
#include "fx/send_fx.h"
#include "hooks.h"
#include "machines.h"
#include "platform.h"
#include "pool/pool.h"
#include "seq/rytm_dump.h"
#include "seq/seq.h"
#include "store/store.h"
#include "ui/remote.h"

namespace core {
namespace song {

// ---- paths: the project's folder (core/project_key.h). Phase 0: a folder named by the old rule (spaces and dots as
// '_', no trim, "_none") is renamed to the key's once, when the key's does not exist yet; with both there the key's is
// used and the old one left alone.
void folder(char* b, int cap) {
  static char s_done[kKeyMax + 1] = {1, 0};   // the project the migration was checked for (none yet)
  char k[kKeyMax], o[kKeyMax];
  project_key(project, k, sizeof k);
  sniprintf(b, cap, "/.rytm/songs/%s", k);
  if (!card_ok || !strcmp(s_done, project)) return;
  sniprintf(s_done, sizeof s_done, "%s", project);
  project_key_song_v0(project, o, sizeof o);
  if (!strcmp(o, k)) return;
  char old[64];
  sniprintf(old, sizeof old, "/.rytm/songs/%s", o);
  Fs fs;
  if (fs.exists(b) || !fs.exists(old)) return;
  if (fs.rename(old, b)) log(CORE_STR("song: %s -> %s (the project key, once)"), old, b);
  else log(CORE_STR("song: cannot rename %s to %s"), old, b);
}

namespace {

constexpr int kSxMax = 16384;              // the longest dump (pattern X, 14988 B)
constexpr int kPiece = 3069;               // a long SysEx in pieces (3-byte aligned: the Pi's records <= 4096 B)
// A request unanswered: asked again after 1.5 s, then every 3 s (4 tries, 10.5 s in all), then the next: never more than
// one request in 1.5 s (a pattern answer is 15 KB the Rytm packs and sends; docs/SONG.md "Traffic to the Rytm")
constexpr uint32_t kDumpTimeoutMs = 1500;
constexpr int kDumpTries = 4;
constexpr int kMaxTakes = 64;
constexpr int kClockTrack = 0;             // BD's step ticks (seq's 2E clock mask while armed / replaying)
constexpr uint32_t kUndoMs = 30000;        // UNDO REPLAY: offered until 30 s stopped (UX review T14)
constexpr uint32_t kGraceMs = 1000;        // T5 kept by the page after the undo ended (a late press: "UNDO EXPIRED")
constexpr int kBackupSlots = 8;            // backup sets kept on the card (.backup-1 .. -8, in turn)

int64_t default_frames() { return (int64_t)seq::frames(); }
int64_t (*g_frames)() = default_frames;

struct Engine {
  State state = kOff;
  Take take;                 // the take recorded or loaded (the display's, the replay's)
  bool take_valid = false;
  uint32_t take_gen = 1;
  Recorder rec;
  Replayer rep;
  Restore rst;
  Clock rclk;                // the replay's clock (the Rytm's t)
  int rsw_to = -1;           // a SWITCH seen, the re-anchor at the next tick
  MidiConfig cfg;            // the newest GLOBAL X read
  bool cfg_valid = false;
  bool settings = false;     // restore settings X too
  bool trace = false;        // song=log:on: each replayed message and the Rytm's switches / mutes logged with t
  char msg[64] = "";
  // the MIDI stream
  uint8_t sx[kSxMax];
  int sxn = -1;
  uint8_t run = 0, d[2] = {0, 0};
  int need = 0, have = 0;
  bool has2d = false;        // the mod's 2D reports seen (FA / FC then not the transport)
  int transport = 0;         // 0 stop 1 play 2 pause
  seq::Follower fol;
  double bpm = 0;
  int pattern = -1;
  // dump fetches for the arm snapshot / the config check
  uint8_t fetch_q[4];
  int fetch_n = 0, fetch_i = 0;
  int fetch_want = -1, fetch_tries = 0;
  uint32_t fetch_ms = 0;
  bool fetch_for_arm = false;
  // the clock mask
  bool clock_on = false;
  // card: the journal
  Buf jbuf;
  File jf;
  bool jopen = false;
  uint32_t jbar = 0, jsync_bar = 0;
  int jevents = 0, jwritten = 0;   // events in the journal so far
  char jpath[96] = "";
  // card: a writer (the song file, a backup, a recovered take)
  JsonGen gen;
  Buf wbuf;
  File wf;
  bool writing = false;
  int write_kind = 0;        // 1 song, 3 recovered (the backup: fmt's posted queue, EnginePort::backup)
  char wpath[96] = "", wtmp[100] = "";
  Take scratch;              // a backup / a recovered take being written
  // UNDO REPLAY's backup sets (song.h): set[cur] the undo's, set[1 - cur] the one a confirm writes; each entry a file,
  // formatted in idle() (jobs), into the RAM store and fmt's posted queue (fid: its write's id, 0 once it is on the card)
  Take set[2][kUndoMax];
  int setn[2] = {0, 0}, setslot[2] = {0, 0};
  bool setmk[2] = {false, false};   // its mutes known (2D 05 seen)
  bool setbad[2] = {false, false};  // a file of it refused or failed
  bool setpart[2] = {false, false}; // it covers the take's start only (the take plays more than kUndoMax patterns)
  int slotn[kBackupSlots + 1] = {};  // each slot's file count (.backups.txt): a reused slot's extra -K files removed
  bool relink_wait = false;          // Restore's linked kit: its file on the card, then relink_done
  int relink_k = -1;                 // (its entry in set[cur])
  // job_step: one file at a time, a piece of its JSON a pass
  int jcur = -1;
  JsonGen jgen;
  Buf jtext;
  int8_t js = -1, jk = -1, jso = 0;
  int uparse = -1;                   // undo(): the set's next file read back (an idle pass each), -1 none
  uint32_t fid[2][kUndoMax] = {};
  struct Job { int8_t s, k, store_only; } jobs[2 * kUndoMax];   // (store_only: the RAM store's copy again, not the card)
  int njobs = 0;
  int cur = 0;
  bool bwait = false;            // set[1 - cur]: the confirm's files on their way (Restore::backup_done once all are)
  bool bnew = false;             // set[1 - cur] backed up for the restore running (the undo's once it writes)
  uint32_t bseq = 0;             // the next set's number (.backups.txt; 0 not read yet)
  // UNDO REPLAY
  bool undo = false;             // set[cur] can be put back (undo_ok: offered now)
  bool undo_settings = false;    // its replay wrote settings X: the undo does
  uint32_t undo_since = 0;       // since when the Rytm is stopped and the replay off or ready (kUndoMs)
  uint32_t undo_gone = 0;        // when it ended (T5's grace)
  bool undo_was = false;
  char undo_why[40] = "";
  Take utake[kUndoMax];          // set[cur] read back from the RAM store
  UndoRun urun;
  int32_t mutes = -1;            // the Rytm's mute mask (2D 05), -1 not seen
  uint32_t pc_ms = 0;            // the Sidecar's own last PC (its 2D 02 report is not the user's pattern change)
  // card: a reader
  File rf;
  Buf rtext;
  JsonRead rd;
  int read_stage = 0;        // 0 none, 1 reading the file, 2 parsing
  char rpath[96] = "";
  bool read_recover = false; // a journal (recovered into scratch, then written)
  // the takes in the project's folder
  char takes[kMaxTakes][40];
  int ntakes = 0, take_i = -1;
  bool list_due = false;
  char project_seen[24] = "\x01";
  bool recover_scan_due = false;
  // the Sidecar's own (polled)
  uint8_t fx_send[kTracks][2], fx_knob[2][6], fx_route;
  int8_t mach[kTracks];
  int16_t src[kTracks];   // rui1038: P1's 14 bits as last recorded (-2: not polled yet)
  uint32_t poll_ms = 0;
  // the display
  bool lanes_on = false;
  uint32_t lanes_gen_built = 0, lanes_ms = 0;
  uint32_t show_req = 0;
  int ui_question = 0;       // the remote page: 1 replay confirm, 2 the config's how-to
  uint32_t ms_now = 0;
  bool inited = false;
};
Engine* E = nullptr;

// ---- the published lanes (two buffers; the reader takes the newer, a counter tells it was not overwritten)
Lanes g_lanes[2];
volatile uint32_t g_lanes_n = 0;

Engine& eng() {
  if (!E) {
    E = new Engine;
    E->take.clear(), E->scratch.clear(), E->rst.backup.clear();
    for (int k = 0; k < kUndoMax; ++k) E->set[0][k].clear(), E->set[1][k].clear(), E->utake[k].clear();
    config_default(&E->cfg);
    for (int t = 0; t < kTracks; ++t) E->mach[t] = -2, E->src[t] = -2;
  }
  return *E;
}

double now_frame() { return (double)g_frames(); }

void set_msg(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsniprintf(eng().msg, sizeof eng().msg, fmt, ap);
  va_end(ap);
  log(CORE_STR("song: %s"), eng().msg);
}

// ---- out
void out(const uint8_t* m, int n) {
  if (n > 3 && m[0] == 0xF0 && n > kPiece) {   // a long dump: in 3-byte aligned pieces (the platform's record limit)
    for (int i = 0; i < n; i += kPiece) to_rytm(m + i, n - i < kPiece ? n - i : kPiece);
    return;
  }
  to_rytm(m, n);
}

void request(uint8_t id) {
  uint8_t m[15];
  dump_request(m, id);
  to_rytm(m, 15);
}

bool card() { return card_ok; }

// ---- the take list (the project's *.json, newest name last: take-001 ...)
void list_takes() {
  Engine& e = eng();
  e.ntakes = 0;
  if (!card()) return;
  char d[64];
  folder(d, sizeof d);
  File dir, f;
  if (!dir.open(d, kRead)) return;
  while (f.openNext(&dir, kRead) && e.ntakes < kMaxTakes) {
    char nm[64];
    f.getName(nm, sizeof nm);
    bool isdir = f.isDir();
    f.close();
    int l = (int)strlen(nm);
    if (isdir || nm[0] == '.' || l < 6 || strcmp(nm + l - 5, ".json")) continue;
    nm[l - 5] = 0;
    sniprintf(e.takes[e.ntakes++], 40, "%s", nm);
  }
  dir.close();
  for (int i = 1; i < e.ntakes; ++i)   // sorted by name
    for (int j = i; j > 0 && strcmp(e.takes[j - 1], e.takes[j]) > 0; --j) {
      char t[40];
      memcpy(t, e.takes[j], 40), memcpy(e.takes[j], e.takes[j - 1], 40), memcpy(e.takes[j - 1], t, 40);
    }
  e.take_i = -1;
  for (int i = 0; i < e.ntakes; ++i)
    if (e.take_valid && !strcmp(e.takes[i], e.take.name)) e.take_i = i;
}

// a free name: take-NNN after the highest (no wall clock on the Pi: numbered)
void new_name(char* out, int cap) {
  Engine& e = eng();
  int hi = 0;
  for (int i = 0; i < e.ntakes; ++i)
    if (!strncmp(e.takes[i], "take-", 5)) {
      int v = 0;
      for (const char* p = e.takes[i] + 5; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (*p - '0');
      if (v > hi) hi = v;
    }
  // the list may be short (it failed to read, or holds kMaxTakes of more): the name is checked on the card too, so a
  // take is never written over another (data-safety audit 1042)
  char d[64], p[112];
  folder(d, sizeof d);
  Fs fs;
  for (int n = hi + 1; n < hi + 1000; ++n) {
    sniprintf(out, cap, "take-%03d", n);
    sniprintf(p, sizeof p, "%s/%s.json", d, out);
    if (card() && fs.exists(p)) continue;
    sniprintf(p, sizeof p, "%s/%s.jsonl", d, out);
    if (card() && fs.exists(p)) continue;
    return;
  }
}

// ---- the clock mask (seq's 2E): BD ticks while armed or replaying
void clock(bool on) {
  Engine& e = eng();
  if (on == e.clock_on) return;
  e.clock_on = on;
  seq::set_clock_tracks(on ? 1u << kClockTrack : 0);
}

// ---- fetches (the arm snapshot, the config check): one request at a time
void fetch_start(const uint8_t* ids, int n, bool for_arm) {
  Engine& e = eng();
  memcpy(e.fetch_q, ids, n);
  e.fetch_n = n, e.fetch_i = 0, e.fetch_for_arm = for_arm;
  e.fetch_want = ids[0], e.fetch_ms = e.ms_now, e.fetch_tries = 1;
  request(ids[0]);
}
void fetch_next() {
  Engine& e = eng();
  if (++e.fetch_i < e.fetch_n) {
    e.fetch_want = e.fetch_q[e.fetch_i], e.fetch_ms = e.ms_now, e.fetch_tries = 1;
    request((uint8_t)e.fetch_want);
    return;
  }
  e.fetch_want = -1;
  if (e.fetch_for_arm && e.state == kArming) {
    e.state = kArmed;
    char p[48];
    config_text(config_problems(e.cfg, false), p, sizeof p);
    if (p[0]) set_msg("armed: check the MIDI CONFIG");
    else set_msg(e.take.kit.n ? "armed: PLAY records" : "armed (no start snapshot)");
  }
}

void sidecar_snapshot(Take& t) {
  t.has_fx = true;
  for (int k = 0; k < kTracks; ++k) t.fx_send[k][0] = fx::control.send[k][0], t.fx_send[k][1] = fx::control.send[k][1];
  for (int s = 0; s < 2; ++s)
    for (int j = 0; j < 6; ++j) t.fx_knob[s][j] = fx::control.knob[s][j];
  t.fx_route = fx::control.route;
}

void undo_end(const char* why);
void undo_take_over();
bool undo_watch(bool playing_too = false) {   // an edit now ends UNDO REPLAY (the Rytm's state no longer the replay's)
  Engine& e = eng();
  return e.undo && (e.state == kOff || e.state == kReplayReady || (playing_too && e.state == kReplaying));
}

void poll_sidecar(bool record) {
  Engine& e = eng();
  double f = now_frame();
  bool edit = false;   // (the Sidecar's send FX: the undo would put them back)
  for (int k = 0; k < kTracks; ++k)
    for (int s = 0; s < 2; ++s)
      if (fx::control.send[k][s] != e.fx_send[k][s]) {
        e.fx_send[k][s] = fx::control.send[k][s], edit = true;
        if (record) e.rec.sidecar(kScSend, (uint16_t)(k * 2 + s), e.fx_send[k][s], f, e.ms_now);
      }
  for (int s = 0; s < 2; ++s)
    for (int j = 0; j < 6; ++j)
      if (fx::control.knob[s][j] != e.fx_knob[s][j]) {
        e.fx_knob[s][j] = fx::control.knob[s][j], edit = true;
        if (record) e.rec.sidecar(kScKnob, (uint16_t)(s * 6 + j), e.fx_knob[s][j], f, e.ms_now);
      }
  if (fx::control.route != e.fx_route) {
    e.fx_route = fx::control.route, edit = true;
    if (record) e.rec.sidecar(kScRoute, 0, e.fx_route, f, e.ms_now);
  }
  if (edit && undo_watch()) undo_end("the send FX changed");
  for (int t = 0; t < kTracks; ++t) {
    int vb = ui::remote_track_machine(t);
    if (vb != e.mach[t]) {
      if (record && e.mach[t] != -2 && vb >= 0) e.rec.machine(t, vb, f, e.ms_now);
      e.mach[t] = (int8_t)vb;
    }
    // rui1038: GRANULAR SRC / WAVETABLE TBL / MULTISAMPLE INST at 14 bits (the bank), from the SRC view's 2A and the
    // Sidecar's own 48 writes (ui::remote_track_src14); their 7-bit CC / NRPN on the track's channel are dropped
    // rui1045: DSP SAMPLE's SMP (P11) the same way (ui::remote_track_src_param 10): its NRPN 1:11 / CC 27 dropped
    int m = vb >= 0 && vb != 0x7F ? vb & 0x78 : -1;
    bool smp = m == kDspSample, banked = smp || m == kGranMachine || m == kWaveMachine || m == kMultiMachine || m == kChopMachine;
    e.rec.banked = (uint16_t)(banked ? e.rec.banked | 1u << t : e.rec.banked & ~(1u << t));
    e.rec.banked_smp = (uint16_t)(smp ? e.rec.banked_smp | 1u << t : e.rec.banked_smp & ~(1u << t));
    int s14 = banked ? ui::remote_track_src14(t) : -1;
    if (e.src[t] == -2) e.src[t] = (int16_t)s14;   // the first poll: what it was before (not a move)
    else if (s14 >= 0 && s14 != e.src[t]) {
      if (record) e.rec.src(t, s14, f, e.ms_now, smp ? 10 : 0);
      e.src[t] = (int16_t)s14;
    }
  }
}

// ---- the port the replay and the restore talk through
struct EnginePort : Port {
  void send(const uint8_t* m, int n) override {
    Engine& e = eng();
    if ((m[0] & 0xF0) == 0xC0) e.pc_ms = e.ms_now;
    if (e.trace && e.state == kReplaying && n <= 3)
      log(CORE_STR("song: out t %u: %02x %02x %02x"), (unsigned)(e.rclk.at(now_frame()) + (uint32_t)e.rep.offset), m[0], m[1],
          n > 2 ? m[2] : 0);
    out(m, n);
  }
  void request(uint8_t id) override { song::request(id); }
  bool backup(const Take& p0, const Take* const* more, int n, bool partial) override;
  bool relink(int k, const Buf& kit) override;
  bool playing() override { return eng().transport != 0; }
  void sidecar(const Ev& ev) override {
    if (ev.a == kScSend && ev.b < kTracks * 2) fx::control.send[ev.b / 2][ev.b % 2] = (uint8_t)ev.v;
    else if (ev.a == kScKnob && ev.b < 12) fx::control.knob[ev.b / 6][ev.b % 6] = (uint8_t)ev.v;
    else if (ev.a == kScRoute) fx::control.route = (uint8_t)ev.v;
  }
  void sidecar_start(const Take& t) override {
    for (int k = 0; k < kTracks; ++k) fx::control.send[k][0] = t.fx_send[k][0], fx::control.send[k][1] = t.fx_send[k][1];
    for (int s = 0; s < 2; ++s)
      for (int j = 0; j < 6; ++j) fx::control.knob[s][j] = t.fx_knob[s][j];
    fx::control.route = t.fx_route;
  }
};
EnginePort g_port;

// ---- the writer
bool write_begin(const Take* t, const char* path, int kind) {
  Engine& e = eng();
  if (e.writing || !card()) return false;
  char d[64];
  folder(d, sizeof d);
  Fs fs;
  fs.mkdir(d);
  sniprintf(e.wpath, sizeof e.wpath, "%s", path);
  sniprintf(e.wtmp, sizeof e.wtmp, "%s.tmp", path);
  if (!e.wf.open(e.wtmp, kWrite)) return false;
  e.gen.begin(t, false);
  e.writing = true, e.write_kind = kind;
  return true;
}

// ---- UNDO REPLAY's backup sets (song.h): no card call here (the control core); the files formatted in idle()
void backup_path(char* out, int cap, int slot, int k) {
  char d[64];
  folder(d, sizeof d);
  if (k) sniprintf(out, cap, "%s/.backup-%d-%d.json", d, slot, k);
  else sniprintf(out, cap, "%s/.backup-%d.json", d, slot);
}
void index_path(char* out, int cap) {
  char d[64];
  folder(d, sizeof d);
  sniprintf(out, cap, "%s/.backups.txt", d);
}
// .backups.txt: "next N" and "counts C1 .. C8" (each slot's files), from the RAM store (the project load read it)
void index_read() {
  Engine& e = eng();
  if (e.bseq) return;
  e.bseq = 1;
  memset(e.slotn, 0, sizeof e.slotn);
  char p[96];
  index_path(p, sizeof p);
  const uint8_t* d;
  uint32_t n;
  if (store::get(p, &d, &n) != store::kPresent || n > 200) return;
  char t[201];
  memcpy(t, d, n), t[n] = 0;
  if (!strncmp(t, "next ", 5)) {   // (no sscanf in the Pi's libc)
    unsigned v = (unsigned)strtoul(t + 5, nullptr, 10);
    if (v) e.bseq = v;
  }
  const char* c = strstr(t, "counts");
  for (int k = 1; c && k <= kBackupSlots; ++k) {   // (no strspn / strcspn in the Pi's libc)
    while (*c && (*c < '0' || *c > '9')) ++c;
    if (!*c) break;
    e.slotn[k] = atoi(c);
    while (*c >= '0' && *c <= '9') ++c;
  }
}
void index_write() {
  Engine& e = eng();
  char p[96], t[160];
  index_path(p, sizeof p);
  int n = sniprintf(t, sizeof t, "next %u\ncounts", (unsigned)e.bseq);
  for (int k = 1; k <= kBackupSlots; ++k) n += sniprintf(t + n, sizeof t - n, " %d", e.slotn[k]);
  n += sniprintf(t + n, sizeof t - n, "\n");
  store::put(p, (const uint8_t*)t, (uint32_t)n);
  fmt::post_queue(p, t, (uint32_t)n);
}
void job_add(int s, int k, bool store_only = false) {
  Engine& e = eng();
  e.fid[s][k] = 0;
  if (e.njobs < (int)(sizeof e.jobs / sizeof e.jobs[0])) e.jobs[e.njobs++] = {(int8_t)s, (int8_t)k, (int8_t)store_only};
  else e.setbad[s] = true;
}
bool jobs_of(int s) {
  Engine& e = eng();
  for (int j = 0; j < e.njobs; ++j)
    if (e.jobs[j].s == s) return true;
  return false;
}
// one file of a set: its JSON a piece an idle pass, then into the RAM store and fmt's posted queue
bool job_step() {
  Engine& e = eng();
  if (e.js < 0) {
    if (!e.njobs) return false;
    Engine::Job j = e.jobs[0];
    memmove(e.jobs, e.jobs + 1, sizeof(Engine::Job) * --e.njobs);
    e.js = j.s, e.jk = j.k, e.jso = j.store_only;
    e.jgen.begin(&e.set[j.s][j.k], false);
    e.jgen.per = 64;
    e.jtext.clear();
    return true;
  }
  if (e.jgen.next(e.jtext)) return true;
  int s = e.js, k = e.jk;
  e.js = e.jk = -1;
  char p[96];
  backup_path(p, sizeof p, e.setslot[s], k);
  bool ok = e.jtext.n > 0 && store::put(p, e.jtext.p, (uint32_t)e.jtext.n);
  ok = ok && (e.jso || fmt::post_queue(p, (const char*)e.jtext.p, (uint32_t)e.jtext.n, &e.fid[s][k]));
  e.jtext.free();
  if (!ok) {   // (refused: not on the card - never undone from RAM alone)
    store::mark(p, store::kAbsent);
    e.setbad[s] = true, e.fid[s][k] = 0;
  }
  return true;
}
// a set's files: 1 all on the card, 0 still on their way, -1 one refused or failed
int set_written(int s) {
  Engine& e = eng();
  if (jobs_of(s) || e.js == s) return e.setbad[s] ? -1 : 0;
  int r = 1;
  for (int k = 0; k < e.setn[s]; ++k) {
    if (!e.fid[s][k]) continue;
    fmt::PostResult x = fmt::post_result(e.fid[s][k]);
    if (x == fmt::kPostOk) e.fid[s][k] = 0;   // (settled: post_result keeps the newest results only)
    else if (x == fmt::kPostPending) r = r < 0 ? r : 0;
    else e.setbad[s] = true;
  }
  return e.setbad[s] ? -1 : r;
}
void set_entry(Take& t, int pattern) {
  Engine& e = eng();
  t.clear();
  sniprintf(t.project, sizeof t.project, "%s", project);
  t.cfg = e.cfg;
  t.pattern = pattern;
}

// The confirm's backup (Restore: nothing written before backup_done): set[1 - cur] from p0 and more, its files queued -
// .backups.txt advanced and posted first (fmt's queue keeps the order: a set on the card is never named "next")
bool EnginePort::backup(const Take& p0, const Take* const* more, int n, bool partial) {
  Engine& e = eng();
  if (!card() || e.bwait) return false;
  int s = 1 - e.cur;
  for (int j = 0; j < e.njobs;)   // (an earlier confirm's files not formatted yet: this set replaces them)
    if (e.jobs[j].s == s) memmove(e.jobs + j, e.jobs + j + 1, sizeof(Engine::Job) * (--e.njobs - j));
    else ++j;
  if (e.js == s) e.js = e.jk = -1, e.jtext.clear();
  index_read();
  int slot = (int)((e.bseq - 1) % kBackupSlots) + 1, count = 1 + n;
  if (e.setn[e.cur] && slot == e.setslot[e.cur]) {   // (never the slot of the set an undo may still read: the next)
    ++e.bseq;
    slot = (int)((e.bseq - 1) % kBackupSlots) + 1;
  }
  for (int k = count; k < e.slotn[slot]; ++k) {   // the slot's older, longer set: its extra files removed
    char p[96];
    backup_path(p, sizeof p, slot, k);
    store::mark(p, store::kAbsent);
    fmt::post_remove(p);
  }
  e.slotn[slot] = count;
  ++e.bseq;
  index_write();
  e.setslot[s] = slot, e.setbad[s] = false, e.setmk[s] = e.mutes >= 0, e.setpart[s] = partial;
  Take& t = e.set[s][0];
  set_entry(t, p0.pattern);
  t.kit.add(p0.kit.p, p0.kit.n), t.pat.add(p0.pat.p, p0.pat.n), t.settings.add(p0.settings.p, p0.settings.n);
  t.tempo = (uint32_t)(e.bpm * 120 + 0.5), t.mutes = e.mutes >= 0 ? (uint32_t)e.mutes : 0;
  sidecar_snapshot(t);
  sniprintf(t.name, sizeof t.name, "backup %u: 1 of %d", (unsigned)(e.bseq - 1), count);
  for (int k = 0; k < n; ++k) {
    Take& x = e.set[s][k + 1];
    set_entry(x, more[k]->pattern);
    x.kit.add(more[k]->kit.p, more[k]->kit.n), x.pat.add(more[k]->pat.p, more[k]->pat.n);
    sniprintf(x.name, sizeof x.name, "backup %u: %d of %d", (unsigned)(e.bseq - 1), k + 2, count);
  }
  e.setn[s] = count;
  for (int k = 0; k < count; ++k) job_add(s, k);
  e.bwait = true;
  return true;
}

// the take's pattern X linked the start pattern to another kit: that kit's state into entry k (kit2), on the card
// before kit X is written (Restore waits: relink_done)
bool EnginePort::relink(int k, const Buf& kit) {
  Engine& e = eng();
  if (e.bnew) undo_take_over();
  int s = e.cur;
  if (k < 0 || k >= e.setn[s]) return false;
  e.set[s][k].kit2.clear();
  e.set[s][k].kit2.add(kit.p, kit.n);
  job_add(s, k);
  e.relink_wait = true, e.relink_k = k;
  return true;
}

bool write_step() {
  Engine& e = eng();
  if (!e.writing) return false;
  e.wbuf.clear();
  bool more = e.gen.next(e.wbuf);
  bool ok = true;
  if (e.wbuf.n && (int)e.wf.write(e.wbuf.p, e.wbuf.n) != e.wbuf.n) ok = false;
  if (more && ok) return true;
  e.wf.close();
  Fs fs;
  fs.remove(e.wpath);
  ok = ok && fs.rename(e.wtmp, e.wpath);
  if (!ok) fs.remove(e.wtmp);
  e.writing = false;
  int kind = e.write_kind;
  if (ok && e.jpath[0] && kind == 1) fs.remove(e.jpath), e.jpath[0] = 0;
  if (kind == 3 && ok) fs.remove(e.rpath);
  set_msg(ok ? "saved %s" : "could not write %s", strrchr(e.wpath, '/') + 1);
  if (kind == 1 && e.state == kSaving) e.state = kOff;
  e.list_due = true;
  return true;
}

// ---- the journal
void journal_open() {
  Engine& e = eng();
  if (!card()) return;
  char d[64];
  folder(d, sizeof d);
  Fs fs;
  fs.mkdir(d);
  sniprintf(e.jpath, sizeof e.jpath, "%s/%s.jsonl", d, e.take.name);
  e.jbuf.clear();
  JsonGen g;
  g.begin(&e.take, true);
  while (g.next(e.jbuf)) {}
  e.jevents = e.jwritten = 0;
  e.jbar = e.jsync_bar = 0;
  e.jopen = e.jf.open(e.jpath, kWrite);
  if (e.jopen) {
    e.jf.write(e.jbuf.p, e.jbuf.n);
    e.jbuf.clear();
  } else {
    set_msg("cannot write the journal %s", strrchr(e.jpath, '/') ? strrchr(e.jpath, '/') + 1 : e.jpath);
  }
}

// events not yet journalled into the line buffer; written every bar or 64 events (or at STOP), the file closed and
// reopened every 4 bars (the card's sync)
bool journal_flush(bool force) {
  Engine& e = eng();
  if (!e.jpath[0]) return false;
  int n = e.take.n;
  uint32_t t = e.rec.on ? e.rec.clk.at(now_frame()) : e.take.end;
  uint32_t bar = t / kBar;
  if (!force && n - e.jwritten < 64 && bar == e.jbar) return false;
  // the newest event may still be thinned (its value replaced): journal up to the one before it unless forced
  int upto = force ? n : n - 1;
  char b[160];
  for (; e.jwritten < upto; ++e.jwritten) {
    int k = event_json(e.take.ev[e.jwritten], b, sizeof b);
    e.jbuf.add(b, k), e.jbuf.addc('\n');
  }
  e.jbar = bar;
  if (force) e.jbuf.addf("{\"end\":%u}\n", (unsigned)e.take.end);
  if (!e.jbuf.n) return false;
  if (!e.jopen) e.jopen = e.jf.open(e.jpath, kAppend);
  if (e.jopen) e.jf.write(e.jbuf.p, e.jbuf.n);
  e.jbuf.clear();
  if (force || bar >= e.jsync_bar + 4) {
    e.jf.close(), e.jopen = false;
    e.jsync_bar = bar;
  }
  return true;
}

// ---- the reader (load a take / a journal to recover)
bool read_begin(const char* path, bool recover) {
  Engine& e = eng();
  if (e.read_stage || !card()) return false;
  if (!e.rf.open(path, kRead)) return false;
  sniprintf(e.rpath, sizeof e.rpath, "%s", path);
  e.rtext.clear();
  e.read_stage = 1, e.read_recover = recover;
  return true;
}

bool read_step() {
  Engine& e = eng();
  if (e.read_stage == 1) {
    if (!e.rtext.reserve(e.rtext.n + 16384 + 1)) { e.rf.close(), e.read_stage = 0; return true; }
    int k = e.rf.read(e.rtext.p + e.rtext.n, 16384);
    if (k > 0) {
      e.rtext.n += k;
      e.rtext.p[e.rtext.n] = 0;
      return true;
    }
    e.rf.close();
    if (e.read_recover) {   // a journal: into scratch, then written as <name>.json
      char err[48];
      e.read_stage = 0;
      if (!recover_journal((const char*)e.rtext.p, e.rtext.n, &e.scratch, err, sizeof err)) {
        set_msg("cannot recover %s", strrchr(e.rpath, '/') ? strrchr(e.rpath, '/') + 1 : e.rpath);
        Fs fs;
        char bad[100];
        sniprintf(bad, sizeof bad, "%s.bad", e.rpath);
        fs.rename(e.rpath, bad);
        return true;
      }
      char p[96];
      sniprintf(p, sizeof p, "%s", e.rpath);
      p[strlen(p) - 1] = 0;   // .jsonl -> .json
      log(CORE_STR("song: recovered %d events from %s"), e.scratch.n, e.rpath);
      if (!write_begin(&e.scratch, p, 3)) e.recover_scan_due = true;
      return true;
    }
    ++e.take_gen;
    e.take_valid = false;
    e.rd.begin((const char*)e.rtext.p, e.rtext.n, &e.take);
    e.read_stage = 2;
    return true;
  }
  if (e.read_stage == 2) {
    if (e.rd.step(400)) return true;
    e.read_stage = 0;
    e.take_valid = e.rd.ok;
    ++e.take_gen;
    if (e.state == kLoading) e.state = kOff;
    if (e.rd.ok) {
      set_msg("loaded %s", e.take.name);
      for (int i = 0; i < e.ntakes; ++i)
        if (!strcmp(e.takes[i], e.take.name)) e.take_i = i;
    } else {
      set_msg("cannot read the take: %s", e.rd.err);
    }
    e.rtext.free();
    return true;
  }
  return false;
}

void load_take(int i) {
  Engine& e = eng();
  if (i < 0 || i >= e.ntakes || e.state != kOff) return;
  char d[64], p[96];
  folder(d, sizeof d);
  sniprintf(p, sizeof p, "%s/%s.json", d, e.takes[i]);
  if (read_begin(p, false)) e.state = kLoading, e.take_i = i;
}

// a journal left in the project's folder: recovered (one a pass)
bool recover_scan() {
  Engine& e = eng();
  e.recover_scan_due = false;
  if (!card() || e.state == kRec || e.read_stage || e.writing) return false;
  char d[64];
  folder(d, sizeof d);
  File dir, f;
  if (!dir.open(d, kRead)) return false;
  char found[64] = "";
  while (f.openNext(&dir, kRead)) {
    char nm[64];
    f.getName(nm, sizeof nm);
    f.close();
    int l = (int)strlen(nm);
    if (l > 6 && !strcmp(nm + l - 6, ".jsonl")) { sniprintf(found, sizeof found, "%s", nm); break; }
  }
  dir.close();
  if (!found[0]) return false;
  char p[128];
  sniprintf(p, sizeof p, "%s/%s", d, found);
  if (read_begin(p, true)) e.recover_scan_due = true;   // (more may follow)
  return true;
}

// ---- states
void arm() {
  Engine& e = eng();
  if (e.state != kOff) { set_msg("busy (%s): not armed", state_name(e.state)); return; }
  if (e.undo) undo_end("armed");
  ++e.take_gen;
  e.take.clear();
  e.take_valid = true;
  sniprintf(e.take.project, sizeof e.take.project, "%s", project);
  new_name(e.take.name, sizeof e.take.name);
  e.rec.arm(&e.take);
  e.rec.pattern = e.pattern;
  if (e.bpm > 0) e.rec.tempo(e.bpm, now_frame(), e.ms_now);
  e.state = kArming;
  clock(true);
  poll_sidecar(false);
  static const uint8_t ids[4] = {kReqGlobal, kReqKit, kReqPattern, kReqSettings};
  fetch_start(ids, 4, true);
  set_msg("arming %s: reading the Rytm", e.take.name);
  ++e.show_req;
}

void disarm() {
  Engine& e = eng();
  if (e.state == kArming || e.state == kArmed) {
    e.state = kOff;
    e.fetch_want = -1;
    e.rec.armed = false;   // (else the Rytm's mutes went on into the next take loaded: its start mutes)
    e.take_valid = false;
    ++e.take_gen;
    clock(false);
    set_msg("disarmed");
  }
}

void rec_start() {
  Engine& e = eng();
  e.take.cfg = e.cfg;
  if (!e.cfg.valid) e.take.cfg.auto_ch = e.cfg.auto_ch;
  e.rec.take = &e.take;
  sidecar_snapshot(e.take);
  e.rec.play(now_frame(), e.ms_now);
  e.state = kRec;
  ++e.take_gen;
  journal_open();
  set_msg("recording %s", e.take.name);
}

void rec_stop() {
  Engine& e = eng();
  e.rec.stop(now_frame(), e.ms_now);
  journal_flush(true);
  clock(false);
  e.state = kSaving;
  ++e.take_gen;
  char d[64], p[96];
  folder(d, sizeof d);
  sniprintf(p, sizeof p, "%s/%s.json", d, e.take.name);
  if (!write_begin(&e.take, p, 1)) {
    set_msg("cannot write %s: the journal keeps it", p);
    e.state = kOff;
  } else {
    log(CORE_STR("song: %s stopped at bar %u, %d events"), e.take.name, (unsigned)(e.take.end / kBar + 1), e.take.n);
  }
}

void replay_request() {
  Engine& e = eng();
  if (e.state != kOff || !e.take_valid || !e.take.n) { set_msg("no take to replay"); return; }
  if (e.transport != 0) { set_msg("stop the Rytm first: a replay starts stopped"); return; }
  e.state = kConfirm;
  ++e.show_req;
  set_msg("replay %s? it overwrites the working kit", e.take.name);
}

void confirm() {
  Engine& e = eng();
  if (e.state != kConfirm) { set_msg("nothing to confirm"); return; }
  if (e.transport != 0) { set_msg("stop the Rytm first"); e.state = kOff; return; }
  // (an earlier undo stays until this replay writes: a backup that fails leaves the Rytm, and that undo, as they were)
  e.state = kRestoring;
  clock(true);
  e.bnew = false;
  e.rst.begin(&e.take, e.settings, e.ms_now, g_port, e.pattern);
  if (e.rst.partial)
    set_msg("restoring: UNDO will cover the start only (the take plays %d patterns)", e.rst.plays);
  else
    set_msg("restoring: backup of %d pattern%s first", e.rst.nvisit + 1, e.rst.nvisit ? "s" : "");
}

void cancel() {
  Engine& e = eng();
  if (e.state == kRestoring) {   // only before anything is written (the backup's reads and files): its PC undone
    if (!e.rst.cancel(g_port)) { set_msg("writing to the Rytm: wait for the restore"); return; }
    e.state = kOff;
    e.bnew = false, e.bwait = false;
    clock(false);
    set_msg("%s", e.rst.msg);
  } else if (e.state == kConfirm || e.state == kReplayReady || e.state == kReplaying) {
    e.state = kOff;
    e.rep.stop();
    clock(false);
    set_msg("replay off");
  } else if (e.state == kArming || e.state == kArmed) {
    disarm();
  }
  e.ui_question = 0;
}

// ---- UNDO REPLAY
void undo_end(const char* why) {
  Engine& e = eng();
  if (!e.undo) return;
  e.undo = false;   // (utake kept: an undo running may still be sending from it)
  e.undo_was = true, e.undo_gone = e.ms_now;
  sniprintf(e.undo_why, sizeof e.undo_why, "%s", why);
  log(CORE_STR("song: UNDO REPLAY no longer offered (%s)"), why);
}

// the new set (the confirm's, backed up) becomes the undo's: once its replay writes to the Rytm
void undo_take_over() {
  Engine& e = eng();
  if (!e.bnew) return;
  e.bnew = false;
  if (e.undo) undo_end("another replay");
  e.cur = 1 - e.cur;
  e.undo_settings = e.rst.with_settings;
  e.undo_since = e.ms_now;
  // a replay that changes the pattern needs the one before it (known: a 2D 02 seen)
  if (e.set[e.cur][0].pattern < 0 && e.setn[e.cur] > 1) {
    log(CORE_STR("song: no UNDO REPLAY for this replay: the pattern before it was not known"));
    return;
  }
  e.undo = true;
}

// set[cur]'s files in the RAM store, read back and checked by UndoRun::begin: 0 all here, else the message
const char* undo_read(bool parse) {
  Engine& e = eng();
  for (int k = 0; k < e.setn[e.cur]; ++k) {
    char p[96];
    backup_path(p, sizeof p, e.setslot[e.cur], k);
    const uint8_t* d;
    uint32_t n;
    if (store::get(p, &d, &n) != store::kPresent) return "the backup is not in RAM";
    if (!parse) continue;
    char err[48];
    e.utake[k].clear();
    if (!read_json((const char*)d, (int)n, &e.utake[k], err, sizeof err)) return "the backup cannot be read";
  }
  return nullptr;
}

// offered now: the set in the RAM store, no project loading, the Rytm stopped, the replay off or ready
bool undo_ok() {
  Engine& e = eng();
  if (!e.undo || e.transport != 0 || (e.state != kOff && e.state != kReplayReady) || project_loading()) return false;
  return !undo_read(false);
}

void undo() {
  Engine& e = eng();
  if (!e.undo) { set_msg("no replay to undo"); return; }
  // a project loading: refused, as the sequencer's edits are (kLockLoading) - the undo kept
  if (project_loading()) { set_msg("project %s still loading: not undone", project); return; }
  if (e.state != kOff && e.state != kReplayReady) { set_msg("busy (%s): not undone", state_name(e.state)); return; }
  if (e.transport != 0) { set_msg("stop the Rytm first: not undone"); return; }
  // the whole set first: in the RAM store; read back a file an idle pass (undo_step), every dump checked
  // (UndoRun::begin) - nothing goes out unless all of it can
  if (const char* why = undo_read(false)) { set_msg("%s: nothing written", why); return; }
  if (e.state == kReplayReady) e.rep.stop(), clock(false);
  e.state = kUndoing;
  e.uparse = 0;
  set_msg("undoing the replay (%d pattern%s)", e.setn[e.cur], e.setn[e.cur] > 1 ? "s" : "");
}

// undo()'s read back: one file of the set an idle pass, then the run begun
bool undo_step() {
  Engine& e = eng();
  if (e.state != kUndoing || e.uparse < 0) return false;
  int k = e.uparse;
  char p[96];
  backup_path(p, sizeof p, e.setslot[e.cur], k);
  const uint8_t* d;
  uint32_t n;
  char err[48];
  e.utake[k].clear();
  const char* why = store::get(p, &d, &n) != store::kPresent ? "the backup is not in RAM"
                    : !read_json((const char*)d, (int)n, &e.utake[k], err, sizeof err) ? "the backup cannot be read" : nullptr;
  if (why) {
    e.uparse = -1, e.state = kOff;
    set_msg("%s: nothing written", why);
    return true;
  }
  if (++e.uparse < e.setn[e.cur]) return true;
  e.uparse = -1;
  const Take* list[kUndoMax];
  for (int j = 0; j < e.setn[e.cur]; ++j) list[j] = &e.utake[j];
  e.urun.begin(list, e.setn[e.cur], e.undo_settings, e.setmk[e.cur] ? (int32_t)e.utake[0].mutes : -1, e.ms_now, g_port);
  if (e.urun.st == UndoRun::kFailed) e.state = kOff, set_msg("%s", e.urun.msg);
  return true;
}

// a played replay reached pattern `to` (the Rytm's 2D 03): one not backed up at confirm ends the undo at once (nothing
// is held or read during the replay)
void undo_reached(int to) {
  Engine& e = eng();
  if (!e.undo) return;
  for (int k = 0; k < e.setn[e.cur]; ++k)
    if (e.set[e.cur][k].pattern == to) return;
  undo_end(e.setpart[e.cur] ? "the replay left its start: UNDO covered only that" : "the replay reached a pattern not backed up");
  if (e.setpart[e.cur]) set_msg("UNDO ended: the take plays more than %d patterns (its start only)", kUndoMax);
  else set_msg("UNDO ended: the replay reached %c%02d, not backed up", 'A' + to / 16, to % 16 + 1);
}

// ---- the 2C / 2D reports
void on_mod(const uint8_t* m, int n) {
  Engine& e = eng();
  if (n < 8 || m[5] != 1) return;
  const uint8_t* b = m + 6;
  int nb = n - 7;
  double f = now_frame();
  if (m[4] == 0x2C) {
    if (nb < 5 || nb != 5 + 4 * b[4]) return;
    uint32_t blk = (uint32_t)b[0] << 21 | b[1] << 14 | b[2] << 7 | b[3];
    int64_t rf = e.fol.unwrap(blk) * 32;
    e.fol.sample((int64_t)f, rf);
    double hf = (double)e.fol.to_host(rf);
    for (int i = 0; i < b[4]; ++i) {
      const uint8_t* q = b + 5 + 4 * i;
      if (q[0] != kClockTrack) continue;
      if (e.rec.on) e.rec.tick(q[1], q[2], q[3], hf);
      if (e.state == kReplaying) {
        e.rclk.tick(q[1], q[2], q[3], hf);
        if (e.rsw_to >= 0) e.rep.switched(e.rsw_to, e.rclk.t, g_port), e.rsw_to = -1;   // on its step 0
      }
    }
    return;
  }
  if (m[4] != 0x2D || nb < 1) return;
  e.has2d = true;
  int k = b[0];
  const uint8_t* a = b + 1;
  int na = nb - 1;
  if (k == 1 && na == 5) {   // TRANSPORT
    int st = a[0], was = e.transport;
    e.transport = st;
    if (st == 1 && was == 0) {
      if (e.state == kArmed || e.state == kArming) rec_start();
      else if (e.state == kReplayReady) {
        e.rclk.start(f);
        e.rep.begin(&e.take);
        e.rep.send_stop = !e.cfg_valid || e.cfg.transport_receive;   // fix1041: the Rytm's own setting (GLOBAL X)
        e.rep.start(g_port);
        e.state = kReplaying;
        set_msg("replaying %s", e.take.name);
      }
    } else if (st == 1 && was == 2) {
      if (e.rec.on) e.rec.cont(f, e.ms_now);
      if (e.state == kReplaying) e.rclk.cont(f);
    } else if (st == 2) {
      if (e.rec.on) e.rec.pause(f, e.ms_now);
      if (e.state == kReplaying) e.rclk.pause(f);
    } else if (st == 0) {
      if (e.state == kRec) rec_stop();
      else if (e.state == kReplaying) {
        e.rep.stop();
        clock(false);
        e.state = kOff;
        set_msg("replay done: %u sent, %u not replayable", (unsigned)e.rep.sent, (unsigned)e.rep.skipped);
      }
    }
  } else if (k == 2 && na == 2) {
    if (a[0] != e.pattern && e.pattern >= 0 && e.state == kOff && e.ms_now - e.pc_ms > 1000 && undo_watch())
      undo_end("the pattern changed");   // (the user's: not the report of a PC the Sidecar sent within the second)
    e.pattern = a[0];
    e.rec.pattern_state(a[0], a[1] == 0x7F ? -1 : a[1], f, e.ms_now);
  } else if (k == 3 && na == 6) {
    if (e.trace) log(CORE_STR("song: switch %d -> %d at t %u"), a[0], a[1],
                     (unsigned)(e.state == kReplaying ? e.rclk.at(f) + (uint32_t)e.rep.offset : e.rec.clk.at(f)));
    e.pattern = a[1];
    e.rec.switched(a[0], a[1], f, e.ms_now);
    if (e.state == kReplaying) {
      e.rclk.switch_seen();
      e.rsw_to = a[1];   // re-anchored at the next tick (the new pattern's step 0)
      undo_reached(a[1]);
    }
  } else if (k == 5 && na == 2) {
    if (e.trace) log(CORE_STR("song: mutes %03x at t %u"), (unsigned)(a[0] | a[1] << 7),
                     (unsigned)(e.state == kReplaying ? e.rclk.at(f) + (uint32_t)e.rep.offset : e.rec.clk.at(f)));
    e.rec.mute_mask((uint32_t)(a[0] | a[1] << 7), f, e.ms_now);
    e.mutes = (int32_t)((a[0] | a[1] << 7) & 0xFFF);
  } else if (k == 7 && na == 3) {
    e.bpm = (a[0] << 14 | a[1] << 7 | a[2]) / 120.0;
    e.rec.tempo(e.bpm, f, e.ms_now);
    e.rclk.tempo(e.bpm);
  } else if (k == 0x0C && na == 1) {
    e.rec.fill(a[0] != 0, f, e.ms_now);
  }
}

void on_dump(const uint8_t* m, int n) {
  Engine& e = eng();
  uint8_t id;
  if (!dump_ok(m, n, &id)) return;
  if (e.rst.busy()) {
    e.rst.dump_in(id, m, n, e.ms_now, g_port);
    return;
  }
  if (e.urun.busy()) {
    e.urun.dump_in(id, m, n, e.ms_now, g_port);
    return;
  }

  if (e.fetch_want < 0 || id != (uint8_t)(e.fetch_want - 0x10)) return;
  if (!e.fetch_for_arm && id != kReqGlobal - 0x10) {   // song=state: the working buffer's payload, fingerprinted
    uint32_t h = 2166136261u;
    for (int i = 10; i < n - 5; ++i) h = (h ^ m[i]) * 16777619u;
    log(CORE_STR("song: state %s %08x (%d B)"), id == kReqKit - 0x10 ? "kit" : "pattern", (unsigned)h, n);
  } else if (id == kReqGlobal - 0x10) {
    uint8_t u[kGlobalBytes + 8];
    int k = unpack7(m + 10, n - 15, u, sizeof u);
    MidiConfig c = e.cfg;
    if (parse_global(u, k, &c)) e.cfg = c, e.cfg_valid = true;
    if (e.fetch_for_arm) e.take.global.clear(), e.take.global.add(m, n);
  } else if (e.fetch_for_arm && e.state == kArming) {
    Buf& b = id == kReqKit - 0x10 ? e.take.kit : id == kReqPattern - 0x10 ? e.take.pat : e.take.settings;
    b.clear();
    b.add(m, n);
  }
  fetch_next();
}

void on_sysex(const uint8_t* m, int n) {
  if (n >= 6 && m[1] == 0x7D && m[2] == 0x52 && m[3] == 0x54 && (m[4] == 0x2C || m[4] == 0x2D)) on_mod(m, n);
  else if (n >= 15 && m[1] == 0 && m[2] == 0x20 && m[3] == 0x3C) on_dump(m, n);
}

void on_channel(uint8_t st, uint8_t d1, uint8_t d2) {
  Engine& e = eng();
  // a knob or a mute on the Rytm (its CC / NRPN out - what the Sidecar sends it is not echoed): edited, also while the
  // replay plays
  if ((st & 0xF0) == 0xB0 && undo_watch(true)) undo_end("the Rytm was edited");
  if ((st & 0xF0) == 0xC0 && e.transport == 1 && !e.rec.on && e.state != kReplaying)
    e.cfg.auto_ch = (uint8_t)((st & 15) + 1);   // the PC at PLAY: the auto channel
  if (e.state == kArmed || e.state == kArming || e.state == kRec) e.rec.channel(st, d1, d2, now_frame(), e.ms_now);
}

void on_realtime(uint8_t b) {
  Engine& e = eng();
  double f = now_frame();
  if (b == 0xF8) {
    if (e.rec.on) e.rec.clk.f8(f);
    if (e.state == kReplaying) e.rclk.f8(f);
    return;
  }
  if (e.has2d) return;   // the mod's TRANSPORT says it (with its block)
  uint8_t m[13] = {0xF0, 0x7D, 0x52, 0x54, 0x2D, 1, 1, 0, 0, 0, 0, 0, 0xF7};
  if (b == 0xFA) m[7] = 1;
  else if (b == 0xFB) m[7] = 1, e.transport = e.transport == 0 ? 2 : e.transport;
  else if (b == 0xFC) m[7] = 0;
  else return;
  on_mod(m, 13);
  e.has2d = false;
}

void publish_lanes() {
  Engine& e = eng();
  uint32_t n = g_lanes_n;
  Lanes& o = g_lanes[(n / 2 + 1) & 1];
  if (e.take_valid) build_lanes(e.take, &o);
  else memset(&o, 0, sizeof o);
  if (e.state == kRec) {
    uint32_t t = e.rec.clk.at(now_frame());
    if (t > o.length) o.length = t;
  }
  __atomic_store_n(&g_lanes_n, n + 2, __ATOMIC_RELEASE);
  e.lanes_gen_built = e.take_gen;
}

}  // namespace

CORE_COLD const char* state_name(int s) {
  static const char* const k[] = {"off", "arming", "armed", "recording", "saving", "confirm", "restoring", "replay ready",
                                  "replaying", "loading", "undoing"};
  return s >= 0 && s <= kUndoing ? k[s] : "?";
}

CORE_COLD void byte(uint8_t b) {
  Engine& e = eng();
  if (b >= 0xF8) { on_realtime(b); return; }
  if (b == 0xF0) { e.sxn = 0, e.sx[e.sxn++] = b; return; }
  if (e.sxn >= 0) {
    if (b == 0xF7) {
      if (e.sxn < kSxMax) {
        e.sx[e.sxn++] = b;
        on_sysex(e.sx, e.sxn);
      }
      e.sxn = -1;
      return;
    }
    if (b & 0x80) {   // (a status inside: dropped, then read as a status below)
      if (e.sxn > 64) log(CORE_STR("song: a SysEx of %d bytes cut by %02x"), e.sxn, b);
      e.sxn = -1;
    }
    else {
      if (e.sxn < kSxMax) e.sx[e.sxn++] = b;
      return;
    }
  }
  if (b & 0x80) {
    if (b >= 0xF0) { e.run = 0; return; }   // system common: not ours
    e.run = b, e.have = 0, e.need = (b & 0xE0) == 0xC0 ? 1 : 2;
    return;
  }
  if (!e.run) return;
  e.d[e.have++] = b;
  if (e.have >= e.need) {
    on_channel(e.run, e.d[0], e.need == 2 ? e.d[1] : 0);
    e.have = 0;
  }
}

CORE_COLD void tick(uint32_t ms) {
  Engine& e = eng();
  e.ms_now = ms;
  if (!e.inited) {
    e.inited = true;
    poll_sidecar(false);
  }
  if (strcmp(e.project_seen, project)) {   // a project named: its takes, its journals recovered
    sniprintf(e.project_seen, sizeof e.project_seen, "%s", project);
    e.list_due = e.recover_scan_due = true;
    undo_end("another project");
  }
  if (e.bwait) {   // the confirm's set on the card (each file's own result), or not: the restore told
    int w = set_written(1 - e.cur);
    if (w) {
      e.bwait = false;
      if (w < 0) {
        for (int k = 0; k < e.setn[1 - e.cur]; ++k) {   // (not on the card: never undone from RAM alone)
          char p[96];
          backup_path(p, sizeof p, e.setslot[1 - e.cur], k);
          store::mark(p, store::kAbsent);
        }
        e.setn[1 - e.cur] = 0;
      } else {
        e.bnew = e.rst.st == Restore::kBackupWrite;
        set_msg("backup written: .backup-%d (%d file%s)", e.setslot[1 - e.cur], e.setn[1 - e.cur], e.setn[1 - e.cur] > 1 ? "s" : "");
      }
      e.rst.backup_done(w > 0, ms, g_port);
    }
  }
  if (e.relink_wait) {   // Restore's linked kit on the card (its entry's file, written again with kit2)
    int c = e.cur, k = e.relink_k;
    bool queued = e.js == c && e.jk == k;
    for (int j = 0; j < e.njobs; ++j) queued |= e.jobs[j].s == c && e.jobs[j].k == k;
    fmt::PostResult r = queued ? fmt::kPostPending : e.fid[c][k] ? fmt::post_result(e.fid[c][k]) : fmt::kPostFailed;
    if (r != fmt::kPostPending) {
      e.relink_wait = false;
      if (r == fmt::kPostOk) {
        e.fid[c][k] = 0;
      } else {   // not on the card: kit2 dropped (kit X is not written, that kit stays as it is); the card keeps the
                 // entry's file before it (the atomic write), the RAM store gets it again; the undo kept
        e.set[c][k].kit2.clear();
        e.fid[c][k] = 0, e.setbad[c] = false;
        job_add(c, k, true);
      }
      e.rst.relink_done(r == fmt::kPostOk, ms, g_port);
    }
  }
  if (e.undo && set_written(e.cur) < 0) undo_end("a backup file could not be written");
  if (e.fetch_want >= 0 && ms - e.fetch_ms > (e.fetch_tries > 1 ? 2 * kDumpTimeoutMs : kDumpTimeoutMs)) {   // (a request the Rytm missed [seen in rytm-emu]: again)
    if (e.fetch_tries < kDumpTries) {
      ++e.fetch_tries, e.fetch_ms = ms;
      request((uint8_t)e.fetch_want);
    } else {
      log(CORE_STR("song: no answer to the dump request %02x"), (unsigned)e.fetch_want);
      fetch_next();
    }
  }
  if (ms - e.poll_ms >= 10) {
    e.poll_ms = ms;
    poll_sidecar(e.state == kRec);
  }
  if (e.rst.busy()) e.rst.tick(ms, g_port);
  if (e.rst.wrote && e.bnew) undo_take_over();   // (the replay wrote: its set is the undo's now)
  if (e.state == kRestoring && !e.rst.busy()) {   // (done in dump_in, failed in tick)
    e.bnew = false, e.bwait = false;   // (a set still on its way: not this replay's any more)
    if (e.rst.st == Restore::kDone) {
      e.state = kReplayReady;
      set_msg("%s: PLAY replays", e.rst.msg);
    } else {
      e.state = kOff;
      clock(false);
      set_msg("%s%s", e.rst.msg, e.undo && e.rst.wrote ? " (UNDO offered)" : "");
    }
  }
  if (e.urun.busy()) e.urun.tick(ms, g_port);
  if (e.state == kUndoing && e.uparse < 0 && !e.urun.busy()) {
    e.state = kOff;
    bool done = e.urun.st == UndoRun::kDone;
    if (done) undo_end("undone");   // once
    set_msg("%s%s", e.urun.msg, !done && e.undo ? " (UNDO kept)" : "");
  }
  // 30 s: the wait runs while the Rytm is stopped and the replay off or ready (playing holds it)
  if (e.undo) {
    if ((e.state != kOff && e.state != kReplayReady) || e.transport != 0) e.undo_since = ms;
    else if (ms - e.undo_since >= kUndoMs) undo_end("30 s stopped");
  }
  if (e.state == kReplaying) e.rep.advance(e.rclk.at(now_frame()), g_port);
  if (e.state == kReplaying && e.rep.ended) {   // fix1041: the take's end (its STOP): the replay ends here
    e.rep.stop();
    clock(false);
    e.state = kOff;
    if (e.rep.send_stop)
      set_msg("replay at its end: STOP sent (%u events)", (unsigned)e.rep.sent);
    else
      set_msg("replay at its end: stop the Rytm (TRANSP RCV off)");
  }
  // the pattern view's dumps (core/seq/rytm_dump.h) wait while a take records, and while the song's own dumps run
  rytm_dump::set_hold(e.state == kArmed || e.state == kRec ? 1
                      : e.state == kArming || e.state == kRestoring || e.state == kReplaying || e.state == kUndoing ? 2 : 0);
  if (e.lanes_on && (e.lanes_gen_built != e.take_gen || (e.state == kRec && ms - e.lanes_ms >= 250) ||
                     ms - e.lanes_ms >= 2000)) {
    e.lanes_ms = ms;
    publish_lanes();
    if (e.state == kRec) e.lanes_gen_built = 0;   // (recording: again at the next quarter second)
  }
}

CORE_COLD bool idle() {
  Engine& e = eng();
  if (e.state == kRec && journal_flush(false)) return true;
  if (undo_step()) return true;  // (UNDO REPLAY's read back: a file a pass)
  if (job_step()) return true;   // (UNDO REPLAY's backup files: formatted here a piece a pass, never in a SysEx handler)
  if (write_step()) return true;
  if (read_step()) return true;
  if (e.list_due) {
    e.list_due = false;
    list_takes();
    return true;
  }
  if (e.recover_scan_due) return recover_scan();
  return false;
}

CORE_COLD Status status() {
  Engine& e = eng();
  Status s = {};
  s.state = e.state;
  s.t_now = e.state == kRec ? e.rec.clk.at(now_frame()) : e.state == kReplaying ? e.rclk.at(now_frame()) + (uint32_t)e.rep.offset : 0;
  s.length = e.take_valid ? (e.state == kRec ? s.t_now : e.take.end) : 0;
  s.events = e.take_valid ? (uint32_t)e.take.n : 0;
  s.tempo = (uint32_t)(e.bpm * 120 + 0.5);
  s.pattern = e.pattern;
  s.take_i = e.take_i, s.takes = e.ntakes;
  bool replay = e.state >= kConfirm && e.state <= kReplaying;
  s.problems = config_problems(replay && e.take_valid ? e.take.cfg : e.cfg, replay);
  if (replay && e.cfg_valid) s.problems = config_problems(e.cfg, true);
  s.cfg_valid = e.cfg_valid, s.settings = e.settings, s.recovered = e.take_valid && e.take.recovered;
  s.cfg_wrong = (int8_t)(e.cfg_valid ? config_wrong(e.cfg) : -1);
  s.undo = undo_ok();
  s.undo_t5 = s.undo || (e.undo_was && e.ms_now - e.undo_gone < kGraceMs);
  sniprintf(s.name, sizeof s.name, "%s", e.take_valid ? e.take.name : "");
  sniprintf(s.project, sizeof s.project, "%s", project);
  sniprintf(s.msg, sizeof s.msg, "%s", e.msg);
  return s;
}

CORE_COLD void lanes_want(bool on) { eng().lanes_on = on; }
CORE_COLD bool lanes_read(Lanes* out, uint32_t* seen) {
  for (int tries = 0; tries < 4; ++tries) {
    uint32_t a = __atomic_load_n(&g_lanes_n, __ATOMIC_ACQUIRE);
    if (a == 0 || a == *seen) return false;
    memcpy(out, &g_lanes[(a / 2) & 1], sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_lanes_n, __ATOMIC_ACQUIRE) - a < 2) {
      *seen = a;
      return true;
    }
  }
  return false;
}
CORE_COLD uint32_t show_requests() { return eng().show_req; }
CORE_COLD bool page_shown() {
  Engine& e = eng();
  return e.state != kOff || e.take_valid || e.ntakes > 0 || e.show_req > 0;
}

// The remote SONG page: T1 ARM / DISARM, T2 REPLAY, T3 MARK, T4 the config's how-to; YES / NO answer the question
CORE_COLD bool ui_key(int code) {
  Engine& e = eng();
  if (code == 0x44) {   // YES
    if (e.state == kConfirm) confirm();
    else if (e.ui_question == 2) e.ui_question = 0;
    else return false;
    return true;
  }
  if (code == 0x45) {   // NO
    if (e.ui_question == 2) { e.ui_question = 0; return true; }
    if (e.state == kConfirm) { cancel(); return true; }
    return false;
  }
  if (code == 0) {
    if (e.state == kOff) arm();
    else if (e.state == kArming || e.state == kArmed) disarm();
    return true;
  }
  if (code == 1) {
    if (e.state == kOff) replay_request();
    else if (e.state == kReplayReady || e.state == kReplaying || e.state == kConfirm) cancel();
    return true;
  }
  if (code == 2) {
    e.rec.mark(now_frame(), e.ms_now);
    return true;
  }
  if (code == 3) {
    e.ui_question = e.ui_question == 2 ? 0 : 2;
    return true;
  }
  if (code == 4) {   // T5: UNDO REPLAY (lit and named in the footer only while offered; a second after: "UNDO EXPIRED")
    if (undo_ok()) undo();
    else if (!e.undo && e.undo_was && e.ms_now - e.undo_gone < kGraceMs)   // (a late press: said, not the Rytm's trig)
      set_msg(strcmp(e.undo_why, "undone") ? "UNDO EXPIRED (%s)" : "the replay is undone already", e.undo_why);
    return true;
  }
  return code >= 0 && code < 16;
}
CORE_COLD int ui_question() { return eng().ui_question; }

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (strncmp(p, "song", 4) || (p[4] && p[4] != '=')) return false;
  Engine& e = eng();
  const char* v = p[4] ? p + 5 : "";
  if (!strcmp(v, "arm")) arm();
  else if (!strcmp(v, "disarm")) disarm();
  else if (!strcmp(v, "replay")) replay_request();
  else if (!strcmp(v, "confirm")) confirm();
  else if (!strcmp(v, "cancel") || !strcmp(v, "stop")) cancel();
  else if (!strcmp(v, "mark")) e.rec.mark(now_frame(), e.ms_now);
  else if (!strcmp(v, "undo")) undo();
  else if (!strcmp(v, "show")) ++e.show_req;
  else if (!strcmp(v, "check")) {
    static const uint8_t ids[1] = {kReqGlobal};
    if (e.fetch_want < 0) fetch_start(ids, 1, false);
  } else if (!strcmp(v, "state")) {   // the working kit and pattern fingerprinted in the log (tests)
    static const uint8_t ids[2] = {kReqKit, kReqPattern};
    if (e.fetch_want < 0) fetch_start(ids, 2, false);
  } else if (!strncmp(v, "log:", 4)) e.trace = !strcmp(v + 4, "on");
  else if (!strcmp(v, "list")) e.list_due = true;
  else if (!strcmp(v, "next")) load_take(e.take_i < 0 ? e.ntakes - 1 : e.take_i + 1);
  else if (!strcmp(v, "prev")) load_take(e.take_i < 0 ? e.ntakes - 1 : e.take_i - 1);
  else if (!strncmp(v, "load:", 5)) {
    int i = -1;
    for (int k = 0; k < e.ntakes; ++k)
      if (!strcmp(e.takes[k], v + 5)) i = k;
    if (i < 0) sniprintf(out, cap, "song: no take %s", v + 5);
    else load_take(i);
    if (i < 0) return true;
  } else if (!strncmp(v, "settings:", 9)) e.settings = !strcmp(v + 9, "on");
  else if (v[0]) {
    sniprintf(out, cap, "song=arm|disarm|replay|confirm|cancel|mark|undo|check|list|load:NAME|next|prev|settings:on|off|show, not \"%s\"", v);
    return true;
  }
  Status s = status();
  char cfgt[64];
  config_text(s.problems, cfgt, sizeof cfgt);
  sniprintf(out, cap, "song: %s, take %s (%u events, bar %u), %d takes, config %s, replay %u sent %u skipped %u pcs %u reanchors; %s",
            state_name(s.state), s.name[0] ? s.name : "-", (unsigned)s.events, (unsigned)(s.t_now / kBar + 1), s.takes,
            cfgt[0] ? cfgt : "ok", (unsigned)e.rep.sent, (unsigned)e.rep.skipped, (unsigned)e.rep.pcs, (unsigned)e.rep.reanchors,
            s.msg);
  return true;
}

CORE_COLD void reset() {
  if (E) {
    if (E->jopen) E->jf.close();
    if (E->writing) E->wf.close();
    if (E->read_stage == 1) E->rf.close();
    E->take.free(), E->scratch.free(), E->rst.backup.free(), E->rst.relinked.free(), E->jtext.free();
    for (int k = 0; k < kUndoMax - 1; ++k) E->rst.extra[k].free();
    for (int k = 0; k < kUndoMax; ++k) E->set[0][k].free(), E->set[1][k].free(), E->utake[k].free();
    E->jbuf.free(), E->wbuf.free(), E->rtext.free();
    delete E;
    E = nullptr;
  }
  g_lanes_n = 0;
  eng();
}
CORE_COLD void test_frames(int64_t (*fn)()) { g_frames = fn ? fn : default_frames; }
CORE_COLD const Take& current() { return eng().take; }
CORE_COLD void want_files() {   // (the project's: folder() names the project the fill is for)
  char p[96];
  index_path(p, sizeof p);
  store::want(p);
  eng().bseq = 0;   // (read again from the store once it is full)
}
CORE_COLD bool restore_busy() { return eng().rst.busy(); }

}  // namespace song
}  // namespace core
#endif
