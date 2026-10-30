// core/bundle/projects.h - the project index (bundles design 6.1, docs/BUNDLES.md "The project index"):
// /.rytm/projects.txt, one line per project key: key<TAB>rytm name<TAB>last-used counter<TAB>last date or -<TAB>flags
// ("-" none; "i": imported from a bundle, not yet named by the Rytm). It gives the UIs the Rytm's own name, a "last
// used" order without a clock, and the imported flag. Pi and Mac only (CORE_MSAMP).
//
// Cores: note() (the Rytm's 10, set_project), save() (idle_save) and load() run on the control core; the file is
// written by the card queue (one cardq kWriteReq, back-to-back changes coalesced into one later write: no card time on
// the control core with a card core). The bundle job (core 0) changes the index only through mark_imported() /
// forget(), which leave a request the control core applies at its next save(); it reads it through copy() (a seqlock:
// never a torn line). The wall clock (set_clock: the web page's POST /time) is any core's.
#pragma once
#include <stdint.h>

#include "../project_key.h"

namespace core {
namespace projects {

constexpr int kMax = 256;       // projects listed at most (the oldest by counter dropped past it)
constexpr int kRytm = 24;       // the Rytm's name (the 10's, at most 23 characters)
constexpr int kDate = 21;       // "2026-10-22T14:03:00Z"
struct Info {
  char key[kKeyMax];
  char rytm[kRytm];
  uint32_t used;                // the last-used counter (0: never named by the Rytm here)
  char date[kDate];             // "" unknown
  bool imported;
};

#if CORE_MSAMP
void load();                    // boot (project_load): the file into RAM
// the Rytm named a project (10): its counter to the top, the date, "i" cleared. 2026-10-27: rytm, when given, is the
// Rytm's name for the project and the first argument its key (a slot key, "@005": core/bundle/prjslot.h)
void note(const char* rytm_name, const char* rytm = nullptr);
// the control core's (core/bundle/prjslot.h): an entry's key changed (its files moved: a migration); false: no entry
bool rekey(const char* from, const char* to);
const Info* at(int i);          // the control core's: entry i (0..count()-1)
void drop(const char* key);     // the control core's: the entry gone (its files went to the project trash)
bool save();                    // idle_save's step: true while a write is pending (posted, or formatted and waiting)
// core 0 (the bundle job): applied by the control core's next save(); false: a request of the kind still waits
bool mark_imported(const char* key, const char* rytm_name);
bool forget(const char* key);
bool requests_idle();           // the bundle job's requests applied
int count();
const Info* find(const char* key);   // the control core's (tests)
int copy(Info* out, int max);   // any core: a consistent copy of the index (seqlock), the entries copied (-1: it kept
                                // changing: unknown)

// The wall clock: from the web page (POST /time), NTP or an RTC; seconds since 1970 (UTC)
enum Clock { kClockNone = 0, kClockBrowser, kClockNtp, kClockRtc };
void set_clock(uint32_t epoch, int source);
bool clock_now(uint32_t* epoch, int* source);   // false: unknown
const char* clock_name(int source);             // "none" "browser" "ntp" "rtc"
// the local time zone (the browser's, POST /time tz=: minutes east of UTC, -840..840; 0 until set): the take folders'
// names and the files' dates (FatFs) are local time; clock_local: clock_now with the zone applied
void set_clock_zone(int minutes);
int clock_zone();
bool clock_local(uint32_t* local, int* source);
// epoch -> "YYYY-MM-DDTHH:MM:SSZ" (kDate bytes) and "YYYYMMDD-HHMM" (14 bytes)
void iso_date(uint32_t epoch, char* out, int cap);
void stamp(uint32_t epoch, char* out, int cap);
void reset_for_tests();
#endif

}  // namespace projects
}  // namespace core
