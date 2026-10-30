// pi/engine/stor_vol.h - the volumes besides the boot card (docs/STORAGE.md): USB mass storage (USB:, USB2:, USB3:,
// Circle's umsd1..3, Bulk-Only) and on the Pi 5 an NVMe SSD (NVME:, Circle's addon/nvme), with the SD card (SD:) listed
// beside them. This part is portable (no Circle; pi/engine/tests/test_stor_vol.cpp): the hot-plug state machine, the
// eject rules, the I/O piece rule, the "why does it not mount" diagnosis of sector 0, the /status JSON. The Circle side
// (stor_pi.cpp) gives it the device tree and FatFs through Ops.
//
// Everything here is core 0's (its loop: after UpdatePlugAndPlay, the console, the web task). USB: and NVME: are core 0's
// volumes only - FatFs's per-volume lock is a spin lock, deleted at an unmount; a later user on another core (the
// bundles' library on a stick) goes through a queue to core 0, as the SD's streams do (core/msamp/cardq.h).
#pragma once
#include <stdint.h>

namespace stor {

enum Vol : int { kSD = 0, kUSB, kUSB2, kUSB3, kNVME, kVols };
const char *name (int v);	// "SD" "USB" "USB2" "USB3" "NVME"
const char *drive (int v);	// "SD:" "USB:" ... (FatFs's volume IDs, Circle's ffconf.h FF_VOLUME_STRS)
const char *device (int v);	// Circle's device name: "emmc1" "umsd1" "umsd2" "umsd3" "nvme1"
int fatfs_index (int v);	// FatFs's volume number (ffconf.h order): 0 2 3 4 6 (its lock's id too)
int parse_vol (const char *s, const char **pEnd = 0);	// a name at s (any case), -1 none; *pEnd after it

enum State : uint8_t
{
	kAbsent = 0,		// nothing plugged (USB), no SSD found at boot (NVME)
	kPresent,		// the device is there but not mounted: no FAT32 / exFAT volume FatFs can read (err, why)
	kMounted,
	kEjected,		// unmounted and taken off the USB tree: safe to pull (until a device shows again)
	kUnavailable		// not on this board (NVME on the Pi 4) or turned off (nvme=0)
};
enum Fs : uint8_t { kFsNone = 0, kFat12, kFat16, kFat32, kExfat };	// FatFs's FS_FAT12.. FS_EXFAT order
enum Link : uint8_t { kLinkNone = 0, kUsbLow, kUsbFull, kUsbHigh, kUsbSuper, kSdHs, kPcie };
const char *state_name (int s);
const char *fs_name (int f);
const char *link_name (int l, int gen);	// "USB 3 SuperSpeed", "USB 2 HighSpeed", "SD HS 4-bit 50 MHz", "PCIe gen 2 x1"

struct Info			// what a mount found
{
	uint8_t fs = kFsNone;
	uint64_t total = 0, free = 0;	// bytes (free: f_getfree)
	bool free_known = true;		// false: a FAT32 without FSINFO, not counted yet (stor=free:VOL)
	uint32_t mount_us = 0;		// the mount's and the free count's time (a FAT32 without FSINFO reads its whole FAT)
};
struct Status
{
	State state = kAbsent;
	uint8_t fs = kFsNone, link = kLinkNone, gen = 0;
	uint64_t total = 0, free = 0;
	bool free_known = true;
	int err = 0;			// the last mount's error (FatFs FRESULT) or the driver's
	char why[64] = "";		// kPresent: what sector 0 shows ("GPT partition table: ...")
	char model[44] = "";		// NVMe: the model; USB: "VID:PID"
	uint32_t mounts = 0, pulls = 0, ejects = 0, mount_us = 0;
	bool busy = false;		// a user holds files open on it (the bench): no eject
	bool rec = false;		// the recorder's destination while armed or recording (rec=vol:): no eject
	bool gone = true;		// kEjected: the device has left the tree since (a new one may mount)
};

// The platform (stor_pi.cpp on the Pi, the tests' fake)
struct Ops
{
	bool (*present) (int v);			// the device is in the device tree now
	int (*mount) (int v, Info *pOut);		// f_mount + the type, size and free count; 0 ok, else FRESULT
	void (*unmount) (int v);			// f_mount (0, drive, 0)
	bool (*eject) (int v);				// off the USB tree (FatFs CTRL_EJECT: RemoveDevice); true done
	void (*describe) (int v, Status *pSt);		// a new device: link, gen, model
	void (*diagnose) (int v, char *pWhy, int nCap);	// a failed mount: what sector 0 shows (classify_sector0)
	void (*log) (int nLevel, const char *pLine);	// 0 notice, 1 warning
};
void set_ops (const Ops *pOps);
void reset (void);				// every volume absent (the tests; boot)
void set_available (int v, bool bOn);		// NVME: false on the Pi 4, or nvme=0
void set_sd (uint64_t nTotal, uint64_t nFree);	// the boot card as the platform reads it (SD is never mounted here)
// A hot-plug pass: after UpdatePlugAndPlay reported a change, and once a second. A new device is mounted (once: a failed
// mount is not retried until it is plugged again or mount_again); a pulled one unmounted (counted in pulls when it was
// mounted: data in flight may be lost); an ejected one stays "ejected" until it has left the tree and a device shows.
void poll (void);
const char *eject (int v);			// 0 done; else why not (SD, nothing there, busy)
const char *mount_again (int v);		// an ejected or unmountable device: tried again at the next poll
void set_busy (int v, bool bBusy);
void set_recording (int v, bool bOn);		// the recorder's (rec_pi.cpp): eject refused while on, even with !
void set_why (int v, const char *pWhy);		// a volume's note (NVME: why no SSD was started)
void set_free (int v, uint64_t nFree);
const Status &status (int v);
bool mounted (int v);

// The I/O rule (Circle's xHCI puts a bulk transfer in one Normal TRB, which must hold <= 64 KB and must not cross a
// 64 KB boundary): the next piece of an I/O of nLen bytes at buffer address nAddr - at most kMaxPiece and never across
// a 64 KB boundary of the address. With a 64 KB-aligned buffer every piece is a whole 64 KB but the last.
constexpr uint32_t kMaxPiece = 65536;
uint32_t piece (uintptr_t nAddr, uint32_t nLen);

// The same rule at the disk layer (stor_diskio.cpp: FatFs's disk_read / disk_write on USB:, USB2:, USB3:), so every
// FatFs path keeps it - its own sector windows, dir_clear's zero-fill (FF_USE_LFN 3: a malloc'd buffer of up to 32 KB),
// and the multi-sector transfers it makes straight to or from a caller's buffer. A transfer of nLen bytes (whole
// sectors) at device offset nOff: from a sector-aligned buffer straight, in piece()s (<= 64 KB, never across a 64 KB
// boundary; the device gets the buffer itself); from any other buffer through pBounce (64 KB-aligned, 64 KB: core 0's
// one, no allocation) in pieces of <= 64 KB. io(ctx, p, n, off) is the device's transfer; false: an error (stops).
bool split_io (uint8_t *pBuf, uint32_t nLen, uint64_t nOff, bool bWrite, uint8_t *pBounce,
	       bool (*io) (void *ctx, uint8_t *p, uint32_t n, uint64_t nOff), void *ctx);

// Why a device does not mount: sector 0 (and, for an MBR partition of type 07, that partition's first sector) read
// raw. Returns the text (static); *pNextLba: >0 when the partition's first sector is needed for the verdict (call again
// with it as pVbr). FatFs here reads MBR FAT12/16/32 only: exFAT needs the STOR_EXFAT build, GPT FF_LBA64 (the same).
const char *classify_sector0 (const uint8_t *pSector0, const uint8_t *pVbr, uint32_t *pNextLba);

// /status's "stor" object's members: "exfat":bool,"vols":[{...}, ...] (stor_pi.cpp adds the bench's beside them)
int json_vols (char *pOut, int nCap, bool bExfat);
// one line per volume for the log / the console: "USB   mounted FAT32 57.6 GB free of 59.6 GB, USB 3 SuperSpeed (0781:5581)"
int line (int v, char *pOut, int nCap);
// MB as the display shows it ("57.6 GB", "812 MB")
int mb_text (uint64_t nBytes, char *pOut, int nCap);

}	// namespace stor
