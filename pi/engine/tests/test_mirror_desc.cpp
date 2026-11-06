// pi/engine/tests/test_mirror_desc.cpp - mirror_desc.h (usb=mirror, docs/usb-mirror.md): make build/test_mirror_desc
//
// The fixture is made up here (pid.codes test IDs, its own strings): the shape of an Overbridge-mode box (IADs; vendor
// interfaces 0-3 with the audio stream's alternates on 1 and 2 and a pipe pair on 3; USB MIDI on 4-5), not any box's bytes.
#include "mirror_desc.h"
#include "ob_block.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> Cfg;
static void D (std::initializer_list<int> b) { for (int x : b) Cfg.push_back ((uint8_t) x); }

static void BuildFixture (bool bExtraOnIf1 = false)
{
	Cfg.clear ();
	D ({9, 2, 0, 0, 6, 1, 7, 0xC0, 0});				// config, iConfiguration 7
	D ({8, 11, 0, 4, 0xFF, 0, 0, 8});				// IAD 0-3, iFunction 8
	D ({9, 4, 0, 0, 0, 0xFF, 0, 0, 0});
	D ({9, 4, 1, 0, 0, 0xFF, 0, 0, 0});
	const int Mps[3] = {32, 88, 368};
	for (int a = 1; a <= 3; a++)
	{
		D ({9, 4, 1, a, 1, 0xFF, 0, 0, 0});
		D ({7, 5, 0x83, 3, Mps[a - 1] & 0xFF, Mps[a - 1] >> 8, 1});
	}
	if (bExtraOnIf1) D ({7, 5, 0x03, 3, 8, 0, 1});		// 0x03 on interface 1 as well: refused
	D ({9, 4, 2, 0, 0, 0xFF, 0, 0, 0});
	for (int a = 1; a <= 3; a++)
	{
		D ({9, 4, 2, a, 1, 0xFF, 0, 0, 0});
		D ({7, 5, 0x03, 3, Mps[a - 1] & 0xFF, Mps[a - 1] >> 8, 1});
	}
	D ({9, 4, 3, 0, 2, 0xFF, 0, 0, 9});				// iInterface 9
	D ({7, 5, 0x02, 2, 0x00, 0x02, 0});
	D ({7, 5, 0x82, 3, 64, 0, 1});
	D ({8, 11, 4, 2, 1, 3, 0, 0});
	D ({9, 4, 4, 0, 0, 1, 1, 0, 0});
	D ({9, 0x24, 1, 0, 1, 9, 0, 1, 5});
	D ({9, 4, 5, 0, 2, 1, 3, 0, 0});
	D ({7, 0x24, 1, 0, 1, 0x41, 0});
	D ({6, 0x24, 2, 1, 1, 10});					// IN jack, iJack 10
	D ({6, 0x24, 2, 2, 2, 0});
	D ({9, 0x24, 3, 1, 3, 1, 2, 1, 11});				// OUT jack, iJack 11
	D ({9, 0x24, 3, 2, 4, 1, 1, 1, 0});
	D ({9, 5, 0x01, 2, 0x00, 0x02, 0, 0, 0});
	D ({5, 0x25, 1, 1, 1});
	D ({9, 5, 0x81, 2, 0x00, 0x02, 0, 0, 0});
	D ({5, 0x25, 1, 1, 3});
	Cfg[2] = Cfg.size () & 0xFF;
	Cfg[3] = Cfg.size () >> 8;
}

static const uint8_t Dev[18] = {18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 0x09, 0x12, 0xFE, 0xFF, 0x01, 0x00, 1, 2, 3, 1};

static const mirror::Ep *Find (const mirror::Layout &l, uint8_t a)
{
	for (unsigned i = 0; i < l.n; i++) if (l.ep[i].addr == a) return &l.ep[i];
	return nullptr;
}

int main ()
{
	using namespace mirror;
	BuildFixture ();
	Layout l;
	CHECK (parse (Cfg.data (), (unsigned) Cfg.size (), ob::kInEp, ob::kOutEp, &l));
	CHECK (l.n == 6);
	CHECK (l.ifaces == 6);
	CHECK (l.max_alt[1] == 3 && l.max_alt[2] == 3 && l.max_alt[3] == 0);

	// one endpoint per address, at the largest alternate's size
	const Ep *in = Find (l, 0x83), *out = Find (l, 0x03);
	CHECK (in && in->role == kAudioIn && in->mps == 368 && in->iface == 1 && in->attr == 3 && in->interval == 1);
	CHECK (out && out->role == kAudioOut && out->mps == 368 && out->iface == 2);
	CHECK (l.audio_in >= 0 && &l.ep[l.audio_in] == in);
	CHECK (l.audio_out >= 0 && &l.ep[l.audio_out] == out);
	CHECK (in->desc[0] == 7 && in->desc[1] == 5 && in->desc[2] == 0x83 && in->desc[3] == 3);
	CHECK ((in->desc[4] | in->desc[5] << 8) == 368 && in->desc[6] == 1);

	// the pipes (async channel) and MIDI
	const Ep *po = Find (l, 0x02), *pi = Find (l, 0x82), *mo = Find (l, 0x01), *mi = Find (l, 0x81);
	CHECK (po && po->role == kPipeOut && po->mps == 512 && po->attr == 2 && po->iface == 3);
	CHECK (pi && pi->role == kPipeIn && pi->mps == 64 && pi->attr == 3);
	CHECK (mo && mo->role == kMidiOut && mo->mps == 512 && mo->iface == 5);
	CHECK (mi && mi->role == kMidiIn && mi->mps == 512);

	// each alternate's own size, and which alternates exist
	const uint8_t *c = Cfg.data ();
	unsigned n = (unsigned) Cfg.size ();
	CHECK (alt_mps (c, n, 1, 1, 0x83) == 32);
	CHECK (alt_mps (c, n, 1, 2, 0x83) == 88);
	CHECK (alt_mps (c, n, 1, 3, 0x83) == 368);
	CHECK (alt_mps (c, n, 2, 3, 0x03) == (unsigned) ob::block_bytes (ob::kChannels));
	CHECK (alt_mps (c, n, 1, 0, 0x83) == 0);
	CHECK (alt_mps (c, n, 2, 3, 0x83) == 0);
	CHECK (has_alt (c, n, 1, 3) && has_alt (c, n, 3, 0) && !has_alt (c, n, 3, 1) && !has_alt (c, n, 6, 0));

	// the strings it names: device 1 2 3, configuration 7, IAD 8, interface 9, jacks 10 11
	uint8_t s[kMaxStrings];
	unsigned k = string_indexes (Dev, c, n, s, kMaxStrings);
	const uint8_t want[] = {1, 2, 3, 7, 8, 9, 10, 11};
	CHECK (k == sizeof want);
	for (unsigned i = 0; i < k && i < sizeof want; i++) CHECK (s[i] == want[i]);
	CHECK (string_indexes (Dev, c, n, s, 2) == 2);

	// refused: a wrong total length, an endpoint on two interfaces, a truncated descriptor, too big
	std::vector<uint8_t> bad = Cfg;
	bad[2]++;
	CHECK (!parse (bad.data (), (unsigned) bad.size (), ob::kInEp, ob::kOutEp, &l));
	BuildFixture (true);
	CHECK (!parse (Cfg.data (), (unsigned) Cfg.size (), ob::kInEp, ob::kOutEp, &l));
	BuildFixture ();
	bad = Cfg;
	bad[9 + 8 + 9] = 0;					// a zero-length descriptor
	CHECK (!parse (bad.data (), (unsigned) bad.size (), ob::kInEp, ob::kOutEp, &l));
	bad.assign (kMaxConfig + 1, 0);
	bad[0] = 9, bad[1] = 2, bad[2] = (kMaxConfig + 1) & 0xFF, bad[3] = (kMaxConfig + 1) >> 8;
	CHECK (!parse (bad.data (), (unsigned) bad.size (), ob::kInEp, ob::kOutEp, &l));

	// the Overbridge name: shown with the suffix, stored without it
	{
		const char *suf = " (SC)";
		uint8_t nm[kNameLen] = {};
		memcpy (nm, "Test Box", 8);
		name_show (nm, kNameLen, suf);
		CHECK (strcmp ((const char *) nm, "Test Box (SC)") == 0);
		name_show (nm, kNameLen, suf);					// not twice
		CHECK (strcmp ((const char *) nm, "Test Box (SC)") == 0);
		name_store (nm, kNameLen, suf);
		CHECK (strcmp ((const char *) nm, "Test Box") == 0);
		for (unsigned i = 8; i < kNameLen; i++) CHECK (nm[i] == 0);
		name_store (nm, kNameLen, suf);					// a plain name stays
		CHECK (strcmp ((const char *) nm, "Test Box") == 0);
		memset (nm, 'x', kNameLen);					// 32 bytes, no NUL: cut so that suffix and NUL fit
		name_show (nm, kNameLen, suf);
		CHECK (nm[kNameLen - 1] == 0 && strlen ((const char *) nm) == kNameLen - 1);
		CHECK (memcmp (nm + kNameLen - 1 - strlen (suf), suf, strlen (suf)) == 0);
		memset (nm, 0, kNameLen);
		memcpy (nm, "Pad   ", 6);					// trailing spaces go
		name_show (nm, kNameLen, suf);
		CHECK (strcmp ((const char *) nm, "Pad (SC)") == 0);
		uint8_t sh[4] = {'A', 'B', 0, 0};					// a reply shorter than the suffix: kept
		name_show (sh, sizeof sh, suf);
		CHECK (strcmp ((const char *) sh, "AB") == 0);
		name_show (nm, kNameLen, "");					// off
		CHECK (strcmp ((const char *) nm, "Pad (SC)") == 0);
	}

	// the computer's per-block bytes: queued only when set, each into the Pi block it is due in, byte 1 as the driver's
	{
		static ExtraRing r;
		r.clear ();
		uint8_t blk[368] = {}, out[368];
		CHECK (!r.push_block (blk, 70));				// all zero: nothing
		blk[4] = 0xEC, blk[5] = 0x40;
		CHECK (r.push_block (blk, 70));
		blk[4] = 0, blk[5] = 0, blk[31] = 0x22;
		CHECK (r.push_block (blk, 77));
		memset (out, 0xAA, sizeof out);
		CHECK (!r.pop_into (out, 63) && out[1] == 0xAA);		// not due yet
		CHECK (r.pop_into (out, 70));
		CHECK (out[1] == 0 && out[3] == 0xAA && out[4] == 0xEC && out[5] == 0x40 && out[6] == 0 && out[31] == 0 && out[32] == 0xAA);
		CHECK (r.pop_into (out, 77) && out[1] == 27 && out[4] == 0 && out[31] == 0x22);
		CHECK (!r.pop_into (out, 84));
		blk[31] = 0, blk[4] = 1;
		r.push_block (blk, 65530);					// late ones are dropped, across the wrap
		r.push_block (blk, 3);
		CHECK (r.pop_into (out, 3) && r.late == 1 && r.head == r.tail);
		for (unsigned i = 0; i < ExtraRing::kSlots + 2; i++) r.push_block (blk, (uint16_t) (100 + 7 * i));
		CHECK (r.dropped == 2 && r.head - r.tail == ExtraRing::kSlots);
		// the box's stream starts again: the queue is dropped, uncounted, and takes the new stream's bytes
		r.flush ();
		CHECK (r.head == r.tail && r.dropped == 2 && r.late == 1);
		CHECK (r.push_block (blk, 7) && r.pop_into (out, 7));
		// when the offset is set again: due in a block already sent, or further ahead than the margin plus the ring
		// (the Pi's counter went back to 0 under an offset set for a counter near 40000); a margin's distance is kept
		CHECK (!ExtraRing::stale (56, 0, 8) && !ExtraRing::stale (0, 0, 8));
		CHECK (ExtraRing::stale (65530, 3, 8));				// behind, across the wrap
		CHECK (!ExtraRing::stale (3 + (8 + ExtraRing::kSlots) * 7, 3, 8));
		CHECK (ExtraRing::stale (4 + (8 + ExtraRing::kSlots) * 7, 3, 8));
		CHECK (ExtraRing::stale ((uint16_t) (40000 + 56), 14, 8));
		CHECK (ExtraRing::stale ((uint16_t) (40000 + 56), 14, 256));	// the largest mirrormargin= too
		// a margin above the ring's slots (mirrormargin= takes up to 256): a fresh offset is not stale (with the ring
		// alone as the bound it was, and every block re-anchored and flushed)
		CHECK (!ExtraRing::stale (3 + 200 * 7, 3, 200) && !ExtraRing::stale (3 + 256 * 7, 3, 256));
		CHECK (ExtraRing::stale (3 + 200 * 7, 3, 0));
	}

	// mirrormargin=200, the way MirrorAudioOut and FillOut use the ring: the computer's and the Pi's blocks in step,
	// about every third with bytes (a plugin open). One anchor, and every block's bytes reach the Pi's block exactly
	// 200 blocks later: none lost, none late
	{
		static ExtraRing r;
		r.clear ();
		const unsigned margin = 200;
		uint8_t blk[368] = {}, out[368];
		uint16_t off = 0;
		bool set = false;
		unsigned anchors = 0, pushed = 0, popped = 0, wrong = 0;
		for (unsigned n = 0; n < 3000; n++)
		{
			const uint16_t comp = (uint16_t) (5000 + 7 * n), pi = (uint16_t) (100 + 7 * n);	// both wrap the counter
			if (set && ExtraRing::stale ((uint16_t) (comp + off), pi, margin))
			{
				if ((int16_t) (uint16_t) (comp + off - pi) > 0) r.flush ();
				set = false;
				anchors++;
			}
			if (!set) off = (uint16_t) (pi - comp + margin * 7), set = true;
			blk[4] = n % 3 == 0 ? (uint8_t) (1 + n / 3 % 200) : 0;
			if (r.push_block (blk, (uint16_t) (comp + off))) pushed++;
			memset (out, 0, sizeof out);
			if (r.pop_into (out, pi))
			{
				popped++;
				wrong += n < margin || out[4] != (uint8_t) (1 + (n - margin) / 3 % 200);	// the block 200 before
			}
		}
		CHECK (anchors == 0 && r.dropped == 0 && r.late == 0 && wrong == 0);
		CHECK (pushed == 1000 && popped == 934 && r.head - r.tail == 66);	// the last 200 blocks: 66 not due yet
	}

	printf ("test_mirror_desc: ok\n");
	return 0;
}
