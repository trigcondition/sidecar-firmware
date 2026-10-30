// pi/engine/rec_pi.cpp - see rec_pi.h.
#include "rec_pi.h"
#include "bundle_pi.h"
#include "stor_pi.h"
#include "stor_vol.h"
#include "fsweb.h"		// live_take: what /fs leaves alone while a take records

#include <circle/alloc.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/timer.h>
#include <circle/util.h>
#include <fatfs/ff.h>
#include "rec/rec_wav.h"
#include "sendfx_pi.h"
#include "bridge.h"
#include "hooks.h"	// core::hooks.transport: the mods' PLAY / STOP arm the recorder too
#include "lib/library.h"	// the sample library: a take indexed (not hashed: docs/LIBRARY.md)
#include "bundle/projects.h"	// the wall clock (the web page's POST /time) and its zone: the take names, FatFs's dates

static const char From[] = "rec";

// a fixed message into an error buffer: one longer than the buffer does not compile (two usage lines once overran
// the stack's err[64])
template <unsigned N, unsigned M> static inline void SetErr (char (&pOut)[N], const char (&pMsg)[M])
{
	static_assert (M <= N, "the message is longer than its buffer");
	memcpy (pOut, pMsg, M);
}
rec::Recorder g_Rec;
recplay::Player g_Play;

namespace {

// The paths: "/Recordings/..." the SD card ("SD:" in front), "USB:/Recordings/..." another volume as it is (the
// recorder's destination, rec=vol:; recorder.h). On another volume (core 0's only, as everything here) every FIL sits
// in a 4 KB slot of one 64 KB-aligned block of the low heap and every byte of data passes through a 64 KB-aligned
// bounce block in pieces of <= 64 KB (stor::piece): Circle's xHCI puts a bulk transfer into one TRB (<= 64 KB, never
// across a 64 KB boundary: docs/STORAGE.md "The rules the volume layer keeps"), and FatFs hands the device the
// caller's buffer for whole sectors and the FIL's own for the rest. The SD keeps its static FILs and direct writes.
const int kSlotRead = rec::kSources, kSlotPlay = rec::kSources + 1, kSlots = rec::kSources + 3;	// + read_at / patch, playback's 2
static_assert (sizeof (FIL) <= 4096, "a FIL in a 4 KB slot");
static_assert (kSlots * 4096 <= 2 * 65536, "the slots in two 64 KB blocks");
u8 *s_pSlots = 0;			// kSlots x 4 KB, then (at 128 KB) the 64 KB bounce
u8 *s_pBounce = 0;
FIL *Slot (int i) { return (FIL *) (s_pSlots + 4096 * i); }

bool Other (const char *p) { return p[0] != '/'; }
void SdPath (char *pOut, unsigned n, const char *p) { strcpy (pOut, "SD:"); strncpy (pOut + 3, p, n - 4); pOut[n - 1] = 0; }
void FullPath (char *pOut, unsigned n, const char *p)
{
	if (Other (p)) { strncpy (pOut, p, n - 1); pOut[n - 1] = 0; }
	else SdPath (pOut, n, p);
}
int VolOf (const char *p) { return Other (p) ? stor::parse_vol (p) : stor::kSD; }
// another volume's: mounted now (a pulled drive is unmounted by stor_pi's pass: from then on no call reaches it)
bool OtherOk (const char *p) { int v = VolOf (p); return s_pBounce && v > stor::kSD && stor::mounted (v); }

// f_write / f_read on another volume: through the bounce, in pieces
bool XWrite (FIL *f, const void *p, uint32_t n)
{
	const u8 *b = (const u8 *) p;
	for (uint32_t d = 0; d < n; )
	{
		UINT k = stor::piece ((uintptr_t) s_pBounce, n - d), w = 0;	// (the bounce is aligned: 64 KB a piece)
		memcpy (s_pBounce, b + d, k);
		if (f_write (f, s_pBounce, k, &w) != FR_OK || w != k) return false;
		d += k;
	}
	return true;
}
int XRead (FIL *f, void *p, uint32_t n)
{
	u8 *b = (u8 *) p;
	uint32_t d = 0;
	while (d < n)
	{
		UINT k = stor::piece ((uintptr_t) s_pBounce, n - d), r = 0;
		if (f_read (f, s_pBounce, k, &r) != FR_OK) return d ? (int) d : -1;
		memcpy (b + d, s_pBounce, r);
		d += r;
		if (r < k) break;	// the file's end
	}
	return (int) d;
}

FIL s_F[rec::kSources];
FIL *s_pF[rec::kSources];		// the handle's FIL: s_F[h] (the SD) or slot h (another volume)
bool s_bOther[rec::kSources];
bool s_bUsed[rec::kSources];
char s_Path[rec::kSources][160];	// each open take file's (the library's touched at its close)

bool BMkdir (const char *p)
{
	if (Other (p) && !OtherOk (p)) return false;
	char sd[200];
	FullPath (sd, sizeof sd, p);
	FRESULT r = f_mkdir (sd);
	return r == FR_OK || r == FR_EXIST;
}
int BOpen (const char *p)
{
	bool bOther = Other (p);
	if (bOther && !OtherOk (p)) return -1;
	char sd[200];
	FullPath (sd, sizeof sd, p);
	for (int i = 0; i < rec::kSources; i++)
	{
		if (s_bUsed[i]) continue;
		FIL *f = bOther ? Slot (i) : &s_F[i];
		if (f_open (f, sd, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;
		s_pF[i] = f, s_bOther[i] = bOther, s_bUsed[i] = true;
		strncpy (s_Path[i], p, sizeof s_Path[i] - 1);
		s_Path[i][sizeof s_Path[i] - 1] = 0;
		return i;
	}
	return -1;
}
bool BWrite (int h, const void *p, uint32_t n)
{
	if (s_bOther[h]) return OtherOk (s_Path[h]) && XWrite (s_pF[h], p, n);
	UINT w = 0;
	return f_write (s_pF[h], p, n, &w) == FR_OK && w == n;
}
bool BPwrite (int h, uint64_t nOff, const void *p, uint32_t n)
{
	FIL *f = s_pF[h];
	if (s_bOther[h] && !OtherOk (s_Path[h])) return false;
	FSIZE_t nEnd = f_tell (f);		// back to where the writes go (the file is longer while space is reserved)
	if (f_lseek (f, nOff) != FR_OK) return false;
	bool ok;
	if (s_bOther[h]) ok = XWrite (f, p, n);
	else
	{
		UINT w = 0;
		ok = f_write (f, p, n, &w) == FR_OK && w == n;
	}
	return ok && f_lseek (f, nEnd) == FR_OK;
}
bool BSync (int h) { return (!s_bOther[h] || OtherOk (s_Path[h])) && f_sync (s_pF[h]) == FR_OK; }
bool BClose (int h)
{
	bool ok = (!s_bOther[h] || OtherOk (s_Path[h])) && f_close (s_pF[h]) == FR_OK;	// (unmounted: FatFs's own refusal)
	s_bUsed[h] = false;
	if (ok && !s_bOther[h] && rec::is_wav_name (s_Path[h]))	// (core 0's, as the library) the SD's /Recordings:
		core::lib::touched (s_Path[h], 0, 0);				// indexed, hashed on demand
	return ok;
}
void BAbandon (int h) { s_bUsed[h] = false; }	// a drive that stopped answering: its FIL forgotten (FF_FS_LOCK 0: no table)
bool BSpace (uint64_t *pFree, uint64_t *pTotal)
{
	DWORD nFree;
	FATFS *pFs;
	if (f_getfree ("SD:", &nFree, &pFs) != FR_OK) return false;
	*pFree = (uint64_t) nFree * pFs->csize * 512;
	*pTotal = (uint64_t) (pFs->n_fatent - 2) * pFs->csize * 512;
	return true;
}
// another volume's: stor's figures (a FAT32 without FSINFO: its total, free not counted - stor=free:VOL counts it;
// f_getfree is cheap once FatFs knows the count, which it keeps up to date as the take grows)
bool BSpaceAt (const char *pRoot, uint64_t *pFree, uint64_t *pTotal)
{
	int v = stor::parse_vol (pRoot);
	if (v <= stor::kSD || !stor::mounted (v)) return false;
	const stor::Status &st = stor::status (v);
	*pTotal = st.total;
	*pFree = ~0ull;				// not counted (recorder.h): shown as unknown
	if (!st.free_known) return true;
	DWORD nFree;
	FATFS *pFs;
	if (f_getfree (pRoot, &nFree, &pFs) != FR_OK) return false;
	*pFree = (uint64_t) nFree * pFs->csize * FF_MAX_SS;
	stor::set_free (v, *pFree);
	return true;
}
bool BReady (const char *pRoot) { int v = stor::parse_vol (pRoot); return s_pBounce && v > stor::kSD && stor::mounted (v); }
bool BRemove (const char *p)
{
	if (Other (p) && !OtherOk (p)) return false;
	char sd[200];
	FullPath (sd, sizeof sd, p);
	return f_unlink (sd) == FR_OK;
}
uint32_t BNow (void) { return CTimer::GetClockTicks (); }
// -1 no such folder, -2 an error (a drive gone, a read error mid-listing): never a partial count (recorder.h)
int BList (const char *p, char (*pNames)[64], uint64_t *pSizes, bool *pDirs, int nMax)
{
	if (Other (p) && !OtherOk (p)) return -2;
	char sd[200];
	FullPath (sd, sizeof sd, p);
	DIR d;
	FILINFO fi;
	FRESULT r = f_opendir (&d, sd);
	if (r != FR_OK) return r == FR_NO_PATH || r == FR_NO_FILE ? -1 : -2;
	int n = 0;
	while (n < nMax)
	{
		if (f_readdir (&d, &fi) != FR_OK) { n = -2; break; }
		if (!fi.fname[0]) break;
		strncpy (pNames[n], fi.fname, 63);
		pNames[n][63] = 0;
		pSizes[n] = fi.fsize;
		pDirs[n] = (fi.fattrib & AM_DIR) != 0;
		n++;
	}
	f_closedir (&d);
	return n;
}
int BMkdirNew (const char *p)		// a take's folder: 1 made now, 0 there already (FR_EXIST), -1 an error
{
	if (Other (p) && !OtherOk (p)) return -1;
	char sd[200];
	FullPath (sd, sizeof sd, p);
	FRESULT r = f_mkdir (sd);
	return r == FR_OK ? 1 : r == FR_EXIST ? 0 : -1;
}
bool BReadAt (const char *p, uint64_t nOff, void *pBuf, uint32_t n)
{
	bool bOther = Other (p);
	if (bOther && !OtherOk (p)) return false;
	char sd[300];
	FullPath (sd, sizeof sd, p);
	FIL fl, *f = bOther ? Slot (kSlotRead) : &fl;
	if (f_open (f, sd, FA_READ) != FR_OK) return false;
	bool ok = f_lseek (f, nOff) == FR_OK;
	if (ok && bOther) ok = XRead (f, pBuf, n) == (int) n;
	else if (ok)
	{
		UINT got = 0;
		ok = f_read (f, pBuf, n, &got) == FR_OK && got == n;
	}
	f_close (f);
	return ok;
}
bool BPatch (const char *p, uint64_t nOff, const void *pBuf, uint32_t n)
{
	bool bOther = Other (p);
	if (bOther && !OtherOk (p)) return false;
	char sd[300];
	FullPath (sd, sizeof sd, p);
	FIL fl, *f = bOther ? Slot (kSlotRead) : &fl;
	if (f_open (f, sd, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return false;
	bool ok = f_lseek (f, nOff) == FR_OK;
	if (ok && bOther) ok = XWrite (f, pBuf, n);
	else if (ok)
	{
		UINT w = 0;
		ok = f_write (f, pBuf, n, &w) == FR_OK && w == n;
	}
	return f_close (f) == FR_OK && ok;
}
void BLog (const char *pLine) { CLogger::Get ()->Write (From, LogNotice, "%s", pLine + (strncmp (pLine, "rec: ", 5) ? 0 : 5)); }
bool BCardBusy (void) { return bridge::card_busy (); }	// the streams need the card (core/msamp/cardq.h)
// the take's name (recorder.h): the wall clock's local time, the Rytm project's name. ProjectName is the one place
// that knows where the core keeps the current project (core::project, the Rytm's 10 name, written by the control core:
// copied a byte at a time up to its NUL, 23 characters at most)
bool BClock (uint32_t *pLocal) { int nSrc; return core::projects::clock_local (pLocal, &nSrc); }
void ProjectName (char *pOut, unsigned nCap)
{
	const char *p = bridge::project ();
	unsigned n = 0;
	for (; n + 1 < nCap && n < 23 && p[n]; n++) pOut[n] = p[n];
	pOut[n] = 0;
}
// space up front (recorder.h): f_expand gives the empty file a contiguous area (its writes then touch no FAT); the
// close cuts it at the write position; recover cuts a closed file. FF_USE_EXPAND: pi/engine/build.sh sets it in the
// tree's ffconf.h - a tree without it records as before (no reservation).
#if FF_USE_EXPAND
bool BReserve (int h, uint64_t nBytes)
{
	if (s_bOther[h] && !OtherOk (s_Path[h])) return false;
	return f_expand (s_pF[h], (FSIZE_t) nBytes, 1) == FR_OK;
}
#endif
bool BTruncate (int h) { return (!s_bOther[h] || OtherOk (s_Path[h])) && f_truncate (s_pF[h]) == FR_OK; }
bool BCut (const char *p, uint64_t nSize)
{
	bool bOther = Other (p);
	if (bOther && !OtherOk (p)) return false;
	char sd[300];
	FullPath (sd, sizeof sd, p);
	FIL fl, *f = bOther ? Slot (kSlotRead) : &fl;
	if (f_open (f, sd, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return false;
	bool ok = f_lseek (f, nSize) == FR_OK && f_tell (f) == nSize && f_truncate (f) == FR_OK;
	return f_close (f) == FR_OK && ok;
}

const rec::Backend s_Card = {BMkdir, BOpen, BWrite, BPwrite, BSync, BClose, BSpace, BNow, BList, BReadAt, BPatch, BLog, BCardBusy,
			     BReady, BSpaceAt, BRemove, BAbandon, BMkdirNew, BClock, ProjectName,
#if FF_USE_EXPAND
			     BReserve, BTruncate,
#else
			     0, 0,
#endif
			     BCut};

// the volumes with takes besides the SD: each mounted drive ("USB:" ...), for playback's and the REC page's lists
int Roots (const char **pOut, int nMax)
{
	int n = 0;
	for (int v = stor::kUSB; v < stor::kVols && n < nMax; v++)
		if (s_pBounce && stor::mounted (v)) pOut[n++] = stor::drive (v);
	return n;
}

// playback's reads (rec_play.h): its own files, a stamp per entry (FatFs' date and time: larger is newer)
FIL s_PF[2];
FIL *s_pPF[2];
bool s_bPOther[2];
char s_PPath[2][120];
bool s_bPUsed[2];
int PList (const char *p, char (*pNames)[64], uint64_t *pSizes, bool *pDirs, uint32_t *pStamps, int nMax)
{
	if (Other (p) && !OtherOk (p)) return -1;
	char sd[200];
	FullPath (sd, sizeof sd, p);
	DIR d;
	FILINFO fi;
	if (f_opendir (&d, sd) != FR_OK) return -1;
	int n = 0;
	while (n < nMax && f_readdir (&d, &fi) == FR_OK && fi.fname[0])
	{
		strncpy (pNames[n], fi.fname, 63);
		pNames[n][63] = 0;
		pSizes[n] = fi.fsize;
		pDirs[n] = (fi.fattrib & AM_DIR) != 0;
		pStamps[n] = (uint32_t) fi.fdate << 16 | fi.ftime;
		n++;
	}
	f_closedir (&d);
	return n;
}
int POpen (const char *p)
{
	bool bOther = Other (p);
	if (bOther && !OtherOk (p)) return -1;
	char sd[300];
	FullPath (sd, sizeof sd, p);
	for (int i = 0; i < 2; i++)
	{
		if (s_bPUsed[i]) continue;
		FIL *f = bOther ? Slot (kSlotPlay + i) : &s_PF[i];
		if (f_open (f, sd, FA_READ) != FR_OK) return -1;
		s_pPF[i] = f, s_bPOther[i] = bOther, s_bPUsed[i] = true;
		strncpy (s_PPath[i], p, sizeof s_PPath[i] - 1);
		s_PPath[i][sizeof s_PPath[i] - 1] = 0;
		return i;
	}
	return -1;
}
int PRead (int h, void *p, uint32_t n)
{
	if (s_bPOther[h]) return OtherOk (s_PPath[h]) ? XRead (s_pPF[h], p, n) : -1;
	UINT got = 0;
	return f_read (s_pPF[h], p, n, &got) == FR_OK ? (int) got : -1;
}
bool PSeek (int h, uint64_t nOff) { return (!s_bPOther[h] || OtherOk (s_PPath[h])) && f_lseek (s_pPF[h], nOff) == FR_OK; }
void PClose (int h)
{
	if (!s_bPOther[h] || OtherOk (s_PPath[h])) f_close (s_pPF[h]);	// (a pulled drive's: forgotten)
	s_bPUsed[h] = false;
}
void PLog (const char *pLine) { CLogger::Get ()->Write ("play", LogNotice, "%s", pLine + (strncmp (pLine, "play: ", 6) ? 0 : 6)); }

const recplay::Backend s_PlayCard = {PList, POpen, PRead, PSeek, PClose, PLog, Roots};
int s_nRecWas;		// the recorder's state at the last poll: a take that ends selects it for playback

// The REC page's takes (sidecar_rec.h): the newest SRV_TAKES folders of /Recordings (the SD's and each mounted drive's:
// "USB:take_004"), read a step a loop pass (the
// folder's listing, then one take's: its WAVs' sizes and the first one's header for the length), never while a take
// records or closes (the card is the recorder's then); published whole when the last step is done.
bool s_bUiWant, s_bScanWant, s_bScopeWant;	// the REC page / the SCOPE page shows (the meters on while either does)
int s_nScanStep = -1, s_nScanN;			// -1: idle; 0: the listing; 1..n: take n - 1
char s_ScanTake[SRV_TAKES][rec::kTakeMax];
srv_take_t s_ScanOut[SRV_TAKES];
srv_take_t s_Takes[SRV_TAKES];
int s_nTakes = -1;
unsigned s_nTakesGen;
char s_Names[128][64];				// a listing (core 0's only)
uint64_t s_Sizes[128];
bool s_bDirs[128];
uint32_t s_Stamps[128];
char s_UiErr[64];				// a refused command of the page's, shown 3 s
unsigned s_nUiErrAt;

// a before b in a take list: the stamp, then the name (whichever volume: take_010 before USB:take_009), then the id
bool TakeNewer (uint32_t sa, const char *a, uint32_t sb, const char *b)
{
	if (sa != sb) return sa > sb;
	const char *ca = strchr (a, ':'), *cb = strchr (b, ':');
	int c = strcmp (ca ? ca + 1 : a, cb ? cb + 1 : b);
	return c ? c > 0 : strcmp (a, b) > 0;
}

bool TakeName (const char *p)			// a take's folder as the recorder names them: no path, no dots
{
	unsigned n = 0;
	for (; p[n]; n++)
		if (!((p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= 'a' && p[n] <= 'z') || (p[n] >= '0' && p[n] <= '9') || p[n] == '_' || p[n] == '-'))
			return false;
	return n > 0 && n < rec::kTakeMax - rec::kRootMax;
}

void ScanStep (void)
{
	if (s_nScanStep < 0)
	{
		if (!s_bScanWant) return;
		s_bScanWant = false;
		s_nScanStep = 0;
	}
	if (s_nScanStep == 0)
	{
		const char *pRoots[8] = {""};
		int nRoots = 1 + Roots (pRoots + 1, 7);
		s_nScanN = 0;
		for (int r = 0; r < nRoots; r++)
		{
			char dir[24], id[rec::kTakeMax];
			strcpy (dir, pRoots[r]);
			strcat (dir, "/Recordings");
			int n = PList (dir, s_Names, s_Sizes, s_bDirs, s_Stamps, 128);
			for (int i = 0; i < n; i++)		// the newest SRV_TAKES (stamp, then name: take_010 after take_009)
			{
				if (!s_bDirs[i] || !TakeName (s_Names[i]) || strlen (pRoots[r]) + strlen (s_Names[i]) >= sizeof id) continue;
				strcpy (id, pRoots[r]);
				strcat (id, s_Names[i]);
				int k = s_nScanN;
				while (k > 0)
				{
					int j = k - 1;
					uint32_t sj = s_ScanOut[j].kb;	// (the stamp, kept in kb until the take is read)
					if (TakeNewer (sj, s_ScanTake[j], s_Stamps[i], id)) break;
					if (j + 1 < SRV_TAKES) { memcpy (s_ScanTake[j + 1], s_ScanTake[j], sizeof s_ScanTake[j]); s_ScanOut[j + 1].kb = sj; }
					k = j;
				}
				if (k >= SRV_TAKES) continue;
				strcpy (s_ScanTake[k], id);
				s_ScanOut[k].kb = s_Stamps[i];
				if (s_nScanN < SRV_TAKES) s_nScanN++;
			}
		}
		s_nScanStep = 1;
		return;
	}
	int t = s_nScanStep - 1;
	if (t < s_nScanN)
	{
		char dir[96], path[192];
		recplay::take_folder (dir, sizeof dir, s_ScanTake[t]);
		int n = BList (dir, s_Names, s_Sizes, s_bDirs, 64);
		srv_take_t &o = s_ScanOut[t];
		memset (&o, 0, sizeof o);
		strcpy (o.name, s_ScanTake[t]);
		uint64_t bytes = 0;
		int first = -1;
		for (int i = 0; i < n; i++)
		{
			if (s_bDirs[i] || !rec::is_wav_name (s_Names[i])) continue;
			o.files++;
			bytes += s_Sizes[i];
			if (first < 0 || strcmp (s_Names[i], s_Names[first]) < 0) first = i;
		}
		o.kb = (unsigned) (bytes >> 10);
		if (first >= 0)				// the length: every file of a take has the same
		{
			uint8_t h[recwav::kHeader];
			strcpy (path, dir);
			strcat (path, "/");
			strcat (path, s_Names[first]);
			uint32_t a = BReadAt (path, 0, h, sizeof h) ? recwav::ours (h) : 0;
			if (a && s_Sizes[first] > recwav::kHeader) o.secs = (unsigned) ((s_Sizes[first] - recwav::kHeader) / a / recwav::kRate);
		}
		s_nScanStep++;
		return;
	}
	memcpy (s_Takes, s_ScanOut, sizeof s_Takes);
	s_nTakes = s_nScanN;
	s_nTakesGen++;
	s_nScanStep = -1;
}

// rec=del:TAKE (the SD's) or del:VOL:TAKE (a drive's: "USB:take_004") - its WAVs, then its folder; not while a take
// records (or closes); its playback stopped first
bool DelTake (const char *pTake, char *pErr, unsigned nErr)
{
	const char *c = strchr (pTake, ':'), *pName = c ? c + 1 : pTake;
	int v = c ? stor::parse_vol (pTake) : stor::kSD;
	if (!TakeName (pName) || v < 0 || (c && v == stor::kSD)) { strncpy (pErr, "del: a take's name (USB:NAME on a drive)", nErr - 1); return false; }
	if (v != stor::kSD && !(s_pBounce && stor::mounted (v))) { strncpy (pErr, "that drive is not mounted", nErr - 1); return false; }
	rec::Status s;
	g_Rec.status (&s, false);
	if (s.state == rec::Recorder::kRecording || s.state == rec::Recorder::kEnded)
	{
		strncpy (pErr, "a take is recording: stop first", nErr - 1);
		return false;
	}
	recplay::Status ps;
	g_Play.status (&ps);
	if (g_Play.active () && strcmp (ps.take, pTake) == 0)
	{
		g_Play.stop ();				// fades out in the OUT fill (10 ms), then closes here
		for (unsigned t0 = CTimer::GetClockTicks (); g_Play.active () && CTimer::GetClockTicks () - t0 < 50000; )
			g_Play.poll ();
		g_Play.poll ();
		if (g_Play.active ()) { strncpy (pErr, "busy: try again", nErr - 1); return false; }
	}
	char dir[96], sd[200];
	recplay::take_folder (dir, sizeof dir, pTake);
	int n = BList (dir, s_Names, s_Sizes, s_bDirs, 64), gone = 0;
	if (n < 0) { strncpy (pErr, "no such take", nErr - 1); return false; }
	for (int i = 0; i < n; i++)
	{
		if (s_bDirs[i]) continue;
		char p[192];
		strcpy (p, dir);
		strcat (p, "/");
		strcat (p, s_Names[i]);
		FullPath (sd, sizeof sd, p);
		if (f_unlink (sd) != FR_OK) { strncpy (pErr, "could not delete a file", nErr - 1); return false; }
		if (!Other (p)) core::lib::touched (p, 0, 0);	// gone from the library (the SD's)
		gone++;
	}
	FullPath (sd, sizeof sd, dir);
	if (f_unlink (sd) != FR_OK) { strncpy (pErr, "the folder is not empty", nErr - 1); return false; }
	CLogger::Get ()->Write (From, LogNotice, "deleted %s (%d files)", pTake, gone);
	g_Play.scan ();				// its list without the take
	s_bScanWant = true;
	return true;
}

unsigned s_nBufMB;
unsigned s_nLastLogUs;
bool s_bWasMounted[stor::kVols];	// the drives as the last poll saw them (one back: its take recovered, the lists again)
bool s_bRescan;				// a drive came or went while a take recorded: the lists once it is done
unsigned s_nSdAt, s_nSdFreeMB, s_nSdTotalMB;	// the SD's space while the recorder's destination is a drive

const char *VolName (const char *pRoot) { return pRoot[0] ? stor::name (stor::parse_vol (pRoot)) : "SD"; }
int s_nPendVol = -1;			// a take on this drive ended abandoned: recovered here once the drive has settled
unsigned s_nPendAt;

// the destination held while armed or recording: no eject (stor::set_recording); at once after a command (rec=arm
// then stor=eject:USB in one batch is refused) and every loop pass
void HoldDest (void)
{
	rec::Status s;
	g_Rec.status (&s, false);
	int nDest = s.root[0] ? stor::parse_vol (s.root) : stor::kSD;
	for (int v = stor::kUSB; v < stor::kVols; v++) stor::set_recording (v, s.state != rec::Recorder::kIdle && v == nDest);
}
bool DestOk (const char *pRoot) { return !pRoot[0] || (s_pBounce && stor::mounted (stor::parse_vol (pRoot))); }

// The destinations' write speeds for the warning before a take (recorder.h rate_warning): stor=bench's sequential
// 64 KB writes (BenchDone), kept in SD:/.rytm/recspeed.txt; 0: not measured (the volume's default then)
uint32_t s_Speed[stor::kVols];
const char *s_VolNames[stor::kVols];
const char kSpeedFile[] = "SD:/.rytm/recspeed.txt";
void SpeedNames (void) { for (int v = 0; v < stor::kVols; v++) s_VolNames[v] = stor::name (v); }
void SpeedLoad (void)
{
	SpeedNames ();
	FIL f;
	if (f_open (&f, kSpeedFile, FA_READ) != FR_OK) return;
	char t[256];
	UINT got = 0;
	if (f_read (&f, t, sizeof t - 1, &got) != FR_OK) got = 0;
	f_close (&f);
	t[got] = 0;
	rec::parse_speeds (t, s_VolNames, stor::kVols, s_Speed);
}
void SpeedSave (void)
{
	char t[256];
	int n = rec::format_speeds (t, sizeof t, s_VolNames, stor::kVols, s_Speed);
	FIL f;
	f_mkdir ("SD:/.rytm");
	if (f_open (&f, kSpeedFile, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
	UINT w;
	f_write (&f, t, (UINT) n, &w);
	f_close (&f);
}
// the destination's speed (KB/s) and whether it was measured
uint32_t DestSpeed (const char *pRoot, bool *pMeasured)
{
	int v = pRoot[0] ? stor::parse_vol (pRoot) : stor::kSD;
	*pMeasured = v >= 0 && v < stor::kVols && s_Speed[v] != 0;
	return *pMeasured ? s_Speed[v] : rec::default_write_kbps (pRoot);
}

// The wall clock into Circle's timer (FatFs's get_fattime reads CTimer::GetLocalTime: the files' and folders' dates):
// the browser's (POST /time, bundleweb.cpp) and its zone, set again when they drift apart by more than 2 s
void ClockSync (void)
{
	uint32_t ep;
	int nSrc;
	if (!core::projects::clock_now (&ep, &nSrc)) return;
	CTimer *T = CTimer::Get ();
	int nZone = core::projects::clock_zone ();
	unsigned nNow = T->GetUniversalTime ();
	if (T->GetTimeZone () == nZone && nNow + 2 >= ep && nNow <= ep + 2) return;
	T->SetTimeZone (nZone);
	if (T->SetTime (ep, FALSE))
		CLogger::Get ()->Write (From, LogNotice, "clock: %s time set (UTC%+d:%02d): take names and file dates from now",
					core::projects::clock_name (nSrc), nZone / 60, (nZone < 0 ? -nZone : nZone) % 60);
}

}  // namespace

namespace rec_pi {

// the mods' transport report (core/seq: whatever the Rytm's MIDI TRANSPORT SEND is): the recorder's arm only - the
// other Transport () users keep the MIDI messages (the sampler's PLAY starts, the send FX's clock)
static void ModsTransport (uint8_t nStatus) { g_Rec.transport (nStatus); }

void Init (void)
{
	core::hooks.transport = ModsTransport;
	CLogger *L = CLogger::Get ();
	// recbuf=MB (cmdline), else sized from the free heap (memalign's: the low heap), 64..256 MB (recorder.h recbuf_mb)
	unsigned nFreeMB = (unsigned) (CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_LOW) >> 20);
	unsigned nHighMB = (unsigned) (CMemorySystem::Get ()->GetHeapFreeSpace (HEAP_HIGH) >> 20);
	unsigned nAsked = CKernelOptions::Get ()->GetAppOptionDecimal ("recbuf", rec::kRecbufAuto);
	s_nBufMB = rec::recbuf_mb (nAsked, nFreeMB, nHighMB);
	size_t nBytes = s_nBufMB ? (size_t) s_nBufMB << 20 : 0;
	void *p = 0;
	if (nBytes)
	{
		size_t nStage = (size_t) rec::kSources * rec::kChunk;
		if (nBytes < nStage + (2u << 20)) nBytes = nStage + (2u << 20);
		p = memalign (64, nBytes);
		if (p == 0) L->Write (From, LogError, "no memory for the recorder (%u MB): recording off", s_nBufMB);
	}
	if (p)	// another volume's FILs and bounce (rec=vol:USB): 64 KB-aligned, low heap; without them the SD only
	{
		u8 *q = (u8 *) CMemorySystem::HeapAllocate (4 * 65536, HEAP_DMA30);
		if (q)
		{
			s_pSlots = (u8 *) (((uintptr) q + 0xFFFF) & ~(uintptr) 0xFFFF);	// (never freed)
			s_pBounce = s_pSlots + 2 * 65536;
		}
		else L->Write (From, LogWarning, "no memory for drive I/O (256 KB): recording to the SD only");
	}
	g_Rec.init (p, p ? nBytes : 0, &s_Card);
	g_Play.init (&s_PlayCard, BNow);
	if (p == 0)
	{
		if (nAsked == 0) L->Write (From, LogNotice, "recording off (recbuf=0)");
		else if (!s_nBufMB) L->Write (From, LogWarning, "recording off: no room for its memory (%u MB of the low heap free, %u MB kept for the rest)",
					      nFreeMB, rec::kRecbufKeep);
		return;					// (memalign's failure: logged above)
	}
	SpeedLoad ();
	unsigned t0 = CTimer::GetClockTicks ();
	g_Rec.recover ("");			// the SD's marker: its take cut back to the lengths it holds (space reserved)
	int nFixed = g_Rec.fix_all ("/Recordings");
	rec::Status s;
	g_Rec.status (&s, true);
	int nTakes = g_Play.scan ();
	L->Write (From, LogNotice, "%u MB reserved at %p (%u MB were free; ring %u ms at the default selection), boot scan of /Recordings: %d fixed in %u ms, %d takes to play",
		  (unsigned) (nBytes >> 20), p, nFreeMB, s.ring_ms, nFixed, (CTimer::GetClockTicks () - t0) / 1000, nTakes);
#if !FF_USE_EXPAND
	L->Write (From, LogNotice, "no space reserved up front (this Circle tree's FatFs has no f_expand: pi/engine/build.sh)");
#endif
}

// Playback's requests (the REC page, the console, the web page) on core 0: never while a take records (or closes).
static bool RecBusy (void)
{
	rec::Status s;
	g_Rec.status (&s, false);
	return s.state == rec::Recorder::kRecording || s.state == rec::Recorder::kEnded;
}
static bool PlayStart (char *pErr, unsigned nErr)
{
	if (RecBusy ())
	{
		strncpy (pErr, "a take is recording", nErr - 1);
		return false;
	}
	recplay::Status ps;
	g_Play.status (&ps);
	if (ps.takes == 0) g_Play.scan ();
	return g_Play.play (pErr, nErr);
}

static void Say (void);

// The remote UI's REC page (core/ui/page_rec.cpp, drawn on core 3): its request (YES, encoder A) run here on core 0,
// the recorder's state handed over every 200 ms and after each request.
static void RuiPoll (void)
{
	static unsigned s_nLast, s_nErrAt, s_nPlayErrAt;
	static char s_Err[32], s_PlayErr[32];
	unsigned now = CTimer::GetClockTicks ();
	int nReq = bridge::rui_rec_take ();
	unsigned nKeys = bridge::rui_rec_toggles ();
	if (nKeys)		// 1026: T1..T8 the tracks' IN channels (2..9), T9 MASTER, T10 / T11 IN L / R (the FX return)
	{
		uint32_t m = g_Rec.mask ();
		for (int k = 0; k < 11; k++)
			if (nKeys >> k & 1) m ^= 1u << (k < 8 ? rec::kIn + 2 + k : k == 8 ? rec::kMaster : rec::kIn + k + 1);
		char err[64] = "";
		if (!g_Rec.configure (m, g_Rec.bits (), err, sizeof err))
		{
			CLogger::Get ()->Write (From, LogWarning, "remote page: channels: %s", err);
			strncpy (s_Err, err, sizeof s_Err - 1);
			s_nErrAt = now;
		}
		Say ();
	}
	int nToggle, nCh, nTake, nLevel;
	bool bPlayIn = bridge::rui_play_take (&nToggle, &nCh, &nTake, &nLevel);
	if (bPlayIn)		// B pushed / turned, C, D (core/ui/page_rec.cpp)
	{
		char err[64] = "";
		bool ok = true;
		if (nLevel) g_Play.set_level (g_Play.level () + nLevel);
		if (nTake) g_Play.step_take (nTake);		// C right: older takes (the list is newest first)
		if (nCh) g_Play.step_channel (nCh);
		if (nToggle)
		{
			if (g_Play.active ()) g_Play.stop ();
			else ok = PlayStart (err, sizeof err);
		}
		if (!ok) CLogger::Get ()->Write ("play", LogWarning, "remote page: %s", err);
		strncpy (s_PlayErr, ok ? "" : err, sizeof s_PlayErr - 1);
		s_nPlayErrAt = now;
	}
	if (nReq)
	{
		char err[64] = "";
		bool ok = true;
		if ((nReq == 1 || nReq == 3) && g_Play.active ()) g_Play.stop ();	// never play while recording
		if (nReq == 1) ok = g_Rec.start (err, sizeof err);
		else if (nReq == 3) ok = g_Rec.arm (err, sizeof err);
		else if (nReq == 2 || nReq == 4) g_Rec.stop ();
		static const char *Name[] = {"", "start", "stop", "arm", "disarm"};
		if (!ok) CLogger::Get ()->Write (From, LogWarning, "remote page: %s: %s", Name[nReq >= 0 && nReq <= 4 ? nReq : 0], err);
		strncpy (s_Err, ok ? "" : err, sizeof s_Err - 1);
		s_nErrAt = now;
		Say ();
	}
	else if (!bPlayIn && !nKeys && now - s_nLast < 200000) return;
	s_nLast = now;
	if (s_Err[0] && now - s_nErrAt > 3000000) s_Err[0] = 0;	// an error shows 3 s
	if (s_PlayErr[0] && now - s_nPlayErrAt > 3000000) s_PlayErr[0] = 0;
	recplay::Status ps;
	g_Play.status (&ps);
	bridge::RuiPlay pl = {};
	pl.state = ps.state, pl.level = ps.level, pl.ch_i = ps.ch_i, pl.channels = ps.channels, pl.take_i = ps.take_i;
	pl.takes = ps.takes, pl.pos_ms = (unsigned) (ps.pos / 48), pl.len_ms = (unsigned) (ps.len / 48);
	strncpy (pl.take, ps.take, sizeof pl.take - 1);
	strncpy (pl.channel, ps.channel, sizeof pl.channel - 1);
	strncpy (pl.error, s_PlayErr[0] ? s_PlayErr : ps.error, sizeof pl.error - 1);
	bridge::rui_play (pl);
	rec::Status s;
	g_Rec.status (&s, false);
	bridge::RuiRec r = {};
	r.state = !s.ready ? 0 : s.state == rec::Recorder::kArmed ? 2 : s.state == rec::Recorder::kRecording ? 3
		: s.state == rec::Recorder::kEnded ? 4 : 1;
	r.secs = (unsigned) (s.frames / 48000);
	r.free_mb = (unsigned) (s.free_bytes >> 20), r.total_mb = (unsigned) (s.total_bytes >> 20), r.need_kbps = s.need_kbps;
	r.in_mask = (s.mask >> rec::kIn & 0xFFF) | (s.mask & (1u << rec::kMaster) ? 3 : 0);	// MASTER: IN 0 / 1
	r.ch_select = 1;
	strncpy (r.take, s.take, sizeof r.take - 1);
	strncpy (r.error, s_Err[0] ? s_Err : s.error, sizeof r.error - 1);
	bridge::rui_rec (r);
}

void Poll (void)
{
	ClockSync ();
	g_Rec.poll ();
	g_Play.poll ();
	rec::Status s;
	g_Rec.status (&s, false);
	if (s.state == rec::Recorder::kRecording || s.state == rec::Recorder::kEnded)
		g_Play.stop ();				// a take started (the Rytm's PLAY on an armed recorder): playback fades out
	if (s.state == rec::Recorder::kIdle && s_nRecWas != rec::Recorder::kIdle && s_nRecWas != rec::Recorder::kArmed && s.take[0])
	{
		g_Play.scan (s.take);			// a take ended: it is the one to play
		if (s_bUiWant) s_bScanWant = true;	// and the REC page's list
	}
	// the drives: the destination held while armed or recording (no eject); one mounted again: the take its
	// .recording names fixed (a take that never closed: pulled, power), the lists read again
	{
		int nDest = s.root[0] ? stor::parse_vol (s.root) : stor::kSD;
		bool bBusy = s.state == rec::Recorder::kRecording || s.state == rec::Recorder::kEnded;
		HoldDest ();
		unsigned t = CTimer::GetClockTicks ();
		// a take that ended abandoned (a drive call waited a timeout): repaired here when the drive is still mounted
		// 3 s later (it answered again; a pulled one has left the tree by then and is recovered when it is back)
		if (s.state == rec::Recorder::kIdle && (s_nRecWas == rec::Recorder::kRecording || s_nRecWas == rec::Recorder::kEnded)
		    && s.abandoned && nDest > stor::kSD)
		{
			s_nPendVol = nDest, s_nPendAt = t;
			CLogger::Get ()->Write (From, LogWarning, "%s stopped answering: %s's files are repaired when it is back (or :rec=fix:%s)",
						stor::name (nDest), s.take, stor::name (nDest));
		}
		if (s_nPendVol > 0 && t - s_nPendAt > 3000000 && !bBusy)
		{
			if (s_pBounce && stor::mounted (s_nPendVol) && s_bWasMounted[s_nPendVol]) g_Rec.recover (stor::drive (s_nPendVol));
			s_nPendVol = -1;
		}
		for (int v = stor::kUSB; v < stor::kVols; v++)
		{
			bool m = s_pBounce && stor::mounted (v);
			if (m == s_bWasMounted[v] || (m && bBusy && v == nDest)) continue;
			s_bWasMounted[v] = m;
			if (m) g_Rec.recover (stor::drive (v));
			s_bRescan = true;
		}
		if (s_bRescan && !bBusy)
		{
			s_bRescan = false;
			g_Play.scan ();
			if (s_bUiWant) s_bScanWant = true;
		}
	}
	if (s.state != rec::Recorder::kRecording && s.state != rec::Recorder::kEnded) ScanStep ();
	s_nRecWas = s.state;
	RuiPoll ();
	unsigned now = CTimer::GetClockTicks ();
	if (s.root[0] && (now - s_nSdAt > 10000000 || !s_nSdAt))	// the SD's space (the STATUS page): ours, the recorder's is the drive's
	{
		s_nSdAt = now | 1;
		DWORD nFree;
		FATFS *pFs;
		if (f_getfree ("SD:", &nFree, &pFs) == FR_OK)
		{
			s_nSdFreeMB = (unsigned) (((uint64_t) nFree * pFs->csize * 512) >> 20);
			s_nSdTotalMB = (unsigned) (((uint64_t) (pFs->n_fatent - 2) * pFs->csize * 512) >> 20);
		}
	}
	if (now - s_nLastLogUs < 10000000) return;
	s_nLastLogUs = now;
	g_Rec.status (&s, false);
	if (s.state != rec::Recorder::kRecording) return;
	CLogger::Get ()->Write (From, LogNotice, "%s %u s, %u KB written, %s %u KB/s (need %u), worst write %u ms, ring %u/%u max %u, dropped %u, OUT short %u",
		s.take, (unsigned) (s.frames / 48000), (unsigned) (s.bytes >> 10), s.root[0] ? "drive" : "card", s.write_kbps, s.need_kbps,
		s.worst_write_us / 1000, s.ring_used, s.ring_slots, s.ring_max_used, s.drops, s.side_under);
}

static void Say (void)
{
	HoldDest ();
	rec::Status s;
	g_Rec.status (&s, false);
	char ids[80];
	rec::list_ids (s.mask, ids, sizeof ids);
	static const char *State[] = {"idle", "armed", "recording", "finishing"};
	CLogger::Get ()->Write (From, LogNotice, "%s%s, to %s, %u-bit, name %s, channels %s (%u files, %u KB/s, ring %u ms), "
		"take %s %u s, dropped %u%s%s", State[s.state & 3], s.arm_mode ? " (PLAY/STOP)" : "", VolName (s.root), s.bits, s.prefix[0] ? s.prefix : "the project's",
		ids[0] ? ids : "none", rec::files (s.mask), s.need_kbps, s.ring_ms, s.take[0] ? s.take : "-",
		(unsigned) (s.frames / 48000), s.drops, s.error[0] ? ", error: " : "", s.error);
}

static unsigned Num (const char *q)
{
	unsigned n = 0;
	for (; *q >= '0' && *q <= '9'; q++) n = n * 10 + (*q - '0');
	return n;
}

static void SayPlay (void)
{
	recplay::Status ps;
	g_Play.status (&ps);
	static const char *State[] = {"no take", "stopped", "playing", "stopping"};
	CLogger::Get ()->Write ("play", LogNotice, "%s: take %s (%d/%d), channel %s (%d/%d, %u-bit %s), %u.%01u / %u.%01u s, level %d, "
		"underruns %u%s%s", State[ps.state & 3], ps.take[0] ? ps.take : "-", ps.take_i + 1, ps.takes, ps.channel[0] ? ps.channel : "-",
		ps.ch_i + 1, ps.channels, ps.bits, ps.width == 2 ? "stereo" : "mono", (unsigned) (ps.pos / 48000),
		(unsigned) (ps.pos % 48000 / 4800), (unsigned) (ps.len / 48000), (unsigned) (ps.len % 48000 / 4800), ps.level,
		ps.underruns, ps.error[0] ? ", error: " : "", ps.error);
}

// take's channel NN (-1: the default, MASTER if recorded) from the start: what plays fades out first (10 ms)
static bool PlayTake (const char *pTake, int nNN, char *pErr, unsigned nErr)
{
	g_Play.stop ();
	for (unsigned t0 = CTimer::GetClockTicks (); g_Play.active () && CTimer::GetClockTicks () - t0 < 50000; )
		g_Play.poll ();
	g_Play.poll ();
	g_Play.scan ();
	if (g_Play.active ()) { strncpy (pErr, "busy: try again", nErr - 1); return false; }
	if (!g_Play.select_by_name (pTake, nNN)) { strncpy (pErr, "no such take", nErr - 1); return false; }
	return PlayStart (pErr, nErr);
}

static bool PlayCommand (const char *p)
{
	char err[128] = "";	// (the usage line: 77)
	bool ok = true;
	if (p[4] == '=')
	{
		const char *v = p + 5;
		if (strcmp (v, "start") == 0) ok = PlayStart (err, sizeof err);
		else if (strcmp (v, "stop") == 0) g_Play.stop ();
		else if (strcmp (v, "scan") == 0) g_Play.scan ();
		else if (strncmp (v, "take:", 5) == 0) g_Play.select_take ((int) Num (v + 5));
		else if (strncmp (v, "ch:", 3) == 0) g_Play.select_channel ((int) Num (v + 3));
		else if (strncmp (v, "level:", 6) == 0) g_Play.set_level ((int) Num (v + 6));
		else if (strcmp (v, "pause") == 0) g_Play.pause ();
		else if (strncmp (v, "toggle:", 7) == 0)	// toggle:TAKE - the REC page's tap: play / pause that take, or switch to it
		{
			recplay::Status ps;
			g_Play.status (&ps);
			if (strcmp (ps.take, v + 7) == 0 && g_Play.active () && ps.state == recplay::kPlaying) g_Play.pause ();
			else if (strcmp (ps.take, v + 7) == 0 && ps.paused && !g_Play.active ()) ok = PlayStart (err, sizeof err);
			else ok = PlayTake (v + 7, -1, err, sizeof err);
		}
		else if (strncmp (v, "start:", 6) == 0)	// start:TAKE/NN - the web page's pick
		{
			char take[rec::kTakeMax];
			const char *sl = strchr (v + 6, '/');
			unsigned n = sl ? (unsigned) (sl - (v + 6)) : 0;
			if (!sl || n == 0 || n >= sizeof take) ok = false, SetErr (err, "play=start:TAKE/NN");
			else
			{
				memcpy (take, v + 6, n);
				take[n] = 0;
				ok = PlayTake (take, (int) Num (sl + 1), err, sizeof err);
			}
		}
		else
		{
			ok = false;
			SetErr (err, "play=start|stop|pause|scan|take:N|ch:N|level:0..127|start:TAKE/NN|toggle:TAKE");
		}
	}
	if (!ok)
	{
		CLogger::Get ()->Write ("play", LogWarning, "%s: %s", p, err);
		strncpy (s_UiErr, err, sizeof s_UiErr - 1);	// the REC page shows it a while
		s_nUiErrAt = CTimer::GetClockTicks ();
	}
	SayPlay ();
	return true;
}

bool Command (const char *p)
{
	if (strncmp (p, "play", 4) == 0 && (p[4] == 0 || p[4] == '=')) return PlayCommand (p);
	if (strncmp (p, "rec", 3) != 0 || (p[3] != 0 && p[3] != '=')) return false;
	CLogger *L = CLogger::Get ();
	char err[128] = "";	// (the usage line: 92)
	bool ok = true;
	if (p[3] == '=')
	{
		const char *v = p + 4;
		if ((strcmp (v, "start") == 0 || strcmp (v, "arm") == 0)) g_Play.stop ();	// never both (a pause's position goes too)
		if (strcmp (v, "start") == 0) ok = g_Rec.start (err, sizeof err);
		else if (strcmp (v, "arm") == 0) ok = g_Rec.arm (err, sizeof err);
		else if (strcmp (v, "stop") == 0) g_Rec.stop ();
		else if (strncmp (v, "ch:", 3) == 0)
		{
			uint32_t m = g_Rec.mask ();
			ok = rec::parse_list (v + 3, &m, err, sizeof err) && g_Rec.configure (m, g_Rec.bits (), err, sizeof err);
		}
		else if (strncmp (v, "bits:", 5) == 0)
		{
			unsigned b = 0;
			for (const char *q = v + 5; *q >= '0' && *q <= '9'; q++) b = b * 10 + (*q - '0');
			ok = g_Rec.configure (g_Rec.mask (), b, err, sizeof err);
		}
		else if (strncmp (v, "name:", 5) == 0)	// "name:" alone: the project's name again
		{
			ok = g_Rec.set_prefix (v + 5);
			if (!ok) SetErr (err, "name: 1..24 of A-Z a-z 0-9 _ - (or none: the project's name)");
		}
		else if (strncmp (v, "del:", 4) == 0) ok = DelTake (v + 4, err, sizeof err);
		else if (strncmp (v, "vol:", 4) == 0)	// the destination: the SD, or a drive (refused while armed or recording)
		{
			const char *e;
			int nVol = stor::parse_vol (v + 4, &e);
			if (nVol < 0 || *e) ok = false, SetErr (err, "vol: SD, USB, USB2, USB3 or NVME");
			else if (stor::status (nVol).state == stor::kUnavailable) ok = false, SetErr (err, "vol: not on this board");
			else if (nVol != stor::kSD && !s_pBounce) ok = false, SetErr (err, "vol: no memory for drive I/O (the SD only)");
			else ok = g_Rec.set_root (nVol == stor::kSD ? "" : stor::drive (nVol), err, sizeof err);
		}
		else if (strncmp (v, "fix:", 4) == 0)	// every take of a volume's /Recordings checked (the boot does the SD's; a
		{					// drive gets the marker's take only, when it mounts)
			const char *e;
			int nVol = stor::parse_vol (v + 4, &e);
			rec::Status rs;
			g_Rec.status (&rs, false);
			if (nVol < 0 || *e) ok = false, SetErr (err, "fix: SD, USB, USB2, USB3 or NVME");
			else if (nVol != stor::kSD && !(s_pBounce && stor::mounted (nVol))) ok = false, SetErr (err, "fix: that drive is not mounted");
			else if (rs.state == rec::Recorder::kRecording || rs.state == rec::Recorder::kEnded) ok = false, SetErr (err, "a take is recording: stop first");
			else
			{
				char dir[24];
				strcpy (dir, nVol == stor::kSD ? "" : stor::drive (nVol));
				strcat (dir, "/Recordings");
				unsigned t0 = CTimer::GetClockTicks ();
				int n = g_Rec.fix_all (dir);
				L->Write (From, LogNotice, "fix %s: %d files' sizes fixed (%u ms)", dir, n, (CTimer::GetClockTicks () - t0) / 1000);
			}
		}
		else
		{
			ok = false;
			SetErr (err, "rec=start|stop|arm|ch:LIST|bits:16|24|name:PREFIX|vol:SD|USB..|fix:VOL|del:TAKE|del:USB:TAKE");
		}
	}
	if (!ok)
	{
		L->Write (From, LogWarning, "%s: %s", p, err);
		strncpy (s_UiErr, err, sizeof s_UiErr - 1);	// the REC page shows it a while
		s_nUiErrAt = CTimer::GetClockTicks ();
	}
	Say ();
	return true;
}

void Status (CString *pOut)
{
	rec::Status s;
	g_Rec.status (&s, true);
	bool bMeasured;
	uint32_t nSpeed = DestSpeed (s.root, &bMeasured);	// the warning before a take (the page compares its selection)
	char ids[80];
	rec::list_ids (s.mask, ids, sizeof ids);
	for (char *c = s.error; *c; c++) if (*c == '"' || *c == '\\') *c = '\'';
	CString t;
	t.Format (",\"rec\":{\"state\":%d,\"arm\":%u,\"ready\":%u,\"bits\":%u,\"prefix\":\"%s\",\"take\":\"%s\",\"ch\":[%s],"
		  "\"frames\":%u,\"bytes\":%u,\"drops\":%u,\"silence\":%u,\"outShort\":%u,\"cardKBps\":%u,\"needKBps\":%u,"
		  "\"worstWriteMs\":%u,\"writes\":%u,\"ringMs\":%u,\"ringUsed\":%u,\"ringMax\":%u,\"ringSlots\":%u,"
		  "\"freeMB\":%u,\"totalMB\":%u,\"error\":\"%s\",\"vol\":\"%s\",\"abandoned\":%u,\"freeKnown\":%u,\"volOk\":%u,"
		  "\"speedKBps\":%u,\"speedMeasured\":%u,\"loadPct\":%u,\"warnPct\":%u,\"peak\":[",
		  s.state, s.arm_mode, s.ready, s.bits, s.prefix, s.take, ids, (unsigned) s.frames, (unsigned) (s.bytes >> 10) /* KB */,
		  s.drops, s.dropped_frames, s.side_under, s.write_kbps, s.need_kbps, s.worst_write_us / 1000, s.writes,
		  s.ring_ms, s.ring_used, s.ring_max_used, s.ring_slots, (unsigned) (s.free_bytes >> 20),
		  (unsigned) (s.total_bytes >> 20), s.error, VolName (s.root), s.abandoned ? 1u : 0u, s.free_known ? 1u : 0u, DestOk (s.root) ? 1u : 0u,
		  nSpeed, bMeasured ? 1u : 0u, rec::load_pct (s.need_kbps, nSpeed), rec::kWarnPct);
	pOut->Append (t);
	for (int i = 0; i < rec::kSources; i++)
	{
		t.Format ("%s%u", i ? "," : "", s.peak[i] * 1000 / 8388607);	// per mille of full scale
		pOut->Append (t);
	}
	pOut->Append ("]}");
	recplay::Status ps;
	g_Play.status (&ps);
	for (char *c = ps.error; *c; c++) if (*c == '"' || *c == '\\') *c = '\'';
	t.Format (",\"play\":{\"state\":%d,\"take\":\"%s\",\"ch\":\"%s\",\"takeI\":%d,\"takes\":%d,\"chI\":%d,\"chs\":%d,"
		  "\"posMs\":%u,\"lenMs\":%u,\"level\":%d,\"underruns\":%u,\"error\":\"%s\"}",
		  ps.state, ps.take, ps.channel, ps.take_i, ps.takes, ps.ch_i, ps.channels, (unsigned) (ps.pos / 48),
		  (unsigned) (ps.len / 48), ps.level, ps.underruns, ps.error);
	pOut->Append (t);
}

void Transport (u8 nStatus, u16 nSPP)
{
	if (nStatus == 0xFA || nStatus == 0xFB || nStatus == 0xFC) g_Rec.transport (nStatus);
	bundle_pi::transport (nStatus);	// project bundles: card writes wait while the Rytm plays (bundle_pi.h)
	stor_pi::Transport (nStatus);		// the storage bench is refused while the Rytm plays (stor_pi.h)
	bridge::samp_transport (nStatus);	// the sampler: PLAY / next-bar starts, the tempo (core/samp/sampler.h)
	sendfx_pi::Clock (nStatus, nSPP);
}

bool TakeBusy (const char *pPath)
{
	rec::Status s;
	g_Rec.status (&s, false);
	if ((s.state != rec::Recorder::kRecording && s.state != rec::Recorder::kEnded) || !s.take[0]) return false;
	return fsweb::live_take (pPath, s.take);	// its folder, in it, above it, its volume's .recording
}

void UiWant (bool bOn)
{
	if (bOn && !s_bUiWant) s_bScanWant = true;	// the takes read again on the way in
	s_bUiWant = bOn;
	g_Rec.set_meters (s_bUiWant || s_bScopeWant);
}

void UiFill (sidecar_rec_view_t *v)
{
	rec::Status s;
	g_Rec.status (&s, false);			// (Status::peak stays the web page's)
	memset (v, 0, sizeof *v);
	v->valid = s.ready;
	v->state = s.state == rec::Recorder::kArmed ? SRV_ARMED : s.state == rec::Recorder::kRecording ? SRV_REC
		 : s.state == rec::Recorder::kEnded ? SRV_SAVING : SRV_IDLE;
	v->arm_mode = s.arm_mode;
	v->bits = s.bits, v->rate = recwav::kRate;
	v->secs = (unsigned) (s.frames / recwav::kRate);
	strncpy (v->take, s.take, sizeof v->take - 1);
	if (s_UiErr[0] && CTimer::GetClockTicks () - s_nUiErrAt > 3000000) s_UiErr[0] = 0;
	strncpy (v->error, s.error[0] ? s.error : s_UiErr, sizeof v->error - 1);
	v->free_mb = (unsigned) (s.free_bytes >> 20), v->total_mb = (unsigned) (s.total_bytes >> 20);
	v->need_kbps = s.need_kbps, v->drops = s.drops;
	bool bMeasured;
	v->speed_kbps = DestSpeed (s.root, &bMeasured);
	v->speed_measured = bMeasured;
	v->warn_pct = rec::rate_warning (s.need_kbps, v->speed_kbps) ? rec::load_pct (s.need_kbps, v->speed_kbps) : 0;
	strncpy (v->vol, VolName (s.root), sizeof v->vol - 1);
	v->vol_missing = !DestOk (s.root);
	v->free_unknown = !s.free_known;
	v->mask = s.mask;
	// the names: the Rytm's tracks and outputs; a Sidecar voice by the track(s) that play it (core/ui/remote.cpp
	// kTrackVoice), never its number
	static const char *const kIn[12] = {"MAIN L", "MAIN R", "BD", "SD", "RS/CP", "BT", "LT", "MT/HT", "CH/OH", "CY/CB", "IN L", "IN R"};
	static const char *const kVoice[8] = {"BD", "BT", "SD", "LT", "CH/OH", "MT/HT", "CY/CB", "RS/CP"};
	strcpy (v->name[rec::kMaster], "MASTER");
	for (int i = 0; i < 12; i++) strcpy (v->name[rec::kIn + i], kIn[i]);
	if (rec::fxin ()) strcpy (v->name[rec::kIn + 10], "FX L"), strcpy (v->name[rec::kIn + 11], "FX R");	// the Rytm's FX return
	for (int i = 0; i < 8; i++) strcpy (v->name[rec::kVoice + i], kVoice[i]);
	strcpy (v->name[rec::kFx], "FX 1");
	strcpy (v->name[rec::kFx + 1], "FX 2");
	// the meters: the IN channels, the voices, the FX returns' L / R
	uint32_t pk[rec::kMeters];
	g_Rec.meter_peaks (pk);
	v->peak[rec::kMaster][0] = pk[0], v->peak[rec::kMaster][1] = pk[1];
	for (int i = 0; i < rec::kInChannels; i++) v->peak[rec::kIn + i][0] = pk[i];
	for (int i = 0; i < rec::kVoices; i++) v->peak[rec::kVoice + i][0] = pk[rec::kInChannels + i];
	for (int k = 0; k < 2; k++)
		for (int c = 0; c < 2; c++) v->peak[rec::kFx + k][c] = pk[rec::kInChannels + rec::kVoices + 2 * k + c];
	recplay::Status ps;
	g_Play.status (&ps);
	if (ps.state != recplay::kNone && (g_Play.active () || ps.paused))
	{
		v->pl_state = ps.paused ? SRV_PL_PAUSED : SRV_PL_PLAYING;
		strncpy (v->pl_take, ps.take, sizeof v->pl_take - 1);
		v->pl_pos_ms = (unsigned) (ps.pos * 1000 / recwav::kRate);
		v->pl_len_ms = (unsigned) (ps.len * 1000 / recwav::kRate);
	}
	v->ntakes = s_nTakes;
	memcpy (v->takes, s_Takes, sizeof v->takes);
	v->takes_gen = s_nTakesGen;
}

void ScopeWant (bool bOn)
{
	s_bScopeWant = bOn;
	g_Rec.set_meters (s_bUiWant || s_bScopeWant);
}

void ScopePeaks (uint32_t *pPeaks)
{
	uint32_t pk[rec::kMeters];
	g_Rec.meter_peaks (pk);
	for (int i = 0; i < rec::kInChannels; i++) pPeaks[i] = pk[i];	// the IN channels: MAIN L R, the stems, IN L R
}

void CardSpace (unsigned *pFreeMB, unsigned *pTotalMB)
{
	rec::Status s;
	g_Rec.status (&s, false);
	if (s.root[0]) *pFreeMB = s_nSdFreeMB, *pTotalMB = s_nSdTotalMB;	// the destination is a drive: the SD's own count
	else *pFreeMB = (unsigned) (s.free_bytes >> 20), *pTotalMB = (unsigned) (s.total_bytes >> 20);
}

void BenchDone (int nVol, unsigned nWriteKBps)
{
	if (nVol < 0 || nVol >= stor::kVols || !nWriteKBps) return;
	if (!s_VolNames[0]) SpeedNames ();
	s_Speed[nVol] = nWriteKBps;
	SpeedSave ();
	CLogger::Get ()->Write (From, LogNotice, "%s writes %u KB/s (the bench): a take needing more than %u %% of it is warned about",
				stor::name (nVol), nWriteKBps, rec::kWarnPct);
}

int UiState (unsigned *pSecs)
{
	rec::Status s;
	g_Rec.status (&s, false);
	*pSecs = (unsigned) (s.frames / 48000);
	return s.state == rec::Recorder::kArmed ? 1 : s.state == rec::Recorder::kIdle ? 0 : 2;
}

}  // namespace rec_pi
