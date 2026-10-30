// pi/engine/stor_pi.h - the volumes besides the boot card in the Pi kernel (docs/STORAGE.md): USB mass storage
// (USB:, USB2:, USB3: - Circle's umsd1..3 and FatFs) mounted and unmounted as sticks come and go (core 0's loop, after
// UpdatePlugAndPlay), a safe eject; on the Pi 5 an NVMe SSD (NVME:, Circle's addon/nvme, started at boot when one
// answers: nvme=0 in cmdline.txt skips it); the bench (stor_bench.h); /status's "stor"; the STATUS page's rows.
// The logic is stor_vol.cpp / stor_bench.cpp (portable, tested on the Mac); this is Circle's side of them.
//
// Everything here is core 0's. Core 3 (the audio) never touches these volumes. A later user (the bundles' library on
// a stick) reads them from core 0 with Read / Write below, pieces of <= 64 KB inside one 64 KB window (the xHCI TRB
// rule) from a cache-aligned buffer (an unaligned one makes Circle bounce each call through a buffer of its own).
#ifndef _stor_pi_h
#define _stor_pi_h

#include <circle/interrupt.h>
#include <circle/string.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <fatfs/ff.h>
#include "../ui/sidecar_status.h"

// stor_diskio.cpp (FatFs's disk layer): the USB drives' bounce block (64 KB, 64 KB-aligned; set by Init), and how
// many transfers it split / pieces it bounced since boot (the `stor` listing)
void stor_diskio_set_bounce (u8 *p);
void stor_diskio_stats (unsigned *pSplit, unsigned *pBounced);

namespace stor_pi {
// CKernel::Initialize, after the USB host started: the volumes' memory; the Pi 5's NVMe SSD (bNVMe: cmdline nvme=,
// default 1) - a PCIe link that does not come up costs ~100 ms, a drive that does not answer its controller's own
// timeout (CAP.TO, reported by the drive: seconds at worst), logged either way
void Init (CUSBHCIDevice *pUSB, CInterruptSystem *pInterrupt, boolean bNVMe);
void Poll (boolean bPlugAndPlay);	// core 0's loop: hot-plug (on a change, and once a second), one bench step
// stor; stor=bench[:VOL][:MB][:n=N][!]; stor=cancel; stor=eject:VOL[!]; stor=mount:VOL; stor=free:VOL; stor=rescan (Pi 4);
// stor=last
boolean Command (const char *pCmd);
void Status (CString *pOut);		// ,"stor":{"exfat":..,"vols":[...],"bench":{...}}
void UiFill (sidecar_status_view_t *pView);	// the STATUS page's USB / NVME rows
void NoteLoop (unsigned nUs);		// core 0's loop: one MsSleep (1)'s wake time (the bench's busy phases)
void Transport (u8 nStatus);		// the Rytm's realtime messages (core 3): playing or not (the bench's refusal)
boolean Mounted (int nVol);		// stor::Vol
const char *Drive (int nVol);		// "USB:"
// core 0: f_read / f_write in pieces (stor::piece); the bytes done (fewer: the file's end or an error), -1 none
int Read (FIL *pFile, void *pBuffer, unsigned nBytes);
int Write (FIL *pFile, const void *pBuffer, unsigned nBytes);
}

#endif
