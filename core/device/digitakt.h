// core/device/digitakt.h - the Digitakt mk1 with the sidecar-digitakt mod: not official support (docs/device-profiles.md).
// Eight tracks as eight Sidecar voices, summed into the two channels back to it. Checked in an emulator and in host tests only,
// never on a Digitakt.
#pragma once
#include "device/device.h"

namespace core {
namespace device {
constexpr uint16_t kIdDigitakt = 2;
extern const Profile kDigitakt;   // kBracketMix: one stereo mix back, its FILTER and AMP pages play the Sidecar's
}  // namespace device
}  // namespace core
