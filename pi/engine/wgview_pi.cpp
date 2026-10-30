// pi/engine/wgview_pi.cpp - see wgview_pi.h.
#include "wgview_pi.h"

#include <string.h>

#include "platform.h"
#include "granular/granular.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "lib/srclib.h"
#include "msamp/msamp.h"
#include "ui/remote.h"
#include "voices.h"
#include "wavetable/wavetable.h"

namespace wgview_pi {

namespace {
bool shows(int vi) { return vi >= 0 && vi < 8 && (core::v[vi].gr || core::v[vi].wt || core::v[vi].ms); }
uint32_t mix(uint32_t h, uint32_t x) { return (h ^ x) * 16777619u; }
uint32_t fbits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}
core::gr::Viz g_z;   // ~1.2 KB, not on the stack
// a list's machine (SAMPLES: GRANULAR, WAVETABLES: WAVETABLE, INSTRUMENTS: MULTISAMPLE) as the view reads it: entry e
// = code e of its pool list; its loader's state (LOADING, RAM, FAILED: P4) and note, the code a voice plays
struct Mach {
  int (*state)(int);
  int (*sel)(int, int);
  const char* (*note)(int, char*, int);
};
const Mach kMach[core::pools::kLists] = {
    {core::gr::entry_state, core::gr::src_entry, core::gr::entry_note},
    {core::wt::entry_state, core::wt::src_entry, core::wt::entry_note},
    {core::msamp::entry_state, core::msamp::src_entry, core::msamp::entry_note},
};
int list_of(int kind) {
  return kind == WGV_GRANULAR ? core::pools::kSamples : kind == WGV_WAVETABLE ? core::pools::kTables
         : kind == WGV_MULTISAMPLE ? core::pools::kInstr : -1;
}
// the instrument's zones (each key range once, its velocity layers counted) and the keys sounding, as voice_pi's VOICE
// panel reads them (racy reads of a resident slot: drawing only)
void fill_zones(sidecar_wg_view_t* o, int vi) {
  o->ms_ready = 0, o->ms_nz = 0;
  memset(o->ms_keys, 0, sizeof o->ms_keys);
  const core::msamp::Desc* d = core::msamp::slot_desc(core::msamp::voice_slot(vi));
  if (d) {
    int nr = d->nregions < d->max_regions ? d->nregions : d->max_regions;
    o->ms_ready = 1;
    for (int i = 0; i < nr; ++i) {
      const core::msamp::Region& r = d->regions[i];
      if (r.lokey > r.hikey || r.hikey > 127) continue;
      int z = 0;
      while (z < o->ms_nz && !(o->ms_lo[z] == r.lokey && o->ms_hi[z] == r.hikey)) ++z;
      if (z == o->ms_nz) {
        if (o->ms_nz >= WGV_ZONES) continue;
        o->ms_lo[z] = r.lokey, o->ms_hi[z] = r.hikey, o->ms_root[z] = r.keycenter > 127 ? r.lokey : r.keycenter, o->ms_vel[z] = 0;
        ++o->ms_nz;
      }
      if (o->ms_vel[z] < 255) ++o->ms_vel[z];
    }
  }
  uint8_t keys[core::msamp::kPerVoice];
  int n = core::msamp::voice_keys(vi, keys, core::msamp::kPerVoice);
  for (int i = 0; i < n; ++i) o->ms_keys[keys[i] >> 3 & 15] |= (unsigned char)(1u << (keys[i] & 7));
}
// the row holding a browser item already (one entry per hash; an input / built-in by its index), -1 none
int row_of_item(int l, int i) {
  char path[core::srclib::kPath];
  if (!core::srclib::item_path(l, i, path, sizeof path)) return -1;
  int type = 0, id = 0;
  if (core::srclib::special(l, path, &type, &id)) {
    for (int c = 0; c < core::pools::kCodes; ++c) {
      const core::pools::Entry& e = core::pools::at(l, c);
      if (e.type == type && e.id == id) return c;
    }
    return -1;
  }
  core::lib::Ref r;
  return core::lib::ref_for_path(path, &r) ? core::pools::find_hash(l, r) : -1;
}
// One bank of list l into o (the bank the view shows), its states every time (LOADING -> RAM, P3's users), the RAM,
// SELECT UNUSED's rows while the view selects, and the list machine's browser when its folder or the list changed (racy
// reads: drawing only). vi: the voice the view shows (its machine's list: the code it plays), -1 none.
uint32_t fill_pool(sidecar_wg_view_t* o, int l, int vi, int p0) {
  namespace pl = core::pools;
  const Mach& m = kMach[l];
  int bank = sidecar_wgview_bank();
  bank = bank < 0 ? 0 : bank > 7 ? 7 : bank;
  uint32_t h = 2166136261u;
  o->list = l, o->bank = bank;
  for (int r = 0; r < WGV_ROWS; ++r) {
    int c = pl::code(bank, r);
    const pl::Entry& e = pl::at(l, c);
    int t = r == 0 ? (int)pl::kOff : (int)e.type;
    const char* nm = r == 0 ? "OFF" : t == pl::kEmpty ? "" : e.name;
    if (t != pl::kEmpty && r && !nm[0]) {   // (no display name cached: the file's)
      const char* s = strrchr(e.ref.hint, '/');
      nm = s ? s + 1 : e.ref.hint;
    }
    sniprintf(o->rname[r], WGV_NAME, "%s", nm);
    unsigned f = 0;
    if (r && t != pl::kEmpty) {
      int ms = m.state(c);
      if (e.st == core::lib::kMissing) f |= WGV_RF_MISSING;
      else if (e.st == core::lib::kChanged) f |= WGV_RF_CHANGED;
      if (ms == core::gr::kStateLoading) f |= WGV_RF_LOADING;
      else if (ms == core::gr::kStateFailed) f |= WGV_RF_FAILED;
      else if (ms == core::gr::kStateRam || pl::resident(l, c)) f |= WGV_RF_RAM;
      if (pl::used(l, c)) f |= WGV_RF_USED;
      if (pl::has_file(e) && e.ch) f |= e.ch >= 2 ? WGV_RF_ST : WGV_RF_MONO;   // docs/STEREO.md
    }
    int u = r ? pl::users(l, c) : 0;
    o->rtype[r] = (unsigned char)t, o->rflags[r] = (unsigned char)f, o->rusers[r] = (unsigned char)(u > 255 ? 255 : u);
    h = mix(h, (uint32_t)t | f << 8 | (uint32_t)o->rusers[r] << 16);
  }
  h = mix(h, pl::sig(l) * 8u + (uint32_t)bank);
  for (int k = 0; k < pl::kLists; ++k) o->count[k] = pl::count(k), h = mix(h, (uint32_t)o->count[k]);
  o->locked = pl::locked(l);
  sniprintf(o->lock_why, sizeof o->lock_why, "%s", o->locked ? pl::lock_why(l) : "");
  o->playing = pl::playing();
  pl::Ram ram = pl::ram();
  o->ram_known = ram.budget > 0;
  o->ram_mb = (unsigned)((ram.used + ram.tables) >> 20), o->ram_budget_mb = (unsigned)(ram.budget >> 20), o->ram_pct = ram.pct;
  h = mix(h, (uint32_t)o->locked | (uint32_t)o->playing << 1 | (uint32_t)o->ram_known << 2);
  h = mix(h, o->ram_mb * 128u + (uint32_t)o->ram_pct);
  o->nunused = 0;
  if (sidecar_wgview_selecting()) {
    o->nunused = pl::select_unused(l, o->unused, WGV_CODES);
    for (int i = 0; i < o->nunused; ++i) h = mix(h, (uint32_t)o->unused[i]);
    h = mix(h, (uint32_t)o->nunused + 1);
  }
  sniprintf(o->warn, sizeof o->warn, "%s", pl::kUnusedWarning);
  // the code the voice plays (its machine's list only)
  o->sel = vi >= 0 && list_of(o->kind) == l ? m.sel(vi, p0) : -1;
  o->note[0] = 0;
  if (o->sel > 0) m.note(o->sel, o->note, sizeof o->note);
  h = mix(h, (uint32_t)(o->sel + 1));
  for (const char* q = o->note; *q; ++q) h = mix(h, (uint8_t)*q);
  // the browser (srclib's kind = the list's number): again when its folder / listing or the list changed
  // (the rows already holding its files only while FILES is shown: a search of the list an item)
  bool rows = sidecar_wgview_lib_shown();
  uint32_t lg = ((core::srclib::view_gen(l) * 3u + (uint32_t)l) * 2654435761u ^ (rows ? pl::sig(l) : 0u)) * 2u + rows;
  if (lg != o->lib_gen) {
    o->lib_gen = lg;
    sniprintf(o->lib_cwd, sizeof o->lib_cwd, "%s", core::srclib::cwd(l));
    o->lib_total = core::srclib::count(l);
    o->lib_done = core::srclib::listing_done(l);
    int n = 0;
    const char* nm;
    bool dir;
    for (; n < WGV_LIB && core::srclib::item(l, n, &nm, &dir, nullptr); ++n) {
      sniprintf(o->lib_name[n], WGV_LNAME, "%s", nm);
      o->lib_flags[n] = (unsigned char)((dir ? WGV_LIB_DIR : 0) | (nm[0] == '@' || o->lib_cwd[0] == '@' ? WGV_LIB_VIRT : 0));
      o->lib_row[n] = (short)(dir || !rows ? -1 : row_of_item(l, n));
    }
    o->lib_n = n;
  }
  return mix(h, o->lib_gen);
}
}  // namespace

void Fill(sidecar_wg_view_t* o, int want) {
  const core::ui::RemoteStats& rs = core::ui::remote_stats();
  int vi = -1, track = -1;
  if (rs.page == 4) track = rs.voice, vi = core::ui::remote_track_voice(rs.voice);
  else if (rs.page == 1) vi = rs.voice;
  if (!shows(vi)) {   // rui1036b: the Rytm's selected track (49 2), followed as the Rytm selects
    int st = core::ui::remote_selected_track(nullptr), v2 = st >= 0 ? core::ui::remote_track_voice(st) : -1;
    // (touch2: a MULTISAMPLE track only when no voice was tapped: a home page tap on a GRAN / WAVE voice keeps its view)
    if (shows(v2) && (want < 0 || !core::v[v2].ms || core::v[v2].gr || core::v[v2].wt)) vi = v2, track = st;
  }
  if (!shows(vi)) vi = shows(want) ? want : -1, track = -1;
  for (int k = 0; k < 8 && vi < 0; ++k)
    if (shows(k)) vi = k;
  uint32_t h = 2166136261u;
  o->kind = WGV_NONE;
  o->voice = vi < 0 ? 0 : vi;
  o->track = track;
  o->n = 0, o->ncols = 0;
  o->name[0] = 0;
  int p0 = vi >= 0 ? core::srclib::code_of(core::v[vi].p[0]) : 0;   // rui1038: the source code (bank * 128 + index)
  if (vi >= 0 && core::v[vi].gr && core::gr::viz(vi, &g_z)) {
    o->kind = WGV_GRANULAR;
    int e = g_z.entry >= 0 ? g_z.entry : core::gr::src_entry(vi, p0);
    sniprintf(o->name, sizeof o->name, "%s", e >= 0 ? core::gr::entry_name(e) : "");
    o->live = g_z.live, o->frozen = g_z.frozen, o->head = g_z.head, o->anim = g_z.anim;
    core::gr::hold_text((int)core::v[vi].p[10], o->hold_text, sizeof o->hold_text);
    core::gr::anim_text((int)core::v[vi].p[11], o->anim_text, sizeof o->anim_text);
    o->ncols = core::gr::viz_wave(vi, WGV_COLS, o->wmn, o->wmx);
    o->n = g_z.n < WGV_GRAINS ? g_z.n : WGV_GRAINS;
    for (int i = 0; i < o->n; ++i) {
      o->gpos[i] = g_z.pos[i], o->gamp[i] = g_z.amp[i], o->grev[i] = g_z.rev[i];
      h = mix(h, (uint32_t)(g_z.pos[i] * 400) | (uint32_t)(g_z.amp[i] * 15) << 12);
    }
    h = mix(h, (uint32_t)(g_z.head * 400) | (uint32_t)g_z.live << 12 | (uint32_t)g_z.frozen << 13);
    if (g_z.live && !g_z.frozen) h = mix(h, core::now_ms() / 100);   // the ring scrolls
  } else if (vi >= 0 && core::v[vi].wt) {
    o->kind = WGV_WAVETABLE;
    int e = core::wt::voice_entry(vi);
    sniprintf(o->name, sizeof o->name, "%s", e >= 0 ? core::wt::entry_name(e) : "");
    o->pos = core::wt::viz_pos(vi, &o->frames);
    if (o->frames > 0) {
      o->ncols = core::wt::viz_frame(vi, o->pos, WGV_COLS, o->cur);
      static int s_e = -2, s_nf = -1;
      if (e != s_e || o->frames != s_nf) {   // the stack only when the table changed
        for (int k = 0; k < WGV_STACK; ++k) core::wt::viz_frame(vi, (float)k / (WGV_STACK - 1), WGV_COLS, o->stack[k]);
        s_e = e, s_nf = o->frames;
      }
    }
    h = mix(h, fbits(o->pos) >> 12);
    h = mix(h, (uint32_t)(e + 7) * 977u + (uint32_t)o->frames);
  } else if (vi >= 0 && core::v[vi].ms) {   // touch2: MULTISAMPLE, its zones
    o->kind = WGV_MULTISAMPLE;
    int e = core::msamp::src_entry(vi, p0);
    sniprintf(o->name, sizeof o->name, "%s", e > 0 ? core::msamp::entry_name(e) : "");
    fill_zones(o, vi);
    h = mix(h, (uint32_t)o->ms_nz * 2u + (uint32_t)o->ms_ready);
    for (int z = 0; z < o->ms_nz; ++z) h = mix(h, (uint32_t)o->ms_lo[z] << 16 | (uint32_t)o->ms_hi[z] << 8 | o->ms_vel[z]);
    for (int k = 0; k < 16; ++k) h = mix(h, o->ms_keys[k]);
  }
  h = mix(h, (uint32_t)o->kind * 31u + (uint32_t)o->voice);
  // the POOL: the POOL page's tab, else the voice's machine's list
  int l = sidecar_wgview_pool_page_shown() ? sidecar_wgview_page_list() : list_of(o->kind);
  if (l >= 0 && l < core::pools::kLists) h = mix(h, fill_pool(o, l, o->kind ? vi : -1, p0));
  else o->list = -1, o->sel = -1, o->note[0] = 0;
  for (const char* q = o->name; *q; ++q) h = mix(h, (uint8_t)*q);
  o->gen = h | 1;
}

}  // namespace wgview_pi
