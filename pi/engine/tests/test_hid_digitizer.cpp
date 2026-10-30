// test_hid_digitizer.cpp - pi/circle/hid_digitizer.h (the USB touch screen's descriptor parser and contact tracker,
// pi/circle/circle-touch.patch) on the Mac: a 0416:c169-style descriptor (one interface: a boot keyboard, a 2-contact
// hybrid multi-touch digitizer with report IDs, a Device Configuration Input Mode), a simple single-touch one without
// report IDs, a pen, and a keyboard + mouse that must not parse.
#include "../../circle/hid_digitizer.h"
#include "check.h"
#include <cstdio>
#include <cstring>

using namespace hiddig;

// 0416:c169-style: keyboard (ID 2), touch screen (ID 1, two Finger collections in two descriptor styles, Contact Count),
// Device Configuration (ID 3, Input Mode + Device Index)
static const uint8_t Tstp[] = {
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00,
	0x25, 0x65, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
	0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x85, 0x01,
	0x09, 0x22, 0xA1, 0x02,					// finger 1: a Usage per field
	0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
	0x09, 0x32, 0x81, 0x02,
	0x75, 0x06, 0x95, 0x01, 0x81, 0x03,
	0x09, 0x51, 0x75, 0x08, 0x25, 0x0A, 0x81, 0x02,
	0x05, 0x01, 0x15, 0x00, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
	0x05, 0x0D, 0xC0,
	0x09, 0x22, 0xA1, 0x02,					// finger 2: Usage lists, Report Count 2
	0x09, 0x42, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
	0x95, 0x06, 0x81, 0x03,
	0x09, 0x51, 0x75, 0x08, 0x95, 0x01, 0x25, 0x0A, 0x81, 0x02,
	0x05, 0x01, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
	0x05, 0x0D, 0xC0,
	0x09, 0x54, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,	// Contact Count
	0x09, 0x55, 0x25, 0x0A, 0xB1, 0x02,			// Contact Count Maximum (feature)
	0xC0,
	0x05, 0x0D, 0x09, 0x0E, 0xA1, 0x01, 0x85, 0x03, 0x09, 0x23, 0xA1, 0x02,
	0x09, 0x52, 0x09, 0x53, 0x15, 0x00, 0x25, 0x0A, 0x75, 0x08, 0x95, 0x02, 0xB1, 0x02, 0xC0, 0xC0,
};

// single touch, no report IDs (eGalax-like)
static const uint8_t Simple[] = {
	0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01, 0x09, 0x22, 0xA1, 0x02,
	0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07, 0x81, 0x03,
	0x05, 0x01, 0x26, 0xFF, 0x0F, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
	0xC0, 0xC0,
};

// a pen: Stylus as a physical collection
static const uint8_t PenDesc[] = {
	0x05, 0x0D, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x05, 0x09, 0x20, 0xA1, 0x00,
	0x09, 0x42, 0x09, 0x32, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03,
	0x05, 0x01, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x01, 0x09, 0x30, 0x81, 0x02, 0x09, 0x31, 0x81, 0x02,
	0xC0, 0xC0,
};

// a boot mouse's report descriptor (with a keyboard before it): no digitizer
static const uint8_t KbdMouse[] = {
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xC0,
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
	0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, 0x05, 0x01,
	0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0,
};

// the GeeekPi 7" (0416:c169) as measured on the Pi (2026-10-24, its 84-byte descriptor): an absolute mouse, ID 2,
// 3 buttons, X/Y 16-bit 0..32767 absolute, a relative wheel and AC Pan
static const uint8_t Geeekpi[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
	0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01,
	0x81, 0x01, 0x05, 0x01, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x55, 0x0e, 0x65, 0x11, 0x09, 0x30, 0x35,
	0x00, 0x46, 0x60, 0x02, 0x81, 0x02, 0x09, 0x31, 0x46, 0x54, 0x01, 0x81, 0x02, 0x09, 0x38, 0x15,
	0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01,
	0x81, 0x06, 0xc0, 0xc0,
};

// a Tstp touch report: two contact slots, the count
static void TstpReport (uint8_t *r, bool t1, unsigned id1, unsigned x1, unsigned y1,
			bool t2, unsigned id2, unsigned x2, unsigned y2, unsigned nCount)
{
	memset (r, 0, 14);
	r[0] = 1;
	r[1] = t1 ? 3 : 0; r[2] = id1; r[3] = x1 & 0xFF; r[4] = x1 >> 8; r[5] = y1 & 0xFF; r[6] = y1 >> 8;
	r[7] = t2 ? 3 : 0; r[8] = id2; r[9] = x2 & 0xFF; r[10] = x2 >> 8; r[11] = y2 & 0xFF; r[12] = y2 >> 8;
	r[13] = nCount;
}

int main ()
{
	Layout L;
	CHECK (Parse (Tstp, sizeof Tstp, &L));
	CHECK (!L.Pen);
	CHECK (L.ReportID == 1);
	CHECK (L.ByteSize == 14);
	CHECK (L.Contacts == 2);
	CHECK (L.C[0].Tip.BitOffset == 0 && L.C[0].Tip.BitSize == 1);
	CHECK (L.C[0].InRange.BitOffset == 1);
	CHECK (L.C[0].ID.BitOffset == 8 && L.C[0].ID.BitSize == 8);
	CHECK (L.C[0].X.BitOffset == 16 && L.C[0].X.BitSize == 16 && L.C[0].X.LogicalMax == 32767);
	CHECK (L.C[0].Y.BitOffset == 32 && L.C[0].Y.LogicalMax == 32767);
	CHECK (L.C[1].Tip.BitOffset == 48 && L.C[1].InRange.BitOffset == 49);
	CHECK (L.C[1].ID.BitOffset == 56);
	CHECK (L.C[1].X.BitOffset == 64 && L.C[1].Y.BitOffset == 80);
	CHECK (L.ContactCount.BitOffset == 96 && L.ContactCount.BitSize == 8);
	CHECK (L.ModeBitSize == 8 && L.ModeReportID == 3 && L.ModeBitOffset == 0 && L.ModeByteSize == 3);
	uint8_t Mode[3];
	BuildInputMode (L, Mode);
	CHECK (Mode[0] == 3 && Mode[1] == 2 && Mode[2] == 0);
	CHECK (MentionsDigitizer (Tstp, sizeof Tstp));

	Tracker T;
	T.SetLayout (L);
	Event E[2 * MaxContacts];
	uint8_t r[64];

	// a keyboard report (ID 2) and a short one: ignored
	uint8_t k[9] = {2, 0, 0, 0x04, 0, 0, 0, 0, 0};
	CHECK (T.Feed (k, sizeof k, E) == 0);
	TstpReport (r, true, 5, 100, 200, false, 0, 0, 0, 1);
	CHECK (T.Feed (r, 10, E) == 0);
	CHECK (T.GetIgnored () == 2);

	// one finger down, moved, held still, up; a report padded to 64 bytes is accepted
	CHECK (T.Feed (r, 14, E) == 1);
	CHECK (E[0].Type == EventDown && E[0].Slot == 0 && E[0].X == 100 && E[0].Y == 200);
	TstpReport (r, true, 5, 16384, 30000, false, 0, 0, 0, 1);
	CHECK (T.Feed (r, 64, E) == 1);
	CHECK (E[0].Type == EventMove && E[0].Slot == 0 && E[0].X == 16384 && E[0].Y == 30000);
	CHECK (T.Feed (r, 14, E) == 0);
	TstpReport (r, false, 5, 16384, 30000, false, 0, 0, 0, 1);	// tip off, still counted
	CHECK (T.Feed (r, 14, E) == 1);
	CHECK (E[0].Type == EventUp && E[0].Slot == 0);

	// two fingers in one report, then the first lifted (count 1, the second in slot 0 of the report)
	TstpReport (r, true, 1, 1000, 2000, true, 2, 3000, 4000, 2);
	CHECK (T.Feed (r, 14, E) == 2);
	CHECK (E[0].Type == EventDown && E[0].Slot == 0 && E[0].X == 1000);
	CHECK (E[1].Type == EventDown && E[1].Slot == 1 && E[1].X == 3000 && E[1].Y == 4000);
	TstpReport (r, true, 2, 3100, 4000, false, 0, 0, 0, 1);
	CHECK (T.Feed (r, 14, E) == 2);
	CHECK (E[0].Type == EventMove && E[0].Slot == 1 && E[0].X == 3100);
	CHECK (E[1].Type == EventUp && E[1].Slot == 0);
	TstpReport (r, false, 0, 0, 0, false, 0, 0, 0, 0);		// count 0, nothing pending: all up
	CHECK (T.Feed (r, 14, E) == 1);
	CHECK (E[0].Type == EventUp && E[0].Slot == 1);

	// hybrid: three contacts over two reports (count 3, then 0); the ups only at the frame's end
	TstpReport (r, true, 7, 10, 11, true, 8, 20, 21, 3);
	CHECK (T.Feed (r, 14, E) == 2);
	TstpReport (r, true, 9, 30, 31, false, 0, 0, 0, 0);
	CHECK (T.Feed (r, 14, E) == 1);
	CHECK (E[0].Type == EventDown && E[0].Slot == 2 && E[0].X == 30);
	TstpReport (r, true, 7, 10, 11, true, 9, 30, 31, 2);		// contact 8 gone
	CHECK (T.Feed (r, 14, E) == 1);
	CHECK (E[0].Type == EventUp && E[0].Slot == 1);
	TstpReport (r, false, 0, 0, 0, false, 0, 0, 0, 0);
	CHECK (T.Feed (r, 14, E) == 2);

	// single touch without report IDs
	CHECK (Parse (Simple, sizeof Simple, &L));
	CHECK (L.ReportID == 0 && L.ByteSize == 5 && L.Contacts == 1 && L.C[0].X.LogicalMax == 4095);
	CHECK (L.C[0].X.BitOffset == 8 && L.C[0].Y.BitOffset == 24 && !L.ContactCount.BitSize && !L.ModeBitSize);
	Tracker S;
	S.SetLayout (L);
	uint8_t s1[5] = {1, 0xFF, 0x0F, 0x00, 0x08};
	CHECK (S.Feed (s1, 5, E) == 1 && E[0].Type == EventDown && E[0].X == 4095 && E[0].Y == 2048);
	uint8_t s0[5] = {0, 0xFF, 0x0F, 0x00, 0x08};
	CHECK (S.Feed (s0, 5, E) == 1 && E[0].Type == EventUp);

	// a pen
	CHECK (Parse (PenDesc, sizeof PenDesc, &L));
	CHECK (L.Pen && L.ReportID == 5 && L.Contacts == 1 && L.ByteSize == 6);
	CHECK (L.C[0].Tip.BitOffset == 0 && L.C[0].InRange.BitOffset == 1 && L.C[0].X.BitOffset == 8);

	// the GeeekPi's absolute mouse: one contact, tip = button 1
	static_assert (sizeof Geeekpi == 84, "the measured descriptor is 84 bytes");
	CHECK (Parse (Geeekpi, sizeof Geeekpi, &L));
	CHECK (L.AbsPointer && !L.Pen && L.ReportID == 2 && L.ByteSize == 8 && L.Contacts == 1);
	CHECK (L.C[0].Tip.BitOffset == 0 && L.C[0].Tip.BitSize == 1);
	CHECK (L.C[0].X.BitOffset == 8 && L.C[0].X.BitSize == 16 && L.C[0].X.LogicalMin == 0 && L.C[0].X.LogicalMax == 32767);
	CHECK (L.C[0].Y.BitOffset == 24 && L.C[0].Y.LogicalMax == 32767);
	CHECK (!L.C[0].ID.BitSize && !L.ContactCount.BitSize && !L.ModeBitSize);
	CHECK (!MentionsDigitizer (Geeekpi, sizeof Geeekpi));
	{
		Tracker G;
		G.SetLayout (L);
		uint8_t down[8] = {2, 0x01, 0x00, 0x40, 0x00, 0x40, 0, 0};	// button 1, X = Y = 16384: the centre
		CHECK (G.Feed (down, 8, E) == 1);
		CHECK (E[0].Type == EventDown && E[0].Slot == 0 && E[0].X == 16384 && E[0].Y == 16384);
		// the centre of a 1024x600 screen, as the display's Map scales it (raw * (W - 1) / max)
		CHECK (16384u * 1023 / 32767 == 511 && 16384u * 599 / 32767 == 299);
		uint8_t move[8] = {2, 0x01, 0xff, 0x7f, 0x00, 0x00, 0, 0};	// to the top right
		CHECK (G.Feed (move, 8, E) == 1 && E[0].Type == EventMove && E[0].X == 32767 && E[0].Y == 0);
		CHECK (G.Feed (move, 8, E) == 0);
		uint8_t right[8] = {2, 0x02, 0xff, 0x7f, 0x00, 0x00, 0, 0};	// button 2 only: not a touch
		CHECK (G.Feed (right, 8, E) == 1 && E[0].Type == EventUp && E[0].Slot == 0);
		CHECK (G.Feed (down, 8, E) == 1 && E[0].Type == EventDown);
		uint8_t up[8] = {2, 0x00, 0x00, 0x40, 0x00, 0x40, 0, 0};	// buttons 0: up
		CHECK (G.Feed (up, 8, E) == 1 && E[0].Type == EventUp && E[0].Slot == 0);
		CHECK (G.Feed (up, 8, E) == 0);
		uint8_t other[8] = {1, 0x01, 0, 0, 0, 0, 0, 0};		// another report ID: ignored
		CHECK (G.Feed (other, 8, E) == 0 && G.GetIgnored () == 1);
	}
	{
		// the panel's real transfers (2026-10-24 log): 64 bytes, four 8-byte reports back to back, the release last,
		// zero padding after; every report counts, in order
		Tracker G;
		G.SetLayout (L);
		Event Ev[64];
		uint8_t t[64] = {0};
		const uint8_t held[8] = {0x02, 0x01, 0xbf, 0x7c, 0x70, 0x1d, 0x00, 0x00};
		for (int i = 0; i < 3; i++) memcpy (t + 8 * i, held, 8);
		memcpy (t + 24, held, 8);
		t[25] = 0x00;							// 02 00 bf 7c 70 1d 00 00: up
		CHECK (!G.SendsRepeats ());
		unsigned n = G.FeedTransfer (t, 64, Ev, 64);
		CHECK (n == 2);
		CHECK (Ev[0].Type == EventDown && Ev[0].X == 0x7cbf && Ev[0].Y == 0x1d70);
		CHECK (Ev[1].Type == EventUp && Ev[1].Slot == 0);
		CHECK (!G.AnyDown ());
		CHECK (G.SendsRepeats ());					// reports 2 and 3 repeated the held one
		CHECK (G.GetIgnored () == 0);					// the padding is not a report
		// a transfer of held reports only: down, no up; then a lost release: ReleaseAll
		memcpy (t + 24, held, 8);
		n = G.FeedTransfer (t, 64, Ev, 64);
		CHECK (n == 1 && Ev[0].Type == EventDown && G.AnyDown ());
		CHECK (G.FeedTransfer (t, 64, Ev, 64) == 0);			// still held
		CHECK (G.ReleaseAll (Ev) == 1 && Ev[0].Type == EventUp && !G.AnyDown ());
		// an 8-byte transfer (one report) and a short one
		CHECK (G.FeedTransfer (held, 8, Ev, 64) == 1 && Ev[0].Type == EventDown);
		CHECK (G.FeedTransfer (held, 5, Ev, 64) == 0);
		// a move inside one transfer: down, move, up in order
		uint8_t m[24];
		memcpy (m, held, 8); memcpy (m + 8, held, 8); memcpy (m + 16, held, 8);
		m[2] = 0x00; m[10] = 0x10; m[17] = 0x00;
		G.ReleaseAll (Ev);
		n = G.FeedTransfer (m, 24, Ev, 64);
		CHECK (n == 3 && Ev[0].Type == EventDown && Ev[1].Type == EventMove && Ev[1].X == 0x7c10 && Ev[2].Type == EventUp);
	}

	// keyboard + mouse (a relative mouse): not a touch screen
	CHECK (!Parse (KbdMouse, sizeof KbdMouse, &L));
	CHECK (!MentionsDigitizer (KbdMouse, sizeof KbdMouse));

	// truncated descriptors are refused, not overrun
	for (unsigned n = 0; n < sizeof Tstp; n++) Parse (Tstp, n, &L);

	printf ("test_hid_digitizer: OK\n");
	return 0;
}
