// pi/engine/tests/test_guard.cpp - the voice guard's bookkeeping (guard.h's inline part): a voice is muted only when it
// is over budget on an overloaded core kOverBlocks blocks in a row (a burst, or an over-budget voice on a core with room,
// never mutes); a fault's Recover mutes the voice that was rendering; each mute is news once; a change of the voice's
// machine unmutes it (Fresh), the same machine does not; Unmute clears every voice and the counts; the first fatal
// reason wins.
#include <cstring>
#include <initializer_list>

#include "check.h"
#include "guard.h"

static const char kJuno[] = "JUNO", kFm[] = "FM";   // the machine keys (bridge::machine_name's static names)

int main() {
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
