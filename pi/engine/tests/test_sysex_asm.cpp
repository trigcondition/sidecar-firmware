// pi/engine/tests/test_sysex_asm.cpp - whole SysEx from the second box's byte stream (sysex_asm.h): a hello and a report
// come out whole, realtime bytes inside are skipped, note bytes around are ignored, a broken or too long message is
// dropped and counted, and the next one after it is whole again.
#include <cstring>
#include <utility>
#include <vector>

#include "check.h"
#include "sysex_asm.h"

template <unsigned N>
static std::vector<std::vector<uint8_t>> feed(SysExAsm<N>& a, const std::vector<uint8_t>& in) {
  std::vector<std::vector<uint8_t>> out;
  for (uint8_t b : in)
    if (unsigned n = a.feed(b)) out.emplace_back(a.data(), a.data() + n);
  return out;
}

int main() {
  const std::vector<uint8_t> hello = {0xF0, 0x7D, 0x52, 0x54, 0x4F, 1, 0, 3, 1, 'A', '4', 0xF7};
  const std::vector<uint8_t> off = {0xF0, 0x7D, 0x52, 0x54, 0x11, 2, 0xF7};
  {
    SysExAsm<64> a;
    std::vector<uint8_t> in = {0x90, 60, 100};   // a note before: not ours
    in.insert(in.end(), hello.begin(), hello.end());
    in.insert(in.end(), off.begin(), off.end());
    auto m = feed(a, in);
    CHECK(m.size() == 2 && m[0] == hello && m[1] == off && a.bad() == 0);
  }
  {
    SysExAsm<64> a;   // a clock tick and active sensing inside the hello
    std::vector<uint8_t> in(hello.begin(), hello.begin() + 5);
    in.push_back(0xF8);
    in.insert(in.end(), hello.begin() + 5, hello.end() - 1);
    in.push_back(0xFE);
    in.push_back(0xF7);
    auto m = feed(a, in);
    CHECK(m.size() == 1 && m[0] == hello && a.bad() == 0);
  }
  {
    SysExAsm<64> a;   // broken by a status byte, cut by a new F0: both dropped, counted; the next whole
    std::vector<uint8_t> in = {0xF0, 0x7D, 0x52, 0x90, 1, 2, 0xF0, 0x7D, 0x52};
    in.insert(in.end(), off.begin(), off.end());
    auto m = feed(a, in);
    CHECK(m.size() == 1 && m[0] == off && a.bad() == 2);
  }
  {
    SysExAsm<8> a;   // longer than the room: dropped whole up to its F7, counted once; the next fits
    std::vector<uint8_t> in = {0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xF7};
    in.insert(in.end(), off.begin(), off.end());
    auto m = feed(a, in);
    CHECK(m.size() == 1 && m[0] == off && a.bad() == 1);
  }
  {
    // the realtime bytes beside it (3(c) step 10): F8 FA FB FC anywhere, F2 with its two bytes, the rest not a clock's
    RealtimeAsm r;
    SysExAsm<64> a;
    std::vector<uint8_t> in = {0xFA, 0xF0, 0x7D, 0xF8, 0x52, 0xF7, 0xFE, 0xF2, 0x10, 0x01, 0xFB, 0xF2, 0x05, 0x90, 60, 100,
                               0xF8, 0xFC, 0xF2, 0x7F};
    std::vector<std::pair<uint8_t, uint16_t>> got;
    unsigned sx = 0;
    for (uint8_t b : in) {
      if (r.feed(b)) got.emplace_back(r.status(), r.spp());
      if (a.feed(b)) ++sx;
    }
    const std::vector<std::pair<uint8_t, uint16_t>> want = {{0xFA, 0}, {0xF8, 0}, {0xF2, 0x10 | 1 << 7}, {0xFB, 0}, {0xF8, 0}, {0xFC, 0}};
    CHECK(got == want && sx == 1);   // (F2 05 cut by the note: no position; the last F2 not whole yet)
    CHECK(r.feed(0x02) && r.status() == 0xF2 && r.spp() == (0x7F | 2 << 7));
  }
  {
    // a box's CC 120 (All Sound Off, 3(c)): any channel, running status, realtime between; SysEx data and other CCs are not
    Cc120Asm c;
    std::vector<uint8_t> in = {0xB3, 120, 0,            // 1: channel 4
                               121, 0,                  // reset all controllers (running status): no
                               0xF8, 120, 0xF8, 0,      // 2: running status, a clock byte between each
                               0x90, 120, 0,            // a note of pitch 120: no
                               0xF0, 0x7D, 120, 0, 0xF7,// SysEx data: no
                               120, 0,                  // after F7: no running status
                               0xBF, 7, 100, 120, 127}; // 3: channel 16, after another CC
    int n = 0;
    for (uint8_t b : in) n += c.feed(b) ? 1 : 0;
    CHECK(n == 3);
  }
  printf("test_sysex_asm: ok\n");
  return 0;
}
