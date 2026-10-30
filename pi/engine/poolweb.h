// pi/engine/poolweb.h - the web page's Pool tab (www/index.html) over core/lib/pools.h: one GET for a view of one bank
// of one POOL list, so a poll is one small request.
//
//   GET /pool?l=smp|wt|inst&b=0..7|A..H[&u=1]
//     {"st":<pools::status_json>,"ram":{"budget","used","tables","entries","pct"},"bank":<pools::bank_json>|null
//      [,"unused":[codes of the whole list],"warn":<pools::kUnusedWarning>]}   (u=1: SELECT UNUSED's rows)
//   "bank" rows: [row, type, name, state, path, used_count, users, size, members] (pools.cpp bank_json); "ram" zeros
//   until the shared sample cache fills it (the page shows "-").
//
// Read only: web.cpp runs it in its tasks on core 0 while the lists are the control core's (core 3). Everything it
// calls only reads the lists (racy, for display: pools.h's at() rule); the edits are the console's pool=... lines,
// queued to the control core through /cmd. Portable: host-tested in tests/test_poolweb.cpp.
#ifndef _pi_engine_poolweb_h
#define _pi_engine_poolweb_h

namespace poolweb {

// params: the query string ("l=smp&b=2"); the JSON into out; returns its length (0: cap too small)
int json(const char* params, char* out, int cap);

}  // namespace poolweb

#endif
