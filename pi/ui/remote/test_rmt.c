// pi/ui/remote/test_rmt.c - the protocol library: frames, hashes, every view's round trip, deltas, version skew, garbage.
#include "rmt_cli.h"
#include "rmt_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_n;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t s_Rng = 12345;
static uint32_t rnd (void) { s_Rng = s_Rng * 1664525u + 1013904223u; return s_Rng >> 8; }

// every byte random, then the strings made strings: whatever the struct has, the codec must carry it
static void fill_random (void *p, size_t n) { uint8_t *b = (uint8_t *) p; for (size_t i = 0; i < n; i++) b[i] = (uint8_t) rnd (); }

// ---- frames ------------------------------------------------------------------------------------------------------

static void test_frames (void)
{
	uint8_t buf[RMT_MAX_FRAME], pay[100];
	for (int i = 0; i < 100; i++) pay[i] = (uint8_t) i;
	size_t n = rmt_frame (buf, sizeof buf, RMT_CMD, 0xA1B2C3D4u, pay, 100);
	CHECK (n == RMT_HDR + 100);
	CHECK (buf[0] == RMT_CMD && buf[2] == 100 && buf[3] == 0 && buf[4] == 0xD4 && buf[7] == 0xA1);	// little-endian
	CHECK (rmt_frame (buf, 50, RMT_CMD, 1, pay, 100) == 0);					// does not fit
	CHECK (rmt_frame (buf, sizeof buf, RMT_CMD, 1, pay, RMT_MAX_PAYLOAD + 1) == 0);

	// two frames fed one byte at a time, then in one lump, then split in the middle of a header
	uint8_t two[2 * RMT_MAX_FRAME];
	size_t n1 = rmt_frame (two, sizeof two, RMT_PING, 7, pay, 4);
	size_t n2 = rmt_frame (two + n1, sizeof two - n1, RMT_CMD, 8, pay, 100);
	for (int mode = 0; mode < 3; mode++)
	{
		rmt_rx_t rx;
		rmt_rx_init (&rx);
		int got = 0;
		size_t pos = 0, total = n1 + n2;
		while (pos < total)
		{
			size_t chunk = mode == 0 ? 1 : mode == 1 ? total : 5;
			if (chunk > total - pos) chunk = total - pos;
			size_t took = rmt_rx_put (&rx, two + pos, chunk);
			pos += took;
			rmt_hdr_t h;
			const uint8_t *p;
			while (rmt_rx_next (&rx, &h, &p))
			{
				if (got == 0) CHECK (h.type == RMT_PING && h.seq == 7 && h.len == 4 && p[3] == 3);
				if (got == 1) CHECK (h.type == RMT_CMD && h.seq == 8 && h.len == 100 && p[99] == 99);
				got++;
			}
			if (took == 0) break;
		}
		CHECK (got == 2 && !rx.err);
	}
	// a length past the limit is an error, not an allocation
	rmt_rx_t rx;
	rmt_rx_init (&rx);
	uint8_t bad[RMT_HDR] = {RMT_VIEW, 0, 0xFF, 0xFF, 0, 0, 0, 0};
	rmt_rx_put (&rx, bad, sizeof bad);
	CHECK (rx.err);
	// reader past the end: zeros and the error flag
	rmt_r r;
	rmt_r_init (&r, pay, 3);
	rmt_r_u16 (&r);
	CHECK (!r.err);
	CHECK (rmt_r_u32 (&r) == 0 && r.err);
	CHECK (rmt_cmd_ok ("vparam=1:2:3", 12) && !rmt_cmd_ok ("a\nb", 3) && !rmt_cmd_ok ("", 0));
	char big[80];
	memset (big, 'a', sizeof big);
	CHECK (rmt_cmd_ok (big, 63) && !rmt_cmd_ok (big, 64));
}

// ---- SHA-256 / HMAC ---------------------------------------------------------------------------------------------------

static int hex_eq (const uint8_t *d, const char *hex)
{
	for (int i = 0; i < 32; i++)
	{
		char t[3];
		snprintf (t, sizeof t, "%02x", d[i]);
		if (strncmp (t, hex + 2 * i, 2) != 0) return 0;
	}
	return 1;
}

static void test_sha (void)
{
	rmt_sha256_t c;
	uint8_t d[32];
	rmt_sha256_init (&c), rmt_sha256_update (&c, "abc", 3), rmt_sha256_final (&c, d);
	CHECK (hex_eq (d, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	rmt_sha256_init (&c), rmt_sha256_final (&c, d);
	CHECK (hex_eq (d, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	// 1,000,000 x 'a' in odd pieces
	rmt_sha256_init (&c);
	char blk[777];
	memset (blk, 'a', sizeof blk);
	size_t left = 1000000;
	while (left) { size_t k = left < sizeof blk ? left : sizeof blk; rmt_sha256_update (&c, blk, k); left -= k; }
	rmt_sha256_final (&c, d);
	CHECK (hex_eq (d, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
	// RFC 4231 case 1 and case 6 (a key longer than the block)
	uint8_t key[131];
	memset (key, 0x0b, 20);
	rmt_hmac_sha256 (key, 20, "Hi There", 8, d);
	CHECK (hex_eq (d, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
	memset (key, 0xaa, 131);
	rmt_hmac_sha256 (key, 131, "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);
	CHECK (hex_eq (d, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
	uint8_t nonce[16] = {1}, p1[32], p2[32];
	rmt_auth_proof ("12345678", nonce, 9, p1);
	rmt_auth_proof ("12345679", nonce, 9, p2);
	CHECK (!rmt_ct_equal (p1, p2, 32));
	rmt_auth_proof ("12345678", nonce, 10, p2);
	CHECK (!rmt_ct_equal (p1, p2, 32));
	rmt_auth_proof ("12345678", nonce, 9, p2);
	CHECK (rmt_ct_equal (p1, p2, 32));
}

// ---- views --------------------------------------------------------------------------------------------------------------

typedef struct { uint8_t data[RMT_MAX_SI][RMT_SEC_BUF]; int len[RMT_MAX_SI], sec[RMT_MAX_SI], inst[RMT_MAX_SI]; int n; } sink_t;
static void sink_fn (void *ud, int view, int sec, int inst, const uint8_t *data, int len)
{
	(void) view;
	sink_t *s = (sink_t *) ud;
	s->sec[s->n] = sec, s->inst[s->n] = inst, s->len[s->n] = len;
	memcpy (s->data[s->n], data, (size_t) len);
	s->n++;
}

// the total wire bytes of every section instance of a view (the sum of its fields' sizes: pinned below)
static int wire_total (int view, const void *v)
{
	const rmt_view_info_t *vi = rmt_view_info (view);
	int tot = 0;
	uint8_t b[RMT_SEC_BUF];
	for (int s = 0; s < vi->nsec; s++)
		for (int i = 0; i < rmt_sec_info (view, s)->ninst; i++) tot += rmt_sec_encode (view, s, i, v, b, sizeof b);
	return tot;
}

static void test_views (void)
{
	// pinned wire sizes: a field added to a view struct and not to the table fails here (and the schema hash moves)
	static const int kWire[RMT_V_COUNT] = {0, 586, 782, 2570, 425, 9840, 4, 11156};
	for (int view = 1; view < RMT_V_COUNT; view++)
	{
		const rmt_view_info_t *vi = rmt_view_info (view);
		CHECK (vi != 0);
		CHECK (rmt_view_nsi (view) <= RMT_MAX_SI);
		uint8_t *a = (uint8_t *) calloc (1, vi->size), *b = (uint8_t *) calloc (1, vi->size);
		fill_random (a, vi->size);
		sink_t *sk = (sink_t *) calloc (1, sizeof *sk);
		rmt_vtx_t t;
		rmt_vtx_reset (&t);
		int n = rmt_vtx_emit (&t, view, a, sink_fn, sk, 0);
		CHECK (n == rmt_view_nsi (view));
		for (int i = 0; i < sk->n; i++) CHECK (rmt_sec_decode (view, sk->sec[i], sk->inst[i], b, sk->data[i], sk->len[i]) == 0);
		// round trip: what the decoder holds encodes to the same bytes (padding and gen are not on the wire)
		sink_t *sk2 = (sink_t *) calloc (1, sizeof *sk2);
		rmt_vtx_t t2;
		rmt_vtx_reset (&t2);
		rmt_vtx_emit (&t2, view, b, sink_fn, sk2, 0);
		CHECK (sk2->n == sk->n);
		for (int i = 0; i < sk->n && i < sk2->n; i++) CHECK (sk->len[i] == sk2->len[i] && !memcmp (sk->data[i], sk2->data[i], (size_t) sk->len[i]));
		// the wire carries every member (padding and gen aside; the sizes are checked against the struct's holes by hand)
		int tot = wire_total (view, a);
		CHECK (tot == kWire[view]);
		printf ("  view %s: struct %zu B, wire %d B, %d sections\n", vi->name, vi->size, tot, n);
		for (int i = 0; i < sk->n; i++) CHECK (sk->len[i] <= 1024);
		free (a), free (b), free (sk), free (sk2);
	}
}

static void test_delta (void)
{
	sidecar_voice_view_t v;
	memset (&v, 0, sizeof v);
	rmt_vtx_t t;
	rmt_vtx_reset (&t);
	sink_t *sk = (sink_t *) calloc (1, sizeof *sk);
	CHECK (rmt_vtx_emit (&t, RMT_V_VOICE, &v, sink_fn, sk, 0) == rmt_view_nsi (RMT_V_VOICE));	// first: everything
	sk->n = 0;
	CHECK (rmt_vtx_emit (&t, RMT_V_VOICE, &v, sink_fn, sk, 0) == 0);			// nothing changed: nothing sent
	v.val[3] = 99;										// a knob: the head only
	CHECK (rmt_vtx_emit (&t, RMT_V_VOICE, &v, sink_fn, sk, 0) == 1 && sk->sec[0] == 0);
	sk->n = 0;
	v.smax[10] = 5, v.level = 40;								// the scope: its (hot) section only
	CHECK (rmt_vtx_emit (&t, RMT_V_VOICE, &v, sink_fn, sk, 0) == 1 && sk->sec[0] == 1);
	sk->n = 0;
	v.val[0] = 1, v.smin[0] = -3;								// a hot_only pass leaves the cold change for later
	CHECK (rmt_vtx_emit (&t, RMT_V_VOICE, &v, sink_fn, sk, 1) == 1 && sk->sec[0] == 1);
	sk->n = 0;
	CHECK (rmt_vtx_emit (&t, RMT_V_VOICE, &v, sink_fn, sk, 0) == 1 && sk->sec[0] == 0);	// ... and the full pass sends it
	sk->n = 0;
	rmt_vtx_reset (&t);									// RESYNC
	CHECK (rmt_vtx_emit (&t, RMT_V_VOICE, &v, sink_fn, sk, 0) == rmt_view_nsi (RMT_V_VOICE));
	// an indexed section: one slot changed, one section instance sent
	sidecar_insfx_view_t iv;
	memset (&iv, 0, sizeof iv);
	rmt_vtx_reset (&t);
	sk->n = 0;
	rmt_vtx_emit (&t, RMT_V_INSFX, &iv, sink_fn, sk, 0);
	sk->n = 0;
	iv.slot[4][1].v[2] = 77;
	CHECK (rmt_vtx_emit (&t, RMT_V_INSFX, &iv, sink_fn, sk, 0) == 1 && sk->sec[0] == 1 && sk->inst[0] == 4);
	free (sk);
}

// the sequencer's views: a note edit is one notes section and the header, the lanes and the play head are their own, the pattern's tracks and lock slots too
static void test_seq_deltas (void)
{
	sidecar_seq_view_t *v = (sidecar_seq_view_t *) calloc (1, sizeof *v);
	sink_t *sk = (sink_t *) calloc (1, sizeof *sk);
	rmt_vtx_t t;
	rmt_vtx_reset (&t);
	v->valid = 1, v->track = 2, v->n = 3;
	for (int i = 0; i < 3; i++) v->note[i].step = (unsigned short) (i * 4), v->note[i].pitch = (unsigned char) (60 + i), v->note[i].vel = 100, v->note[i].len = 96;
	CHECK (rmt_view_nsi (RMT_V_SEQ) == 3 + SEQED_NOTES / 64 && rmt_view_nsi (RMT_V_SEQ) <= RMT_MAX_SI);
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0) == rmt_view_nsi (RMT_V_SEQ));
	sk->n = 0;
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0) == 0);					// nothing changed: nothing sent
	v->note[1].vel = 50;										// a velocity edit: the first notes section only
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0) == 1 && sk->sec[0] == 3 && sk->inst[0] == 0);
	sk->n = 0;
	v->note[900].pitch = 33;									// stale bytes past n never move a hash (the notes are zeros there)
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0) == 0);
	v->n = 4, v->note[3].step = 70, v->note[3].pitch = 64;						// a note added: the header (n) and the section holding it
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0) == 2 && sk->sec[0] == 0 && sk->sec[1] == 3 && sk->inst[1] == 0);
	sk->n = 0;
	v->n = 130, v->note[129].step = 5, v->note[129].pitch = 1, v->bpm10 = 1200;			// note 129 is in the third section
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0) == 2 && sk->sec[0] == 0 && sk->sec[1] == 3 && sk->inst[1] == 2);	// the header and the third section only
	sk->n = 0;
	v->locks[5] = 2;										// a lane: lanes1 alone
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0) == 1 && sk->sec[0] == 1);
	sk->n = 0;
	v->arpl[100] = 7;										// lanes2
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0) == 1 && sk->sec[0] == 2);
	// a decoder fed the whole view holds the same notes
	sidecar_seq_view_t *d = (sidecar_seq_view_t *) calloc (1, sizeof *d);
	rmt_vtx_reset (&t);
	sk->n = 0;
	rmt_vtx_emit (&t, RMT_V_SEQ, v, sink_fn, sk, 0);
	for (int i = 0; i < sk->n; i++) rmt_sec_decode (RMT_V_SEQ, sk->sec[i], sk->inst[i], d, sk->data[i], sk->len[i]);
	CHECK (d->n == 130 && d->note[129].step == 5 && d->note[1].vel == 50 && d->note[3].pitch == 64 && d->note[900].pitch == 0 && d->locks[5] == 2 && d->arpl[100] == 7);
	CHECK (d->bpm10 == 1200 && d->track == 2 && d->gen > 0);
	free (v), free (sk), free (d);

	rmt_seqpos_view_t p = {-1, 0};									// the play head: its own tiny view
	sk = (sink_t *) calloc (1, sizeof *sk);
	rmt_vtx_reset (&t);
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQPOS, &p, sink_fn, sk, 0) == 1 && sk->len[0] == 4);
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQPOS, &p, sink_fn, sk, 0) == 0);
	p.pos32 = 32 * 7 + 5;
	CHECK (rmt_vtx_emit (&t, RMT_V_SEQPOS, &p, sink_fn, sk, 0) == 1);
	free (sk);

	rmt_pat_view_t *pv = (rmt_pat_view_t *) calloc (1, sizeof *pv);					// PAT: the tracks, 8 lock slots a section
	sk = (sink_t *) calloc (1, sizeof *sk);
	rmt_vtx_reset (&t);
	pv->pat.valid = 1, pv->pat.nlocks = 20, pv->index = 5;
	CHECK (rmt_view_nsi (RMT_V_PAT) == 2 + RYTM_TRACKS + RYTM_LOCKS / 8 && rmt_view_nsi (RMT_V_PAT) <= RMT_MAX_SI);
	rmt_vtx_emit (&t, RMT_V_PAT, pv, sink_fn, sk, 0);
	sk->n = 0;
	pv->pat.t[7].trig[3] = 1;									// one track: one section instance
	CHECK (rmt_vtx_emit (&t, RMT_V_PAT, pv, sink_fn, sk, 0) == 1 && sk->sec[0] == 2 && sk->inst[0] == 7);
	sk->n = 0;
	pv->pat.lock[10].val[4] = 99;									// slot 10 is in the second lock section
	CHECK (rmt_vtx_emit (&t, RMT_V_PAT, pv, sink_fn, sk, 0) == 1 && sk->sec[0] == 3 && sk->inst[0] == 1);
	sk->n = 0;
	pv->pat.lock[60].val[4] = 99;									// past nlocks: not on the wire
	CHECK (rmt_vtx_emit (&t, RMT_V_PAT, pv, sink_fn, sk, 0) == 0);
	snprintf (pv->where, sizeof pv->where, "from the Rytm 3 s ago");				// the texts: their own section
	CHECK (rmt_vtx_emit (&t, RMT_V_PAT, pv, sink_fn, sk, 0) == 1 && sk->sec[0] == 1);
	free (pv), free (sk);
}

// STATUS 1.1 appends bpm10 to the live section: it round-trips, an old Pi (the data stops before it) leaves it as it was, and
// the schema says 1.1 (major 1: an old app still takes the view and ignores the new field)
static void test_status_bpm10 (void)
{
	static sidecar_status_view_t a, b;
	memset (&a, 0, sizeof a), memset (&b, 0, sizeof b);
	a.valid = 1, a.bpm10 = 1215;
	uint8_t buf[RMT_SEC_BUF];
	int n = rmt_sec_encode (RMT_V_STATUS, 0, 0, &a, buf, sizeof buf);
	CHECK (n > 4 && rmt_view_info (RMT_V_STATUS)->schema == 0x0101);
	CHECK (rmt_sec_decode (RMT_V_STATUS, 0, 0, &b, buf, n) == 0 && b.bpm10 == 1215 && b.valid == 1);
	b.bpm10 = 777;
	CHECK (rmt_sec_decode (RMT_V_STATUS, 0, 0, &b, buf, n - 4) == 0 && b.bpm10 == 777 && b.valid == 1);
}

static void test_skew (void)
{
	sidecar_mfx_view_t a, b;
	memset (&a, 0, sizeof a), memset (&b, 0, sizeof b);
	for (int i = 0; i < 4; i++) a.slot[i].type = 100 + i, a.slot[i].v[0] = (unsigned char) (10 + i);
	a.valid = 1, a.lev = 90, a.in = 11, a.out = 22;
	uint8_t buf[RMT_SEC_BUF];
	int n = rmt_sec_encode (RMT_V_MFX, 0, 0, &a, buf, sizeof buf);
	CHECK (n > 8);
	// an older sender: the data stops after valid / on / byp: the rest of the struct stays as it was
	b.lev = 33;
	unsigned g0 = b.gen;
	CHECK (rmt_sec_decode (RMT_V_MFX, 0, 0, &b, buf, 4 + 2) == 0);
	CHECK (b.valid == 1 && b.on == a.on && b.byp == a.byp && b.mix == 0 && b.lev == 33 && b.gen == g0 + 1);
	// a newer sender: bytes past the last field are ignored
	uint8_t big[RMT_SEC_BUF + 50];
	memcpy (big, buf, (size_t) n);
	memset (big + n, 0xEE, 50);
	memset (&b, 0, sizeof b);
	CHECK (rmt_sec_decode (RMT_V_MFX, 0, 0, &b, big, n + 50) == 0 && b.lev == 90);
	// unknown view, section, instance: refused, the struct untouched
	memset (&b, 0, sizeof b);
	CHECK (rmt_sec_decode (99, 0, 0, &b, buf, n) == -1);
	CHECK (rmt_sec_decode (RMT_V_MFX, 9, 0, &b, buf, n) == -1);
	CHECK (rmt_sec_decode (RMT_V_MFX, 2, 4, &b, buf, n) == -1);
	CHECK (rmt_sec_decode (RMT_V_MFX, 2, 3, &b, buf, 0) == 0 && b.slot[3].type == 0);	// empty data: nothing applied
	CHECK (rmt_view_info (0) == 0 && rmt_view_info (RMT_V_COUNT) == 0 && rmt_view_info (200) == 0);
	CHECK (rmt_schema_hash () == rmt_schema_hash () && rmt_schema_hash () != 0);
}

// ---- the client against hand-made Pi messages -------------------------------------------------------------------------

typedef struct { uint8_t out[8192]; size_t n; int pages, last_page, results, last_ok; uint32_t last_seq; } net_t;
static int net_send (void *ud, const uint8_t *d, size_t n)
{
	net_t *t = (net_t *) ud;
	if (t->n + n > sizeof t->out) return -1;
	memcpy (t->out + t->n, d, n), t->n += n;
	return 0;
}
static void net_page (void *ud, int page) { net_t *t = (net_t *) ud; t->pages++, t->last_page = page; }
static void net_result (void *ud, uint32_t seq, int ok, int code, const char *why) { net_t *t = (net_t *) ud; (void) code; (void) why; t->results++, t->last_ok = ok, t->last_seq = seq; }

static size_t welcome (uint8_t *buf, int proto, int flags, int schema_voice, int with_unknown)
{
	uint8_t b[128];
	rmt_w w;
	rmt_w_init (&w, b, sizeof b);
	rmt_w_u16 (&w, (unsigned) proto), rmt_w_u16 (&w, (unsigned) flags), rmt_w_u32 (&w, 0xCAFE);
	uint8_t nonce[16];
	for (int i = 0; i < 16; i++) nonce[i] = (uint8_t) (i * 3);
	rmt_w_bytes (&w, nonce, 16), rmt_w_u32 (&w, 1), rmt_w_u8 (&w, 8);
	rmt_w_u8 (&w, 5), rmt_w_bytes (&w, "0.9.0", 5);
	rmt_w_u8 (&w, (unsigned) (with_unknown ? 4 : 3));
	rmt_w_u8 (&w, RMT_V_VOICE), rmt_w_u16 (&w, (unsigned) schema_voice), rmt_w_u8 (&w, 10);
	rmt_w_u8 (&w, RMT_V_INSFX), rmt_w_u16 (&w, 0x0100), rmt_w_u8 (&w, 10);
	rmt_w_u8 (&w, RMT_V_STATUS), rmt_w_u16 (&w, 0x0107), rmt_w_u8 (&w, 2);		// a newer minor: fine
	if (with_unknown) rmt_w_u8 (&w, 77), rmt_w_u16 (&w, 0x0100), rmt_w_u8 (&w, 10);
	rmt_w_u8 (&w, 0xAB);									// trailing bytes a newer Pi may add
	return rmt_frame (buf, RMT_MAX_FRAME, RMT_WELCOME, 1, b, w.n);
}

static void test_client (void)
{
	static net_t net;
	static rmt_cli_t c;
	memset (&net, 0, sizeof net);
	rmt_cli_cfg_t cfg = {net_send, &net, 123, 1280, 720, 1, "12345678", net_result, net_page};
	rmt_cli_init (&c, &cfg);
	rmt_cli_want (&c, RMT_V_VOICE, 255);
	rmt_cli_want (&c, RMT_V_MFX, 10);							// the Pi will not offer mfx
	CHECK (rmt_cli_cmd (&c, "vparam=1:2:3") == 0);					// not ready: refused here
	rmt_cli_connected (&c, 1000);
	CHECK (c.state == RMT_CLI_HELLO && net.n > RMT_HDR && !memcmp (net.out + RMT_HDR, "SCRM", 4));
	net.n = 0;
	uint8_t f[RMT_MAX_FRAME];
	size_t n = welcome (f, RMT_PROTO, 0, 0x0102, 1);
	CHECK (rmt_cli_feed (&c, f, n, 1010) == 0 && rmt_cli_ready (&c));
	CHECK (c.pi_page == 8 && !strcmp (c.fw, "0.9.0"));
	CHECK (rmt_cli_view_ok (&c, RMT_V_VOICE) && rmt_cli_view_ok (&c, RMT_V_INSFX) && rmt_cli_view_ok (&c, RMT_V_STATUS));
	CHECK (!rmt_cli_view_ok (&c, RMT_V_MFX) && !rmt_cli_view_ok (&c, 77));
	CHECK (net.n == RMT_HDR + 2 && net.out[0] == RMT_SUB && net.out[RMT_HDR] == RMT_V_VOICE && net.out[RMT_HDR + 1] == 0);	// one SUB: voice
	net.n = 0;
	// a VIEW frame, in two pieces
	sidecar_voice_view_t src;
	memset (&src, 0, sizeof src);
	strcpy (src.name, "BD PLASTIC");
	src.val[2] = 77, src.level = 100, src.track = 5;
	uint8_t sec[RMT_SEC_BUF + 4];
	sec[0] = RMT_V_VOICE, sec[1] = 0, sec[2] = 0, sec[3] = 0;
	int sl = rmt_sec_encode (RMT_V_VOICE, 0, 0, &src, sec + 4, RMT_SEC_BUF);
	n = rmt_frame (f, sizeof f, RMT_VIEW, 2, sec, (size_t) sl + 4);
	CHECK (rmt_cli_feed (&c, f, 9, 1020) == 0 && !rmt_cli_take (&c, RMT_V_VOICE));
	CHECK (rmt_cli_feed (&c, f + 9, n - 9, 1021) == 0 && rmt_cli_take (&c, RMT_V_VOICE) && !rmt_cli_take (&c, RMT_V_VOICE));
	CHECK (!strcmp (c.voice.name, "BD PLASTIC") && c.voice.val[2] == 77 && c.voice.track == 5 && c.voice.gen == 1);
	// a view the Pi did not offer, an unknown view, an unknown message type, a PAGE, a PING
	sec[0] = RMT_V_MFX;
	n = rmt_frame (f, sizeof f, RMT_VIEW, 3, sec, (size_t) sl + 4);
	CHECK (rmt_cli_feed (&c, f, n, 1030) == 0 && !rmt_cli_take (&c, RMT_V_MFX));
	sec[0] = 77;
	n = rmt_frame (f, sizeof f, RMT_VIEW, 3, sec, (size_t) sl + 4);
	CHECK (rmt_cli_feed (&c, f, n, 1030) == 0);
	n = rmt_frame (f, sizeof f, 200, 3, "xyz", 3);
	CHECK (rmt_cli_feed (&c, f, n, 1030) == 0 && rmt_cli_ready (&c));
	uint8_t pg = 9;
	n = rmt_frame (f, sizeof f, RMT_PAGE, 4, &pg, 1);
	CHECK (rmt_cli_feed (&c, f, n, 1031) == 0 && net.pages == 1 && net.last_page == 9 && c.pi_page == 9);
	uint32_t tok = 0x01020304;
	n = rmt_frame (f, sizeof f, RMT_PING, 5, &tok, 4);
	net.n = 0;
	CHECK (rmt_cli_feed (&c, f, n, 1032) == 0 && net.n == RMT_HDR + 4 && net.out[0] == RMT_PONG && !memcmp (net.out + RMT_HDR, &tok, 4));
	// a command and its answers
	net.n = 0;
	int seq = rmt_cli_cmd (&c, "vparam=5:1:64");
	CHECK (seq > 0 && net.out[0] == RMT_CMD && !memcmp (net.out + RMT_HDR, "vparam=5:1:64", 13));
	CHECK (rmt_cli_cmd (&c, "bad\nline") == 0);
	uint8_t ok[4] = {(uint8_t) seq, 0, 0, 0};
	n = rmt_frame (f, sizeof f, RMT_OK, 6, ok, 4);
	CHECK (rmt_cli_feed (&c, f, n, 1040) == 0 && net.results == 1 && net.last_ok == 1 && net.last_seq == (uint32_t) seq);
	uint8_t er[8] = {(uint8_t) seq, 0, 0, 0, RMT_E_CMD, 0, 'n', 'o'};
	n = rmt_frame (f, sizeof f, RMT_ERR, 7, er, 8);
	CHECK (rmt_cli_feed (&c, f, n, 1041) == 0 && net.results == 2 && net.last_ok == 0);
	// silence: three missed pings and it is dead
	CHECK (rmt_cli_tick (&c, 1041 + 10000) == 0 && rmt_cli_tick (&c, 1041 + RMT_CLI_DEAD_MS + 1) == -1);
	// lost: views forgotten, the wish for views kept; the next connection starts clean
	rmt_cli_lost (&c);
	CHECK (c.state == RMT_CLI_IDLE && c.voice.gen == 0 && c.voice.name[0] == 0 && c.want[RMT_V_VOICE] == 255 && !rmt_cli_view_ok (&c, RMT_V_VOICE));

	// a major schema mismatch greys the view; a protocol mismatch is refused
	memset (&net, 0, sizeof net);
	rmt_cli_connected (&c, 0);
	n = welcome (f, RMT_PROTO, 0, 0x0200, 0);
	CHECK (rmt_cli_feed (&c, f, n, 1) == 0 && !rmt_cli_view_ok (&c, RMT_V_VOICE) && rmt_cli_view_ok (&c, RMT_V_INSFX));
	rmt_cli_connected (&c, 0);
	n = welcome (f, 2, 0, 0x0100, 0);
	CHECK (rmt_cli_feed (&c, f, n, 1) == -1 && c.state == RMT_CLI_REFUSED && c.refused_code == RMT_E_VERSION);

	// the reason for a refusal outlives the lost connection (the overlay says it), and is gone once a connection is ready
	{
		static const int kCodes[3] = {RMT_E_BUSY, RMT_E_VERSION, RMT_E_AUTH};
		for (int i = 0; i < 3; i++)
		{
			rmt_cli_connected (&c, 0);
			uint8_t bye[2] = {(uint8_t) kCodes[i], 0};
			n = rmt_frame (f, sizeof f, RMT_BYE, 3, bye, 2);
			CHECK (rmt_cli_feed (&c, f, n, 1) == -1 && c.refused_code == kCodes[i]);
			rmt_cli_lost (&c);
			CHECK (c.refused_code == kCodes[i] && c.state == RMT_CLI_IDLE);
		}
		rmt_cli_connected (&c, 0);					// the next try: still shown while it runs ...
		CHECK (c.refused_code == RMT_E_AUTH);
		n = welcome (f, RMT_PROTO, 0, 0x0100, 0);
		CHECK (rmt_cli_feed (&c, f, n, 1) == 0 && rmt_cli_ready (&c) && c.refused_code == 0);	// ... gone when it works
	}
	// a SUB that did not go out is sent again at the next tick
	{
		static net_t n2;
		memset (&n2, 0, sizeof n2);
		rmt_cli_cfg_t cfg2 = {net_send, &n2, 1, 800, 480, 1, 0, 0, 0};
		rmt_cli_init (&c, &cfg2);
		rmt_cli_connected (&c, 0);
		n = welcome (f, RMT_PROTO, 0, 0x0100, 0);
		rmt_cli_feed (&c, f, n, 1);
		n2.n = sizeof n2.out - 3;					// no room: the send fails
		rmt_cli_want (&c, RMT_V_VOICE, 10);
		CHECK (c.resub);
		n2.n = 0;
		CHECK (rmt_cli_tick (&c, 5) == 0 && !c.resub);
		CHECK (n2.n == (RMT_HDR + 2) + 2 * (RMT_HDR + 1));		// the wanted view's SUB, an UNSUB for each of the others offered
		rmt_cli_init (&c, &cfg);
	}
	// auth: the proof goes out, OK makes it ready; no code is a refusal; ERR is a refusal
	rmt_cli_connected (&c, 0);
	net.n = 0;
	n = welcome (f, RMT_PROTO, 1, 0x0100, 0);
	CHECK (rmt_cli_feed (&c, f, n, 1) == 0 && c.state == RMT_CLI_AUTH && net.n == RMT_HDR + 32 && net.out[0] == RMT_AUTH);
	uint8_t nonce[16], proof[32];
	for (int i = 0; i < 16; i++) nonce[i] = (uint8_t) (i * 3);
	rmt_auth_proof ("12345678", nonce, 0xCAFE, proof);
	CHECK (!memcmp (net.out + RMT_HDR, proof, 32));
	n = rmt_frame (f, sizeof f, RMT_OK, 2, ok, 4);
	CHECK (rmt_cli_feed (&c, f, n, 2) == 0 && rmt_cli_ready (&c));
	rmt_cli_connected (&c, 0);
	n = welcome (f, RMT_PROTO, 1, 0x0100, 0);
	rmt_cli_feed (&c, f, n, 1);
	n = rmt_frame (f, sizeof f, RMT_ERR, 2, er, 8);
	CHECK (rmt_cli_feed (&c, f, n, 2) == -1 && c.state == RMT_CLI_REFUSED);
	rmt_cli_cfg_t nocode = cfg;
	nocode.code = 0;
	rmt_cli_init (&c, &nocode);
	rmt_cli_connected (&c, 0);
	n = welcome (f, RMT_PROTO, 1, 0x0100, 0);
	CHECK (rmt_cli_feed (&c, f, n, 1) == -1 && c.refused_code == RMT_E_AUTH);
}

static void test_garbage (void)
{
	static rmt_cli_t c;
	static net_t net;
	rmt_cli_cfg_t cfg = {net_send, &net, 1, 800, 480, 1, 0, 0, 0};
	for (int round = 0; round < 300; round++)
	{
		net.n = 0;
		rmt_cli_init (&c, &cfg);
		rmt_cli_want (&c, RMT_V_VOICE, 10);
		rmt_cli_connected (&c, 0);
		uint8_t g[400];
		fill_random (g, sizeof g);
		if (round % 3 == 0) { uint8_t f[RMT_MAX_FRAME]; size_t n = welcome (f, RMT_PROTO, 0, 0x0100, 0); rmt_cli_feed (&c, f, n, 1); }
		if (round % 3 == 1) g[0] = RMT_VIEW, g[2] = 100, g[3] = 0;	// a plausible header over noise
		for (size_t pos = 0; pos < sizeof g; pos += 37)
			if (rmt_cli_feed (&c, g + pos, sizeof g - pos < 37 ? sizeof g - pos : 37, (uint32_t) pos) != 0) break;
		net.n = 0;
	}
	// random section data into every decoder: no overrun (the sanitizers watch)
	for (int view = 1; view < RMT_V_COUNT; view++)
	{
		const rmt_view_info_t *vi = rmt_view_info (view);
		uint8_t *v = (uint8_t *) calloc (1, vi->size);
		for (int s = 0; s < vi->nsec; s++)
			for (int i = 0; i < rmt_sec_info (view, s)->ninst; i++)
				for (int len = 0; len < 1100; len += 97)
				{
					uint8_t d[1100];
					fill_random (d, sizeof d);
					rmt_sec_decode (view, s, i, v, d, len);
				}
		free (v);
	}
	CHECK (1);
}

int main (void)
{
	test_frames ();
	test_sha ();
	test_views ();
	test_delta ();
	test_seq_deltas ();
	test_skew ();
	test_status_bpm10 ();
	test_client ();
	test_garbage ();
	printf ("%d checks, %d failed\n", g_n, g_fail);
	return g_fail != 0;
}
