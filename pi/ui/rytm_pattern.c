// pi/ui/rytm_pattern.c - see rytm_pattern.h
#include "rytm_pattern.h"
#include <string.h>

// track record offsets (docs/POLYPHONY.md)
#define O_TRIG		0x000		// 112 B: 14 bits a step, packed [measured]; bit order MSB first [inferred]
#define O_NOTE		0x070
#define O_VEL		0x0b0
#define O_LEN		0x0f0
#define O_MICRO		0x130
#define O_RTLEN		0x170
#define O_RTRATE	0x1b0
#define O_COND		0x1f0
#define O_DEFS		0x230
#define O_SNDLOCK	0x237
#define O_STEPS		0x276		// [inferred]
#define O_SPEED		0x277

static unsigned get14 (const unsigned char *b, int s)
{
	unsigned v = 0;
	for (int k = 0; k < 14; k++)
	{
		int bit = s * 14 + k;
		v = v << 1 | ((b[bit >> 3] >> (7 - (bit & 7))) & 1);
	}
	return v;
}

static void put14 (unsigned char *b, int s, unsigned v)
{
	for (int k = 0; k < 14; k++)
	{
		int bit = s * 14 + k;
		unsigned char m = (unsigned char) (1 << (7 - (bit & 7)));
		if ((v >> (13 - k)) & 1) b[bit >> 3] |= m; else b[bit >> 3] &= (unsigned char) ~m;
	}
}

int rytm_pattern_decode (const unsigned char *rec, int n, rytm_pattern_t *p)
{
	memset (p, 0, sizeof *p);
	if (n != RYTM_PAT_BYTES) return -1;
	for (int t = 0; t < RYTM_TRACKS; t++)
	{
		const unsigned char *r = rec + 4 + RYTM_TRACK_BYTES * t;
		rytm_track_t *k = &p->t[t];
		for (int s = 0; s < RYTM_STEPS; s++)
		{
			k->trig[s] = (unsigned short) get14 (r + O_TRIG, s);
			unsigned char nb = r[O_NOTE + s];
			k->note[s] = nb == 0xFF ? RYTM_DEFAULT : (unsigned char) (nb & 0x7F);
			k->vel[s] = r[O_VEL + s];
			k->len[s] = r[O_LEN + s];
			unsigned char m = r[O_MICRO + s];
			k->micro[s] = (signed char) ((m & 0x20) ? (int) (m & 0x3F) - 64 : (int) (m & 0x3F));
			k->mflags[s] = (unsigned char) (m >> 6);
			k->cond[s] = r[O_COND + s];
		}
		k->dnote = r[O_DEFS];
		k->dvel = r[O_DEFS + 1];
		k->dlen = r[O_DEFS + 2];
		k->steps = r[O_STEPS] >= 1 && r[O_STEPS] <= 64 ? r[O_STEPS] : 16;
		k->speed = r[O_SPEED] <= 6 ? r[O_SPEED] : 2;
	}
	const unsigned char *l = rec + RYTM_LOCKS_AT;
	for (int i = 0; i < RYTM_LOCKS; i++, l += RYTM_LOCK_BYTES)
	{
		if (l[0] == 0xFF) continue;
		p->nlocks++;
		if (l[0] == 0x80 && l[1] == 0x80)		// the previous slot's fine half
		{
			if (p->nparams) p->lock[p->nparams - 1].fine = 1;
			continue;
		}
		rytm_lock_t *k = &p->lock[p->nparams++];
		k->type = l[0];
		k->track = l[1];
		memcpy (k->val, l + 2, RYTM_STEPS);
	}
	memcpy (p->tail, rec + RYTM_PAT_BYTES - 12, 12);
	p->valid = 1;
	return 0;
}

void rytm_pattern_encode (const rytm_pattern_t *p, unsigned char *rec)
{
	memset (rec, 0, RYTM_PAT_BYTES);
	rec[3] = 5;						// version 5 [measured: pattern_load checks it]
	for (int t = 0; t < 13; t++)
	{
		unsigned char *r = rec + 4 + RYTM_TRACK_BYTES * t;
		memset (r + O_NOTE, 0xFF, 3 * RYTM_STEPS);
		memset (r + O_MICRO, 0xC0, RYTM_STEPS);
		memset (r + O_RTLEN, 0xAE, RYTM_STEPS);
		memset (r + O_RTRATE, 0xE9, RYTM_STEPS);
		memset (r + O_SNDLOCK, 0xFF, RYTM_STEPS);
		r[O_DEFS] = 0x3C; r[O_DEFS + 1] = 0x64; r[O_DEFS + 2] = 0x0A;
		r[O_STEPS] = 16; r[O_SPEED] = 2;
		if (t >= RYTM_TRACKS) continue;
		const rytm_track_t *k = &p->t[t];
		for (int s = 0; s < RYTM_STEPS; s++)
		{
			put14 (r + O_TRIG, s, k->trig[s] & 0x3FFF);
			r[O_NOTE + s] = k->note[s] == RYTM_DEFAULT ? 0xFF : (unsigned char) (0x80 | k->note[s]);
			r[O_VEL + s] = k->vel[s];
			r[O_LEN + s] = k->len[s];
			r[O_MICRO + s] = (unsigned char) ((k->micro[s] & 0x3F) | (k->mflags[s] & 3) << 6);
			r[O_COND + s] = k->cond[s];
		}
		r[O_DEFS] = k->dnote; r[O_DEFS + 1] = k->dvel; r[O_DEFS + 2] = k->dlen;
		r[O_STEPS] = k->steps; r[O_SPEED] = k->speed;
	}
	unsigned char *l = rec + RYTM_LOCKS_AT;
	int used = 0;
	for (int i = 0; i < p->nparams && used < RYTM_LOCKS; i++)
	{
		l[0] = p->lock[i].type; l[1] = p->lock[i].track;
		memcpy (l + 2, p->lock[i].val, RYTM_STEPS);
		l += RYTM_LOCK_BYTES; used++;
		if (p->lock[i].fine && used < RYTM_LOCKS)
		{
			l[0] = 0x80; l[1] = 0x80;
			memset (l + 2, 0, RYTM_STEPS);
			l += RYTM_LOCK_BYTES; used++;
		}
	}
	for (; used < RYTM_LOCKS; used++, l += RYTM_LOCK_BYTES) memset (l, 0xFF, RYTM_LOCK_BYTES);
	memcpy (rec + RYTM_PAT_BYTES - 12, p->tail, 12);
}

int rytm_sysex_unpack (const unsigned char *in, int n, unsigned char *out, int cap)
{
	int o = 0;
	for (int i = 0; i < n; i += 8)
	{
		unsigned char msb = in[i];
		for (int k = 1; k < 8 && i + k < n; k++)
		{
			if (o >= cap) return o;
			out[o++] = (unsigned char) (in[i + k] | ((msb << k) & 0x80));
		}
	}
	return o;
}

int rytm_sysex_pack (const unsigned char *in, int n, unsigned char *out, int cap)
{
	int o = 0;
	for (int i = 0; i < n; i += 7)
	{
		if (o >= cap) return o;
		int m = o++;
		out[m] = 0;
		for (int k = 0; k < 7 && i + k < n; k++)
		{
			if (o >= cap) return o;
			out[m] |= (unsigned char) ((in[i + k] >> 7) << (6 - k));
			out[o++] = in[i + k] & 0x7F;
		}
	}
	return o;
}

// The voice tracks' sound words in the order of the lock types [inferred from public parameter lists (the manual's
// pages: SYN, SMP, FLTR, AMP, LFO); to verify per type in rytm-emu before anything writes them]
static const char *const kLock[42] = {
	"SYN 1", "SYN 2", "SYN 3", "SYN 4", "SYN 5", "SYN 6", "SYN 7", "SYN 8",
	"SMP TUN", "SMP FIN", "SMP SMP", "SMP BR", "SMP STA", "SMP END", "SMP LOP", "SMP LEV",
	"FLT ATK", "FLT DEC", "FLT SUS", "FLT REL", "FLT FRQ", "FLT RES", "FLT TYP", "FLT ENV",
	"AMP ATK", "AMP HLD", "AMP DEC", "AMP OVR", "AMP DEL", "AMP REV", "AMP PAN", "AMP VOL",
	"LFO SPD", "LFO MUL", "LFO FAD", "LFO DST", "LFO WAV", "LFO PHS", "LFO MOD", "LFO DEP",
	"AMP ACC", "SYN MCH"};

const char *rytm_lock_name (int type)
{
	return type >= 0 && type < 42 ? kLock[type] : "?";
}

// The TRC values as the manual lists them [order public; the stored codes inferred: 1.. in this order]. No stdio
// (the Pi's Circle side has none).
static char *put (char *b, char *e, const char *t) { while (*t && b < e) *b++ = *t++; return b; }
static char *num (char *b, char *e, int v)
{
	char t[4];
	int n = 0;
	do t[n++] = (char) ('0' + v % 10); while ((v /= 10) && n < 4);
	while (n && b < e) *b++ = t[--n];
	return b;
}

const char *rytm_cond_name (int c, char *buf, int cap)
{
	static const char *const kFirst[8] = {"FILL", "!FILL", "PRE", "!PRE", "NEI", "!NEI", "1ST", "!1ST"};
	static const unsigned char kPct[21] = {1, 3, 4, 6, 9, 13, 19, 25, 33, 41, 50, 59, 67, 75, 81, 87, 91, 94, 96, 98, 99};
	char *b = buf, *e = buf + cap - 1;
	if (cap < 1) return buf;
	if (c <= 0) b = put (b, e, "-");
	else if (c <= 8) b = put (b, e, kFirst[c - 1]);
	else if (c <= 29) b = put (num (b, e, kPct[c - 9]), e, "%");
	else
	{
		int k = c - 30, d = 2;					// 1:2 2:2 1:3 2:3 3:3 ... 8:8
		while (d <= 8 && k >= d) k -= d++;
		if (d <= 8) b = num (put (num (b, e, k + 1), e, ":"), e, d);
		else b = num (put (b, e, "?"), e, c);
	}
	*b = 0;
	return buf;
}

const char *const *rytm_track_names (void)
{
	static const char *const kNames[RYTM_TRACKS] = {"BD", "SD", "RS", "CP", "BT", "LT", "MT", "HT", "CH", "OH", "CY", "CB"};
	return kNames;
}

int rytm_locks_on (const rytm_pattern_t *p, int t, int s)
{
	int n = 0;
	for (int i = 0; i < p->nparams; i++) n += p->lock[i].track == t && p->lock[i].val[s] != 0xFF;
	return n;
}
