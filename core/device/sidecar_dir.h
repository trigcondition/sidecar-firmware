// core/device/sidecar_dir.h - the Pi's own folder on the card, /.sidecar (design decided 2026-11-21: the card's layout with
// two boxes). What belongs to the Pi and not to a box lives here, whichever boxes are attached: the network (wifi.txt,
// wpa.conf, wifi.try), the pairing (pair.txt), the screen, CPU clock, fan and USB mode (display.txt), the black box
// (blackbox*.log, logs/), the previous boot's report (prevboot.txt), the storage benches (recspeed.txt, storbench-*,
// sdtest.txt), the firmware update (update/) and the web app (www-next/), and the indexes of the shared libraries (the
// card index env / folders.bin / files.bin, library/, dx7.bin: /Samples, /DX7 and the rest are every box's to read).
//
// Each box's own state stays in its type's folder (/.rytm, /.a4, /.dt: core/device/device.h state_path); a path in
// /.sidecar never passes through a box's folder. Before this build these files were in /.rytm (the Rytm's folder, which a
// second box's state_path then turned into its own folder: a Digitakt in slot 1 read a /.dt/library that was never
// built). migrate() moves them over once, at the engine's first boot on the card.
#pragma once

namespace core {
namespace sidecar_dir {

constexpr char kDir[] = "/.sidecar";

// The Pi-wide names in /.rytm: a name is one when it is a stem below or a stem followed by '.' or '-' (display.txt.prev,
// blackbox.1.log, storbench-SD.txt, www-next.swp). A box's own names (projects, pools, keys.txt ...) are none of them.
bool pi_wide(const char* name);

// Boot (engine_begin, before any read of the card and before any box has a folder): /.sidecar made, then every Pi-wide
// entry of /.rytm renamed into it. An entry already in /.sidecar is kept and the /.rytm one left where it is (logged):
// nothing is overwritten. Returns the entries moved; a card with no /.rytm, or already moved, costs one listing.
int migrate();

}  // namespace sidecar_dir
}  // namespace core
