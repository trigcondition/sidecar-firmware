// core/tests/test_pool_state.cpp - the pool's state (core/pool/pool.h): sizes as the Teensy had them, an open
// slot file forgotten.
#include "check.h"
#include "pool/pool.h"

int main() {
  using namespace core;
  CHECK(kPool == 127 && kPath == 160 && sizeof(PoolEntry) == 4 + card::kName + kPath);
  CHECK(prj().npool == 0 && prj().key[0] == 0 && !card_ok);
  pool_open[3] = true;
  pool_forget(3);
  CHECK(!pool_open[3]);
  printf("test_pool_state: PASS\n");
  return 0;
}
