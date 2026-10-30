// core/voice_name.h - a Sidecar voice is named for the Rytm track(s) that play it when it is shown to the user, never a
// number (voice 1 is BT's, voice 2 SD's; the routing is core/ui/remote.cpp kTrackVoice). The console, the wire and the
// card files keep "V1".."V8".
#pragma once

namespace core {

inline const char* voice_name(int v) {
  static const char* const k[8] = {"BD", "BT", "SD", "LT", "CH/OH", "MT/HT", "CY/CB", "RS/CP"};
  return k[v & 7];
}

}  // namespace core
