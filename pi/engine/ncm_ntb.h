// pi/engine/ncm_ntb.h - CDC-NCM (USB Communications Class, Network Control Model 1.0) for the Sidecar's USB-C network
// (docs/usb-network.md): the NTB16 framing both ways, the class requests on EP0 and the two notifications. Portable,
// header only, no Circle (tests/test_ncm_ntb.cpp).
//
// An NTB (NCM Transfer Block) is one bulk transfer: a 12-byte header (NTH16 "NCMH"), the datagrams (Ethernet frames
// without FCS) and a datagram pointer table (NDP16 "NCM0": index/length pairs, ended by a 0/0 pair). OUT (from the
// computer): parse16 checks every offset against the block before a datagram is handed on. IN (to the computer):
// Ntb16Builder packs frames into one block: NTH16, the datagrams 4-byte aligned, the NDP at the end.
//
// Only NTB16, no CRC (NCM0), one NDP chain of at most kMaxNdps; the device's limits are what GET_NTB_PARAMETERS says.
#ifndef _ncm_ntb_h
#define _ncm_ntb_h

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ncm {

constexpr uint32_t kNth16Sig = 0x484D434E;	// "NCMH"
constexpr uint32_t kNdp16Sig = 0x304D434E;	// "NCM0" (no CRC)
constexpr unsigned kNth16Len = 12;
constexpr unsigned kNdp16Min = 16;		// header 8 + one entry + the 0/0 end
constexpr unsigned kMinFrame = 14;		// an Ethernet header
constexpr unsigned kMaxFrame = 1514;		// without FCS (wMaxSegmentSize)
constexpr unsigned kMaxNdps = 4;		// a chain longer than this is refused (a loop)

// the device's NTB limits (GET_NTB_PARAMETERS)
constexpr uint32_t kNtbInMax = 16384;		// the largest IN block we build; the computer may ask for less (SET_NTB_INPUT_SIZE)
constexpr uint32_t kNtbInMin = 2048;		// the spec's floor for SET_NTB_INPUT_SIZE
constexpr uint32_t kNtbOutMax = 16384;		// the OUT transfer buffer
constexpr uint16_t kOutMaxDatagrams = 8;	// per OUT block (the RX ring keeps this many slots free before it re-arms)
constexpr uint16_t kAlign = 4;			// wNdpIn/OutDivisor and -Alignment

inline uint16_t rd16 (const uint8_t *p)			{ return (uint16_t) (p[0] | p[1] << 8); }
inline uint32_t rd32 (const uint8_t *p)			{ return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24; }
inline void wr16 (uint8_t *p, uint16_t v)		{ p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
inline void wr32 (uint8_t *p, uint32_t v)		{ wr16 (p, (uint16_t) v); wr16 (p + 2, (uint16_t) (v >> 16)); }

enum Parse { kOk, kShort, kBadNth, kBadLength, kBadNdp, kTooMany };

struct ParseStats { unsigned datagrams, bad; };

// An OUT block of nLength bytes: fn(pFrame, nFrameLength) for each datagram in order, at most nMax of them. kOk also when
// a datagram was skipped (outside the block, too short or too long: counted in bad); any other result: the block (or
// the rest of the chain) was refused, datagrams already handed on stay handed on.
template <class F>
inline Parse parse16 (const uint8_t *p, size_t nLength, unsigned nMax, F &&fn, ParseStats *pStats = 0)
{
	ParseStats st = {0, 0};
	Parse r = kOk;
	if (nLength < kNth16Len) r = kShort;
	else if (rd32 (p) != kNth16Sig || rd16 (p + 4) != kNth16Len) r = kBadNth;
	else
	{
		size_t nBlock = rd16 (p + 8);
		if (nBlock < kNth16Len + kNdp16Min || nBlock > nLength) r = kBadLength;
		else
		{
			size_t nNdp = rd16 (p + 10);
			for (unsigned hop = 0; r == kOk && nNdp != 0; hop++)
			{
				if (hop >= kMaxNdps) { r = kBadNdp; break; }
				if (nNdp < kNth16Len || nNdp % 4 != 0 || nNdp + 8 > nBlock || rd32 (p + nNdp) != kNdp16Sig) { r = kBadNdp; break; }
				size_t nNdpLen = rd16 (p + nNdp + 4);
				if (nNdpLen < kNdp16Min || nNdpLen % 4 != 0 || nNdp + nNdpLen > nBlock) { r = kBadNdp; break; }
				size_t nNext = rd16 (p + nNdp + 6);
				for (size_t e = nNdp + 8; e + 4 <= nNdp + nNdpLen; e += 4)
				{
					size_t nIdx = rd16 (p + e), nLen = rd16 (p + e + 2);
					if (nIdx == 0 || nLen == 0) break;		// the 0/0 end
					if (st.datagrams >= nMax) { r = kTooMany; break; }
					if (nIdx < kNth16Len || nIdx + nLen > nBlock || nLen < kMinFrame || nLen > kMaxFrame) { st.bad++; continue; }
					st.datagrams++;
					fn (p + nIdx, (unsigned) nLen);
				}
				if (nNext != 0 && nNext <= nNdp) { if (r == kOk) r = kBadNdp; break; }	// forward only: no loop
				nNdp = nNext;
			}
		}
	}
	if (pStats) *pStats = st;
	return r;
}

// An IN block: begin, add frames while they fit, finish. The length finish returns is never a multiple of the
// endpoint's 512 bytes unless it is the computer's whole NTB input size (the transfer would not end there: no ZLP).
class Ntb16Builder
{
public:
	void begin (uint8_t *pBuf, size_t nCap, uint16_t nSeq)
	{
		m_p = pBuf;
		m_nCap = nCap & ~(size_t) (kAlign - 1);	// whole words: a block that fills it is exactly the computer's size
		m_nSeq = nSeq;
		m_nOff = kNth16Len;
		m_nCount = 0;
	}
	// TRUE: the frame is in; FALSE: it does not fit (or is not a frame): the block is left as it was
	bool add (const uint8_t *pFrame, unsigned nLen)
	{
		if (nLen < kMinFrame || nLen > kMaxFrame || m_nCount >= kMaxPerBlock) return false;
		size_t nAt = align (m_nOff);
		size_t nEnd = nAt + nLen;
		if (align (nEnd) + ndp_len (m_nCount + 1) > m_nCap) return false;
		memset (m_p + m_nOff, 0, nAt - m_nOff);
		memcpy (m_p + nAt, pFrame, nLen);
		m_nIdx[m_nCount] = (uint16_t) nAt;
		m_nLen[m_nCount] = (uint16_t) nLen;
		m_nCount++;
		m_nOff = nEnd;
		return true;
	}
	unsigned count () const		{ return m_nCount; }
	// the block's length (0: no frame in it)
	size_t finish (size_t nPacket = 512)
	{
		if (m_nCount == 0) return 0;
		size_t nNdp = align (m_nOff);
		memset (m_p + m_nOff, 0, nNdp - m_nOff);
		size_t nNdpLen = ndp_len (m_nCount);
		uint8_t *q = m_p + nNdp;
		wr32 (q, kNdp16Sig);
		wr16 (q + 4, (uint16_t) nNdpLen);
		wr16 (q + 6, 0);
		for (unsigned i = 0; i < m_nCount; i++) { wr16 (q + 8 + 4 * i, m_nIdx[i]); wr16 (q + 10 + 4 * i, m_nLen[i]); }
		wr16 (q + 8 + 4 * m_nCount, 0);
		wr16 (q + 10 + 4 * m_nCount, 0);
		size_t nBlock = nNdp + nNdpLen;
		if (nPacket && nBlock % nPacket == 0 && nBlock + kAlign <= m_nCap)	// a full last packet: 4 bytes more
		{
			memset (m_p + nBlock, 0, kAlign);
			nBlock += kAlign;
		}
		wr32 (m_p, kNth16Sig);
		wr16 (m_p + 4, kNth16Len);
		wr16 (m_p + 6, m_nSeq);
		wr16 (m_p + 8, (uint16_t) nBlock);
		wr16 (m_p + 10, (uint16_t) nNdp);
		return nBlock;
	}
	static constexpr unsigned kMaxPerBlock = 16;

private:
	static size_t align (size_t n)			{ return (n + kAlign - 1) & ~(size_t) (kAlign - 1); }
	static size_t ndp_len (unsigned nEntries)	{ return 8 + 4 * (nEntries + 1); }	// + the 0/0 end (a multiple of 4)
	uint8_t *m_p = 0;
	size_t m_nCap = 0, m_nOff = 0;
	uint16_t m_nSeq = 0;
	unsigned m_nCount = 0;
	uint16_t m_nIdx[kMaxPerBlock], m_nLen[kMaxPerBlock];
};

// ---- EP0: the NCM class requests (bmRequestType class, recipient interface: the communication interface) ----------

enum Request : uint8_t
{
	kSendEncapsulated = 0x00, kGetEncapsulated = 0x01,
	kSetMulticastFilters = 0x40, kSetPmFilter = 0x41, kGetPmFilter = 0x42, kSetPacketFilter = 0x43, kGetStatistic = 0x44,
	kGetNtbParameters = 0x80, kGetNetAddress = 0x81, kSetNetAddress = 0x82, kGetNtbFormat = 0x83, kSetNtbFormat = 0x84,
	kGetNtbInputSize = 0x85, kSetNtbInputSize = 0x86, kGetMaxDatagram = 0x87, kSetMaxDatagram = 0x88,
	kGetCrcMode = 0x89, kSetCrcMode = 0x8A,
};

// The device's state of the requests above. request(): the answer's length for a device-to-host request (in pIn, at
// most wLength), 0 for a host-to-device one taken, -1 to STALL. pOut: a host-to-device request's data stage (wLength).
class Control
{
public:
	void reset ()
	{
		m_nNtbIn = kNtbInMax;
		m_nFilter = 0;
	}
	uint32_t ntb_in_size () const		{ return m_nNtbIn; }
	uint16_t packet_filter () const		{ return m_nFilter; }

	int request (uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wLength, const uint8_t *pOut, uint8_t *pIn)
	{
		const bool bIn = (bmRequestType & 0x80) != 0;
		uint8_t a[28];
		unsigned n = 0;
		switch (bRequest)
		{
		case kGetNtbParameters:
			if (!bIn) return -1;
			wr16 (a + 0, 28);
			wr16 (a + 2, 0x0001);			// NTB16 only
			wr32 (a + 4, kNtbInMax);
			wr16 (a + 8, kAlign);			// wNdpInDivisor
			wr16 (a + 10, 0);			// wNdpInPayloadRemainder
			wr16 (a + 12, kAlign);			// wNdpInAlignment
			wr16 (a + 14, 0);
			wr32 (a + 16, kNtbOutMax);
			wr16 (a + 20, kAlign);
			wr16 (a + 22, 0);
			wr16 (a + 24, kAlign);
			wr16 (a + 26, kOutMaxDatagrams);
			n = 28;
			break;
		case kGetNtbFormat:
			if (!bIn) return -1;
			wr16 (a, 0);				// NTB16
			n = 2;
			break;
		case kSetNtbFormat:
			return !bIn && wValue == 0 ? 0 : -1;	// NTB32 not supported
		case kGetNtbInputSize:
			if (!bIn) return -1;
			wr32 (a, m_nNtbIn);
			n = 4;
			break;
		case kSetNtbInputSize: {
			if (bIn || (wLength != 4 && wLength != 8) || pOut == 0) return -1;
			uint32_t v = rd32 (pOut);		// (an 8-byte form adds wNtbInMaxDatagrams: 0, no limit, is all we do)
			if (v < kNtbInMin || v > kNtbInMax) return -1;
			m_nNtbIn = v;
			return 0;
			}
		case kGetMaxDatagram:
			if (!bIn) return -1;
			wr16 (a, kMaxFrame);
			n = 2;
			break;
		case kSetMaxDatagram:
			if (bIn || wLength != 2 || pOut == 0) return -1;
			return rd16 (pOut) >= kMaxFrame ? 0 : -1;	// we never send more than kMaxFrame anyway
		case kGetCrcMode:
			if (!bIn) return -1;
			wr16 (a, 0);
			n = 2;
			break;
		case kSetCrcMode:
			return !bIn && wValue == 0 ? 0 : -1;	// no CRC
		case kSetPacketFilter:
			if (bIn) return -1;
			m_nFilter = wValue;			// (one computer on a point-to-point link: everything goes to it anyway)
			return 0;
		case kSetMulticastFilters:
		case kSetPmFilter:
			return bIn ? -1 : 0;
		default:
			return -1;
		}
		if (n > wLength) n = wLength;
		memcpy (pIn, a, n);
		return (int) n;
	}

private:
	uint32_t m_nNtbIn = kNtbInMax;
	uint16_t m_nFilter = 0;
};

// ---- the notifications on the interrupt IN endpoint ----------------------------------------------------------------

// CONNECTION_SPEED_CHANGE (16 bytes): the link's bit rates, down and up
inline unsigned notify_speed (uint8_t *p, uint8_t uchIface, uint32_t nBps)
{
	const uint8_t h[8] = {0xA1, 0x2A, 0, 0, uchIface, 0, 8, 0};
	memcpy (p, h, 8);
	wr32 (p + 8, nBps);
	wr32 (p + 12, nBps);
	return 16;
}

// NETWORK_CONNECTION (8 bytes): the cable "connected" (bUp) or not
inline unsigned notify_connection (uint8_t *p, uint8_t uchIface, bool bUp)
{
	const uint8_t h[8] = {0xA1, 0x00, (uint8_t) (bUp ? 1 : 0), 0, uchIface, 0, 0, 0};
	memcpy (p, h, 8);
	return 8;
}

}	// namespace ncm

#endif
