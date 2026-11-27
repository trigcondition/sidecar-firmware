// pi/engine/teensy_hex.h - the Teensy 4.1's program as the Pi writes it into the Teensy's bootloader (docs/teensy-flash.md):
// an Intel HEX file (teensy/mirror_relay/build.ps1's mirror_relay.ino.hex) parsed into a flash image, and the reports the
// bootloader takes. Portable (no Circle): teensy_flash.cpp on the Pi and tests/test_teensy_hex.cpp on the build host.
//
// The bootloader (PJRC's HalfKay, USB 16C0:0478, one HID interface) takes the program as HID output reports, sent as
// SET_REPORT control requests (bmRequestType 0x21, bRequest 9, wValue 0x0200, wIndex 0), one per 1 KB block of flash:
//   bytes 0..2     the block's offset in flash, little endian (the hex's addresses less the FlexSPI base 0x60000000)
//   bytes 3..63    zero
//   bytes 64..1087 the block's 1024 bytes (0xFF where the hex has none)
// Block 0 is always sent (its write erases the chip), every other block only when it holds a byte other than 0xFF, in
// address order. A last report with offset 0xFFFFFF (and nothing else) starts the new program. While the chip erases the
// bootloader refuses or holds off the first writes, so a writer retries a refused block (up to 45 s for the first ones).
// This is the published format; the code here is written from it (teensy_loader_cli is not copied).
#ifndef _teensy_hex_h
#define _teensy_hex_h

#include <stdint.h>
#include <string.h>

namespace teensyhex {

constexpr uint32_t kFlashBase = 0x60000000;	// FlexSPI: where the hex puts the program
constexpr uint32_t kCodeSize = 8126464;		// the Teensy 4.1's program flash the bootloader writes (7936 KB)
constexpr uint32_t kBlock = 1024;
constexpr uint32_t kBlocks = kCodeSize / kBlock;
constexpr uint32_t kHeader = 64;
constexpr uint32_t kReport = kHeader + kBlock;	// 1088 bytes per SET_REPORT
constexpr uint32_t kBootAddr = 0xFFFFFF;
constexpr unsigned kMaxLine = 1 + 2 * (1 + 2 + 1 + 255 + 1);	// ':' and the longest record's hex digits

// the bootloader's SET_REPORT
constexpr uint8_t kReqType = 0x21, kReq = 9;
constexpr uint16_t kValue = 0x0200, kIndex = 0;

// The parser: fed the file in pieces (an upload's chunks), it fills the image and checks every record. The image is
// the caller's (kCodeSize bytes); begin() fills it with 0xFF.
class Parser
{
public:
	void begin (uint8_t *pImage)
	{
		m_p = pImage;
		memset (m_p, 0xFF, kCodeSize);
		memset (m_Used, 0, sizeof m_Used);
		m_nLine = 0; m_nLen = 0; m_nBase = 0; m_bEnd = false; m_pErr = 0; m_nErrLine = 0; m_nBytes = 0; m_nRecords = 0;
		m_nLow = kCodeSize; m_nHigh = 0;
	}

	// a piece of the file; false: an error (err() says which, at line errline())
	bool feed (const char *p, unsigned n)
	{
		for (unsigned i = 0; i < n && m_pErr == 0; i++)
		{
			char c = p[i];
			if (c == '\n' || c == '\r') { if (m_nLen) Line (); m_nLen = 0; continue; }
			if (m_nLen == 0) m_nLine++;
			if (m_nLen >= kMaxLine + 4) { Fail ("a line is too long"); break; }
			m_Buf[m_nLen++] = c;
		}
		return m_pErr == 0;
	}

	// the file is complete: false when it is not a whole program (no end record, no data, an error)
	bool finish (void)
	{
		if (m_pErr) return false;
		if (m_nLen) { Line (); m_nLen = 0; }
		if (m_pErr) return false;
		if (!m_bEnd) return Fail ("no end record (the file is cut short?)");
		if (m_nBytes == 0) return Fail ("no data");
		return true;
	}

	const char *err (void) const	{ return m_pErr; }
	unsigned errline (void) const	{ return m_nErrLine; }
	uint32_t bytes (void) const	{ return m_nBytes; }	// data bytes in the hex
	uint32_t low (void) const	{ return m_nLow; }	// lowest and highest+1 offset written
	uint32_t high (void) const	{ return m_nHigh; }
	// the blocks to send: block 0 and every block with a byte other than 0xFF
	bool sends (uint32_t nBlock) const
	{
		if (nBlock == 0) return true;
		if (nBlock >= kBlocks || !(m_Used[nBlock >> 3] & (1u << (nBlock & 7)))) return false;
		const uint8_t *b = m_p + nBlock * kBlock;
		for (uint32_t i = 0; i < kBlock; i++) if (b[i] != 0xFF) return true;
		return false;
	}
	// the next block to send from nFrom on (kBlocks: none)
	uint32_t next (uint32_t nFrom) const
	{
		while (nFrom < kBlocks && !sends (nFrom)) nFrom++;
		return nFrom;
	}
	uint32_t count (void) const	{ uint32_t n = 0; for (uint32_t b = next (0); b < kBlocks; b = next (b + 1)) n++; return n; }
	const uint8_t *image (void) const	{ return m_p; }

private:
	bool Fail (const char *pWhy) { if (!m_pErr) { m_pErr = pWhy; m_nErrLine = m_nLine; } return false; }
	static int Nib (char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		return -1;
	}
	void Line (void)
	{
		unsigned n = m_nLen;
		while (n && (m_Buf[n - 1] == ' ' || m_Buf[n - 1] == '\t')) n--;
		if (n == 0) return;
		if (m_bEnd) { Fail ("data after the end record"); return; }
		if (m_Buf[0] != ':') { Fail ("a line does not start with ':'"); return; }
		if (n < 11 || (n - 1) % 2) { Fail ("a record has an odd or too short length"); return; }
		uint8_t r[(kMaxLine + 4) / 2];
		unsigned nr = (n - 1) / 2;
		for (unsigned i = 0; i < nr; i++)
		{
			int h = Nib (m_Buf[1 + 2 * i]), l = Nib (m_Buf[2 + 2 * i]);
			if (h < 0 || l < 0) { Fail ("a record has a character that is not a hex digit"); return; }
			r[i] = (uint8_t) (h << 4 | l);
		}
		unsigned len = r[0];
		if (nr != len + 5) { Fail ("a record's length byte does not match the line"); return; }
		uint8_t sum = 0;
		for (unsigned i = 0; i < nr; i++) sum = (uint8_t) (sum + r[i]);
		if (sum != 0) { Fail ("a record's checksum is wrong"); return; }
		uint32_t addr = (uint32_t) r[1] << 8 | r[2];
		const uint8_t *d = r + 4;
		m_nRecords++;
		switch (r[3])
		{
		case 0x00:	// data
		{
			uint64_t a = (uint64_t) m_nBase + addr;
			if (a < kFlashBase || a + len > (uint64_t) kFlashBase + kCodeSize)
				{ Fail ("data outside the Teensy 4.1's program flash (0x60000000..0x607C0000)"); return; }
			uint32_t off = (uint32_t) (a - kFlashBase);
			memcpy (m_p + off, d, len);
			for (uint32_t b = off / kBlock; len && b <= (off + len - 1) / kBlock; b++) m_Used[b >> 3] |= (uint8_t) (1u << (b & 7));
			if (len) { if (off < m_nLow) m_nLow = off; if (off + len > m_nHigh) m_nHigh = off + len; }
			m_nBytes += len;
			break;
		}
		case 0x01: if (len != 0) { Fail ("an end record with data"); return; } m_bEnd = true; break;
		case 0x02: if (len != 2) { Fail ("a segment record not 2 bytes"); return; } m_nBase = ((uint32_t) d[0] << 8 | d[1]) << 4; break;
		case 0x04: if (len != 2) { Fail ("a linear address record not 2 bytes"); return; } m_nBase = ((uint32_t) d[0] << 8 | d[1]) << 16; break;
		case 0x03: case 0x05: break;	// a start address: the bootloader starts the program its own way
		default: Fail ("an unknown record type"); return;
		}
	}

	uint8_t *m_p;
	uint8_t m_Used[kBlocks / 8];		// blocks some data record touched
	char m_Buf[kMaxLine + 8];
	unsigned m_nLen, m_nLine, m_nErrLine;
	uint32_t m_nBase, m_nBytes, m_nRecords, m_nLow, m_nHigh;
	bool m_bEnd;
	const char *m_pErr;
};

// block nBlock's report (kReport bytes) from the image
inline void block_report (const uint8_t *pImage, uint32_t nBlock, uint8_t *pOut)
{
	uint32_t off = nBlock * kBlock;
	memset (pOut, 0, kHeader);
	pOut[0] = (uint8_t) off; pOut[1] = (uint8_t) (off >> 8); pOut[2] = (uint8_t) (off >> 16);
	memcpy (pOut + kHeader, pImage + off, kBlock);
}

// the last report: start the program
inline void boot_report (uint8_t *pOut)
{
	memset (pOut, 0, kReport);
	pOut[0] = pOut[1] = pOut[2] = 0xFF;
}

// how long a writer keeps retrying block number k of the write (0 = the first sent): the first ones wait for the erase
inline unsigned retry_ms (unsigned k) { return k < 4 ? 45000 : 2000; }

// the relay's build as its log's start line names it ("... build <stamp>"): copied into pOut (cap bytes), false if absent
inline bool find_build (const char *pLog, unsigned n, char *pOut, unsigned cap)
{
	static const char kKey[] = "mirror relay up, protocol ";
	const unsigned k = sizeof kKey - 1;
	bool found = false;
	for (unsigned i = 0; i + k <= n; i++)
	{
		if (memcmp (pLog + i, kKey, k) != 0) continue;
		unsigned j = i + k;
		while (j + 7 <= n && memcmp (pLog + j, " build ", 7) != 0 && pLog[j] != '\n') j++;
		if (j + 7 > n || pLog[j] == '\n') continue;
		j += 7;
		unsigned m = 0;
		while (j < n && pLog[j] != '\n' && pLog[j] != '\r' && m + 1 < cap) pOut[m++] = pLog[j++];
		pOut[m] = 0;
		found = true;		// the last start line wins (a log with two starts: the newest)
	}
	return found;
}

}	// namespace teensyhex

#endif
