// core/host/core_port.h - the Mac's (POSIX) port of the engine core (core/platform.h), for the tests and
// sim/coreengine.py. The card is a folder (host_set_card); paths are the card's ("/Samples/x.wav").
// Directory entries come in name order, each entry's dirIndex() its place in that order.
#pragma once
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

#define CORE_COLD
#define CORE_RAM2
#define CORE_STR(s) (s)
#define sniprintf snprintf
#define vsniprintf vsnprintf

namespace core {

constexpr int kRead = 0, kWrite = 1, kAppend = 2, kReadWrite = 3;

class File {
 public:
  File() = default;
  File(const File& o) { *this = o; }
  File& operator=(const File& o);
  ~File() { close(); }
  bool open(const char* path, int mode);
  bool open(File* dir, uint32_t index, int mode);
  bool openNext(File* dir, int mode);
  bool close();
  bool isOpen() const { return fp_ != nullptr || dir_; }
  bool isDir() const { return dir_; }
  size_t getName(char* out, size_t n) const;
  uint32_t dirIndex() const { return index_; }
  uint64_t fileSize() const { return size_; }
  int read(void* buf, size_t n);
  int read();
  size_t write(const void* buf, size_t n);
  size_t write(uint8_t b) { return write(&b, 1); }
  bool seekSet(uint64_t pos);
  uint64_t curPosition() const { return pos_; }
  int available() const { return dir_ || pos_ >= size_ ? 0 : (int)(size_ - pos_); }
  void rewind() { seekSet(0); next_ = 0; }
  bool isContiguous() const { return true; }
  bool contiguousRange(uint32_t* first, uint32_t* last) const { *first = *last = 0; return false; }
  bool getModifyDateTime(uint16_t* date, uint16_t* time) const;   // FAT encoding of the file's mtime
  // a folder's next entry without opening it (core/platform.h): false at the end; shares openNext's place
  bool nextEntry(char* name, size_t n, bool* dir, uint64_t* size, uint16_t* fat_date, uint16_t* fat_time);
  bool nextFailed() const { return rd_err_; }   // the last nextEntry's false was an error (host_fail_readdir)

 private:
  bool open_full(const std::string& card_path, int mode, uint32_t index);
  std::string path_;                 // the card's path
  FILE* fp_ = nullptr;
  bool dir_ = false;
  std::vector<std::string> names_;   // a directory's entries ('.' and '..' left out), sorted
  uint32_t next_ = 0, index_ = 0;
  uint64_t size_ = 0, pos_ = 0;
  uint64_t wrote_ = 0;               // the card model: bytes written since the open (charged at the close)
  uint64_t rsec_ = ~0ull;            // the card model: the sector FatFs holds (a read inside it costs no card time)
  int mode_ = kRead;
  bool rd_err_ = false;
};

class Fs {
 public:
  bool begin();
  bool mkdir(const char* path);
  bool rename(const char* from, const char* to);
  bool remove(const char* path);
  bool exists(const char* path);
  int lookup(const char* path);   // 1 there, 0 not there, -1 an error (core/platform.h; host_fail_lookup)
  uint64_t tree_hash(const char* path, uint32_t* entries, uint32_t* hidden);
  const char* type_name() const { return "posix"; }
};

void host_set_card(const char* dir);                      // the folder standing in for the card
void host_fake_time(bool on, uint64_t us);                // tests: now_us / now_ms read us (off: the real clock)
void host_set_log_sink(void (*sink)(const char* line, int n));   // default: stderr
std::string host_path(const char* card_path);              // card path -> Mac path
// tests: a removable volume (the Pi's USB:, USB2:, ... - docs/STORAGE.md) as a folder: paths "USB:/x" map to dir/x.
// dir null or "": pulled - every path on it fails, and files left open on it fail their next read / write.
void host_set_volume(const char* drive, const char* dir);
// tests: a model of the Pi's card time (Circle's EMMC by PIO with busy-waits, FatFs's volume lock held for a whole
// call): with it on, each file operation adds its modeled time to host_card_us(), charged to whichever core the test
// says runs it (host_card_core). Off (the default): nothing counted. The figures: platform_host.cpp, "card model".
void host_card_model(bool on);
void host_card_core(int core);                             // 0 the card core, 3 the control core (the default)
uint64_t host_card_us(int core);                           // modeled card time charged to that core so far
// tests: a power cut - after `after` more changes to the card (an open for writing, a write, a rename, a remove, a
// mkdir) every further change fails, as if the card had lost power (reads still work: the test's "after the
// reboot" is host_power_cut(-1)). host_changes(): the changes made so far.
void host_power_cut(int64_t after);
// tests: opening this card path fails (a card error on a file that is there), until set off
void host_fail_open(const char* card_path, bool on);
void host_fail_close(const char* path, bool on);   // that file's close() false (FatFs's f_close failed), closed anyway
// tests: listing this card folder (File::nextEntry) fails after `after` entries - nextFailed() true - until set off
void host_fail_readdir(const char* card_path, int after, bool on);
// tests: Fs::lookup / exists of this card path is a card error (-1 / false), until set off
void host_fail_lookup(const char* card_path, bool on);
uint64_t host_changes();

}  // namespace core
