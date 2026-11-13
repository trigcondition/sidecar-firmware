// core/tests/test_diag.cpp - the diagnostics for a Rytm freeze (an xy-freeze report's proposals 1 and 3):
//   - the freeze probe (0x4E, rytm-mods 0026 fp_check, every ~5 s while the Rytm's UI task is stuck): the first probe of
//     a freeze logged once with the uptime; the recent log copied to the card once a boot (hooks.log_snapshot, asked on
//     the first probe of the first freeze); a probe after 15 s without one starts a new freeze (logged, no copy);
//   - the xy console's set / clear / hold logged as they came (pos / up quiet: every 30 ms while a finger drags).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "check.h"
#include "engine.h"
#include "mod/xypad.h"

using namespace core;

static std::vector<std::string> g_log;
static int g_snaps = 0;
static std::string g_snap_name;
static uint64_t g_us = 1000000;

static void probe(int secs) {
  std::vector<uint8_t> m = {0xF0, 0x7D, 0x52, 0x54, 0x4E, 0x01, 1, (uint8_t)secs};
  m.resize(53, 0);
  m.push_back(0xF7);   // 54 bytes: 9 words of 5 bytes from d[8]
  engine_sysex(m.data(), (int)m.size());
}
static int lines_with(const char* s) {
  int n = 0;
  for (auto& l : g_log) n += l.find(s) != std::string::npos;
  return n;
}
static void at_s(uint64_t s) { g_us = s * 1000000, host_fake_time(true, g_us); }

int main() {
  char tmpl[] = "/tmp/core_diag_XXXXXX";
  CHECK(mkdtemp(tmpl));
  CHECK(system((std::string("mkdir -p '") + tmpl + "/Samples'").c_str()) == 0);
  host_set_card(tmpl);
  hooks.to_rytm = [](const uint8_t*, int) {};
  hooks.log_snapshot = [](const char* name) { ++g_snaps, g_snap_name = name; };
  host_set_log_sink([](const char* l, int n) { g_log.emplace_back(l, (size_t)n); });
  host_fake_time(true, g_us);
  CHECK(engine_begin());

  // ---- the freeze probe
  g_log.clear();
  at_s(3723);   // 01:02:03
  probe(2);
  CHECK(lines_with("FREEZE PROBE: the Rytm's UI task running") == 1);
  CHECK(lines_with("FREEZE PROBE: the first of this freeze at uptime 01:02:03 (stuck 2 s then); the log goes to the card") == 1);
  CHECK(g_snaps == 1 && g_snap_name == "freeze-3723");
  for (int i = 1; i <= 30; ++i) at_s(3723 + 5 * i), probe(i * 5 + 2 > 127 ? 127 : i * 5 + 2);   // the same freeze, 150 s
  CHECK(lines_with("FREEZE PROBE: the first of") == 1 && g_snaps == 1 && lines_with("FREEZE PROBE: the Rytm's") == 31);
  at_s(3723 + 150 + 60);   // a minute without a probe: another freeze
  probe(2);
  CHECK(lines_with("FREEZE PROBE: the first of this freeze at uptime 01:05:33 (stuck 2 s then)") == 1);
  CHECK(lines_with("the log goes to the card") == 1 && g_snaps == 1);   // (once a boot)
  printf("diag: the freeze probe - its first logged with the time, the log to the card once a boot\n");

  // ---- the xy console: set / clear / hold logged, pos / up not
  g_log.clear();
  char o[200];
  CHECK(xypad::command("xy=set:x:1:V1:P1:-50", o, sizeof o) && strstr(o, "X1 V1:P1 -50"));
  CHECK(xypad::command("xy=set:y:2:V1:P2:7", o, sizeof o) && strstr(o, "Y2 V1:P2 +7"));
  CHECK(xypad::command("xy=hold:1", o, sizeof o));
  CHECK(xypad::command("xy=pos:100:200", o, sizeof o) && xypad::command("xy=up", o, sizeof o));
  CHECK(xypad::command("xy=clear:x:1", o, sizeof o));
  CHECK(xypad::command("xy=hold:2", o, sizeof o) && strstr(o, "cannot"));
  CHECK(lines_with("xy: set:x:1:V1:P1:-50") == 1 && lines_with("xy: set:y:2:V1:P2:7") == 1 && lines_with("xy: hold:1") == 1);
  CHECK(lines_with("xy: clear:x:1") == 1 && lines_with("xy: hold:2 (refused)") == 1);
  CHECK(lines_with("xy: pos") == 0 && lines_with("xy: up") == 0);
  printf("diag: the xy console's set / clear / hold logged (pos / up quiet)\n");

  // ---- no plus flag in a format: the Pi's printf (pi/engine/platform_pi.cpp core_vsnprintf, Circle's CString) prints
  // "%+d" as it is; the sign is written by hand (fx/insert.cpp eq_value_text). The core's and the Pi engine's sources.
  {
    FILE* p = popen("grep -rnE '(printf|CORE_STR|Write).*\"[^\"]*%[-0 #]*\\+' --include='*.cpp' --include='*.h' --include='*.c' "
                    ". ../pi/engine 2>/dev/null | grep -v '^\\./tests/' | grep -v '^\\./build/' | grep -v '^\\.\\./pi/engine/tests/'", "r");
    CHECK(p);
    std::string hits;
    char b[512];
    while (fgets(b, sizeof b, p)) hits += b;
    pclose(p);
    if (!hits.empty()) fprintf(stderr, "a plus flag in a format:\n%s", hits.c_str());
    CHECK(hits.empty());
  }
  printf("diag: no %%+ in the core's and the Pi engine's formats\n");

  host_set_log_sink(nullptr);
  CHECK(system((std::string("rm -rf '") + tmpl + "'").c_str()) == 0);
  printf("test_diag: PASS\n");
  return 0;
}
