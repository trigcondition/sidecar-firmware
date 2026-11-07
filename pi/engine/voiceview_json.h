// pi/engine/voiceview_json.h - the touchscreen's VOICE page (pi/ui/sidecar_voice.h, voice_pi::Fill) as the /ws "voices"
// topic's JSON (docs/web-ws.md "voices"): the selected track's machine, its P1..P8 with the Rytm's labels, the
// machine's own panel (6-OP FM, MULTISAMPLE, DAISYSP) and the track's p-locks. No scope: the web draws its meters from
// "levels". Portable, header only, no allocation (tests/test_ws.cpp).
#ifndef _voiceview_json_h
#define _voiceview_json_h

#include "../ui/sidecar_voice.h"
#include "ws_session.h"

namespace voicejson {

inline wsproto::Writer &i (wsproto::Writer &w, int v)
{
	if (v < 0) { w.s ("-"); return w.u ((uint32_t) (-(int64_t) v)); }
	return w.u ((uint32_t) v);
}

// a fixed char field (not always terminated) as a JSON string
template <size_t N> inline wsproto::Writer &fs (wsproto::Writer &w, const char (&f)[N])
{
	char b[N + 1];
	memcpy (b, f, N);
	b[N] = 0;
	return w.str (b);
}

template <typename T> inline void arr (wsproto::Writer &w, const T *a, int n)
{
	w.s ("[");
	for (int k = 0; k < n; k++) { if (k) w.s (","); i (w, (int) a[k]); }
	w.s ("]");
}

// The view into pOut (max bytes); its length, 0 if it did not fit
inline size_t view (const sidecar_voice_view_t &v, char *pOut, size_t max)
{
	wsproto::Writer w (pOut, max);
	if (!v.valid) { w.s ("{\"valid\":false}"); return w.ok ? w.n : 0; }
	w.s ("{\"valid\":true,\"track\":"); i (w, v.track);
	w.s (",\"machine\":"); i (w, v.machine);
	w.s (",\"sidecar\":").s (v.sidecar ? "true" : "false");
	w.s (",\"name\":"); fs (w, v.name);
	w.s (",\"val\":"); arr (w, v.val, 8);
	w.s (",\"label\":[");
	for (int k = 0; k < 8; k++) { if (k) w.s (","); fs (w, v.label[k]); }
	w.s ("],\"pattern\":"); i (w, v.pattern);
	int st = v.steps < 0 ? 0 : v.steps > SVV_STEPS ? SVV_STEPS : v.steps;
	w.s (",\"locks\":"); arr (w, v.locks, st);
	w.s (",\"trig\":"); arr (w, v.trig, st);
	static const char *const kMode[] = {"plain", "fm", "multi", "daisy"};
	int m = v.mode >= 0 && v.mode <= SVV_DAISY ? v.mode : 0;
	w.s (",\"mode\":").str (kMode[m]);
	if (m == SVV_FM)
	{
		w.s (",\"fm\":{\"name\":"); fs (w, v.fm_name);
		w.s (",\"alg\":"); i (w, v.fm_alg);
		w.s (",\"fb\":"); i (w, v.fm_fb);
		w.s (",\"cols\":"); i (w, v.fm_ncols);
		w.s (",\"levels\":"); i (w, v.fm_nlev);
		w.s (",\"col\":"); arr (w, v.fm_col, 6);
		w.s (",\"lev\":"); arr (w, v.fm_lev, 6);
		w.s (",\"mod\":"); arr (w, v.fm_mod, 6);
		w.s (",\"car\":"); i (w, v.fm_car);
		w.s (",\"fbSrc\":"); i (w, v.fm_fb_src);
		w.s (",\"fbDst\":"); i (w, v.fm_fb_dst);
		w.s (",\"out\":"); arr (w, v.fm_out, 6);
		w.s (",\"freq\":[");
		for (int k = 0; k < 6; k++) { if (k) w.s (","); fs (w, v.fm_freq[k]); }
		w.s ("]}");
	}
	else if (m == SVV_MULTI)
	{
		int nz = v.ms_nz < 0 ? 0 : v.ms_nz > SVV_ZONES ? SVV_ZONES : v.ms_nz;
		w.s (",\"multi\":{\"inst\":"); fs (w, v.ms_inst);
		w.s (",\"ready\":").s (v.ms_ready ? "true" : "false");
		w.s (",\"lo\":"); arr (w, v.ms_lo, nz);
		w.s (",\"hi\":"); arr (w, v.ms_hi, nz);
		w.s (",\"root\":"); arr (w, v.ms_root, nz);
		w.s (",\"vel\":"); arr (w, v.ms_vel, nz);
		w.s (",\"keys\":"); arr (w, v.ms_keys, 16);
		w.s ("}");
	}
	else if (m == SVV_DAISY)
	{
		int n = v.dz_n < 0 ? 0 : v.dz_n > SVV_MODES ? SVV_MODES : v.dz_n;
		w.s (",\"daisy\":{\"model\":"); i (w, v.dz_model);
		w.s (",\"ratio\":"); arr (w, v.dz_ratio, n);
		w.s (",\"ring\":"); arr (w, v.dz_ring, n);
		w.s ("}");
	}
	w.s (",\"gen\":").u (v.gen).s ("}");
	return w.ok ? w.n : 0;
}

}	// namespace voicejson

#endif
