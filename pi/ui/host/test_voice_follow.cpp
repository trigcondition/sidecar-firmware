// pi/ui/host/test_voice_follow.cpp - which touchscreen pages follow the Rytm's selected track, and the page a machine
// needs (pi/engine/voice_pi.h): the CHOP page used to be left out, so selecting a non-CHOP track after a CHOP one kept
// the CHOP page up. The decision is the pair FollowsTrack (the page showing) + PageForMachine (the new track's machine);
// core 0's glue (kernel.cpp: Request -> sidecar_ui_show) is the Pi's alone and is not run here.
// make -C pi/ui/host test
#include <stdio.h>

#include "../../engine/voice_pi.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

enum { PG_HOME = 0, PG_SOUND = 1, PG_SEQ = 4, PG_RYTM = 5, PG_MACH = 7, PG_VOICE = 8, PG_INS = 9, PG_SONG = 10, PG_REC = 11, PG_SCOPE = 12,
       PG_CHOP = 17 };

// what the display does when the selected track (machine vb) changes while `page` shows (-1: stays)
static int after_change(int page, int vb) {
  return voice_pi::FollowsTrack(page) ? voice_pi::PageForMachine(vb) : -1;
}

int main() {
  const int chop = core::kChopMachine, gran = core::kGranMachine, wave = core::kWaveMachine;
  CHECK(voice_pi::PageForMachine(chop) == PG_CHOP && voice_pi::PageForMachine(chop | 3) == PG_CHOP);
  CHECK(voice_pi::PageForMachine(gran) == PG_MACH && voice_pi::PageForMachine(wave) == PG_MACH);
  CHECK(voice_pi::PageForMachine(-1) == PG_VOICE && voice_pi::PageForMachine(0x7F) == PG_VOICE);   // unknown, analog
  // the reported bug: on CHOP, a non-CHOP track is selected: the display leaves CHOP
  CHECK(after_change(PG_CHOP, 0x08) == PG_VOICE);         // another Sidecar machine
  CHECK(after_change(PG_CHOP, 0x7F) == PG_VOICE);         // an analog track
  CHECK(after_change(PG_CHOP, gran) == PG_MACH);          // GRANULAR / WAVETABLE: their machine view, as the HOME tile
  CHECK(after_change(PG_CHOP, wave) == PG_MACH);
  CHECK(after_change(PG_CHOP, chop) == PG_CHOP);          // another CHOP track: stays (the view follows the voice)
  // the pages that already followed, and entering CHOP
  CHECK(after_change(PG_VOICE, chop) == PG_CHOP);
  CHECK(after_change(PG_VOICE, gran) == PG_MACH);
  CHECK(after_change(PG_MACH, chop) == PG_CHOP);
  CHECK(after_change(PG_MACH, 0x08) == PG_VOICE);
  // every other page is left alone by a track change (a deliberate navigation is not undone)
  const int others[] = {PG_HOME, PG_SOUND, PG_SEQ, PG_RYTM, PG_INS, PG_SONG, PG_REC, PG_SCOPE, 13, 14, 15, 16};
  for (int p : others) CHECK(after_change(p, chop) == -1 && after_change(p, 0x08) == -1);
  if (g_fail) return 1;
  printf("test_voice_follow: ok\n");
  return 0;
}
