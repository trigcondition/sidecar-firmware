// rmp_format.h - the .rmp file layout (tools/rmp_pack.py writes it; keep in step,
// tools/tests/test_rmp_pack.py compiles this header and compares the layout).
#pragma once
#include <stdint.h>
#define RMP_FILE_MAGIC 0x31504D52u       /* "RMP1": an ARM Thumb-2 image (the Teensy) */
#define RMP_FILE_MAGIC_A64 0x41504D52u   /* "RMPA": an AArch64 image (the Pi; 2026-10-17, docs/user-machines-pi.md): the
                                           same header, manifest and plugin ABI built for AArch64; relocations
                                           RMP_REL_ABS64 only, the image placed at a 4096-byte aligned address */
#define RMP_FORMAT 4u          /* the newest the loader accepts. rmp_pack.py writes 3 (the manifest head and knobs
                                  below), or 4: format 3 with an RLB2 block (below) - a package an older host (3 at
                                  most) refuses rather than show its list knob's first list for every value */
#define RMP_FORMAT_RMP1 3u     /* the newest for an RMP1 (Teensy) package: format 4 is RMPA only */
#define RMP_FORMAT_MIN 1u      /* the oldest the loader accepts: 1 differs only in the manifest
                                  (20-byte knobs p, look, text, pad, long, short; no name table) */
#define RMP_HEADER_BYTES 96u
/* RMPA packages only: ABS64 a 64-bit word, moved by the code part's base when its value is below data_start, else
   by the data part's; A64_ADRP_DATA an ADRP in the code part whose target is in the data part (the page distance
   between the two parts changes). An RMPA header's reserved[0..3] is data_start (page-aligned): [0, data_start) is
   code + read-only data (executable memory), [data_start, image_size) + bss the writable part (RW memory). */
enum { RMP_REL_ABS32 = 0, RMP_REL_MOVW = 1, RMP_REL_MOVT = 2, RMP_REL_ABS64 = 3, RMP_REL_A64_ADRP_DATA = 4 };
enum { RMP_TARGET_ARM32 = 0, RMP_TARGET_A64 = 1 };   /* a package's target, from its magic */
#define RMP_A64_ALIGN 4096u   /* an RMPA image's base alignment (ADRP and the :lo12: offsets are page-relative) */
typedef struct __attribute__((packed)) {
  uint32_t magic; uint16_t format; uint16_t api;
  char id[32]; uint32_t version; uint32_t compatible_from;
  uint32_t image_size; uint32_t bss_size; uint32_t entry_offset;
  uint32_t reloc_count; uint32_t manifest_size; uint32_t flags; uint32_t crc32;
  uint8_t reserved[20];
} rmp_header;
/* The header's flags (seqx E0, 2026-10-22; 0 in every older package, and an older host ignores them: it plays the
   machine mono, as before). tools/rmp_pack.py sets them from the manifest's "max_voices" (1..8) and "pitch".
   RMP_HF_INSTANCES: the plugin keeps everything in its state (no globals that render() writes), so the host may run
   several states of it on one voice - a poly track's notes, a unison stack (core/inst.h). Without it: one state a
   voice. Bits 4..6: max_voices - 1 (with RMP_HF_INSTANCES). Bits 8..9: how note_on's note is meant (RMP_PITCH_*). */
#define RMP_HF_INSTANCES 0x1u
#define RMP_HF_VOICES_SHIFT 4
#define RMP_HF_PITCH_SHIFT 8
enum { RMP_PITCH_NOTE = 0, RMP_PITCH_NONE = 2 };   /* NOTE: the pitch (60 = C4); NONE: a fixed-pitch drum (60 always) */
static inline int rmp_hf_max_voices(uint32_t f) { return (f & RMP_HF_INSTANCES) ? (int)((f >> RMP_HF_VOICES_SHIFT) & 7u) + 1 : 1; }
static inline int rmp_hf_pitch(uint32_t f) { return (int)((f >> RMP_HF_PITCH_SHIFT) & 3u) == RMP_PITCH_NONE ? RMP_PITCH_NONE : RMP_PITCH_NOTE; }
typedef struct __attribute__((packed)) { uint32_t where_type; uint32_t value; } rmp_reloc;
/* enum_count names for this knob's values (look "list"): entries enum_first ..
   enum_first + enum_count - 1 of the name table that follows the knobs. */
typedef struct __attribute__((packed)) {
  uint8_t p, look, text, enum_first, enum_count, reserved; char long_name[12]; char short_name[4];
} rmp_knob;
/* Manifest: rmp_manifest_head, knob_count rmp_knob, then the name table (as many
   entries as the knobs' enum_count add up to, each RMP_ENUM_NAME_BYTES, NUL-padded
   ASCII, at most RMP_ENUM_NAME_BYTES - 1 characters). */
#define RMP_ENUM_NAME_BYTES 8
#define RMP_ENUM_MAX 16             /* names per knob */
typedef struct __attribute__((packed)) { char name[RMP_ENUM_NAME_BYTES]; } rmp_enum_name;
typedef struct __attribute__((packed)) {
  char name[10]; char short_name[4]; char licence[48]; uint8_t page2; uint8_t knob_count;
} rmp_manifest_head;
/* Format 3 (2026-10-10, sub-project 2): the head adds the page-2 title and the native presets'
   file extension; a knob adds its preset role and its range and default (0..127). Formats 1 and 2
   read with role free, min 0, max 127, default 64, no title, no native presets. */
typedef struct __attribute__((packed)) {
  uint8_t p, look, text, enum_first, enum_count, role, min, max, def, reserved; char long_name[12]; char short_name[4];
} rmp_knob3;
typedef struct __attribute__((packed)) {
  char name[10]; char short_name[4]; char licence[48]; uint8_t page2; uint8_t knob_count;
  char page2_title[13];   /* NUL-padded; "" = the machine's name */
  char preset_ext[7];     /* letters and digits, no dot, NUL-padded; "" = no native presets */
} rmp_manifest_head3;
/* Labels that follow a list knob (2026-10-26, plugins/mdkit: one machine whose TYPE knob picks a voice with its own
   controls). An optional block at the END of a format-3 manifest (inside manifest_size, after the name table), so the
   package stays format 3: an older host reads the manifest as before (it never looks past the name table) and shows the
   knobs' static labels; a host that knows the block shows, per value of the list knob `knob`, that value's long / short
   label for each P in p[]. Layout: knob (the list knob's P, 1..16), nsets (= that knob's names count, 1..16), nparams
   (1..RMP_LABELS_PARAMS), 0, p[nparams] (distinct, 1..16, not knob), then nsets x nparams entries of
   RMP_LABEL_BYTES (long 12, short 4, NUL-padded; an all-NUL entry: the P is unlabelled - inert - for that value), then
   the footer: u16 the block's length before the footer (little-endian), the magic "RLB1". tools/rmp_pack.py writes it
   from the manifest's "type_labels"; rmp_manifest_labels reads it. Which value is meant: the list knob's zone,
   (value * nsets) >> 7 for a value 0..127, as a plugin reads its list knob (plugins/noiseplethora/np_bank.cpp). */
#define RMP_LABELS_MAGIC 0x31424C52u   /* "RLB1" little-endian */
/* A list whose names follow another list knob (2026-10-27, plugins/mdkit's MD: ENGINE picks the voices TYPE lists).
   The same place and footer with the magic "RLB2"; layout: knob (the list knob the sets follow: TYPE), nsets (all
   values' names together, 1..RMP_LABELS_SETS), nparams, dep (the list knob that picks knob's names: ENGINE, 1..16, not
   knob, not in p[]), ndep (= dep's names count, 1..RMP_LABELS_DEPS), count[ndep] (knob's names count for each value of
   dep, 1..16, adding up to nsets), p[nparams], nsets names of RMP_ENUM_NAME_BYTES (dep value 0's first, then 1's, ...),
   then nsets x nparams label entries as in RLB1. For dep value v, knob has count[v] names in zones (value * count[v])
   >> 7, and its value z means set base[v] + z (base[v]: the counts before v). The knob's own static names (the name
   table) are what an older host shows: give it dep value 0's. */
#define RMP_LABELS_MAGIC2 0x32424C52u  /* "RLB2" little-endian */
#define RMP_LABELS_PARAMS 8
#define RMP_LABELS_SETS1 16            /* an RLB1 block's sets at most (one per name of its knob) */
#define RMP_LABELS_SETS 32             /* an RLB2 block's (the names of all of dep's values) */
#define RMP_LABELS_DEPS 16
#define RMP_LABEL_BYTES 16
#define RMP_LABELS_FOOTER 6
enum { RMP_LOOK_DIAL = 0, RMP_LOOK_CENTRED, RMP_LOOK_FADER, RMP_LOOK_LIST };
enum { RMP_TEXT_NUMBER = 0, RMP_TEXT_BIPOLAR, RMP_TEXT_ENUM, RMP_TEXT_LIST, RMP_TEXT_NOTE, RMP_TEXT_TIME,
       RMP_TEXT_PRESET, RMP_TEXT_POOL };
enum { RMP_ROLE_FREE = 0, RMP_ROLE_PRESET, RMP_ROLE_OFFSET };
#ifdef __cplusplus
static_assert(sizeof(rmp_header) == RMP_HEADER_BYTES, "rmp header size");
static_assert(sizeof(rmp_reloc) == 8, "rmp reloc size");
#else
_Static_assert(sizeof(rmp_header) == RMP_HEADER_BYTES, "rmp header size");
_Static_assert(sizeof(rmp_reloc) == 8, "rmp reloc size");
#endif
#ifdef __cplusplus
static_assert(sizeof(rmp_knob) == 22, "rmp knob size");
#else
_Static_assert(sizeof(rmp_knob) == 22, "rmp knob size");
#endif
#ifdef __cplusplus
static_assert(sizeof(rmp_knob3) == 26, "rmp knob3 size");
static_assert(sizeof(rmp_manifest_head3) == 84, "rmp manifest head3 size");
#else
_Static_assert(sizeof(rmp_knob3) == 26, "rmp knob3 size");
_Static_assert(sizeof(rmp_manifest_head3) == 84, "rmp manifest head3 size");
#endif
