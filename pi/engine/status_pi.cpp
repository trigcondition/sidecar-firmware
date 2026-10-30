// pi/engine/status_pi.cpp - see status_pi.h.
#include "status_pi.h"

#include "bridge.h"
#include "inst.h"
#include "platform.h"
#include "ui/remote.h"

namespace status_pi {

void Core(sidecar_status_view_t* o) {
  for (int v = 0; v < SST_VOICES; ++v)   // LIVE IN: its input level / gate / feedback mute instead ("LIVE -18DB")
    if (!bridge::livein_status(v, o->mach[v], sizeof o->mach[v]))
      sniprintf(o->mach[v], sizeof o->mach[v], "%s", bridge::machine_name(v));
  // E0 (core/inst.h): a voice playing the sequencer's instances shows how many ("PLAITS x4"); the pool's use, the
  // measured cost of one instance (their render time over the instances that rendered), the RAM of one
  int used = 0, meas = 0, n = 0;
  for (int v = 0; v < SST_VOICES; ++v) {
    core::inst::VoiceStat s = core::inst::stat(v);
    if (s.path == 3 && s.instances > 1)
      sniprintf(o->mach[v], sizeof o->mach[v], "%.6s x%d", bridge::machine_name(v), s.instances);
    if (s.path == 3) used += s.sounding;
    if (s.path == 3 && s.meas100 > 0 && s.sounding > 0) meas += s.meas100, n += s.sounding;
  }
  o->inst_used = used, o->inst_total = core::inst::kTotal;
  o->inst_cpu100 = n ? meas / n : -1;
  o->inst_kb = core::inst::inst_bytes() >> 10;
  // the newest of the mods' messages seen (rytm-mods 0026: 35 hello 1023, 33 machines 1025, 29 / 2A the SRC view 1027,
  // 49 the display pages and the selected track 1036b)
  const core::ui::RemoteStats& rs = core::ui::remote_stats();
  uint32_t sel_gen = 0;
  int sel = core::ui::remote_selected_track(&sel_gen);
  const char* lvl = core::ui::remote_display_request(nullptr) || sel_gen || sel >= 0 ? "RUI1036"
                    : rs.src_views || rs.src_vals                                ? "RUI1027"
                    : rs.machines                                                ? "RUI1025"
                    : rs.hellos                                                  ? "RUI1023"
                                                                                 : nullptr;
  if (lvl) sniprintf(o->rytm_mod, sizeof o->rytm_mod, "%s+ (SEEN)", lvl);
  else o->rytm_mod[0] = 0;
}

}  // namespace status_pi
