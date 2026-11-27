// pi/engine/relay_proto.h - usb=mirror through a Teensy 4.1 (docs/teensy-mirror.md): the link between the Pi and the
// Teensy that shows a box to the computer on its second USB port. Portable (no Circle, no Teensy core): the Pi's mirror
// target (teensy_relay.cpp) and the Teensy's firmware (teensy/mirror_relay) both include it, and
// tests/test_relay_proto.cpp checks it on the build host.
//
//   box --USB--> Pi USB-A (host: the box's own stream, as with the DWC2 mirror)
//               Pi USB-A (host) --USB--> Teensy USB1 (micro-USB, device: the "link", fixed descriptors below)
//                                         Teensy USB2 (5-pin header, device: the box as the computer sees it)
//
// The Teensy is a thin relay. The Pi keeps all of the mirror's logic (descriptors read from the box, the name suffix,
// the per-block bytes, the MIDI filters, the forwarded class and vendor requests); the Teensy only:
//   - serves the descriptors the Pi uploaded and answers the standard requests itself (SET_ADDRESS, SET_CONFIGURATION,
//     SET_INTERFACE kept local as the DWC2 mirror does, GET_STATUS, CLEAR/SET_FEATURE on its endpoints);
//   - hands every class and vendor request to the Pi (an event) and NAKs EP0 until the Pi answers (kReqCtlAnswer);
//   - moves each mirrored endpoint's packets over a fixed link endpoint ("channel").
//
// Link layout (USB1, high speed, one vendor interface, alt 0):
//   0x81 bulk IN 512 B (1.00-1.01: interrupt 64 B)  events (Event below), one per packet (16 B: a short packet)
//   0x82 bulk IN 512 B (1.00: interrupt)            the computer's audio OUT blocks (the box's ob::kOutEp), one per packet
//   0x02 bulk OUT 512 B (1.00: interrupt)           audio IN blocks for the computer (the box's ob::kInEp), one per packet
// 1.02 has no interrupt endpoint left: beside a Rytm on the Pi 4's VL805 the Teensy's iv1 interrupt endpoints went
// unserved (PB1/PB2, T406/T407), the event pipe too on some boots (BL3 end, T411: ev 0, the computer's first forwarded
// request never answered). The Pi opens what the descriptor gives, so a 1.02 Pi takes a 1.00/1.01 Teensy and back.
//   0x83..0x86 bulk IN 512 B                    channel 0..3 IN: packets the computer sent to an OUT endpoint of the box
//   0x03..0x06 bulk OUT 512 B                   channel 0..3 OUT: packets for the computer from an IN endpoint of the box
// Every link packet carries exactly one packet of the mirrored endpoint (a zero-length packet too), so the Pi's URBs are
// 512 B and each completes on one packet.
//
// Flow control:
//   - box -> computer (channel OUT): the Teensy keeps kChanDepth link buffers per channel; a packet the computer took
//     frees one (the link endpoint takes the next) and sends kEvInTaken. The pipe mirror renews the box's IN request on
//     that event ("the box's IN request is renewed only once the computer has the last packet", docs/usb-mirror.md).
//   - computer -> box (channel IN): the Teensy primes the mirrored OUT endpoint only while it holds credits, so the
//     computer is NAKed until the Pi has room, as the DWC2 mirror NAKs until the box took the packet. The Pi grants
//     credits with kReqCredit (a pipe: one per packet the box took; MIDI: as its queue drains). Credits drop to 0 at the
//     computer's bus reset and at kReqConnect.
//   - audio: no flow control. Each side keeps a short queue and drops the oldest block when it is full (counted).
#ifndef _relay_proto_h
#define _relay_proto_h

#include <stdint.h>
#include <string.h>
#include "mirror_desc.h"

namespace relay {

// the link's IDs: pid.codes test VID/PID (no Elektron IDs on the link); bcdDevice = kVersion
constexpr uint16_t kVid = 0x1209, kPid = 0x0002;
constexpr uint16_t kVersion = 0x0102;			// 1.02: event pipe 0x81 bulk too; 1.01: link audio bulk; the Pi refuses another major
constexpr uint8_t kIfaceClass = 0xFF, kIfaceSub = 0x53, kIfaceProto = 0x01;	// vendor, 'S', 1

constexpr uint8_t kEvEp = 0x81;
constexpr uint8_t kAudioToPiEp = 0x82, kAudioFromPiEp = 0x02;
constexpr unsigned kChannels = 4;
constexpr uint8_t kChanFirstEp = 3;			// channel c: IN 0x80 | (3 + c), OUT 3 + c
constexpr unsigned kLinkMps = 512, kEvMps = 64;		// kEvMps: the buffer for one event packet (an Event is 16 B)
constexpr unsigned kChanDepth = 4;			// link buffers per channel and direction on the Teensy
constexpr unsigned kMaxCredits = kChanDepth;

inline uint8_t chan_in_ep (unsigned c)  { return (uint8_t) (0x80 | (kChanFirstEp + c)); }
inline uint8_t chan_out_ep (unsigned c) { return (uint8_t) (kChanFirstEp + c); }

// ---- vendor requests on the link's EP0 (bmRequestType 0x40 OUT / 0xC0 IN, recipient device) -------------------------

enum Request : uint8_t
{
	kReqSetDevice = 0x01,		// OUT, 18 B: the box's device descriptor
	kReqSetConfig = 0x02,		// OUT, <= mirror::kMaxConfig: its configuration descriptor (wTotalLength bytes)
	kReqSetString = 0x03,		// OUT, wValue = index: a string descriptor (index 0: the language table)
	kReqSetExtra = 0x04,		// OUT, wValue = type: BOS (15), or 0xEE = an MS OS 2.0 set (wIndex = its vendor code);
					// bench only (the box has none): lets a test device bind WinUSB on the computer
	kReqConnect = 0x05,		// wValue 1: check what was uploaded and show it (pull-up on); 0: leave the bus
	kReqCtlAnswer = 0x06,		// OUT, wValue = seq, wIndex = kAnsStall or 0, data = an IN request's answer (none for OUT)
	kReqCtlData = 0x07,		// IN, wValue = seq: the OUT data stage of the computer's request
	kReqCredit = 0x08,		// wValue = mirrored endpoint address (OUT), wIndex = credits to add
	kReqStats = 0x09,		// IN: Stats
	kReqStatsReset = 0x0A,
	kReqLog = 0x0B,			// IN: the Teensy's log text since the last read (up to wLength)
	kReqHalt = 0x0C,		// wValue = mirrored endpoint address, wIndex 1 = STALL it to the computer, 0 = clear
	kReqBootloader = 0x0D,		// wValue 0x4C42 ("BL"): the Teensy reboots into its bootloader (reflash without the button)
};

constexpr uint16_t kAnsStall = 1;

// ---- events (link 0x81, one per packet) ----------------------------------------------------------------------------

enum EventType : uint8_t
{
	kEvHello = 1,		// a = protocol major, b = minor; after the link is configured and at kReqConnect
	kEvBus = 2,		// a = BusState, b = 1 if high speed
	kEvConfig = 3,		// a = the computer's SET_CONFIGURATION value (0 = unconfigured)
	kEvAlt = 4,		// a = interface, b = alternate (the computer's SET_INTERFACE, kept on the Teensy)
	kEvSetup = 5,		// a = seq, data = the 8 setup bytes; an OUT data stage is read with kReqCtlData
	kEvAbort = 6,		// a = seq: the computer moved on (a new SETUP or a bus reset); an answer for it is ignored
	kEvInTaken = 7,		// a = mirrored IN endpoint address, b = packets the computer took since the last one
	kEvDrop = 8,		// a = DropKind, b = mirrored endpoint address; counted in Stats too
};

enum BusState : uint8_t { kBusReset = 1, kBusSuspend = 2, kBusResume = 3, kBusOff = 4 };
enum DropKind : uint8_t { kDropAudioToPc = 1, kDropAudioToPi = 2, kDropNoCredit = 3, kDropBadLen = 4 };

struct Event
{
	uint8_t type, a, b, c;
	uint32_t t_us;			// the Teensy's microsecond clock when it happened
	uint8_t data[8];
};
static_assert (sizeof (Event) == 16, "Event is 16 bytes on the wire");

// ---- the Teensy's counters (kReqStats); all from the last kReqStatsReset -------------------------------------------

constexpr uint32_t kStatsMagic = 0x594C5252;	// "RRLY"
constexpr unsigned kHistBins = 16;		// residency histogram: bin i = i..i+1 microframes (125 us), the last open

struct Stats
{
	uint32_t magic, version;
	uint32_t uptime_ms;
	uint32_t link_hs, link_configured, link_resets;
	uint32_t pc_hs, pc_configured, pc_resets, pc_suspends;
	uint32_t shown;				// 1: USB2 on the bus with the uploaded descriptors
	uint32_t setups, ctl_forwarded, ctl_answered, ctl_stalled, ctl_aborted, ctl_late;
	uint32_t audio_to_pc, audio_to_pc_drop, audio_to_pc_empty;	// blocks sent; dropped (queue full); polls with none
	uint32_t audio_to_pi, audio_to_pi_drop;
	uint32_t chan_to_pc[kChannels], chan_to_pi[kChannels], chan_nocredit[kChannels];
	uint32_t events, events_drop;
	uint32_t q_to_pc_max, q_to_pi_max;	// deepest audio queues seen (blocks)
	uint32_t hist_to_pc[kHistBins];		// audio block: link arrival -> the computer took it
	uint32_t hist_to_pi[kHistBins];		// audio block: the computer sent it -> the Pi took it
	uint32_t worst_to_pc_us, worst_to_pi_us;
	uint32_t isr1, isr2;
	// TR2b (T329): the link's audio OUT packets from the Pi took 4-8+ ms with the real A4 and the Engine (0.5-2 ms in
	// relaytest); appended so an older reader still gets the fields above
	uint32_t isr1_max_us, isr2_max_us;	// the longest run of each port's interrupt
	uint32_t link_gap_max_us;		// longest gap between two link audio packets taken (under 1 s: a stream, not idle)
	uint32_t link_gap_hist[kHistBins];	// those gaps, 125 us bins
	uint32_t link_dry;			// a link audio packet taken with no other dTD armed: the next one NAKed until re-armed
	uint32_t link_nak_polls;		// loop() polls that saw the link audio OUT endpoint NAK (ENDPTNAK)
	uint32_t pc_audio_err;			// the computer's audio IN dTDs that ended with an error
	// TR5c (T339): the computer's side in time: gaps between blocks the computer took (IN) and sent (OUT), while streaming
	// (under 1 s), bins < 0.25, 0.5, 1, 2, 4, 8, 16 ms, more; and the longest
	uint32_t pc_in_gap[8], pc_out_gap[8], pc_in_gap_max_us, pc_out_gap_max_us;
	uint32_t pc_idle_enter, pc_idle_leave;	// the computer stopped / started again taking the box's IN (stale blocks not kept)
	uint32_t pace_primed, pace_held, pace_empty;	// paced IN: blocks handed out at SOFs; SOFs due but 2 primed; due but none queued
};

inline unsigned gap_bin (uint32_t us)
{
	unsigned b = 0;
	for (uint32_t lim = 250; b < 7 && us >= lim; lim <<= 1) b++;
	return b;
}

inline unsigned hist_bin (uint32_t us)
{
	unsigned b = us / 125;
	return b < kHistBins ? b : kHistBins - 1;
}

// ---- the channel map: which mirrored endpoint rides which link endpoint -----------------------------------------------
// Both sides derive it from the same configuration descriptor (mirror::parse), so nothing about it is sent. The audio
// endpoints ride 0x82 / 0x02; every other endpoint the gadget would carry (pipes, MIDI; mirror::kNone too: the Teensy
// relays it, the Pi decides what to do with it) takes the next free channel of its direction, in descriptor order.

struct Map
{
	mirror::Layout l;
	int8_t chan[mirror::kMaxEps];		// per l.ep[i]: channel, -1 for audio
	uint8_t n_in, n_out;			// channels used per direction (mirrored IN endpoints / OUT endpoints)
	int8_t ep_of_in[kChannels];		// channel -> index into l.ep (mirrored IN endpoint), -1
	int8_t ep_of_out[kChannels];
};

// FALSE: the descriptor is not one the link can carry (mirror::parse's limits, or more than kChannels per direction)
inline bool make_map (const uint8_t *cfg, unsigned len, uint8_t in_ep, uint8_t out_ep, Map *m)
{
	memset (m, 0, sizeof *m);
	for (unsigned c = 0; c < kChannels; c++) m->ep_of_in[c] = m->ep_of_out[c] = -1;
	if (!mirror::parse (cfg, len, in_ep, out_ep, &m->l)) return false;
	for (unsigned i = 0; i < m->l.n; i++)
	{
		const mirror::Ep &e = m->l.ep[i];
		m->chan[i] = -1;
		if (e.role == mirror::kAudioIn || e.role == mirror::kAudioOut) continue;
		if (e.mps > kLinkMps) return false;
		if (e.addr & 0x80)
		{
			if (m->n_in == kChannels) return false;
			m->ep_of_in[m->n_in] = (int8_t) i;
			m->chan[i] = (int8_t) m->n_in++;
		}
		else
		{
			if (m->n_out == kChannels) return false;
			m->ep_of_out[m->n_out] = (int8_t) i;
			m->chan[i] = (int8_t) m->n_out++;
		}
	}
	if (m->l.audio_in >= 0 && m->l.ep[m->l.audio_in].mps > kLinkMps) return false;
	if (m->l.audio_out >= 0 && m->l.ep[m->l.audio_out].mps > kLinkMps) return false;
	return true;
}

// the link endpoint a mirrored endpoint rides (0: none)
inline uint8_t link_ep (const Map &m, uint8_t addr)
{
	for (unsigned i = 0; i < m.l.n; i++)
	{
		if (m.l.ep[i].addr != addr) continue;
		if (m.l.ep[i].role == mirror::kAudioIn) return kAudioFromPiEp;
		if (m.l.ep[i].role == mirror::kAudioOut) return kAudioToPiEp;
		return (addr & 0x80) ? chan_out_ep ((unsigned) m.chan[i]) : chan_in_ep ((unsigned) m.chan[i]);
	}
	return 0;
}

// ---- a deferred control request: one in flight; the seq tells a late answer from the current one -------------------

struct CtlSlot
{
	uint8_t seq;			// of the request in flight (or the last one)
	bool pending;			// waiting for the Pi's answer
	uint8_t setup[8];

	uint8_t begin (const uint8_t *s) { seq = (uint8_t) (seq + 1); pending = true; memcpy (setup, s, 8); return seq; }
	// an answer: TRUE when it is for the request in flight (which it then ends)
	bool answer (uint8_t s) { if (!pending || s != seq) return false; pending = false; return true; }
	// the computer moved on: TRUE when a request was in flight (the Pi is told with kEvAbort)
	bool abort () { bool p = pending; pending = false; return p; }
};

} // namespace relay

#endif
