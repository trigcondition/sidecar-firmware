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
#include <sys/mman.h>

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
const um::Exec kExec = {alloc, reset, write_begin, sync, nullptr, kArena, nullptr, data_alloc};
}  // namespace

namespace core {
const um::Exec* host_um_exec() { return &kExec; }
}  // namespace core
