// pi/engine/stor_pi.cpp - see stor_pi.h. Circle's side of stor_vol.cpp (the device tree, FatFs's mounts, the eject)
// and stor_bench.cpp (the files, the raw device, core 0's probes).
#include "stor_pi.h"
#include "bridge.h"
#include "stor_bench.h"
#include "stor_vol.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/timer.h>
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbmassdevice.h>
#include <circle/util.h>
#include <fatfs/diskio.h>
#if RASPPI >= 5
#include <nvme/nvme.h>
#endif

#include "bridge.h"
#include "ob_usb.h"
#include "rec_pi.h"
#include "shared.h"

static const char From[] = "stor";

namespace stor_pi {

namespace {

CUSBHCIDevice *s_pUSB = 0;
// One block from the low heap (DMA-able by every controller here), 64 KB-aligned: the FATFS of each extra volume and
// the bench's two FILs in 4 KB slots (their sector windows never cross 64 KB), a sector buffer for the diagnosis
u8 *s_pBlock = 0;
FATFS *s_pFs[stor::kVols];
FIL *s_pFil[2];
u8 *s_pSector = 0;
u8 *s_pBench = 0;			// the bench's two 1 MB buffers, 64 KB-aligned (made at the first bench)
volatile boolean s_bPlaying = FALSE;
unsigned s_nPollAt = 0, s_nSdAt = 0;
unsigned s_nLoopMax = 0, s_nLoopSum = 0, s_nLoopN = 0;
unsigned s_nHeldBack = 0;		// bench steps held back for the SD's streams (cardq busy)
storbench::State s_LastState = storbench::kIdle;

u8 *Aligned64K (size_t nBytes)
{
	u8 *p = (u8 *) CMemorySystem::HeapAllocate (nBytes + 0x10000, HEAP_DMA30);
	return p ? (u8 *) (((uintptr) p + 0xFFFF) & ~(uintptr) 0xFFFF) : 0;	// (never freed)
}

CDevice *Dev (int v) { return CDeviceNameService::Get ()->GetDevice (stor::device (v), TRUE); }
unsigned Now (void) { return CTimer::Get ()->GetClockTicks (); }

void Log (int nLevel, const char *pLine) { CLogger::Get ()->Write (From, nLevel ? LogWarning : LogNotice, "%s", pLine); }

// ---- stor_vol's platform
bool OpPresent (int v) { return Dev (v) != 0; }

int OpMount (int v, stor::Info *pOut)
{
	if (!s_pFs[v]) return FR_NOT_ENOUGH_CORE;
	unsigned t = Now ();
	memset (s_pFs[v], 0, sizeof (FATFS));
	FRESULT r = f_mount (s_pFs[v], stor::drive (v), 1);
	if (r != FR_OK)
	{
		f_mount (0, stor::drive (v), 0);
		return r;
	}
	FATFS *pFs = s_pFs[v];
	pOut->fs = pFs->fs_type <= 4 ? pFs->fs_type : stor::kFsNone;
	pOut->total = (u64) (pFs->n_fatent - 2) * pFs->csize * FF_MAX_SS;
	// the free count: FSINFO's when valid (FAT32 as Windows / macOS leave it), exFAT's bitmap (1 bit a cluster: small);
	// a FAT32 without FSINFO would need its whole FAT read (seconds on a big stick, core 0 stopped): counted only when
	// asked (stor=free:VOL, and a bench's start)
	DWORD nFree = 0;
	FATFS *p;
	boolean bCheap = pFs->free_clst <= pFs->n_fatent - 2;
#if FF_FS_EXFAT
	bCheap = bCheap || pFs->fs_type == FS_EXFAT;
#endif
	pOut->free_known = bCheap && f_getfree (stor::drive (v), &nFree, &p) == FR_OK;
	if (pOut->free_known) pOut->free = (u64) nFree * pFs->csize * FF_MAX_SS;
	pOut->mount_us = Now () - t;
	return 0;
}

void OpUnmount (int v) { f_mount (0, stor::drive (v), 0); }

bool OpEject (int v)	// FatFs's CTRL_EJECT: Circle's RemoveDevice (USB: the device leaves the tree; NVMe: not supported)
{
	return disk_ioctl ((BYTE) stor::fatfs_index (v), CTRL_EJECT, 0) == RES_OK;
}

void OpDescribe (int v, stor::Status *pSt)
{
	CDevice *d = Dev (v);
	if (!d) return;
	if (v == stor::kNVME)
	{
		pSt->link = stor::kPcie, pSt->gen = 2;	// Circle's PCIE_GEN (lib/bcmpciehostbridge.cpp): the link's maximum
		const char *m = d->GetProperty (CDevice::PropertyProduct);
		int n = 0;
		for (; m && m[n] && n < (int) sizeof pSt->model - 1; n++) pSt->model[n] = m[n];
		while (n > 0 && pSt->model[n - 1] == ' ') n--;	// (NVMe pads its model with spaces)
		pSt->model[n] = 0;
		return;
	}
	CUSBDevice *u = static_cast<CUSBBulkOnlyMassStorageDevice *> (d)->GetDevice ();	// umsdN is always one
	if (!u) return;
	switch (u->GetSpeed ())
	{
	case USBSpeedLow:	pSt->link = stor::kUsbLow; break;
	case USBSpeedFull:	pSt->link = stor::kUsbFull; break;
	case USBSpeedHigh:	pSt->link = stor::kUsbHigh; break;
	case USBSpeedSuper:	pSt->link = stor::kUsbSuper; break;
	default:		pSt->link = stor::kLinkNone; break;
	}
	const TUSBDeviceDescriptor *pDesc = u->GetDeviceDescriptor ();
	if (pDesc)
	{
		CString s;
		s.Format ("%04x:%04x", (unsigned) pDesc->idVendor, (unsigned) pDesc->idProduct);
		strncpy (pSt->model, (const char *) s, sizeof pSt->model - 1);
	}
}

int RawRead (CDevice *d, u64 nOff, void *p, unsigned n)
{
	if (d->Seek (nOff) != nOff) return -1;
	return d->Read (p, n);
}

void OpDiagnose (int v, char *pWhy, int nCap)	// a device that did not mount (only core 0 has it: no lock needed)
{
	CDevice *d = Dev (v);
	pWhy[0] = 0;
	if (!d || !s_pSector) return;
	if (RawRead (d, 0, s_pSector, 512) != 512) { strncpy (pWhy, "sector 0 unreadable", nCap - 1); return; }
	u32 nNext = 0;
	const char *w = stor::classify_sector0 (s_pSector, 0, &nNext);
	if (nNext && RawRead (d, (u64) nNext * 512, s_pSector + 512, 512) == 512) w = stor::classify_sector0 (s_pSector, s_pSector + 512, 0);
	strncpy (pWhy, w, nCap - 1);
	pWhy[nCap - 1] = 0;
}

const stor::Ops kOps = {OpPresent, OpMount, OpUnmount, OpEject, OpDescribe, OpDiagnose, Log};

// ---- stor_bench's platform
const char *BenchPath (int nVol, int f)
{
	static char Path[2][32];
	CString s;
	s.Format ("%s%s", stor::drive (nVol), nVol == stor::kSD ? (f ? "/.rytm/storbench2.tmp" : "/.rytm/storbench.tmp")
								 : (f ? "/storbench2.tmp" : "/storbench.tmp"));
	strncpy (Path[f], (const char *) s, sizeof Path[f] - 1);
	return Path[f];
}
bool BOpen (int nVol, int f, bool bWrite)
{
	if (nVol == stor::kSD) f_mkdir ("SD:/.rytm");
	return f_open (s_pFil[f], BenchPath (nVol, f), bWrite ? FA_WRITE | FA_CREATE_ALWAYS : FA_READ) == FR_OK;
}
int BWrite (int f, const void *p, u32 n)
{
	UINT w = 0;
	return f_write (s_pFil[f], p, n, &w) == FR_OK ? (int) w : -1;
}
int BReadAt (int f, u64 nOff, void *p, u32 n)
{
	if (f_tell (s_pFil[f]) != nOff && f_lseek (s_pFil[f], nOff) != FR_OK) return -1;
	UINT r = 0;
	return f_read (s_pFil[f], p, n, &r) == FR_OK ? (int) r : -1;
}
bool BSync (int f) { return f_sync (s_pFil[f]) == FR_OK; }
void BClose (int f) { f_close (s_pFil[f]); }
void BRemove (int nVol, int f) { f_unlink (BenchPath (nVol, f)); }
// the raw device, under FatFs's lock of its volume (core 3 reads the SD through FatFs meanwhile)
int BRaw (int nVol, u64 nOff, void *p, u32 n)
{
	CDevice *d = Dev (nVol);
	if (!d || !stor::mounted (nVol)) return -1;
	int k = stor::fatfs_index (nVol);
	ff_mutex_take (k);
	int r = RawRead (d, nOff, p, n);
	ff_mutex_give (k);
	return r;
}
u64 BRawSize (int nVol)
{
	CDevice *d = Dev (nVol);
	u64 n = d ? d->GetSize () : 0;
	return n == (u64) -1 ? 0 : n;
}
u32 BNow (void) { return Now (); }
bool BStreaming (void) { return CObUsb::Streaming (); }
u32 BUnder (void) { return g_Output.Underflows (); }
void BLoopReset (void) { s_nLoopMax = s_nLoopSum = s_nLoopN = 0; }
void BLoopRead (u32 *pMax, u32 *pAvg) { *pMax = s_nLoopMax, *pAvg = s_nLoopN ? s_nLoopSum / s_nLoopN : 0; }
bool BHold (void) { return storbench::vol () == stor::kSD && bridge::card_busy (); }	// the SD's streams first
void BLog (const char *pLine)
{
	CLogger::Get ()->Write (From, LogNotice, "%s", pLine);
	CTimer::Get ()->MsDelay (2);	// ~70 B a line at 921600 baud: Circle's TX buffer drops when full (as LogBench)
}
const storbench::Io kBenchIo = {BOpen, BWrite, BReadAt, BSync, BClose, BRemove, BRaw, BRawSize, BNow, BStreaming, BUnder,
				BLoopReset, BLoopRead, BLog, BHold};

// the finished bench's table on the card: SD:/.rytm/storbench-<VOL>.txt (the console's stor=last prints it again)
void SaveResult (void)
{
	static char t[1600];
	int n = storbench::table (storbench::results (), t, sizeof t);
	if (n >= (int) sizeof t) n = sizeof t - 1;
	CString Path;
	Path.Format ("SD:/.rytm/storbench-%s.txt", stor::name (storbench::vol ()));
	FIL *f = s_pFil[0];		// (the bench has closed both)
	f_mkdir ("SD:/.rytm");
	if (f_open (f, (const char *) Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
	UINT w;
	f_write (f, t, (UINT) n, &w);
	f_close (f);
}

void CountFree (int v)	// f_getfree now (core 0 waits for it: a whole FAT without FSINFO), logged with its time
{
	unsigned t = Now ();
	DWORD n;
	FATFS *p;
	if (!stor::mounted (v) || f_getfree (stor::drive (v), &n, &p) != FR_OK) return;
	stor::set_free (v, (u64) n * p->csize * FF_MAX_SS);
	CLogger::Get ()->Write (From, LogNotice, "%s: free space counted in %u ms", stor::name (v), (Now () - t) / 1000);
}

const char *LinkOf (int v)
{
	const stor::Status &st = stor::status (v);
	return stor::link_name (st.link, st.gen);
}

void ListVolumes (void)
{
	for (int v = 0; v < stor::kVols; v++)
	{
		char b[200];
		stor::line (v, b, sizeof b);
		CLogger::Get ()->Write (From, LogNotice, "%s", b);
	}
	CLogger::Get ()->Write (From, LogNotice, "exFAT %s in this build; bench: %s%s", FF_FS_EXFAT ? "on" : "off",
				storbench::phase ()[0] ? storbench::phase () : "not run", storbench::state () == storbench::kRunning ? "" : " (stor=bench[:VOL])");
	unsigned nSplit, nBounced;
	stor_diskio_stats (&nSplit, &nBounced);
	CLogger::Get ()->Write (From, LogNotice, "USB disk layer (64 KB rule): %u transfers split, %u pieces through the bounce",
				nSplit, nBounced);
}

}	// namespace

void Init (CUSBHCIDevice *pUSB, CInterruptSystem *pInterrupt, boolean bNVMe)
{
	s_pUSB = pUSB;
	stor::reset ();
	stor::set_ops (&kOps);
	static_assert (sizeof (FATFS) <= 4096 && sizeof (FIL) <= 4096, "a 4 KB slot each");
	s_pBlock = Aligned64K (0x20000);	// the slots; at +64 KB the disk layer's bounce (stor_diskio.cpp)
	if (s_pBlock)
	{
		stor_diskio_set_bounce (s_pBlock + 0x10000);
		for (int v = 0; v < stor::kVols; v++) s_pFs[v] = v == stor::kSD ? 0 : (FATFS *) (s_pBlock + v * 0x1000);
		s_pFil[0] = (FIL *) (s_pBlock + 0x8000), s_pFil[1] = (FIL *) (s_pBlock + 0x9000);
		s_pSector = s_pBlock + 0xA000;
	}
	else CLogger::Get ()->Write (From, LogError, "no memory for the volumes (128 KB): USB drives are not mounted");
	storbench::set_io (&kBenchIo);
#if RASPPI >= 5
	if (!bNVMe)
	{
		stor::set_available (stor::kNVME, false);
		CLogger::Get ()->Write (From, LogNotice, "NVME: off (nvme=0)");
	}
	else
	{
		unsigned t = Now ();
		CLogger::Get ()->Write (From, LogNotice, "NVME: looking for an SSD on the PCIe connector (Circle's addon/nvme, experimental)...");
		CNVMeDevice *pNVMe = new CNVMeDevice (pInterrupt);
		boolean bOK = pNVMe != 0 && pNVMe->Initialize ();	// (a failed one is left alone: no destructor)
		t = Now () - t;
		if (bOK) CLogger::Get ()->Write (From, LogNotice, "NVME: an SSD answered in %u ms (mounted at the first pass)", t / 1000);
		else
		{
			CLogger::Get ()->Write (From, LogNotice, "NVME: none (%u ms; the nvme lines above say why)", t / 1000);
			stor::set_why (stor::kNVME, "no SSD answered at boot");
		}
	}
#else
	(void) pInterrupt, (void) bNVMe;
	stor::set_available (stor::kNVME, false);	// the Pi 4 has no PCIe connector
#endif
	s_nPollAt = Now ();				// (the first mounts: core 0's loop, not the boot)
}

void Poll (boolean bPlugAndPlay)
{
	unsigned t = Now ();
	if (bPlugAndPlay || t - s_nPollAt >= 1000000)
	{
		s_nPollAt = t;
		unsigned nFree, nTotal;
		rec_pi::CardSpace (&nFree, &nTotal);		// the card as the recorder reads it (every 10 s)
		if (nTotal) stor::set_sd ((u64) nTotal << 20, (u64) nFree << 20);
		else if (t - s_nSdAt >= 10000000 || !s_nSdAt)	// no recorder (recbuf=0): our own count, as often
		{
			s_nSdAt = t | 1;
			DWORD n;
			FATFS *p;
			if (f_getfree ("SD:", &n, &p) == FR_OK)
				stor::set_sd ((u64) (p->n_fatent - 2) * p->csize * FF_MAX_SS, (u64) n * p->csize * FF_MAX_SS);
			else stor::set_sd (0, 0);
		}
		stor::poll ();
	}
	if (storbench::state () == storbench::kRunning)
	{
		// the existing priority: the SD's streams before anything else on that card (core/msamp/cardq.h)
		if (storbench::vol () == stor::kSD && bridge::card_busy ()) s_nHeldBack++;
		else storbench::step ();
	}
	storbench::State st = storbench::state ();
	if (st != s_LastState)
	{
		s_LastState = st;
		if (st == storbench::kDone)
		{
			SaveResult ();
			int v = storbench::vol ();
			{	// the streaming budget of that volume (core/pool/stream_budget.h): its 64 KB reads' rate and p99
				const storbench::Results &r = storbench::results ();
				bridge::stream_bench (v, storbench::mbps_x100 (r.seq[0].rbytes, r.seq[0].rus), r.rnd64.p99);
				// the recorder's warning before a take: the 64 KB writes' rate (its own write size), in KB/s
				rec_pi::BenchDone (v, (unsigned) ((uint64_t) storbench::mbps_x100 (r.seq[0].wbytes, r.seq[0].wus) * 1024 / 100));
			}
			if (v != stor::kSD) CountFree (v);	// after the test file went (known now: FatFs keeps it from here)
			if (s_nHeldBack) CLogger::Get ()->Write (From, LogNotice, "bench: %u steps waited for the SD's streams", s_nHeldBack);
		}
	}
}

boolean Command (const char *p)
{
	if (strncmp (p, "stor", 4) != 0 || (p[4] != 0 && p[4] != '=')) return FALSE;
	CLogger *L = CLogger::Get ();
	const char *a = p[4] ? p + 5 : "";
	if (!*a || !strcmp (a, "list"))
	{
		ListVolumes ();
		return TRUE;
	}
	if (!strncmp (a, "bench", 5) && (a[5] == 0 || a[5] == ':' || a[5] == '!'))
	{
		storbench::Args Args;
		const char *pErr = 0;
		if (!storbench::parse (a + 5, &Args, &pErr))
		{
			L->Write (From, LogWarning, "bench: %s", pErr);
			return TRUE;
		}
		storbench::Cond c;
		unsigned nSecs;
		c.mounted = stor::mounted (Args.vol);
		c.playing = s_bPlaying || g_Play.active ();
		c.recording = rec_pi::UiState (&nSecs) != 0;	// armed too: a take may start any moment
		c.running = storbench::state () == storbench::kRunning;
		c.sdtest = bridge::sdtest_running ();
		const char *pWhy = s_pBlock ? storbench::refuse (Args, c) : "no memory for the volumes";
		if (!pWhy && !stor::status (Args.vol).free_known) CountFree (Args.vol);
		if (!pWhy && !s_pBench && (s_pBench = Aligned64K (2 * storbench::kBufBytes)) != 0)
			storbench::set_buffers (s_pBench, s_pBench + storbench::kBufBytes);
		if (!pWhy)
		{
			const stor::Status &vs = stor::status (Args.vol);
			// calls of <= 64 KB everywhere: USB the TRB rule; NVMe: Circle's driver does not split at the drive's MDTS (its
			// largest transfer), so a 1 MB command may be refused; SD: a 1 MB f_write would hold the SD's FatFs lock for
			// 50+ ms, long enough for core 3 to take a stream's read back (cardq kTakeBackUs) and spin on that lock.
			// The 256 KB / 1 MB rows: more 64 KB calls a pass
			s_nHeldBack = 0;
			pWhy = storbench::start (Args, true, vs.free, LinkOf (Args.vol),
						 stor::fs_name (vs.fs));
		}
		if (pWhy) L->Write (From, LogWarning, "bench refused: %s", pWhy);
		else L->Write (From, LogNotice, "bench %s started: %u MB, %d reads a phase (stor=cancel stops it; the table follows here)",
			       stor::name (Args.vol), (unsigned) (storbench::results ().size >> 20), Args.n);
		return TRUE;
	}
	if (!strcmp (a, "cancel"))
	{
		boolean bRun = storbench::state () == storbench::kRunning;
		storbench::cancel ();
		L->Write (From, LogNotice, "bench: %s", bRun ? "cancelled, its files removed" : "not running");
		return TRUE;
	}
	if (!strcmp (a, "last"))
	{
		if (!storbench::have ()) L->Write (From, LogNotice, "bench: no result yet (stor=bench[:VOL])");
		else
		{
			static char t[1600];
			storbench::table (storbench::results (), t, sizeof t);
			for (char *l = t; *l;)
			{
				char *e = l;
				while (*e && *e != '\n') e++;
				char c = *e;
				*e = 0;
				BLog (l);
				if (!c) break;
				l = e + 1;
			}
		}
		return TRUE;
	}
	if (!strncmp (a, "eject:", 6) || !strncmp (a, "mount:", 6))
	{
		const char *e;
		int v = stor::parse_vol (a + 6, &e);
		boolean bForce = *e == '!';
		if (v < 0 || (*e && !(bForce && !e[1])))
		{
			L->Write (From, LogWarning, "stor=%s:SD|USB|USB2|USB3|NVME", a[0] == 'm' ? "mount" : "eject");
			return TRUE;
		}
		const char *pWhy;
		if (a[0] == 'm') pWhy = stor::mount_again (v);
		else
		{
			if (bForce && stor::status (v).busy && storbench::vol () == v) storbench::cancel ();	// '!': the bench goes first
			pWhy = stor::eject (v);
		}
		if (pWhy) L->Write (From, LogWarning, "%s %s: %s", a[0] == 'm' ? "mount" : "eject", stor::name (v), pWhy);
		else if (a[0] == 'm') L->Write (From, LogNotice, "mount %s: at the next pass", stor::name (v));
		return TRUE;
	}
	if (!strncmp (a, "free:", 5))
	{
		int v = stor::parse_vol (a + 5, 0);
		if (v <= stor::kSD || !stor::mounted (v)) L->Write (From, LogWarning, "stor=free:USB|USB2|USB3|NVME (a mounted one)");
		else CountFree (v);
		return TRUE;
	}
	if (!strcmp (a, "rescan"))
	{
#if RASPPI == 4
		if (s_pUSB) s_pUSB->ReScanDevices ();	// an ejected stick still plugged comes back (the next UpdatePlugAndPlay)
		L->Write (From, LogNotice, "USB: rescanned");
#else
		L->Write (From, LogNotice, "USB: rescan is Pi 4 only here (unplug and plug the drive)");
#endif
		return TRUE;
	}
	L->Write (From, LogWarning, "stor | stor=bench[:VOL][:MB][:n=N][!] | stor=cancel | stor=last | stor=eject:VOL[!] | stor=mount:VOL | stor=free:VOL | stor=rescan");
	return TRUE;
}

void Status (CString *pOut)
{
	static char b[4096];
	pOut->Append (",\"stor\":{");
	stor::json_vols (b, sizeof b, FF_FS_EXFAT != 0);
	pOut->Append (b);
	pOut->Append (",");
	storbench::json (b, sizeof b);
	pOut->Append (b);
	pOut->Append ("}");
}

void UiFill (sidecar_status_view_t *v)
{
	v->usb_state = 0, v->usb_more = 0, v->usb_fs[0] = v->usb_link[0] = 0, v->usb_free_mb = v->usb_total_mb = 0;
	int shown = -1;
	for (int k = stor::kUSB; k <= stor::kUSB3; k++)
	{
		const stor::Status &st = stor::status (k);
		if (st.state == stor::kAbsent) continue;
		if (shown < 0 || (st.state == stor::kMounted && stor::status (shown).state != stor::kMounted)) shown = k;
	}
	for (int k = stor::kUSB; k <= stor::kUSB3; k++) if (k != shown && stor::mounted (k)) v->usb_more++;
	if (shown >= 0)
	{
		const stor::Status &st = stor::status (shown);
		v->usb_state = st.state;
		strncpy (v->usb_fs, stor::fs_name (st.fs), sizeof v->usb_fs - 1);
		strncpy (v->usb_link, st.link == stor::kUsbSuper ? "USB 3" : st.link == stor::kUsbHigh ? "USB 2" : st.link ? "USB 1" : "",
			 sizeof v->usb_link - 1);
		v->usb_free_mb = st.free_known ? (unsigned) (st.free >> 20) : ~0u, v->usb_total_mb = (unsigned) (st.total >> 20);
	}
	const stor::Status &n = stor::status (stor::kNVME);
	v->nvme_state = n.state;
	v->nvme_fs[0] = 0;
	strncpy (v->nvme_fs, stor::fs_name (n.fs), sizeof v->nvme_fs - 1);
	v->nvme_free_mb = n.free_known ? (unsigned) (n.free >> 20) : ~0u, v->nvme_total_mb = (unsigned) (n.total >> 20);
}

void NoteLoop (unsigned nUs)
{
	s_nLoopN++;
	s_nLoopSum += nUs;
	if (nUs > s_nLoopMax) s_nLoopMax = nUs;
}

void Transport (u8 nStatus)
{
	if (nStatus == 0xFA || nStatus == 0xFB) s_bPlaying = TRUE;
	else if (nStatus == 0xFC) s_bPlaying = FALSE;
}

boolean Mounted (int nVol) { return stor::mounted (nVol); }
const char *Drive (int nVol) { return stor::drive (nVol); }

int Read (FIL *pFile, void *pBuffer, unsigned nBytes)
{
	u8 *b = (u8 *) pBuffer;
	unsigned d = 0;
	while (d < nBytes)
	{
		UINT k = stor::piece ((uintptr) (b + d), nBytes - d), r = 0;
		if (f_read (pFile, b + d, k, &r) != FR_OK) return d ? (int) d : -1;
		d += r;
		if (r < k) break;	// the file's end
	}
	return (int) d;
}

int Write (FIL *pFile, const void *pBuffer, unsigned nBytes)
{
	const u8 *b = (const u8 *) pBuffer;
	unsigned d = 0;
	while (d < nBytes)
	{
		UINT k = stor::piece ((uintptr) (b + d), nBytes - d), w = 0;
		if (f_write (pFile, b + d, k, &w) != FR_OK) return d ? (int) d : -1;
		d += w;
		if (w < k) break;	// the volume is full
	}
	return (int) d;
}

}	// namespace stor_pi
