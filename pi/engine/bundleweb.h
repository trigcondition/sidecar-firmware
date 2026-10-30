// pi/engine/bundleweb.h - the web page's project bundles (docs/BUNDLES.md "Web"): the /bundle/* endpoints, /time and
// /usb/* over core/bundle. Portable (hosted flags): web.cpp calls it from its tasks on core 0 (the job's own core: the
// web tasks and the loop's bundle_pi::Poll take turns, Circle's scheduler is cooperative); the Mac tests it on the
// core's host port (pi/engine/tests/test_bundleweb.cpp).
//
//   GET  /bundle/projects                 the card's projects (bundle::projects_json)
//   GET  /bundle/list?src=card|usb[&vol=USB:]  the bundles in /Bundles, or a stick's /SIDECAR and root (each with its path)
//   GET  /bundle/status                   the job: {"job","phase","done","total","key","file","name","msg","waiting","bytes","seq"}
//   GET  /bundle/plan                     an import's summary (READY) or its done screen (DONE)
//   GET  /bundle/zip?off=&len=            a browser export's bytes (SERVE), at most 64 KB; an empty answer with
//                                         Retry-After while paused (the recorder records)
//   GET  /bundle-sw.js                    the download's service worker (secure contexts only: see docs/BUNDLES.md)
//   POST /bundle/export  key=&dest=browser|card|usb[&vol=USB:][&takes=0]   (usb: the first stick when vol= is left out)
//   POST /bundle/verify  path=/Bundles/...sidecar.zip | USB:/[SIDECAR/]...sidecar.zip
//   POST /bundle/import  [name=][&mode=new|replace][&packages=0|1][&skip=<slot bits>][&slot=1..128]  (slot: a card keyed
//                                         by the Rytm's slots - the plan's "slot" / "slots"; a slot with Sidecar files
//                                         only with mode=replace: they go to the project trash)
//   POST /bundle/cancel, /bundle/finish (the done / failed screen dismissed)
//   POST /bundle/delete  key=&confirm=<key>     (2026-10-27: to the project trash, core/bundle/prjslot.h)
//   GET  /bundle/trash                    the project trash: [{"id","key","name","why","date","items"}], the newest first
//   GET  /bundle/prjslot                  {"mode","migrated","active","name","busy","ask","last","trash_next"}
//   POST /bundle/trash/restore  id=[&to=@NNN][&replace=1]   back under its key (or to=); refused over files unless replace
//                                         (they go to the trash first), and for the Rytm's current project
//   POST /bundle/trash/remove   id=&confirm=<id>   one entry for good
//   POST /bundle/trash/empty    confirm=EMPTY      every entry for good
//   POST /time           epoch=<seconds since 1970, UTC>[&tz=<minutes east of UTC>]  (the page's clock and zone: bundle
//                                         names and dates, the take folders' names, the files' dates)
//   GET  /usb/status                      {"present","vols":[{drive,name,free,total,free_known}],"job":<the job's drive>}
//   POST /usb/eject      [vol=USB:]       a job on it cancelled first, then unmounted: safe to remove (409 + why not)
// Every POST needs from_page (web.cpp's strict same-origin check, as the Wi-Fi settings) and its arguments in the
// body; a refused one answers 403.
#ifndef _pi_engine_bundleweb_h
#define _pi_engine_bundleweb_h
#include <stdint.h>

namespace bundleweb {

struct Reply {
  int status;          // 200, 400, 403, 404, 409
  const char* type;    // the Content-Type (with extra header lines after CRLFs, as web.cpp does)
  uint32_t len;        // bytes in out
};
// true: the path is one of these endpoints (r filled); false: not ours
bool handle(const char* path, const char* params, const char* form, bool from_page, uint8_t* out, uint32_t cap, Reply* r);
bool form_value(const char* form, const char* name, char* out, unsigned cap);   // URL-decoded (+ a space)

}  // namespace bundleweb
#endif
