#pragma once
#include <stdint.h>
#include <vector>
// the looper's hooks as ob_usb.cpp calls them, recorded (the real ones: ../../loop_pi.h)
namespace loop_pi {
struct InCall {
  unsigned slot;
  std::vector<int32_t> lr;
  unsigned lost;
  uint64_t arrival;
};
inline std::vector<InCall> g_in;
inline bool g_wants = false;
inline std::vector<uint64_t> g_out_frames;                          // the render frame each OutMix got
inline std::vector<unsigned> g_out_slots;                           // the slot each OutMix got
inline void (*g_out_hook)(int32_t (*)[12], unsigned) = nullptr;     // what the loop adds
inline bool WantsIn(unsigned) { return g_wants; }
inline void InBlock(unsigned slot, const int32_t* lr, unsigned n, unsigned lost, uint64_t arrival) { g_in.push_back({slot, std::vector<int32_t>(lr, lr + 2 * n), lost, arrival}); }
inline void OutMix(unsigned slot, int32_t (*F)[12], unsigned n, uint64_t render_frame) {
  g_out_frames.push_back(render_frame), g_out_slots.push_back(slot);
  if (g_out_hook) g_out_hook(F, n);
}
inline std::vector<unsigned> g_box_slots;                           // the slot each SetBox got
inline void SetBox(unsigned slot, const void*) { g_box_slots.push_back(slot); }
inline void Poll() {}
}  // namespace loop_pi
