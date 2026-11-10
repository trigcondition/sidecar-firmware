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

// The remote UI on its screen (128 x 64, the Rytm's Bitmap layout [read in OS 1.53 and 1.54]): SCOPE and the meters on IN 2 + the voice, its tracks 1..8.
static const Screen kDigitaktScreen = {{"T1", "T2", "T3", "T4", "T5", "T6", "T7", "T8"}, "UP + YES: PAGES"};

const Profile kDigitakt = {
    kIdDigitakt, "Digitakt mk1", 0x1935, 0x0b2c,
    8, 8, 16,
    {0x7D, 0x52, 0x54},
    0x04, 0x03, 0x11,
    kUserSlots | kRemoteUi,   // U1..U5 on its SRC list, their labels from 19 / 1A, filled by the web's User machines tab or the USER SLOTS page, in
                              // /.digitakt/slots. Remote UI: the pages on its screen, text drawn here (mod 0.3: before it the mod had no receive path)
    0, 0, {}, {},
    // what goes back: the slots' 1A labels and 1D generations, the pages alone (36 37 38 39 3A 3C), and the hello's answer
    {1u << 0x1A | 1u << 0x1D, 1u << (0x36 - 32) | 1u << (0x37 - 32) | 1u << (0x38 - 32) | 1u << (0x39 - 32) | 1u << (0x3A - 32) | 1u << (0x3C - 32)},
    nullptr,
    "/.digitakt",
    {1u << 0x03 | 1u << 0x04 | 1u << 0x11 | 1u << 0x19, 1u << (0x30 - 32) | 1u << (0x31 - 32) | 1u << (0x32 - 32)},   // 19 a slot's labels; 30 31 32 its screen
    kBracketMix, &kDigitaktAudio, &kDigitaktControls, false, false, 0, false, &kDigitaktScreen, "Digitakt"};

}  // namespace device
}  // namespace core
