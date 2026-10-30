// rmp_loader.cpp
#include "rmp_loader.h"
#include "rmp_api.h"
#include <string.h>

// RMP_PLUGIN_BYTES (sizeof(rmp_plugin) on the 32-bit Teensy) is fixed in rmp_api.h, which asserts it
// under __arm__; the Mac test sees the same bound as the device. tools/rmp_pack.py uses the same constant.

// Two table steps a byte (the 16-entry table of poly 0xEDB88320): ~4x the bitwise loop.
static const uint32_t kCrcNib[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
RMP_COLD uint32_t rmp_crc32_update(uint32_t c, const uint8_t *p, uint32_t n) {
  while (n--) { c ^= *p++; c = (c >> 4) ^ kCrcNib[c & 15]; c = (c >> 4) ^ kCrcNib[c & 15]; }
  return c;
}
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

// Thumb-2 MOVW/MOVT T3 encoding: imm16 = imm4:i:imm3:imm8 over two halfwords.
RMP_COLD static void set_imm16(uint8_t *at, uint16_t imm) {
  uint16_t hw1 = rd16(at), hw2 = rd16(at + 2);
  hw1 = (uint16_t)((hw1 & 0xFBF0u) | ((imm >> 12) & 0xFu) | (((imm >> 11) & 1u) << 10));
  hw2 = (uint16_t)((hw2 & 0x8F00u) | (((imm >> 8) & 7u) << 12) | (imm & 0xFFu));
  wr16(at, hw1); wr16(at + 2, hw2);
}

#ifdef RMP_HOST_A64
uint8_t rmp_host_target = RMP_TARGET_A64;
#else
uint8_t rmp_host_target = RMP_TARGET_ARM32;
#endif
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

RMP_COLD rmp_status rmp_verify_header(const uint8_t *hdr, uint32_t len, rmp_info *info) {
  rmp_status st = rmp_verify_header_any(hdr, len, info);
  if (st == RMP_OK && info->target != rmp_host_target) return RMP_ERR_TARGET;
  return st;
}

RMP_COLD rmp_status rmp_verify_header_any(const uint8_t *hdr, uint32_t len, rmp_info *info) {
  if (len < RMP_HEADER_BYTES) return RMP_ERR_SIZE;
  rmp_header h; memcpy(&h, hdr, sizeof h);
  if (h.magic != RMP_FILE_MAGIC && h.magic != RMP_FILE_MAGIC_A64) return RMP_ERR_MAGIC;
  uint8_t target = h.magic == RMP_FILE_MAGIC_A64 ? RMP_TARGET_A64 : RMP_TARGET_ARM32;
  uint32_t table = target == RMP_TARGET_A64 ? RMP_PLUGIN_BYTES_A64 : RMP_PLUGIN_BYTES;
  // 1..3: same image, relocations; 4 (format 3 with an RLB2 block) an RMPA package only: the Teensy shows no
  // labels that follow a knob, so an RMP1 package stops at 3
  if (h.format < RMP_FORMAT_MIN || h.format > (target == RMP_TARGET_A64 ? RMP_FORMAT : RMP_FORMAT_RMP1)) return RMP_ERR_FORMAT;
  if (h.api < RMP_API_MIN || h.api > RMP_API) return RMP_ERR_API;                  // API 1 plugins still load
  if (h.id[0] == 0 || !memchr(h.id, 0, sizeof h.id)) return RMP_ERR_FORMAT;   // non-empty, NUL inside the 32 bytes
  uint64_t need = (uint64_t)RMP_HEADER_BYTES + h.image_size + 8ull * h.reloc_count + h.manifest_size;
  // the packer pads the body to 4 bytes: no more, no less (no trailing garbage)
  if (need > len || (need + 3u) / 4u * 4u != len || h.image_size > (1u << 20) || h.bss_size > (1u << 20) ||
      (h.entry_offset & 3u) || (uint64_t)h.entry_offset + table > h.image_size) return RMP_ERR_SIZE;
  uint32_t ds = h.image_size;
  if (target == RMP_TARGET_A64) {
    memcpy(&ds, h.reserved, 4);
    if (ds == 0 || (ds & (RMP_A64_ALIGN - 1u)) || ds > h.image_size || h.entry_offset + table > ds) return RMP_ERR_FORMAT;
  }
  memcpy(info->id, h.id, 32);
  info->target = target;
  info->data_start = ds;
  info->format = h.format; info->api = h.api;
  info->version = h.version; info->compatible_from = h.compatible_from;
  info->image_size = h.image_size; info->bss_size = h.bss_size; info->entry_offset = h.entry_offset;
  info->reloc_count = h.reloc_count; info->manifest_size = h.manifest_size; info->crc32 = h.crc32;
  info->flags = h.flags;
  info->image = info->relocs = info->manifest = nullptr;
  return RMP_OK;
}

RMP_COLD rmp_status rmp_verify(const uint8_t *pkg, uint32_t len, rmp_info *info) {
  rmp_status st = rmp_verify_header(pkg, len, info);
  if (st != RMP_OK) return st;
  if (~rmp_crc32_update(0xFFFFFFFFu, pkg + RMP_HEADER_BYTES, len - RMP_HEADER_BYTES) != info->crc32) return RMP_ERR_CRC;
  info->image = pkg + RMP_HEADER_BYTES;
  info->relocs = info->image + info->image_size;
  info->manifest = info->relocs + 8u * info->reloc_count;
  return RMP_OK;
}

// One relocation, shared by rmp_place and rmp_place_stream.
RMP_COLD static rmp_status apply_reloc(uint8_t *dst, uint32_t image_size, const uint8_t *rel, uintptr_t base, uint8_t target) {
  uint32_t wt = rd32(rel), val = rd32(rel + 4);
  uint32_t off = wt & 0x0FFFFFFFu, type = wt >> 28;
  if ((uint64_t)off + 4 > image_size) return RMP_ERR_RELOC;
  if (target == RMP_TARGET_A64) return RMP_ERR_RELOC;   // RMPA images: apply_a64
  uint32_t b32 = (uint32_t)base;
  switch (type) {
    case RMP_REL_ABS32: wr32(dst + off, rd32(dst + off) + b32); break;
    case RMP_REL_MOVW: set_imm16(dst + off, (uint16_t)((val + b32) & 0xFFFFu)); break;
    case RMP_REL_MOVT: set_imm16(dst + off, (uint16_t)((val + b32) >> 16)); break;
    default: return RMP_ERR_RELOC;
  }
  return RMP_OK;
}

// One RMPA relocation (rmp_place_a64).
RMP_COLD static rmp_status apply_a64(const rmp_info *info, uint8_t *code, uintptr_t code_base, uint8_t *data,
                                     uintptr_t data_base, const uint8_t *rel) {
  uint32_t wt = rd32(rel);
  uint32_t off = wt & 0x0FFFFFFFu, type = wt >> 28, ds = info->data_start;
  uint8_t *at = off < ds ? code + off : data + (off - ds);
  if (type == RMP_REL_ABS64) {
    if ((uint64_t)off + 8 > info->image_size || (off & 7u) || (off < ds && off + 8 > ds)) return RMP_ERR_RELOC;
    uint64_t v = rd64(at);
    wr64(at, v < ds ? v + (uint64_t)code_base : v - ds + (uint64_t)data_base);
    return RMP_OK;
  }
  if (type == RMP_REL_A64_ADRP_DATA) {
    if ((uint64_t)off + 4 > ds || (off & 3u)) return RMP_ERR_RELOC;
    uint32_t insn = rd32(at);
    if ((insn & 0x9F000000u) != 0x90000000u) return RMP_ERR_RELOC;   // ADRP
    int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));
    if (imm & (1 << 20)) imm -= 1 << 21;
    int64_t delta = ((int64_t)data_base - (int64_t)ds) - (int64_t)code_base;   // page-aligned: both bases and ds
    imm += delta >> 12;
    if (imm < -(1 << 20) || imm >= (1 << 20)) return RMP_ERR_RELOC;   // the parts are more than 4 GB apart
    uint32_t u = (uint32_t)imm & 0x1FFFFFu;
    insn = (insn & 0x9F00001Fu) | ((u & 3u) << 29) | ((u >> 2) << 5);
    wr32(at, insn);
    return RMP_OK;
  }
  return RMP_ERR_RELOC;
}

RMP_COLD rmp_status rmp_place_a64(const rmp_info *info, uint8_t *code, uintptr_t code_base, uint8_t *data, uintptr_t data_base) {
  if (info->target != RMP_TARGET_A64 || (code_base & (RMP_A64_ALIGN - 1u)) || (data_base & (RMP_A64_ALIGN - 1u)))
    return RMP_ERR_RELOC;
  uint32_t ds = info->data_start;
  memcpy(code, info->image, ds);
  memcpy(data, info->image + ds, info->image_size - ds);
  memset(data + (info->image_size - ds), 0, info->bss_size);
  for (uint32_t k = 0; k < info->reloc_count; ++k) {
    rmp_status st = apply_a64(info, code, code_base, data, data_base, info->relocs + 8 * k);
    if (st != RMP_OK) return st;
  }
  return RMP_OK;
}

RMP_COLD rmp_status rmp_place(const rmp_info *info, uint8_t *dst, uintptr_t base) {
  if (info->target == RMP_TARGET_A64)
    return rmp_place_a64(info, dst, base, dst + info->data_start, base + info->data_start);
  memcpy(dst, info->image, info->image_size);
  memset(dst + info->image_size, 0, info->bss_size);
  for (uint32_t k = 0; k < info->reloc_count; ++k) {
    rmp_status st = apply_reloc(dst, info->image_size, info->relocs + 8 * k, base, info->target);
    if (st != RMP_OK) return st;
  }
  return RMP_OK;
}

RMP_COLD rmp_status rmp_place_stream(const rmp_info *info, rmp_read_fn rd, void *ctx, uint8_t *dst, uintptr_t base) {
  if (info->target == RMP_TARGET_A64) return RMP_ERR_RELOC;   // RMPA: rmp_place_a64 from a package in RAM
  if (!rd(ctx, RMP_HEADER_BYTES, dst, info->image_size)) return RMP_ERR_SIZE;
  memset(dst + info->image_size, 0, info->bss_size);
  uint8_t chunk[32 * 8];
  uint32_t at = RMP_HEADER_BYTES + info->image_size;
  for (uint32_t k = 0; k < info->reloc_count;) {
    uint32_t c = info->reloc_count - k < 32u ? info->reloc_count - k : 32u;
    if (!rd(ctx, at + 8u * k, chunk, 8u * c)) return RMP_ERR_SIZE;
    for (uint32_t j = 0; j < c; ++j) {
      rmp_status st = apply_reloc(dst, info->image_size, chunk + 8 * j, base, info->target);
      if (st != RMP_OK) return st;
    }
    k += c;
  }
  return RMP_OK;
}

RMP_COLD bool rmp_id_ok(const char *id) {
  if (!id || !*id || id[0] == '.') return false;
  size_t l = strnlen(id, 32);
  if (l == 0 || l > 31) return false;
  for (size_t i = 0; i < l; ++i) {
    unsigned char c = (unsigned char)id[i];
    if (c < 0x21 || c > 0x7E || c == '/' || c == '\\') return false;   // printable ASCII, no space
  }
  return !strstr(id, "..");
}
