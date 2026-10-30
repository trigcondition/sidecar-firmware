// pi/engine/bundle_pi.cpp - see bundle_pi.h.
#include "bundle_pi.h"

#include <string.h>

#include "bridge.h"
#include "bundle/bundle.h"
#include "bundle/feed.h"
#include "core_port.h"
#include "msamp/cardq.h"
#include "pool/pool.h"
#include "song/song.h"
#include "stor_vol.h"
#include "version.h"

namespace bundle_pi {

namespace {
volatile bool g_playing = false;
volatile int g_rec = 0;
void (*g_cmd)(const char*) = nullptr;

void rules(core::bundle::IoRule* r) {
  using namespace core::bundle;
  if (g_rec == 2) *r = {0, 0, 0, "RECORDING"};
  else if (g_playing) *r = {kSlowPiece, 0, kSlowGapUs, "PLAYING"};
  else if (core::song::status().state == core::song::kRec) *r = {kSlowPiece, 0, kSlowGapUs, "SONG REC"};
  else *r = {kPiece, kPiece, 0, ""};
}
bool card_busy() { return core::cardq::worker() && core::cardq::busy(); }
void current(char* out, int cap) {   // core 3's (a stale copy at worst)
  char p[24];
  memcpy(p, (const char*)core::project, sizeof p);
  p[23] = 0;
  sniprintf(out, cap, "%s", p);
}
bool song_recording() {
  uint8_t s = core::song::status().state;
  return s == core::song::kRec || s == core::song::kSaving;
}
// what an import put on the card: the machine list again (/Plugins), the browsers' listings of that folder (srclib's
// browse=changed:, its older name lib=changed:); the library already has every file the import wrote (lib::touched)
void changed(const char* path) {
  if (!g_cmd) return;
  if (!strncmp(path, "/Plugins", 8)) g_cmd("um=rescan");
  else {
    char c[200];
    sniprintf(c, sizeof c, "browse=changed:%s", path);
    g_cmd(c);
  }
}
// the USB drives (stor_vol.h, core 0's like the job): mounted ones as bundle destinations / sources
int vol_index(const char* drive) {
  for (int v = stor::kUSB; v <= stor::kUSB3; ++v)
    if (!strcmp(stor::drive(v), drive)) return v;
  return -1;
}
int volumes(core::bundle::Volume* out, int max) {
  int n = 0;
  for (int v = stor::kUSB; v <= stor::kUSB3 && n < max; ++v) {
    if (!stor::mounted(v)) continue;
    const stor::Status& st = stor::status(v);
    core::bundle::Volume& o = out[n++];
    memset(&o, 0, sizeof o);
    sniprintf(o.drive, sizeof o.drive, "%s", stor::drive(v));
    sniprintf(o.name, sizeof o.name, "%s", stor::name(v));
    o.free = st.free, o.total = st.total, o.free_known = st.free_known;
  }
  return n;
}
bool vol_ok(const char* drive) {
  int v = vol_index(drive);
  return v >= 0 && stor::mounted(v);
}
const char* vol_eject(const char* drive) {
  int v = vol_index(drive);
  return v < 0 ? "not a USB drive" : stor::eject(v);
}
}  // namespace

void io_rule(uint32_t* rp, uint32_t* wp, uint32_t* gap, const char** why) {
  core::bundle::IoRule r;
  rules(&r);
  *rp = r.read_piece, *wp = r.write_piece, *gap = r.read_gap_us, *why = r.why;
}

void init(void (*command)(const char*)) {
  g_cmd = command;
  core::bundle::Env e{};
  e.rules = rules;
  e.card_busy = card_busy;
  e.current = current;
  e.song_recording = song_recording;
  e.changed = changed;
  e.version = SIDECAR_VERSION;
#if RASPPI == 5
  e.board = "pi5";
#else
  e.board = "pi4";
#endif
  e.flush_ms = 1500;
  e.volumes = volumes;
  e.vol_ok = vol_ok;
  e.vol_eject = vol_eject;
  core::bundle::set_env(e);
  core::bundle::feed::set_available(core::card_ok);   // the PROJECT pages: shown while the card is up
}

void transport(uint8_t st) {
  if (st == 0xFA || st == 0xFB) g_playing = true;
  else if (st == 0xFC) g_playing = false;
}

bool poll(int recorder_state) {
  g_rec = recorder_state;
  core::bundle::feed::set_available(core::card_ok);
  bool did = core::bundle::step();
  core::bundle::feed::tick();   // the Rytm page's commands, the lists when owed, the status published on a change
  return did;
}

bool command(const char* line) {
  char out[200];
  if (!core::bundle::feed::command(line, out, sizeof out)) return false;
  core::log(CORE_STR("%s"), out);
  return true;
}

void ui_fill(sidecar_project_view_t* v) {
  using namespace core::bundle;
  static feed::Feed f;   // (core 0's: the display's loop)
  if (!feed::copy(&f)) {
    memset(v, 0, sizeof *v);
    return;
  }
  v->valid = 1;
  v->gen = f.gen, v->reading = f.reading;
  v->nproj = f.nproj < SPJ_PROJECTS ? f.nproj : SPJ_PROJECTS;
  for (int i = 0; i < v->nproj; ++i) {
    const ProjectRow& r = f.proj[i];
    spj_project_t& p = v->proj[i];
    sniprintf(p.key, sizeof p.key, "%s", r.key), sniprintf(p.rytm, sizeof p.rytm, "%s", r.rytm);
    sniprintf(p.date, sizeof p.date, "%s", r.date);
    p.files = r.files, p.used = r.used, p.bytes = r.bytes, p.current = r.current, p.imported = r.imported;
  }
  v->nvol = f.nvol < SPJ_VOLS ? f.nvol : SPJ_VOLS;
  for (int i = 0; i < v->nvol; ++i) {
    sniprintf(v->vol[i].drive, sizeof v->vol[i].drive, "%s", f.vol[i].drive);
    sniprintf(v->vol[i].name, sizeof v->vol[i].name, "%s", f.vol[i].name);
    v->vol[i].free = f.vol[i].free, v->vol[i].total = f.vol[i].total, v->vol[i].free_known = f.vol[i].free_known;
  }
  sniprintf(v->src, sizeof v->src, "%s", f.src);
  v->nbund = f.nbund < SPJ_BUNDLES ? f.nbund : SPJ_BUNDLES;
  for (int i = 0; i < v->nbund; ++i) {
    const BundleRow& r = f.bund[i];
    spj_bundle_t& b = v->bund[i];
    sniprintf(b.name, sizeof b.name, "%s", r.name), sniprintf(b.path, sizeof b.path, "%s", r.path);
    sniprintf(b.date, sizeof b.date, "%s", r.date);
    b.size = r.size, b.incomplete = r.incomplete;
  }
  v->card_free = f.card_free;
  v->eject_n = f.eject_n;
  sniprintf(v->eject_msg, sizeof v->eject_msg, "%s", f.eject_msg);
  const Status& s = f.st;
  v->seq = s.seq, v->job = s.job, v->phase = s.phase, v->done = s.done, v->total = s.total, v->bytes = s.bytes;
  sniprintf(v->key, sizeof v->key, "%s", s.key), sniprintf(v->file, sizeof v->file, "%s", s.file);
  sniprintf(v->msg, sizeof v->msg, "%s", s.msg), sniprintf(v->waiting, sizeof v->waiting, "%s", s.waiting);
  sniprintf(v->job_vol, sizeof v->job_vol, "%s", s.vol);
  v->have_plan = f.have_plan;
  const PlanInfo& p = f.plan;
  sniprintf(v->plan_name, sizeof v->plan_name, "%s", p.name), sniprintf(v->plan_from, sizeof v->plan_from, "%s", p.from);
  sniprintf(v->plan_step, sizeof v->plan_step, "%s", p.step), sniprintf(v->plan_rytm, sizeof v->plan_rytm, "%s", p.rytm_file);
  v->plan_clash = p.clash, v->plan_slot = p.slot, v->plan_pick = p.pick, v->plan_new = p.new_bytes, v->plan_reused = p.reused_bytes;
  v->plan_write = p.write, v->plan_reuse = p.reuse, v->plan_samples = p.samples, v->plan_songs = p.songs;
  v->plan_patterns = p.patterns, v->plan_missing = p.missing;
  v->ntrash = f.ntrash < SPJ_TRASH ? f.ntrash : SPJ_TRASH;   // 2026-10-27: the project trash
  for (int i = 0; i < v->ntrash; ++i) {
    const core::prjslot::TrashRow& r = f.trash[i];
    spj_trash_t& t = v->trash[i];
    t.id = r.id, t.items = r.items;
    sniprintf(t.key, sizeof t.key, "%s", r.key), sniprintf(t.name, sizeof t.name, "%s", r.name);
    sniprintf(t.why, sizeof t.why, "%s", r.why), sniprintf(t.date, sizeof t.date, "%s", r.date);
  }
  sniprintf(v->prj_last, sizeof v->prj_last, "%s", f.prj_last);
  v->nmach = p.nmach < SPJ_MACH ? p.nmach : SPJ_MACH;
  for (int i = 0; i < v->nmach; ++i) {
    v->mach[i].slot = p.mach[i].slot, v->mach[i].action = p.mach[i].action;
    sniprintf(v->mach[i].name, sizeof v->mach[i].name, "%s", p.mach[i].name);
    sniprintf(v->mach[i].ver, sizeof v->mach[i].ver, "%s", p.mach[i].ver);
  }
}

int status_json(char* out, int cap) {
  using namespace core::bundle;
  const Status& s = status();
  const Stats& t = stats();
  return sniprintf(out, cap, ",\"bundle\":{\"phase\":\"%s\",\"done\":%llu,\"total\":%llu,\"waiting\":\"%s\",\"steps\":%lu,"
                   "\"held\":%lu,\"maxRead\":%lu,\"maxWrite\":%lu,\"maxUs\":%lu}",
                   phase_name(s.phase), (unsigned long long)s.done, (unsigned long long)s.total, s.waiting,
                   (unsigned long)t.steps, (unsigned long)t.waits, (unsigned long)t.max_read, (unsigned long)t.max_write,
                   (unsigned long)t.max_us);
}

}  // namespace bundle_pi
