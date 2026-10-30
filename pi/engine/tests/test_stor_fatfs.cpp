// pi/engine/tests/test_stor_fatfs.cpp - Circle's FatFs (R0.16, addon/fatfs) on the Mac over file images, built twice
// (Makefile): with the engine's ffconf.h as it is (FAT12/16/32 on MBR only) and with the STOR_EXFAT switch's two changes
// (FF_FS_EXFAT 1, FF_LBA64 1: exFAT and GPT), the host's own changes beside them (no lock, f_mkfs, no clock, LFN on
// the stack). For each format FatFs can make: mkfs, mount, the volume's type and free count, files written and read
// back through the 64 KB piece rule (stor_vol.h); the disk layer as the Pi's (stor_diskio.cpp: stor::split_io), so every
// transfer the device gets is inside one 64 KB window and <= 128 sectors - also straight f_read / f_write from buffers
// across 64 KB and, built a third time with the Pi's FF_USE_LFN 3, dir_clear's buffer put across 64 KB on purpose; a file over 4 GB on exFAT (sparse, FF_LBA64 build), unmount and mount again. With a folder argument, the
// macOS-made images in it too (make fatfs-mac: hdiutil, MBR / GPT x FAT32 / exFAT): which mount here, and what
// stor::classify_sector0 says about the others.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "check.h"
#include "ff.h"
#include "diskio.h"
#include "stor_vol.h"

namespace {
FILE *g_Img[FF_VOLUMES];
uint64_t g_Sectors[FF_VOLUMES];
unsigned g_MaxCount, g_Cross, g_Calls, g_MaxReq, g_Odd, g_Bounced;
uint8_t *g_Bounce;			// stor_diskio.cpp's: 64 KB, 64 KB-aligned

// what the device gets (the umsd driver: one TRB a transfer): counted after the disk layer's split
void note (const void *b, UINT n)
{
	g_Calls++;
	if (n > g_MaxCount) g_MaxCount = n;
	uintptr_t a = (uintptr_t) b, e = a + (uintptr_t) n * 512 - 1;
	if ((a >> 16) != (e >> 16)) g_Cross++;
	if (b == g_Bounce) g_Bounced++;
}
struct Dev { int pdrv; bool write; };
bool dev_io (void *ctx, uint8_t *p, uint32_t n, uint64_t off)
{
	Dev *d = (Dev *) ctx;
	if (n % 512) { g_Odd++; return false; }
	note (p, n / 512);
	if (fseeko (g_Img[d->pdrv], (off_t) off, SEEK_SET)) return false;
	if (d->write) return fwrite (p, 512, n / 512, g_Img[d->pdrv]) == n / 512;
	size_t got = fread (p, 512, n / 512, g_Img[d->pdrv]);
	if (got < n / 512) memset (p + got * 512, 0, (n / 512 - got) * 512);	// (a sparse tail)
	return true;
}
}	// namespace

extern "C" {
DSTATUS disk_status (BYTE pdrv) { return pdrv < FF_VOLUMES && g_Img[pdrv] ? 0 : STA_NOINIT; }
DSTATUS disk_initialize (BYTE pdrv) { return disk_status (pdrv); }
// as pi/engine/stor_diskio.cpp for a USB volume: every transfer through stor::split_io
DRESULT disk_read (BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (disk_status (pdrv)) return RES_NOTRDY;
	if (count > g_MaxReq) g_MaxReq = count;
	Dev d = {pdrv, false};
	return stor::split_io (buff, count * 512, (uint64_t) sector * 512, false, g_Bounce, dev_io, &d) ? RES_OK : RES_ERROR;
}
DRESULT disk_write (BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	if (disk_status (pdrv)) return RES_NOTRDY;
	if (count > g_MaxReq) g_MaxReq = count;
	Dev d = {pdrv, true};
	return stor::split_io ((uint8_t *) buff, count * 512, (uint64_t) sector * 512, true, g_Bounce, dev_io, &d) ? RES_OK : RES_ERROR;
}
#if FF_USE_LFN == 3
// Circle's ff_memalloc is malloc: 64-byte aligned, anywhere. Here every buffer over 4 KB (dir_clear's zero-fill, up to
// 32 KB) is put across a 64 KB boundary on purpose, sector-aligned and not in turn, so both of split_io's paths run
unsigned g_BigAllocs;
static uint8_t *g_Arena;
void *ff_memalloc (UINT n)
{
	if (!g_Arena) g_Arena = (uint8_t *) aligned_alloc (65536, 4 * 65536);
	if (n <= 4096) return malloc (n);
	CHECK (n <= 0x8000);
	return g_Arena + 0x10000 - 0x4000 + (g_BigAllocs++ & 1 ? 0x40 : 0);	// crosses 64 KB at +0x4000
}
void ff_memfree (void *p)
{
	if (g_Arena && (uint8_t *) p >= g_Arena && (uint8_t *) p < g_Arena + 4 * 65536) return;	// (the arena's: kept)
	free (p);
}
#endif

DRESULT disk_ioctl (BYTE pdrv, BYTE cmd, void *buff)
{
	if (disk_status (pdrv)) return RES_NOTRDY;
	switch (cmd)
	{
	case CTRL_SYNC: fflush (g_Img[pdrv]); return RES_OK;
	case GET_SECTOR_COUNT: *(LBA_t *) buff = (LBA_t) g_Sectors[pdrv]; return RES_OK;
	case GET_SECTOR_SIZE: *(WORD *) buff = 512; return RES_OK;
	case GET_BLOCK_SIZE: *(DWORD *) buff = 1; return RES_OK;
	default: return RES_PARERR;
	}
}
}

namespace {
const int kUsb = 2;			// FatFs's "USB" volume (pdrv 2, umsd1 on the Pi)
const char *kDrive = "USB:";

bool attach (const char *path, uint64_t nBytes, bool bCreate)
{
	if (g_Img[kUsb]) fclose (g_Img[kUsb]);
	g_Img[kUsb] = fopen (path, bCreate ? "w+b" : "r+b");
	if (!g_Img[kUsb]) return false;
	if (bCreate && ftruncate (fileno (g_Img[kUsb]), (off_t) nBytes)) return false;
	struct stat st;
	fstat (fileno (g_Img[kUsb]), &st);
	g_Sectors[kUsb] = (uint64_t) st.st_size / 512;
	return true;
}
void detach (void)
{
	if (g_Img[kUsb]) fclose (g_Img[kUsb]);
	g_Img[kUsb] = 0;
}

// the volume layer's I/O rule: pieces of <= 64 KB never across a 64 KB boundary of the buffer
bool put (FIL *f, const uint8_t *b, uint32_t n)
{
	for (uint32_t d = 0; d < n;)
	{
		UINT k = stor::piece ((uintptr_t) (b + d), n - d), w = 0;
		if (f_write (f, b + d, k, &w) != FR_OK || w != k) return false;
		d += k;
	}
	return true;
}
bool get (FIL *f, uint8_t *b, uint32_t n)
{
	for (uint32_t d = 0; d < n;)
	{
		UINT k = stor::piece ((uintptr_t) (b + d), n - d), r = 0;
		if (f_read (f, b + d, k, &r) != FR_OK || r != k) return false;
		d += k;
	}
	return true;
}

FATFS *g_Fs;
FIL *g_F;
uint8_t *g_W, *g_R;
const uint32_t kBuf = 1u << 20;

const char *fs_name (BYTE t)
{
	switch (t)
	{
	case FS_FAT12: return "FAT12";
	case FS_FAT16: return "FAT16";
	case FS_FAT32: return "FAT32";
#if FF_FS_EXFAT
	case FS_EXFAT: return "exFAT";
#endif
	default: return "?";
	}
}

// mounted: files through the piece rule, the free count, a re-mount; the image keeps a file "keep.bin" checked after
void exercise (const char *what, uint64_t nFileBytes)
{
	g_MaxCount = g_Cross = g_Calls = 0;
	DWORD nfree;
	FATFS *pfs;
	CHECK (f_getfree (kDrive, &nfree, &pfs) == FR_OK);
	uint64_t free0 = (uint64_t) nfree * pfs->csize * 512;
	FRESULT md = f_mkdir ("USB:/SIDECAR");
	CHECK (md == FR_OK || md == FR_EXIST);
	CHECK (f_open (g_F, "USB:/SIDECAR/keep.bin", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
	for (uint64_t p = 0; p < nFileBytes; p += kBuf)
	{
		uint32_t n = nFileBytes - p < kBuf ? (uint32_t) (nFileBytes - p) : kBuf;
		g_W[0] = (uint8_t) (p >> 20);
		CHECK (put (g_F, g_W, n));
	}
	CHECK (f_close (g_F) == FR_OK);
	CHECK (f_open (g_F, "USB:/SIDECAR/keep.bin", FA_READ) == FR_OK && f_size (g_F) == nFileBytes);
	for (uint64_t p = 0; p < nFileBytes; p += kBuf)
	{
		uint32_t n = nFileBytes - p < kBuf ? (uint32_t) (nFileBytes - p) : kBuf;
		g_W[0] = (uint8_t) (p >> 20);
		CHECK (get (g_F, g_R, n) && memcmp (g_R, g_W, n) == 0);
	}
	// an unaligned read (a file position mid-sector, a buffer mid-window): still inside the rule
	CHECK (f_lseek (g_F, 1000) == FR_OK && get (g_F, g_R + 777, 70000) && memcmp (g_R + 777, g_W + 1000, 70000) == 0);
	// without the rule above it: one f_read of 1 MB - FatFs hands the disk layer up to a cluster at once (a 128 KB+
	// exFAT cluster: over the TRB limit). Shown (the stick's format decides); the device still gets pieces (checked below)
	g_MaxReq = 0;
	UINT got = 0;
	CHECK (f_lseek (g_F, 0) == FR_OK && f_read (g_F, g_R, nFileBytes < kBuf ? (UINT) nFileBytes : kBuf, &got) == FR_OK);
	unsigned unsplit = g_MaxReq;
	CHECK (f_close (g_F) == FR_OK);
	CHECK (f_getfree (kDrive, &nfree, &pfs) == FR_OK);
	uint64_t free1 = (uint64_t) nfree * pfs->csize * 512;
	CHECK (free0 - free1 >= nFileBytes && free0 - free1 < nFileBytes + (4u << 20));
	printf ("  %s: %s, %u-sector clusters, %llu MB free -> %llu MB after %llu MB; %u device calls, the largest %u sectors, %u across 64 KB"
		" (one 1 MB f_read: %u sectors asked of the disk layer at once)\n",
		what, fs_name (pfs->fs_type), (unsigned) pfs->csize, (unsigned long long) (free0 >> 20),
		(unsigned long long) (free1 >> 20), (unsigned long long) (nFileBytes >> 20), g_Calls, g_MaxCount, g_Cross, unsplit);
	CHECK (g_MaxCount <= 128 && g_Cross == 0);
	// straight f_write / f_read of 300 KB from buffers across a 64 KB boundary, sector-aligned (split in place) and not
	// (through the bounce): FatFs hands the disk layer multi-sector transfers from the caller's buffer; the device still
	// gets <= 64 KB pieces inside one 64 KB window, and the bytes come back intact
	g_MaxCount = g_Cross = g_Odd = g_Bounced = 0;
	for (uint32_t off : {0xE000u, 0xE001u})
	{
		CHECK (f_open (g_F, "USB:/SIDECAR/raw.bin", FA_WRITE | FA_READ | FA_CREATE_ALWAYS) == FR_OK);
		UINT w = 0, r = 0;
		CHECK (f_write (g_F, g_W + off, 300000, &w) == FR_OK && w == 300000);
		memset (g_R, 0, 400000);
		CHECK (f_lseek (g_F, 0) == FR_OK && f_read (g_F, g_R + off, 300000, &r) == FR_OK && r == 300000);
		CHECK (memcmp (g_R + off, g_W + off, 300000) == 0 && f_close (g_F) == FR_OK);
	}
	CHECK (g_MaxCount <= 128 && g_Cross == 0 && g_Odd == 0 && g_Bounced > 0);
	CHECK (f_unlink ("USB:/SIDECAR/raw.bin") == FR_OK);
	// a new folder (FF_USE_LFN 3 on the Pi: dir_clear zero-fills its cluster from a malloc'd buffer of up to 32 KB)
	g_MaxCount = g_Cross = 0;
	CHECK (f_mkdir ("USB:/SIDECAR/take_001") == FR_OK && f_mkdir ("USB:/SIDECAR/take_002") == FR_OK);
	CHECK (g_MaxCount <= 128 && g_Cross == 0);
	CHECK (f_unlink ("USB:/SIDECAR/take_001") == FR_OK && f_unlink ("USB:/SIDECAR/take_002") == FR_OK);
	// unmount (the eject's first step), mount again: the file is there
	CHECK (f_mount (0, kDrive, 0) == FR_OK);
	CHECK (f_mount (g_Fs, kDrive, 1) == FR_OK);
	FILINFO fi;
	CHECK (f_stat ("USB:/SIDECAR/keep.bin", &fi) == FR_OK && fi.fsize == nFileBytes);
	CHECK (f_unlink ("USB:/SIDECAR/keep.bin") == FR_OK);
}

void made_here (const char *dir, const char *name, BYTE fmt, uint64_t nBytes, bool bBig, DWORD nAu = 0)
{
	std::string p = std::string (dir) + "/" + name;
	CHECK (attach (p.c_str (), nBytes, true));
	MKFS_PARM opt = {fmt, 0, 0, 0, nAu};
	static uint8_t work[64 * 1024];
	FRESULT fr = f_mkfs (kDrive, &opt, work, sizeof work);
	CHECK (fr == FR_OK);
	CHECK (f_mount (g_Fs, kDrive, 1) == FR_OK);
	exercise (name, 24u << 20);
#if FF_FS_EXFAT && FF_LBA64
	if (bBig)	// a sample library's long take: one file over 4 GB (exFAT only; FAT32 stops at 4 GB - 1)
	{
		g_MaxCount = g_Cross = 0;
		CHECK (f_open (g_F, "USB:/big.wav", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
		CHECK (f_lseek (g_F, (5ull << 30) - kBuf) == FR_OK && f_tell (g_F) == (5ull << 30) - kBuf);
		CHECK (put (g_F, g_W, kBuf) && f_close (g_F) == FR_OK);
		CHECK (f_open (g_F, "USB:/big.wav", FA_READ) == FR_OK && f_size (g_F) == 5ull << 30);
		CHECK (f_lseek (g_F, (5ull << 30) - kBuf) == FR_OK && get (g_F, g_R, kBuf) && !memcmp (g_R, g_W, kBuf));
		CHECK (f_close (g_F) == FR_OK && g_Cross == 0);
		CHECK (f_unlink ("USB:/big.wav") == FR_OK);
		printf ("  %s: a 5 GB file written at its end and read back\n", name);
	}
#else
	(void) bBig;
#endif
	CHECK (f_mount (0, kDrive, 0) == FR_OK);
	detach ();
	unlink (p.c_str ());
}

// a macOS-made image: mounts here or not, and the diagnosis when not
void made_by_mac (const char *path, const char *name)
{
	CHECK (attach (path, 0, false));
	FRESULT fr = f_mount (g_Fs, kDrive, 1);
	bool exfat = strstr (name, "exfat") != 0, gpt = strstr (name, "gpt") != 0;
	bool want = (!exfat || FF_FS_EXFAT) && (!gpt || FF_LBA64);
	if (fr == FR_OK)
	{
		CHECK (want);
		exercise (name, 8u << 20);
		CHECK (f_mount (0, kDrive, 0) == FR_OK);
	}
	else
	{
		uint8_t s0[512], vbr[512];
		uint32_t next = 0;
		CHECK (pread (fileno (g_Img[kUsb]), s0, 512, 0) == 512);
		const char *why = stor::classify_sector0 (s0, 0, &next);
		if (next) { CHECK (pread (fileno (g_Img[kUsb]), vbr, 512, (off_t) next * 512) == 512); why = stor::classify_sector0 (s0, vbr, 0); }
		printf ("  %s: not mounted (FatFs %d): %s\n", name, (int) fr, why);
		CHECK (!want && fr == FR_NO_FILESYSTEM);
		CHECK (strstr (why, gpt ? "GPT" : "exFAT"));
		f_mount (0, kDrive, 0);
	}
	detach ();
}
}	// namespace

int main (int argc, char **argv)
{
	printf ("test_stor_fatfs (FF_FS_EXFAT %d, FF_LBA64 %d):\n", FF_FS_EXFAT, FF_LBA64);
	g_Fs = (FATFS *) aligned_alloc (4096, 4096);	// as stor_pi.cpp: a FATFS / FIL inside one 4 KB page, so its sector
	g_F = (FIL *) aligned_alloc (4096, 4096);	// window never crosses 64 KB
	static_assert (sizeof (FATFS) <= 4096 && sizeof (FIL) <= 4096, "4 KB");
	memset (g_Fs, 0, 4096), memset (g_F, 0, 4096);
	g_W = (uint8_t *) aligned_alloc (65536, kBuf), g_R = (uint8_t *) aligned_alloc (65536, kBuf);
	g_Bounce = (uint8_t *) aligned_alloc (65536, 65536);
	for (uint32_t i = 0, r = 1; i < kBuf; i++) r = r * 1664525u + 1013904223u, g_W[i] = (uint8_t) (r >> 24);
	char dir[] = "/tmp/stor_fatfsXXXXXX";
	CHECK (mkdtemp (dir));
	made_here (dir, "fat32-mbr.img", FM_FAT32, 128ull << 20, false);
	made_here (dir, "fat16-mbr.img", FM_FAT, 64ull << 20, false);
	made_here (dir, "fat-32k.img", FM_ANY, 64ull << 20, false, 32u << 10);	// a 32 KB cluster: dir_clear's 32 KB buffer
	made_here (dir, "fat32-64k.img", FM_FAT32, 5ull << 30, false, 64u << 10);	// a 64 KB cluster (FAT32 at its largest; sparse)
#if FF_FS_EXFAT
	made_here (dir, "exfat-mbr.img", FM_EXFAT, 128ull << 20, false);
#if FF_LBA64
	made_here (dir, "exfat-6g.img", FM_EXFAT, 6ull << 30, true);	// sparse on the Mac's disk: a few MB used
	made_here (dir, "exfat-256k.img", FM_EXFAT, 1ull << 30, false, 256u << 10);	// a big drive's cluster (Windows: 256 GB+)
#endif
#endif
	rmdir (dir);
	if (argc > 1)
	{
		static const char *const kMac[] = {"fat32-mbr.dmg", "fat32-gpt.dmg", "exfat-mbr.dmg", "exfat-gpt.dmg"};
		int n = 0;
		for (const char *m : kMac)
		{
			made_by_mac ((std::string (argv[1]) + "/" + m).c_str (), m);
			n++;
		}
		CHECK (n >= 4);
	}
#if FF_USE_LFN == 3
	CHECK (g_BigAllocs >= 2);	// dir_clear's buffers were made (across 64 KB) and went through the rule
	printf ("test_stor_fatfs: FF_USE_LFN 3 (the Pi's): %u dir_clear buffers across 64 KB, the device's pieces inside the rule\n", g_BigAllocs);
#endif
	printf ("test_stor_fatfs (exFAT %d): ok\n", FF_FS_EXFAT);
	return 0;
}
