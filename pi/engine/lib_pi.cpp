// pi/engine/lib_pi.cpp - see lib_pi.h.
#include "lib_pi.h"

#include <string.h>

#include "bundle/bundle.h"
#include "bundle_pi.h"
#include "core_port.h"
#include "lib/library.h"
#include "lib/pools.h"
#include "msamp/cardq.h"
#include "pool/pool.h"

namespace lib_pi {

namespace {
void rules(core::lib::IoRule* r) {
  bundle_pi::io_rule(&r->read_piece, &r->write_piece, &r->read_gap_us, &r->why);
}
bool card_busy() { return core::cardq::worker() && core::cardq::busy(); }
bool other_job() { return core::bundle::busy(); }
// the current project's references: its three POOL lists' Refs, members included (core/lib/pools.h refs(): any core,
// each ref copied under the list's sequence counter); their hints handed over as pointers
char s_Refs[4096][core::lib::kPath];
int current_refs(const char** out, int max) {
  struct Ctx {
    const char** out;
    int max, n;
  } c = {out, max < 4096 ? max : 4096, 0};
  for (int l = 0; l < core::pools::kLists; ++l)
    core::pools::refs(l, [](void* x, int, int, int, const core::lib::Ref& r) {
      Ctx* k = (Ctx*)x;
      if (k->n >= k->max || r.hint[0] != '/') return;
      sniprintf(s_Refs[k->n], sizeof s_Refs[k->n], "%s", r.hint);
      k->out[k->n] = s_Refs[k->n];
      ++k->n;
    }, &c);
  return c.n;
}
char s_Project[24] = "";
}  // namespace

void begin(bool card_ok) {
  // (the pool lists are set up by nm::begin, engine_begin, before; current_refs reads them only once they exist)
  core::lib::Env e{};
  e.rules = rules;
  e.card_busy = card_busy;
  e.other_job = other_job;
  e.current_refs = current_refs;
  core::lib::set_env(e);
  core::lib::begin(card_ok);
}

bool poll() {
  char p[24];
  memcpy(p, (const char*)core::project, sizeof p);
  p[23] = 0;
  if (strcmp(p, s_Project)) {   // another project: its references first
    memcpy(s_Project, p, sizeof s_Project);
    core::lib::refs_changed();
  }
  return core::lib::step();
}

bool command(const char* line) {
  char out[400];
  if (!core::lib::command(line, out, sizeof out)) return false;
  core::log(CORE_STR("%s"), out);
  return true;
}

int status_json(char* out, int cap) { return core::lib::status_json(out, cap); }

void ui_fill(sidecar_status_view_t* v) {
  core::lib::Stats s = core::lib::stats();
  v->lib_state = s.state;
  v->lib_files = s.files, v->lib_hashed = s.hashed, v->lib_pending = s.unhashed, v->lib_dups = s.dups;
  v->lib_missing = s.gone, v->lib_pct = s.pct;
  sniprintf(v->lib_waiting, sizeof v->lib_waiting, "%s", s.waiting);
}

}  // namespace lib_pi
