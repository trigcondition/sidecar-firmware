// pi/engine/stor_diskio.cpp - FatFs's disk layer for the engine (disk_status, disk_initialize, disk_read, disk_write,
// disk_ioctl), in place of Circle's addon/fatfs/diskio.cpp (Circle 51.1.1, R. Stange; the same device table and the
// same code for every volume but the USB drives). Circle's file is not edited: this object is linked before
// libfatfs.a, so the archive's diskio.o is never pulled in (an archive member is linked only for a symbol still
// undefined; a stray second definition would fail the link, not pass silently).
//
// What differs: on USB:, USB2:, USB3: (umsd1..3) every transfer keeps the xHCI TRB rule (docs/STORAGE.md "The rules
// the volume layer keeps") at this layer, below FatFs, so every FatFs path keeps it - its own sector windows,
// dir_clear's zero-fill (FF_USE_LFN 3: a malloc'd buffer of up to 32 KB, 64-byte aligned, that may cross 64 KB), and
// the multi-sector transfers it makes straight to or from a caller's buffer (up to a cluster: 256 KB on a big exFAT
// drive). stor::split_io (stor_vol.h, host-tested in tests/test_stor_fatfs.cpp): a sector-aligned buffer goes to the
// device in pieces of <= 64 KB never across a 64 KB boundary; any other through one 64 KB-aligned bounce block
// (stor_pi::Init reserves it with the volumes' block, HEAP_DMA30). Core 0 only (the USB volumes' rule; Circle's USB
// transfers do not yield here: NO_BUSY_WAIT is off), so one bounce block serves every drive.
// The SD (emmc1/2), ufd1 and the NVMe SSD (nvme1) keep Circle's code exactly: a word-aligned bounce, one call.
#include <fatfs/ff.h>
#include <fatfs/diskio.h>
#include <circle/device.h>
#include <circle/devicenameservice.h>
#include <circle/util.h>
#include <circle/types.h>
#include <assert.h>
#include "stor_vol.h"
#include "crumb.h"		// the crash breadcrumb: the transfer in flight (2026-10-29)

struct TCrumbDisk
{
	TCrumbDisk (BYTE d, LBA_t s, UINT n, bool w) { crumb::DiskBegin (d, (u32) s, (u32) n, w); }
	~TCrumbDisk () { crumb::DiskEnd (); }
};

#if FF_MIN_SS != FF_MAX_SS
	#error FF_MIN_SS != FF_MAX_SS is not supported!
#endif
#define SECTOR_SIZE		FF_MIN_SS

static const char *s_pVolumeName[FF_VOLUMES] =
{
	"emmc1",
	"emmc2",
	"umsd1",
	"umsd2",
	"umsd3",
	"ufd1",
	"nvme1"
};
static_assert (FF_VOLUMES == 7, "Circle's volume table (ffconf.h FF_VOLUME_STRS)");

static CDevice *s_pVolume[FF_VOLUMES] = {0};

static u8 *s_pBuffer = 0;
static unsigned s_nBufferSize = 0;

static u8 *s_pBounce = 0;		// the USB drives' (stor_diskio_set_bounce): 64 KB, 64 KB-aligned
static unsigned s_nSplit = 0, s_nBounced = 0;	// transfers split / through the bounce (stor_diskio_stats)

static bool IsUsb (BYTE pdrv) { return pdrv >= 2 && pdrv <= 4; }	// umsd1..3 (stor_vol.cpp kFatfs: 2 3 4)

void stor_diskio_set_bounce (u8 *p) { s_pBounce = p; }
void stor_diskio_stats (unsigned *pSplit, unsigned *pBounced) { *pSplit = s_nSplit, *pBounced = s_nBounced; }

struct TUsbIo
{
	CDevice *pDevice;
	boolean bWrite;
	unsigned nPieces;
};

static bool UsbIo (void *ctx, u8 *p, u32 n, u64 nOff)
{
	TUsbIo *t = (TUsbIo *) ctx;
	t->nPieces++;
	if (p == s_pBounce) s_nBounced++;
	t->pDevice->Seek (nOff);
	return (t->bWrite ? t->pDevice->Write (p, n) : t->pDevice->Read (p, n)) >= 0;
}

static DRESULT UsbTransfer (CDevice *pDevice, BYTE *buff, LBA_t sector, UINT count, boolean bWrite)
{
	TUsbIo t = {pDevice, bWrite, 0};
	QWORD offset = sector;
	offset *= SECTOR_SIZE;
	if (!stor::split_io (buff, count * SECTOR_SIZE, offset, bWrite, s_pBounce, UsbIo, &t)) return RES_ERROR;	// (no bounce:
	if (t.nPieces > 1) s_nSplit++;										// an unaligned one fails)
	return RES_OK;
}

static void disk_removed (
	CDevice *pDevice,	/* device removed */
	void *pContext
)
{
	*((CDevice **) pContext) = 0;
}

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber to identify the drive */
)
{
	if (   pdrv < FF_VOLUMES
	    && s_pVolume[pdrv] != 0)
	{
		return 0;
	}

	return STA_NOINIT;
}

DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive nmuber to identify the drive */
)
{
	if (pdrv >= FF_VOLUMES)
	{
		return STA_NOINIT;
	}

	s_pVolume[pdrv] = CDeviceNameService::Get ()->GetDevice (s_pVolumeName[pdrv], TRUE);
	if (s_pVolume[pdrv] != 0)
	{
		s_pVolume[pdrv]->RegisterRemovedHandler (disk_removed, &s_pVolume[pdrv]);

		return 0;
	}

	return STA_NOINIT;
}

DRESULT disk_read (
	BYTE pdrv,		/* Physical drive nmuber to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
	TCrumbDisk Crumb (pdrv, sector, count, false);
	if (pdrv >= FF_VOLUMES)
	{
		return RES_PARERR;
	}

	CDevice *pDevice = s_pVolume[pdrv];
	if (pDevice == 0)
	{
		return RES_NOTRDY;
	}

	if (IsUsb (pdrv))
	{
		return UsbTransfer (pDevice, buff, sector, count, FALSE);
	}

	/* Ensure that the transfer buffer is word aligned */
	BYTE *pBuffer = buff;
	unsigned nSize = count * SECTOR_SIZE;
	if (((uintptr) pBuffer & 3) != 0)
	{
		if (s_nBufferSize < nSize)
		{
			delete [] s_pBuffer;

			s_nBufferSize = nSize;

			s_pBuffer = new u8[s_nBufferSize];
			assert (s_pBuffer != 0);
		}

		pBuffer = s_pBuffer;
	}

	QWORD offset = sector;
	offset *= SECTOR_SIZE;
	pDevice->Seek (offset);

	if (pDevice->Read (pBuffer, nSize) < 0)
	{
		return RES_ERROR;
	}

	if (pBuffer != buff)
	{
		memcpy (buff, pBuffer, nSize);
	}

	return RES_OK;
}

#if FF_FS_READONLY == 0

DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
	TCrumbDisk Crumb (pdrv, sector, count, true);
	if (pdrv >= FF_VOLUMES)
	{
		return RES_PARERR;
	}

	CDevice *pDevice = s_pVolume[pdrv];
	if (pDevice == 0)
	{
		return RES_NOTRDY;
	}

	if (IsUsb (pdrv))
	{
		return UsbTransfer (pDevice, (BYTE *) buff, sector, count, TRUE);	// (split_io only reads it)
	}

	/* Ensure that the transfer buffer is word aligned */
	const BYTE *pBuffer = buff;
	unsigned nSize = count * SECTOR_SIZE;
	if (((uintptr) pBuffer & 3) != 0)
	{
		if (s_nBufferSize < nSize)
		{
			delete [] s_pBuffer;

			s_nBufferSize = nSize;

			s_pBuffer = new u8[s_nBufferSize];
			assert (s_pBuffer != 0);
		}

		memcpy (s_pBuffer, buff, nSize);

		pBuffer = s_pBuffer;
	}

	QWORD offset = sector;
	offset *= SECTOR_SIZE;
	pDevice->Seek (offset);

	if (pDevice->Write (pBuffer, nSize) < 0)
	{
		return RES_ERROR;
	}

	return RES_OK;
}

#endif

DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
	switch (cmd)
	{
	case GET_SECTOR_COUNT:
		{
			if (pdrv >= FF_VOLUMES)
			{
				return RES_PARERR;
			}

			CDevice *pDevice =
				CDeviceNameService::Get ()->GetDevice (s_pVolumeName[pdrv], TRUE);
			if (pDevice != 0)
			{
				u64 ullSize = pDevice->GetSize ();
				if (ullSize == (u64) -1)
				{
					return RES_PARERR;
				}

				*(LBA_t *) buff = (LBA_t) (ullSize / SECTOR_SIZE);
			}
			else
			{
				return RES_NOTRDY;
			}
		}
		return RES_OK;

	case CTRL_SYNC:
		{
			if (pdrv >= FF_VOLUMES)
			{
				return RES_PARERR;
			}

			CDevice *pDevice =
				CDeviceNameService::Get ()->GetDevice (s_pVolumeName[pdrv], TRUE);
			if (pDevice != 0)
			{
				/* This fails, if unsupported, so ignore eventual errors. */
				pDevice->IOCtl (DEVICE_IOCTL_SYNC, 0);
			}
			else
			{
				return RES_NOTRDY;
			}
		}
		return RES_OK;

	case GET_SECTOR_SIZE:
		assert (buff != 0);
		*(WORD *) buff = SECTOR_SIZE;
		return RES_OK;

	case CTRL_EJECT:
		if (pdrv >= FF_VOLUMES)
		{
			return RES_PARERR;
		}

		if (s_pVolume[pdrv] == 0)
		{
			s_pVolume[pdrv] = CDeviceNameService::Get ()->GetDevice (s_pVolumeName[pdrv], TRUE);
			if (s_pVolume[pdrv] == 0)
			{
				return RES_NOTRDY;
			}
		}

		if (!s_pVolume[pdrv]->RemoveDevice ())
		{
			return RES_ERROR;
		}

		s_pVolume[pdrv] = 0;

		return RES_OK;
	}

	return RES_PARERR;
}
