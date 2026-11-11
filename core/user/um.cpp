// core/user/um.cpp - see um.h. Control core only, but user_render (a render core) and json_copy (any core).
#include "../engine.h"   // idle_note (7z)
#include "um.h"
#include "inst.h"

#include <string.h>

#include "fmt/fmt.h"
#include "hooks.h"
#include "platform.h"
#include "pool/pool.h"
#include "lfo.h"
#include "rmp_api.h"
#include "rmp_api3.h"
#include "rmp_loader.h"
#if CORE_MSAMP
#include "../msamp/cardq.h"   // the package's reads on the card core (7z)
namespace cardq = core::cardq;
#endif
#include "rmp_manifest.h"
#include "rmp_meta.h"
#include "rmp_pool.h"
#include "rmp_presets.h"
#include "rmp_proto.h"
#include "rmp_resolve.h"
#include "rmp_slots.h"
#include "ui/remote.h"
#include "voices.h"

// The board's heap for the package buffer and the voices' state (weak: the C++ heap; the Pi's platform has its own).
extern "C" void* um_heap(uint32_t bytes);

namespace um {
namespace {

constexpr uint32_t kChunk = 8192;              // the card read of one idle step
constexpr uint32_t kPkgMax = RMP_HEADER_BYTES + (1u << 20) + 65536u;   // the largest package read (image <= 1 MB)
// a 1E's answer at the latest: LOADING then (0021 takes it as done; READY follows in a 1D). The Rytm's 2 s cover its
// list ask and this answer together, so well inside that.
constexpr uint32_t kAnsMs = 400;

#ifndef UM_NAMES_CACHE
#define UM_NAMES_CACHE CORE_MSAMP   // the Pi (and the host tests): 2 KB a package; not on a small board
#endif
#if UM_NAMES_CACHE
constexpr int kNamesMax = 256 + RMP_ENUM_MAX;   // enum_first is 8 bits: the last knob's names end below this
#endif
// What a Rytm caches of a list knob. 0021 since 0.4.0 says 36 in its slot asks (19 what 0, arg 1); 0.3.x's sends arg 0 and
// keeps 16: its steps store 8 k. So such a Rytm gets a list of 16, entry k the name the plugin's own zone of 8 k has (a 36-name
// list: every 2.25th), and what it shows is what plays, the other names unreachable; the knob's long name says so.
constexpr int kOldRytmNames = 16;
bool g_rytm_names36 = false;
uint8_t g_old_said;   // slots whose list was cut, logged once
struct Pkg {
  char file[80];          // card path
  char id[32];
  char name[11];          // the Rytm's list name: unique in its first 9 characters (a duplicate "~2")
  bool renamed;
  uint32_t version, compat, size, image, bss, state, cpu, crc;
  uint32_t hflags;        // the header's flags (rmp_format.h RMP_HF_*: seqx E0's max_voices and pitch)
  uint8_t api, target;
  uint16_t fp;
  bool ok;                // loadable here
  bool older;             // an older version of an id also installed newer: kept for a project that names it exactly
                          // (bundles phase 4), not in the Rytm's list, never a slot's default
  char why[40];           // else why not
  rmp_desc desc;
  rmp_labels lbl;         // labels that follow a list knob (rmp_format.h RMP_LABELS_*; nsets 0: none)
#if UM_NAMES_CACHE
  // the manifest's name table (every list knob's value names), read once at the scan: the Rytm's ENUM ASK is answered
  // from here, never from the card (send_enum runs in the SysEx handler on the control core; a card read there took
  // 3 ms a time - an underrun on every ask, and a label set following its ENGINE / TYPE asks after every switch)
  uint8_t names[kNamesMax * RMP_ENUM_NAME_BYTES];
  uint16_t nnames;        // entries cached (0: none; a table cut short: what it holds)
#endif
};

const Exec* ex = nullptr;
char g_dir[40] = "/Plugins";

// ---- the store: g_pkg (published, what everything uses) and the scan's work list
Pkg g_pkg[kStore], g_work[kStore];
int g_npkg = 0, g_nwork = 0, g_over = 0;
int g_scan = 0;            // 0 idle, 1 asked, 2 listing done: file g_scan_i next
int g_scan_i = 0;
bool g_scan_again = false;
core::File g_sf;           // the file the scan reads
uint32_t g_scan_off = 0, g_scan_crc = 0;
rmp_info g_scan_info;
char g_names[kStore][80];  // the listing
int g_nnames = 0;

// ---- the project's map and the slots
#if CORE_MSAMP
// 7z (2026-10-31): a project's map and notes are read by the card core (map_step, from idle()): until both are here
// g_map and its file names stay the old project's and everything goes on as if the project had changed a moment later.
enum : uint8_t { kMrIdle, kMrMap, kMrNotes };
uint8_t g_mr = kMrIdle;           // the read under way
bool g_mr_again = false;          // a newer project meanwhile: read again when its request ends
#endif
rmp_slot_map g_map;
char g_map_file[64] = "";
// ---- which machine a slot plays (rmp_resolve.h): the notes file next to the map, the fingerprints the Rytm reports
rmp_slot_notes g_notes;
char g_notes_file[64] = "";
rmp_slot_map g_prev;              // the previous project's map (what a fresh project inherits)
// 1042 (data safety): the project's map / notes file is there but did not parse (a newer Sidecar's line, damage):
// never written over - the slots still change in memory, the file keeps its bytes (before: the next save wrote the
// in-memory map, every slot of the unparsed file lost)
bool g_map_locked = false, g_notes_locked = false;
bool g_fresh = false;             // this project had no map and no notes: inherits once the fingerprints are in
uint32_t g_fresh_until = 0;       // ... or at this time (a Rytm that sends 0x25 gets this long to send it)
bool g_prints_rytm = false;       // a 0x25 arrived since boot: this Rytm reports the fingerprints
bool g_prints_have = false;       // a 0x25 was used for this project (then no lazy resolve from the trigs)
uint32_t g_prints_hash = 0;       // its content
bool g_prints_pending = false;    // a 0x25 waits for the scan's package list
rmp_prints g_prints;
uint16_t g_lazy_fp[kSlots];       // the trig tag last tried per slot (lazy resolve, an older Rytm)
bool g_resolve_save = false;      // the map and notes to the card (after a resolve)
uint32_t g_resolve_n = 0;         // resolves that changed something (the JSON's)
constexpr uint32_t kFreshWaitMs = 1500;
struct Slot {
  rmp_slot_state st;
  const rmp_plugin* p;
  int pkg;                 // its package in g_pkg (-1 none)
  uint32_t state_bytes, crc;
  uint16_t fp;
};
// Plugin API 3's user effects UFX1..UFX4 (core/fx/send_fx.h kFirstUser): load targets kSlots + u next to the machine
// slots - the same job, the same checks, an arena and the guard - but never in the Rytm's slot protocol (19..1E).
constexpr int kUfx = 4;
constexpr int kAll = kSlots + kUfx;
Slot g_slot[kAll];
// ---- Plugin API 3 (sdk/rmp/rmp_api3.h, docs/plugin-api-3-plan.md): per slot its API 3 table, its image's code part,
// its arena. Set by the load (job_step, the arena phase), cleared when the slot's assignment starts again (job_begin);
// read by the render cores only while the slot is READY.
struct V3 {
  const rmp_plugin* p;     // the entry table (the arena phase's, until the slot is READY)
  const rmp3_plugin* p3;   // null: a plain API 1 / 2 plugin
  const uint8_t* code;     // the image's code part (the fault offsets: "+0x1234")
  uint32_t code_bytes;
  uint8_t* base;           // the arena: one block from the pool (null: none), zeroed by the arena phase
  uint8_t* shared;         // ... the slot's shared part (null: 0 bytes)
  uint8_t* voices;         // ... then states x voice_bytes
  uint32_t shared_bytes, voice_bytes, states;
  uint32_t zero_off, zero_len;   // the arena phase: zeroed so far, in all
  uint32_t cap;            // the arena block's size (>= zero_len: a reused block may be bigger)
  bool room_said;          // a poly instance past the arena's states was logged (once a load)
  uint32_t setup_calls;    // setup() calls that asked for more (RMP3_SETUP_MORE)
  uint32_t setup_over;     // setup() calls over the limit (the second refuses)
};
V3 g_v3[kAll];
volatile uint32_t g_muted = 0;   // the guard muted the slot (bit s): a plugin call faulted, or ran late three times
uint8_t g_late[kAll];            // late control calls in a row (API 3 plugins: three mute)
uint32_t g_late_n[kAll];         // late control calls since the load (every plugin: the JSON's "late")
// the effects: which package each UFX plays (um=fx:<n>:<id>; in memory), its two states (send FX 1 and 2: in its
// arena block), live while core 2 may run it (published after both states are set up), busy while core 2 runs it
rmp_slot_entry g_fxmap[kUfx];
uint8_t* g_fxst[kUfx][2];
volatile uint8_t g_fx_live[kUfx], g_fx_busy[kUfx];
volatile uint64_t g_fx_fault[kUfx];   // core 2's fault address (the control core words the why)
volatile uint8_t g_fx_total[kUfx];
volatile uint32_t g_render_faults = 0;   // voices whose render faulted (render_fault, any core): bit vi    // core 2 muted it as the dearest when its effects together were over their share
uint32_t g_fx_lastgen[kUfx][2];       // the gen of each state's last call (N5: edges older than that are not news)
volatile uint32_t g_fx_slow_us[kUfx]; // core 2: the render that took the effect over its budget (the why), 0 none
uint8_t g_fx_over[kUfx][2];           // core 2: calls over budget in a row, per send slot
volatile bool g_fx_dead[kUfx];        // core 2 never left its render (fx_stop gave up): its states and arena kept out of reuse
volatile uint32_t g_fx_era[kUfx];     // bumped by each fx_stop: a render that began before it mutes nothing after it
// an effect's render of 12..16 frames: about half a block (the proposal: under 30 us); and all of core 2's effects in a
// block together (both send slots, every UFX): kFxTotalUs. um=fxbudget:<call>:<total> sets both (tests, the bench).
uint32_t g_fx_budget_us = 120, g_fx_total_us = 150;
constexpr int kFxOverMax = 16;        // calls (or blocks) over it in a row (a burst, a cold cache never mutes)
// core 2's account of the block it is in (by the transport's gen): every effect's time, the frames, the overs in a row
struct FxAcct { uint32_t gen, us, frames, us_u[kUfx], over; };
FxAcct g_fxa;
constexpr uint32_t kFxHugeUs = 2000;  // one call this long mutes at once: the FIFO's 2 ms
constexpr uint32_t kFxStopWaitUs = 5000;   // fx_stop's wait for core 2 (then it gives up: core 3's block watchdog sees a hang)
float g_fx_p[kUfx][2][16];            // the knobs last handed to each state (b->p_changed)
uint32_t g_fx_ev[kUfx][2][3];         // the transport's START / STOP / JUMP counts each state has seen (once each)
uint8_t g_jph = 0;               // the load's phase: 0 the package, 1 the arena (zeroed in pieces, then setup())
enum : uint8_t { kJphImage = 0, kJphArena = 1 };
constexpr uint32_t kZeroStep = 256u << 10;     // the arena zeroed per idle step (the slot LOADING meanwhile)
constexpr uint32_t kCallLateUs = 500;          // a control call's limit (the proposal's; the Mini's)
// One API 3 control call longer than the FIFO (2 ms) has already been a dropout of every track: muted at once, not
// after three (three 15 ms params() were 45 ms of silence on the Pi 5). The host tests move it (call_stall_limit).
constexpr uint32_t kCallStallUs = 2000;
uint32_t g_call_stall_us = kCallStallUs;
// init and each setup call of an API 3 plugin: 1 ms. They run on the control core between blocks; the FIFO holds 2 ms,
// so a longer call is a dropout of every track. Heavy work goes in pieces: setup returns RMP3_SETUP_MORE and is called
// again from the next idle step (at most kSetupCallsMax times).
constexpr uint32_t kSetupLateUs = 1000;
constexpr uint32_t kSetupCallsMax = 4000;
// the arenas' pool (Exec::arena): a bump allocator with a short list of returned blocks (a slot's arena comes back
// when the slot loads again, empties or takes another plugin, and is reused by any load it fits), reset with the
// code's arena (a full reload, only when the free space is there but in pieces)
uint8_t* g_pool = nullptr;
uint32_t g_pool_bytes = 0, g_pool_used = 0;
constexpr int kFree = 16;
struct FreeBlk { uint8_t* base; uint32_t cap; };
FreeBlk g_free[kFree];
int g_nfree = 0;
rmp3_host g_h3;                  // what an API 2 or 3 plugin finds in rmp_host.reserved[RMP_H_V3]
rmp_host g_host3;                // the host table of an API 3 only plugin (api 3)
// the transport snapshots (pre_block): two, the render cores read the block's (g_xp_gen & 1)
rmp3_transport g_xp[2];
volatile uint32_t g_xp_gen = 0;
constexpr uint32_t kLeadFrames = 385;   // the FIFO's 96 frames + the Rytm's measured 289-frame loop (the proposal's estimate)
uint32_t g_xp_starts = 0, g_xp_stops = 0, g_xp_jumps = 0;
// per snapshot: how many START, STOP and JUMP blocks there were up to it (a consumer out of step with the blocks, the
// send FX's effects on core 2, takes each edge once: fx_run)
uint32_t g_xp_ev[2][3];
uint32_t g_xp_evn[3];
int32_t g_xp_song = -1;
// what a block hands a plugin that declares signals while the host has no bus (RMP3_CAP_BUS): silence in, a sink out
float g_sig_zero[16];
rmp3_sig_in g_sig_none[4];
char g_why[kAll][64];    // why the slot is not READY, in words (the web page's "why"; "" when READY / EMPTY / LOADING)
uint32_t g_need = 0;       // slots to (re)load, bit s
bool g_full = false;       // reset the arena and load every slot again
uint32_t g_arena_used = 0;
uint32_t g_epoch = 1;      // bumped after code is placed (render cores' barrier)
uint32_t g_voice_epoch[8];   // the epoch each voice last rendered at (a barrier when it moved on)
// the load under way
int g_job = -1;
uint8_t* g_buf = nullptr;  // the package being read (kPkgMax, once)
// (7z: each branch timed by name for the control loop's slow-step line, core/engine.h IdleSlow)
struct UmTimer {
  const char* n;
  uint32_t t = core::now_us();
  explicit UmTimer(const char* name) : n(name) {}
  ~UmTimer() { core::idle_note(n, core::now_us() - t); }
};
#define UMT(name) UmTimer umt_(name)
core::File g_jf;
// 7z (2026-10-30): the load job's card work (open, 8 KB reads, close) goes through the card queue (core/msamp/cardq.h:
// the Pi's core 0), a step at a time, so the control core never waits on the card. A read here on the control core
// waited for core 0's hold on the card (the recorder, an upload, the black box): 15 ms passes, a dropout at a kit
// change. Without a card core (the Mac's tests) async runs at once, as before.
#if CORE_MSAMP
cardq::Req g_jrq;
bool g_jopen = false;   // the open is still to come (job_step's first part)
bool g_jclosing = false;   // the close is posted (job_step's last part, until its request is done)
bool g_jdrop = false;   // a job ended or abandoned: its request's end and the file's close still to come (jf_drain)
// The file free again (true), else called again from idle(): a request under way runs to its end, the close is a request
// too (2026-10-31: this waited for the request and closed the file here, on the control core, the card's holder's time).
// No new job posts anything before (job_step).
bool jf_drain() {
  if (!cardq::idle(&g_jrq)) return false;
  g_jrq.state = cardq::kFree;
  if (g_jf.isOpen() && !cardq::async(&g_jrq, cardq::kCloseReq, &g_jf, nullptr, 0, nullptr, 0)) return false;
  g_jdrop = false;
  return true;
}
#endif
void jf_close() {   // a job ended or abandoned (failure paths)
#if CORE_MSAMP
  g_jopen = g_jclosing = false;
  g_jdrop = true;
  jf_drain();
#else
  if (g_jf.isOpen()) g_jf.close();
#endif
}
uint32_t g_job_off = 0, g_job_len = 0, g_job_crc = 0;

// ---- voices
uint8_t* g_vstate[core::kVoices];
int8_t g_cur[core::kVoices];       // the slot whose voice_init the voice's state had; -1 none
uint8_t g_mis = 0;                 // sound-tag mismatch, bit vi
// E0 (core/inst.h): the instances' states, one per instance of the pool (core::inst::kTotal, placed at boot), each the
// state of the slot g_icur[i] (-1 none) for voice g_ivi[i]
constexpr int kInst = 32;   // = core::inst::kTotal (static_assert below)
uint8_t* g_istate[kInst];
int8_t g_icur[kInst], g_ivi[kInst];

// ---- the measured cost: each voice's render time by slot (a render core adds to its voice's row; a voice renders
// on one core at a time), summed by the control core against the slot's baseline at its last load
uint64_t g_cyc[core::kVoices][kSlots];
uint32_t g_blk[core::kVoices][kSlots];
uint64_t g_cyc0[kSlots];
uint32_t g_blk0[kSlots];
uint32_t g_cost_ms = 0;     // the JSON again once a second while something plays (its "ns" moves)
uint32_t g_cost_blk = 0;
void cost_sum(int s, uint64_t* cyc, uint32_t* blk) {
  *cyc = 0, *blk = 0;
  for (int vi = 0; vi < core::kVoices; ++vi) {
    *cyc += __atomic_load_n(&g_cyc[vi][s], __ATOMIC_RELAXED);
    *blk += __atomic_load_n(&g_blk[vi][s], __ATOMIC_RELAXED);
  }
}
// slot s's mean ns per voice block since its load (0: not played)
uint32_t cost_ns(int s, uint32_t* blocks) {
  uint64_t c;
  uint32_t b;
  cost_sum(s, &c, &b);
  c -= g_cyc0[s], b -= g_blk0[s];
  if (blocks) *blocks = b;
  if (!b) return 0;
  return (uint32_t)(c / b * 1000000000ull / core::cycles_per_second());
}

// ---- protocol
rmp_meta g_meta;
uint32_t g_1d_ms = 0;
bool g_1d_owed = false, g_1d_sent = false;
int8_t g_ans[kSlots];              // a 1E waiting for its answer: its op, -1 none
uint32_t g_ans_t[kSlots];
uint8_t g_inst[kStore];            // the Rytm's installed list: g_pkg indices of the loadable packages
int g_ninst = 0;
uint32_t g_list_print = 0;
bool g_list_owed = false;          // the Rytm asked for the list while the first scan ran: sent when it ends

// ---- the JSON (seqlock, double buffer)
char g_js[2][16384];
int g_jlen[2];
UiView g_ui[2];
volatile uint32_t g_jseq = 0;
bool g_pub = true;
bool g_mp = true;           // something 1A says may have changed: meta_poll in the next idle
uint32_t g_mp_ms = 0;
uint32_t g_pub_ms = 0;

rmp_host g_host1, g_host2;

// A plugin's log line: copied first, bounded (API 3: inside a guarded call a bad pointer faults here, before the log's
// lock is taken, so the slot is muted and no host lock stays held)
void plog(const char* m) {
  char b[128];
  int n = 0;
  if (m)
    for (; n < (int)sizeof b - 1 && m[n]; ++n) b[n] = m[n];
  b[n] = 0;
  core::log("plugin: %s", b);
}
uint32_t prandom(uint32_t* s) {
  uint32_t x = *s ? *s : 2463534242u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return *s = x;
}
const rmp_host* host_for(const rmp_plugin* p) { return p->api >= RMP_API3 ? &g_host3 : p->api >= 2u ? &g_host2 : &g_host1; }

void copy(char* d, const char* s, int cap) {
  int i = 0;
  for (; s && s[i] && i < cap - 1; ++i) d[i] = s[i];
  d[i] = 0;
}
bool ends_rmp(const char* n) {
  int l = (int)strlen(n);
  if (l < 5) return false;
  const char* e = n + l - 4;
  return e[0] == '.' && (e[1] | 32) == 'r' && (e[2] | 32) == 'm' && (e[3] | 32) == 'p';
}
#if !CORE_MSAMP || !UM_NAMES_CACHE
bool file_rd(void* ctx, uint32_t off, uint8_t* b, uint32_t n) {
  core::File* f = (core::File*)ctx;
  return f->seekSet(off) && f->read(b, n) == (int)n;
}
#endif
#if UM_NAMES_CACHE
// a package's cached name table read at the manifest's own offsets (names_at ..), as rmp_manifest_enum asks for them
struct NamesRd { const Pkg* e; };
bool names_rd(void* ctx, uint32_t off, uint8_t* b, uint32_t n) {
  const Pkg* e = ((const NamesRd*)ctx)->e;
  uint32_t at = e->desc.names_at, len = (uint32_t)e->nnames * RMP_ENUM_NAME_BYTES;
  if (off < at || (uint64_t)off - at + n > len) return false;
  memcpy(b, e->names + (off - at), n);
  return true;
}
#endif
bool buf_rd(void* ctx, uint32_t off, uint8_t* b, uint32_t n) {
  const uint32_t* len = (const uint32_t*)ctx;
  if ((uint64_t)off + n > *len) return false;
  memcpy(b, g_buf + off, n);
  return true;
}
const char* vstr(uint32_t v, char* b) {
  sniprintf(b, 12, "%u.%u.%u", (unsigned)(v >> 16), (unsigned)(v >> 8 & 255), (unsigned)(v & 255));
  return b;
}
const char* state_name(rmp_slot_state s) {
  static const char* const n[] = {"EMPTY", "READY", "MISSING", "BAD", "NO_ROOM", "MUTED", "LOADING"};
  return (unsigned)s < 7 ? n[s] : "?";
}
int find_pkg(const char* id) {   // the newest installed version of id (the Rytm's list's)
  for (int i = 0; i < g_npkg; ++i)
    if (g_pkg[i].ok && !g_pkg[i].older && strncmp(g_pkg[i].id, id, 32) == 0) return i;
  return -1;
}
// The package a slot plays (docs/BUNDLES.md "Exactness"): the exact version its map names when that one is installed
// (side by side with others: a bundle's project binds the version it carried), else the newest - which plays the slot
// only if the version wanted is in its compatible range (job_begin), else the slot is MISSING.
int find_ver(const char* id, uint32_t version) {
  for (int i = 0; i < g_npkg; ++i)
    if (g_pkg[i].ok && g_pkg[i].version == version && strncmp(g_pkg[i].id, id, 32) == 0) return i;
  return find_pkg(id);
}
const rmp_slot_entry& entry_of(int s) { return s < kSlots ? g_map.slot[s] : g_fxmap[s - kSlots]; }
int find_slot(int s) { const rmp_slot_entry& m = entry_of(s); return m.id[0] ? find_ver(m.id, m.version) : -1; }
int find_any(const char* id) {
  for (int i = 0; i < g_npkg; ++i)
    if (strncmp(g_pkg[i].id, id, 32) == 0) return i;
  return -1;
}

// ---- the slot's description, flags, state as 1A says them
// (the presets' part, below: what 1A's flag and generation follow)
uint32_t g_pscan_todo = 0;                 // slots whose preset folder is to be scanned (bit s)
int g_pscan_slot = -1;                     // the slot being scanned
bool g_pre_has[kSlots];                    // its last scan found presets (1A's flag)
uint32_t g_pre_fp[kSlots];                 // its last scan's list fingerprint (moves the 1A generation)
// Labels that follow a list knob (a package with an RMP_LABELS block, plugins/mdkit's TYPE): the slot's knob names are
// those of the value the voice being looked at has - the Rytm's selected track's voice when it plays the slot, else the
// voice whose knobs moved last on it (a report without a trig: a knob turn, a kit load), else the first voice on it.
// The description changes with it, so rmp_meta's print moves the slot's generation and the 1D makes the Rytm ask 1A
// again: its user-machine page and the touchscreen (which shows the Rytm's labels) follow TYPE. -1: the static labels.
// The set shown moves at most every kLblMs (a 1D and the Rytm's 1A each time: two tracks edited in turn do not flood).
int8_t g_lbl_vi[kSlots];        // the voice whose knobs the user moved last on the slot (-1 none)
int8_t g_lbl_cur[kSlots];       // the set the slot's description shows (-1 the static labels)
uint32_t g_lbl_ms[kSlots];      // when it last moved
uint32_t g_lbl_key[kSlots];     // the package (crc ^ size) g_lbl_cur indexes: another package's sets are other lists
constexpr uint32_t kLblMs = 150;
uint32_t lbl_key(const Pkg& e) { return e.crc ^ e.size; }
int lbl_shown(int s, const Pkg& e) { return g_lbl_key[s] == lbl_key(e) ? g_lbl_cur[s] : -1; }   // (review 5)
int lbl_set(int s, const Pkg& e) {
  if (!e.lbl.nsets) return -1;
  auto on = [s](int vi) { return vi >= 0 && vi < core::kVoices && core::v[vi].user == s && g_cur[vi] == s; };
  int vi = core::ui::remote_track_voice(core::ui::remote_selected_track(nullptr));
  if (!on(vi)) vi = g_lbl_vi[s];
  for (int w = 0; !on(vi) && w < core::kVoices; ++w) vi = on(w) ? w : -1;
  return on(vi) ? rmp_labels_pick(&e.lbl, core::v[vi].p) : -1;   // (RLB2: the TYPE among its ENGINE's names)
}
const rmp_desc* slot_desc(int s, rmp_desc* tmp) {
  const rmp_slot_entry& e = g_map.slot[s];
  if (!e.id[0]) return nullptr;
  int k = find_slot(s);
  if (k >= 0) {
    *tmp = g_pkg[k].desc;
    memcpy(tmp->name, g_pkg[k].name, sizeof g_pkg[k].name);   // the list's (maybe renamed) name
    rmp_labels_apply(&g_pkg[k].lbl, lbl_shown(s, g_pkg[k]), tmp);
    return tmp;
  }
  rmp_desc_from_id(e.id, tmp);
  return tmp;
}
int slot_flags(int s) {
  rmp_desc d;
  const rmp_desc* p = slot_desc(s, &d);
  return (p && p->page2 ? RMP_FLAG_PARAMS : 0) | (g_pre_has[s] ? RMP_FLAG_PRESETS : 0);
}
bool settled(int s) {   // (its presets too: a 1E's answer carries the generation the list moved)
  return !g_full && !(g_need >> s & 1) && g_job != s && g_scan == 0 && !(g_pscan_todo >> s & 1) && g_pscan_slot != s;
}
void lbl_update() {   // the label sets the slots show, from their voices (rate-limited)
  for (int s = 0; s < kSlots; ++s) {
    int k = g_map.slot[s].id[0] ? find_slot(s) : -1;
    int want = k >= 0 ? lbl_set(s, g_pkg[k]) : -1;
    uint32_t key = k >= 0 ? lbl_key(g_pkg[k]) : 0;
    if (key != g_lbl_key[s]) g_lbl_key[s] = key, g_lbl_cur[s] = -1, g_lbl_ms[s] = core::now_ms() - kLblMs;   // a new package: at once
    if (want == g_lbl_cur[s]) continue;
    if (core::now_ms() - g_lbl_ms[s] < kLblMs) { g_mp = true; continue; }   // (again at the next idle)
    g_lbl_cur[s] = (int8_t)want, g_lbl_ms[s] = core::now_ms();
  }
}
void meta_poll() {
  lbl_update();
  for (int s = 0; s < kSlots; ++s) {
    if (!settled(s)) continue;   // (its presets scanned first)
    const rmp_slot_entry& e = g_map.slot[s];
    rmp_desc d;
    const rmp_desc* p = slot_desc(s, &d);
    // the preset list's fingerprint too: a list change moves the generation (the Rytm's name cache)
    // and the reason (the name the Rytm shows for a non-READY slot: "!ROOM NPPR")
    int st = rmp_proto_state(g_slot[s].st);
    // and the label set shown (review 1): an RLB2 list's names are not in the description (two lists of one count
    // with the same labels would print the same), and its 1B answers from the set shown
    int k = find_slot(s);
    uint32_t lp = k >= 0 && g_pkg[k].lbl.nsets ? (uint32_t)(lbl_shown(s, g_pkg[k]) + 2) * 0x85EBCA6Bu : 0u;
    if (rmp_meta_set(&g_meta, s, rmp_meta_print(st, slot_flags(s), e.id, e.version, p) ^ g_pre_fp[s] * 2654435761u ^
                                     (uint32_t)rmp_why_of(st, RMP_WHY_NONE) * 0x9E3779B1u ^ lp))
      g_pub = true;
  }
}
void send_1d(int slot, int result) {
  uint8_t m[RMP_MSG_MAX];
  meta_poll();
  core::to_rytm(m, rmp_build_slots(m, g_meta.gen, g_meta.store_gen, slot, result));
  rmp_meta_take(&g_meta);
  g_1d_ms = core::now_ms();
  g_1d_owed = false;
  g_1d_sent = true;
}
void send_meta(int s) {
  uint8_t m[RMP_MSG_MAX];
  rmp_desc d;
  const rmp_desc* p = slot_desc(s, &d);
  if (p) {   // the unused knobs as empty labels (rmp_desc_pad_unused): the Rytm greys them instead of naming them P11
    if (p != &d) d = *p;
    rmp_desc_pad_unused(&d);
    p = &d;
    if (!g_rytm_names36)
      for (int k = 0; k < d.nknobs; ++k) {
        rmp_desc_knob& q = d.knob[k];
        if (q.enum_count <= kOldRytmNames) continue;
        char* t = q.long_name;   // "Osc 1 16/36" (13 B): shown as the knob turns
        int i = 0;
        while (i < 6 && t[i]) ++i;
        t[i++] = ' ', t[i++] = '1', t[i++] = '6', t[i++] = '/';
        t[i++] = (char)('0' + q.enum_count / 10), t[i++] = (char)('0' + q.enum_count % 10), t[i] = 0;
        q.enum_count = kOldRytmNames;
        if (!(g_old_said >> s & 1)) {
          g_old_said |= (uint8_t)(1u << s);
          core::log("um: U%d %s: this Rytm's mods hold 16 names of a list: 16 reachable (update the Sidecar mods)", s + 1, d.name);
        }
      }
  }
  int g = g_meta.gen[s];
  int st = rmp_proto_state(g_slot[s].st);
  core::to_rytm(m, rmp_build_meta_head_why(m, s + 1, g, st, slot_flags(s), p, g_slot[s].fp, rmp_why_of(st, RMP_WHY_NONE)));
  for (int part = 1; p && part < rmp_meta_nparts(p); ++part) core::to_rytm(m, rmp_build_meta_part(m, s + 1, g, part, p));
}
void send_enum(int s, int arg) {
  char names[RMP_ENUM_MAX][8];
  int n = 0;
  int k = find_slot(s);
  if (k >= 0) {
    const rmp_desc& d = g_pkg[k].desc;
    int q = rmp_desc_find(&d, arg + 1);
    // a list whose names follow another list knob (RLB2: MD's TYPE after its ENGINE): the set shown's names, from the
    // block read at the scan; the static set (-1) and every other knob: the package's own name table
    int ln = q >= 0 ? rmp_labels_enum(&g_pkg[k].lbl, lbl_shown(s, g_pkg[k]), arg + 1, names) : -1;
    if (ln >= 0) {
      n = ln;
#if UM_NAMES_CACHE
    } else if (q >= 0 && g_pkg[k].nnames) {
      NamesRd r{&g_pkg[k]};
      n = rmp_manifest_enum(&d, q, names_rd, &r, names);
    }
#else
    } else if (core::File f; q >= 0 && f.open(g_pkg[k].file, core::kRead)) {
      n = rmp_manifest_enum(&d, q, file_rd, &f, names);
      f.close();
    }
#endif
  }
  if (n < 0) n = 0;
  if (!g_rytm_names36 && n > kOldRytmNames) {   // ascending: an entry only moves down over ones already taken
    for (int i = 0; i < kOldRytmNames; ++i) memmove(names[i], names[i * n / kOldRytmNames], sizeof names[i]);
    n = kOldRytmNames;
  }
  uint8_t m[RMP_MSG_MAX];
  int first = 0;
  do {
    int c = n - first < RMP_LIST_PER_MSG ? n - first : RMP_LIST_PER_MSG;
    core::to_rytm(m, rmp_build_list(m, s + 1, arg, g_meta.gen[s], first == 0, n, first, c, names[first], 8));
    first += c;
  } while (first < n);
}
void send_installed() {
  uint8_t m[RMP_MSG_MAX];
  char names[RMP_LIST_PER_MSG][14];
  int first = 0;
  do {
    int c = g_ninst - first < RMP_LIST_PER_MSG ? g_ninst - first : RMP_LIST_PER_MSG;
    for (int i = 0; i < c; ++i) {
      memset(names[i], 0, sizeof names[i]);
      memcpy(names[i], g_pkg[g_inst[first + i]].name, 10);
    }
    core::to_rytm(m, rmp_build_list(m, 0, RMP_LIST_INSTALLED, g_meta.store_gen, first == 0, g_ninst, first, c, names[0], 14));
    first += c;
  } while (first < g_ninst);
}
// ---- presets (rmp_presets.h, the Teensy's rules): /PRESETS/<id>/ - generic <name>.txt ("P<n> <value>" lines) and
// native <name>.<preset_ext> (the plugin's load_entry), one list in name order. Scanned between requests (4 entries a
// step), loaded between requests (a file of 4 KB at most a step); the report path only decides (rmp_pv).
rmp_preset_names g_pnames;                 // ONE for all slots: the names of the slot scanned last (1B 0x20)
rmp_preset_index g_pix[kSlots];            // per slot: each entry's folder index and kind, a generation
rmp_knobs g_knobs[kSlots];                 // per slot: roles and ranges (its description)
rmp_pv g_pv[core::kVoices];                // per voice: its preset state
uint8_t g_rbuf[4096];                      // a preset file being read
uint32_t g_pre_key[kSlots];                // what the slot's presets were scanned for (its id and package)
core::File g_pdir;
#if CORE_MSAMP
// 7z (2026-10-30): the scan's and the load's card work on the card core through the card queue (as the package job's,
// g_jrq), each with a request and a File of its own: an openNext or a preset's open and read on the control core waited
// for core 0's hold on the card (4-10 ms passes, measured). Without a card core (the Mac's tests) each runs at once.
cardq::Req g_prq;                          // the scan's: the folder's open, its entries, its close
cardq::DirRec g_pent[8];                   // a kDirReq's entries
enum : uint8_t { kPsIdle, kPsOpen, kPsList, kPsClose };
uint8_t g_ps = kPsIdle;                    // where the scan's requests are
int g_ps_s = -1;                           // the slot g_pdir is open for (g_pscan_slot unless the scan was abandoned)
bool g_ps_found = false;                   // the folder opened (the scan's end logged)
cardq::Req g_lrq;                          // the load's: the preset's open, read, close
core::File g_lf;
struct PreJob { int8_t vi, which, s; uint8_t seq, stage; bool native, cut; uint16_t dir; int n; uint32_t t0; };
enum : uint8_t { kLjOpen, kLjRead, kLjClose, kLjDone };
PreJob g_lj = {-1, 0, 0, 0, kLjDone, false, false, 0, -1, 0};   // the one load in flight (vi -1: none)
#endif
uint8_t g_pre_page[kSlots];                // name pages asked for (19 what 2): bit p
uint8_t g_pv_new = 0, g_pv_wait = 0, g_last_trig = 0, g_pv_resync = 0;   // bit vi
struct PreLoad { bool on; int8_t s; uint8_t gen; int16_t entry; bool sync; float p[16]; uint8_t seq; };   // seq: moves with each new decision
PreLoad g_pl[core::kVoices][2];            // [0] the sound's own preset (written back), [1] a p-lock's play
struct HeldNote { uint8_t note, vel; bool held; uint32_t t; };
HeldNote g_hn[core::kVoices];              // a trig's note_on held until its preset load ran
uint8_t g_note_wait = 0;                   // bit vi: silent meanwhile
constexpr uint32_t kNoteHoldUs = 2000;     // a held note plays by then, loaded or not
uint32_t g_pre_again_ms = 0;               // a preset written (core 0 writes it later): every list scanned again then
bool g_pre_again = false;
char g_pre_path[72];                       // that preset (its write landed first)

// ---- pool samples for API-2 plugins (pool_info / pool_read): the sample RAM's copy of the pool slot (the Pi's heap),
// resolved and pinned by the control core; a render core only reads its voice's entry, or asks for a slot (want).
// Without sample RAM a plugin gets RMP_POOL_EMPTY (the Teensy's head/ring reader is not ported).
struct UPool {
  int slot;                // the slot resolved (1..127), 0 none
  const int16_t* pcm;      // its frames (int16 as the sample RAM holds them: ch 1, or 2 interleaved)
  uint32_t frames;
  int ch;                  // a plugin always reads mono (a stereo sample summed, pcache::mono_at)
  float rate;
  int folder, file;        // the pin held
  volatile int want;       // a slot pool_info asked for and not resolved; 0 none
  int bad;                 // a slot that cannot be had (EMPTY until the pool or the voice changes)
  bool waiting;            // want_t holds when the wait for it began
  uint32_t want_t;
};
UPool g_up[core::kVoices];
constexpr uint32_t kPoolWaitMs = 10000;    // a sample still not in RAM after this: EMPTY (failed, or no room)
#if defined(RYTM_RENDER_CORES) && RYTM_RENDER_CORES > 1
}  // namespace
}  // namespace um
extern "C" int rytm_render_core();
namespace um {
namespace {
constexpr int kCores = RYTM_RENDER_CORES < 4 ? 4 : RYTM_RENDER_CORES;
inline int this_core() { return rytm_render_core() & (kCores - 1); }
#else
constexpr int kCores = 1;
inline int this_core() { return 0; }
#endif
int g_vnow[kCores] = {-1};                 // the voice the host calls a plugin for, per core; -1 none
struct VNow {                              // within a plugin call for voice vi
  int c, was;
  VNow(int vi) : c(this_core()), was(g_vnow[c]) { g_vnow[c] = vi; }
  ~VNow() { g_vnow[c] = was; }
};

// ---- Plugin API 3: the guarded control calls (Exec::guard). Every plugin call on the control core goes through
// gcall: a fault leaves the call, the slot is muted (silent, no more calls) until it is loaded again, and the engine
// plays on (before API 3 a fault there stopped the stream and rebooted the board). API 3 plugins are held to the
// proposal's time limit too: a call over kCallLateUs three times in a row mutes the slot, one over kCallStallUs at once;
// an older plugin's late calls are only counted (the JSON's "late"): it was never told the limit.
template <class F> struct GThunk {
  static void run(void* c) { (*(F*)c)(); }
};
// f() guarded: 0 returned, 1 faulted (*pc). *us its time.
template <class F> int guarded(F f, uint64_t* pc, uint32_t* us) {
  uint32_t t0 = core::now_us();
  int r = 0;
  if (ex && ex->guard) r = ex->guard(GThunk<F>::run, &f, pc);
  else f();
  *us = core::now_us() - t0;
  return r;
}
// "+0x1234" inside the slot's code, else the address
void fault_where(int s, uint64_t pc, char* b, int cap) {
  const V3& x = g_v3[s];
  uintptr_t c0 = (uintptr_t)x.code;
  if (pc && c0 && pc >= c0 && pc < c0 + x.code_bytes) sniprintf(b, cap, "+0x%x", (unsigned)(pc - c0));
  else sniprintf(b, cap, "0x%llx (outside its code)", (unsigned long long)pc);
}
const char* slot_pfx(int s) { return s < kSlots ? "U" : "UFX"; }
int slot_no(int s) { return s < kSlots ? s + 1 : s - kSlots + 1; }
void slot_mute1(int s, const char* why) {
  __atomic_or_fetch(&g_muted, 1u << s, __ATOMIC_RELEASE);
  for (int vi = 0; vi < core::kVoices; ++vi)
    if (g_cur[vi] == s) core::v[vi].live = false;
  copy(g_why[s], why, sizeof g_why[s]);
  if (s >= kSlots) core::fx::list_changed();
  core::log("um: %s%d %s %s", slot_pfx(s), slot_no(s), entry_of(s).id, why);
  g_pub = true;
}
// A fault mutes the slot and every slot sharing its image (its globals may be what the fault damaged); a slow plugin
// (late calls, an effect over budget) only its own slot: share false
void slot_mute(int s, const char* why, bool share = true) {
  slot_mute1(s, why);
  if (!share || !g_slot[s].p) return;
  for (int t = 0; t < kAll; ++t)
    if (t != s && g_slot[t].p == g_slot[s].p && !(__atomic_load_n(&g_muted, __ATOMIC_ACQUIRE) >> t & 1)) {
      char w[64];
      sniprintf(w, sizeof w, "muted: shares the image of %s%d, which faulted", slot_pfx(s), slot_no(s));
      slot_mute1(t, w);
    }
}
template <class F> bool gcall(int s, const char* what, F f) {
  if (__atomic_load_n(&g_muted, __ATOMIC_ACQUIRE) >> s & 1) return false;
  uint64_t pc = 0;
  uint32_t us = 0;
  char w[96], at[32];
  if (guarded(f, &pc, &us)) {
    fault_where(s, pc, at, sizeof at);
    sniprintf(w, sizeof w, "muted: crashed in %s at %s", what, at);
    slot_mute(s, w);
    return false;
  }
  if (us <= kCallLateUs) {
    g_late[s] = 0;
    return true;
  }
  ++g_late_n[s];
  if (g_v3[s].p3 && us > g_call_stall_us) {
    sniprintf(w, sizeof w, "muted: %s ran %u us, over the FIFO's %u", what, (unsigned)us,
              (unsigned)g_call_stall_us);
    slot_mute(s, w, false);
    return false;
  }
  if (g_v3[s].p3 && ++g_late[s] >= 3) {
    sniprintf(w, sizeof w, "muted: %s ran %u us, three late calls in a row (limit %u)", what, (unsigned)us, (unsigned)kCallLateUs);
    slot_mute(s, w, false);
    return false;
  }
  return true;
}
bool muted(int s) { return s >= 0 && (__atomic_load_n(&g_muted, __ATOMIC_ACQUIRE) >> s & 1); }

// ---- the calls themselves (null functions are allowed in an API 3 only table: nothing to call)
// A state starts on slot s: zeroed, voice_init, then (API 3) voice_attach with its arena part. vi the voice, inst the
// poly instance (-1: the voice's own state, arena state vi; else 8 + inst). False: the slot is muted, or the state has
// no room in the arena (past its states: silent, as the proposal says).
constexpr int kInstState0 = 8;
// A state of slot s has its arena part (or needs none: a plugin without an arena keeps everything in its state). A poly
// instance past the arena's states is refused with a word (the log once, the JSON's why), never silently.
bool state_room(int s, int inst, int vi) {
  V3& x = g_v3[s];
  if (!x.p3 || !x.voice_bytes) return true;
  uint32_t idx = inst < 0 ? (uint32_t)vi : (uint32_t)(kInstState0 + inst);
  if (idx < x.states) return true;
  if (!x.room_said) {
    x.room_said = true;
    char w[64];
    sniprintf(w, sizeof w, "no room: %s %d past the arena's %u states", inst < 0 ? "voice" : "poly instance",
              inst < 0 ? vi : inst, (unsigned)x.states);
    copy(g_why[s], w, sizeof g_why[s]);
    core::log("um: %s%d %s %s (set states in its rmp3_plugin)", slot_pfx(s), slot_no(s), entry_of(s).id, w);
    g_pub = true;
  }
  return false;
}
bool c_voice_init(int s, void* st, int vi, int inst) {
  const Slot& sl = g_slot[s];
  memset(st, 0, sl.state_bytes);
  if (sl.p->voice_init && !gcall(s, "voice_init", [&] { sl.p->voice_init(st); })) return false;
  const V3& x = g_v3[s];
  if (x.p3 && x.p3->voice_attach) {
    uint32_t idx = inst < 0 ? (uint32_t)vi : (uint32_t)(kInstState0 + inst);
    rmp3_voice v;
    v.shared = x.shared, v.shared_bytes = x.shared_bytes;
    v.arena = x.voice_bytes && idx < x.states ? x.voices + (size_t)idx * x.voice_bytes : nullptr;
    v.arena_bytes = v.arena ? x.voice_bytes : 0;
    v.slot = s + 1, v.voice = vi, v.instance = inst, v.place = RMP3_AT_VOICE;
    if (!gcall(s, "voice_attach", [&] { x.p3->voice_attach(st, &v); })) return false;
  }
  return true;
}
void c_vfn(int s, rmp_voice_fn f, void* st, const char* what) {
  if (f) gcall(s, what, [&] { f(st); });
}
void c_params(int s, void* st, const float* q) {
  const rmp_plugin* p = g_slot[s].p;
  if (p->params) gcall(s, "params", [&] { p->params(st, q); });
}
void c_note_on(int s, void* st, float note, float vel) {
  const rmp_plugin* p = g_slot[s].p;
  if (p->note_on) gcall(s, "note_on", [&] { p->note_on(st, note, vel); });
}
void c_note_off(int s, void* st) {
  const rmp_plugin* p = g_slot[s].p;
  if (p->note_off) gcall(s, "note_off", [&] { p->note_off(st); });
}

// ---- the block (a render core)
float g_sig_sink[kCores][4][16];   // signals a plugin writes while the host has no bus: dropped
float* g_sig_sinkp[kCores][4];
float g_sig_val[kCores][4];
void block_fill(rmp3_block* b, int n, float* l, float* r, uint32_t place) {
  int c = this_core();
  memset(b, 0, sizeof *b);
  b->size = sizeof *b;
  b->n = n;
  b->gen = __atomic_load_n(&g_xp_gen, __ATOMIC_ACQUIRE);
  b->flags = r ? 0u : (uint32_t)RMP3_B_MONO_OUT;
  b->out[0] = l, b->out[1] = r;
  b->place = place;
  b->transport = &g_xp[b->gen & 1];
  b->sig_in = g_sig_none;
  for (int k = 0; k < 4; ++k) g_sig_sinkp[c][k] = g_sig_sink[c][k];
  b->sig_audio = g_sig_sinkp[c];
  b->sig_value = g_sig_val[c];
}

void upool_drop(int vi) {   // the voice's pin and entry go
  UPool& u = g_up[vi];
  if (u.pcm && core::psram) core::psram->unpin(u.folder, u.file);
  u.slot = 0, u.pcm = nullptr, u.want = 0, u.bad = 0, u.waiting = false;
}
int32_t h_pool_info(int32_t slot, rmp_pool_info* info) {
  int vi = g_vnow[this_core()];
  if (vi < 0 || !info || slot < 1 || slot > core::npool || core::pool[slot - 1].folder < 0) return RMP_POOL_EMPTY;
  UPool& u = g_up[vi];
  if (u.slot == slot && u.pcm) {
    info->frames = u.frames, info->channels = 1, info->rate = u.rate;
    info->where = RMP_POOL_PSRAM, info->head_frames = u.frames;
    return RMP_POOL_OK;
  }
  if (!core::psram || !core::psram->ok() || u.bad == slot) return RMP_POOL_EMPTY;
  u.want = slot;
  return RMP_POOL_NOT_READY;
}
int32_t h_pool_read(int32_t slot, uint32_t pos, float* buf, int32_t n) {
  int vi = g_vnow[this_core()];
  if (vi < 0 || !buf || n <= 0) return 0;
  const UPool& u = g_up[vi];
  if (u.slot != slot || !u.pcm) return 0;
  if (u.ch != 2) return rmp_pool_decode((const uint8_t*)u.pcm, u.frames, 1, 16, pos, buf, n);
  int32_t i = 0;   // a stereo sample: (L + R) / 2, as the summed load gave it
  for (; i < n && pos + (uint32_t)i < u.frames; ++i)
    buf[i] = (float)(((int32_t)u.pcm[2 * (pos + i)] + (int32_t)u.pcm[2 * (pos + i) + 1]) / 2) * (1.0f / 32768.0f);
  return i;
}
// The control core, between requests: the slots the voices asked for, resolved once the sample RAM holds them.
bool upool_pass() {
  bool did = false;
  for (int vi = 0; vi < core::kVoices; ++vi) {
    UPool& u = g_up[vi];
    int slot = u.want;
    if (!slot) continue;
    if (g_cur[vi] < 0 || !core::psram) { u.want = 0; continue; }
    const core::PoolEntry& e = core::pool[slot - 1];
    if (slot > core::npool || e.folder < 0) { u.want = 0; u.bad = slot; continue; }
    const int16_t* pcm;
    uint32_t frames;
    float rate;
    bool cut;
    int ch = 1;
    if (core::psram->get(e.folder, e.file, &pcm, &frames, &rate, &cut, &ch)) {
      if (u.pcm) core::psram->unpin(u.folder, u.file);
      core::psram->pin(e.folder, e.file);
      u.folder = e.folder, u.file = e.file, u.frames = frames, u.rate = rate, u.slot = slot, u.ch = ch;
      __atomic_store_n(&u.pcm, pcm, __ATOMIC_RELEASE);
      u.want = 0, u.waiting = false;
      core::log("um: voice %d pool slot %d %s in RAM (%u frames%s)", vi, slot, e.name, (unsigned)frames, cut ? ", cut" : "");
      did = true;
      continue;
    }
    if (!u.waiting) u.waiting = true, u.want_t = core::now_ms();
    else if (core::now_ms() - u.want_t > kPoolWaitMs) {
      core::log("um: voice %d pool slot %d %s: not in RAM after %u s - empty", vi, slot, e.name, (unsigned)(kPoolWaitMs / 1000));
      u.want = 0, u.waiting = false, u.bad = slot;
      continue;
    }
    core::psram->want(e.folder, e.file, false);   // loaded by idle_stream's steps
  }
  return did;
}
void on_pool_changed(int line) {   // a pool slot (or the pool: -1) changed: the voices reading it ask again
  for (int vi = 0; vi < core::kVoices; ++vi)
    if (line < 0 || g_up[vi].slot == line + 1 || g_up[vi].bad == line + 1 || g_up[vi].want == line + 1) upool_drop(vi);
}

// ---- presets
void pre_invalidate(int s) {
#if CORE_MSAMP
  if (g_pscan_slot == s) g_pscan_slot = -1;   // (its requests still run to the folder's close, the entries dropped)
#else
  if (g_pscan_slot == s) { if (g_pdir.isOpen()) g_pdir.close(); g_pscan_slot = -1; }
#endif
  rmp_preset_invalidate(&g_pix[s]);
  g_pre_has[s] = false;
  g_pre_fp[s] = 0;
  if (g_map.slot[s].id[0]) g_pscan_todo |= 1u << s;
  else g_pscan_todo &= ~(1u << s);
  g_mp = true;
}
// The slot's machine or package may have changed: its knobs again, its presets scanned again when they did.
void pre_slot(int s) {
  rmp_desc d;
  const rmp_desc* p = slot_desc(s, &d);
  rmp_knobs_from_desc(&g_knobs[s], p);
  uint32_t k = 2166136261u;
  for (const char* c = g_map.slot[s].id; *c; ++c) k = (k ^ (uint8_t)*c) * 16777619u;
  int pk = g_slot[s].pkg;
  if (pk >= 0) k = (k ^ g_pkg[pk].crc ^ g_pkg[pk].size) * 16777619u;
  if (k == g_pre_key[s]) return;
  g_pre_key[s] = k;
  pre_invalidate(s);
}
int pre_knob(int s) {   // the slot's preset knob as a P index (0..15), -1 none
  rmp_desc d;
  const rmp_desc* p = slot_desc(s, &d);
  return p && p->pre >= 0 && p->pre < p->nknobs ? p->knob[p->pre].p - 1 : -1;
}
void pscan_end(int s) {
  rmp_preset_scan_end(&g_pnames, &g_pix[s]);
  g_pre_has[s] = g_pix[s].n > 0;
  g_pre_fp[s] = g_pix[s].print;
  g_mp = g_pub = true;
}
#if CORE_MSAMP
// One step of scanning a slot's /PRESETS/<id>/: the folder opened, then its entries (8 a request), then closed - a
// card request a step, run by the card core. A request under way always runs to its end (its File and g_pent are
// not touched before): a scan abandoned meanwhile (pre_invalidate) closes the folder and drops what it read.
bool pscan_busy() { return g_pscan_slot >= 0 || g_pscan_todo || g_ps != kPsIdle; }
void pscan_step() {
  if (g_ps == kPsIdle) {
    if (!g_pscan_todo) return;
    int s = __builtin_ctz(g_pscan_todo);
    g_pscan_todo &= ~(1u << s);
    rmp_preset_scan_begin(&g_pnames, &g_pix[s], s);
    if (!core::card_ok || !g_map.slot[s].id[0]) {
      pscan_end(s);
      return;
    }
    g_ps = kPsOpen, g_ps_s = g_pscan_slot = s, g_ps_found = false;
  }
  int s = g_ps_s;
  bool live = g_pscan_slot == s;
  if (g_ps == kPsOpen) {
    char path[48];
    sniprintf(path, sizeof path, "/PRESETS/%s", g_map.slot[s].id);
    if (!cardq::async(&g_prq, cardq::kOpenReq, &g_pdir, path, 0, nullptr, 0)) return;   // (later)
    g_ps_found = g_prq.got == 1 && g_pdir.isDir();
    if (g_ps_found && live) { g_ps = kPsList; return; }
    g_ps = g_pdir.isOpen() ? kPsClose : kPsIdle;   // not there (a plain file of that name: closed), or abandoned
  } else if (g_ps == kPsList) {
    if (live || g_prq.state != cardq::kFree) {   // (a request posted: its end first)
      if (!cardq::async(&g_prq, cardq::kDirReq, &g_pdir, nullptr, 0, (uint8_t*)g_pent, sizeof g_pent)) return;
      int n = g_prq.got;
      if (live && n > 0) {
        rmp_desc d;
        const rmp_desc* p = slot_desc(s, &d);
        for (int k = 0; k < n; ++k) {
          const cardq::DirRec& e = g_pent[k];
          if (!e.dir && e.name[0] && e.name[0] != '.') rmp_preset_add(&g_pnames, &g_pix[s], e.name, e.index, p ? p->preset_ext : "");
        }
      }
      if (live && n == (int)(sizeof g_pent / sizeof g_pent[0])) return;   // (fewer: the folder's end)
    }
    g_ps = kPsClose;
  }
  if (g_ps == kPsClose) {   // (by the request's end, not the handle's flag: the close may be running)
    if (!cardq::async(&g_prq, cardq::kCloseReq, &g_pdir, nullptr, 0, nullptr, 0)) return;
    g_ps = kPsIdle;
  }
  if (g_pscan_slot != s) return;   // abandoned
  g_pscan_slot = -1;
  pscan_end(s);
  if (g_ps_found) core::log("um: presets U%d %s: %d", s + 1, g_map.slot[s].id, (int)g_pix[s].n);
}
#else
// One step of scanning a slot's /PRESETS/<id>/: open it, then kScanPerStep entries.
constexpr int kScanPerStep = 4;
void pscan_step() {
  int s = g_pscan_slot;
  if (s < 0) {
    s = __builtin_ctz(g_pscan_todo);
    g_pscan_todo &= ~(1u << s);
    rmp_preset_scan_begin(&g_pnames, &g_pix[s], s);
    char path[48];
    sniprintf(path, sizeof path, "/PRESETS/%s", g_map.slot[s].id);
    if (core::card_ok && g_map.slot[s].id[0] && g_pdir.open(path, core::kRead) && g_pdir.isDir()) {
      g_pscan_slot = s;
      return;
    }
    if (g_pdir.isOpen()) g_pdir.close();
    pscan_end(s);
    return;
  }
  rmp_desc d;
  const rmp_desc* p = slot_desc(s, &d);
  for (int k = 0; k < kScanPerStep; ++k) {   // (the Pi's openNext opens each entry: a few a step)
    core::File e;
    if (!e.openNext(&g_pdir, core::kRead)) {
      g_pdir.close();
      g_pscan_slot = -1;
      pscan_end(s);
      core::log("um: presets U%d %s: %d", s + 1, g_map.slot[s].id, (int)g_pix[s].n);
      return;
    }
    char nm[128];
    bool dir = e.isDir();
    uint32_t idx = e.dirIndex();
    if (!e.getName(nm, sizeof nm)) nm[0] = 0;
    e.close();
    if (!dir && nm[0] && nm[0] != '.' && idx <= 0xFFFFu)
      rmp_preset_add(&g_pnames, &g_pix[s], nm, (uint16_t)idx, p ? p->preset_ext : "");
  }
}
bool pscan_busy() { return g_pscan_slot >= 0 || g_pscan_todo; }
#endif
// 1B 0x20: page p of slot s's names (16 a page), only while the shared names are the slot's.
void send_presets(int s, int page) {
  const rmp_preset_names& l = g_pnames;
  uint8_t m[RMP_MSG_MAX];
  int from = page * 16, to = from + 16 < l.n ? from + 16 : l.n;
  if (from >= l.n) {
    core::to_rytm(m, rmp_build_list(m, s + 1, RMP_LIST_PRESETS, g_meta.gen[s], page == 0, l.n, from, 0, l.name[0], RMP_PRESET_NAME));
    return;
  }
  for (int f = from; f < to; f += RMP_LIST_PER_MSG) {
    int c = to - f < RMP_LIST_PER_MSG ? to - f : RMP_LIST_PER_MSG;
    core::to_rytm(m, rmp_build_list(m, s + 1, RMP_LIST_PRESETS, g_meta.gen[s], page == 0 && f == from, l.n, f, c, l.name[f],
                                    RMP_PRESET_NAME));
  }
}
// The voice's preset decision for its latest report (the Teensy's plug_pv_decide): a load runs from idle().
void pv_decide(int vi, int s, bool is_trig, const float* p) {
  uint8_t bit = (uint8_t)(1u << vi);
  g_pv_wait &= (uint8_t)~bit;
  int pre = pre_knob(s);
  if (pre < 0 || pre > 15) return;
  const rmp_preset_index& ix = g_pix[s];
  if (!rmp_preset_index_ok(&ix)) {   // the list first (a scan), decided after
    g_pv_wait |= bit;
    if (g_pscan_slot != s) g_pscan_todo |= 1u << s;
    return;
  }
  if (g_pv_resync & bit) {   // a write-back the list change dropped: decided again first, as a non-trig
    g_pv_resync &= (uint8_t)~bit;
    if (is_trig) pv_decide(vi, s, false, g_pl[vi][0].p);
  }
  rmp_pv_check(&g_pv[vi], ix.gen);
  bool sync = false;
  int e = rmp_pv_decide(&g_pv[vi], pre, ix.n, is_trig, (g_pv_new & bit) != 0, p, &sync);
  g_pv_new &= (uint8_t)~bit;
  PreLoad* P = g_pl[vi];
  bool takeover = false;
  if (is_trig && P[0].on && P[0].gen == ix.gen && ix.n > 0) {   // the sound's new preset not loaded yet
    int want = (int)p[pre];
    if (want > ix.n - 1) want = ix.n - 1;
    if (want == P[0].entry) e = want, sync = true, takeover = true;
    else if (e < 0 && want != g_pv[vi].failed) e = want, sync = false;
  }
  if (e < 0) {
    if (is_trig) P[1].on = false;
    return;
  }
  if (sync && (is_trig || !(g_note_wait >> vi & 1))) P[1].on = false;
  PreLoad& L = P[sync ? 0 : 1];
  if (!L.on || L.s != s || L.gen != ix.gen || L.entry != e || L.sync != sync || (!takeover && memcmp(L.p, p, sizeof L.p)))
    ++L.seq;   // (a load of the one before still under way is dropped: pre_job)
  L.on = true, L.s = (int8_t)s, L.gen = ix.gen, L.entry = (int16_t)e, L.sync = sync;
  if (!takeover) memmove(L.p, p, sizeof L.p);
}
void pv_params(int vi, int s, bool is_trig, float* q) {   // what params() gets: the hold, a p-lock, the offsets
  memcpy(q, core::v[vi].p, 16 * sizeof(float));
  rmp_pv_apply(&g_pv[vi], q, is_trig, core::now_ms());
  rmp_pv_effective(&g_pv[vi], &g_knobs[s], q);
}
void pv_reset(int vi) {   // the voice starts (or leaves) a slot: presets afresh
  uint8_t bit = (uint8_t)(1u << vi);
  rmp_pv_reset(&g_pv[vi]);
  g_pl[vi][0].on = g_pl[vi][1].on = false;
  g_pv_wait &= (uint8_t)~bit;
  g_note_wait &= (uint8_t)~bit;
  g_pv_resync &= (uint8_t)~bit;
}
void note_fire(int vi) {   // a trig's held note_on, now
  if (!(g_note_wait >> vi & 1)) return;
  g_note_wait &= (uint8_t)~(1u << vi);
  int s = core::v[vi].user;
  if (s < 0 || g_cur[vi] != s || g_slot[s].st != RMP_SLOT_READY || !g_slot[s].p) return;
  const HeldNote& n = g_hn[vi];
  {
    VNow now(vi);
    c_note_on(s, g_vstate[vi], (float)n.note, (n.vel < 1 ? 1 : n.vel) / 127.0f);
  }
  core::v[vi].live = !muted(s);
  if (n.held) core::log("um: trig voice %d U%d note %d (after its preset load: %u us)", vi, s + 1, n.note,
                        (unsigned)(core::now_us() - n.t));
}
// A pending load of voice vi that cannot run now (L.on cleared, as for any load taken): the voice moved on, or the list
// changed meanwhile (decided again). True: dropped.
bool pre_stale(int vi, PreLoad& L) {
  L.on = false;
  int s = L.s;
  const rmp_preset_index& ix = g_pix[s];
  const Slot& sl = g_slot[s];
  if (core::v[vi].user != s || g_cur[vi] != s || sl.st != RMP_SLOT_READY || !sl.p) return true;   // moved on
  if (!rmp_preset_index_ok(&ix) || ix.gen != L.gen || L.entry >= ix.n) {   // the list changed meanwhile: decided again
    if (L.sync || g_pl[vi][0].on) g_pv_resync |= (uint8_t)(1u << vi);
    g_pl[vi][0].on = g_pl[vi][1].on = false;
    g_pv_wait |= (uint8_t)(1u << vi);
    return true;
  }
  return false;
}
// The preset's bytes read (g_rbuf[0..n), n -1 not read; cut: the file is over 4096 B): its values to the voice.
void pre_apply(int vi, PreLoad& L, int n, bool cut, uint32_t t0) {
  int s = L.s;
  const rmp_preset_index& ix = g_pix[s];
  const Slot& sl = g_slot[s];
  bool native = ix.kind[L.entry] == RMP_PRESET_NATIVE;
  const char* name = rmp_preset_names_ok(&g_pnames, &ix, s) ? g_pnames.name[L.entry] : "";
  int16_t vals[16];
  for (int k = 0; k < 16; ++k) vals[k] = -1;
  const char* why = nullptr;
  if (n < 0) why = cut ? "over 4096 B" : "cannot read";
  else if (!native) rmp_preset_parse((const char*)g_rbuf, (uint32_t)n, vals, cut);
  else if (!rmp_load_entry_of(sl.p)) why = "the plugin has no load_entry";
  else {
    VNow now(vi);
    int32_t r = -1;
    rmp_load_entry_fn le = rmp_load_entry_of(sl.p);
    if (!gcall(s, "load_entry", [&] { r = le(g_vstate[vi], g_rbuf, n, vals); })) why = "the plugin crashed (the slot is muted)";
    else if (r != 0) why = "refused by the plugin";
  }
  if (why) {
    rmp_pv_failed(&g_pv[vi], L.entry);
    core::log("um: preset voice %d U%d %d %s: %s", vi, s + 1, (int)L.entry, name, why);
    return;
  }
  uint8_t ok[16], ov[16];
  int m = rmp_pv_load(&g_pv[vi], L.entry, L.sync, vals, &g_knobs[s], core::now_ms(), L.p, ok, ov);
  if (m) {
    uint8_t msg[RMP_MSG_MAX];
    core::to_rytm(msg, rmp_build_knobs(msg, vi, s + 1, m, ok, ov));
  }
  float q[16];
  pv_params(vi, s, g_last_trig >> vi & 1, q);
  {
    VNow now(vi);
    c_params(s, g_vstate[vi], q);
  }
  core::log("um: preset voice %d U%d %d %s (%s%s, %u us)", vi, s + 1, (int)L.entry, name, L.sync ? "written back" : "played",
            cut ? ", cut at 4096 B" : "", (unsigned)(core::now_us() - t0));
}
#if CORE_MSAMP
// The load in flight, a card request a step (its open, a read of 4096 B at most, the close), then its values - unless
// a newer decision replaced it meanwhile (its seq moved, it was dropped, or the sound's own preset came before a
// p-lock's play): dropped then, the newer one loaded next, as if it had come before this one started. The voice's
// checks (moved on, the list changed) are made again when the bytes are there. True when it ended.
bool pre_job() {
  PreJob& j = g_lj;
  for (;;) {
    if (j.stage == kLjOpen) {
      char path[48];
      sniprintf(path, sizeof path, "/PRESETS/%s", g_map.slot[j.s].id);
      if (!cardq::async(&g_lrq, cardq::kOpenAtReq, &g_lf, path, j.dir, nullptr, 0)) return false;   // (later)
      if (g_lrq.got == 1) {
        j.cut = g_lf.fileSize() > sizeof g_rbuf;
        j.stage = j.native && j.cut ? kLjClose : kLjRead;   // a native preset is whole or not at all
      } else {
        j.stage = g_lf.isOpen() ? kLjClose : kLjDone;
      }
    } else if (j.stage == kLjRead) {
      if (!cardq::async(&g_lrq, cardq::kReadReq, &g_lf, nullptr, 0, g_rbuf, sizeof g_rbuf)) return false;
      j.n = g_lrq.got;
      j.stage = kLjClose;
    } else if (j.stage == kLjClose) {   // (by the request's end, not the handle's flag)
      if (!cardq::async(&g_lrq, cardq::kCloseReq, &g_lf, nullptr, 0, nullptr, 0)) return false;
      j.stage = kLjDone;
    } else {
      break;
    }
  }
  int vi = j.vi;
  j.vi = -1;
  PreLoad* P = g_pl[vi];
  PreLoad& L = P[(int)j.which];
  if (!L.on || L.seq != j.seq || (j.which == 1 && P[0].on)) return true;   // replaced meanwhile
  if (pre_stale(vi, L)) return true;
  pre_apply(vi, L, j.n, j.cut, j.t0);
  return true;
}
#endif
// One pending load of voice vi (P[which]); with a card queue it starts here and ends in pre_job.
void pre_load(int vi, int which) {
  uint32_t t0 = core::now_us();
  PreLoad& L = g_pl[vi][which];
  if (pre_stale(vi, L)) return;
  int s = L.s;
  const rmp_preset_index& ix = g_pix[s];
  bool native = ix.kind[L.entry] == RMP_PRESET_NATIVE;
  if (!core::card_ok) {
    pre_apply(vi, L, -1, false, t0);
    return;
  }
#if CORE_MSAMP
  L.on = true;   // (until it ends: a decision meanwhile sees it pending, as before it started)
  g_lj = PreJob{(int8_t)vi, (int8_t)which, (int8_t)s, L.seq, kLjOpen, native, false, ix.dir[L.entry], -1, t0};
  pre_job();
#else
  int n = -1;
  bool cut = false;
  char path[48];
  sniprintf(path, sizeof path, "/PRESETS/%s", g_map.slot[s].id);
  core::File d, f;
  if (d.open(path, core::kRead) && f.open(&d, ix.dir[L.entry], core::kRead)) {
    cut = f.fileSize() > sizeof g_rbuf;
    if (!(native && cut)) n = f.read(g_rbuf, sizeof g_rbuf);   // a native preset is whole or not at all
  }
  if (f.isOpen()) f.close();
  if (d.isOpen()) d.close();
  pre_apply(vi, L, n, cut, t0);
#endif
}
// One preset step (the Teensy's plug_preset_pass): a waiting voice decided, else a pending load. True when it did.
bool pre_pass() {
  for (int vi = 0; vi < core::kVoices; ++vi) {
    if (!(g_pv_wait >> vi & 1)) continue;
    int s = core::v[vi].user;
    if (s < 0 || g_cur[vi] != s) { g_pv_wait &= (uint8_t)~(1u << vi); continue; }
    if (rmp_preset_index_ok(&g_pix[s])) {
      pv_decide(vi, s, g_last_trig >> vi & 1, core::v[vi].p);
      if (!g_pl[vi][0].on && !g_pl[vi][1].on) note_fire(vi);
      return true;
    }
    if (g_pscan_slot != s) g_pscan_todo |= 1u << s;
  }
#if CORE_MSAMP
  if (g_lj.vi >= 0) {   // one load at a time: the one under way first
    int vi = g_lj.vi;
    if (!pre_job()) return false;
    PreLoad* P = g_pl[vi];
    if (!P[0].on && !P[1].on && !(g_pv_wait >> vi & 1)) note_fire(vi);
    return true;
  }
#endif
  for (int vi = 0; vi < core::kVoices; ++vi) {
    PreLoad* P = g_pl[vi];
    if (!P[0].on && !P[1].on) {
      if ((g_note_wait >> vi & 1) && !(g_pv_wait >> vi & 1)) { note_fire(vi); return true; }
      continue;
    }
    pre_load(vi, P[0].on ? 0 : 1);
    if (!P[0].on && !P[1].on && !(g_pv_wait >> vi & 1)) note_fire(vi);
    return true;
  }
  return false;
}
// Preset names asked for: sent while the shared names are the slot's, else its folder scanned first.
bool pre_pages() {
  for (int s = 0; s < kSlots; ++s) {
    if (!g_pre_page[s] || !rmp_preset_names_ok(&g_pnames, &g_pix[s], s)) continue;
    int pg = __builtin_ctz(g_pre_page[s]);
    g_pre_page[s] &= (uint8_t)~(1u << pg);
    send_presets(s, pg);
    return true;
  }
  for (int s = 0; s < kSlots; ++s) {
    if (!g_pre_page[s]) continue;
    if (!g_map.slot[s].id[0]) {   // nothing assigned: no presets
      g_pre_page[s] = 0;
      uint8_t m[RMP_MSG_MAX];
      core::to_rytm(m, rmp_build_list(m, s + 1, RMP_LIST_PRESETS, g_meta.gen[s], 1, 0, 0, 0, g_pnames.name[0], RMP_PRESET_NAME));
      return true;
    }
    if (g_pscan_slot != s) g_pscan_todo |= 1u << s;
  }
  return false;
}
void notes_overdue() {   // every pass: a held note plays by kNoteHoldUs whatever runs
  uint32_t now = core::now_us();
  for (int vi = 0; vi < core::kVoices; ++vi)
    if ((g_note_wait >> vi & 1) && now - g_hn[vi].t >= kNoteHoldUs) note_fire(vi);
}
// "um=save:<slot>:<name>": a generic preset of what slot's voice plays now (the last voice on it).
bool pre_save(int s, const char* raw, char* out, int cap) {
  char name[RMP_PRESET_NAME];
  int n = 0;
  for (const char* c = raw; *c && n < RMP_PRESET_NAME - 1; ++c) {
    char x = *c;
    bool ok = (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9') || x == ' ' || x == '-' || x == '_';
    name[n++] = ok ? x : '_';
  }
  while (n && name[n - 1] == ' ') --n;
  name[n] = 0;
  if (!n || name[0] == ' ') { sniprintf(out, cap, "usage: um=save:<1..5>:<name> (letters, digits, - _ and spaces)"); return false; }
  const Slot& sl = g_slot[s];
  rmp_desc d;
  const rmp_desc* p = slot_desc(s, &d);
  if (sl.st != RMP_SLOT_READY || !p) { sniprintf(out, cap, "U%d plays nothing", s + 1); return false; }
  int vi = -1;
  for (int k = 0; k < core::kVoices; ++k)
    if (g_cur[k] == s) vi = k;
  if (vi < 0) { sniprintf(out, cap, "no voice plays U%d yet (a trig first)", s + 1); return false; }
  float q[16];
  pv_params(vi, s, false, q);
  bool roles = false;
  for (int k = 0; k < p->nknobs; ++k) roles = roles || p->knob[k].role == RMP_ROLE_PRESET || p->knob[k].role == RMP_ROLE_OFFSET;
  char txt[400];
  uint32_t len = 0;
  for (int k = 0; k < p->nknobs; ++k) {
    const rmp_desc_knob& kb = p->knob[k];
    if (k == p->pre || (roles && kb.role != RMP_ROLE_PRESET && kb.role != RMP_ROLE_OFFSET)) continue;
    float v = q[kb.p - 1];
    int iv = !(v > 0.0f) ? 0 : v >= 127.0f ? 127 : (int)(v + 0.5f);
    len += (uint32_t)sniprintf(txt + len, sizeof txt - len, "P%d %d\n", kb.p, iv);
  }
  char path[72];
  sniprintf(path, sizeof path, "/PRESETS/%s/%s.txt", g_map.slot[s].id, name);
#if CORE_MSAMP
  if (cardq::worker()) {   // (write_atomic makes the folders)
    if (!core::fmt::post_queue(path, txt, len)) { sniprintf(out, cap, "%s not written", path); return false; }
  } else
#endif
  if (!(ex->write_file ? ex->write_file(path, txt, len) : false)) {
    core::Fs fs;
    char dir[48];
    fs.mkdir("/PRESETS");
    sniprintf(dir, sizeof dir, "/PRESETS/%s", g_map.slot[s].id);
    fs.mkdir(dir);
    core::File f;
    bool ok = f.open(path, core::kWrite) && f.write(txt, len) == len;
    if (f.isOpen()) f.close();
    if (!ok) { sniprintf(out, cap, "%s not written", path); return false; }
  }
  copy(g_pre_path, path, sizeof g_pre_path);
  g_pre_again = true;   // core 0 may write it a little later: every list scanned again then
  g_pre_again_ms = core::now_ms() + 300;
  sniprintf(out, cap, "U%d preset %s saved (voice %d)", s + 1, name, vi);
  return true;
}

// ---- voices
void voice_leave(int vi) {
  int s = g_cur[vi];
  g_mis &= (uint8_t)~(1u << vi);
  if (s < 0) return;
  g_cur[vi] = -1;
  pv_reset(vi);
  const Slot& sl = g_slot[s];
  if (sl.st == RMP_SLOT_READY && sl.p)   // without g_vnow: pool_info in voice_leave answers EMPTY
    c_vfn(s, rmp_voice_leave_of(sl.p), g_vstate[vi], "voice_leave");
  upool_drop(vi);
}
void inst_leave(int i) {   // E0: instance i's state leaves its slot
  int s = g_icur[i];
  if (s < 0) return;
  g_icur[i] = -1;
  const Slot& sl = g_slot[s];
  if (sl.st == RMP_SLOT_READY && sl.p) c_vfn(s, rmp_voice_leave_of(sl.p), g_istate[i], "voice_leave");
}
void slot_stop(int s) {   // every voice on slot s leaves it (before its image changes)
  for (int vi = 0; vi < core::kVoices; ++vi)
    if (g_cur[vi] == s) { voice_leave(vi); core::v[vi].live = false; }
  for (int i = 0; i < kInst; ++i)
    if (g_icur[i] == s) inst_leave(i);
}

void resolved(uint32_t ch, const char* how);
int pkg_refs(rmp_pkg_ref* r);
void note_log(int s, const char* what);
// Lazily (a Rytm without 0x25): a trig's tag names the machine an empty or inherited slot should play.
void lazy_tag(int s, int tag) {
#if CORE_MSAMP
  if (g_mr) return;   // (not against the old project's map)
#endif
  if (g_prints_have || g_scan || !tag || g_lazy_fp[s] == tag) return;
  g_lazy_fp[s] = (uint16_t)tag;
  rmp_pkg_ref r[kStore];
  int n = pkg_refs(r);
  if (rmp_resolve_tag(&g_map, &g_notes, s, (uint16_t)tag, r, n)) {
    resolved(1u << s, "from a trig's sound tag");
  } else if (g_notes.slot[s].flags & (RMP_RF_UNKNOWN | RMP_RF_COLLISION)) {
    note_log(s, g_notes.slot[s].flags & RMP_RF_COLLISION ? "the sound's fingerprint names several machines"
                                                         : "no machine installed has the sound's fingerprint");
    g_pub = true;
  }
}
void on_prints(const uint8_t* d, int len) {
  rmp_prints p;
  if (!rmp_parse_prints(d, len, &p)) return;
  g_prints_rytm = true;
  g_prints = p;
  g_prints_pending = true;   // applied from idle(), once the package list is there
}
void on_report(int vi, int s, int was, int note, int vel, int tag) {
  core::V& x = core::v[vi];
  if (note >= 0 && tag && (g_slot[s].st != RMP_SLOT_READY || tag != g_slot[s].fp)) lazy_tag(s, tag);
  if (g_cur[vi] >= 0 && g_cur[vi] != s) voice_leave(vi);
  Slot& sl = g_slot[s];
  if (sl.st != RMP_SLOT_READY || !sl.p || muted(s) || !state_room(s, -1, vi)) {   // (API 3: a state past the arena's: silent)
    x.live = false;
    return;
  }
  uint8_t bit = (uint8_t)(1u << vi);
  if (note >= 0) {
    if (tag && tag != sl.fp) {   // the sound was made for another machine: silent until a matching trig
      if (g_mis & bit) return;
      if (g_cur[vi] == s) c_note_off(s, g_vstate[vi]);
      voice_leave(vi);
      g_mis |= bit;
      x.live = false;
      core::log("um: trig voice %d U%d: sound tag %d, the slot's %d - silent", vi, s + 1, tag, (int)sl.fp);
      return;
    }
    g_mis &= (uint8_t)~bit;
  } else if (g_mis & bit) {
    return;
  }
  void* st = g_vstate[vi];
  VNow now(vi);
  if (g_cur[vi] != s) {
    upool_drop(vi);   // before voice_init: a pool_info there asks afresh
    if (!c_voice_init(s, st, vi, -1)) {   // (zeroed there; a fault: the slot muted)
      x.live = false;
      return;
    }
    g_cur[vi] = (int8_t)s;
    pv_reset(vi);
    g_pv_new |= bit;   // its next decision is a first one
    c_vfn(s, rmp_voice_enter_of(sl.p), st, "voice_enter");
  }
  // presets (rmp_pv): what this report means for the voice's preset; a load runs from idle()
  if (note >= 0) g_last_trig |= bit;
  else g_last_trig &= (uint8_t)~bit;
  // its knobs moved (the Rytm's report, not an LFO or a step lock: core::user_report_internal): the labels follow it
  if (note < 0 && !core::user_report_internal && g_lbl_vi[s] != vi) g_lbl_vi[s] = (int8_t)vi, g_mp = true;
  pv_decide(vi, s, note >= 0, x.p);
  float q[16];
  pv_params(vi, s, note >= 0, q);
  c_params(s, st, q);
  if (muted(s)) {
    x.live = false;
    return;
  }
  if (note >= 0) {
    bool held = g_pl[vi][0].on || g_pl[vi][1].on || (g_pv_wait >> vi & 1);
    g_hn[vi] = HeldNote{(uint8_t)note, (uint8_t)vel, held, core::now_us()};
    g_note_wait |= bit;
    x.live = true;   // renders silence until the note plays (on_render)
    if (!held) note_fire(vi);
  }
}
void on_leave(int vi) {
  if (g_cur[vi] >= 0) voice_leave(vi);
}
void on_note_off(int vi) {
  int s = g_cur[vi];
  note_fire(vi);   // a held trig plays first
  VNow now(vi);
  if (s >= 0 && g_slot[s].st == RMP_SLOT_READY && g_slot[s].p) c_note_off(s, g_vstate[vi]);
}
// A render core: kBlock samples of voice vi (silence unless its slot plays it).
void on_render(int vi, float* o) {
  int s = core::v[vi].user;
  const Slot& sl = g_slot[s < 0 ? 0 : s];
  if (s < 0 || g_cur[vi] != s || sl.st != RMP_SLOT_READY || !sl.p || (g_note_wait >> vi & 1) || muted(s)) {
    for (int k = 0; k < core::kBlock; ++k) o[k] = 0.0f;
    return;
  }
  uint32_t e = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
  if (g_voice_epoch[vi] != e) {
    if (ex->barrier) ex->barrier();
    g_voice_epoch[vi] = e;
  }
  uint32_t t0 = core::cycles();
  {
    VNow now(vi);
    const rmp3_plugin* p3 = g_v3[s].p3;
    if (p3) {   // API 3: one block of data (mono here: out[1] null)
      rmp3_block b;
      block_fill(&b, core::kBlock, o, nullptr, RMP3_AT_VOICE);
      p3->render(g_vstate[vi], &b);
    } else {
      sl.p->render(g_vstate[vi], o, core::kBlock);
    }
  }
  uint32_t dt = core::cycles() - t0;
  __atomic_store_n(&g_cyc[vi][s], g_cyc[vi][s] + dt, __ATOMIC_RELAXED);
  __atomic_store_n(&g_blk[vi][s], g_blk[vi][s] + 1, __ATOMIC_RELAXED);
  for (int k = 0; k < core::kBlock; ++k)   // a NaN or a runaway level: clipped
    o[k] = o[k] == o[k] ? (o[k] > 1.0f ? 1.0f : o[k] < -1.0f ? -1.0f : o[k]) : 0.0f;
}

// API 3, a voice on OUT PRE / POST: a stereo plugin (out_channels 2) writes L and R (true); anything else L (false: R = L).
bool on_render2(int vi, float* l, float* r) {
  int s = core::v[vi].user;
  const rmp3_plugin* p3 = s >= 0 && s < kSlots ? g_v3[s].p3 : nullptr;
  if (!p3 || p3->out_channels < 2 || g_cur[vi] != s || g_slot[s].st != RMP_SLOT_READY || (g_note_wait >> vi & 1) || muted(s)) {
    on_render(vi, l);
    return false;
  }
  uint32_t e = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
  if (g_voice_epoch[vi] != e) {
    if (ex->barrier) ex->barrier();
    g_voice_epoch[vi] = e;
  }
  uint32_t t0 = core::cycles();
  {
    VNow now(vi);
    rmp3_block b;
    block_fill(&b, core::kBlock, l, r, RMP3_AT_VOICE);
    p3->render(g_vstate[vi], &b);
  }
  uint32_t dt = core::cycles() - t0;
  __atomic_store_n(&g_cyc[vi][s], g_cyc[vi][s] + dt, __ATOMIC_RELAXED);
  __atomic_store_n(&g_blk[vi][s], g_blk[vi][s] + 1, __ATOMIC_RELAXED);
  for (int k = 0; k < core::kBlock; ++k) {   // per channel: a NaN or a runaway level clipped
    l[k] = l[k] == l[k] ? (l[k] > 1.0f ? 1.0f : l[k] < -1.0f ? -1.0f : l[k]) : 0.0f;
    r[k] = r[k] == r[k] ? (r[k] > 1.0f ? 1.0f : r[k] < -1.0f ? -1.0f : r[k]) : 0.0f;
  }
  return true;
}

// ---- the map
void map_file(char* out, int cap) {
  char fn[48];
  core::pool_file(fn, sizeof fn);   // "/.rytm/pools/<name>.txt"
  sniprintf(out, cap, "/.rytm/slots/%s", fn + 13);
}
// The notes' name from the map's
void notes_file(char* d, const char* map, int cap) {
  copy(d, map, cap);
  int l = (int)strlen(d);
  if (l > 4 && !strcmp(d + l - 4, ".txt")) memcpy(d + l - 4, ".inf", 4);
}
// The project's map (b[0..n)) and notes (nb[0..k)) as read; n, k -1: missing or unreadable.
void map_take(const char* b, int n, const char* nb, int k) {
  memset(&g_map, 0, sizeof g_map);
  g_map_locked = n > 0 && !rmp_slots_parse(b, (uint32_t)n, &g_map);
  if (g_map_locked)
    core::log("um: %s is not a slot map this build reads (a newer Sidecar's, or damaged) - every slot empty, the file "
              "kept as it is (not written)", g_map_file);
  bool notes = k > 0 && rmp_notes_parse(nb, (uint32_t)k, &g_notes);
  g_notes_locked = k > 0 && !notes;
  if (g_notes_locked) core::log("um: %s is not a notes file this build reads - kept as it is (not written)", g_notes_file);
  g_fresh = rmp_resolve_open(&g_map, &g_notes, n >= 0, notes);
}
#if CORE_MSAMP
cardq::Req g_mrq;
char g_mr_map[64], g_mr_notes[64];   // the files read (g_map_file, g_notes_file once both are here)
char g_mr_b[2][512];
int g_mr_n[2];
// The map's read, a request a file (kSmallReq: recover, open, read, close): the map, the notes, then both taken at once.
// A save of the file still on its way lands first (a project left and come back to at once). The request's own step:
// a newer project meanwhile (g_mr_again) reads again from the map.
void map_step() {
  while (g_mr) {
    int w = g_mr == kMrMap ? 0 : 1;
    const char* path = w ? g_mr_notes : g_mr_map;
    if (g_mrq.state == cardq::kFree && core::fmt::pending(path)) return;
    if (!core::card_ok) g_mr_n[w] = -1;
    else if (!cardq::async(&g_mrq, cardq::kSmallReq, nullptr, path, 0, (uint8_t*)g_mr_b[w], sizeof g_mr_b[w])) return;
    else g_mr_n[w] = g_mrq.got;
    if (g_mr_again) {
      g_mr_again = false;
      g_mr = kMrMap;
    } else if (!w) {
      g_mr = kMrNotes;
    } else {
      g_mr = kMrIdle;
      copy(g_map_file, g_mr_map, sizeof g_map_file);
      copy(g_notes_file, g_mr_notes, sizeof g_notes_file);
      map_take(g_mr_b[0], g_mr_n[0], g_mr_b[1], g_mr_n[1]);
      core::log("um: project %s, map %s%s", core::project, g_map_file, g_fresh ? " (none: fresh)" : "");
    }
  }
}
void map_read() {   // (no card core, as at boot: done here; with one from idle())
  map_file(g_mr_map, sizeof g_mr_map);
  notes_file(g_mr_notes, g_mr_map, sizeof g_mr_notes);
  if (g_mr) g_mr_again = true;
  else g_mr = kMrMap;
  if (!cardq::worker()) map_step();
}
#else
// a small file read whole: its length (-1 missing / unreadable)
int read_small(const char* path, char* b, int cap) {
  core::File f;
  if (!core::card_ok || !f.open(path, core::kRead)) return -1;
  int n = f.read(b, cap);
  f.close();
  return n;
}
void map_read() {
  map_file(g_map_file, sizeof g_map_file);
  notes_file(g_notes_file, g_map_file, sizeof g_notes_file);
  char b[512], nb[512];
  core::fmt::recover(g_map_file);     // 0.3.0: a cut between write_atomic's renames - the previous map back (fmt.h)
  core::fmt::recover(g_notes_file);
  int n = read_small(g_map_file, b, sizeof b);
  int k = read_small(g_notes_file, nb, sizeof nb);
  map_take(b, n, nb, k);
  core::log("um: project %s, map %s%s", core::project, g_map_file, g_fresh ? " (none: fresh)" : "");
}
#endif
// 0.3.0 power-cut safety: core::fmt::write_atomic, not the tmp-remove-rename this did before. The slot map says which
// plugin is in which U-slot and cannot be worked out again from anything else, and a cut between that remove and that
// rename left no file at all; write_atomic reads the .tmp back and keeps the old file as .prev until the new one is in
// place (fmt.h; it makes the folders too). With a card core the write is fmt's posted queue (7z 2026-10-31: the same
// write_atomic on core 0, a newer text of a file replacing an older one, map_step's wait for it); true: taken, a failed
// write logged by post_step. Without one this is the Mac's own path.
bool write_whole(const char* path, const char* txt, uint32_t n) {
#if CORE_MSAMP
  if (cardq::worker()) return core::fmt::post_queue(path, txt, n);
#endif
  if (ex->write_file) return ex->write_file(path, txt, n);
  return core::fmt::write_atomic(path, txt, n);
}
bool map_save() {   // the map (what an older build reads), then the notes
  if (g_map_locked) {   // 1042: never over a map this build could not read
    core::log("um: %s not written: it did not parse (the slots change in memory only)", g_map_file);
    return false;
  }
  char txt[400];
  uint32_t n = rmp_slots_format(&g_map, txt, sizeof txt);
  bool ok = write_whole(g_map_file, txt, n);
  char nt[512];   // the notes only next to a written map (an "empty" note beside an old map would be stale)
  n = rmp_notes_format(&g_notes, nt, sizeof nt);
  if (ok && !g_notes_locked && !write_whole(g_notes_file, nt, n)) core::log("um: %s not written", g_notes_file);
  g_fresh = false;
  return ok;
}
void note_log(int s, const char* what) {
  const rmp_slot_note& e = g_notes.slot[s];
  core::log("um: U%d %s: %s (%s, sounds' fingerprint %u)", s + 1, g_map.slot[s].id[0] ? g_map.slot[s].id : "-", what,
            rmp_src_name(e.src), (unsigned)e.fp);
}
// The slots `ch` changed by a resolve: loaded again, the map and notes saved, each logged.
void resolved(uint32_t ch, const char* how) {
  for (int s = 0; s < kSlots; ++s) {
    if (!(ch >> s & 1)) continue;
    g_need |= 1u << s;
    note_log(s, g_notes.slot[s].flags & RMP_RF_UPDATED ? "updated from the project's sounds" : how);
  }
  if (ch) ++g_resolve_n;
  g_resolve_save = g_resolve_save || ch;
  g_pub = true;
  g_mp = true;
}
int pkg_refs(rmp_pkg_ref* r) {   // the loadable packages, as the resolver sees them
  int n = 0;
  for (int i = 0; i < g_npkg && n < kStore; ++i)
    if (g_pkg[i].ok && !g_pkg[i].older) r[n++] = rmp_pkg_ref{g_pkg[i].id, g_pkg[i].version, g_pkg[i].fp, g_pkg[i].desc.name};
  return n;
}
void inherit_now() {   // a fresh project: the previous project's slots where nothing else says
  if (!g_fresh) return;
  g_fresh = false;   // (nothing inherited: no file written, the project stays fresh for its next load)
  resolved(rmp_resolve_inherit(&g_map, &g_notes, &g_prev), "inherited from the previous project");
}
// A 0x25 for this project: resolved once the package list is there (the scan), a repeat ignored.
void prints_apply() {
  g_prints_pending = false;
  uint32_t h = rmp_prints_hash(&g_prints);
  if (g_prints_have && h == g_prints_hash) return;
  g_prints_have = true;
  g_prints_hash = h;
  rmp_pkg_ref r[kStore];
  int n = pkg_refs(r);
  resolved(rmp_resolve_prints(&g_map, &g_notes, &g_prints, r, n), "from the project's sounds");
  core::log("um: slot prints (seq %d, %d entr%s) for %s", (int)g_prints.seq, (int)g_prints.n, g_prints.n == 1 ? "y" : "ies",
            core::project[0] ? core::project : "_default");
  inherit_now();
}
void project_reload() {
  if (g_resolve_save) {   // the old project's resolve, still to be saved: to its own files first
    g_resolve_save = false;
    if (!map_save()) core::log("um: %s not written", g_map_file);
  }
  memcpy(&g_prev, &g_map, sizeof g_prev);
  map_read();
  g_full = true;
  g_prints_have = false;
  g_prints_pending = false;
  memset(g_lazy_fp, 0, sizeof g_lazy_fp);
  // a fresh project waits for the Rytm's fingerprints (if it sends them) before it inherits
  // (always: after a boot this Rytm may not have sent one yet; an older Rytm only inherits 1.5 s later)
  g_fresh_until = core::now_ms() + kFreshWaitMs;
}

// ---- the scan of the card's packages
void list_build() {   // the Rytm's list and the unique names, from g_pkg
  g_ninst = 0;
  for (int i = 0; i < g_npkg; ++i) {
    Pkg& e = g_pkg[i];
    memset(e.name, 0, sizeof e.name);
    memcpy(e.name, e.desc.name, 10);
    e.renamed = false;
    if (!e.ok || e.older || (e.hflags & RMP_HF_EFFECT)) continue;   // (an older version, an effect: not in the Rytm's list)
    char base[11];
    memcpy(base, e.name, sizeof base);
    for (int k = 2;; ++k) {
      bool dup = false;
      for (int j = 0; j < g_ninst; ++j) if (!strncmp(g_pkg[g_inst[j]].name, e.name, 9)) dup = true;
      if (!dup) break;
      char sfx[5];
      int sl = sniprintf(sfx, sizeof sfx, "~%d", k);
      int bl = (int)strnlen(base, 9 - sl);
      memset(e.name, 0, sizeof e.name);
      memcpy(e.name, base, bl);
      memcpy(e.name + bl, sfx, sl);
      e.renamed = true;
    }
    g_inst[g_ninst++] = (uint8_t)i;
  }
}
void scan_finish() {
  // what the slots used: a slot whose package changed or went is loaded again (or becomes MISSING)
  uint32_t print = 2166136261u;
  for (int i = 0; i < g_nwork; ++i) {
    const Pkg& e = g_work[i];
    const uint8_t* b = (const uint8_t*)e.id;
    for (int k = 0; k < 32 && b[k]; ++k) print = (print ^ b[k]) * 16777619u;
    print = (print ^ e.crc ^ e.version ^ (e.ok ? 1u : 0u)) * 16777619u;
  }
  bool changed = print != g_list_print;
  g_list_print = print;
  // one id installed in several versions (bundles phase 4): all kept side by side, the newest is the id's default (the
  // Rytm's list, a fresh assignment); the same version twice: the first copy only
  for (int i = 0; i < g_nwork; ++i) g_work[i].older = false;
  for (int i = 0; i < g_nwork; ++i)
    for (int j = 0; j < g_nwork; ++j) {
      if (i == j || !g_work[i].ok || !g_work[j].ok || strncmp(g_work[i].id, g_work[j].id, 32)) continue;
      if (g_work[j].version == g_work[i].version && j < i) {
        g_work[i].ok = false;
        copy(g_work[i].why, "duplicate id (the same version twice)", sizeof g_work[i].why);
      } else if (g_work[j].version > g_work[i].version) {
        g_work[i].older = true;
      }
    }
  memcpy(g_pkg, g_work, sizeof(Pkg) * (size_t)g_nwork);
  g_npkg = g_nwork;
  for (int s = 0; s < kAll; ++s) {   // a slot whose package changed or went: loaded again (UFX1..UFX4 too)
    if (!entry_of(s).id[0]) continue;
    int k = find_slot(s);
    uint32_t crc = k >= 0 ? g_pkg[k].crc ^ g_pkg[k].size : 0;
    if (crc != (g_slot[s].crc ^ 0) || g_slot[s].st != RMP_SLOT_READY) g_need |= 1u << s;
    if (s < kSlots && (k < 0 || lbl_key(g_pkg[k]) != g_lbl_key[s])) g_lbl_cur[s] = -1, g_lbl_ms[s] = core::now_ms() - kLblMs;   // (review 5: another package)
  }
  list_build();
  for (int s = 0; s < kAll; ++s) {   // the slots' package indices into the new list (a READY slot not reloaded keeps its image)
    int k = find_slot(s);
    if (k < 0 && g_slot[s].st == RMP_SLOT_READY) g_need |= 1u << s;
    g_slot[s].pkg = k;
  }
  if (changed) rmp_meta_store(&g_meta);
  if (changed) {   // other packages: a fingerprint nothing had may resolve now (only the slots still open), a trig's again
    memset(g_lazy_fp, 0, sizeof g_lazy_fp);
    uint32_t open = rmp_resolve_open_slots(&g_notes);
    if (g_prints_have && open) {
      rmp_pkg_ref r[kStore];
      int n = pkg_refs(r);
      resolved(rmp_resolve_prints(&g_map, &g_notes, &g_prints, r, n, open), "from the project's sounds (a new package)");
    }
  }
  g_pub = true;
  if (g_list_owed) { g_list_owed = false; send_installed(); }
  core::log("um: %d package%s in %s, %d for the Rytm's list (generation %d)%s", g_npkg, g_npkg == 1 ? "" : "s", g_dir,
            g_ninst, (int)g_meta.store_gen, g_over ? " (more not listed)" : "");
}
void scan_fail(Pkg& e, const char* why) {
  e.ok = false;
  copy(e.why, why, sizeof e.why);
}
// The whole file read: its CRC, the entry table's sizes, the manifest (rd: where those bytes are).
void scan_check(Pkg& e, rmp_read_fn rd, void* ctx) {
  bool ok = ~g_scan_crc == e.crc;
  if (!ok) scan_fail(e, "CRC wrong (damaged file)");
  uint8_t t[16];
  if (ok && rd(ctx, RMP_HEADER_BYTES + g_scan_info.entry_offset, t, 16)) {
    uint32_t magic, api;
    memcpy(&magic, t, 4); memcpy(&api, t + 4, 4); memcpy(&e.state, t + 8, 4); memcpy(&e.cpu, t + 12, 4);
    uint32_t top = e.target == RMP_TARGET_A64 ? RMP_API_RMPA : RMP_API;   // (API 3: RMPA packages only)
    if (magic != RMP_PLUGIN_MAGIC || api < RMP_API_MIN || api > top) { ok = false; scan_fail(e, "bad entry table"); }
  }
  if (ok && !rmp_manifest_read(&g_scan_info, rd, ctx, &e.desc)) { ok = false; scan_fail(e, "bad manifest"); }
  if (ok) rmp_manifest_labels(&g_scan_info, rd, ctx, &e.desc, &e.lbl);   // (none, or malformed: the static labels)
#if UM_NAMES_CACHE
  e.nnames = 0;
  if (ok && e.desc.names_at) {   // the name table, whole: the entries every knob's enum_first + enum_count reach
    uint32_t total = 0;
    for (int j = 0; j < e.desc.nknobs; ++j) {
      uint32_t t = (uint32_t)e.desc.knob[j].enum_first + e.desc.knob[j].enum_count;
      if (e.desc.knob[j].enum_count && t > total) total = t;
    }
    uint32_t room = e.desc.names_end > e.desc.names_at ? (e.desc.names_end - e.desc.names_at) / RMP_ENUM_NAME_BYTES : 0;
    if (total > room) total = room;   // a table cut short: what it holds (a list past it has no names, as read from the file)
    if (total && total <= (uint32_t)kNamesMax && rd(ctx, e.desc.names_at, e.names, total * RMP_ENUM_NAME_BYTES))
      e.nnames = (uint16_t)total;
  }
#endif
  if (ok && e.target != rmp_host_target) { ok = false; scan_fail(e, e.target == RMP_TARGET_A64 ? "for the Pi (AArch64)" : "for the Teensy (ARM)"); }
  if (ok && e.state > kStateMax) { ok = false; scan_fail(e, "state too big"); }
  if (ok && g_scan_info.data_start + RMP_A64_ALIGN > (ex ? ex->total : 0)) { ok = false; scan_fail(e, "code too big"); }
  e.ok = ok;
}
#if CORE_MSAMP
// 7z (2026-10-31): the scan's card work on the card core, as the load's and the presets' (measured on the Pi with the
// Rytm streaming: 25 ms stalls of the control core, three dropouts of 0.3-1 s). The folder's open, its entries 8 a kDirReq,
// the close; each package's open, reads of kScanRead, the close: a request a step, a request of the scan's own at a time.
// The CRC runs over what a read brought, up to 300 us a step. What the old scan read again at the end (the entry table, the
// manifest, the name table, the labels) is kept from those same reads, windows set from the header. A rescan during a
// pass abandons it: the request under way runs to its end, the file open is closed, the pass starts over; only a whole
// pass is ever published (before: the pass ran on and published what it had read, then the next one).
constexpr uint32_t kScanRead = 32768;      // one card operation of ~2 ms on the card core, as a stream's read
constexpr uint32_t kCrcUs = 300;
constexpr uint32_t kCapHead = 84u + 32u * 26u + (256u + RMP_ENUM_MAX) * RMP_ENUM_NAME_BYTES;   // head, knobs, name table
constexpr uint32_t kCapTail = 5u + RMP_LABELS_DEPS + RMP_LABELS_PARAMS + RMP_LABELS_SETS * RMP_ENUM_NAME_BYTES +
                              RMP_LABELS_SETS * RMP_LABELS_PARAMS * RMP_LABEL_BYTES + RMP_LABELS_FOOTER;   // the labels' block
cardq::Req g_srq;
cardq::DirRec g_sent[8];
core::File g_sd;                           // the folder
uint8_t g_sbuf[kScanRead];
uint32_t g_sb_n = 0, g_sb_at = 0;          // g_sbuf's bytes, the CRC's place in them
uint8_t g_cap[16 + kCapHead + kCapTail];
struct ScanWin { uint32_t at, len; uint8_t* d; };
ScanWin g_win[3];                          // the entry table, the manifest's start, its end (package offsets)
enum : uint8_t { kSsBegin, kSsDirOpen, kSsDirList, kSsDirClose, kSsNext, kSsOpen, kSsRead, kSsCrc, kSsEnd, kSsClose, kSsDrop };
uint8_t g_ss = kSsBegin;
// A burst of rescans (an upload's, a bundle's, one a file): the next pass starts once they have been quiet this long, and
// everything else in idle() goes on meanwhile
constexpr uint32_t kRescanQuietMs = 150;
uint32_t g_scan_at = 0;
bool scan_wait() { return g_scan && g_ss == kSsBegin && (int32_t)(core::now_ms() - g_scan_at) < 0; }
void win_set(const rmp_info& i) {
  uint32_t at = RMP_HEADER_BYTES + i.image_size + 8u * i.reloc_count, ms = i.manifest_size;
  uint32_t h = ms < kCapHead ? ms : kCapHead, t = ms < kCapTail ? ms : kCapTail;
  g_win[0] = ScanWin{RMP_HEADER_BYTES + i.entry_offset, 16, g_cap};
  g_win[1] = ScanWin{at, h, g_cap + 16};
  g_win[2] = ScanWin{at + ms - t, t, g_cap + 16 + kCapHead};
}
void win_take(uint32_t off, const uint8_t* b, uint32_t n) {   // what a read brought of the windows
  for (const ScanWin& w : g_win) {
    uint32_t lo = off > w.at ? off : w.at, hi = off + n < w.at + w.len ? off + n : w.at + w.len;
    if (lo < hi) memcpy(w.d + (lo - w.at), b + (lo - off), hi - lo);
  }
}
bool win_rd(void*, uint32_t off, uint8_t* b, uint32_t n) {
  for (const ScanWin& w : g_win)
    if (off >= w.at && (uint64_t)off - w.at + n <= w.len) {
      memcpy(b, w.d + (off - w.at), n);
      return true;
    }
  return false;
}
// One scan step: at most one request's result taken and the next posted, and kCrcUs of CRC.
void scan_step() {
  if (g_scan_again) {
    if (!cardq::idle(&g_srq)) return;   // (its File and buffer not touched before)
    g_srq.state = cardq::kFree;
    g_scan_again = false;
    g_scan = 1;
    g_ss = g_sd.isOpen() || g_sf.isOpen() ? kSsDrop : kSsBegin;
  }
  for (;;) {
    Pkg& e = g_work[g_nwork < kStore ? g_nwork : 0];
    if (g_ss == kSsBegin) {
      if (scan_wait()) return;
      g_nnames = g_over = g_nwork = g_scan_i = 0;
      if (core::card_ok) {
        g_ss = kSsDirOpen;
      } else {
        g_ss = kSsNext;
        g_scan = 2;
      }
    } else if (g_ss == kSsDirOpen) {
      if (!cardq::async(&g_srq, cardq::kOpenReq, &g_sd, g_dir, 0, nullptr, 0)) return;
      g_ss = g_srq.got == 1 && g_sd.isDir() ? kSsDirList : g_sd.isOpen() ? kSsDirClose : kSsNext;
      if (g_ss == kSsNext) g_scan = 2;
    } else if (g_ss == kSsDirList) {
      if (!cardq::async(&g_srq, cardq::kDirReq, &g_sd, nullptr, 0, (uint8_t*)g_sent, sizeof g_sent)) return;
      int got = g_srq.got;
      for (int k = 0; k < got; ++k) {
        char n[64];
        copy(n, g_sent[k].name, sizeof n);
        if (g_sent[k].dir || n[0] == '.' || !ends_rmp(n)) continue;
        if (g_nnames >= kStore) { ++g_over; continue; }
        sniprintf(g_names[g_nnames++], sizeof g_names[0], "%s/%s", g_dir, n);
      }
      if (got == (int)(sizeof g_sent / sizeof g_sent[0])) return;   // (fewer: the folder's end)
      g_ss = kSsDirClose;
    } else if (g_ss == kSsDirClose) {
      if (!cardq::async(&g_srq, cardq::kCloseReq, &g_sd, nullptr, 0, nullptr, 0)) return;
      g_ss = kSsNext;
      g_scan = 2;
    } else if (g_ss == kSsNext) {
      if (g_scan_i >= g_nnames) {
        g_ss = kSsBegin;
        g_scan = 0;
        scan_finish();
        return;
      }
      memset(&e, 0, sizeof e);
      copy(e.file, g_names[g_scan_i], sizeof e.file);
      const char* base = strrchr(e.file, '/');
      rmp_desc_from_id(base ? base + 1 : e.file, &e.desc);
      memset(g_win, 0, sizeof g_win);
      memset(g_cap, 0, sizeof g_cap);
      g_ss = kSsOpen;
    } else if (g_ss == kSsOpen) {
      if (!cardq::async(&g_srq, cardq::kOpenReq, &g_sf, e.file, 0, nullptr, 0)) return;
      if (g_srq.got != 1) {
        scan_fail(e, "unreadable");
        ++g_nwork, ++g_scan_i;
        g_ss = kSsNext;
        continue;
      }
      e.size = (uint32_t)g_sf.fileSize();
      g_scan_off = 0;
      if (e.size >= RMP_HEADER_BYTES) {
        g_ss = kSsRead;
      } else {
        scan_fail(e, "bad size");
        g_ss = kSsClose;
      }
    } else if (g_ss == kSsRead) {
      uint32_t n = e.size - g_scan_off < kScanRead ? e.size - g_scan_off : kScanRead;
      if (!cardq::async(&g_srq, cardq::kReadReq, &g_sf, nullptr, g_scan_off, g_sbuf, n)) return;
      g_sb_n = n, g_sb_at = 0, g_ss = kSsCrc;
      if (g_srq.got != (int32_t)n) {
        scan_fail(e, g_scan_off ? "read error" : "bad size");
        g_ss = kSsClose;
      } else if (g_scan_off == 0) {   // a new file: its header
        rmp_status st = rmp_verify_header_any(g_sbuf, e.size, &g_scan_info);
        if (st != RMP_OK) {
          scan_fail(e, st == RMP_ERR_MAGIC ? "not a machine package" : st == RMP_ERR_API ? "plugin API too new"
                       : st == RMP_ERR_FORMAT ? "format not supported" : "bad size");
          g_ss = kSsClose;
          continue;
        }
        copy(e.id, g_scan_info.id, sizeof e.id);
        e.version = g_scan_info.version, e.compat = g_scan_info.compatible_from;
        e.image = g_scan_info.image_size, e.bss = g_scan_info.bss_size;
        e.api = (uint8_t)g_scan_info.api, e.target = g_scan_info.target, e.crc = g_scan_info.crc32;
        e.hflags = g_scan_info.flags;
        e.fp = rmp_plugin_fp(e.id);
        rmp_desc_from_id(e.id, &e.desc);
        win_set(g_scan_info);
        g_sb_at = RMP_HEADER_BYTES;
        g_scan_crc = 0xFFFFFFFFu;
      }
      if (g_ss == kSsCrc) win_take((uint32_t)g_scan_off, g_sbuf, n);
      return;
    } else if (g_ss == kSsCrc) {   // 8 KB of the CRC at a time, for kCrcUs a step (one idle pass each would slow a boot's loads)
      uint32_t t0 = core::now_us();
      do {
        uint32_t n = g_sb_n - g_sb_at < kChunk ? g_sb_n - g_sb_at : kChunk;
        g_scan_crc = rmp_crc32_update(g_scan_crc, g_sbuf + g_sb_at, n);
        g_sb_at += n;
      } while (g_sb_at < g_sb_n && core::now_us() - t0 < kCrcUs);
      if (g_sb_at < g_sb_n) return;
      g_scan_off += g_sb_n;
      if (g_scan_off >= e.size) {
        g_ss = kSsEnd;
        return;
      }
      g_ss = kSsRead;
    } else if (g_ss == kSsEnd) {
      scan_check(e, win_rd, nullptr);   // (memory only)
      g_ss = kSsClose;
    } else if (g_ss == kSsClose) {
      if (!cardq::async(&g_srq, cardq::kCloseReq, &g_sf, nullptr, 0, nullptr, 0)) return;
      ++g_nwork, ++g_scan_i;
      g_ss = kSsNext;
    } else {   // kSsDrop: the pass abandoned
      if (!cardq::async(&g_srq, cardq::kCloseReq, g_sd.isOpen() ? &g_sd : &g_sf, nullptr, 0, nullptr, 0)) return;
      g_ss = kSsBegin;
    }
  }
}
#else
bool scan_wait() { return false; }
// One scan step: the listing, or one package's header, 8 KB of its CRC, or its manifest.
bool scan_step() {
  if (g_scan == 1) {
    g_nnames = g_over = 0;
    core::File d, f;
    if (core::card_ok && d.open(g_dir, core::kRead) && d.isDir()) {
      while (f.openNext(&d, core::kRead)) {
        char n[64];
        f.getName(n, sizeof n);
        bool isfile = !f.isDir();
        f.close();
        if (!isfile || n[0] == '.' || !ends_rmp(n)) continue;
        if (g_nnames >= kStore) { ++g_over; continue; }
        sniprintf(g_names[g_nnames++], sizeof g_names[0], "%s/%s", g_dir, n);
      }
      d.close();
    }
    g_nwork = 0;
    g_scan_i = 0;
    g_scan_off = 0;
    g_scan = 2;
    return true;
  }
  if (g_scan_i >= g_nnames) {
    if (g_sf.isOpen()) g_sf.close();
    g_scan = 0;
    scan_finish();
    if (g_scan_again) { g_scan_again = false; g_scan = 1; }
    return true;
  }
  Pkg& e = g_work[g_nwork];
  if (g_scan_off == 0) {   // a new file: its header
    memset(&e, 0, sizeof e);
    copy(e.file, g_names[g_scan_i], sizeof e.file);
    const char* base = strrchr(e.file, '/');
    rmp_desc_from_id(base ? base + 1 : e.file, &e.desc);
    if (!g_sf.open(e.file, core::kRead)) {
      scan_fail(e, "unreadable");
      ++g_nwork, ++g_scan_i;
      return true;
    }
    e.size = (uint32_t)g_sf.fileSize();
    uint8_t h[RMP_HEADER_BYTES];
    rmp_status st = e.size >= RMP_HEADER_BYTES && g_sf.read(h, sizeof h) == (int)sizeof h
                        ? rmp_verify_header_any(h, e.size, &g_scan_info) : RMP_ERR_SIZE;
    if (st != RMP_OK) {
      scan_fail(e, st == RMP_ERR_MAGIC ? "not a machine package" : st == RMP_ERR_API ? "plugin API too new"
                   : st == RMP_ERR_FORMAT ? "format not supported" : "bad size");
      g_sf.close();
      ++g_nwork, ++g_scan_i;
      return true;
    }
    copy(e.id, g_scan_info.id, sizeof e.id);
    e.version = g_scan_info.version, e.compat = g_scan_info.compatible_from;
    e.image = g_scan_info.image_size, e.bss = g_scan_info.bss_size;
    e.api = (uint8_t)g_scan_info.api, e.target = g_scan_info.target, e.crc = g_scan_info.crc32;
    e.hflags = g_scan_info.flags;
    e.fp = rmp_plugin_fp(e.id);
    rmp_desc_from_id(e.id, &e.desc);
    g_scan_off = RMP_HEADER_BYTES;
    g_scan_crc = 0xFFFFFFFFu;
    return true;
  }
  if (g_scan_off < e.size) {   // 8 KB of the CRC
    uint8_t b[kChunk];
    uint32_t n = e.size - g_scan_off < kChunk ? e.size - g_scan_off : kChunk;
    if (!g_sf.seekSet(g_scan_off) || g_sf.read(b, n) != (int)n) {
      scan_fail(e, "read error");
      g_sf.close();
      g_scan_off = 0;
      ++g_nwork, ++g_scan_i;
      return true;
    }
    g_scan_crc = rmp_crc32_update(g_scan_crc, b, n);
    g_scan_off += n;
    return true;
  }
  scan_check(e, file_rd, &g_sf);
  g_sf.close();
  g_scan_off = 0;
  ++g_nwork, ++g_scan_i;
  return true;
}
#endif

// ---- the writable parts of the images (data + bss): a bump arena of 4096-aligned blocks, reset with the code's
constexpr uint32_t kDataArena = 4u << 20;
uint8_t* g_data = nullptr;
uint32_t g_data_used = 0;
uint8_t* data_alloc(uint32_t bytes) {
  if (ex->data_alloc) {
    uint8_t* r = ex->data_alloc(bytes);
    if (r) g_data_used += bytes;
    return r;
  }
  if (!g_data) {
    uint8_t* raw = (uint8_t*)um_heap(kDataArena + RMP_A64_ALIGN);
    if (!raw) return nullptr;
    g_data = (uint8_t*)(((uintptr_t)raw + RMP_A64_ALIGN - 1) & ~(uintptr_t)(RMP_A64_ALIGN - 1));
  }
  if (bytes > kDataArena - g_data_used) return nullptr;
  uint8_t* r = g_data + g_data_used;
  g_data_used += bytes;
  return r;
}

// ---- a user effect's labels on the Rytm's SEND FX page (0x24, core/fx/knob_labels.h), from its manifest: knob p 1..4
// (the send slot's P2..P5) gives its short name; a knob with names (look "list") gives zones, name k from value
// ceil(128 k / n), as the plugin reads value * n >> 7. Adjacent equal names are one zone. A name ending in '#' shows
// the knob's number inside its zone ("6ST 21", as RINGS' "MODAL 12"); a knob whose zones all lack it steps one zone per
// detent on the Rytm. At most kMaxZones zones in all (a knob that would pass it keeps none). Built on the control
// core when the slot loads, published with g_fxl_on (the readers: the host link, the UI's labels on core 0). Two copies per
// effect: a load builds the idle one and then publishes it, so a reader that took the other a moment ago still reads
// whole strings (it would have to hold it across two loads to see one change). Without UM_NAMES_CACHE the knobs get
// their short names but no zones.
struct FxLabels {
  core::fx::KnobLabels kl;
  core::fx::Zone z[core::fx::kMaxZones];
  char lab[4][core::fx::kLabelChars + 1];
  char zn[core::fx::kMaxZones][core::fx::kZoneChars + 1];
};
FxLabels g_fxl[kUfx][2];
volatile uint8_t g_fxl_on;   // bit u: the loaded effect u has labels; bit 4 + u: they are in g_fxl[u][1]
const core::fx::KnobLabels* fx_labels(int u) {
  if (u < 0 || u >= kUfx) return nullptr;
  uint8_t on = __atomic_load_n(&g_fxl_on, __ATOMIC_ACQUIRE);
  return on >> u & 1 ? &g_fxl[u][on >> (4 + u) & 1].kl : nullptr;
}
// knob q's zones of package e into L from zone *nz on; false (nothing added) if they would not fit, or it has none
bool fx_zones(const Pkg& e, int q, int knob, FxLabels& L, int* nz) {
  char names[RMP_ENUM_MAX][8];
  int n = -1;
#if UM_NAMES_CACHE
  if (e.nnames) {
    NamesRd r{&e};
    n = rmp_manifest_enum(&e.desc, q, names_rd, &r, names);
  }
#else
  (void)e, (void)q;
#endif
  if (n < 2) return false;
  int z = *nz, first = z;
  bool any_show = false;
  for (int k = 0; k < n; ++k) {
    char nm[8];
    memcpy(nm, names[k], 8);
    nm[7] = 0;
    size_t len = strlen(nm);
    bool show = len > 0 && nm[len - 1] == '#';
    if (show) nm[--len] = 0;
    if (len > (size_t)core::fx::kZoneChars) nm[core::fx::kZoneChars] = 0;
    if (z > first && L.z[z - 1].show == show && strcmp(L.zn[z - 1], nm) == 0) continue;   // one zone
    if (z >= core::fx::kMaxZones) return false;
    strcpy(L.zn[z], nm);
    L.z[z] = core::fx::Zone{(uint8_t)knob, (uint8_t)((k * 128 + n - 1) / n), 0, (uint8_t)show, 0, L.zn[z]};
    any_show |= show;
    ++z;
  }
  if (z - first < 2) return false;
  for (int j = first; j < z; ++j) L.z[j].step = !any_show;
  *nz = z;
  return true;
}
void fx_labels_build(int u, int pkg) {
  if (pkg < 0) {
    __atomic_fetch_and(&g_fxl_on, (uint8_t)~(1u << u), __ATOMIC_ACQ_REL);
    return;
  }
  const Pkg& e = g_pkg[pkg];
  int b = !(__atomic_load_n(&g_fxl_on, __ATOMIC_ACQUIRE) >> (4 + u) & 1);   // the copy no reader was given
  FxLabels& L = g_fxl[u][b];
  memset(&L, 0, sizeof L);
  int nz = 0;
  for (int j = 0; j < 4; ++j) {
    int q = rmp_desc_find(&e.desc, j + 1);
    if (q < 0) continue;
    memcpy(L.lab[j], e.desc.knob[q].short_name, core::fx::kLabelChars);
    if (e.desc.knob[q].look == RMP_LOOK_LIST) fx_zones(e, q, j, L, &nz);
  }
  for (int j = 0; j < 4; ++j) L.kl.label[j] = L.lab[j];
  L.kl.zones = L.z;
  L.kl.nzones = (uint8_t)nz;
  uint8_t on = __atomic_load_n(&g_fxl_on, __ATOMIC_ACQUIRE);   // (only this core writes it)
  on = (uint8_t)((on & ~(1u << (4 + u))) | 1u << u | b << (4 + u));
  __atomic_store_n(&g_fxl_on, on, __ATOMIC_RELEASE);
}

// ---- loading a slot
void slot_set(int s, rmp_slot_state st, const rmp_plugin* p, int pkg) {
  Slot& sl = g_slot[s];
  sl.st = st, sl.p = p, sl.pkg = pkg;
  g_why[s][0] = 0;
  sl.state_bytes = pkg >= 0 ? g_pkg[pkg].state : 0;
  sl.crc = pkg >= 0 && st == RMP_SLOT_READY ? g_pkg[pkg].crc ^ g_pkg[pkg].size : 0;
  sl.fp = st == RMP_SLOT_READY && pkg >= 0 ? g_pkg[pkg].fp : 0;
  g_pub = true;
  if (s >= kSlots) {   // (an effect slot: the Rytm's TYPE list, labels and states again)
    fx_labels_build(s - kSlots, st == RMP_SLOT_READY ? pkg : -1);
    core::fx::list_changed();
    return;
  }   // (an effect: no cost row, no presets, nothing the Rytm asks for)
  cost_sum(s, &g_cyc0[s], &g_blk0[s]);   // the cost is measured from here
  g_mp = true;
  pre_slot(s);
}
void job_end(int s, rmp_slot_state st, const rmp_plugin* p, int pkg, const char* what) {
  jf_close();
  slot_set(s, st, p, pkg);
  if (st != RMP_SLOT_READY) sniprintf(g_why[s], sizeof g_why[s], "%s: %s", st == RMP_SLOT_NO_ROOM ? "no room" : "bad", what);
  g_job = -1;
  g_jph = kJphImage;
  core::log("um: %s%d %s: %s", s < kSlots ? "U" : "UFX", s < kSlots ? s + 1 : s - kSlots + 1, entry_of(s).id, what);
}

// A machine slot plays machines, an effect slot effects (with the send position, the one built yet): else why not.
const char* kind_wrong(int s, const rmp3_plugin* p3) {
  if (s < kSlots) return p3 && p3->kind != RMP3_KIND_MACHINE ? "an effect (UFX1..UFX4), not a machine" : nullptr;
  if (!p3 || p3->kind != RMP3_KIND_EFFECT) return "a machine (U1..U5), not an effect";
  if (!(p3->places & RMP3_AT_SEND)) return "its places lack a send slot (the one position built yet)";
  return nullptr;
}
// ---- API 3's user effects: the states, the stop, core 2's render
// An effect's two states (send FX 1 and 2), set up by the control core before core 2 may run them: zeroed, voice_init,
// voice_attach (place SEND, voice -1, instance 0 / 1, arena parts 0 / 1). False: a fault (BAD, the job ended).
bool fx_states(int s) {
  int u = s - kSlots;
  const V3& x = g_v3[s];
  const rmp_plugin* p = x.p;
  for (int k = 0; k < 2; ++k) {
    void* st = g_fxst[u][k];
    memset(st, 0, g_slot[s].state_bytes);
    for (int j = 0; j < 16; ++j) g_fx_p[u][k][j] = -1.0f;   // (the first block: every knob changed)
    for (int q = 0; q < 3; ++q) g_fx_ev[u][k][q] = g_xp_evn[q];   // (no old edges)
    uint64_t pc = 0;
    uint32_t us = 0;
    rmp3_voice v;
    v.shared = x.shared, v.shared_bytes = x.shared_bytes;
    v.arena = x.voice_bytes && (uint32_t)k < x.states ? x.voices + (size_t)k * x.voice_bytes : nullptr;
    v.arena_bytes = v.arena ? x.voice_bytes : 0;
    v.slot = 64 + u, v.voice = -1, v.instance = k, v.place = RMP3_AT_SEND;
    if (guarded([&] {
          if (p->voice_init) p->voice_init(st);
          if (x.p3->voice_attach) x.p3->voice_attach(st, &v);
        }, &pc, &us)) {
      char w[64], at[32];
      fault_where(s, pc, at, sizeof at);
      sniprintf(w, sizeof w, "crashed in voice_init / voice_attach at %s", at);
      job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, w);
      return false;
    }
  }
  return true;
}
// UFX u READY: the TYPE list's name and the kits' tag (send_fx.h), the name first, the tag released last
void fx_named(int u) {
  int k = g_slot[kSlots + u].pkg;
  core::fx::UserFx& f = core::fx::user[u];
  __atomic_store_n(&f.tag, (uint16_t)0, __ATOMIC_RELEASE);
  copy(f.name, k >= 0 ? g_pkg[k].desc.name : "", sizeof f.name);
  __atomic_store_n(&f.tag, core::fx::fx_tag(f.name), __ATOMIC_RELEASE);
  core::fx::list_changed();
}
// UFX u leaves core 2 before its states, arena or image change: not live, then core 2 out of it (a render under way
// ends; at most one effect block)
void fx_stop(int u) {
  __atomic_store_n(&g_fx_live[u], 0, __ATOMIC_SEQ_CST);
  __atomic_add_fetch(&g_fx_era[u], 1, __ATOMIC_SEQ_CST);   // (a render still under way mutes nothing of what comes next)
  uint32_t t0 = core::now_us();
  while (__atomic_load_n(&g_fx_busy[u], __ATOMIC_SEQ_CST)) {
    if (g_fx_dead[u] || core::now_us() - t0 > kFxStopWaitUs) {   // stuck in its render: give up (once), keep its memory
      if (!g_fx_dead[u]) core::log("um: UFX%d %s: core 2 never left its render (%u ms): left behind", u + 1, g_fxmap[u].id,
                                   (unsigned)(kFxStopWaitUs / 1000));
      g_fx_dead[u] = true;
      break;
    }
  }
  if (!__atomic_load_n(&g_fx_busy[u], __ATOMIC_SEQ_CST)) g_fx_dead[u] = false;   // (it came back: its memory is free again)
  g_fx_fault[u] = 0, g_fx_slow_us[u] = 0, g_fx_over[u][0] = g_fx_over[u][1] = 0, g_fx_total[u] = 0;
}
bool fx_any_dead() {
  for (int u = 0; u < kUfx; ++u)
    if (g_fx_dead[u] && __atomic_load_n(&g_fx_busy[u], __ATOMIC_SEQ_CST)) return true;
  return false;
}
template <class F> struct FxCall {
  F* f;
  static void run(void* c) { (*((FxCall*)c)->f)(); }
};
// Core 2 (the send FX's core, after its block): UFX u in send slot sl (0 FX 1, 1 FX 2) over x[n], its wet stereo
// return added into y. The knobs: k[2..5] (the slot's P2..P5) are the effect's P1..P4, in b->p. Silent unless live.
bool fx_ready(int u) { return u >= 0 && u < kUfx && __atomic_load_n(&g_fx_live[u], __ATOMIC_ACQUIRE) && !muted(kSlots + u); }
// Core 2, after each effect call: the block's sum of every effect's time (N4). A block whose effects together ran over
// kFxTotal (scaled by its frames) counts; kFxOverMax such blocks in a row mute the dearest effect of the last one.
void fx_account(uint32_t gen, int u, uint32_t us, int n) {
  FxAcct& a = g_fxa;
  if (gen != a.gen) {   // a new block: the last one's verdict
    if (a.frames) {
      uint32_t share = g_fx_total_us * (a.frames > 12 ? a.frames : 12) / 12u;
      if (a.us > share) {
        if (++a.over >= kFxOverMax) {
          int w = 0;
          for (int k = 1; k < kUfx; ++k)
            if (a.us_u[k] > a.us_u[w]) w = k;
          g_fx_total[w] = 1;
          g_fx_slow_us[w] = a.us_u[w] ? a.us_u[w] : 1;
          __atomic_or_fetch(&g_muted, 1u << (kSlots + w), __ATOMIC_RELEASE);
          a.over = 0;
        }
      } else {
        a.over = 0;
      }
    }
    a.gen = gen, a.us = 0, a.frames = 0;
    for (uint32_t& x : a.us_u) x = 0;
  }
  a.us += us, a.us_u[u] += us;
  if ((uint32_t)n > a.frames) a.frames = (uint32_t)n;
}
void fx_run(int u, int sl, const float* xin, int n, float (*y)[2], const uint8_t* k) {
  if (u < 0 || u >= kUfx || sl < 0 || sl > 1) return;
  __atomic_store_n(&g_fx_busy[u], 1, __ATOMIC_SEQ_CST);
  if (__atomic_load_n(&g_fx_live[u], __ATOMIC_SEQ_CST) && !muted(kSlots + u)) {
    int s = kSlots + u;
    const uint32_t era = __atomic_load_n(&g_fx_era[u], __ATOMIC_SEQ_CST);
    // what this call learns about the effect counts only while it is still the same load (N2: a late return after
    // fx_stop gave up mutes nothing of the next one)
    auto same = [&] { return __atomic_load_n(&g_fx_live[u], __ATOMIC_SEQ_CST) && __atomic_load_n(&g_fx_era[u], __ATOMIC_SEQ_CST) == era; };
    const rmp3_plugin* p3 = g_v3[s].p3;
    void* st = g_fxst[u][sl];
    uint32_t e = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
    static uint32_t s_epoch = 0;   // (core 2 only)
    if (s_epoch != e) {
      if (ex->barrier) ex->barrier();
      s_epoch = e;
    }
    float p[16];
    uint32_t ch = 0;
    for (int j = 0; j < 16; ++j) {
      p[j] = j < 4 ? (float)k[2 + j] : 0.0f;
      if (p[j] != g_fx_p[u][sl][j]) ch |= 1u << j, g_fx_p[u][sl][j] = p[j];
    }
    // the transport: one snapshot for the whole call; its edges (START, STOP, JUMP) once for this state, in the first
    // sub-block - core 2's send FX runs out of step with the blocks, so it may see a snapshot twice or skip one
    uint32_t g = __atomic_load_n(&g_xp_gen, __ATOMIC_ACQUIRE);
    rmp3_transport tr = g_xp[g & 1], tr0;
    tr.flags &= ~(uint32_t)(RMP3_T_START | RMP3_T_STOP | RMP3_T_JUMP);
    tr0 = tr;
    static const uint32_t kEdge[3] = {RMP3_T_START, RMP3_T_STOP, RMP3_T_JUMP};
    // a state not called for more than two blocks (its send slot played something else): older edges are not news
    if (g - g_fx_lastgen[u][sl] > 2u)
      for (int q = 0; q < 3; ++q) g_fx_ev[u][sl][q] = g_xp_ev[g & 1][q] - ((g_xp[g & 1].flags & kEdge[q]) ? 1u : 0u);
    g_fx_lastgen[u][sl] = g;
    for (int q = 0; q < 3; ++q)
      if (g_xp_ev[g & 1][q] != g_fx_ev[u][sl][q]) tr0.flags |= kEdge[q], g_fx_ev[u][sl][q] = g_xp_ev[g & 1][q];
    uint32_t t0 = core::now_us();
    for (int at = 0; at < n; at += 16) {   // blocks of 16 frames at most
      int m = n - at < 16 ? n - at : 16;
      float l[16], r[16];
      rmp3_block b;
      block_fill(&b, m, l, r, RMP3_AT_SEND);
      b.flags |= RMP3_B_WET_ONLY;
      b.gen = g;
      b.transport = at ? &tr : &tr0;
      b.in[0] = xin + at;
      b.p = p, b.p_changed = at ? 0u : ch;
      uint64_t pc = 0;
      auto f = [&] { p3->render(st, &b); };
      FxCall<decltype(f)> c{&f};
      int bad = ex->guard ? ex->guard(FxCall<decltype(f)>::run, &c, &pc) : (f(), 0);
      if (bad) {   // muted; the control core says why (idle)
        if (same()) {
          g_fx_fault[u] = pc ? pc : 1;
          __atomic_or_fetch(&g_muted, 1u << s, __ATOMIC_RELEASE);
        }
        break;
      }
      for (int i = 0; i < m; ++i) {
        float a = l[i] == l[i] ? (l[i] > 1.0f ? 1.0f : l[i] < -1.0f ? -1.0f : l[i]) : 0.0f;
        float c2 = r[i] == r[i] ? (r[i] > 1.0f ? 1.0f : r[i] < -1.0f ? -1.0f : r[i]) : 0.0f;
        y[at + i][0] += a, y[at + i][1] += c2;
      }
    }
    // the budget (as the voices' guard): kFxOverMax calls of this state over it in a row, or one call past the FIFO,
    // mute the effect
    uint32_t us = core::now_us() - t0, budget = g_fx_budget_us * (uint32_t)(n > 12 ? n : 12) / 12u;
    if (same()) {
      if (us > budget) {
        if (++g_fx_over[u][sl] >= kFxOverMax || us >= kFxHugeUs) {
          g_fx_slow_us[u] = us ? us : 1;
          __atomic_or_fetch(&g_muted, 1u << s, __ATOMIC_RELEASE);
        }
      } else {
        g_fx_over[u][sl] = 0;
      }
      fx_account(g, u, us, n);
    }
  }
  __atomic_store_n(&g_fx_busy[u], 0, __ATOMIC_SEQ_CST);
}
// the control core: a fault core 2 noted, in words (a fault mutes the slots sharing its image too)
void fx_faults() {
  for (int u = 0; u < kUfx; ++u) {
    uint32_t slow = g_fx_slow_us[u];
    if (slow && strncmp(g_why[kSlots + u], "muted", 5)) {
      char w[96];
      if (g_fx_total[u])
        sniprintf(w, sizeof w, "muted: ran %u us, dearest over core 2's share (%u us)",
                  (unsigned)slow, (unsigned)g_fx_total_us);
      else
        sniprintf(w, sizeof w, "muted: render ran %u us, over its budget (%u us a block)", (unsigned)slow, (unsigned)g_fx_budget_us);
      slot_mute(kSlots + u, w, false);
      continue;
    }
    uint64_t pc = g_fx_fault[u];
    if (!pc || !strncmp(g_why[kSlots + u], "muted", 5)) continue;
    char w[96], at[32];
    fault_where(kSlots + u, pc == 1 ? 0 : pc, at, sizeof at);
    sniprintf(w, sizeof w, "muted: crashed in render at %s", at);
    slot_mute(kSlots + u, w);
  }
  // voices whose render faulted on a render core (the board's guard: render_fault): their slot, and its sharers
  uint32_t rf = __atomic_exchange_n(&g_render_faults, 0u, __ATOMIC_ACQ_REL);
  for (int vi = 0; vi < core::kVoices; ++vi) {
    if (!(rf >> vi & 1)) continue;
    int s = g_cur[vi];
    if (s < 0 || muted(s)) continue;
    slot_mute(s, "muted: crashed in render (voice's guard)");
  }
}

// ---- API 3: the table checked, the arena, setup()
uint32_t host_caps() {
  return RMP3_CAP_TRANSPORT | RMP3_CAP_STEREO | RMP3_CAP_EFFECT | (ex && ex->guard ? (uint32_t)RMP3_CAP_GUARD : 0u) |
         (g_pool_bytes ? (uint32_t)RMP3_CAP_ARENA : 0u);
}
void caps_names(uint32_t c, char* b, int cap) {
  static const char* const kN[] = {"transport", "arena", "guard", "stereo", "audio in", "effects", "signal bus", "block params", "pages"};
  b[0] = 0;
  int n = 0;
  for (int k = 0; k < 9; ++k)
    if (c >> k & 1) n += sniprintf(b + n, cap - n, "%s%s", n ? ", " : "", kN[k]);
}
// The plugin's API 3 table (reserved[RMP_P_V3]) if it has one: inside its code part, its magic and size, every function
// pointer of both tables inside the code, the counts in range, its needs met. False: refused (why).
bool v3_check(const rmp_plugin* p, const uint8_t* code, uint32_t bytes, const rmp3_plugin** out, char* why, int cap) {
  *out = nullptr;
  uintptr_t c0 = (uintptr_t)code, c1 = c0 + bytes;
  auto inside = [&](const void* q, uint64_t n) { uintptr_t a = (uintptr_t)q; return a >= c0 && n <= c1 - c0 && a <= c1 - n; };
  auto fn_ok = [&](const void* f) { return !f || (inside(f, 4) && !((uintptr_t)f & 3)); };
  const void* r = p->api >= 2u ? p->reserved[RMP_P_V3] : nullptr;
  if (!r) {
    if (p->api >= RMP_API3) { copy(why, "an API 3 table without its rmp3_plugin", cap); return false; }
    return true;   // a plain API 1 / 2 plugin: as before
  }
  if (((uintptr_t)r & 7) || !inside(r, RMP3_PLUGIN_BYTES_A64)) { copy(why, "its rmp3_plugin is outside its code", cap); return false; }
  const rmp3_plugin* q = (const rmp3_plugin*)r;
  if (q->magic != RMP3_MAGIC || q->size < RMP3_PLUGIN_BYTES_A64 || !inside(q, q->size)) { copy(why, "bad rmp3_plugin (magic or size)", cap); return false; }
  if (q->kind != RMP3_KIND_MACHINE && q->kind != RMP3_KIND_EFFECT) { copy(why, "bad rmp3_plugin kind", cap); return false; }
  const void* fns[] = {(const void*)p->init, (const void*)p->voice_init, (const void*)p->note_on, (const void*)p->note_off,
                       (const void*)p->params, (const void*)p->render, p->reserved[RMP_P_LOAD_ENTRY], p->reserved[RMP_P_VOICE_ENTER],
                       p->reserved[RMP_P_VOICE_LEAVE], (const void*)q->setup, (const void*)q->voice_attach,
                       (const void*)q->render, (const void*)q->view};
  for (const void* f : fns)
    if (!fn_ok(f)) { copy(why, "a function outside its code", cap); return false; }
  if (!q->render) { copy(why, "its rmp3_plugin has no render", cap); return false; }
  if (q->sig_out_n > 4 || q->sig_in_n > 4 || (q->sig_out_n && !inside(q->sig_out, 12u * q->sig_out_n)) ||
      (q->sig_in_n && !inside(q->sig_in, 12u * q->sig_in_n))) { copy(why, "bad signal declarations", cap); return false; }
  if (q->out_channels < 1 || q->out_channels > 2 || q->in_channels > 2 || q->aux_channels > 2 || q->states > kArenaStates ||
      q->latency_frames > 4096) { copy(why, "channels, states or latency out of range", cap); return false; }
  if (q->kind == RMP3_KIND_EFFECT && p->api < RMP_API3) { copy(why, "an effect must be API 3 only (api 3)", cap); return false; }
  uint32_t miss = q->needs & ~host_caps();
  if (miss) {
    char n[48];
    caps_names(miss, n, sizeof n);
    sniprintf(why, cap, "needs %s", n);
    return false;
  }
  *out = q;
  return true;
}
uint32_t arena_limit() { return g_pool_bytes / 4; }   // (the proposal's table: 16 / 32 / 64 MB a slot of 64 / 128 / 256)
// a block of `total` bytes: the smallest returned block it fits, else from the pool's top; null none
uint8_t* pool_take(uint64_t total, uint32_t* cap) {
  int best = -1;
  for (int k = 0; k < g_nfree; ++k)
    if (g_free[k].cap >= total && (best < 0 || g_free[k].cap < g_free[best].cap)) best = k;
  if (best >= 0) {
    uint8_t* b = g_free[best].base;
    *cap = g_free[best].cap;
    g_free[best] = g_free[--g_nfree];
    return b;
  }
  if (total > (uint64_t)(g_pool_bytes - g_pool_used)) return nullptr;
  uint8_t* b = g_pool + g_pool_used;
  g_pool_used += (uint32_t)total;
  *cap = (uint32_t)total;
  return b;
}
// slot s's block back (its load starts again, or the slot empties): to the pool's top when it is the last, else listed
void arena_release(int s) {
  V3& x = g_v3[s];
  if (!x.base || !x.cap) return;
  if (s >= kSlots && g_fx_dead[s - kSlots]) return;   // (core 2 may still be in it)
  uint8_t* b = x.base;
  uint32_t c = x.cap;
  x.base = nullptr, x.cap = 0;
  if (b + c == g_pool + g_pool_used) {
    g_pool_used -= c;
    for (bool again = true; again;) {   // free blocks now at the top go back too
      again = false;
      for (int k = 0; k < g_nfree; ++k)
        if (g_free[k].base + g_free[k].cap == g_pool + g_pool_used) {
          g_pool_used -= g_free[k].cap;
          g_free[k] = g_free[--g_nfree];
          again = true;
          break;
        }
    }
    return;
  }
  if (g_nfree < kFree) g_free[g_nfree++] = FreeBlk{b, c};   // (a full list: lost until the next full reload)
}
uint64_t pool_free() {   // what the pool has left, in pieces or not
  uint64_t f = g_pool_bytes - g_pool_used;
  for (int k = 0; k < g_nfree; ++k) f += g_free[k].cap;
  return f;
}
// The slot's arena from the pool: shared part + states x voice part, each the plugin's want, else its min (64-byte
// rounded), at most the per-slot limit; an effect's two host states at the end. True: reserved (perhaps nothing), the
// arena phase next; false: no room (need: the least it takes).
bool arena_reserve(int s, uint32_t* need) {
  V3& x = g_v3[s];
  const rmp3_plugin* q = x.p3;
  int k = g_slot[s].pkg;
  bool poly = k >= 0 && (g_pkg[k].hflags & RMP_HF_INSTANCES);
  // states: the plugin's, else 2 for an effect (send FX 1, 2), 40 for a poly machine (its voices and instances), else 8
  uint32_t states = q->states ? q->states : q->kind == RMP3_KIND_EFFECT ? 2u : poly ? (uint32_t)kArenaStates : 8u;
  auto al = [](uint64_t n) { return (n + 63u) & ~(uint64_t)63u; };
  bool has = (host_caps() & RMP3_CAP_ARENA) && (q->wants | q->needs) & RMP3_CAP_ARENA;
  uint64_t sw = al(q->shared_want > q->shared_min ? q->shared_want : q->shared_min), vw = al(q->voice_want > q->voice_min ? q->voice_want : q->voice_min);
  uint64_t sm = al(q->shared_min), vm = al(q->voice_min);
  if (!has) sw = vw = sm = vm = 0;
  if (s >= kSlots && !g_pool_bytes) { *need = 1; return false; }   // (an effect's states live in the pool)
  uint64_t fxs = s >= kSlots ? al(g_slot[s].state_bytes) : 0;   // an effect's two states (send FX 1, 2) at the end
  uint64_t lim = arena_limit();
  uint64_t want = sw + states * vw + 2 * fxs, least = sm + states * vm + 2 * fxs;
  uint64_t sb = 0, vb = 0, total = 0;
  uint8_t* base = nullptr;
  uint32_t cap = 0;
  auto pg = [](uint64_t n) { return (n + 4095u) & ~(uint64_t)4095u; };
  if (!want) base = nullptr, cap = 0;   // (no arena: nothing taken, the free list untouched)
  else if (want <= lim && (base = pool_take(pg(want), &cap))) sb = sw, vb = vw, total = pg(want);
  else if (least <= lim && (base = pool_take(pg(least), &cap))) sb = sm, vb = vm, total = pg(least);
  else if (least) { *need = (uint32_t)(least > 0xFFFFFFFFu ? 0xFFFFFFFFu : least); return false; }
  if (!total) base = nullptr, cap = 0;
  if (s >= kSlots) {
    g_fxst[s - kSlots][0] = base + sb + states * vb;
    g_fxst[s - kSlots][1] = g_fxst[s - kSlots][0] + fxs;
  }
  x.states = states;
  x.base = base, x.cap = cap;
  x.shared = sb ? x.base : nullptr;
  x.voices = vb ? x.base + sb : nullptr;
  x.shared_bytes = (uint32_t)sb, x.voice_bytes = (uint32_t)vb;
  x.zero_off = 0, x.zero_len = (uint32_t)total;
  return true;
}
// After init (or sharing another slot's image): the arena, then the arena phase. False: the job ended (no room).
bool v3_start(int s) {
  uint32_t need = 0;
  if (!arena_reserve(s, &need)) {
    // the free space is there, but in pieces: start over (a full reload packs the arenas), else NO ROOM
    if (need <= arena_limit() && g_nfree && pool_free() >= need) {
      core::log("um: %s%d %s: the arena pool is in pieces (%u KB free in all): every slot loads again", slot_pfx(s),
                slot_no(s), entry_of(s).id, (unsigned)(pool_free() >> 10));
      g_full = true;
      g_job = -1;
      g_jph = kJphImage;
      slot_set(s, RMP_SLOT_LOADING, nullptr, g_slot[s].pkg);
      return false;
    }
    char w[64];
    sniprintf(w, sizeof w, "the arena: needs %u KB, %u KB free, %u KB a slot", (unsigned)(need >> 10),
              (unsigned)(pool_free() >> 10), (unsigned)(arena_limit() >> 10));
    job_end(s, RMP_SLOT_NO_ROOM, nullptr, g_slot[s].pkg, w);
    return false;
  }
  g_job = s;
  g_jph = kJphArena;
  return true;
}
// One step of the arena phase: kZeroStep zeroed, or (all zeroed) setup() and READY.
void arena_step() {
  int s = g_job;
  V3& x = g_v3[s];
  if (x.zero_off < x.zero_len) {
    uint32_t n = x.zero_len - x.zero_off < kZeroStep ? x.zero_len - x.zero_off : kZeroStep;
    memset(x.base + x.zero_off, 0, n);
    x.zero_off += n;
    return;
  }
  const rmp_plugin* p = x.p;
  const rmp3_plugin* q = x.p3;
  if (q->setup) {
    rmp3_setup su;
    su.host = host_for(p), su.host3 = &g_h3;
    su.shared = x.shared, su.shared_bytes = x.shared_bytes, su.voice_bytes = x.voice_bytes, su.states = x.states;
    su.slot = s < kSlots ? s + 1 : 64 + (s - kSlots);
    int32_t r = 0;
    uint64_t pc = 0;
    uint32_t us = 0;
    char w[64], at[32];
    if (guarded([&] { r = q->setup(&su); }, &pc, &us)) {
      fault_where(s, pc, at, sizeof at);
      sniprintf(w, sizeof w, "crashed in setup at %s", at);
      job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, w);
      return;
    }
    // over the limit: refused; one call up to twice the limit is forgiven (an interrupt or a card wait in the wall time)
    if (us > 2 * kSetupLateUs || (us > kSetupLateUs && ++x.setup_over >= 2)) {
      sniprintf(w, sizeof w, "setup ran %u us (limit %u a call: return RMP3_SETUP_MORE)", (unsigned)us, (unsigned)kSetupLateUs);
      job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, w);
      return;
    }
    if (r == RMP3_SETUP_MORE) {   // more work: called again from the next idle step (the engine plays between)
      if (++x.setup_calls > kSetupCallsMax) { job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, "setup never finished"); return; }
      return;
    }
    if (r != 0) { job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, "its setup() refused"); return; }
  }
  if (s >= kSlots && !fx_states(s)) return;   // (BAD: job_end ran)
  char w[64];
  sniprintf(w, sizeof w, "ready (API 3, arena %u KB: %u states)", (unsigned)(x.zero_len >> 10), (unsigned)x.states);
  job_end(s, RMP_SLOT_READY, p, g_slot[s].pkg, w);
  if (s >= kSlots) {
    fx_named(s - kSlots);
    __atomic_store_n(&g_fx_live[s - kSlots], 1, __ATOMIC_SEQ_CST);   // core 2 may run it from now
  }
}
// Starts slot s's load, or settles it at once (empty, missing, shared). True when a job runs.
bool job_begin(int s) {
  const rmp_slot_entry& m = entry_of(s);
  if (s >= kSlots) fx_stop(s - kSlots);
  else slot_stop(s);
  arena_release(s);   // (API 3: its arena block back for any load it fits)
  g_v3[s] = V3{};     // (a fresh start: no table, no arena, not muted)
  __atomic_and_fetch(&g_muted, ~(1u << s), __ATOMIC_RELEASE);
  g_late[s] = 0, g_late_n[s] = 0;
  if (!m.id[0]) { slot_set(s, RMP_SLOT_EMPTY, nullptr, -1); return false; }
  int k = find_slot(s);
  char b1[12], b2[12], b3[12];
  if (k < 0) {
    slot_set(s, RMP_SLOT_MISSING, nullptr, -1);
    copy(g_why[s], "missing: not installed", sizeof g_why[s]);
    core::log("um: %s%d %s %s: not installed", s < kSlots ? "U" : "UFX", s < kSlots ? s + 1 : s - kSlots + 1, m.id, vstr(m.version, b1));
    return false;
  }
  const Pkg& e = g_pkg[k];
  if (e.version != m.version && (m.version > e.version || m.version < e.compat)) {   // (not the exact one: the newest's range)
    slot_set(s, RMP_SLOT_MISSING, nullptr, k);
    sniprintf(g_why[s], sizeof g_why[s], "missing: the slot wants %s, installed %s (from %s)", vstr(m.version, b1),
              vstr(e.version, b2), vstr(e.compat, b3));
    core::log("um: U%d %s: the slot wants %s, installed %s (from %s)", s + 1, m.id, vstr(m.version, b1),
              vstr(e.version, b2), vstr(e.compat, b2));
    return false;
  }
  for (int t = 0; t < kAll; ++t)   // the same package READY in another slot: its image
    if (t != s && g_slot[t].st == RMP_SLOT_READY && g_slot[t].pkg == k && g_slot[t].crc == (e.crc ^ e.size) && !muted(t)) {
      if (const char* w = kind_wrong(s, g_v3[t].p3)) {
        slot_set(s, RMP_SLOT_BAD, nullptr, k);
        sniprintf(g_why[s], sizeof g_why[s], "bad: %s", w);
        return false;
      }
      if (g_v3[t].p3) {   // API 3: the image shared, the arena and setup() the slot's own
        V3& x = g_v3[s];
        x.p = g_slot[t].p, x.p3 = g_v3[t].p3, x.code = g_v3[t].code, x.code_bytes = g_v3[t].code_bytes;
        slot_set(s, RMP_SLOT_LOADING, nullptr, k);
        core::log("um: %s%d %s: shares %s%d's image, its own arena", s < kSlots ? "U" : "UFX", s < kSlots ? s + 1 : s - kSlots + 1,
                  m.id, t < kSlots ? "U" : "UFX", t < kSlots ? t + 1 : t - kSlots + 1);
        return v3_start(s);
      }
      g_v3[s].p = g_slot[t].p, g_v3[s].code = g_v3[t].code, g_v3[s].code_bytes = g_v3[t].code_bytes;   // (fault offsets)
      slot_set(s, RMP_SLOT_READY, g_slot[t].p, k);
      core::log("um: U%d %s: ready (shares U%d's image)", s + 1, m.id, t + 1);
      return false;
    }
#if CORE_MSAMP
  jf_close();
  if (g_buf && e.size <= kPkgMax) g_jopen = true;   // opened by job_step through the card queue
  if (!g_jopen) {
#else
  if (!g_buf || e.size > kPkgMax || !g_jf.open(e.file, core::kRead)) {
#endif
    slot_set(s, RMP_SLOT_BAD, nullptr, k);
    copy(g_why[s], "bad: cannot read the package", sizeof g_why[s]);
    core::log("um: U%d %s: cannot read %s", s + 1, m.id, e.file);
    return false;
  }
  slot_set(s, RMP_SLOT_LOADING, nullptr, k);
  g_job = s, g_job_off = 0, g_job_len = e.size, g_job_crc = 0xFFFFFFFFu;
  return true;
}
// One step of the load: 8 KB read, or the placement and init().
void job_step() {
  int s = g_job;
  if (g_jph == kJphArena) { arena_step(); return; }
  const Pkg& e = g_pkg[g_slot[s].pkg];
#if CORE_MSAMP
  if (g_jdrop) return;   // (the job before it: its file still closing)
  if (g_jopen) {   // the open, on the card core
    if (!cardq::async(&g_jrq, cardq::kOpenReq, &g_jf, e.file, 0, nullptr, 0)) return;
    g_jopen = false;
    if (g_jrq.got != 1) {
      slot_set(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg);
      copy(g_why[s], "bad: cannot read the package", sizeof g_why[s]);
      core::log("um: U%d %s: cannot read %s", s + 1, g_map.slot[s].id, e.file);
      g_job = -1;
      return;
    }
    return;
  }
#endif
  if (g_job_off < g_job_len) {
    uint32_t n = g_job_len - g_job_off < kChunk ? g_job_len - g_job_off : kChunk;
#if CORE_MSAMP
    if (!cardq::async(&g_jrq, cardq::kReadReq, &g_jf, nullptr, g_job_off, g_buf + g_job_off, n)) return;   // (later)
    if (g_jrq.got != (int32_t)n) {
#else
    if (!g_jf.seekSet(g_job_off) || g_jf.read(g_buf + g_job_off, n) != (int)n) {
#endif
      job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, "read error");
      return;
    }
    if (g_job_off >= RMP_HEADER_BYTES) g_job_crc = rmp_crc32_update(g_job_crc, g_buf + g_job_off, n);
    else g_job_crc = rmp_crc32_update(g_job_crc, g_buf + RMP_HEADER_BYTES, n - RMP_HEADER_BYTES);
    g_job_off += n;
    return;
  }
#if CORE_MSAMP
  if (g_jf.isOpen()) g_jclosing = true;   // (by the request's end, not the handle's flag: the close may be running)
  if (g_jclosing && !cardq::async(&g_jrq, cardq::kCloseReq, &g_jf, nullptr, 0, nullptr, 0)) return;   // (later)
  g_jclosing = false;
#else
  if (g_jf.isOpen()) g_jf.close();
#endif
  UMT("um.place");   // the placement, the cache sync and the plugin's init() (one step)
  rmp_info info;
  rmp_status st = rmp_verify_header(g_buf, g_job_len, &info);
  if (st == RMP_OK && ~g_job_crc != info.crc32) st = RMP_ERR_CRC;
  if (st == RMP_OK && strncmp(info.id, e.id, 32)) st = RMP_ERR_FORMAT;
  if (st != RMP_OK) { job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, "package changed or damaged"); return; }
  info.image = g_buf + RMP_HEADER_BYTES;
  info.relocs = info.image + info.image_size;
  info.manifest = info.relocs + 8u * info.reloc_count;
  // W^X: the code part into the board's executable memory, the writable part (data + bss) into the data arena
  uint32_t bytes = (info.data_start + RMP_A64_ALIGN - 1) & ~(RMP_A64_ALIGN - 1);
  uint32_t dbytes = (info.image_size - info.data_start + info.bss_size + RMP_A64_ALIGN - 1) & ~(RMP_A64_ALIGN - 1);
  uint8_t* img = ex->alloc(bytes);
  uint8_t* dat = img ? data_alloc(dbytes) : nullptr;
  if (!img || !dat) {   // the arenas are full of old images: start over once, else NO ROOM
    if (g_arena_used) { g_full = true; g_job = -1; slot_set(s, RMP_SLOT_LOADING, nullptr, g_slot[s].pkg); return; }
    char w[64];
    uint32_t cf = ex->total > g_arena_used ? ex->total - g_arena_used : 0, df = kDataArena > g_data_used ? kDataArena - g_data_used : 0;
    sniprintf(w, sizeof w, "needs %u B code + %u B data, %u + %u B free", (unsigned)bytes, (unsigned)dbytes, (unsigned)cf, (unsigned)df);
    job_end(s, RMP_SLOT_NO_ROOM, nullptr, g_slot[s].pkg, w);
    return;
  }
  g_arena_used += bytes;
  if (ex->write_begin) ex->write_begin();
  st = rmp_place_a64(&info, img, (uintptr_t)img, dat, (uintptr_t)dat);
  ex->sync(img, bytes);
  const rmp_plugin* p = (const rmp_plugin*)(img + info.entry_offset);
  if (st != RMP_OK) {
    job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, "relocation failed (code and data more than 4 GB apart?)");
    return;
  }
  if (p->magic != RMP_PLUGIN_MAGIC || p->api < RMP_API_MIN || p->api > RMP_API_RMPA || (p->api < RMP_API3 && (!p->init || !p->render))) {
    job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, "bad relocation or entry table");
    return;
  }
  // API 3: the table checked before any of its code runs
  const rmp3_plugin* p3 = nullptr;
  char why[64];
  if (!v3_check(p, img, bytes, &p3, why, sizeof why)) { job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, why); return; }
  if (const char* w = kind_wrong(s, p3)) { job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, w); return; }
  {
    V3& x = g_v3[s];
    x.p = p, x.p3 = p3, x.code = img, x.code_bytes = bytes;
  }
  __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELEASE);
  if (p->init) {   // guarded: a fault refuses the package, the engine plays on
    int32_t r = 0;
    uint64_t pc = 0;
    uint32_t us = 0;
    char at[32];
    if (guarded([&] { r = p->init(host_for(p)); }, &pc, &us)) {
      fault_where(s, pc, at, sizeof at);
      sniprintf(why, sizeof why, "crashed in init at %s", at);
      job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, why);
      return;
    }
    if (r != 0) { job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, "its init() refused"); return; }
    if (p3 && us > kSetupLateUs) {
      sniprintf(why, sizeof why, "init ran %u us (limit %u: build tables in setup, in pieces)", (unsigned)us, (unsigned)kSetupLateUs);
      job_end(s, RMP_SLOT_BAD, nullptr, g_slot[s].pkg, why);
      return;
    }
  }
  if (p3) {   // the arena and setup(): the arena phase (the slot LOADING meanwhile)
    v3_start(s);
    return;
  }
  char b[12];
  core::log("um: U%d %s %s: ready (%u B code at 0x%x, %u B state a voice)", s + 1, e.id, vstr(e.version, b),
            (unsigned)bytes, (unsigned)(uintptr_t)img, (unsigned)e.state);
  job_end(s, RMP_SLOT_READY, p, g_slot[s].pkg, "loaded");
}

// ---- a 1E's answer
int result_of(int s) {
  if (!g_map.slot[s].id[0]) return RMP_RES_READY;
  switch (g_slot[s].st) {
    case RMP_SLOT_READY: return RMP_RES_READY;
    case RMP_SLOT_MISSING: return RMP_RES_NOT_INSTALLED;
    case RMP_SLOT_NO_ROOM: return RMP_RES_NO_ROOM;
    case RMP_SLOT_LOADING: return RMP_RES_LOADING;
    default: return RMP_RES_BAD;
  }
}
void answer(int s, int r) {
  send_1d(s + 1, r);
  g_ans[s] = -1;
  core::log("um: U%d answered %d (%u ms after the 1E)", s + 1, r, (unsigned)(core::now_ms() - g_ans_t[s]));
}
bool assign_id(int s, const char* id, uint32_t version) {
  if (id && id[0] && !rmp_id_ok(id)) return false;
  rmp_resolve_set(&g_map, &g_notes, s, id, version, RMP_SRC_SAVED);   // the user's: saved (empty: explicitly)
  g_fresh = false;
  g_need |= 1u << s;
  g_pub = true;
  g_mp = true;
  if (!map_save()) core::log("um: %s not written (the slot changes until the next project load)", g_map_file);
  return true;
}

#if CORE_MSAMP
// A change of the map while the project's map is being read (a 1E, a console or web um=... right after a project change)
// waits for it, from idle(), never here: made now it would change the old project's map, and the read takes as long as
// the card core does. One 1E a slot (the newest), kHeldCmds commands.
constexpr int kHeldCmds = 8;
struct HeldAssign { bool on; rmp_assign a; };
HeldAssign g_held_as[kSlots];
char g_held_cmd[kHeldCmds][96];
int g_nheld_cmd = 0;
#endif
void assign_run(const rmp_assign& a);
#if CORE_MSAMP
void held_run();
#endif
void on_assign(const uint8_t* d, int len) {
  rmp_assign a;
  if (!rmp_parse_assign(d, len, &a) || a.slot < 1 || a.slot > kSlots) return;
  g_ans_t[a.slot - 1] = core::now_ms();
#if CORE_MSAMP
  if (g_mr) {
    g_held_as[a.slot - 1] = HeldAssign{true, a};
    return;
  }
#endif
  assign_run(a);
}
void assign_run(const rmp_assign& a) {
  int s = a.slot - 1;
  if (a.op == RMP_OP_CLEAR) {
    assign_id(s, "", 0);
    g_ans[s] = (int8_t)a.op;
  } else if (a.op == RMP_OP_ASSIGN) {
    if (a.installed_gen != g_meta.store_gen || a.arg >= g_ninst) { g_ans[s] = 0; answer(s, RMP_RES_NOT_INSTALLED); return; }
    const Pkg& e = g_pkg[g_inst[a.arg]];
    if (!assign_id(s, e.id, e.version)) { g_ans[s] = 0; answer(s, RMP_RES_BAD); return; }
    g_ans[s] = (int8_t)a.op;
  } else {   // install from the card: on this board the card is the store
    g_ans[s] = (int8_t)a.op;
    answer(s, g_slot[s].st == RMP_SLOT_READY ? RMP_RES_READY : RMP_RES_NOT_ON_CARD);
  }
}
void on_ask(const uint8_t* d, int len) {
  rmp_ask a;
  if (!rmp_parse_ask(d, len, &a) || a.slot > kSlots) return;
  if (a.what == RMP_ASK_INSTALLED || core::now_ms() - g_1d_ms >= 3000) g_1d_owed = true;
  int s = a.slot - 1;
  if (a.what == RMP_ASK_META && a.slot >= 1) {
    g_rytm_names36 = a.arg & 1;
    core::ui::remote_rytm_040(g_rytm_names36);   // (and SMP on its knob A: rytm-mods 7h, the same flash)
    meta_poll();
    if (a.gen && a.gen == g_meta.gen[s] && settled(s)) {
      uint8_t m[RMP_MSG_MAX];
      core::to_rytm(m, rmp_build_meta_unchanged(m, a.slot, a.gen));
    } else {
      send_meta(s);
    }
  } else if (a.what == RMP_ASK_ENUM && a.slot >= 1 && a.arg < 16) {
    send_enum(s, a.arg);
  } else if (a.what == RMP_ASK_PRESETS && a.slot >= 1 && a.arg <= 7) {   // page arg of the names, from idle()
    g_pre_page[s] |= (uint8_t)(1u << a.arg);
  } else if (a.what == RMP_ASK_INSTALLED) {
    if (g_scan && g_npkg == 0) g_list_owed = true;   // an empty list now would turn the Rytm's YES into a clear
    else send_installed();
  }
}
void on_project_seen() { g_1d_owed = true; }
void on_project_changed() {
  for (int vi = 0; vi < core::kVoices; ++vi) voice_leave(vi);
  project_reload();
}

// ---- the JSON
struct Js {
  char* b; int cap, n;
  void put(const char* s) { while (*s && n < cap - 1) b[n++] = *s++; b[n] = 0; }
  void str(const char* s) {   // a JSON string, printable ASCII
    put("\"");
    for (; *s && n < cap - 8; ++s) {
      char c = *s;
      if (c == '"' || c == '\\') { b[n++] = '\\'; b[n++] = c; }
      else if (c >= 0x20 && c <= 0x7e) b[n++] = c;
      else b[n++] = '_';
    }
    b[n] = 0;
    put("\"");
  }
  void num(const char* k, uint32_t v) { char t[24]; sniprintf(t, sizeof t, "\"%s\":%u", k, (unsigned)v); put(t); }
};
// Slot s as the Rytm shows it (the display's view, the USER SLOTS page): its machine's name, "---" empty, "NAME..."
// loading, else the reason ("!ROOM NPPR")
void slot_shown(int s, char* out, int cap) {
  rmp_desc d;
  const rmp_desc* p = slot_desc(s, &d);
  rmp_slot_state st = g_slot[s].st;
  const char* nm = p ? p->name : "";
  if (!g_map.slot[s].id[0]) copy(out, "---", cap);
  else if (st == RMP_SLOT_READY) copy(out, nm, cap);
  else if (st == RMP_SLOT_LOADING) sniprintf(out, cap, "%.8s...", nm);
  else {
    char w[10];
    rmp_why_name(w, rmp_why_of(rmp_proto_state(st), RMP_WHY_NONE), p);
    sniprintf(out, cap, "!%.10s", w[0] ? w : nm);
  }
}
void publish() {
  uint32_t seq = g_jseq;
  int w = (int)((seq / 2 + 1) & 1);
  Js j{g_js[w], (int)sizeof g_js[w], 0};
  char vb[12];
  uint32_t state_used = 0;
  j.put("[");
  for (int i = 0; i < g_npkg; ++i) {
    const Pkg& e = g_pkg[i];
    j.put("{\"t\":\"plugin\",\"id\":"); j.str(e.id[0] ? e.id : "?");
    j.put(",\"ver\":"); j.str(vstr(e.version, vb));
    j.put(","); j.num("size", e.size); j.put(","); j.num("api", (e.hflags & RMP_HF_API3) ? 3u : (uint32_t)e.api);   // (a dual package's header says 2)
    if (e.hflags & RMP_HF_EFFECT) j.put(",\"effect\":true");   // (API 3: a user effect, UFX1..UFX4; never a machine)
    j.put(","); j.num("image", e.image);
    j.put(","); j.num("bss", e.bss); j.put(","); j.num("state", e.state); j.put(","); j.num("cpu", e.cpu);
    uint32_t ns = 0, nb = 0;   // measured: the slowest slot playing it
    for (int s = 0; s < kSlots; ++s)
      if (g_slot[s].st == RMP_SLOT_READY && g_slot[s].pkg == i) {
        uint32_t b = 0, x = cost_ns(s, &b);
        if (x > ns) ns = x;
        nb += b;
      }
    j.put(","); j.num("ns", ns); j.put(","); j.num("blocks", nb);
    j.put(",\"name\":"); j.str(e.name[0] ? e.name : e.desc.name);
    j.put(",\"slots\":[");
    bool first = true;
    for (int s = 0; s < kSlots; ++s)
      if (e.id[0] && !strncmp(g_map.slot[s].id, e.id, 32)) {
        char t[8];
        sniprintf(t, sizeof t, "%s%d", first ? "" : ",", s + 1);
        j.put(t);
        first = false;
      }
    j.put("],\"fit\":"); j.put(e.ok ? "true" : "false");
    j.put(",\"older\":"); j.put(e.ok && e.older ? "true" : "false");
    j.put(",\"why\":"); j.str(e.ok ? "" : e.why);
    j.put(",\"file\":"); j.str(e.file);
    j.put(",\"target\":"); j.str(e.target == RMP_TARGET_A64 ? "a64" : "arm32");
    j.put(","); j.num("fp", e.fp);
    j.put(",\"status\":"); j.str(e.ok ? "ok" : e.why);
    j.put("},");
  }
  for (int s = 0; s < kSlots; ++s) {
    const rmp_slot_entry& m = g_map.slot[s];
    rmp_desc d;
    const rmp_desc* p = slot_desc(s, &d);
    if (g_slot[s].st == RMP_SLOT_READY) state_used += g_slot[s].state_bytes;
    j.put("{\"t\":\"slot\","); j.num("slot", (uint32_t)s + 1);
    j.put(",\"id\":"); j.str(m.id);
    j.put(",\"ver\":"); j.str(m.id[0] ? vstr(m.version, vb) : "");
    j.put(",\"state\":"); j.str(state_name(g_slot[s].st));
    {   // labels that follow a list knob: the value's set shown (-1: the static labels, or none)
      int k = find_slot(s);
      if (k >= 0 && g_pkg[k].lbl.nsets) { j.put(","); j.put("\"lset\":"); char t[8]; sniprintf(t, sizeof t, "%d", lbl_shown(s, g_pkg[k])); j.put(t); }
    }
    int bp = g_slot[s].pkg >= 0 && g_slot[s].pkg < g_npkg ? g_slot[s].pkg : -1;   // the package it plays (its exact version)
    j.put(",\"file\":"); j.str(bp >= 0 && g_slot[s].st == RMP_SLOT_READY ? g_pkg[bp].file : "");
    j.put(",\"play_ver\":"); j.str(bp >= 0 && g_slot[s].st == RMP_SLOT_READY ? vstr(g_pkg[bp].version, vb) : "");
    j.put(",\"name\":"); j.str(p ? p->name : "");
    if (g_why[s][0]) { j.put(",\"why\":"); j.str(g_why[s]); }
    if (g_slot[s].st == RMP_SLOT_READY) {   // API 3: the guard's mute, late calls, the plugin's API, its arena
      j.put(",\"muted\":"); j.put(muted(s) ? "true" : "false");
      j.put(","); j.num("late", g_late_n[s]);
      j.put(","); j.num("api", g_v3[s].p3 ? 3u : g_slot[s].p ? g_slot[s].p->api : 0u);
      j.put(","); j.num("arena", g_v3[s].zero_len);
    }
    j.put(","); j.num("fp", g_slot[s].fp);
    uint32_t nb = 0, ns = g_slot[s].st == RMP_SLOT_READY ? cost_ns(s, &nb) : 0;
    j.put(","); j.num("ns", ns); j.put(","); j.num("blocks", nb);
    const rmp_slot_note& e = g_notes.slot[s];   // where the assignment came from, what the sounds want
    j.put(",\"src\":"); j.str(rmp_src_name(e.src));
    j.put(",\"updated\":"); j.put(e.flags & RMP_RF_UPDATED ? "true" : "false");
    j.put(",\"collision\":"); j.put(e.flags & RMP_RF_COLLISION ? "true" : "false");
    j.put(",\"unknown\":"); j.put(e.flags & RMP_RF_UNKNOWN ? "true" : "false");
    j.put(",\"mixed\":"); j.put(e.flags & RMP_RF_MIXED ? "true" : "false");
    j.put(","); j.num("want_fp", e.fp); j.put(","); j.num("want_n", e.nfp);
    j.put(",\"want_id\":"); j.str(e.id);
    j.put(",\"want_ver\":"); j.str(e.id[0] ? vstr(e.version, vb) : "");
    j.put(",\"want_name\":"); j.str(e.name);
    j.put(","); j.num("presets", rmp_preset_index_ok(&g_pix[s]) ? g_pix[s].n : 0);
    j.put(",\"pre_ok\":"); j.put(rmp_preset_index_ok(&g_pix[s]) ? "true" : "false");
    j.put(",\"pre_ext\":"); j.str(p ? p->preset_ext : "");
    j.put("},");
  }
  for (int u = 0; u < kUfx; ++u) {   // API 3's user effects (UFX1..UFX4), when one is assigned
    int s = kSlots + u;
    if (!g_fxmap[u].id[0]) continue;
    j.put("{\"t\":\"ufx\","); j.num("ufx", (uint32_t)u + 1);
    j.put(",\"id\":"); j.str(g_fxmap[u].id);
    j.put(",\"state\":"); j.str(state_name(g_slot[s].st));
    if (g_why[s][0]) { j.put(",\"why\":"); j.str(g_why[s]); }
    j.put(",\"muted\":"); j.put(muted(s) ? "true" : "false");
    j.put(",\"left_behind\":"); j.put(g_fx_dead[u] ? "true" : "false");
    j.put(","); j.num("arena", g_v3[s].zero_len);
    j.put("},");
  }
  j.put("{\"t\":\"plugins\","); j.num("n", (uint32_t)g_npkg);
  j.put(",\"prints\":"); j.str(g_prints_have ? "rytm" : g_prints_rytm ? "waiting" : "trigs");
  j.put(","); j.num("resolves", g_resolve_n);
  j.put(",\"fresh\":"); j.put(g_fresh ? "true" : "false");
  j.put(",\"project\":"); j.str(core::project[0] ? core::project : "_default");
  char t[96];
  sniprintf(t, sizeof t, ",\"arena\":[%u,%u],", (unsigned)(ex ? ex->total : 0), (unsigned)g_arena_used);
  j.put(t);
  sniprintf(t, sizeof t, "\"plugpool\":[%u,%u],", (unsigned)g_pool_bytes, (unsigned)g_pool_used);   // API 3: the arenas' pool
  j.put(t);
  j.num("state_limit", kStateMax);
  j.put(","); j.num("block_ns", (uint32_t)(core::kBlock * 1000000000ull / 48000));   // a voice block's real time
  j.put(","); j.num("listed", (uint32_t)g_ninst); j.put(","); j.num("gen", g_meta.store_gen);
  j.put(",\"dir\":"); j.str(g_dir);
  j.put(",\"scanning\":"); j.put(g_scan ? "true" : "false");
  j.put("},");
  sniprintf(t, sizeof t, "{\"t\":\"mem\",\"k\":\"code\",\"size\":%u,\"used\":%u},", (unsigned)(ex ? ex->total : 0),
            (unsigned)g_arena_used);
  j.put(t);
  sniprintf(t, sizeof t, "{\"t\":\"mem\",\"k\":\"data\",\"size\":%u,\"used\":%u},", (unsigned)kDataArena, (unsigned)g_data_used);
  j.put(t);
  sniprintf(t, sizeof t, "{\"t\":\"mem\",\"k\":\"state\",\"size\":%u,\"used\":%u},", (unsigned)(kStateMax * core::kVoices),
            (unsigned)(state_used * core::kVoices));
  j.put(t);
  j.put("{\"t\":\"mem_end\"}]");
  g_jlen[w] = j.n;
  UiView& u = g_ui[w];   // the display's view, in the same buffer half
  copy(u.project, core::project[0] ? core::project : "_default", sizeof u.project);
  for (int s = 0; s < kSlots; ++s) {
    u.slot[s].state = (uint8_t)g_slot[s].st;
    slot_shown(s, u.slot[s].name, sizeof u.slot[s].name);
  }
  __atomic_store_n(&g_jseq, seq + 2, __ATOMIC_RELEASE);   // the reader takes buffer (seq/2) & 1
}

// The USER SLOTS page's hooks (core/ui/remote.h RemoteSlots): the installed list the box gets (1B), the slots as the
// display shows them, and the console's own commands for a change - the web page's path, so the map, the fingerprints,
// the save and the project's list are the same whoever asks
int ui_machines() { return g_ninst; }
bool ui_machine(int i, char* name, int cap, const char** id) {
  if (i < 0 || i >= g_ninst) return false;
  const Pkg& e = g_pkg[g_inst[i]];
  copy(name, e.name, cap);
  *id = e.id;
  return true;
}
const char* ui_slot(int s, char* name, int cap) {
  if (s < 0 || s >= kSlots) { copy(name, "", cap); return ""; }
  slot_shown(s, name, cap);
  return g_map.slot[s].id;
}
const core::ui::RemoteSlots kUiSlots = {ui_machines, ui_machine, ui_slot, command};

}  // namespace

void rekey(const char* key) {
#if CORE_MSAMP
  if (g_mr) {   // a read under way: of the name just moved; g_map_file stays g_map's until it ends
    sniprintf(g_mr_map, sizeof g_mr_map, "/.rytm/slots/%s.txt", key);
    notes_file(g_mr_notes, g_mr_map, sizeof g_mr_notes);
    g_mr_again = true;
    return;
  }
#endif
  sniprintf(g_map_file, sizeof g_map_file, "/.rytm/slots/%s.txt", key);
  notes_file(g_notes_file, g_map_file, sizeof g_notes_file);
}

void begin(const Exec* e, const char* d) {
  ex = e;
  copy(g_dir, d, sizeof g_dir);
#if defined(__aarch64__)
  rmp_host_target = RMP_TARGET_A64;   // this board runs RMPA images (the Pi, the arm64 Mac)
#endif
  g_host1 = rmp_host{1u, 48000.0f, core::kBlock, plog, prandom, {}};
  g_host2 = rmp_host{RMP_API, 48000.0f, core::kBlock, plog, prandom, {}};
  g_buf = (uint8_t*)um_heap(kPkgMax);   // everything from the heap now, at boot (the Pi's heap is core 0's)
  if (!e->data_alloc) data_alloc(0);
  for (int vi = 0; vi < core::kVoices; ++vi) {
    g_vstate[vi] = (uint8_t*)um_heap(kStateMax);
    g_cur[vi] = -1;
  }
  for (int i = 0; i < kInst; ++i) g_istate[i] = (uint8_t*)um_heap(kStateMax), g_icur[i] = -1, g_ivi[i] = -1;
  for (int s = 0; s < kSlots; ++s) g_lbl_vi[s] = g_lbl_cur[s] = -1, g_lbl_ms[s] = 0, g_lbl_key[s] = 0;
  for (int s = 0; s < kSlots; ++s) g_ans[s] = -1;
  for (int s = 0; s < kAll; ++s) g_slot[s] = Slot{RMP_SLOT_EMPTY, nullptr, -1, 0, 0, 0};
  for (int c = 0; c < kCores; ++c) g_vnow[c] = -1;
  rmp_preset_names_init(&g_pnames);
  for (int s = 0; s < kSlots; ++s) rmp_preset_index_init(&g_pix[s]), rmp_knobs_from_desc(&g_knobs[s], nullptr);
  for (int vi = 0; vi < core::kVoices; ++vi) rmp_pv_reset(&g_pv[vi]);
  g_host2.reserved[RMP_H_POOL_INFO] = RMP_FN(h_pool_info);   // API 2: the project's pool (the sample RAM's copies)
  g_host2.reserved[RMP_H_POOL_READ] = RMP_FN(h_pool_read);
  // API 3: the arenas' pool, the host's table (in reserved[RMP_H_V3] of the API 2 table too: a dual plugin finds it
  // there, its host->api still 2), an API 3 only plugin's host table, the first transport snapshot
  g_pool_bytes = 0;
  g_pool = e->arena ? e->arena(&g_pool_bytes) : nullptr;
  if (!g_pool) g_pool_bytes = 0;
  memset(&g_h3, 0, sizeof g_h3);
  g_h3.magic = RMP3_MAGIC, g_h3.size = sizeof g_h3, g_h3.minor = RMP3_MINOR, g_h3.caps = host_caps();
  g_h3.board = e->board, g_h3.cpu_mhz = 0, g_h3.arena_limit = arena_limit();
  g_host2.reserved[RMP_H_V3] = &g_h3;
  g_host3 = g_host2;
  g_host3.api = RMP_API3;
  for (rmp3_transport& t : g_xp) t = rmp3_transport{0, 120.0f, 20833.333f, 0.0f, -1, 0.0f, 0, kLeadFrames};
  for (rmp3_sig_in& x : g_sig_none) x = rmp3_sig_in{g_sig_zero, 0.0f, 0};
  core::hooks.pool_changed = on_pool_changed;
  rmp_meta_init(&g_meta, core::random_seed() | 1u);
  core::hooks.user_report = on_report;
  core::hooks.user_leave = on_leave;
  core::hooks.user_note_off = on_note_off;
  core::hooks.user_render = on_render;
  core::hooks.user_render2 = on_render2;
  core::fx::user_ready = fx_ready;   // API 3's user effects in the send FX's UFX TYPEs (core/fx/send_fx.h)
  core::fx::user_run = fx_run;
  core::fx::user_labels = fx_labels;
  core::fx::user_loaded = [](int u) { return u >= 0 && u < kUfx && g_slot[kSlots + u].st == RMP_SLOT_READY && g_fxmap[u].id[0] != 0; };
  core::hooks.slot_ask = on_ask;
  core::hooks.slot_assign = on_assign;
  core::hooks.project_seen = on_project_seen;
  core::hooks.project_changed = on_project_changed;
  core::hooks.slot_prints = on_prints;
  core::ui::remote_slots(&kUiSlots);   // the USER SLOTS page on the box's screen
  project_reload();
#if CORE_MSAMP
  g_scan_at = core::now_ms();   // (not the 0 it starts as: now_ms() - 0 reads as in the future after 24.8 days of uptime)
#endif
  g_scan = 1;
  core::log("um: user machines from %s (code arena %u KB, %u KB state a voice; API 3: arena pool %u KB, %s)", g_dir,
            (unsigned)(e->total >> 10), (unsigned)(kStateMax >> 10), (unsigned)(g_pool_bytes >> 10),
            e->guard ? "guarded calls" : "calls not guarded");
}

bool idle() {
  if (!ex) return false;
  bool did = true;
#if CORE_MSAMP
  if (g_jdrop) jf_drain();
  if (g_mr) {
    UMT("um.map");
    map_step();
    if (!g_mr) held_run();
  } else
#endif
  if (g_scan && !scan_wait()) { UMT("um.scan"); scan_step(); }
  else if (g_full && fx_any_dead()) {   // (N2: core 2 is still in a left-behind effect: no reset under it)
    did = false;
  } else if (g_full) {
    for (int vi = 0; vi < core::kVoices; ++vi) voice_leave(vi);
    for (int u = 0; u < kUfx; ++u) fx_stop(u);   // (core 2 out of the effects before their images go)
    for (int s = 0; s < kAll; ++s) slot_set(s, entry_of(s).id[0] ? RMP_SLOT_LOADING : RMP_SLOT_EMPTY, nullptr, -1);
    jf_close();
    g_job = -1;
    ex->reset();
    g_arena_used = 0;
    g_data_used = 0;
    g_pool_used = 0;   // (API 3: the arenas start over with the code)
    g_nfree = 0;
    g_jph = kJphImage;
    for (int s = 0; s < kAll; ++s) g_v3[s] = V3{};
    g_full = false;
    g_need = (1u << kSlots) - 1;
    for (int u = 0; u < kUfx; ++u)
      if (g_fxmap[u].id[0]) g_need |= 1u << (kSlots + u);
  } else if (g_prints_pending) { UMT("um.prints"); prints_apply(); }
  else if (g_fresh && (int32_t)(core::now_ms() - g_fresh_until) >= 0) { UMT("um.inherit"); inherit_now(); }
  else if (g_resolve_save) {
    g_resolve_save = false;
    UMT("um.mapsave");
    if (!map_save()) core::log("um: %s not written (the slots change until the next project load)", g_map_file);
  } else if ([] { UMT("um.prepass"); return pre_pass(); }()) {
  } else if (g_job >= 0) { UMT("um.job"); job_step(); }
  else if (g_need) {
    int s = __builtin_ctz(g_need);
    g_need &= ~(1u << s);
    UMT("um.begin");
    job_begin(s);
  } else if ([] { UMT("um.prepages"); return pre_pages(); }()) {
  } else if (pscan_busy()) {
    UMT("um.pscan");
    pscan_step();
  } else did = scan_wait();
  { UMT("um.notes"); notes_overdue(); }
  fx_faults();
  { UMT("um.upool"); if (upool_pass()) did = true; }
  if (g_pre_again && (int32_t)(core::now_ms() - g_pre_again_ms) >= 0 && !core::fmt::pending(g_pre_path)) {   // a preset written: the lists again
    g_pre_again = false;
    for (int s = 0; s < kSlots; ++s) pre_invalidate(s);
  }
  for (int s = 0; s < kSlots; ++s) {   // answers: settled, or late
    if (g_ans[s] < 0) continue;
    if (settled(s)) { answer(s, result_of(s)); did = true; }
    else if (core::now_ms() - g_ans_t[s] > kAnsMs) { answer(s, RMP_RES_LOADING); did = true; }
  }
  if (g_mp || did || core::now_ms() - g_mp_ms >= 100) {   // (the prints hash every description: not every pass)
    g_mp = false;
    g_mp_ms = core::now_ms();
    UMT("um.meta");
    meta_poll();
  }
  uint32_t age = core::now_ms() - g_1d_ms;
  if (g_meta.dirty || (g_1d_owed && age >= 1000) || age >= 5000 || !g_1d_sent) { UMT("um.1d"); send_1d(0, 0); did = true; }
  if (core::now_ms() - g_cost_ms >= 1000) {   // the measured cost moves while something plays
    g_cost_ms = core::now_ms();
    uint32_t tot = 0;
    for (int s = 0; s < kSlots; ++s) { uint64_t c; uint32_t b; cost_sum(s, &c, &b); tot += b; }
    if (tot != g_cost_blk) { g_cost_blk = tot; g_pub = true; }
  }
  if (g_pub && core::now_ms() - g_pub_ms >= 100) {
    g_pub = false;
    g_pub_ms = core::now_ms();
    publish();
  }
  return did;
}

void call_stall_limit(uint32_t us) { g_call_stall_us = us ? us : kCallStallUs; }
void render_fault(int vi) {
  if (vi >= 0 && vi < core::kVoices) __atomic_or_fetch(&g_render_faults, 1u << vi, __ATOMIC_ACQ_REL);
}
// API 3: the block's transport snapshot (see um.h). The newest becomes the render cores' at the release store.
void pre_block() {
  if (!ex) return;
  core::lfo::ClockState c;
  core::lfo::clock_state(&c);
  uint32_t g = g_xp_gen + 1;
  rmp3_transport& t = g_xp[g & 1];
  uint32_t since = core::now_us() - c.last_us;
  bool clocked = c.ticking && since < 250000;
  bool locked = c.run && clocked && c.pos >= 0;
  float tu = c.tick_us > 100.0f ? c.tick_us : 20833.333f;
  uint32_t f = (c.run ? (uint32_t)RMP3_T_RUNNING : 0u) | (clocked ? (uint32_t)RMP3_T_CLOCKED : 0u) | (locked ? (uint32_t)RMP3_T_LOCKED : 0u);
  if (c.starts != g_xp_starts) f |= RMP3_T_START;
  if (c.stops != g_xp_stops) f |= RMP3_T_STOP;
  int32_t song = locked ? c.pos : -1;
  // a song position message, or the position going back (a restart the Rytm sent no F2 for)
  if (c.jumps != g_xp_jumps || (song >= 0 && g_xp_song >= 0 && song < g_xp_song)) f |= RMP3_T_JUMP;
  g_xp_starts = c.starts, g_xp_stops = c.stops, g_xp_jumps = c.jumps, g_xp_song = song;
  float frac = locked ? (float)since / tu : 0.0f;
  t.flags = f;
  t.tick_us = tu;
  t.bpm = 60e6f / (24.0f * tu);
  t.free_tick = c.tp;
  t.song_tick = song;
  t.song_frac = frac < 0.0f ? 0.0f : frac > 0.999f ? 0.999f : frac;
  t.step16 = locked ? (uint32_t)c.pos / 6u : 0u;
  t.lead_frames = kLeadFrames;
  if (f & RMP3_T_START) ++g_xp_evn[0];
  if (f & RMP3_T_STOP) ++g_xp_evn[1];
  if (f & RMP3_T_JUMP) ++g_xp_evn[2];
  for (int k = 0; k < 3; ++k) g_xp_ev[g & 1][k] = g_xp_evn[k];
  __atomic_store_n(&g_xp_gen, g, __ATOMIC_RELEASE);
}

void rescan() {
  if (g_scan) {
    g_scan_again = true;
#if CORE_MSAMP
    g_scan_at = core::now_ms() + kRescanQuietMs;
#endif
  } else {
#if CORE_MSAMP
    g_scan_at = core::now_ms();   // (the last rescan's time may be more than 24.8 days old: a stale one waits as if in the future)
#endif
    g_scan = 1;
  }
}

bool tag_ok(int s, int tag) { return s >= 0 && s < kSlots && g_slot[s].st == RMP_SLOT_READY && tag == g_slot[s].fp; }

// ---- E0: instances (core/inst.h)
static_assert(kInst == core::inst::kTotal, "um's instance states: one per core::inst instance");
bool inst_decl(int vi, uint32_t* flags, uint32_t* cpu, uint32_t* state) {
  int s = vi >= 0 && vi < core::kVoices ? core::v[vi].user : -1;
  if (s < 0 || s >= kSlots || g_slot[s].st != RMP_SLOT_READY || !g_slot[s].p || g_slot[s].pkg < 0) return false;
  const Pkg& e = g_pkg[g_slot[s].pkg];
  *flags = e.hflags, *cpu = e.cpu, *state = e.state;
  return true;
}
bool inst_note_on(int vi, int i, float note, float vel) {
  int s = vi >= 0 && vi < core::kVoices ? core::v[vi].user : -1;
  if (i < 0 || i >= kInst || !g_istate[i] || s < 0 || s >= kSlots) return false;
  Slot& sl = g_slot[s];
  if (sl.st != RMP_SLOT_READY || !sl.p || (g_mis >> vi & 1) || muted(s) || !state_room(s, i, vi)) return false;
  void* st = g_istate[i];
  VNow now(vi);
  if (g_icur[i] != s || g_ivi[i] != vi) {   // a fresh state of the voice's machine
    inst_leave(i);
    if (!c_voice_init(s, st, vi, i)) return false;
    g_icur[i] = (int8_t)s, g_ivi[i] = (int8_t)vi;
    c_vfn(s, rmp_voice_enter_of(sl.p), st, "voice_enter");
  }
  float q[16];
  pv_params(vi, s, true, q);   // the voice's params as its own state gets them (its preset's hold, the offsets)
  c_params(s, st, q);
  c_note_on(s, st, note, vel < 0.0f ? 0.0f : vel > 1.0f ? 1.0f : vel);
  return !muted(s);
}
void inst_note_off(int vi, int i) {
  if (i < 0 || i >= kInst || g_icur[i] < 0 || g_ivi[i] != vi) return;
  const Slot& sl = g_slot[g_icur[i]];
  VNow now(vi);
  if (sl.st == RMP_SLOT_READY && sl.p) c_note_off(g_icur[i], g_istate[i]);
}
void inst_params(int vi) {
  int s = vi >= 0 && vi < core::kVoices ? core::v[vi].user : -1;
  if (s < 0 || s >= kSlots || g_slot[s].st != RMP_SLOT_READY || !g_slot[s].p) return;
  float q[16];
  pv_params(vi, s, false, q);
  VNow now(vi);
  for (int i = 0; i < kInst; ++i)
    if (g_icur[i] == s && g_ivi[i] == vi) c_params(s, g_istate[i], q);
}
void inst_stop(int vi) {
  for (int i = 0; i < kInst; ++i)
    if (g_ivi[i] == vi) inst_leave(i), g_ivi[i] = -1;
}
// A render core: instance i of voice vi, kBlock samples (silence when its state is not the voice's slot's)
void inst_render(int vi, int i, float* o) {
  int s = vi >= 0 && vi < core::kVoices ? core::v[vi].user : -1;
  if (i < 0 || i >= kInst || s < 0 || g_icur[i] != s || g_ivi[i] != vi || g_slot[s].st != RMP_SLOT_READY || !g_slot[s].p ||
      muted(s)) {
    for (int k = 0; k < core::kBlock; ++k) o[k] = 0.0f;
    return;
  }
  uint32_t e = __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE);
  if (g_voice_epoch[vi] != e) {
    if (ex->barrier) ex->barrier();
    g_voice_epoch[vi] = e;
  }
  {
    VNow now(vi);
    const rmp3_plugin* p3 = g_v3[s].p3;
    if (p3) {
      rmp3_block b;
      block_fill(&b, core::kBlock, o, nullptr, RMP3_AT_VOICE);
      p3->render(g_istate[i], &b);
    } else {
      g_slot[s].p->render(g_istate[i], o, core::kBlock);
    }
  }
  for (int k = 0; k < core::kBlock; ++k)   // a NaN or a runaway level: clipped
    o[k] = o[k] == o[k] ? (o[k] > 1.0f ? 1.0f : o[k] < -1.0f ? -1.0f : o[k]) : 0.0f;
}

#if CORE_MSAMP
namespace {
void held_run() {   // the map is here
  for (int s = 0; s < kSlots; ++s)
    if (g_held_as[s].on) {
      g_held_as[s].on = false;
      assign_run(g_held_as[s].a);
    }
  for (int i = 0; i < g_nheld_cmd; ++i) {
    char o[160];
    if (command(g_held_cmd[i], o, sizeof o) && o[0]) core::log("um: %s", o);
  }
  g_nheld_cmd = 0;
}
bool map_cmd(const char* c) {   // a command that changes the map
  return !strncmp(c, "um=assign:", 10) || !strncmp(c, "um=clear:", 9) || !strcmp(c, "um=empty") || !strncmp(c, "um=put:", 7);
}
}  // namespace
#endif

bool command(const char* c, char* out, int cap) {
  out[0] = 0;
#if CORE_MSAMP
  if (g_mr && map_cmd(c)) {
    if (g_nheld_cmd >= kHeldCmds || strlen(c) >= sizeof g_held_cmd[0]) { sniprintf(out, cap, "the project's map is loading: again in a moment"); return true; }
    copy(g_held_cmd[g_nheld_cmd++], c, sizeof g_held_cmd[0]);
    sniprintf(out, cap, "after the project's map is read");
    return true;
  }
#endif
  if (strcmp(c, "um") == 0) {
    char vb[12];
    for (int i = 0; i < g_npkg; ++i)
      core::log("um: %s %s %s %u B, state %u B, cpu %u, %s", g_pkg[i].file, g_pkg[i].id, vstr(g_pkg[i].version, vb),
                (unsigned)g_pkg[i].size, (unsigned)g_pkg[i].state, (unsigned)g_pkg[i].cpu, g_pkg[i].ok ? "ok" : g_pkg[i].why);
    for (int s = 0; s < kSlots; ++s)
      core::log("um: U%d %s %s (%s, sounds' fingerprint %u)", s + 1, g_map.slot[s].id[0] ? g_map.slot[s].id : "-",
                state_name(g_slot[s].st), rmp_src_name(g_notes.slot[s].src), (unsigned)g_notes.slot[s].fp);
    sniprintf(out, cap, "%d packages, project %s, code %u/%u B", g_npkg, core::project[0] ? core::project : "_default",
              (unsigned)g_arena_used, (unsigned)(ex ? ex->total : 0));
    return true;
  }
  if (strncmp(c, "um=fx:", 6) == 0) {   // API 3: UFX<n> plays an installed effect (its newest version), or none ("-")
    int u = c[6] - '1';
    if (u < 0 || u >= kUfx || c[7] != ':') { sniprintf(out, cap, "usage: um=fx:<1..4>:<id|->"); return true; }
    const char* id = c + 8;
    fx_stop(u);   // (core 2 out of the one it plays, before anything about it changes)
    rmp_slot_entry& m = g_fxmap[u];
    if (!strcmp(id, "-")) {
      m.id[0] = 0;
    } else {
      int k = find_pkg(id);
      if (k < 0) { sniprintf(out, cap, "UFX%d: %s is not installed", u + 1, id); return true; }
      copy(m.id, g_pkg[k].id, sizeof m.id);
      m.version = g_pkg[k].version;
    }
    __atomic_store_n(&core::fx::user[u].tag, (uint16_t)0, __ATOMIC_RELEASE);   // (named again once it is READY: fx_named)
    g_need |= 1u << (kSlots + u);
    sniprintf(out, cap, "UFX%d: %s", u + 1, m.id[0] ? m.id : "empty");
    return true;
  }
  if (strncmp(c, "um=fxbudget:", 12) == 0) {   // API 3: the effects' budgets, us a 12-frame block: one call, all of core 2's
    unsigned a = 0, b = 0;
    const char* q = c + 12;
    while (*q >= '0' && *q <= '9') a = a * 10 + (unsigned)(*q++ - '0');
    if (*q == ':') ++q;
    while (*q >= '0' && *q <= '9') b = b * 10 + (unsigned)(*q++ - '0');
    if (a) g_fx_budget_us = a;
    if (b) g_fx_total_us = b;
    sniprintf(out, cap, "effects: %u us a call, %u us all of core 2's a block", (unsigned)g_fx_budget_us, (unsigned)g_fx_total_us);
    return true;
  }
  if (strncmp(c, "um=reload:", 10) == 0) {   // API 3: load the slot again (a muted slot back, its states and arena fresh)
    int s = c[10] == 'f' ? kSlots + (c[11] - '1') : c[10] - '1';   // um=reload:f<1..4>: UFX<n>
    if (s < 0 || s >= kAll || (s >= kSlots && c[12]) || (s < kSlots && c[11])) { sniprintf(out, cap, "usage: um=reload:<1..5> or f<1..4>"); return true; }
    g_need |= 1u << s;
    sniprintf(out, cap, "%s%d: loading again%s", s < kSlots ? "U" : "UFX", s < kSlots ? s + 1 : s - kSlots + 1, muted(s) ? " (it was muted)" : "");
    return true;
  }
  if (strncmp(c, "um=save:", 8) == 0) {   // a generic preset of what the slot's voice plays now
    int s = c[8] - '1';
    if (s < 0 || s >= kSlots || c[9] != ':') { sniprintf(out, cap, "usage: um=save:<1..5>:<name>"); return true; }
    pre_save(s, c + 10, out, cap);
    return true;
  }
  if (strcmp(c, "um=presets") == 0) {   // the card's /PRESETS changed (the web's rename / delete): every list again
    for (int s = 0; s < kSlots; ++s) pre_invalidate(s);
    sniprintf(out, cap, "presets: rescanning");
    return true;
  }
  if (strcmp(c, "um=rescan") == 0) {
    rescan();
    sniprintf(out, cap, "rescanning %s", g_dir);
    return true;
  }
  if (strcmp(c, "um=empty") == 0) {   // "start this project empty": every slot explicitly empty
    for (int s = 0; s < kSlots; ++s) rmp_resolve_set(&g_map, &g_notes, s, "", 0, RMP_SRC_SAVED);
    g_fresh = false;
    g_need = (1u << kSlots) - 1;
    g_pub = g_mp = true;
    if (!map_save()) core::log("um: %s not written", g_map_file);
    sniprintf(out, cap, "project %s starts empty", core::project[0] ? core::project : "_default");
    return true;
  }
  if (strncmp(c, "um=put:", 7) == 0) {   // an imported slot list's entry: um=put:<1..5>:<id|->:<a.b.c>[:<name>]
    char id[32] = "", name[11] = "";
    unsigned a = 0, b = 0, v = 0;
    int s = c[7] - '1';
    const char* p = c + 9;
    const char* q = s >= 0 && s < kSlots && c[8] == ':' ? strchr(p, ':') : nullptr;
    bool ok = q && q > p && q - p < 32;
    if (ok) { memcpy(id, p, (size_t)(q - p)); id[q - p] = 0; }
    const char* r = ok ? q + 1 : nullptr;
    if (ok) {   // a.b.c
      int k = 0;
      unsigned part[3] = {0, 0, 0};
      int nd = 0;   // digits in the current part: 1..3
      for (; ok && *r && *r != ':'; ++r) {
        if (*r == '.') { if (!nd || ++k > 2) ok = false; nd = 0; continue; }
        if (*r < '0' || *r > '9' || ++nd > 3) { ok = false; break; }
        part[k] = part[k] * 10 + (unsigned)(*r - '0');
      }
      ok = ok && k == 2 && nd && part[0] < 256 && part[1] < 256 && part[2] < 256;
      a = part[0], b = part[1], v = part[2];
      if (ok && *r == ':') copy(name, r + 1, sizeof name);
    }
    bool empty = ok && !strcmp(id, "-");
    if (!ok || (!empty && !rmp_id_ok(id))) { sniprintf(out, cap, "usage: um=put:<1..5>:<id|->:<a.b.c>[:<name>]"); return true; }
    rmp_resolve_import(&g_map, &g_notes, s, empty ? "" : id, a << 16 | b << 8 | v, 0, name);
    g_fresh = false;
    g_need |= 1u << s;
    g_pub = g_mp = true;
    if (!map_save()) core::log("um: %s not written", g_map_file);
    int k = empty ? 0 : find_pkg(id);
    sniprintf(out, cap, empty ? "U%d empty" : k >= 0 ? "U%d = %s" : "U%d = %s (not installed: MISSING)", s + 1, id);
    return true;
  }
  if (strncmp(c, "um=assign:", 10) == 0 || strncmp(c, "um=clear:", 9) == 0) {
    bool clear = c[3] == 'c';
    const char* p = c + (clear ? 9 : 10);
    int s = *p - '1';
    if (s < 0 || s >= kSlots || (!clear && p[1] != ':')) { sniprintf(out, cap, "usage: um=assign:<1..5>:<id> | um=clear:<1..5>"); return true; }
    if (clear) {
      assign_id(s, "", 0);
      sniprintf(out, cap, "U%d cleared", s + 1);
      return true;
    }
    int k = find_pkg(p + 2);
    if (k < 0) { sniprintf(out, cap, "%s is not installed", p + 2); return true; }
    assign_id(s, g_pkg[k].id, g_pkg[k].version);
    sniprintf(out, cap, "U%d = %s", s + 1, g_pkg[k].id);
    return true;
  }
  return false;
}

int json_copy(char* out, int cap) {
  for (int tries = 0; tries < 8; ++tries) {
    uint32_t a = __atomic_load_n(&g_jseq, __ATOMIC_ACQUIRE);
    if (a == 0) return 0;
    int r = (int)((a / 2) & 1);
    int n = g_jlen[r];
    if (n >= cap) return 0;
    memcpy(out, g_js[r], (size_t)n);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_jseq, __ATOMIC_ACQUIRE) - a < 2) { out[n] = 0; return n; }   // not overwritten meanwhile
  }
  return 0;
}

bool ui_view(UiView* out) {
  for (int tries = 0; tries < 8; ++tries) {
    uint32_t a = __atomic_load_n(&g_jseq, __ATOMIC_ACQUIRE);
    if (a == 0) return false;
    memcpy(out, &g_ui[(a / 2) & 1], sizeof *out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&g_jseq, __ATOMIC_ACQUIRE) - a < 2) return true;
  }
  return false;
}
void publish_now() { g_pub = false; g_pub_ms = core::now_ms(); publish(); }
const char* dir() { return g_dir; }
uint32_t code_epoch() { return __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE); }
SlotView slot_view(int s) {
  if (s >= kSlots && s < kAll) {   // UFX1..UFX4: kSlots + u
    const V3& x = g_v3[s];
    return SlotView{g_slot[s].st, g_fxmap[s - kSlots].id, g_fxmap[s - kSlots].version, 0, "", 0, 0, 0, muted(s), x.p3 ? 3 : 0,
                    x.zero_len, g_why[s]};
  }
  rmp_desc d;
  const rmp_desc* p = slot_desc(s, &d);
  static char name[11];
  copy(name, p ? p->name : "", sizeof name);
  const V3& x = g_v3[s];
  int api = g_v3[s].p3 ? 3 : g_slot[s].p ? (int)g_slot[s].p->api : 0;
  return SlotView{g_slot[s].st, g_map.slot[s].id, g_map.slot[s].version, g_slot[s].fp, name, g_notes.slot[s].src,
                  g_notes.slot[s].flags, g_notes.slot[s].fp, muted(s), api, x.zero_len, g_why[s]};
}
int installed() { return g_ninst; }
uint8_t store_gen() { return g_meta.store_gen; }

}  // namespace um

extern "C" __attribute__((weak)) void* um_heap(uint32_t bytes) { return new uint8_t[bytes]; }
