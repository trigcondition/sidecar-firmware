// pi/engine/voice_pi.cpp - see voice_pi.h. Hosted (the core's flags); core 0.
#include "voice_pi.h"

#include <math.h>
#include <stddef.h>
#include "platform.h"
#include <string.h>

#include "lib/srclib.h"
#include "machines.h"
#include "msamp/msamp.h"
#include "voices.h"
#include "ui/extra.h"
#include "ui/page_keys.h"
#include "ui/remote.h"

namespace voice_pi {

namespace {
constexpr int kPgHome = 0, kPgSound = 1, kPgSeq = 4, kPgRytm = 5, kPgVoice = 8, kPgIns = 9, kPgSong = 10, kPgRec = 11,
              kPgScope = 12, kPgStatus = 13, kPgProject = 14, kPgPool = 15;
// the touchscreen's pages by the device table's names (ui/page_keys.h; sidecar_ui.h's page numbers); a name not here
// (SAMPLE) is the Rytm's screen's alone. VOICE: VoicePage (the machine view or VOICE).
struct TouchPage { const char* name; int page; };
constexpr TouchPage kTouch[] = {{"HOME", kPgHome}, {"SEQ", kPgSeq}, {"PAT", kPgRytm}, {"VOICE", kPgVoice}, {"SET", kPgSound},
                                {"INSERT FX", kPgIns}, {"REC", kPgRec}, {"SCOPE", kPgScope}, {"STATUS", kPgStatus},
                                {"SONG", kPgSong}, {"PROJECT", kPgProject}, {"POOL", kPgPool}};
int touch_page(const char* name) {
  for (const TouchPage& t : kTouch) if (sc_streq(t.name, name)) return t.page;
  return -1;
}
uint32_t g_rd = 0;
int g_vb = -2;   // the selected track's machine (its voice byte, -1 unknown) as Request last saw it
// UX review T3: a HOME tile's track shown instead of the Rytm's selection, until the Rytm selects another track
int g_pick = -1;
uint32_t g_pick_sel = 0;
int selected_vb() {
  int t = core::ui::remote_selected_track(nullptr);
  return t >= 0 ? core::ui::remote_track_machine(t) : -1;
}
// ---- the machines' panels (sidecar_voice.h)
void fill_fm(sidecar_voice_view_t& v) {
  core::ui::FmInfo f;
  v.fm_alg = -1, v.fm_fb_src = v.fm_fb_dst = -1;
  if (!core::ui::fm_info(v.val[0], &f)) return;   // P1 PAT, the Rytm's own value
  sniprintf(v.fm_name, sizeof v.fm_name, "%s", f.name);
  v.fm_alg = f.alg, v.fm_fb = f.fb, v.fm_ncols = f.ncols, v.fm_nlev = f.nlev;
  v.fm_car = f.car, v.fm_fb_src = (signed char)f.fb_src, v.fm_fb_dst = (signed char)f.fb_dst;
  for (int k = 0; k < 6; ++k) {
    v.fm_col[k] = f.col[k], v.fm_lev[k] = f.lev[k], v.fm_mod[k] = f.mod[k], v.fm_out[k] = f.out[k];
    unsigned r = f.freq[k];   // three characters at most (the OLED row: six of them in 104 dots)
    if (f.fixed[k]) sniprintf(v.fm_freq[k], 6, "%s", "FIX");
    else if (r >= 995) sniprintf(v.fm_freq[k], 6, "%u", (r + 50) / 100);
    else sniprintf(v.fm_freq[k], 6, "%u.%u", (r + 5) / 100, (r + 5) / 10 % 10);
  }
}

void fill_multi(sidecar_voice_view_t& v, int vi, int t) {
  // P1 INST at 14 bits (rui1038: its bank): the SRC view's 2A / our own 48, else the voice's report when it names the
  // same index, else the 49 3's 7-bit value (bank A); never the 7-bit value alone when the bank is known
  int s14 = t >= 0 ? core::ui::remote_track_src14(t) : -1;
  int code = s14 >= 0 && (s14 >> 7) == v.val[0] ? core::srclib::code_of14(s14)
             : (int)core::v[vi].p[0] == v.val[0] ? core::srclib::code_of(core::v[vi].p[0]) : v.val[0];
  sniprintf(v.ms_inst, sizeof v.ms_inst, "%s", core::msamp::entry_name(core::msamp::src_entry(vi, code)));
  const core::msamp::Desc* d = core::msamp::slot_desc(core::msamp::voice_slot(vi));
  if (d) {   // the zones: each key range once, its velocity layers counted (racy reads of a resident slot: drawing only)
    int nr = d->nregions < d->max_regions ? d->nregions : d->max_regions;
    v.ms_ready = 1;
    for (int i = 0; i < nr; ++i) {
      const core::msamp::Region& r = d->regions[i];
      if (r.lokey > r.hikey || r.hikey > 127) continue;
      int z = 0;
      while (z < v.ms_nz && !(v.ms_lo[z] == r.lokey && v.ms_hi[z] == r.hikey)) ++z;
      if (z == v.ms_nz) {
        if (v.ms_nz >= SVV_ZONES) continue;
        v.ms_lo[z] = r.lokey, v.ms_hi[z] = r.hikey, v.ms_root[z] = r.keycenter > 127 ? r.lokey : r.keycenter, v.ms_vel[z] = 0;
        ++v.ms_nz;
      }
      if (v.ms_vel[z] < 255) ++v.ms_vel[z];
    }
  }
  uint8_t keys[core::msamp::kPerVoice];
  int n = core::msamp::voice_keys(vi, keys, core::msamp::kPerVoice);
  for (int i = 0; i < n; ++i) v.ms_keys[keys[i] >> 3 & 15] |= (unsigned char)(1u << (keys[i] & 7));
}

// DAISYSP's MODAL / RESON resonator as its engine sets it (sim/daisysp/daisy_engine.h BlockResonator::Prepare): the
// modes' frequencies (the stiffness stretch from STR) and how long each rings (its Q, from DMP and BRI); STRING draws
// from the knobs alone (sidecar_voice.c)
float stiffness_of(float sig) {
  if (sig < 0.25f) return -(0.25f - sig) * 0.25f;
  if (sig < 0.3f) return 0.0f;
  if (sig < 0.9f) return (sig - 0.3f) / 0.6f;
  sig = (sig - 0.9f) * 10.0f;
  sig *= sig;
  return 1.5f - cosf(sig * 3.14159265f) * 0.5f;
}
void fill_daisy(sidecar_voice_view_t& v) {
  v.dz_model = core::eng_zone(v.val[0], 3);   // P1 ENG: MODAL STRING RESON
  float structure = v.val[1] / 127.0f, brightness = v.val[2] / 127.0f, damping = 1.0f - v.val[3] / 127.0f;
  float stiff = stiffness_of(structure), comp = 1.0f, st2 = stiff;
  for (int i = 0; i < 2; ++i) comp += st2, st2 *= st2 < 0.0f ? 0.93f : 0.98f;
  float harmonic = 1.0f / comp, stretch = 1.0f, f0 = harmonic;
  float q_sqrt = powf(2.0f, damping * 79.7f / 12.0f), q = 500.0f * q_sqrt * q_sqrt;
  const float qmax = 500.0f * powf(2.0f, 2.0f * 79.7f / 12.0f);   // DMP 0: the longest ring
  brightness *= 1.0f - structure * 0.3f;
  brightness *= 1.0f - damping * 0.3f;
  float q_loss = brightness * (2.0f - brightness) * 0.85f + 0.15f;
  v.dz_n = SVV_MODES;
  for (int i = 0; i < SVV_MODES; ++i) {
    float r = harmonic * stretch / f0;
    v.dz_ratio[i] = (unsigned short)(r < 0 ? 0 : r > 600 ? 60000 : r * 100 + 0.5f);
    float ring = logf(1.0f + q) / logf(1.0f + qmax);
    v.dz_ring[i] = (unsigned char)(ring <= 0 ? 0 : ring >= 1 ? 255 : ring * 255);
    stretch += stiff;
    stiff *= stiff < 0.0f ? 0.93f : 0.98f;
    harmonic += f0;
    q *= q_loss;
  }
}
}  // namespace

int VoicePage() {
  int t = core::ui::remote_selected_track(nullptr);
  int vb = t >= 0 ? core::ui::remote_track_machine(t) : -1;
  return PageForMachine(vb);
}

void Seen(uint32_t* seen, uint32_t* track_seen) {
  *seen = core::ui::remote_display_request(nullptr);
  core::ui::remote_selected_track(track_seen);
  g_vb = selected_vb();
}

int Request(uint32_t* seen, bool voice_mode, uint32_t* track_seen) {
  int page = 0;
  uint32_t g = core::ui::remote_display_request(&page), tg = 0;
  core::ui::remote_selected_track(&tg);
  if (g != *seen) {
    *seen = g, *track_seen = tg;
    // the device's table (ui/page_keys.h): rui1052's UP + T1..T13 -> HOME SEQ PAT VOICE INSERT FX SONG REC SET SCOPE
    // STATUS, (SAMPLE: the Rytm's own screen, core/ui/remote.cpp), PROJECT POOL (OwnPages keeps the Rytm's screen as is)
    const sc_page_key_t* p = sc_page_of_code(page);
    int pg = p ? touch_page(p->name) : -1;
    return pg == kPgVoice ? VoicePage() : pg;
  }
  int vb = selected_vb();
  if (tg != *track_seen || vb != g_vb) {   // another track selected, or its machine changed on the Rytm (33 / 49 2):
    *track_seen = tg, g_vb = vb;           // VOICE follows it, and its kind of view (the machine view or VOICE)
    if (voice_mode) return VoicePage();
  }
  return -1;
}

void Pick(int track) {
  g_pick = track >= 0 && track < 12 ? track : -1;
  core::ui::remote_selected_track(&g_pick_sel);
}

void Fill(sidecar_voice_view_t* o, const rytm_pattern_t* pat, int pattern) {
  int t = -1, m = -1;
  char name[13];
  uint8_t val[8];
  char label[8][5];
  core::ui::remote_track_info(&t, &m, name, val, label);
  uint32_t sel = 0;
  core::ui::remote_selected_track(&sel);
  if (g_pick >= 0 && sel != g_pick_sel) g_pick = -1;   // the Rytm selected a track since: follow it again
  if (g_pick >= 0 && g_pick != t) {   // a tile's track: a Sidecar machine's P1..P8 as the host has them, its labels
    t = g_pick, m = -1;
    int vb = core::ui::remote_track_machine(t), vi = core::ui::remote_track_voice(t);
    memset(name, 0, sizeof name), memset(val, 0, sizeof val), memset(label, 0, sizeof label);
    if (vb >= 0 && vb != 0x7F)
      for (int k = 0; k < 8; ++k) {
        const char* l = core::ui::voice_label(vi, k);
        val[k] = (uint8_t)core::ui::remote_track_param(t, k);
        if (l) sniprintf(label[k], sizeof label[k], "%.4s", l);
      }
    else
      sniprintf(name, sizeof name, "%s", "TRK+PAD: KNOBS");   // an analog track: its knobs only once the Rytm selects it
  }
  sidecar_voice_view_t v;
  memset(&v, 0, sizeof v);
  v.valid = t >= 0;
  v.track = t;
  v.machine = m;
  int vb = t >= 0 ? core::ui::remote_track_machine(t) : -1;
  v.sidecar = vb >= 0 && vb != 0x7F;
  memcpy(v.name, name, 13);
  if (v.sidecar) sniprintf(v.name, sizeof v.name, "%s", core::machine_long(vb & 0x78));   // the shared long name (UX review T3, T7)
  memcpy(v.val, val, 8);
  memcpy(v.label, label, sizeof v.label);
  // the scope and level: the track's stem from the input tap (its voice's channel)
  if (t >= 0) {
    int vi = core::ui::remote_track_voice(t);
    core::ui::tap_enable(true);
    core::ui::tap_source(vi + 1);
    static int16_t e[SVV_COLS * 8][2];
    uint32_t wp = core::ui::tap_write_pos();
    if (wp - g_rd > SVV_COLS * 8) g_rd = wp - SVV_COLS * 8;
    int n = core::ui::tap_read(&g_rd, e, SVV_COLS * 8);
    static signed char mn[SVV_COLS], mx[SVV_COLS];
    static int keep = 0;
    if (n > 0) {   // the newest n entries over the columns (older ones kept on a short read)
      int per = n / SVV_COLS < 1 ? 1 : n / SVV_COLS, cols = n / per > SVV_COLS ? SVV_COLS : n / per;
      memmove(mn, mn + cols, SVV_COLS - cols), memmove(mx, mx + cols, SVV_COLS - cols);
      for (int c = 0; c < cols; ++c) {
        int lo = 0, hi = 0;
        for (int i = 0; i < per; ++i) {
          const int16_t* x = e[n - cols * per + c * per + i];
          if (x[0] < lo) lo = x[0];
          if (x[1] > hi) hi = x[1];
        }
        mn[SVV_COLS - cols + c] = (signed char)(lo / 258), mx[SVV_COLS - cols + c] = (signed char)(hi / 258);
      }
      keep = SVV_COLS;
    }
    v.ncols = keep;
    memcpy(v.smin, mn, sizeof mn), memcpy(v.smax, mx, sizeof mx);
    uint16_t pk[core::ui::kTapCh];
    core::ui::tap_take_peaks(pk);
    int ch = core::ui::tap_channel_of_voice(vi), p = ch >= 0 && ch < core::ui::kTapCh ? pk[ch] : 0;
    static int lev = 0;
    int nl = p * 127 / 32767;
    lev = nl > lev ? nl : lev - 6 < nl ? nl : lev - 6;   // rise at once, fall ~60 a second
    v.level = lev < 0 ? 0 : lev;
  }
  // the machine's panel
  int mach = v.sidecar ? vb & 0x78 : -1;
  v.mode = mach == core::kFm ? SVV_FM : mach == core::kMultiMachine ? SVV_MULTI : mach == core::kDaisy ? SVV_DAISY : SVV_PLAIN;
  if (v.mode == SVV_FM) fill_fm(v);
  else if (v.mode == SVV_MULTI) fill_multi(v, core::ui::remote_track_voice(t), t);
  else if (v.mode == SVV_DAISY) fill_daisy(v);
  // the p-locks: the Rytm's pattern, its steps
  v.pattern = pattern;
  if (pat && pat->valid && t >= 0 && t < RYTM_TRACKS) {
    int st = pat->t[t].steps;
    v.steps = st < 1 ? 16 : st > SVV_STEPS ? SVV_STEPS : st;
    for (int s = 0; s < v.steps; ++s) {
      v.locks[s] = (unsigned char)rytm_locks_on(pat, t, s);
      v.trig[s] = (pat->t[t].trig[s] & RYTM_TRIG_ON) ? 1 : 0;
    }
  }
  uint32_t h = 2166136261u;
  const unsigned char* b = (const unsigned char*)&v;
  for (size_t i = 0; i < offsetof(sidecar_voice_view_t, gen); ++i) h = (h ^ b[i]) * 16777619u;
  v.gen = h;
  *o = v;
}

bool ComboMap() { return core::ui::keys_map_on(); }

// a display is up: the device's pages that have a title on its own screen and a page here (SONG REC SCOPE STATUS PROJECT
// POOL) are the touchscreen's. core/ui/remote.cpp's one signal for "a touchscreen is up" (remote_display_present) is
// these pages being owned, so this is called once the panel has initialised, never when it was only configured.
void OwnPages() {
  const sc_keymap_t* m = sc_keymap();
  uint16_t own = 0;
  for (int i = 0; i < m->npages; ++i)
    if (m->pages[i].rytm && m->pages[i].code < 16 && touch_page(m->pages[i].name) >= 0) own |= (uint16_t)(1u << m->pages[i].code);
  core::ui::remote_display_owns(own);
}

}  // namespace voice_pi
