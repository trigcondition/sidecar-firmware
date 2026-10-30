// Plaits' user_data.h replaced for rytm-teensy (the simulator's build puts
// this first on the include path; teensy/setup_plaits_lib.sh copies it into
// the Teensy's library). Plaits' own reads custom wavetables / DX7 banks from
// an STM32 flash address (it would fault here); this one has no stored user
// data, except a DX7 bank handed over for one reload: while fm_user_bank()
// points at 4096 bytes (32 packed voices), the six-op engine of the first
// ROM bank (engine 2) loads them instead of its ROM bank (Voice::Render on a
// new engine or after ReloadUserData). sim/plaits/fm_machine.h sets it around
// such a render and clears it again (per core, RYTM_RENDER_CORES below).
#ifndef PLAITS_USER_DATA_H_
#define PLAITS_USER_DATA_H_
#include <stddef.h>
#include <stdint.h>
// RYTM_RENDER_CORES > 1 (the Pi engine: several cores render voices at once): one bank pointer per core, picked by
// rytm_render_core() (the platform's: the calling core's index, 0..RYTM_RENDER_CORES-1), so a reload on one core
// never hands its bank to a voice reloading on another. Unset (the Teensy, the simulator): one pointer, as before.
// The same macro and function make GRAIN's budget (sample_modes.h), stmlib's Random (shim stmlib/utils/random.h)
// and rand() (core/host/newlib_rand.cpp) per core.
#ifndef RYTM_RENDER_CORES
#define RYTM_RENDER_CORES 1
#endif  // RYTM_RENDER_CORES
#if RYTM_RENDER_CORES > 1
extern "C" int rytm_render_core();
#endif  // RYTM_RENDER_CORES > 1
namespace plaits {

// The calling core's index (0 with one core): its fm_user_bank and its 6-OP FM stage (core/voices.cpp).
inline int fm_core() {
#if RYTM_RENDER_CORES > 1
  return rytm_render_core();
#else
  return 0;
#endif  // RYTM_RENDER_CORES > 1
}

inline const uint8_t*& fm_user_bank() {
  static const uint8_t* bank[RYTM_RENDER_CORES] = {};
  return bank[fm_core()];
}

// With fm_user_bank: bit 0, which of the six-op engine's first two patches the one handed-over voice goes into
// (fm_machine.h alternates them). The engine unloads only the voices on that patch, so a note ringing out on the
// other keeps its patch: a PAT change does not cut it (teensy/setup_plaits_lib.sh). Bit 1: unload every voice (a
// first load after Init / Leave / silence, so a note frozen in the engine meanwhile does not resume).
inline int& fm_user_slot() {
  static int slot[RYTM_RENDER_CORES] = {};
  return slot[fm_core()];
}

class UserData {
 public:
  enum { SIZE = 0x1000 };
  inline const uint8_t* ptr(int slot) const { return slot == 2 ? fm_user_bank() : NULL; }
  inline bool Save(uint8_t* rx_buffer, int slot) { (void)rx_buffer; (void)slot; return false; }
};

}  // namespace plaits
#endif
