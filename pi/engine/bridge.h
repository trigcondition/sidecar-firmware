// pi/engine/bridge.h - the engine core as plain functions for the kernel's Circle-compiled files (kernel, cores,
// guard, console): the core's headers pull Plaits' C++ library headers, which Circle's flags hide. bridge.cpp is
// compiled hosted, with the core.
#pragma once
#include <stdint.h>

namespace bridge {
const int kVoices = 8, kBlock = 12, kMachines = 7;   // core::kVoices, kBlock, kMachines (bridge.cpp checks)
// Before begin(): /Samples/pi-test/sine440.wav (2 s, 440 Hz, 16-bit mono, 48 kHz) and, if the project's pool file
// is missing, /.rytm/pools/_default.txt naming it as slot 1; nothing that exists is touched. False: no card.
bool selftest_content();
// The core's big buffers (core/heap_buffers.h: Plaits' scratch, DSP SAMPLE's heads and rings, the pool's files, the
// card's folder table): the bytes to reserve, then their placement into that memory (zeroed here). Once, before
// begin() and before the cores start. Without CORE_HEAP_BUFFERS (the Mac's port): 0, and nothing to place.
unsigned long heap_bytes();
void heap_place(void* mem);
bool begin(void (*to_host)(const uint8_t* m, int n));   // core::hooks.to_rytm = to_host; core::engine_begin()
void sysex(const uint8_t* d, int len);                  // core::engine_sysex
uint32_t render(int16_t (*block)[kVoices], int v0, int v1, uint64_t* mach,   // one block of voices v0..v1-1
                int16_t (*block_r)[kVoices] = nullptr, uint8_t* st = nullptr);  // (R, real-stereo bytes: core/voices.h)
bool idle_prepare();
bool idle_save();
void idle_stream();
const char* idle_slowest(unsigned* us);   // the slowest idle step since the last call (core/engine.h IdleSlow)
void idle_note(const char* name, unsigned us);
// The control core's loop, every pass: the 13-byte 1D (user slots, all EMPTY) every 5 s, as the Teensy's loop sends
// it (core::slots_1d(5000)): a Rytm whose 1D went missing sees a host again and asks for the slots' 1A.
void tick();
// The Sidecar sequencer (core/seq/seq.h): before each render block, on the control core (its due notes start / end)
void seq_block();
// The remote UI (core/ui/remote.h): its tick runs in tick(); the status for its SIDECAR page (core 0's stats, once a
// second: plain words); the console's rui=on|off (the page opened / closed from here) and rui (its numbers, logged).
// The rest of the remote UI's SIDECAR page (1021): the recorder (1 idle, 2 armed, 3 recording, 4 off), the network
// (eth / wifi: 0 none, 1 down, 2 up; the address), U1..U5 as the Rytm shows them. Only up / down and the address of
// the network go to the Rytm, never the Wi-Fi's name or key.
struct RuiMore {
  int rec;
  unsigned rec_secs;
  int eth, wifi;
  unsigned char ip[4];
  char user[5][9];
};
// A popup on the Rytm's own screen (core::ui::remote_popup): dropped when the Rytm cannot take it (a page open, the
// splash, no 36, the pairing code's popup owed or up); rui_popups counts the status popups shown (a changed count: it was; the
// pairing code's popup is not counted, so it never reads as the temperature warning having been seen).
// rui_popup and rui_status are core 0's (the stats tick, the only caller of each): they queue, and the control core's
// tick() hands them to the remote UI, which is that core's alone.
void rui_popup(const char* line1, const char* line2);
uint32_t rui_popups();
void rui_status(int ob_running, uint32_t underruns, int c1, int c2, int c3, const RuiMore* more = nullptr);
bool rui_command(const char* p);
// The pairing code on the Rytm's screen (pi/engine/paircode.h, paircode_pi.h): a 30 s popup with the code in it (a key
// ends it). The control core's, like everything else of the remote UI; the line comes from paircode_pi::TakeShow.
bool pair_popup(const char* line);   // FALSE: the Rytm would not take it (paircode_pi::ShowFailed)
void pair_code(const char* grouped); // the SIDECAR status page's "PAIR 1234 5678" line ("": none; paircode_pi::StatusLine)
uint32_t rytm_hellos();              // the Rytm's hellos so far (a new one: paircode_pi::RytmHello)
// the SIDECAR MENU's WEB UI page (core::ui::RemoteWebUi): paircode_pi's (cores.cpp, the control core at its start)
void webui_init(bool (*on)(), void (*set_on)(bool), int (*gate)(), int (*devices)(), bool (*new_code)(),
                void (*forget)());
// The Rytm has said hello on this run: before that a refused popup is "no Rytm yet", not "this Rytm cannot" - the
// boot's code stays owed until it arrives instead of being given up after ten seconds (the modal never appeared:
// the Rytm's first hello comes well after the engine's boot, audit 2026-10-26)
bool rytm_greeted();
void pair_popup_end();               // a device paired: take it off now
// The IN L/R source (core/host_link.h, 26 / 27): the console's `fxin`, `fxin=1|0|?` (the answer in out; false: not an fxin
// command) and /status's "fxin":{...} (any core, plain reads)
bool fxin_command(const char* p, char* out, int cap);
int fxin_json(char* out, int cap);
// The Sidecar's LFOs (core/lfo.h): the Rytm's channel messages (a track's trig retrigs, its knob's CC is the base), the
// console's `lfo`, `lfo=N:KEY:VALUE` (false: not an lfo command) and /status's "lfo":{...} (any core, plain reads).
void lfo_midi(uint8_t st, uint8_t d1, uint8_t d2);
bool lfo_command(const char* p, char* out, int cap);
// Poly FM (core/poly.h): the Rytm's channel messages (its pads' notes: live chords), the console's `poly` (its stats;
// false: not a poly command)
void poly_midi(uint8_t st, uint8_t d1, uint8_t d2);
bool poly_command(const char* p, char* out, int cap);   // (and the keyboards' `keys`, core/keys.h)
// The Pi's own keyboards (core/keys.h; src 0..3 the USB-A keyboards, 4 the computer on USB-C), on the control core:
// a channel message, a source plugged in / out, the Rytm-bound notes' sender (set once at begin). keys_computer_on and
// keys_line: any core, plain reads (a stale value at worst).
void keys_midi(int src, uint8_t st, uint8_t d1, uint8_t d2);
void keys_source(int src, bool present, const char* name);
void keys_set_forward(void (*f)(uint8_t st, uint8_t d1, uint8_t d2));
void asamp_set_midi_out(bool (*f)(int port, const uint8_t* m, int n));   // the auto-sampler's EXT outputs (core/samp/autosamp.h)
bool keys_computer_on();
const char* keys_line(char* b, int cap);   // "KEYS MPKMINI > SD POLY", null: no keyboard
// The SD card test (core/msamp/sdtest.h): `sdtest`, `sdtest=start[!]`, `sdtest=cancel` (busy: a take records), and
// /status's "sdtest":{...} (any core, plain reads)
bool sdtest_command(const char* p, bool busy, char* out, int cap);
// The card queue (core/msamp/cardq.h): card_worker(true) at boot (cmdline cardq=1, the default): core 0's loop runs
// the streams' card requests (card_service, at most n a call: the count run) before the recorder's writes, which hold
// off (and uploads wait) while card_busy(). Off: core 3 reads the streams itself, as before.
void card_worker(bool on);
int card_service(int n);
bool card_busy();
// SAMPLING and the AUTO-SAMPLER (core/samp/sampler.h, autosamp.h): the console's samp... / asamp... (core 3), MIDI
// realtime for the sampler's PLAY / bar starts and tempo (core 3), the producer's frames (core 0: the IN completion,
// the OUT fill), the card side (core 0's loop: true when it did a step), /status's ,"samp":{...},"asamp":{...}.
// Their control-core tick runs in tick().
bool samp_command(const char* p, char* out, int cap);
void samp_transport(uint8_t st);
bool samp_wants_in();
void samp_in(const int32_t* f, int n, int stride);
bool samp_wants_voices();
void samp_voices(const int16_t (*v)[kVoices], int n, const int16_t (*lr)[2 * kVoices] = nullptr, const float (*fx)[4] = nullptr);   // fx: the send FX returns (CHOP RESAMPLE)
void pool_mono(bool on);   // poolmono=1 (cmdline): stereo files summed at load (core/pool/pcache.h set_mono)
void stereo_trim(int pair, int db10);   // trimpre= / trimpost= (cmdline): the OUT route's pair trims (core/fx/out.h)
// the storage bench's result for volume vol (stor_pi.cpp): its 64 KB sequential reads (MB/s x 100) and 64 KB random
// reads' p99 (us) - the streaming budget (core/pool/stream_budget.h)
void stream_bench(int vol, unsigned mbps_x100, unsigned p99_us);   // poolmono=1 (cmdline): stereo files summed at load (core/pool/pcache.h set_mono)
bool samp_poll();
int samp_json(char* out, int cap);
int sdtest_json(char* out, int cap);
bool sdtest_running();   // the SD card test runs (the storage bench is refused meanwhile; any core, a plain read)
int lfo_json(char* out, int cap);
int poly_json(char* out, int cap);   // E0: "\"poly\":[..],\"inst\":{..},\"md\":{..}" - the seq tracks' poly configs, the instances (core/inst.h), the built-in MD's control-core cost (core/md/md.h)
int seqx_json(char* out, int cap);   // seqtop: "\"seqx\":{..}" - the pattern's key / scale / seed, the seq tracks' seqtop settings
// The remote UI's input tap (core/ui/extra.h: SCOPE's waveform, the meters): tap_on() while a page reads it, then each
// IN block's frames (channel c of frame i at f[i * stride + c], the Rytm's 12 channels first) from the IN completion.
bool tap_on();
void tap_in(const int32_t* f, int n, int stride);
// GRANULAR's live sources (core/granular/granular.h): gr_live_on() while a voice reads one, then each IN block's frames
// (as tap_in's) from the IN completion into its capture ring
bool gr_live_on();
void gr_live_in(const int32_t* f, int n, int stride);
// LIVE IN (core/livein/livein.h): livein_on() while a voice plays it, then each IN block's frames (as tap_in's) from the
// IN completion into its ring (the device's AUDIO IN channels); the output FIFO's target for its latency estimate (core
// 3, once a second); a LIVE IN voice's STATUS words ("LIVE -18DB"; 0: the voice is not LIVE IN)
bool livein_on();
void livein_in(const int32_t* f, int n, int stride);
void livein_fifo(unsigned frames);
int livein_status(int vi, char* b, int cap);
// seq-p2 (core/seq/seq.h, rytm_dump.h): cal_on() while the sequencer's latency click is due or on its way back, then each
// IN block's frames as tap_in's (an interrupt: the calibration only compares and stores); every byte of the Rytm's USB
// MIDI, as it comes (core 3), to the pattern dump's reader (its answer is longer than MidiIn's SysEx buffer).
bool cal_on();
void cal_in(const int32_t* f, int n, int stride);
void rytm_byte(uint8_t b);
// The remote UI's REC page (core/ui/extra.h RecInfo): the recorder's state from core 0 (rec_pi.cpp), and the page's
// request for it taken there: 0 none, 1 start, 2 stop, 3 arm, 4 disarm. state 0: no recorder (the page hidden).
struct RuiRec {
  int state;   // 0 none, 1 idle, 2 armed, 3 recording, 4 finishing
  unsigned secs, free_mb, total_mb, need_kbps, in_mask;
  char take[24], error[32];
  int ch_select;   // 1026: the trig keys pick the take's channels on the REC page
};
void rui_rec(const RuiRec& r);
int rui_rec_take();
unsigned rui_rec_toggles();   // 1026: the REC page's trig keys since the last call (bit k: T(k+1); core/ui/extra.h)
// The REC page's playback (core/ui/extra.h PlayInfo; rec_play.h): its state from core 0, the page's input taken there
// (B pushed: toggle; B / C / D turned: channel, take, level deltas). state 0: no take to play.
struct RuiPlay {
  int state;   // 0 none, 1 stopped, 2 playing, 3 stopping
  int level, ch_i, channels, take_i, takes;
  unsigned pos_ms, len_ms;
  char take[24], channel[12], error[32];
};
void rui_play(const RuiPlay& p);
bool rui_play_take(int* toggle, int* channel, int* take, int* level);
void reinit_voices();                  // core::voices_init and every voice back on PLAITS (after the boot self-check)
// The boot bench (one core at a time, before the render loops start): voice 0 on the machine (the note adapter's
// defaults, note 48, a retrigger on note 55 halfway), 4000 blocks = 1 s of audio. Returns the render ticks (generic
// timer) in total; *worst_block the longest single block; *peak (if given) voice 0's largest |sample| (a machine
// that renders silence shows 0). Ends with the voices as at boot (reinit_voices).
uint64_t bench(const char* machine, uint32_t* worst_block, int* peak = nullptr);
// fx-ins (core/fx/insert.h): the INSERT chains' cost on the calling core: 8 voice chains with TYPEs a / b in their two
// slots, 4000 12-frame blocks (1 s), an engine and values of its own (the live chains untouched). Returns the ticks in
// total, *worst_block the longest block. ins_bench_name: "FILTER" ..
uint64_t ins_bench(int a, int b, uint32_t* worst_block);
int ins_json(char* out, int cap);   // "\"ins\":{...}" (core/fx/insert.h json: the /status page's)
const char* ins_bench_name(int type);
// INSERT AMP (core/fx/amp.h): one NAM A2-Lite instance (a synthetic capture) over 4000 12-frame blocks (1 s); ticks as
// ins_bench
uint64_t amp_bench(uint32_t* worst_block);
unsigned poly_voices();
int rytm_pattern(int* transport);   // UX review T1: the Rytm's pattern (-1 not known) and transport (core/seq's view)   // UX review S1: bit vi - a Rytm track on voice vi is a poly track (the Sidecar sequences it)
uint32_t machine_gen(int voice);       // with machine_name, the guard's key: 0, or a user voice's slot + 1 and the
                                       // plugin loader's epoch (um::code_epoch): another plugin or a reload is new
const char* machine_name(int voice);   // the voice's current machine: PLAITS JUNO FM DAISY DSPS GRAIN DD USER MULTI WAVE GRAN
// 1025: each Rytm track's SRC machine as the Rytm itself reports it (33), not what we derive or force. Twelve entries
// in track order (BD SD RS CP BT LT MT HT CH OH CY CB): {"m": the short name, "vb": the raw voice byte}. "m" is
// "RYTM" when the track is not ours (0x7F: its own analog or sample voice) and "?" before the Rytm has reported.
// Writes "\"src\":[..]" without a leading comma, as poly_json does; returns the length, 0 if it does not fit.
int track_src_json(char* out, int cap);
// 2026-10-26: "store":{files,bytes,ready,unwritten,oldestMs,held,heapFree} (docs/pi-engine.md); heap_free: the kernel's
// CMemorySystem::GetHeapFreeSpace(HEAP_LOW), the heap core's new takes the store's buffers from
int store_json(char* out, int cap, unsigned long heap_free);
const char* stat_name(int k);          // core's kM* order: fm juno dsps daisy md plaits user
int card_folders();
const char* project();
int pool_slots();
uint32_t to_host_dropped();
}  // namespace bridge
