// core/engine.cpp - see engine.h. Boot and between-requests work: cold.
#include "engine.h"

#include "fmt/fmt.h"
#include "fx/send_fx.h"
#include "fx/insert.h"
#include "keys.h"
#include "lfo.h"
#include "seq/seq.h"
#include "song/song.h"
#if CORE_MSAMP
#include "bundle/bundle.h"
#include "bundle/kits.h"
#include "bundle/prjslot.h"
#include "bundle/projects.h"
#include "bundle/rename.h"
#include "msamp/msamp.h"
#include "msamp/sdtest.h"
#include "nmach.h"
#include "livein/livein.h"
#endif

namespace core {

CORE_COLD bool engine_begin() {
  // Plaits' scratch (128 KB, before the voices take it) sorts an index rebuild
#if CORE_MSAMP
  {   // an import cut by a power loss: finished or undone before anything reads the card (core/bundle/bundle.h)
    Fs fs;
    if (fs.begin()) {
      bundle::recover();
      fmt::sweep_boot();   // 2026-10-26: every atomic write's .prev / .tmp, for the readers that never recover()
    }
  }
#endif
  card_ok = cards.begin(log, shared, sizeof shared[0] * kVoices);   // (a pointer on CORE_HEAP_BUFFERS)
  sample_ram_init();
  if (card_ok) junolib.begin(&cards.sd(), log);
  if (card_ok) dxlib.begin(&cards.sd(), log);
  if (card_ok) project_load();
#if CORE_MSAMP
  // the project's LFOs and inserts (core/lfo.h, core/fx/insert.h): its files into the RAM store first, by the card
  // core from the idle pass, then loaded from there (project_fill_step)
  if (card_ok) {
    // the project named now, not "" until the fill ends: an edit before it is saved into this project's files (and
    // put into its store, which the fill keeps)
    lfo::rekey(project);
#if CORE_POLY
    ins::rekey(project);
#endif
    project_fill_begin();
  }
#else
  if (card_ok) lfo::load();   // the project's LFOs (core/lfo.h)
#endif
#if CORE_POLY
#if !CORE_MSAMP
  if (card_ok) ins::load();   // the project's inserts (core/fx/insert.h)
#endif
  amp::begin(card_ok);        // AMP's captures: /NAM/ listed at the next steps (core/fx/amp.h)
#endif
  if (card_ok) fx::card_load();
  if (card_ok) fxin::load();
  if (card_ok) sendfx_project();
  voices_init();
#if CORE_POLY
  seq::init();   // the Sidecar sequencer (core/seq/seq.h): its pattern buffers; its tracks and A01 once the store is full
  if (card_ok) keys::load();   // the Pi's own keyboards' routing (core/keys.h)
#endif
#if CORE_MSAMP
  if (card_ok) sdtest::load();   // the SD card test's last result (msamp/sdtest.h)
  msamp::begin(card_ok);   // MULTISAMPLE: the arena, the /MULTISAMPLE list queued (msamp/msamp.h)
  nm::begin(card_ok);      // WAVETABLE's built-in tables, GRANULAR's buffers; /WAVETABLES and /GRANULAR queued
#endif
  return card_ok;
}

// ---- the idle steps' cost (engine.h IdleSlow)
namespace {
const char* g_slow_name = "";
uint32_t g_slow_us = 0;
template <class F>
CORE_COLD bool timed(const char* n, F f) {
  uint32_t t = now_us();
  bool r = f();
  idle_note(n, now_us() - t);
  return r;
}
}  // namespace
CORE_COLD void idle_note(const char* name, uint32_t us) {
  if (us > g_slow_us) g_slow_us = us, g_slow_name = name;
}
CORE_COLD IdleSlow idle_slowest_take() {
  IdleSlow r = {g_slow_name, g_slow_us};
  g_slow_name = "", g_slow_us = 0;
  return r;
}
#define T_(n, e) timed(n, [&]() -> bool { return (e); })
#define TV_(n, e) timed(n, [&]() -> bool { e; return false; })

CORE_COLD bool idle_prepare() { return T_("prepare", prepare_pending()); }

CORE_COLD bool idle_save() {
#if CORE_MSAMP
  TV_("rename", rename::tick(now_ms()));       // a project rename's looks / moves (card-queue steps) and its question's timeout
  if (rename::moving()) return false;   // (no save while the old project's files move: FLUSH made them first)
  TV_("prjslot", prjslot::tick(now_ms()));      // the slot reports' jobs: copies, the trash, a switch (card-queue steps), the question
  if (prjslot::moving()) {   // (no save while a project's files move or are copied: the job's FLUSH made them) -
    TV_("post", fmt::post_step());        // only the posted writes land (the job's own journal line among them)
    return false;
  }
  // a project load's RAM store fill (pool.h): one step; nothing below reads the store (the saves after it are of the
  // project still loaded, and post_step lands the writes a fill waits for)
  if (T_("fill", project_fill_step())) return true;
  if (T_("kits", kits::tick())) return true;   // the Rytm's SAVE / RELOAD KIT: the per-kit files' saved copies (core/bundle/kits.h)
#endif
  // 0.3.0 power-cut safety: the posted settings write first (fmt.h), so one writer owns the shared buffer at a time.
  // A write merely held (the card core held up past kTakeBackUs, phase 3) does not stop the rest: post_begin is null
  // while the slot is busy, so no saver below can touch its buffer, and the sequencer's loads, the SONG journal and the
  // queued saves need no card core of their own here - returning would freeze them for as long as the hold lasts
  if (T_("post", fmt::post_step()) && !fmt::post_stalled()) return true;
  if (fx::card_dirty && fmt::post_begin()) {   // (the slot or the queue busy: card_save would not post - not a step)
    TV_("fxsave", fx::card_save());
    return true;
  }
  if (T_("fxin", fxin::save())) return true;
  if (T_("lfo", lfo::save())) return true;
#if CORE_POLY
  if (T_("ins", ins::save())) return true;
#endif
#if CORE_POLY
  if (T_("keys", keys::save())) return true;
  if (T_("seq", seq::idle())) return true;   // the sequencer's pattern files: load, prefetch, save
  if (T_("song", song::idle())) return true;  // SONG RECORD: the journal, the song file, a load, a recovery (core/song/song.h)
#endif
#if CORE_MSAMP
  bool ix = T_("projects", projects::save());   // the project index, through the card queue (core/bundle/projects.h)
#else
  bool ix = false;
#endif
  if (!project_dirty) return ix;
  TV_("project", project_save());   // (with the index: the same project change's two small files, one step)
  return true;
}

// With the sample RAM: a stream within ~85 ms of running dry first, then a load step (one open or one 8 KB read),
// then the streams' top-ups.
// MULTISAMPLE's streams and loads (one card job) before the streams' top-ups.
CORE_COLD void idle_stream() {
  if (psram) {
    if (T_("refill", refill_neediest(4096)) || T_("psram", psram->step())) return;
  }
#if CORE_MSAMP
  TV_("livein", livein::step());               // LIVE IN's news (a feedback mute, a measure's result) into the log: no card, no wait
  if (T_("sdtest", sdtest::step())) return;   // the SD card test while it runs (msamp/sdtest.h)
  if (msamp::step()) return;   // (timed inside, by part)
  if (T_("nm", nm::step())) return;   // WAVETABLE's and GRANULAR's loads (core/nmach.h)
#endif
#if CORE_POLY
  if (T_("amp", amp::step())) return;  // AMP: the /NAM/ list, a capture's load (card reads posted to the card core), its prewarm
#endif
  TV_("refill", refill_neediest(0xffffffffu));
}

}  // namespace core
