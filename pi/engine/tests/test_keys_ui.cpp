// pi/engine/tests/test_keys_ui.cpp - bridge::keys_ui (Settings > MIDI on the touchscreen, 0.5.0) against the core's Mac
// library: the route, the auto channel, the computer's notes; the USB-A devices' names joined (the computer left out) and
// cut at the buffer; the last key's way without "KEYS " ("" before the first key); the MIDI-learn routes, -1 when
// learn.txt is a newer build's (locked).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "bridge.h"
#include "check.h"
#include "core.h"
#include "keys.h"
#include "mod/learn.h"

static void fwd(uint8_t, uint8_t, uint8_t) {}

struct Ui {
  int mode = -2, auto_ch = -2, mac = -2, learn = -2;
  char in[48], last[40];
};
static Ui read(int in_cap = 48) {
  Ui u;
  memset(u.in, 'x', sizeof u.in), memset(u.last, 'x', sizeof u.last);
  bridge::keys_ui(&u.mode, &u.auto_ch, &u.mac, u.in, in_cap, u.last, sizeof u.last, &u.learn);
  return u;
}

int main() {
  char tmpl[] = "/tmp/pi-keysui-XXXXXX";
  CHECK(mkdtemp(tmpl));
  std::string card = tmpl;
  CHECK(system(("mkdir -p '" + card + "/.rytm'").c_str()) == 0);
  core::host_set_card(tmpl);
  core::keys::reset();
  core::learn::reset();
  core::keys::set_forward(fwd);

  Ui u = read();   // the defaults, nothing plugged in
  CHECK(u.mode == 0 && u.auto_ch == 14 && u.mac == 1 && !strcmp(u.in, "") && !strcmp(u.last, "") && u.learn == 0);

  char o[200];
  CHECK(core::keys::command("keys=channel", o, sizeof o) && core::keys::command("keys=auto:10", o, sizeof o) &&
        core::keys::command("keys=mac:off", o, sizeof o));
  core::keys::source(0, true, "MPK mini");
  core::keys::source(2, true, "Launch Control XL");
  core::keys::source(4, true, "IAC");   // the computer: not a USB-A device
  u = read();
  CHECK(u.mode == 1 && u.auto_ch == 10 && u.mac == 0 && !strcmp(u.in, "MPKMINI, LAUNCHCONTROLXL"));
  CHECK(!strcmp(u.last, ""));   // a keyboard in, no key yet: nothing (not "KEYS BY CHANNEL")
  u = read(12);                 // cut at the buffer, terminated
  CHECK(strlen(u.in) == 11 && !strncmp(u.in, "MPKMINI, LA", 11));

  core::keys::midi(0, 0x92, 60, 100);   // channel 3 (by channel): RS, the Rytm's
  u = read();
  CHECK(strncmp(u.last, "KEYS ", 5) != 0 && strstr(u.last, "MPKMINI > RS") == u.last);

  CHECK(core::learn::arm("fader") && core::learn::midi(1, 0xB0, 7, 64));
  CHECK(read().learn == 1);
  FILE* f = fopen((card + "/.rytm/learn.txt").c_str(), "wb");
  CHECK(f);
  fputs("#sidecar learn 1\nR 1 1 7 fader\nZ newer\n", f);
  fclose(f);
  core::learn::load();
  CHECK(core::learn::locked() && read().learn == -1);

  core::keys::reset();
  core::learn::reset();
  system(("rm -rf '" + card + "'").c_str());
  printf("test_keys_ui: PASS\n");
  return 0;
}
