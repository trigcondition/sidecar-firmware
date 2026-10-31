// pi/engine/tests/test_blackbox.cpp - the black box's take (blackbox.h) on the crumb's log ring: what was logged comes
// out once, in order, across the ring's wrap; a lag past the ring skips to its oldest trusted byte (kMargin past the
// oldest) and counts what was lost;
// max caps a take; due's timing (2 s, a full chunk, the card busy).
#define CRUMB_HOST 1
#include <string>

#include "blackbox.h"
#include "check.h"

static void logs(const std::string& s) { crumb::Log(s.data(), s.size()); }
static std::string take(uint64_t* done, uint64_t* lost, unsigned max = 1u << 20) {
  static char buf[1 << 20];
  unsigned n = blackbox::take(crumb::area(), done, buf, max, lost);
  return std::string(buf, n);
}

int main() {
  crumb::Begin();
  uint64_t done = 0, lost = 0;
  CHECK(take(&done, &lost).empty() && done == 0);
  logs("one\n");
  logs("two\n");
  CHECK(take(&done, &lost) == "one\ntwo\n" && lost == 0);
  CHECK(take(&done, &lost).empty());   // once
  // max caps a take; the rest comes next
  logs("abcdefgh");
  CHECK(take(&done, &lost, 3) == "abc" && take(&done, &lost) == "defgh");
  // across the wrap: everything, in order
  std::string all;
  for (int i = 0; i < 6000; ++i) {
    std::string l = "line " + std::to_string(i) + "\n";
    logs(l);
    all += l;
    if (i % 100 == 99) {
      std::string got = take(&done, &lost);
      CHECK(all == got);
      all.clear();
    }
  }
  CHECK(lost == 0 && crumb::area()->log_total > crumb::kLog);
  // a lag past the ring: the oldest kLog bytes come out, the rest counted lost
  std::string big(crumb::kLog + 1000, 'x');
  big.back() = 'E';
  logs(big);
  std::string got = take(&done, &lost);
  CHECK(got.size() == crumb::kLog - blackbox::kMargin && got.back() == 'E' && lost == 1000 + blackbox::kMargin);
  CHECK(done == crumb::area()->log_total);
  // \r is dropped by the ring
  logs("a\r\nb\n");
  CHECK(take(&done, &lost) == "a\nb\n");
  // never ahead of the ring
  uint64_t ahead = crumb::area()->log_total + 50;
  CHECK(take(&ahead, &lost).empty() && ahead == crumb::area()->log_total);

  CHECK(!blackbox::due(0, 99999, false));
  CHECK(!blackbox::due(10, 1999, false) && blackbox::due(10, 2000, false));
  CHECK(blackbox::due(blackbox::kChunk, 0, false));
  CHECK(!blackbox::due(10, 9999, true) && blackbox::due(10, 10000, true));
  CHECK(blackbox::due(crumb::kLog / 2, 0, true));
  printf("test_blackbox: ok\n");
  return 0;
}
