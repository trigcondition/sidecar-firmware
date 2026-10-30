// rmp_manifest.cpp - see rmp_manifest.h.
#include "rmp_manifest.h"
#include <string.h>

// n bytes of a NUL-padded field, printable ASCII ('_' otherwise); everything after the first NUL is NUL;
// dst[n] = 0 (dst holds n + 1 bytes).
RMP_COLD static void clean(char *dst, const uint8_t *src, int n) {
  dst[n] = 0;
  bool end = false;
  for (int i = 0; i < n; ++i) {
    uint8_t c = src[i];
    if (!c) end = true;
    dst[i] = end ? 0 : (c >= 0x20 && c <= 0x7e) ? (char)c : '_';
  }
}
RMP_COLD static uint8_t sat127(uint8_t v) { return v > 127 ? 127 : v; }
RMP_COLD static bool alnum(uint8_t c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

RMP_COLD int rmp_desc_find(const rmp_desc *d, int p) {
  for (int j = 0; j < d->nknobs; ++j)
    if (d->knob[j].p == p) return j;
  return -1;
}

RMP_COLD bool rmp_manifest_read(const rmp_info *info, rmp_read_fn rd, void *ctx, rmp_desc *d) {
  memset(d, 0, sizeof *d);
  d->pre = -1;
  int fmt = info->format;
  if (fmt < 1 || fmt > 4) return false;   // (4: format 3 with an RLB2 block, rmp_format.h)
  uint32_t hb = fmt >= 3 ? 84u : 64u, kb = fmt == 1 ? 20u : fmt == 2 ? 22u : 26u;
  uint64_t at64 = (uint64_t)RMP_HEADER_BYTES + info->image_size + 8ull * info->reloc_count;
  if (info->manifest_size < hb || at64 + info->manifest_size > 0xFFFFFFFFull) return false;
  uint32_t at = (uint32_t)at64;
  uint8_t h[84];
  if (!rd(ctx, at, h, hb)) return false;
  uint32_t n = h[63];
  if (n > 32 || hb + n * kb > info->manifest_size) return false;
  clean(d->name, h, 10);
  clean(d->short_name, h + 10, 4);
  d->page2 = h[62] ? 1 : 0;
  d->format = (uint8_t)fmt;
  if (fmt >= 3) {
    clean(d->p2_title, h + 64, 13);
    int e = 0;
    while (e < 7 && alnum(h[77 + e])) ++e;
    bool ok = e > 0 && (e == 7 || h[77 + e] == 0);   // letters and digits up to the NUL padding
    if (ok) memcpy(d->preset_ext, h + 77, (size_t)e);
  }
  if (fmt >= 2) {
    d->names_at = at + hb + n * kb;
    d->names_end = at + info->manifest_size;
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint8_t k[26];
    if (!rd(ctx, at + hb + i * kb, k, kb)) return false;
    rmp_desc_knob q;
    memset(&q, 0, sizeof q);
    q.p = k[0];
    q.look = k[1] <= RMP_LOOK_LIST ? k[1] : RMP_LOOK_DIAL;
    q.kind = k[2] <= RMP_TEXT_POOL ? k[2] : RMP_TEXT_NUMBER;
    q.min = 0; q.max = 127; q.def = 64;
    const uint8_t *s;
    if (fmt == 1) {
      s = k + 4;
    } else if (fmt == 2) {
      q.enum_first = k[3]; q.enum_count = k[4];
      s = k + 6;
    } else {
      q.enum_first = k[3]; q.enum_count = k[4];
      q.role = k[5] <= RMP_ROLE_OFFSET ? k[5] : RMP_ROLE_FREE;
      uint8_t lo = sat127(k[6]), hi = sat127(k[7]), df = sat127(k[8]);
      if (lo <= hi) { q.min = lo; q.max = hi; q.def = df < lo ? lo : df > hi ? hi : df; }
      s = k + 10;
    }
    if (q.enum_count > RMP_ENUM_MAX) q.enum_count = RMP_ENUM_MAX;
    clean(q.long_name, s, 12);
    clean(q.short_name, s + 12, 4);
    if (q.p < 1 || q.p > 16 || rmp_desc_find(d, q.p) >= 0 || d->nknobs >= RMP_DESC_KNOBS) continue;
    int j = d->nknobs++;
    while (j > 0 && d->knob[j - 1].p > q.p) { d->knob[j] = d->knob[j - 1]; --j; }   // keep p order
    d->knob[j] = q;
  }
  for (int j = 0; j < d->nknobs; ++j)
    if (d->knob[j].kind == RMP_TEXT_PRESET) { d->pre = (int8_t)j; break; }
  return true;
}

RMP_COLD int rmp_manifest_enum(const rmp_desc *d, int k, rmp_read_fn rd, void *ctx, char names[16][8]) {
  memset(names, 0, 16 * 8);
  if (k < 0 || k >= d->nknobs || !d->names_at) return 0;
  const rmp_desc_knob &q = d->knob[k];
  if (!q.enum_count) return 0;
  uint64_t from = (uint64_t)d->names_at + 8ull * q.enum_first, to = from + 8ull * q.enum_count;
  if (to > d->names_end) return -1;
  uint8_t b[16 * 8];
  if (!rd(ctx, (uint32_t)from, b, 8u * q.enum_count)) return -1;
  for (int i = 0; i < q.enum_count; ++i) {
    clean(names[i], b + 8 * i, 7);   // the 8th byte is the terminator
  }
  return q.enum_count;
}

RMP_COLD static int zone_of(float value, int n) {
  int z = value >= 0.0f && value <= 127.0f ? (int)value : value > 127.0f ? 127 : 0;   // NaN: 0
  return (z * n) >> 7;
}

RMP_COLD bool rmp_manifest_labels(const rmp_info *info, rmp_read_fn rd, void *ctx, const rmp_desc *d, rmp_labels *out) {
  memset(out, 0, sizeof *out);
  if (!d || d->format < 3 || !d->names_at || d->names_end < d->names_at) return false;
  uint32_t names = 0;   // the name table's end: the block starts after the last name a knob uses
  for (int j = 0; j < d->nknobs; ++j) {
    uint32_t e = 8u * ((uint32_t)d->knob[j].enum_first + d->knob[j].enum_count);
    if (e > names) names = e;
  }
  uint32_t lo = d->names_at + names, end = d->names_end;
  if (end < lo + 4u + RMP_LABELS_FOOTER) return false;
  uint8_t f[RMP_LABELS_FOOTER];
  if (!rd(ctx, end - RMP_LABELS_FOOTER, f, RMP_LABELS_FOOTER)) return false;
  uint32_t magic, len = (uint32_t)f[0] | (uint32_t)f[1] << 8;
  memcpy(&magic, f + 2, 4);
  bool v2 = magic == RMP_LABELS_MAGIC2;
  if ((magic != RMP_LABELS_MAGIC && !v2) || len < 4u || len > end - RMP_LABELS_FOOTER - lo) return false;
  // the block in one read (a card read on the Pi's control core): RLB2 at most 5 + 16 + 8 + 32 x 8 + 32 x 8 x 16 bytes
  static uint8_t blk[5 + RMP_LABELS_DEPS + RMP_LABELS_PARAMS + RMP_LABELS_SETS * RMP_ENUM_NAME_BYTES +
                     RMP_LABELS_SETS * RMP_LABELS_PARAMS * RMP_LABEL_BYTES];
  if (len > sizeof blk || !rd(ctx, end - RMP_LABELS_FOOTER - len, blk, len)) return false;
  const uint8_t *h = blk;
  uint8_t knob = h[0], nsets = h[1], np = h[2], dep = v2 ? h[3] : 0, ndep = v2 ? h[4] : 0;
  uint32_t head = v2 ? 5u + ndep : 4u;   // the Ps' offset
  if (np < 1 || np > RMP_LABELS_PARAMS || nsets < 1 || nsets > (v2 ? RMP_LABELS_SETS : RMP_LABELS_SETS1) ||
      (v2 && (ndep < 1 || ndep > RMP_LABELS_DEPS || len < head)) ||
      len != head + np + (v2 ? (uint32_t)nsets * RMP_ENUM_NAME_BYTES : 0u) + (uint32_t)nsets * np * RMP_LABEL_BYTES)
    return false;
  int k = rmp_desc_find(d, knob);
  if (k < 0 || d->knob[k].look != RMP_LOOK_LIST || (!v2 && d->knob[k].enum_count != nsets)) return false;
  if (v2) {
    int q = rmp_desc_find(d, dep);
    if (dep == knob || q < 0 || d->knob[q].look != RMP_LOOK_LIST || d->knob[q].enum_count != ndep) return false;
    uint32_t sum = 0;
    for (int v = 0; v < ndep; ++v) {
      uint8_t c = h[5 + v];
      if (c < 1 || c > RMP_ENUM_MAX) { memset(out, 0, sizeof *out); return false; }   // (out all zero, as every refusal)
      out->count[v] = c, out->base[v] = (uint8_t)sum;
      sum += c;
    }
    if (sum != nsets) { memset(out, 0, sizeof *out); return false; }
  }
  const uint8_t *ps = h + head;
  for (int c = 0; c < np; ++c) {
    uint8_t p = ps[c];
    if (p < 1 || p > 16 || p == knob || (v2 && p == dep) || rmp_desc_find(d, p) < 0 || memchr(ps, p, (size_t)c)) {
      memset(out, 0, sizeof *out);
      return false;
    }
  }
  const uint8_t *nm = ps + np, *lb = nm + (v2 ? (uint32_t)nsets * RMP_ENUM_NAME_BYTES : 0u);
  for (int s = 0; v2 && s < nsets; ++s) {
    char t[RMP_ENUM_NAME_BYTES];
    clean(t, nm + s * RMP_ENUM_NAME_BYTES, RMP_ENUM_NAME_BYTES - 1);   // the 8th byte is the terminator
    memcpy(out->names[s], t, RMP_ENUM_NAME_BYTES);
  }
  uint32_t row = (uint32_t)np * RMP_LABEL_BYTES;
  for (int s = 0; s < nsets; ++s) {
    const uint8_t *b = lb + s * row;
    for (int c = 0; c < np; ++c) {   // printable ASCII, NUL-padded (as clean() makes the knobs' own)
      char t[13];
      clean(t, b + c * RMP_LABEL_BYTES, 12);
      memcpy(out->lbl[s][c], t, 12);
      clean(t, b + c * RMP_LABEL_BYTES + 12, 4);
      memcpy(out->lbl[s][c] + 12, t, 4);
    }
  }
  memcpy(out->p, ps, np);
  out->knob = knob, out->nparams = np, out->nsets = nsets, out->dep = dep, out->ndep = ndep;
  return true;
}

RMP_COLD int rmp_labels_set(const rmp_labels *l, float value) {
  if (!l || !l->nsets || l->dep) return -1;
  return zone_of(value, l->nsets);
}

RMP_COLD int rmp_labels_pick(const rmp_labels *l, const float *p) {
  if (!l || !l->nsets || !p) return -1;
  if (!l->dep) return rmp_labels_set(l, p[l->knob - 1]);
  int v = zone_of(p[l->dep - 1], l->ndep);
  return l->base[v] + zone_of(p[l->knob - 1], l->count[v]);
}

RMP_COLD static int dep_of(const rmp_labels *l, int set) {   // the dep value whose names hold set
  int v = 0;
  while (v + 1 < l->ndep && set >= l->base[v + 1]) ++v;
  return v;
}

RMP_COLD bool rmp_labels_apply(const rmp_labels *l, int set, rmp_desc *d) {
  if (!l || set < 0 || set >= l->nsets) return false;
  bool did = false;
  for (int c = 0; c < l->nparams; ++c) {
    int k = rmp_desc_find(d, l->p[c]);
    if (k < 0) continue;
    memset(d->knob[k].long_name, 0, sizeof d->knob[k].long_name);
    memset(d->knob[k].short_name, 0, sizeof d->knob[k].short_name);
    memcpy(d->knob[k].long_name, l->lbl[set][c], 12);
    memcpy(d->knob[k].short_name, l->lbl[set][c] + 12, 4);
    did = true;
  }
  int k = l->dep ? rmp_desc_find(d, l->knob) : -1;
  if (k >= 0) d->knob[k].enum_count = l->count[dep_of(l, set)], did = true;
  return did;
}

RMP_COLD int rmp_labels_enum(const rmp_labels *l, int set, int p, char names[16][8]) {
  if (!l || !l->dep || set < 0 || set >= l->nsets || p != l->knob) return -1;
  int v = dep_of(l, set);
  memset(names, 0, 16 * 8);
  for (int i = 0; i < l->count[v]; ++i) memcpy(names[i], l->names[l->base[v] + i], 8);
  return l->count[v];
}

RMP_COLD void rmp_desc_from_id(const char *id, rmp_desc *d) {
  memset(d, 0, sizeof *d);
  d->pre = -1;
  const char *last = strrchr(id, '.');
  const char *p = last && last[1] ? last + 1 : id;
  int n = 0;
  for (; p[n] && n < 10; ++n) {
    char c = p[n];
    d->name[n] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (c >= 0x20 && c <= 0x7e) ? c : '_';
  }
  memcpy(d->short_name, d->name, n < 4 ? (size_t)n : 4u);
}
