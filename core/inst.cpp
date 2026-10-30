// core/inst.cpp - see inst.h. Per note and per config: cold (the control core); render_add: the render core's.
#include "inst.h"

#if CORE_POLY
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "poly.h"
#include "user/um.h"
#include "voices.h"
#include "md/md.h"   // CORE_MD
#if CORE_MSAMP
#include "nmach.h"
#endif

namespace core {
namespace inst {

namespace {

// An instance: one engine state of any of the instanced machines (only `kind`'s part is used), its note and gate. The
// control core starts and releases it between blocks; the render core renders it and frees it once it is released and
// quiet (owner -1), as core/poly.cpp's slots.
struct Inst {
  volatile int8_t owner;    // the voice, -1 free
  int8_t last;              // the voice its engine state was last made for (kept when the render core frees it: the
                            // same voice's next note reuses the state without a re-init on the control core)
  uint8_t kind;             // the engine its state was made for (voices.h kM*), 0xFF none yet
  bool held;                // gated (its note on, not yet off)
  bool trig;                // a note to start (the render core takes it)
  bool used;                // its Plaits voice played since its last re-init
  float note, target, level;
  float pan;                // UNISON's -1 .. 1
  uint32_t order;
  volatile float peak;      // the render core's: its last block's peak (the allocator's quietest tail)
  float ref;                // its peak while held (a released tail's weight in the headroom: peak / ref)
  uint16_t quiet;           // the render core's: released blocks in a row below the floor
  plaits::Voice voice;
  plaits::Patch patch;
  plaits::Modulations mod;
  char* scratch;            // its Plaits voice's 16 KB
  rytm::DaisyEngine daisy;
  juno::Voice juno;
};
Inst* pool = nullptr;
uint32_t order_n = 0;

enum : int { kPathNone = 0, kPathPool = 1, kPathMono = 2, kPathInst = 3 };
struct VP {
  int path, machine;
  int lanes, unison, instances;
  bool clamped;
  int cpu100;
  int8_t idx[kMaxInst];       // the pool instance of stack place j = lane * unison + k (-1 none; stale when its owner moved on)
  float cents[kMaxInst];      // its UNISON detune
  Alloc a;
  uint8_t held[32];           // own-pool machines: the seq notes held, oldest first (VOICES' cap; AUTO: 32, more than
                              // any of their pools holds a voice, so their own steals come first, as before E0)
  int nheld;
  int mono;                   // the mono path's note held, -1 none
  uint32_t notes;
  float head;                 // the render core's headroom gain
  uint64_t cyc;               // the render core's: instance render cycles, blocks
  uint32_t blks;
};
VP vp[kVoices];
uint32_t g_noroom = 0;

CORE_COLD int machine_now(int vi) {
  const V& x = v[vi];
  if (x.user >= 0) return 0x50 + 8 * x.user;
#if CORE_MSAMP
  if (nm::is(vi)) return nm::of(vi);
  if (x.li) return kLiveMachine;
#endif
  if (x.fm) return kFm;
  if (x.juno) return kJuno;
  if (x.md) return kMd;
  if (x.grain) return kGrainMachine;
  if (x.dsps) return kDspSample;
  if (x.daisy) return kDaisy;
  return kPlaits;
}
CORE_COLD int kind_of(int m) {
  if (user_slot_of(m) >= 0) return kMUser;
  switch (m) {
    case kJuno: return kMJuno;
    case kDaisy: return kMDaisy;
    default: return kMPlaits;
  }
}
CORE_COLD Decl decl_of(int vi, int m) {
  const Decl* b = builtin(m);
  if (b) return *b;
  uint32_t fl = 0, cpu = 0, stb = 0;
  if (!um::inst_decl(vi, &fl, &cpu, &stb)) fl = 0;   // no ready slot: mono (its own path says why it is silent)
  return user_decl(m, fl, cpu, stb);
}

// a free instance for voice vi; none: the quietest released one anywhere, else none (counted)
CORE_COLD int grab(int vi) {
  for (int i = 0; i < kTotal; ++i)
    if (pool[i].owner < 0) return i;
  int b = -1;
  for (int i = 0; i < kTotal; ++i)
    if (!pool[i].held && (b < 0 || pool[i].peak < pool[b].peak)) b = i;
  if (b < 0) ++g_noroom;
  (void)vi;
  return b;
}
CORE_COLD void free_inst(int i) {
  Inst& I = pool[i];
  if (I.owner >= 0 && I.kind == kMUser) um::inst_note_off(I.owner, i);
  I.owner = -1, I.held = false, I.trig = false;
}
CORE_COLD bool has_user(int vi) {
  for (int i = 0; pool && i < kTotal; ++i)
    if (pool[i].owner == vi && pool[i].kind == kMUser) return true;
  return false;
}

CORE_COLD void drop_instances(int vi) {   // every instance of vi gone at once (its stack places forgotten)
  if (has_user(vi)) um::inst_stop(vi);   // the plugin's states leave (voice_leave), as the voice's own does
  for (int i = 0; pool && i < kTotal; ++i)
    if (pool[i].owner == vi) free_inst(i);
  VP& p = vp[vi];
  for (int j = 0; j < kMaxInst; ++j) p.idx[j] = -1;
  uint32_t steals = p.a.steals;   // (the count is the voice's since boot: STATUS, the console)
  reset(p.a, p.lanes > 0 ? p.lanes : 1);
  p.a.steals = steals;
}
CORE_COLD void leave_path(int vi) {   // vi stops using its current path's notes
  VP& p = vp[vi];
  if (p.path == kPathInst) drop_instances(vi);
  if (p.path == kPathPool) {
    for (int k = 0; k < p.nheld; ++k) poly::seq_off(vi, p.held[k]);
    p.nheld = 0;
  }
  if (p.path == kPathMono && p.mono >= 0) voice_note_off(vi), p.mono = -1;
  p.path = kPathNone;
}

// the lanes' state from the instances (the render core's peaks and frees)
CORE_COLD void sync(int vi) {
  VP& p = vp[vi];
  for (int l = 0; l < p.a.lanes; ++l) {
    Lane& L = p.a.lane[l];
    bool any = false;
    float pk = 0;
    for (int k = 0; k < p.unison; ++k) {
      int i = p.idx[l * p.unison + k];
      if (i < 0 || pool[i].owner != vi) continue;
      any = true;
      if (pool[i].peak > pk) pk = pool[i].peak;
    }
    L.sounding = any || L.held;
    if (!any && L.held) L.held = false;   // (taken by another voice meanwhile)
    L.level = pk;
  }
}

CORE_COLD void start(int vi, int j, int m, const Decl& d, int note, int vel, const seq::PolyCfg& cfg, int u, int k) {
  VP& p = vp[vi];
  int i = p.idx[j];
  if (i < 0 || pool[i].owner != vi) i = grab(vi);
  if (i < 0) return;
  Inst& I = pool[i];
  int kind = kind_of(m);
  bool fresh = I.last != vi || I.kind != kind;
  if (I.owner >= 0 && I.owner != vi) free_inst(i);   // a steal from another voice
  for (int jj = 0; jj < kMaxInst; ++jj)
    if (p.idx[jj] == i) p.idx[jj] = -1;   // (a tail of this voice's own taken over: no longer that place's)
  p.idx[j] = (int8_t)i;
  const V& x = v[vi];
  float cents, pan;
  unison_offset(cfg, u, k, &cents, &pan);
  p.cents[j] = cents;
  if (fresh) {
    switch (kind) {
      case kMPlaits:
        if (I.used) {
          stmlib::BufferAllocator allocator(I.scratch, 16384);
          I.voice.Init(&allocator);
        }
        memset(&I.patch, 0, sizeof I.patch);
        memset(&I.mod, 0, sizeof I.mod);
        I.mod.trigger_patched = true;
        I.note = I.target = (float)note;
        break;
      case kMDaisy: I.daisy.Init(48000.0f); break;
      case kMJuno: I.juno = junos[vi]; break;   // the voice's patch and knobs, a voice of its own
      default: break;
    }
    I.kind = (uint8_t)kind;
    I.last = (int8_t)vi;
  } else if (kind == kMJuno && strcmp(I.juno.name(), junos[vi].name()) && !I.held) {
    I.juno = junos[vi];   // the voice's PAT changed since this instance's state was made: its new patch
  }
  float semis = cents / 100.0f;
  float vf = (vel < 1 ? 1 : vel) / 127.0f;
  I.pan = pan, I.peak = 0, I.ref = 0, I.quiet = 0;
  switch (kind) {
    case kMPlaits: {   // voices.cpp's PLAITS trig: TUN semitones, FIN a semitone either way, VEL how much velocity counts
      I.target = note + roundf(x.p[6] - 64) + (x.p[9] - 64) / 64.0f + semis;
      if (x.p[14] <= 0 || fresh) I.note = I.target;
      float sens = x.p[13] / 127.0f, l = (1 - sens + sens * vf) * x.p[7] / 127.0f;
      I.level = l < 0.05f ? 0.05f : (l > 1 ? 1 : l);
      I.trig = true, I.used = true;
      break;
    }
    case kMDaisy: {   // DAISYSP's: TUN semitones
      I.note = I.target = note + roundf(x.p[6] - 64) + semis;
      float l = vf * x.p[7] / 127.0f;
      I.level = l < 0.05f ? 0.05f : (l > 1 ? 1 : l);
      I.trig = true;
      break;
    }
    case kMJuno:
      I.juno.SetKnobs(x.p);
      I.juno.NoteOn((float)note + semis, vf);
      break;
    case kMUser:
      if (!um::inst_note_on(vi, i, d.pitch == kPitchNone ? 60.0f : (float)note + semis, vf)) return;
      break;
    default: break;
  }
  I.owner = (int8_t)vi, I.held = true, I.order = ++order_n;
}
CORE_COLD void stop(int i) {   // a note off: the gate closes, its tail goes on
  Inst& I = pool[i];
  I.held = false;
  if (I.kind == kMJuno) I.juno.NoteOff();
  if (I.kind == kMUser && I.owner >= 0) um::inst_note_off(I.owner, i);
}

}  // namespace

CORE_COLD void init() {
  if (!pool) {
    pool = new Inst[kTotal];
    for (int i = 0; i < kTotal; ++i) {
      Inst& I = pool[i];
      I.scratch = new char[16384];
      stmlib::BufferAllocator allocator(I.scratch, 16384);
      I.voice.Init(&allocator);
      I.daisy.Init(48000.0f);
      I.juno.Init(48000.0f);
    }
  }
  reset();
}

CORE_COLD void reset() {
  for (int i = 0; pool && i < kTotal; ++i) {
    Inst& I = pool[i];
    I.owner = -1, I.last = -1, I.held = I.trig = false, I.kind = 0xFF, I.peak = 0, I.quiet = 0;
  }
  for (int vi = 0; vi < kVoices; ++vi) {
    VP& p = vp[vi];
    memset(&p, 0, sizeof p);
    p.machine = -1, p.mono = -1, p.head = 1.0f, p.lanes = 1, p.unison = 1;
    for (int j = 0; j < kMaxInst; ++j) p.idx[j] = -1;
    inst::reset(p.a, 1);
  }
  g_noroom = 0;
}

CORE_COLD bool seq_on(int vi, int note, int vel, const seq::PolyCfg& cfg) {
  if (vi < 0 || vi >= kVoices || note < 0 || note > 127 || !pool) return false;
  VP& p = vp[vi];
  int m = machine_now(vi);
  if (m != p.machine) leave_path(vi), p.machine = m;
  if (m == kMd && !CORE_MD) return false;   // MD where the board has none: nothing plays it
  Decl d = decl_of(vi, m);
  ++p.notes;
  if (d.own_pool) {   // 6-OP FM, the note machines: their pool; VOICES caps the notes held (AUTO: as before E0)
    if (p.path != kPathPool) leave_path(vi), p.path = kPathPool, p.nheld = 0;
    Resolved r = resolve(cfg, d);
    p.instances = r.instances, p.lanes = r.lanes, p.unison = 1, p.clamped = r.clamped, p.cpu100 = r.cpu100;
    for (int k = 0; k < p.nheld; ++k)
      if (p.held[k] == note) {   // retriggered: out of the order (it goes to the end)
        memmove(p.held + k, p.held + k + 1, (size_t)(p.nheld - k - 1));
        --p.nheld;
        break;
      }
    int cap = cfg.voices ? r.lanes : (int)sizeof p.held;
    while (p.nheld >= cap || p.nheld >= (int)sizeof p.held) {
      poly::seq_off(vi, p.held[0]);
      memmove(p.held, p.held + 1, (size_t)(p.nheld - 1));
      --p.nheld;
      ++p.a.steals;
    }
    // 6-OP FM: PAT picks the voice at the note, as a Rytm trig does (voices.cpp) - a PAT locked on the step
    // (seq.cpp apply_locks) reaches it here; notes already sounding keep theirs (poly: a slot's own copy)
    if (m == kFm) fm_select(vi, (int)v[vi].p[0]);
    if (!poly::seq_on(vi, note, vel)) return false;
    p.held[p.nheld++] = (uint8_t)note;
    return true;
  }
  Resolved r = resolve(cfg, d);
  p.clamped = r.clamped, p.cpu100 = r.cpu100;
  if (r.instances <= 1) {   // mono: the voice's own machine, a new note retriggers it (the old one choked)
    if (p.path != kPathMono) leave_path(vi), p.path = kPathMono;
    p.instances = 1, p.lanes = 1, p.unison = 1;
    if (p.mono >= 0 && p.mono != note) ++p.a.steals;
    p.mono = note;
    voice_play(vi, d.pitch == kPitchNone ? 60 : note, vel);
    return true;
  }
  if (p.path != kPathInst || r.lanes != p.lanes || r.unison != p.unison) {   // (re)configured: a fresh set
    leave_path(vi);
    p.path = kPathInst, p.lanes = r.lanes, p.unison = r.unison, p.instances = r.instances;
    drop_instances(vi);
  }
  sync(vi);
  int stole = 0;
  int lane = note_on(p.a, cfg, note, &stole);
  for (int k = 0; k < p.unison; ++k) {
    int j = lane * p.unison + k, i = p.idx[j];
    if (i >= 0 && pool[i].owner == vi && pool[i].held) stop(i);   // the stolen / retriggered note's gate
    start(vi, j, m, d, note, vel, cfg, p.unison, k);
  }
  voice_wake(vi);
  return true;
}

CORE_COLD void seq_off(int vi, int note) {
  if (vi < 0 || vi >= kVoices || !pool) return;
  VP& p = vp[vi];
  if (p.path == kPathPool) {
    for (int k = 0; k < p.nheld; ++k)
      if (note < 0 || p.held[k] == note) {
        memmove(p.held + k, p.held + k + 1, (size_t)(p.nheld - k - 1));
        --p.nheld, --k;
      }
    poly::seq_off(vi, note);
    return;
  }
  if (p.path == kPathMono) {
    if (p.mono >= 0 && (note < 0 || note == p.mono)) voice_note_off(vi), p.mono = -1;
    return;
  }
  if (p.path != kPathInst) {
    poly::seq_off(vi, note);   // (a note from before any config: the pool machines' own release)
    return;
  }
  for (;;) {
    int lane = note >= 0 ? note_off(p.a, note) : -1;
    if (note < 0)
      for (int l = 0; l < p.a.lanes && lane < 0; ++l)
        if (p.a.lane[l].held) p.a.lane[l].held = false, lane = l;
    if (lane < 0) break;
    for (int k = 0; k < p.unison; ++k) {
      int i = p.idx[lane * p.unison + k];
      if (i >= 0 && pool[i].owner == vi) stop(i);
    }
    if (note >= 0) break;
  }
}

CORE_COLD bool seq_mono(int vi) { return vi >= 0 && vi < kVoices && vp[vi].path == kPathMono && vp[vi].mono >= 0; }
CORE_COLD bool active(int vi) {
  for (int i = 0; pool && i < kTotal; ++i)
    if (pool[i].owner == vi) return true;
  return false;
}
CORE_COLD int voice_notes(int vi) {
  int n = 0;
  for (int i = 0; pool && i < kTotal; ++i) n += pool[i].owner == vi;
  return n;
}
CORE_COLD bool juno_patch_is(int vi, const char* name) {
  int n = 0;
  for (int i = 0; pool && i < kTotal; ++i)
    if (pool[i].owner == vi && pool[i].kind == kMJuno && pool[i].held) {
      if (strcmp(pool[i].juno.name(), name)) return false;
      ++n;
    }
  return n > 0;
}
CORE_COLD void silence(int vi) {
  if (vi < 0 || vi >= kVoices || !pool) return;
  VP& p = vp[vi];
  drop_instances(vi);
  p.nheld = 0, p.mono = -1;
}
CORE_COLD void knobs(int vi) {
  if (vi < 0 || vi >= kVoices || !pool) return;
  bool user = false;
  for (int i = 0; i < kTotal; ++i) {
    Inst& I = pool[i];
    if (I.owner != vi) continue;
    if (I.kind == kMJuno) I.juno.SetKnobs(v[vi].p);
    user |= I.kind == kMUser;
  }
  if (user) um::inst_params(vi);
}

void render_add(int vi, float* l, float* r, bool stereo) {
  if (!pool) return;
  VP& p = vp[vi];
  const V& x = v[vi];
  uint32_t t0 = cycles();
  const float atk = 1.0f - expf(-1000.0f * kBlock / (48000.0f * poly::kHeadAtkMs));
  const float rel = 1.0f - expf(-1000.0f * kBlock / (48000.0f * poly::kHeadRelMs));
  float accl[kBlock] = {0}, accr[kBlock] = {0};
  float e = 0;
  bool any = false;
  for (int i = 0; i < kTotal; ++i) {
    Inst& I = pool[i];
    if (I.owner != vi) continue;
    any = true;
    float o[kBlock];
    switch (I.kind) {
      case kMPlaits: {
        if (x.p[14] > 0) {   // GLD: up to 0.5 s
          float k = kBlock / 48000.0f / (0.002f + x.p[14] / 127.0f * 0.5f);
          I.note += (I.target - I.note) * (k > 1 ? 1 : k);
        }
        I.patch = x.patch;
        I.patch.note = I.note;
        I.mod.level = I.level;
        I.mod.trigger = I.trig ? 1.0f : 0.0f;
        I.mod.trigger_patched = true;
        I.trig = false;
        plaits::Voice::Frame f[kBlock];
        I.voice.Render(I.patch, I.mod, f, kBlock);
        float aux = x.p[10] / 127.0f, gain = x.p[15] / 100.0f / 32767.0f;   // AUX, VOL (the mono voice's, without its 0.7)
        for (int s = 0; s < kBlock; ++s) o[s] = (f[s].out * (1 - aux) + f[s].aux * aux) * gain;
        break;
      }
      case kMDaisy: {
        int model = daisy_model(x);
        I.daisy.Render(model, I.note, x.p[1] / 127.0f, x.p[2] / 127.0f, x.p[3] / 127.0f, 0.8f, x.p[5] / 127.0f, I.trig,
                       I.level, o, kBlock);
        I.trig = false;
        break;
      }
      case kMJuno: I.juno.Render(o, kBlock); break;
      case kMUser: um::inst_render(vi, i, o); break;
      default:
        for (int s = 0; s < kBlock; ++s) o[s] = 0;
        break;
    }
    float pk = 0;
    for (int s = 0; s < kBlock; ++s) {
      float a = o[s] < 0 ? -o[s] : o[s];
      pk = a > pk ? a : pk;
    }
    I.peak = pk;
    if (I.held) {
      if (pk > I.ref) I.ref = pk;
      e += 1.0f;
    } else {
      e += I.ref > 0 ? (pk < I.ref ? pk / I.ref : 1.0f) : 0.0f;
      I.quiet = pk < 1e-4f ? (uint16_t)(I.quiet + 1) : 0;
    }
    // the pan (UNISON's SPREAD) on a stereo voice: a balance, the centre at unity
    float gl = stereo && I.pan > 0 ? 1.0f - I.pan : 1.0f, gr = stereo && I.pan < 0 ? 1.0f + I.pan : 1.0f;
    for (int s = 0; s < kBlock; ++s) accl[s] += o[s] * gl, accr[s] += o[s] * gr;
    if (!I.held && (I.quiet >= 40 || I.quiet > 0 && I.ref <= 0)) I.owner = -1;   // released and quiet for 10 ms: free
  }
  float g0 = p.head, target = e > 1.0f ? 1.0f / sqrtf(e) : 1.0f;
  float g1 = g0 + (target - g0) * (target < g0 ? atk : rel);
  if (fabsf(g1 - target) < 1e-5f) g1 = target;
  p.head = g1;
  if (!any) return;
  float dg = (g1 - g0) / (float)kBlock;
  for (int s = 0; s < kBlock; ++s) {
    float g = g0 + dg * (float)(s + 1);
    l[s] += accl[s] * g;
    if (stereo) r[s] += accr[s] * g;
  }
  __atomic_store_n(&p.cyc, p.cyc + (uint64_t)(cycles() - t0), __ATOMIC_RELAXED);
  __atomic_store_n(&p.blks, p.blks + 1, __ATOMIC_RELAXED);
}

CORE_COLD bool resolve_for(int vi, const seq::PolyCfg& cfg, Resolved* r, int* machine) {
  if (vi < 0 || vi >= kVoices) return false;
  int m = machine_now(vi);
  Decl d = decl_of(vi, m);
  *r = resolve(cfg, d);
  if (d.own_pool && !cfg.voices) r->clamped = false;   // AUTO on a pool machine: its pool as before E0
  if (machine) *machine = m;
  return true;
}

CORE_COLD uint32_t inst_bytes() { return (uint32_t)sizeof(Inst) + 16384u; }

CORE_COLD VoiceStat stat(int vi) {
  VoiceStat s = {};
  if (vi < 0 || vi >= kVoices) return s;
  const VP& p = vp[vi];
  s.machine = p.machine, s.path = p.path, s.instances = p.path ? p.instances : 0, s.lanes = p.lanes, s.unison = p.unison;
  s.sounding = p.path == kPathInst ? voice_notes(vi) : p.path == kPathMono ? p.mono >= 0 : p.path == kPathPool ? p.nheld : 0;
  s.clamped = p.clamped, s.cpu100 = p.cpu100, s.notes = p.notes, s.steals = p.a.steals;
  s.ram = p.path == kPathInst ? (uint32_t)p.instances * inst_bytes() : 0;
  uint64_t cyc = __atomic_load_n(&p.cyc, __ATOMIC_RELAXED);
  uint32_t blks = __atomic_load_n(&p.blks, __ATOMIC_RELAXED);
  double block_cyc = (double)cycles_per_second() * kBlock / 48000.0;
  s.meas100 = blks && block_cyc > 0 ? (int)((double)cyc / blks / block_cyc * 10000.0 + 0.5) : 0;
  return s;
}

namespace {
const char* path_name(int p) {
  static const char* const n[] = {"-", "pool", "mono", "inst"};
  return p >= 0 && p < 4 ? n[p] : "?";
}
}  // namespace

CORE_COLD int json(char* out, int cap) {
  int n = sniprintf(out, cap, "\"inst\":{\"total\":%d,\"bytes\":%u,\"noroom\":%u,\"v\":[", kTotal, (unsigned)inst_bytes(),
                    (unsigned)g_noroom);
  for (int vi = 0; vi < kVoices && n < cap; ++vi) {
    VoiceStat s = stat(vi);
    n += sniprintf(out + n, cap - n,
                   "%s{\"m\":\"%s\",\"path\":\"%s\",\"inst\":%d,\"lanes\":%d,\"uni\":%d,\"on\":%d,\"clamped\":%d,"
                   "\"cpu\":%d,\"meas\":%d,\"ram\":%u,\"notes\":%u,\"steals\":%u}",
                   vi ? "," : "", s.machine >= 0 ? machine_short(s.machine) : "-", path_name(s.path), s.instances, s.lanes,
                   s.unison, s.sounding, s.clamped ? 1 : 0, s.cpu100, s.meas100, (unsigned)s.ram, (unsigned)s.notes,
                   (unsigned)s.steals);
  }
  if (n < cap) n += sniprintf(out + n, cap - n, "]}");
  return n < cap ? n : 0;
}

CORE_COLD bool command(const char* p, char* out, int cap) {
  // tests and the console: inst=on:V:NOTE:VEL[:VOICES[:UNISON[:ALLOC]]] (V 1..8) / inst=off:V:NOTE|all - a note as the
  // sequencer sends it, with that config
  if (!strncmp(p, "inst=on:", 8) || !strncmp(p, "inst=off:", 9)) {
    bool on = p[5] == 'o' && p[6] == 'n';
    const char* q = p + (on ? 8 : 9);
    int a[6] = {0, -1, 100, 0, 0, 0}, k = 0;
    while (k < 6 && *q) {
      a[k++] = !strncmp(q, "all", 3) ? -1 : atoi(q);
      q = strchr(q, ':');
      if (!q) break;
      ++q;
    }
    int vi = a[0] - 1;
    if (vi < 0 || vi >= kVoices || (on && (a[1] < 0 || a[1] > 127)))
      return sniprintf(out, cap, "inst=on:V:NOTE:VEL[:VOICES[:UNISON[:ALLOC]]] | inst=off:V:NOTE|all"), true;
    if (!on) {
      seq_off(vi, a[1]);
      return sniprintf(out, cap, "inst: v%d off %d", vi + 1, a[1]), true;
    }
    seq::PolyCfg c = {};
    c.voices = (uint8_t)(a[3] < 0 ? 0 : a[3] > kMaxInst ? kMaxInst : a[3]);
    c.unison = (uint8_t)(a[4] < 0 ? 0 : a[4] > kMaxInst ? kMaxInst : a[4]);
    c.alloc = (uint8_t)(a[5] >= 0 && a[5] < seq::kAllocs ? a[5] : 0);
    bool ok = seq_on(vi, a[1], a[2], c);
    if (ok) voice_wake(vi);
    VoiceStat st = stat(vi);
    return sniprintf(out, cap, "inst: v%d note %d %s, %s %d/%d", vi + 1, a[1], ok ? "on" : "not played", path_name(st.path),
                     st.sounding, st.instances), true;
  }
  if (strcmp(p, "inst") != 0) return false;
  int n = sniprintf(out, cap, "inst: %d instances (%u B each), no room %u;", kTotal, (unsigned)inst_bytes(), (unsigned)g_noroom);
  for (int vi = 0; vi < kVoices && n < cap; ++vi) {
    VoiceStat s = stat(vi);
    if (!s.path) continue;
    n += sniprintf(out + n, cap - n, " v%d %s %s %d/%d (x%d)%s cpu %d.%02d%% meas %d.%02d%% %u KB, %u notes %u steals;", vi + 1,
                   s.machine >= 0 ? machine_short(s.machine) : "-", path_name(s.path), s.sounding, s.instances, s.unison,
                   s.clamped ? " CLAMPED" : "", s.cpu100 / 100, s.cpu100 % 100, s.meas100 / 100, s.meas100 % 100,
                   (unsigned)(s.ram >> 10), (unsigned)s.notes, (unsigned)s.steals);
  }
  return true;
}

}  // namespace inst
}  // namespace core
#endif
