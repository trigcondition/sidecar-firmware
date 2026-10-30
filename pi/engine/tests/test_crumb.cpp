// pi/engine/tests/test_crumb.cpp - the crash breadcrumb (crumb.h) on the Mac: nothing from a power-on; what each core
// was doing, the disk transfer in flight, the last file call and web request and the log's tail survive "a reboot"
// (Begin again on the same memory) and are reported; a fault and a planned reboot's reason are kept; the log ring
// wraps and PrevLog hands over its whole tail oldest first; a broken area (its end word overwritten) reports nothing.
#define CRUMB_HOST 1
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "crumb.h"

static std::vector<std::string> report() {
  std::vector<std::string> v;
  crumb::Report([&](const char* l) { v.push_back(l); }, 0x1000, 6144);
  return v;
}
static bool has(const std::vector<std::string>& v, const char* s) {
  for (auto& l : v)
    if (l.find(s) != std::string::npos) return true;
  return false;
}
static void logl(const char* s) { crumb::Log(s, strlen(s)); }

int main() {
  // a power-on: no area yet
  crumb::Begin();
  auto r = report();
  CHECK(r.size() == 1 && has(r, "no breadcrumb from a previous boot"));
  CHECK(crumb::area()->boot == 1);

  // the boot that hangs: core 0 in the card service, an SD read in flight, the last file a kit record
  uint64_t& t = crumb::host_clock();
  t = 1000000;   // 1 s (the host clock: 1 MHz)
  logl("00:00:52.28 core: kits: kit 007 reloaded (its saved copy)\r\n");
  logl("00:00:53.28 engine: engine: fifo min 78\n");
  crumb::host_core() = 3;
  crumb::Beat(3);
  crumb::Enter(105);
  crumb::host_core() = 1;
  crumb::Beat(1);
  crumb::host_core() = 0;
  crumb::Beat(0);
  {
    crumb::Section s(16);
    CHECK(crumb::area()->core[0].sec == 16);
    crumb::Web("/status", true);
    crumb::Web(nullptr, false);
    crumb::Fs("open r", "SD:/.rytm/kits/@047/k007.sks");
    crumb::DiskBegin(0, 123456, 8, false);
    t = 21000000;   // core 3 runs on 20 s (the watchdog's time), core 0 never comes back
    crumb::host_core() = 3;
    crumb::Beat(3);
    crumb::host_core() = 0;
    crumb::Begin();   // "the reboot": the section is never left
  }
  CHECK(crumb::had_prev() && crumb::area()->boot == 2 && crumb::area()->log_total == 0);
  r = report();
  for (auto& l : r) printf("  %s\n", l.c_str());
  CHECK(has(r, "the previous boot (#1) ended at uptime 21000 ms"));
  CHECK(has(r, "core 0: last heartbeat 20000 ms before the end (1 beats); in section 16 - the card service"));
  CHECK(has(r, "core 3: last heartbeat 0 ms before the end (2 beats); in section 105 - idle_prepare"));
  CHECK(has(r, "core 1: last heartbeat 20000 ms") && has(r, "core 2: no heartbeat recorded"));
  CHECK(has(r, "drive 0 sector 123456 count 8, a READ STILL IN FLIGHT, 20000 ms before the end (1 transfers)"));
  CHECK(has(r, "last file call: c0:open r SD:/.rytm/kits/@047/k007.sks"));
  CHECK(has(r, "web: none running, the last /status (1 requests)"));
  CHECK(!crumb::PrevPlanned() && crumb::PrevDiedIn(16) && !crumb::PrevDiedIn(41));
  CHECK(has(r, "no fault and no planned reboot recorded"));
  CHECK(has(r, "prev| 00:00:52.28 core: kits: kit 007 reloaded (its saved copy)") && has(r, "prev| 00:00:53.28 engine"));
  CHECK(!has(r, "\r"));
  printf("crumb: a hang: core 0's section, the read in flight, the last file, the log's last lines reported at the next boot\n");

  // a fault on core 3, then a planned reboot keeps its reason; the next boot says so
  crumb::Fault(3, 0x96000004u, 0x81234, 0xdeadbeef, "an exception outside a voice's render");
  crumb::Fault(3, 0x1, 0x2, 0x3, nullptr);   // (a second one: counted, the first kept)
  crumb::Begin();
  r = report();
  CHECK(has(r, "crumb: an exception outside a voice's render (faults 2: core 3 ESR 0x96000004 ELR 0x81234 FAR 0xdeadbeef)"));
  CHECK(!crumb::PrevPlanned());
  crumb::Why("a planned reboot: the flasher's magic");
  crumb::Begin();
  r = report();
  CHECK(has(r, "crumb: a planned reboot: the flasher's magic") && crumb::area()->boot == 4 && crumb::PrevPlanned());
  printf("crumb: a fault (core, ESR, ELR, FAR) and a planned reboot's reason are kept\n");

  // the log ring wraps: the tail is the newest kLog bytes, oldest first
  std::string all;
  for (int i = 0; all.size() < crumb::kLog * 2 + 100; ++i) {
    char b[64];
    snprintf(b, sizeof b, "line %06d of the log\n", i);
    all += b;
    logl(b);
  }
  crumb::Begin();
  static char out[crumb::kLog];
  uint32_t n = crumb::PrevLog(out, sizeof out);
  CHECK(n == crumb::kLog && !memcmp(out, all.data() + all.size() - crumb::kLog, crumb::kLog));
  r = report();
  CHECK(has(r, all.substr(all.size() - 23, 22).c_str()));   // the last line, whole
  size_t prev_lines = 0;
  for (auto& l : r) prev_lines += l.rfind("prev| line ", 0) == 0;
  CHECK(prev_lines >= 6144 / 23 - 1 && prev_lines <= 6144 / 23 + 1);   // ~6 KB of whole lines (23 bytes each)
  printf("crumb: the log ring wraps; PrevLog hands over its whole tail, the report its last ~6 KB of whole lines\n");

  // M1: core 3 asleep (its last beat early), core 0's section / file / web later: the end is the latest of them all,
  // no difference wraps
  t = 5000000;
  crumb::host_core() = 3;
  crumb::Beat(3);
  crumb::host_core() = 0;
  t = 9000000;
  crumb::Enter(40);
  crumb::Web("/bundle/export", true);   // (an export, its handler yielding:)
  t = 9500000;
  crumb::Web("/status", true);          // a second request meanwhile, answered
  crumb::Web(nullptr, false);
  t = 9800000;
  crumb::Fs("open w", "SD:/.rytm/seq/@047/p000.sqp");
  crumb::Begin();
  r = report();
  for (auto& l : r)
    if (l.rfind("crumb:", 0) == 0) printf("  %s\n", l.c_str());
  CHECK(has(r, "ended at uptime 9800 ms"));
  CHECK(has(r, "core 3: last heartbeat 4800 ms before the end"));
  CHECK(has(r, "core 0: no heartbeat; in section 40 - the network's tasks"));   // (no beat this boot)
  CHECK(has(r, "in section 40 - the network's tasks (MsSleep: the web server, the TCP links) - for 800 ms"));
  CHECK(has(r, "web: 1 requests STILL RUNNING, the oldest /bundle/export (begun 800 ms before the end), the newest /status"));
  CHECK(has(r, "last file call: c0:open w SD:/.rytm/seq/@047/p000.sqp, 0 ms before the end"));
  for (auto& l : r) CHECK(l.find("18446744") == std::string::npos);   // (never a wrapped difference)
  printf("crumb: the end is the latest record of any kind; open web requests counted, the oldest named\n");

  // L4: a damaged area that still has its magic words: strings ended inside their fields, a tick rate of 0..999 no
  // division by zero
  crumb::Fs("open r", "x");
  memset(crumb::area()->fs.path, 'A', sizeof crumb::area()->fs.path);
  memset(crumb::area()->fault.why, 'B', sizeof crumb::area()->fault.why);
  crumb::area()->freq = 7;
  crumb::Begin();
  r = report();
  CHECK(has(r, std::string(sizeof crumb::area()->fs.path - 1, 'A').c_str()) &&
        !has(r, std::string(sizeof crumb::area()->fs.path, 'A').c_str()));
  printf("crumb: damaged strings end inside their fields; a tick rate under 1 kHz does not divide by zero\n");

  // a broken area (its end word overwritten, a RAM not kept whole): nothing from it
  crumb::area()->magic2 = 0;
  crumb::Begin();
  CHECK(!crumb::had_prev() && crumb::area()->boot == 1 && crumb::PrevLog(out, sizeof out) == 0);
  printf("crumb: a broken area reports nothing\n");
  printf("crumb: ALL PASS\n");
  return 0;
}
