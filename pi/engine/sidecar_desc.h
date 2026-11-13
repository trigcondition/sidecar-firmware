// pi/engine/sidecar_desc.h - the Sidecar device's configuration descriptor on USB-C (CLASS COMPLIANT; sidecar_gadget.cpp
// serves it): UAC2 + USB MIDI as before, and with bNet the USB-C network's CDC-NCM function after them
// (usbnet_cfg.h, docs/usb-network.md). Portable, header only (tests/test_sidecar_desc.cpp: without the network the
// bytes are exactly those of 0.4.2's BuildConfig, and the whole composite device is walked).
#ifndef _sidecar_desc_h
#define _sidecar_desc_h

#include <stdint.h>
#include <string.h>

#include "usbnet_cfg.h"

namespace sidecar_desc {

enum { StrManufacturer = 1, StrProduct, StrSerial, StrAudio, StrMIDI, StrNet, StrNetMac, StrOutCh = 16, StrInCh = 32 };

// interfaces 0..4 audio + MIDI (unchanged), 5..6 the network; its endpoints 0x86 (notify), 0x87 + 0x07 (data): EP 6 and
// 7 were free, and 7 IN + 7 OUT share a number as the mirror's box endpoints do (circle-gadget-mirror.patch)
constexpr uint8_t kNetIface = 5, kEpNotify = 0x86, kEpNetIn = 0x87, kEpNetOut = 0x07;
constexpr unsigned kCap = 512;				// 318 bytes (12 in) / 318 (20 in) without the network, + 85 with it

struct Offsets { unsigned out, fb, in, midi_out, midi_in; usbnet::DescOffsets net; };

// UAC2 (Audio Device Class 2.0 4.7-4.10, the Teensy T1 layout): interfaces 0 AC, 1 AS OUT (EP 0x01, feedback 0x82),
// 2 AS IN (EP 0x83); one internal fixed 48 kHz clock 0x10; IT 0x01 (USB) -> OT 0x02, IT 0x03 -> OT 0x04 (USB); raw
// channels (bmChannelConfig 0) named by strings. USB MIDI 1.0: interfaces 3 AC, 4 MS (bulk 0x04 / 0x85), one cable.
// Its length (0: does not fit nCap).
inline unsigned build (uint8_t *pBuf, unsigned nCap, unsigned nIn, unsigned nOut, unsigned nMaxIn, unsigned nMaxOut, bool bNet,
		       Offsets *pOff)
{
	if (nCap < kCap) return 0;
	const unsigned Clock = 0x10, AcCs = 9 + 8 + 17 + 12 + 17 + 12, MsCs = 7 + 6 + 6 + 9 + 9 + 9 + 5 + 9 + 5;
	const uint8_t kConfiguration = 2, kInterface = 4, kEndpoint = 5;
	unsigned n = 0;
#define D(...)	do { const uint8_t d_[] = {__VA_ARGS__}; memcpy (pBuf + n, d_, sizeof d_); n += sizeof d_; } while (0)
#define U16(v)	(uint8_t) ((v) & 0xFF), (uint8_t) (((v) >> 8) & 0xFF)
#define U32(v)	(uint8_t) ((v) & 0xFF), (uint8_t) (((v) >> 8) & 0xFF), (uint8_t) (((v) >> 16) & 0xFF), (uint8_t) (((v) >> 24) & 0xFF)
	D (9, kConfiguration, 0, 0, (uint8_t) (bNet ? 7 : 5), 1, 0, 0x80, 250);
	D (8, 11, 0, 3, 0x01, 0x00, 0x20, StrAudio);
	D (9, kInterface, 0, 0, 0, 0x01, 0x01, 0x20, 0);
	D (9, 0x24, 0x01, U16 (0x0200), 0x08, U16 (AcCs), 0x00);
	D (8, 0x24, 0x0A, Clock, 0x01, 0x05, 0, 0);
	D (17, 0x24, 0x02, 0x01, U16 (0x0101), 0, Clock, (uint8_t) nOut, U32 (0), StrOutCh, U16 (0), 0);
	D (12, 0x24, 0x03, 0x02, U16 (0x0301), 0, 0x01, Clock, U16 (0), 0);
	D (17, 0x24, 0x02, 0x03, U16 (0x0201), 0, Clock, (uint8_t) nIn, U32 (0), StrInCh, U16 (0), 0);
	D (12, 0x24, 0x03, 0x04, U16 (0x0101), 0, 0x03, Clock, U16 (0), 0);
	D (9, kInterface, 1, 0, 0, 0x01, 0x02, 0x20, 0);
	D (9, kInterface, 1, 1, 2, 0x01, 0x02, 0x20, 0);
	D (16, 0x24, 0x01, 0x01, 0x00, 0x01, U32 (1), (uint8_t) nOut, U32 (0), StrOutCh);
	D (6, 0x24, 0x02, 0x01, 3, 24);
	pOff->out = n;
	D (7, kEndpoint, 0x01, 0x05, U16 (nMaxOut), 2);
	D (8, 0x25, 0x01, 0, 0, 0, 0, 0);
	pOff->fb = n;
	D (7, kEndpoint, 0x82, 0x11, U16 (4), 4);
	D (9, kInterface, 2, 0, 0, 0x01, 0x02, 0x20, 0);
	D (9, kInterface, 2, 1, 1, 0x01, 0x02, 0x20, 0);
	D (16, 0x24, 0x01, 0x04, 0x00, 0x01, U32 (1), (uint8_t) nIn, U32 (0), StrInCh);
	D (6, 0x24, 0x02, 0x01, 3, 24);
	pOff->in = n;
	D (7, kEndpoint, 0x83, 0x05, U16 (nMaxIn), 2);
	D (8, 0x25, 0x01, 0, 0, 0, 0, 0);
	// MIDI
	D (8, 11, 3, 2, 0x01, 0x03, 0x00, StrMIDI);
	D (9, kInterface, 3, 0, 0, 0x01, 0x01, 0x00, 0);
	D (9, 0x24, 0x01, U16 (0x0100), U16 (9), 1, 4);
	D (9, kInterface, 4, 0, 2, 0x01, 0x03, 0x00, 0);
	D (7, 0x24, 0x01, U16 (0x0100), U16 (MsCs));
	D (6, 0x24, 0x02, 0x01, 1, StrMIDI);		// IN jack, embedded (from the computer)
	D (6, 0x24, 0x02, 0x02, 2, 0);			// IN jack, external
	D (9, 0x24, 0x03, 0x01, 3, 1, 2, 1, StrMIDI);	// OUT jack, embedded (to the computer), from jack 2
	D (9, 0x24, 0x03, 0x02, 4, 1, 1, 1, 0);		// OUT jack, external, from jack 1
	pOff->midi_out = n;
	D (9, kEndpoint, 0x04, 0x02, U16 (512), 0, 0, 0);
	D (5, 0x25, 0x01, 1, 1);
	pOff->midi_in = n;
	D (9, kEndpoint, 0x85, 0x02, U16 (512), 0, 0, 0);
	D (5, 0x25, 0x01, 1, 3);
#undef D
#undef U16
#undef U32
	pOff->net = {0, 0, 0};
	if (bNet)
	{
		usbnet::DescOffsets o;
		unsigned nAt = n;
		n += usbnet::put_desc (pBuf + n, kNetIface, StrNet, StrNetMac, kEpNotify, kEpNetIn, kEpNetOut, &o);
		pOff->net = {nAt + o.notify, nAt + o.in, nAt + o.out};
	}
	pBuf[2] = (uint8_t) (n & 0xFF);
	pBuf[3] = (uint8_t) (n >> 8);
	return n;
}

}	// namespace sidecar_desc

#endif
