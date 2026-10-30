// core/pool/stream_budget.cpp - see stream_budget.h.
#include "pool/stream_budget.h"

#include <string.h>

#include "platform.h"

namespace core {
namespace sbudget {

namespace {

Bench g_meas[kVols];
int g_sd_voices = 0;
int g_forced = 0;
const char* const kNames[kVols] = {"SD", "USB", "USB2", "USB3", "NVME"};
// the defaults for a volume never measured [inferred: docs/STORAGE.md's expectations, cautious end]
const Bench kDefault[kVols] = {{12.0f, 4.0f, false}, {20.0f, 6.0f, false}, {20.0f, 6.0f, false},
                               {80.0f, 3.0f, false}, {250.0f, 1.0f, false}};

int num(const char** p) {
  int v = 0;
  bool any = false;
  while (**p >= '0' && **p <= '9') v = v * 10 + (*(*p)++ - '0'), any = true;
  return any ? v : -1;
}

}  // namespace

Bench bench(int v) {
  if (v < 0 || v >= kVols) v = 0;
  if (g_meas[v].measured) return g_meas[v];
  return kDefault[v];
}
void set_bench(int v, float read_mbps, float p99_ms) {
  if (v < 0 || v >= kVols || read_mbps <= 0 || p99_ms <= 0) return;
  g_meas[v] = Bench{read_mbps, p99_ms, true};
}
void set_sd_voices(int voices) { g_sd_voices = voices > 0 ? voices : 0; }
void clear() {
  for (Bench& b : g_meas) b = Bench{0, 0, false};
  g_sd_voices = 0, g_forced = 0;
}

int streams_for(const Bench& b, uint32_t rate, int ch, int bits) {
  if (b.read_mbps <= 0 || b.p99_ms <= 0 || !rate || ch < 1 || bits < 8) return 0;
  float bps = (float)rate * (float)(ch > 2 ? 2 : ch) * (float)((bits + 7) / 8);   // bytes a second a stream reads
  float reads = bps / (float)kPiece;                                             // reads a second
  float t = b.p99_ms * 0.001f + (float)kPiece / (b.read_mbps * 1048576.0f);       // seconds a read
  float n_lat = kShare / (reads * t), n_tp = kShare * b.read_mbps * 1048576.0f / bps;
  float n = n_lat < n_tp ? n_lat : n_tp;
  return n < 0 ? 0 : n > 999 ? 999 : (int)n;
}

int streams(int v, uint32_t rate, int ch, int bits) {
  if (v < 0 || v >= kVols) v = 0;
  if (!g_meas[v].measured && v == 0 && g_sd_voices > 0) {   // the SD card test: its stereo 24-bit 48 kHz voices
    float ref = 48000.0f * 2 * 3, bps = (float)rate * (float)(ch > 2 ? 2 : ch) * (float)((bits + 7) / 8);
    float n = bps > 0 ? g_sd_voices * ref / bps : 0;
    return n > 999 ? 999 : (int)n;
  }
  return streams_for(bench(v), rate, ch, bits);
}

int vol_of(const char* p) {
  if (!p) return 0;
  for (int v = 1; v < kVols; ++v) {
    size_t n = strlen(kNames[v]);
    if (!strncmp(p, kNames[v], n) && p[n] == ':') return v;
  }
  return 0;
}

bool fits(const char* path, uint32_t rate, int ch, int bits) { return streams(vol_of(path), rate, ch, bits) >= kWantStreams; }

void forced(int d) {
  g_forced += d;
  if (g_forced < 0) g_forced = 0;
}
int forced_now() { return g_forced; }

bool command(const char* p, char* o, int cap) {
  if (strncmp(p, "stream", 6) != 0 || (p[6] && p[6] != '=')) return false;
  if (p[6] && !strncmp(p + 7, "bench:", 6)) {   // stream=bench:VOL:MBPS:P99MS (whole numbers)
    const char* q = p + 13;
    int v = -1;
    for (int k = 0; k < kVols; ++k) {
      size_t n = strlen(kNames[k]);
      if (!strncmp(q, kNames[k], n) && q[n] == ':') v = k, q += n + 1;
    }
    int mb = num(&q);
    if (*q == ':') ++q;
    int ms = num(&q);
    if (v < 0 || mb <= 0 || ms <= 0) {
      sniprintf(o, cap, "stream=bench:SD|USB|USB2|USB3|NVME:MBPS:P99MS");
      return true;
    }
    set_bench(v, (float)mb, (float)ms);
  }
  int n = sniprintf(o, cap, "stream: budget (24-bit 48 kHz stereo / mono streams, the margin kept):");
  for (int v = 0; v < kVols && n > 0 && n < cap; ++v) {
    Bench b = bench(v);
    int k = sniprintf(o + n, cap - n, CORE_STR(" %s %d/%d (%s)"), kNames[v], streams(v, 48000, 2, 24), streams(v, 48000, 1, 24),
                      g_meas[v].measured ? "bench" : v == 0 && g_sd_voices ? "sdtest" : "default");
    (void)b;
    if (k > 0) n += k;
  }
  if (n > 0 && n < cap) sniprintf(o + n, cap - n, CORE_STR("; %d instrument%s in RAM for the budget"), g_forced, g_forced == 1 ? "" : "s");
  return true;
}

int json(char* o, int cap) {
  int n = sniprintf(o, cap, "\"stream\":{\"vols\":[");
  for (int v = 0; v < kVols && n > 0 && n < cap; ++v) {
    int k = sniprintf(o + n, cap - n, CORE_STR("%s{\"vol\":\"%s\",\"stereo\":%d,\"mono\":%d,\"src\":\"%s\"}"), v ? "," : "",
                      kNames[v], streams(v, 48000, 2, 24), streams(v, 48000, 1, 24),
                      g_meas[v].measured ? "bench" : v == 0 && g_sd_voices ? "sdtest" : "default");
    if (k < 0 || k >= cap - n) return cap;
    n += k;
  }
  int k = sniprintf(o + n, n < cap ? cap - n : 0, CORE_STR("],\"forced\":%d,\"want\":%d}"), g_forced, kWantStreams);
  return n + (k > 0 ? k : 0);
}

}  // namespace sbudget
}  // namespace core
