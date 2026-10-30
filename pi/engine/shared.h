// pi/engine/shared.h - what the cores share (one definition, shared.cpp): the output stage (control core -> the
// transport), the host's MIDI bytes (the transport -> control core), the engine's bytes for the host (control
// core -> core 0's transport), the console's commands (core 0 -> control core). Every queue has one producer and
// one consumer;
// g_MidiOut is read only through g_ToHost (records.h), whichever transport consumes it.
#ifndef _shared_h
#define _shared_h
#include <stdint.h>

#include "output.h"
#include "records.h"
#include "spsc.h"

// g_MidiOut: a burst of replies (0x10: pool table, JUNO and FM names; a 0D listing) fits whole while core 0 sends
const unsigned kMidiOutBytes = 32768;

struct TCmd { char text[192]; };   // pool=S:<a path under /Samples> is the longest

extern Output g_Output;
extern Spsc<uint8_t, 8192> g_MidiIn;
extern Spsc<uint8_t, kMidiOutBytes> g_MidiOut;
extern RecordReader<Spsc<uint8_t, kMidiOutBytes>> g_ToHost;	// g_MidiOut's only reader: every consumer takes whole records
extern Spsc<TCmd, 16> g_Cmds;
// The Pi's own keyboards (core/keys.h): their channel messages for the control core, each (src << 24 | status << 16 |
// d1 << 8 | d2) - src 0..3 a USB-A keyboard, 4 the computer; status 0xF4 a source plugged in (d1 1) / out (d1 0), its
// name in g_KeyNames[src] (written before the push). Producers: core 0's USB IRQs (xHCI, the gadget: they do not nest)
// and core 0's loop with IRQs masked. And the notes core::keys sends to the Rytm (control core -> core 0's
// transport), USB MIDI event packets (cable 0).
extern Spsc<uint32_t, 1024> g_KeysIn;
extern Spsc<uint32_t, 512> g_KeysToRytm;
extern char g_KeyNames[5][24];
// The auto-sampler's notes for an external synth (core/samp/autosamp.h kAExt; control core -> core 0's loop, which sends
// them): USB MIDI event packets, the cable nibble the output (1 the computer on the Sidecar's USB-C MIDI, 2 the first USB
// MIDI device on the USB-A ports; sent on cable 0). g_bComputerMidi: the Sidecar gadget (USB-C) is up (core 0 sets it).
extern Spsc<uint32_t, 256> g_AsampMidi;
extern volatile bool g_bComputerMidi;

#endif
