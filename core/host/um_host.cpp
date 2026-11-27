// core/host/um_host.cpp - the user machines' executable memory on the Mac (arm64): one MAP_JIT region, a bump
// allocator of 4096-aligned blocks, the per-thread write toggle, sys_icache_invalidate. The same RMPA images the Pi
// runs (sdk/rmp/plugin.mk `make a64`) run here, so the core lib (sim/um_link.py, core/tests/test_um.cpp) plays them.
// On Linux (tools/golden_a64.sh's aarch64 build of the core tests) a plain RWX mapping and the compiler's cache flush.
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#else
#define MAP_JIT 0
#endif
#include <pthread.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#include "user/um.h"
#include "um_host.h"

namespace {
constexpr uint32_t kArena = 4u << 20;
uint8_t* g_base = nullptr;
uint32_t g_used = 0;
uint8_t* g_data = nullptr;   // RW, mapped right after the code (ADRP reaches +-4 GB)
uint32_t g_dused = 0;

uint8_t* alloc(uint32_t bytes) {
  if (!g_base) {
    void* p = mmap(nullptr, kArena, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    g_base = (uint8_t*)p;
  }
  bytes = (bytes + 4095u) & ~4095u;
  if (bytes > kArena - g_used) return nullptr;
  uint8_t* r = g_base + g_used;
  g_used += bytes;
  return r;
}
void reset() { g_used = 0; g_dused = 0; }
uint8_t* data_alloc(uint32_t bytes) {
  if (!g_data) {
    if (!g_base && !alloc(0)) return nullptr;
    void* p = mmap(g_base + kArena, kArena, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    g_data = (uint8_t*)p;
  }
  bytes = (bytes + 4095u) & ~4095u;
  if (bytes > kArena - g_dused) return nullptr;
  uint8_t* r = g_data + g_dused;
  g_dused += bytes;
  return r;
}
#ifdef __APPLE__
void write_begin() { pthread_jit_write_protect_np(0); }
void sync(void* p, uint32_t n) {
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(p, n);
}
#else
void write_begin() {}
void sync(void* p, uint32_t n) { __builtin___clear_cache((char*)p, (char*)p + n); }
#endif
// ---- Plugin API 3 (docs/plugin-api-3-plan.md)
// The slot arenas' pool: 64 MB (the proposal's default for a 2 GB board), mapped once, not from um_heap (the user
// machines' heap keeps the layout it had before API 3: test_api3_compat places it at a fixed address).
constexpr uint32_t kPool = 64u << 20;
uint8_t* arena(uint32_t* bytes) {
  static uint8_t* p = nullptr;
  if (!p) {
    void* m = mmap(nullptr, kPool, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    p = m == MAP_FAILED ? nullptr : (uint8_t*)m;
  }
  *bytes = p ? kPool : 0;
  return p;
}
// The guard: a fault inside a guarded plugin call (SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE on this thread) leaves the
// call through siglongjmp, as the Pi's exception handler leaves it into the control core's loop. Outside a guarded call
// the handler puts the default action back and returns: the instruction faults again and the process dies as before.
thread_local sigjmp_buf* t_jb = nullptr;
thread_local uint64_t t_pc = 0;
const int kSigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
void on_fault(int sig, siginfo_t*, void* uc) {
  if (!t_jb) {
    signal(sig, SIG_DFL);
    return;
  }
#ifdef __APPLE__
  t_pc = (uint64_t)((ucontext_t*)uc)->uc_mcontext->__ss.__pc;
#else
  t_pc = (uint64_t)((ucontext_t*)uc)->uc_mcontext.pc;
#endif
  siglongjmp(*t_jb, 1);
}
int guard(void (*fn)(void*), void* ctx, uint64_t* pc) {
  static bool on = false;
  if (!on) {
    on = true;
    struct sigaction sa = {};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;   // (NODEFER: the signal is not left blocked after the jump)
    sigemptyset(&sa.sa_mask);
    for (int s : kSigs) sigaction(s, &sa, nullptr);
  }
  sigjmp_buf jb;
  sigjmp_buf* was = t_jb;
  if (sigsetjmp(jb, 0)) {
    t_jb = was;
#ifdef __APPLE__
    pthread_jit_write_protect_np(1);   // (a fault while a write window was open: closed again)
#endif
    *pc = t_pc;
    return 1;
  }
  t_jb = &jb;
  fn(ctx);
  t_jb = was;
  return 0;
}
// guard_timed: guard, and a call still running after limit_us is left by SIGUSR2 from a watchdog thread, sent to this
// thread only (pthread_kill), through the same siglongjmp: 2 then, *pc where it was (the Pi's core 3 does this from its
// virtual timer interrupt, pi/engine/guard.cpp). One watchdog thread, started at the first call.
thread_local volatile sig_atomic_t t_timed = 0;   // inside a timed call (the SIGUSR2 is ours)
thread_local volatile sig_atomic_t t_stopped = 0;
pthread_mutex_t g_wd_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_wd_cv = PTHREAD_COND_INITIALIZER;
pthread_t g_wd_target;
bool g_wd_armed = false;
uint64_t g_wd_gen = 0;
timespec g_wd_at;
void on_stop(int, siginfo_t*, void* uc) {
  if (!t_jb || !t_timed) return;   // late (the call returned): nothing to stop
  t_stopped = 1;
#ifdef __APPLE__
  t_pc = (uint64_t)((ucontext_t*)uc)->uc_mcontext->__ss.__pc;
#else
  t_pc = (uint64_t)((ucontext_t*)uc)->uc_mcontext.pc;
#endif
  siglongjmp(*t_jb, 1);
}
void* watchdog(void*) {
  pthread_mutex_lock(&g_wd_mu);
  for (;;) {
    while (!g_wd_armed) pthread_cond_wait(&g_wd_cv, &g_wd_mu);
    const uint64_t gen = g_wd_gen;
    int r = 0;
    while (g_wd_armed && g_wd_gen == gen && r != ETIMEDOUT) r = pthread_cond_timedwait(&g_wd_cv, &g_wd_mu, &g_wd_at);
    if (g_wd_armed && g_wd_gen == gen) pthread_kill(g_wd_target, SIGUSR2), g_wd_armed = false;
  }
  return nullptr;
}
int guard_timed(void (*fn)(void*), void* ctx, uint64_t* pc, uint32_t limit_us) {
  static bool on = false;
  if (!on) {
    on = true;
    struct sigaction sa = {};
    sa.sa_sigaction = on_stop;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, nullptr);
    pthread_t t;
    pthread_create(&t, nullptr, watchdog, nullptr);
    pthread_detach(t);
  }
  timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  const uint64_t ns = (uint64_t)now.tv_nsec + (uint64_t)limit_us * 1000u;
  pthread_mutex_lock(&g_wd_mu);
  g_wd_target = pthread_self(), g_wd_at.tv_sec = now.tv_sec + (time_t)(ns / 1000000000u);
  g_wd_at.tv_nsec = (long)(ns % 1000000000u), g_wd_armed = true, ++g_wd_gen;
  pthread_cond_signal(&g_wd_cv);
  pthread_mutex_unlock(&g_wd_mu);
  t_stopped = 0, t_timed = 1;
  const int r = guard(fn, ctx, pc);
  t_timed = 0;
  pthread_mutex_lock(&g_wd_mu);
  g_wd_armed = false;
  pthread_cond_signal(&g_wd_cv);
  pthread_mutex_unlock(&g_wd_mu);
  return t_stopped ? 2 : r;
}
const um::Exec kExec = {alloc, reset, write_begin, sync, nullptr, kArena, nullptr, data_alloc, arena, guard, 0, guard_timed};
}  // namespace

namespace core {
const um::Exec* host_um_exec() { return &kExec; }
}  // namespace core
