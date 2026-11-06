// pi/engine/seqview_json.h - the touch pattern editor's view (pi/ui/sidecar_seqed.h, seqed_pi::View) as the /ws "seq"
// topic's JSON (docs/web-ws.md "seq"). Portable, header only, no allocation (tests/test_ws.cpp). Compact: a note is an
// array [step, micro, pitch, vel, len, flags], and only the steps with anything on them are listed.
#ifndef _seqview_json_h
#define _seqview_json_h

#include "../ui/sidecar_seqed.h"
#include "ws_session.h"

namespace seqjson {

inline wsproto::Writer &i (wsproto::Writer &w, int v)		// a signed integer
{
	if (v < 0) { w.s ("-"); return w.u ((uint32_t) (-(int64_t) v)); }
	return w.u ((uint32_t) v);
}

// The view into pOut (max bytes); its length, 0 if it did not fit
inline size_t view (const sidecar_seq_view_t &v, char *pOut, size_t max)
{
	wsproto::Writer w (pOut, max);
	if (!v.valid) { w.s ("{\"valid\":false}"); return w.ok ? w.n : 0; }
	w.s ("{\"valid\":true,\"track\":"); i (w, v.track);
	w.s (",\"tracks\":[");
	for (int t = 0; t < SEQED_TRACKS; t++) { if (t) w.s (","); i (w, v.tracks[t]); }
	w.s ("],\"pattern\":"); i (w, v.pattern);
	w.s (",\"project\":").str (v.project).s (",\"machine\":").str (v.machine);
	w.s (",\"length\":"); i (w, v.length);
	w.s (",\"pages\":"); i (w, v.npages);
	w.s (",\"defaults\":{\"note\":"); i (w, v.dnote); w.s (",\"vel\":"); i (w, v.dvel); w.s (",\"len\":"); i (w, v.dlen);
	w.s ("},\"transport\":"); i (w, v.transport);
	w.s (",\"bpm10\":"); i (w, v.bpm10);
	w.s (",\"undo\":"); i (w, v.undo);
	w.s (",\"redo\":"); i (w, v.redo);
	w.s (",\"gen\":").u (v.gen);
	w.s (",\"chords\":[");
	for (int c = 0; c < SEQED_CHORDS; c++)
	{
		char n[sizeof v.chord[0] + 1];
		memcpy (n, v.chord[c], sizeof v.chord[0]);
		n[sizeof v.chord[0]] = 0;
		w.s (c ? "," : "").str (n);
	}
	w.s ("],\"swing\":"); i (w, v.swing);
	w.s (",\"poly\":{\"voices\":"); i (w, v.poly_voices); w.s (",\"alloc\":"); i (w, v.poly_alloc);
	w.s (",\"unison\":"); i (w, v.poly_unison); w.s (",\"info\":").str (v.poly_info);
	w.s ("},\"speed\":"); i (w, v.speed);
	w.s (",\"dir\":"); i (w, v.dir);
	w.s (",\"key\":"); i (w, v.key); w.s (",\"scale\":"); i (w, v.scale); w.s (",\"qnt\":"); i (w, v.qnt);
	w.s (",\"rnd\":["); i (w, v.rnd_mode); w.s (","); i (w, v.rnd_oct); w.s (","); i (w, v.rnd_wght);
	w.s ("],\"seed\":"); i (w, v.seed); w.s (",\"seedFixed\":"); w.s (v.seed_fixed ? "true" : "false");
	w.s (",\"editPage\":"); i (w, v.edit_page);
	w.s (",\"noteEdit\":{\"on\":"); w.s (v.ne_on ? "true" : "false");
	w.s (",\"step\":"); i (w, v.ne_step); w.s (",\"pitch\":"); i (w, v.ne_pitch); w.s (",\"sel\":"); i (w, v.ne_sel);
	w.s ("},\"notes\":[");
	int n = v.n < 0 ? 0 : v.n > SEQED_NOTES ? SEQED_NOTES : v.n;
	for (int k = 0; k < n; k++)
	{
		const sidecar_seq_note_t &q = v.note[k];
		w.s (k ? ",[" : "[").u (q.step).s (","); i (w, q.micro);
		w.s (",").u (q.pitch).s (",").u (q.vel).s (",").u (q.len).s (",").u (q.flags).s ("]");
	}
	// per step, only the steps with something: [step, locks, cond, extras, ratchet count, ratchet mode, fx cmd, fx value,
	// lock cond, arp locks]
	w.s ("],\"steps\":[");
	int len = v.length < 0 ? 0 : v.length > SEQED_STEPS ? SEQED_STEPS : v.length;
	bool first = true;
	for (int s = 0; s < len; s++)
	{
		if (!v.locks[s] && !v.cond[s] && !v.xstep[s] && !v.rt_cnt[s] && !v.rt_mode[s] && !v.fx_cmd[s] && !v.fx_val[s]
		    && !v.lcond[s] && !v.arpl[s]) continue;
		w.s (first ? "[" : ",[").u ((uint32_t) s).s (",").u (v.locks[s]).s (",").u (v.cond[s]).s (",").u (v.xstep[s]);
		w.s (",").u (v.rt_cnt[s]).s (",").u (v.rt_mode[s]).s (",").u (v.fx_cmd[s]).s (","); i (w, v.fx_val[s]);
		w.s (",").u (v.lcond[s]).s (",").u (v.arpl[s]).s ("]");
		first = false;
	}
	w.s ("]}");
	return w.ok ? w.n : 0;
}

// The play head (seqed_pi::Playhead: steps x 1000, -1 stopped) and the transport, for the "seqpos" topic
inline size_t pos (int pos_x1000, int track, int transport, char *pOut, size_t max)
{
	wsproto::Writer w (pOut, max);
	w.s ("{\"pos\":"); i (w, pos_x1000);
	w.s (",\"track\":"); i (w, track);
	w.s (",\"transport\":"); i (w, transport);
	w.s ("}");
	return w.ok ? w.n : 0;
}

}	// namespace seqjson

#endif
