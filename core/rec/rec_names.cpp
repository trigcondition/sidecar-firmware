// core/rec/rec_names.cpp - see rec_names.h.
#include "rec/rec_names.h"

#include <string.h>

#include "platform.h"

namespace rec {

static const Source kSrc[kSources] = {
    {"MASTER", 2, 0, 0},
    {"MAIN_L", 1, 0, 0}, {"MAIN_R", 1, 0, 1}, {"BD", 1, 0, 2}, {"SD", 1, 0, 3}, {"RS-CP", 1, 0, 4}, {"BT", 1, 0, 5},
    {"LT", 1, 0, 6}, {"MT-HT", 1, 0, 7}, {"CH-OH", 1, 0, 8}, {"CY-CB", 1, 0, 9}, {"IN_L", 1, 0, 10}, {"IN_R", 1, 0, 11},
    {"VOICE1", 1, 1, 0}, {"VOICE2", 1, 1, 1}, {"VOICE3", 1, 1, 2}, {"VOICE4", 1, 1, 3},
    {"VOICE5", 1, 1, 4}, {"VOICE6", 1, 1, 5}, {"VOICE7", 1, 1, 6}, {"VOICE8", 1, 1, 7},
    {"FX1", 2, 1, 8}, {"FX2", 2, 1, 10}, {"PRE", 2, 1, 12}, {"POST", 2, 1, 14},
};

static const Source kFxIn[2] = {{"FX_L", 1, 0, 10}, {"FX_R", 1, 0, 11}};   // ids 11, 12 while the FX return is on IN L/R
static volatile bool s_fxin;
CORE_COLD void set_fxin(bool on) { s_fxin = on; }
CORE_COLD bool fxin() { return s_fxin; }

CORE_COLD const Source& source(int id) {
  if (s_fxin && (id == kIn + 10 || id == kIn + 11)) return kFxIn[id - (kIn + 10)];
  return kSrc[id < 0 || id >= kSources ? 0 : id];
}

CORE_COLD static bool same(const char* a, unsigned n, const char* b) {
  if (strlen(b) != n) return false;
  for (unsigned i = 0; i < n; ++i) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (x == '/') x = '-';
    if (x != y) return false;
  }
  return true;
}

CORE_COLD bool parse_list(const char* s, uint32_t* mask, char* err, unsigned errlen) {
  uint32_t m = 0, all = (1u << kSources) - 1;
  bool rel = *s == '+' || *s == '-';
  if (rel) m = *mask;
  while (*s) {
    while (*s == ',' || *s == ' ') ++s;
    if (!*s) break;
    char op = 0;
    if (*s == '+' || *s == '-') op = *s++;
    const char* e = s;
    while (*e && *e != ',' && *e != ' ') ++e;
    unsigned n = (unsigned)(e - s);
    uint32_t bits = 0;
    if (same(s, n, "ALL")) bits = all;
    else if (same(s, n, "NONE")) bits = 0, m = op ? m : 0;
    else if (same(s, n, "IN")) bits = kInMask;
    else if (same(s, n, "VOICES")) bits = ((1u << kVoices) - 1) << kVoice;
    else {
      int id = -1;
      if (n > 0 && n <= 2 && s[0] >= '0' && s[0] <= '9' && (n == 1 || (s[1] >= '0' && s[1] <= '9')))
        id = n == 1 ? s[0] - '0' : (s[0] - '0') * 10 + s[1] - '0';
      for (int i = 0; i < kSources && id < 0; ++i)
        if (same(s, n, kSrc[i].name)) id = i;
      for (int i = 0; i < 2 && id < 0; ++i)
        if (same(s, n, kFxIn[i].name)) id = kIn + 10 + i;
      if (id < 0 || id >= kSources) {
        if (err) sniprintf(err, errlen, CORE_STR("unknown channel \"%.*s\""), (int)(n > 20 ? 20 : n), s);
        return false;
      }
      bits = 1u << id;
    }
    if (op == '-') m &= ~bits;
    else m |= bits;
    s = e;
  }
  *mask = m;
  return true;
}

CORE_COLD int list_ids(uint32_t mask, char* out, unsigned cap) {
  int n = 0;
  out[0] = 0;
  for (int i = 0; i < kSources; ++i)
    if (mask & (1u << i)) {
      int k = sniprintf(out + n, cap - n, CORE_STR("%s%d"), n ? "," : "", i);
      if (k < 0 || (unsigned)(n + k) >= cap) break;
      n += k;
    }
  return n;
}

CORE_COLD unsigned samples_per_frame(uint32_t mask, uint8_t st) {
  unsigned n = 0;
  for (int i = 0; i < kSources; ++i)
    if (mask & (1u << i)) n += kSrc[i].width + (i >= kVoice && i < kVoice + kVoices && (st >> (i - kVoice) & 1) ? 1 : 0);
  return n;
}

CORE_COLD unsigned files(uint32_t mask) { return (unsigned)__builtin_popcount(mask & ((1u << kSources) - 1)); }

CORE_COLD uint32_t bytes_per_second(uint32_t mask, unsigned bits, uint8_t st) {
  return samples_per_frame(mask, st) * (bits / 8) * 48000u;
}

CORE_COLD bool valid_prefix(const char* p) {
  unsigned n = (unsigned)strlen(p);
  if (n == 0 || n > 24) return false;
  for (unsigned i = 0; i < n; ++i) {
    char c = p[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
  }
  return true;
}

CORE_COLD int take_number(const char* name, const char* prefix) {
  unsigned pl = (unsigned)strlen(prefix);
  if (strncmp(name, prefix, pl) || name[pl] != '_') return -1;
  const char* p = name + pl + 1;
  if (!*p) return -1;
  int v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  return *p ? -1 : v;
}

CORE_COLD void take_name(char* out, unsigned cap, const char* prefix, int n) { sniprintf(out, cap, CORE_STR("%s_%03d"), prefix, n); }
CORE_COLD void take_dir(char* out, unsigned cap, const char* take) { sniprintf(out, cap, CORE_STR("/Recordings/%s"), take); }
CORE_COLD void file_path(char* out, unsigned cap, const char* take, int id) {
  sniprintf(out, cap, CORE_STR("/Recordings/%s/%02d_%s.wav"), take, id, source(id).name);
}

CORE_COLD bool is_wav_name(const char* name) {
  unsigned l = (unsigned)strlen(name);
  return l >= 4 && (!strcmp(name + l - 4, ".wav") || !strcmp(name + l - 4, ".WAV"));
}

static inline char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static inline bool alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }

CORE_COLD void slug(const char* name, char* out, unsigned cap) {
  unsigned n = 0, max = cap ? (cap - 1 < kSlugMax ? cap - 1 : kSlugMax) : 0;
  bool dash = false;
  for (const char* s = name ? name : ""; *s && n < max; ++s) {
    char c = lower(*s);
    if (alnum(c)) {
      if (dash && n) {
        out[n++] = '-';
        if (n >= max) break;
      }
      out[n++] = c, dash = false;
    } else dash = true;
  }
  while (n && out[n - 1] == '-') --n;   // (cut at the limit right after a '-')
  if (cap) out[n] = 0;
}

static void civil(uint32_t t, int* y, int* mo, int* d, int* h, int* mi) {
  uint32_t days = t / 86400, rem = t % 86400;
  *h = (int)(rem / 3600), *mi = (int)(rem / 60 % 60);
  // Howard Hinnant's civil_from_days (1970..2105: unsigned is enough)
  uint32_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*mo <= 2));
}

CORE_COLD void stamp_name(char* out, unsigned cap, const char* sl, uint32_t local) {
  int y, mo, d, h, mi;
  civil(local, &y, &mo, &d, &h, &mi);
  sniprintf(out, cap, CORE_STR("%s_%04d-%02d-%02d_%02d%02d"), sl, y, mo, d, h, mi);
}

CORE_COLD void date_time(uint32_t local, char* out, unsigned cap) {
  int y, mo, d, h, mi;
  civil(local, &y, &mo, &d, &h, &mi);
  sniprintf(out, cap, CORE_STR("%04d-%02d-%02d %02d:%02d"), y, mo, d, h, mi);
}

CORE_COLD void variant(char* out, unsigned cap, const char* base, int k) {
  if (k <= 1) sniprintf(out, cap, CORE_STR("%s"), base);
  else sniprintf(out, cap, CORE_STR("%s_%d"), base, k);
}

// name starts with p (any case): the rest, else null
static const char* after(const char* name, const char* p) {
  for (; *p; ++p, ++name)
    if (lower(*name) != lower(*p)) return nullptr;
  return name;
}

// "123" (no sign, no leading '0' unless lead): its value, else -1
static int digits(const char* s, bool lead) {
  if (!*s || (!lead && *s == '0')) return -1;
  int v = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || v > 100000) return -1;
    v = v * 10 + (*s - '0');
  }
  return v;
}

CORE_COLD int variant_number(const char* name, const char* base) {
  const char* r = after(name, base);
  if (!r) return 0;
  if (!*r) return 1;
  int k = *r == '_' ? digits(r + 1, false) : -1;
  return k >= 2 ? k : 0;
}

CORE_COLD void seq_name(char* out, unsigned cap, const char* sl, int n) { sniprintf(out, cap, CORE_STR("%s_take-%03d"), sl, n); }

CORE_COLD int seq_number(const char* name, const char* sl) {
  const char* r = after(name, sl);
  r = r ? after(r, "_take-") : nullptr;
  return r ? digits(r, true) : -1;
}

CORE_COLD int next_number(const char (*names)[64], int n, const char* sl, bool clock, const char* base) {
  int next = 1;
  for (int i = 0; i < n; ++i) {
    int v = clock ? variant_number(names[i], base) : seq_number(names[i], sl);
    if (v >= next) next = v + 1;
  }
  return next;
}

}  // namespace rec
