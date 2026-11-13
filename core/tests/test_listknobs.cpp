// core/tests/test_listknobs.cpp - list knobs (ui/page.h list_turn, core/ui/README.md "List knobs"): one entry a detent
// on every list, enumerated value and switch, whatever the Rytm's raw counts say; a report's size never skips entries, a
// slow turn is never lost, a jittering knob does not bounce; continuous values keep their counts and FUNC x 8. The
// reports below come as the Rytm's do: one sum a knob every 67 ms [measured, emulator].
#include <cstdio>
#include <cstring>
#include <string>

#include <unistd.h>

#include "arp/arp.h"
#include "check.h"
#include "core_port.h"
#include "engine.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "lfo.h"
#include "samp/autosamp.h"
#include "samp/sampler.h"
#include "seq/seq.h"
#include "ui/extra.h"
#include "ui/remote.h"
#include "voices.h"

using namespace core;
using namespace core::ui;

static void sink(const uint8_t*, int) {}
static uint64_t g_us = 1000000000;
static void wait_ms(int ms) { host_fake_time(true, g_us += (uint64_t)ms * 1000); }
static void rest() { wait_ms(1000); }

// one report of `counts`, then the 67 ms to the next
static int report(int e, int counts) {
  int s = list_turn(e, counts);
  wait_ms(67);
  return s;
}
// `n` reports of `counts` into a page's encoder
static void spin(const ExtraPage& p, int e, int counts, int n, bool func = false) {
  for (int i = 0; i < n; ++i) p.enc(e, counts, func), wait_ms(67);
}
// a fast hard turn: 12 reports of 30 counts; a single detent after a rest
static void flick(const ExtraPage& p, int e, int dir) { rest(), spin(p, e, 30 * dir, 12); }
static void detent(const ExtraPage& p, int e, int dir, bool func = false) { rest(), p.enc(e, 4 * dir, func), wait_ms(67); }

static void test_helper() {
  // a lone turn, any size: one entry, at once
  rest(); CHECK(list_turn(0, 1) == 1);
  rest(); CHECK(list_turn(0, -1) == -1);
  rest(); CHECK(list_turn(0, 63) == 1);
  rest(); CHECK(list_turn(0, -63) == -1);
  CHECK(list_turn(0, 0) == 0);
  // a slow turn is never lost: one count at a time, far apart
  int sum = 0;
  for (int i = 0; i < 10; ++i) rest(), sum += list_turn(1, 1);
  CHECK(sum == 10);
  // a detent a report: one entry each; a detent spread over reports: one entry a detent, the first at once
  rest(), sum = 0;
  for (int i = 0; i < 12; ++i) sum += report(2, 4);
  CHECK(sum == 12);
  rest(), sum = 0;
  for (int i = 0; i < 12; ++i) sum += report(2, 1);
  CHECK(sum == 1 + 2);   // counts 1, 5, 9
  rest(), sum = 0;
  for (int i = 0; i < 12; ++i) sum += report(2, 2);
  CHECK(sum == 1 + 5);
  // click, pause shorter than a rest, click: a step each
  rest(), sum = 0;
  for (int i = 0; i < 5; ++i) sum += list_turn(2, 4), wait_ms(150);
  CHECK(sum == 5);
  // an accelerated turn does not skip: one entry a report, whatever its size
  rest(), sum = 0;
  for (int i = 0; i < 20; ++i) sum += report(3, 40);
  CHECK(sum == 20);
  // reports closer than the gap: at most one entry every 60 ms, and the counts wait for the next report
  rest(), sum = 0;
  CHECK(list_turn(3, 10) == 1);
  wait_ms(20); CHECK(list_turn(3, 10) == 0);
  wait_ms(20); CHECK(list_turn(3, 10) == 0);
  wait_ms(19); CHECK(list_turn(3, 1) == 0);    // 59 ms
  wait_ms(1); CHECK(list_turn(3, 1) == 1);     // 60 ms
  for (int i = 0; i < 300; ++i) wait_ms(1), sum += list_turn(3, 63);
  CHECK(sum >= 4 && sum <= 5);                 // 300 ms
  // a reversal starts over: a knob that jitters on a detent does not bounce
  rest(), sum = 0;
  CHECK(list_turn(4, 1) == 1);
  for (int i = 0; i < 40; ++i) sum += report(4, i % 2 ? 1 : -1);
  CHECK(sum == 0);
  rest(); CHECK(list_turn(4, 4) == 1);
  wait_ms(67); CHECK(list_turn(4, -3) == 0);
  wait_ms(67); CHECK(list_turn(4, -1) == -1);   // a whole detent back
  // each knob its own
  rest();
  CHECK(list_turn(5, 1) == 1 && list_turn(6, 1) == 1 && list_turn(5, 1) == 0 && list_turn(8, -1) == -1);
  // beyond the panel's knobs: the sign
  CHECK(list_turn(15, 9) == 1 && list_turn(-1, -9) == -1);
  // the long lists: a spin moves 4 a report, a turn below it one, a report under the gap nothing
  rest(); CHECK(list_turn_long(7, 4) == 1);
  wait_ms(67); CHECK(list_turn_long(7, 7) == 1);
  wait_ms(67); CHECK(list_turn_long(7, 8) == 4);
  wait_ms(67); CHECK(list_turn_long(7, -3) == 0);   // (the turn back: a count of its own)
  wait_ms(67); CHECK(list_turn_long(7, -20) == -4);
  wait_ms(10); CHECK(list_turn_long(7, -20) == 0);
  // a quick spin through 127 entries, 12 counts a report: about 2 s; 4 counts a report: under 9 s; FUNC is the page's x 8
  rest();
  int at = 0, reports = 0;
  while (at < 126) at += list_turn_long(7, 12), wait_ms(67), ++reports;
  printf("127 entries: spin %.1f s\n", reports * 0.067);
  CHECK(reports * 67 < 2300);
  rest(), at = 0, reports = 0;
  while (at < 126) at += list_turn_long(7, 4), wait_ms(67), ++reports;
  CHECK(reports * 67 < 9000);
}

static void test_step_value() {
  const Page* plaits = page_for_machine(0x00);
  CHECK(plaits);
  CHECK(eng_zone(step_value(*plaits, 0, 64, 3, false), 17) == 8 + 1);    // one zone, whatever the size
  CHECK(eng_zone(step_value(*plaits, 0, 64, -50, false), 17) == 8 - 1);
  CHECK(eng_zone(step_value(*plaits, 0, 127, 50, false), 17) == 16);     // the end holds
  CHECK(step_value(*plaits, 1, 64, -3, false) == 61 && step_value(*plaits, 1, 64, 2, true) == 80);   // a dial as before
}

static void test_ins() {
  const int ch = 0;
  ins::set_type(ch, 0, 0);
  // E slot A / B: a switch, B on any turn up, stays there; a detent back A
  detent(kInsPage, 5, 1);
  CHECK(ins::control.type[ch][0] == 1);
  flick(kInsPage, 5, -1);
  CHECK(ins::control.type[ch][0] == 0);
  // F TYPE: a detent, one type; a hard turn does not jump the list: at most one a report
  detent(kInsPage, 5, 1), detent(kInsPage, 5, 1);
  CHECK(ins::control.type[ch][0] == 2);
  rest(), spin(kInsPage, 5, 30, 3);
  CHECK(ins::control.type[ch][0] == 2 + 3);
  // E, then F writes slot B: the switch moved once, and does not bounce back on a turn over its end
  flick(kInsPage, 4, 1);
  ins::set_type(ch, 1, 0);
  detent(kInsPage, 5, 1);
  CHECK(ins::control.type[ch][1] == 1);
  flick(kInsPage, 4, 1);
  detent(kInsPage, 5, 1);
  CHECK(ins::control.type[ch][1] == 2);
  ins::set_type(ch, 1, 0);
  flick(kInsPage, 4, -1);
  ins::set_type(ch, 0, 0);
  // A..D stay continuous: counts, FUNC x 8
  ins::set_type(ch, 0, 1);
  int p = ins::param(ch, 0, 0);
  ins::set_param(ch, 0, 0, 40), p = 40;
  kInsPage.enc(0, 5, false); CHECK(ins::param(ch, 0, 0) == p + 5);
  kInsPage.enc(0, 5, true); CHECK(ins::param(ch, 0, 0) == p + 5 + 40);
  ins::set_type(ch, 0, 0);
}

static void test_master() {
  master::set_src(master::kRet);
  ins::set_type(master::chain(0), master::slot(0), 0);
  detent(kMasterPage, 5, 1);
  int t1 = ins::control.type[master::chain(0)][master::slot(0)];
  CHECK(t1 != 0);
  rest(), kMasterPage.enc(5, 30, false), wait_ms(20), kMasterPage.enc(5, 30, false);   // the second, inside the gap
  int t2 = ins::control.type[master::chain(0)][master::slot(0)];
  CHECK(t2 == master::next_type(0, t1, 1));
  rest(), spin(kMasterPage, 7, 4, 3);
  CHECK(master::control.src == master::kMix);
  // a jitter on SRC does not bounce
  rest(), kMasterPage.enc(7, 1, false); CHECK(master::control.src == master::kMix);
  wait_ms(67), kMasterPage.enc(7, -1, false); CHECK(master::control.src == master::kMix);
  master::set_src(master::kRet);
  ins::set_type(master::chain(0), master::slot(0), 0);
}

static void test_lfo() {
  lfo::Settings s = lfo::get(0);
  s.target = 0, s.wave = 0, s.retrig = 0, s.sync = 0, s.rate = 64;
  lfo::set(0, s);
  // WAVE, one a detent, none skipped; RETRIG; SYNC, a switch
  detent(kLfoPage, 4, 1); CHECK(lfo::get(0).wave == 1);
  rest(), spin(kLfoPage, 4, 30, 2); CHECK(lfo::get(0).wave == 3);
  detent(kLfoPage, 7, 1); CHECK(lfo::get(0).retrig == 1);
  flick(kLfoPage, 6, 1); CHECK(lfo::get(0).sync == 1);
  rest(), kLfoPage.enc(6, 1, false), wait_ms(67), kLfoPage.enc(6, -1, false); CHECK(lfo::get(0).sync == 1);
  flick(kLfoPage, 6, -1); CHECK(lfo::get(0).sync == 0);
  // TARGET: a detent, one; a spin, four a report
  detent(kLfoPage, 1, 1); CHECK(lfo::target_pos(lfo::get(0).target) == 1);   // list positions (the display order): one entry a step
  CHECK(lfo::get(0).target == lfo::kTVoice);   // the stored ids stay the ids: BD is V1
  rest(), spin(kLfoPage, 1, 4, 2); CHECK(lfo::target_pos(lfo::get(0).target) == 3 && lfo::get(0).target == lfo::kTVoice + 7);   // RS (unknown: the pair's owner): V8
  rest(), spin(kLfoPage, 1, 20, 2); CHECK(lfo::target_pos(lfo::get(0).target) == 3 + 4 + 4 && lfo::get(0).target == lfo::target_from_name("I1"));   // past the 8 voices, FX1 FX2: I1 (no Rytm track entries since the owner's test)
  // DEPTH and RATE (free) stay continuous
  s = lfo::get(0), s.depth = 0, s.rate = 64, lfo::set(0, s);
  kLfoPage.enc(3, 5, false); CHECK(lfo::get(0).depth == 5);
  kLfoPage.enc(3, 5, true); CHECK(lfo::get(0).depth == 45);
  kLfoPage.enc(5, 3, false); CHECK(lfo::get(0).rate == 67);
  // the design view (YES): step, shape, preset. A report the gap holds back leaves the design alone
  kLfoPage.key(0x44);
  s = lfo::get(0), s.step[0] = 11, s.shape[0] = 0, s.shape[1] = 0, lfo::set(0, s);
  detent(kLfoPage, 2, 1);
  CHECK(lfo::get(0).shape[0] == 1);
  rest(), kLfoPage.enc(3, 4, false);                 // PRESET: 1
  s = lfo::get(0), s.step[0] = 11, lfo::set(0, s);
  wait_ms(10), kLfoPage.enc(3, 4, false);            // held back: the steps stay
  CHECK(lfo::get(0).step[0] == 11);
  kLfoPage.key(0x44);
}

static void test_samp() {
  samp::Config c = samp::config();
  // SOURCE: the first of 18, a hard turn walks one a report
  flick(kSampPage, 0, -1);
  samp::Config c0 = samp::config();
  detent(kSampPage, 0, 1);
  samp::Config c1 = samp::config();
  CHECK(c0.src != c1.src || c0.sub != c1.sub);
  flick(kSampPage, 0, -1);
  rest(), kSampPage.enc(0, 40, false);
  samp::Config c2 = samp::config();
  CHECK(c1.src == c2.src && c1.sub == c2.sub);   // 40 counts: the same one entry as a detent
  // START, SLICES
  flick(kSampPage, 1, -1); CHECK(samp::config().start == 0);
  rest(), kSampPage.enc(1, 3, false); CHECK(samp::config().start == 1);
  samp::configure(c);
}

static void test_auto() {
  samp::Plan p = samp::asamp_plan();
  p.src = (samp::ASrc)0;
  samp::asamp_configure(p);
  rest(), kAutoPage.enc(0, 30, false);
  CHECK((int)samp::asamp_plan().src == 1);
  rest(), kAutoPage.enc(6, 30, false);                  // RR 1..4
  CHECK(samp::asamp_plan().rr == p.rr + 1);
  int lo = samp::asamp_plan().lo;
  kAutoPage.enc(2, 3, false);                           // LO stays a value
  CHECK(samp::asamp_plan().lo == lo + 3);
}

// ---- every other page: what 40 counts in one report do is what a detent does, from the low end, on the page as drawn
static Canvas g_low, g_a, g_b;
static void draw(const ExtraPage& p, Canvas& c) {
  c.clear(), c.clear_text();
  p.draw(c, 0, 1, 12345);
}
static bool same(const Canvas& a, const Canvas& b) {
  uint64_t m[2] = {};
  return !memcmp(a.fb, b.fb, sizeof a.fb) && !a.diff_text(b, m);
}
static void low(const ExtraPage& p, int e) {
  for (int i = 0; i < 4; ++i) flick(p, e, -1);
}
static void one_entry(const ExtraPage& p, int e, int big = 40, bool moves = true) {
  low(p, e);
  draw(p, g_low);
  detent(p, e, 1);
  draw(p, g_a);
  CHECK(!moves || !same(g_low, g_a));
  low(p, e);
  rest();
  p.enc(e, big, false);
  draw(p, g_b);
  CHECK(same(g_a, g_b));
  low(p, e);
}

static void machine(int t, int vb) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, 0x33, 1, (uint8_t)t, 1, 44, (uint8_t)vb, 0xF7};
  engine_sysex(m, sizeof m);
}
static void msg3(uint8_t cmd, int a, int b, int c) {
  const uint8_t m[] = {0xF0, 0x7D, 0x52, 0x54, cmd, 1, (uint8_t)a, (uint8_t)b, (uint8_t)c, 0xF7};
  engine_sysex(m, sizeof m);
}

static void test_arp() {
  for (int e = 0; e < 5; ++e) one_entry(kArpPage, e);
  rest(), kArpPage.enc(0, 40, false);
  CHECK(arp::get(0).mode == 1);
  wait_ms(67), kArpPage.enc(0, 1, false);   // (it took the sign before: a count on from the detent is no entry yet)
  CHECK(arp::get(0).mode == 1);
  low(kArpPage, 0);
}
static void test_scope() {
  const ExtraPage* p = nullptr;
  for (int i = 0; i < extra_pages(); ++i)
    if (!strcmp(extra_page(i).title, "SCOPE")) p = &extra_page(i);
  CHECK(p);
  one_entry(*p, 0), one_entry(*p, 1), one_entry(*p, 2);
}
static void test_menu() {
  one_entry(kMenuPage, 0);
  one_entry(kPoolPage, 0), one_entry(kPoolPage, 1), one_entry(kPoolPage, 3);
  one_entry(kPoolPage, 2, 7);   // (the long list's spin is 8 counts a report)
}
static void test_rec() {
  remote_play(PlayInfo{});
  PlayInput in;
  rest(), kRecPage.enc(1, 40, false);
  rest(), kRecPage.enc(2, 7, false);
  kRecPage.enc(3, 40, false);
  CHECK(remote_play_take(&in) && in.channel == 1 && in.take == 1 && in.level == 40);
  rest(), kRecPage.enc(2, 40, false);
  wait_ms(10), kRecPage.enc(2, 40, false);   // inside the gap
  wait_ms(10), kRecPage.enc(1, -40, false);
  CHECK(remote_play_take(&in) && in.channel == -1 && in.take == 4 && !in.level);
}
static void test_poly() {
  char out[64];
  CHECK(seq::command("seq=track:1:on", out, sizeof out));
  CHECK(seq::owns_track(0));
  for (int e : {0, 1, 2, 5}) one_entry(kPolyPage, e);
  rest(), kPolyPage.enc(0, 40, false);
  CHECK(seq::poly_of(0).voices == 1);
  wait_ms(67), kPolyPage.enc(0, 40, false), wait_ms(20), kPolyPage.enc(0, 40, false);   // the second inside the gap
  CHECK(seq::poly_of(0).voices == 2);
  low(kPolyPage, 0);
  int det = seq::poly_of(0).detune;
  kPolyPage.enc(3, 5, false);   // a value: counts
  CHECK(seq::poly_of(0).detune == det + 5);
}
static void test_seq_page() {
  char out[64];
  CHECK(seq::command("seq=track:1:on", out, sizeof out));
  // no held trig: QNT, FOLW; DEFAULTS' DIR, SPD; SCALE's KEY, SCL; RANDOM's WGHT, MODE
  one_entry(kSeqPage, 3);
  seq::ui_key(0x30, true, true), seq::ui_key(0x30, false, true);
  one_entry(kSeqPage, 4), one_entry(kSeqPage, 5);
  seq::ui_key(0x30, true, true), seq::ui_key(0x30, false, true);
  one_entry(kSeqPage, 0), one_entry(kSeqPage, 1);
  seq::ui_key(0x30, true, true), seq::ui_key(0x30, false, true);
  one_entry(kSeqPage, 2), one_entry(kSeqPage, 3);
  seq::ui_key(0x30, true, true), seq::ui_key(0x30, false, true);
  // a held trig: TRIG's COND a long list, CHD, INV a list
  seq::ui_key(0, true, true);
  seq::ui_key(0x30, true, true), seq::ui_key(0x30, false, true);
  one_entry(kSeqPage, 6);    // CHD
  one_entry(kSeqPage, 4, 7);   // COND
  rest(), kSeqPage.enc(4, 4, false);
  draw(kSeqPage, g_a);
  low(kSeqPage, 4);
  rest(), kSeqPage.enc(4, 40, false);
  draw(kSeqPage, g_b);       // a spin: more than a detent, not 40
  CHECK(!same(g_a, g_b));
  seq::ui_key(0x30, true, true), seq::ui_key(0x30, false, true);   // RTRG
  one_entry(kSeqPage, 0), one_entry(kSeqPage, 2), one_entry(kSeqPage, 4);
  seq::ui_key(0x30, true, true), seq::ui_key(0x30, false, true);   // FX
  one_entry(kSeqPage, 0);
  seq::ui_key(0, false, true);
}
static void test_voice_page() {   // PLAITS' ENG through the host: 32 as the Rytm sends it
  remote_reset();
  uint8_t r[39] = {0xF0, 0x7D, 0x52, 0x54, 0x03, 0x00};
  for (int k = 0; k < 16; ++k) r[6 + 2 * k] = 45;
  r[38] = 0xF7;
  engine_sysex(r, sizeof r);
  msg3(0x30, 0, 1, 0);
  uint32_t t = 1000;
  remote_tick(t);
  for (int i = 0; i < 8 && remote_stats().page != 1; ++i) msg3(0x31, 0, 0x41, 1), msg3(0x31, 0, 0x41, 0), remote_tick(t += 30);
  CHECK(remote_stats().page == 1 && remote_stats().voice == 0);
  const int z = eng_zone(v[0].p[0], 17);
  rest(), msg3(0x32, 0, 64 + 40, 0);
  CHECK(eng_zone(v[0].p[0], 17) == z + 1);
  wait_ms(20), msg3(0x32, 0, 64 + 40, 0);
  CHECK(eng_zone(v[0].p[0], 17) == z + 1);
  rest(), msg3(0x32, 0, 64 - 40, 0);
  CHECK(eng_zone(v[0].p[0], 17) == z);
  const float hrm = v[0].p[1];   // HRM: a value, counts
  msg3(0x32, 1, 64 + 5, 0);
  CHECK(v[0].p[1] == hrm + 5);
}

int main(int argc, char** argv) {
  list_pacing(true);
  char tmpl[] = "/tmp/core_listknobs_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = sink;
  host_fake_time(true, g_us);
  CHECK(engine_begin());
  const uint8_t proj[] = {0xF0, 0x7D, 0x52, 0x54, 0x10, 'L', 'K', 0xF7};
  engine_sysex(proj, sizeof proj);
  for (int i = 0; i < 200000 && core::project_loading(); ++i) core::idle_save();
  CHECK(!core::project_loading());
  machine(0, 0x00);
  struct { const char* name; void (*fn)(); } tests[] = {
      {"helper", test_helper}, {"step_value", test_step_value}, {"ins", test_ins}, {"master", test_master}, {"lfo", test_lfo},
      {"samp", test_samp}, {"auto", test_auto}, {"arp", test_arp}, {"scope", test_scope}, {"menu", test_menu}, {"rec", test_rec},
      {"poly", test_poly}, {"seq", test_seq_page}, {"voice", test_voice_page}};
  for (auto& t : tests)
    if (argc < 2 || !strcmp(argv[1], t.name)) printf("-- %s\n", t.name), fflush(stdout), t.fn();
  printf("list knobs: all pass\n");
  return 0;
}
