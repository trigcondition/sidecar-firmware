// core/slots.cpp - see slots.h.
#include "slots.h"

#include "hooks.h"
#include "host_link.h"
#include "platform.h"
#include "sysex.h"

namespace core {

CORE_COLD void slots_ask(const uint8_t* d, int len) {
  uint8_t m[43];
  if (len == 11 && d[10] == 0xF7 && d[5] == 1 && d[6] == 0 && d[8] == 3) {
    slots_1d(1000);   // a Rytm that never got a 1D: seen before its list
    to_rytm(m, sysex_installed_empty(m));
    return;
  }
  if (len != 11 || d[10] != 0xF7 || d[5] != 1 || d[6] < 1 || d[6] > 5 || d[8] != 0) return;
  to_rytm(m, d[7] == 1 ? sysex_slot_unchanged(m, d[6]) : sysex_slot_empty(m, d[6]));
}

// A 1E: 1D with every generation 1 and the slot's answer NOT INSTALLED (sim/protocol.py's slots()).
CORE_COLD void slots_assign(const uint8_t* d, int len) {
  if (len != 11 || d[10] != 0xF7 || d[5] != 1 || d[6] < 1 || d[6] > 5 || d[7] > 2) return;
  uint8_t m[16];
  to_rytm(m, host_slots_caps(m, sysex_slots(m, d[6], 1)));
}

// The 13-byte 1D unless the last went out under gap ms ago (true when sent).
CORE_COLD bool slots_1d(uint32_t gap) {
  static uint32_t t = 0;
  static bool sent = false;
  if (sent && now_ms() - t < gap && !host_resync_due()) return false;
  sent = true;
  t = now_ms();
  uint8_t m[16];
  to_rytm(m, host_slots_caps(m, sysex_slots(m, 0, 0)));   // (the host's caps: params-delta, host_link.h)
  return true;
}

}  // namespace core
