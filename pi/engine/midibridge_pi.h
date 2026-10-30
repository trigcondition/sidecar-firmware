// pi/engine/midibridge_pi.h - the Rytm MIDI bridge's Pi side (the portable part and the rules: midibridge.h, the merge:
// usbmidi_merge.h; docs/pi-engine.md "Rytm MIDI bridge", docs/HARDWARE-TESTING.md): TCP port 7330, its two tasks (the
// listener with the challenge, the relay), the hooks in the USB MIDI paths, /status, the web page's GET /midibridge and
// POST /midibridge/set, and the console's `midibridge` commands.
//
// Everything runs on core 0: the tasks, the web and console (Circle's cooperative tasks, never preempting each other),
// CObUsbTransport::Poll (core 0's loop) and FromRytm (the USB MIDI interrupt, on core 0). The control core (3) is not
// involved and nothing here touches FatFs.
#ifndef _pi_engine_midibridge_pi_h
#define _pi_engine_midibridge_pi_h

#include <circle/net/netsubsystem.h>
#include <circle/string.h>
#include <circle/types.h>

#include "midibridge.h"

namespace midibridge_pi {

void Start (CNetSubSystem *pNet);			// the kernel, once, with a network: the two tasks (port 7330)
// the USB MIDI interrupt (CKernel::MIDIPacketHandler): one event packet's MIDI bytes, copied to a connected client
void FromRytm (const u8 *pMidi, unsigned nLength);
// CObUsbTransport::Poll (usbmidi_merge.h): the client's queue for the Rytm, its pace, and the merge's failed sends
midibridge::ToRytm *Queue (void);
midibridge::Pacer *Pace (void);
void SetRytmFail (unsigned n);
void Status (CString *pOut);				// /status's ",\"midibridge\":{...}" (never the token or the address)
// GET /midibridge (paired, same-origin): the state, the client's address, and the token while on; the JSON's length
int WebJson (char *pOut, unsigned nSize);
// POST /midibridge/set: on=, os=, data= 0|1 (0: not given). 0: done, else the refusal's text
const char *WebSet (const char *pOn, const char *pOs, const char *pData);
bool Command (const char *p);	// `midibridge`, `midibridge=on|off`, `midibridge=os:on|off`, `midibridge=data:on|off`
void PairingReset (void);				// every device unpaired (pair=revoke, POST /pair/revoke): closed, off

}

#endif
