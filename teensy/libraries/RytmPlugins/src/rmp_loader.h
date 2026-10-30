// rmp_loader.h - verify a .rmp and place its image at a given address. No
// Teensy dependency: the Mac tests it; rmp_arena.cpp supplies the memory.
#pragma once
#include <stdint.h>
#include "rmp_format.h"
// RMP_COLD: load-time code (verify, place, slots, store) runs from program flash on
// the Teensy, keeping it out of ITCM (RAM1, shared with the host's DTCM: voiceloop
// is tight); nothing on the render path. RMP_SNPRINTF: integer-only on the Teensy
// (newlib's sniprintf, which the host already links; snprintf pulls the float
// printf, ~17 KB of ITCM). Neither formats a float.
#if defined(__IMXRT1062__)
#define RMP_COLD __attribute__((section(".flashmem")))
#define RMP_SNPRINTF sniprintf
#else
#define RMP_COLD
#ifndef RMP_SNPRINTF
#define RMP_SNPRINTF snprintf
#endif
#endif
enum rmp_status { RMP_OK = 0, RMP_ERR_MAGIC, RMP_ERR_FORMAT, RMP_ERR_API, RMP_ERR_SIZE, RMP_ERR_CRC, RMP_ERR_RELOC, RMP_ERR_TARGET };
struct rmp_info {
  char id[32];
  uint16_t format, api;   // the header's
  uint8_t target;         // RMP_TARGET_ARM32 (RMP1) or RMP_TARGET_A64 (RMPA), from the magic
  uint32_t data_start;    // RMPA: the writable part's offset (page-aligned); RMP1: image_size
  uint32_t version, compatible_from, image_size, bss_size, entry_offset, reloc_count, manifest_size, crc32;   // crc32: the header's, of the body after the 96-byte header
  uint32_t flags;         // the header's flags (rmp_format.h RMP_HF_*: seqx's max_voices / pitch; 0 in older packages)
  const uint8_t *image, *relocs, *manifest;   // inside the package
};
// Everything except the CRC, from the 96-byte header alone (hdr must hold at least
// RMP_HEADER_BYTES bytes; len is the TOTAL package length): magic, format, API, a
// non-empty id with a NUL inside its 32 bytes (else RMP_ERR_FORMAT), sizes, entry
// bounds/alignment, exact padded length. info->image/relocs/manifest are left null.
rmp_status rmp_verify_header(const uint8_t *hdr, uint32_t len, rmp_info *info);
// The target this host runs (RMP_TARGET_ARM32 by default; the Pi's build and the Mac's core lib, which run RMPA
// images, set RMP_TARGET_A64). rmp_verify_header refuses the other target's package with RMP_ERR_TARGET, so a
// Teensy never places AArch64 code and the Pi never places Thumb code (an older Teensy, which knows only RMP1,
// refuses an RMPA package as RMP_ERR_MAGIC). rmp_verify_header_any reads either (the Pi's web upload check lists
// a Teensy package as "for the Teensy" rather than unreadable).
extern uint8_t rmp_host_target;
rmp_status rmp_verify_header_any(const uint8_t *hdr, uint32_t len, rmp_info *info);
// Streaming CRC-32 (IEEE, as rmp_pack.py): start with crc = 0xFFFFFFFF, feed the
// body (everything after the header) in any chunking, the result is ~crc. Compare
// with info->crc32.
uint32_t rmp_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n);
// Checks magic, format, API, sizes and CRC; fills info.
rmp_status rmp_verify(const uint8_t *pkg, uint32_t len, rmp_info *info);
// Copies the image to dst (which will execute at address `base`), zeroes bss,
// applies the relocations for `base`. dst must hold image_size + bss_size bytes.
// `info` must come from rmp_verify on a package that is still alive (it points
// into it). On RMP_ERR_RELOC dst is partly written and must be discarded.
// base: where dst will execute; an RMPA image's must be RMP_A64_ALIGN-aligned (else RMP_ERR_RELOC).
rmp_status rmp_place(const rmp_info *info, uint8_t *dst, uintptr_t base);
// Reads n bytes at offset `off` of the PACKAGE (0 = the header's first byte) into buf;
// false on any failure (short read included).
typedef bool (*rmp_read_fn)(void *ctx, uint32_t off, uint8_t *buf, uint32_t n);
// rmp_place without the package in RAM: reads the image straight into dst, zeroes bss,
// then reads the relocations 32 at a time (256 bytes of stack) and applies them exactly
// as rmp_place does (same code). `info` only needs rmp_verify_header's fields; the CRC
// is the caller's (check it first, streaming with rmp_crc32_update). RMP_ERR_SIZE when
// a read fails, RMP_ERR_RELOC as rmp_place; on any error dst must be discarded.
rmp_status rmp_place_stream(const rmp_info *info, rmp_read_fn rd, void *ctx, uint8_t *dst, uintptr_t base);
// An RMPA image in two places (W^X): [0, data_start) to code (data_start bytes, executing at code_base), the rest of
// the image and the zeroed bss to data (image_size - data_start + bss_size bytes, at data_base); both bases
// RMP_A64_ALIGN-aligned. rmp_place on an RMPA image is this with the parts back to back.
rmp_status rmp_place_a64(const rmp_info *info, uint8_t *code, uintptr_t code_base, uint8_t *data, uintptr_t data_base);
// An id that is safe as a file name (/machines/<id>.rmp) and in a slot map: non-empty,
// at most 31 chars (NUL within 32 bytes), printable ASCII 0x21..0x7E only (no space,
// control or non-ASCII byte), no '/' or '\\', no leading '.', no "..".
bool rmp_id_ok(const char *id);
