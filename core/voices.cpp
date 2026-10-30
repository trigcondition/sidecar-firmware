// core/voices.cpp - see voices.h. Everything above the marker line was moved from teensy/voiceloop/voiceloop.ino
// as it was (tools/move_code.py); the functions below it are report()'s voice part, render_block()'s loop and
// setup()'s voice part, written around core::hooks where the Teensy had its plugin calls.
#include "voices.h"
#if CORE_MSAMP
#include "lib/pools.h"
#include "lib/srclib.h"
#endif

#include "md/md.h"   // MD on 0x40 (CORE_MD: Pi, Mac)
#include "sysex.h"
#include "ui/remote.h"
#include "lfo.h"
#include "fx/insert.h"   // the voices' INSERT chains (CORE_POLY builds)
#include "fx/out.h"      // the OUT route (CORE_POLY builds): PRE / POST voices render stereo
#include "poly.h"
#include "inst.h"     // E0: the sequencer's instances (CORE_POLY builds)
#include "seq/seq.h"
#if CORE_MSAMP
#include "msamp/cardq.h"
#include "msamp/msamp.h"
#include "nmach.h"   // WAVETABLE, GRANULAR
#include "livein/livein.h"   // LIVE IN
#endif

#include <math.h>
#include <string.h>

#ifdef CORE_HEAP_BUFFERS
#include <new>
#endif

namespace core {

static_assert(kBlock == plaits::kBlockSize, "core::kBlock is Plaits' block size");

V v[kVoices];

CORE_RAM2 rytm::DaisyEngine daisy[kVoices];   // 6 KB each, in RAM2

const uint8_t kQuiet[18] = {0};   // an empty slot: no oscillator on

CORE_RAM2 juno::Voice junos[kVoices];           // 2.2 KB each (its chorus line), in RAM2

#ifdef CORE_HEAP_BUFFERS
char (*shared)[16384] = nullptr;   // heap_buffers_place
#else
CORE_RAM2 alignas(32) char shared[kVoices][16384];   // Plaits' scratch, in RAM2; a voice on a user slot keeps its plugin state here (plug_report)
#endif

rytm::FmMachine fms[kVoices];   // 6-OP FM: each on its V's Plaits voice

// a voice reload's bank, RAM2: one per render core (plaits::fm_core(); one on the Teensy), as plaits::fm_user_bank
CORE_RAM2 uint8_t fm_stage[RYTM_RENDER_CORES][rytm::FmMachine::kStageBytes];

int plaits_model(const V& x) { return kPlaitsModels[eng_zone(x.p[0], kPlaitsEngs)]; }

int daisy_model(const V& x) { return eng_zone(x.p[0], rytm::DaisyEngine::MODELS); }

void apply(V& x) {
  x.patch.frequency_modulation_amount = (x.p[8] - 64) / 64.0f;   // FM
  x.patch.timbre_modulation_amount = x.p[11] / 127.0f;           // TMD
  x.patch.morph_modulation_amount = x.p[12] / 127.0f;            // MMD
  x.patch.engine = plaits_model(x);   // P1 ENG
  x.patch.harmonics = x.p[1] / 127.0f;
  x.patch.timbre = x.p[2] / 127.0f;
  x.patch.morph = x.p[3] / 127.0f;
  x.patch.decay = x.p[4] / 127.0f;
  x.patch.lpg_colour = x.p[5] / 127.0f;
}

// DSP SAMPLE voices stream their sample from the card (stream_voice.h): a head
// read when the sample is chosen, a ring refilled from the main loop.
rytm::StreamVoice streams[8];

#ifdef CORE_HEAP_BUFFERS   // heap_buffers_place
uint8_t (*stream_heads)[rytm::StreamVoice::kHead] = nullptr;
uint8_t (*stream_rings_a)[rytm::StreamVoice::kRing] = nullptr;
uint8_t (*stream_rings_b)[rytm::StreamVoice::kRing] = nullptr;

static size_t up64(size_t n) { return (n + 63) & ~(size_t)63; }
static const size_t kSharedB = up64(sizeof(char[kVoices][16384])), kHeadsB = up64(sizeof(uint8_t[8][rytm::StreamVoice::kHead])),
                    kRingsB = up64(sizeof(uint8_t[4][rytm::StreamVoice::kRing])), kFilesB = up64(sizeof(File[kPoolLines]));

#if CORE_POLY
static const size_t kInsB = up64(ins::heap_bytes());   // the INSERT chains' DELAY lines (core/fx/insert.h)
#else
static const size_t kInsB = 0;
#endif
size_t heap_buffers_bytes() { return kSharedB + kHeadsB + 2 * kRingsB + kFilesB + kInsB + up64(card::heap_bytes()); }

CORE_COLD void heap_buffers_place(void* mem) {
  uint8_t* p = (uint8_t*)mem;
  memset(p, 0, heap_buffers_bytes());
  shared = (char(*)[16384])p, p += kSharedB;
  stream_heads = (uint8_t(*)[rytm::StreamVoice::kHead])p, p += kHeadsB;
  stream_rings_a = (uint8_t(*)[rytm::StreamVoice::kRing])p, p += kRingsB;
  stream_rings_b = (uint8_t(*)[rytm::StreamVoice::kRing])p, p += kRingsB;
  pool_files = new (p) File[kPoolLines], p += kFilesB;
#if CORE_POLY
  ins::heap_place(p), p += kInsB;
#endif
  card::heap_place(p);
}
#else
CORE_RAM2 uint8_t stream_heads[8][rytm::StreamVoice::kHead];   // 64 KB, RAM2

uint8_t stream_rings_a[4][rytm::StreamVoice::kRing];        // 64 KB, RAM1

CORE_RAM2 uint8_t stream_rings_b[4][rytm::StreamVoice::kRing]; // 64 KB, RAM2
#endif

int prepared[8][3];                                          // folder, file, STA

float trig_note[8];

// With PSRAM (core/pool/sample_ram_pool.h, docs/psram.md): the pool's samples whole in RAM and
// DSP SAMPLE's playback modes (MOD, word 20) on them; without, none of this
// runs (psram null) and the voices stream as before.
rytm::SampleRamPool* psram = nullptr;   // itself in PSRAM

rytm::SampleModes* engines = nullptr;   // 8, from the heap (RAM2) when there is PSRAM

bool from_ram[8];       // the voice's note plays from RAM (engines), not the stream

int ram_pin[8][2];      // the sample the voice holds pinned (folder, file; -1 none)

int dsp_mode(const V& x) {
  return x.grain ? (int)rytm::SampleModes::kGrain : rytm::SampleModes::Zone(x.p[11], rytm::SampleModes::kModes);
}
// The sample engine's P9..P16 (TUN FIN SMP MOD STA END LOP LEV): the voice's own, or for the GRAIN machine its words
// with MOD in GRAIN's zone (its P12 is SPRY: the engine's spray)
static inline const float* sample_params(const V& x, float* t) {
  if (!x.grain) return x.p + 8;
  for (int k = 0; k < 8; ++k) t[k] = x.p[8 + k];
  t[3] = kGrainModValue;
  return t;
}

// The voice leaves RAM playback (streams, plays another machine, or the
// project changed): its engine stops (grains back to the budget), its pin goes.
void ram_release(int vi) {
  if (!psram) return;
  if (from_ram[vi]) engines[vi].Stop();
  from_ram[vi] = false;
  if (ram_pin[vi][0] != -1) psram->unpin(ram_pin[vi][0], ram_pin[vi][1]);
  ram_pin[vi][0] = ram_pin[vi][1] = -1;
}
// The sample RAM's key of SMP slot (1..npool, pool_slot: rui1045 every SAMPLES bank): the card index's folder / file (under /Samples), or, on the Pi and the
// Mac, a sample outside /Samples (the pools design 3.6: RAM only, no stream) as folder rytm::kSlotKey - line; -1 none.
CORE_COLD int ram_key(int slot, int* smp) {
  *smp = 0;
  if (slot < 1 || slot > npool) return -1;
  const PoolEntry& e = pool[slot - 1];
  if (e.folder >= 0) return *smp = e.file, e.folder;
#if CORE_MSAMP
  if (e.path[0] == '/') return rytm::SampleRamPool::kSlotKey - (slot - 1);
#endif
  return -1;
}

// Get voice vi's sample ready if its pool slot's file or STA changed. SMP
// (word 19) is the slot (pool_slot: its row and, rui1045, its bank): 0 off; an empty slot or one beyond the pool silent.
// Opening the file and reading its head takes 1-4 ms on the card, so a report
// only notes what the voice wants; loop() prepares it between requests
// (prepare_pending), and a trig that arrives before it is ready starts when
// it is (a sample per step on eight voices: 8 prepares on one step overran
// the Overbridge buffer when done inline).
int want_prep[8][4];          // folder, file, STA, pool line wanted

bool prep_pending[8], trig_pending[8];
bool prep_deferred[8];   // loadstall: a prepare left out (the sample RAM plays it): trigger_stream's, if it streams

float pending_note[8];

uint32_t trig_wait_t[8], max_trig_wait_us = 0;

// 6-OP FM: the voice PAT picks, entry min(PAT, total - 1) of the effective
// FM list; read (from the card, for a card bank) only when PAT or the list
// changed. Loading another voice cuts what the voice was playing, so PAT is
// applied at trigs (and when the list changes). No knob sync: TIM and MOR are
// offsets on the voice.
int fm_gen = 0;             // the list's generation (a new project, LOAD TO PROJ)

int fm_key[kVoices];        // fm_gen << 8 | entry, -1 none yet

int fm_pat[kVoices];

__attribute__((noinline)) CORE_COLD void fm_select(int vi, int pat) {
  pat = pat < 0 ? 0 : pat > 127 ? 127 : pat;
  fm_pat[vi] = pat;
  int nl = dxlib.listed(), total = nl ? nl : dx7::kRomVoices;
  int e = pat >= total ? total - 1 : pat;
  int key = fm_gen << 8 | e;
  if (key == fm_key[vi]) return;
  fm_key[vi] = key;
  uint8_t b[128];
  const uint8_t* bytes = nullptr;
  if (!nl) bytes = dx7::rom_bank(e / 32) + 128 * (e % 32);
  else if (dxlib.voice(e, b)) bytes = b;
  fms[vi].SetVoice(bytes);
#if CORE_POLY
  poly::set_voice(vi, bytes);   // the voice's chord notes play it too
#endif
  if (!bytes) {   // an empty slot, or its bank not on the card: silent
    log(CORE_STR("voice %d: FM slot %d %s"), vi, e, dxlib.slot(e).used ? "not on the card" : "empty");
  }
}

// The DX7 voice entry e of the effective FM list into b (false: an empty slot / not on the card).
CORE_COLD bool fm_bytes(int pat, uint8_t* b) {
  pat = pat < 0 ? 0 : pat > 127 ? 127 : pat;
  int nl = dxlib.listed(), total = nl ? nl : dx7::kRomVoices;
  int e = pat >= total ? total - 1 : pat;
  if (!nl) {
    memcpy(b, dx7::rom_bank(e / 32) + 128 * (e % 32), 128);
    return true;
  }
  return dxlib.voice(e, b);
}

// The FM list changed: FM voices play what their PAT now picks.
CORE_COLD void fm_relist() {
  ++fm_gen;
  for (int i = 0; i < kVoices; ++i)
    if (v[i].fm) fm_select(i, fm_pat[i]);
}

// JUNO knob sync (15 <voice> <CUT RES ENV ATK DEC REL>): when the sound's PAT
// (a 0x03 report, or the first report that makes the voice JUNO; not a
// p-locked PAT on a trig) or the list entry it points at picks another patch,
// the Rytm gets that patch's own six sliders for the track's knobs; until a
// report carries all six (or kJunoHold ms passed) the incoming six are
// ignored and the patch's own play.
const int kJunoByte[6] = {5, 6, 7, 11, 12, 14};

const uint32_t kJunoHold = 300;

int juno_pat[kVoices];            // the sound's PAT; -1 none yet

int juno_key[kVoices];            // the patch the knobs were sent: factory p, 128 + list slot

uint8_t juno_kb[kVoices][18];     // its bytes

uint8_t juno_hold[kVoices][6];

uint32_t juno_hold_t[kVoices];

bool juno_holding[kVoices];

// The patch PAT picks: the project's JUNO list's slot min(PAT, total - 1),
// else factory patch PAT. Returns its key; the bytes and name in b and name.
int juno_pick(int pat, const uint8_t** b, const char** name) {
  int nl = junolib.listed();
  if (!nl) {
    *b = juno::kPatches[pat & 127].b;
    *name = juno::kPatches[pat & 127].name;
    return pat & 127;
  }
  int slot = pat >= nl ? nl - 1 : pat;
  const jlib::Patch& jp = junolib.slot(slot);
  *b = jp.used ? jp.b : kQuiet;
  *name = jp.used ? jp.name : "(empty)";
  return 128 + slot;
}

// Voice vi plays the patch PAT picks; sync: PAT is the sound's - when it
// picks another patch (or force) 15 goes out. Then the hold over x.p[1..6].
__attribute__((noinline)) CORE_COLD void juno_select(int vi, int pat, bool sync, bool force) {
  V& x = v[vi];
  pat = pat < 0 ? 0 : pat > 127 ? 127 : pat;
  const uint8_t* b;
  const char* name;
  int key = juno_pick(pat, &b, &name);
  junos[vi].UsePatch(junolib.listed() ? b : nullptr, name);
  if (sync) {
    juno_pat[vi] = pat;
    if (force || key != juno_key[vi] || memcmp(b, juno_kb[vi], 18) != 0) {
      juno_key[vi] = key;
      memcpy(juno_kb[vi], b, 18);
      for (int i = 0; i < 6; ++i) juno_hold[vi][i] = b[kJunoByte[i]] & 0x7f;
      uint8_t m[13];
      to_rytm(m, sysex_juno_knobs(m, vi, juno_hold[vi]));
      juno_holding[vi] = true;
      juno_hold_t[vi] = now_ms();
    }
  }
  if (juno_holding[vi]) {
    bool same = true;
    for (int i = 0; i < 6; ++i) same = same && (int)x.p[1 + i] == juno_hold[vi][i];
    if (same || now_ms() - juno_hold_t[vi] >= kJunoHold)
      juno_holding[vi] = false;
    else
      for (int i = 0; i < 6; ++i) x.p[1 + i] = juno_hold[vi][i];
  }
  junos[vi].SetKnobs(x.p);
}

// The JUNO list changed (a new project, LOAD TO PROJ): voices whose PAT now
// picks another patch play it, and the Rytm's knobs get it.
CORE_COLD void juno_relist() {
  for (int i = 0; i < kVoices; ++i) {
    if (!v[i].juno || juno_pat[i] < 0) continue;
    juno_select(i, juno_pat[i], true, false);
  }
}

// Top up the DSP SAMPLE stream with the least audio ahead, if it has less
// than `below` frames: one card read (about 0.75 ms). Returns whether it read.
bool refill_neediest(uint32_t below) {
  uint8_t tried = 0;
  for (;;) {
    int best = -1;
    uint32_t least = below;
    for (int i = 0; i < kVoices; ++i) {
      if (!v[i].dsps || !streams[i].playing() || (tried >> i & 1)) continue;
      uint32_t a = streams[i].ahead(below);
      if (a < least) least = a, best = i;
    }
    if (best < 0) return false;
    tried |= 1 << best;
    uint32_t tr = now_us();
    uint32_t got = streams[best].refill();
    if (!got) continue;   // its ring is full: the next
    tr = now_us() - tr;
    ++refills;
    refill_kb += got >> 10;
    if (tr > max_refill_us) max_refill_us = tr;
    return true;
  }
}

CORE_COLD void prepare_stream(int vi) {   // (per report: cold, as when it was inlined into voiceloop's report())
  if (!card_ok) return;
  int slot = pool_slot(v[vi].p[10]), sta = (int)v[vi].p[12];
  if (slot < 1 || slot > npool || pool[slot - 1].folder < 0) {
    if (prepared[vi][0] != -1) streams[vi].clear();
    prepared[vi][0] = -1;
    prep_pending[vi] = trig_pending[vi] = false;
    int smp, f = ram_key(slot, &smp);
    if (psram && f != -1) psram->want(f, smp, false);   // outside /Samples: from RAM only
    else ram_release(vi);   // no sample: silent, as the stream
    return;
  }
  int f = pool[slot - 1].folder, smp = pool[slot - 1].file;
  if (psram) psram->want(f, smp, false);   // into RAM if not there (loop() loads it)
  want_prep[vi][0] = f;
  want_prep[vi][1] = smp;
  want_prep[vi][2] = sta;
  want_prep[vi][3] = slot - 1;
  prep_pending[vi] = !(prepared[vi][0] == f && prepared[vi][1] == smp && prepared[vi][2] == sta);
  prep_deferred[vi] = false;
  if (prep_pending[vi] && psram) {   // loadstall: no stream prepare (an open and a head read on the card) the voice will
    int mode = dsp_mode(v[vi]);       // not play - from the sample RAM it plays whole, or a mode that never streams
    const int16_t* pcm = nullptr;
    uint32_t frames = 0;
    float rate = 48000.0f;
    bool cut = false;
    bool ram = psram->get(f, smp, &pcm, &frames, &rate, &cut, nullptr) && !cut;
    if (ram || (mode != rytm::SampleModes::kOneShot && mode != rytm::SampleModes::kLoop))
      prep_pending[vi] = false, prep_deferred[vi] = true;   // (a trig that streams after all prepares it: trigger_stream)
  }
}

// A DSP SAMPLE trig: now, or when the voice's sample is ready.
CORE_COLD void trigger_stream(int vi, int note) {
  if (psram) {   // from RAM if the sample is there (rytm::ChoosePath)
    int slot = pool_slot(v[vi].p[10]), mode = dsp_mode(v[vi]);
    int smp, f = ram_key(slot, &smp);
    const int16_t* pcm = nullptr;
    uint32_t frames = 0;
    float rate = 48000.0f;
    bool cut = false;
    int ch = 1;
    bool res = f != -1 && psram->get(f, smp, &pcm, &frames, &rate, &cut, &ch);
    if (cut && (mode == rytm::SampleModes::kOneShot || mode == rytm::SampleModes::kLoop))
      res = false;   // longer than RAM holds: ONE and LOOP stream it whole
    int path = rytm::ChoosePath(true, res, mode);
    if (path != rytm::kPathRam) ram_release(vi);
    from_ram[vi] = path == rytm::kPathRam;
    if (path != rytm::kPathStream) {
      streams[vi].stop();
      trig_pending[vi] = false;
      if (path == rytm::kPathSilent) {
        log(CORE_STR("voice %d: %s waits for its sample in PSRAM"), vi, rytm::SampleModes::Name(mode));
        return;
      }
      if (ram_pin[vi][0] != f || ram_pin[vi][1] != smp) {   // hold it while it plays
        int of = ram_pin[vi][0], os = ram_pin[vi][1];
        ram_pin[vi][0] = ram_pin[vi][1] = -1;   // none held while it changes (as h_pool_info)
        if (of != -1) psram->unpin(of, os);
        psram->pin(f, smp);
        ram_pin[vi][0] = f;
        ram_pin[vi][1] = smp;
      }
      trig_note[vi] = note;
      engines[vi].Set(pcm, frames, rate, ch);
      float gp[8];
      engines[vi].SetSpray(v[vi].grain ? v[vi].p[11] / 127.0f : 0.25f);
      engines[vi].Trigger(note, sample_params(v[vi], gp));
      return;
    }
  }
  if (prep_deferred[vi]) prep_pending[vi] = true, prep_deferred[vi] = false;   // it streams after all: prepared first
  if (prep_pending[vi]) {
    if (!trig_pending[vi]) trig_wait_t[vi] = now_us();
    trig_pending[vi] = true;
    pending_note[vi] = note;
    return;
  }
  trig_note[vi] = note;
  streams[vi].trigger();
}

// loadstall (0.3.0): a pool slot's file opened by the card core (cardq kOpenReq, by its path), not by the control core
// (cards.open: /Samples and each folder down to it opened, then the file - 11.5 ms measured on the Pi, a gap in the
// audio). One open at a time; true when line's file is open (pool_open) or its open failed, false while it waits.
// An open that finishes for a slot whose file changed meanwhile (pool_sync) is closed again.
#if CORE_MSAMP
static cardq::Req s_open;
static int s_open_line = -1, s_open_f = 0, s_open_smp = 0;
static char s_open_path[cardq::kPathMax];
static CORE_COLD void open_done() {
  int l = s_open_line;
  s_open_line = -1;
  if (s_open.got != 1) {
    log(CORE_STR("voice: cannot open %s"), s_open_path);
    return;
  }
  if (pool[l].folder == s_open_f && pool[l].file == s_open_smp && !strcmp(pool[l].path, s_open_path + 9) &&
      cards.file(s_open_f, s_open_smp, &pool_recs[l]))
    pool_open[l] = true;
  else
    pool_files[l].close();   // (the slot changed while it opened)
}
static CORE_COLD bool open_on_card(int line, int f, int smp) {
  if (s_open_line >= 0 && (s_open_line != line || s_open_f != f || s_open_smp != smp)) {   // another one first
    if (!cardq::async(&s_open, cardq::kOpenReq, &pool_files[s_open_line], s_open_path, 0, nullptr, 0)) return false;
    open_done();
    if (pool_open[line]) return true;
  }
  if (s_open_line < 0) {
    if (sniprintf(s_open_path, sizeof s_open_path, "/Samples/%s", pool[line].path) >= (int)sizeof s_open_path)
      return true;   // (a path too long for a request: not opened, the prepare fails as a file it cannot read)
    s_open_line = line, s_open_f = f, s_open_smp = smp;
  }
  if (!cardq::async(&s_open, cardq::kOpenReq, &pool_files[line], s_open_path, 0, nullptr, 0)) return false;
  open_done();
  return true;
}
static bool card_core() { return cardq::worker(); }
#else
static bool card_core() { return false; }
static bool open_on_card(int, int, int) { return true; }
#endif

// One voice's pending prepare (a waiting trig's first); false if none.
CORE_COLD bool prepare_pending() {
  int vi = -1;
  for (int i = 0; i < 8; ++i)
    if (prep_pending[i] && (vi < 0 || (trig_pending[i] && !trig_pending[vi]))) vi = i;
  if (vi < 0) return false;
  int f = want_prep[vi][0], smp = want_prep[vi][1], sta = want_prep[vi][2];
  if (!pool_open[want_prep[vi][3]] && card_core() && !open_on_card(want_prep[vi][3], f, smp))
    return false;   // its open waits for the card core: the streams' top-ups go on meanwhile
  prep_pending[vi] = false;
  prepared[vi][0] = f;
  prepared[vi][1] = smp;
  prepared[vi][2] = sta;
  uint32_t t0 = now_us();
  int line = want_prep[vi][3];
  if (!pool_open[line] && !card_core() && cards.file(f, smp, &pool_recs[line]) && cards.open(f, pool_recs[line], &pool_files[line]))
    pool_open[line] = true;   // (no card core: the Mac's tests and library, the old path)
  bool ok = pool_open[line] && streams[vi].prepare(pool_recs[line], pool_files[line], f, smp, sta / 120.0f);
  if (trig_pending[vi]) {
    trig_pending[vi] = false;
    trig_note[vi] = pending_note[vi];
    streams[vi].trigger();
    uint32_t w = now_us() - trig_wait_t[vi];
    if (w > max_trig_wait_us) max_trig_wait_us = w;
  }
  log(CORE_STR("voice %d: %s %s (%lu us)"), vi, ok ? "ready" : "cannot read", streams[vi].name(),
       (unsigned long)(now_us() - t0));
  return true;
}

uint32_t refills = 0, refill_kb = 0, max_refill_us = 0;   // refill_neediest's, for the stats line

// ---- written for the core ----

// The sample RAM: its pool (when the board has memory for it) and the playback-mode engines, before the voices
// play. Boot only (cold); after the card.
CORE_COLD void sample_ram_init() {
  if (card_ok) psram = rytm::SampleRamPool::create(&cards, log);
  if (psram) engines = new rytm::SampleModes[8];
  for (int i = 0; i < 8; ++i) {
    if (engines) engines[i].Init(i + 1), engines[i].AttachPluck(psram->pluck_line(i));
    from_ram[i] = false;
    ram_pin[i][0] = ram_pin[i][1] = -1;
  }
  if (psram && !engines) psram = nullptr;   // (no heap left: stream as without PSRAM)
}

// The voices at boot, after the project is loaded (setup()'s two loops, in their order).
CORE_COLD void voices_init() {
  for (int i = 0; i < 8; ++i) {
    junos[i].Init(48000.0f);
    juno_pat[i] = -1;
    fms[i].Init();
#if CORE_POLY
    poly::set_voice(i, nullptr);
#endif
    fm_key[i] = -1;
    fm_pat[i] = 0;
    juno_holding[i] = false;
    streams[i].attach(stream_heads[i], i < 4 ? stream_rings_a[i] : stream_rings_b[i - 4]);
    prepared[i][0] = -1;
    trig_note[i] = 60;
  }
  for (int i = 0; i < kVoices; ++i) {
    V& x = v[i];
    stmlib::BufferAllocator allocator(shared[i], sizeof(shared[i]));
    x.voice.Init(&allocator);
    memset(&x.patch, 0, sizeof(x.patch));
    memset(&x.mod, 0, sizeof(x.mod));
    const float init[16] = {45, 64, 64, 64, 64, 64, 64, 100, 64, 64, 0, 0, 0, 120, 0, 100};
    for (int k = 0; k < 16; ++k) x.p[k] = init[k];
    apply(x);
    x.note = x.target = 48;
    x.level = 0.8f;
    x.mod.trigger_patched = true;
    x.live = x.trig = x.daisy = x.fm = x.md = false;
#if CORE_MSAMP
    x.ms = x.wt = x.gr = x.li = x.cp = false;
#endif
    x.user = -1;   // no user slot
    daisy[i].Init(48000.0f);
  }
#if CORE_MD
  md::init();     // MD's voices (core/md/md.h): FM BD each
#endif
#if CORE_POLY
  poly::init();   // the chord notes' pool (core/poly.h): every note free
  inst::init();   // E0: the sequencer's instances (core/inst.h)
#endif
}

// 0x11 NOTE_OFF: the voice's gate closed (plain voice, no flags).
// All Sound Off (CC 120) for a voice: the gate closed, and its output ramped to zero in 5 ms whatever the machine,
// held there until the next trig (voice_report 0x04). The ramp runs at the end of render_voices.
static volatile bool silenced[kVoices];
static float silence_gain[kVoices] = {1, 1, 1, 1, 1, 1, 1, 1};   // the render core's: 1 sounding .. 0 silent
// A note the Sidecar sequencer starts on vi: its output back after a CC 120 / link reset (as a trig brings it back),
// and the voice rendering (an empty Rytm track never sends the 0x04 that does both).
CORE_COLD void voice_wake(int vi) {
  silenced[vi & 7] = false;
  v[vi & 7].live = !v[vi & 7].md || CORE_MD;   // (no MD on this build: 0x40 stays silent)
}

CORE_COLD void voice_silence(int vi) {
  voice_note_off(vi);
#if CORE_POLY
  poly::silence(vi & 7);
  inst::silence(vi & 7);     // E0: the sequencer's instances
#endif
#if CORE_MSAMP
  nm::silence_all(vi & 7);   // MULTISAMPLE, WAVETABLE, GRANULAR
  if (v[vi & 7].li) {   // LIVE IN: a live input, not a sound that rings on - its envelope cut, the input passes on (an
    livein::silence(vi & 7);   // effects lane on PRE / POST has no trig to bring it back after a link reset)
    return;
  }
#endif
  silenced[vi & 7] = true;
}

CORE_COLD void voice_note_off(int vi) {
  if (v[vi].juno) junos[vi].NoteOff();
  if (v[vi].fm) fms[vi].NoteOff();
#if CORE_POLY
  if (v[vi].fm) poly::note_off(vi);   // the chord's notes (a live chord's go with their pads)
#endif
  if (v[vi].user >= 0 && hooks.user_note_off) hooks.user_note_off(vi);
#if CORE_MSAMP
  if (v[vi].ms || v[vi].wt || v[vi].gr || v[vi].cp) nm::note_off(vi);
  if (v[vi].li) livein::release(vi);   // MON TRIG's release
#endif
  if (v[vi].dsps) dsps_env_off(vi);   // PRE / POST: a 2 ms release (no Rytm AMP after it)
  if (v[vi].dsps && from_ram[vi]) engines[vi].NoteOff();   // LOOP GRAIN STRCH WAVE fade out
  else if (psram && v[vi].dsps && dsp_mode(v[vi]) == rytm::SampleModes::kLoop)
    streams[vi].stop();   // LOOP streamed (not in RAM yet): ends at the gate too
}

static CORE_COLD void play_mono(int vi, int note, int vel);
static inline int engine_sig(const V& x) {   // E0: which engine a voice plays (a change silences its instances)
  int s = (x.user >= 0) * 32 + x.fm * 16 + x.juno * 8 + x.md * 4 + x.daisy * 2 + x.dsps;
#if CORE_MSAMP
  s += x.ms * 64 + x.wt * 128 + x.gr * 256 + x.li * 512 + x.cp * 1024;
#endif
  return s;
}

// A 0x04 TRIG+PARAMS or 0x03 PARAMS report for voice d[5] & 7: the machine (voice byte & 0x78, exactly; only
// the voice's current engine renders), P1..P16, and on a trig the note. Per message, never per sample: cold.
__attribute__((noinline)) CORE_COLD void voice_report(int cmd, const uint8_t* d, int len) {
  int vi = d[5] & 7;
  const uint8_t* raw;
  int note = -1, vel = 0;
  int np;
  int tag = 0;   // the sound tag (spec 2.6): a 43-byte 0x04 / 41-byte 0x03 ends with it, else 0 (older firmware)
  if (cmd == 0x04 && len >= 25) { note = d[6]; vel = d[7]; raw = d + 8; np = len >= 41 ? 16 : 8; if (len == 43) tag = d[40] << 7 | d[41]; }
  else if (cmd == 0x03 && len >= 23) { raw = d + 6; np = len >= 39 ? 16 : 8; if (len == 41) tag = d[38] << 7 | d[39]; }
  else return;
  if (cmd == 0x04) silenced[vi] = false;
  if (cmd == 0x04 && hooks.trig_seen) hooks.trig_seen();
  V& x = v[vi];
  bool was_juno = x.juno, was_fm = x.fm;
  int was_kind = engine_sig(x);   // E0: the engine (the instances' kind)
  int m = machine_of(d[5]);
#if CORE_MSAMP
  bool was_ms = x.ms, was_wt = x.wt, was_gr = x.gr, was_cp = x.cp;
  if (nm::forced(vi) && user_slot_of(m) < 0) m = nm::forced(vi);   // msamp= / wt= / gr=voice:V (the PoC, msamp.h)
  if (livein::forced(vi) && user_slot_of(m) < 0) m = kLiveMachine;   // livein=voice:V (until the Rytm reports 0x78)
  bool was_li = x.li;
  x.li = m == kLiveMachine;
  if (was_li != x.li) livein::silence(vi);   // (left it, or a fresh start: the read point at the newest block)
  x.ms = m == kMultiMachine;
  x.wt = m == kWaveMachine;
  x.gr = m == kGranMachine;
  x.cp = m == kChopMachine;
  if (was_ms && !x.ms) msamp::silence(vi);
  if (was_wt && !x.wt) wt::silence(vi);
  if (was_gr && !x.gr) gr::silence(vi);
  if (was_cp && !x.cp) chop::silence(vi);
#endif
  bool was_md = x.md;
  x.md = m == kMd;
  x.juno = m == kJuno;
  x.fm = m == kFm;
  x.grain = m == kGrainMachine;
  x.daisy = m == kDaisy || m == kDspSample || x.grain;
  x.dsps = m == kDspSample || x.grain;
  if (!x.dsps) ram_release(vi);   // another machine: no RAM note to resume
  decode_params(raw, np, x.p);
  ui::remote_params(vi, m, x.p, np);   // the remote UI's held edits (core/ui/remote.h)
#if CORE_POLY
  seq::params_in(vi, x.p, np);         // seq-ux: a param the sequencer's step locked keeps its lock (core/seq/seq.h)
#endif
  lfo::voice_params(vi, x.p, np, cmd == 0x04);   // the Sidecar's LFOs on top (core/lfo.h)
  apply(x);
  if (was_fm && !x.fm) fms[vi].Leave(&x.voice, &x.mod);   // PLAITS gets its ROM bank back
#if CORE_POLY
  if (was_fm && !x.fm) poly::silence(vi);
#endif
  int was_user = x.user;
  x.user = (int8_t)user_slot_of(m);   // U1..U5: 0x50 0x58 0x60 0x68 0x70
#if CORE_POLY
  // E0: the sequencer's instances were of another engine: gone (a new note makes them again); else they take the knobs
  if (engine_sig(x) != was_kind || x.user != was_user)
    inst::silence(vi);
  bool note_machine = x.fm;
#if CORE_MSAMP
  note_machine = note_machine || x.ms || x.wt || x.gr || x.cp;
#endif
  // E0: any machine on a Sidecar-sequenced track (core/seq/seq.h): its own trig makes no note, its pad does (fix1040b's
  // rule, the note machines' and 6-OP FM's below)
  if (note >= 0 && !note_machine && seq::owns_voice(vi) && !poly::seq_pad(vi, note, vel)) note = -1;
#endif
  if (x.user >= 0) {
    if (hooks.user_report) hooks.user_report(vi, x.user, was_user, note, vel, tag);
    else x.live = false;   // no machine plugins: a user slot is silent (never another machine)
#if CORE_POLY
    inst::knobs(vi);   // E0: its instances get the params as the voice's own state did
#endif
    return;
  }
  if (hooks.user_leave) hooks.user_leave(vi);   // left the user slots: voice_leave; a return starts afresh
  if (x.md) {   // MD (core/md/md.h): its knobs, then the hit; DIGIDRUM's old page (no MD tag) is read as MD
#if CORE_MD
    if (!was_md) md::enter(vi);   // a fresh voice (FM BD), then this report's knobs
    md::report(vi, x.p, np, tag);
    if (!was_md && tag != md::kLayoutTag)
      log(CORE_STR("voice %d: DIGIDRUM's page (0x40 without the MD tag): plays as MD %s"), vi, md::set_name(md::voice_set(vi)));
    if (note >= 0) {
      md::trig(vi, vel);
      x.live = true;
      log(CORE_STR("trig voice %d MD %s"), vi, md::set_name(md::voice_set(vi)));
    }
#else
    x.live = false;   // no MD on this board (a PI voice's state does not fit): silent, said once a change
    if (!was_md) log(CORE_STR("voice %d: MD (0x40) is not on this board: silent"), vi);
#endif
    return;
  }
#if CORE_MSAMP
  if (x.li) {   // LIVE IN (core/livein/livein.h): always rendering (MON ALW passes the input); a trig opens MON TRIG
    x.live = true;
    if (note >= 0) livein::trig(vi);
    return;
  }
  if (x.ms || x.wt || x.gr || x.cp) {   // the note machines (core/nmach.h)
#if CORE_POLY
    // a bound pad's own trig (core/poly.h's live binding): its live notes play it; the params still go
    if (note >= 0 && poly::take_live(vi, note, vel)) note = -1;
    if (note >= 0 && seq::owns_voice(vi) && !poly::seq_pad(vi, note, vel)) note = -1;   // a Sidecar-sequenced track: its
                                                  // own trig makes no note, its pad does (fix1040b)
#endif
    nm::report(vi, note, vel, x.p);
    return;
  }
#endif
  if (x.fm) {   // PAT picks the voice at a trig (a p-locked one too), or when the
                // voice becomes FM; a 0x03's PAT waits for the next trig, so a
                // sounding note is not cut by a voice change. P2..P8 the machine's
    if (note >= 0 || !was_fm) fm_select(vi, (int)x.p[0]);
    else fm_pat[vi] = (int)x.p[0];
    fms[vi].SetKnobs(x.p);
#if CORE_POLY
    poly::knobs(vi, x.p);
#endif
  }
  if (x.juno)   // PAT steps through the project's JUNO list, if any; the
                // knobs follow the sound's PAT (0x03), not a p-locked one (0x04)
    juno_select(vi, (int)x.p[0], cmd == 0x03 || !was_juno, !was_juno);
#if CORE_POLY
  if (x.juno) inst::knobs(vi);   // E0: the instances' knobs (a PAT change reaches them at their next note)
#endif
  if (x.dsps && np >= 16) prepare_stream(vi);
#if CORE_MSAMP
  if (x.dsps && np >= 16 && srclib::index_of(srclib::code_of(x.p[10])))   // the used set: SMP's row (rui1045: its bank)
    pools::seen(pools::kSamples, srclib::code_of(x.p[10]));
#endif
  if (note >= 0 && x.fm) {
#if CORE_POLY
    if (seq::owns_voice(vi) && !poly::seq_pad(vi, note, vel)) {   // a Sidecar-sequenced track (core/seq/seq.h): the
      poly::take_live(vi, note, vel);                               // pads' binding only, no note - but its own pad in
      return;                                                       // the normal pad mode plays (fix1040b)
    }
    if (poly::take_trig(vi, note, vel, x.p)) return;   // a chord (CHD on), or a live pad's own trig (core/poly.h)
#endif
    fm_note_on(vi, note, vel);
  } else if (note >= 0) {
    play_mono(vi, note, vel);
  }
}

// A trig's note on the mono machines (JUNO, DAISYSP / DSP SAMPLE / GRAIN, PLAITS): voice_report's, and the
// sequencer's mono path (voice_play).
static CORE_COLD void play_mono(int vi, int note, int vel) {
  V& x = v[vi];
  if (x.juno) {
    junos[vi].NoteOn(note, (vel < 1 ? 1 : vel) / 127.0f);
    x.live = true;
    log(CORE_STR("trig voice %d JUNO %s note %d"), vi, junos[vi].name(), note);
  } else if (x.daisy) {
    // TUN semitones; P9..P16 are the sound's own sample page
    x.note = x.target = note + roundf(x.p[6] - 64);
    float l = (vel < 1 ? 1 : vel) / 127.0f * x.p[7] / 127.0f;
    x.level = l < 0.05f ? 0.05f : (l > 1 ? 1 : l);
    x.trig = true;
    x.live = true;
    if (x.grain && !psram) {   // GRAIN plays from the sample RAM only: silent, and said (the page names it too)
      x.live = false;
      log(CORE_STR("trig voice %d GRAIN: no sample RAM (PSRAM) on this board"), vi);
    } else if (x.dsps) {
      dsps_env_trig(vi);
      trigger_stream(vi, note);
      // SMP (the pool slot), MIX, STA END LEV: what a silent sample voice is missing
      log(CORE_STR("trig voice %d note %d DSP SAMPLE model %d smp %d slot %d mix %d sta %d end %d lev %d"), vi, (int)x.note,
          daisy_model(x), (int)x.p[10], pool_slot(x.p[10]), (int)x.p[4], (int)x.p[12], (int)x.p[13], (int)x.p[15]);   // (slot: rui1045's bank * 127 + row)
    } else {
      log(CORE_STR("trig voice %d note %d DAISYSP model %d"), vi, (int)x.note, daisy_model(x));
    }
  } else {
    // TUN in semitones, FIN a semitone either way; VEL how much velocity counts
    x.target = note + roundf(x.p[6] - 64) + (x.p[9] - 64) / 64.0f;
    if (x.p[14] <= 0) x.note = x.target;
    float sens = x.p[13] / 127.0f;
    float l = (1 - sens + sens * (vel < 1 ? 1 : vel) / 127.0f) * x.p[7] / 127.0f;
    x.level = l < 0.05f ? 0.05f : (l > 1 ? 1 : l);
    x.trig = true;
    x.live = true;
    log(CORE_STR("trig voice %d note %d engine %d"), vi, (int)x.note, x.patch.engine);
  }
}

// E0: the sequencer's mono note (voices.h): the voice's own machine plays it as a Rytm trig would (and wakes it)
CORE_COLD void voice_play(int vi, int note, int vel) {
  vi &= 7;
  V& x = v[vi];
  silenced[vi] = false;
  if (x.md) {   // MD: the sequencer's note is a hit (pitch none: PTCH is its own knob)
#if CORE_MD
    md::trig(vi, vel);
    x.live = true;
#endif
    return;
  }
  if (x.user >= 0) {   // a user machine: its slot's state, as a trig (untagged: the slot's own sound)
    if (hooks.user_report) hooks.user_report(vi, x.user, x.user, note, vel, 0);
    return;
  }
  if (x.fm) {
    fm_note_on(vi, note, vel);
    return;
  }
#if CORE_MSAMP
  if (x.cp) {   // CHOP: the sequencer's mono note is the slice (note 0 = slice 1), as its poly notes are
    chop::seq_hit(vi, note, vel);   // (released by the voice's note-off, as a trig's)
    return;
  }
  if (x.ms || x.wt || x.gr) {
    nm::report(vi, note, vel, x.p);
    return;
  }
  if (x.li) {   // LIVE IN: the note is MON TRIG's trig (its pitch and velocity unused)
    x.live = true;
    livein::trig(vi);
    return;
  }
#endif
  play_mono(vi, note, vel);
}

// 6-OP FM's mono note on voice vi (a trig with CHD off; core/poly.cpp's held trigs too).
CORE_COLD void fm_note_on(int vi, int note, int vel) {
  fms[vi].NoteOn(note, (vel < 1 ? 1 : vel) / 127.0f);
  v[vi].live = true;
  log(CORE_STR("trig voice %d 6-OP FM entry %d note %d"), vi, fm_key[vi] & 0xff, note);
}

// The control side changed x.p between reports (core/lfo.h): the machine takes the new values as a 0x03 would, without
// the report's selections (FM's PAT at a trig, JUNO's list, DSP SAMPLE's prepare); a user machine gets a parameter call.
CORE_COLD void voice_params_changed(int vi) {
  V& x = v[vi & 7];
  if (x.user >= 0) {
    user_report_internal = true;
    if (hooks.user_report) hooks.user_report(vi & 7, x.user, x.user, -1, 0, 0);
    user_report_internal = false;
#if CORE_POLY
    inst::knobs(vi & 7);
#endif
    return;
  }
  apply(x);
#if CORE_MD
  if (x.md) md::params(vi & 7, x.p);
#endif
#if CORE_MSAMP
  if (x.ms || x.wt || x.gr || x.cp) nm::report(vi & 7, -1, 0, x.p);
#endif
  if (x.fm) fms[vi & 7].SetKnobs(x.p);
#if CORE_POLY
  if (x.fm) poly::knobs(vi & 7, x.p);
#endif
  if (x.juno) junos[vi & 7].SetKnobs(x.p);
#if CORE_POLY
  inst::knobs(vi & 7);   // E0: the sequencer's instances (JUNO's knobs; PLAITS / DAISYSP read x.p as they render)
#endif
}

static inline int16_t clip16(float y) { return (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y); }
// DSP SAMPLE's envelope on PRE / POST: the control core's counters, the render core's state
static volatile uint32_t g_env_trig[kVoices], g_env_off[kVoices];
static uint32_t r_env_trig[kVoices], r_env_off[kVoices];
static float r_env_g[kVoices] = {1, 1, 1, 1, 1, 1, 1, 1};
static bool r_env_rel[kVoices];
CORE_COLD void dsps_env_trig(int vi) { __atomic_fetch_add(&g_env_trig[vi & 7], 1u, __ATOMIC_RELEASE); }
CORE_COLD void dsps_env_off(int vi) { __atomic_fetch_add(&g_env_off[vi & 7], 1u, __ATOMIC_RELEASE); }
#if CORE_POLY
static volatile uint32_t g_limited[kVoices];
// o (-1..1 before the 0.7 headroom) into the ring through the soft limiter (voices.h kSoftKnee): a compare a sample,
// tanhf only over the knee
static inline void soft_out(int16_t (*ring)[kVoices], const int* row, int i, const float* o) {
  bool over = false;
  for (int s = 0; s < kBlock; ++s) {
    float y = o[s] * 0.7f, a = y < 0 ? -y : y;
    if (a > kSoftKnee) {
      a = kSoftKnee + (kSoftCeil - kSoftKnee) * tanhf((a - kSoftKnee) / (kSoftCeil - kSoftKnee));
      y = y < 0 ? -a : a;
      over = true;
    }
    ring[row[s]][i] = clip16(y * 32767.0f);
  }
  if (over) __atomic_fetch_add(&g_limited[i], 1u, __ATOMIC_RELAXED);
}
uint32_t voice_limited(int vi) { return __atomic_load_n(&g_limited[vi & 7], __ATOMIC_RELAXED); }
// soft_out for a stereo pair: L into ring, R into ring_r
static inline void soft_out2(int16_t (*ring)[kVoices], int16_t (*ring_r)[kVoices], const int* row, int i, const float* o,
                             const float* r) {
  bool over = false;
  for (int c = 0; c < 2; ++c) {
    const float* x = c ? r : o;
    int16_t (*d)[kVoices] = c ? ring_r : ring;
    for (int s = 0; s < kBlock; ++s) {
      float y = x[s] * 0.7f, a = y < 0 ? -y : y;
      if (a > kSoftKnee) {
        a = kSoftKnee + (kSoftCeil - kSoftKnee) * tanhf((a - kSoftKnee) / (kSoftCeil - kSoftKnee));
        y = y < 0 ? -a : a;
        over = true;
      }
      d[row[s]][i] = clip16(y * 32767.0f);
    }
  }
  if (over) __atomic_fetch_add(&g_limited[i], 1u, __ATOMIC_RELAXED);
}
#else
uint32_t voice_limited(int) { return 0; }
#endif
static inline int mach_of(const V& p) {
#if CORE_MSAMP
  if (p.ms || p.wt || p.gr || p.li || p.cp) return kMDsps;   // (the sample machines' column; WAVETABLE, GRANULAR, LIVE IN, CHOP too)
#endif
  return p.user >= 0 ? kMUser : p.fm ? kMFm : p.juno ? kMJuno : p.dsps ? kMDsps : p.daisy ? kMDaisy : p.md ? kMMd : kMPlaits;
}

// One block of voices v0..v1-1. Hot: ITCM on the Teensy (no CORE_COLD).
uint32_t render_voices(int16_t (*ring)[kVoices], int ring_len, int w, int v0, int v1, uint64_t* mach,
                       int16_t (*ring_r)[kVoices], uint8_t* stm) {
  plaits::Voice::Frame f[kBlock];
  bool stv[kVoices] = {};   // the voice renders a stereo pair this block (OUT PRE / POST)
#if CORE_POLY
  if (ring_r)
    for (int i = v0; i < v1; ++i) stv[i] = out::stereo(i);
#else
  (void)ring_r;
#endif
  // the block's ring rows, wrapped once here (w may sit near the end: a wrap mid-block) rather than a
  // division per sample per voice (ring_len is a runtime value)
  int row[kBlock];
  for (int s = 0, r = w % ring_len; s < kBlock; ++s) {
    row[s] = r;
    if (++r == ring_len) r = 0;
  }
  uint32_t t0 = cycles();
  uint32_t tv = t0;
  for (int i = v0; i < v1; ++i) {
    V& x = v[i];
    if (i > v0) {   // the previous voice's cycles to its machine
      uint32_t now = cycles();
      const V& p = v[i - 1];
      if (p.live) mach[mach_of(p)] += now - tv;
      tv = now;
    }
    if (!x.live) {
      for (int s = 0; s < kBlock; ++s) ring[row[s]][i] = 0;
      continue;
    }
    if (x.user >= 0) {
      float o[kBlock];
      if (hooks.user_render) hooks.user_render(i, o);
      else for (int s = 0; s < kBlock; ++s) o[s] = 0.0f;
      for (int s = 0; s < kBlock; ++s) ring[row[s]][i] = clip16(o[s] * 0.7f * 32767.0f);
      continue;
    }
#if CORE_MSAMP
    if (x.li) {   // LIVE IN (core/livein/livein.h): AUDIO IN as INPT / GAIN / GATE / MON say; PRE / POST: a pair
      float o[kBlock], r[kBlock];
      livein::render(i, x.p, o, stv[i] ? r : nullptr, stv[i], kBlock);
      for (int s = 0; s < kBlock; ++s) ring[row[s]][i] = clip16(o[s] * 0.7f * 32767.0f);
      if (stv[i])
        for (int s = 0; s < kBlock; ++s) ring_r[row[s]][i] = clip16(r[s] * 0.7f * 32767.0f);
      continue;
    }
    if (x.ms || x.wt || x.gr || x.cp) {   // MULTISAMPLE, WAVETABLE, GRANULAR, CHOP (core/nmach.h)
      float o[kBlock] = {0};
      if (stv[i]) {   // OUT PRE / POST: the pair (WAVETABLE: L = R)
        float r[kBlock] = {0};
        if (!nm::render_add2(i, o, r, kBlock)) {
          nm::render_add(i, o, kBlock);
          for (int s = 0; s < kBlock; ++s) r[s] = o[s];
        }
        soft_out2(ring, ring_r, row, i, o, r);
        continue;
      }
      nm::render_add(i, o, kBlock);
      soft_out(ring, row, i, o);
      continue;
    }
#endif
    if (x.fm) {
      float o[kBlock];
      fms[i].Render(&x.voice, &x.patch, &x.mod, fm_stage[plaits::fm_core()], o, kBlock);
#if CORE_POLY
      poly::render_add(i, o, kBlock, fm_stage[plaits::fm_core()]);   // its chord notes (core/poly.h)
      soft_out(ring, row, i, o);
#else
      for (int s = 0; s < kBlock; ++s) ring[row[s]][i] = clip16(o[s] * 0.7f * 32767.0f);
#endif
      continue;
    }
#if CORE_MD
    if (x.md) {   // MD (core/md/md.h): mono, L = R on PRE / POST as the other mono machines
      float o[kBlock];
      md::render(i, o, kBlock);
      for (int s = 0; s < kBlock; ++s) ring[row[s]][i] = clip16(o[s] * 0.7f * 32767.0f);
      continue;
    }
#endif
    if (x.juno) {
      float o[kBlock];
      junos[i].Render(o, kBlock);
      for (int s = 0; s < kBlock; ++s) ring[row[s]][i] = clip16(o[s] * 0.7f * 32767.0f);
      continue;
    }
    if (x.dsps) {
      // the card sample straight into the resonator; MIX dry/wet (as sim's
      // render_dsps); SRC: ENG STR BRI DMP MIX FB TUN LEV, SMPL by word: 17 TUN
      // 18 FIN 19 SMP (pool slot) 20 FLD (unused; MOD with PSRAM) 21 STA 22 END 23 LOP 24 LEV
      float in[kBlock], o[kBlock], inr[kBlock];
      float* rr = stv[i] ? inr : nullptr;   // OUT PRE / POST: the sample's R
      float semis = (x.p[8] - 64) + (x.p[9] - 64) / 64.0f + (trig_note[i] - 60);
      bool loop = x.p[14] >= 1;
      if (from_ram[i]) {
        float gp[8];
        if (x.grain) engines[i].SetSpray(x.p[11] / 127.0f);
        engines[i].Render(sample_params(x, gp), in, kBlock, rr);   // MOD's mode (the GRAIN machine's: GRAIN), from the sample RAM
      } else {
        if (psram && dsp_mode(x) == rytm::SampleModes::kLoop) loop = true;   // LOOP not in RAM yet
        streams[i].render(semis, x.p[12] / 120.0f, x.p[13] / 120.0f, loop, x.p[15] / 100.0f, in, kBlock, rr);
      }
      float mix = x.grain ? 0.0f : x.p[4] / 127.0f;   // the GRAIN machine: no resonator
      if (mix > 0.0f) {   // MIX 0: the sample alone, no resonator to run
        int model = daisy_model(x);
        float ms[kBlock];   // a stereo pair excites the resonator with its mono sum
        if (rr)
          for (int s = 0; s < kBlock; ++s) ms[s] = 0.5f * (in[s] + inr[s]);
        daisy[i].Render(model, x.note, x.p[1] / 127.0f, x.p[2] / 127.0f, x.p[3] / 127.0f, 0.8f,
                        0.25f, x.trig, x.level, o, kBlock, rr ? ms : in);
      } else {
        for (int s = 0; s < kBlock; ++s) o[s] = 0.0f;
      }
      x.trig = false;
      if (rr) {   // PRE / POST: the pair, the resonator (fed the mono sum: see below) on both sides; DEC / the release
        uint32_t tg = __atomic_load_n(&g_env_trig[i], __ATOMIC_ACQUIRE), of = __atomic_load_n(&g_env_off[i], __ATOMIC_ACQUIRE);
        if (tg != r_env_trig[i]) r_env_trig[i] = tg, r_env_off[i] = of, r_env_g[i] = 1.0f, r_env_rel[i] = false;
        if (of != r_env_off[i]) r_env_off[i] = of, r_env_rel[i] = true;
        float g = r_env_g[i];
#if CORE_POLY
        const float dk = out::dec_coef(out::control.dec[i]);
#else
        const float dk = 1.0f;
#endif
        for (int s = 0; s < kBlock; ++s) {
          g = r_env_rel[i] ? g - 1.0f / 96.0f : g * dk;   // the release: 2 ms
          g = g < 0.0f ? 0.0f : g;
          float wet = mix * o[s];
          ring[row[s]][i] = clip16(tanhf((1 - mix) * in[s] + wet) * g * 0.7f * 32767.0f);
          ring_r[row[s]][i] = clip16(tanhf((1 - mix) * inr[s] + wet) * g * 0.7f * 32767.0f);
        }
        r_env_g[i] = g;
        continue;
      }
      for (int s = 0; s < kBlock; ++s)
        ring[row[s]][i] = clip16(tanhf((1 - mix) * in[s] + mix * o[s]) * 0.7f * 32767.0f);
      continue;
    }
    if (x.daisy) {
      float o[kBlock];
      int model = daisy_model(x);
      daisy[i].Render(model, x.note, x.p[1] / 127.0f, x.p[2] / 127.0f, x.p[3] / 127.0f,
                      0.8f, x.dsps ? 0.25f : x.p[5] / 127.0f, x.trig, x.level, o, kBlock);
      // P5 is INP (INT or an analog voice). The Rytm's voice audio does not
      // reach the engine yet, so every INP plays the built-in strike here.
      x.trig = false;
      for (int s = 0; s < kBlock; ++s) ring[row[s]][i] = clip16(o[s] * 0.7f * 32767.0f);
      continue;
    }
    if (x.p[14] > 0) {   // GLD: up to 0.5 s
      float k = kBlock / 48000.0f / (0.002f + x.p[14] / 127.0f * 0.5f);
      x.note += (x.target - x.note) * (k > 1 ? 1 : k);
    }
    x.patch.note = x.note;
    x.mod.level = x.level;
    x.mod.trigger = x.trig ? 1.0f : 0.0f;
    x.trig = false;
    x.voice.Render(x.patch, x.mod, f, kBlock);
    float aux = x.p[10] / 127.0f, gain = x.p[15] / 100.0f * 0.7f;   // AUX, VOL
    for (int s = 0; s < kBlock; ++s) ring[row[s]][i] = clip16((f[s].out * (1 - aux) + f[s].aux * aux) * gain);
  }
  // a mono machine on PRE / POST: L = R before its stereo chain (the user slots, FM, JUNO, ... wrote L only)
  for (int i = v0; i < v1; ++i)
    if (stv[i] && !(v[i].live && (v[i].ms || v[i].gr || v[i].wt || v[i].li || v[i].cp || v[i].dsps)))
      for (int s = 0; s < kBlock; ++s) ring_r[row[s]][i] = ring[row[s]][i];
#if CORE_POLY
  for (int i = v0; i < v1; ++i) {   // E0: the sequencer's instances (core/inst.h) after the voice's own sound, before its chain
    if (!inst::active(i)) continue;
    uint32_t t1 = cycles();
    float l[kBlock], r[kBlock];
    const float k = 1.0f / (0.7f * 32767.0f);   // the rows back into the voice's units
    for (int s = 0; s < kBlock; ++s) l[s] = ring[row[s]][i] * k, r[s] = stv[i] ? ring_r[row[s]][i] * k : l[s];
    inst::render_add(i, l, r, stv[i]);
    if (stv[i]) soft_out2(ring, ring_r, row, i, l, r);
    else soft_out(ring, row, i, l);
    mach[mach_of(v[i])] += cycles() - t1;
  }
  for (int i = v0; i < v1; ++i) ins::voice_out(i, ring, row, kBlock, stv[i] ? ring_r : nullptr);   // the voice's INSERT chain (core/fx/insert.h)
#if CORE_MSAMP
  for (int i = v0; i < v1; ++i)   // LIVE IN after its chain: the limiter, the feedback guard, the measure's clicks
    if (v[i].li && v[i].live) livein::post(i, ring, stv[i] ? ring_r : nullptr, row, kBlock);
#endif
#endif
  for (int i = v0; i < v1; ++i) {   // CC 120: the silenced voices ramp out (5 ms), a trig brings them back (1 ms)
    float g = silence_gain[i];
    bool down = silenced[i];
    if (!down && g >= 1.0f) continue;
    for (int s = 0; s < kBlock; ++s) {
      g += down ? -1.0f / 240.0f : 1.0f / 48.0f;
      g = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g;
      ring[row[s]][i] = (int16_t)(ring[row[s]][i] * g);
      if (stv[i]) ring_r[row[s]][i] = (int16_t)(ring_r[row[s]][i] * g);
    }
    silence_gain[i] = g;
  }
#if CORE_POLY
  seq::cal_render(ring, row, v0, v1);   // the sequencer's latency click (seq=cal), when one is due: one compare else
#endif
  if (ring_r)   // TRK voices: R = L; the pairs' real-stereo bits
    for (int i = v0; i < v1; ++i) {
      bool diff = false, any = false;
      if (!stv[i])
        for (int s = 0; s < kBlock; ++s) ring_r[row[s]][i] = ring[row[s]][i];
      else
        for (int s = 0; s < kBlock && !diff; ++s) diff = ring_r[row[s]][i] != ring[row[s]][i], any |= ring[row[s]][i] != 0;
      // a silent block keeps the last bit (the PAN law does not flip at a note's start or in its tail)
      if (stm && (!stv[i] || diff || any)) stm[i] = diff;
    }
  uint32_t dt = cycles() - t0;
  if (v1 > v0 && v[v1 - 1].live) mach[mach_of(v[v1 - 1])] += t0 + dt - tv;
  return dt;
}

CORE_COLD bool voice_sound(int vi, VoiceSound* o) {
  vi &= 7;
  memset(o, 0, sizeof *o);
  const V& x = v[vi];
  for (int k = 0; k < 16; ++k) o->p[k] = x.p[k];
  if (x.fm) {
    o->machine = kFm;
    o->has_dx7 = fm_bytes((int)x.p[0], o->dx7);
    memcpy(o->name, o->dx7 + 118, 10);   // the DX7 voice's name (packed bytes 118..127)
    return true;
  }
  if (x.juno) {
    o->machine = kJuno;
    const uint8_t* b;
    const char* name;
    juno_pick((int)x.p[0], &b, &name);
    memcpy(o->juno, b, 18);
    o->juno_ext = junolib.listed() > 0;
    strncpy(o->name, name ? name : "", sizeof o->name - 1);
    return true;
  }
  return false;
}

}  // namespace core
