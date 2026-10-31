// pi/engine/crumb.h - a crash breadcrumb (diagnostic, 2026-10-29): what each core was doing, kept in RAM that a
// watchdog reset leaves alone, reported at the next boot. Why: the Pi rebooted with nothing in the log (core 0 hung:
// no HTTP, no ping, then the BCM watchdog; or a fault that halts before /log is read again) - the log the page reads
// is RAM only and the polling saw it up to 5 s before the end. Here, in a fixed area (Area) that Circle never uses
// (the gap between its exception stacks and the coherent region, memorymap64.h) and the firmware does not load into:
//   - the log's tail (kLog bytes, CLogTee::Write: every line any core logs, as it is logged)
//   - per core: its last heartbeat (CNTPCT) and the section it is in (core 0: mbtrace's ids, set by mbtrace::Scope;
//     core 3: its pass's part; cores 1-2: a block's render) with the time it entered it
//   - the last FatFs disk transfer (stor_diskio.cpp: drive, sector, count, still in flight or not, a running count)
//   - the last file-system call by path (platform_pi.cpp: open / stat / rename / remove / mkdir) and the web request
//   - an exception outside a voice's render (guard.cpp: core, ESR, ELR, FAR) and a planned reboot's or panic's reason
// Every write is cleaned to the point of coherency (dc cvac): a reset drops dirty cache lines, not DRAM.
// At the next boot (crumb::Begin first thing in CKernel::Initialize) the old area is copied aside when its magic
// words hold, and crumb::Report logs it: "crumb: the previous boot ..." lines and its log's last lines ("prev| ...");
// kernel.cpp also writes that whole log tail to /.rytm/prevboot.txt. No magic (a power-on, RAM not kept): one line
// says so. Reading it: the core whose heartbeat stopped first is the one that hung; core 0's section and the disk /
// file lines say where (a disk transfer in flight: the SD driver; the disk count far past the section's start: FatFs
// looping; neither: code of the engine's own).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(CRUMB_HOST) && __has_include(<circle/memorymap.h>)
#include <circle/sysconfig.h>   // (KERNEL_MAX_SIZE, KERNEL_STACK_SIZE: memorymap.h needs them)
#include <circle/memorymap.h>
#define CRUMB_PI 1
extern "C" int core_snprintf(char* out, size_t n, const char* fmt, ...);   // platform_pi.cpp (no float, no snprintf)
#define CRUMB_FMT core_snprintf
#elif defined(CRUMB_HOST)
#include <stdio.h>
#define CRUMB_PI 0
#define CRUMB_FMT snprintf
#else
#error "crumb.h: Circle's headers (a Pi build) or CRUMB_HOST (the host tests) - never a host area in a Pi kernel"
#endif

namespace crumb {

constexpr uint32_t kMagic = 0x424d5243u, kMagic2 = 0x21444e45u, kVersion = 2;   // "CRMB", "END!"
constexpr unsigned kLog = 32768;   // a power of two: ~30 s of the log (the stats lines are ~1 KB a second)

struct alignas(64) CoreRec {
  volatile uint64_t beat;    // CNTPCT at its last heartbeat
  volatile uint64_t sec_t;   // CNTPCT when it entered sec
  volatile uint32_t sec;     // the section (see sec_name)
  volatile uint32_t beats;
};
struct alignas(64) DiskRec {
  volatile uint64_t t;       // CNTPCT at the transfer's start
  volatile uint32_t ops;     // transfers since the boot
  volatile uint32_t drv, sector, count;
  volatile uint32_t busy;    // 1 read in flight, 2 write in flight, 0 none
};
struct alignas(64) FsRec {
  volatile uint64_t t;
  volatile uint32_t n;       // calls since the boot
  char op[12];
  char path[104];
};
struct alignas(64) WebRec {   // the HTTP workers interleave (a handler may yield): requests open counted
  volatile uint64_t t;       // when path (the oldest request still open) began
  volatile uint32_t n, busy; // requests since the boot, requests open now
  char path[48];             // the oldest open request (or the last one, none open)
  char last[48];             // the newest request begun
};
struct alignas(64) FaultRec {
  volatile uint64_t elr, far, t;
  volatile uint32_t core, esr, n;
  char why[68];
};
struct alignas(64) Area {
  uint32_t magic, version, boot, pad;
  uint64_t freq;             // CNTFRQ
  CoreRec core[4];
  DiskRec disk;
  FsRec fs;
  WebRec web;
  FaultRec fault;
  volatile uint64_t log_total;   // bytes logged since the boot (log[total % kLog] the next)
  char log[kLog];
  uint32_t magic2;
};

// ---- the area and the cache
#if CRUMB_PI
// the middle of the gap [MEM_EXCEPTION_STACK_END, MEM_COHERENT_REGION) (>= 1 MB: memorymap64.h rounds +2 MB down to
// 1 MB), 64 KB aligned
constexpr uintptr_t kAddr = ((MEM_EXCEPTION_STACK_END + MEM_COHERENT_REGION) / 2) & ~(uintptr_t)0xFFFF;
static_assert(kAddr >= MEM_EXCEPTION_STACK_END && kAddr + sizeof(Area) <= MEM_COHERENT_REGION, "the crumb's gap");
inline Area* area() { return (Area*)kAddr; }
inline uint64_t now() {
  uint64_t t;
  asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t)::"memory");
  return t;
}
inline uint64_t freq() {
  uint64_t f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}
inline unsigned this_core() {
  uint64_t m;
  asm volatile("mrs %0, mpidr_el1" : "=r"(m));
  return (unsigned)(m & 3);
}
// to the point of coherency: DRAM keeps it over a reset. No barrier on the hot paths (a hang resets 15 s later: the
// clean completes by itself); sync (dsb) where a reset may follow at once (a fault, a planned reboot, Begin)
inline void clean(const volatile void* p, size_t n, bool sync = false) {
  uintptr_t a = (uintptr_t)p & ~(uintptr_t)63, e = (uintptr_t)p + n;
  for (; a < e; a += 64) asm volatile("dc cvac, %0" ::"r"(a) : "memory");
  if (sync) asm volatile("dsb sy" ::: "memory");
}
#else
// the host tests: an ordinary buffer ("a reboot" is Begin again on the same memory)
inline Area* area() {
  static Area a;
  return &a;
}
inline uint64_t& host_clock() {
  static uint64_t t = 1;
  return t;
}
inline uint64_t now() { return host_clock(); }
inline uint64_t freq() { return 1000000; }
inline unsigned& host_core() {
  static unsigned c = 0;
  return c;
}
inline unsigned this_core() { return host_core(); }
inline void clean(const volatile void*, size_t, bool = false) {}
#endif

inline bool& on() {   // Begin ran (nothing is written to the area before: it holds the last boot's)
  static bool b = false;
  return b;
}
inline Area& prev() {   // the previous boot's area, copied aside by Begin
  static Area p;
  return p;
}
inline bool& had_prev() {
  static bool b = false;
  return b;
}

inline void copy_str(char* out, size_t cap, const char* s) {
  size_t k = 0;
  for (; s && s[k] && k + 1 < cap; ++k) out[k] = s[k];
  out[k] = 0;
}

// boot: the old area aside (when whole), a fresh one
inline void Begin() {
  Area* a = area();
  had_prev() = a->magic == kMagic && a->magic2 == kMagic2 && a->version == kVersion;
  uint32_t boot = 1;
  if (had_prev()) {
    memcpy((void*)&prev(), (const void*)a, sizeof(Area));
    Area& p = prev();   // (DRAM may have lost bits over the reset: every string ends inside its field)
    p.fs.op[sizeof p.fs.op - 1] = 0, p.fs.path[sizeof p.fs.path - 1] = 0;
    p.web.path[sizeof p.web.path - 1] = 0, p.web.last[sizeof p.web.last - 1] = 0;
    p.fault.why[sizeof p.fault.why - 1] = 0;
    boot = p.boot + 1;
  }
  memset((void*)a, 0, sizeof(Area));
  a->magic = kMagic, a->version = kVersion, a->boot = boot, a->freq = freq(), a->magic2 = kMagic2;
  clean(a, sizeof(Area), true);
  on() = true;
}

// ---- the writers (any core; each record written by one core at a time, or racy but harmless for a diagnostic)
inline void Beat(unsigned c) {
  if (!on() || c > 3) return;
  CoreRec& r = area()->core[c];
  r.beat = now(), r.beats = r.beats + 1;
  clean(&r, sizeof r);
}
inline void BeatEnter(unsigned c, uint32_t sec) {   // a heartbeat and a section in one write (the render cores)
  if (!on() || c > 3) return;
  CoreRec& r = area()->core[c];
  uint64_t t = now();
  r.beat = t, r.beats = r.beats + 1, r.sec = sec, r.sec_t = t;
  clean(&r, sizeof r);
}
inline uint32_t Enter(uint32_t sec) {   // the calling core's section: the one before returned (for Leave)
  if (!on()) return 0;
  CoreRec& r = area()->core[this_core()];
  uint32_t was = r.sec;
  r.sec = sec, r.sec_t = now();
  clean(&r, sizeof r);
  return was;
}
inline void Leave(uint32_t was) {
  if (!on()) return;
  CoreRec& r = area()->core[this_core()];
  r.sec = was, r.beat = now();
  clean(&r, sizeof r);
}
inline void DiskBegin(unsigned drv, uint32_t sector, uint32_t count, bool write) {
  if (!on()) return;
  DiskRec& d = area()->disk;
  d.t = now(), d.ops = d.ops + 1, d.drv = drv, d.sector = sector, d.count = count, d.busy = write ? 2 : 1;
  clean(&d, sizeof d);
}
inline void DiskEnd() {
  if (!on()) return;
  DiskRec& d = area()->disk;
  d.busy = 0;
  clean(&d, sizeof d);
}
inline void Fs(const char* op, const char* path) {
  if (!on()) return;
  FsRec& f = area()->fs;
  f.t = now(), f.n = f.n + 1;
  f.op[0] = 'c', f.op[1] = (char)('0' + this_core()), f.op[2] = ':';   // (the calling core: only core 0 owns the card)
  copy_str(f.op + 3, sizeof f.op - 3, op);
  copy_str(f.path, sizeof f.path, path);
  clean(&f, sizeof f);
}
inline void Web(const char* path, bool begin) {   // core 0's HTTP workers only (one core: no atomics needed)
  if (!on()) return;
  WebRec& w = area()->web;
  if (begin) {
    w.n = w.n + 1;
    copy_str(w.last, sizeof w.last, path);
    if (!w.busy) w.t = now(), copy_str(w.path, sizeof w.path, path);   // (the oldest open one kept)
    w.busy = w.busy + 1;
  } else if (w.busy) {
    w.busy = w.busy - 1;
  }
  clean(&w, sizeof w);
}
inline void Fault(unsigned core, uint32_t esr, uint64_t elr, uint64_t far, const char* why) {
  if (!on()) return;
  FaultRec& f = area()->fault;
  if (f.n++ == 0 || why) {   // the first fault kept (a second one, a consequence, counted)
    f.core = core, f.esr = esr, f.elr = elr, f.far = far, f.t = now();
    copy_str(f.why, sizeof f.why, why ? why : "exception");
  }
  clean(&f, sizeof f, true);
}
inline void Why(const char* why) {   // a planned reboot's reason, a panic
  if (!on()) return;
  FaultRec& f = area()->fault;
  f.t = now();
  copy_str(f.why, sizeof f.why, why);
  clean(&f, sizeof f, true);
}
inline void Log(const char* p, size_t n) {   // CLogTee::Write (under its lock)
  if (!on()) return;
  Area* a = area();
  uint64_t t = a->log_total;
  for (size_t i = 0; i < n; ++i) {
    if (p[i] == '\r') continue;
    a->log[t++ & (kLog - 1)] = p[i];
  }
  uint64_t from = a->log_total;
  __atomic_store_n(&a->log_total, t, __ATOMIC_RELEASE);   // (the black box reads it from core 0: the bytes first)
  if (t - from >= kLog) clean(a->log, kLog);
  else {
    uint32_t s = (uint32_t)(from & (kLog - 1)), e = (uint32_t)(t & (kLog - 1));
    if (s <= e) clean(a->log + s, e - s);
    else clean(a->log + s, kLog - s), clean(a->log, e);
  }
  clean(&a->log_total, sizeof a->log_total);
}

// ---- the report (the next boot)
inline const char* sec_name(unsigned core, uint32_t s) {
  if (core == 0) switch (s) {
      case 0: return "the loop (outside a timed section)";
      case 1: return "ReadPower (VC mailbox)";
      case 2: return "CPUThrottle::Update (VC mailbox)";
      case 3: return "the temperature (VC mailbox)";
      case 4: return "the ARM clock read (VC mailbox)";
      case 6: return "the gadget's throttled flags (VC mailbox)";
      case 10: return "USB plug-and-play";
      case 11: return "the USB gadget's Poll";
      case 12: return "the Sidecar's Poll";
      case 13: return "MIDI (UpdateMIDI)";
      case 14: return "the transport's Poll";
      case 15: return "the console";
      case 16: return "the card service (cardq: the control core's card requests)";
      case 17: return "the recorder (rec_pi::Poll)";
      case 19: return "storage (stor_pi::Poll)";
      case 20: return "the sampler (samp_poll)";
      case 21: return "a project bundle's step (bundle_pi::poll)";
      case 22: return "the sample library's step (lib_pi::poll)";
      case 23: return "the display (pages and Update)";
      case 24: return "the stats line (LogStats)";
      case 40: return "the network's tasks (MsSleep: the web server, the TCP links)";
      case 41: return "writing /.rytm/prevboot.txt (the breadcrumb's copy on the card)";
      case 42: return "writing /.rytm/blackbox.log (the black box, blackbox_pi.cpp)";
      default: return "?";
    }
  if (core == 3) switch (s) {
      case 100: return "MIDI in (the Rytm's bytes)";
      case 101: return "keys in";
      case 102: return "console commands";
      case 103: return "the tick (bridge::tick: the host link's work, the drone)";
      case 104: return "rendering blocks (waits for cores 1-2)";
      case 105: return "idle_prepare / idle_save / idle_stream (the store, prjslot, kits, posted writes)";
      case 106: return "waiting (wfe)";
      case 0: return "-";
      default: return "?";
    }
  return s == 200 ? "rendering a block" : s == 201 ? "waiting for a block" : s == 0 ? "-" : "?";
}

// lines (no '\n') to put(line, ctx): the summary, then the old log's last tail_bytes as "prev| " lines
template <typename Put>
void Report(Put put, uint32_t pm_rsts, unsigned tail_bytes) {
  char l[400];
  if (!had_prev()) {
    CRUMB_FMT(l, sizeof l, "crumb: no breadcrumb from a previous boot (a power-on, or its RAM not kept); PM_RSTS 0x%08lx",
             (unsigned long)pm_rsts);
    put(l);
    return;
  }
  const Area& p = prev();
  uint64_t f = p.freq ? p.freq / 1000 : 54000;   // ticks a ms
  if (f < 1) f = 1;
  // the end: the latest time anything was recorded (core 3 sleeps in wfe without a stream: core 0's section, file or
  // web time may be its last sign of life)
  uint64_t end = 0;
  auto late = [&](uint64_t t) { end = t > end ? t : end; };
  for (const CoreRec& c : p.core) late(c.beat), late(c.sec_t);
  late(p.disk.t), late(p.fs.t), late(p.web.t), late(p.fault.t);
  auto ago = [&](uint64_t t) { return (unsigned long)(t && t < end ? (end - t) / f : 0); };   // (clamped: never wraps)
  CRUMB_FMT(l, sizeof l, "crumb: the previous boot (#%lu) ended at uptime %lu ms (the last thing recorded); PM_RSTS 0x%08lx",
           (unsigned long)p.boot, (unsigned long)(end / f), (unsigned long)pm_rsts);
  put(l);
  for (unsigned c = 0; c < 4; ++c) {
    const CoreRec& r = p.core[c];
    if (!r.beats && !r.sec_t) {
      CRUMB_FMT(l, sizeof l, "crumb: core %u: no heartbeat recorded", c);
      put(l);
      continue;
    }
    char hb[64];
    if (r.beats) CRUMB_FMT(hb, sizeof hb, "last heartbeat %lu ms before the end (%lu beats)", ago(r.beat), (unsigned long)r.beats);
    else CRUMB_FMT(hb, sizeof hb, "no heartbeat");
    CRUMB_FMT(l, sizeof l, "crumb: core %u: %s; in section %lu - %s - for %lu ms", c, hb, (unsigned long)r.sec,
              sec_name(c, r.sec), ago(r.sec_t));
    put(l);
  }
  CRUMB_FMT(l, sizeof l, "crumb: last disk transfer: drive %lu sector %lu count %lu, %s, %lu ms before the end (%lu transfers)",
           (unsigned long)p.disk.drv, (unsigned long)p.disk.sector, (unsigned long)p.disk.count,
           p.disk.busy == 1 ? "a READ STILL IN FLIGHT" : p.disk.busy == 2 ? "a WRITE STILL IN FLIGHT" : "done",
           ago(p.disk.t), (unsigned long)p.disk.ops);
  put(l);
  CRUMB_FMT(l, sizeof l, "crumb: last file call: %s %s, %lu ms before the end (%lu calls)", p.fs.op[0] ? p.fs.op : "-",
           p.fs.path, ago(p.fs.t), (unsigned long)p.fs.n);
  put(l);
  if (p.web.busy)
    CRUMB_FMT(l, sizeof l, "crumb: web: %lu requests STILL RUNNING, the oldest %s (begun %lu ms before the end), the newest %s (%lu requests)",
              (unsigned long)p.web.busy, p.web.path, ago(p.web.t), p.web.last, (unsigned long)p.web.n);
  else
    CRUMB_FMT(l, sizeof l, "crumb: web: none running, the last %s (%lu requests)", p.web.last[0] ? p.web.last : "-",
              (unsigned long)p.web.n);
  put(l);
  if (p.fault.why[0] || p.fault.n) {
    CRUMB_FMT(l, sizeof l, "crumb: %s (faults %lu: core %lu ESR 0x%08lx ELR 0x%lx FAR 0x%lx), %lu ms before the end", p.fault.why,
             (unsigned long)p.fault.n, (unsigned long)p.fault.core, (unsigned long)p.fault.esr, (unsigned long)p.fault.elr,
             (unsigned long)p.fault.far, ago(p.fault.t));
    put(l);
  } else {
    put("crumb: no fault and no planned reboot recorded (a hang, then the watchdog - or a halt without a fault)");
  }
  if (!tail_bytes) return;
  // the old log's last tail_bytes, from a line start
  uint64_t tot = p.log_total, held = tot < kLog ? tot : kLog;
  uint64_t from = tot - (tail_bytes < held ? tail_bytes : held);
  while (from < tot && from > tot - held && p.log[(from - 1) & (kLog - 1)] != '\n') ++from;
  put("crumb: the previous boot's last log lines:");
  int k = 0;
  char line[300];
  for (uint64_t i = from; i < tot; ++i) {
    char c = p.log[i & (kLog - 1)];
    if (c == '\n' || k == (int)sizeof line - 1) {
      line[k] = 0;
      CRUMB_FMT(l, sizeof l, "prev| %s", line);
      put(l);
      k = 0;
      if (c == '\n') continue;
    }
    line[k++] = c;
  }
  if (k) line[k] = 0, CRUMB_FMT(l, sizeof l, "prev| %s", line), put(l);
}

// the previous boot ended with a planned reboot (StopAndReboot: an update, the web page's reboot) - not a crash
inline bool PrevPlanned() {
  return had_prev() && !strncmp(prev().fault.why, "a planned reboot", 16);
}
// the previous boot's core 0 died in section sec
inline bool PrevDiedIn(uint32_t sec) {
  return had_prev() && prev().core[0].sec == sec;
}

// the previous boot's whole log tail (oldest first) into out: its length
inline uint32_t PrevLog(char* out, uint32_t cap) {
  if (!had_prev()) return 0;
  const Area& p = prev();
  uint64_t tot = p.log_total, held = tot < kLog ? tot : kLog;
  uint32_t n = 0;
  for (uint64_t i = tot - held; i < tot && n < cap; ++i) out[n++] = p.log[i & (kLog - 1)];
  return n;
}

// mbtrace::Scope's companion: the section a core is in, for a block
class Section {
 public:
  explicit Section(uint32_t sec) : was_(Enter(sec)) {}
  Section(const Section&) = delete;
  Section& operator=(const Section&) = delete;
  ~Section() { Leave(was_); }
 private:
  uint32_t was_;
};

}  // namespace crumb
