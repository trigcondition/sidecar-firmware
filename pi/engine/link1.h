// pi/engine/link1.h - the second box's USB MIDI: link 1 of core/device/device.h (docs/device-profiles.md "Several devices").
// One Elektron box on USB that is not the host link (the Rytm's, link 0) gets its own queues: its bytes to the control core,
// where each whole SysEx is handled as link 1's (core::engine_sysex_link), and the engine's replies to link 1 back to it as
// records, sent from core 0's loop. Nothing here touches link 0's queues (g_MidiIn / g_MidiOut), so a Rytm alone runs as it did.
//
//   core 0, USB IRQ   Packet         the box's MIDI bytes -> g_Link1In
//   core 0, loop      Send           g_Link1Out's records -> the box's MIDI OUT, packed as USB MIDI event packets
//   core 0, loop      SetDevice      the box came (its CUSBMIDIDevice) or went (0)
//   control core      Poll           g_Link1In -> whole SysEx -> engine_sysex_link(1, ...); the sender handed to
//                                    core::device::set_out(1, ...) while a device is there; its MIDI clock (realtime
//                                    bytes, song position) to pRealtime as transport 1's (3(c) step 10)
#ifndef _link1_h
#define _link1_h
#include <stdint.h>

#include "spsc.h"
#include "records.h"
#include "sysex_asm.h"

class CUSBMIDIDevice;

namespace link1 {

const unsigned kInBytes = 4096, kOutBytes = 8192;
extern Spsc<uint8_t, kInBytes> g_In;
extern Spsc<uint8_t, kOutBytes> g_Out;

struct TStats {
	unsigned nInLost;	// its bytes lost: g_In full
	unsigned nOutFull;	// engine bytes for it lost: g_Out full, or a record > 4096
	unsigned nSent, nSendFail;	// SysEx sent to it; failed sends (the message dropped)
	unsigned nSysEx, nBadSysEx;	// whole SysEx handed to the engine; pieces dropped (too long, broken)
	unsigned nRealtime;		// realtime bytes and song positions handed on (its clock)
};
const TStats &Stats (void);

void Packet (unsigned nCable, uint8_t *pPacket, unsigned nLength);
void SetDevice (CUSBMIDIDevice *pDevice);
CUSBMIDIDevice *Device (void);
void Send (void);
typedef void TRealtime (int nTransport, uint8_t nStatus, uint16_t nSPP);
typedef void TAllSoundOff (int nTransport);
typedef void TByte (uint8_t nByte);
void Poll (TRealtime *pRealtime = 0, TAllSoundOff *pAllSoundOff = 0, TByte *pByte = 0);	// pByte: each byte of the box's (its notes: cores.cpp)	// pRealtime: the box's F8 FA FB FC, and F2 with its position in 16ths
void ToBox (const uint8_t *pData, int nLength);	// core::device::set_out(1, ...): records into g_Out

// usb=mirror through a Teensy relay (teensy_target.cpp, docs/teensy-mirror.md): the box's USB MIDI to and from the relay's
// computer, beside the engine. Event packets (4 bytes, cable + CIN first), as on the wire.
typedef void TTap (const uint8_t *pEventPacket);
void SetTap (TTap *pTap);			// each packet the box sends, also to pTap (0: none); core 0, task or loop
void Tap (const uint8_t *pEventPacket);		// xHCI IRQ: the box's packet handler (kernel.cpp), before anything else
const unsigned kFromComputerPackets = 2048;	// as the Rytm's (sidecar_gadget.h MIDIQueue, teensy_target.h RytmQueue)
bool FromComputer (const uint8_t *pEventPacket);	// xHCI IRQ: one packet for the box; false: the queue is full (dropped)
unsigned FromComputerRoom (void);		// packets that fit now
// usbmidi=os: on the relay (core 0's loop): the computer's packets lost on the way (a failed send, no box), whether any
// still wait or a SysEx of its is part way out, and dropping all of what waits (the count)
unsigned ComputerLost (void);
bool ComputerBusy (void);
unsigned CutFromComputer (void);

}  // namespace link1

#endif
