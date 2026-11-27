// pi/engine/tests/test_side_out.cpp - the side output stage (side_out.h): which stage the control core fills next,
// slot 1's stream pulling its own FIFO only while a box sits beside a Rytm, a new stream starting from an empty FIFO,
// and each stage mapping its voices to its own box's channels.
#define CORE_VOICES 16   // the Pi's engine: slot 0's voices 0..7, slot 1's 8..15
#include "side_out.h"

#include "check.h"

Output g_Output, g_OutputSide;
namespace core {
namespace device {
Link g_links[kLinks] = {};
Link* g_base = &g_links[0];
}  // namespace device
}  // namespace core

static int16_t blk[12][8];

int main() {
  using side_out::pick;
  // which pass next: each FIFO to its target (96), the emptier first, the side stage only while there is one
  CHECK(pick(0, false, 0, 96, 12) == 0);
  CHECK(pick(90, false, 0, 96, 12) == -1);     // within a block of the target: nothing to do
  CHECK(pick(84, false, 0, 96, 12) == 0);      // exactly a block short: one more
  CHECK(pick(84, true, 0, 96, 12) == 1);       // both short: the emptier (the side)
  CHECK(pick(10, true, 60, 96, 12) == 0);      // both short: the emptier (the base)
  CHECK(pick(40, true, 40, 96, 12) == 0);      // a tie: the base box first
  CHECK(pick(96, true, 20, 96, 12) == 1);      // the base full, the side short
  CHECK(pick(96, true, 90, 96, 12) == -1);

  // no box beside the Rytm: slot 1's stream sends silence and takes nothing from the side FIFO
  core::device::Profile box = {};
  box.id = 2, box.name = "A4";
  CHECK(core::device::side_slot() == -1);
  for (int s = 0; s < 12; ++s) blk[s][0] = 8192;   // its voice 0 (engine voice 8) at 0.25
  g_OutputSide.SetMode(Output::kTracks);
  g_OutputSide.SetMeasured(false);
  g_OutputSide.SetFifoTarget(96);
  g_OutputSide.Push(blk, 12);
  int32_t f[7][12];
  int16_t v[7][8];
  f[0][4] = 123, v[0][0] = 5;
  side_out::Pull(1, &core::out::kRytmMk2, f, 7, v);
  CHECK(f[0][4] == 0 && v[0][0] == 0 && g_OutputSide.Level() == 12);

  // a box beside the Rytm (slot 1 has a box, the base is slot 0): its stream's pull starts the stage afresh - the frames
  // queued before it are dropped, the fade starts at 0
  core::device::g_links[1].box = &box, core::device::g_links[1].held = &box;   // (attach order: slot 1 held, the second box)
  CHECK(core::device::side_slot() == 1 && core::device::on_side(8) && !core::device::on_side(7));
  side_out::Pull(0, &core::out::kRytmMk2, f, 7);   // another slot's stream: still silence
  CHECK(f[3][4] == 0 && g_OutputSide.Level() == 12);
  // the side box's own device row: its voice 0 on channel 9 (a made-up box), the base box's table untouched
  core::out::Device dev = core::out::kRytmMk2;
  dev.voice[0] = 9, dev.voice[5] = 4;   // (voices 0 and 5 swapped)
  side_out::Pull(1, &dev, f, 7);
  CHECK(g_OutputSide.Streams() == 1 && f[0][9] == 0);   // the queued frames went with the Reset: an empty FIFO
  for (int k = 0; k < 1400; ++k) {                      // past the 200 ms fade, the FIFO never empty
    g_OutputSide.Push(blk, 12);
    side_out::Pull(1, &dev, f, 7, v);
  }
  CHECK(f[6][9] > 0 && f[6][core::out::kRytmMk2.voice[0]] == 0 && v[6][0] == 8192);
  CHECK(&g_Output.Dev() == &core::out::device() && &g_OutputSide.Dev() == &dev);
  // the side box's stream starts again (side_out::Restart): faded in again from an empty FIFO
  side_out::Restart();
  g_OutputSide.Push(blk, 12);
  side_out::Pull(1, &dev, f, 7);
  CHECK(g_OutputSide.Streams() == 2 && f[0][9] == 0 && g_OutputSide.Level() == 0);

  // the Rytm goes (the base becomes slot 1, the box alone): no side stage, slot 1's voices are the base box's again
  core::device::g_base = &core::device::g_links[1];
  CHECK(core::device::side_slot() == -1);
  side_out::Pull(1, &dev, f, 7);
  CHECK(f[0][9] == 0);
  // and back beside a Rytm: a fresh start once more
  core::device::g_base = &core::device::g_links[0];
  g_OutputSide.Push(blk, 12);
  side_out::Pull(1, &dev, f, 7);
  CHECK(g_OutputSide.Streams() == 3 && g_OutputSide.Level() == 0);
  printf("test_side_out: ok\n");
  return 0;
}
