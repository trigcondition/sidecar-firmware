// pi/engine/stor_vol.cpp - see stor_vol.h (portable: no libc beyond the language)
#include "stor_vol.h"
#include "stor_out.h"

namespace stor {

namespace {

const char *const kName[kVols] = {"SD", "USB", "USB2", "USB3", "NVME"};
const char *const kDrive[kVols] = {"SD:", "USB:", "USB2:", "USB3:", "NVME:"};
const char *const kDevice[kVols] = {"emmc1", "umsd1", "umsd2", "umsd3", "nvme1"};
const int kFatfs[kVols] = {0, 2, 3, 4, 6};

const Ops *s_pOps = 0;
Status s_St[kVols];

void say (int nLevel, const char *t)
{
	if (s_pOps && s_pOps->log) s_pOps->log (nLevel, t);
}

void esc (Out &o, const char *t)	// a JSON string's text: no escapes needed after this
{
	for (; *t; t++) o.c (*t == '"' || *t == '\\' || (unsigned char) *t < 0x20 ? '_' : *t);
}

char up (char c) { return c >= 'a' && c <= 'z' ? (char) (c - 32) : c; }

void mount_new (int v)
{
	Status &st = s_St[v];
	if (s_pOps->describe) s_pOps->describe (v, &st);
	Info in;
	int e = s_pOps->mount (v, &in);
	char b[200];
	Out o (b, sizeof b);
	if (e == 0)
	{
		st.state = kMounted, st.fs = in.fs, st.total = in.total, st.free = in.free, st.err = 0, st.why[0] = 0;
		st.free_known = in.free_known;
		st.mount_us = in.mount_us;
		st.mounts++;
		char l[160];
		line (v, l, sizeof l);
		o.s ("stor: ").s (l).s (" (mounted in ").u (in.mount_us / 1000).s (" ms)");
		say (0, b);
		return;
	}
	st.state = kPresent, st.err = e, st.fs = kFsNone, st.total = st.free = 0;
	st.why[0] = 0;
	if (s_pOps->diagnose) s_pOps->diagnose (v, st.why, sizeof st.why);
	o.s ("stor: ").s (kName[v]).s (" not mounted (FatFs error ").i (e).s (")");
	if (st.why[0]) o.s (": ").s (st.why);
	say (1, b);
}

}	// namespace

const char *name (int v) { return v >= 0 && v < kVols ? kName[v] : "?"; }
const char *drive (int v) { return v >= 0 && v < kVols ? kDrive[v] : ""; }
const char *device (int v) { return v >= 0 && v < kVols ? kDevice[v] : ""; }
int fatfs_index (int v) { return v >= 0 && v < kVols ? kFatfs[v] : -1; }

int parse_vol (const char *s, const char **pEnd)
{
	int best = -1, bestLen = 0;
	for (int v = 0; s && v < kVols; v++)
	{
		int n = 0;
		while (kName[v][n] && up (s[n]) == kName[v][n]) n++;
		if (kName[v][n] || (s[n] && s[n] != ':' && s[n] != '!' && s[n] != ' ')) continue;	// whole names only
		if (n > bestLen) best = v, bestLen = n;
	}
	if (pEnd) *pEnd = s ? s + bestLen : s;
	return best;
}

const char *state_name (int s)
{
	static const char *const k[] = {"absent", "present", "mounted", "ejected", "unavailable"};
	return s >= 0 && s <= kUnavailable ? k[s] : "?";
}
const char *fs_name (int f)
{
	static const char *const k[] = {"", "FAT12", "FAT16", "FAT32", "exFAT"};
	return f >= 0 && f <= kExfat ? k[f] : "?";
}
const char *link_name (int l, int gen)
{
	switch (l)
	{
	case kUsbLow:	return "USB 1 LowSpeed";
	case kUsbFull:	return "USB 1 FullSpeed";
	case kUsbHigh:	return "USB 2 HighSpeed";
	case kUsbSuper:	return "USB 3 SuperSpeed";
	case kSdHs:	return "SD HS 4-bit 50 MHz";
	case kPcie:	return gen >= 3 ? "PCIe gen 3 x1" : gen == 1 ? "PCIe gen 1 x1" : "PCIe gen 2 x1";
	default:	return "";
	}
}

void set_ops (const Ops *pOps) { s_pOps = pOps; }

void reset (void)
{
	for (int v = 0; v < kVols; v++) s_St[v] = Status ();
}

void set_available (int v, bool bOn)
{
	if (v <= kSD || v >= kVols) return;
	Status &st = s_St[v];
	if (!bOn) st = Status (), st.state = kUnavailable;
	else if (st.state == kUnavailable) st.state = kAbsent;
}

void set_sd (uint64_t nTotal, uint64_t nFree)
{
	Status &st = s_St[kSD];
	st.state = nTotal ? kMounted : kAbsent;
	st.total = nTotal, st.free = nFree;
	st.link = nTotal ? kSdHs : kLinkNone;
	if (!st.fs && nTotal) st.fs = kFat32;	// the boot card: FAT32 (the Pi's firmware reads FAT only)
}

void poll (void)
{
	if (!s_pOps) return;
	for (int v = kUSB; v < kVols; v++)
	{
		Status &st = s_St[v];
		if (st.state == kUnavailable) continue;
		bool p = s_pOps->present (v);
		switch (st.state)
		{
		case kAbsent:
			if (p) mount_new (v);
			break;
		case kPresent:
		case kMounted:
			if (p) break;
			{
				char b[120];
				Out o (b, sizeof b);
				if (st.state == kMounted)
				{
					s_pOps->unmount (v);
					st.pulls++;
					o.s ("stor: ").s (kName[v]).s (" was pulled while mounted (stor=eject:").s (kName[v])
					 .s (" first: a write in flight may be lost)");
					say (1, b);
				}
				else
				{
					o.s ("stor: ").s (kName[v]).s (" removed");
					say (0, b);
				}
				uint32_t m = st.mounts, pu = st.pulls, ej = st.ejects;
				st = Status ();
				st.mounts = m, st.pulls = pu, st.ejects = ej;
			}
			break;
		case kEjected:
			if (!p) st.gone = true;
			else if (st.gone) mount_new (v);	// a device again (a new one plugged, or a rescan)
			break;
		default:
			break;
		}
	}
}

const char *eject (int v)
{
	if (v == kSD) return "the SD card is the boot card: it is never ejected";
	if (v < 0 || v >= kVols) return "no such volume (SD, USB, USB2, USB3, NVME)";
	Status &st = s_St[v];
	if (st.state == kUnavailable) return "not on this board";
	if (st.state == kEjected) return "already ejected: safe to remove";
	if (st.state == kAbsent) return "nothing to eject";
	if (st.rec) return "the recorder writes to it (armed or recording): rec=stop first";
	if (st.busy) return "in use (the bench: stor=cancel first, or add ! to cancel it)";
	if (!s_pOps) return "no driver";
	if (st.state == kMounted) s_pOps->unmount (v);
	bool ok = s_pOps->eject (v);
	uint32_t m = st.mounts, pu = st.pulls, ej = st.ejects + 1;
	st = Status ();
	st.mounts = m, st.pulls = pu, st.ejects = ej;
	st.state = kEjected;
	st.gone = !s_pOps->present (v);
	char b[120];
	Out o (b, sizeof b);
	o.s ("stor: ").s (kName[v]).s (ok ? " ejected: safe to remove"
				       : v == kNVME ? " unmounted (the SSD stays attached until power-off; stor=mount:NVME mounts it again)"
						    : " unmounted (the USB device did not detach: safe to remove)");
	say (0, b);
	return 0;
}

const char *mount_again (int v)
{
	if (v <= kSD || v >= kVols) return "SD is mounted at boot only";
	Status &st = s_St[v];
	if (st.state == kMounted) return "already mounted";
	if (st.state == kUnavailable) return "not on this board";
	if (st.state == kAbsent) return "nothing plugged";
	st.state = kAbsent;	// kPresent / kEjected: the next poll mounts it when it is (still) in the tree
	return 0;
}

void set_why (int v, const char *pWhy)
{
	if (v < 0 || v >= kVols) return;
	int i = 0;
	for (; pWhy && pWhy[i] && i < (int) sizeof s_St[v].why - 1; i++) s_St[v].why[i] = pWhy[i];
	s_St[v].why[i] = 0;
}

void set_busy (int v, bool bBusy) { if (v >= 0 && v < kVols) s_St[v].busy = bBusy; }
void set_recording (int v, bool bOn) { if (v >= 0 && v < kVols) s_St[v].rec = bOn; }
void set_free (int v, uint64_t nFree) { if (v >= 0 && v < kVols) s_St[v].free = nFree, s_St[v].free_known = true; }
const Status &status (int v) { return s_St[v >= 0 && v < kVols ? v : 0]; }
bool mounted (int v) { return v >= 0 && v < kVols && s_St[v].state == kMounted; }

uint32_t piece (uintptr_t nAddr, uint32_t nLen)
{
	uint32_t room = kMaxPiece - (uint32_t) (nAddr & (kMaxPiece - 1));	// to the next 64 KB boundary
	return nLen < room ? nLen : room;
}

static void copy (uint8_t *d, const uint8_t *s, uint32_t n)	// (no libc here: 8 bytes a step, then the rest)
{
	uint32_t i = 0;
	for (; i + 8 <= n; i += 8) __builtin_memcpy (d + i, s + i, 8);
	for (; i < n; i++) d[i] = s[i];
}

bool split_io (uint8_t *pBuf, uint32_t nLen, uint64_t nOff, bool bWrite, uint8_t *pBounce,
	       bool (*io) (void *ctx, uint8_t *p, uint32_t n, uint64_t nOff), void *ctx)
{
	for (uint32_t d = 0; d < nLen;)
	{
		uint8_t *p = pBuf + d;
		uint32_t k;
		if (((uintptr_t) p & 511) == 0)		// (a 64 KB boundary is a sector's too: the pieces stay whole sectors)
		{
			k = piece ((uintptr_t) p, nLen - d);
			if (!io (ctx, p, k, nOff + d)) return false;
		}
		else
		{
			if (!pBounce || ((uintptr_t) pBounce & (kMaxPiece - 1))) return false;
			k = nLen - d < kMaxPiece ? nLen - d : kMaxPiece;
			if (bWrite) copy (pBounce, p, k);
			if (!io (ctx, pBounce, k, nOff + d)) return false;
			if (!bWrite) copy (p, pBounce, k);
		}
		d += k;
	}
	return true;
}

const char *classify_sector0 (const uint8_t *s, const uint8_t *pVbr, uint32_t *pNextLba)
{
	static char b[64];
	if (pNextLba) *pNextLba = 0;
	auto oem = [] (const uint8_t *q, const char *t) {
		for (int i = 0; t[i]; i++) if (q[3 + i] != (uint8_t) t[i]) return false;
		return true;
	};
	if (!s || s[510] != 0x55 || s[511] != 0xAA) return "no partition table or boot sector (blank?)";
	if (oem (s, "EXFAT   ")) return "exFAT without a partition table: FAT32 only in this build";
	if (oem (s, "NTFS    ")) return "NTFS: not supported (format FAT32)";
	bool fatvbr = (s[0] == 0xEB || s[0] == 0xE9) && s[11] == 0 && s[12] == 2;	// a jump and 512 bytes a sector
	int first = -1;
	uint32_t lba = 0;
	for (int k = 0; k < 4; k++)
	{
		const uint8_t *e = s + 446 + 16 * k;
		if (e[4] == 0xEE) return "GPT partition table: use the MBR scheme (FAT32)";
		if (e[4] && first < 0) first = e[4], lba = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t) e[11] << 24;
	}
	if (fatvbr && first < 0) return "a FAT volume FatFs did not accept (check it on a computer)";
	switch (first)
	{
	case -1:
		return "an empty partition table (format it FAT32)";
	case 0x07:
		if (!pVbr)
		{
			if (pNextLba) *pNextLba = lba ? lba : 1;
			return "partition type 07 (exFAT or NTFS)";
		}
		if (oem (pVbr, "EXFAT   ")) return "exFAT: FAT32 only in this build (STOR_EXFAT)";
		if (oem (pVbr, "NTFS    ")) return "NTFS: not supported (format FAT32)";
		return "partition type 07, neither exFAT nor NTFS";
	case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E:
		return "a FAT partition FatFs did not accept (check it on a computer)";
	case 0x83:
		return "a Linux partition: not supported (format FAT32)";
	case 0xAF:
		return "HFS+ / APFS: not supported (format FAT32)";
	default:
		{
			Out o (b, sizeof b);
			const char *hx = "0123456789ABCDEF";
			o.s ("partition type ").c (hx[first >> 4]).c (hx[first & 15]).s (": not supported (format FAT32)");
			return b;
		}
	}
}

int mb_text (uint64_t nBytes, char *pOut, int nCap)
{
	Out o (pOut, nCap);
	uint64_t mb = nBytes >> 20;
	if (mb >= 10240) o.u (mb / 1024).s (" GB");
	else if (mb >= 1024) o.fix (mb * 10 / 1024, 1).s (" GB");
	else o.u (mb).s (" MB");
	return o.len;
}

int line (int v, char *pOut, int nCap)
{
	Out o (pOut, nCap);
	const Status &st = status (v);
	o.s (name (v));
	for (int n = (int) o.len; n < 5; n++) o.c (' ');
	o.c (' ').s (state_name (st.state));
	if (st.state == kMounted)
	{
		char a[24], t[24];
		mb_text (st.free, a, sizeof a);
		mb_text (st.total, t, sizeof t);
		if (st.free_known) o.c (' ').s (fs_name (st.fs)).c (' ').s (a).s (" free of ").s (t);
		else o.c (' ').s (fs_name (st.fs)).c (' ').s (t).s (", free space not counted (stor=free:").s (name (v)).c (')');
	}
	if (st.link) o.s (", ").s (link_name (st.link, st.gen));
	if (st.model[0]) o.s (" (").s (st.model).s (")");
	if ((st.state == kPresent || st.state == kAbsent) && st.why[0]) o.s (": ").s (st.why);
	if (st.state == kEjected) o.s (": safe to remove");
	if (st.pulls) o.s (", pulled while mounted ").u (st.pulls).s ("x");
	return o.len;
}

int json_vols (char *pOut, int nCap, bool bExfat)
{
	Out o (pOut, nCap);
	o.s ("\"exfat\":").b (bExfat).s (",\"vols\":[");
	for (int v = 0; v < kVols; v++)
	{
		const Status &st = s_St[v];
		if (v) o.c (',');
		o.s ("{\"name\":\"").s (kName[v]).s ("\",\"state\":\"").s (state_name (st.state)).s ("\",\"fs\":\"")
		 .s (fs_name (st.fs)).s ("\",\"totalMB\":").u (st.total >> 20).s (",\"freeMB\":");
		if (st.free_known) o.u (st.free >> 20); else o.s ("null");
		o
		 .s (",\"link\":\"").s (link_name (st.link, st.gen)).s ("\",\"model\":\"");
		esc (o, st.model);
		o.s ("\",\"why\":\"");
		esc (o, st.why);
		o.s ("\",\"err\":").i (st.err).s (",\"busy\":").b (st.busy)
		 .s (",\"pulls\":").u (st.pulls).s (",\"mountMs\":").u (st.mount_us / 1000).s ("}");
	}
	o.c (']');
	return o.len;
}

}	// namespace stor
