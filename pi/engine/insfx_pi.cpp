// pi/engine/insfx_pi.cpp - see insfx_pi.h. Hosted (the core's flags); core 0.
#include "insfx_pi.h"
#include "core_port.h"   // sniprintf: this build has no snprintf

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "fx/insert.h"
#include "fx/out.h"
#include "fx/send_fx.h"
#include "ui/remote.h"

namespace insfx_pi {

void Fill(sidecar_insfx_view_t* o, const float (*peaks)[2]) {
  using namespace core::ins;
  sidecar_insfx_view_t v;
  memset(&v, 0, sizeof v);
  v.valid = 1;
  int t = core::ui::remote_selected_track(nullptr);
  v.track = t;
  int vi = t >= 0 ? core::ui::remote_track_voice(t) : -1;
  v.voice = vi >= 0 && vi < kVoices ? vi : -1;
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
    for (int b = 0; b < kBuses; ++b) v.send[i][b] = control.send[i][b];
  for (int k = 0; k < 2; ++k) {
    const char* n = core::fx::type_name(core::fx::control.knob[k][0], k);
    strncpy(v.bus_fx[k], n ? n : "?", sizeof v.bus_fx[k] - 1);
    v.bus_post[k] = core::fx::control.route == core::fx::kRouteMain ||
                    (core::fx::control.route == core::fx::kRouteSlot && core::fx::control.knob[k][1] >= 64);
  }
  v.bus_post[2] = control.route3;
  for (int i = 0; i < kVoices; ++i) {   // the OUT route
    v.out_route[i] = core::out::control.route[i];
    const uint8_t pv[3] = {core::out::control.lev[i], core::out::control.pan[i], core::out::control.wid[i]};
    for (int k = 0; k < 3; ++k) v.out_v[i][k] = pv[k], core::out::value_text(k, core::out::param(i, k), v.out_val[i][k], sizeof v.out_val[i][k]);
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

}  // namespace insfx_pi
