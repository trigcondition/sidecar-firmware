// pi/circle/hid_digitizer.h - a HID report descriptor parser for touch screens (digitizers) and a contact tracker,
// for Circle's USB touch screen driver (pi/circle/circle-touch.patch; build.sh copies this file into the Circle tree as
// include/circle/usb/hiddigitizer.h) and the Mac's unit test (pi/engine/tests/test_hid_digitizer.cpp).
//
// Why: Circle's own parser knew one descriptor style (a Usage per field, Report Count 1) and the factory chose the touch
// driver only for HID interfaces without a boot protocol. Cheap HDMI panels (e.g. GeeekPi 7", 0416:c169 "TSTP MTouch")
// declare their only interface a boot keyboard and send Windows-style multi-touch reports with report IDs on it.
//
// Also an absolute pointer: a Mouse/Pointer application with absolute X/Y, button 1 as the touch (one contact).
// What it understands: the HID 1.11 items (global state with Push/Pop, local Usage lists, Usage Minimum/Maximum,
// extended 32-bit usages), report IDs, the first Digitizer application collection Touch Screen (0x04), else Pen (0x02),
// its Finger (0x22) / Stylus (0x20) collections (or the application itself as one contact), Tip Switch, In Range,
// Confidence, Contact Identifier, X, Y (with their logical ranges), Contact Count (hybrid mode: a frame's contacts spread
// over several reports, the later ones with count 0), and the Device Configuration collection's Input Mode feature.
// No allocation, no library: freestanding C++ (stdint.h only).
#ifndef SIDECAR_HID_DIGITIZER_H
#define SIDECAR_HID_DIGITIZER_H

#include <stdint.h>

namespace hiddig {

static const unsigned MaxContacts = 10;		// contacts per report the layout keeps (Circle's MaxContactCount)

struct Field
{
	uint16_t BitOffset;			// from the first byte after the report ID
	uint8_t	 BitSize;			// 0: not in the report
	int32_t	 LogicalMin, LogicalMax;
};

struct Contact
{
	Field Tip, InRange, Confidence, ID, X, Y;
};

struct Layout
{
	uint8_t	 ReportID;			// the touch input report's ID, 0: the device uses none
	uint16_t ByteSize;			// that report's size, the ID byte included
	uint8_t	 Contacts;			// contacts per report
	bool	 Pen;				// a Pen application (no Touch Screen found)
	bool	 AbsPointer;			// a Generic Desktop Mouse/Pointer with absolute X/Y (no digitizer found)
	Contact	 C[MaxContacts];
	Field	 ContactCount;
	// the Device Configuration collection's Input Mode (SET_REPORT feature, value 2 = touch screen); ModeBitSize 0: none
	uint8_t	 ModeReportID;
	uint16_t ModeByteSize;			// the feature report's size, the ID byte included
	uint16_t ModeBitOffset;
	uint8_t	 ModeBitSize;
};

// Usages (page << 16 | id)
static const uint32_t UsageX = 0x00010030, UsageY = 0x00010031;
static const uint32_t UsagePen = 0x000D0002, UsageTouchScreen = 0x000D0004, UsageDeviceConfig = 0x000D000E;
static const uint32_t UsageStylus = 0x000D0020, UsageFinger = 0x000D0022, UsageInputMode = 0x000D0052;
static const uint32_t UsageTipSwitch = 0x000D0042, UsageInRange = 0x000D0032, UsageConfidence = 0x000D0047;
static const uint32_t UsageContactID = 0x000D0051, UsageContactCount = 0x000D0054;
static const uint32_t UsagePointer = 0x00010001, UsageMouse = 0x00010002, UsageButton1 = 0x00090001;

namespace detail {

struct Globals
{
	uint32_t Page;
	int32_t	 LogMin, LogMax;
	uint32_t LogMaxU;			// Logical Maximum unsigned (used when the minimum is not negative)
	uint32_t Size, Count, ReportID;
};

static inline int32_t SignExtend (uint32_t v, unsigned nBytes)
{
	if (nBytes == 1) return (int8_t) v;
	if (nBytes == 2) return (int16_t) v;
	return (int32_t) v;
}

// One pass over the descriptor for the first top-level application collection with usage nTarget
static inline bool ParseFor (const uint8_t *pDesc, unsigned nSize, uint32_t nTarget, Layout *pL)
{
	*pL = Layout ();
	Globals G = {0, 0, 0, 0, 0, 0, 0}, Stack[4];
	unsigned nStack = 0;
	uint32_t Usages[16];
	unsigned nUsages = 0;
	uint32_t UsageMin = 0, UsageMax = 0;
	bool bHaveMin = false, bHaveMax = false;

	static const unsigned MaxIDs = 16;	// bit offsets per report ID (IDs over 15 share the last slot)
	uint32_t InBits[MaxIDs] = {0}, FeatBits[MaxIDs] = {0};

	unsigned nColls = 0;			// collection depth
	int nAppLevel = -1;			// the target application's depth, -1: not in it
	int nContactLevel = -1;			// the open Finger/Stylus collection's depth
	int nCfgLevel = -1;			// the Device Configuration application's depth
	int nContact = -1;			// the contact being filled
	bool bDone = false;			// the target application closed: the rest is not ours
	bool bIDSet = false;
	bool bAppSeen = false;
	const bool bPointer = nTarget == UsageMouse || nTarget == UsagePointer;	// tip = button 1

	unsigned i = 0;
	while (i < nSize)
	{
		uint8_t b = pDesc[i++];
		if (b == 0xFE)			// long item: skipped
		{
			if (i + 2 > nSize) return false;
			i += 2 + pDesc[i];
			continue;
		}
		unsigned nBytes = (b & 3) == 3 ? 4 : (b & 3);
		if (i + nBytes > nSize) return false;
		uint32_t v = 0;
		for (unsigned k = 0; k < nBytes; k++) v |= (uint32_t) pDesc[i + k] << (8 * k);
		i += nBytes;
		uint8_t nType = (b >> 2) & 3, nTag = b >> 4;

		if (nType == 1)				// global
		{
			switch (nTag)
			{
			case 0: G.Page = v; break;
			case 1: G.LogMin = SignExtend (v, nBytes); break;
			case 2: G.LogMax = SignExtend (v, nBytes); G.LogMaxU = v; break;
			case 7: G.Size = v; break;
			case 8: G.ReportID = v; break;
			case 9: G.Count = v; break;
			case 10: if (nStack < 4) Stack[nStack++] = G; break;
			case 11: if (nStack) G = Stack[--nStack]; break;
			default: break;			// exponents, units, physical ranges: not needed
			}
			continue;
		}
		if (nType == 2)				// local
		{
			uint32_t u = nBytes == 4 ? v : (G.Page << 16 | v);
			switch (nTag)
			{
			case 0: if (nUsages < 16) Usages[nUsages++] = u; break;
			case 1: UsageMin = u; bHaveMin = true; break;
			case 2: UsageMax = u; bHaveMax = true; break;
			default: break;
			}
			continue;
		}
		if (nType != 0) continue;		// reserved

		// main items
		uint32_t nFirstUsage = nUsages ? Usages[0] : (bHaveMin ? UsageMin : 0);
		unsigned nID = G.ReportID < MaxIDs ? G.ReportID : MaxIDs - 1;
		switch (nTag)
		{
		case 10:				// Collection
			nColls++;
			if (v == 1 && nAppLevel < 0 && !bDone && nColls == 1 && nFirstUsage == nTarget)
			{
				nAppLevel = (int) nColls;
				bAppSeen = true;
			}
			else if (v == 1 && nColls == 1 && nFirstUsage == UsageDeviceConfig)
			{
				nCfgLevel = (int) nColls;
			}
			else if (nAppLevel > 0 && nContactLevel < 0 && (v == 2 || v == 0)
				 && (nFirstUsage == UsageFinger || nFirstUsage == UsageStylus))
			{
				nContactLevel = (int) nColls;
				nContact = pL->Contacts < MaxContacts ? pL->Contacts++ : -1;
				if (nContact < 0) nContact = MaxContacts;	// past the kept ones: offsets still counted
			}
			break;

		case 12:				// End Collection
			if (nColls == 0) return false;
			if ((int) nColls == nContactLevel) { nContactLevel = -1; nContact = -1; }
			if ((int) nColls == nAppLevel) { nAppLevel = -1; bDone = true; }
			if ((int) nColls == nCfgLevel) nCfgLevel = -1;
			nColls--;
			break;

		case 8:					// Input
		{
			bool bVar = (v & 0x02) != 0, bConst = (v & 0x01) != 0, bRel = (v & 0x04) != 0;
			uint32_t nBase = InBits[nID];
			InBits[nID] += G.Size * G.Count;
			if (nAppLevel < 0 || bConst || !bVar || G.Size == 0 || G.Size > 32) break;
			if (bIDSet && G.ReportID != pL->ReportID) break;	// another report inside the application
			for (unsigned f = 0; f < G.Count; f++)
			{
				uint32_t u;
				if (nUsages) u = Usages[f < nUsages ? f : nUsages - 1];
				else if (bHaveMin) { u = UsageMin + f; if (bHaveMax && u > UsageMax) u = UsageMax; }
				else continue;
				int32_t nMax = G.LogMin >= 0 && G.LogMax < G.LogMin ? (int32_t) G.LogMaxU : G.LogMax;
				Field Fd = {(uint16_t) (nBase + f * G.Size), (uint8_t) G.Size, G.LogMin, nMax};
				Field *pF = 0;
				int nC = nContact;
				if ((u == UsageX || u == UsageY) && bRel) continue;	// relative: a real mouse
				if (bPointer && u == UsageButton1) u = UsageTipSwitch;	// an absolute pointer: button 1 = touch
				else if (bPointer && u != UsageX && u != UsageY) continue;
				if (u == UsageContactCount) pF = &pL->ContactCount;
				else if (u == UsageX || u == UsageY || u == UsageTipSwitch || u == UsageInRange
					 || u == UsageConfidence || u == UsageContactID)
				{
					if (nC < 0)		// a contact's field outside a Finger/Stylus collection: the
					{			// application is one contact (pens, simple touch screens)
						if (pL->Contacts == 0) pL->Contacts = 1;
						nC = 0;
					}
					if (nC >= (int) MaxContacts) continue;
					Contact &C = pL->C[nC];
					pF = u == UsageX ? &C.X : u == UsageY ? &C.Y : u == UsageTipSwitch ? &C.Tip
					   : u == UsageInRange ? &C.InRange : u == UsageConfidence ? &C.Confidence : &C.ID;
				}
				if (!pF || pF->BitSize) continue;	// first one wins
				if (!bIDSet) { pL->ReportID = (uint8_t) G.ReportID; bIDSet = true; }
				*pF = Fd;
			}
			break;
		}

		case 11:				// Feature
		{
			uint32_t nBase = FeatBits[nID];
			FeatBits[nID] += G.Size * G.Count;
			if (nCfgLevel < 0 || (v & 0x01) || !(v & 0x02) || pL->ModeBitSize) break;
			for (unsigned f = 0; f < G.Count && f < nUsages; f++)
			{
				if (Usages[f] != UsageInputMode) continue;
				pL->ModeReportID = (uint8_t) G.ReportID;
				pL->ModeBitOffset = (uint16_t) (nBase + f * G.Size);
				pL->ModeBitSize = (uint8_t) (G.Size > 8 ? 8 : G.Size);
				break;
			}
			break;
		}

		default:				// Output: not needed
			break;
		}
		nUsages = 0;				// a main item ends the local state
		bHaveMin = bHaveMax = false;
	}
	if (!bAppSeen) return false;

	unsigned nRID = pL->ReportID < MaxIDs ? pL->ReportID : MaxIDs - 1;
	pL->ByteSize = (uint16_t) ((InBits[nRID] + 7) / 8 + (pL->ReportID ? 1 : 0));
	if (pL->ModeBitSize)
	{
		unsigned nM = pL->ModeReportID < MaxIDs ? pL->ModeReportID : MaxIDs - 1;
		pL->ModeByteSize = (uint16_t) ((FeatBits[nM] + 7) / 8 + (pL->ModeReportID ? 1 : 0));
	}

	// usable: every kept contact has X and Y; several contacts need IDs (or are told apart by their slot)
	unsigned n = 0;
	while (n < pL->Contacts && pL->C[n].X.BitSize && pL->C[n].Y.BitSize) n++;
	pL->Contacts = (uint8_t) n;
	return n > 0;
}

} // namespace detail

// The touch layout of a report descriptor: the first Touch Screen application, else the first Pen, else the first
// Generic Desktop Mouse (or Pointer) whose X/Y are absolute (touch panels that act as an absolute mouse, e.g. GeeekPi
// 0416:c169: button 1 = finger down). False: none usable (a relative mouse, a keyboard).
static inline bool Parse (const uint8_t *pDesc, unsigned nSize, Layout *pL)
{
	if (detail::ParseFor (pDesc, nSize, UsageTouchScreen, pL)) return true;
	if (detail::ParseFor (pDesc, nSize, UsagePen, pL)) { pL->Pen = true; return true; }
	if (detail::ParseFor (pDesc, nSize, UsageMouse, pL)) { pL->AbsPointer = true; return true; }
	if (detail::ParseFor (pDesc, nSize, UsagePointer, pL)) { pL->AbsPointer = true; return true; }
	return false;
}

// Does the descriptor name the Digitizer page (0x0D) at all (for the log of descriptors we could not use)
static inline bool MentionsDigitizer (const uint8_t *pDesc, unsigned nSize)
{
	for (unsigned i = 0; i < nSize; )
	{
		uint8_t b = pDesc[i++];
		unsigned nBytes = (b & 3) == 3 ? 4 : (b & 3);
		if (b == 0x05 && i < nSize && pDesc[i] == 0x0D) return true;
		if (b == 0x06 && i + 1 < nSize && pDesc[i] == 0x0D && pDesc[i + 1] == 0) return true;
		i += nBytes;
	}
	return false;
}

// LSB-first bits of a report (pData: after the report ID), 0 past its end
static inline uint32_t GetBits (const uint8_t *pData, unsigned nLen, const Field &F)
{
	uint32_t v = 0;
	for (unsigned k = 0; k < F.BitSize; k++)
	{
		unsigned nBit = F.BitOffset + k;
		if (nBit / 8 >= nLen) break;
		if (pData[nBit / 8] >> (nBit % 8) & 1) v |= (uint32_t) 1 << k;
	}
	return v;
}

// Write the Input Mode value into a feature report buffer of L.ModeByteSize bytes (the ID byte first, if any)
static inline void BuildInputMode (const Layout &L, uint8_t *pBuf, uint8_t nMode = 2)
{
	for (unsigned k = 0; k < L.ModeByteSize; k++) pBuf[k] = 0;
	unsigned nBase = 0;
	if (L.ModeReportID) { pBuf[0] = L.ModeReportID; nBase = 8; }
	for (unsigned k = 0; k < L.ModeBitSize; k++)
	{
		unsigned nBit = nBase + L.ModeBitOffset + k;
		if (nMode >> k & 1) pBuf[nBit / 8] |= (uint8_t) (1u << (nBit % 8));
	}
}

enum EventType { EventDown, EventMove, EventUp };

struct Event
{
	EventType Type;
	unsigned  Slot;				// 0..MaxContacts-1, the lowest free one at its down (0: the first finger)
	unsigned  X, Y;				// raw, in the X/Y logical range (Up: the last position)
};

// Reports -> down / move / up per slot. A frame is one report, or (Contact Count in hybrid mode) the reports until
// the count is used up; a contact not in a frame is up.
class Tracker
{
public:
	Tracker (void) : m_nRemaining (0), m_nIgnored (0), m_bRepeats (false)
	{
		for (unsigned i = 0; i < MaxContacts; i++) { m_bDown[i] = m_bSeen[i] = false; m_nID[i] = 0; m_nX[i] = m_nY[i] = 0; }
	}

	void SetLayout (const Layout &L) { m_L = L; }
	const Layout &GetLayout (void) const { return m_L; }
	unsigned GetIgnored (void) const { return m_nIgnored; }

	// The device repeats its report while a contact is held (a report without a change seen while down): then a
	// gap in the reports means the finger is up (see ReleaseAll)
	bool SendsRepeats (void) const { return m_bRepeats; }
	bool AnyDown (void) const
	{
		for (unsigned i = 0; i < MaxContacts; i++) if (m_bDown[i]) return true;
		return false;
	}
	// every contact up (a lost release); the events into pEv (room for MaxContacts)
	unsigned ReleaseAll (Event *pEv)
	{
		unsigned nEv = 0;
		for (unsigned s = 0; s < MaxContacts; s++)
		{
			if (!m_bDown[s]) continue;
			m_bDown[s] = false;
			pEv[nEv++] = Event {EventUp, s, m_nX[s], m_nY[s]};
		}
		m_nRemaining = 0;
		return nEv;
	}

	// One interrupt transfer: several reports may come back to back (GeeekPi 0416:c169: 64 bytes, four 8-byte
	// reports, the release often the last one). With a report ID each one of our size that starts with it is fed, in
	// order, until a short remainder or another byte (zero padding, another report); without IDs the first only. The
	// events into pEv (room for nMaxEv; reports that might not fit are left out); their number.
	unsigned FeedTransfer (const uint8_t *pData, unsigned nLen, Event *pEv, unsigned nMaxEv)
	{
		if (pData == 0 || m_L.ByteSize == 0 || nLen < m_L.ByteSize || !m_L.ReportID)
		{
			return nMaxEv >= 2 * MaxContacts ? Feed (pData, nLen, pEv) : 0;
		}
		unsigned nEv = 0;
		for (unsigned nOff = 0; nOff + m_L.ByteSize <= nLen; nOff += m_L.ByteSize)
		{
			if (nOff && pData[nOff] != m_L.ReportID) break;
			if (nMaxEv - nEv < 2 * MaxContacts) break;
			nEv += Feed (pData + nOff, m_L.ByteSize, pEv + nEv);
		}
		return nEv;
	}

	// one input report as received (with its ID byte); the events into pEv (room for 2 * MaxContacts); their number
	unsigned Feed (const uint8_t *pReport, unsigned nLen, Event *pEv)
	{
		if (pReport == 0 || nLen < m_L.ByteSize || (m_L.ReportID && pReport[0] != m_L.ReportID))
		{
			m_nIgnored++;			// another report (keyboard, mouse, a vendor one) or short
			return 0;
		}
		const uint8_t *p = pReport + (m_L.ReportID ? 1 : 0);
		unsigned nData = nLen - (m_L.ReportID ? 1 : 0);
		unsigned nEv = 0;

		unsigned n = m_L.Contacts;
		bool bFrameEnd = true;
		if (m_L.ContactCount.BitSize)
		{
			unsigned c = GetBits (p, nData, m_L.ContactCount);
			if (c > 0 || m_nRemaining == 0)	// a new frame (count 0 and none pending: all up)
			{
				for (unsigned i = 0; i < MaxContacts; i++) m_bSeen[i] = false;
				m_nRemaining = c;
			}
			n = m_nRemaining < n ? m_nRemaining : n;
			m_nRemaining -= n;
			bFrameEnd = m_nRemaining == 0;
		}
		else
		{
			for (unsigned i = 0; i < MaxContacts; i++) m_bSeen[i] = false;
		}

		for (unsigned i = 0; i < n; i++)
		{
			const Contact &C = m_L.C[i];
			bool bTouch = C.Tip.BitSize ? GetBits (p, nData, C.Tip) != 0
				    : C.InRange.BitSize ? GetBits (p, nData, C.InRange) != 0 : true;
			if (!bTouch) continue;
			unsigned nID = C.ID.BitSize ? GetBits (p, nData, C.ID) : i;
			unsigned x = GetBits (p, nData, C.X), y = GetBits (p, nData, C.Y);
			unsigned s;
			for (s = 0; s < MaxContacts; s++) if (m_bDown[s] && m_nID[s] == nID) break;
			if (s == MaxContacts)
			{
				for (s = 0; s < MaxContacts; s++) if (!m_bDown[s]) break;
				if (s == MaxContacts) continue;	// no free slot
				m_bDown[s] = true;
				m_nID[s] = nID;
				pEv[nEv++] = Event {EventDown, s, x, y};
			}
			else if (x != m_nX[s] || y != m_nY[s])
			{
				pEv[nEv++] = Event {EventMove, s, x, y};
			}
			else
			{
				m_bRepeats = true;	// held, reported again unchanged
			}
			m_nX[s] = x;
			m_nY[s] = y;
			m_bSeen[s] = true;
		}

		if (bFrameEnd)
		{
			for (unsigned s = 0; s < MaxContacts; s++)
			{
				if (m_bDown[s] && !m_bSeen[s])
				{
					m_bDown[s] = false;
					pEv[nEv++] = Event {EventUp, s, m_nX[s], m_nY[s]};
				}
			}
		}
		return nEv;
	}

private:
	Layout	 m_L;
	unsigned m_nRemaining;
	unsigned m_nIgnored;
	bool	 m_bRepeats;
	bool	 m_bDown[MaxContacts], m_bSeen[MaxContacts];
	unsigned m_nID[MaxContacts], m_nX[MaxContacts], m_nY[MaxContacts];
};

} // namespace hiddig

#endif
