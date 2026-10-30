// pi/engine/core_port.h - the engine core's Pi port (core/platform.h): files on FatFs (Circle's addon/fatfs, the SD
// card as "SD:"), with SdFat's names; FatFs's objects live inside each File (opaque storage, so the core's hosted
// code never sees Circle's or FatFs's headers; platform_pi.cpp checks the size). One open card, used by one core at
// a time (the control core, after the boot on core 0).
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#ifndef __circle__   // the hosted core (Circle's own files, platform_pi.cpp, have circle/util.h)
#include <string.h>    // the core's string functions (the Teensy's Arduino.h and the Mac's port bring them along)
#include <strings.h>   // strcasecmp
#endif

#define CORE_COLD
#define CORE_RAM2
#define CORE_HEAP_BUFFERS   // the core's big buffers placed at boot in Circle's heap (core/heap_buffers.h): KERNEL_MAX_SIZE counts .bss
#define CORE_STR(s) (s)
// newlib's integer printf subset: %d %i %u %x %X %c %s, flags - and 0, a width, l / ll / z, and a precision for %s
// (".N" or ".*": core/presets/juno_lib.h). platform_pi.cpp; no heap, no float. C linkage: newlib's <stdio.h>, which
// the hosted code may include after this, declares sniprintf / vsniprintf extern "C" (through these macros).
extern "C" int core_vsnprintf(char* out, size_t n, const char* fmt, va_list ap);
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...);
#define sniprintf core_snprintf
#define vsniprintf core_vsnprintf

namespace core {

constexpr int kRead = 0, kWrite = 1, kAppend = 2, kReadWrite = 3;

class File {
 public:
  File();
  File(const File& o);
  File& operator=(const File& o);
  ~File();
  bool open(const char* path, int mode);
  bool open(File* dir, uint32_t index, int mode);
  bool openNext(File* dir, int mode);
  bool close();
  bool isOpen() const;
  bool isDir() const;
  size_t getName(char* out, size_t n) const;
  uint32_t dirIndex() const;
  uint64_t fileSize() const;
  int read(void* buf, size_t n);
  int read();
  size_t write(const void* buf, size_t n);
  size_t write(uint8_t b) { return write(&b, 1); }
  bool seekSet(uint64_t pos);
  uint64_t curPosition() const;
  int available() const;
  void rewind();
  bool isContiguous() const { return true; }
  bool contiguousRange(uint32_t* first, uint32_t* last) const { *first = *last = 0; return false; }
  bool getModifyDateTime(uint16_t* date, uint16_t* time) const;
  // A folder's next entry without opening it (the library's walk, core/lib/library.h; core/platform.h): its name, a
  // folder or not, its size and FAT date / time; false at the end. Shares openNext's place in the folder.
  bool nextEntry(char* name, size_t n, bool* dir, uint64_t* size, uint16_t* fat_date, uint16_t* fat_time);
  bool nextFailed() const;   // the last nextEntry's false was f_readdir's error, not the folder's end

  static const unsigned kImplBytes = 1536;   // FIL + DIR + flags + the path (platform_pi.cpp's static_assert)

 private:
  alignas(16) unsigned char impl_[kImplBytes];
};

class Fs {
 public:
  bool begin();   // mounts "SD:" (the EMMC device must be initialised: kernel.cpp)
  bool mkdir(const char* path);   // with its parents
  bool rename(const char* from, const char* to);
  bool remove(const char* path);
  bool exists(const char* path);
  int lookup(const char* path);   // 1 there, 0 not there (FR_NO_FILE / FR_NO_PATH), -1 an error (core/platform.h)
  uint64_t tree_hash(const char* path, uint32_t* entries, uint32_t* hidden);
  const char* type_name() const { return "FAT32"; }
};

}  // namespace core
