// pi/engine/tests/test_usb_box.cpp - which USB MIDI device is the host link: the strings as the firmware images and a real Analog Four MKII give them.
#include "check.h"
#include "usb_box.h"
#include "hello_gate.h"
#include "box_list.h"

int main() {
  const char* v = "Elektron Music Machines";
  CHECK(usb_box(v, "Elektron Analog Rytm MKII") == UsbBoxHost);
  CHECK(usb_box(v, "Elektron Analog Rytm") == UsbBoxHost);
  CHECK(usb_box(v, "Elektron Digitakt") == UsbBoxHost);
  CHECK(usb_box(0, 0) == UsbBoxHost);   // no strings: as before
  CHECK(usb_box("", "") == UsbBoxHost);
  CHECK(usb_box(0, "Analog Rytm MKII MIDI") == UsbBoxHost);
  // what the engine's replies must not reach
  CHECK(usb_box(v, "Elektron Analog Four MKII") == UsbBoxHello);
  CHECK(usb_box(v, "Elektron Analog Keys") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Analog Four") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Digitakt II") == UsbBoxOther);
  CHECK(usb_box(v, "Elektron Syntakt") == UsbBoxOther);
  CHECK(usb_box(v, 0) == UsbBoxOther);  // an Elektron box of no known name is not the Rytm
  CHECK(usb_box(v, "") == UsbBoxOther);
  CHECK(usb_box(0, "Elektron Analog Four MKII") == UsbBoxHello);
  // everybody else stays a keyboard
  CHECK(usb_box("Arturia", "KeyStep") == UsbBoxKeyboard);
  CHECK(usb_box(0, "USB MIDI") == UsbBoxKeyboard);
  CHECK(usb_box("Somebody", 0) == UsbBoxKeyboard);
  {  // the gate before an A4 is the box in use: whole hellos, nothing else
    HelloGate g;
    const unsigned char hello[] = {0xF0, 0x7D, 0x52, 0x54, 0x4F, 1, 0, 3, 1, 'A', '4', 0xF7};
    const unsigned char noise[] = {0x90, 60, 100, 0xF8, 0xF0, 0x00, 0x20, 0x3C, 0x06, 0x00, 0x7F, 0xF7, 0xF0, 0x7D, 0x52, 0x54, 0x04, 0, 60, 100, 0xF7};
    unsigned got = 0;
    for (unsigned char b : noise) got += g.feed(b);
    CHECK(got == 0);
    for (unsigned k = 0; k < sizeof hello; ++k) got = g.feed(hello[k]);
    CHECK(got == sizeof hello && !memcmp(g.data(), hello, got));
    for (unsigned k = 0; k < sizeof hello - 1; ++k) got = g.feed(hello[k]);   // a clock inside it, then a cut
    CHECK(g.feed(0xF8) == 0 && g.feed(0x90) == 0 && g.feed(0xF7) == 0);
    for (int k = 0; k < 100; ++k) g.feed(k == 0 ? 0xF0 : 0x11);               // too long: dropped, no overrun
    CHECK(g.feed(0xF7) == 0);
  }
  {  // the boxes for /status: two of a kind are two rows, the served box is there while unplugged, a USB string cannot break the JSON
    CHECK(usb_box_id("Elektron Analog Rytm MKII") == 1 && usb_box_id("Elektron Digitakt") == 2 && usb_box_id("Elektron Analog Four MKII") == 3);
    CHECK(usb_box_id("Elektron Digitakt II") == 0 && usb_box_id("Elektron Analog Four") == 0 && usb_box_id(0) == 0);
    auto caps = [](uint16_t id, BoxCaps* c) {
      if (id != 3) return false;
      *c = {6, 4, 16, 0, "/.a4"};
      return true;
    };
    char b[1536];
    BoxList l;
    CHECK(l.json(b, sizeof b, 3, "Analog Four MKII", caps) > 0);
    CHECK(!strcmp(b, ",\"boxes\":[{\"key\":\"3-0\",\"id\":3,\"n\":1,\"name\":\"Analog Four MKII\",\"role\":\"host\",\"on\":0,"
                     "\"tracks\":6,\"voices\":4,\"params\":16,\"feat\":0,\"state\":\"/.a4\"}]"));
    l.add(3, 4, BoxSpare, "Elektron Analog Four MKII");
    l.add(1, 3, BoxSpare, "Elektron Analog Rytm MKII");
    l.add(3, 2, BoxHost, "Elektron Analog Four MKII");
    CHECK(l.json(b, sizeof b, 3, "Analog Four MKII", caps) > 0);
    const char* rytm = strstr(b, "\"key\":\"1-3\""), *a1 = strstr(b, "\"key\":\"3-2\""), *a2 = strstr(b, "\"key\":\"3-4\"");
    CHECK(rytm && a1 && a2 && rytm < a1 && a1 < a2);          // type, umidiN
    CHECK(strstr(a1, "\"n\":1,") == a1 + strlen("\"key\":\"3-2\",\"id\":3,") && strstr(a2, "\"n\":2,"));
    CHECK(!strstr(rytm, "\"on\":0"));                         // the served A4 is here: no row of its own for it
    BoxList e;
    e.add(0, 2, BoxOther, "ev\"il\\\n<b>");
    CHECK(e.json(b, sizeof b, 1, "Analog Rytm MKII", caps) > 0);
    CHECK(strstr(b, "\"name\":\"evil<b>\""));
    {   // a box's whole USB name, not "Elektron Analog Four MK"
      BoxList l;
      l.add(3, 1, BoxHost, "Elektron Analog Four MKII");
      char j[256];
      CHECK(l.json(j, sizeof j, 3, "x", nullptr) != 0 && strstr(j, "\"name\":\"Elektron Analog Four MKII\""));
    }
    CHECK(!strstr(b, "\"tracks\""));                           // no profile, no counts
    BoxList f;
    for (unsigned k = 0; k < BoxList::kMax + 3; ++k) f.add(1, (uint8_t)k, BoxSpare, "Elektron Analog Rytm MKII with a long name");
    CHECK(f.json(b, sizeof b, 3, "Analog Four MKII", caps) > 0);   // the kernel's buffer holds a full list and the served row
    char tiny[64];
    CHECK(f.json(tiny, sizeof tiny, 3, "Analog Four MKII", caps) == 0);
  }
  {   // two Rytms (design 2026-11-26): keyed by their UIDs, each with its name
    BoxList l;
    l.add(1, 2, BoxSpare, "Elektron Analog Rytm MKII", 0x9f003fa2, "Live rig");
    l.add(1, 1, BoxHost, "Elektron Analog Rytm MKII", 0x1234abcd, "Rytm abcd");
    char j[1536];
    CHECK(l.json(j, sizeof j, 1, "Analog Rytm MKII", nullptr) > 0);
    const char* a = strstr(j, "{\"key\":\"1-1234abcd\",\"id\":1,\"uid\":\"1234abcd\",\"label\":\"Rytm abcd\",\"n\":1,");
    const char* b = strstr(j, "{\"key\":\"1-9f003fa2\",\"id\":1,\"uid\":\"9f003fa2\",\"label\":\"Live rig\",\"n\":2,");
    CHECK(a && b && a < b);
  }
  printf("test_usb_box: ok\n");
  return 0;
}
