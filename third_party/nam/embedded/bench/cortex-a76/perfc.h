/* User-space cycle/instruction counters via perf_event_open (no perf tool needed). */
#pragma once
#include <linux/perf_event.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
static int perfc_open(uint64_t cfg, int group) {
  struct perf_event_attr a; memset(&a, 0, sizeof a);
  a.type = PERF_TYPE_HARDWARE; a.size = sizeof a; a.config = cfg;
  a.disabled = group < 0; a.exclude_kernel = 1; a.exclude_hv = 1;
  return (int)syscall(__NR_perf_event_open, &a, 0, -1, group, 0);
}
typedef struct { int cyc, ins; } perfc_t;
static perfc_t perfc_init(void) { perfc_t p; p.cyc = perfc_open(PERF_COUNT_HW_CPU_CYCLES, -1); p.ins = perfc_open(PERF_COUNT_HW_INSTRUCTIONS, p.cyc); return p; }
static void perfc_start(perfc_t p) { ioctl(p.cyc, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP); ioctl(p.cyc, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP); }
static void perfc_stop(perfc_t p, uint64_t* cyc, uint64_t* ins) { ioctl(p.cyc, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP); if (read(p.cyc, cyc, 8) != 8) *cyc = 0; if (read(p.ins, ins, 8) != 8) *ins = 0; }
