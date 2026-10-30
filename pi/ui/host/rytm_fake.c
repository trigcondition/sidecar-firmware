// pi/ui/host/rytm_fake.c - a made-up Rytm pattern for the host render and the tests (a 4-page groove with ghost notes,
// micro-timing, conditions, a fill, trigless locks and p-lock ramps). Synthesised here, not read from any firmware or
// project: only the record layout (rytm_pattern.h) is the Rytm's.
#include "../rytm_pattern.h"
#include <string.h>

enum { BD, SD, RS, CP, BT, LT, MT, HT, CH, OH, CY, CB };

static void trig (rytm_pattern_t *p, int t, int s, int vel, int micro, unsigned flags)
{
	rytm_track_t *k = &p->t[t];
	k->trig[s] = (unsigned short) (RYTM_TRIG_ON | flags);
	k->vel[s] = (unsigned char) vel;
	k->micro[s] = (signed char) micro;
}

static rytm_lock_t *lock (rytm_pattern_t *p, int t, int type)
{
	rytm_lock_t *l = &p->lock[p->nparams++];
	l->type = (unsigned char) type;
	l->track = (unsigned char) t;
	memset (l->val, 0xFF, sizeof l->val);
	p->nlocks++;
	return l;
}

void rytm_fake (rytm_pattern_t *p)
{
	memset (p, 0, sizeof *p);
	for (int t = 0; t < RYTM_TRACKS; t++)
	{
		rytm_track_t *k = &p->t[t];
		memset (k->note, RYTM_DEFAULT, sizeof k->note);
		memset (k->vel, RYTM_DEFAULT, sizeof k->vel);
		memset (k->len, RYTM_DEFAULT, sizeof k->len);
		k->dnote = 60; k->dvel = 100; k->dlen = 10; k->speed = 2; k->steps = 64;
	}
	p->t[CB].steps = 48;						// a shorter track: the rest shaded
	for (int pg = 0; pg < 4; pg++)
	{
		int b = pg * 16;
		trig (p, BD, b, 120, 0, RYTM_TRIG_ACCENT);
		trig (p, BD, b + 7, 96, -3, 0);
		trig (p, BD, b + 10, pg == 1 ? 70 : 104, 0, 0);
		if (pg == 2) trig (p, BD, b + 14, 58, 5, 0);
		trig (p, SD, b + 4, 110, 0, 0);
		trig (p, SD, b + 12, 118, 0, RYTM_TRIG_ACCENT);
		trig (p, SD, b + 15, 42, 7, 0);
		trig (p, RS, b + 3, 80, 0, 0);
		trig (p, RS, b + 11, 72, 0, 0);
		p->t[RS].cond[b + 11] = 30;				// 1:2
		if (pg != 3) trig (p, CP, b + 12, 96, 2, 0);
		for (int s = 0; s < 16; s++)
		{
			static const unsigned char kV[4] = {104, 52, 78, 60};
			trig (p, CH, b + s, kV[s % 4] - (pg == 3 && s > 8 ? 20 : 0), s % 2 ? 4 : 0, s % 4 == 0 ? RYTM_TRIG_ACCENT : 0);
			if (s % 4 == 3) p->t[CH].cond[b + s] = 22;		// 75%
		}
		trig (p, OH, b + 2, 88, 0, 0);
		trig (p, OH, b + 6, 84, 0, 0);
		trig (p, OH, b + 10, 92, 0, 0);
		trig (p, OH, b + 14, 100, 0, 0);
		p->t[OH].cond[b + 14] = 2;				// !FILL
		if (pg < 3)
		{
			trig (p, CB, b + 3, 90, -5, 0);
			trig (p, CB, b + 11, 84, -5, 0);
			trig (p, CB, b + 13, 76, 0, 0);
			p->t[CB].note[b + 11] = 63;
			p->t[CB].note[b + 13] = 66;
		}
	}
	p->t[BD].cond[26] = 19;						// 50%
	trig (p, CY, 0, 112, 0, RYTM_TRIG_ACCENT);
	p->t[CY].len[0] = 90;
	trig (p, CY, 32, 96, 0, 0);
	p->t[BT].trig[0] = RYTM_TRIG_LOCK;					// a trigless lock: the BT's tune for the bar
	p->t[BT].trig[32] = RYTM_TRIG_LOCK;
	static const struct { int t, s; } kFill[8] = {{LT, 52}, {LT, 54}, {LT, 57}, {MT, 58}, {MT, 60}, {HT, 61}, {HT, 62}, {HT, 63}};
	for (int i = 0; i < 8; i++)
	{
		trig (p, kFill[i].t, kFill[i].s, 80 + i * 5, i % 2 ? 3 : 0, 0);
		p->t[kFill[i].t].cond[kFill[i].s] = 1;			// FILL
	}
	for (int s = 56; s < 64; s++)					// the snare roll into the turnaround
	{
		trig (p, SD, s, 50 + (s - 56) * 9, 0, s == 63 ? RYTM_TRIG_ACCENT : 0);
		p->t[SD].cond[s] = s >= 60 ? 1 : 0;
	}
	p->t[SD].len[63] = 40;
	// p-locks
	rytm_lock_t *l = lock (p, BD, 1);				// SYN 2: the ghost kicks shorter
	for (int pg = 0; pg < 4; pg++) l->val[pg * 16 + 10] = 34;
	l->val[30] = 22;
	l = lock (p, SD, 20);						// FLT FRQ: the roll opens up (fine resolution)
	for (int s = 56; s < 64; s++) l->val[s] = (unsigned char) (40 + (s - 56) * 12);
	l->fine = 1;
	p->nlocks++;
	l = lock (p, BT, 0);						// SYN 1 on the trigless locks
	l->val[0] = 52; l->val[32] = 47;
	l = lock (p, CH, 26);						// AMP DEC: a saw over each bar
	for (int s = 0; s < 64; s++) l->val[s] = (unsigned char) (18 + (s % 16) * 5);
	l = lock (p, CH, 20);						// FLT FRQ on the offbeats
	for (int s = 2; s < 64; s += 4) l->val[s] = (unsigned char) (96 - (s / 16) * 10);
	l = lock (p, CH, 12);						// SMP STA on a few
	l->val[7] = 30; l->val[23] = 44; l->val[39] = 58; l->val[55] = 72;
	l = lock (p, CH, 30);						// AMP PAN: a sweep in the last bar
	for (int s = 48; s < 64; s++) l->val[s] = (unsigned char) (20 + (s - 48) * 6);
	l = lock (p, OH, 26);
	for (int pg = 0; pg < 4; pg++) l->val[pg * 16 + 14] = 76;
	l = lock (p, CB, 0);
	l->val[13] = 70; l->val[29] = 70; l->val[45] = 82;
	p->valid = 1;
}
