// pi/engine/mirror_desc.h - usb=mirror (docs/usb-mirror.md): what the Sidecar's USB-C side needs to know about the
// attached box's configuration descriptor, which it serves to the computer unchanged. Portable (no Circle), tested on
// the build host by tests/test_mirror_desc.cpp with a made-up descriptor of the same shape.
//
// The descriptors are read from the box at run time (ob_usb.cpp) and kept in RAM only: no box's bytes are in the repo.
//
// Endpoint roles, by interface class and address (generic: any box whose device profile has an audio row):
//   vendor class (0xFF), ob::kInEp / ob::kOutEp     the audio stream: IN relayed block for block, OUT decoded and added
//                                                   to what the engine sends (sidecar_gadget.cpp)
//   vendor class, any other endpoint                a pipe: relayed packet for packet to the same endpoint of the box
//                                                   (the Overbridge async channel: Transfer, Total Recall, the plugins)
//   audio class (0x01), MIDI streaming (0x03)       USB MIDI, through the Sidecar's filters as in the default mode
// The gadget creates one endpoint per address, with the largest wMaxPacketSize of any alternate setting (a smaller
// alternate's packets are then short packets), so no endpoint is resized at SET_INTERFACE.
#ifndef _mirror_desc_h
#define _mirror_desc_h

#include <stdint.h>
#include <string.h>

namespace mirror {

constexpr unsigned kMaxConfig = 512;		// bytes of configuration descriptor (EP0's buffer in Circle's gadget)
constexpr unsigned kMaxEps = 14;		// DWC2: 7 IN + 7 OUT besides EP0
constexpr unsigned kMaxIfaces = 16;
constexpr unsigned kMaxStrings = 24;		// string descriptors kept (index 0, the languages, included)

enum Role : uint8_t { kNone, kAudioIn, kAudioOut, kPipeIn, kPipeOut, kMidiIn, kMidiOut };

struct Ep
{
	uint8_t addr;				// bEndpointAddress
	uint8_t attr;				// bmAttributes (transfer type in bits 0-1)
	uint8_t interval;			// bInterval (the first alternate's)
	uint16_t mps;				// the largest wMaxPacketSize & 0x7FF across alternates
	uint8_t iface;				// bInterfaceNumber
	Role role;
	uint8_t desc[7];			// the endpoint descriptor the gadget creates the endpoint from (mps above)
};

struct Layout
{
	Ep ep[kMaxEps];
	unsigned n;
	unsigned ifaces;			// highest interface number + 1
	uint8_t max_alt[kMaxIfaces];		// highest alternate setting of each interface
	int audio_in, audio_out;		// index into ep[], or -1
};

inline uint16_t le16 (const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }

// Walks the configuration descriptor; FALSE when it is malformed, too big, or has more endpoints or interfaces than the
// gadget can carry. in_ep / out_ep: the audio stream's endpoint addresses (ob::kInEp, ob::kOutEp).
inline bool parse (const uint8_t *cfg, unsigned len, uint8_t in_ep, uint8_t out_ep, Layout *l)
{
	memset (l, 0, sizeof *l);
	l->audio_in = l->audio_out = -1;
	if (len < 9 || len > kMaxConfig || cfg[1] != 2 || le16 (cfg + 2) != len) return false;
	int iface = -1;
	uint8_t cls = 0, sub = 0;
	for (unsigned o = 0; o < len;)
	{
		unsigned dl = cfg[o];
		if (dl < 2 || o + dl > len) return false;
		const uint8_t *d = cfg + o;
		if (d[1] == 4 && dl >= 9)			// interface
		{
			iface = d[2];
			if (iface >= (int) kMaxIfaces) return false;
			if ((unsigned) iface + 1 > l->ifaces) l->ifaces = iface + 1;
			if (d[3] > l->max_alt[iface]) l->max_alt[iface] = d[3];
			cls = d[5], sub = d[6];
		}
		else if (d[1] == 5 && dl >= 7)			// endpoint
		{
			if (iface < 0) return false;
			uint8_t a = d[2];
			uint16_t m = le16 (d + 4) & 0x7FF;
			unsigned i = 0;
			while (i < l->n && l->ep[i].addr != a) i++;
			if (i == l->n)
			{
				if (l->n == kMaxEps || (a & 0x0F) == 0 || (a & 0x0F) > 7) return false;
				Ep &e = l->ep[l->n++];
				e.addr = a, e.attr = d[3] & 0x03, e.interval = d[6], e.iface = (uint8_t) iface;
				if (cls == 0xFF) e.role = a == in_ep ? kAudioIn : a == out_ep ? kAudioOut : (a & 0x80) ? kPipeIn : kPipeOut;
				else if (cls == 0x01 && sub == 0x03) e.role = (a & 0x80) ? kMidiIn : kMidiOut;
				else e.role = kNone;
				if (e.role == kAudioIn) l->audio_in = (int) i;
				if (e.role == kAudioOut) l->audio_out = (int) i;
			}
			else if (l->ep[i].iface != iface) return false;	// one address, one interface
			if (m > l->ep[i].mps) l->ep[i].mps = m;
		}
		o += dl;
	}
	for (unsigned i = 0; i < l->n; i++)
	{
		Ep &e = l->ep[i];
		const uint8_t d[7] = {7, 5, e.addr, e.attr, (uint8_t) (e.mps & 0xFF), (uint8_t) (e.mps >> 8), e.interval};
		memcpy (e.desc, d, 7);
	}
	return l->n > 0;
}

// wMaxPacketSize & 0x7FF of endpoint `addr` at interface `iface`, alternate `alt` (0: not there)
inline unsigned alt_mps (const uint8_t *cfg, unsigned len, uint8_t iface, uint8_t alt, uint8_t addr)
{
	int cur = -1, cur_alt = -1;
	for (unsigned o = 0; o + 2 <= len && cfg[o] >= 2 && o + cfg[o] <= len; o += cfg[o])
	{
		const uint8_t *d = cfg + o;
		if (d[1] == 4 && d[0] >= 9) cur = d[2], cur_alt = d[3];
		else if (d[1] == 5 && d[0] >= 7 && cur == iface && cur_alt == alt && d[2] == addr) return le16 (d + 4) & 0x7FF;
	}
	return 0;
}

// Is there an interface `iface` with alternate setting `alt`?
inline bool has_alt (const uint8_t *cfg, unsigned len, uint8_t iface, uint8_t alt)
{
	for (unsigned o = 0; o + 2 <= len && cfg[o] >= 2 && o + cfg[o] <= len; o += cfg[o])
		if (cfg[o + 1] == 4 && cfg[o] >= 9 && cfg[o + 2] == iface && cfg[o + 3] == alt) return true;
	return false;
}

// The string indexes the device and configuration descriptors name (iManufacturer, iProduct, iSerialNumber,
// iConfiguration, each IAD's iFunction, each interface's iInterface, a MIDI jack's iJack), each once, in order of
// first use; returns how many (at most max)
inline unsigned string_indexes (const uint8_t *dev, const uint8_t *cfg, unsigned len, uint8_t *out, unsigned max)
{
	unsigned n = 0;
	auto add = [&] (uint8_t i)
	{
		if (i == 0 || n == max) return;
		for (unsigned k = 0; k < n; k++) if (out[k] == i) return;
		out[n++] = i;
	};
	add (dev[14]), add (dev[15]), add (dev[16]);
	if (len >= 9) add (cfg[6]);
	uint8_t cls = 0, sub = 0;
	for (unsigned o = 0; o + 2 <= len && cfg[o] >= 2 && o + cfg[o] <= len; o += cfg[o])
	{
		const uint8_t *d = cfg + o;
		if (d[1] == 11 && d[0] >= 8) add (d[7]);			// IAD
		else if (d[1] == 4 && d[0] >= 9) cls = d[5], sub = d[6], add (d[8]);
		else if (d[1] == 0x24 && cls == 0x01 && sub == 0x03 && d[0] >= 6)	// MIDI streaming, class-specific
		{
			if (d[2] == 0x02 && d[0] >= 6) add (d[5]);		// IN jack
			else if (d[2] == 0x03 && d[0] >= 7) add (d[d[0] - 1]);	// OUT jack: iJack is the last byte
		}
	}
	return n;
}

// Overbridge's device name (vendor request 1 to the device, wValue 0, wIndex 0: IN reads it, OUT sets it; 32 bytes,
// NUL-padded), which Elektron's software shows the unit by and keys nothing on (the unit ID is request 0's). The mirror
// shows the computer the box's name with a suffix ("Analog Rytm MKII (SC)") and takes the suffix off a name the
// computer sets, so the box stores its plain name. Only for an Elektron box (kNameVendor): request 1 is another
// vendor's own.
constexpr uint16_t kNameVendor = 0x1935;
constexpr uint8_t kNameRequest = 1;
constexpr unsigned kNameLen = 32;

inline unsigned name_len (const uint8_t *n, unsigned len)
{
	unsigned l = 0;
	while (l < len && n[l] != 0) l++;
	return l;
}

inline bool name_ends_with (const uint8_t *n, unsigned l, const char *suffix)
{
	unsigned s = (unsigned) strlen (suffix);
	return s > 0 && l >= s && memcmp (n + l - s, suffix, s) == 0;
}

// the name the computer is shown: the box's own (cut so that the suffix and a NUL fit), then the suffix, NUL-padded
inline void name_show (uint8_t *n, unsigned len, const char *suffix)
{
	unsigned s = (unsigned) strlen (suffix);
	unsigned l = name_len (n, len);
	if (s == 0 || s + 1 > len || name_ends_with (n, l, suffix)) return;
	while (l > 0 && n[l - 1] == ' ') l--;
	if (l > len - 1 - s) l = len - 1 - s;
	memcpy (n + l, suffix, s);
	memset (n + l + s, 0, len - l - s);
}

// the name the box stores, from the one the computer sets: the suffix taken off its end when it is there
inline void name_store (uint8_t *n, unsigned len, const char *suffix)
{
	unsigned l = name_len (n, len);
	if (!name_ends_with (n, l, suffix)) return;
	l -= (unsigned) strlen (suffix);
	memset (n + l, 0, len - l);
}

// The computer's per-block bytes on the audio OUT stream. Overbridge's driver writes byte 1 as the index (0-27) of the
// first non-zero byte of 4-31, or 0xFF when they are all zero, and bytes 4-31 from the Engine; the Engine's latency
// check puts a marker there and counts the samples until the box's pulse comes back on IN, five times, and wants the
// same count each time. The Pi sends the box its own blocks, so a computer block whose bytes 4-31 are not all zero is
// queued here (gadget IRQ) under the Pi's block counter it is due in, and copied into that block (ob_usb.cpp FillOut).
// The due counter is the computer's block counter plus an offset fixed at the stream's first block: both streams run
// on the box's clock, so the delay the Pi adds is the same every time. One producer, one consumer.
constexpr unsigned kExtraFirst = 4, kExtraBytes = 28;

struct ExtraRing
{
	static constexpr unsigned kSlots = 128;		// the margin plus a burst of the computer's blocks (with a plugin open
						// about every third block carries bytes, 2026-11-04 [hw])
	uint8_t d[kSlots][kExtraBytes];
	uint16_t due[kSlots];
	volatile unsigned head, tail;
	unsigned dropped, late;

	void clear () { head = tail = 0; dropped = late = 0; }
	// the box's stream started again (the box went away, or came back): what is queued is due under the old stream's
	// counter, so it is dropped. Not counted. IRQs masked, or the consumer's own IRQ (both run on core 0).
	void flush () { tail = head; }
	static constexpr unsigned kStep = 7;	// the block counter's step (ob::kFramesPerBlock)
	// an offset that no longer fits (the computer's block due at `due`, the Pi's OUT counter at `pi`, the offset set
	// `margin` blocks ahead): due in a block the Pi has already sent (the computer fell behind), or further ahead than
	// the margin plus a full ring of the computer's blocks running ahead (the Pi's counter went back, as when the box's
	// stream starts again). The gadget then sets the offset again. The margin is in the bound: a fresh offset is
	// `margin` blocks ahead, so with a bound of the ring alone every block of a margin above kSlots re-anchored and
	// flushed (mirrormargin= takes up to 256). The ring holds entries, not blocks: only blocks with bytes take one.
	static bool stale (uint16_t due, uint16_t pi, unsigned margin)
	{
		const int16_t d = (int16_t) (uint16_t) (due - pi);
		return d < 0 || d > (int) ((margin + kSlots) * kStep);
	}
	// the block's bytes queued for the Pi's block `at` when any is set; FALSE when all are zero or the ring is full
	bool push_block (const uint8_t *blk, uint16_t at)
	{
		const uint8_t *x = blk + kExtraFirst;
		unsigned i = 0;
		while (i < kExtraBytes && x[i] == 0) i++;
		if (i == kExtraBytes) return false;
		if (head - tail >= kSlots) { dropped++; return false; }
		memcpy (d[head % kSlots], x, kExtraBytes);
		due[head % kSlots] = at;
		head = head + 1;
		return true;
	}
	// into the Pi's block with counter `at`: the bytes due in it, and byte 1 as Overbridge's driver writes it; bytes
	// due earlier are dropped (late: the offset was too small). FALSE when none are due.
	bool pop_into (uint8_t *blk, uint16_t at)
	{
		while (tail != head)
		{
			unsigned k = tail % kSlots;
			int16_t diff = (int16_t) (uint16_t) (due[k] - at);
			if (diff > 0) return false;
			if (diff < 0) { late++; tail = tail + 1; continue; }
			memcpy (blk + kExtraFirst, d[k], kExtraBytes);
			unsigned i = 0;
			while (i < kExtraBytes && d[k][i] == 0) i++;
			blk[1] = i < kExtraBytes ? (uint8_t) i : 0xFF;
			tail = tail + 1;
			return true;
		}
		return false;
	}
};

} // namespace mirror

#endif
