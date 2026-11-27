// pi/engine/mirror_stream.h - usb=mirror's audio between one box and the computer, apart from the port that shows the box:
// the computer's alternate -> channels, the box's IN blocks cut to them, the computer's OUT blocks into a frame ring that
// the box's OUT fill adds, and Overbridge's per-block bytes (mirror::ExtraRing) a fixed distance ahead. The same steps as
// CSidecarGadget's MirrorSetInterface / MirrorAudioIn / MirrorAudioOut / ToRytm (docs/usb-mirror.md), written once more
// here so a second port (the Teensy relay, teensy_target.cpp) runs them for its own box. Portable; tests/test_mirror_stream.cpp.
//
// Threads: in_block and out_block run in the xHCI IRQ (core 0), to_box and out_extra in the box's OUT fill (xHCI IRQ or task
// on core 0), set_alt in core 0's loop. One producer and one consumer per ring, as the gadget's.
#ifndef _mirror_stream_h
#define _mirror_stream_h

#include <stdint.h>
#include <string.h>
#include "mirror_desc.h"
#include "ob_block.h"

namespace mirror {

struct StreamStats
{
	unsigned in_blocks, in_sent, out_blocks, out_bad, out_under, out_over, out_extra, anchor, alt_bad;
	unsigned lvl_min, lvl_max;		// the OUT ring's level (frames) seen by to_box since the reader last reset them (T342)
};

struct Stream
{
	static constexpr unsigned kRing = 512, kOutTarget = 48;	// the gadget's Ring and OutTarget (frames)
	unsigned out_target = kOutTarget;	// frames the OUT ring fills to before the box takes from it (the relay: more, T336)
	static constexpr unsigned kCh = 12;

	unsigned in_ch, out_ch;			// the computer's alternates (0: not streaming)
	unsigned margin;			// mirrormargin= blocks
	int32_t ring[kRing][kCh];
	volatile unsigned head, tail;
	bool priming;
	ExtraRing extra;
	bool off_set;
	uint16_t off;
	StreamStats st;

	void init (unsigned nMargin) { memset (this, 0, sizeof *this); margin = nMargin; priming = true; out_target = kOutTarget; }
	void out_reset () { head = tail = 0; priming = true; }
	// the box's stream started again (a box that went away and came back): its counter starts from 0, a new offset
	void box_restart () { off_set = false; extra.flush (); }

	// channels of an audio alternate (32-byte header + 7 frames x 4 bytes x ch); 0 when it is not one (counted)
	unsigned channels (const uint8_t *cfg, unsigned len, uint8_t iface, uint8_t alt, uint8_t addr)
	{
		unsigned m = alt_mps (cfg, len, iface, alt, addr);
		if (m < (unsigned) ob::kHeaderBytes) return 0;
		unsigned d = m - ob::kHeaderBytes, f = ob::kFramesPerBlock * 4;
		if (d % f != 0 || d / f > (unsigned) ob::kChannels) { st.alt_bad++; return 0; }
		return d / f;
	}

	// the computer's SET_INTERFACE: FALSE when the configuration has no such alternate
	bool set_alt (const Layout &l, const uint8_t *cfg, unsigned len, uint8_t iface, uint8_t alt)
	{
		if (!has_alt (cfg, len, iface, alt)) return false;
		if (l.audio_in >= 0 && l.ep[l.audio_in].iface == iface) in_ch = channels (cfg, len, iface, alt, ob::kInEp);
		if (l.audio_out >= 0 && l.ep[l.audio_out].iface == iface)
		{
			out_ch = channels (cfg, len, iface, alt, ob::kOutEp);
			off_set = false;		// a new stream: its own offset
			out_reset ();
		}
		return true;
	}

	// one 12-channel IN block of the box, cut to the computer's channels into out; bytes (0: the computer takes none)
	unsigned in_block (const uint8_t *blk, uint8_t *out)
	{
		st.in_blocks++;
		if (!in_ch) return 0;
		if (in_ch == (unsigned) ob::kChannels) memcpy (out, blk, ob::block_bytes (ob::kChannels));
		else
		{
			memcpy (out, blk, ob::kHeaderBytes);
			for (int f = 0; f < ob::kFramesPerBlock; f++)
				memcpy (out + ob::kHeaderBytes + 4 * in_ch * f, blk + ob::kHeaderBytes + 4 * ob::kChannels * f, 4 * in_ch);
		}
		st.in_sent++;
		return ob::block_bytes (in_ch);
	}

	// one OUT block of the computer (n bytes); pi = the box's next OUT counter (CObUsb::NextOutCounter)
	void out_block (const uint8_t *p, unsigned n, uint16_t pi)
	{
		unsigned ch = out_ch;
		if (ch == 0 || n != (unsigned) ob::block_bytes (ch)) { st.out_bad++; return; }
		st.out_blocks++;
		uint16_t comp = ob::block_counter (p);
		if (off_set && ExtraRing::stale ((uint16_t) (comp + off), pi, margin))
		{
			if ((int16_t) (uint16_t) (comp + off - pi) > 0) extra.flush ();
			off_set = false;
			st.anchor++;
		}
		if (!off_set)
		{
			off = (uint16_t) (pi - comp + margin * ob::kFramesPerBlock);
			off_set = true;
		}
		if (extra.push_block (p, (uint16_t) (comp + off))) st.out_extra++;
		for (int f = 0; f < ob::kFramesPerBlock; f++)
		{
			if (head - tail >= kRing) { st.out_over++; continue; }
			int32_t *fr = ring[head % kRing];
			for (unsigned c = 0; c < kCh; c++) fr[c] = c < ch ? ob::block_sample (p, ch, f, c) >> 8 : 0;	// 24-bit
			head = head + 1;
		}
	}

	bool out_extra (uint8_t *blk, uint16_t counter) { return extra.pop_into (blk, counter); }

	// the box's OUT fill: the computer's frames added (saturating), as the gadget's ToRytm
	void to_box (int32_t (*F)[kCh], unsigned n)
	{
		for (unsigned f = 0; f < n; f++)
		{
			if (!out_ch) continue;
			unsigned level = head - tail;
			if (!priming) { if (level < st.lvl_min) st.lvl_min = level; if (level > st.lvl_max) st.lvl_max = level; }
			if (priming)
			{
				if (level < out_target) continue;
				priming = false;
			}
			if (level == 0) { st.out_under++; priming = true; continue; }
			const int32_t *m = ring[tail % kRing];
			tail = tail + 1;
			for (unsigned c = 0; c < kCh; c++)
			{
				int64_t v = (int64_t) F[f][c] + ((int64_t) m[c] << 8);
				F[f][c] = v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFFLL - 1 ? (int32_t) 0x80000000 : (int32_t) v;
			}
		}
	}
};

} // namespace mirror

#endif
