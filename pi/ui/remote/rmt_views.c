// pi/ui/remote/rmt_views.c - the section tables (see rmt_views.h). F (kind, member): kinds I32 U32 U16 U8 I8 are scalars,
// BYTES a raw byte array (strings, 2-D char arrays, u8 / s8 arrays), U16A / I32A / U32A arrays of that integer. The member
// is written against `v` (the view) and `idx` (the instance of an indexed section).
#include "rmt_views.h"
#include "rmt_proto.h"
#include "../sidecar_status.h"
#include "../sidecar_voice.h"
#include "../sidecar_insfx.h"
#include "../sidecar_mfx.h"
#include "rmt_seqview.h"
#include <string.h>

// ---- the table -----------------------------------------------------------------------------------------------------

#define VOICE_HEAD(F) F (I32, valid) F (I32, track) F (I32, machine) F (I32, sidecar) F (BYTES, name) F (BYTES, val) F (BYTES, label) \
	F (I32, ncols) F (I32, steps) F (I32, pattern) F (I32, mode) F (U8, wedit) F (BYTES, wk) F (U8, wfail) F (U8, wkind) F (U32, wgen)
#define VOICE_SCOPE(F) F (BYTES, smin) F (BYTES, smax) F (I32, level)
#define VOICE_LOCKS(F) F (BYTES, locks) F (BYTES, trig)
#define VOICE_FM(F) F (BYTES, fm_name) F (I32, fm_alg) F (I32, fm_fb) F (I32, fm_ncols) F (I32, fm_nlev) F (BYTES, fm_col) F (BYTES, fm_lev) \
	F (BYTES, fm_mod) F (U8, fm_car) F (I8, fm_fb_src) F (I8, fm_fb_dst) F (BYTES, fm_out) F (BYTES, fm_freq)
#define VOICE_MS(F) F (BYTES, ms_inst) F (I32, ms_ready) F (I32, ms_nz) F (BYTES, ms_lo) F (BYTES, ms_hi) F (BYTES, ms_root) F (BYTES, ms_vel) \
	F (BYTES, ms_keys)
#define VOICE_DZ(F) F (I32, dz_model) F (I32, dz_n) F (U16A, dz_ratio) F (BYTES, dz_ring)

#define INSFX_HEAD(F) F (I32, valid) F (I32, voice) F (I32, track)
#define INSFX_SLOT_ONE(F, b) F (I32, slot[idx][b].type) F (BYTES, slot[idx][b].name) F (BYTES, slot[idx][b].label) F (BYTES, slot[idx][b].val) \
	F (BYTES, slot[idx][b].v) F (BYTES, slot[idx][b].base) F (BYTES, slot[idx][b].def)
#define INSFX_SLOT(F) INSFX_SLOT_ONE (F, 0) INSFX_SLOT_ONE (F, 1)
#define INSFX_SENDS(F) F (BYTES, send) F (BYTES, bus_fx) F (BYTES, bus_post) F (BYTES, out_route) F (BYTES, out_v) F (BYTES, out_base) \
	F (BYTES, out_def) F (BYTES, out_val)
#define INSFX_PEAK(F) F (BYTES, out_peak)
#define INSFX_PEQ(F) F (BYTES, peq)

#define MFX_HEAD(F) F (I32, valid) F (U8, on) F (U8, byp) F (U8, mix) F (U8, rep) F (U8, onmain) F (U8, lev) F (U8, lev_def) F (BYTES, peq)
#define MFX_METER(F) F (U8, in) F (U8, out)
#define MFX_SLOT(F) F (I32, slot[idx].type) F (BYTES, slot[idx].name) F (BYTES, slot[idx].next) F (BYTES, slot[idx].label) \
	F (BYTES, slot[idx].val) F (BYTES, slot[idx].v) F (BYTES, slot[idx].base) F (BYTES, slot[idx].def) F (U8, slot[idx].send) \
	F (U8, slot[idx].mix) F (U8, slot[idx].mixdef) F (U8, slot[idx].amp) F (U8, slot[idx].pair)

#define STATUS_LIVE(F) F (I32, valid) F (I32A, vload) F (BYTES, muted) F (I32A, load) F (U32, block_max_us) F (U32, block_budget_us) \
	F (U32, pass_max_us) F (U32, underruns) F (I32, inst_used) F (I32, inst_total) F (I32, inst_cpu100) F (U32, inst_kb) F (U32, fifo_min) \
	F (U32, fifo_target) F (I32, temp_c) F (I32, temp_warn) F (I32, clock_low_mhz) F (I32, cpu_mhz) F (U32, blocks) F (U32, ui_max_us) \
	F (I32, bpm10)
#define STATUS_ID(F) F (BYTES, mach) F (BYTES, fx_type) F (BYTES, fx_post) F (I32, rytm) F (I32, ob) F (BYTES, link) F (BYTES, gadget) \
	F (I32, mac) F (I32, net) F (I32, wifi) F (BYTES, ip) F (BYTES, host) F (BYTES, pair) F (I32, pair_warn) F (BYTES, build) \
	F (BYTES, circle) F (BYTES, rytm_mod)
#define STATUS_STORE(F) F (U32, card_free_mb) F (U32, card_total_mb) F (I32, usb_state) F (I32, usb_more) F (BYTES, usb_fs) F (BYTES, usb_link) \
	F (U32, usb_free_mb) F (U32, usb_total_mb) F (I32, nvme_state) F (BYTES, nvme_fs) F (U32, nvme_free_mb) F (U32, nvme_total_mb) \
	F (I32, lib_state) F (U32, lib_files) F (U32, lib_hashed) F (U32, lib_pending) F (U32, lib_dups) F (U32, lib_missing) F (U32, lib_pct) \
	F (BYTES, lib_waiting)


// SEQ: the header (small, and every field that is not a lane or a note), the step lanes, and the notes in 64s (a section is 1 KB at most)
#define SEQ_HEAD(F) F (I32, valid) F (I32, track) F (I32A, tracks) F (I32, pattern) F (BYTES, project) F (BYTES, machine) F (I32, length) \
	F (I32, npages) F (I32, dnote) F (I32, dvel) F (I32, dlen) F (I32, transport) F (I32, bpm10) F (I32, undo) F (I32, redo) F (BYTES, chord) \
	F (U32, show_req) F (I32, swing) F (I32, poly_voices) F (I32, poly_alloc) F (I32, poly_unison) F (BYTES, poly_info) F (I32, speed) \
	F (I32, dir) F (I32, key) F (I32, scale) F (I32, qnt) F (I32, rnd_mode) F (I32, rnd_oct) F (I32, rnd_wght) F (I32, seed) \
	F (I32, seed_fixed) F (I32, edit_page) F (I32, ne_on) F (I32, ne_step) F (I32, ne_pitch) F (I32, ne_sel) F (U32, ne_gen) F (I32, n)
#define SEQ_LANES1(F) F (BYTES, locks) F (BYTES, cond) F (BYTES, xstep) F (BYTES, rt_cnt) F (BYTES, rt_mode)
#define SEQ_LANES2(F) F (BYTES, fx_cmd) F (BYTES, fx_val) F (BYTES, lcond) F (U16A, arpl)
#define SEQ_NOTES(F) F (NOTES, note)
#define SEQPOS_POS(F) F (I32, pos32)
#define PAT_HEAD(F) F (I32, pat.valid) F (I32, index) F (I32, pat.nlocks) F (I32, pat.nparams) F (BYTES, pat.tail)
#define PAT_TEXT(F) F (I32, stale) F (BYTES, project) F (BYTES, kit) F (BYTES, where)
#define PAT_TRACK(F) F (U16A, pat.t[idx].trig) F (BYTES, pat.t[idx].note) F (BYTES, pat.t[idx].vel) F (BYTES, pat.t[idx].len) \
	F (BYTES, pat.t[idx].micro) F (BYTES, pat.t[idx].mflags) F (BYTES, pat.t[idx].cond) F (U8, pat.t[idx].dnote) F (U8, pat.t[idx].dvel) \
	F (U8, pat.t[idx].dlen) F (U8, pat.t[idx].speed) F (U8, pat.t[idx].steps)
#define PAT_LOCKS(F) F (LOCKS, pat.lock)

// ---- generators ----------------------------------------------------------------------------------------------------

#define SZ_I32(m)	4
#define SZ_U32(m)	4
#define SZ_U16(m)	2
#define SZ_U8(m)	1
#define SZ_I8(m)	1
#define SZ_BYTES(m)	sizeof (v->m)
#define SZ_U16A(m)	sizeof (v->m)
#define SZ_I32A(m)	sizeof (v->m)
#define SZ_U32A(m)	sizeof (v->m)
#define NOTES_PER	64		// SEQ's notes a section (8 bytes each)
#define LOCKS_PER	8		// PAT's lock slots a section (67 bytes each)
#define SZ_NOTES(m)	(NOTES_PER * 8)
#define SZ_LOCKS(m)	(LOCKS_PER * (3 + RYTM_STEPS))

#define ENC_I32(m)	rmt_w_u32 (w, (uint32_t) v->m);
#define ENC_U32(m)	rmt_w_u32 (w, v->m);
#define ENC_U16(m)	rmt_w_u16 (w, v->m);
#define ENC_U8(m)	rmt_w_u8 (w, v->m);
#define ENC_I8(m)	rmt_w_u8 (w, (uint8_t) v->m);
#define ENC_BYTES(m)	rmt_w_bytes (w, v->m, sizeof (v->m));
#define ENC_U16A(m)	for (size_t i_ = 0; i_ < sizeof (v->m) / 2; i_++) rmt_w_u16 (w, v->m[i_]);
#define ENC_I32A(m)	for (size_t i_ = 0; i_ < sizeof (v->m) / 4; i_++) rmt_w_u32 (w, (uint32_t) v->m[i_]);
#define ENC_U32A(m)	for (size_t i_ = 0; i_ < sizeof (v->m) / 4; i_++) rmt_w_u32 (w, v->m[i_]);
// the notes / lock slots of this instance; those past the used count are sent as zeros (stale bytes there would only make the hash move)
#define ENC_NOTES(m)	for (int i_ = 0; i_ < NOTES_PER; i_++) { const int k_ = idx * NOTES_PER + i_; const int on_ = k_ < v->n; \
		rmt_w_u16 (w, on_ ? v->m[k_].step : 0); rmt_w_u8 (w, on_ ? (uint8_t) v->m[k_].micro : 0); rmt_w_u8 (w, on_ ? v->m[k_].pitch : 0); \
		rmt_w_u8 (w, on_ ? v->m[k_].vel : 0); rmt_w_u8 (w, on_ ? v->m[k_].flags : 0); rmt_w_u16 (w, on_ ? v->m[k_].len : 0); }
#define ENC_LOCKS(m)	for (int i_ = 0; i_ < LOCKS_PER; i_++) { const int k_ = idx * LOCKS_PER + i_; const int on_ = k_ < v->pat.nlocks && k_ < RYTM_LOCKS; \
		rmt_w_u8 (w, on_ ? v->m[k_].type : 0); rmt_w_u8 (w, on_ ? v->m[k_].track : 0); \
		for (int j_ = 0; j_ < RYTM_STEPS; j_++) rmt_w_u8 (w, on_ ? v->m[k_].val[j_] : 0xFF); rmt_w_u8 (w, on_ ? v->m[k_].fine : 0); }

// a field the data is too short for stops the section (the fields before it were applied)
#define DEC_(m, size, body)	if (rmt_r_left (r) < (size)) goto done; body
#define DEC_I32(m)	DEC_ (m, 4, v->m = (int) rmt_r_u32 (r);)
#define DEC_U32(m)	DEC_ (m, 4, v->m = rmt_r_u32 (r);)
#define DEC_U16(m)	DEC_ (m, 2, v->m = (unsigned short) rmt_r_u16 (r);)
#define DEC_U8(m)	DEC_ (m, 1, v->m = (unsigned char) rmt_r_u8 (r);)
#define DEC_I8(m)	DEC_ (m, 1, v->m = (signed char) rmt_r_u8 (r);)
#define DEC_BYTES(m)	DEC_ (m, sizeof (v->m), rmt_r_bytes (r, v->m, sizeof (v->m));)
#define DEC_U16A(m)	DEC_ (m, sizeof (v->m), for (size_t i_ = 0; i_ < sizeof (v->m) / 2; i_++) v->m[i_] = (unsigned short) rmt_r_u16 (r);)
#define DEC_I32A(m)	DEC_ (m, sizeof (v->m), for (size_t i_ = 0; i_ < sizeof (v->m) / 4; i_++) v->m[i_] = (int) rmt_r_u32 (r);)
#define DEC_U32A(m)	DEC_ (m, sizeof (v->m), for (size_t i_ = 0; i_ < sizeof (v->m) / 4; i_++) v->m[i_] = rmt_r_u32 (r);)
#define DEC_NOTES(m)	DEC_ (m, NOTES_PER * 8, for (int i_ = 0; i_ < NOTES_PER; i_++) { sidecar_seq_note_t *n_ = &v->m[idx * NOTES_PER + i_]; \
		n_->step = (unsigned short) rmt_r_u16 (r); n_->micro = (signed char) rmt_r_u8 (r); n_->pitch = (unsigned char) rmt_r_u8 (r); \
		n_->vel = (unsigned char) rmt_r_u8 (r); n_->flags = (unsigned char) rmt_r_u8 (r); n_->len = (unsigned short) rmt_r_u16 (r); })
#define DEC_LOCKS(m)	DEC_ (m, LOCKS_PER * (3 + RYTM_STEPS), for (int i_ = 0; i_ < LOCKS_PER; i_++) { rytm_lock_t *k_ = &v->m[idx * LOCKS_PER + i_]; \
		k_->type = (unsigned char) rmt_r_u8 (r); k_->track = (unsigned char) rmt_r_u8 (r); rmt_r_bytes (r, k_->val, RYTM_STEPS); \
		k_->fine = (unsigned char) rmt_r_u8 (r); })

// the schema hash takes each field's kind, name and wire size
#define HASH_F(kind, m)	h = rmt_fnv (h, #kind ":" #m, strlen (#kind ":" #m)); { uint32_t s_ = (uint32_t) SZ_##kind (m); h = rmt_fnv (h, &s_, 4); }
#define ENC_F(kind, m)	ENC_##kind (m)
#define DEC_F(kind, m)	DEC_##kind (m)

// Hash and sizes need a view to name members through; the instance's index is only a name there
#define SECTION(V, T, name, FIELDS) \
	static void enc_##V##_##name (const T *v, int idx, rmt_w *w) { (void) idx; FIELDS (ENC_F) } \
	static void dec_##V##_##name (T *v, int idx, rmt_r *r) { (void) idx; FIELDS (DEC_F) done: v->gen++; } \
	static uint32_t hash_##V##_##name (uint32_t h) { const T *v = 0; int idx = 0; (void) v; (void) idx; FIELDS (HASH_F) return h; }

// the hash's sizeof (v->m) is unevaluated: v may be null there
SECTION (status, sidecar_status_view_t, live, STATUS_LIVE)
SECTION (status, sidecar_status_view_t, id, STATUS_ID)
SECTION (status, sidecar_status_view_t, store, STATUS_STORE)
SECTION (voice, sidecar_voice_view_t, head, VOICE_HEAD)
SECTION (voice, sidecar_voice_view_t, scope, VOICE_SCOPE)
SECTION (voice, sidecar_voice_view_t, locks, VOICE_LOCKS)
SECTION (voice, sidecar_voice_view_t, fm, VOICE_FM)
SECTION (voice, sidecar_voice_view_t, ms, VOICE_MS)
SECTION (voice, sidecar_voice_view_t, dz, VOICE_DZ)
SECTION (insfx, sidecar_insfx_view_t, head, INSFX_HEAD)
SECTION (insfx, sidecar_insfx_view_t, slot, INSFX_SLOT)
SECTION (insfx, sidecar_insfx_view_t, sends, INSFX_SENDS)
SECTION (insfx, sidecar_insfx_view_t, peak, INSFX_PEAK)
SECTION (insfx, sidecar_insfx_view_t, peq, INSFX_PEQ)
SECTION (mfx, sidecar_mfx_view_t, head, MFX_HEAD)
SECTION (mfx, sidecar_mfx_view_t, meter, MFX_METER)
SECTION (mfx, sidecar_mfx_view_t, slot, MFX_SLOT)
SECTION (seq, sidecar_seq_view_t, head, SEQ_HEAD)
SECTION (seq, sidecar_seq_view_t, lanes1, SEQ_LANES1)
SECTION (seq, sidecar_seq_view_t, lanes2, SEQ_LANES2)
SECTION (seq, sidecar_seq_view_t, notes, SEQ_NOTES)
SECTION (seqpos, rmt_seqpos_view_t, pos, SEQPOS_POS)
SECTION (pat, rmt_pat_view_t, head, PAT_HEAD)
SECTION (pat, rmt_pat_view_t, text, PAT_TEXT)
SECTION (pat, rmt_pat_view_t, track, PAT_TRACK)
SECTION (pat, rmt_pat_view_t, locks, PAT_LOCKS)

// ---- the section tables --------------------------------------------------------------------------------------------

typedef void (*enc_fn) (const void *v, int idx, rmt_w *w);
typedef void (*dec_fn) (void *v, int idx, rmt_r *r);
typedef uint32_t (*hash_fn) (uint32_t h);
typedef struct { rmt_sec_info_t info; enc_fn enc; dec_fn dec; hash_fn hash; } sec_t;
#define S(V, name, n, hot) {{n, hot, #name}, (enc_fn) enc_##V##_##name, (dec_fn) dec_##V##_##name, hash_##V##_##name}

static const sec_t s_Status[] = {S (status, live, 1, 1), S (status, id, 1, 0), S (status, store, 1, 0)};
static const sec_t s_Voice[] = {S (voice, head, 1, 0), S (voice, scope, 1, 1), S (voice, locks, 1, 0), S (voice, fm, 1, 0), S (voice, ms, 1, 0),
				S (voice, dz, 1, 0)};
static const sec_t s_Insfx[] = {S (insfx, head, 1, 0), S (insfx, slot, SIF_CHAINS, 0), S (insfx, sends, 1, 0), S (insfx, peak, 1, 1),
				S (insfx, peq, 1, 0)};
static const sec_t s_Mfx[] = {S (mfx, head, 1, 0), S (mfx, meter, 1, 1), S (mfx, slot, SMF_SLOTS, 0)};
static const sec_t s_Seq[] = {S (seq, head, 1, 0), S (seq, lanes1, 1, 0), S (seq, lanes2, 1, 0), S (seq, notes, SEQED_NOTES / NOTES_PER, 0)};
static const sec_t s_SeqPos[] = {S (seqpos, pos, 1, 1)};
static const sec_t s_Pat[] = {S (pat, head, 1, 0), S (pat, text, 1, 0), S (pat, track, RYTM_TRACKS, 0), S (pat, locks, RYTM_LOCKS / LOCKS_PER, 0)};

typedef struct { rmt_view_info_t info; const sec_t *sec; } view_t;
#define NS(a) (uint8_t) (sizeof (a) / sizeof (a)[0])
static const view_t s_Views[RMT_V_COUNT] = {
	{{0, 0, 0, 0, 0, 0}, 0},
	{{RMT_V_STATUS, 2, NS (s_Status), 0x0101, sizeof (sidecar_status_view_t), "status"}, s_Status},
	{{RMT_V_VOICE, 10, NS (s_Voice), 0x0100, sizeof (sidecar_voice_view_t), "voice"}, s_Voice},
	{{RMT_V_INSFX, 10, NS (s_Insfx), 0x0100, sizeof (sidecar_insfx_view_t), "insfx"}, s_Insfx},
	{{RMT_V_MFX, 10, NS (s_Mfx), 0x0100, sizeof (sidecar_mfx_view_t), "mfx"}, s_Mfx},
	{{RMT_V_SEQ, 10, NS (s_Seq), 0x0100, sizeof (sidecar_seq_view_t), "seq"}, s_Seq},
	{{RMT_V_SEQPOS, 30, NS (s_SeqPos), 0x0100, sizeof (rmt_seqpos_view_t), "seqpos"}, s_SeqPos},
	{{RMT_V_PAT, 10, NS (s_Pat), 0x0100, sizeof (rmt_pat_view_t), "pat"}, s_Pat},
};

const rmt_view_info_t *rmt_view_info (int view)
{
	return view >= 1 && view < RMT_V_COUNT ? &s_Views[view].info : 0;
}

const rmt_sec_info_t *rmt_sec_info (int view, int sec)
{
	const rmt_view_info_t *vi = rmt_view_info (view);
	return vi && sec >= 0 && sec < vi->nsec ? &s_Views[view].sec[sec].info : 0;
}

int rmt_view_nsi (int view)
{
	const rmt_view_info_t *vi = rmt_view_info (view);
	int n = 0;
	for (int s = 0; vi && s < vi->nsec; s++) n += s_Views[view].sec[s].info.ninst;
	return n;
}

static const sec_t *find (int view, int sec, int inst)
{
	const rmt_sec_info_t *si = rmt_sec_info (view, sec);
	return si && inst >= 0 && inst < si->ninst ? &s_Views[view].sec[sec] : 0;
}

int rmt_sec_encode (int view, int sec, int inst, const void *v, uint8_t *out, int cap)
{
	const sec_t *s = find (view, sec, inst);
	if (!s || cap < 0) return -1;
	rmt_w w;
	rmt_w_init (&w, out, (size_t) cap);
	s->enc (v, inst, &w);
	return w.err ? -1 : (int) w.n;
}

int rmt_sec_decode (int view, int sec, int inst, void *v, const uint8_t *data, int len)
{
	const sec_t *s = find (view, sec, inst);
	if (!s || len < 0) return -1;
	rmt_r r;
	rmt_r_init (&r, data, (size_t) len);
	s->dec (v, inst, &r);
	return 0;
}

uint32_t rmt_schema_hash (void)
{
	uint32_t h = RMT_FNV_INIT;
	for (int v = 1; v < RMT_V_COUNT; v++)
		for (int s = 0; s < s_Views[v].info.nsec; s++)
		{
			h = rmt_fnv (h, s_Views[v].sec[s].info.name, strlen (s_Views[v].sec[s].info.name));
			h = s_Views[v].sec[s].hash (h);
		}
	return h;
}

void rmt_vtx_reset (rmt_vtx_t *t) { memset (t, 0, sizeof *t); }

int rmt_vtx_emit (rmt_vtx_t *t, int view, const void *v, rmt_emit_fn fn, void *ud, int hot_only)
{
	const rmt_view_info_t *vi = rmt_view_info (view);
	uint8_t buf[RMT_SEC_BUF];
	int si = 0, sent = 0;
	for (int s = 0; vi && s < vi->nsec; s++)
	{
		const sec_t *sc = &s_Views[view].sec[s];
		for (int i = 0; i < sc->info.ninst && si < RMT_MAX_SI; i++, si++)
		{
			if (hot_only && !sc->info.hot) continue;
			int n = rmt_sec_encode (view, s, i, v, buf, (int) sizeof buf);
			if (n < 0) continue;
			uint32_t h = rmt_fnv (RMT_FNV_INIT, buf, (size_t) n);
			if (t->sent[si] && t->hash[si] == h) continue;
			t->sent[si] = 1, t->hash[si] = h;
			fn (ud, view, s, i, buf, n);
			sent++;
		}
	}
	return sent;
}
