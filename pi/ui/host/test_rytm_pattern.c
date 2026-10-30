// pi/ui/host/test_rytm_pattern.c - the Rytm pattern record (pi/ui/rytm_pattern.h): a made-up pattern (rytm_fake.c)
// encoded to the 13101-byte record and decoded back unchanged; the offsets docs/POLYPHONY.md measured (NOTE 0x80 | note
// at +0x70, the lock slots at +0x2091, the version byte); the 7-bit SysEx packing both ways; the display names.
// make -C pi/ui/host test
#include "../rytm_pattern.h"
#include <stdio.h>
#include <string.h>

void rytm_fake (rytm_pattern_t *p);

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf ("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main (void)
{
	static rytm_pattern_t a, b;
	static unsigned char rec[RYTM_PAT_BYTES], rec2[RYTM_PAT_BYTES], sx[RYTM_PAT_BYTES * 8 / 7 + 8], back[RYTM_PAT_BYTES];
	rytm_fake (&a);
	rytm_pattern_encode (&a, rec);
	CHECK (rec[3] == 5);
	// track 11 (CB) step 11: NOTE 63 stored as 0x80 | 63 at +4 + 641 * 11 + 0x70 + 11
	CHECK (rec[4 + 641 * 11 + 0x70 + 11] == (0x80 | 63));
	CHECK (rec[4 + 641 * 11 + 0x70 + 12] == 0xFF);
	CHECK (rec[RYTM_LOCKS_AT] == 1 && rec[RYTM_LOCKS_AT + 1] == 0);		// the first slot: BD's lock type 1
	CHECK (rec[RYTM_LOCKS_AT + 66 * 2] == 0x80 && rec[RYTM_LOCKS_AT + 66 * 2 + 1] == 0x80);	// SD's fine half
	CHECK (rec[RYTM_LOCKS_AT + 66 * 71] == 0xFF);					// the last slot free
	CHECK (rytm_pattern_decode (rec, RYTM_PAT_BYTES - 1, &b) == -1 && !b.valid);
	CHECK (rytm_pattern_decode (rec, RYTM_PAT_BYTES, &b) == 0 && b.valid);
	CHECK (b.nparams == a.nparams && b.nlocks == a.nlocks && b.nlocks == a.nparams + 1);
	CHECK (memcmp (a.t, b.t, sizeof a.t) == 0);
	CHECK (memcmp (a.lock, b.lock, sizeof a.lock[0] * a.nparams) == 0);
	CHECK (b.t[8].micro[1] == 4 && b.t[0].micro[7] == -3 && b.t[0].trig[0] == (RYTM_TRIG_ON | RYTM_TRIG_ACCENT));
	CHECK (b.t[4].trig[0] == RYTM_TRIG_LOCK && rytm_locks_on (&b, 4, 0) == 1 && rytm_locks_on (&b, 8, 2) == 2);
	rytm_pattern_encode (&b, rec2);
	CHECK (memcmp (rec, rec2, sizeof rec) == 0);
	// every 14-bit trig value survives its neighbours
	for (int s = 0; s < RYTM_STEPS; s++) a.t[3].trig[s] = (unsigned short) ((s * 2731 + 0x2A55) & 0x3FFF);
	rytm_pattern_encode (&a, rec);
	rytm_pattern_decode (rec, RYTM_PAT_BYTES, &b);
	CHECK (memcmp (a.t[3].trig, b.t[3].trig, sizeof a.t[3].trig) == 0);
	// 7-bit packing: 8 bytes for every 7, every byte < 0x80, back unchanged
	int n = rytm_sysex_pack (rec, RYTM_PAT_BYTES, sx, sizeof sx);
	CHECK (n == RYTM_PAT_BYTES / 7 * 8 + (RYTM_PAT_BYTES % 7 ? RYTM_PAT_BYTES % 7 + 1 : 0));
	int hi = 0;
	for (int i = 0; i < n; i++) hi |= sx[i] & 0x80;
	CHECK (!hi);
	CHECK (rytm_sysex_unpack (sx, n, back, sizeof back) == RYTM_PAT_BYTES && memcmp (back, rec, sizeof rec) == 0);
	// names
	char c[12];
	CHECK (!strcmp (rytm_cond_name (1, c, sizeof c), "FILL") && !strcmp (rytm_cond_name (19, c, sizeof c), "50%"));
	CHECK (!strcmp (rytm_cond_name (30, c, sizeof c), "1:2") && !strcmp (rytm_cond_name (31, c, sizeof c), "2:2"));
	CHECK (!strcmp (rytm_cond_name (64, c, sizeof c), "8:8") && !strcmp (rytm_cond_name (0, c, sizeof c), "-"));
	CHECK (!strcmp (rytm_lock_name (20), "FLT FRQ") && !strcmp (rytm_track_names ()[8], "CH"));
	printf (g_fail ? "test_rytm_pattern: %d FAILED\n" : "test_rytm_pattern: ok\n", g_fail);
	return g_fail != 0;
}
