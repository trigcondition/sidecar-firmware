// core/tests/test_polypat.cpp - poly tracks per pattern (docs/superpowers/specs/2026-10-26-per-pattern-poly-design.md).
//
// The user's report (2026-10-26): a new pattern with a cleared kit "sticks" - every track reporting a Rytm machine,
// yet mask 0x737 (eight tracks captured, pinned, silenced), the trigs blue and no regular trig addable; and the SEQ
// page's TURN OFF + YES came back on at the next step recording. The model: which tracks are poly is the PATTERN's
// (each slot's own on flag, v4 block kBlkOn), and seq's mask is derived from it - the current pattern's slots that are
// on, whose track plays a Sidecar machine (or has not been reported yet). One case per spec test (1..8 below).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "core_port.h"
#include "engine.h"
#include "hooks.h"
#include "fmt/fmt.h"
#include "machines.h"
#include "pool/pool.h"
#include "project_key.h"
#include "seq/seq.h"
#include "ui/extra.h"
#include "ui/remote.h"

using namespace core;
using namespace core::ui;

static uint64_t g_us = 1000000;
static uint32_t g_ms = 10000;
struct Out {
  int64_t frame;   // seq::frames() when it went
  std::vector<uint8_t> m;
};
static std::vector<Out> g_out;
static void out(const uint8_t* m, int n) { g_out.push_back({(int64_t)seq::frames(), std::vector<uint8_t>(m, m + n)}); }

static std::vector<uint8_t> msg(uint8_t cmd, std::vector<uint8_t> body) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, cmd, 1};
  m.insert(m.end(), body.begin(), body.end());
  m.push_back(0xF7);
  return m;
}
static void from_rytm(std::vector<uint8_t> body, uint8_t cmd) {
  auto m = msg(cmd, body);
  engine_sysex(m.data(), (int)m.size());
}
static void report(int vb) {   // the voice's 03 (its machine), P1..P16 at 64
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x03, (uint8_t)vb};
  for (int k = 0; k < 16; ++k) m.push_back(64), m.push_back(0);
  m.push_back(0xF7);
  engine_sysex(m.data(), (int)m.size());
}
static void machine(int t, int m) {   // 33: track t's voice byte (m 0x7F: a Rytm machine)
  if (m == 0x7F) return from_rytm({(uint8_t)t, 1, 0, 0x7F}, 0x33);
  from_rytm({(uint8_t)t, 1, 44, (uint8_t)(m | remote_track_voice(t))}, 0x33);
  report(m | remote_track_voice(t));
}
static void all_rytm() {   // 33 for every track: the cleared kit (each track a Rytm machine)
  std::vector<uint8_t> b = {0, 12};
  for (int t = 0; t < 12; ++t) b.push_back(0), b.push_back(0x7F);
  from_rytm(b, 0x33);
}
static void key(int code) { from_rytm({0, (uint8_t)code, 1}, 0x31), from_rytm({0, (uint8_t)code, 0}, 0x31); }
// updown1029: FUNC + DOWN, the poly track's menu (DOWN alone is the Rytm's: CHRO's octave)
static void menu() { from_rytm({1, 0, 1}, 0x31), key(0x43), from_rytm({1, 0, 0}, 0x31); }
static void tick() {
  g_ms += 60;
  g_us += 60000;
  host_fake_time(true, g_us);
  remote_tick(g_ms);
  seq::tick(now_ms());
}
// the last catch mask (37) sent names key k (-1: no mask sent since g_out was cleared)
static int caught(int k) {
  for (size_t i = g_out.size(); i-- > 0;)
    if (g_out[i].m.size() > 6 && g_out[i].m[4] == 0x37) {
      size_t at = 6 + 3 + (size_t)k / 7;
      return at + 1 < g_out[i].m.size() && (g_out[i].m[at] >> (k % 7) & 1);
    }
  return -1;
}
// ux1027 review 8: a question on the SEQ page catches YES (it answers it), DOWN leaves the question as it is; PLAY and
// SCNE never caught (YES + PLAY / SCNE: the Rytm's quick saves). updown1029: UP / DOWN caught under it too
static void question_keys(int prompt) {
  tick();
  CHECK(caught(0x44) == 1 && caught(0x51) == 0 && caught(0x13) == 0 && caught(0x42) == 1 && caught(0x43) == 1);
  key(0x43), tick();
  CHECK(seq::view().prompt == prompt);
}
static void loaded() {
  for (int i = 0; i < 200000 && project_loading(); ++i) idle_save();
  CHECK(!project_loading());
}
static std::string cmd(const char* c) {
  char b[900];
  CHECK(seq::command(c, b, sizeof b));
  return b;
}
static uint16_t mask() {
  uint16_t m = 0;
  for (int r = 0; r < 12; ++r) m |= (uint16_t)(seq::owns_track(r) ? 1u << r : 0);
  return m;
}
static int last_2e() {   // the last 2E's mask, -1 none since g_out was cleared
  int m = -1;
  for (auto& o : g_out)
    if (o.m.size() >= 10 && o.m[4] == 0x2E) m = o.m[6] | o.m[7] << 7;
  return m;
}
static void go(int p) {   // the Rytm on pattern p (stopped: a change without a switch), loaded
  from_rytm({seq::kPattern, (uint8_t)p, 0x7F}, 0x2D);
  while (seq::idle()) {}
  tick();
}
static void settle() {   // the debounced saves written (into the store and the card queue)
  for (int i = 0; i < 30; ++i) tick(), seq::idle();
  for (int i = 0; i < 1000 && fmt::post_busy(); ++i) fmt::post_step();
}
static void page(int code) {   // 30: the Rytm asks for page code (0: closed)
  from_rytm({(uint8_t)code, (uint8_t)(code ? 1 : 0), (uint8_t)(code ? 0 : 1)}, 0x30);
  tick();
}
static void step(int r, int s, int pitch) {
  char c[48];
  snprintf(c, sizeof c, "seq=step:%d:%d:%d", r + 1, s + 1, pitch);
  cmd(c);
}
static int steps(int r) {
  const seq::Track* t = seq::edit_track(r, false);
  return t ? seq::used_steps(*t) : -1;
}
static void dump(const char* what) {
  printf("  %s: mask 0x%03x, slots", what, mask());
  for (const seq::Track& t : seq::edit_pattern().t)
    if (t.rytm >= 0) printf(" %d%s(%d)", t.rytm + 1, t.on ? "on" : "off", seq::used_steps(t));
  printf("\n");
}
constexpr int CH = 8, OH = 9, BD = 0;

// ---- 7: the switch's timing (the Rytm side simulated: its ticks stamped with their block, arriving `delay` later)
struct Msg {
  int64_t at;
  std::vector<uint8_t> m;
};
static std::vector<Msg> g_wire;
static std::vector<uint8_t> blk4(int64_t frame) {
  uint32_t b = (uint32_t)(frame / 32) & ((1u << 28) - 1);
  return {(uint8_t)(b >> 21 & 0x7F), (uint8_t)(b >> 14 & 0x7F), (uint8_t)(b >> 7 & 0x7F), (uint8_t)(b & 0x7F)};
}
static void wire(int64_t at, const std::vector<uint8_t>& m) { g_wire.push_back({at, m}); }
static void deliver(int64_t h) {
  for (size_t i = 0; i < g_wire.size();) {
    if (g_wire[i].at <= h) {
      engine_sysex(g_wire[i].m.data(), (int)g_wire[i].m.size());
      g_wire.erase(g_wire.begin() + (long)i);
    } else {
      ++i;
    }
  }
}
// the Rytm ticks every track the last 2E names (seq tracks and clock tracks) once that 2E has reached it
static int g_rytm_mask = 0;
static int64_t g_t0 = 0, g_next = 0;
static const double kPer = 6000;   // 120 BPM
static const int kDelay = 700;
struct Fired {
  int64_t h;
  int voice, note;
};
static std::vector<Fired> g_fired;
static void run_until(int64_t until) {
  while ((int64_t)seq::frames() < until) {
    int64_t h = (int64_t)seq::frames();
    for (auto& o : g_out)   // the 2Es sent: the Rytm's capture (and clock) from their arrival on (USB: 1 ms)
      if (o.m.size() >= 10 && o.m[4] == 0x2E && o.frame + 48 <= h && o.frame + 48 > h - kBlock) {
        int m = o.m[6] | o.m[7] << 7;
        if (o.m.size() >= 12) m |= o.m[9] | o.m[10] << 7;   // its clock tracks
        g_rytm_mask = m;
      }
    for (;;) {
      int64_t tf = g_t0 + (int64_t)llround((double)g_next * kPer);
      if (tf > h + 2) break;
      std::vector<uint8_t> b = blk4(tf);
      std::vector<uint8_t> tr;
      for (int r = 0; r < 12; ++r)
        if (g_rytm_mask >> r & 1) tr.insert(tr.end(), {(uint8_t)r, (uint8_t)(g_next % 16), 16, 2});
      if (!tr.empty()) {
        b.push_back((uint8_t)(tr.size() / 4));
        b.insert(b.end(), tr.begin(), tr.end());
        wire(tf + kDelay, msg(0x2C, b));
      }
      ++g_next;
    }
    deliver(h);
    int64_t f[512], f2[512];
    uint8_t on[512], vo[512], no[512], on2[512], vo2[512], no2[512];
    int n = seq::pending(f, on, vo, no, 512);
    seq::pre_block();
    int n2 = seq::pending(f2, on2, vo2, no2, 512);
    for (int i = 0; i < n; ++i) {   // the ons that went
      if (!on[i]) continue;
      bool still = false;
      for (int j = 0; j < n2 && !still; ++j) still = on2[j] && f2[j] == f[i] && no2[j] == no[i];
      if (!still) g_fired.push_back({h, vo[i], no[i]});
    }
    g_us += 250;
    host_fake_time(true, g_us);
    seq::tick(now_ms());
  }
}

int main() {
  char tmpl[] = "/tmp/core_polypat_XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  hooks.to_rytm = out;
  {   // the user's project: tracks.txt still says mask=0x737 (BD SD RS BT LT CH OH CY), as written by fix1051b's build
    char pk[64];
    project_key("PPAT", pk, sizeof pk);
    std::string dir = card + "/.rytm/seq/" + pk;
    CHECK(system(("mkdir -p '" + dir + "'").c_str()) == 0);
    FILE* f = fopen((dir + "/tracks.txt").c_str(), "w");
    CHECK(f);
    fputs("mask=0x737\n", f);
    fclose(f);
  }
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'P', 'P', 'A', 'T', 0xF7};
  engine_sysex(proj, sizeof proj);
  loaded();
  remote_reset();

  // ---- 1. a new pattern with a cleared kit: every track a Rytm machine -> nothing captured, a 2E with mask 0 sent
  {
    all_rytm();
    g_out.clear();
    go(32);   // C01, never used
    tick();
    dump("1. C01, a cleared kit");
    printf("  1. 2E sent: mask 0x%03x\n", last_2e() < 0 ? 0xFFF : last_2e());
    CHECK(mask() == 0);
    CHECK(last_2e() == 0);
    CHECK(!seq::any_tracks());
    // the Rytm's step keys are its own (seq mode is not entered for a track that is not a seq track; a key with the
    // SEQ page closed is never taken)
    CHECK(!seq::ui_key(3, true, false) && !seq::ui_key(3, false, false));
  }

  // every track a Sidecar machine from here (CH WAVETABLE, the rest PLAITS)
  for (int t = 0; t < 12; ++t) machine(t, t == CH ? kWaveMachine : kPlaits);
  tick();

  // ---- 2. pattern A (A01) with CH poly, pattern B (A02) empty: A -> B -> A gives {CH} -> {} -> {CH}
  {
    go(0);
    dump("2. A01 before");
    CHECK(mask() == 0);   // (a pattern nobody made a poly track in: plain, whatever tracks.txt's mask= says)
    cmd("seq=track:9:on");
    step(CH, 0, 60);
    tick();
    CHECK(mask() == 1u << CH);
    settle();
    go(1);
    dump("2. A02");
    CHECK(mask() == 0);
    CHECK(seq::edit_track(CH, false) == nullptr);
    go(0);
    dump("2. A01 again");
    CHECK(mask() == 1u << CH && steps(CH) == 1);
  }

  // ---- 3. TURN OFF CH in A (the SEQ page's track menu, YES): off at once; leave and re-enter step recording: still
  // off; B and C untouched; YES again: on, CH's steps back
  {
    go(2);   // C = A03: OH poly there
    cmd("seq=track:10:on");
    step(OH, 4, 64);
    settle();
    go(0);
    CHECK(mask() == 1u << CH);
    page(0x10 + CH);   // TRK + TRIG: CH's SEQ page
    menu();            // FUNC + DOWN: the track menu (updown1029; DOWN ux1027; YES before)
    key(0x43);         // DOWN: TURN OFF
    key(0x44);         // its YES: asked
    key(0x44);         // YES: off
    tick();
    dump("3. A01 after TURN OFF");
    CHECK(mask() == 0);
    CHECK(steps(CH) == 1);   // its steps kept
    page(0);
    from_rytm({seq::kSeqMode, 1, CH}, 0x2D);   // step recording on CH, off, on again
    tick();
    from_rytm({seq::kSeqMode, 0, CH}, 0x2D);
    tick();
    from_rytm({seq::kSeqMode, 1, CH}, 0x2D);
    tick();
    from_rytm({seq::kSeqMode, 0, CH}, 0x2D);
    tick();
    page(0);
    dump("3. A01 after step recording again");
    CHECK(mask() == 0);
    settle();
    go(1);
    CHECK(mask() == 0);
    go(2);
    dump("3. A03 (C)");
    CHECK(mask() == 1u << OH && steps(OH) == 1);
    go(0);   // back: still off (the flag came back from the file)
    dump("3. A01 reloaded");
    CHECK(mask() == 0 && steps(CH) == 1);
    page(0x10 + CH);   // the SEQ page of a track that is no poly track: the make prompt; YES
    key(0x44);
    tick();
    page(0);
    dump("3. A01 after YES again");
    CHECK(mask() == 1u << CH && steps(CH) == 1);
  }

  // ---- 4. CH poly in A, the kit gives CH a Rytm machine: released (its steps kept); a Sidecar machine again: captured
  {
    machine(CH, 0x7F);
    tick();
    dump("4. CH plays a Rytm machine");
    CHECK(mask() == 0);
    CHECK(steps(CH) == 1 && seq::edit_track(CH, false)->on);
    machine(CH, kWaveMachine);
    tick();
    dump("4. CH plays WAVETABLE again");
    CHECK(mask() == 1u << CH && steps(CH) == 1);
  }

  // ---- 5. an old file (no kBlkOn): a slot with steps is on, an empty slot off
  {
    seq::Pattern p;
    seq::clear(p, 5);
    seq::Track* a = seq::track_of(p, BD, true);
    seq::Track* b = seq::track_of(p, CH, true);
    seq::add_note(a->step[0], {60, 100, 24, 0, 0});
    a->on = 1, b->on = 0;   // as the rule reads them: no block written, the file as an older Sidecar wrote it
    static uint8_t buf[200000];
    int n = seq::pack(p, buf, sizeof buf);
    CHECK(n > 0 && buf[4] < 4);   // v2: no blocks at all
    seq::Pattern q;
    CHECK(seq::unpack(q, 5, buf, n));
    CHECK(seq::track_of(q, BD)->on == 1 && seq::track_of(q, CH)->on == 0);
    // the cases the rule cannot say: an empty slot on, a slot with steps off -> the block (and they come back)
    a->on = 0, b->on = 1;
    n = seq::pack(p, buf, sizeof buf);
    CHECK(n > 0 && buf[4] == 4);
    CHECK(seq::unpack(q, 5, buf, n));
    CHECK(seq::track_of(q, BD)->on == 0 && seq::track_of(q, CH)->on == 1);
    printf("  5. an old file: BD (1 step) on, CH (empty) off; with the block: the flags as written\n");
  }

  // ---- 6. four slots holding steps: a fifth YES refused with the reason; an off empty slot is taken
  {
    go(6);   // A07
    for (int r : {0, 1, 2, 3}) {
      char c[32];
      snprintf(c, sizeof c, "seq=track:%d:on", r + 1);
      cmd(c);
      step(r, 0, 60);
    }
    dump("6. A07 four poly tracks with steps");
    CHECK(mask() == 0xF);
    std::string o = cmd("seq=track:5:on");
    dump("6. a fifth");
    CHECK(!seq::owns_track(4));
    // one of them turned off and its steps cleared: an empty slot that is off, taken by the next YES
    cmd("seq=track:4:off");
    cmd("seq=clear:4");
    CHECK(seq::edit_track(3, false) && !seq::edit_track(3, false)->on);
    cmd("seq=track:5:on");
    dump("6. BT takes CP's empty, off slot");
    CHECK(seq::owns_track(4) && seq::edit_track(3, false) == nullptr);
    // an empty slot that is ON is never taken: BT's (just made, empty) stays when SD's steps go and a sixth asks
    cmd("seq=track:6:on");
    dump("6. a sixth with BT on and empty");
    CHECK(!seq::owns_track(5) && seq::owns_track(4));
  }

  // ---- 8. a Pi reboot (no machine report yet): the pattern's on slots stay captured
  {
    settle();
    remote_reset();   // (the link's state as after a boot: no 33 yet)
    CHECK(remote_track_machine(CH) < 0);
    go(0);
    tick();
    dump("8. A01, no machine reported");
    CHECK(mask() == 1u << CH);
    // review round 1: one track's machine changed in the SRC list (a single-track 33, BD alone) - the tracks it does
    // not name are still unreported, not Rytm machines: CH stays captured
    machine(BD, kPlaits);
    tick();
    dump("8. a 33 for BD alone");
    CHECK(remote_track_machine(CH) < 0);
    CHECK(mask() == 1u << CH);
    for (int t = 0; t < 12; ++t) machine(t, t == CH ? kWaveMachine : kPlaits);
    tick();
  }

  // ---- 9 (review round 1, the controller's ruling): the Rytm's stock pattern CLEAR empties the pattern, its poly
  // tracks with it ("a new / empty pattern has no poly tracks": no sticky poly on a cleared pattern); its UNDO brings
  // them back
  {
    go(0);
    CHECK(mask() == 1u << CH && steps(CH) == 1);
    from_rytm({seq::kEdit, seq::kPClear, 0, 0x7F, 1, 0, 0, 0, 0}, 0x2D);   // CLEAR PATTERN (stock, src 0)
    tick();
    dump("9. A01 after the Rytm's CLEAR PATTERN");
    CHECK(mask() == 0 && steps(CH) == -1);
    from_rytm({seq::kEdit, seq::kPUndo, 0, 0x7F, 1, 0, 1, 0, 0}, 0x2D);   // its UNDO (arg 1: the clear's)
    tick();
    dump("9. A01 after UNDO");
    CHECK(mask() == 1u << CH && steps(CH) == 1);
  }

  // ---- 10 (review round 1): the Pi's touch editor edits a poly track's slot only - an off slot (TURN OFF) or a
  // released one (a Rytm machine) is not there for it (its edits would be saved and never played)
  {
    go(0);
    CHECK(mask() == 1u << CH && seq::edit_seq_track(CH, false) && seq::edit_seq_track(CH, true));
    cmd("seq=track:9:off");
    CHECK(seq::edit_track(CH, false) && !seq::edit_seq_track(CH, false) && !seq::edit_seq_track(CH, true));
    cmd("seq=track:9:on");
    machine(CH, 0x7F);
    tick();
    CHECK(!seq::edit_seq_track(CH, true));
    machine(CH, kWaveMachine);
    tick();
    CHECK(seq::edit_seq_track(CH, false) == seq::edit_track(CH, false));
    CHECK(!seq::edit_seq_track(OH, true) && !seq::edit_track(OH, false));   // (not poly here: none made either)
    printf("  10. the touch editor: CH off / released -> not editable, on -> its slot\n");
  }

  // ---- 11 (fix1028, hardware report): TURN OFF in step recording - "the poly steps are still displayed on the rytm
  // display and poly mode display is still visible; after NO and leaving step recording it shows the correct display".
  // Seq mode on CH opens the SEQ page; YES, DOWN, YES, YES turns CH off. At once (before any NO or GRID REC off): no poly
  // page left (the page asked closed), no poly trig LED. Then the Rytm's own answer to the 2E (rui.c sq_tick: seq mode
  // ends with its track out of the mask, 2D 08 0 7F) changes nothing more. YES again is unchanged: the steps come back.
  {
    go(0);
    CHECK(mask() == 1u << CH && steps(CH) == 1);
    page(0);
    tick();
    g_out.clear();
    from_rytm({seq::kSeqMode, 1, CH}, 0x2D);   // GRID REC on CH: seq mode, the SEQ page asked open
    tick();
    from_rytm({0, 1, 0}, 0x30);   // the Rytm opens the page
    for (int i = 0; i < 4; ++i) tick();
    CHECK(remote_page_title() && !strcmp(remote_page_title(), "SEQ") && seq::seqmode_on() == CH);
    auto lit = [] {   // the trig LEDs the host sent last (34): any not off
      for (size_t i = g_out.size(); i-- > 0;)
        if (g_out[i].m.size() > 6 && g_out[i].m[4] == 0x34) {
          if (g_out[i].m[6] == 0) return false;
          for (size_t k = 7; k + 1 < g_out[i].m.size(); ++k)
            if (g_out[i].m[k]) return true;
          return false;
        }
      return false;
    };
    auto asked_closed = [] {   // the last open request (3A) the host sent: close
      for (size_t i = g_out.size(); i-- > 0;)
        if (g_out[i].m.size() > 7 && g_out[i].m[4] == 0x3A) return g_out[i].m[7] == 0;
      return false;
    };
    CHECK(lit());   // CH's step 1 lit (ours)
    menu(), tick();      // FUNC + DOWN: the track menu (updown1029)
    key(0x43), tick();   // DOWN: TURN OFF
    key(0x44), tick();   // YES: asked
    key(0x44), tick();   // YES: off
    tick();
    printf("  11. after TURN OFF in seq mode: mask 0x%03x seqmode %d page %s asked closed %d leds lit %d\n", mask(),
           seq::seqmode_on(), remote_page_title() ? remote_page_title() : "-", asked_closed(), lit());
    CHECK(mask() == 0 && steps(CH) == 1);
    CHECK(seq::seqmode_on() < 0 && seq::view().held < 0);   // seq mode is over for the host at once
    CHECK(asked_closed());   // the poly page goes (3A 0 0), as when the selection leaves a seq track
    CHECK(!lit());           // no poly step on the trig LEDs
    from_rytm({0, 0, 1}, 0x30);   // the Rytm closes the page
    tick();
    from_rytm({seq::kSeqMode, 0, 0x7F}, 0x2D);   // its own seq mode ends (the 2E without CH)
    for (int i = 0; i < 3; ++i) tick();
    CHECK(!remote_open() && !lit() && seq::seqmode_on() < 0);
    // the SEQ page shown again on CH some other way (the page list): the plain state - its kept step neither drawn nor lit
    remote_show_page("SEQ");
    from_rytm({0, 1, 0}, 0x30);
    for (int i = 0; i < 4; ++i) tick();
    {
      uint8_t tx[36 * kTextSlots];
      int n = sent_texts(tx, kTextSlots);
      bool plain = false;
      for (int i = 0; i < n; ++i) plain |= strstr((const char*)tx + 36 * i + 4, "NOT SEQUENCED") != nullptr;
      printf("  11. the SEQ page on CH (off): page %s, NOT SEQUENCED %d, leds lit %d\n",
             remote_page_title() ? remote_page_title() : "-", plain, lit());
      CHECK(remote_page_title() && !strcmp(remote_page_title(), "SEQ") && plain && !lit());
      // review 2: its trig keys and PAGE are not the page's (stock's): a tap edits no hidden kept step
      int pg = seq::view().page;
      key(2), key(0x84), tick();
      printf("  11. a tap and PAGE on the off track's page: steps %d page %d\n", steps(CH), seq::view().page);
      CHECK(steps(CH) == 1 && seq::view().page == pg && !seq::shown_poly());
    }
    page(0);
    // YES again turns it on with its steps (the current design, unchanged)
    cmd("seq=track:9:on");
    CHECK(mask() == 1u << CH && steps(CH) == 1);
    tick();
    // review: TURN OFF on the page TRK + TRIG opened (no seq mode) closes it as well
    page(0x10 + CH);
    g_out.clear();
    menu(), key(0x43), key(0x44), key(0x44), tick();
    CHECK(mask() == 0 && asked_closed());
    page(0);
    cmd("seq=track:9:on");
    // review: seq mode ends early at a switch to a pattern without CH (a trig held then let go edits nothing); back in
    // A01 the Rytm's 2D 08 1 brings it back
    from_rytm({seq::kSeqMode, 1, CH}, 0x2D), tick();
    from_rytm({0, 1, 0}, 0x30), tick();
    CHECK(seq::seqmode_on() == CH);
    from_rytm({0, 0, 1}, 0x31), tick();   // T1 held
    g_out.clear();
    go(1);   // A02: CH is no poly track there
    printf("  11. switched to A02 in seq mode: seqmode %d mask 0x%03x asked closed %d\n", seq::seqmode_on(), mask(), asked_closed());
    CHECK(seq::seqmode_on() < 0 && mask() == 0 && asked_closed() && seq::view().held < 0);
    from_rytm({0, 0, 0}, 0x31), tick();   // T1 let go
    page(0);
    go(0);
    CHECK(steps(CH) == 1);
    from_rytm({seq::kSeqMode, 1, CH}, 0x2D), tick();
    CHECK(seq::seqmode_on() == CH);
    from_rytm({seq::kSeqMode, 0, 0x7F}, 0x2D), tick();
    page(0);
  }

  // ---- 12 (fix1028 follow-up, the user's ruling "Ask on make"): the make prompt on a track with kept notes (an earlier
  // TURN OFF) asks a second question, RESTORE n NOTES? - YES brings them back, NO starts empty through an edit the
  // shared history keeps (FUNC + YES / the touchscreen's UNDO bring them back). No kept notes: no second question.
  {
    static std::vector<std::pair<int, seq::Track>> hist;
    hist.clear();
    static const seq::UndoHooks h = {
        [](int r, int, const seq::Track& t) { hist.push_back({r, t}); },
        [](int r, int, seq::Track& t) {
          if (hist.empty() || hist.back().first != r) return false;
          t = hist.back().second, hist.pop_back();
          return true;
        },
        nullptr};
    seq::set_undo_hooks(&h);
    auto said = [](const char* x) {
      uint8_t tx[36 * kTextSlots];
      int n = sent_texts(tx, kTextSlots);
      for (int i = 0; i < n; ++i)
        if (strstr((const char*)tx + 36 * i + 4, x)) return true;
      return false;
    };
    auto func_yes = [] { from_rytm({1, 0, 1}, 0x31), key(0x44), from_rytm({1, 0, 0}, 0x31); };
    go(0);
    step(CH, 4, 64);   // CH: two notes kept
    CHECK(mask() == 1u << CH && steps(CH) == 2);
    cmd("seq=track:9:off");
    // (a) YES: restored
    page(0x10 + CH);
    CHECK(seq::view().prompt == seq::kPromptMake);
    question_keys(seq::kPromptMake);
    key(0x44), tick();
    printf("  12. make with kept notes: prompt %d mask 0x%03x\n", seq::view().prompt, mask());
    CHECK(seq::view().prompt == seq::kPromptRestore && mask() == 1u << CH && said("RESTORE 2 NOTES?"));
    question_keys(seq::kPromptRestore);
    key(0x44), tick();
    CHECK(seq::view().prompt == 0 && mask() == 1u << CH && steps(CH) == 2 && hist.empty());
    tick();
    CHECK(caught(0x44) == 0 && caught(0x51) == 0 && caught(0x13) == 0);   // answered: YES the Rytm's again
    CHECK(caught(0x42) == 0 && caught(0x43) == 0);   // updown1029: UP / DOWN too (CHRO's octave)
    // (b) NO: empty, one undo level; FUNC + YES on the SEQ page brings the notes back
    cmd("seq=track:9:off");
    page(0x10 + CH);
    key(0x44), tick();
    CHECK(seq::view().prompt == seq::kPromptRestore);
    key(0x45), tick();
    printf("  12. NO: steps %d, levels %d\n", steps(CH), (int)hist.size());
    CHECK(seq::view().prompt == 0 && mask() == 1u << CH && steps(CH) == 0 && hist.size() == 1);
    func_yes(), tick();
    printf("  12. FUNC + YES: steps %d\n", steps(CH));
    CHECK(steps(CH) == 2 && hist.empty() && seq::view().prompt == 0);
    // (c) no kept notes: on at once, no second question
    CHECK(!seq::edit_track(OH, false));
    page(0x10 + OH);
    CHECK(seq::view().prompt == seq::kPromptMake);
    key(0x44), tick();
    CHECK(seq::view().prompt == 0 && (mask() >> OH & 1));
    cmd("seq=track:10:off");
    seq::clear_steps(*seq::edit_track(CH, false), 4, 1), seq::touched();   // (back to case 11's CH: one step)
    page(0);
    page(2), tick();   // updown1029: only the SEQ page leaves UP / DOWN to the Rytm - INSERT (UP + FX) keeps them
    CHECK(caught(0x42) == 1 && caught(0x43) == 1 && caught(0x40) == 1);
    page(0);
    seq::set_undo_hooks(nullptr);
    CHECK(mask() == 1u << CH);
  }

  // ---- 13 (fix1028 review): a prompt belongs to its pattern and its track - a pattern switch or the track leaving the
  // poly tracks drops RESTORE / OFF (NO never clears another pattern's track); NO on a track emptied meanwhile takes no
  // level; without a history NO says the notes go for good; kept = the notes within the length, lock-only steps named
  {
    static std::vector<std::pair<int, seq::Track>> hist;
    hist.clear();
    static const seq::UndoHooks h = {
        [](int r, int, const seq::Track& t) { hist.push_back({r, t}); },
        [](int r, int, seq::Track& t) {
          if (hist.empty() || hist.back().first != r) return false;
          t = hist.back().second, hist.pop_back();
          return true;
        },
        nullptr};
    seq::set_undo_hooks(&h);
    auto said = [](const char* x) {
      uint8_t tx[36 * kTextSlots];
      int n = sent_texts(tx, kTextSlots);
      for (int i = 0; i < n; ++i)
        if (strstr((const char*)tx + 36 * i + 4, x)) return true;
      return false;
    };
    // A04: CH poly with a step of its own
    go(3);
    cmd("seq=track:9:on");
    step(CH, 0, 50);
    settle();
    go(0);
    CHECK(mask() == 1u << CH && steps(CH) == 1);
    auto restore_asked = [&] {
      cmd("seq=track:9:off");
      page(0x10 + CH);
      key(0x44), tick();
      return seq::view().prompt == seq::kPromptRestore;
    };
    // (1) RESTORE shows, the Rytm goes to A04, NO: A04's CH untouched, the prompt gone
    CHECK(restore_asked());
    go(3);
    printf("  13. RESTORE then A04: prompt %d, CH steps %d\n", seq::view().prompt, steps(CH));
    CHECK(seq::view().prompt == 0);
    size_t lv = hist.size();   // (the switch may cover A01's outside edits: NB-3's level, not NO's)
    key(0x45), tick();
    dump("13. A04 after NO");
    CHECK(mask() == 1u << CH && steps(CH) == 1 && hist.size() == lv);
    hist.clear();
    page(0);
    go(0);
    CHECK(mask() == 1u << CH && steps(CH) == 1);
    // (1b) the OFF question, then A04: YES turns nothing off there
    page(0x10 + CH);
    menu(), key(0x43), key(0x44), tick();
    CHECK(seq::view().prompt == seq::kPromptOff);
    question_keys(seq::kPromptOff);
    go(3);
    key(0x44), tick();
    printf("  13. OFF asked then A04 + YES: mask 0x%03x\n", mask());
    CHECK(mask() == 1u << CH);
    page(0);
    go(0);
    // (6) RESTORE shows, the console turns CH off: the prompt goes
    CHECK(restore_asked());
    cmd("seq=track:9:off"), tick();
    CHECK(seq::view().prompt == 0 && steps(CH) == 1);
    page(0);
    // (3) RESTORE shows, the track emptied meanwhile (the touchscreen): NO takes no level
    CHECK(restore_asked());
    seq::clear_steps(*seq::edit_track(CH, false), 0, seq::kMaxSteps), seq::touched();
    hist.clear();
    key(0x45), tick();
    printf("  13. NO on an emptied track: levels %d\n", (int)hist.size());
    CHECK(steps(CH) == 0 && hist.empty() && mask() == 1u << CH);
    page(0);
    // (7) kept: the notes within the length and the lock-only steps
    {
      seq::Track* t = seq::edit_track(CH, false);
      t->length = 16;
      seq::add_note(t->step[0], {60, 100, 72, 0, 0});
      seq::add_note(t->step[20], {62, 100, 72, 0, 0});   // past the length: never plays
      t->step[2].nl = 1, t->step[2].lock[0] = {0, 0, 5000};   // a lock-only step
      seq::touched();
    }
    CHECK(restore_asked());
    tick();
    printf("  13. kept: %d notes, %d lock steps\n", seq::view().prompt_n, seq::view().prompt_locks);
    CHECK(seq::view().prompt_n == 1 && seq::view().prompt_locks == 1 && said("RESTORE 1 NOTE?") && said("1 STEP OF LOCKS"));
    CHECK(said("UNDO-ABLE"));
    key(0x44), tick();
    page(0);
    // (4) without a history: the prompt says the notes go for good, and NO promises no undo
    seq::set_undo_hooks(nullptr);
    CHECK(restore_asked());
    tick();
    CHECK(said("FOR GOOD") && !said("UNDO-ABLE"));
    key(0x45), tick();
    CHECK(steps(CH) == 0 && !said("FUNC+YES"));
    page(0);
    // back: CH with one step (case 7 and on)
    step(CH, 0, 60);
    seq::edit_track(CH, false)->length = 0;
    seq::touched();
    tick();
    CHECK(mask() == 1u << CH && steps(CH) == 1);
  }

  // ---- 7. the switch's timing: the next pattern's mask goes out with the prediction, before its first step
  {
    // A01: CH poly (a note on step 0), A02: CH and OH poly (notes on step 0); playing A01, A02 queued at step 8
    go(1);
    cmd("seq=track:10:on");
    cmd("seq=track:9:on");
    step(OH, 0, 70);
    step(CH, 0, 61);
    settle();
    go(0);
    CHECK(mask() == 1u << CH);
    tick();
    g_out.clear();
    tick();
    from_rytm({seq::kScale, 0x7F, 16, 0, 2, 0, 0, 0}, 0x2D);   // the pattern's length 16, normal mode (the switch at 16)
    g_rytm_mask = 1 << CH;
    g_t0 = (int64_t)seq::frames() + 3000, g_next = 0;
    wire(g_t0 + kDelay, msg(0x2D, [&] { std::vector<uint8_t> x = {1, 1}; auto bb = blk4(g_t0); x.insert(x.end(), bb.begin(), bb.end()); return x; }()));
    run_until(g_t0 + (int64_t)(8.5 * kPer));
    from_rytm({seq::kPattern, 0, 1}, 0x2D);   // A02 queued
    while (seq::idle()) {}
    int64_t sw = g_t0 + (int64_t)llround(16 * kPer);
    wire(sw + kDelay, msg(0x2D, [&] { std::vector<uint8_t> x = {3, 0, 1}; auto bb = blk4(sw); x.insert(x.end(), bb.begin(), bb.end()); return x; }()));
    g_fired.clear();
    run_until(sw + (int64_t)(3 * kPer));
    int64_t at = -1;
    for (auto& o : g_out)
      if (at < 0 && o.m.size() >= 10 && o.m[4] == 0x2E && (o.m[6] | o.m[7] << 7) == ((1 << CH) | (1 << OH))) at = o.frame;
    int64_t off = seq::stats().offset;
    printf("  7. the 2E for A02 (CH OH) went %lld frames before the switch block (host frame %lld, switch %lld)\n",
           at < 0 ? 0LL : (long long)(sw + off - at), (long long)at, (long long)(sw + off));
    CHECK(at >= 0);
    CHECK(at < sw + off);                   // before the switch's first tick (it fires at sw, reaches us at sw + delay)
    CHECK(at > sw + off - (int64_t)kPer);   // and not bars early (the old pattern's last step kept its capture)
    CHECK(seq::stats().predict_ok == 1 && mask() == ((1u << CH) | (1u << OH)));
    bool oh_on_time = false;
    for (auto& f : g_fired)
      if (f.note == 70) oh_on_time |= f.h < sw + off + kDelay;
    printf("  7. OH's step 0 in A02 %s\n", oh_on_time ? "planned ahead (before the switch report)" : "late");
    CHECK(oh_on_time);
    // stop
    int64_t now = (int64_t)seq::frames();
    from_rytm({1, 0, 0, 0, 0, 0}, 0x2D);
    run_until(now + 4 * kBlock);
    (void)now;
  }

  CHECK(system(("rm -rf '" + card + "'").c_str()) == 0);
  printf("test_polypat: OK\n");
  return 0;
}
