#pragma once
#include "midibridge.h"
namespace midibridge_pi {
inline midibridge::ToRytm* Queue() { static midibridge::ToRytm q; return &q; }
inline midibridge::Pacer* Pace() { static midibridge::Pacer p; return &p; }
inline void SetRytmFail(bool) {}
}
