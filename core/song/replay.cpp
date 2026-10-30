// core/song/replay.cpp - see song.h: the replay scheduler, the confirmed restore and the timeline's lanes. Pure: every
// message goes to a Port (the engine's, or a test's fake Rytm). Cold.
#include "song/song.h"

#if CORE_POLY
#include <string.h>

#include "platform.h"

namespace core {
namespace song {

namespace {
void cc(Port& p, int ch1, int n, int v) {
  uint8_t m[3] = {(uint8_t)(0xB0 | ((ch1 - 1) & 15)), (uint8_t)(n & 127), (uint8_t)(v & 127)};
  p.send(m, 3);
}
int track_channel(const MidiConfig& c, int a) {
  if (a >= 0x40) return (a & 15) + 1;
  if (a == kFxTrack) return c.fx_ch ? c.fx_ch : 13;
  return a < kTracks && c.track_ch[a] ? c.track_ch[a] : a + 1;
}
}  // namespace

CORE_COLD uint32_t pc_time(const Take& t, int sw) {
  const Ev& s = t.ev[sw];
  uint32_t prev = 0;
  int from = 0;
  for (int i = sw - 1; i >= 0; --i)
    if (t.ev[i].type == kSw || t.ev[i].type == kPlay) {
      prev = t.ev[i].t, from = i;
      break;
    }
  for (int i = sw - 1; i > from; --i)
    if (t.ev[i].type == kPc && t.ev[i].a == s.b) return t.ev[i].t > prev ? t.ev[i].t : prev + 1;
  uint32_t lead = 2 * kPpq;
  uint32_t at = s.t > lead ? s.t - lead : 0;
  return at > prev ? at : prev + 1;
}

// ---- Replayer
CORE_COLD void Replayer::begin(const Take* t) {
  take = t, idx = 0, offset = 0, mutes = t ? t->mutes : 0, running = false, pc_sent = false;
  sent = skipped = reanchors = pcs = 0;
  ended = false;
  int64_t stop_t = -1;
  uint32_t last = t ? t->end : 0;   // without a recorded STOP: the take's last tick, or its last event
  for (int i = 0; t && i < t->n; ++i) {
    if (t->ev[i].type == kStop) stop_t = t->ev[i].t;
    if (t->ev[i].t > last) last = t->ev[i].t;
  }
  end_t = stop_t >= 0 ? (uint32_t)stop_t : last;
  next_sw = -1;
  for (int i = 0; t && i < t->n; ++i)
    if (t->ev[i].type == kSw) { next_sw = i; break; }
}

CORE_COLD void Replayer::mute_to(uint32_t mask, Port& p) {
  for (int k = 0; k < kTracks; ++k)
    if ((mask ^ mutes) >> k & 1) cc(p, track_channel(take->cfg, k), 94, mask >> k & 1);
  mutes = mask;
}

CORE_COLD void Replayer::send_event(const Ev& e, Port& p) {
  const MidiConfig& c = take->cfg;
  switch (e.type) {
    case kParam: {
      int ch = track_channel(c, e.a);
      cc(p, ch, 99, e.b >> 7), cc(p, ch, 98, e.b & 127), cc(p, ch, 6, e.v >> 7 & 127), cc(p, ch, 38, e.v & 127);
      break;
    }
    case kCc: cc(p, e.a, e.b, e.v); break;
    case kMute: mute_to(e.v, p); break;
    case kScene: cc(p, c.auto_ch ? c.auto_ch : 14, 92, e.a); break;
    case kPerf: cc(p, c.auto_ch ? c.auto_ch : 14, e.a, e.v); break;
    case kSc: p.sidecar(e); break;
    case kSrc: {   // rui1038: the Rytm's P1 at 14 bits by 48 (as a knob turn: the kit keeps it), not a CC
      uint8_t m[11] = {0xF0, 0x7D, 0x52, 0x54, 0x48, 0x01, (uint8_t)(e.a % kTracks), (uint8_t)(e.b & 15), (uint8_t)(e.v >> 7 & 127),
                       (uint8_t)(e.v & 127), 0xF7};
      p.send(m, 11);
      break;
    }
    case kPc: case kSw: case kPlay: case kStop: case kPause: case kCont: return;   // (the switches: their own PCs)
    default: ++skipped; return;   // tempo, fill, machine, mark: not replayable (docs/SONG.md)
  }
  ++sent;
}

CORE_COLD void Replayer::start(Port& p) {
  running = true;
  idx = 0, offset = 0, pc_sent = false;
  advance(0, p);
}

CORE_COLD void Replayer::advance(uint32_t t_now, Port& p) {
  if (!running || !take) return;
  int64_t rec = (int64_t)t_now + offset;
  if (next_sw >= 0 && !pc_sent && rec >= (int64_t)pc_time(*take, next_sw)) {
    uint8_t m[2] = {(uint8_t)(0xC0 | (((take->cfg.auto_ch ? take->cfg.auto_ch : 14) - 1) & 15)), (uint8_t)(take->ev[next_sw].b & 127)};
    p.send(m, 2);
    pc_sent = true;
    ++pcs;
  }
  while (idx < take->n && (int64_t)take->ev[idx].t <= rec) {
    const Ev& e = take->ev[idx];
    if (e.type == kSw && idx == next_sw && !pc_sent) break;   // (its PC not out yet: wait - the Rytm decides the switch)
    if (e.type == kSw && idx == next_sw) {
      // the Rytm's own SWITCH re-anchors (switched()); a switch that has not come yet holds the events after it, for
      // up to kSwWait ticks (a Rytm without the 2D reports: then on, unanchored)
      if (rec < (int64_t)e.t + kSwWait) break;
      ++idx;
      int nx = -1;
      for (int i = idx; i < take->n; ++i)
        if (take->ev[i].type == kSw) { nx = i; break; }
      next_sw = nx, pc_sent = false;
      continue;
    }
    send_event(e, p);
    ++idx;
  }
  // fix1041: the take's end: every event before it out, the Rytm stopped as the take was
  if (!ended && take->n && rec >= (int64_t)end_t && (idx >= take->n || take->ev[idx].t >= end_t)) {
    ended = true;
    if (send_stop) {
      uint8_t m[1] = {0xFC};
      p.send(m, 1);
    }
  }
}

CORE_COLD void Replayer::switched(int to, uint32_t t_now, Port& p) {
  if (!running || !take || next_sw < 0) return;
  const Ev& s = take->ev[next_sw];
  if ((int)s.b != to) return;   // (a switch the take did not have: the user's; nothing re-anchored)
  int64_t want = s.t;
  if (want - ((int64_t)t_now + offset) != 0) ++reanchors;
  offset = want - (int64_t)t_now;
  // everything before the switch out now, then past it
  while (idx < take->n && (idx < next_sw || (int64_t)take->ev[idx].t < want)) {
    if (idx != next_sw) send_event(take->ev[idx], p);
    ++idx;
  }
  if (idx == next_sw) ++idx;
  int nx = -1;
  for (int i = next_sw + 1; i < take->n; ++i)
    if (take->ev[i].type == kSw) { nx = i; break; }
  next_sw = nx, pc_sent = false;
  advance(t_now, p);
}

CORE_COLD void Replayer::stop() { running = false; }

// ---- Restore
CORE_COLD bool same_dump(const Buf& a, const Buf& b) {
  if (a.n != b.n || a.n < 15) return false;
  return !memcmp(a.p + 10, b.p + 10, a.n - 10);
}

namespace {
void pc(Port& p, const MidiConfig& c, int pattern) {   // a pattern selected (stopped: at once; PROG CH RECEIVE on)
  uint8_t m[2] = {(uint8_t)(0xC0 | (((c.auto_ch ? c.auto_ch : 14) - 1) & 15)), (uint8_t)(pattern & 127)};
  p.send(m, 2);
}
void mutes_to(Port& p, const MidiConfig& c, uint32_t mask) {   // every track's mute (CC 94 on its channel)
  for (int k = 0; k < kTracks; ++k) cc(p, c.track_ch[k] ? c.track_ch[k] : k + 1, 94, (int)(mask >> k & 1));
}
bool whole(const Buf& d, int id) {
  uint8_t got;
  return d.n >= 15 && dump_ok(d.p, d.n, &got) && got == id;
}
}  // namespace

CORE_COLD void Restore::begin(const Take* t, bool settings, uint32_t ms, Port& p, int current) {
  take = t, with_settings = settings, from = current, selected = false, wrote = false, partial = false;
  backup.clear(), relinked.clear();
  for (int k = 0; k < kUndoMax - 1; ++k) extra[k].clear();
  backup.pattern = current;
  verify_kit = verify_pat = false;
  msg[0] = 0;
  if (!t || !t->kit.n || !t->pat.n) {
    st = kFailed;
    sniprintf(msg, sizeof msg, "the take has no start snapshot: nothing written");
    return;
  }
  // the patterns the take plays besides the one selected: its start, then its switches' (distinct, in order)
  int all[128 + 1], na = 0;
  auto add = [&](int v) {
    if (v < 0 || v > 127 || v == current) return;
    for (int k = 0; k < na; ++k)
      if (all[k] == v) return;
    all[na++] = v;
  };
  add(t->pattern);
  for (int i = 0; i < t->n; ++i)
    if (t->ev[i].type == kSw) add(t->ev[i].b & 127);
  plays = na + 1;
  partial = plays > kUndoMax;
  nvisit = partial ? (t->pattern >= 0 && t->pattern != current ? 1 : 0) : na;
  for (int k = 0; k < nvisit; ++k) visit[k] = all[k], extra[k].pattern = all[k];
  vi = 0;
  st = kBackupReq, at = ms, want = kReqKit, tries = 1;
  p.request(kReqKit);
}

// failed before anything was written: the backup phase's PCs undone
CORE_COLD void Restore::fail_unwritten(const char* why, Port& p) {
  st = kFailed, want = -1;
  if (selected && from >= 0) pc(p, take->cfg, from);
  sniprintf(msg, sizeof msg, "%s: nothing written%s", why, selected && from < 0 ? " (pattern changed)" : "");
}

CORE_COLD void Restore::to_write(uint32_t ms, Port& p) {
  want = -1;
  st = kBackupWrite, at = ms;
  const Take* more[kUndoMax - 1];
  for (int k = 0; k < nvisit; ++k) more[k] = &extra[k];
  if (!p.backup(backup, more, nvisit, partial)) fail_unwritten("the backup could not be written", p);
}

// the next pattern to back up selected (stopped: at once), or the backup written
CORE_COLD void Restore::next_visit(uint32_t ms, Port& p) {
  want = -1, at = ms;
  if (vi >= nvisit) { to_write(ms, p); return; }
  pc(p, take->cfg, visit[vi]);
  selected = true;
  st = kBackupSel;
}

CORE_COLD void Restore::dump_in(uint8_t id, const uint8_t* m, int n, uint32_t ms, Port& p) {
  if (want < 0 || id != (uint8_t)(want - 0x10)) return;
  Buf* b = nullptr;
  if (st == kBackupReq) b = want == kReqKit ? &backup.kit : want == kReqPattern ? &backup.pat : &backup.settings;
  if (st == kBackupReq1) b = want == kReqKit ? &extra[vi].kit : &extra[vi].pat;
  if (st == kRelink) b = &relinked;
  if (st == kVerify) b = want == kReqKit ? &vkit : &vpat;
  if (!b) return;
  b->clear();
  b->add(m, n);
  at = ms;
  if (st == kBackupReq) {
    if (want == kReqKit) want = kReqPattern, tries = 1, p.request(kReqPattern);
    else if (want == kReqPattern) want = kReqSettings, tries = 1, p.request(kReqSettings);
    else next_visit(ms, p);
    return;
  }
  if (st == kBackupReq1) {
    if (want == kReqKit) want = kReqPattern, tries = 1, p.request(kReqPattern);
    else ++vi, next_visit(ms, p);
    return;
  }
  if (st == kRelink) {   // the kit the written pattern X selects: the one backed up for the take's start, or another
    want = -1;
    int k = 0;   // (the entry of the take's start: P0 when it is the one selected)
    for (int j = 0; j < nvisit; ++j)
      if (visit[j] == take->pattern) k = j + 1;
    const Buf& was = k ? extra[k - 1].kit : backup.kit;
    if (same_dump(relinked, was)) {
      st = kPattern;
      return;
    }
    st = kRelinkWrite;
    if (!p.relink(k, relinked)) {
      st = kFailed;
      sniprintf(msg, sizeof msg, "the linked kit's backup could not be written: stopped part way");
    }
    return;
  }
  // verify: kit first, then the pattern
  if (want == kReqKit) {
    verify_kit = same_dump(vkit, take->kit);
    want = kReqPattern, tries = 1, p.request(kReqPattern);
    return;
  }
  verify_pat = same_dump(vpat, take->pat);
  want = -1;
  st = verify_kit && verify_pat ? kDone : kFailed;
  if (verify_kit && verify_pat)
    sniprintf(msg, sizeof msg, "restored (kit and pattern verified)");
  else
    sniprintf(msg, sizeof msg, "restore not verified: kit %s, pattern %s", verify_kit ? "ok" : "differs", verify_pat ? "ok" : "differs");
}

CORE_COLD void Restore::backup_done(bool ok, uint32_t ms, Port& p) {
  if (st != kBackupWrite) return;   // (cancelled, timed out: a late result changes nothing)
  if (!ok) {
    fail_unwritten("the backup could not be written", p);
    return;
  }
  st = kSelect, at = ms;
  // the take's start pattern (not again when the backup's last PC was to it)
  if (take->pattern >= 0 && !(nvisit && visit[nvisit - 1] == take->pattern)) pc(p, take->cfg, take->pattern), selected = true;
}

CORE_COLD void Restore::relink_done(bool ok, uint32_t ms, Port& p) {
  if (st != kRelinkWrite) return;
  if (!ok) {
    st = kFailed;
    sniprintf(msg, sizeof msg, "the linked kit's backup could not be written: stopped part way");
    return;
  }
  st = kPattern, at = ms;
}

CORE_COLD bool Restore::cancel(Port& p) {
  if (!busy()) return true;
  if (wrote || st > kSelect) return false;
  fail_unwritten("cancelled", p);
  return true;
}

CORE_COLD void Restore::tick(uint32_t ms, Port& p) {
  if (!busy()) return;
  if (st != kVerify && p.playing()) {   // (a PLAY: the writes would land in the playing pattern, the reads be of it)
    if (wrote) st = kFailed, want = -1, sniprintf(msg, sizeof msg, "the Rytm played: the restore stopped part way");
    else fail_unwritten("the Rytm played", p);
    return;
  }
  if (want >= 0) {
    if (ms - at > kRetryMs * (uint32_t)tries && tries < kTries && ms - at <= kTimeoutMs) {   // asked again
      ++tries;
      p.request((uint8_t)want);
      return;
    }
    if (ms - at > kTimeoutMs) {
      if (st == kVerify) st = kFailed, want = -1, sniprintf(msg, sizeof msg, "no answer from the Rytm (verify)");
      else if (wrote) st = kFailed, want = -1, sniprintf(msg, sizeof msg, "no answer from the Rytm: stopped part way");
      else fail_unwritten("no answer from the Rytm (backup)", p);
    }
    return;
  }
  if (st == kBackupWrite || st == kRelinkWrite) {   // the files on the card (fmt's queue): bounded
    if (ms - at <= kWriteTimeoutMs) return;
    if (st == kBackupWrite) fail_unwritten("the backup is not on the card after 10 s", p);
    else st = kFailed, sniprintf(msg, sizeof msg, "the linked kit's backup is not on the card: stopped part way");
    return;
  }
  if (ms - at < (st == kSelect || st == kBackupSel ? 4 * kGapMs : kGapMs)) return;
  at = ms;
  switch (st) {
    case kBackupSel: st = kBackupReq1, want = kReqKit, tries = 1, p.request(kReqKit); break;
    case kSelect:   // pattern X, then the kit it selects read (relink) before kit X goes over it
      p.send(take->pat.p, take->pat.n), wrote = true;
      st = kRelink, want = kReqKit, tries = 1, p.request(kReqKit);
      break;
    case kPattern: p.send(take->kit.p, take->kit.n), st = kKit; break;
    case kKit:
      if (with_settings && take->settings.n) p.send(take->settings.p, take->settings.n);
      st = kSettings;
      break;
    case kSettings:
      mutes_to(p, take->cfg, take->mutes);   // every track's mute to the take's start
      if (take->has_fx) p.sidecar_start(*take);
      st = kMutes;
      break;
    case kMutes: st = kVerify, want = kReqKit, tries = 1, p.request(kReqKit); break;
    default: break;
  }
}

// ---- UndoRun
CORE_COLD void UndoRun::begin(const Take* const* entries, int count, bool settings, int32_t mute_mask, uint32_t ms, Port& p) {
  n = count < kUndoMax ? count : kUndoMax, with_settings = settings, mutes = mute_mask;
  want = -1, msg[0] = 0, wrote = false;
  bool ok = n > 0;
  for (int k = 0; k < n && ok; ++k) {
    const Take* e = list[k] = entries[k];
    ok = e && whole(e->kit, kReqKit - 0x10) && (e->pat.n ? whole(e->pat, kReqPattern - 0x10) : k > 0) &&
         (!e->kit2.n || whole(e->kit2, kReqKit - 0x10)) &&
         (k || !settings || whole(e->settings, kReqSettings - 0x10)) && (e->pattern >= 0 || n == 1);
  }
  if (!ok) {
    st = kFailed;
    sniprintf(msg, sizeof msg, "the backup is not whole: nothing written");
    return;
  }
  i = n - 1;
  select(ms, p);
}

CORE_COLD void UndoRun::select(uint32_t ms, Port& p) {
  if (list[i]->pattern >= 0) pc(p, list[i]->cfg, list[i]->pattern);
  st = kSelect, at = ms;
}

CORE_COLD void UndoRun::tick(uint32_t ms, Port& p) {
  if (!busy()) return;
  if (st != kVerify && p.playing()) {
    st = kFailed, want = -1;
    sniprintf(msg, sizeof msg, wrote ? "the Rytm played: the undo stopped part way" : "the Rytm played: nothing written");
    return;
  }
  if (want >= 0) {
    if (ms - at > Restore::kRetryMs * (uint32_t)tries && tries < Restore::kTries && ms - at <= Restore::kTimeoutMs) {
      ++tries;
      p.request((uint8_t)want);
    } else if (ms - at > Restore::kTimeoutMs) {
      st = kFailed, want = -1;
      sniprintf(msg, sizeof msg, "no answer from the Rytm (verify)");
    }
    return;
  }
  if (ms - at < (st == kSelect ? 4 * Restore::kGapMs : Restore::kGapMs)) return;
  at = ms;
  const Take& e = *list[i];
  switch (st) {
    case kSelect:
      if (e.kit2.n) {   // the kit the replay's pattern X links: first, while that pattern X is still the one there
        p.send(e.kit2.p, e.kit2.n), wrote = true;
        st = kKit2;
        break;
      }
      // fall through
    case kVerify2:
      if (e.pat.n) p.send(e.pat.p, e.pat.n), wrote = true;
      st = kPattern;
      break;
    case kKit2: st = kVerify2, want = kReqKit, tries = 1, p.request(kReqKit); break;
    case kPattern: p.send(e.kit.p, e.kit.n), st = kKit, wrote = true; break;
    case kKit:
      if (!i && with_settings) p.send(e.settings.p, e.settings.n);
      st = kSettings;
      break;
    case kSettings:
      if (!i && mutes >= 0) mutes_to(p, e.cfg, (uint32_t)mutes);
      if (!i && e.has_fx) p.sidecar_start(e);
      st = kMutes;
      break;
    case kMutes: st = kVerify, want = kReqKit, tries = 1, p.request(kReqKit); break;
    default: break;
  }
}

CORE_COLD void UndoRun::dump_in(uint8_t id, const uint8_t* m, int len, uint32_t ms, Port& p) {
  if ((st != kVerify && st != kVerify2) || want < 0 || id != (uint8_t)(want - 0x10)) return;
  const Take& e = *list[i];
  if (st == kVerify2) {   // kit2 verified: then on to its pattern X (tick)
    vkit.clear(), vkit.add(m, len);
    want = -1, at = ms;
    if (!same_dump(vkit, e.kit2)) {
      st = kFailed;
      int pat = e.pattern < 0 ? 0 : e.pattern;
      sniprintf(msg, sizeof msg, "undo not verified: %c%02d's linked kit differs", 'A' + pat / 16, pat % 16 + 1);
    }
    return;
  }
  Buf& b = want == kReqKit ? vkit : vpat;
  b.clear();
  b.add(m, len);
  at = ms;
  bool ok = same_dump(b, want == kReqKit ? e.kit : e.pat);
  if (ok && want == kReqKit && e.pat.n) {
    want = kReqPattern, tries = 1, p.request(kReqPattern);
    return;
  }
  want = -1;
  int pat = e.pattern < 0 ? 0 : e.pattern;
  if (!ok) {
    st = kFailed;
    sniprintf(msg, sizeof msg, "undo not verified: %c%02d's %s differs", 'A' + pat / 16, pat % 16 + 1,
              b.p == vkit.p ? "kit" : "pattern");
    return;
  }
  if (i > 0) {
    --i;
    select(ms, p);
    return;
  }
  st = kDone;
  sniprintf(msg, sizeof msg, "replay undone (%d pattern%s: kits and patterns verified)", n, n > 1 ? "s" : "");
}

// ---- lanes
namespace {
const char* const kTrk[13] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB", "FX"};
}

// NRPN MSB 1 (a track's sound): LSB 0..7 SRC A..H, 8..15 SMPL, 16..23 FLTR, 24..31 AMP, 32..39 LFO [measured: 1/0 LEV,
// 1/1 TUN, 1/24 AMP ATK, 1/29 REV; the page blocks inferred], 100 LEVEL; MSB 2 the FX track's, MSB 3 TRIG
CORE_COLD const char* param_label(int trk, int msb, int lsb, char* b, int cap) {
  const char* tn = trk >= 0 && trk <= kFxTrack ? kTrk[trk] : "CH";
  static const char* const kPg[5] = {"SRC", "SMP", "FLT", "AMP", "LFO"};
  if (msb == 1 && lsb == 0) sniprintf(b, cap, "%s LEV", tn);
  else if (msb == 1 && lsb == 1) sniprintf(b, cap, "%s TUN", tn);
  else if (msb == 1 && lsb == 100) sniprintf(b, cap, "%s LVL", tn);
  else if (msb == 1 && lsb < 40) sniprintf(b, cap, "%s %s %c", tn, kPg[lsb / 8], 'A' + lsb % 8);   // "LT SRC C" (UX review T12)
  else if (msb == 3) sniprintf(b, cap, "%s TRG%d", tn, lsb);
  else sniprintf(b, cap, "%s %d.%d", tn, msb, lsb);
  return b;
}

CORE_COLD void build_lanes(const Take& t, Lanes* o) {
  memset(o, 0, sizeof *o);
  uint32_t len = t.end;
  for (int i = 0; i < t.n; ++i) len = t.ev[i].t > len ? t.ev[i].t : len;
  o->length = len;
  // patterns: from the start pattern, a block per switch
  int pat = t.pattern < 0 ? 0 : t.pattern;
  uint32_t t0 = 0;
  uint32_t mute = t.mutes, mute_at[kTracks] = {};
  int scene = t.scene;
  uint32_t scene_at = 0;
  auto blk = [&](Block* arr, int* n, int max, uint32_t a, uint32_t b, int v) {
    if (*n < max && b >= a) arr[(*n)++] = {a, b, (uint8_t)v};
  };
  for (int k = 0; k < kTracks; ++k) mute_at[k] = 0;
  // the parameter lanes: the keys that moved, the most active first
  struct K { uint32_t key; int count; };
  K keys[64];
  int nk = 0;
  for (int i = 0; i < t.n; ++i) {
    const Ev& e = t.ev[i];
    if (e.type == kSw) {
      blk(o->blocks, &o->nblocks, kLaneBlocks, t0, e.t, pat);
      pat = e.b, t0 = e.t;
    } else if (e.type == kMute) {
      for (int k = 0; k < kTracks; ++k) {
        bool was = mute >> k & 1, now = e.v >> k & 1;
        if (was && !now) blk(o->mutes, &o->nmutes, kLaneMutes, mute_at[k], e.t, k);
        if (!was && now) mute_at[k] = e.t;
      }
      mute = e.v;
    } else if (e.type == kScene) {
      if (scene) blk(o->scenes, &o->nscenes, kLaneScenes, scene_at, e.t, scene);
      scene = e.a, scene_at = e.t;
    } else if (e.type == kTempo && o->ntempo < kLaneTempo) {
      o->tempo[o->ntempo++] = {e.t, (uint16_t)(e.v / 12)};
    } else if (e.type == kMark && o->nmarks < 16) {
      o->marks[o->nmarks++] = e.t;
    } else if (e.type == kParam || e.type == kCc || e.type == kSc || e.type == kSrc) {
      uint32_t key = (uint32_t)e.type << 24 | (uint32_t)e.a << 16 | e.b;
      int j = 0;
      while (j < nk && keys[j].key != key) ++j;
      if (j == nk && nk < 64) keys[nk++] = {key, 0};
      if (j < nk) keys[j].count++;
    }
  }
  blk(o->blocks, &o->nblocks, kLaneBlocks, t0, len, pat);
  for (int k = 0; k < kTracks; ++k)
    if (mute >> k & 1) blk(o->mutes, &o->nmutes, kLaneMutes, mute_at[k], len, k);
  if (scene) blk(o->scenes, &o->nscenes, kLaneScenes, scene_at, len, scene);
  // the busiest kLanes keys (a stable order: by first appearance among the chosen)
  bool chosen[64] = {};
  for (int c = 0; c < kLanes && c < nk; ++c) {
    int best = -1;
    for (int j = 0; j < nk; ++j)
      if (!chosen[j] && (best < 0 || keys[j].count > keys[best].count)) best = j;
    chosen[best] = true;
  }
  for (int j = 0; j < nk && o->nlanes < kLanes; ++j) {
    if (!chosen[j]) continue;
    Lane& L = o->lane[o->nlanes++];
    uint32_t key = keys[j].key;
    int type = key >> 24, a = key >> 16 & 0xFF, b = key & 0xFFFF;
    if (type == kParam) param_label(a >= 0x40 ? -1 : a, b >> 7, b & 127, L.label, sizeof L.label);
    else if (type == kCc) sniprintf(L.label, sizeof L.label, "CH%d CC%d", a, b);
    else sniprintf(L.label, sizeof L.label, a == kScSend ? "SC SND%d" : a == kScKnob ? "SC FX%d" : "SC RT%d", b), L.sidecar = 1;
    // the points (decimated evenly when more than fit; values to 14 bits)
    int total = keys[j].count, step = (total + kLanePoints - 1) / kLanePoints, seen = 0;
    if (step < 1) step = 1;
    for (int i = 0; i < t.n && L.n < kLanePoints; ++i) {
      const Ev& e = t.ev[i];
      if (((uint32_t)e.type << 24 | (uint32_t)e.a << 16 | e.b) != key) continue;
      if (seen++ % step && seen != total) continue;
      uint32_t v = type == kParam ? e.v : e.v << 7;
      L.p[L.n++] = {e.t, (uint16_t)(v > 16383 ? 16383 : v)};
    }
  }
  // PERF: all its CCs as one curve
  for (int i = 0; i < t.n && o->nperf < kLanePoints; ++i)
    if (t.ev[i].type == kPerf) o->perf[o->nperf++] = {t.ev[i].t, (uint16_t)(t.ev[i].v << 7)};
}

}  // namespace song
}  // namespace core
#endif
