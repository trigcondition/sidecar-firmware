// core/bundle/kits.h - the Sidecar's per-kit data follows the Rytm's SAVE KIT / RELOAD KIT, as its patterns follow
// SAVE / RELOAD PATTERN (seq.cpp on_save_reload, 1042). The Rytm keeps a working and a saved copy of every kit; so does
// the Sidecar now. Pi and Mac only (CORE_MSAMP).
//
// The Rytm (rytm-mods 0026 rui1052, 1052d) reports, F0 7D 52 54 4C 01 <op> <kit hi lo> <src hi lo> F7 (kits 1-based):
//   01 SAVE <kit> <src>     SAVE KIT / YES + KIT: the working kit src saved as kit (kit != src: SAVE KIT to another
//                           kit, which the Rytm then makes the active one)
//   02 RELOAD <kit>         RELOAD KIT / NO + KIT: the working kit back to its saved state
//   03 RESTORE <kit>        the working copy kept aside by the last reload back, the one it replaces kept aside in
//                           turn (a swap: rytm-mods 0010's EDITS RESTORED; no rui build sends it today: 0010
//                           cannot be built with 0021)
//   04 RELOAD ALL <kit>     the KIT menu's RELOAD set to ALL: every kit back to its saved state
//
// The working copy is what it was: each feature's per-kit file (/.rytm/lfo/<key>/kNNN.txt, /.rytm/ins/<key>/kNNN.txt).
// The saved copy is one record per kit holding every per-kit feature, /.rytm/kits/<key>/kNNN.sks: a comment line, then
// for each feature "=<name>" and that feature's file text as it was at the save, or "#none" when the kit had no file of
// its own (it inherits the project's default file: a reload keeps it inheriting; a kit that has its own file by then
// gets the default's text as it is now). A reload first keeps the working copy as the same kind of record, kNNN.prv
// (one deep, as a pattern's .prv) - a feature whose working text is already the saved one keeps the old .prv's section
// (a reload cut between its features' writes and done again never keeps the half-reloaded copy aside; 0.3.1: also when
// nothing was cut, a kit unedited since its save keeps an older .prv - harmless while no UI sends RESTORE) - then writes
// each feature's section over its working file. A reload of a kit with no saved record (the Sidecar never saw a SAVE of
// it) keeps the working copy: nothing is replaced by a guess. A file the store holds as unreadable (kLocked) or does not
// know (kUnknown) refuses the whole operation. RELOAD ALL looks for the records kScanPass kits a pass, then reloads four
// a pass.
//
// Both copies live in the RAM store (want_files: the fill reads them with the project's other files) and on the card
// (posted writes, fmt::post_queue): no card call on the control core. The key is the project's (slot keys too: the
// folder is one of bundle.cpp's kProjectFiles, so a slot's copy, move and trash take it along).
//
// Events wait (in order, at most kPending) while the store fills, a slot job, a rename or a question is under way, or a
// switch waits (prjslot::busy). Each carries the generation of the Rytm's project when it came: the Rytm changing
// projects (its ACTIVE on another slot, a LOAD of another slot, a 10 with another name: project_changing) makes the
// waiting ones stale - dropped (logged, counted), never applied to the next project; a RELOAD ALL under way stops.
// A LOST from the Rytm (its events dropped) is logged: a kit SAVE / RELOAD may have been missed (lost()).
#pragma once
#include <stdint.h>

namespace core {
namespace kits {

enum Op : uint8_t { kSave = 1, kReload = 2, kRestore = 3, kReloadAll = 4 };
constexpr int kPending = 16;

void sysex(const uint8_t* d, int len);   // a 4C from the Rytm (host_link.cpp)
bool tick();                             // idle_save, once the store is ready: the events waiting; true if it did one
void want_files();                       // the fill's paths: every kit's .sks and .prv of the project now named
void project_changing();                 // the Rytm is on another project now (prjslot: ACTIVE / LOAD of another slot)
void name_seen(const uint8_t* n, int len);   // the Rytm's 10 <name> (host_link): another name -> project_changing
                                             // (name mode only: in slot mode ACTIVE / LOAD say it, in order)
void lost(int n);                        // the Rytm's LOST (prjslot): kit reports may be among them (logged)
void reset();                            // tests

// kept_aside: .prv sections kept from the old .prv (a reload again after a cut); stale: events (or a RELOAD ALL) of a
// project the Rytm left; lost: LOSTs seen
struct Stats { uint32_t saves, reloads, restores, reload_alls, kept, refused, dropped, kept_aside, stale, lost, scan_max; };
const Stats& stats();

}  // namespace kits
}  // namespace core
