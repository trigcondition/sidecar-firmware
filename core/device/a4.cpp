// core/device/a4.cpp - see a4.h. Its mod reports as the Rytm's does, the engine's own 03 / 04 / 11 with P1..P16 and the machine in the voice byte, so the
// profile lists those commands and nothing is translated.
//
// The stream [read in OS 1.56]: host -> A4 6 channels / 200 B. 0, 1 MAIN: added to the effects' return on the DAC, after everything but the analog output
// stage, through USB TO MAIN's gain. 2..5 the synth voices: each added to that voice's own DAC word, the one its digital noise takes, so a Sidecar voice goes
// through the voice's analog filters and amp, and from there to the A4's chorus, delay and reverb: these are the per-channel insert points, as on the Rytm. The gain
// there follows the voice's amp, from 0.25 up, and the analog VCA must be open: a track that plays. A4 -> host 8 channels / 256 B: MAIN L R, the voices 1..4,
// INPUT L R (names from Overwitch; a unit showed MAIN on 0 / 1, its voices on theirs). The loop: 197 frames, 4.1 ms, a voice's tap to its channel on IN [measured on a unit, voice 1; 191 voice 3]. The driver widens that block to the
// 12 channels its consumers take: 0..5 as they are, 6..9 silent, INPUT on 10 / 11. So this row's `audio_in` is the wide block's.
#include "device/a4.h"

#include "fx/out.h"

namespace core {
namespace device {

static constexpr uint8_t N = out::kNoChannel;
const out::Device kA4Audio = {"Analog Four MKII", kA4OutChannels, {0, 1}, {0, 1}, {2, 3, 4, 5, N, N, N, N}, {0, 1, 2, 3, 4, 5, 6, 7},
                              {{0, -1}, {1, -1}, {2, -1}, {3, -1}, {-1, -1}, {-1, -1}, {-1, -1}, {-1, -1}}, {10, 11}, 197,
                              kA4InChannels, out::kPerVoice};

static const Screen kA4Screen = {{"T1", "T2", "T3", "T4", "-", "-", "-", "-"}, "UP + YES: PAGES"};

const Profile kA4 = {
    kIdA4, "Analog Four MKII", kA4Vid, kA4Pid,
    6, 4, 16,
    {0x7D, 0x52, 0x54},
    0x04, 0x03, 0x11,
    kUserSlots | kRemoteUi,   // U1..U5 on its SIDECAR page; the web's User machines tab fills them, in /.a4/slots. Remote UI: the pages on its screen, text drawn here
    0, 0, {}, {},
    {0, 1u << (0x2E - 32) | 1u << (0x2F - 32) | 1u << (0x34 - 32) |   // 3(c): its sequencer's 2E mask, 2F PAGE LEDs, 34 trig LEDs (sidecar-a4 0.14)
            1u << (0x36 - 32) | 1u << (0x37 - 32) | 1u << (0x38 - 32) | 1u << (0x39 - 32) | 1u << (0x3A - 32) | 1u << (0x3C - 32)},   // the pages alone: 36 37 38 39 3A 3C, and the hello's answer
    nullptr,
    "/.a4",
    {1u << 0x03 | 1u << 0x04 | 1u << 0x11, 1u << (0x2C - 32) | 1u << (0x2D - 32) |   // 3(c): 2C step ticks, 2D state (companion 0.3.0, sidecar-a4 0.14)
            1u << (0x30 - 32) | 1u << (0x31 - 32) | 1u << (0x32 - 32) | 1u << (0x3B - 32)},   // 3B its kit; 30 31 32 its screen
    kBracketFull, &kA4Audio, nullptr,
    true, true,
    180,   // +18 dB: the insert point's gain rests at 0.25 [on a unit: MAIN peaks 0.17-0.36 at +12 with two tracks, still under the A4's own oscillators by ear; +18 set live, the owner's ear]; peaks past full scale go through Own's knee
    true,   // a chord on any track: the mod reports each note. All of it stays on the voice's channel, and that enters one analog voice
    &kA4Screen,
    "A4"};

}  // namespace device
}  // namespace core
