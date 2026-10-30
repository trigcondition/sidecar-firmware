// core/seq/xcmd.cpp - see seq.h "seqtop": the console's commands on the pattern model itself (no clock, no card, no
// voices): the speed, direction, key / scale, random notes, SEED MOD, ratchets, FX columns, lock conditions and slides.
// core/seq/seq.cpp hands every line here first (then replans and saves); the touch editor's host build (pi/ui/host
// seqdemo.cpp) applies the same lines to its own pattern, so the shots show what core 3 would.
#include "seq/seq.h"

#if CORE_POLY
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "seq/rng.h"

namespace core {
namespace seq {

namespace {
// seqtop: the next "key=value" of a comma-separated list (q moves past it); false at its end
CORE_COLD bool kv_next(const char*& q, char* k, int kc, char* val, int vc) {
  if (!q || !*q) return false;
  int n = 0;
  while (*q && *q != '=' && *q != ',') {
    if (n < kc - 1) k[n++] = *q;
    ++q;
  }
  k[n] = 0, n = 0;
  if (*q == '=') ++q;
  while (*q && *q != ',') {
    if (n < vc - 1) val[n++] = *q;
    ++q;
  }
  val[n] = 0;
  if (*q == ',') ++q;
  return true;
}

CORE_COLD int word_of(const char* w, const char* const* names, int n) {   // a name (any case) or a number
  if (*w >= '0' && *w <= '9') {
    int x = atoi(w);
    return x < n ? x : -1;
  }
  for (int i = 0; i < n; ++i) {
    const char* a = w;
    const char* b = names[i];
    while (*a && *b && (*a == *b || *a == *b + 32)) ++a, ++b;
    if (!*a && !*b) return i;
  }
  return -1;
}

CORE_COLD Note def_note(const Track& t) {   // a step's first note: the track's defaults (as a tap's)
  return {(uint8_t)(t.dnote & 0x7F), (uint8_t)(t.dvel ? t.dvel : 100), t.dlen ? t.dlen : (uint16_t)(kSub * 3 / 4), 0, 0};
}
}  // namespace

CORE_COLD int command_x(Pattern& pt, const char* p, int* track, char* out, int cap) {
  *track = -2;
  if (!strncmp(p, "seq=spd:", 8)) {   // seqtop P2: seq=spd:T:follow|1/8|1/4|1/2|3/4|3/2|2|2/3|5/4|7/8 (track T's own speed)
    int r = atoi(p + 8) - 1;
    const char* q = strchr(p + 8, ':');
    int sp = q ? speed_parse(q + 1) : -1;
    Track* t = sp >= 0 && r >= 0 && r < kRytmTracks ? track_of(pt, r, true) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=spd:T:follow|1/8|1/4|1/2|3/4|3/2|2|2/3|5/4|7/8")), -1;
    t->speed = (uint8_t)sp;
    *track = r;
    return sniprintf(out, cap, CORE_STR("seq: track %d speed %s"), r + 1, speed_name(sp)), 1;
  }
  if (!strncmp(p, "seq=dir:", 8)) {   // seqtop P1: seq=dir:T:fwd|rev|pend|ping|rnd|brown (track T's play direction)
    int r = atoi(p + 8) - 1;
    const char* q = strchr(p + 8, ':');
    int d = q ? dir_parse(q + 1) : -1;
    Track* t = d >= 0 && r >= 0 && r < kRytmTracks ? track_of(pt, r, true) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=dir:T:fwd|rev|pend|ping|rnd|brown")), -1;
    t->dir = (uint8_t)d;
    *track = r;
    return sniprintf(out, cap, CORE_STR("seq: track %d direction %s"), r + 1, dir_name(d)), 1;
  }
  if (!strncmp(p, "seq=key:", 8)) {   // seqtop M1: seq=key:K:SCALE[:off|edit|play] | seq=key:off (the pattern's key / scale)
    const char* q = p + 8;
    if (!strcmp(q, "off")) {
      pt.key = 0, pt.scale = 0, pt.qnt = 0;
    } else {
      char kb[8] = {0};
      const char* c1 = strchr(q, ':');
      int kl = c1 ? (int)(c1 - q) : 0;
      if (kl > 0 && kl < (int)sizeof kb) memcpy(kb, q, (size_t)kl);
      char sb[12] = {0};
      const char* c2 = c1 ? strchr(c1 + 1, ':') : nullptr;
      int sl = c1 ? (int)(c2 ? c2 - c1 - 1 : (int)strlen(c1 + 1)) : 0;
      if (sl > 0 && sl < (int)sizeof sb) memcpy(sb, c1 + 1, (size_t)sl);
      int k = key_parse(kb), sc = scale_parse(sb), qn = !c2 ? pt.qnt : !strcmp(c2 + 1, "off") ? 0 : !strcmp(c2 + 1, "edit") ? 1
                                                            : !strcmp(c2 + 1, "play") ? 2 : -1;
      if (k < 0 || sc < 0 || qn < 0) return sniprintf(out, cap, CORE_STR("seq=key:C..B:CHR|MAJ|MIN|DOR|PENT|PHR|LYD|MIX|LOC|HMIN|MMIN|PMAJ|BLUES|WHOLE[:off|edit|play] | seq=key:off")), -1;
      pt.key = (uint8_t)k, pt.scale = (uint8_t)sc, pt.qnt = (uint8_t)qn;
    }
    *track = -1;
    return sniprintf(out, cap, CORE_STR("seq: pattern key %s %s, QNT %s"), key_name(pt.key), scale_name(pt.scale),
                     pt.qnt == kQntPlay ? "PLAY" : pt.qnt == kQntEdit ? "EDIT" : "OFF"), 1;
  }
  if (!strncmp(p, "seq=qnt:", 8)) {   // seqtop M1: seq=qnt:off|edit|play
    int qn = !strcmp(p + 8, "off") ? 0 : !strcmp(p + 8, "edit") ? 1 : !strcmp(p + 8, "play") ? 2 : -1;
    if (qn < 0) return sniprintf(out, cap, CORE_STR("seq=qnt:off|edit|play")), -1;
    pt.qnt = (uint8_t)qn;
    *track = -1;
    return sniprintf(out, cap, CORE_STR("seq: QNT %s (%s %s)"), qn == 2 ? "PLAY" : qn ? "EDIT" : "OFF", key_name(pt.key),
                     scale_name(pt.scale)), 1;
  }
  if (!strncmp(p, "seq=quantise:", 13)) {   // seqtop M1: seq=quantise:T - track T's notes moved into the pattern's scale
    int r = atoi(p + 13) - 1;
    Track* t = r >= 0 && r < kRytmTracks ? track_of(pt, r, false) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=quantise:T (a seq track)")), -1;
    int moved = 0;
    for (Step& st_ : t->step) {
      Step was = st_;
      st_.n = 0;
      for (int k = 0; k < was.n; ++k) {   // (two notes on one pitch now: one kept)
        Note n = was.note[k];
        int q = quantise(n.pitch, pt.key, pt.scale);
        moved += q != n.pitch;
        n.pitch = (uint8_t)q;
        bool dup = false;
        for (int j = 0; j < st_.n; ++j) dup |= st_.note[j].pitch == n.pitch;
        if (!dup) st_.note[st_.n++] = n;
      }
    }
    *track = r;
    return sniprintf(out, cap, CORE_STR("seq: track %d: %d notes moved into %s %s"), r + 1, moved, key_name(pt.key),
                     scale_name(pt.scale)), 1;
  }
  if (!strncmp(p, "seq=rnd:", 8)) {   // seqtop R1: seq=rnd:T:mode=off|steps|track,low=N,oct=1..4,wght=flat|root|chord|low|high|step
    int r = atoi(p + 8) - 1;
    const char* q = strchr(p + 8, ':');
    Track* t = q && r >= 0 && r < kRytmTracks ? track_of(pt, r, true) : nullptr;
    static const char* const kModes[] = {"off", "steps", "track"};
    static const char* const kW[] = {"flat", "root", "chord", "low", "high", "step"};
    bool ok = t != nullptr;
    int md = t ? t->rnd_mode : 0, lo = t ? t->rnd_low : 0, oc = t ? t->rnd_oct : 0, wg = t ? t->rnd_wght : 0;
    char k[12], val[16];
    for (q = q ? q + 1 : nullptr; ok && kv_next(q, k, sizeof k, val, sizeof val);) {
      int x;
      if (!strcmp(k, "mode") && (x = word_of(val, kModes, kRndModes)) >= 0) md = x;
      else if (!strcmp(k, "low") && val[0] >= '0' && val[0] <= '9' && atoi(val) <= 127) lo = atoi(val);
      else if (!strcmp(k, "oct") && atoi(val) >= 1 && atoi(val) <= 4) oc = atoi(val);
      else if (!strcmp(k, "wght") && (x = word_of(val, kW, kWeights)) >= 0) wg = x;
      else ok = false;
    }
    if (!ok) return sniprintf(out, cap, CORE_STR("seq=rnd:T:mode=off|steps|track,low=0..127,oct=1..4,wght=flat|root|chord|low|high|step")), -1;
    t->rnd_mode = (uint8_t)md, t->rnd_low = (uint8_t)lo, t->rnd_oct = (uint8_t)oc, t->rnd_wght = (uint8_t)wg;
    *track = r;
    return sniprintf(out, cap, CORE_STR("seq: track %d set"), r + 1), 1;
  }
  if (!strncmp(p, "seq=rndstep:", 12)) {   // seqtop R1: seq=rndstep:T:S:0|1 - step S (1..)'s notes random (mode STEPS)
    int r = atoi(p + 12) - 1;
    const char* q = strchr(p + 12, ':');
    const char* q2 = q ? strchr(q + 1, ':') : nullptr;
    int si = q ? atoi(q + 1) - 1 : -1;
    Track* t = q2 && si >= 0 && si < kMaxSteps && r >= 0 && r < kRytmTracks ? track_of(pt, r, true) : nullptr;
    if (!t) return sniprintf(out, cap, CORE_STR("seq=rndstep:T:S:0|1")), -1;
    Step& st_ = t->step[si];
    if (atoi(q2 + 1)) {
      if (!st_.n) add_note(st_, def_note(*t));
      st_.xf |= kXfRnd;
      if (!t->rnd_mode) t->rnd_mode = kRndSteps;   // (a flagged step plays random at once)
    } else {
      st_.xf &= (uint8_t)~kXfRnd;
    }
    *track = r;
    return sniprintf(out, cap, CORE_STR("seq: track %d step %d: notes %s (RND %s)"), r + 1, si + 1, st_.xf & kXfRnd ? "random" : "as written",
                     rnd_mode_name(t->rnd_mode)), 1;
  }
  if (!strncmp(p, "seq=smod:", 9)) {   // seqtop R1b: seq=smod:T:src=0..16,range=2..64,quant=step|loop|bar,aff=notes|chance|order|all
    int r = atoi(p + 9) - 1;
    const char* q = strchr(p + 9, ':');
    Track* t = q && r >= 0 && r < kRytmTracks ? track_of(pt, r, true) : nullptr;
    static const char* const kQ[] = {"step", "loop", "bar"};
    static const char* const kA[] = {"notes", "chance", "order", "all"};
    bool ok = t != nullptr;
    int sr = t ? t->smod_src : 0, rg = t ? t->smod_range : 0, qu = t ? t->smod_quant : 0, af = t ? t->smod_aff : 0;
    char k[12], val[16];
    for (q = q ? q + 1 : nullptr; ok && kv_next(q, k, sizeof k, val, sizeof val);) {
      int x;
      if (!strcmp(k, "src") && val[0] >= '0' && val[0] <= '9' && atoi(val) <= 16) sr = atoi(val);
      else if (!strcmp(k, "range") && atoi(val) >= 2 && atoi(val) <= 64) rg = atoi(val);
      else if (!strcmp(k, "quant") && (x = word_of(val, kQ, kSqs)) >= 0) qu = x;
      else if (!strcmp(k, "aff") && (x = word_of(val, kA, kAffs)) >= 0) af = x;
      else ok = false;
    }
    if (!ok) return sniprintf(out, cap, CORE_STR("seq=smod:T:src=0..16 (P1..P16 of the track's voice, 0 off),range=2..64,quant=step|loop|bar,aff=notes|chance|order|all")), -1;
    t->smod_src = (uint8_t)sr, t->smod_range = (uint8_t)rg, t->smod_quant = (uint8_t)qu, t->smod_aff = (uint8_t)af;
    *track = r;
    return sniprintf(out, cap, CORE_STR("seq: track %d set"), r + 1), 1;
  }
  if (!strncmp(p, "seq=lcond:", 10)) {   // seqtop V1: seq=lcond:T:S:P:C - step S's lock of param P (as seq=lock; all: every
                                         // lock of the step) gets condition C (--- fill !fill 1st !1st A:B N% or the code)
    int r = atoi(p + 10) - 1;
    const char* q = strchr(p + 10, ':');
    const char* q2 = q ? strchr(q + 1, ':') : nullptr;
    const char* q3 = q2 ? strchr(q2 + 1, ':') : nullptr;
    bool all = q2 && !strncmp(q2 + 1, "all:", 4);
    int si = q ? atoi(q + 1) - 1 : -1, prm = all ? -1 : q2 ? atoi(q2 + 1) - 1 : -2, c = q3 ? lock_cond_parse(q3 + 1) : -1;
    Track* t = c >= 0 && si >= 0 && si < kMaxSteps && prm >= -1 && prm < kLockParams && r >= 0 && r < kRytmTracks
                   ? track_of(pt, r, false) : nullptr;
    int n = 0;
    for (int k = 0; t && k < t->step[si].nl; ++k)
      if (all || t->step[si].lock[k].param == prm) t->step[si].lock[k].cond = (uint8_t)c, ++n;
    if (!n) return sniprintf(out, cap, CORE_STR("seq=lcond:T:S:P|all:---|fill|!fill|1st|!1st|A:B|N%% (a lock that is there)")), -1;
    *track = r;
    char b[12];
    return sniprintf(out, cap, CORE_STR("seq: track %d step %d: %d lock%s %s"), r + 1, si + 1, n, n > 1 ? "s" : "", lock_cond_text(c, b, sizeof b)), 1;
  }
  if (!strncmp(p, "seq=fx:", 7)) {   // seqtop H1: seq=fx:T:S:C:CMD:VAL | seq=fx:T:S:C:off (C 1 / 2; CMD RTG DLY CUT CHN RND RNV OCT
                                      // TRS JMP JRN LOP SKP DIR FIL; LOP's VAL NxX, DIR's a direction)
    int r = atoi(p + 7) - 1;
    const char* q = strchr(p + 7, ':');
    const char* q2 = q ? strchr(q + 1, ':') : nullptr;
    const char* q3 = q2 ? strchr(q2 + 1, ':') : nullptr;
    int si = q ? atoi(q + 1) - 1 : -1, col = q2 ? atoi(q2 + 1) - 1 : -1;
    Track* t = q3 && si >= 0 && si < kMaxSteps && (col == 0 || col == 1) && r >= 0 && r < kRytmTracks ? track_of(pt, r, true) : nullptr;
    Fx f = {0, 0};
    bool ok = t != nullptr;
    if (ok && strcmp(q3 + 1, "off")) {
      char c[8] = {0};
      const char* q4 = strchr(q3 + 1, ':');
      int cl = q4 ? (int)(q4 - q3 - 1) : (int)strlen(q3 + 1);
      if (cl > 0 && cl < (int)sizeof c) memcpy(c, q3 + 1, (size_t)cl);
      ok = fx_parse(c, q4 ? q4 + 1 : nullptr, &f) != 0;
    }
    if (!ok) return sniprintf(out, cap, CORE_STR("seq=fx:T:S:1|2:RTG n|DLY n|CUT n|CHN %%|RND r|RNV r|OCT +-n|TRS +-n|JMP s|JRN w|LOP NxX|SKP n|DIR d|FIL p | seq=fx:T:S:1|2:off")), -1;
    t->step[si].fx[col] = f;
    *track = r;
    char b[16];
    return sniprintf(out, cap, CORE_STR("seq: track %d step %d FX%d %s"), r + 1, si + 1, col + 1, fx_text(f, b, sizeof b)), 1;
  }
  if (!strncmp(p, "seq=slide:", 10)) {   // seqtop X1: seq=slide:T:S:P:0|1 - step S's lock of param P (as seq=lock; all: every
                                         // lock of the step) slides
    int r = atoi(p + 10) - 1;
    const char* q = strchr(p + 10, ':');
    const char* q2 = q ? strchr(q + 1, ':') : nullptr;
    const char* q3 = q2 ? strchr(q2 + 1, ':') : nullptr;
    bool all = q2 && !strncmp(q2 + 1, "all:", 4);
    int si = q ? atoi(q + 1) - 1 : -1, prm = all ? -1 : q2 ? atoi(q2 + 1) - 1 : -2;
    Track* t = q3 && si >= 0 && si < kMaxSteps && prm >= -1 && prm < kLockParams && r >= 0 && r < kRytmTracks
                   ? track_of(pt, r, false) : nullptr;
    bool on = q3 && atoi(q3 + 1);
    int n = 0;
    for (int k = 0; t && k < t->step[si].nl; ++k) {
      int x = t->step[si].lock[k].param;
      if (!all && x != prm) continue;
      t->step[si].slide = on ? t->step[si].slide | 1u << x : t->step[si].slide & ~(1u << x);
      ++n;
    }
    if (!n) return sniprintf(out, cap, CORE_STR("seq=slide:T:S:P|all:0|1 (a lock that is there)")), -1;
    *track = r;
    return sniprintf(out, cap, CORE_STR("seq: track %d step %d: %d lock%s %s"), r + 1, si + 1, n, n > 1 ? "s" : "",
                     on ? "slide to the next" : "hold"), 1;
  }

  if (!strncmp(p, "seq=rtrg:", 9)) {   // seqtop P4 / M4: seq=rtrg:T:S:off | seq=rtrg:T:S:cnt=0..16,rate=auto|2|1|2/3|1/2|1/3|
                                        // 1/4|1/6|1/8,vcrv=flat|up|down|updn,pcrv=-24..24,mode=chord|walk|up|down|updn|rnd|
                                        // played|conv,oct=1..4,gate=1..100
    int r = atoi(p + 9) - 1;
    const char* q = strchr(p + 9, ':');
    const char* q2 = q ? strchr(q + 1, ':') : nullptr;
    int si = q ? atoi(q + 1) - 1 : -1;
    Track* t = q2 && si >= 0 && si < kMaxSteps && r >= 0 && r < kRytmTracks ? track_of(pt, r, true) : nullptr;
    static const char* const kRates[] = {"auto", "2", "1", "2/3", "1/2", "1/3", "1/4", "1/6", "1/8"};
    static const char* const kVc[] = {"flat", "up", "down", "updn"};
    static const char* const kModes[] = {"chord", "walk", "up", "down", "updn", "rnd", "played", "conv"};
    bool ok = t != nullptr;
    Rtrg x = t ? t->step[si].rt : Rtrg{};
    if (ok && !strcmp(q2 + 1, "off")) {
      x = Rtrg{};
    } else if (ok) {
      const char* kq = q2 + 1;
      char k[8], val[12];
      while (ok && kv_next(kq, k, sizeof k, val, sizeof val)) {
        int y = -1;
        if (!strcmp(k, "cnt") && val[0] >= '0' && val[0] <= '9' && atoi(val) <= kRtMax) x.cnt = (uint8_t)atoi(val);
        else if (!strcmp(k, "rate")) {   // by name only ("2" is two steps, not the id 2)
          for (int i = 0; i < kRtRates; ++i)
            if (!strcmp(val, kRates[i]) || (i == 0 && !strcmp(val, "AUTO"))) y = i;
          if (y < 0) ok = false;
          else x.rate = (uint8_t)y;
        } else if (!strcmp(k, "vcrv") && (y = word_of(val, kVc, kVcs)) >= 0) x.vcrv = (uint8_t)y;
        else if (!strcmp(k, "pcrv") && atoi(val) >= -24 && atoi(val) <= 24) x.pcrv = (int8_t)atoi(val);
        else if (!strcmp(k, "mode") && (y = word_of(val, kModes, kRtModes)) >= 0) x.mode = (uint8_t)y;
        else if (!strcmp(k, "oct") && atoi(val) >= 1 && atoi(val) <= 4) x.oct = (uint8_t)atoi(val);
        else if (!strcmp(k, "gate") && atoi(val) >= 1 && atoi(val) <= 100) x.gate = (uint8_t)atoi(val);
        else ok = false;
      }
    }
    if (!ok) return sniprintf(out, cap, CORE_STR("seq=rtrg:T:S:off | seq=rtrg:T:S:cnt=0..16,rate=auto|2|1|2/3|1/2|1/3|1/4|1/6|1/8,vcrv=flat|up|down|updn,pcrv=-24..24,mode=chord|walk|up|down|updn|rnd|played|conv,oct=1..4,gate=1..100")), -1;
    Step& st_ = t->step[si];
    if (!st_.n && rt_on(x)) add_note(st_, def_note(*t));
    st_.rt = x;
    *track = r;
    return sniprintf(out, cap, CORE_STR("seq: track %d step %d: %s cnt %d rate %s vel %s pitch %+d oct %d gate %d%%"), r + 1, si + 1,
                     rt_on(x) ? rt_mode_name(x.mode) : "OFF", x.cnt, rt_rate_name(x.rate), vcrv_name(x.vcrv), x.pcrv, x.oct ? x.oct : 1,
                     x.gate ? x.gate : 100), 1;
  }
  return 0;
}

}  // namespace seq
}  // namespace core
#endif
