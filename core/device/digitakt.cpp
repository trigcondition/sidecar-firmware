// core/device/digitakt.cpp - see digitakt.h. Its mod reports as the Rytm's does, the engine's own 03 / 04 / 11 with P1..P16
// and the machine in the voice byte, so the profile lists those commands and nothing is translated. USB id 1935:0b2c, OVERBRIDGE
// mode (USB MIDI mode is 1935:000d, USB AUDIO/MIDI 1935:102c: no stream here).
//
// The stream's facts [sources in docs/device-profiles.md, "Audio"]: 12 channels / 368 B Digitakt -> host (MAIN L R, tracks 1..8,
// INPUT L R), 2 / 88 B host -> Digitakt (MAIN L R, after everything: stock routes USB to the main output only).
#include "device/digitakt.h"

#include "fx/out.h"


namespace core {
namespace device {

static constexpr uint8_t N = out::kNoChannel;
const out::Device kDigitaktAudio = {"Digitakt", 2, {0, 1}, {0, 1}, {N, N, N, N, N, N, N, N}, {0, 1, 2, 3, 4, 5, 6, 7},
                                    {{0, -1}, {1, -1}, {2, -1}, {3, -1}, {4, -1}, {5, -1}, {6, -1}, {7, -1}}, {10, 11}, 0,
                                    12, out::kStereoMix};

// The mod's controls report, in this order: its FILTER page's page 1 and its AMP page without OVER, DEL, REV. Those three,
// BASE, WDTH, SRR and ROUT reach nothing. TYPE [measured in an emulator, OS 1.53]: 0 OFF, 1 LP, 2 HP, 3..7 EQ_1..EQ_5.
static const Controls kDigitaktControls = {
    13,
    {kCtlFreq, kCtlReso, kCtlType, kCtlFltEnv, kCtlFltA, kCtlFltD, kCtlFltS, kCtlFltR, kCtlAmpA, kCtlAmpH, kCtlAmpD, kCtlVol, kCtlPan},
    {kFltOff, kFltLp, kFltHp, kFltBell, kFltBell, kFltBell, kFltBell, kFltBell}};

const Profile kDigitakt = {
    kIdDigitakt, "Digitakt mk1", 0x1935, 0x0b2c,
    8, 8, 16,
    {0x7D, 0x52, 0x54},
    0x04, 0x03, 0x11,
    0,
    0, 0, {}, {},
    {},   // nothing goes back but the hello's answer: the Digitakt's receive path stays out of it
    nullptr,
    "/.digitakt",
    {1u << 0x03 | 1u << 0x04 | 1u << 0x11},
    kBracketMix, &kDigitaktAudio, &kDigitaktControls};

}  // namespace device
}  // namespace core
