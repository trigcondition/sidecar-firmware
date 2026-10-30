// The Teensy's platform layer for the engine core (core/, docs/core.md): the serial bridge and the USB host
// (usb_host.h) carry the Rytm's SysEx into core::engine_sysex and the core's blocks out (render_block ->
// core::render_voices); the machine plugins on U1..U5 (RytmPlugins) reach the voices through core::hooks; the
// watchdog, the device-info report and the stats line are the Teensy's own. Over USB serial the Mac plays the
// Rytm (sim/teensy_bridge.py relays rytm-emu's reports and audio requests); with the host cable the same engine
// sits behind the Overbridge host instead.
//
// Serial protocol (binary):
//   Mac -> Teensy  'S' <len u8> <SysEx F0..F7>     a report (sim/protocol.py)
//                  'N' <n u16 LE>                  render n frames
//                  'R'                             reboot
//                  'M' <status> <d1> <d2>          a MIDI channel message, as the Rytm's USB MIDI in the host
//                                                  build: with notes on, channel N's note-on/off plays voice N-1
//                  ':' <text> '\n'                 a console line, the Pi engine's syntax (both builds; plain
//                                                  text, so a serial monitor can type it in the host build):
//                                                  notes=1 / notes=0 (the stock Rytm's MIDI notes play voices,
//                                                  core/notes.h; off at boot unless -DRYTM_NOTES=1), notemach=V:NAME
//                                                  (voice V's machine: plaits juno daisy dsps grain fm), notes
//                                                  (the state); the host build's out=main / out=tracks (the
//                                                  voices summed on Overbridge MAIN L/R for a stock Rytm, faded
//                                                  in, limited at -12 dBFS, core/main_out.h / each voice on its
//                                                  track's channel for the mod build; tracks at boot unless
//                                                  -DRYTM_OUT_MAIN=1), out (the state)
//                  'Y' <n u16 LE> <payload> <crc32> the web console (webui.h, docs/webui-protocol.md): card files and
//                                                  plugin installs; not a development frame (the host log stays plain)
//   Teensy -> Mac  'A' <n u16 LE> <n x 8 x i16 LE> the voices, physical voice
//                                                  order 0..7 (host build with out=main: slot 0 the
//                                                  MAIN mix, the others 0)
//                  'L' <len u8> <text>             a log line
//                  'X' <len u8> <SysEx F0..F7>     for the Rytm (listings, the pool)
//
// Voice handling is sim/obhost.py's, so the two sound the same: P1 engine
// (17 curated models, eng_zone in core/machines.h), P2 harmonics, P3 timbre, P4 morph, P5 decay, P6 LPG
// colour, P7 tune (semitones from 64), P8 level; output x 0.7. DAISYSP voices:
// sim/daisysp/daisy_engine.h (ENG STR BRI DMP INP EXC TUN LEV). (0x40, DIGIDRUM's
// flag until 2026-10-29, is the built-in MD on the Pi and the Mac: not on the Teensy, silent here.) JUNO
// voices (flag 0x08): sim/juno/juno_voice.h - P1..P8 PAT CUT RES ENV ATK DEC
// REL LEN, the note the trig's; 0x11 NOTE_OFF releases it (LEN 127 holds until
// it), the other machines play out and ignore it. CUT..REL are the patch's
// sliders, absolute; when PAT picks another patch the Rytm gets its values
// (15) and the list's names go to it as 16 (sim/protocol.py). 6-OP FM voices
// (flag 0x48): Plaits' six-op engine on DX7 voices (fm_machine.h) - P1..P8
// PAT TIM MOR DEC LPG TUN LEV LEN, PAT an entry of the effective FM list (the
// project's, /.rytm/fm, else Plaits' 96 ROM voices; core/presets/dx7_lib.h); 17 sends its
// names, 18 asks for them. The machine is the voice byte & 0x78, matched
// exactly (0x00 PLAITS, 0x08 JUNO, 0x10 DAISYSP, 0x20 DSP SAMPLE, 0x48
// 6-OP FM; 0x40 MD, silent here).
// RYTM_PLUGINS 1 (default in both builds; -DRYTM_PLUGINS=0 leaves them out): machine plugins (.rmp, teensy/libraries/RytmPlugins,
// docs/plugins.md) on the user slots U1..U5, voice flags 0x50 0x58 0x60 0x68
// 0x70: the project's slot map /.rytm/slots/<PROJECT>.txt names the machine
// each slot plays (installed in the store in program flash); every call into a
// plugin goes through the guard (a fault, a timeout or three overruns of twice
// its declared CPU mute the slot). Development install over the serial link:
//   Mac -> Teensy  'I' <total u32 LE>               begin an install
//                  'J' <n u16 LE> <n bytes>         a chunk, n <= 256
//                  'K'                              commit: "plugins: installed <id>"
//                                                   or "plugins: install failed <status>"
//                  'U' <slot 1..5> <len> <id> <a b c>  assign (len 0: clear) in the
//                                                   project's map, save it, load that slot
//                  'D'                              device-info report: JSON as "info: i/n chunk"
//                                                   log lines (tools/rmp_install.py info)
//                  'W' <mode> <len> <path> <n u16 LE> <n bytes>  a file on the card under /PRESETS/ or
//                                                   /MACHINES/: mode 0 create, 1 append, 2 delete, 3 a
//                                                   preset scan fault (tests); n <= 200
//                  'T' <0|1>                        the transport stopped / running (the bridge relays
//                                                   MIDI Stop / Start, Continue): no store install while it runs
//                  'Z' <len> <id>                   remove a package from the store
//                  'E' <len> <id>                   export a package: to the card's /MACHINES/<id>-<a.b.c>.rmp
//                                                   and as "export:" hex log lines (tools/rmp_install.py export)
//                  'F' "WIPE"                       format the plugin store (every package lost): the only way past
//                                                   a "foreign store layout" at boot (tools/rmp_install.py wipe)
// (tools/rmp_install.py). Installing stalls the flash (a 64 KB erase holds interrupts off ~0.25-0.28 s): not during play.
// RYTM_USB_HOST 1: the Teensy is the Rytm's USB host (host header; Overbridge
// audio and USB MIDI, teensy/voiceloop/usb_host.h, docs/HOST-BRINGUP.md)
// instead of the serial bridge above; the log is plain text lines then, until
// a development tool sends a framed command ('P' ping answers "pong"), then
// framed until a reboot.
#ifndef RYTM_USB_HOST
#define RYTM_USB_HOST 0
#endif

#include <Arduino.h>
#if RYTM_USB_HOST
#include <RytmHost.h>
#include <USBHost_t36.h>
#endif
#include <DaisySP.h>
#include <Juno.h>
#include <Plaits.h>
#include <RytmCore.h>   // the engine core (core/, teensy/platform/; teensy/setup_core_lib.sh)
#include <version.h>    // SIDECAR_VERSION, SIDECAR_COMMIT (VERSION, tools/gen_version.sh; docs/versioning.md)
#include <main_out.h>   // core::MainOut: the host build's out=main
#include <ui/extra.h>   // the remote UI's input tap (SCOPE, the meters)
#include <rec/mini_rec.h>   // the recorder (rec_mini.h: the host build)
#if RYTM_USB_HOST
#include <fx/send_fx.h>   // the send FX's small set (sendfx_host.h)
#include <new>
#endif

#define RYTM_PSRAM 1   // the sample RAM is the core's (core/pool/sample_ram_pool.h), always built: off without PSRAM

#ifndef RYTM_PLUGINS
// On in both builds (2026-10-10): the USB-host build is the final setup and plays user machines.
// It fits RAM1 with teensy/cold_ld.sh (LittleFS's core and SdFat in flash) and the per-message
// parsers (report, poll_input, host_poll, loop) in FLASHMEM: ITCM 157752 B in 5 blocks, 45344 B
// for locals (measured). -DRYTM_PLUGINS=0 builds without them (U1..U5 silent).
#define RYTM_PLUGINS 1
#endif
#if RYTM_PLUGINS
#include <rmp_api.h>
#include <rmp_arena.h>
#include <rmp_arenas.h>
#include <rmp_guard.h>
#include <rmp_slots.h>
#include <rmp_resolve.h>
#include <rmp_state.h>
#include <rmp_store.h>
#include <rmp_manifest.h>
#include <rmp_meta.h>
#include <rmp_pool.h>
#include <rmp_presets.h>
#include <rmp_proto.h>
// The RTWDOG's timeout, armed at the end of setup() (the boot's card work, an
// index rebuild included, runs before it). loop()'s longest passes, measured
// 2026-10-10: a slot load pass 3.7 ms at most (the store's file open; a load is
// split into passes, one open a package), a store install 10-14 ms (the flash
// stall), the device-info report's passes 3.7 ms, an 'N' of 1024 frames on 8
// sine_test voices 3.1 ms; project_load (5-slot pool) 2.7 ms. 2 s leaves room for large pools and slow cards.
#ifndef RYTM_WDOG_MS
#define RYTM_WDOG_MS 2000
#endif
#ifndef RYTM_LOOP_WARN_US   // a loop() pass longer than this is logged (each new longest)
#define RYTM_LOOP_WARN_US (RYTM_WDOG_MS * 250u)
#endif
#ifndef RYTM_DEBUG_TOOLS     // 1: the developer's benches (':sendfxbench', ':cap=MS'); the release build has 0 (host_stereo.sh)
#define RYTM_DEBUG_TOOLS 1
#endif
#ifndef RYTM_STACK_DEBUG     // 1: the stack's high-water mark and the RAM2 heap logged
#define RYTM_STACK_DEBUG 0
#endif
#if RYTM_PLUGINS
extern "C" {
extern uint8_t external_psram_size;       // MB, found by the core at boot
extern char _itcm_block_count;           // the linker's count of 32 KB ITCM blocks (RAM1: the rest is locals/stack)
extern unsigned long _ebss, _estack;
}
#endif
#if RYTM_STACK_DEBUG
#include <malloc.h>
extern "C" {
extern unsigned long _heap_end;   // startup.c: the RAM2 heap's end and its break
extern char* __brkval;
extern unsigned long _ebss, _estack;   // RAM1: the stack's room is between them
}
#endif
#endif

#ifndef RYTM_WEBUI
#define RYTM_WEBUI 1   // the web console's commands (webui.h)
#endif
#if RYTM_WEBUI
extern "C" {   // the memory map (webui.h web_mem): the linker's symbols and the core's heap break
extern unsigned long _stext, _etext, _sdata, _ebss, _heap_start, _heap_end, _flashimagelen, _estack;
extern char _itcm_block_count;
extern char* __brkval;
extern uint8_t external_psram_size;
}
#endif

namespace {

using namespace core;   // the engine core: its names as voiceloop's own (docs/core.md)

#define RYTM_CORE_VERSION "0.1.0"   // the Teensy core firmware (device-info report); bump on a firmware release

// Rendered frames waiting to go out, per voice (a request larger than this is
// rendered and sent in pieces: rytm-emu asks for up to 512 at a time). The host build renders only what the
// Overbridge FIFO lacks (host_feed: host_target, 96 frames, at most RytmOverbridge::kFifo - 32) and its serial
// requests come in pieces too (send_frames): 256 frames (4 KB of RAM1) there, 12 KB less than the serial build's.
#ifndef RYTM_RING_FRAMES
#if RYTM_USB_HOST
#define RYTM_RING_FRAMES 256
#else
#define RYTM_RING_FRAMES 1024
#endif
#endif
const int kRing = RYTM_RING_FRAMES;
int16_t ring[kRing][kVoices];
int ring_rd = 0, ring_n = 0;

uint64_t cycles = 0, frames_done = 0;
uint32_t last_report = 0;
uint32_t peak_block = 0;
// diagnostics for the cpu log's second line (sim/stress.py reads both)
int tenths(float x) { return (int)(x * 10.0f + 0.5f); }
uint64_t mach_cyc[kMachines];
uint32_t max_request_us = 0, max_report_us = 0;
uint32_t max_loop_us = 0, loop_t = 0;   // the longest pass of loop(): how late input can be read

// The USB-host build logs plain text lines (a serial monitor) until a framed
// command arrives on the serial port ('S' 'N' 'I' 'J' 'K' 'U' 'D' 'P' 'W' 'T' 'Z' 'E' 'F': the
// development tools, tools/rmp_install.py --host); from then on, until a reboot,
// it speaks the serial build's framing ('L' log lines, 'X' copies of what goes
// to the Rytm). A frame letter added later joins the set with its parser.
#if RYTM_USB_HOST
bool log_framed = false;
#else
const bool log_framed = true;
#endif

// A log line, plain (a serial monitor) or framed ('L'): the core's log (core::log) ends here too.
void log_line(const char* b, int n) {
#if RYTM_USB_HOST
  // No program has the port open (DTR off: pyserial, the web console and serial monitors set it): drop the line.
  // Written anyway, the USB serial buffers fill and the next write waits 120 ms (usb_serial.c's timeout) - loop()
  // stalls and the Rytm's Overbridge FIFO runs dry (measured: ~170 ms of xruns after each re-enumeration).
  if (!log_framed && !Serial.dtr()) return;
#endif
  if (!log_framed) {
    Serial.write((const uint8_t*)b, n);
    Serial.write('\n');
    return;
  }
  Serial.write('L');
  Serial.write((uint8_t)n);
  Serial.write((const uint8_t*)b, n);
}

// A log line. Integers only (vsniprintf, newlib's integer printf): with no
// float formatting anywhere, the full printf (12 KB with its dtoa) stays out
// of ITCM.
void logf(const char* fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsniprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if (n > 199) n = 199;
  log_line(b, n);
}

// The pool ("static" samples, as the Octatrack's), one per Rytm project: files
// added with LOAD TO PROJ (0F <folder> <file>) or ASSIGN (12 <slot> <folder>
// <file>) on the TEENSY folder in the Rytm's browser (rytm-mods 0023) while
// that project is the Rytm's (10 <name>, sent before each and every few
// seconds). Kept on the card as text, /.rytm/pools/<PROJECT>.txt: slot N
// (1..127) is line N-1, the file's path under /Samples
// ("Drums/Hits/dcm8_002.wav"), an empty line for an empty slot - slots keep
// their numbers, and paths survive an index rebuild. A path not on the card
// any more stays in its slot (silent) until the file is back. The last
// project's name is kept in /.rytm/project, so the Teensy starts with its pool.
// DSP SAMPLE's SMP picks slot SMP (0 off); the Rytm gets the table as 14.
// The card, the pool and the project (state, pool file, LOAD TO PROJ / ASSIGN, 14/16/17, the 0D/0E listing):
// core/pool/pool.h. Their plugin side: hk_project_changed, hk_pool_changed.
#if RYTM_PLUGINS
// Each voice's pool sample for its plugin (API 2's pool_info/pool_read, without PSRAM or before the
// sample is resident): rmp_pool's head/ring reader on the voice's own DSP SAMPLE buffers (stream_heads,
// stream_rings_a/b; the plugin owns them while the voice plays its slot), prepared and refilled from
// loop() (plug_pool_pass). Cleared when the voice leaves its slot or the pool changes.
rmp_pool_reader plug_pool[8];
#endif

#if RYTM_PLUGINS
bool plug_state_reloaded = false;   // a state-pool-full reload ran since the map/project last changed (at most one)
bool plug_reload_pending = false;   // the project's slot map, loaded from loop() (plug_reload)
#endif

#if RYTM_USB_HOST
void host_to_rytm(const uint8_t* m, int n);   // usb_host.h
#endif
FLASHMEM void teensy_to_rytm(const uint8_t* m, int n) {
#if RYTM_USB_HOST
  host_to_rytm(m, n);
  if (!log_framed) return;   // the development link also sees what the Rytm gets
#endif
  Serial.write('X');
  Serial.write((uint8_t)n);
  Serial.write(m, n);
}

#if RYTM_PLUGINS
// ---------------------------------------------------------------------------
// Machine plugins on the user slots U1..U5 (voice flags 0x50..0x70). The
// project's slot map is loaded between requests (plug_reload, from loop());
// a voice's state lives in its own engine memory (shared[vi], 16 KB; Plaits is
// initialised again when the voice returns to it) or, for a larger state and
// only with PSRAM, in one rmp_state block per voice, until the next project. Every
// call into a plugin goes through rmp_guard_call; a fault or a timeout mutes
// the slot - and every slot sharing its image - until the next reload, as do
// three renders in a row over twice the plugin's declared CPU.
rmp_slot_map plug_map;
// Which machine a slot plays when a project loads (rmp_resolve.h; docs/user-machines-pi.md, "Which machine a slot
// plays"): the notes next to the map (/.rytm/slots/<PROJECT>.inf), the Rytm's 0x25 fingerprints, the previous
// project's map (a fresh project inherits it), lazily a trig's tag (an older Rytm).
rmp_slot_notes plug_notes;
rmp_slot_map plug_prev;
bool plug_proj_new = false;     // the next plug_reload is a project change (plug_prev, the prints state reset)
bool plug_fresh = false;        // no map and no notes: inherits once the prints are in (or at plug_fresh_until)
uint32_t plug_fresh_until = 0;
bool plug_prints_rytm = false;  // a 0x25 arrived since boot
bool plug_prints_have = false;  // one was used for this project (no lazy resolve then)
bool plug_prints_pending = false;
uint32_t plug_prints_hash = 0;
rmp_prints plug_prints;
uint16_t plug_lazy_req[RMP_SLOTS], plug_lazy_fp[RMP_SLOTS];   // a trig's tag to resolve from, the last one tried
uint8_t plug_res_gen = 0;       // the store generation the last resolve saw (an install: the open slots again)
rmp_loaded plug[RMP_SLOTS];
uint32_t plug_bytes[RMP_SLOTS];     // state_bytes and cpu_permille, copied at load (the
uint32_t plug_permille[RMP_SLOTS];  // header in the arena is plugin-writable memory)
void* plug_state[RMP_VOICES][RMP_SLOTS];   // per voice and slot: shared[vi] or the voice's PSRAM block; only plug_cur's is live
void* plug_vblk[RMP_VOICES];               // the voice's PSRAM state block (state > 16 KB, with PSRAM only)
uint32_t plug_vblk_cap[RMP_VOICES];        // its size; a slot needing more gets a new block, the old one is stale until the next reload
uint32_t plug_state_stale = 0;             // bytes of stale PSRAM blocks
int8_t plug_cur[RMP_VOICES];               // the slot whose voice_init the voice's state had; -1 none
uint8_t plug_overruns[RMP_SLOTS];
int8_t plug_place[RMP_SLOTS];              // the slot's image: RMP_PLACE_INTERNAL (the DTCM arena) or RMP_PLACE_PSRAM (rmp_arenas.h)
bool plug_trig_said[RMP_SLOTS];            // a trig on a slot that does not play: logged once a reload
uint32_t plug_max_cyc = 0, plug_max_limit = 0;   // the longest render (cycles) in the cpu log's period, its limit
bool plug_off = false;                     // safe mode: no user machines this boot
bool plug_installing = false;              // an install ('I') is open
rmp_load_job plug_job;                     // the incremental load under way (plug_job_pass)
bool plug_job_on = false;
int plug_job_slot = 0;
uint32_t plug_todo = 0;                    // slots waiting for an incremental load (bit s)
uint32_t plug_job_t = 0, plug_job_max = 0;
int plug_job_passes = 0;
rmp_loaded plug_act_res;
int plug_act_slot = -1;                    // a slot loaded, its init() is the next pass
uint32_t plug_reinit_max = 0;
uint64_t plug_cyc_sum[RMP_SLOTS];          // render cycles per 12-sample block call since the slot loaded (device-info report)
uint32_t plug_cyc_n[RMP_SLOTS], plug_cyc_peak[RMP_SLOTS];
bool plug_dirty[RMP_VOICES];               // the voice's shared[] holds plugin state: Plaits must be re-initialised before it renders
bool plug_arena_dirty = false;             // an incremental load has run since the arena was reset (it may hold stale images)
const uint32_t kPlugStepBytes = 8192;      // CRC bytes read in one loop pass (~0.3 ms with the table CRC)
uint32_t plug_wdog_warn_us = RYTM_LOOP_WARN_US;
const uint32_t kPlugMaxPermille = 500;     // a plugin declaring more than half the Teensy for a voice: BAD
// ---- user slots on the Rytm (rytm-mods 0021's machines 35..39 as U1..U5; sim/protocol.py 19..1E) ----
DMAMEM rmp_desc plug_desc[RMP_SLOTS];      // each slot's manifest (rmp_manifest), or a name from its id
bool plug_desc_ok[RMP_SLOTS];              // plug_desc[s] is the slot's current id's
uint32_t plug_desc_todo = 0;               // slots whose description is to be read (bit s), one a loop pass
rmp_meta plug_meta;                        // generations; a 1D due
// A Rytm that started after the Teensy has seen no 1D (us.seen 0: its slot menu sends no 1E, it re-asks every
// slot's 1A): a 10 or a 19 what 3 (the slot menu's installed list) makes a 1D owed, and any other 19 too when
// the last 1D is 3 s old; an owed 1D goes out at most once a second (any 1D pays it), from plug_1d_pass ahead
// of the reload, job and save passes. Spec 2.1.
uint32_t plug_1d_ms = 0;                   // the last 1D (plug_ms)
bool plug_1d_owed = false;
uint32_t plug_meta_t = 0;                  // the fingerprints' last refresh (ms)
uint32_t plug_ask_todo = 0;                // slots whose 1A (all parts) is to be sent (bit s)
uint16_t plug_enum_todo[RMP_SLOTS];        // enum name lists asked for (19 what 1): bit k = P(k+1)
// ---- presets (rmp_presets.h's glue): /PRESETS/<id>/ (generic .txt, native preset_ext), scanned from loop() ----
DMAMEM rmp_preset_names plug_pnames;       // ONE for all slots: the shown names of the slot scanned last (1B 0x20)
DMAMEM rmp_preset_index plug_pix[RMP_SLOTS];   // per slot: each entry's folder index and kind, a generation
DMAMEM rmp_knobs plug_knobs[RMP_SLOTS];    // per slot: roles and ranges (from its description)
DMAMEM rmp_pv plug_pv[RMP_VOICES];         // per voice: its preset state
DMAMEM alignas(32) uint8_t plug_rbuf[4096];            // a preset file being read (a generic one is cut there, a native one refused)
// ---- the Rytm's slot menu: the installed list (1B 0x21), 1E assign / clear / install from the card ----
// (Its log formats are PSTR, in flash: the serial build's DTCM data ended within bytes of an 8 KB step of
// the arena's alignment - a few hundred bytes more there cost 8 KB of stack.)
struct PlugInst { char id[32]; uint32_t version, size; char name[11]; bool renamed; };   // renamed: a duplicate's "~2"
DMAMEM PlugInst plug_inst[32];             // the installed packages for the Rytm's slot menu (1B 0x21), store order
int plug_inst_n = 0;
int plug_inst_over = 0;                    // packages past the 32 listed
uint8_t plug_inst_gen = 0;                 // the store generation the list is for (0: none yet)
int plug_inst_phase = 0;                   // building it: 0 idle (the next pass lists the store), 2 reading entry plug_inst_i
int plug_inst_i = 0;
bool plug_inst_asked = false;              // a 19 what 3 waits for the list
rmp_assign plug_1e_q[RMP_SLOTS];           // a 1E per slot, run from loop()
bool plug_1e_on[RMP_SLOTS];
uint32_t plug_1e_t[RMP_SLOTS];             // when it arrived (plug_ms): the Rytm waits 2 s for the answer
int8_t plug_ans[RMP_SLOTS];                // a 1E waiting for the slot to settle: its op, -1 none
DMAMEM rmp_slot_entry plug_ans_want[RMP_SLOTS];   // what the slot is to hold then
uint32_t plug_ans_t[RMP_SLOTS];            // (plug_ms)
// An answer not settled by then: LOADING (or BAD: the map did not change). 1 s, not nearer the Rytm's 2 s: the
// answer still has to go out after it, and a flash erase (an install's) can hold the loop ~0.27 s.
const uint32_t kPlugAnsMs = 1000;
// Milliseconds from the cycle counter (DWT CYCCNT, enabled by the core and in setup()) for the 1E deadlines: a
// flash erase holds interrupts off ~0.25-0.28 s a 64 KB block (measured; the core's eeprom.c), and millis()/micros() lose that
// time. CYCCNT wraps every 7.16 s at 600 MHz: each call adds the cycles since the last one, so it must run more
// often than that. loop() calls it every pass; a pass may be longer than the watchdog's 2 s now that the watchdog
// is fed inside long operations (every flash erase, every 1 KB of an export), so those call it too: before every
// erase (rmp_store_on_erase, below) and every 1 KB of plug_export - no wrap goes unseen.
uint32_t plug_ms_now = 0, plug_ms_cyc = 0, plug_ms_rem = 0;
FLASHMEM uint32_t plug_ms() {
  uint32_t c = ARM_DWT_CYCCNT, d = c - plug_ms_cyc, per = F_CPU_ACTUAL / 1000u;
  plug_ms_cyc = c;
  plug_ms_now += d / per;
  plug_ms_rem += d % per;
  if (plug_ms_rem >= per) plug_ms_rem -= per, ++plug_ms_now;
  return plug_ms_now;
}
}  // namespace (rmp_store_on_erase overrides rmp_store.cpp's weak one: outside the anonymous namespace)
// Before every flash erase of the store (rmp_store.cpp, after it feeds the watchdog): one LittleFS operation may
// erase several 64 KB blocks, ~0.25-0.28 s each.
FLASHMEM void rmp_store_on_erase() { plug_ms(); }
namespace {
// Cycles to microseconds, for the install timings (CYCCNT: they span flash erases; under 7.16 s each).
FLASHMEM uint32_t plug_cyc_us(uint32_t cyc) { return cyc / (F_CPU_ACTUAL / 1000000u); }
bool plug_on_card[RMP_SLOTS];              // a MISSING slot's package is on the card (/MACHINES/<id>-<a.b.c>.rmp)
uint32_t plug_card_todo = 0;               // MISSING slots to look up on the card (bit s)
struct PlugCardInst { bool on; int s; uint32_t size, done; uint32_t ms0, passes, cyc_max; };   // ms0 (plug_ms), cyc_max: its timing
PlugCardInst plug_ci;                      // a card -> flash copy (1E op 2), 2 KB a pass
DMAMEM FsFile plug_ci_f;
bool plug_transport = false;               // MIDI Start/Continue seen and no Stop since (host: the Rytm's; serial: 'T')
uint32_t plug_trig_ms = 0;                 // the last trig report (0x04)
// Playing: a flash erase would stall the Teensy ~0.25-0.28 s (docs/plugins.md 4), so no install. A Rytm with
// TRANSPORT SEND off still counts as playing for 1 s after any trig.
FLASHMEM bool plug_transport_running() { return plug_transport || (plug_trig_ms && millis() - plug_trig_ms < 1000); }
FLASHMEM void plug_store_changed(const char* id);
FLASHMEM bool plug_assign(int slot, const uint8_t* id, int len, const uint8_t* ver);
FLASHMEM void plug_desc_listname(int s, bool reread);
FLASHMEM bool plug_saving();               // a slot map save is under way (plug_sv, below)
FLASHMEM void plug_save_for_1e();          // the save just queued is a 1E's (plug_sv.ans)
struct PlugPreLoad { bool on; int8_t s; uint8_t gen; int16_t entry; bool sync; float p[16]; };
// Per voice, run from loop(): [0] the sound's own preset (written back), then [1] a preset played without
// write-back (a p-lock). A newer decision of the same kind replaces one; a play never replaces a write-back.
DMAMEM PlugPreLoad plug_pl[RMP_VOICES][2];
struct PlugNote { uint8_t note, vel; bool held; uint32_t t; };
PlugNote plug_note[RMP_VOICES];            // a trig's note_on held until its preset load ran (bit vi of plug_note_wait)
uint8_t plug_note_wait = 0;                // the voice renders silence meanwhile
uint32_t plug_note_lat_max = 0;            // the longest such hold (us)
const uint32_t kPlugNoteHoldUs = 2000;     // a held note plays by then, loaded or not (a save / job / scan in the way)
uint8_t plug_pv_resync = 0;                // voices whose pending write-back the list change dropped: decided again as a non-trig
uint8_t plug_scan_err = 0;                 // slots whose last scan hit a card error (bit s): no rescan until
uint32_t plug_scan_err_t[RMP_SLOTS];       // kPlugScanRetryMs later, a description read or a 'W' to the folder
const uint32_t kPlugScanRetryMs = 5000;
uint8_t plug_scan_fault = 0;               // 'W' mode 3 (tests): the slot's next scan fails as a card error
uint32_t plug_pre_fp[RMP_SLOTS];           // the slot's last finished scan's list fingerprint (in its generation)
// The preset folder of the slot loaded from last, kept open: a load opens its file by index without
// walking the card's root again (~0.5 ms, against ~3 ms once stream reads evicted SdFat's cache). Closed
// whenever the slot's list is invalidated (description read, 'W' to the folder, a scan's card error).
DMAMEM FsFile plug_load_dir;
int plug_load_dir_slot = -1;
// Each slot's small preset files (<= 4096 B and within one cluster), noted by its scan: a load reads their
// sectors raw - an SdFat open by index re-reads the folder's directory sector, measured ~2.5 ms on the
// test card against ~0.1 ms for a file's own sectors. Valid while the slot's index is (a 'W' rescans).
struct PlugPreRec { uint16_t dir, len; uint32_t sector; };
DMAMEM PlugPreRec plug_rec[RMP_SLOTS][RMP_PRESETS_MAX];
uint8_t plug_rec_n[RMP_SLOTS];
FLASHMEM int plug_rec_find(int s, uint16_t dir) {
  for (int i = 0; i < plug_rec_n[s]; ++i)
    if (plug_rec[s][i].dir == dir) return i;
  return -1;
}
uint32_t plug_scan_todo = 0;               // slots whose preset folder is to be scanned (bit s)
int plug_scan_slot = -1;                   // the slot being scanned, 16 entries a pass
FsFile plug_scan_dir;
uint8_t plug_pre_page[RMP_SLOTS];          // preset name pages asked for (19 what 2): bit p
uint8_t plug_pv_new = 0;                   // voices that just became a slot's machine (bit vi): first = true at their next decide
uint8_t plug_pv_wait = 0;                  // voices whose report found the slot's index stale: decided once it is scanned
uint8_t plug_last_trig = 0;                // voices whose last report was a trig (0x04)
uint8_t plug_pre_has[RMP_SLOTS];           // the slot's last finished scan found presets (1A's flag)
// The sound tag (spec 2.6): a saved Rytm sound carries the fingerprint of the plugin it was made for (0x04 / 0x03's
// last two bytes, 0 untagged). plug_fp[s] is slot s's rmp_plugin_fp, set when it becomes READY (read only while it
// is READY: 1A part 0 and the compare). Bit vi of plug_mis: the voice's last 0x04 carried a tag for another plugin -
// the voice was released and stays silent, its 0x03s dropped, until a 0x04 that matches (or is untagged); cleared
// in plug_leave (another slot, or none) and for the slot's voices when the slot loads or becomes READY.
uint16_t plug_fp[RMP_SLOTS];
uint8_t plug_mis = 0;
FLASHMEM void plug_leave(int vi);
FLASHMEM void plug_voice_start(int vi);
FLASHMEM void plug_pv_decide(int vi, int s, bool is_trig, const float* p);
FLASHMEM void plug_note_fire(int vi);
FLASHMEM void plug_pv_params(int vi, int s, bool is_trig, float* q);
#if RYTM_STACK_DEBUG
// The stack's high-water mark: its free room is painted at the start of
// setup() and checked every 2 s.
const uint32_t kStackPaint = 0xA5A5A5A5u;
uint32_t plug_stack_seen = 0;
// The core's MPU traps the 32 bytes at _ebss (a stack overflow): start above them.
uint32_t* plug_stack_floor() { return (uint32_t*)((((uint32_t)&_ebss) + 63u) & ~31u); }
FLASHMEM void plug_stack_paint() {
  uint32_t sp;
  asm volatile("mov %0, sp" : "=r"(sp));
  for (uint32_t* p = plug_stack_floor(); (uint32_t)p < sp - 256; ++p) *p = kStackPaint;
}
uint32_t plug_stack_used() {
  const uint32_t* p = plug_stack_floor();
  while ((uint32_t)p < (uint32_t)&_estack && *p == kStackPaint) ++p;
  return (uint32_t)&_estack - (uint32_t)p;
}
uint32_t plug_stack_room() { return (uint32_t)&_estack - (uint32_t)plug_stack_floor(); }
// The RAM2 heap's free bytes: above the break, and freed blocks below it.
uint32_t plug_heap_free() { return (uint32_t)((char*)&_heap_end - __brkval) + mallinfo().fordblks; }
#endif
void plug_log(const char* m) { logf(PSTR("plugin: %s"), m); }
void plug_slots_log(const char* m) { logf(PSTR("plugins: %s"), m); }
uint32_t plug_random(uint32_t* s) {
  uint32_t x = *s ? *s : 2463534242u;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *s = x;
}
// One host table per plugin API: an API-1 plugin checks h->api == 1 and never reads reserved[];
// an API-2 plugin finds pool_info/pool_read in reserved[0..1] (h_pool_info/h_pool_read, set in plug_setup).
const rmp_host plug_host_v1 = {1u, 48000.0f, kBlock, plug_log, plug_random, {0}};
rmp_host plug_host_v2 = {RMP_API, 48000.0f, kBlock, plug_log, plug_random, {0}};
const rmp_host* plug_host_for(const rmp_plugin* p) { return p->api >= 2u ? &plug_host_v2 : &plug_host_v1; }
const rmp_platform plug_pf = {rmp_store_size, rmp_store_read_at, rmp_arena_alloc, rmp_arena_reset,
                              rmp_arena_sync, plug_slots_log, &plug_host_v2};

// The guarded calls: captureless, an argument block each.
struct PlugCall {
  const rmp_plugin* p;
  void* st;
  float* out;
  const float* prm;
  float note, vel;
  int32_t ret;
  const uint8_t* bytes;   // load_entry: a native preset file
  int32_t len;
  int16_t* vals;          // load_entry's values
};
void pc_init(void* a) { PlugCall* c = (PlugCall*)a; c->ret = c->p->init(plug_host_for(c->p)); }
void pc_voice_init(void* a) { PlugCall* c = (PlugCall*)a; c->p->voice_init(c->st); }
void pc_params(void* a) { PlugCall* c = (PlugCall*)a; c->p->params(c->st, c->prm); }
void pc_note_on(void* a) { PlugCall* c = (PlugCall*)a; c->p->note_on(c->st, c->note, c->vel); }
void pc_note_off(void* a) { PlugCall* c = (PlugCall*)a; c->p->note_off(c->st); }
void pc_render(void* a) { PlugCall* c = (PlugCall*)a; c->p->render(c->st, c->out, kBlock); }
// voice_enter / voice_leave / load_entry run from the report path or loop(), never from render: flash.
FLASHMEM void pc_voice_enter(void* a) { PlugCall* c = (PlugCall*)a; rmp_voice_enter_of(c->p)(c->st); }
FLASHMEM void pc_voice_leave(void* a) { PlugCall* c = (PlugCall*)a; rmp_voice_leave_of(c->p)(c->st); }
FLASHMEM void pc_load_entry(void* a) { PlugCall* c = (PlugCall*)a; c->ret = rmp_load_entry_of(c->p)(c->st, c->bytes, c->len, c->vals); }
const uint32_t kPlugInitCycles = 60000000;   // 100 ms
const uint32_t kPlugCallCycles = 300000;     // 0.5 ms: voice_init, params, note_on, note_off

// A slot muted during a render (inside send_frames' audio frame, where a log
// line would break the stream) is logged from loop(): plug_flush_log.
const char* plug_why[RMP_SLOTS];
// Why a slot is not READY, kept for the Rytm's name (1A: "!CRSH NPPR", rmp_why_name) and the web page /
// info (in words): a MUTED slot's reason (RMP_WHY_CRASH or RMP_WHY_TIME) and text, set when it is
// muted; a BAD slot's text when plug_activate refused it (else the package itself is bad). Cleared
// when the slot is (re)loaded (plug_todo_slot).
uint8_t plug_mute_why[RMP_SLOTS];
const char* plug_mute_txt[RMP_SLOTS];
char plug_bad_txt[RMP_SLOTS][48];
// Slot s and every slot sharing its image (the same plugin: its globals are
// suspect after a fault or an abandoned call) stop playing.
void plug_mute(int s, const char* why) {
  const rmp_plugin* p = plug[s].plugin;
  uint8_t w = (!strcmp(why, "timed out") || !strcmp(why, "over budget")) ? RMP_WHY_TIME : RMP_WHY_CRASH;
  for (int t = 0; t < RMP_SLOTS; ++t)
    if (t == s || (p && plug[t].plugin == p && plug[t].state == RMP_SLOT_READY)) {
      plug[t].state = RMP_SLOT_MUTED;
      plug_why[t] = t == s ? why : "shares the image -";
      plug_mute_why[t] = w;
      plug_mute_txt[t] = why;
    }
}
void plug_flush_log() {
  for (int s = 0; s < RMP_SLOTS; ++s)
    if (plug_why[s]) {
      logf(PSTR("plugins: U%d %s %s - muted"), s + 1, plug_map.slot[s].id, plug_why[s]);
      plug_why[s] = nullptr;
    }
}

// fn through the guard for slot s; a fault or a timeout mutes the slot. True if it returned.
// A PSRAM image's limit is longer (rmp_limit_hard: cold caches); the internal arena's is as given.
bool plug_call(int s, void (*fn)(void*), PlugCall* c, uint32_t limit) {
  if (plug_place[s] == RMP_PLACE_PSRAM) limit = rmp_limit_hard(limit, RMP_PLACE_PSRAM);
  rmp_guard_result g = rmp_guard_call(fn, c, limit);
  if (g == RMP_GUARD_OK) return true;
  plug_mute(s, g == RMP_GUARD_FAULT ? "CRASHED" : g == RMP_GUARD_TIMEOUT ? "timed out" : "guard busy");
  return false;
}
// The voice the host is calling a plugin for (pool_info / pool_read answer for it); -1 none (init, voice_leave).
int plug_vnow = -1;
// plug_call for one voice's state.
bool plug_vcall(int s, int vi, void (*fn)(void*), PlugCall* c, uint32_t limit) {
  plug_vnow = vi;
  bool ok = plug_call(s, fn, c, limit);
  plug_vnow = -1;
  return ok;
}

// The slot map's file: the pool's name rule (pool_file), under /.rytm/slots;
// tmp: its .tmp sibling (written first, then renamed over it).
FLASHMEM void plug_file(char* out, int max, bool tmp = false) {
  char fn[48];
  pool_file(fn, sizeof fn);   // "/.rytm/pools/<name>.txt"
  sniprintf(out, max, "/.rytm/slots/%s%s", fn + 13, tmp ? ".tmp" : "");
}

FLASHMEM void plug_notes_file(char* out, int max) {   // /.rytm/slots/<name>.inf
  plug_file(out, max);
  int l = (int)strlen(out);
  if (l > 4) memcpy(out + l - 4, ".inf", 4);
}
// Read and parse a slot map file; false if it is missing, unreadable or not a map.
FLASHMEM bool plug_read_map(const char* fn, rmp_slot_map* m, bool* exists) {
  memset(m, 0, sizeof *m);
  FsFile f;
  *exists = card_ok && f.open(fn, O_RDONLY);
  if (!*exists) return false;
  char b[512];
  int n = f.read(b, sizeof b);
  bool whole = f.available() == 0;
  f.close();
  if (n == 0) return true;   // an empty map
  return n > 0 && whole && rmp_slots_parse(b, n, m);
}

// A slot that was just loaded: its state size and CPU copied out of the arena, init() once
// per image through the guard (not again for a slot sharing an image that is READY already),
// then the "ready" line. incr: any other READY slot with the same image counts as already
// initialised (an incremental load); else only an earlier one (the full load's order).
FLASHMEM void plug_activate(int s, bool incr) {
  if (!plug_map.slot[s].id[0]) return;
  if (plug[s].state != RMP_SLOT_READY) {
    logf(PSTR("plugins: U%d %s state %d"), s + 1, plug_map.slot[s].id, (int)plug[s].state);
    return;
  }
  plug_place[s] = (int8_t)rmp_arena_where(plug[s].plugin);
  plug_bytes[s] = plug[s].plugin->state_bytes;
  plug_permille[s] = plug[s].plugin->cpu_permille;
  if (plug_bytes[s] > sizeof shared[0] && plug_bytes[s] > rmp_state_capacity()) {   // no PSRAM (or too big for it): only the voice's own memory
    plug[s].state = RMP_SLOT_BAD;
    sniprintf(plug_bad_txt[s], sizeof plug_bad_txt[s], "needs %lu B state a voice, the voice has %lu", (unsigned long)plug_bytes[s],
             (unsigned long)(rmp_state_capacity() > sizeof shared[0] ? rmp_state_capacity() : sizeof shared[0]));
    logf(PSTR("plugins: U%d %s needs %lu B state, the voice has %lu%s"), s + 1, plug_map.slot[s].id,
         (unsigned long)plug_bytes[s], (unsigned long)(rmp_state_capacity() > sizeof shared[0] ? rmp_state_capacity() : sizeof shared[0]),
         rmp_state_capacity() ? " (PSRAM pool)" : "");
    return;
  }
  int t = 0;   // another slot with the same image: init ran once
  for (; t < (incr ? RMP_SLOTS : s); ++t)
    if (t != s && plug[t].plugin == plug[s].plugin && plug[t].state == RMP_SLOT_READY) break;
  uint32_t init_us = 0;
  if (t < (incr ? RMP_SLOTS : s)) {
    plug_bytes[s] = plug_bytes[t];
    plug_permille[s] = plug_permille[t];
  } else if (plug_permille[s] > kPlugMaxPermille) {
    plug[s].state = RMP_SLOT_BAD;
    sniprintf(plug_bad_txt[s], sizeof plug_bad_txt[s], "declares %lu permille CPU a voice (at most %lu)", (unsigned long)plug_permille[s],
             (unsigned long)kPlugMaxPermille);
    logf(PSTR("plugins: U%d %s declares %lu permille a voice (at most %lu): not played"), s + 1, plug_map.slot[s].id,
         (unsigned long)plug_permille[s], (unsigned long)kPlugMaxPermille);
    return;
  } else {
    PlugCall c = {plug[s].plugin, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr};
    uint32_t ti = micros();
    rmp_guard_result g = rmp_guard_call(pc_init, &c, rmp_limit_hard(kPlugInitCycles, plug_place[s]));
    init_us = micros() - ti;
    if (g != RMP_GUARD_OK || c.ret != 0) {
      plug[s].state = RMP_SLOT_BAD;
      sniprintf(plug_bad_txt[s], sizeof plug_bad_txt[s], "init() %s",
               g == RMP_GUARD_FAULT ? "crashed" : g == RMP_GUARD_TIMEOUT ? "timed out" : "refused");
      logf(PSTR("plugins: U%d %s init %s"), s + 1, plug_map.slot[s].id,
           g == RMP_GUARD_FAULT ? "CRASHED" : g == RMP_GUARD_TIMEOUT ? "timed out" : g != RMP_GUARD_OK ? "refused (guard busy)" : "refused");
      return;
    }
  }
  logf(PSTR("plugins: U%d %s ready (%lu B a voice, cpu %lu permille, init %lu us%s)"), s + 1, plug_map.slot[s].id,
       (unsigned long)plug_bytes[s], (unsigned long)plug_permille[s], (unsigned long)init_us,
       plug_place[s] == RMP_PLACE_PSRAM ? PSTR(", code in PSRAM") : PSTR(""));
}

// The project's slot map: read it (or a .tmp a write left behind), load its
// machines into a fresh arena, init each through the guard. Voices' plugin
// state goes (rmp_state_reset).
FLASHMEM void plug_save_finish();
FLASHMEM void plug_todo_slot(int s);
FLASHMEM void plug_reload() {
  plug_save_finish();   // a map write under way: the file read below must see it
  uint32_t tr0 = micros();
  plug_reload_pending = false;
  plug_flush_log();
  char fn[48], tmp[52];
  plug_file(fn, sizeof fn);
  plug_file(tmp, sizeof tmp, true);
  bool exists, tmp_exists = false;
  rmp_slot_map tm;
  if (plug_proj_new) {   // another project: what it may inherit (its prints state was reset by hk_project_changed)
    plug_proj_new = false;
    plug_prev = plug_map;
  }
  if (!plug_read_map(fn, &plug_map, &exists)) {
    if (plug_read_map(tmp, &tm, &tmp_exists)) {   // a write that stopped before its rename: finish it
      plug_map = tm;
      if (exists) cards.sd().remove(fn);
      bool ok = cards.sd().rename(tmp, fn);
      logf(PSTR("plugins: %s from its .tmp (rename %s)"), fn, ok ? "done" : "FAILED");
    } else if (exists) {
      logf(PSTR("plugins: %s: not a slot map, no user machines"), fn);
    }
  }
  {   // the notes (where each assignment came from); a project with neither file is fresh
    char nf[48];
    plug_notes_file(nf, sizeof nf);
    FsFile f;
    char b[400];
    int n = card_ok && f.open(nf, O_RDONLY) ? f.read(b, sizeof b) : -1;
    if (f) f.close();
    bool notes = n > 0 && rmp_notes_parse(b, (uint32_t)n, &plug_notes);
    plug_fresh = rmp_resolve_open(&plug_map, &plug_notes, exists || tmp_exists, notes);
  }
  plug_job_on = false;   // a full reload supersedes an incremental one
  plug_act_slot = -1;
  plug_state_stale = 0;
  for (int vi = 0; vi < RMP_VOICES; ++vi) plug_vblk[vi] = nullptr, plug_vblk_cap[vi] = 0;
  plug_todo = 0;
  for (int vi = 0; vi < RMP_VOICES; ++vi) {
    plug_leave(vi);   // voice_leave while the old machines are still in the arena
    for (int s = 0; s < RMP_SLOTS; ++s) plug_state[vi][s] = nullptr;
  }
  for (int s = 0; s < RMP_SLOTS; ++s) plug_desc_ok[s] = false;
  plug_desc_todo = (1u << RMP_SLOTS) - 1;   // every slot's description again (an empty one is cleared)
  rmp_state_reset();
  memset(plug, 0, sizeof plug);
  memset(plug_overruns, 0, sizeof plug_overruns);
  memset(plug_trig_said, 0, sizeof plug_trig_said);
  int used = 0;
  for (int s = 0; s < RMP_SLOTS; ++s) used += plug_map.slot[s].id[0] != 0;
  if (plug_off) {
    if (used) logf(PSTR("plugins: safe mode - not loaded (%s)"), fn);
    return;
  }
  // the machines load one at a time from loop(), a few ms a pass (plug_job_pass), into the
  // fresh arena; a slot plays when its package is loaded and initialised
  rmp_arena_reset();
  plug_arena_dirty = false;
  for (int s = 0; s < RMP_SLOTS; ++s)
    if (plug_map.slot[s].id[0]) plug_todo_slot(s);
  if (used) logf(PSTR("plugins: %s: %d slots to load (map read %lu us)"), fn, used, (unsigned long)(micros() - tr0));
}

// Incremental assign: the slots whose package is to be (re)loaded, appended to the arena
// one at a time, a few ms a loop pass (plug_job_pass). Everything else keeps playing.
FLASHMEM void plug_todo_slot(int s) {
  if (plug_job_on && plug_job_slot == s) plug_job_on = false;   // its half-loaded package is abandoned
  if (plug_act_slot == s) plug_act_slot = -1;
  for (int vi = 0; vi < RMP_VOICES; ++vi)
    if (plug_cur[vi] == s) plug_leave(vi);   // voice_leave while the old machine is still there
  for (int vi = 0; vi < RMP_VOICES; ++vi)
    if (v[vi].user == s) plug_mis &= (uint8_t)~(1u << vi);   // its plugin changes: the next trig decides again
  plug_todo |= 1u << s;
  plug[s] = rmp_loaded{RMP_SLOT_LOADING, nullptr, 0, 0, 0};   // plays nothing until it is loaded and initialised
  plug_place[s] = RMP_PLACE_INTERNAL;
  plug_mute_why[s] = 0;
  plug_mute_txt[s] = nullptr;
  plug_bad_txt[s][0] = 0;
  plug_desc_ok[s] = false;
  plug_desc_todo |= 1u << s;
  plug_overruns[s] = 0;
  plug_trig_said[s] = false;
  plug_cyc_sum[s] = plug_cyc_n[s] = plug_cyc_peak[s] = 0;
}
// One pass of the incremental load: the init() of the slot loaded last, or the next queued slot's
// start, or a step of the running one. init() has a pass of its own (its time is in the "ready" line).
FLASHMEM void plug_job_pass() {
  uint32_t t0 = micros();
  if (plug_act_slot >= 0) {
    int s = plug_act_slot;
    plug_act_slot = -1;
    plug[s] = plug_act_res;
    int dead = -1;   // another slot on this image faulted while this one waited for its init pass
    for (int t = 0; t < RMP_SLOTS; ++t)
      if (t != s && plug[s].plugin && plug[t].plugin == plug[s].plugin && plug[t].state == RMP_SLOT_MUTED) dead = t;
    if (dead >= 0) {
      plug[s].state = RMP_SLOT_MUTED;
      plug_mute_why[s] = plug_mute_why[dead];
      plug_mute_txt[s] = plug_mute_txt[dead];
      logf(PSTR("plugins: U%d %s shares the image of a muted slot - muted"), s + 1, plug_map.slot[s].id);
    } else {
      plug_activate(s, true);
    }
    if (plug[s].state == RMP_SLOT_READY) {   // the sound tag: its fingerprint; the slot's voices decide afresh
      plug_fp[s] = rmp_plugin_fp(plug_map.slot[s].id);
      for (int vi = 0; vi < RMP_VOICES; ++vi)
        if (v[vi].user == s) plug_mis &= (uint8_t)~(1u << vi);
    }
    if (!plug_todo && !plug_job_on) {
      rmp_store_release();
      if (rmp_arena_psram_size())
        logf(PSTR("plugins: arena %lu of %lu B, PSRAM arena %lu of %lu B"), (unsigned long)rmp_arena_used(),
             (unsigned long)RMP_ARENA_BYTES, (unsigned long)rmp_arena_psram_used(), (unsigned long)rmp_arena_psram_size());
      else
        logf(PSTR("plugins: arena %lu of %lu B"), (unsigned long)rmp_arena_used(), (unsigned long)RMP_ARENA_BYTES);
    }
    return;
  }
  if (!plug_job_on) {
    if (!plug_todo) return;
    int s = 0;
    while (!(plug_todo & (1u << s))) ++s;
    plug_todo &= ~(1u << s);
    plug_job_t = t0; plug_job_passes = 0; plug_job_max = 0;
    rmp_store_stats_reset();
    plug_job_on = !rmp_slots_one_begin(&plug_job, &plug_pf, &plug_map, s, plug);
    plug_job_slot = s;
  } else {
    plug_job_on = !rmp_slots_one_step(&plug_job, plug, kPlugStepBytes);
  }
  ++plug_job_passes;
  uint32_t now = micros();
  if (now - t0 > plug_job_max) plug_job_max = now - t0;
  if (!plug_job_on) {
    int s = plug_job_slot;
    rmp_store_stats_t ss = rmp_store_stats();
    if (plug[s].state == RMP_SLOT_NO_ROOM && plug_arena_dirty) {   // the arena is full of old images: start over with a full load
      logf(PSTR("plugins: U%d %s: arena full - reloading all slots"), s + 1, plug_map.slot[s].id);
      plug_reload_pending = true;
      rmp_store_release();
      return;
    }
    logf(PSTR("plugins: U%d load %lu us over %d passes (longest %lu us; store: %lu opens %lu us, %lu reads %lu us, %lu B)"), s + 1,
         (unsigned long)(now - plug_job_t), plug_job_passes, (unsigned long)plug_job_max,
         (unsigned long)ss.opens, (unsigned long)ss.us_open, (unsigned long)ss.reads, (unsigned long)ss.us_read,
         (unsigned long)ss.bytes);
    plug_act_res = plug[s];   // plays nothing until init() has run (next pass)
    if (plug_act_res.state == RMP_SLOT_READY) plug[s] = rmp_loaded{RMP_SLOT_LOADING, nullptr, 0, 0};
    plug_act_slot = s;
  }
}

// A report for voice vi on user slot x.user: the voice's state (voice_init when
// it starts playing the slot), then params and, on a trig, note_on. tag: the sound's (spec 2.6; 0 untagged).
__attribute__((noinline)) FLASHMEM void plug_report(int vi, int note, int vel, int tag) {
  V& x = v[vi];
  int s = x.user;
  if (plug_cur[vi] >= 0 && plug_cur[vi] != s) plug_leave(vi);   // from another slot: its voice_leave first
  if (note >= 0 && tag && !plug_prints_have && plug_lazy_fp[s] != tag &&
      (plug[s].state != RMP_SLOT_READY || tag != plug_fp[s]))
    plug_lazy_req[s] = (uint16_t)tag;   // an older Rytm: the tag may name the machine (plug_resolve_pass)
  if (plug[s].state != RMP_SLOT_READY) {
    if (note >= 0 && !plug_trig_said[s]) {   // once a slot and reload
      plug_trig_said[s] = true;
      logf(PSTR("trig voice %d U%d: %s"), vi, s + 1, plug_map.slot[s].id[0] ? "not playing (see plugins:)" : "empty");
    }
    return;   // (no tag compare: a slot not READY has no fingerprint yet; READY clears the voices' bits)
  }
  // The sound tag (spec 2.6), only with the slot READY: a 0x04 sets or clears the voice's mismatch bit, a 0x03
  // never does (it is dropped while the bit is set). Clear -> set releases the voice first - note_off, then
  // plug_leave (which clears the bit, so the bit is set after it), its held note dropped - so a note held
  // across the mismatch cannot come back; the next matching trig starts the voice afresh (voice_init).
  uint8_t bit = (uint8_t)(1u << vi);
  if (note >= 0) {
    if (tag && tag != plug_fp[s]) {
      if (plug_mis & bit) return;   // set -> set: nothing
      if (plug_cur[vi] == s) {
        PlugCall c = {plug[s].plugin, plug_state[vi][s], nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr};
        plug_vcall(s, vi, pc_note_off, &c, kPlugCallCycles);
      }
      plug_leave(vi);
      plug_note_wait &= (uint8_t)~bit;
      plug_mis |= bit;
      x.live = false;
      logf(PSTR("trig voice %d U%d %s: sound tag %d, the slot's %d - silent until a matching trig"), vi, s + 1,
           plug_map.slot[s].id, tag, (int)plug_fp[s]);
      return;
    }
    plug_mis &= (uint8_t)~bit;   // untagged or matching: plays
  } else if (plug_mis & bit) {
    return;
  }
  const rmp_plugin* p = plug[s].plugin;
  void*& st = plug_state[vi][s];
  if (plug_bytes[s] <= sizeof shared[vi]) {   // the state lives in the voice's own engine memory (Plaits' scratch)
    if (st != (void*)shared[vi]) { st = shared[vi]; plug_cur[vi] = -1; }
    plug_dirty[vi] = true;   // Plaits must be re-initialised before this voice renders it again
  } else if (plug_vblk[vi] && plug_vblk_cap[vi] >= plug_bytes[s]) {   // PSRAM: the voice's block serves any slot it covers
    if (st != plug_vblk[vi]) { st = plug_vblk[vi]; plug_cur[vi] = -1; }
  } else {   // a bigger (or the first) block, only with PSRAM (plug_activate refuses it otherwise)
    void* b = rmp_state_alloc(plug_bytes[s]);
    if (!b) {
      if (plug_state_stale && !plug_state_reloaded) {   // old blocks are dead weight: start over, once
        plug_state_reloaded = true;
        logf(PSTR("plugins: state pool full (%lu B stale) - reloading all slots"), (unsigned long)plug_state_stale);
        plug_reload_pending = true;
      } else if (note >= 0) {
        logf(PSTR("plugins: no state room for voice %d on U%d (%lu B)"), vi, s + 1, (unsigned long)plug_bytes[s]);
      }
      return;
    }
    plug_state_stale += plug_vblk_cap[vi];
    plug_vblk[vi] = st = b;
    plug_vblk_cap[vi] = plug_bytes[s];
    plug_cur[vi] = -1;
  }
  float q[16];   // what the plugin gets: the report with the voice's preset state applied (x.p stays the report's)
  PlugCall c = {p, st, nullptr, q, 0, 0, 0, nullptr, 0, nullptr};
  if (plug_cur[vi] != s) {   // the voice starts playing the slot: state zeroed, voice_init, voice_enter (API 2)
    memset(st, 0, plug_bytes[s]);
    rmp_pool_clear(&plug_pool[vi]);   // before voice_init: a pool_info there keeps its want (prepared once the voice plays the slot)
    if (!plug_vcall(s, vi, pc_voice_init, &c, kPlugCallCycles)) {
#if RYTM_PSRAM
      ram_release(vi);   // a pool_info in voice_init may have pinned a sample; plug_leave will not run
#endif
      return;
    }
    plug_cur[vi] = s;
    plug_voice_start(vi);
    if (rmp_voice_enter_of(p) && !plug_vcall(s, vi, pc_voice_enter, &c, kPlugCallCycles)) return;
  }
  // presets (rmp_pv): what this report means for the voice's preset; a load runs from loop() (plug_preset_pass)
  if (note >= 0) plug_last_trig |= (uint8_t)(1u << vi);
  else plug_last_trig &= (uint8_t)~(1u << vi);
  plug_pv_decide(vi, s, note >= 0, v[vi].p);
  plug_pv_params(vi, s, note >= 0, q);
  if (!plug_vcall(s, vi, pc_params, &c, kPlugCallCycles)) return;
  if (note >= 0) {
    // its preset loads first (the next loop pass), or its decision once the slot's description / list is read;
    // then the note - at the latest kPlugNoteHoldUs after the trig (plug_note_overdue)
    bool held = plug_pl[vi][0].on || plug_pl[vi][1].on || (plug_pv_wait >> vi & 1);
    plug_note[vi] = PlugNote{(uint8_t)note, (uint8_t)vel, held, micros()};
    plug_note_wait |= (uint8_t)(1u << vi);
    if (held) return;
    plug_note_fire(vi);
  }
}

// A trig's held note_on (plug_report): played once the voice's preset loads ran; dropped when the voice
// moved on. A note-off for it plays it first.
FLASHMEM void plug_note_fire(int vi) {
  if (!(plug_note_wait >> vi & 1)) return;
  plug_note_wait &= (uint8_t)~(1u << vi);
  int s = v[vi].user;
  if (s < 0 || plug_cur[vi] != s || plug[s].state != RMP_SLOT_READY) return;
  const PlugNote& n = plug_note[vi];
  PlugCall c = {plug[s].plugin, plug_state[vi][s], nullptr, nullptr, (float)n.note, (n.vel < 1 ? 1 : n.vel) / 127.0f, 0,
                nullptr, 0, nullptr};
  uint32_t held = micros() - n.t;
  bool early = plug_pl[vi][0].on || plug_pl[vi][1].on || (plug_pv_wait >> vi & 1);   // overdue: its preset still to come
  if (!plug_vcall(s, vi, pc_note_on, &c, kPlugCallCycles)) return;
  v[vi].live = true;
  if (n.held) {   // held for a preset load
    if (held > plug_note_lat_max) plug_note_lat_max = held;
    logf(PSTR("trig voice %d U%d %s note %d (%s its preset load: %lu us)"), vi, s + 1, plug_map.slot[s].id, n.note,
         early ? "held, before" : "after", (unsigned long)held);
  } else {
    logf(PSTR("trig voice %d U%d %s note %d"), vi, s + 1, plug_map.slot[s].id, n.note);
  }
}

// Held notes past kPlugNoteHoldUs played now (every loop pass, whatever else runs): a slot save, a package
// job or a scan in the way delays the preset, not the note; the load still runs after and applies at once.
FLASHMEM void plug_note_overdue() {
  uint32_t now = micros();
  for (int vi = 0; vi < RMP_VOICES; ++vi)
    if ((plug_note_wait >> vi & 1) && now - plug_note[vi].t >= kPlugNoteHoldUs) plug_note_fire(vi);
}

// One block of voice i on its user slot into o: silence unless the slot plays.
void plug_render(int i, float* o) {
  int s = v[i].user;
  void* st = plug_state[i][s];
  if (plug[s].state != RMP_SLOT_READY || plug_cur[i] != s || !st || (plug_note_wait >> i & 1)) {
    for (int k = 0; k < kBlock; ++k) o[k] = 0.0f;   // (a trig's note held for its preset load: silent until it plays)
    return;
  }
  // twice the declared CPU of one 12-sample block (600 MHz, 4000 blocks/s), plus the call
  // (a PSRAM image: x RMP_PSRAM_LIMIT_X for the overrun count, plus the cold-cache allowance for the timeout, in plug_call)
  uint32_t base = (uint32_t)(2ull * plug_permille[s] * (600000000u / 4000u) / 1000u) + 2000u;
  uint32_t limit = rmp_limit_budget(base, plug_place[s]);
  PlugCall c = {plug[s].plugin, st, o, nullptr, 0, 0, 0, nullptr, 0, nullptr};
  if (!plug_vcall(s, i, pc_render, &c, base)) {
    for (int k = 0; k < kBlock; ++k) o[k] = 0.0f;
    return;
  }
  uint32_t cyc = rmp_guard_last_cycles();
  if (cyc > plug_max_cyc) plug_max_cyc = cyc, plug_max_limit = limit;
  plug_cyc_sum[s] += cyc; ++plug_cyc_n[s];
  if (cyc > plug_cyc_peak[s]) plug_cyc_peak[s] = cyc;
  if (cyc > limit) {
    if (++plug_overruns[s] >= 3) plug_mute(s, "over budget");
  } else {
    plug_overruns[s] = 0;
  }
  for (int k = 0; k < kBlock; ++k)   // a NaN or a runaway level: clipped
    o[k] = o[k] == o[k] ? (o[k] > 1.0f ? 1.0f : o[k] < -1.0f ? -1.0f : o[k]) : 0.0f;
}

// ---------------------------------------------------------------------------
// User slots on the Rytm (rytm-mods 0021's machines 35..39 as U1..U5). What the Rytm shows comes from
// here: 1A metadata from each slot's manifest, generations bumped when it changes (rmp_meta), 1D pushed,
// enum names (1B) read from the package on demand - all from loop() between requests (plug_slots_pass);
// in the report path only the 10-byte "unchanged".

// The voice stops playing its slot: voice_leave (API 2, through the guard) while the slot's machine is
// loaded and playing; a muted one is not called.
FLASHMEM void plug_leave(int vi) {
  plug_mis &= (uint8_t)~(1u << vi);   // the sound tag's mismatch goes too, also for a voice released already
  int s = plug_cur[vi];
  if (s < 0) return;
  plug_cur[vi] = -1;
  rmp_pool_clear(&plug_pool[vi]);   // its pool sample goes with it
#if RYTM_PSRAM
  ram_release(vi);                  // and its PSRAM pin
#endif
  rmp_pv_reset(&plug_pv[vi]);   // its preset state, a pending load and decision go with it
  plug_pl[vi][0].on = plug_pl[vi][1].on = false;
  plug_pv_wait &= (uint8_t)~(1u << vi);
  plug_note_wait &= (uint8_t)~(1u << vi);
  plug_pv_resync &= (uint8_t)~(1u << vi);
  if (plug[s].state != RMP_SLOT_READY || !plug[s].plugin || !rmp_voice_leave_of(plug[s].plugin)) return;
  PlugCall c = {plug[s].plugin, plug_state[vi][s], nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr};
  // without plug_vnow: pool_info / pool_read in voice_leave answer EMPTY / 0 - a prepare asked for here would
  // overwrite the DSP SAMPLE head the voice may be using already (plug_pool_pass checks the voice as well)
  plug_call(s, pc_voice_leave, &c, kPlugCallCycles);
}

// The voice starts a slot (after voice_init): its DSP SAMPLE stream stops - the plugin owns the voice
// and its stream buffers now; DSP SAMPLE prepares afresh when the voice goes back to it.
FLASHMEM void plug_voice_start(int vi) {
  streams[vi].clear();
  prepared[vi][0] = -2;
  prep_pending[vi] = trig_pending[vi] = false;
  rmp_pv_reset(&plug_pv[vi]);   // presets afresh: its next decision is a first one
  plug_pl[vi][0].on = plug_pl[vi][1].on = false;
  plug_pv_new |= (uint8_t)(1u << vi);
  plug_pv_wait &= (uint8_t)~(1u << vi);
  plug_note_wait &= (uint8_t)~(1u << vi);
  plug_pv_resync &= (uint8_t)~(1u << vi);   // (its pool reader was cleared before voice_init: plug_report)
}

// Slot s's last scan hit a card error less than kPlugScanRetryMs ago: no rescan on demand until then
// (a description read or a 'W' to its folder clears it at once).
FLASHMEM bool plug_scan_blocked(int s) {
  if (!(plug_scan_err >> s & 1)) return false;
  if (millis() - plug_scan_err_t[s] < kPlugScanRetryMs) return true;
  plug_scan_err &= (uint8_t)~(1u << s);
  return false;
}

// The voice's preset decision for its latest report (x.p; rmp_presets.h's glue). It needs the slot's
// index current: a stale one is scanned first (a loop job) and the voice decided after (plug_preset_pass).
// A load decided here runs from loop() (plug_preset_pass): card I/O never in the report path.
FLASHMEM void plug_pv_decide(int vi, int s, bool is_trig, const float* p) {
  const rmp_desc& d = plug_desc[s];
  int pre = plug_desc_ok[s] && d.pre >= 0 && d.pre < d.nknobs ? d.knob[d.pre].p - 1 : -1;   // d.pre is a knob[] index
  uint8_t bit = (uint8_t)(1u << vi);
  plug_pv_wait &= (uint8_t)~bit;
  if (!plug_desc_ok[s] || (plug_desc_todo >> s & 1)) {   // its description first (the preset knob, preset_ext)
    plug_pv_wait |= bit;
    return;
  }
  if (pre < 0 || pre > 15) return;
  const rmp_preset_index& ix = plug_pix[s];
  if (!rmp_preset_index_ok(&ix)) {
    if (plug_scan_blocked(s)) return;   // a card error: no presets for now
    plug_pv_wait |= bit;
    if (plug_scan_slot != s) plug_scan_todo |= 1u << s;
    return;
  }
  if (plug_pv_resync & bit) {   // a write-back the list change dropped: its report decided again first, as a non-trig
    plug_pv_resync &= (uint8_t)~bit;
    if (is_trig) plug_pv_decide(vi, s, false, plug_pl[vi][0].p);
  }
  rmp_pv_check(&plug_pv[vi], ix.gen);
  bool sync = false;
  int e = rmp_pv_decide(&plug_pv[vi], pre, ix.n, is_trig, (plug_pv_new & bit) != 0, p, &sync);
  plug_pv_new &= (uint8_t)~bit;
  PlugPreLoad* P = plug_pl[vi];
  bool takeover = false;
  if (is_trig && P[0].on && P[0].gen == ix.gen && ix.n > 0) {   // the sound's new preset not loaded yet
    int want = (int)p[pre];
    if (want > ix.n - 1) want = ix.n - 1;
    if (want == P[0].entry) e = want, sync = true, takeover = true;   // the trig plays it: still written back
    else if (e < 0 && want != plug_pv[vi].failed) e = want, sync = false;   // the trig's PRE differs (rmp_pv holds the old sound's)
  }
  if (e < 0) {
    if (is_trig) P[1].on = false;   // the trig plays what is loaded: a pending p-lock play is superseded
    return;
  }
  // the sound's own preset: a pending play before it is moot - unless a held trig's (a non-trig report
  // after the trig, before its load: the trig's p-lock still plays after the write-back)
  if (sync && (is_trig || !(plug_note_wait >> vi & 1))) P[1].on = false;
  PlugPreLoad& L = P[sync ? 0 : 1];
  L.on = true;
  L.s = (int8_t)s;
  L.gen = ix.gen;
  L.entry = (int16_t)e;
  L.sync = sync;
  // the report the load was decided on (p may be L.p: a resync); a trig taking over P[0] keeps the sound's
  // report there - a resync decides that again, not the trig's p-locked values
  if (!takeover) memmove(L.p, p, sizeof L.p);
}
// What the plugin's params() gets for the voice's latest report: the hold and a p-locked entry applied,
// offset knobs relative to the playing entry's values.
FLASHMEM void plug_pv_params(int vi, int s, bool is_trig, float* q) {
  memcpy(q, v[vi].p, 16 * sizeof(float));
  rmp_pv_apply(&plug_pv[vi], q, is_trig, millis());
  rmp_pv_effective(&plug_pv[vi], &plug_knobs[s], q);
}

FLASHMEM bool plug_store_rd(void* ctx, uint32_t off, uint8_t* b, uint32_t n) { return rmp_store_read_at((const char*)ctx, off, b, n); }

// One slot's description: its installed package's manifest, else (not installed, unreadable) a name
// from its id; an empty slot's is empty.
FLASHMEM void plug_desc_read(int s) {
  plug_desc_todo &= ~(1u << s);
  const char* id = plug_map.slot[s].id;
  rmp_desc& d = plug_desc[s];
  plug_desc_ok[s] = true;
  if (plug_scan_slot == s) {   // a scan for the slot's previous machine
    plug_scan_dir.close();
    plug_scan_slot = -1;
  }
  rmp_preset_invalidate(&plug_pix[s]);   // its presets: scanned again after the description (preset_ext)
  if (plug_load_dir_slot == s) plug_load_dir.close(), plug_load_dir_slot = -1;
  plug_rec_n[s] = 0;
  plug_pre_has[s] = 0;
  plug_scan_err &= (uint8_t)~(1u << s);
  plug_scan_todo |= 1u << s;
  plug_on_card[s] = false;
  if (id[0] && plug[s].state == RMP_SLOT_MISSING) plug_card_todo |= 1u << s;   // is it on the card?
  if (!id[0]) {
    memset(&d, 0, sizeof d);
    d.pre = -1;
    rmp_knobs_from_desc(&plug_knobs[s], nullptr);
    return;
  }
  uint32_t len = 0;
  uint8_t h[RMP_HEADER_BYTES];
  rmp_info info;
  bool ok = rmp_store_size(id, &len) && rmp_store_read_at(id, 0, h, sizeof h) &&
            rmp_verify_header(h, len, &info) == RMP_OK && rmp_manifest_read(&info, plug_store_rd, (void*)id, &d);
  rmp_store_release();
  if (!ok) rmp_desc_from_id(id, &d);
  plug_desc_listname(s, false);
  rmp_knobs_from_desc(&plug_knobs[s], &d);
}

// 1A's flags for slot s.
FLASHMEM uint8_t plug_flags(int s) {
  uint8_t f = 0;
  if (plug_desc_ok[s] && plug_desc[s].page2) f |= RMP_FLAG_PARAMS;
  if (plug_pre_has[s]) f |= RMP_FLAG_PRESETS;
  if (plug_on_card[s] && plug[s].state == RMP_SLOT_MISSING) f |= RMP_FLAG_ON_CARD;
  return f;
}
// The protocol state 1A says (safe mode: an assigned slot is MUTED, shown !NAME).
FLASHMEM int plug_pstate(int s) {
  return plug_off && plug_map.slot[s].id[0] ? RMP_PST_MUTED : rmp_proto_state(plug[s].state);
}
// Why slot s is not READY (RMP_WHY_*, the Rytm's name: "!ROOM NPPR"); NONE when it is READY, EMPTY or LOADING.
FLASHMEM int plug_why_code(int s) {
  if (plug_off && plug_map.slot[s].id[0]) return RMP_WHY_SAFE;
  return rmp_why_of(rmp_proto_state(plug[s].state), plug_mute_why[s]);
}
// The same in words, for the web page and the info JSON ("" when there is nothing to say).
FLASHMEM void plug_why_text(int s, char* out, int n) {
  out[0] = 0;
  switch (plug_why_code(s)) {
    case RMP_WHY_SAFE: sniprintf(out, n, "safe mode: user machines off this boot"); break;
    case RMP_WHY_ROOM: {
      uint32_t used = rmp_arena_used(), fr = used < RMP_ARENA_BYTES ? RMP_ARENA_BYTES - used : 0;
      uint32_t pz = rmp_arena_psram_size(), pu = rmp_arena_psram_used();
      if (pz)
        sniprintf(out, n, "no room: needs %lu B, %lu B + %lu B (PSRAM) free", (unsigned long)plug[s].need, (unsigned long)fr,
                  (unsigned long)(pu < pz ? pz - pu : 0));
      else
        sniprintf(out, n, "no room: needs %lu B, %lu B free", (unsigned long)plug[s].need, (unsigned long)fr);
      break;
    }
    case RMP_WHY_MISS: sniprintf(out, n, "missing: not installed, or the version is out of range"); break;
    case RMP_WHY_BAD: sniprintf(out, n, "bad: %s", plug_bad_txt[s][0] ? plug_bad_txt[s] : "the package does not verify or place"); break;
    case RMP_WHY_TIME:
    case RMP_WHY_CRASH: sniprintf(out, n, "muted: %s", plug_mute_txt[s] ? plug_mute_txt[s] : "crashed"); break;
    default: break;
  }
}
// What 1A says, fingerprinted every 10 ms (or now): a change bumps the slot's generation, a 1D is due.
FLASHMEM void plug_meta_poll(bool now) {
  if (!now && millis() - plug_meta_t < 10) return;
  plug_meta_t = millis();
  for (int s = 0; s < RMP_SLOTS; ++s) {
    if (plug_desc_todo >> s & 1) continue;   // its description first: one generation step a change, not two
    if ((plug_scan_todo >> s & 1) || plug_scan_slot == s) continue;   // and its presets
    const rmp_slot_entry& e = plug_map.slot[s];
    const rmp_desc* d = e.id[0] && plug_desc_ok[s] ? &plug_desc[s] : nullptr;
    // the preset list's fingerprint too: a list change moves the generation (the Rytm's name cache)
    // and the reason (the name the Rytm shows for a non-READY slot)
    rmp_meta_set(&plug_meta, s, rmp_meta_print(plug_pstate(s), plug_flags(s), e.id, e.version, d) ^ plug_pre_fp[s] * 2654435761u ^
                                  (uint32_t)plug_why_code(s) * 0x9E3779B1u);
  }
}
// Slot s's 1A, every part, at its current generation.
FLASHMEM void plug_send_meta(int s) {
  uint8_t m[RMP_MSG_MAX];
  const rmp_slot_entry& e = plug_map.slot[s];
  rmp_desc pd;   // (stack: no RAM1 kept) the description with its unused knobs as empty labels (rmp_desc_pad_unused): what 1A carries
  const rmp_desc* d = nullptr;
  if (e.id[0] && plug_desc_ok[s]) pd = plug_desc[s], rmp_desc_pad_unused(&pd), d = &pd;
  int g = plug_meta.gen[s];
  to_rytm(m, rmp_build_meta_head_why(m, s + 1, g, plug_pstate(s), plug_flags(s), d, plug_fp[s], plug_why_code(s)));   // fp only if READY
  for (int part = 1; d && part < rmp_meta_nparts(d); ++part) to_rytm(m, rmp_build_meta_part(m, s + 1, g, part, d));
}
// 1B list arg: P(arg+1)'s enum names (read from the package), 8 a message. The package stays open
// while more of the slot's enum asks wait (a burst costs one store open), released after the last.
FLASHMEM void plug_send_enum(int s, int arg) {
  char names[16][8];
  int k = plug_desc_ok[s] ? rmp_desc_find(&plug_desc[s], arg + 1) : -1;
  int n = k >= 0 ? rmp_manifest_enum(&plug_desc[s], k, plug_store_rd, (void*)plug_map.slot[s].id, names) : 0;
  if (!plug_enum_todo[s]) rmp_store_release();
  if (n < 0) n = 0;
  uint8_t m[RMP_MSG_MAX];
  int first = 0;
  do {
    int c = n - first < RMP_LIST_PER_MSG ? n - first : RMP_LIST_PER_MSG;
    to_rytm(m, rmp_build_list(m, s + 1, arg, plug_meta.gen[s], first == 0, n, first, c, names[first], 8));
    first += c;
  } while (first < n);
}

// One step of scanning a slot's preset folder /PRESETS/<id>/ (rmp_presets.h): open it, then 16 entries
// a pass (a folder of 127 takes 8 passes); directories, hidden entries and names starting with '.' are
// skipped. No folder (or no card): no presets. The shared names are the scanned slot's after it.
FLASHMEM void plug_scan_pass() {
  int s = plug_scan_slot;
  if (s < 0) {
    uint32_t todo = plug_scan_todo & ~plug_desc_todo;   // the description (preset_ext) first
    s = __builtin_ctz(todo);
    plug_scan_todo &= ~(1u << s);
    rmp_preset_scan_begin(&plug_pnames, &plug_pix[s], s);
    plug_rec_n[s] = 0;
    char path[48];
    sniprintf(path, sizeof path, "/PRESETS/%s", plug_map.slot[s].id);
    if (card_ok && plug_map.slot[s].id[0] && plug_scan_dir.open(path, O_RDONLY) && plug_scan_dir.isDir()) {
      plug_scan_slot = s;
      return;
    }
    plug_scan_dir.close();
    rmp_preset_scan_end(&plug_pnames, &plug_pix[s]);
    plug_pre_has[s] = plug_pix[s].n > 0;
    plug_pre_fp[s] = plug_pix[s].print;
    return;
  }
  FsFile e;
  char nm[128];
  for (int k = 0; k < 16; ++k) {
    bool fault = plug_scan_fault >> s & 1;   // (tests: 'W' mode 3)
    if (fault || !e.openNext(&plug_scan_dir, O_RDONLY)) {
      bool err = fault || plug_scan_dir.getError() != 0;
      plug_scan_fault &= (uint8_t)~(1u << s);
      plug_scan_dir.close();
      plug_scan_slot = -1;
      if (err) {   // a card error midway: nothing of it is trusted; no presets, no rescan for a while
        rmp_preset_invalidate(&plug_pix[s]);
        rmp_preset_names_init(&plug_pnames);
        if (plug_load_dir_slot == s) plug_load_dir.close(), plug_load_dir_slot = -1;
        plug_rec_n[s] = 0;
        plug_pre_has[s] = 0;
        plug_pre_fp[s] = 0;
        plug_scan_err |= (uint8_t)(1u << s);
        plug_scan_err_t[s] = millis();
        plug_scan_todo &= ~(1u << s);
        for (int vi = 0; vi < RMP_VOICES; ++vi)   // voices waiting for the list: no preset this time
          if ((plug_pv_wait >> vi & 1) && v[vi].user == s) plug_pv_wait &= (uint8_t)~(1u << vi);
        logf(PSTR("presets U%d %s: card error (no presets; retried in %lu s or after a change)"), s + 1, plug_map.slot[s].id,
             (unsigned long)(kPlugScanRetryMs / 1000));
        return;
      }
      rmp_preset_scan_end(&plug_pnames, &plug_pix[s]);
      plug_pre_has[s] = plug_pix[s].n > 0;
      plug_pre_fp[s] = plug_pix[s].print;
      logf(PSTR("presets U%d %s: %d"), s + 1, plug_map.slot[s].id, plug_pix[s].n);
      return;
    }
    bool skip = e.isDir() || e.isHidden();
    uint32_t idx = e.dirIndex();
    uint64_t size = e.fileSize();
    uint32_t sector = e.firstSector();
    if (!e.getName(nm, sizeof nm)) nm[0] = 0;
    e.close();
    if (!skip && nm[0] && nm[0] != '.' && idx <= 0xFFFFu &&
        rmp_preset_add(&plug_pnames, &plug_pix[s], nm, (uint16_t)idx, plug_desc[s].preset_ext) && sector && size &&
        size <= sizeof plug_rbuf && size <= cards.sd().bytesPerCluster() && plug_rec_n[s] < RMP_PRESETS_MAX)
      plug_rec[s][plug_rec_n[s]++] = PlugPreRec{(uint16_t)idx, (uint16_t)size, sector};
  }
}
// 1B 0x20: page p of slot s's preset names (16 a page, two messages; page 0's first replaces the
// Rytm's list). Only with the shared names holding the slot's current list (rmp_preset_names_ok).
FLASHMEM void plug_send_presets(int s, int page) {
  const rmp_preset_names& l = plug_pnames;
  uint8_t m[RMP_MSG_MAX];
  int from = page * 16, to = from + 16 < l.n ? from + 16 : l.n;
  if (from >= l.n) {   // past the end (or none): the total alone
    to_rytm(m, rmp_build_list(m, s + 1, RMP_LIST_PRESETS, plug_meta.gen[s], page == 0, l.n, from, 0, l.name[0], RMP_PRESET_NAME));
    return;
  }
  for (int f = from; f < to; f += RMP_LIST_PER_MSG) {
    int c = to - f < RMP_LIST_PER_MSG ? to - f : RMP_LIST_PER_MSG;
    to_rytm(m, rmp_build_list(m, s + 1, RMP_LIST_PRESETS, plug_meta.gen[s], page == 0 && f == from, l.n, f, c, l.name[f],
                              RMP_PRESET_NAME));
  }
}

// 19 ASK (report path): what 0 at the current generation is answered here (10 bytes), the rest from loop().
FLASHMEM void plug_on_ask(const uint8_t* d, int len) {
  rmp_ask a;
  if (!rmp_parse_ask(d, len, &a) || a.slot > RMP_SLOTS) return;
  // a Rytm that missed every 1D: the slot menu's list ask (inside its 2 s wait), else after 3 s without one
  if (a.what == RMP_ASK_INSTALLED || plug_ms() - plug_1d_ms >= 3000) plug_1d_owed = true;
  int s = a.slot - 1;
  if (a.what == RMP_ASK_META && a.slot >= 1) {
    if (a.gen && a.gen == plug_meta.gen[s] && !(plug_desc_todo >> s & 1)) {
      uint8_t m[RMP_MSG_MAX];
      to_rytm(m, rmp_build_meta_unchanged(m, a.slot, a.gen));
    } else {
      plug_ask_todo |= 1u << s;
    }
  } else if (a.what == RMP_ASK_ENUM && a.slot >= 1 && a.arg < 16) {
    plug_enum_todo[s] |= (uint16_t)(1u << a.arg);
  } else if (a.what == RMP_ASK_PRESETS && a.slot >= 1 && a.arg <= 7) {   // page arg of the names (16 a page), from loop()
    plug_pre_page[s] |= (uint8_t)(1u << a.arg);
  } else if (a.what == RMP_ASK_INSTALLED) {
    plug_inst_asked = true;
  }
}

FLASHMEM bool plug_card_install_pass();
FLASHMEM bool plug_answer_pass();
FLASHMEM void plug_do_assign(const rmp_assign& a);
FLASHMEM void plug_card_path(int s, char* out, int max);
FLASHMEM void plug_inst_pass();
FLASHMEM void plug_send_installed();
// One step from loop(), between requests: a card copy's next 2 KB, a MISSING slot looked up on the card,
// the installed list built / sent, a description to read (not while a slot loads: one store file at a
// time), a 1A to send, an enum list to send, then a due 1D (the 1Es and their answers: plug_deadline_pass).
// True when it did something.
FLASHMEM bool plug_slots_pass() {
  plug_meta_poll(false);
  if (plug_card_install_pass()) return true;
  if (plug_card_todo) {   // a MISSING slot's package on the card? (1A's flag)
    int s = __builtin_ctz(plug_card_todo);
    char path[64];
    plug_card_todo &= ~(1u << s);
    plug_card_path(s, path, sizeof path);
    plug_on_card[s] = card_ok && plug_map.slot[s].id[0] && cards.sd().exists(path);
    plug_meta_poll(true);
    return true;
  }
  if (plug_desc_todo && !plug_job_on && !plug_todo && plug_act_slot < 0) {
    plug_desc_read(__builtin_ctz(plug_desc_todo));
    plug_meta_poll(true);
    return true;
  }
  // the installed list: built (again) here only when a 19 what 3 waits for it - else after everything
  // below (descriptions, scans, 1A: a settling 1E's answer waits for those, not for the list)
  bool store_idle = !plug_job_on && !plug_todo && plug_act_slot < 0 && !plug_ci.on;
  bool inst_stale = plug_inst_phase || plug_inst_gen != plug_meta.store_gen;
  if (plug_inst_asked && store_idle && inst_stale) {
    plug_inst_pass();
    return true;
  }
  // a card copy holds the store (the rebuild waits for it, maybe for a paused copy's whole Play): the last list
  // built, at its own generation (a 1E with it is refused NOT INSTALLED once the store moved on), not nothing
  if (plug_inst_asked && plug_ci.on && inst_stale && !plug_inst_phase && plug_inst_gen) {
    plug_inst_asked = false;
    plug_send_installed();
    return true;
  }
  if (plug_inst_asked && !inst_stale) {
    plug_inst_asked = false;
    plug_send_installed();
    return true;
  }
  bool card_free = !plug_job_on && !plug_todo && plug_act_slot < 0;
  if (plug_scan_slot >= 0 && card_free) {   // a preset scan under way: its next 16 entries
    plug_scan_pass();
    if (plug_scan_slot < 0) plug_meta_poll(true);
    return true;
  }
  for (int s = 0; s < RMP_SLOTS; ++s) {   // preset names asked for: sent while the shared names are the slot's
    if (!plug_pre_page[s] || (plug_desc_todo >> s & 1) || !rmp_preset_names_ok(&plug_pnames, &plug_pix[s], s)) continue;
    int pg = __builtin_ctz(plug_pre_page[s]);
    plug_pre_page[s] &= (uint8_t)~(1u << pg);
    plug_send_presets(s, pg);
    return true;
  }
  for (int s = 0; s < RMP_SLOTS; ++s) {   // the others: their folder scanned (again) first
    if (!plug_pre_page[s] || (plug_desc_todo >> s & 1)) continue;
    if (plug_scan_blocked(s)) {   // a card error: no names (total 0), the asks dropped
      plug_pre_page[s] = 0;
      uint8_t m[RMP_MSG_MAX];
      to_rytm(m, rmp_build_list(m, s + 1, RMP_LIST_PRESETS, plug_meta.gen[s], 1, 0, 0, 0, plug_pnames.name[0], RMP_PRESET_NAME));
      return true;
    }
    plug_scan_todo |= 1u << s;
  }
  if ((plug_scan_todo & ~plug_desc_todo) && card_free) {
    plug_scan_pass();
    if (plug_scan_slot < 0) plug_meta_poll(true);
    return true;
  }
  for (int s = 0; s < RMP_SLOTS; ++s) {
    if (plug_desc_todo >> s & 1) continue;   // its description first
    if (plug_ask_todo >> s & 1) {
      plug_ask_todo &= ~(1u << s);
      plug_meta_poll(true);
      plug_send_meta(s);
      return true;
    }
    if (plug_enum_todo[s]) {
      int arg = __builtin_ctz(plug_enum_todo[s]);
      plug_enum_todo[s] &= (uint16_t)~(1u << arg);
      plug_send_enum(s, arg);
      return true;
    }
  }
  if (rmp_meta_take(&plug_meta)) {
    uint8_t m[RMP_MSG_MAX];
    to_rytm(m, rmp_build_slots(m, plug_meta.gen, plug_meta.store_gen, 0, 0));
    plug_1d_ms = plug_ms();
    plug_1d_owed = false;
    return true;
  }
  if (store_idle && inst_stale) {
    plug_inst_pass();
    return true;
  }
  return false;
}

// An owed 1D (a 10, a 19: plug_1d_owed), at most once a second, and one every 5 s anyway (a lost 1D caught
// up, spec 2.1): from loop() ahead of the reload, job and save passes (a 10 naming another project gets its 1D
// before the slots load). 13 bytes, no answer pair.
FLASHMEM bool plug_1d_pass() {
  uint32_t age = plug_ms() - plug_1d_ms;
  if (age < (plug_1d_owed ? 1000u : 5000u)) return false;
  uint8_t m[RMP_MSG_MAX];
  plug_meta_poll(true);
  to_rytm(m, rmp_build_slots(m, plug_meta.gen, plug_meta.store_gen, 0, 0));
  plug_1d_ms = plug_ms();
  plug_1d_owed = false;
  return true;
}

// One voice's preset step from loop(): a voice waiting for its slot's index decided, else a pending
// load run - the file by its folder index, a generic file's values or the plugin's load_entry (native,
// through the guard); rmp_pv_load decides what is written back (1C) and what plays, and the voice gets
// params() with it at once. True when it did something.
FLASHMEM void plug_preset_load(int vi, PlugPreLoad& L);
FLASHMEM bool plug_preset_pass() {
  for (int vi = 0; vi < RMP_VOICES; ++vi) {
    if (!(plug_pv_wait >> vi & 1)) continue;
    int s = v[vi].user;
    if (s < 0 || plug_cur[vi] != s || plug_scan_blocked(s)) {   // moved on, or no list for now
      plug_pv_wait &= (uint8_t)~(1u << vi);
      continue;
    }
    if (!plug_desc_ok[s] || (plug_desc_todo >> s & 1)) continue;   // its description first
    if (rmp_preset_index_ok(&plug_pix[s])) {
      plug_pv_decide(vi, s, plug_last_trig >> vi & 1, v[vi].p);
      if (!plug_pl[vi][0].on && !plug_pl[vi][1].on) plug_note_fire(vi);
      return true;
    }
    if (plug_scan_slot != s) plug_scan_todo |= 1u << s;
  }
  for (int vi = 0; vi < RMP_VOICES; ++vi) {
    PlugPreLoad* P = plug_pl[vi];
    if (!P[0].on && !P[1].on) {
      if ((plug_note_wait >> vi & 1) && !(plug_pv_wait >> vi & 1)) {   // a held note whose load was dropped
        plug_note_fire(vi);
        return true;
      }
      continue;
    }
    plug_preset_load(vi, P[0].on ? P[0] : P[1]);   // the write-back first, then a play
    if (!P[0].on && !P[1].on && !(plug_pv_wait >> vi & 1)) plug_note_fire(vi);   // the trig's note, now on its preset
    return true;
  }
  return false;
}
// One pending load of voice vi (L, one of plug_pl[vi]).
FLASHMEM void plug_preset_load(int vi, PlugPreLoad& L) {
  uint32_t t0 = micros();
  L.on = false;
  int s = L.s;
  const rmp_preset_index& ix = plug_pix[s];
  if (v[vi].user != s || plug_cur[vi] != s || plug[s].state != RMP_SLOT_READY) return;   // moved on
  if (!rmp_preset_index_ok(&ix) || ix.gen != L.gen || L.entry >= ix.n) {   // the list changed meanwhile: decided again
    if (L.sync || plug_pl[vi][0].on) plug_pv_resync |= (uint8_t)(1u << vi);   // a write-back was pending (its report in [0].p)
    plug_pl[vi][0].on = plug_pl[vi][1].on = false;
    plug_pv_wait |= (uint8_t)(1u << vi);
    return;
  }
  const rmp_plugin* p = plug[s].plugin;
  bool native = ix.kind[L.entry] == RMP_PRESET_NATIVE;
  const char* name = rmp_preset_names_ok(&plug_pnames, &ix, s) ? plug_pnames.name[L.entry] : "";
  int n = -1;
  bool cut = false;
  int r = plug_rec_find(s, ix.dir[L.entry]);
  if (r >= 0 && card_ok) {   // a small file in one cluster: its sectors read raw, no directory read (~0.1 ms)
    const PlugPreRec& R = plug_rec[s][r];
    if (cards.sd().card()->readSectors(R.sector, plug_rbuf, (R.len + 511u) / 512u)) n = R.len;
  }
  if (n < 0) {   // through the folder (kept open): a bigger file, or the raw read failed
    if (plug_load_dir_slot != s) {
      char path[48];
      sniprintf(path, sizeof path, "/PRESETS/%s", plug_map.slot[s].id);
      plug_load_dir.close();
      plug_load_dir_slot = card_ok && plug_load_dir.open(path, O_RDONLY) ? s : -1;
    }
    FsFile f;
    if (plug_load_dir_slot == s && f.open(&plug_load_dir, ix.dir[L.entry], O_RDONLY)) {
      cut = f.fileSize() > sizeof plug_rbuf;
      if (!(native && cut)) n = f.read(plug_rbuf, sizeof plug_rbuf);   // a native preset is whole or not at all
    }
    f.close();
    if (n < 0 && plug_load_dir_slot == s) plug_load_dir.close(), plug_load_dir_slot = -1;   // opened afresh next time
  }
  int16_t vals[16];
  for (int k = 0; k < 16; ++k) vals[k] = -1;
  const char* why = nullptr;
  if (n < 0) {
    why = cut ? "over 4096 B" : "cannot read";
  } else if (!native) {
    rmp_preset_parse((const char*)plug_rbuf, (uint32_t)n, vals, cut);
  } else if (!rmp_load_entry_of(p)) {
    why = "the plugin has no load_entry";
  } else {
    PlugCall c = {p, plug_state[vi][s], nullptr, nullptr, 0, 0, 0, plug_rbuf, n, vals};
    if (!plug_vcall(s, vi, pc_load_entry, &c, kPlugCallCycles)) return;   // muted (logged)
    if (c.ret != 0) why = "refused by the plugin";
  }
  if (why) {
    rmp_pv_failed(&plug_pv[vi], L.entry);
    logf(PSTR("preset voice %d U%d %d %s: %s"), vi, s + 1, L.entry, name, why);
    return;
  }
  uint8_t ok[16], ov[16];
  int m = rmp_pv_load(&plug_pv[vi], L.entry, L.sync, vals, &plug_knobs[s], millis(), L.p, ok, ov);
  if (m) {
    uint8_t msg[RMP_MSG_MAX];
    to_rytm(msg, rmp_build_knobs(msg, vi, s + 1, m, ok, ov));
  }
  float q[16];
  plug_pv_params(vi, s, plug_last_trig >> vi & 1, q);
  PlugCall c = {p, plug_state[vi][s], nullptr, q, 0, 0, 0, nullptr, 0, nullptr};
  if (!plug_vcall(s, vi, pc_params, &c, kPlugCallCycles)) return;
  logf(PSTR("preset voice %d U%d %d %s (%s%s, %lu us)"), vi, s + 1, L.entry, name, L.sync ? "written back" : "played",
       cut ? ", cut at 4096 B" : "", (unsigned long)(micros() - t0));
}


// 'W' <mode> <path len> <path> <n u16 LE> <n bytes> (development, tools/rmp_install.py put): a file under
// /PRESETS/ or /MACHINES/ on the card - mode 0 create (truncate) and write, 1 append, 2 delete (n 0; an
// empty folder too), 3 (tests) the next scan of that preset folder fails as a card error. A preset folder
// that changed is scanned again for the slots playing its machine (a card error's back-off ends).
FLASHMEM void plug_put(int mode, const char* pp, int pl, const uint8_t* data, int n) {
  char path[81];
  memcpy(path, pp, pl);
  path[pl] = 0;
  bool ok = card_ok && (int)strlen(path) == pl && !strstr(path, "..") && mode >= 0 && mode <= 3 &&
            ((!strncmp(path, "/PRESETS/", 9) && path[9]) || (!strncmp(path, "/MACHINES/", 10) && path[10]));
  if (!ok) {
    logf(PSTR("card: cannot write %s (refused)"), path);
    return;
  }
  if (mode == 3) {
    logf(PSTR("card: a scan fault armed for %s"), path);
  } else if (mode == 2) {
    bool r = cards.sd().remove(path) || cards.sd().rmdir(path);
    logf(PSTR("card: removed %s%s"), path, r ? "" : " (it was not there)");
  } else {
    char dir[81];
    memcpy(dir, path, sizeof dir);
    *strrchr(dir, '/') = 0;
    cards.sd().mkdir(dir, true);
    FsFile f;
    bool w = f.open(path, mode == 0 ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_CREAT | O_APPEND)) &&
             (int)f.write(data, n) == n && f.sync();
    f.close();
    logf(PSTR("card: %s %s (%d B)"), w ? "wrote" : "cannot write", path, n);
  }
  if (!strncmp(path, "/PRESETS/", 9))
    for (int s = 0; s < RMP_SLOTS; ++s) {
      size_t l = strnlen(plug_map.slot[s].id, 32);
      if (!l || strncasecmp(path + 9, plug_map.slot[s].id, l) || (path[9 + l] != '/' && path[9 + l])) continue;   // FAT: any case
      if (plug_scan_slot == s) {   // a scan under way saw the folder before this change
        plug_scan_dir.close();
        plug_scan_slot = -1;
      }
      rmp_preset_invalidate(&plug_pix[s]);
      if (plug_load_dir_slot == s) plug_load_dir.close(), plug_load_dir_slot = -1;
      plug_rec_n[s] = 0;
      plug_scan_err &= (uint8_t)~(1u << s);
      if (mode == 3) plug_scan_fault |= (uint8_t)(1u << s);
      plug_scan_todo |= 1u << s;
    }
  if (!strncmp(path, "/MACHINES/", 10))   // a package came or went: the MISSING slots look again
    for (int s = 0; s < RMP_SLOTS; ++s)
      if (plug[s].state == RMP_SLOT_MISSING) plug_card_todo |= 1u << s;
}

// Plugin API 2's pool access, for the voice being called (plug_vnow): the project's pool slot 1..127.
// With PSRAM and the sample resident (sample_ram_pool.h): all of it, 16-bit mono [measured: sample_ram_pool.h:6,
// loads convert to int16 mono at the file's rate], pinned while the voice reads it - untested until the
// PSRAM check. Otherwise the voice's head/ring reader (rmp_pool), prepared and refilled by plug_pool_pass.
// Both run inside the plugin's guarded call (render too): no card I/O, no logging. Not for a voice in
// voice_leave (plug_vnow -1 there: EMPTY / 0).
// With PSRAM, known limits (not fixed): PSRAM is asked for the sample only at the reader's prepare - when
// it had no room then, the voice keeps the head and ring until its reader is prepared again (a pool change,
// the voice leaving its slot); and a voice holds one pin, so a plugin alternating between two resident
// slots moves the pin each pool_info, and a pool_read of the slot not pinned goes to the head/ring reader
// (NOT_READY until prepared).
// FLASHMEM: a lookup once a block at most (rmp_pool_info_get runs from flash already); pool_read stays in ITCM.
FLASHMEM int32_t h_pool_info(int32_t slot, rmp_pool_info* info) {
  int vi = plug_vnow;
  if (vi < 0 || !info || !card_ok || slot < 1 || slot > npool || pool[slot - 1].folder < 0) return RMP_POOL_EMPTY;
#if RYTM_PSRAM
  if (psram) {
    const PoolEntry& e = pool[slot - 1];
    const int16_t* pcm;
    uint32_t frames;
    float rate;
    bool cut;
    if (psram->get(e.folder, e.file, &pcm, &frames, &rate, &cut) && !cut) {
      if (ram_pin[vi][0] != e.folder || ram_pin[vi][1] != (int)e.file) {
        int of = ram_pin[vi][0], os = ram_pin[vi][1];
        ram_pin[vi][0] = ram_pin[vi][1] = -1;   // a call cut short here leaks a pin at most, never unpins twice
        if (of >= 0) psram->unpin(of, os);
        psram->pin(e.folder, e.file);
        ram_pin[vi][0] = e.folder;
        ram_pin[vi][1] = e.file;
      }
      info->frames = frames; info->channels = 1; info->rate = rate;
      info->where = RMP_POOL_PSRAM; info->head_frames = frames;
      return RMP_POOL_OK;
    }   // not resident: the head and the ring meanwhile (plug_pool_pass asks PSRAM for it)
  }
#endif
  return rmp_pool_info_get(&plug_pool[vi], slot, info);
}
#if RYTM_PSRAM
// pool_read of a PSRAM-resident sample the voice holds pinned (cold: no PSRAM fitted yet); -1 when not.
__attribute__((noinline)) FLASHMEM int32_t h_pool_read_psram(int vi, int32_t slot, uint32_t pos, float* buf, int32_t n) {
  const PoolEntry& e = pool[slot - 1];
  if (ram_pin[vi][0] != e.folder || ram_pin[vi][1] != (int)e.file) return -1;
  const int16_t* pcm;
  uint32_t frames;
  float rate;
  bool cut;
  if (!psram->get(e.folder, e.file, &pcm, &frames, &rate, &cut) || cut) return -1;
  return rmp_pool_decode((const uint8_t*)pcm, frames, 1, 16, pos, buf, n);
}
#endif
int32_t h_pool_read(int32_t slot, uint32_t pos, float* buf, int32_t n) {
  int vi = plug_vnow;
  if (vi < 0 || !buf || n <= 0 || slot < 1 || slot > npool) return 0;
#if RYTM_PSRAM
  if (psram) {
    int32_t got = h_pool_read_psram(vi, slot, pos, buf, n);
    if (got >= 0) return got;
  }
#endif
  return rmp_pool_read(&plug_pool[vi], slot, pos, buf, n);
}
bool plug_pool_rd_err = false;   // the last plug_pool_rd failed (a card error, not a full ring)
FLASHMEM bool plug_pool_rd(void* ctx, uint32_t off, uint8_t* b, uint32_t n) {
  FsFile& f = pool_files[(int)(intptr_t)ctx];
  bool ok = f.seekSet(off) && (uint32_t)f.read(b, n) == n;
  if (!ok) plug_pool_rd_err = true;
  return ok;
}
// The voice plays its plugin (the slot it reports, started): its reader is the plugin's. Else the voice's
// stream buffers may be DSP SAMPLE's again - a prepare or refill into them would overwrite its head.
bool plug_pool_mine(int vi) { return v[vi].user >= 0 && plug_cur[vi] == v[vi].user; }
const uint32_t kPlugPoolRetryMs = 250;   // a reader whose card read failed is not refilled again before
uint8_t plug_pool_err = 0;               // voices backing off a failed refill
uint8_t plug_pool_said = 0;              // ... whose failure is logged (from loop(): never inside an audio frame)
uint32_t plug_pool_err_t[8];
uint8_t plug_pool_err_slot[8];           // the pool slot whose read failed (its reader may be cleared before it is logged)
// One card read for the neediest stream: the plugin reader with the least lead under `below` output frames
// (its frames at the sample's rate, as played at pitch 1: the plugin's speed is not known), unless a DSP
// SAMPLE stream has less ahead (refill_neediest first). A reader whose refill reads nothing (its ring full,
// or the end) gives the next its turn; one whose card read fails backs off kPlugPoolRetryMs and ends the
// call (one SD timeout a call at most). dsp: between render blocks (send_frames, host_feed: inside an audio
// frame, so no logging - the failure is logged by the next call from loop()); with no plugin reader under
// `below`, a DSP SAMPLE stream's top-up is wanted too. True when it read. Same context as the readers.
FLASHMEM bool plug_pool_refill(uint32_t below, bool dsp) {
  uint8_t tried = 0;
  uint32_t now = plug_ms();   // (the back-off spans flash erases: millis() loses them)
  if (!dsp && (plug_pool_err & ~plug_pool_said)) {   // from loop(): failures not logged yet
    for (int vi = 0; vi < RMP_VOICES; ++vi)
      if ((plug_pool_err & ~plug_pool_said) >> vi & 1)   // the prepare's line: no new string in RAM1
        logf(PSTR("voice %d: pool slot %d for its plugin cannot be read"), vi, plug_pool_err_slot[vi]);
    plug_pool_said |= plug_pool_err;
  }
  for (;;) {
    int best = -1;
    uint32_t least = below;
    for (int vi = 0; vi < RMP_VOICES; ++vi) {
      const rmp_pool_reader& r = plug_pool[vi];
      if (!r.slot || (tried >> vi & 1) || !plug_pool_mine(vi)) continue;
      if ((plug_pool_err >> vi & 1) && now - plug_pool_err_t[vi] < kPlugPoolRetryMs) continue;
      uint32_t a = rmp_pool_ahead(&r);
      if (a != 0xFFFFFFFFu && r.src.rate > 1.0f) {
        float t = a * (48000.0f / r.src.rate);
        a = t < 4.0e9f ? (uint32_t)t : 0xFFFFFFFEu;
      }
      if (a < least) least = a, best = vi;
    }
    if (best < 0) return dsp && refill_neediest(below);
    if (refill_neediest(least)) return true;   // a DSP SAMPLE stream with less ahead first
    tried |= (uint8_t)(1u << best);
    plug_pool_rd_err = false;
    if (rmp_pool_refill(&plug_pool[best]) > 0) {
      plug_pool_err &= (uint8_t)~(1u << best);
      plug_pool_said &= (uint8_t)~(1u << best);
      return true;
    }
    if (plug_pool_rd_err) {   // logged once until a read succeeds (above, from loop())
      plug_pool_err |= (uint8_t)(1u << best);
      plug_pool_err_t[best] = now;
      plug_pool_err_slot[best] = (uint8_t)plug_pool[best].slot;
      return false;
    }
  }
}
// Between requests: a voice's wanted pool sample prepared (the slot's file opened once and kept, as DSP
// SAMPLE's; the head read), else one refill (8 KB) for the plugin reader with the least lead under 4096
// output frames (a pending seek first; plug_pool_refill). True when it did either. A want of a voice not
// playing its plugin any more (it left in voice_leave, or its voice_init failed) is dropped.
FLASHMEM bool plug_pool_pass() {
  for (int vi = 0; vi < RMP_VOICES; ++vi) {
    rmp_pool_reader& r = plug_pool[vi];
    if (!r.want) continue;
    int slot = r.want, line = slot - 1;
    r.want = 0;
    if (!plug_pool_mine(vi)) continue;   // its buffers may be DSP SAMPLE's now
    if (!card_ok || slot > npool || pool[line].folder < 0) {
      r.bad = slot;
      return true;
    }
#if RYTM_PSRAM
    if (psram) psram->want(pool[line].folder, pool[line].file, false);   // into PSRAM too, when there is room
#endif
    uint32_t t0 = micros();
    if (!pool_open[line] && cards.file(pool[line].folder, pool[line].file, &pool_recs[line]) &&
        cards.open(pool[line].folder, pool_recs[line], &pool_files[line]))
      pool_open[line] = true;
    const card::FileRec& rec = pool_recs[line];
    rmp_pool_src src = {rec.data_offset, rec.data_len, (float)rec.rate, rec.channels, rec.bits};
    bool ok = pool_open[line] && rmp_pool_prepare(&r, slot, &src, plug_pool_rd, (void*)(intptr_t)line);
    if (!ok) r.bad = slot;
    plug_pool_err &= (uint8_t)~(1u << vi);
    plug_pool_said &= (uint8_t)~(1u << vi);
    if (ok)
      logf(PSTR("voice %d: pool slot %d for its plugin ready (%lu frames, %lu in the head, %lu us)"), vi, slot,
           (unsigned long)(r.src.data_len / r.bpf), (unsigned long)(r.hlen / r.bpf), (unsigned long)(micros() - t0));
    else
      logf(PSTR("voice %d: pool slot %d for its plugin cannot be read"), vi, slot);
    return true;
  }
  return plug_pool_refill(4096, false);
}

// The installed list (1B 0x21) for the Rytm's slot menu, built when the store changed: pass 1 lists the
// store (up to 32; its own directory handle, not the 'D' report's iterator), then one package a pass - its
// header (version) and its manifest's name (the head's first 10 bytes).
FLASHMEM void plug_inst_add(const char* id, uint32_t size, void*) {
  if (plug_inst_n >= 32) {
    ++plug_inst_over;
    return;
  }
  memcpy(plug_inst[plug_inst_n].id, id, 32);
  plug_inst[plug_inst_n++].size = size;
}
// Entries 0..n-1: one with nm's first 9 characters?
FLASHMEM bool plug_inst_dup(const char* nm, int n) {
  for (int i = 0; i < n; ++i)
    if (!strncmp(plug_inst[i].name, nm, 9)) return true;
  return false;
}
FLASHMEM void plug_inst_pass() {
  if (plug_inst_phase == 0) {
    plug_inst_n = plug_inst_over = 0;
    rmp_store_list(plug_inst_add, nullptr);
    plug_inst_phase = 2;
    plug_inst_i = 0;
    return;
  }
  if (plug_inst_i < plug_inst_n) {
    PlugInst& e = plug_inst[plug_inst_i++];
    uint8_t h[RMP_HEADER_BYTES], nm[10];
    rmp_info info;
    memset(e.name, 0, sizeof e.name);
    e.version = 0;
    if (rmp_store_read_at(e.id, 0, h, sizeof h) && rmp_verify_header(h, e.size, &info) == RMP_OK) {
      e.version = info.version;
      uint32_t at = RMP_HEADER_BYTES + info.image_size + 8u * info.reloc_count;
      if (info.manifest_size >= 10 && rmp_store_read_at(e.id, at, nm, 10))
        for (int k = 0; k < 10 && nm[k]; ++k) e.name[k] = (nm[k] >= 0x20 && nm[k] <= 0x7e) ? (char)nm[k] : '_';
    }
    if (!e.name[0]) {   // the slot description's fallback (plug_desc_read), so plug_desc_listname matches it
      rmp_desc d;
      rmp_desc_from_id(e.id, &d);
      memcpy(e.name, d.name, sizeof e.name);
    }
    // unique in the first 9 characters (what the Rytm keeps and matches a slot's 1A name against): a
    // duplicate gets "~2", "~3", ... within those 9
    char base[11];
    memcpy(base, e.name, sizeof base);
    e.renamed = false;
    for (int k = 2; plug_inst_dup(e.name, plug_inst_i - 1); ++k) {
      char sfx[5];
      int sl = sniprintf(sfx, sizeof sfx, PSTR("~%d"), k);
      int bl = (int)strnlen(base, 9 - sl);
      memset(e.name, 0, sizeof e.name);
      memcpy(e.name, base, bl);
      memcpy(e.name + bl, sfx, sl);
      e.renamed = true;
    }
    return;
  }
  rmp_store_release();
  plug_inst_phase = 0;
  plug_inst_gen = plug_meta.store_gen;
  logf(PSTR("plugins: %d installed (the Rytm's list, generation %d)"), plug_inst_n, (int)plug_inst_gen);
  if (plug_inst_over)
    logf(PSTR("plugins: the Rytm's list holds the first 32 of %d installed packages"), 32 + plug_inst_over);
  for (int s = 0; s < RMP_SLOTS; ++s) plug_desc_listname(s, true);
  plug_meta_poll(true);
}
// Slot s's 1A name: the installed list's for its package when the list renamed it (a duplicate's "~2"),
// so the Rytm's slot menu, which finds the slot's machine in the list by name, steps on from the right
// entry. reread (after a list build): a slot renamed before and not now reads its description again.
FLASHMEM void plug_desc_listname(int s, bool reread) {
  const rmp_slot_entry& m = plug_map.slot[s];
  if (!plug_desc_ok[s] || !m.id[0] || plug_inst_phase || plug_inst_gen != plug_meta.store_gen) return;
  for (int i = 0; i < plug_inst_n; ++i) {
    const PlugInst& e = plug_inst[i];
    if (strncmp(e.id, m.id, 32)) continue;
    if (e.renamed) memcpy(plug_desc[s].name, e.name, sizeof e.name);
    else if (reread && strncmp(plug_desc[s].name, e.name, 10)) plug_desc_todo |= 1u << s;
    return;
  }
}
// 1B 0x21: the installed list, 8 names a message, slot 0, the generation it was built for (the store's, but
// while a card copy blocks a rebuild).
FLASHMEM void plug_send_installed() {
  uint8_t m[RMP_MSG_MAX];
  char names[RMP_LIST_PER_MSG][14];
  int first = 0;
  do {
    int c = plug_inst_n - first < RMP_LIST_PER_MSG ? plug_inst_n - first : RMP_LIST_PER_MSG;
    for (int i = 0; i < c; ++i) {
      memset(names[i], 0, sizeof names[i]);
      memcpy(names[i], plug_inst[first + i].name, 10);
    }
    to_rytm(m, rmp_build_list(m, 0, RMP_LIST_INSTALLED, plug_inst_gen, first == 0, plug_inst_n, first, c, names[0], 14));
    first += c;
  } while (first < plug_inst_n);
}
// The card copy of a slot's package: /MACHINES/<id>-<a.b.c>.rmp.
FLASHMEM void plug_card_path(int s, char* out, int max) {
  const rmp_slot_entry& e = plug_map.slot[s];
  sniprintf(out, max, PSTR("/MACHINES/%s-%d.%d.%d.rmp"), e.id, (int)(e.version >> 16), (int)((e.version >> 8) & 255),
            (int)(e.version & 255));
}
// A 1E's answer: 1D with the slot's pair (generations fresh). plug_reply leaves a pending answer
// (plug_ans) as it is: a 1E refused while the slot's card copy runs. A slot plug_meta_poll still skips
// (its description or preset scan pending: a LOADING at the deadline) gets a new generation for the answer:
// the old one would tell the Rytm its cached 1A (the previous machine) is current, and the slot menu's
// assign would take that machine's defaults (0021 us_tick3). The bump invalidates its fingerprint, so the
// settled slot bumps it again (a push) even when it says what it said before: a 1A the Rytm got at the bumped
// generation before the slot settled (no presets flag yet, say) is always asked again.
// This 1D is built from plug_meta right after the poll and the bump: it carries every current generation, so a
// pending push (the bump's own dirty, or any other) is paid by it - no second 13-byte 1D behind it.
FLASHMEM void plug_reply(int s, int result) {
  uint8_t m[RMP_MSG_MAX];
  plug_meta_poll(true);
  if ((plug_desc_todo >> s & 1) || (plug_scan_todo >> s & 1) || plug_scan_slot == s) rmp_meta_bump(&plug_meta, s);
  to_rytm(m, rmp_build_slots(m, plug_meta.gen, plug_meta.store_gen, s + 1, result));
  rmp_meta_take(&plug_meta);
  plug_1d_ms = plug_ms();
  plug_1d_owed = false;
  logf(PSTR("plugins: U%d answered %d (%lu ms after the 1E)"), s + 1, result, (unsigned long)(plug_ms() - plug_1e_t[s]));
}
FLASHMEM void plug_answer(int s, int result) {
  plug_reply(s, result);
  plug_ans[s] = -1;
}
FLASHMEM void plug_ans_begin(int s, int op, const char* id, uint32_t version) {
  plug_ans[s] = (int8_t)op;
  memset(&plug_ans_want[s], 0, sizeof plug_ans_want[s]);
  strncpy(plug_ans_want[s].id, id, 31);
  plug_ans_want[s].version = version;
  plug_ans_t[s] = plug_1e_t[s];   // the Rytm's 2 s run from the 1E's arrival
}
// A 1E (from loop()): op 0 assign entry arg of the installed list (refused when the Rytm's list is stale),
// 1 clear, 2 install the slot's missing package from the card (not while the transport runs).
FLASHMEM void plug_do_assign(const rmp_assign& a) {
  // a 10 <project> just before (the same write, or the first after a reboot): the 1E is about that project's
  // map, so its reload runs first (the map read; the slot loads it queues stay behind the answers)
  if (plug_reload_pending) plug_reload();
  int s = a.slot - 1;
  if (plug_ci.on && plug_ci.s == s) {   // the slot's card copy runs: refused; the copy's answer still pending
    plug_reply(s, RMP_RES_BAD);
    return;
  }
  if (a.op == RMP_OP_CLEAR) {
    uint8_t zero[3] = {0, 0, 0};
    plug_ans_begin(s, a.op, "", 0);
    if (!plug_assign(a.slot, zero, 0, zero)) plug_answer(s, RMP_RES_BAD);
    else plug_save_for_1e();
  } else if (a.op == RMP_OP_ASSIGN) {
    if (a.installed_gen != plug_meta.store_gen || plug_inst_gen != plug_meta.store_gen || a.arg >= plug_inst_n) {
      plug_answer(s, RMP_RES_NOT_INSTALLED);
      return;
    }
    const PlugInst& e = plug_inst[a.arg];
    uint8_t ver[3] = {(uint8_t)(e.version >> 16), (uint8_t)(e.version >> 8), (uint8_t)e.version};
    plug_ans_begin(s, a.op, e.id, e.version);
    if (!plug_assign(a.slot, (const uint8_t*)e.id, (int)strnlen(e.id, 31), ver)) plug_answer(s, RMP_RES_BAD);
    else plug_save_for_1e();
  } else {
    if (plug_map.slot[s].id[0] && plug[s].state == RMP_SLOT_READY) {   // installed meanwhile
      plug_answer(s, RMP_RES_READY);
      return;
    }
    if (plug_transport_running()) {
      plug_answer(s, RMP_RES_STOP_TO_INSTALL);
      return;
    }
    const rmp_slot_entry& e = plug_map.slot[s];
    char path[64];
    plug_card_path(s, path, sizeof path);
    if (!e.id[0] || !card_ok || plug_ci.on || !plug_ci_f.open(path, O_RDONLY)) {
      plug_answer(s, plug_ci.on ? RMP_RES_BAD : RMP_RES_NOT_ON_CARD);
      return;
    }
    uint32_t size = plug_ci_f.fileSize();
    if (plug_installing || !rmp_store_install_begin(size)) {
      plug_ci_f.close();
      plug_answer(s, RMP_RES_BAD);
      return;
    }
    plug_ci = PlugCardInst{true, s, size, 0, plug_ms(), 0, 0};
    plug_ans_begin(s, a.op, e.id, e.version);
    logf(PSTR("plugins: U%d installing %s from the card (%lu B)"), s + 1, path, (unsigned long)size);
  }
}
// One pass of a card -> flash copy: 2 KB read and written (a fresh block's erase: ~0.25-0.28 s), paused while the
// transport runs; the first chunk's header must be the package the slot named (id, version). At the end
// the commit (CRC-verified, atomic rename) and the load. True when it ran.
FLASHMEM bool plug_card_install_pass() {
  if (!plug_ci.on) return false;
  if (plug_transport_running()) return false;
  uint8_t b[2048];
  uint32_t c0 = ARM_DWT_CYCCNT;   // the pass's time across a flash erase (micros() stops then)
  int s = plug_ci.s;
  const rmp_slot_entry& w = plug_ans_want[s];
  uint32_t n = plug_ci.size - plug_ci.done < sizeof b ? plug_ci.size - plug_ci.done : (uint32_t)sizeof b;
  bool ok = (uint32_t)plug_ci_f.read(b, n) == n;
  if (ok && plug_ci.done == 0) {
    rmp_info info;
    ok = n >= RMP_HEADER_BYTES && rmp_verify_header(b, plug_ci.size, &info) == RMP_OK && !strncmp(info.id, w.id, 32) &&
         info.version == w.version;
    if (!ok) logf(PSTR("plugins: U%d card copy refused: its header is not %s at the slot's version"), s + 1, w.id);
  }
  ok = ok && rmp_store_install_write(b, n);
  plug_ci.done += n;
  ++plug_ci.passes;
  uint32_t dc = ARM_DWT_CYCCNT - c0;
  if (dc > plug_ci.cyc_max) plug_ci.cyc_max = dc;
  if (ok && plug_ci.done < plug_ci.size) return true;
  plug_ci_f.close();
  plug_ci.on = false;
  char id[32] = {0};
  int st = -1;   // -1: a card read, the header or a flash write failed
  uint32_t c1 = ARM_DWT_CYCCNT;
  if (!ok) rmp_store_install_abort();
  else st = (int)rmp_store_install_commit(id);
  logf(PSTR("plugins: U%d card copy %lu ms wall in %lu passes (longest %lu us), commit %lu us"), s + 1,
       (unsigned long)(plug_ms() - plug_ci.ms0), (unsigned long)plug_ci.passes, (unsigned long)plug_cyc_us(plug_ci.cyc_max),
       (unsigned long)plug_cyc_us(ARM_DWT_CYCCNT - c1));
  if (st == RMP_OK) plug_store_changed(id);
  if (st != RMP_OK || strncmp(id, w.id, 32)) {
    logf(PSTR("plugins: U%d card copy failed (%d, %s)"), s + 1, st, id);
    if (plug_ans[s] == RMP_OP_INSTALL) plug_answer(s, RMP_RES_BAD);   // (not after LOADING went out)
    return true;
  }
  logf(PSTR("plugins: U%d %s installed from the card"), s + 1, id);
  return true;
}
// A 1E whose slot has settled (saved, loaded, initialised, described): its answer. A save that failed
// answered BAD at once (plug_save_failed), a failed card copy too; an install whose slot the project
// change took meanwhile: READY (the package is in). kPlugAnsMs after the 1E (the Rytm waits 2 s): the
// map still not changed - BAD, or LOADING while a save still runs; a load or a card copy still under way -
// LOADING (a clear: READY). Called from plug_deadline_pass, ahead of the reload, load and save passes.
FLASHMEM bool plug_answer_pass() {
  for (int s = 0; s < RMP_SLOTS; ++s) {
    if (plug_ans[s] < 0) continue;
    bool late = plug_ms() - plug_ans_t[s] > kPlugAnsMs;
    if (plug_ci.on && plug_ci.s == s) {   // its card copy (paused while the transport runs)
      if (!late) continue;
      plug_answer(s, RMP_RES_LOADING);
      return true;
    }
    if (plug_saving() && !late) continue;
    const rmp_slot_entry& e = plug_map.slot[s];
    if (strncmp(e.id, plug_ans_want[s].id, 32) || e.version != plug_ans_want[s].version) {
      if (plug_ans[s] == RMP_OP_INSTALL) {   // copied and committed (a failure answered BAD), the project changed since
        plug_answer(s, RMP_RES_READY);
        return true;
      }
      if (!late) continue;
      // A save still running: it may yet land - LOADING. That answer ends the 1E (plug_ans cleared), so if the save
      // then fails, plug_save_failed has no 1E to answer BAD: the failure shows only through the slot's 1A (which
      // still names the old machine, as the map is unchanged).
      plug_answer(s, plug_saving() ? RMP_RES_LOADING : RMP_RES_BAD);
      return true;
    }
    // settled: loaded, described, its preset folder scanned and its card lookup done - what plug_meta_poll
    // waits for before it fingerprints the slot, so the answer carries the generation of the slot's next 1A
    if (plug[s].state == RMP_SLOT_LOADING || (plug_todo >> s & 1) || plug_act_slot == s ||
        (plug_job_on && plug_job_slot == s) || (plug_desc_todo >> s & 1) || (plug_scan_todo >> s & 1) ||
        plug_scan_slot == s || (plug_card_todo >> s & 1)) {
      if (!late) continue;
      if (plug_ans[s] == RMP_OP_CLEAR) plug_answer(s, RMP_RES_READY);
      else plug_answer(s, RMP_RES_LOADING);
      return true;
    }
    int st = plug_pstate(s);   // what 1A says: an assigned slot in safe mode is MUTED - BAD
    int r = RMP_RES_BAD;   // (if, not a ?: chain: GCC's lookup table for that went to DTCM)
    if (!e.id[0] || st == RMP_PST_READY) r = RMP_RES_READY;
    else if (st == RMP_PST_MISSING) r = RMP_RES_NOT_INSTALLED;
    else if (st == RMP_PST_NO_ROOM) r = RMP_RES_NO_ROOM;
    plug_answer(s, r);
    return true;
  }
  return false;
}
// 'Z' <len> <id> (development, tools/rmp_install.py remove): a package out of the store; the slots naming
// it become MISSING.
FLASHMEM void plug_remove(const uint8_t* p, int len) {
  char id[32] = {0};
  if (len < 1 || len > 31) {
    logf(PSTR("plugins: cannot remove a %d-byte id"), len);
    return;
  }
  memcpy(id, p, len);
  if (plug_ci.on) {   // a card copy (1E install) holds the store, as for 'I'
    logf(PSTR("plugins: cannot remove %s (a card copy runs)"), id);
    return;
  }
  bool ok = rmp_store_remove(id);
  logf(ok ? PSTR("plugins: removed %s") : PSTR("plugins: cannot remove %s"), id);
  if (ok) plug_store_changed(id);
}
// 'F' "WIPE" (development, tools/rmp_install.py wipe): the store formatted - the explicit step a "foreign store
// layout" at boot waits for (rmp_store.h). Every package is lost; the project's slots load again (MISSING).
// Not while the transport runs, an install is open (the erases stall the flash ~0.25-0.28 s each), a slot loads or the
// info pass ('D') iterates the store.
FLASHMEM bool plug_info_on();
FLASHMEM void plug_wipe() {
  if (plug_ci.on || plug_installing) {
    logf(PSTR("plugins: cannot wipe the store (an install runs)"));
    return;
  }
  if (plug_info_on()) {   // the info pass ('D') iterates the store's files
    logf(PSTR("plugins: cannot wipe the store (the info pass runs)"));
    return;
  }
  if (plug_transport_running()) {
    logf(PSTR("plugins: cannot wipe the store (STOP TO INSTALL: the transport runs)"));
    return;
  }
  for (int s = 0; s < RMP_SLOTS; ++s)   // a load reads its package from the store: not under it
    if (plug[s].state == RMP_SLOT_LOADING || plug_act_slot == s || (plug_job_on && plug_job_slot == s)) {
      logf(PSTR("plugins: cannot wipe the store (U%d is loading)"), s + 1);
      return;
    }
  uint32_t c0 = ARM_DWT_CYCCNT;
  bool ok = rmp_store_wipe();
  logf(ok ? PSTR("plugins: store wiped (%lu ms)") : PSTR("plugins: store wipe FAILED (%lu ms)"),
       (unsigned long)(plug_cyc_us(ARM_DWT_CYCCNT - c0) / 1000));
  rmp_meta_store(&plug_meta);
  plug_inst_phase = 0;
  plug_reload_pending = true;   // every slot again, from the empty store
}

// 'E' <len> <id> (development, tools/rmp_install.py export): a package from the store to the card's
// /MACHINES/<id>-<a.b.c>.rmp (what a 1E install copies back) and to the Mac as log lines "export: <id> <offset>
// <hex>", 64 bytes a line, then "export: <id> done <size> <crc32> <a.b.c> card <ok|failed|none|skipped|kept>" (none:
// no card; skipped: the package's header does not verify; kept: a <path>.tmp from an earlier export's failed rename is
// there, the card left alone until it is dealt with). The card copy is written as <path>.tmp and renamed over <path>
// only when complete (a failed write removes the .tmp, the old copy stays; a failed remove of the old copy or a failed
// rename keeps the .tmp, logged with the step). One call: a 64 KB package is
// ~1000 lines, the watchdog fed and plug_ms() called every 1 KB. Development only, with the transport stopped: the
// call holds the loop for the whole package (no audio requests answered meanwhile).
FLASHMEM void plug_export(const uint8_t* p, int len) {
  char id[32] = {0};
  if (len < 1 || len > 31) {
    logf(PSTR("export: cannot export a %d-byte id"), len);
    return;
  }
  memcpy(id, p, len);
  uint32_t n = 0;
  uint8_t b[64];
  rmp_info info;
  if (!rmp_store_size(id, &n) || n < RMP_HEADER_BYTES) {
    rmp_store_release();
    logf(PSTR("export: cannot export %s (not installed)"), id);
    return;
  }
  uint8_t h[RMP_HEADER_BYTES];
  bool hdr = rmp_store_read_at(id, 0, h, sizeof h) && rmp_verify_header(h, n, &info) == RMP_OK;
  uint32_t ver = hdr ? info.version : 0;
  char path[64], tmp[68];
  sniprintf(path, sizeof path, PSTR("/MACHINES/%s-%d.%d.%d.rmp"), id, (int)(ver >> 16), (int)((ver >> 8) & 255), (int)(ver & 255));
  sniprintf(tmp, sizeof tmp, PSTR("%s.tmp"), path);
  FsFile f;
  if (hdr && card_ok) cards.sd().mkdir(PSTR("/MACHINES"), true);   // (false when it exists)
  bool kept = hdr && card_ok && cards.sd().exists(tmp);   // a kept complete copy: never truncated or removed here
  if (kept) logf(PSTR("export: %s: %s is there (an earlier export kept it): the card is left alone"), id, tmp);
  bool card = hdr && card_ok && !kept && f.open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
  bool wrote = card;
  uint32_t crc = 0xFFFFFFFFu;
  bool ok = true;
  const char* hx = PSTR("0123456789abcdef");
  for (uint32_t off = 0; off < n && ok; off += sizeof b) {
    uint32_t k = n - off < sizeof b ? n - off : (uint32_t)sizeof b;
    ok = rmp_store_read_at(id, off, b, k);
    if (!ok) break;
    crc = rmp_crc32_update(crc, b, k);
    if (wrote && f.write(b, k) != k) wrote = false;
    char hex[2 * sizeof b + 1];
    for (uint32_t i = 0; i < k; ++i) {
      hex[2 * i] = hx[b[i] >> 4];
      hex[2 * i + 1] = hx[b[i] & 15];
    }
    hex[2 * k] = 0;
    logf(PSTR("export: %s %lu %s"), id, (unsigned long)off, hex);
    if ((off & 1023) == 0) {
      rmp_guard_feed();
      plug_ms();   // (the whole export is one loop pass: CYCCNT's wraps)
    }
  }
  rmp_store_release();
  if (card) {
    wrote = f.sync() && wrote && ok;
    f.close();
    // the complete copy replaces the old one (SdFat's rename does not overwrite); anything less is removed. A
    // complete copy whose rename fails stays as the .tmp (the old one may be gone already)
    if (!wrote) {
      cards.sd().remove(tmp);
    } else if (cards.sd().exists(path) && !cards.sd().remove(path)) {
      wrote = false;
      logf(PSTR("export: %s: removing the old %s failed, the complete copy kept as %s"), id, path, tmp);
    } else if (!cards.sd().rename(tmp, path)) {
      wrote = false;
      logf(PSTR("export: %s: the rename failed, the complete copy kept as %s"), id, tmp);
    }
  }
  if (!ok) {
    logf(PSTR("export: cannot export %s (a store read failed)"), id);
    return;
  }
  logf(PSTR("export: %s done %lu %08lx %d.%d.%d card %s"), id, (unsigned long)n, (unsigned long)~crc, (int)(ver >> 16),
       (int)((ver >> 8) & 255), (int)(ver & 255),
       card ? (wrote ? PSTR("ok") : PSTR("failed")) : !card_ok ? PSTR("none") : !hdr ? PSTR("skipped") :
       kept ? PSTR("kept") : PSTR("failed"));
  if (wrote)
    for (int s = 0; s < RMP_SLOTS; ++s)   // a package came to the card: the MISSING slots look again
      if (plug[s].state == RMP_SLOT_MISSING) plug_card_todo |= 1u << s;
}

// The development install / assign frames ('I' 'J' 'K' 'U', from poll_input).
// A package was installed (serial 'K', or a card copy) or removed ('Z'): the store's generation moves on
// (the installed list is built again), and the slots naming it load afresh one at a time (the others
// keep playing) - or all at the pending reload / not at all in safe mode.
FLASHMEM void plug_store_changed(const char* id) {
  rmp_meta_store(&plug_meta);
  plug_inst_phase = 0;   // a list being built is stale: start over
  if (plug_off || plug_reload_pending) {
    plug_reload_pending = true;
    return;
  }
  for (int s = 0; s < RMP_SLOTS; ++s) {
    if (strncmp(plug_map.slot[s].id, id, 32)) continue;
    plug_arena_dirty = true;
    plug_todo_slot(s);
  }
}
uint32_t plug_inst_ms0 = 0, plug_inst_wmax = 0;   // a serial install's start (plug_ms) and longest 'J' write (cycles)
FLASHMEM void plug_install_begin(uint32_t total) {
  if (plug_ci.on) {   // a card copy (1E install) holds the store's one install
    plug_installing = false;
    logf(PSTR("plugins: install failed (a card copy runs)"));
    return;
  }
  plug_inst_ms0 = plug_ms();
  plug_inst_wmax = 0;
  plug_installing = rmp_store_install_begin(total);
  if (!plug_installing) logf(PSTR("plugins: install failed (begin, %lu B)"), (unsigned long)total);
}
FLASHMEM void plug_install_abort(const char* why) {
  if (plug_installing) rmp_store_install_abort();
  plug_installing = false;
  logf(PSTR("plugins: install failed (%s)"), why);
}
FLASHMEM void plug_install_write(const uint8_t* p, int n) {
  if (!plug_installing) return;   // its failure was logged
  uint32_t c0 = ARM_DWT_CYCCNT;
  if (!rmp_store_install_write(p, n)) plug_install_abort("write");
  uint32_t dc = ARM_DWT_CYCCNT - c0;
  if (dc > plug_inst_wmax) plug_inst_wmax = dc;
}
FLASHMEM void plug_install_commit() {
  char id[32] = {0};
  uint32_t c0 = ARM_DWT_CYCCNT;   // across the flash erases (micros() stops while they mask interrupts)
  if (!plug_installing) {   // its failure was logged; a card copy's install is not this one's to commit
    logf(PSTR("plugins: install failed (not begun)"));
    return;
  }
  plug_installing = false;
  rmp_status st = rmp_store_install_commit(id);
  if (st == RMP_OK) {
    logf(PSTR("plugins: installed %s (commit %lu us, longest write %lu us, %lu ms since the begin)"), id,
         (unsigned long)plug_cyc_us(ARM_DWT_CYCCNT - c0), (unsigned long)plug_cyc_us(plug_inst_wmax),
         (unsigned long)(plug_ms() - plug_inst_ms0));
    plug_store_changed(id);
  } else {
    logf(PSTR("plugins: install failed %d"), (int)st);
  }
}
// The map as the project's slot file: <name>.tmp written and synced, the old file
// removed, the .tmp renamed over it (plug_reload finishes a rename a reset
// interrupted). The SD operations take 1-2 ms each, so it runs in three loop passes
// (write+sync, remove, rename: plug_save_pass) between requests; the map in use
// (plug_map) changes only once the rename has succeeded (plug_assign_apply). A new save, a reload or a project change first
// finishes a pending one (plug_save_finish).
struct PlugSave {
  bool on; int phase; bool dir; char fn[48], tmp[52], txt[RMP_SLOTS * 48]; uint32_t n; uint32_t us;
  int retries;
  rmp_slot_map map; int slot;   // the map being saved and the slot it changes (-1: any, a resolve): applied (plug_map) only when the rename has succeeded
  rmp_slot_notes notes; char nfn[48], ntxt[RMP_SLOTS * 96]; uint32_t nn;   // the notes, written whole in phase 0 (advisory)
  bool ans;                     // a 1E's save (plug_do_assign): its failure answers that 1E (plug_save_failed)
};
PlugSave plug_sv;
FLASHMEM bool plug_saving() { return plug_sv.on; }
FLASHMEM void plug_save_for_1e() { plug_sv.ans = true; }
// The saved map becomes the map in use. Only the assigned slot loads (appended to the arena); the
// others keep their images and voices. The same id and version on a slot that plays already: nothing.
FLASHMEM void plug_assign_apply() {
  int sl = plug_sv.slot;
  plug_notes = plug_sv.notes;
  if (sl < 0) {   // a resolve: every slot whose machine changed loads again (appended), the others keep playing
    rmp_slot_map old = plug_map;
    plug_map = plug_sv.map;
    plug_state_reloaded = false;
    for (int s = 0; s < RMP_SLOTS; ++s) {
      const rmp_slot_entry &o = old.slot[s], &e = plug_map.slot[s];
      if (!strncmp(o.id, e.id, 32) && o.version == e.version) continue;
      logf(PSTR("plugins: U%d = %s (%s%s)"), s + 1, e.id[0] ? e.id : "-", rmp_src_name(plug_notes.slot[s].src),
           plug_notes.slot[s].flags & RMP_RF_UPDATED ? PSTR(": updated from the project's sounds") : PSTR(""));
      if (plug_off) plug_reload_pending = true;
      else { plug_arena_dirty = true; plug_todo_slot(s); }
    }
    return;
  }
  const rmp_slot_entry& o = plug_map.slot[sl];
  const rmp_slot_entry& e = plug_sv.map.slot[sl];
  bool same = !strncmp(o.id, e.id, 32) && o.version == e.version;
  bool playing = plug[sl].state == RMP_SLOT_READY || plug[sl].state == RMP_SLOT_LOADING;
  plug_map = plug_sv.map;
  plug_state_reloaded = false;
  if (e.id[0])
    logf(PSTR("plugins: U%d = %s %d.%d.%d (%s)"), sl + 1, e.id, (int)(e.version >> 16), (int)((e.version >> 8) & 255), (int)(e.version & 255), plug_sv.fn);
  else
    logf(PSTR("plugins: U%d cleared (%s)"), sl + 1, plug_sv.fn);
  if (plug_off) {
    plug_reload_pending = true;   // says "safe mode"
  } else if (same && (playing || !e.id[0])) {
    if (e.id[0] && plug[sl].state == RMP_SLOT_READY)
      logf(PSTR("plugins: U%d %s ready (unchanged, already loaded)"), sl + 1, e.id);
    else if (!e.id[0])
      ;   // an empty slot stays empty
  } else {
    plug_arena_dirty = true;
    plug_todo_slot(sl);
  }
}
// A save that failed (the map unchanged): the 1E assign / clear it was for is answered BAD now (only that
// 1E's own save: a serial 'U' on the slot meanwhile is not its answer).
FLASHMEM void plug_save_failed() {
  int s = plug_sv.slot;
  plug_sv.on = false;
  if (plug_sv.ans && s >= 0 && s < RMP_SLOTS && (plug_ans[s] == RMP_OP_ASSIGN || plug_ans[s] == RMP_OP_CLEAR))
    plug_answer(s, RMP_RES_BAD);
}
FLASHMEM void plug_save_fail(const char* what) {
  cards.sd().remove(plug_sv.tmp);
  logf(PSTR("plugins: cannot write %s (%s)"), plug_sv.tmp, what);
  plug_save_failed();
}
// The notes, once the map is in place (never beside an old map: an "empty" note there would be stale). Whole, no
// rename (advisory: a torn one is skipped line by line); not written when they did not fit.
FLASHMEM void plug_save_notes() {
  if (!plug_sv.nn) {
    bool any = false;
    for (int s = 0; s < RMP_SLOTS; ++s) any = any || plug_sv.notes.slot[s].src || plug_sv.notes.slot[s].fp;
    if (any) { logf(PSTR("plugins: %s does not fit: not written"), plug_sv.nfn); return; }
  }
  FsFile g;
  if (g.open(plug_sv.nfn, O_WRONLY | O_CREAT | O_TRUNC)) {
    if (plug_sv.nn) g.write(plug_sv.ntxt, plug_sv.nn);
    g.close();
  }
}
FLASHMEM void plug_save_pass() {
  if (!plug_sv.on) return;
  uint32_t t0 = micros();
  if (plug_sv.phase == 0) {
    if (!plug_sv.dir) { cards.sd().mkdir("/.rytm/slots"); plug_sv.dir = true; }
    FsFile f;
    if (!f.open(plug_sv.tmp, O_WRONLY | O_CREAT | O_TRUNC)) {
      logf(PSTR("plugins: cannot write %s"), plug_sv.tmp);
      plug_save_failed();
      return;
    }
    size_t w = plug_sv.n ? f.write(plug_sv.txt, plug_sv.n) : 0;
    bool ok = w == plug_sv.n && f.sync();
    f.close();
    if (!ok) { plug_save_fail("short write"); return; }
  } else if (plug_sv.phase == 1) {
    if (cards.sd().exists(plug_sv.fn) && !cards.sd().remove(plug_sv.fn)) {
      cards.sd().remove(plug_sv.tmp);   // the old map is still the file: nothing changes
      logf(PSTR("plugins: cannot replace %s: the slot map is unchanged"), plug_sv.fn);
      plug_save_failed();
      return;
    }
  } else {
    if (!cards.sd().rename(plug_sv.tmp, plug_sv.fn)) {
      // the old file is gone: the .tmp is the stored map now (plug_reload recovers it at the next
      // load). Retry the rename on the next passes; then apply it anyway - it is what the card holds.
      if (++plug_sv.retries < 3) { plug_sv.us += micros() - t0; return; }
      logf(PSTR("plugins: cannot rename %s: the new map stays in that file (applied)"), plug_sv.tmp);
      plug_save_notes();
      plug_sv.on = false;
      plug_assign_apply();
      return;
    }
    plug_save_notes();
    plug_sv.on = false;
    plug_sv.us += micros() - t0;
    logf(PSTR("plugins: %s saved (%lu us in 3 passes)"), plug_sv.fn, (unsigned long)plug_sv.us);
    plug_assign_apply();
    return;
  }
  plug_sv.us += micros() - t0;
  if (plug_sv.on) ++plug_sv.phase;
}
FLASHMEM void plug_save_finish() { while (plug_sv.on) plug_save_pass(); }
// Queue the map's save; false when it cannot be (no card, does not fit).
FLASHMEM bool plug_save_map(const rmp_slot_map* m, const rmp_slot_notes* n, const char* fn, int slot) {
  plug_save_finish();
  if (!card_ok) {
    logf(PSTR("plugins: no card, %s not written"), fn);
    return false;
  }
  plug_file(plug_sv.tmp, sizeof plug_sv.tmp, true);
  strncpy(plug_sv.fn, fn, sizeof plug_sv.fn - 1);
  plug_sv.fn[sizeof plug_sv.fn - 1] = 0;
  plug_sv.n = rmp_slots_format(m, plug_sv.txt, sizeof plug_sv.txt);
  plug_sv.map = *m; plug_sv.slot = slot;
  plug_sv.notes = *n;
  plug_notes_file(plug_sv.nfn, sizeof plug_sv.nfn);
  plug_sv.nn = rmp_notes_format(n, plug_sv.ntxt, sizeof plug_sv.ntxt);
  plug_fresh = false;
  plug_sv.phase = 0; plug_sv.us = 0; plug_sv.retries = 0; plug_sv.ans = false; plug_sv.on = true;
  return true;
}
// False when refused (the map unchanged; logged); true when the save is queued.
FLASHMEM bool plug_assign(int slot, const uint8_t* id, int len, const uint8_t* ver) {
  if (plug_reload_pending) plug_reload();   // edit the current project's map, not a stale one
  rmp_slot_entry e;
  memset(&e, 0, sizeof e);
  if (slot < 1 || slot > RMP_SLOTS || len > 31) {
    logf(PSTR("plugins: assign U%d refused"), slot);
    return false;
  }
  memcpy(e.id, id, len);
  e.version = (uint32_t)ver[0] << 16 | ver[1] << 8 | ver[2];
  if (len && !rmp_id_ok(e.id)) {
    logf(PSTR("plugins: assign U%d refused: unsafe id"), slot);
    return false;
  }
  plug_save_finish();   // a pending save applies first: this one builds on its map
  rmp_slot_map m = plug_map;
  rmp_slot_notes nt = plug_notes;
  rmp_resolve_set(&m, &nt, slot - 1, e.id, e.version, RMP_SRC_SAVED);   // the user's: saved (empty: explicitly)
  char fn[48];
  plug_file(fn, sizeof fn);
  // saved in three passes; the map is applied (and the slot loads) when the rename has
  // succeeded, a failure is logged and changes nothing (plug_assign_apply)
  return plug_save_map(&m, &nt, fn, slot - 1);
}
// An imported slot list's entry (web console plugin=put): the package need not be installed (MISSING by name until it
// is); id "" explicitly empty. False when refused.
FLASHMEM bool plug_import(int slot, const char* id, uint32_t version, const char* name) {
  if (plug_reload_pending) plug_reload();
  if (slot < 1 || slot > RMP_SLOTS || (id[0] && !rmp_id_ok(id))) return false;
  plug_save_finish();
  rmp_slot_map m = plug_map;
  rmp_slot_notes nt = plug_notes;
  rmp_resolve_import(&m, &nt, slot - 1, id, version, 0, name);
  char fn[48];
  plug_file(fn, sizeof fn);
  return plug_save_map(&m, &nt, fn, -1);
}
// "Start this project empty": every slot explicitly empty.
FLASHMEM bool plug_start_empty() {
  if (plug_reload_pending) plug_reload();
  plug_save_finish();
  rmp_slot_map m = plug_map;
  rmp_slot_notes nt = plug_notes;
  for (int s = 0; s < RMP_SLOTS; ++s) rmp_resolve_set(&m, &nt, s, "", 0, RMP_SRC_SAVED);
  char fn[48];
  plug_file(fn, sizeof fn);
  return plug_save_map(&m, &nt, fn, -1);
}
// The installed packages as the resolver sees them (the Rytm's list; its version 0: unreadable, left out).
FLASHMEM int plug_refs(rmp_pkg_ref* r) {
  int n = 0;
  for (int i = 0; i < plug_inst_n; ++i)
    if (plug_inst[i].version) r[n++] = rmp_pkg_ref{plug_inst[i].id, plug_inst[i].version, rmp_plugin_fp(plug_inst[i].id), plug_inst[i].name};
  return n;
}
// One pass of the resolution (from loop(), after the slots pass): a 0x25 for this project once the installed list
// is current, a fresh project's inheritance at its deadline, a trig's tag (an older Rytm). True when it did something.
FLASHMEM bool plug_resolve_pass() {
  if (plug_reload_pending || plug_sv.on || plug_off) return false;
  bool list_ok = !plug_inst_phase && plug_inst_gen == plug_meta.store_gen && plug_inst_gen;
  rmp_slot_map m = plug_map;
  rmp_slot_notes nt = plug_notes;
  uint32_t ch = 0;
  bool save = false;
  if (list_ok && plug_res_gen != plug_inst_gen) {   // the installed packages changed: open slots again
    plug_res_gen = plug_inst_gen;
    memset(plug_lazy_fp, 0, sizeof plug_lazy_fp);
    uint32_t open = rmp_resolve_open_slots(&nt);
    if (plug_prints_have && open && !plug_prints_pending) {
      rmp_pkg_ref r[32];
      ch = rmp_resolve_prints(&m, &nt, &plug_prints, r, plug_refs(r), open);
      if (ch) logf(PSTR("plugins: %d slot%s resolved with the new packages"), __builtin_popcount(ch), __builtin_popcount(ch) == 1 ? "" : "s");
      save = ch != 0;
    } else {
      return false;
    }
  } else if (plug_prints_pending && list_ok) {
    plug_prints_pending = false;
    uint32_t h = rmp_prints_hash(&plug_prints);
    if (plug_prints_have && h == plug_prints_hash) return true;   // a repeat
    plug_prints_have = true;
    plug_prints_hash = h;
    rmp_pkg_ref r[32];
    ch = rmp_resolve_prints(&m, &nt, &plug_prints, r, plug_refs(r));
    logf(PSTR("plugins: slot prints (seq %d, %d entries) for %s: %d slot%s from the sounds"), (int)plug_prints.seq,
         (int)plug_prints.n, project, __builtin_popcount(ch), __builtin_popcount(ch) == 1 ? "" : "s");
    if (plug_fresh) ch |= rmp_resolve_inherit(&m, &nt, &plug_prev), plug_fresh = false;
    save = ch != 0;
  } else if (plug_fresh && (int32_t)(millis() - plug_fresh_until) >= 0 && !plug_prints_pending) {
    plug_fresh = false;
    ch = rmp_resolve_inherit(&m, &nt, &plug_prev);
    if (ch) logf(PSTR("plugins: %s inherits %d slot%s from the previous project"), project, __builtin_popcount(ch), __builtin_popcount(ch) == 1 ? "" : "s");
    save = ch != 0;
  } else if (list_ok && !plug_prints_have) {
    int s = 0;
    while (s < RMP_SLOTS && !plug_lazy_req[s]) ++s;
    if (s == RMP_SLOTS) return false;
    uint16_t fp = plug_lazy_req[s];
    plug_lazy_req[s] = 0;
    plug_lazy_fp[s] = fp;
    rmp_pkg_ref r[32];
    if (rmp_resolve_tag(&m, &nt, s, fp, r, plug_refs(r))) {
      ch = 1u << s, save = true;
      logf(PSTR("plugins: U%d = %s from a trig's sound tag %d"), s + 1, m.slot[s].id, (int)fp);
    } else if (nt.slot[s].flags & (RMP_RF_UNKNOWN | RMP_RF_COLLISION)) {
      logf(PSTR("plugins: U%d: sound tag %d %s"), s + 1, (int)fp, nt.slot[s].flags & RMP_RF_COLLISION ?
           PSTR("names several installed machines") : PSTR("names no installed machine"));
    }
  } else {
    return false;
  }
  if (!save) { plug_notes = nt; return true; }   // the notes only (the UI's)
  char fn[48];
  plug_file(fn, sizeof fn);
  plug_save_map(&m, &nt, fn, -1);
  return true;
}
// The Rytm waits 2 s for a 1E's answer, so the 1Es come ahead of a project's reload, a slot's load and a
// map save (a 10 <project> precedes every 1E: its reload and the slot loads it queues would hold a 1E
// behind them): from loop() right after project_save, a few compares when nothing waits. A queued 1E
// runs (after the save under way: one step of it a pass), the 1E's own map save goes on a step a pass, and
// a waiting answer goes out once it has settled or its deadline came (plug_answer_pass). True when it did
// something.
FLASHMEM bool plug_deadline_pass() {
  for (int s = 0; s < RMP_SLOTS; ++s)
    if (plug_1e_on[s]) {
      if (plug_saving()) {
        plug_save_pass();
        return true;
      }
      plug_1e_on[s] = false;
      plug_do_assign(plug_1e_q[s]);
      return true;
    }
  if (plug_sv.on && plug_sv.ans) {
    plug_save_pass();
    return true;
  }
  for (int s = 0; s < RMP_SLOTS; ++s)
    if (plug_ans[s] >= 0) return plug_answer_pass();
  return false;
}


// ---------------------------------------------------------------------------
// The device-info report ('D'): one JSON document for the companion / installer, sent as log
// lines "info: <i>/<n> <chunk>" (the chunks, joined, are the JSON). It is built over several loop
// passes between requests (each pass one store/SD operation of at most a few ms, the same as a slot
// load) and sent a few lines a pass; plug_info_pass is called from loop().
struct PlugInfo {
  bool on;
  int stage, idx, npk, nsd, lines, sent, passes;
  bool more, sd_more, listed;   // listed: the packages from the installed list (plug_inst), no store opens
  uint32_t t_max, t_start, t_stage[6];   // t_stage: the longest pass of each stage (us)
  char pk_id[12][32];
  uint32_t pk_size[12], pk_ver[12];
  char sd_name[12][32];
};
PlugInfo plug_inf;
FLASHMEM bool plug_info_on() { return plug_inf.on; }
DMAMEM char plug_inf_buf[3584];
int plug_inf_len;
void ia(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int room = (int)sizeof plug_inf_buf - plug_inf_len;
  int n = vsniprintf(plug_inf_buf + plug_inf_len, room, fmt, ap);
  va_end(ap);
  plug_inf_len += n < 0 ? 0 : n >= room ? room - 1 : n;
}
const char* plug_state_name(rmp_slot_state st) {
  static const char* nm[] = {"EMPTY", "READY", "MISSING", "BAD", "NO_ROOM", "MUTED", "LOADING"};
  return (int)st < 7 ? nm[(int)st] : "?";
}
FLASHMEM void plug_info_start() {
  memset(&plug_inf, 0, sizeof plug_inf);
  plug_inf.on = true;
  plug_inf.t_start = micros();
  plug_inf_len = 0;
}
// A string as JSON text: ids and file names are printable ASCII without '"' or '\\' (rmp_id_ok; the SD
// names are filtered here).
FLASHMEM void plug_info_str(const char* t) {
  ia(PSTR("\""));
  for (; *t; ++t) ia(PSTR("%c"), (*t == '"' || *t == '\\' || *t < 0x20 || *t > 0x7e) ? '?' : *t);
  ia(PSTR("\""));
}
FLASHMEM void plug_info_pass() {
  if (!plug_inf.on) return;
  uint32_t t0 = micros();
  PlugInfo& I = plug_inf;
  const int st0 = I.stage;
  if (I.stage == 0) {   // in RAM: the core, the arena, the project and the slots
    ia(PSTR("{\"info\":1,\"fw\":\"" RYTM_CORE_VERSION "\",\"api\":%d,\"formats\":[%d,%d],\"psram_mb\":%d,"), (int)RMP_API,
       (int)RMP_FORMAT_MIN, (int)RMP_FORMAT, (int)external_psram_size);
    uint32_t cap = rmp_state_capacity();
    ia(PSTR("\"state_limit\":%lu,\"state_psram\":%lu,\"itcm_blocks\":%lu,"), (unsigned long)(cap > sizeof shared[0] ? cap : sizeof shared[0]),
       (unsigned long)cap, (unsigned long)(uintptr_t)&_itcm_block_count);
    ia(PSTR("\"arena\":{\"size\":%lu,\"used\":%lu,\"psram_size\":%lu,\"psram_used\":%lu},\"project\":"), (unsigned long)RMP_ARENA_BYTES,
       (unsigned long)rmp_arena_used(), (unsigned long)rmp_arena_psram_size(), (unsigned long)rmp_arena_psram_used());
    plug_info_str(project);
    ia(PSTR(",\"slots\":["));
    for (int s = 0; s < RMP_SLOTS; ++s) {
      const rmp_slot_entry& e = plug_map.slot[s];
      uint32_t n = plug_cyc_n[s];
      uint32_t avg = n ? (uint32_t)(plug_cyc_sum[s] / n) : 0;
      ia(PSTR("%s{\"u\":%d,\"id\":"), s ? "," : "", s + 1);
      plug_info_str(e.id);
      ia(PSTR(",\"ver\":\"%d.%d.%d\",\"state\":\"%s\""), (int)(e.version >> 16), (int)((e.version >> 8) & 255), (int)(e.version & 255),
         plug_state_name(plug[s].state));
      char why[64];
      plug_why_text(s, why, sizeof why);
      if (why[0]) ia(PSTR(",\"why\":")), plug_info_str(why);
      if (plug[s].state == RMP_SLOT_READY || plug[s].state == RMP_SLOT_MUTED)   // the image's arena (rmp_place_name)
        ia(PSTR(",\"place\":\"%s\""), rmp_place_name(plug_place[s]));
      if (plug[s].state == RMP_SLOT_READY || plug[s].state == RMP_SLOT_MUTED)   // declared, and measured per 12-sample block (150000 cycles = 100 %)
        ia(PSTR(",\"permille\":%lu,\"avg_cyc\":%lu,\"peak_cyc\":%lu,\"calls\":%lu,\"avg_permille\":%lu,\"peak_permille\":%lu"),
           (unsigned long)plug_permille[s], (unsigned long)avg, (unsigned long)plug_cyc_peak[s], (unsigned long)n,
           (unsigned long)(avg / 150u), (unsigned long)(plug_cyc_peak[s] / 150u));
      ia(PSTR("}"));
    }
    ia(PSTR("],"));
    I.stage = 1;
  } else if (I.stage == 1) {   // the packages in flash: the installed list's when it is current, else the
                               // directory open, then one entry a pass
    if (!I.idx && !plug_inst_phase && plug_inst_gen == plug_meta.store_gen) {
      for (int i = 0; i < plug_inst_n && I.npk < 12; ++i, ++I.npk) {
        memcpy(I.pk_id[I.npk], plug_inst[i].id, 32);
        I.pk_size[I.npk] = plug_inst[i].size;
        I.pk_ver[I.npk] = plug_inst[i].version;
      }
      I.more = plug_inst_n > 12 || plug_inst_over;
      I.listed = true;
      I.stage = 2;
    } else if (!I.idx) {
      rmp_store_iter_begin();
      I.idx = 1;
    } else if (I.npk < 12) {
      if (rmp_store_iter_next(I.pk_id[I.npk], &I.pk_size[I.npk])) ++I.npk;
      else I.stage = 2;
    } else {
      I.more = true;   // (a 13th package is not listed)
      rmp_store_iter_end();
      I.stage = 2;
    }
  } else if (I.stage == 2) {   // the store's size (usedSize walks the file system)
    uint32_t tot = 0, used = 0;
    bool ok = rmp_store_space(&tot, &used);
    ia(PSTR("\"store\":{\"size\":%lu,\"free\":%lu,\"pkgs\":["), (unsigned long)tot, (unsigned long)(tot - used));
    (void)ok;
    I.stage = 3;
    I.idx = 0;
  } else if (I.stage == 3) {   // one package a pass: its header gives the version (an open, 0.1-0.4 ms)
    if (I.listed) {   // the installed list's versions (0: its header did not verify)
      for (int k = 0; k < I.npk; ++k) {
        ia(PSTR("%s{\"id\":"), k ? "," : "");
        plug_info_str(I.pk_id[k]);
        uint32_t v = I.pk_ver[k];
        if (v) ia(PSTR(",\"ver\":\"%d.%d.%d\""), (int)(v >> 16), (int)((v >> 8) & 255), (int)(v & 255));
        ia(PSTR(",\"size\":%lu}"), (unsigned long)I.pk_size[k]);
      }
      I.idx = I.npk;
    }
    if (I.idx < I.npk) {
      uint8_t h[RMP_HEADER_BYTES];
      rmp_info inf;
      bool ok = rmp_store_read_at(I.pk_id[I.idx], 0, h, sizeof h) && rmp_verify_header(h, I.pk_size[I.idx], &inf) == RMP_OK;
      ia(PSTR("%s{\"id\":"), I.idx ? "," : "");
      plug_info_str(I.pk_id[I.idx]);
      if (ok) ia(PSTR(",\"ver\":\"%d.%d.%d\""), (int)(inf.version >> 16), (int)((inf.version >> 8) & 255), (int)(inf.version & 255));
      ia(PSTR(",\"size\":%lu}"), (unsigned long)I.pk_size[I.idx]);
      ++I.idx;
    } else {
      rmp_store_release();
      ia(PSTR("],\"more\":%s},\"sd\":%s,\"machines\":["), I.more ? "true" : "false", card_ok ? "true" : "false");
      I.stage = 4;
    }
  } else if (I.stage == 4) {   // the card's /MACHINES (at most 12 names)
    FsFile d, e;
    if (card_ok && d.open("/MACHINES", O_RDONLY) && d.isDir()) {
      char nm[40];
      while (e.openNext(&d, O_RDONLY)) {
        e.getName(nm, sizeof nm);
        bool dir = e.isDir();
        e.close();
        if (dir || nm[0] == '.') continue;
        if (I.nsd < 12) strncpy(I.sd_name[I.nsd++], nm, 31); else { I.sd_more = true; break; }
      }
      d.close();
    }
    for (int i = 0; i < I.nsd; ++i) {
      ia(i ? "," : "");
      plug_info_str(I.sd_name[i]);
    }
    ia(PSTR("],\"sd_more\":%s,\"build_us\":%lu,\"passes\":%d,\"pass_max_us\":%lu,\"stage_us\":[%lu,%lu,%lu,%lu,%lu]}"),
       I.sd_more ? "true" : "false", (unsigned long)(micros() - I.t_start), I.passes + 1,
       (unsigned long)(I.t_max > micros() - t0 ? I.t_max : micros() - t0), (unsigned long)I.t_stage[0], (unsigned long)I.t_stage[1],
       (unsigned long)I.t_stage[2], (unsigned long)I.t_stage[3], (unsigned long)(micros() - t0));
    I.lines = (plug_inf_len + 139) / 140;
    I.stage = 5;
  } else {   // send: 4 lines a pass
    for (int k = 0; k < 4 && I.sent < I.lines; ++k, ++I.sent) {
      int off = I.sent * 140, n = plug_inf_len - off < 140 ? plug_inf_len - off : 140;
      char c[141];
      memcpy(c, plug_inf_buf + off, n);
      c[n] = 0;
      logf(PSTR("info: %d/%d %s"), I.sent + 1, I.lines, c);
    }
    if (I.sent >= I.lines) I.on = false;
  }
  ++I.passes;
  uint32_t d = micros() - t0;
  if (d > I.t_stage[st0 < 6 ? st0 : 5]) I.t_stage[st0 < 6 ? st0 : 5] = d;
  if (d > I.t_max) I.t_max = d;
}

// CrashReport's text as log lines (its own print would break the framed link).
struct PlugLinePrint : public Print {
  char b[160];
  int n = 0;
  size_t write(uint8_t c) override {
    if (c == '\n' || n == (int)sizeof b - 1) {
      b[n] = 0;
      if (n) logf(PSTR("crash report: %s"), b);
      n = 0;
      if (c == '\n') return 1;
    }
    if (c != '\r') b[n++] = c;
    return 1;
  }
};

// The generations' seed at boot (rmp_meta_init; spec 2.1): a powered Rytm keeps its cache across a Teensy reboot,
// so they must not restart at 1. The i.MX RT's TRNG (one 512-bit entropy block, ~a few ms; the clock gate on, the
// registers as the core's Entropy library programs them, here their reset defaults), else - no entropy within
// 100 ms, or its error bit - the cycle counter after the card mount (its time varies).
FLASHMEM uint32_t plug_gen_seed(bool* trng, uint32_t* us) {
  uint32_t c0 = ARM_DWT_CYCCNT, x = c0;
  CCM_CCGR6 |= CCM_CCGR6_TRNG(CCM_CCGR_ON);
  TRNG_MCTL = TRNG_MCTL_RST_DEF | TRNG_MCTL_PRGM;   // the defaults, program mode
  TRNG_MCTL = TRNG_MCTL_SAMP_MODE(2);               // run mode: an entropy block
  (void)TRNG_ENT15;                                 // a block left from before a soft reset: dropped, a fresh one started
  while (!(TRNG_MCTL & (TRNG_MCTL_ENT_VAL | TRNG_MCTL_ERR)) && ARM_DWT_CYCCNT - c0 < F_CPU_ACTUAL / 10) {}
  *trng = (TRNG_MCTL & TRNG_MCTL_ENT_VAL) && !(TRNG_MCTL & TRNG_MCTL_ERR);
  if (*trng) {
    volatile uint32_t* ent = &TRNG_ENT0;
    for (int i = 0; i < 16; ++i) x = (x ^ ent[i]) * 16777619u;   // ENT15 last (its read starts the next block)
  }
  *us = (ARM_DWT_CYCCNT - c0) / (F_CPU_ACTUAL / 1000000u);
  return x == 1 ? 2 : x;   // (1: rmp_meta's every-generation-1 for tests)
}

// The end of setup(): the guard (handlers, 2 kHz timer, RTWDOG), the arena,
// the state pool, the store; the slot map loads from loop().
FLASHMEM void plug_setup() {
  uint32_t boot_ms = millis();
  rmp_guard_begin(RYTM_WDOG_MS);   // before CrashReport is printed (its print clears SRC_SRSR)
  if (CrashReport) {               // a stale record is consumed here, once
    PlugLinePrint p;
    p.print(CrashReport);
    p.write('\n');
  }
  plug_off = rmp_guard_safe_mode();
  if (plug_off) logf(PSTR("plugins: SAFE MODE (the watchdog restarted the Teensy) - user machines off this boot"));
  rmp_arena_begin();
  rmp_state_begin();   // after the PSRAM sample pool took its share
  if (rmp_arena_psram_begin(RMP_PSRAM_ARENA_BYTES))   // with PSRAM: the second code arena (nothing happens without)
    logf(PSTR("plugins: PSRAM code arena %lu KB at %p (MPU regions %d, %d)"), (unsigned long)(rmp_arena_psram_size() >> 10),
         rmp_arena_psram_base(), RMP_PSRAM_REGION, RMP_PSRAM_REGION2);
  else if (external_psram_size)
    logf(PSTR("plugins: PSRAM %d MB but no room for the %lu KB code arena"), (int)external_psram_size,
         (unsigned long)(RMP_PSRAM_ARENA_BYTES >> 10));
  uint32_t t0 = micros();
  if (!rmp_store_begin()) logf(PSTR("plugins: store not mounted"));
  {
    char d[176];
    rmp_store_describe(d, sizeof d);
    if (rmp_store_foreign())
      logf(PSTR("plugins: foreign store layout - not mounted, not formatted; user slots MISSING until an explicit "
                "wipe (tools/rmp_install.py wipe --yes)"));
    logf(PSTR("plugins: store probe: %s"), d);
  }
  logf(PSTR("plugins: boot work %lu ms before the guard; store probed and mounted (or not) in %lu us, watchdog %d ms%s"), (unsigned long)boot_ms,
       (unsigned long)(micros() - t0), RYTM_WDOG_MS, rmp_guard_timer_ok() ? "" : ", NO TIMER (no timeouts)");
  logf(PSTR("boot: ITCM %lu blocks of 32 KB, RAM1 room for locals and stack %lu B"), (unsigned long)(uintptr_t)&_itcm_block_count,
       (unsigned long)((uint32_t)&_estack - (uint32_t)&_ebss));
#if RYTM_STACK_DEBUG
  plug_stack_seen = plug_stack_used();
  logf(PSTR("plugins: RAM2 heap free %lu B, stack used %lu of %lu B"), (unsigned long)plug_heap_free(),
       (unsigned long)plug_stack_seen, (unsigned long)plug_stack_room());
#endif
  for (int i = 0; i < kVoices; ++i)   // a plugin voice's pool reader: the voice's own DSP SAMPLE buffers
    rmp_pool_attach(&plug_pool[i], stream_heads[i], rytm::StreamVoice::kHead, i < 4 ? stream_rings_a[i] : stream_rings_b[i - 4],
                    rytm::StreamVoice::kRing, rytm::StreamVoice::kChunk);
  plug_host_v2.reserved[RMP_H_POOL_INFO] = RMP_FN(h_pool_info);
  plug_host_v2.reserved[RMP_H_POOL_READ] = RMP_FN(h_pool_read);
  for (int s = 0; s < RMP_SLOTS; ++s) plug_ans[s] = -1;
  {
    bool trng;
    uint32_t us, seed = plug_gen_seed(&trng, &us);
    rmp_meta_init(&plug_meta, seed);   // generations from the seed, a 1D due
    logf(PSTR("plugins: generations seeded from the %s in %lu us: U1..U5 %d %d %d %d %d, installed %d"),
         trng ? "TRNG" : "cycle counter", (unsigned long)us, plug_meta.gen[0], plug_meta.gen[1], plug_meta.gen[2],
         plug_meta.gen[3], plug_meta.gen[4], plug_meta.store_gen);
  }
  rmp_preset_names_init(&plug_pnames);   // RAM2 (DMAMEM) is not zeroed at boot
  for (int s = 0; s < RMP_SLOTS; ++s) {
    rmp_preset_index_init(&plug_pix[s]);
    rmp_knobs_from_desc(&plug_knobs[s], nullptr);
  }
  for (int vi = 0; vi < RMP_VOICES; ++vi) {
    rmp_pv_reset(&plug_pv[vi]);
    plug_pl[vi][0].on = plug_pl[vi][1].on = false;
  }
  plug_reload_pending = true;
}
#endif

#if RYTM_PLUGINS
// The voice's engine memory (shared[vi]) held a plugin's state: Plaits' engines find it
// trashed, so the voice is initialised again (all engines on that memory) before it renders.
// Done here, on the engine switch, so the render path has nothing extra to do.
__attribute__((noinline)) FLASHMEM void plug_voice_reinit(int vi) {
  V& x = v[vi];
  uint32_t t0 = micros();
  stmlib::BufferAllocator allocator(shared[vi], sizeof(shared[vi]));
  x.voice.Init(&allocator);
  apply(x);
  fms[vi].Leave(&x.voice, &x.mod);   // the 6-OP FM machine reloads its bank; PLAITS gets its own back
  x.mod.trigger_patched = true;
  plug_dirty[vi] = false;
  uint32_t us = micros() - t0;
  if (us > plug_reinit_max) plug_reinit_max = us;
  logf(PSTR("plugins: voice %d Plaits re-initialised after a plugin (%lu us)"), vi, (unsigned long)us);
}
#endif

// A whole SysEx from the Rytm (or the serial bridge's 'S'): the core's host link. Flash: per message.
__attribute__((noinline)) FLASHMEM void report(const uint8_t* d, int len) { engine_sysex(d, len); }

// The stock Rytm's MIDI notes (core/notes.h): channel N's note-on/off and CC 16..31 play voice N-1 on the machine
// notemach chose, as the mods' 0x04/0x11/0x03 reports into report(). Off by default: the mod firmware's trig
// reports play the voices then, and its notes (if it sends any) must not trig them twice. Runtime: ':notes=1'.
#ifndef RYTM_NOTES
#define RYTM_NOTES 0
#endif
bool notes_on = RYTM_NOTES;
uint32_t notes_seen = 0;   // note-ons since the last stats line (counted with notes off too)
uint16_t notes_chans = 0;  // their channels, bit 0 = channel 1
NoteAdapter* notes;       // made in setup() (FLASHMEM): a global's constructor would run from ITCM
FLASHMEM void notes_begin() {
  static NoteAdapter n(report);
  notes = &n;
}
#if RYTM_USB_HOST
// The output (host build): out=tracks puts each voice on its track's Overbridge channel (the mod build plays them);
// out=main sums them onto MAIN L/R (core/main_out.h: faded in, limited at -12 dBFS) - a stock Rytm plays only MAIN.
#ifndef RYTM_OUT_MAIN
#define RYTM_OUT_MAIN 0
#endif
bool out_main = RYTM_OUT_MAIN;
void host_out(bool main);                    // usb_host.h
void main_mix(int16_t (*f)[kVoices], int n);   // usb_host.h
bool cap_on = false;                         // a latency capture runs (usb_host.h, ':cap=MS')
void cap_note(uint8_t st, uint8_t d2);       // usb_host.h
void cap_start(int ms);
int cap_num(const char* v);
void host_fifo(const char* v);
void host_mac(const char* v);                // usb_host.h: ':mac=on|off', the computer's UAC2 OUT to the Rytm
void host_usbmode(const char* v);            // usb_host.h: ':usbmode=multi|stereo|off', what the computer sees
void host_settings_changed();                // usb_host.h: ':out' / ':notes' changed (kept in the Sidecar build)
bool sfx_console(const char* p);             // sendfx_host.h: ':sendfx', ':sendfx=K:TYPE', ... ':sendfxbench'
#endif
FLASHMEM void midi_channel(uint8_t st, uint8_t d1, uint8_t d2) {
  if ((st & 0xF0) == 0x90 && d2) {
    ++notes_seen;
    notes_chans |= 1u << (st & 0x0F);
  }
#if RYTM_USB_HOST
  if (cap_on) cap_note(st, d2);
#endif
  if (notes_on) notes->Channel(st, d1, d2);
  core::lfo::midi_in(st, d1, d2);   // the LFOs: a Rytm track's trig (an FX target's TRIG)
}
#include "webui.h"   // the web console's commands and replies (web/, docs/webui-protocol.md)
#if RYTM_USB_HOST
#include "rec_mini.h"   // the recorder: the Overbridge IN stream to the card (docs/recorder-mini.md)
#endif
#if RYTM_PLUGINS
#include "psram_bench.h"   // ':psram', ':psram bench [ID]' (docs/psram-testing.md)
#endif
// ':fxin', ':fxin=1|0|?': the IN L/R source (core/host_link.h, rytm-mods 0030-fx-in): 1 the Rytm's FX return on Overbridge
// IN L/R, 0 its AUDIO IN, ? ask. The answer as a log line and, for the web console, @{"t":"fxin","fxin":{want,state,rytm}}.
FLASHMEM bool fxin_console(const char* p) {
  char b[128];
  if (!core::fxin::command(p, b, sizeof b)) return false;
  logf(PSTR("%s"), b);
#if RYTM_WEBUI
  char j[96];
  core::fxin::json(j, sizeof j);
  wj_begin(PSTR("fxin"));
  wj(PSTR(",%s"), j);
  wj_end();
#endif
  return true;
}

// ':lfo', ':lfo=N:KEY:VALUE': the Sidecar's LFOs (core/lfo.h, docs/lfo.md). The answer as a log line and, for the web
// console, @{"t":"lfo","lfo":{"n":4,"l":[...]}}.
FLASHMEM bool lfo_console(const char* p) {
  char b[200];
  if (!core::lfo::command(p, b, sizeof b)) return false;
  logf(PSTR("%s"), b);
#if RYTM_WEBUI
  wj_begin(PSTR("lfo"));
  wj(PSTR(","));
  web_n += core::lfo::json(web_line + web_n, (int)sizeof web_line - web_n - 2);
  if (web_n > (int)sizeof web_line - 2) web_n = (int)sizeof web_line - 2;
  wj_end();
#endif
  return true;
}

// ':version': the Sidecar's version and commit as a log line and, for the web console, @{"t":"version",...}.
FLASHMEM bool version_console(const char* p) {
  if (strcmp(p, PSTR("version"))) return false;
  logf(PSTR("version: Sidecar Mini " SIDECAR_VERSION_FULL));
#if RYTM_WEBUI
  wj_begin(PSTR("version"));
  wj(PSTR(",\"version\":\"" SIDECAR_VERSION "\",\"commit\":\"" SIDECAR_COMMIT "\",\"dirty\":%s"), SIDECAR_DIRTY ? PSTR("true") : PSTR("false"));
  wj_end();
#endif
  return true;
}

FLASHMEM void console_line(const char* p) {
#if RYTM_WEBUI
  if (web_console(p)) return;
#endif
  if (version_console(p)) return;
  if (fxin_console(p)) return;
  if (lfo_console(p)) return;
  if (!strncmp(p, "rui=srclog=", 11)) {   // 1027: each 29 / 2A from the Rytm and SRC view commit logged
    core::ui::remote_src_log(p[11] == '1');
    const core::ui::RemoteStats& s = core::ui::remote_stats();
    logf(PSTR("rui: SRC view log %s, 29s %lu, 2As %lu, frames %lu"), p[11] == '1' ? "on" : "off", (unsigned long)s.src_views,
         (unsigned long)s.src_vals, (unsigned long)s.src_frames);
    return;
  }
  if (!strncmp(p, "notemach=", 9) && p[9] >= '0' && p[9] <= '7' && p[10] == ':') {
    if (notes->SetMachine(p[9] - '0', p + 11)) logf(PSTR("notes: voice %d plays %s"), p[9] - '0', p + 11);
    else logf(PSTR("notes: no machine \"%s\" (%s)"), p + 11, NoteAdapter::Names());
  } else if (!strncmp(p, "notes=", 6)) {
    notes_on = p[6] == '1';
    logf(PSTR("MIDI notes %s"), notes_on ? "on" : "off");
#if RYTM_USB_HOST
    host_settings_changed();
#endif
#if RYTM_USB_HOST
    if (notes_on && !out_main) logf(PSTR("output: tracks (a stock Rytm plays only MAIN: ':out=main' to hear the notes)"));
#endif
  } else if (!strcmp(p, "out=main") || !strcmp(p, "out=tracks") || !strcmp(p, "out")) {
#if RYTM_USB_HOST
    if (p[3]) {
      host_out(p[4] == 'm');
      host_settings_changed();
    }
    logf(PSTR("output: %s"), out_main ? "main" : "tracks");
#else
    logf(PSTR("output: the host build's (RYTM_USB_HOST=1); here the serial link carries the 8 voices"));
#endif
#if RYTM_USB_HOST
  } else if (!strncmp(p, "fifo", 4) && (!p[4] || p[4] == '=')) {
    host_fifo(p[4] ? p + 5 : "");
  } else if (!strncmp(p, "cap=", 4)) {
    cap_start(cap_num(p + 4));
  } else if (!strncmp(p, "mac", 3) && (!p[3] || p[3] == '=')) {
    host_mac(p[3] ? p + 4 : "");
  } else if (!strncmp(p, "usbmode", 7) && (!p[7] || p[7] == '=')) {
    host_usbmode(p[7] ? p + 8 : "");
  } else if (sfx_console(p)) {
  } else if (rec_console(p)) {
#endif
#if RYTM_PLUGINS
  } else if (!strncmp(p, PSTR("psram"), 5) && (!p[5] || p[5] == ' ')) {
    psram_console(p[5] ? p + 6 : PSTR(""));
#endif
  } else if (!strcmp(p, "notes")) {
    logf(PSTR("MIDI notes %s: voices 0..7 play %s %s %s %s %s %s %s %s"), notes_on ? "on" : "off",
         NoteAdapter::Name(notes->Machine(0)), NoteAdapter::Name(notes->Machine(1)), NoteAdapter::Name(notes->Machine(2)),
         NoteAdapter::Name(notes->Machine(3)), NoteAdapter::Name(notes->Machine(4)), NoteAdapter::Name(notes->Machine(5)),
         NoteAdapter::Name(notes->Machine(6)), NoteAdapter::Name(notes->Machine(7)));
  } else {
    logf(PSTR("console: unknown command \"%s\" (version, notes=1|0, notemach=V:NAME, notes, out=main|tracks, out, fxin=1|0|?, fifo=N, fifo, cap=MS, mac=on|off, usbmode=multi|stereo|off, sendfx, sendfx=K:TYPE|off, sendfxout=fx|main|slot, sendlvl=K:T|all:V, sendknob=K:I:V, sendfxbench[=N], mem, df, plugins, plugin=..., psram, psram bench [ID])"), p);
  }
}

void render_block() {
  int w = (ring_rd + ring_n) % kRing;
  uint32_t dt = render_voices(ring, kRing, w, 0, kVoices, mach_cyc);
  cycles += dt;
  if (dt > peak_block) peak_block = dt;
  ring_n += kBlock;
  frames_done += kBlock;
}

// The core's hooks (core/hooks.h): the Rytm's SysEx out, and the machine plugins on U1..U5.
#if RYTM_PLUGINS
FLASHMEM void hk_user_report(int vi, int slot, int was_slot, int note, int vel, int tag) {
  if (slot != was_slot) plug_leave(vi);   // another slot: voice_leave, and a released voice's tag mismatch goes
  plug_report(vi, note, vel, tag);
}
FLASHMEM void hk_user_leave(int vi) {   // left the user slots: voice_leave; Plaits back on the voice's memory if a plugin used it
  plug_leave(vi);
  if (plug_dirty[vi] && !v[vi].daisy && !v[vi].juno) plug_voice_reinit(vi);
}
FLASHMEM void hk_user_note_off(int vi) {
  int s = v[vi].user;
  if (plug[s].state == RMP_SLOT_READY && plug_cur[vi] == s) {
    plug_note_fire(vi);   // a note still held for its preset load: played first
    PlugCall c = {plug[s].plugin, plug_state[vi][s], nullptr, nullptr, 0, 0, 0, nullptr, 0, nullptr};
    plug_vcall(s, vi, pc_note_off, &c, kPlugCallCycles);
  }
}
FLASHMEM void hk_trig_seen() { plug_trig_ms = millis() | 1; }   // the transport counts as running for 1 s after a trig
FLASHMEM void hk_project_changed() {   // the project's slot map, loaded from loop() between requests (plug_reload)
  plug_reload_pending = true;
  plug_proj_new = true;
  // the 0x25 that follows the 10 at once must survive until plug_reload runs: the prints state is reset here
  plug_prints_have = plug_prints_pending = false;
  memset(plug_lazy_req, 0, sizeof plug_lazy_req);
  memset(plug_lazy_fp, 0, sizeof plug_lazy_fp);
  plug_fresh_until = millis() + 1500u;   // a fresh project waits this long for the 0x25 before it inherits
  plug_state_reloaded = false;
}
FLASHMEM void hk_project_seen() { plug_1d_owed = true; }   // answered with a 1D (rate-limited, plug_1d_pass)
FLASHMEM void hk_slot_assign(const uint8_t* d, int len) {   // a 1E: run from loop()
  rmp_assign a;
  if (rmp_parse_assign(d, len, &a)) {
    plug_1e_q[a.slot - 1] = a;
    plug_1e_on[a.slot - 1] = true;
    plug_1e_t[a.slot - 1] = plug_ms();
  }
}
FLASHMEM void hk_slot_prints(const uint8_t* d, int len) {   // a 0x25: resolved from loop() (plug_resolve_pass)
  rmp_prints p;
  if (!rmp_parse_prints(d, len, &p)) return;
  plug_prints = p;
  plug_prints_pending = true;
  plug_prints_rytm = true;
}
FLASHMEM void hk_pool_changed(int line) {   // pool line `line` (-1: all) changed: the plugin voices' readers of it
  for (int i = 0; i < 8; ++i)
    if (line < 0 || plug_pool[i].slot == line + 1 || plug_pool[i].bad == line + 1) rmp_pool_clear(&plug_pool[i]);
}
#endif
FLASHMEM void install_hooks() {
  hooks.to_rytm = teensy_to_rytm;
#if RYTM_PLUGINS
  hooks.trig_seen = hk_trig_seen;
  hooks.user_report = hk_user_report;
  hooks.user_leave = hk_user_leave;
  hooks.user_note_off = hk_user_note_off;
  hooks.user_render = plug_render;
  hooks.project_changed = hk_project_changed;
  hooks.pool_changed = hk_pool_changed;
  hooks.project_seen = hk_project_seen;
  hooks.slot_ask = plug_on_ask;
  hooks.slot_assign = hk_slot_assign;
  hooks.slot_prints = hk_slot_prints;
#endif
}

uint64_t busy_us = 0, busy_frames = 0;   // send_frames' wall time, card reads included

// noinline: the per-block request loop stays one ITCM function (poll_input, its caller, is in flash).
// The host build's audio goes over Overbridge (usb_host.h host_feed); its 'N' requests (tests, no stream) run from
// flash: the render itself (render_block) stays in ITCM.
#if RYTM_USB_HOST
#define SEND_FRAMES_PLACE FLASHMEM
#else
#define SEND_FRAMES_PLACE
#endif
__attribute__((noinline)) SEND_FRAMES_PLACE void send_frames(int n) {
  // the header, then the frames in pieces the ring holds (any n: a piece is
  // rendered, then sent)
  Serial.write('A');
  Serial.write((uint8_t)(n & 0xff));
  Serial.write((uint8_t)(n >> 8));
  uint32_t took = 0;
  busy_frames += n;
  while (n > 0) {
    int want = min(n, kRing - kBlock);
    uint32_t t_in = micros();
    // the streams are topped up while the sketch waits for requests (loop());
    // here only one that would run dry before this piece is done
    while (ring_n < want) {
#if RYTM_PLUGINS
      plug_pool_refill((uint32_t)(want - ring_n) + 4 * kBlock, true);   // plugin pool readers too, the neediest first
#else
      refill_neediest((uint32_t)(want - ring_n) + 4 * kBlock);
#endif
      render_block();
    }
#if RYTM_PLUGINS
    rmp_guard_feed();   // a long request: the watchdog sees it alive
#endif
    took += micros() - t_in;
    while (want > 0) {
      int run = min(want, kRing - ring_rd);
#if RYTM_USB_HOST
      if (out_main) main_mix(&ring[ring_rd], run);   // no Overbridge stream: the link gets what the Rytm would
#endif
      Serial.write((const uint8_t*)ring[ring_rd], run * kVoices * 2);
      ring_rd = (ring_rd + run) % kRing;
      ring_n -= run;
      want -= run;
      n -= run;
    }
  }
  busy_us += took;
  if (took > max_request_us) max_request_us = took;
}

#if RYTM_USB_HOST
// 'N' while the Rytm streams: the frames belong to it, the link gets n silent frames.
FLASHMEM void send_silence(int n) {
  static const uint8_t z[64] = {0};
  Serial.write('A');
  Serial.write((uint8_t)(n & 0xff));
  Serial.write((uint8_t)(n >> 8));
  for (uint32_t b = (uint32_t)n * kVoices * 2; b;) {
    uint32_t k = b < sizeof z ? b : sizeof z;
    Serial.write(z, k);
    b -= k;
  }
}
#endif

#if RYTM_USB_HOST
#include "usb_host.h"
#endif

// Input parser (1100: a web console 'Y' frame, webui.h, fits whole).
uint8_t buf[1100];
int have = 0;

#if RYTM_PLUGINS
// A frame that stops halfway (a tool killed mid-send) is dropped after
// kPartialMs without a new byte, so its payload is never read as commands;
// an open install is aborted with it. An oversized 'J' chunk's payload is
// skipped (j_skip), never parsed.
const uint32_t kPartialMs = 100;
uint32_t in_t = 0;
int in_have = 0;
uint32_t j_skip = 0, j_skip_last = 0, j_skip_t = 0;   // j_skip is dropped after kPartialMs without progress
#endif

__attribute__((noinline)) FLASHMEM void poll_input() {
  while (Serial.available() && have < (int)sizeof(buf)) buf[have++] = Serial.read();
  int used = 0;
#if RYTM_PLUGINS
  if (j_skip && have) {
    int d = (uint32_t)have < j_skip ? have : (int)j_skip;
    j_skip -= d;
    used += d;
  }
#endif
  for (;;) {
    int left = have - used;
    if (left < 1) break;
    uint8_t k = buf[used];
#if RYTM_PLUGINS
    rmp_guard_feed();
#endif
#if RYTM_USB_HOST
    if (k == '+' || k == '-' || k == 's') {   // the host build's monitor keys (usb_host.h)
      host_key((char)k);
      used += 1;
      continue;
    }
    if (k && strchr("SNIJKUDPWTZEFM", (char)k)) log_framed = true;   // a development tool: framed from now on
#endif
    if (k == 'S') {
      if (left < 2 || left < 2 + buf[used + 1]) break;
      uint32_t tr = micros();
      report(buf + used + 2, buf[used + 1]);
      tr = micros() - tr;
      if (tr > max_report_us) max_report_us = tr;
      if (tr > 2000) logf(PSTR("slow report %02x: %lu us"), buf[used + 2 + 4], (unsigned long)tr);
      used += 2 + buf[used + 1];
    } else if (k == 'N') {
      if (left < 3) break;
      int n = buf[used + 1] | buf[used + 2] << 8;
      used += 3;
#if RYTM_USB_HOST
      if (n > 0 && rytm_ob.streaming()) send_silence(n);   // the voices go to the Rytm
      else
#endif
      if (n > 0) send_frames(n);
    } else if (k == 'P') {   // ping: the link is framed now; answered with a log line
      logf(PSTR("pong"));
      used += 1;
#if RYTM_WEBUI
    } else if (k == 'Y') {   // a web console frame (webui.h): n u16, payload, crc32; the log stays plain
      if (left < 3) break;
      int n = buf[used + 1] | buf[used + 2] << 8;
      if (n > kWebFrameMax) {   // not one of ours: resync on the next byte
        used += 1;
        continue;
      }
      if (left < 7 + n) break;
      const uint8_t* q = buf + used + 3;
      web_frame(q, n, ~web_crc(0xFFFFFFFFu, q, n) == web_u32(q + n));
      used += 7 + n;
#endif
    } else if (k == 'M') {   // a MIDI channel message (the host build's USB MIDI path, for tests)
      if (left < 4) break;
      midi_channel(buf[used + 1], buf[used + 2], buf[used + 3]);
      used += 4;
    } else if (k == ':') {   // a console line up to '\n' or '\r' (at most 63 characters)
      int e = used + 1;
      while (e < have && buf[e] != '\n' && buf[e] != '\r') ++e;
      if (e >= have) {
        if (left > 64) used = have;   // no end in sight: dropped
        else break;
        continue;
      }
      char line[64];
      int n = e - used - 1;
      memcpy(line, buf + used + 1, n);
      line[n] = 0;
      console_line(line);
      used = e + 1;
#if RYTM_PLUGINS
    } else if (k == 'I') {   // development install: begin (total u32)
      if (left < 5) break;
      plug_install_begin(buf[used + 1] | buf[used + 2] << 8 | buf[used + 3] << 16 | (uint32_t)buf[used + 4] << 24);
      used += 5;
    } else if (k == 'J') {   // a chunk: n u16, n <= 256 (the input buffer)
      if (left < 3) break;
      int n = buf[used + 1] | buf[used + 2] << 8;
      if (n > 256) {   // the payload is skipped, not parsed
        plug_install_abort("a chunk over 256 B");
        used += 3;
        int d = have - used < n ? have - used : n;
        used += d;
        j_skip = n - d;
        continue;
      }
      if (left < 3 + n) break;
      plug_install_write(buf + used + 3, n);
      used += 3 + n;
    } else if (k == 'K') {   // commit
      plug_install_commit();
      used += 1;
    } else if (k == 'D') {   // device-info report (answered as 'info:' log lines, built between requests)
      plug_info_start();
      used += 1;
    } else if (k == 'U') {   // assign: slot, len, id, a b c
      if (left < 3 || left < 6 + buf[used + 2]) break;
      int n = buf[used + 2];
      plug_assign(buf[used + 1], buf + used + 3, n, buf + used + 3 + n);
      used += 6 + n;
    } else if (k == 'W') {   // a file on the card (development): mode, path, n bytes
      if (left < 3) break;
      int pl = buf[used + 2];
      if (pl > 80) {   // refused, and the rest of the buffer with it (a broken tool)
        logf(PSTR("card: 'W' refused (a %d-byte path)"), pl);
        used = have;
        continue;
      }
      if (left < 5 + pl) break;
      int n = buf[used + 3 + pl] | buf[used + 4 + pl] << 8;
      if (n > 200) {
        logf(PSTR("card: 'W' refused (%d data bytes)"), n);
        used = have;
        continue;
      }
      if (left < 5 + pl + n) break;
      plug_put(buf[used + 1], (const char*)buf + used + 3, pl, buf + used + 5 + pl, n);
      used += 5 + pl + n;
    } else if (k == 'T') {   // the transport (sim/teensy_bridge.py relays MIDI Start/Continue 1, Stop 0)
      if (left < 2) break;
      plug_transport = buf[used + 1] != 0;
      logf(plug_transport ? PSTR("transport running") : PSTR("transport stopped"));
      used += 2;
    } else if (k == 'Z') {   // remove a package from the store (development): len, id
      if (left < 2 || left < 2 + buf[used + 1]) break;
      plug_remove(buf + used + 2, buf[used + 1]);
      used += 2 + buf[used + 1];
    } else if (k == 'E') {   // export a package from the store (development): len, id
      if (left < 2 || left < 2 + buf[used + 1]) break;
      plug_export(buf + used + 2, buf[used + 1]);
      used += 2 + buf[used + 1];
    } else if (k == 'F') {   // wipe the store (development): "WIPE" follows, else the byte is junk
      if (left < 5) break;
      if (memcmp(buf + used + 1, "WIPE", 4)) {
        used += 1;
        continue;
      }
      plug_wipe();
      used += 5;
#endif
    } else if (k == 'R') {   // reboot (sim/stress.py boot: the boot log's timing)
      Serial.flush();
      delay(20);
      SCB_AIRCR = 0x05FA0004;
    } else {
      used += 1;   // resync on junk
    }
  }
  if (used) {
    memmove(buf, buf + used, have - used);
    have -= used;
  }
#if RYTM_PLUGINS
  uint32_t now = millis();
  if (j_skip) {   // the rest of an oversized 'J' payload: a dead tool must not make this swallow later frames
    if (j_skip != j_skip_last) {   // bytes were skipped: progress
      j_skip_last = j_skip; j_skip_t = now;
    } else if (now - j_skip_t > kPartialMs) {
      logf(PSTR("serial: skipping an oversized 'J' chunk timed out (%lu B still expected)"), (unsigned long)j_skip);
      j_skip = j_skip_last = 0;
    }
  } else {
    j_skip_last = 0;
  }
  if (!have || have != in_have || used) {
    in_have = have;
    in_t = now;
  } else if (now - in_t > kPartialMs) {   // a partial frame, no progress: dropped
    logf(PSTR("serial: dropped %d bytes of an unfinished '%c' frame"), have, buf[0] >= 0x20 && buf[0] < 0x7f ? buf[0] : '?');
    if (plug_installing) plug_install_abort("unfinished frame");
    have = in_have = 0;
  }
#endif
}

// The 2-second statistics (cold: flash, not ITCM, which is tight).
__attribute__((noinline)) FLASHMEM void stats_report(uint32_t now) {
    // CPU: render cycles against the real-time budget of the frames produced.
    float pct = 100.0f * cycles / ((float)frames_done * (F_CPU_ACTUAL / 48000));
    float peak = 100.0f * peak_block / ((float)kBlock * (F_CPU_ACTUAL / 48000));
    uint32_t under = 0;
    for (int i = 0; i < kVoices; ++i) under += streams[i].underruns();
    float load = busy_frames ? 100.0f * busy_us / (busy_frames / 0.048f) : 0.0f;
    // (whole tenths: logf formats integers only)
    float budget = (float)frames_done * (F_CPU_ACTUAL / 48000) / 1000.0f;
    int c10 = tenths(pct), m10[kMachines];
    for (int k = 0; k < kMachines; ++k) m10[k] = (int)(mach_cyc[k] / budget + 0.5f);
    logf(PSTR("cpu %d.%d %% (8 voices, real time), peak block %d %%, with card reads %d %%, "
         "stream underruns %lu"), c10 / 10, c10 % 10, (int)(peak + 0.5f), (int)(load + 0.5f), (unsigned long)under);
    {   // the remote UI's SIDECAR page (core/ui/remote.h): the render load, the streams' underruns
      core::ui::RemoteStatus rs = {-1, under, {(int8_t)((c10 + 5) / 10), -1, -1}};
#if RYTM_USB_HOST
      rs.ob = host_was_streaming ? 1 : 0;
#endif
#if RYTM_USB_HOST
      rec_status_base(rs);   // with the recorder's row
#else
      core::ui::remote_status(rs);
#endif
    }
    if (notes_seen) {   // the MIDI notes since the last line (as the Pi's "notes N")
      logf(PSTR("midi notes %lu, channels %x, MIDI notes %s"), (unsigned long)notes_seen, notes_chans,
           notes_on ? "on" : "off");
      notes_seen = notes_chans = 0;
    }
#if RYTM_PLUGINS
#if RYTM_STACK_DEBUG
    {
      uint32_t su = plug_stack_used();
      if (su > plug_stack_seen + 256) {
        plug_stack_seen = su;
        logf(PSTR("plugins: stack used %lu of %lu B"), (unsigned long)su, (unsigned long)plug_stack_room());
      }
    }
#endif
    if (mach_cyc[kMUser])
      logf(PSTR("load user %d.%d %% (machine plugins), longest render %lu cycles (limit %lu)"), m10[kMUser] / 10,
           m10[kMUser] % 10, (unsigned long)plug_max_cyc, (unsigned long)plug_max_limit);
    plug_max_cyc = plug_max_limit = 0;
    if (plug_reinit_max) logf(PSTR("plugins: Plaits re-init after a plugin, longest %lu us"), (unsigned long)plug_reinit_max);
    plug_reinit_max = 0;
#endif
    logf(PSTR("load fm %d.%d juno %d.%d dsps %d.%d daisy %d.%d plaits %d.%d %%; max request %lu us, "
         "max report %lu us, refills %lu (%lu KB, max %lu us), max loop %lu us, max trig wait %lu us"),
         m10[kMFm] / 10, m10[kMFm] % 10, m10[kMJuno] / 10, m10[kMJuno] % 10, m10[kMDsps] / 10, m10[kMDsps] % 10,
         m10[kMDaisy] / 10, m10[kMDaisy] % 10, m10[kMPlaits] / 10,
         m10[kMPlaits] % 10, (unsigned long)max_request_us, (unsigned long)max_report_us, (unsigned long)refills,
         (unsigned long)refill_kb, (unsigned long)max_refill_us, (unsigned long)max_loop_us,
         (unsigned long)max_trig_wait_us);
#if RYTM_PSRAM
    if (psram) {
      logf(PSTR("psram %lu of %lu KB, %lu loads, %lu evictions, max step %lu us"), (unsigned long)(psram->used_bytes() >> 10),
           (unsigned long)(psram->arena_bytes() >> 10), (unsigned long)psram->loads(),
           (unsigned long)psram->evictions(), (unsigned long)psram->max_step_us());
      psram->reset_max();
    }
#endif
    for (int k = 0; k < kMachines; ++k) mach_cyc[k] = 0;
    max_request_us = max_report_us = refills = refill_kb = max_refill_us = max_loop_us = max_trig_wait_us = 0;
    busy_us = busy_frames = 0;
    cycles = frames_done = 0;
    peak_block = 0;
    last_report = now;
}

}  // namespace

FLASHMEM void setup() {
  core::set_log_sink(log_line);
#if RYTM_PLUGINS && RYTM_STACK_DEBUG
  plug_stack_paint();
#endif
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) {}
  logf(PSTR("boot: Sidecar Mini " SIDECAR_VERSION_FULL));
  ARM_DEMCR |= ARM_DEMCR_TRCENA;
  ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;
  install_hooks();
  notes_begin();
  engine_begin();
#if RYTM_PLUGINS
  plug_setup();
#endif
#if RYTM_USB_HOST
  rec_setup();   // after the plugin state took its PSRAM
  host_setup();
#endif
}

FLASHMEM void loop() {
#if RYTM_PLUGINS
  rmp_guard_feed();
  plug_ms();   // every pass: CYCCNT's wraps (7.16 s) all seen
#endif
  uint32_t lt = micros();
  if (loop_t && lt - loop_t > max_loop_us) max_loop_us = lt - loop_t;
#if RYTM_PLUGINS
  plug_flush_log();
  if (loop_t && lt - loop_t > plug_wdog_warn_us) {
    plug_wdog_warn_us = lt - loop_t;
    logf(PSTR("plugins: a loop pass took %lu us (watchdog %d ms)"), (unsigned long)(lt - loop_t), RYTM_WDOG_MS);
  }
#endif
  loop_t = lt;
#if RYTM_USB_HOST
  host_poll();
#if RYTM_PLUGINS
  {
    static bool attached = false;   // the Rytm's MIDI came (back): a 1D, the boot's went nowhere
    if (midi_attached && !attached) plug_meta.dirty = true;
    attached = midi_attached;
  }
#endif
  host_log();
  poll_input();                    // the serial link: monitor keys and the development frames
  bool waiting = host_waiting();   // the FIFO at its target
#else
  poll_input();
  bool waiting = !Serial.available();
#endif
  // between requests: the streams' rings topped up, the neediest first (a
  // read, ~0.75 ms, delays a request arriving meanwhile by as much; the
  // Overbridge side buffers ~10 ms)
#if RYTM_PLUGINS
  if (plug_note_wait) plug_note_overdue();   // a held trig's note: bounded, whatever runs below
#endif
  if (waiting && !idle_prepare()) {
#if RYTM_PLUGINS
    bool pre_tried = plug_note_wait != 0;   // plug_preset_pass once a pass
    if (pre_tried && plug_preset_pass()) {} else   // a held trig's preset ahead of a save or job step
#endif
#if RYTM_USB_HOST
    if (rec_pass()) {}   // the recorder's card write (a block, only while the card is not busy): ahead of saves
    else
#endif
    if (idle_save()) {}
#if RYTM_PLUGINS
    else if (plug_deadline_pass()) {}   // the 1Es and their answers: the Rytm waits 2 s
    else if (plug_1d_pass()) {}         // an owed 1D (13 bytes): ahead of a reload, a job or a save
    else if (plug_reload_pending) plug_reload();
    else if (plug_job_on || plug_todo || plug_act_slot >= 0) plug_job_pass();
    else if (plug_sv.on) plug_save_pass();
    else if (!pre_tried && plug_preset_pass()) {}  // a voice's preset decision or load (1C)
    else if (plug_slots_pass()) {}   // the Rytm's user slots: descriptions, presets, 1A, 1B, 1D
    else if (plug_resolve_pass()) {} // which machine a slot plays: the Rytm's 0x25, inheritance, a trig's tag
    else if (plug_pool_pass()) {}    // plugin voices' pool samples: prepares, refills
    else if (plug_inf.on) plug_info_pass();
#else
    else if (slots_1d(5000)) {}   // every 5 s anyway: a lost 1D caught up
#endif
#if RYTM_WEBUI
    else if (web_pass()) {}          // the web console's card / store operation, one a pass
#endif
    else idle_stream();
  }
  uint32_t now = millis();
#if RYTM_USB_HOST
  rec_tick(now);   // the remote REC page's requests and state, the SYSTEM page's recorder row
#endif
  core::ui::remote_tick(now);   // the Rytm's remote page (core/ui/remote.h): nothing while it is closed
  core::lfo::tick(micros());    // the Sidecar's LFOs (core/lfo.h): once a millisecond at most
  if (now - last_report >= 2000 && frames_done) stats_report(now);
}
