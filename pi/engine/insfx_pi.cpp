// pi/engine/insfx_pi.cpp - see insfx_pi.h. Hosted (the core's flags); core 0.
#include "insfx_pi.h"
#include "core_port.h"   // sniprintf: this build has no snprintf

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "device/device.h"
#include "fx/insert.h"
#include "fx/master.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "machines.h"
#include "ui/remote.h"

namespace insfx_pi {

// 10b: both pages follow the slot the touchscreen follows (core::ui::remote_display_slot: the box whose page or display
// report came last). Off the control core every settings reader (param, send_knob, the master's) is that slot's
// (core/pool/pool.cpp fx_slot) and the remote_* readers too; the structures read here directly are named by it.
void Fill(sidecar_insfx_view_t* o, const float (*peaks)[2]) {
  using namespace core::ins;
  const int sl = core::ui::remote_display_slot();
  const Control& control = control_of(sl);
  const core::fx::Control& fxc = core::fx::control_of(sl);
  const core::out::Control& oc = core::out::control_of(sl);
  // the meters are the base box's output stage's (kernel.cpp g_Output): another slot's voices show none
  if (sl != core::slot_of_voice(core::device::base_voice_base())) peaks = nullptr;
  sidecar_insfx_view_t v;
  memset(&v, 0, sizeof v);
  v.valid = 1;
  int t = core::ui::remote_selected_track(nullptr);
  v.track = t;
  int vi = t >= 0 ? core::ui::remote_track_voice(t) : -1;
  v.voice = vi >= 0 ? core::box_voice(vi) : -1;   // the track's voice's chain (its box's V1..V8)
  for (int c = 0; c < kChains && c < SIF_CHAINS; ++c)
    for (int s = 0; s < kSlots; ++s) {
      sidecar_insfx_slot_t& x = v.slot[c][s];
      int ty = control.type[c][s];
      const Labels& L = labels(ty);
      x.type = ty;
      strncpy(x.name, L.name, sizeof x.name - 1);
      for (int k = 0; k < kParams; ++k) {
        strncpy(x.label[k], L.label[k], sizeof x.label[k] - 1);
        x.v[k] = control.play[c][s][k];
        x.base[k] = (uint8_t)param(c, s, k);   // as set: where a touch drag starts (the played value carries an LFO / a lock)
        x.def[k] = L.def[k];
        if (ty) slot_value_text(c, s, k, param(c, s, k), x.val[k], sizeof x.val[k]);
      }
    }
  for (int c = 0; c < kChains && c < SIF_CHAINS; ++c) {   // ins1112: the chain's POST EQ, "" when it is flat
    if (!eq_on(c)) continue;
    const Labels& L = eq_labels();
    int n = 0;
    for (int k = 0; k < kEqParams; ++k) {
      char b[12];
      eq_value_text(k, eq_playing(c, k), b, sizeof b);
      n += sniprintf(v.peq[c] + n, sizeof v.peq[c] - (size_t)n, "%s%s %s", k ? "  " : "", L.label[k], b);
      if (n >= (int)sizeof v.peq[c] - 1) break;
    }
  }
  for (int i = 0; i < kVoices; ++i)
    for (int b = 0; b < kBuses; ++b) v.send[i][b] = (uint8_t)send_knob(i, b);
  for (int k = 0; k < 2; ++k) {
    const char* n = core::fx::type_name(fxc.knob[k][0], k);
    strncpy(v.bus_fx[k], n ? n : "?", sizeof v.bus_fx[k] - 1);
    v.bus_post[k] = fxc.route == core::fx::kRouteMain || (fxc.route == core::fx::kRouteSlot && fxc.knob[k][1] >= 64);
  }
  v.bus_post[2] = control.route3;
  v.out_def[0] = 100, v.out_def[1] = 64, v.out_def[2] = 64;   // LEV unity, PAN centre, WID as rendered (core/fx/out.h)
  for (int i = 0; i < kVoices; ++i) {   // the OUT route
    v.out_route[i] = oc.route[i];
    const uint8_t pv[3] = {oc.lev[i], oc.pan[i], oc.wid[i]};
    for (int k = 0; k < 3; ++k) {
      v.out_v[i][k] = pv[k], v.out_base[i][k] = (uint8_t)core::out::param(i, k);
      core::out::value_text(k, core::out::param(i, k), v.out_val[i][k], sizeof v.out_val[i][k]);
    }
    for (int c = 0; c < 2 && peaks; ++c) {
      float lim = v.out_route[i] == core::out::kPre ? core::out::kLimitPre : core::out::kLimitPost, p = peaks[i][c] / lim;
      v.out_peak[i][c] = (unsigned char)(p >= 1.0f ? 127 : p <= 0.0f ? 0 : p * 127.0f);
    }
  }
  uint32_t h = 2166136261u;
  const unsigned char* b = (const unsigned char*)&v;
  for (size_t i = 0; i < offsetof(sidecar_insfx_view_t, gen); ++i) h = (h ^ b[i]) * 16777619u;
  v.gen = h;
  *o = v;
}

void FillMaster(sidecar_mfx_view_t* o) {
  using namespace core::ins;
  namespace m = core::master;
  const int sl = core::ui::remote_display_slot();   // (10b: as Fill)
  const Control& control = control_of(sl);
  const m::Control& mc = m::control_of(sl);
  float in, out;
  m::take_peaks(sl, &in, &out);
  sidecar_mfx_view_t v;
  memset(&v, 0, sizeof v);
  v.valid = 1, v.on = m::on(sl), v.byp = mc.byp, v.mix = mc.src == m::kMix, v.lev = mc.lev;
  v.lev_def = m::kLevUnity;
  // the stems' report is the Rytm's (its slot's master: m::rytm(); known is set there only)
  v.rep = mc.known, v.onmain = (unsigned char)(mc.known ? m::stems_on_main() : 0);
  v.in = (unsigned char)(in >= 1.0f ? 127 : in * 127.0f), v.out = (unsigned char)(out >= 1.0f ? 127 : out * 127.0f);
  for (int i = 0; i < m::kSlots; ++i) {
    const int c = m::chain(i), s = m::slot(i), ty = control.type[c][s];
    sidecar_mfx_slot_t& x = v.slot[i];
    const Labels& L = labels(ty);
    x.type = ty;
    strncpy(x.name, L.name, sizeof x.name - 1);
    strncpy(x.next, type_name(m::next_type(i, ty, 1)), sizeof x.next - 1);
    for (int k = 0; k < kParams; ++k) {
      strncpy(x.label[k], L.label[k], sizeof x.label[k] - 1);
      x.v[k] = control.play[c][s][k];
      x.base[k] = (uint8_t)param(c, s, k);   // as set: where a touch drag starts
      x.def[k] = L.def[k];
      if (ty) slot_value_text(c, s, k, param(c, s, k), x.val[k], sizeof x.val[k]);
    }
    x.mixdef = m::kMixDef;
    x.send = m::send_type(ty), x.mix = mc.mix[i], x.amp = ty == kAmp, x.pair = m::pair(i);
  }
  if (eq_on(m::kEqChain)) {
    const Labels& L = eq_labels();
    int n = 0;
    for (int k = 0; k < kEqParams; ++k) {
      char b[12];
      eq_value_text(k, eq_playing(m::kEqChain, k), b, sizeof b);
      n += sniprintf(v.peq + n, sizeof v.peq - (size_t)n, "%s%s %s", k ? "  " : "", L.label[k], b);
      if (n >= (int)sizeof v.peq - 1) break;
    }
  }
  uint32_t h = 2166136261u;
  const unsigned char* b = (const unsigned char*)&v;
  for (size_t i = 0; i < offsetof(sidecar_mfx_view_t, gen); ++i) h = (h ^ b[i]) * 16777619u;
  v.gen = h;
  *o = v;
}

}  // namespace insfx_pi
