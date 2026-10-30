// core/tests/pool_util.h - the machine tests' way to fill the project's POOL lists (core/lib/pools.h): a row set the way
// the touchscreen / console set it (forced: no P3 prompt), then the control core's steps until the machines composed it.
#pragma once
#include <cstring>

#include "check.h"
#include "lib/pools.h"
#include "msamp/msamp.h"
#include "nmach.h"

namespace pu {

inline void steps(int n = 20000) {
  for (int i = 0; i < n; ++i) {
    bool a = core::nm::step(), b = core::msamp::step();
    if (!a && !b && core::pools::ready() && !core::pools::pending() && !core::pools::filling()) break;
  }
}
inline void ready() {
  for (int i = 0; i < 20000 && !core::pools::ready(); ++i) core::nm::step();
  CHECK(core::pools::ready());
}
inline int code(const char* row) {
  int c = -1;
  CHECK(core::pools::parse_row(row, &c));
  return c;
}
// path: a card file, or "@IN" / "@PAD" (an input / built-in by name); "" unloads the row
inline int put(int list, const char* row, const char* path) {
  ready();
  int c = code(row), out = -1, r;
  if (!path || !path[0]) {
    r = core::pools::unload(list, c, core::pools::kForced);
  } else if (path[0] == '@') {
    int in = core::pools::input_of(path + 1), bi = core::pools::builtin_of(list, path + 1);
    r = core::pools::set_special(list, c, in >= 0 && list == core::pools::kSamples ? core::pools::kInput : core::pools::kBuiltin,
                                 in >= 0 && list == core::pools::kSamples ? in : bi, core::pools::kForced);
  } else {
    r = core::pools::set_file(list, c, path, &out, core::pools::kForced);
  }
  for (int i = 0; i < 2000 && core::pools::pending(); ++i) core::pools::step();
  if (r == core::pools::kPending) r = core::pools::last_result(&out);
  steps();
  return r;
}
inline void clear(int list) {   // every row of the list emptied
  ready();
  for (int c = 0; c < core::pools::kCodes; ++c)
    if (core::pools::row_of(c) && core::pools::at(list, c).type != core::pools::kEmpty) core::pools::unload(list, c, core::pools::kForced);
  steps();
}

}  // namespace pu
