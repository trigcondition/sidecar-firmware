// pi/engine/card_guard.h - testcard=1 (docs/pi5.md "Engine test mode"): the engine runs on the boot partition of Raspberry
// Pi OS, which is not its own card. What it may change there: Sidecar's folders, nothing else. This is the rule; the
// gate is card_guard.cpp, which wraps FatFs's calls that change a name (the linker's --wrap, PI5_TESTMODE=1 builds only).
// A change to anything else is refused with FR_DENIED, so config.txt, cmdline.txt, the kernels, overlays/ and whatever
// else lies at the root stay as they are, whichever of the engine's paths asks (the file port's PUT and DEL too).
// Reads are not limited. Other drives (USB:, NVME:) are not this card. pi/engine/tests/test_card_guard.cpp runs it on the Mac.
#ifndef CARD_GUARD_H
#define CARD_GUARD_H

namespace cardguard {

// Sidecar's folders at the root: /.rytm, /.sidecar, /.dt, /.a4 and fsweb.cpp's kContent; add a folder there and here
inline const char *const *folders (unsigned *pCount)
{
	static const char *const k[] = {".rytm", ".sidecar", ".dt", ".a4", "Bundles", "CHOP", "DX7", "GRANULAR", "JUNO", "MACHINES", "MULTISAMPLE",
					"NAM", "PRESETS", "Plugins", "Recordings", "Samples", "WAVETABLES"};
	*pCount = sizeof k / sizeof k[0];
	return k;
}

inline char lower (char c) { return c >= 'A' && c <= 'Z' ? (char) (c + 32) : c; }

inline bool same (const char *a, unsigned n, const char *b)		// a's n characters are b, in FAT's way: no case
{
	for (unsigned i = 0; i < n; i++)
		if (b[i] == 0 || lower (a[i]) != lower (b[i])) return false;
	return b[n] == 0;
}

// A FatFs path on the card: the part after its drive; 0 on one of the other drives, the only ones free (anything else,
// "0:" and "SD2:" too, counts as the card)
inline const char *on_card (const char *p)
{
	for (const char *q = p; *q && *q != '/'; q++)
		if (*q == ':')
		{
			const unsigned n = (unsigned) (q - p);
			return same (p, n, "USB") || same (p, n, "USB2") || same (p, n, "USB3") || same (p, n, "NVME") ? 0 : q + 1;
		}
	return p;
}

// The part of the path after the drive, in one of Sidecar's folders (or the folder itself); nothing with a ".." name
inline bool in_folder (const char *p)
{
	while (*p == '/') p++;
	unsigned n = 0;
	while (p[n] && p[n] != '/') n++;
	if (n == 0) return false;
	for (const char *q = p; *q; )
	{
		unsigned k = 0;
		while (q[k] && q[k] != '/') k++;
		if (k == 2 && q[0] == '.' && q[1] == '.') return false;
		q += k;
		while (*q == '/') q++;
	}
	unsigned nFolders;
	const char *const *pFolders = folders (&nFolders);
	for (unsigned i = 0; i < nFolders; i++)
		if (same (p, n, pFolders[i])) return true;
	return false;
}

// f_mkdir, f_unlink
inline bool change_ok (const char *pPath)
{
	const char *p = on_card (pPath);
	return p == 0 || in_folder (p);
}

// f_rename: the new name has no drive, it is on the old one's
inline bool rename_ok (const char *pOld, const char *pNew)
{
	const char *p = on_card (pOld);
	if (p == 0) return true;
	const char *q = on_card (pNew);
	return in_folder (p) && q != 0 && in_folder (q);
}

// f_open with a write or create mode. bCreate: it may make the file. /usbtest.log, a file made beforehand under Linux, is
// the test log (blackbox_pi.cpp TestLog): its bytes may be overwritten, it is never made here.
inline bool open_ok (const char *pPath, bool bCreate)
{
	const char *p = on_card (pPath);
	if (p == 0 || in_folder (p)) return true;
	while (*p == '/') p++;
	return !bCreate && same (p, 11, "usbtest.log") && p[11] == 0;
}

}	// namespace cardguard

#endif
