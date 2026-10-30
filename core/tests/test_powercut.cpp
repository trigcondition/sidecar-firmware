// core/tests/test_powercut.cpp - 0.3.0 "power-cut safety" (docs/BACKLOG.md, "Roadmap to 0.3.0", bugs before features):
// the Sidecar is switched off at the wall with no shutdown ritual, so every card file that holds state the user cannot
// get back again must survive a cut mid-save.
//
// Two separate things are checked here.
//
//  1. Atomic writes. Each writer goes through fmt::write_atomic (<path>.tmp written, read back and compared,
//     <path> -> <path>.prev, .tmp -> <path>, .prev removed), the small ones posted to the card core (fmt.h
//     post_begin / post_end), and a reader calls fmt::recover first. With host_power_cut(k) every card change past
//     the k'th fails, as if the power had gone: for every k the file must end up exactly the old one or exactly the
//     new one, never half, and the boot must leave no .tmp or .prev behind. The same injection covers a card with no
//     room left (every change fails from the start: the original untouched) and a failing rename.
//  2. Lossy reparse. A reader that parses the lines it knows and rewrites the file drops what a newer Sidecar wrote.
//     key=value settings (class S) carry the unknown lines forward unchanged (fmt::kv_keep / kv_append); the
//     positional per-kit files (LFO, INSERT) cannot merge an unknown line into their fixed slots, so they lock
//     instead and are never written over. A damaged or truncated file is refused whole, never half-applied.
//
// Where the I/O lands is checked too (the last section): a posted save charges the control core no card time at all -
// it renders audio in the same loop and its OUT FIFO holds 2 ms, while an atomic write of a few KB is tens of ms.
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core.h"
#include "fmt/fmt.h"
#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "host_link.h"
#include "keys.h"
#include "lfo.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "samp/out.h"
#include "seq/seq.h"
#include "store/store.h"

using namespace core;

static std::string g_card;
static uint64_t g_us = 5000000;

static void at(uint64_t us) { g_us = us, host_fake_time(true, g_us); }
static std::string rd(const std::string& p) {
  std::string s;
  FILE* f = fopen((g_card + p).c_str(), "rb");
  if (!f) return "<none>";
  char b[8192];
  for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
  fclose(f);
  return s;
}
static void put(const std::string& p, const std::string& s) {   // the fixture's own writes: not the card's "changes"
  CHECK(system(("mkdir -p \"$(dirname '" + g_card + p + "')\"").c_str()) == 0);
  FILE* f = fopen((g_card + p).c_str(), "wb");
  CHECK(f);
  fwrite(s.data(), 1, s.size(), f);
  fclose(f);
}
static bool exists(const std::string& p) {
  struct stat st;
  return stat((g_card + p).c_str(), &st) == 0;
}
static void rm(const std::string& p) { ::remove((g_card + p).c_str()); }
// the engine's between-requests work until it has nothing left (the saves wait a second after the last change)
static void flush() {
  at(g_us + 1100000);
  for (int i = 0; i < 64; ++i) idle_save();
}
// no writer is owed anything: a flush now changes nothing on the card (so a cut loop below measures one writer)
static void quiesce() {
  flush();
  flush();
  uint64_t c = host_changes();
  flush();
  CHECK(host_changes() == c);
}

// ---- the writers under test: how a boot reads the file, and how to put a given value in memory
struct Writer {
  const char* name;
  const char* path;
  void (*reload)();           // what a boot does: fmt::recover, then load
  void (*change)(int v);      // leave the module dirty with value v (v = 1 or 2)
};

// 2026-10-26, the RAM store (Task 4): the LFO and INSERT files are read from the store, which a project load (or the
// boot) fills from the card - so a file the fixture writes is seen after a refill, as the Sidecar sees it after a load
static void refill() {
  store::clear();
  lfo::want_files();
  ins::want_files();
  seq::want_files();   // (phase 2: the sequencer's files too)
  store::fill_begin();
  for (int i = 0; i < 100000 && !store::fill_step(); ++i) {
  }
  CHECK(store::ready());
}
static void loaded() {   // a project load (0x10): the store filled from the idle pass, then the LFOs and inserts
  for (int i = 0; i < 100000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
// the boot: its sweep (fmt::sweep_boot: a .prev put back, a stray .tmp dropped), the store filled, then the load
static void lfo_reload() { fmt::sweep_boot(), refill(), lfo::load(); }
static void lfo_change(int v) {
  char o[200];
  char c[40];
  snprintf(c, sizeof c, "lfo=2:dep:%d", v == 1 ? 11 : -22);
  CHECK(lfo::command("lfo=2:tgt:v3", o, sizeof o) && !strstr(o, "cannot"));
  CHECK(lfo::command(c, o, sizeof o) && !strstr(o, "cannot"));
}
static void ins_reload() { fmt::sweep_boot(), refill(), ins::load(); }
static void ins_change(int v) {
  ins::set_type(1, 0, ins::kEq);
  ins::set_param(1, 0, 2, v == 1 ? 100 : 20);
}
static void keys_reload() { keys::load(); }
static void keys_change(int v) {
  char o[200];
  char c[40];
  snprintf(c, sizeof c, "keys=auto:%d", v == 1 ? 5 : 9);
  CHECK(keys::command(c, o, sizeof o));
}
static void fxin_reload() { fxin::load(); }
static void fxin_change(int v) { fxin::set(v == 1); }
static void sendfx_reload() { fx::card_load(); }
static void sendfx_change(int v) {
  fx::control.route = v == 1 ? fx::kRouteMain : fx::kRouteFx;
  fx::card_dirty = true;
}

static const Writer kWriters[] = {
    {"LFO (per kit)", "/.rytm/lfo/PWR/k005.txt", lfo_reload, lfo_change},
    {"INSERT (per kit)", "/.rytm/ins/PWR/k005.txt", ins_reload, ins_change},
    {"keys.txt", "/.rytm/keys.txt", keys_reload, keys_change},
    {"fxin.txt", "/.rytm/fxin.txt", fxin_reload, fxin_change},
    {"sendfx.txt", "/.rytm/sendfx.txt", sendfx_reload, sendfx_change},
};

// A clean save of value v, and the bytes it leaves. No .tmp or .prev may be left over.
static std::string clean_save(const Writer& w, int v) {
  w.change(v);
  flush();
  std::string s = rd(w.path);
  CHECK(s != "<none>" && !s.empty());
  CHECK(!exists(std::string(w.path) + ".tmp") && !exists(std::string(w.path) + ".prev"));
  return s;
}

// The cut: for every k, the power goes after k more card changes. Afterwards the file is exactly A or exactly B.
static void test_cut(const Writer& w) {
  quiesce();
  std::string A = clean_save(w, 1), B = clean_save(w, 2);
  CHECK(A != B);
  int kept = 0, landed = 0;
  for (int k = 0; k < 60; ++k) {
    put(w.path, A);
    rm(std::string(w.path) + ".tmp");
    rm(std::string(w.path) + ".prev");
    w.reload();
    CHECK(rd(w.path) == A);
    w.change(2);
    host_power_cut(k);
    flush();
    host_power_cut(-1);   // the reboot
    w.reload();           // the boot's read: fmt::recover puts a .prev back and drops a stray .tmp
    std::string s = rd(w.path);
    CHECK(s == A || s == B);   // never half a file, never missing
    CHECK(!exists(std::string(w.path) + ".tmp"));
    CHECK(!exists(std::string(w.path) + ".prev"));
    if (s == A) ++kept;
    else ++landed;
  }
  CHECK(kept >= 3 && landed >= 1);   // the window was really hit, and a whole save really lands
  printf("powercut: %-18s %d cuts kept the old file, %d landed the new one, no .tmp / .prev left\n", w.name, kept,
         landed);
}

// A card with no room: every change fails from the first one. The original must be byte-identical and no .tmp left.
static void test_full_card(const Writer& w) {
  quiesce();
  std::string A = clean_save(w, 1);
  put(w.path, A);
  w.reload();
  w.change(2);
  host_power_cut(0);   // as a full card: every write, rename and remove refused
  flush();
  host_power_cut(-1);
  CHECK(rd(w.path) == A);
  CHECK(!exists(std::string(w.path) + ".tmp") && !exists(std::string(w.path) + ".prev"));
  w.reload();
  CHECK(rd(w.path) == A);
  printf("powercut: %-18s a full card leaves the original whole, no half .tmp\n", w.name);
}

int main() {
  char tmpl[] = "/tmp/core_powercut_XXXXXX";
  CHECK(mkdtemp(tmpl));
  g_card = tmpl;
  CHECK(mkdir((g_card + "/Samples").c_str(), 0755) == 0);   // a card (the index wants its /Samples)
  host_set_card(g_card.c_str());
  at(g_us);
  engine_begin();
  card_ok = true;
  {
    uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'P', 'W', 'R', 0xF7};   // the project: PWR
    engine_sysex(m, (int)sizeof m);
    loaded();
  }
  lfo::kit_changed(4), ins::kit_changed(4);   // kit 5: its own LFO and INSERT files

  // ---- 1. a clean save, a cut at every point, a full card
  for (const Writer& w : kWriters) test_cut(w);
  for (const Writer& w : kWriters) test_full_card(w);

  // ---- a rename that fails. write_atomic's last card changes are its renames, so a cut placed on them is exactly
  // that case: the file is the old one and nothing is left over. (Counted here so the case is named, not inferred.)
  {
    quiesce();
    const Writer& w = kWriters[0];
    std::string A = clean_save(w, 1), B = clean_save(w, 2);
    int renames = 0;
    for (int k = 0; k < 60; ++k) {
      put(w.path, A);
      w.reload();
      w.change(2);
      uint64_t c0 = host_changes();
      host_power_cut(k);
      flush();
      bool failed = rd(w.path) != B;
      uint64_t used = host_changes() - c0;
      host_power_cut(-1);
      w.reload();
      // the write and the read-back come first; a cut that got past them but not through is a failing rename
      if (failed && used >= 2) ++renames;
      CHECK(rd(w.path) == A || rd(w.path) == B);
    }
    CHECK(renames >= 1);
    printf("powercut: a rename that fails leaves the original (%d of 60 cuts landed past the .tmp)\n", renames);
  }

  // ---- 2. what a boot does with a .part / .tmp / .prev left on the card
  {
    quiesce();
    const char* P = "/.rytm/keys.txt";
    // a cut during the .tmp's write: a stray .tmp, the file whole. The .tmp is dropped, the file read.
    put(P, "mode=channel\nauto=5\nmac=1\n");
    put(std::string(P) + ".tmp", "mode=sel");   // half written
    keys::load();
    CHECK(!exists(std::string(P) + ".tmp") && rd(P) == "mode=channel\nauto=5\nmac=1\n");
    CHECK(keys::view().mode == keys::kChannel);
    // a cut between the two renames: the file is gone and the .prev holds the previous one. It comes back.
    rm(P);
    put(std::string(P) + ".prev", "mode=selected\nauto=7\nmac=0\n");
    keys::load();
    CHECK(rd(P) == "mode=selected\nauto=7\nmac=0\n" && !exists(std::string(P) + ".prev"));
    // a cut just before the .prev's removal: the new file is whole, the .prev is the stale one. The new one wins.
    put(P, "mode=channel\nauto=3\nmac=1\n");
    put(std::string(P) + ".prev", "mode=selected\nauto=7\nmac=0\n");
    keys::load();
    CHECK(rd(P) == "mode=channel\nauto=3\nmac=1\n" && !exists(std::string(P) + ".prev"));
    // nothing at all: defaults, and no save is owed (an interrupted save never stops the Sidecar starting)
    rm(P), rm(std::string(P) + ".prev"), rm(std::string(P) + ".tmp");
    keys::load();
    CHECK(!keys::save());
    printf("powercut: boot: a stray .tmp dropped, a .prev put back when the file is missing, a stale .prev removed\n");
  }

  // ---- /.rytm/project (which project the Sidecar opens): a cut must never leave half a name
  {
    const char* P = "/.rytm/project";
    put(std::string(P) + ".prev", "PWR");
    rm(P);
    project_load();
    CHECK(rd(P) == "PWR" && !exists(std::string(P) + ".prev"));
    CHECK(!strcmp(project, "PWR"));
    printf("powercut: /.rytm/project: the previous name back, never a half name to open\n");
  }

  // ---- the slot map, a pattern file, a CHOP slice map (the same write_atomic, their own places): old or new, never
  // missing. The slot map is the Pi's umwrite task (um_pi.cpp SdWriteWhole) and the Mac's um::write_whole; the
  // pattern is seq::write_pattern; the slice map is chop's cardq kAtomicReq.
  {
    for (const char* p : {"/.rytm/slots/PWR.txt", "/.rytm/slots/PWR.inf", "/.rytm/seq/PWR/p001.sqp",
                          "/CHOP/PWR/take.chop.sfz"}) {
      put(p, "old\n");
      int kept = 0, landed = 0;
      for (int k = 0; k < 20; ++k) {
        put(p, "old\n");
        rm(std::string(p) + ".tmp"), rm(std::string(p) + ".prev");
        host_power_cut(k);
        fmt::write_atomic(p, "new contents\n", 13);
        host_power_cut(-1);
        fmt::recover(p);
        std::string s = rd(p);
        CHECK(s == "old\n" || s == "new contents\n");
        CHECK(!exists(std::string(p) + ".tmp") && !exists(std::string(p) + ".prev"));
        if (s == "old\n") ++kept;
        else ++landed;
      }
      CHECK(kept >= 2 && landed >= 1);
      printf("powercut: %-28s %d kept, %d landed\n", p, kept, landed);
    }
    // 2026-10-26, the RAM store (phase 2): the sequencer no longer recovers a pattern when it loads one (no card call on
    // the control core) - a cut in write_atomic's rename window that left p001.sqp as .prev alone is put back by the boot
    // sweep, before the store's fill, so the fill holds the old pattern (never an absent one a save would replace)
    {
      const char* p = "/.rytm/seq/PWR/p001.sqp";
      put(std::string(p) + ".prev", "old\n");
      rm(p);
      fmt::sweep_boot();
      refill();
      const uint8_t* d;
      uint32_t n;
      CHECK(store::get(p, &d, &n) == store::kPresent && n == 4 && !memcmp(d, "old\n", 4));
      CHECK(!exists(std::string(p) + ".prev"));
      printf("powercut: %-28s a .prev alone: back by the boot sweep, held by the store's fill\n", p);
    }
    // the sampler's and auto-sampler's .sfz (samp::write_text): a cut leaves the previous one, never a truncated one
    // that would parse into a quietly wrong instrument. No reader calls fmt::recover for these: the rename window left
    // a .prev with no .sfz until the boot sweep (2026-10-26), which runs here as at a boot - so no orphan any more.
    {
      const char* p = "/Samples/PWR/SMP_001.sfz";
      int kept = 0, landed = 0, orphan = 0;
      for (int k = 0; k < 24; ++k) {
        put(p, "<region> sample=old.wav\n");
        rm(std::string(p) + ".tmp"), rm(std::string(p) + ".prev");
        host_power_cut(k);
        samp::write_text(p, "<region> sample=new.wav\n", 24);
        host_power_cut(-1);
        fmt::sweep_boot();   // the next boot
        std::string s = rd(p);
        CHECK(s == "<region> sample=old.wav\n" || s == "<region> sample=new.wav\n" || s == "<none>");
        if (s == "<none>") ++orphan;   // the rename window: the .prev is beside it
        else if (s == "<region> sample=old.wav\n") ++kept;
        else ++landed;
        if (s == "<none>") CHECK(rd(std::string(p) + ".prev") == "<region> sample=old.wav\n");
      }
      CHECK(orphan == 0 && kept >= 2 && landed >= 1);
      printf("powercut: %-28s %d kept, %d landed, 0 left as .prev (the boot sweep)\n", p, kept, landed);
    }
  }

  // ---- 3. the lossy reparse: key=value settings keep what they do not read, across a save
  {
    quiesce();
    put("/.rytm/keys.txt", "mode=channel\nauto=5\nmac=1\nfuture_keys=on\nlater=a=b\n");
    keys::load();
    keys_change(2);
    flush();
    std::string s = rd("/.rytm/keys.txt");
    CHECK(s.find("future_keys=on\n") != std::string::npos && s.find("later=a=b\n") != std::string::npos);
    CHECK(s.find("auto=9\n") != std::string::npos);
    put("/.rytm/fxin.txt", "src=fx\nfuture_in=2\n");
    fxin::load();
    fxin::set(false);
    flush();
    CHECK(rd("/.rytm/fxin.txt") == "src=in\nfuture_in=2\n");
    put("/.rytm/sendfx.txt", "out=fx\nfuture_route=x\n");
    fx::card_load();
    sendfx_change(1);
    flush();
    CHECK(rd("/.rytm/sendfx.txt") == "out=main\nfuture_route=x\n");
    printf("powercut: class S: a newer Sidecar's keys survive this build's load + save (keys, fxin, sendfx)\n");
  }

  // ---- the positional per-kit files cannot merge an unknown line into their fixed slots: they lock instead, and a
  // save never writes over them (so a tester going 0.4 -> 0.3 -> 0.4 keeps 0.4's LFOs and chains)
  {
    quiesce();
    const char* L = "/.rytm/lfo/PWR/k007.txt";
    const std::string bad = "# Sidecar LFOs\n1 V1 1 0 60 0 0 20 64 \n2 FUTURETGT 1 0 60 0 0 20 64 \n";
    put(L, bad);
    refill();
    lfo::kit_changed(6);   // kit 7: that file
    lfo_change(2);
    flush();
    CHECK(rd(L) == bad);   // untouched
    const char* I = "/.rytm/ins/PWR/k007.txt";
    const std::string badi = "V1 A EQ 64 64 100\nV2 B FUTUREFX 1 2 3\n";
    put(I, badi);
    refill();
    ins::kit_changed(6);
    ins_change(2);
    flush();
    CHECK(rd(I) == badi);
    printf("powercut: positional per-kit files with a line this build does not read are locked, not merged\n");
  }

  // ---- a hostile or truncated per-kit file is refused whole and kept as it is: a line this build cannot read locks
  // the file, and a save never writes over a locked one. A line whose first byte is a NUL counts as unreadable, not as
  // a blank line (it used to be skipped, which left the file unlocked and the next save wrote over whatever was in it).
  {
    quiesce();
    const char* L = "/.rytm/lfo/PWR/k009.txt";
    const char* I = "/.rytm/ins/PWR/k009.txt";
    const std::string hostile[] = {
        std::string("\x00\x01\x02\xff\xfe", 5),                              // binary junk, NUL first
        std::string(600, 'A') + "\n",                                        // longer than the reader's buffer
        "1 V1 1 0 60 0 0 20 64 \n" + std::string("\x00\x00g\n", 4),          // a NUL-led line after a good one
        "../../etc/passwd\n1 V1 1 0 60 0 0 20 64 \n",                        // a path where a setting belongs
        "\xef\xbb\xbf" "1 V1 1 0 60 0 0 20 64 \n",                           // a BOM a Mac editor added
    };
    int locked = 0;
    for (const std::string& h : hostile)
      for (const char* p : {L, I}) {
        put(p, h);
        refill();
        p == L ? lfo::kit_changed(8) : ins::kit_changed(8);
        p == L ? lfo_change(2) : ins_change(2);
        flush();
        CHECK(rd(p) == h);   // kept byte for byte: never written over, never half a file
        ++locked;
        p == L ? lfo::kit_changed(4) : ins::kit_changed(4);
      }
    printf("powercut: %d hostile / truncated per-kit files refused whole, each kept byte for byte\n", locked);
    // A file whose every line this build does read is not hostile: it loads and the next save rewrites it (the same
    // settings, written out whole). Blank lines, trailing newlines and a run of bare CRs are whitespace, not content.
    // The LFO reader builds a whole Settings and assigns it only when the line parses, so a refused line applies
    // nothing - the file holding one good line leaves every other LFO at its default, never a mix.
    for (const std::string& ok : {std::string("1 V1 1 0 60 0 0 20 64 \n"),
                                  std::string("1 V1 1 0 60 0 0 20 64 \n\n\n") + std::string(40, '\r'),
                                  std::string("# a note\n1 V1 1 0 60 0 0 20 64 \n")}) {
      put(L, ok);
      refill();
      lfo::kit_changed(8);
      CHECK(lfo::get(0).target == lfo::kTVoice + 0);
      lfo::Settings def;
      lfo::defaults(&def);
      CHECK(lfo::get(1).target == 0 && lfo::get(1).depth == def.depth);   // the rest are the defaults, not a mix
      lfo_change(2);
      flush();
      CHECK(rd(L) != ok && rd(L).find("-22") != std::string::npos);   // read, so this build may write it
      lfo::kit_changed(4);
    }
    printf("powercut: a per-kit file this build does read is loaded whole and may be written; a refused line applies "
           "nothing\n");
  }

  // ---- 4. where the I/O lands. idle_save() runs on the Pi's control core (core 3), in the same loop that renders
  // audio blocks, and that core's OUT FIFO holds 2 ms. A posted save must charge it no card time at all: the card
  // core (core 0) does the whole atomic write.
  {
    quiesce();
    lfo::kit_changed(4);
    lfo_change(1);
    flush();   // (a clean file to write over)
    cardq::reset();
    cardq::set_worker(true);   // as the Pi: core 0 serves the queue
    host_card_model(true);
    host_card_core(3);
    lfo_change(2);
    at(g_us + 1100000);
    int posted = 0;
    for (int i = 0; i < 8 && !posted; ++i)
      if (idle_save() && fmt::post_busy()) posted = 1;
    CHECK(posted && fmt::post_busy());
    uint64_t c3 = host_card_us(3);
    CHECK(c3 == 0);   // the control core touched the card not at all
    host_card_core(0);
    while (cardq::service()) {}
    uint64_t c0 = host_card_us(0);
    CHECK(c0 > 0);
    while (fmt::post_step()) {}
    CHECK(fmt::post_ok() && host_card_us(3) == 0);
    CHECK(rd("/.rytm/lfo/PWR/k005.txt").find("-22") != std::string::npos);
    printf("powercut: a posted LFO save: control core 0 us of card time, card core %llu us (the whole atomic write)\n",
           (unsigned long long)c0);
    // What the control core used to pay, and would pay if the atomic write ran there: the comparison the scheduling
    // rests on. The FIFO on that core holds 2000 us, so both are dropouts and only posting them away fixes it.
    std::string t = rd("/.rytm/lfo/PWR/k005.txt");
    const char* P = "/.rytm/lfo/PWR/io.txt";
    host_card_model(true);
    host_card_core(3);
    {   // as the writer did before: one open (truncating), one write, one close
      File f;
      CHECK(f.open(P, kWrite) && f.write(t.data(), t.size()) == t.size());
      f.close();
    }
    uint64_t plain = host_card_us(3);
    host_card_model(true);
    host_card_core(3);
    CHECK(fmt::write_atomic(P, t.data(), (uint32_t)t.size()));
    uint64_t atomic = host_card_us(3);
    CHECK(atomic > plain);
    printf("powercut: on the control core the old plain write was %llu us and the atomic one would be %llu us "
           "(x%llu.%llu); the OUT FIFO there holds 2000 us, so the save is posted away instead\n",
           (unsigned long long)plain, (unsigned long long)atomic, (unsigned long long)(atomic / plain),
           (unsigned long long)(atomic * 10 / plain % 10));
    rm(P), rm(std::string(P) + ".prev"), rm(std::string(P) + ".tmp");
    host_card_model(false);
    // a card core held up past cardq's kTakeBackUs (phase 3, 2026-10-26): the write stays posted - the control core
    // never runs it itself (that was a card write on core 3, now possibly a pattern-sized one: a dropout). It is
    // counted and logged once, lands once the card core runs again, and the writes queued behind it meanwhile all land
    // too, in their order, the newest text of each path
    rm("/.rytm/lfo/PWR/k005.txt");
    lfo_change(1);
    at(g_us + 1100000);
    for (int i = 0; i < 8 && !fmt::post_busy(); ++i) idle_save();
    CHECK(fmt::post_busy() && cardq::pending() == 1);
    uint32_t took0 = cardq::stats().taken_back, held0 = fmt::post_held();
    host_card_model(true);
    host_card_core(3);   // the control core's passes from here on: charged nothing
    at(g_us + 2 * cardq::kTakeBackUs);
    for (int i = 0; i < 50; ++i) CHECK(fmt::post_step() && fmt::post_busy());   // still posted, pass after pass
    CHECK(cardq::stats().taken_back == took0 && cardq::pending() == 1);
    CHECK(fmt::post_held() == held0 + 1);   // counted once, not per pass
    CHECK(rd("/.rytm/lfo/PWR/k005.txt") == "<none>");   // not written by the control core
    // (fix round 1) the idle pass goes on during the hold: an LFO edit's idle save reaches the store and the queue
    // (behind the held write of the same file), and a pattern change without a switch installs (seq::idle's load_due)
    CHECK(fmt::post_stalled());
    lfo_change(2);
    at(g_us + 1100000);
    for (int i = 0; i < 8 && lfo::dirty(); ++i) idle_save();
    CHECK(!lfo::dirty() && fmt::pending("/.rytm/lfo/PWR/k005.txt"));
    {
      const uint8_t* d = nullptr;
      uint32_t n = 0;
      CHECK(store::get("/.rytm/lfo/PWR/k005.txt", &d, &n) == store::kPresent &&
            std::string((const char*)d, n).find("-22") != std::string::npos);   // the edit's text, in RAM
    }
    int pat0 = seq::pattern_now(), pat1 = pat0 == 9 ? 10 : 9;
    {
      const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, seq::kStateCmd, 1, seq::kPattern, (uint8_t)pat1, seq::kNone, 0xF7};
      CHECK(seq::sysex(m, (int)sizeof m));
    }
    for (int i = 0; i < 8 && seq::pattern_now() != pat1; ++i) idle_save();
    CHECK(seq::pattern_now() == pat1);   // installed while the card core is held
    CHECK(fmt::post_busy() && fmt::post_stalled() && host_card_us(3) == 0);   // all of it without the card
    // more saves behind it while the card core is still held (a kit change's, a pattern's): queued, never a wait
    const char* Q1 = "/.rytm/lfo/PWR/held-a.txt";
    const char* Q2 = "/.rytm/lfo/PWR/held-b.txt";
    const char* Q3 = "/.rytm/lfo/PWR/held-big.txt";
    std::string big(fmt::kPostMax * 3 + 17, 'p');   // over kPostMax: a pattern-sized write, its own copy
    CHECK(fmt::post_queue(Q1, "a1\n", 3) && fmt::post_queue(Q2, "b1\n", 3));
    CHECK(fmt::post_queue(Q3, big.data(), (uint32_t)big.size()));
    CHECK(fmt::post_queue(Q1, "a2\n", 3));   // a newer text of a queued path: it replaces the older
    at(g_us + 10 * cardq::kTakeBackUs);
    for (int i = 0; i < 50; ++i) CHECK(fmt::post_step() && fmt::post_busy());
    CHECK(fmt::pending(Q1) && fmt::pending(Q2) && fmt::pending(Q3));
    CHECK(fmt::post_held() == held0 + 1 && cardq::stats().taken_back == took0);
    CHECK(host_card_us(3) == 0);   // all of it: the control core touched the card not at all
    CHECK(rd(Q1) == "<none>" && rd(Q2) == "<none>" && rd(Q3) == "<none>");
    // the card core runs again: the held write lands, then each queued one, all on the card core
    host_card_core(0);
    int passes = 0;
    while (fmt::post_busy() && passes < 100) {
      host_card_core(0);
      while (cardq::service()) {}
      host_card_core(3);
      fmt::post_step();
      ++passes;
    }
    CHECK(!fmt::post_busy() && fmt::post_ok() && cardq::stats().taken_back == took0);
    CHECK(host_card_us(3) == 0 && host_card_us(0) > 0);
    CHECK(rd("/.rytm/lfo/PWR/k005.txt").find("-22") != std::string::npos);   // the newer edit, after the held one
    CHECK(rd(Q1) == "a2\n" && rd(Q2) == "b1\n" && rd(Q3) == big);   // nothing lost, the newest of each
    printf("powercut: a card core held up past %u us: the posted save stays posted (counted once), the control core "
           "0 us, the idle pass goes on (an LFO save queued, a pattern installed); it and the writes queued behind it land once the card core runs (%d passes)\n",
           (unsigned)cardq::kTakeBackUs, passes);
    host_card_model(false);
    for (const char* p : {Q1, Q2, Q3}) rm(p);
    cardq::set_worker(false);
    cardq::reset();
  }

  CHECK(system(("rm -rf '" + g_card + "'").c_str()) == 0);
  // ---- the boot sweep (2026-10-26): files written atomically whose readers never call recover() - a CHOP slice map
  // (found by a scan for ".chop.sfz", which never sees a .prev), the sampler's .sfz, a pool file. A cut inside the
  // rename window leaves only <path>.prev: the map or instrument looked lost although every byte was on the card. One
  // sweep at boot puts every such .prev back and clears stray .tmp / stale .prev, so no reader has to remember.
  {
    struct Case { const char* path; const char* old; };
    const Case orphans[] = {{"/CHOP/PWR/take-1a2b3c4d.chop.sfz", "<control> slices=8\n"},
                            {"/Samples/PWR/SMP_002.sfz", "<region> sample=old.wav\n"},
                            {"/.rytm/projects/PWR/samples.pool.txt", "# pool\nkick.wav\n"}};
    for (const Case& c : orphans) rm(c.path), rm(std::string(c.path) + ".tmp"), put(std::string(c.path) + ".prev", c.old);
    put("/.rytm/lfo/PWR/k004.txt.tmp", "half a wri");           // a stray .tmp: its file was never there
    put("/.rytm/ins/PWR/k005.txt", "1 A FILTER 1 2 3 4\n");     // whole, with a stale .prev beside it
    put("/.rytm/ins/PWR/k005.txt.prev", "older\n");
    int n = fmt::sweep_boot();
    for (const Case& c : orphans) {
      CHECK(rd(c.path) == c.old);
      CHECK(!exists(std::string(c.path) + ".prev") && !exists(std::string(c.path) + ".tmp"));
    }
    CHECK(!exists("/.rytm/lfo/PWR/k004.txt.tmp") && !exists("/.rytm/lfo/PWR/k004.txt"));
    CHECK(rd("/.rytm/ins/PWR/k005.txt") == "1 A FILTER 1 2 3 4\n" && !exists("/.rytm/ins/PWR/k005.txt.prev"));
    CHECK(n == 5);   // three put back, one stray .tmp dropped, one stale .prev dropped
    CHECK(fmt::sweep_boot() == 0);   // nothing left to do: a second boot touches nothing
    printf("powercut: boot sweep: a CHOP map, a sampler .sfz and a pool file put back from .prev; %d fixed in all\n", n);
  }
  printf("test_powercut: ok\n");
  return 0;
}
