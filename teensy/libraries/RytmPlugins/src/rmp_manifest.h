// rmp_manifest.h - a package's manifest (formats 1..4; 4 is 3 with an RLB2 block) as a slot description, read through a
// streaming callback (the store's read_at: never the package whole in RAM). The Teensy serves
// 1A (metadata) and 1B (enum names) from it (sim/protocol.py). Portable: the Mac tests it.
#pragma once
#include <stdint.h>
#include "rmp_loader.h"
#include "rmp_format.h"
#define RMP_DESC_KNOBS 16
struct rmp_desc_knob {
  uint8_t p;                          // 1..16
  uint8_t look, kind, role;           // RMP_LOOK_*, RMP_TEXT_* (the manifest's "text"), RMP_ROLE_*
  uint8_t min, max, def;              // 0..127, min <= def <= max
  uint8_t enum_first, enum_count;     // into the package's name table (enum_count <= 16)
  char long_name[13], short_name[5];  // printable ASCII, NUL-padded and always NUL-terminated (12 / 4 characters)
};
struct rmp_desc {
  char name[11], short_name[5], p2_title[14];   // printable ASCII, NUL-padded and always NUL-terminated
                                                // (10 / 4 / 13 characters: the manifest's widths, plus the NUL)
  char preset_ext[8];                           // NUL-terminated, letters and digits; "" none
  uint8_t page2;                                // 1: P9..P16 are knobs ("params"), 0: the sample layer
  uint8_t nknobs;                               // knob[0..nknobs-1] in p order, each p once
  int8_t pre;                                   // the knob with kind preset (the first by p), -1 none
  uint8_t format;                               // the package's format, 1..4
  uint32_t names_at, names_end;                 // the name table's package offsets (0, 0: none)
  rmp_desc_knob knob[RMP_DESC_KNOBS];
};
// The manifest of a package whose header *info came from rmp_verify_header (format, image_size,
// reloc_count, manifest_size): false on a read error, a format outside 1..4, more than 32 knobs, or a
// manifest shorter than its head and knobs. Knobs with p outside 1..16, or a p seen before, are skipped;
// strings become printable ASCII ('_' for others), NUL-padded and NUL-terminated (every string field
// has one byte more than its manifest width). Format-3 min/max/default bytes above 127 saturate to
// 127; a range with min > max is read as 0..127 with the default 64; otherwise the default is clamped
// into min..max. Reads: the head, then one knob a read.
bool rmp_manifest_read(const rmp_info *info, rmp_read_fn rd, void *ctx, rmp_desc *d);
// Knob k's enum names (knob[k].enum_count of them) into names[][8], NUL-padded, at most 7 characters:
// the count, 0 when it has none, -1 on a read error or a name table that does not hold them.
int rmp_manifest_enum(const rmp_desc *d, int k, rmp_read_fn rd, void *ctx, char names[16][8]);
// The knob for P p (1..16): its index in knob[], -1 none.
int rmp_desc_find(const rmp_desc *d, int p);
// Labels that follow a list knob (rmp_format.h RMP_LABELS_*): per value of the list knob `knob`, the long / short
// label of each P in p[]. nsets 0: the package has none (or a malformed block: ignored, the static labels stand).
// dep (an RLB2 block): the list knob whose value picks knob's names - count[v] of them for dep value v, names[base[v]..]
// - and the set: base[v] + knob's zone among count[v]. dep 0: an RLB1 block (one set per name of knob, none of these).
struct rmp_labels {
  uint8_t knob, nsets, nparams;                                       // knob: the list knob's P (1..16)
  uint8_t dep, ndep;                                                  // RLB2: dep's P and names count (0, 0: RLB1)
  uint8_t count[RMP_LABELS_DEPS], base[RMP_LABELS_DEPS];              // knob's names for each value of dep
  uint8_t p[RMP_LABELS_PARAMS];                                       // the P each column labels
  char names[RMP_LABELS_SETS][RMP_ENUM_NAME_BYTES];                   // RLB2: knob's names, set order (NUL-padded, 7 at most)
  char lbl[RMP_LABELS_SETS][RMP_LABELS_PARAMS][RMP_LABEL_BYTES];      // [set][column]: long 12, short 4, NUL-padded
};
// The block at the end of d's manifest (d from rmp_manifest_read of the same package): true with *out filled, false
// (out->nsets 0) when there is none, the read failed, or it does not fit d - its knob not a list knob of d with nsets
// names (RLB1; RLB2: a list knob, dep a list knob of d with ndep names, the counts adding up to nsets), a P outside
// 1..16, twice, the list knob itself (or dep), or not one of d's knobs. Two reads (the footer, the block, into a static
// buffer: not reentrant; the control core's).
bool rmp_manifest_labels(const rmp_info *info, rmp_read_fn rd, void *ctx, const rmp_desc *d, rmp_labels *out);
// The set a value of the list knob means (0..nsets-1): (value * nsets) >> 7 of value 0..127 (NaN: 0); -1 none (or an
// RLB2 block, whose set needs dep's value too: rmp_labels_pick).
int rmp_labels_set(const rmp_labels *l, float value);
// The set the knob values p[0..15] (P1..P16) mean: RLB1 as rmp_labels_set(p[knob - 1]); RLB2 dep's zone v among ndep,
// then base[v] + knob's zone among count[v]. -1 none.
int rmp_labels_pick(const rmp_labels *l, const float *p);
// d's knobs in l->p[] take set's labels (an all-NUL entry: "" - the Rytm greys an unlabelled knob); RLB2: knob's
// enum_count becomes count[v] for the dep value v of set (1A's enum count: the zones the Rytm shows). False: nothing.
bool rmp_labels_apply(const rmp_labels *l, int set, rmp_desc *d);
// RLB2: knob's names for set's dep value (count[v] of them, into names[][8], NUL-padded) when p is knob; -1 otherwise
// (RLB1, another P, no set: the package's own name table, rmp_manifest_enum).
int rmp_labels_enum(const rmp_labels *l, int set, int p, char names[16][8]);
// A description without a manifest (a missing package): the name is the id's last dot-separated
// part in upper case, at most 10 characters (befaco.np.atari -> "ATARI"), the short name its first 4
// (both NUL-terminated); no knobs.
void rmp_desc_from_id(const char *id, rmp_desc *d);
