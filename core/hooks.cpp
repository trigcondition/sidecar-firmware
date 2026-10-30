// core/hooks.cpp - see hooks.h.
#include "hooks.h"

#include "platform.h"

namespace core {

bool user_report_internal = false;
Hooks hooks = {};
static uint32_t g_dropped = 0;

uint32_t to_rytm_dropped() { return g_dropped; }

CORE_COLD void to_rytm(const uint8_t* m, int n) {   // per message: cold
  if (hooks.to_rytm) hooks.to_rytm(m, n);
  else ++g_dropped;
}

}  // namespace core
