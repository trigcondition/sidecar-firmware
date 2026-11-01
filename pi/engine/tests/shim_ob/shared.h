#pragma once
#include <stdint.h>
#include "output.h"
#include "records.h"
#include "spsc.h"
struct TToHostStub {
  const uint8_t* Next(int*) { return nullptr; }
  unsigned Drain() { return 0; }
};
inline Output g_Output;
inline TToHostStub g_ToHost;
inline Spsc<uint32_t, 512> g_KeysToRytm;
