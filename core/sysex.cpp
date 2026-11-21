// core/sysex.cpp - see sysex.h. Cold: per message, never per sample.
#include "sysex.h"

#include <string.h>

#include "platform.h"

namespace core {

static int head(uint8_t* m, uint8_t cmd) {
  m[0] = 0xf0;
  m[1] = 0x7d;
  m[2] = 0x52;
  m[3] = 0x54;
  m[4] = cmd;
  return 5;
}

CORE_COLD void name13(uint8_t* out, const char* name, int len) {
  for (int i = 0; i < 13; ++i) {
    char c = i < len ? name[i] : 0;
    out[i] = !c ? 0 : (c >= 0x20 && c <= 0x7e) ? c : '_';
  }
}

CORE_COLD int sysex_pool(uint8_t* m, bool replace, const PoolRow* rows, int count) {
  int n = head(m, 0x14);
  m[n++] = replace ? 1 : 0;
  m[n++] = (uint8_t)count;
  for (int k = 0; k < count; ++k) {
    const PoolRow& r = rows[k];
    m[n++] = r.slot;
    m[n++] = (r.folder >> 7) & 0x7f;
    m[n++] = r.folder & 0x7f;
    m[n++] = (r.file >> 7) & 0x7f;
    m[n++] = r.file & 0x7f;
    name13(m + n, r.name, r.len);
    n += 13;
  }
  m[n++] = 0xf7;
  return n;
}

CORE_COLD int sysex_names(uint8_t* m, uint8_t cmd, bool replace, int total, int first, const char* const* names,
                          int count) {
  int n = head(m, cmd);
  m[n++] = replace ? 1 : 0;
  m[n++] = (uint8_t)total;
  for (int k = 0; k < count; ++k) {
    const char* nm = names[k] ? names[k] : "";
    m[n++] = (uint8_t)(first + k);
    name13(m + n, nm, (int)strnlen(nm, 13));
    n += 13;
  }
  m[n++] = 0xf7;
  return n;
}

CORE_COLD int sysex_juno_knobs(uint8_t* m, int voice, const uint8_t knobs[6]) {
  int n = head(m, 0x15);
  m[n++] = (uint8_t)voice & 7;
  for (int i = 0; i < 6; ++i) m[n++] = knobs[i] & 0x7f;
  m[n++] = 0xf7;
  return n;
}

CORE_COLD int sysex_listing(uint8_t* m, int f, int parent, int first, int total, const ListEntry* e, int count) {
  int n = head(m, 0x0e);
  m[n++] = (f >> 7) & 0x7f; m[n++] = f & 0x7f;
  m[n++] = (parent >> 7) & 0x7f; m[n++] = parent & 0x7f;
  m[n++] = (first >> 7) & 0x7f; m[n++] = first & 0x7f;
  m[n++] = (total >> 7) & 0x7f; m[n++] = total & 0x7f;
  for (int k = 0; k < count; ++k) {
    uint32_t size = e[k].size, id = e[k].id;
    m[n++] = e[k].kind;
    m[n++] = (id >> 7) & 0x7f; m[n++] = id & 0x7f;
    if (size >= (1u << 28)) size = (1u << 28) - 1;
    m[n++] = (size >> 21) & 0x7f; m[n++] = (size >> 14) & 0x7f;
    m[n++] = (size >> 7) & 0x7f; m[n++] = size & 0x7f;
    int len = (int)strlen(e[k].name);
    m[n++] = (uint8_t)len;
    memcpy(m + n, e[k].name, len);
    n += len;
  }
  m[n++] = 0xf7;
  return n;
}

CORE_COLD int sysex_slot_unchanged(uint8_t* m, int slot) {
  const uint8_t u[10] = {0xf0, 0x7d, 0x52, 0x54, 0x1a, 1, (uint8_t)slot, 1, 0x7f, 0xf7};
  memcpy(m, u, 10);
  return 10;
}

CORE_COLD int sysex_slot_empty(uint8_t* m, int slot) {
  memset(m, 0, 43);
  const uint8_t h[13] = {0xf0, 0x7d, 0x52, 0x54, 0x1a, 1, (uint8_t)slot, 1, 0, 0, 0, 0, 1};
  memcpy(m, h, 13);
  m[42] = 0xf7;
  return 43;
}

CORE_COLD int sysex_installed_empty(uint8_t* m) {
  const uint8_t l[12] = {0xf0, 0x7d, 0x52, 0x54, 0x1b, 1, 0, 0x21, 1, 1, 0, 0xf7};
  memcpy(m, l, 12);
  return 12;
}

CORE_COLD int sysex_slots(uint8_t* m, int answer_slot, int result) {
  const uint8_t g[12] = {0xf0, 0x7d, 0x52, 0x54, 0x1d, 1, 1, 1, 1, 1, 1, 1};
  memcpy(m, g, 12);
  int n = 12;
  if (answer_slot) {
    m[n++] = (uint8_t)answer_slot;
    m[n++] = (uint8_t)result;
  }
  m[n++] = 0xf7;
  return n;
}

CORE_COLD int sysex_slots_caps(uint8_t* m, int n, uint8_t caps) {
  if ((n != 13 && n != 15) || m[4] != 0x1d || m[n - 1] != 0xf7) return n;
  if (n == 13) m[12] = 0, m[13] = 0;
  m[14] = caps & 0x7f;
  m[15] = 0xf7;
  return 16;
}

}  // namespace core
