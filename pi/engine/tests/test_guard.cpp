// pi/engine/tests/test_guard.cpp - the voice guard's bookkeeping (guard.h's inline part): a voice is muted only when it
// is over budget on an overloaded core kOverBlocks blocks in a row (a burst, or an over-budget voice on a core with room,
// never mutes); a fault's Recover mutes the voice that was rendering; each mute is news once; a change of the voice's
// machine unmutes it (Fresh), the same machine does not; Unmute clears every voice and the counts; the first fatal
// reason wins.
#include <cstring>
#include <initializer_list>

#include "check.h"
#define CGUARD_HOST_JMP 1   // (the host's setjmp for the guarded calls' jump: see guard.h)
#include "guard.h"

static const char kJuno[] = "JUNO", kFm[] = "FM";   // the machine keys (bridge::machine_name's static names)

// Plugin API 3's guarded calls (ControlCallOn): the exception's way back stood in for by what ResumeControl does after
// Throw's eret (NoteControlFault, then a longjmp into the core's buffer)
static CGuard* g_pg;
static unsigned g_core;
static void ok_call(void* p) { ++*(int*)p; }
static void fault_call(void* p) {
  ++*(int*)p;
  CHECK(g_pg->ControlArmed(g_core));
  g_pg->NoteControlFault(g_core, 0x4000 + g_core);
  GUARD_LONGJMP(g_pg->ControlJumpBuffer(g_core));
}
static void nested_call(void* p) {   // a guarded call inside one: runs unguarded, the outer guard stays
  u64 pc = 0;
  CHECK(g_pg->ControlCallOn(g_core, ok_call, p, &pc) == 0 && g_pg->ControlArmed(g_core));
}
static void control_calls() {
  static CGuard g;
  g_pg = &g;
  for (unsigned c : {2u, 3u}) {
    g_core = c;
    u64 pc = 0;
    int n = 0;
    CHECK(g.ControlCallOn(c, ok_call, &n, &pc) == 0 && n == 1 && !g.ControlArmed(c) && pc == 0);
    for (int k = 0; k < 3; ++k) {   // a fault, again and again: back each time, the PC delivered, disarmed
      CHECK(g.ControlCallOn(c, fault_call, &n, &pc) == 1 && pc == 0x4000 + c && !g.ControlArmed(c));
    }
    CHECK(g.ControlCallOn(c, ok_call, &n, &pc) == 0 && n == 5);
    CHECK(g.ControlCallOn(c, nested_call, &n, &pc) == 0 && n == 6 && !g.ControlArmed(c));
  }
  CHECK(g.ControlFaults() == 6);
  // cores 0 and 1 run the call unguarded (a fault there is the board's)
  int n = 0;
  u64 pc = 0;
  CHECK(g.ControlCallOn(0, ok_call, &n, &pc) == 0 && !g.ControlArmed(0) && n == 1);
  CHECK(g.ControlCallOn(1, ok_call, &n, &pc) == 0 && !g.ControlArmed(1) && n == 2);
  // (core 2 inside a voice's render, Enter: unguarded too - Enter lives in guard.cpp, which this test does not build)
}

// The timed call (ControlCallTimedOn): the timer stood in for by start / stop hooks, its interrupt by what TimerIRQ
// does (TakeTimeout with the frame's ELR and SPSR, then the jump ResumeControl makes after the eret)
static int g_starts, g_stops;
static u32 g_us;
static bool g_running;
static void t_start(u32 us) { ++g_starts, g_us = us, g_running = true; }
static void t_stop() { ++g_stops, g_running = false; }
static const u64 kEL1t = 0x4, kEL1h = 0x5;
static void fire(u64 pc) {   // the interrupt while the call runs
  CHECK(g_running && g_pg->TimedArmed(3));
  if (g_pg->TakeTimeout(3, pc, kEL1t)) GUARD_LONGJMP(g_pg->ControlJumpBuffer(3));
  CHECK(false);
}
static void slow_call(void* p) { ++*(int*)p; fire(0x7000); }
static void timed_fault_call(void* p) {   // a fault inside a timed call: 1, not 2 (Throw stops the timer)
  ++*(int*)p;
  g_pg->NoteControlFault(3, 0x4003);
  GUARD_LONGJMP(g_pg->ControlJumpBuffer(3));
}
static void elsewhere_call(void* p) {   // an interrupt that did not come from task level: the call goes on
  ++*(int*)p;
  CHECK(!g_pg->TakeTimeout(3, 0x7100, kEL1h) && g_pg->ControlArmed(3));
}
static void nested_timed(void* p) {   // nested: runs unguarded and unlimited, the outer call's limit stays
  u64 pc = 0;
  int s = g_starts;
  CHECK(g_pg->ControlCallTimedOn(3, ok_call, p, &pc, 50, t_start, t_stop) == 0 && g_starts == s);
  CHECK(g_pg->ControlArmed(3) && g_pg->TimedArmed(3) == g_running);
}
static void timed_calls() {
  static CGuard g;
  g_pg = &g;
  u64 pc = 0;
  int n = 0;
  // returns in time: 0, the timer started inside the guard and stopped (twice: TimedEntry, then the caller)
  CHECK(g.ControlCallTimedOn(3, ok_call, &n, &pc, 1000, t_start, t_stop) == 0 && n == 1 && pc == 0);
  CHECK(g_starts == 1 && g_us == 1000 && !g_running && g_stops == 2 && !g.TimedArmed(3) && !g.ControlArmed(3));
  // stopped at the limit: 2, the interrupted PC, disarmed, the timer off; and again
  for (int k = 0; k < 3; ++k) {
    CHECK(g.ControlCallTimedOn(3, slow_call, &n, &pc, 1000, t_start, t_stop) == 2 && pc == 0x7000);
    CHECK(!g_running && !g.TimedArmed(3) && !g.ControlArmed(3));
  }
  CHECK(n == 4 && g.ControlTimeouts() == 3 && g.ControlFaults() == 0);
  // a late interrupt (after the call returned) is not taken, and the next call is not stopped by it
  CHECK(!g.TakeTimeout(3, 0x7200, kEL1t));
  CHECK(g.ControlCallTimedOn(3, ok_call, &n, &pc, 1000, t_start, t_stop) == 0 && n == 5);
  // a fault: 1 with its PC, not counted as a stop
  CHECK(g.ControlCallTimedOn(3, timed_fault_call, &n, &pc, 1000, t_start, t_stop) == 1 && pc == 0x4003 && !g_running);
  CHECK(g.ControlFaults() == 1 && g.ControlTimeouts() == 3 && !g.TimedArmed(3));
  CHECK(g.ControlCallTimedOn(3, elsewhere_call, &n, &pc, 1000, t_start, t_stop) == 0 && n == 7);
  CHECK(g.ControlCallTimedOn(3, nested_timed, &n, &pc, 1000, t_start, t_stop) == 0 && n == 8 && !g.ControlArmed(3));
  // inside an ordinary guarded call: unlimited (the guard is the outer one's)
  int s = g_starts;
  CHECK(g.ControlCallOn(3, nested_timed, &n, &pc) == 0 && n == 9 && g_starts == s);
  // core 2 (and 0, 1): ControlCall's behaviour, no timer, never 2; the interrupt is core 3's only
  CHECK(g.ControlCallTimedOn(2, ok_call, &n, &pc, 1000, t_start, t_stop) == 0 && n == 10 && g_starts == s);
  CHECK(g.ControlCallTimedOn(0, ok_call, &n, &pc, 1000, t_start, t_stop) == 0 && n == 11 && g_starts == s);
  CHECK(!g.TakeTimeout(2, 0x7300, kEL1t) && g.ControlTimeouts() == 3);
}

int main() {
  control_calls();
  timed_calls();
  static CGuard g;
  const unsigned N = CGuard::kOverBlocks;
  g.SetBudget(100);
  g.SetBlock(1000);   // overloaded from 800 ticks
  CHECK(g.Budget() == 100 && g.MutedMask() == 0);

  // over budget while the core has room: never muted, however long
  g.CoreDone(1, 500);
  for (unsigned i = 0; i < 3 * N; i++) g.Leave(1, 2, 300, kJuno);
  CHECK(!g.Muted(2) && g.TakeNews() == 0 && g.MaxTicks(2) == 300);

  // a burst on an overloaded core (a machine starting, a poly switch): N - 1 blocks, then one under -> not muted
  g.CoreDone(1, 900);
  for (unsigned i = 0; i < N - 1; i++) g.Leave(1, 2, 300, kJuno);
  g.Leave(1, 2, 50, kJuno);
  for (unsigned i = 0; i < N - 1; i++) g.Leave(1, 2, 300, kJuno);
  CHECK(!g.Muted(2) && g.TakeNews() == 0);
  // the core gets room again mid-run: the count restarts
  g.CoreDone(1, 400);
  g.Leave(1, 2, 300, kJuno);
  g.CoreDone(1, 900);
  for (unsigned i = 0; i < N - 1; i++) g.Leave(1, 2, 300, kJuno);
  CHECK(!g.Muted(2));
  g.Leave(1, 2, 300, kJuno);   // the N-th in a row on an overloaded core
  CHECK(g.Muted(2) && g.MutedMask() == (1u << 2));
  CHECK(strcmp(g.Why(2), "over budget on an overloaded core") == 0);
  CHECK(g.TakeNews() == (1u << 2) && g.TakeNews() == 0);   // logged once
  CHECK(!g.Armed(1));

  // exactly the budget is within it
  g.CoreDone(2, 900);
  for (unsigned i = 0; i < 2 * N; i++) g.Leave(2, 5, 100, kFm);
  CHECK(!g.Muted(5));

  // the same machine stays muted; another machine on the voice starts clean (unmuted, counts reset, freed news once)
  CHECK(!g.Fresh(2, kJuno) && g.Muted(2));
  CHECK(g.Fresh(2, kFm) && !g.Muted(2) && g.MaxTicks(2) == 0);
  CHECK(g.TakeFreed() == (1u << 2) && g.TakeFreed() == 0);
  CHECK(!g.Fresh(2, kJuno));   // not muted: nothing to free
  g.Leave(1, 2, 300, kFm);     // its count restarted: one over does not mute
  CHECK(!g.Muted(2));

  // a user voice: the same name for every plugin - the slot and the plugin loader's epoch (machine_gen) tell them apart
  static const char kUser[] = "USER";
  g.CoreDone(1, 900);
  CHECK(g.Recover(1, kUser, 1 | 7u << 3) == 0 && g.Muted(0));   // U1, epoch 7 crashed (Recover: voice 0, see below)
  g.TakeNews();
  CHECK(!g.Fresh(0, kUser, 1 | 7u << 3));   // the same plugin: muted
  CHECK(g.Fresh(0, kUser, 2 | 7u << 3) && !g.Muted(0));   // the voice now on U2
  g.Recover(1, kUser, 2 | 7u << 3);
  g.TakeNews();
  CHECK(g.Fresh(0, kUser, 2 | 8u << 3));   // U2 reloaded (another plugin, or the same again): new
  g.TakeFreed();
  // the N-th over exactly at the hot line (800 of 1000): counted
  g.CoreDone(2, 800);
  for (unsigned i = 0; i < N; i++) g.Leave(2, 4, 101, kFm);
  CHECK(g.Muted(4));
  CHECK(g.Fresh(4, kJuno));
  g.CoreDone(2, 799);   // just under: not counted
  for (unsigned i = 0; i < 2 * N; i++) g.Leave(2, 4, 101, kJuno);
  CHECK(!g.Muted(4));
  g.TakeNews(); g.TakeFreed();

  // a fault: Recover mutes the core's current voice (set by Enter, which is Pi-only: guard.cpp; before any Enter,
  // voice 0) at once and disarms the core; a new machine frees it too
  CHECK(g.Recover(2, kJuno) == 0 && g.Muted(0) && strcmp(g.Why(0), "crashed") == 0 && !g.Armed(2));
  CHECK(g.TakeNews() == 1u);
  CHECK(!g.Fresh(0, kJuno) && g.Fresh(0, kFm) && !g.Muted(0));
  g.TakeFreed();

  // Unmute: every voice back, the counts and the cores' load too
  g.CoreDone(1, 900);
  for (unsigned i = 0; i < N - 1; i++) g.Leave(1, 3, 500, kJuno);
  g.Unmute();
  CHECK(g.MutedMask() == 0 && g.MaxTicks(2) == 0 && g.MaxTicks(3) == 0);
  g.Leave(1, 3, 500, kJuno);   // core 1 is not overloaded after Unmute until its next block says so
  CHECK(!g.Muted(3));
  g.CoreDone(1, 900);
  for (unsigned i = 0; i < N - 1; i++) g.Leave(1, 3, 500, kJuno);
  CHECK(!g.Muted(3));
  g.Leave(1, 3, 500, kJuno);
  CHECK(g.Muted(3));

  // the first fatal reason is kept
  CHECK(g.FatalWhy() == nullptr);
  g.Fatal("first");
  g.Fatal("second");
  CHECK(strcmp(g.FatalWhy(), "first") == 0);
  printf("test_guard: PASS\n");
  return 0;
}
