// pi/engine/platform_pi.cpp - core/platform.h on Circle (Circle's flags): FatFs files, the ARM generic timer
// (CNTPCT, 54 MHz on the Pi 4) for the clock and cycles, Circle's logger, the heap for the sample RAM, and the
// few C library pieces the core's hosted code needs that Circle does not have (printf to a buffer, strrchr).
#include <circle/alloc.h>
#include <circle/koptions.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/multicore.h>
#include <circle/string.h>
#include <circle/util.h>
#include <fatfs/ff.h>

#include "platform.h"
#include "console.h"
#include "crumb.h"   // the crash breadcrumb: the last file call by path (2026-10-29)

namespace {

struct Impl {
  FIL fil;
  FIL* uf;                // a file on a USB drive / the NVMe SSD: its FIL in a slot of the aligned block (fil_take)
  DIR dir;
  bool open, isdir;
  bool rd_err;            // the last nextEntry stopped on f_readdir's error (not the folder's end)
  int mode;
  uint32_t index, next;   // the entry's place in its folder; openNext's place in a folder
  char path[260];         // the card's path, "/Samples/x.wav"
};
static_assert(sizeof(Impl) <= core::File::kImplBytes, "core::File's storage is too small for FatFs's objects");

// A copy of a kReadWrite handle: read and write, without truncating (as core/host/platform_host.cpp).
constexpr int kReopen = 4;

FATFS s_FatFs;

// A path on another volume names its drive: "USB:/SIDECAR/x.zip" (FatFs's volume IDs, docs/STORAGE.md). Card paths
// never hold a ':' (fsweb::path_ok refuses one). Returns the drive's length with its colon ("USB:" 4), 0 a card path.
int vol_len(const char* p) {
  int i = 0;
  while (i < 6 && ((p[i] >= 'A' && p[i] <= 'Z') || (p[i] >= '0' && p[i] <= '9'))) ++i;
  return i > 0 && p[i] == ':' ? i + 1 : 0;
}
bool other_vol(const char* p) { int n = vol_len(p); return n && !(n == 3 && p[0] == 'S' && p[1] == 'D'); }

void full(char* out, size_t n, const char* path) {   // "SD:" + the card's path ("/" added if it has none); a drive's as is
  size_t k = 0;
  int v = vol_len(path);
  const char* pre = v ? "" : path[0] == '/' ? "SD:" : "SD:/";
  for (const char* p = pre; *p && k + 1 < n; ++p) out[k++] = *p;
  for (const char* p = path; *p && k + 1 < n; ++p) {
    out[k++] = *p;
    if (v && p == path + v - 1 && p[1] != '/' && k + 1 < n) out[k++] = '/';   // "USB:" -> "USB:/"
  }
  out[k] = 0;
}

// I/O on a USB drive (or the NVMe SSD) keeps the volume layer's rule (pi/engine/stor_vol.h stor::piece): a transfer
// of at most 64 KB that never crosses a 64 KB boundary of its buffer, from a cache-aligned buffer - Circle's xHCI puts
// a bulk transfer into one TRB. The caller's buffer can be anything, so the bytes pass through one 64 KB-aligned
// bounce block (core 0 alone uses these volumes: one block is enough).
// ... and FatFs moves every partial sector through the FIL's own sector buffer: a USB file's FIL sits in a 4 KB slot of
// a 64 KB-aligned block (as stor_pi's bench FILs), so that buffer is cache-aligned and never crosses 64 KB either.
constexpr int kFilSlots = 16;   // 16 x 4 KB: one 64 KB block (files open on the other volumes at once)
static_assert(sizeof(FIL) <= 4096, "a FIL in a 4 KB slot");
uint8_t* s_pFilBlock;
uint32_t s_nFilUsed;            // bit k: slot k taken
FIL* fil_take() {
  if (!s_pFilBlock) {
    uint8_t* p = (uint8_t*)CMemorySystem::HeapAllocate(2 * 65536, HEAP_DMA30);
    s_pFilBlock = p ? (uint8_t*)(((uintptr)p + 0xFFFF) & ~(uintptr)0xFFFF) : 0;   // (never freed)
    if (!s_pFilBlock) return 0;
  }
  for (int k = 0; k < kFilSlots; ++k)
    if (!(s_nFilUsed >> k & 1)) {
      s_nFilUsed |= 1u << k;
      return (FIL*)(s_pFilBlock + 4096 * k);
    }
  return 0;
}
void fil_give(FIL* f) {
  if (!f || !s_pFilBlock) return;
  int k = (int)(((uint8_t*)f - s_pFilBlock) / 4096);
  if (k >= 0 && k < kFilSlots) s_nFilUsed &= ~(1u << k);
}
inline FIL* FL(Impl& f) { return f.uf ? f.uf : &f.fil; }
inline const FIL* FL(const Impl& f) { return f.uf ? f.uf : &f.fil; }
uint8_t* s_pBounce;
uint8_t* bounce() {
  if (!s_pBounce) {
    uint8_t* p = (uint8_t*)CMemorySystem::HeapAllocate(2 * 65536, HEAP_DMA30);
    s_pBounce = p ? (uint8_t*)(((uintptr)p + 0xFFFF) & ~(uintptr)0xFFFF) : 0;   // (never freed)
  }
  return s_pBounce;
}

uint64_t ticks() {
  uint64_t t;
  asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t)::"memory");
  return t;
}
uint64_t freq() {
  uint64_t f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}

// ---- printf to a buffer (core_port.h's subset). Circle's CString has no ".*" precision and allocates.
struct Out {
  char* p;
  size_t n, k;   // room (with the NUL), characters produced (all of them, as snprintf's return)
  void put(char c) {
    if (k + 1 < n) p[k] = c;
    ++k;
  }
  void pad(char c, int w) {
    while (w-- > 0) put(c);
  }
};

}  // namespace

extern "C" int core_vsnprintf(char* out, size_t n, const char* fmt, va_list ap) {
  Out o = {out, n, 0};
  for (const char* f = fmt; *f; ++f) {
    if (*f != '%') {
      o.put(*f);
      continue;
    }
    ++f;
    bool left = false, zero = false;
    for (;; ++f)
      if (*f == '-') left = true;
      else if (*f == '0') zero = true;
      else break;
    int width = 0, prec = -1;
    if (*f == '*') {
      width = va_arg(ap, int);
      ++f;
    } else {
      while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
    }
    if (*f == '.') {
      ++f;
      prec = 0;
      if (*f == '*') {
        prec = va_arg(ap, int);
        ++f;
      } else {
        while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
      }
    }
    int lng = 0;   // 0 int, 1 long, 2 long long, 3 size_t
    if (*f == 'l') {
      lng = 1;
      if (*++f == 'l') {
        lng = 2;
        ++f;
      }
    } else if (*f == 'z') {
      lng = 3;
      ++f;
    } else if (*f == 'h') {
      ++f;
      if (*f == 'h') ++f;
    }
    char c = *f;
    if (!c) break;
    if (c == '%') {
      o.put('%');
    } else if (c == 'c') {
      char ch = (char)va_arg(ap, int);
      if (!left) o.pad(' ', width - 1);
      o.put(ch);
      if (left) o.pad(' ', width - 1);
    } else if (c == 's') {
      const char* s = va_arg(ap, const char*);
      if (!s) s = "(null)";
      int len = 0;
      while (s[len] && (prec < 0 || len < prec)) ++len;
      if (!left) o.pad(' ', width - len);
      for (int i = 0; i < len; ++i) o.put(s[i]);
      if (left) o.pad(' ', width - len);
    } else if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'p') {
      unsigned long long u;
      bool neg = false;
      if (c == 'p') {
        u = (unsigned long long)(uintptr_t)va_arg(ap, void*);
      } else if (c == 'd' || c == 'i') {
        long long s = lng == 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long)
                    : lng == 3 ? (long long)va_arg(ap, size_t) : va_arg(ap, int);
        neg = s < 0;
        u = neg ? 0ull - (unsigned long long)s : (unsigned long long)s;
      } else {
        u = lng == 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long)
          : lng == 3 ? va_arg(ap, size_t) : va_arg(ap, unsigned);
      }
      unsigned base = (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
      const char* dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
      char b[24];
      int k = 0;
      do {
        b[k++] = dig[u % base];
        u /= base;
      } while (u);
      int len = k + (neg ? 1 : 0);
      if (!left && !zero) o.pad(' ', width - len);
      if (neg) o.put('-');
      if (!left && zero) o.pad('0', width - len);
      while (k) o.put(b[--k]);
      if (left) o.pad(' ', width - len);
    } else {   // not in the subset: shown as is
      o.put('%');
      o.put(c);
    }
  }
  if (n) out[o.k < n ? o.k : n - 1] = 0;
  return (int)o.k;
}

extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int k = core_vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return k;
}

// The calling core (0..3): the voices' shared render state is per core (RYTM_RENDER_CORES=4 in the hosted build: the
// 6-OP FM reload stage and bank, GRAIN's budget, stmlib's Random, rand()), since cores 1 and 2 render at once.
extern "C" int rytm_render_core() { return (int)CMultiCoreSupport::ThisCore(); }

// memchr: the plugin loader's (teensy/libraries/RytmPlugins/src, hosted: core/user/um.cpp)
extern "C" void* memchr(const void* p, int c, size_t n) {
  const unsigned char* s = (const unsigned char*)p;
  for (; n--; ++s)
    if (*s == (unsigned char)c) return (void*)s;
  return 0;
}

extern "C" char* strrchr(const char* s, int c) {
  const char* last = 0;
  for (;; ++s) {
    if (*s == (char)c) last = s;
    if (!*s) return (char*)last;
  }
}

namespace core {

// ticks * m / f without the product's overflow (ticks * 1000000 passes 2^64 after 94.9 h at 54 MHz).
static uint64_t scale(uint64_t t, uint64_t m) {
  uint64_t f = freq();
  return t / f * m + t % f * m / f;
}
uint32_t now_us() { return (uint32_t)scale(ticks(), 1000000); }
uint32_t now_ms() { return (uint32_t)scale(ticks(), 1000); }
uint32_t cycles() { return (uint32_t)ticks(); }
uint32_t cycles_per_second() { return (uint32_t)freq(); }
uint32_t random_seed() { return (uint32_t)ticks(); }

void log(const char* fmt, ...) {
  if (g_bQuietLog) {   // a capture is dumping (console.h)
    g_nQuietLogDropped = g_nQuietLogDropped + 1;
    return;
  }
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  core_vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  CLogger::Get()->Write("core", LogNotice, "%s", b);
}

// The sample RAM: sampleram=MB in cmdline.txt (default 256; 0 off), from the low heap (malloc's). At most what
// that heap has free less 128 MB for USB and the rest: an allocation past its end is Circle's "Out of memory" panic.
uint32_t sample_ram_total() {
  size_t mb = CKernelOptions::Get()->GetAppOptionDecimal("sampleram", 256);
  size_t room = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW) >> 20;
  room = room > 128 ? room - 128 : 0;
  if (mb > room) mb = room;
  if (mb > 1024) mb = 1024;
  return (uint32_t)(mb << 20);
}
// The core allocates its arena once at boot (pool/sample_ram_pool.cpp). Circle does not reuse a freed block over
// its largest bucket (512 KB): freeing and allocating the arena again would leak it.
void* sample_ram_alloc(uint32_t bytes) { return malloc(bytes); }
void sample_ram_free(void* p) { free(p); }
// MULTISAMPLE's instruments: msampram=MB (default 512), from the high heap (above 1 GB: the Pi 4/5 with 2 GB or more)
// less 64 MB, else the low heap less 192 MB. One block at boot (never freed: see sample_ram_alloc).
uint64_t msamp_ram_total() {
  uint64_t mb = CKernelOptions::Get()->GetAppOptionDecimal("msampram", 512);
  uint64_t hi = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_HIGH) >> 20;
  uint64_t room = hi > 64 ? hi - 64 : 0;
  if (!room) {
    uint64_t lo = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW) >> 20;
    room = lo > 192 ? lo - 192 : 0;
  }
  if (mb > room) mb = room;
  return mb << 20;
}
void* msamp_ram_alloc(uint64_t bytes) { return CMemorySystem::HeapAllocate(bytes, HEAP_ANY); }
// The pool RAM (core/pool/pcache.h): poolram=MB, default 1024 MB (a 2 / 4 GB board) or 3072 MB (8 GB), at most the
// high heap's free space (else the low heap's) less 64 MB at the time it is taken (after the machines' own buffers).
uint64_t pool_ram_budget() {
  uint64_t total = CMemorySystem::Get()->GetMemSize() >> 20;
  uint64_t def = total >= 6000 ? 3072 : 1024;
  uint64_t mb = CKernelOptions::Get()->GetAppOptionDecimal("poolram", (unsigned)def);
  uint64_t hi = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_HIGH) >> 20;
  uint64_t room = hi > 64 ? hi - 64 : 0;
  if (!room) {
    uint64_t lo = CMemorySystem::Get()->GetHeapFreeSpace(HEAP_LOW) >> 20;
    room = lo > 192 ? lo - 192 : 0;
  }
  if (mb > room) mb = room;
  return mb << 20;
}
uint64_t card_free_bytes() {   // f_getfree walks the FAT once (cached by FatFs after that)
  DWORD nFree;
  FATFS* pFs;
  if (f_getfree("SD:", &nFree, &pFs) != FR_OK) return 0;
  return (uint64_t)nFree * pFs->csize * 512;
}

// ---- files
static Impl& I(unsigned char* p) { return *reinterpret_cast<Impl*>(p); }
static const Impl& I(const unsigned char* p) { return *reinterpret_cast<const Impl*>(p); }

File::File() { memset(impl_, 0, sizeof(Impl)); }
File::~File() { close(); }
File::File(const File& o) {
  memset(impl_, 0, sizeof(Impl));
  *this = o;
}

static bool is_root(const char* path) {   // the card's root, or a drive's ("USB:", "USB:/")
  int v = vol_len(path);
  return !path[v] || !strcmp(path + v, "/");
}

// isdir: 1 a folder, 0 a file (the caller has its FILINFO), -1 unknown (f_stat, which walks the path).
static bool open_path(Impl& f, const char* path, int mode, uint32_t index, int isdir = -1) {
  char p[270];
  full(p, sizeof p, path);
  crumb::Fs(mode == kRead ? "open r" : "open w", p);
  bool dir = isdir >= 0 ? isdir == 1 : is_root(path);
  if (isdir < 0 && !dir) {
    FILINFO fi;
    dir = f_stat(p, &fi) == FR_OK && (fi.fattrib & AM_DIR);
  }
  if (dir) {
    if (mode != kRead || f_opendir(&f.dir, p) != FR_OK) return false;
    f.isdir = true;
  } else {
    BYTE m = mode == kRead        ? FA_READ
           : mode == kWrite       ? (FA_WRITE | FA_CREATE_ALWAYS)
           : mode == kAppend      ? (FA_WRITE | FA_OPEN_APPEND)
           : mode == kReadWrite   ? (FA_READ | FA_WRITE | FA_CREATE_ALWAYS)
                                  : (FA_READ | FA_WRITE | FA_OPEN_EXISTING);   // kReopen
    f.uf = other_vol(path) ? fil_take() : 0;
    if (other_vol(path) && !f.uf) return false;   // (all slots in use)
    if (f_open(FL(f), p, m) != FR_OK) {
      fil_give(f.uf);
      f.uf = 0;
      return false;
    }
    f.isdir = false;
  }
  f.open = true;
  f.mode = mode == kReopen ? kReadWrite : mode;
  f.index = index;
  f.next = 0;
  strncpy(f.path, path, sizeof f.path - 1);
  f.path[sizeof f.path - 1] = 0;
  return true;
}

// A second handle with its own position. A file open for reading is copied by value, FIL and all, without
// touching the card (DSP SAMPLE copies a pool file on every trig: voices.cpp, stream_voice.h); FatFs keeps no
// shared per-file state with FF_FS_LOCK 0, so read-only copies are independent. A folder, and a file open for
// writing, are opened again by path (a write handle's copy: kReadWrite without truncating, else kAppend). Writing
// through two handles of one file is unsupported: each FIL caches its own sector and size.
File& File::operator=(const File& o) {
  if (this == &o) return *this;
  close();
  const Impl& s = I(o.impl_);
  if (!s.open) return *this;
  Impl& d = I(impl_);
  if (!s.isdir && s.mode == kRead && !s.uf) {   // (a USB file's FIL is in its slot: opened again below)
    memcpy(&d, &s, sizeof(Impl));
    return *this;
  }
  int mode = s.mode == kRead ? kRead : s.mode == kReadWrite ? kReopen : kAppend;
  if (open_path(d, s.path, mode, s.index)) {
    if (s.isdir) {
      FILINFO fi;
      for (uint32_t k = 0; k < s.next && f_readdir(&d.dir, &fi) == FR_OK && fi.fname[0]; ++k) d.next++;
    } else {
      f_lseek(FL(d), f_tell(FL(s)));
    }
  }
  return *this;
}

bool File::open(const char* path, int mode) {
  close();
  return open_path(I(impl_), path, mode, 0);
}

static bool child_path(const Impl& dir, const char* name, char* out, size_t n) {
  size_t a = strlen(dir.path);
  bool slash = a && dir.path[a - 1] == '/';
  if (a + 1 + strlen(name) + 1 > n) return false;
  strcpy(out, dir.path);
  if (!slash) strcat(out, "/");
  strcat(out, name);
  return true;
}

bool File::open(File* dir, uint32_t index, int mode) {   // a fresh DIR: dir's own iteration is untouched
  close();
  if (!dir) return false;
  const Impl& d = I(dir->impl_);
  if (!d.open || !d.isdir) return false;
  char p[270];
  full(p, sizeof p, d.path);
  DIR dd;
  if (f_opendir(&dd, p) != FR_OK) return false;
  FILINFO fi;
  bool ok = false;
  for (uint32_t k = 0; f_readdir(&dd, &fi) == FR_OK && fi.fname[0]; ++k)
    if (k == index) {
      char c[260];
      ok = child_path(d, fi.fname, c, sizeof c) && open_path(I(impl_), c, mode, index, (fi.fattrib & AM_DIR) ? 1 : 0);
      break;
    }
  f_closedir(&dd);
  return ok;
}

bool File::openNext(File* dir, int mode) {
  close();
  if (!dir) return false;
  Impl& d = I(dir->impl_);
  if (!d.open || !d.isdir) return false;
  FILINFO fi;
  while (f_readdir(&d.dir, &fi) == FR_OK && fi.fname[0]) {
    uint32_t k = d.next++;
    char c[260];
    if (child_path(d, fi.fname, c, sizeof c) && open_path(I(impl_), c, mode, k, (fi.fattrib & AM_DIR) ? 1 : 0))
      return true;
  }
  return false;
}

bool File::nextEntry(char* name, size_t n, bool* dir, uint64_t* size, uint16_t* fat_date, uint16_t* fat_time) {
  Impl& d = I(impl_);
  d.rd_err = false;
  if (!d.open || !d.isdir || !n) return false;
  FILINFO fi;
  if (f_readdir(&d.dir, &fi) != FR_OK) return d.rd_err = true, false;   // (nextFailed: never taken for the end)
  if (!fi.fname[0]) return false;
  d.next++;
  size_t k = strlen(fi.fname) < n - 1 ? strlen(fi.fname) : n - 1;
  memcpy(name, fi.fname, k);
  name[k] = 0;
  *dir = (fi.fattrib & AM_DIR) != 0;
  *size = *dir ? 0 : (uint64_t)fi.fsize;
  *fat_date = fi.fdate, *fat_time = fi.ftime;
  return true;
}

bool File::close() {   // false: nothing open, or FatFs's close failed (a write's last sync: data or entry not kept)
  Impl& f = I(impl_);
  if (!f.open) return false;
  bool ok = f.isdir ? f_closedir(&f.dir) == FR_OK : f_close(FL(f)) == FR_OK;
  if (f.uf) fil_give(f.uf), f.uf = 0;
  f.open = false;
  return ok;
}

bool File::nextFailed() const { return I(impl_).rd_err; }
bool File::isOpen() const { return I(impl_).open; }
bool File::isDir() const { return I(impl_).open && I(impl_).isdir; }
uint32_t File::dirIndex() const { return I(impl_).index; }

size_t File::getName(char* out, size_t n) const {
  const char* p = I(impl_).path;
  const char* s = strrchr(p, '/');
  s = s ? s + 1 : p;
  size_t k = strlen(s) < n - 1 ? strlen(s) : n - 1;
  memcpy(out, s, k);
  out[k] = 0;
  return k;
}

uint64_t File::fileSize() const {
  const Impl& f = I(impl_);
  return f.open && !f.isdir ? f_size(FL(f)) : 0;
}

int File::read(void* buf, size_t n) {
  Impl& f = I(impl_);
  if (!f.open || f.isdir || (f.mode != kRead && f.mode != kReadWrite)) return -1;
  UINT got = 0;
  if (other_vol(f.path)) {   // in 64 KB pieces through the aligned block
    uint8_t* b = bounce();
    if (!b) return -1;
    size_t d = 0;
    while (d < n) {
      UINT k = n - d < 65536 ? (UINT)(n - d) : 65536, r = 0;
      if (f_read(FL(f), b, k, &r) != FR_OK) return d ? (int)d : -1;
      memcpy((uint8_t*)buf + d, b, r);
      d += r;
      if (r < k) break;   // the file's end
    }
    return (int)d;
  }
  return f_read(FL(f), buf, (UINT)n, &got) == FR_OK ? (int)got : -1;
}

int File::read() {
  uint8_t b;
  return read(&b, 1) == 1 ? b : -1;
}

size_t File::write(const void* buf, size_t n) {
  Impl& f = I(impl_);
  if (!f.open || f.isdir || f.mode == kRead) return 0;
  if (f.mode == kAppend && f_lseek(FL(f), f_size(FL(f))) != FR_OK) return 0;   // as O_APPEND: always at the end
  UINT put = 0;
  if (other_vol(f.path)) {   // in 64 KB pieces through the aligned block
    uint8_t* b = bounce();
    if (!b) return 0;
    size_t d = 0;
    while (d < n) {
      UINT k = n - d < 65536 ? (UINT)(n - d) : 65536, w = 0;
      memcpy(b, (const uint8_t*)buf + d, k);
      if (f_write(FL(f), b, k, &w) != FR_OK) break;
      d += w;
      if (w < k) break;   // the volume is full
    }
    f_sync(FL(f));
    return d;
  }
  if (f_write(FL(f), buf, (UINT)n, &put) != FR_OK) return 0;
  f_sync(FL(f));
  return put;
}

bool File::seekSet(uint64_t pos) {
  Impl& f = I(impl_);
  if (!f.open) return false;
  if (f.isdir) {
    if (pos != 0 || f_rewinddir(&f.dir) != FR_OK) return false;
    f.next = 0;
    return true;
  }
  if (pos > f_size(FL(f))) return false;
  return f_lseek(FL(f), (FSIZE_t)pos) == FR_OK;
}

uint64_t File::curPosition() const {
  const Impl& f = I(impl_);
  return f.open && !f.isdir ? f_tell(FL(f)) : 0;
}

int File::available() const {
  const Impl& f = I(impl_);
  return f.open && !f.isdir ? (int)(f_size(FL(f)) - f_tell(FL(f))) : 0;
}

void File::rewind() {
  Impl& f = I(impl_);
  if (!f.open) return;
  if (f.isdir) {
    f_rewinddir(&f.dir);
    f.next = 0;
  } else {
    f_lseek(FL(f), 0);
  }
}

bool File::getModifyDateTime(uint16_t* date, uint16_t* time) const {
  *date = *time = 0;
  const Impl& f = I(impl_);
  if (!f.open || is_root(f.path)) return false;
  char p[270];
  full(p, sizeof p, f.path);
  FILINFO fi;
  if (f_stat(p, &fi) != FR_OK) return false;
  *date = fi.fdate;
  *time = fi.ftime;
  return true;
}

// ---- the card
bool Fs::begin() { return f_mount(&s_FatFs, "SD:", 1) == FR_OK; }

bool Fs::mkdir(const char* path) {   // with its parents, as SdFat's mkdir
  char p[270];
  full(p, sizeof p, path);
  crumb::Fs("mkdir", p);
  int r = vol_len(p) + 1;   // after "SD:/" (or "USB:/")
  for (char* c = p + r;; ++c)
    if (*c == '/' || !*c) {
      char keep = *c;
      *c = 0;
      FILINFO fi;
      if (c > p + r && f_stat(p, &fi) != FR_OK && f_mkdir(p) != FR_OK) return false;
      if (!keep) return true;
      *c = keep;
    }
}

bool Fs::rename(const char* a, const char* b) {
  char pa[270], pb[270];
  if (vol_len(a) != vol_len(b) || strncmp(a, b, vol_len(a))) return false;   // (FatFs renames within one volume only)
  full(pa, sizeof pa, a);
  full(pb, sizeof pb, b);
  crumb::Fs("rename", pa);
  return f_rename(pa, pb + vol_len(pb)) == FR_OK;   // FatFs: the new name's drive is the old one's ("SD:" left off)
}

bool Fs::remove(const char* path) {
  char p[270];
  full(p, sizeof p, path);
  crumb::Fs("remove", p);
  return f_unlink(p) == FR_OK;
}

bool Fs::exists(const char* path) {
  if (is_root(path)) return true;
  char p[270];
  full(p, sizeof p, path);
  crumb::Fs("exists", p);
  FILINFO fi;
  return f_stat(p, &fi) == FR_OK;
}

int Fs::lookup(const char* path) {
  if (is_root(path)) return 1;
  char p[270];
  full(p, sizeof p, path);
  crumb::Fs("lookup", p);
  FILINFO fi;
  FRESULT r = f_stat(p, &fi);
  return r == FR_OK ? 1 : r == FR_NO_FILE || r == FR_NO_PATH ? 0 : -1;
}

static void fnv(uint64_t* h, const void* d, size_t n) {
  const uint8_t* b = (const uint8_t*)d;
  for (size_t i = 0; i < n; ++i) {
    *h ^= b[i];
    *h *= 0x100000001b3ull;
  }
}

// FatFs lists a folder in its directory order (no sort): the fingerprint changes when entries move, which only
// rebuilds the index.
static void walk(const char* sdpath, int depth, uint64_t* h, uint32_t* entries, uint32_t* hidden) {
  DIR d;
  if (f_opendir(&d, sdpath) != FR_OK) return;
  FILINFO fi;
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
    if (fi.fname[0] == '.') {
      ++*hidden;
      continue;
    }
    fnv(h, fi.fname, strlen(fi.fname));
    fnv(h, &fi.fsize, sizeof fi.fsize);
    fnv(h, &fi.fdate, sizeof fi.fdate);
    fnv(h, &fi.ftime, sizeof fi.ftime);
    uint8_t mark = (fi.fattrib & AM_DIR) ? 1 : 0;
    fnv(h, &mark, 1);
    ++*entries;
    if (mark && depth < 12) {
      char sub[270];
      size_t a = strlen(sdpath), b = strlen(fi.fname);
      bool slash = a && sdpath[a - 1] == '/';   // the root is "SD:/"
      if (a + 1 + b + 1 > sizeof sub) continue;
      memcpy(sub, sdpath, a);
      if (!slash) sub[a++] = '/';
      memcpy(sub + a, fi.fname, b + 1);
      walk(sub, depth + 1, h, entries, hidden);
      fnv(h, "/", 1);
    }
  }
  f_closedir(&d);
}

uint64_t Fs::tree_hash(const char* path, uint32_t* entries, uint32_t* hidden) {
  *entries = *hidden = 0;
  char p[270];
  full(p, sizeof p, path);
  FILINFO fi;
  if (!is_root(path) && (f_stat(p, &fi) != FR_OK || !(fi.fattrib & AM_DIR))) return 0;
  uint64_t h = 0xcbf29ce484222325ull;
  walk(p, 0, &h, entries, hidden);
  return h;
}

}  // namespace core
