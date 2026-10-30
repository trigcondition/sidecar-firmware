// pi/ui/rytm_pattern.h - a Rytm MKII pattern as the host sees it (docs/TOUCH-EDITOR.md part B): the stored pattern
// record (13101 B, version 5; the same bytes a SysEx pattern dump carries once 7-bit-unpacked) turned into per-step
// arrays for the read-only view (sidecar_rytmpat.h), and back (tests, and the write design). Pure C, no LVGL.
//
// The layout is docs/POLYPHONY.md's (measured in rytm-emu on OS 1.73, offsets per track record of 641 B at +4 + 641 t;
// 72 lock slots of 66 B at +0x2091; a 12-byte tail). What is only inferred is marked so below; nothing here is taken
// from the firmware image.
#ifndef RYTM_PATTERN_H
#define RYTM_PATTERN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RYTM_PAT_BYTES		13101
#define RYTM_TRACK_BYTES	641
#define RYTM_TRACKS		12		// the voice tracks (record 12 is the FX track: not shown)
#define RYTM_STEPS		64
#define RYTM_LOCKS		72
#define RYTM_LOCK_BYTES		66
#define RYTM_LOCKS_AT		0x2091		// 4 + 13 x 641
#define RYTM_DEFAULT		0xFF		// note / velocity / length: the track's default

// trig bits (14 a step) [measured: docs/POLYPHONY.md]
#define RYTM_TRIG_ON		0x0001
#define RYTM_TRIG_LOCK		0x0004		// a trigless lock
#define RYTM_TRIG_ACCENT	0x0008
#define RYTM_TRIG_SWING		0x0010
#define RYTM_TRIG_SLIDE		0x0020

typedef struct
{
	unsigned short	trig[RYTM_STEPS];	// the 14 trig bits
	unsigned char	note[RYTM_STEPS];	// MIDI note (stored 0x80 | note), RYTM_DEFAULT: the track's
	unsigned char	vel[RYTM_STEPS];	// 1..127, RYTM_DEFAULT
	unsigned char	len[RYTM_STEPS];	// the LEN code, RYTM_DEFAULT
	signed char	micro[RYTM_STEPS];	// 6-bit two's complement: -23..+23 in 1/24 step [unit inferred: the manual]
	unsigned char	mflags[RYTM_STEPS];	// the micro byte's top 2 bits [inferred: condition bits]
	unsigned char	cond[RYTM_STEPS];	// +0x1f0: 0 none, else the trig condition code [inferred]
	unsigned char	dnote, dvel, dlen;	// the track's defaults (+0x230)
	unsigned char	speed;			// 0..6 (+0x277) [measured]
	unsigned char	steps;			// the track's length, 1..64 (+0x276) [inferred]
} rytm_track_t;

typedef struct
{
	unsigned char	type;			// the parameter's storage slot 0..41 [measured]
	unsigned char	track;			// 0..12
	unsigned char	val[RYTM_STEPS];	// 0..127, 0xFF: no lock on that step
	unsigned char	fine;			// followed by a 0x80 0x80 fine continuation slot [measured]
} rytm_lock_t;

typedef struct
{
	int		valid;
	rytm_track_t	t[RYTM_TRACKS];
	int		nlocks;				// slots in use (a fine continuation counts: it takes a slot)
	int		nparams;			// of them, (track, parameter) pairs
	rytm_lock_t	lock[RYTM_LOCKS];		// the pairs (continuations folded into .fine)
	unsigned char	tail[12];			// master length, scale, kit [in use; layout not decoded]
} rytm_pattern_t;

// rec: the 13101-byte record. 0: decoded; -1: wrong size.
int rytm_pattern_decode (const unsigned char *rec, int n, rytm_pattern_t *p);
// The inverse (for tests and the write design): a record from p (the FX track and the tail zero).
void rytm_pattern_encode (const rytm_pattern_t *p, unsigned char *rec);
// Elektron's 7-bit SysEx packing: each group of up to 7 bytes after one byte of their MSBs. The bytes written.
int rytm_sysex_unpack (const unsigned char *in, int n, unsigned char *out, int cap);
int rytm_sysex_pack (const unsigned char *in, int n, unsigned char *out, int cap);
// Display names [inferred, to verify]: a lock type's parameter ("FLT FRQ"), a trig condition ("50%", "1:2", "FILL")
const char *rytm_lock_name (int type);
const char *rytm_cond_name (int cond, char *buf, int cap);
const char *const *rytm_track_names (void);	// "BD" "SD" ... "CB"
// the locks of track t on step s: how many parameters
int rytm_locks_on (const rytm_pattern_t *p, int t, int s);

#ifdef __cplusplus
}
#endif

#endif
