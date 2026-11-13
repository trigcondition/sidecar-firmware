// core/tests/test_param_names.cpp - the modulation targets' params by name (2026-11, the owner on hardware: "can we have
// the names of parameters displayed in the sidecar lfo targets and xy pad and morph target settings? It just says p1"):
// ui::voice_label for every machine's P1..P16 (its pages, its SMPL page's labels, a user machine's manifest through
// hooks.user_label, MD's TYPE), following a machine change; mod::label / param_label ("V1:HRM", "FX1:CUT", "I1:ACUT",
// "V2:-" unused); lfo::param_name; the LFO JSON's pns and the scenes JSON's pl / vl for the web; the ids unchanged.
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "core.h"
#include "fx/insert.h"
#include "fx/send_fx.h"
#include "hooks.h"
#include "lfo.h"
#include "machines.h"
#include "md/md.h"
#include "mod/scene.h"
#include "mod/target.h"
#include "ui/extra.h"
#include "voices.h"

using namespace core;

static std::string vl(int vi, int k) {
  char b[8];
  return ui::voice_label(vi, k, b, sizeof b);
}
static mod::Tid T(const char* n) {
  mod::Tid t = 0;
  CHECK(mod::parse(n, &t));
  return t;
}
static std::string label(const char* n) {
  char b[24];
  return mod::label(T(n), b, sizeof b);
}
static std::string pname(int target, int p) {
  char b[8];
  return lfo::param_name(target, p, b, sizeof b);
}
static void plain(int vi) {   // no machine flag: PLAITS
  V& x = v[vi];
  x.fm = x.juno = x.daisy = x.dsps = x.smpl = x.grain = x.md = false;
  x.user = -1;
#if CORE_MSAMP
  x.ms = x.wt = x.gr = x.li = x.cp = x.mi = false;
#endif
}
static bool fake_label(int vi, int k, char* b, int cap) {   // a user machine whose manifest names P3 WARP, P1 only on U2
  if (v[vi].user < 0) return false;
  snprintf(b, (size_t)cap, "%s", k == 2 ? "WARP" : k == 0 && v[vi].user == 1 ? "OSC" : "");
  return true;
}

int main() {
  host_fake_time(true, 1000000);
  engine_begin();
  for (int vi = 0; vi < 8; ++vi) plain(vi);

  // ---- PLAITS: its page's eight, its PAGE 2's eight; the ids stay P<n>
  CHECK(vl(0, 0) == "ENG" && vl(0, 1) == "HRM" && vl(0, 7) == "LEV" && vl(0, 8) == "FM" && vl(0, 12) == "MMD" && vl(0, 15) == "VOL");
  CHECK(label("V1:P2") == "V1:HRM" && label("V1:P13") == "V1:MMD");
  {
    char b[16];
    CHECK(!strcmp(mod::name(T("V1:P2"), b, sizeof b), "V1:P2"));          // (the id: files, console, xy=set)
    CHECK(!strcmp(mod::param_label(T("V1:P2"), b, sizeof b), "HRM"));
  }
  CHECK(pname(lfo::kTVoice, 1) == "HRM" && pname(lfo::kTVoice, 9) == "FIN");
  printf("names: PLAITS P2 HRM, P13 MMD (its page 2)\n");

  // ---- a machine change: the names follow (JUNO, DAISYSP, 6-OP FM; an unused P "-")
  v[1].juno = true;
  CHECK(vl(1, 1) == "CUT" && vl(1, 7) == "LEN" && vl(1, 8) == "");
  CHECK(label("V2:P2") == "V2:CUT" && label("V2:P9") == "V2:-" && pname(lfo::kTVoice + 1, 8) == "-");
  v[2].daisy = true;
  CHECK(vl(2, 4) == "INP" && vl(2, 10) == "SEED" && vl(2, 15) == "WID");
  v[3].fm = true;
  CHECK(vl(3, 0) == "PAT" && vl(3, 8) == "" && vl(3, 12) == "CHD" && vl(3, 15) == "VOI");
  plain(1);
  CHECK(label("V2:P2") == "V2:HRM");
  printf("names: JUNO CUT, DAISYSP SEED, FM CHD; back to PLAITS HRM; unused V2:-\n");

  // ---- the sample machines: DSP SAMPLE (its SMPL page and its SRC page), SAMPLER (P9..P16 only), GRAIN
  v[4].dsps = true;
  CHECK(vl(4, 8) == "TUN" && vl(4, 4) == "MIX" && vl(4, 0) == "ENG");
  v[4].smpl = true;   // SAMPLER: DSP SAMPLE's flag with SAMPLER's tag
  CHECK(vl(4, 8) == "TUNE" && vl(4, 10) == "SMP" && vl(4, 13) == "END" && vl(4, 0) == "");
  CHECK(label("V5:P11") == "V5:SMP" && label("V5:P1") == "V5:-");
  plain(4);
#if CORE_MSAMP
  v[4].cp = true;
  CHECK(vl(4, 1) == "SLC" && vl(4, 8) == "STUT" && vl(4, 15) == "LOOP");
  plain(4);
  v[4].ms = true;
  CHECK(vl(4, 0) == "INST" && vl(4, 9) == "FINE" && vl(4, 12) == "CHD");
  plain(4);
  v[4].li = true;
  CHECK(vl(4, 0) == "INPT" && vl(4, 8) == "REL" && vl(4, 9) == "");
  plain(4);
  v[4].mi = true;
  CHECK(vl(4, 1) == "CHAN" && vl(4, 12) == "SEL1");
  plain(4);
#endif
  printf("names: DSP SAMPLE MIX / TUN, SAMPLER SMP / TUNE (no P1..P8), CHOP, MULTISAMPLE, LIVE IN, MIDI\n");

  // ---- a user machine: its manifest's labels (the hook), "-" a knob it does not have; another slot, then none
  hooks.user_label = fake_label;
  v[5].user = 0;
  CHECK(vl(5, 2) == "WARP" && vl(5, 0) == "" && label("V6:P3") == "V6:WARP" && label("V6:P1") == "V6:-");
  v[5].user = 1;
  CHECK(vl(5, 0) == "OSC" && label("V6:P1") == "V6:OSC");
  plain(5);
  CHECK(label("V6:P3") == "V6:TIM");
  hooks.user_label = nullptr;
  v[5].user = 2;
  CHECK(vl(5, 2) == "");   // (a board without plugins: no names)
  plain(5);
  printf("names: a user machine's manifest (WARP, OSC), back to PLAITS TIM\n");

#if CORE_MD
  v[6].md = true;
  md::enter(6);
  CHECK(vl(6, 8) != "" && vl(6, 9) != "" && vl(6, 0) != "" && vl(6, 15) == "");   // ENGINE TYPE, a control; P16 unused
  printf("names: MD %s %s %s\n", vl(6, 0).c_str(), vl(6, 8).c_str(), vl(6, 9).c_str());
  plain(6);
#endif

  // ---- send FX by its TYPE, INSERT by its slots' TYPEs and the POST EQ, the fixed names as they were
  {
    const fx::KnobLabels* l = fx::labels(fx::control.knob[0][0]);
    std::string want = l && l->label[0] && l->label[0][0] ? std::string("FX1:") + l->label[0] : "FX1:-";
    CHECK(label("FX1:P2") == want);
  }
#if CORE_POLY
  ins::control.type[0][0] = ins::kOff;
  CHECK(label("I1:A1") == "I1:A-");
  ins::control.type[0][0] = ins::kFilter;
  CHECK(label("I1:A1") == "I1:ACUT" && label("I1:A2") == "I1:ARES" && label("I1:E1") == "I1:QHPF");
  ins::control.type[0][0] = ins::kOff;
  // the bus chains B1..B3 (8..10, lfo kTIns + 8..) and the master's M1 / M2 (11, 12: lfo kTMaster) by their slots' TYPEs
  for (int c : {8, 9, 10, 11, 12}) {
    char n[16], want[16];
    snprintf(n, sizeof n, "%s:A1", ins::chain_name(c));
    CHECK(label(n) == std::string(ins::chain_name(c)) + ":A-");
    ins::control.type[c][0] = ins::kFilter;
    snprintf(want, sizeof want, "%s:ACUT", ins::chain_name(c));
    CHECK(label(n) == want);
    snprintf(n, sizeof n, "%s:E1", ins::chain_name(c));
    snprintf(want, sizeof want, "%s:QHPF", ins::chain_name(c));
    CHECK(label(n) == want);
    ins::control.type[c][0] = ins::kOff;
  }
  CHECK(label("B3:A2") == "B3:A-" && label("M2:A1") == "M2:A-");
  CHECK(label("RYTM:PERF3") == "RYTM:PERF3" && label("MFX:LEV") == "MFX:LEV" && label("S5:F1") == "S5:F1" && label("T1:SPD") == "T1:SPD");
  printf("names: FX1 by its TYPE, I1:ACUT / I1:QHPF, the fixed ones unchanged\n");

  // ---- the web: the LFO JSON names every param of its target (pns), the scenes JSON its locks (pl) and every voice's (vl)
  {
    lfo::Settings s = lfo::get(0);
    s.target = (uint8_t)lfo::kTVoice, s.param = 1;
    CHECK(lfo::set(0, s));
    static char js[8192];
    int n = lfo::json(js, sizeof js);
    CHECK(n > 0 && n < (int)sizeof js);
    std::string j(js, (size_t)n);
    CHECK(j.find("\"pn\":\"HRM\"") != std::string::npos);
    CHECK(j.find("\"pns\":[\"ENG\",\"HRM\",\"TIM\",\"MOR\",\"DEC\",\"LPG\",\"TUN\",\"LEV\",\"FM\",\"FIN\",\"AUX\",\"TMD\",\"MMD\",\"VEL\",\"GLD\",\"VOL\"]") != std::string::npos);
    CHECK(j.find("\"pns\":[]") != std::string::npos);   // an LFO off: no params
    lfo::reset();
  }
  {   // every LFO on a voice with 16 named params, into the Pi's /status buffer (lfo::kJsonCap): it fits, valid; and 26
      // LFOs (the next release: 2 per Rytm track T1..T12, 2 for the FX track) at this worst case per LFO fit it too
    for (int vi = 0; vi < 8; ++vi) plain(vi), v[vi].daisy = true;   // (DAISY: 16 named params)
    for (int i = 0; i < lfo::kLfos; ++i) {
      lfo::Settings s = lfo::get(i);
      s.target = (uint8_t)(lfo::kTVoice + i % 8), s.param = 15, s.depth = -64, s.rate = 127, s.wave = lfo::kDesign;
      CHECK(lfo::set(i, s));
    }
    static char js[lfo::kJsonCap];
    int n = lfo::json(js, sizeof js);
    CHECK(n > 0 && n < (int)sizeof js);
    std::string j(js, (size_t)n);
    size_t c = 0;
    for (size_t at = 0; (at = j.find("\"pns\":[\"ENG\",\"STR\"", at)) != std::string::npos; ++at) ++c;
    CHECK((int)c == lfo::kLfos && j.compare(j.size() - 2, 2, "]}") == 0);
    int one = n / lfo::kLfos;   // (the head is in it too: an overestimate)
    printf("names: %d LFOs, %d bytes of LFO JSON (%d each), 26 at that: %d of %d\n", lfo::kLfos, n, one, 26 * one,
           lfo::kJsonCap);
    CHECK(26 * one + 64 < lfo::kJsonCap);
    for (int vi = 0; vi < 8; ++vi) plain(vi);
    lfo::reset();
  }
  {
    scene::reset();
    CHECK(scene::lock(0, T("V1:P2"), 6400) && scene::lock(0, T("MFX:LEV"), 100) && scene::lock(1, T("V1:P2"), 0));
    static char js[16384];
    int n = scene::json(js, sizeof js);
    CHECK(n > 0 && n < (int)sizeof js);
    std::string j(js, (size_t)n);
    CHECK(j.find("[\"V1:P2\",6400]") != std::string::npos);                 // (the ids as before)
    CHECK(j.find("\"pl\":{\"V1:P2\":\"V1:HRM\"}") != std::string::npos);    // once, MFX:LEV needs none
    CHECK(j.find("\"vl\":[[\"ENG\",\"HRM\",") != std::string::npos && j.find("],\"src\":") != std::string::npos);
    CHECK(j.compare(j.size() - 11, 11, ",\"rscn\":-1}") == 0);
    // no room for pl / vl: the old fields only, valid, ending as before; at every buffer size from that to the full one
    // (each sniprintf inside the 32 bytes kept for src / rscn: never a size <= 0)
    const size_t p0 = j.find(",\"pl\":"), p1 = j.find(",\"src\":");
    CHECK(p0 != std::string::npos && p1 != std::string::npos);
    const std::string old = j.substr(0, p0) + j.substr(p1);
    for (int cap = (int)old.size() + 1; cap <= n + 1; ++cap) {
      static char sm[16384];
      memset(sm, 'x', sizeof sm);
      int m = scene::json(sm, cap);
      CHECK(m > 0 && m < cap && sm[m] == 0 && sm[cap] == 'x');
      std::string k(sm, (size_t)m);
      CHECK(k == j || k == old);
      if (cap < n + 1) CHECK(k == old);
    }
    scene::reset();
  }
  printf("names: /status lfo pns, scene pl / vl\n");
#endif
  printf("test_param_names: PASS\n");
  return 0;
}
