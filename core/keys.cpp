// core/keys.cpp - see keys.h. Per message: cold.
#include "keys.h"

#include "platform.h"

#if CORE_POLY
#include <stdlib.h>
#include <string.h>

#include "fmt/fmt.h"
#include "hooks.h"
#include "machines.h"
#if CORE_MSAMP
#include "nmach.h"
#endif
#include "poly.h"
#include "seq/seq.h"
#include "ui/remote.h"

namespace core {
namespace keys {
namespace {

const char* const kTrackNames[12] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
const char kFile[] = "/.rytm/keys.txt";
constexpr int kMaxKeys = 32;   // held keys a source

struct Key {
  bool used;
  uint8_t ch, note, vel;
  int8_t vi;        // the Sidecar voice playing it, -1 none
  int8_t track;     // its target track, -1 unknown
  int8_t fwd;       // the channel it went to the Rytm on (its note-off goes there too), -1 not forwarded
  bool probe;       // forwarded to learn the selected track: started on the voice the Rytm's 0x04 names
  bool sus;         // released while the pedal was down: ends at the pedal's release
};
struct Src {
  Key k[kMaxKeys];
  bool sustain;
  bool present;
  char name[16];
  // the gesture (a chord: note-ons kGestureMs apart at most) on a Sidecar voice
  bool open, rec;
  int8_t g_vi, g_track;
  uint32_t g_last;
  uint8_t g_notes[poly::kMaxNotes], g_n;
};
struct Echo {
  bool used;
  int8_t vi;        // the voice the 0x04 is expected on, -1 any (learning)
  bool probe;
  uint32_t ms;
};

Src srcs[kSrcs];
Echo echoes[16];
int echo_w = 0;
uint32_t guard_ms[kVoices];   // a forwarded key's release on a Sidecar voice: the Rytm's 0x04s there swallowed kEchoMs after
void (*fwd_fn)(uint8_t, uint8_t, uint8_t) = nullptr;
Mode mode = kSelected;
int auto_ch = kAutoCh;
bool mac_on = true, dirty = false;
int learned = -1;
uint32_t last_key_ms = 0;
int last_src_track = -1;
View v_last = {-1, "", -1, -1, kNone, 0, 0, kSelected};
Stats st;

CORE_COLD int kc(int src, int ch) { return poly::kKeysCh0 + src * 16 + ch; }

CORE_COLD void forward(uint8_t s, uint8_t d1, uint8_t d2) {
  if (fwd_fn) fwd_fn(s, d1, d2);
  else ++st.dropped;
}

// The Sidecar machines that play poly notes from a keyboard (poly::plays_poly): 6-OP FM, MULTISAMPLE, WAVETABLE,
// GRANULAR (integ-0.3; JUNO is mono in the Sidecar: its notes go to the Rytm)
CORE_COLD bool forced_poly(int vi) {
#if CORE_MSAMP
  return nm::forced(vi) != 0;
#else
  (void)vi;
  return false;
#endif
}
CORE_COLD bool poly_machine(int vb) {
  int m = vb & 0x78;
  return m == kFm || m == kMultiMachine || m == kWaveMachine || m == kGranMachine || m == kChopMachine;
}

// The track on voice vi the Rytm's 33 says plays a Sidecar machine (a poly machine first), else its first track
CORE_COLD int track_on_voice(int vi) {
  int first = -1, ours = -1;
  for (int t = 0; t < 12; ++t) {
    if (ui::remote_track_voice(t) != vi) continue;
    if (first < 0) first = t;
    int vb = ui::remote_track_machine(t);
    if (vb >= 0 && vb != 0x7F && poly_machine(vb)) return t;
    if (vb >= 0 && vb != 0x7F && ours < 0) ours = t;
  }
  return ours >= 0 ? ours : first;
}

// Track t plays the Sidecar's poly notes: its machine (the Rytm's 33, when it sent one) is a poly machine (or the voice
// is forced to one: wt= / gr= / msamp=voice:V) and its voice plays it now
CORE_COLD bool poly_track(int t, int* vi) {
  *vi = ui::remote_track_voice(t);
  if (*vi < 0) return false;
  int vb = ui::remote_track_machine(t);
  if (vb == 0x7F) return false;   // an analog track (a shared voice's other track may be ours)
  if (vb >= 0 && !poly_machine(vb) && !forced_poly(*vi)) return false;
  return poly::plays_poly(*vi);
}

struct Target {
  int track;     // -1 unknown (selected, not known yet) or none (channel 13..16)
  int vi;        // the Sidecar voice, -1 none
  int fwd;       // the Rytm's channel for it
  bool by_channel;
};
CORE_COLD Target target(int src, int ch) {
  Target r = {-1, -1, ch, false};
  r.by_channel = mode == kChannel || (src == kComputer && ch != auto_ch - 1);
  if (r.by_channel) {
    if (ch < 12) r.track = ch;
  } else {
    r.fwd = auto_ch - 1;
    r.track = selected();
  }
  int vi;
  if (r.track >= 0 && poly_track(r.track, &vi)) r.vi = vi;
  return r;
}

CORE_COLD Key* find(Src& s, int ch, int note) {
  for (Key& k : s.k)
    if (k.used && k.ch == ch && k.note == note) return &k;
  return nullptr;
}

CORE_COLD void release(int src, Key& k) {
  if (k.vi >= 0) {
    poly::key_off(k.vi, k.note, kc(src, k.ch));
    seq::note(k.vi, false, k.note, 0, k.track);
  }
  if (k.fwd >= 0) {
    forward((uint8_t)(0x80 | k.fwd), k.note, 0);
    if (k.vi >= 0) guard_ms[k.vi] = now_ms() | 1;
  }
  k.used = false;
}

CORE_COLD int held_count() {
  int n = 0;
  for (Src& s : srcs)
    for (Key& k : s.k) n += k.used;
  return n;
}

// The chord view and the gesture: the source's held notes on voice vi
CORE_COLD void view_held(Src& s, int vi) {
  uint8_t ns[16];
  int n = 0;
  for (Key& k : s.k)
    if (k.used && k.vi == vi && !k.sus && n < 16) ns[n++] = k.note;
  if (n) poly::keys_view(vi, ns, n);
}

CORE_COLD void close_gesture(Src& s) {
  s.open = false;
  if (s.g_n >= 2 && s.g_vi >= 0) {
    poly::Detected d = poly::keys_view(s.g_vi, s.g_notes, s.g_n);
    if (s.rec && s.g_track >= 0) {   // the Rytm recorded the gesture's first note: the chord into that step
      poly::Chord w = d.c;
      uint8_t back[poly::kMaxNotes];
      if (w.chd) w.voi = poly::expand(d.root, d.c, back);
      uint8_t m[16];
      int len = poly::sysex_chord_write(m, s.g_track, d.root, w);
      to_rytm(m, len);
      ++st.writes;
      log(CORE_STR("keys: chord on track %s: root %d %s, %d notes -> 28 write-back"), kTrackNames[s.g_track], d.root,
          poly::kChordNames[w.chd], s.g_n);
    }
  }
  s.g_n = 0;
  s.rec = false;
}

CORE_COLD void note_on(int src, int ch, int note, int vel) {
  Src& s = srcs[src];
  uint32_t ms = now_ms();
  if (Key* old = find(s, ch, note)) release(src, *old);   // the same key again (a sustained one too): retriggered
  Key* k = nullptr;
  for (Key& x : s.k)
    if (!x.used) {
      k = &x;
      break;
    }
  if (!k) {   // every slot held: the oldest-looking one (slot 0) gives way
    release(src, s.k[0]);
    k = &s.k[0];
  }
  *k = {true, (uint8_t)ch, (uint8_t)note, (uint8_t)vel, -1, -1, -1, false, false};
  ++st.notes;
  last_key_ms = ms;
  Target t = target(src, ch);
  k->track = (int8_t)t.track;
  v_last.src = src, v_last.name = s.name, v_last.track = t.track;
  if (t.vi >= 0) {   // a Sidecar poly track: its voice plays the note, the Rytm does not
    k->vi = (int8_t)t.vi;
    poly::key_on(t.vi, note, vel, kc(src, ch));
    ++st.poly;
    v_last.voice = t.vi, v_last.route = kPoly;
    if (!s.open || s.g_vi != t.vi || ms - s.g_last > poly::kGestureMs) {
      if (s.open) close_gesture(s);
      s.open = true, s.g_vi = (int8_t)t.vi, s.g_track = (int8_t)t.track, s.g_n = 0;
      // the Rytm records live on it (not a seq track): its first note goes to the Rytm too, so it records a trig
      s.rec = seq::rytm_live_rec() && !seq::owns_track(t.track) && fwd_fn;
      if (s.rec) {
        k->fwd = (int8_t)t.fwd;
        forward((uint8_t)(0x90 | t.fwd), note, vel);
        ++st.forwarded;
        echoes[echo_w] = {false, (int8_t)t.vi, false, ms};
        echo_w = (echo_w + 1) & 15;
      }
    }
    s.g_last = ms;
    if (s.g_n < poly::kMaxNotes) s.g_notes[s.g_n++] = (uint8_t)note;
    view_held(s, t.vi);
    seq::note(t.vi, true, note, vel, t.track);
    return;
  }
  // the Rytm plays it (an analog track, a mono Sidecar machine, or the selected track not known yet: learning)
  k->fwd = (int8_t)t.fwd;
  forward((uint8_t)(0x90 | t.fwd), note, vel);
  ++st.forwarded;
  v_last.voice = -1;
  v_last.route = kRytm;
  if (!t.by_channel && t.track < 0) {
    k->probe = true;
    ++st.probes;
    v_last.route = kProbe;
    echoes[echo_w] = {false, -1, true, ms};
    echo_w = (echo_w + 1) & 15;
  }
}

CORE_COLD void note_off(int src, int ch, int note) {
  Src& s = srcs[src];
  Key* k = find(s, ch, note);
  if (!k || k->sus) return;
  if (s.sustain) {
    k->sus = true;
    ++st.sustained;
    return;
  }
  release(src, *k);
}

CORE_COLD void learn(int vi) {
  int t = track_on_voice(vi);
  if (t < 0) return;
  if (learned != t) log(CORE_STR("keys: the Rytm's selected track is %s (its trig on voice %d)"), kTrackNames[t], vi);
  learned = t;
  ++st.learned;
  int pv;
  if (!poly_track(t, &pv) || pv != vi) return;
  // the keys forwarded to learn it: on the voice now (their note-offs still go to the Rytm)
  for (int src = 0; src < kSrcs; ++src)
    for (Key& k : srcs[src].k)
      if (k.used && k.probe && k.vi < 0) {
        k.vi = (int8_t)vi, k.track = (int8_t)t, k.probe = false;
        poly::key_on(vi, k.note, k.vel, kc(src, k.ch));
        seq::note(vi, true, k.note, k.vel, t);
        ++st.poly;
        v_last.voice = vi, v_last.track = t, v_last.route = kPoly;
      }
}

}  // namespace

CORE_COLD void set_forward(void (*f)(uint8_t, uint8_t, uint8_t)) { fwd_fn = f; }
CORE_COLD bool computer_on() { return mac_on; }

CORE_COLD int auto_channel() { return auto_ch; }
CORE_COLD int selected() {
  int t = ui::remote_src_track();
  if (t >= 0 && t < 12) return t;
  t = seq::seqmode_on();
  if (t >= 0) return t;
  return learned;
}

CORE_COLD void midi(int src, uint8_t status, uint8_t d1, uint8_t d2) {
  if (src < 0 || src >= kSrcs || status < 0x80 || status >= 0xF0) return;
  if (src == kComputer && !mac_on) return;
  Src& s = srcs[src];
  if (!s.present) {
    s.present = true;
    if (!s.name[0]) strcpy(s.name, src == kComputer ? "COMPUTER" : "USB");
  }
  int kind = status & 0xF0, ch = status & 0x0F;
  d1 &= 0x7F, d2 &= 0x7F;
  if (kind == 0x90 && d2) return note_on(src, ch, d1, d2);
  if (kind == 0x80 || kind == 0x90) return note_off(src, ch, d1);
  if (kind == 0xB0 && d1 == 64) {   // sustain: the held-back note-offs at its release
    bool down = d2 >= 64;
    if (s.sustain && !down)
      for (Key& k : s.k)
        if (k.used && k.sus) release(src, k);
    s.sustain = down;
    return;
  }
  if (kind == 0xB0 && (d1 == 120 || d1 == 123)) {   // all sound / notes off: the channel's keys now
    for (Key& k : s.k)
      if (k.used && k.ch == ch) release(src, k);
  }
  // the rest (CCs, pitch bend, aftertouch, program changes): the Rytm's, on the routed channel
  forward((uint8_t)(kind | target(src, ch).fwd), d1, d2);
}

CORE_COLD void tick(uint32_t ms) {
  for (Src& s : srcs)
    if (s.open && ms - s.g_last > poly::kGestureMs) close_gesture(s);
  int sv = ui::remote_src_track();
  if (sv != last_src_track) last_src_track = sv, learned = -1;   // the SRC page says again (or stopped saying)
  if (learned >= 0 && ms - last_key_ms > kLearnMs && !held_count()) learned = -1;
}

CORE_COLD bool take_trig(int vi, int note) {
  (void)note;
  if (vi < 0 || vi >= kVoices) return false;
  uint32_t ms = now_ms();
  for (Echo& e : echoes) {
    if (e.used || !e.ms || ms - e.ms > kEchoMs || (e.vi >= 0 && e.vi != vi)) continue;
    e.used = true;
    if (e.probe) {
      learn(vi);
      int pv;
      if (learned < 0 || !poly_track(learned, &pv) || pv != vi) return false;   // the Rytm's own voice plays it (mono)
    }
    ++st.swallowed;
    return true;
  }
  // a key the Rytm holds too (forwarded) on this voice: its later trigs (a mono retrigger at a release) are not ours to play
  bool held = guard_ms[vi] && ms - guard_ms[vi] <= kEchoMs;
  for (int src = 0; src < kSrcs && !held; ++src)
    for (Key& k : srcs[src].k)
      if (k.used && k.fwd >= 0 && k.vi == vi) held = true;
  if (held) ++st.swallowed;
  return held;
}

CORE_COLD void source(int src, bool present, const char* name) {
  if (src < 0 || src >= kSrcs) return;
  Src& s = srcs[src];
  if (!present)
    for (Key& k : s.k)
      if (k.used) release(src, k);
  s.present = present;
  s.sustain = false;
  s.name[0] = 0;
  if (name) {   // the Rytm's display font: upper case, no spaces (8 characters shown)
    int n = 0;
    for (const char* p = name; *p && n < (int)sizeof s.name - 1; ++p)
      if (*p != ' ') s.name[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    s.name[n] = 0;
  }
  log(CORE_STR("keys: %s %s %s"), src == kComputer ? "the computer" : "keyboard", s.name, present ? "connected" : "gone");
}

CORE_COLD View view() {
  View v = v_last;
  v.held = held_count();
  v.present = 0;
  for (int i = 0; i < kSrcs; ++i) v.present |= srcs[i].present << i;
  v.mode = mode;
  return v;
}

CORE_COLD const char* line(char* b, int cap) {
  View v = view();
  if (!v.present) return nullptr;
  if (v.src < 0) {
    sniprintf(b, cap, CORE_STR("KEYS %s"), mode == kChannel ? "BY CHANNEL" : "> SELECTED");
    return b;
  }
  const char* r = v.route == kPoly ? "POLY" : v.route == kProbe ? "RYTM ?" : "RYTM";
  if (v.track >= 0) sniprintf(b, cap, CORE_STR("KEYS %.8s > %s %s"), v.name, kTrackNames[v.track], r);
  else sniprintf(b, cap, CORE_STR("KEYS %.8s > %s"), v.name, r);
  return b;
}

const Stats& stats() { return st; }

fmt::KvKeep g_keep;   // the lines a newer build wrote that this one does not read (fmt.h, class S: kept)
CORE_COLD void load() {
  g_keep.n = 0;
  File f;
  fmt::post_wait(kFile);   // 0.3.0: a posted save of it lands before we read it back (fmt.h)
  fmt::recover(kFile);   // a power cut between write_atomic's renames: the previous file back, a stray .tmp removed
  if (!f.open(kFile, kRead)) return;
  static char b[1536];
  memset(b, 0, sizeof b);
  int got = f.read(b, sizeof b - 1);
  f.close();
  static const char* const kKnown[] = {"mode", "auto", "mac", nullptr};
  if (got > 0) fmt::kv_keep(b, (uint32_t)got, kKnown, &g_keep);
  mode = strstr(b, "mode=channel") ? kChannel : kSelected;
  const char* a = strstr(b, "auto=");
  if (a) {
    int c = atoi(a + 5);
    if (c >= 1 && c <= 16) auto_ch = c;
  }
  mac_on = !strstr(b, "mac=0");
}

// 0.3.0 power-cut safety: posted to the card core as one atomic write (fmt.h), the keys a newer build wrote appended
// unchanged (class S, kv_append) - so a cut leaves keys.txt whole and a downgrade does not drop 0.4's settings.
CORE_COLD bool save() {
  if (!dirty) return false;
  char* t = fmt::post_begin();
  if (!t) return false;   // another settings write is in flight: at a later pass (still dirty)
  uint32_t n = (uint32_t)sniprintf(t, 64, CORE_STR("mode=%s\nauto=%d\nmac=%d\n"),
                                   mode == kChannel ? "channel" : "selected", auto_ch, mac_on ? 1 : 0);
  n = fmt::kv_append(g_keep, t, n, fmt::kPostMax);
  if (!fmt::post_end(kFile, n)) return false;
  dirty = false;
  return true;
}

CORE_COLD bool command(const char* p, char* out, int cap) {
  if (!strncmp(p, "keys=", 5)) {
    const char* a = p + 5;
    if (!strcmp(a, "selected") || !strcmp(a, "channel")) {
      Mode m = a[0] == 'c' ? kChannel : kSelected;
      if (m != mode) dirty = true;
      mode = m;
    } else if (!strncmp(a, "auto:", 5) && atoi(a + 5) >= 1 && atoi(a + 5) <= 16) {
      if (atoi(a + 5) != auto_ch) dirty = true;
      auto_ch = atoi(a + 5);
    } else if (!strcmp(a, "mac:on") || !strcmp(a, "mac:off")) {
      bool on = !strcmp(a, "mac:on");
      if (on != mac_on) dirty = true;
      mac_on = on;
    } else {
      sniprintf(out, cap, CORE_STR("keys=selected|channel, keys=auto:1..16, keys=mac:on|off"));
      return true;
    }
  } else if (strcmp(p, "keys") != 0) {
    return false;
  }
  View v = view();
  char l[40];
  const char* ln = line(l, sizeof l);
  int sel = selected();
  int n = sniprintf(out, cap, CORE_STR("keys: %s (auto channel %d, computer %s), selected %s; %s; %lu notes (%lu Sidecar, "
                                       "%lu to the Rytm, %lu probes, %lu learned, %lu swallowed, %lu sustained, %lu chords "
                                       "written, %lu dropped); sources"),
                    mode == kChannel ? "by channel" : "the selected track", auto_ch, mac_on ? "on" : "off",
                    sel >= 0 ? kTrackNames[sel] : "unknown", ln ? ln : "no source", (unsigned long)st.notes,
                    (unsigned long)st.poly, (unsigned long)st.forwarded, (unsigned long)st.probes,
                    (unsigned long)st.learned, (unsigned long)st.swallowed, (unsigned long)st.sustained,
                    (unsigned long)st.writes, (unsigned long)st.dropped);
  for (int i = 0; i < kSrcs && n < cap; ++i)
    if (v.present >> i & 1) n += sniprintf(out + n, cap - n, " %d:%s", i + 1, srcs[i].name);
  if (!v.present && n < cap) sniprintf(out + n, cap - n, " none");
  return true;
}

CORE_COLD void reset() {
  memset(srcs, 0, sizeof srcs);
  memset(echoes, 0, sizeof echoes);
  memset(guard_ms, 0, sizeof guard_ms);
  memset(&st, 0, sizeof st);
  echo_w = 0;
  mode = kSelected, auto_ch = kAutoCh, mac_on = true, dirty = false;
  learned = -1, last_key_ms = 0, last_src_track = -1;
  v_last = {-1, "", -1, -1, kNone, 0, 0, kSelected};
}

}  // namespace keys
}  // namespace core
#endif
