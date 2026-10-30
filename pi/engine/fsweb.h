// pi/engine/fsweb.h - the web page's file browser (sidecar.local, docs/user-machines-pi.md "File browser"): list,
// download in ranges, upload in chunks, delete, new folder, rename, free space - over a card Backend (FatFs on the
// Pi: web.cpp; stdio in pi/engine/tests/test_fsweb.cpp). Portable (hosted flags): no Circle, no allocation.
//
// WHAT IT CAN REACH: the content folders only (reachable(), below) - /Samples, /Plugins, /Recordings, /Bundles,
// /MULTISAMPLE, /WAVETABLES, /GRANULAR, /PRESETS, /MACHINES, /NAM, /CHOP, /JUNO, /DX7 - for reads as well as writes.
// The boot files and /.rytm are not reachable here at all, whoever is asking.
// The USB drives' (and the NVMe SSD's) recordings too, as /USB/Recordings, /USB2/Recordings, /USB3/Recordings,
// /NVME/Recordings (the recorder's destination, docs/STORAGE.md): listed, downloaded and deleted - nothing else of a
// drive, and nothing written there (put, mkdir, mv refused). The backend maps them with volume_path().
//
// The safe write path (as sdfiles' PUT): an upload goes to <path>.part, chunk by chunk, each chunk read back and
// compared, a running CRC-32 kept; the last chunk's CRC must equal the client's, then <path>.part replaces <path>.
// A failure leaves <path> as it was. Boot files (config.txt, cmdline.txt, sdboot*, engine.img*, kernel*.img,
// start*.elf, fixup*.dat, bootcode.bin, armstub*, tryboot.txt*, *.dtb, overlays/) are read-only here: listed with
// "ro":1, never written, deleted or renamed (sdfiles on TCP 7310 installs them). A package for /Plugins (*.rmp) is
// checked before it goes in place (header, size, CRC, ABI version, target): a bad one is refused with the reason.
// Chunks are written in 4 KB pieces with the backend's yield() between them, so one request never holds the card
// (or core 0's scheduler) for long. Each chunk also goes into the upload's sha256 (core/lib/sha256.h): a finished
// upload is reported to the library with its hash (Backend::stored).
#ifndef _pi_engine_fsweb_h
#define _pi_engine_fsweb_h
#include <stdint.h>

namespace fsweb {

constexpr int kPath = 200;               // the longest path
constexpr uint32_t kChunkMax = 64 * 1024;   // the largest upload chunk (the page sends 48 KB)

struct Entry { char name[96]; uint64_t size; bool dir; uint16_t fdate, ftime; };   // fdate / ftime: FatFs's FILINFO (0: none)

struct Backend {
  bool (*stat)(const char* path, uint64_t* size, bool* dir);
  int (*list)(const char* path, Entry* out, int max, bool* more);     // -1: not a folder
  bool (*read)(const char* path, uint64_t off, void* buf, uint32_t n, uint32_t* got);
  bool (*write_at)(const char* path, uint64_t off, const void* buf, uint32_t n, bool create);   // create: truncate first
  bool (*remove)(const char* path);   // a file or an empty folder
  bool (*rename)(const char* from, const char* to);   // to must not exist
  bool (*mkdir)(const char* path);
  bool (*space)(uint64_t* free, uint64_t* total);
  void (*yield)();                    // between 4 KB pieces (Circle: the scheduler)
  void (*changed)(const char* path);  // a file under /Plugins changed (the engine's rescan); may be null
  // the sample library (core/lib/library.h; each may be null): an upload in place with the sha256 and CRC-32 made
  // from its chunks as they came (no second read), a file or folder removed, renamed
  void (*stored)(const char* path, const uint8_t sha[32], uint32_t crc);
  void (*removed)(const char* path);
  void (*renamed)(const char* from, const char* to);
  // optional: the path is the take being recorded, inside it or one of its folders (rec_pi::TakeBusy): never deleted
  // or renamed then - FatFs keeps no table of open files (FF_FS_LOCK 0), so an unlink under the recorder's open
  // file would free clusters it goes on writing
  bool (*busy)(const char* path);
};

struct Result { int status; char msg[160]; };   // status: an HTTP code (200 ok)

bool path_ok(const char* p);          // absolute, printable ASCII, no "..", "//", '\\' or ':', at most kPath
// Whether this API may touch the path at all: the card's content folders and what is under them, and "/" for the
// listing. Everything else - the boot files, /.rytm (the Wi-Fi key, the paired devices), /overlays, anything else
// someone drops on the card - is refused for every operation, read or write. The split is: /fs/* is content,
// TCP 7310 (sdfiles.h) is firmware, /.rytm is neither. See the list in fsweb.cpp and docs/pi-engine.md.
bool reachable(const char* p);
bool read_only(const char* p);        // a boot file (or the root): see above
bool is_package(const char* p);       // /Plugins/<name>.rmp
// The Sidecar's own folders (/.rytm, /Samples, /Plugins, /Bundles): never deleted or renamed here (their contents may be);
// listed with "ro":2. config.txt.bak (the restore backup) is a boot file (config.txt.prev stays writable).
bool kept_dir(const char* p);
// A drive path ("/USB/...", "/USB2/...", "/USB3/...", "/NVME/..."): 1..4 (stor_vol.h's order), *rest its path on that
// volume ("/Recordings/..."); 0 the SD card's path (*rest = p).
int drive_of(const char* p, const char** rest = nullptr);
// FatFs's path: "SD:/Samples/a.wav", "USB:/Recordings/take_004/00_MASTER.wav"; false when it does not fit
bool volume_path(const char* p, char* out, int cap);
// The take being recorded (its id: "take_004" on the SD, "USB:take_004" on a drive) and this API's path: the take's
// folder, anything in it, a folder above it, or its volume's /Recordings/.recording (its crash recovery) - any case,
// as FatFs. rec_pi::TakeBusy (Backend::busy) asks this while a take records or closes.
bool live_take(const char* path, const char* take_id);

// The JSON of a folder: {"path":..,"free":..,"total":..,"more":0|1,"entries":[{"n":..,"d":0|1,"s":..,"t":..,"ro":0|1}]}
// (t: its date and time, "YYYY-MM-DD HH:MM" local, "" when none was written - no clock when it was made)
int list_json(const Backend& b, const char* path, char* out, int cap, Result* r);
void fat_time(uint16_t fdate, uint16_t ftime, char out[17]);   // FatFs's date / time as above ("" 0 or not a date)
// Upload chunk: data[0..n-1] at offset off of the file `path`, total bytes in all; crc the whole file's CRC-32
// (checked with the last chunk). 200 "ok <next offset>" or "done <bytes>"; 409 "expected <offset>" when off is not
// where the upload stands (resume from there; off 0 always starts over); 400/403/507 with the reason.
void put(const Backend& b, const char* path, uint64_t off, uint64_t total, uint32_t crc, const uint8_t* data, uint32_t n,
         Result* r);
void remove(const Backend& b, const char* path, Result* r);
void mkdir(const Backend& b, const char* path, Result* r);
// mkdir -p (the page's folder upload): every missing folder on the way; 200 "made" or "exists", 409 when a file
// is in the way.
void mkdirs(const Backend& b, const char* path, Result* r);
void rename(const Backend& b, const char* from, const char* to, Result* r);
// A package's check (the last chunk of an upload to /Plugins, and the tests): its .part file through b.read;
// nullptr when good, else the reason.
const char* check_package(const Backend& b, const char* path, uint64_t size);
uint32_t crc32(uint32_t crc, const uint8_t* p, uint32_t n);   // zlib's (start 0)

}  // namespace fsweb
#endif
