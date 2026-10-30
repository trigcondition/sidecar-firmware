// pi/engine/tests/test_usbmidi_pack.cpp - the engine's SysEx for the Rytm as USB MIDI event packets
// (usbmidi_pack.h): every length 2..300 decodes back (a USB MIDI 1.0 receiver: CIN 4 three bytes, 5/6/7 one/two/
// three and the end), cable 0, padding 0, the core's 13-byte 1D and 43-byte 1A exactly, and refusals.
#include <vector>

#include "check.h"
#include "usbmidi_pack.h"

static std::vector<uint8_t> unpack(const uint8_t* p, int n, bool* ended) {
  std::vector<uint8_t> out;
  *ended = false;
  for (int i = 0; i < n; i += 4) {
    CHECK((p[i] >> 4) == 0);   // cable 0
    int cin = p[i] & 15, k = cin == 4 ? 3 : cin == 5 ? 1 : cin == 6 ? 2 : cin == 7 ? 3 : -1;
    CHECK(k > 0);
    CHECK(!*ended);            // nothing after the end
    for (int j = 0; j < 3; ++j) {
      if (j < k) out.push_back(p[i + 1 + j]);
      else CHECK(p[i + 1 + j] == 0);
    }
    if (cin != 4) *ended = true;
  }
  return out;
}

int main() {
  for (int n = 2; n <= 300; ++n) {
    std::vector<uint8_t> m(n);
    m[0] = 0xF0;
    for (int i = 1; i < n - 1; ++i) m[i] = uint8_t(i * 7 & 0x7F);
    m[n - 1] = 0xF7;
    std::vector<uint8_t> p(usbmidi_sysex_bytes(n) + 4, 0xAA);
    int b = usbmidi_pack_sysex(m.data(), n, p.data());
    CHECK(b == usbmidi_sysex_bytes(n));
    CHECK(p[b] == 0xAA);       // nothing written past it
    bool ended;
    CHECK(unpack(p.data(), b, &ended) == m);
    CHECK(ended);
  }
  // the 13-byte 1D (core/sysex.cpp's sysex_slots(m, 0, 0)): 5 packets, the last CIN 5 with F7 alone
  const uint8_t d1[13] = {0xF0, 0x7D, 0x52, 0x54, 0x1D, 1, 1, 1, 1, 1, 1, 1, 0xF7};
  uint8_t p[256];
  CHECK(usbmidi_pack_sysex(d1, 13, p) == 20);
  const uint8_t want[20] = {4, 0xF0, 0x7D, 0x52, 4, 0x54, 0x1D, 1, 4, 1, 1, 1, 4, 1, 1, 1, 5, 0xF7, 0, 0};
  for (int i = 0; i < 20; ++i) CHECK(p[i] == want[i]);
  // 43 bytes (a 1A part 0): 15 packets, the last CIN 5
  uint8_t a[43] = {0xF0};
  a[42] = 0xF7;
  CHECK(usbmidi_pack_sysex(a, 43, p) == 60 && p[56] == 5 && p[57] == 0xF7);
  // 3n bytes end on CIN 7
  const uint8_t s6[6] = {0xF0, 1, 2, 3, 4, 0xF7};
  CHECK(usbmidi_pack_sysex(s6, 6, p) == 8 && p[0] == 4 && p[4] == 7 && p[7] == 0xF7);
  // refused: not F0 .. F7, a status byte inside, too short
  const uint8_t bad1[3] = {0x90, 60, 100}, bad2[4] = {0xF0, 1, 2, 3}, bad3[5] = {0xF0, 1, 0xF7, 2, 0xF7};
  CHECK(usbmidi_pack_sysex(bad1, 3, p) == 0);
  // a CC (the LFOs, core/lfo.h): one packet, CIN 0xB, cable 0; a SysEx through usbmidi_pack as before; junk refused
  const uint8_t cc[3] = {0xB1, 74, 99};
  CHECK(usbmidi_pack(cc, 3, p) == 4 && p[0] == 0x0B && p[1] == 0xB1 && p[2] == 74 && p[3] == 99);
  CHECK(usbmidi_pack(s6, 6, p) == 8 && p[0] == 4);
  const uint8_t badcc[3] = {0xB1, 0x80, 1}, rt[3] = {0xF8, 0, 0};
  CHECK(usbmidi_pack(badcc, 3, p) == 0 && usbmidi_pack(rt, 3, p) == 0);
  CHECK(usbmidi_pack_sysex(bad2, 4, p) == 0);
  CHECK(usbmidi_pack_sysex(bad3, 5, p) == 0);
  CHECK(usbmidi_pack_sysex(bad2, 1, p) == 0);
  // core/song: a program change (2 bytes), one packet CIN 0xC; a 3-byte record with a PC status refused
  const uint8_t pc[2] = {0xCD, 5}, pc3[3] = {0xCD, 5, 0};
  CHECK(usbmidi_pack(pc, 2, p) == 4 && p[0] == 0x0C && p[1] == 0xCD && p[2] == 5 && p[3] == 0);
  CHECK(usbmidi_pack(pc3, 3, p) == 0);
  // fix1041: the replay's STOP at the take's end (1 byte, realtime): CIN 0xF; a 1-byte non-realtime refused
  const uint8_t fc[1] = {0xFC}, one[1] = {0xF6};
  CHECK(usbmidi_pack(fc, 1, p) == 4 && p[0] == 0x0F && p[1] == 0xFC && p[2] == 0 && p[3] == 0);
  CHECK(usbmidi_pack(one, 1, p) == 0);
  // a 15000-byte dump in 3069-byte pieces: each piece packs, the packets together decode to the dump
  {
    std::vector<uint8_t> m(15000);
    m[0] = 0xF0;
    for (size_t i = 1; i + 1 < m.size(); ++i) m[i] = uint8_t(i * 13 & 0x7F);
    m.back() = 0xF7;
    std::vector<uint8_t> all;
    for (size_t i = 0; i < m.size(); i += 3069) {
      size_t k = m.size() - i < 3069 ? m.size() - i : 3069;
      std::vector<uint8_t> q(4 * ((k + 2) / 3));
      int b = usbmidi_pack(m.data() + i, (int)k, q.data());
      CHECK(b == (int)q.size());
      all.insert(all.end(), q.begin(), q.end());
    }
    bool ended;
    CHECK(unpack(all.data(), (int)all.size(), &ended) == m && ended);
    const uint8_t odd[4] = {0xF0, 1, 2, 3}, mid[3] = {1, 0x90, 2};
    CHECK(usbmidi_pack(odd, 4, p) == 0 && usbmidi_pack(mid, 3, p) == 0);   // not 3-aligned; a status inside
  }
  printf("test_usbmidi_pack: ok\n");
  return 0;
}
