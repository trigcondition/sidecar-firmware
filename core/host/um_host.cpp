// core/host/um_host.cpp - the user machines' executable memory on the Mac (arm64): one MAP_JIT region, a bump
// allocator of 4096-aligned blocks, the per-thread write toggle, sys_icache_invalidate. The same RMPA images the Pi
// runs (sdk/rmp/plugin.mk `make a64`) run here, so the core lib (sim/um_link.py, core/tests/test_um.cpp) plays them.
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
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
void write_begin() { pthread_jit_write_protect_np(0); }
void sync(void* p, uint32_t n) {
  pthread_jit_write_protect_np(1);
  sys_icache_invalidate(p, n);
}
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
  t_pc = (uint64_t)((ucontext_t*)uc)->uc_mcontext->__ss.__pc;
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
    pthread_jit_write_protect_np(1);   // (a fault while a write window was open: closed again)
    *pc = t_pc;
    return 1;
  }
  t_jb = &jb;
  fn(ctx);
  t_jb = was;
  return 0;
}
const um::Exec kExec = {alloc, reset, write_begin, sync, nullptr, kArena, nullptr, data_alloc, arena, guard, 0};
}  // namespace

namespace core {
const um::Exec* host_um_exec() { return &kExec; }
}  // namespace core
