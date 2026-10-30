// core/tests/demo/bundle_tool.cpp - the Sidecar's bundle job (core/bundle) on a Mac, over a folder that stands in for
// the card (a copy of a Sidecar SD card, or tools/make_bundle_fixture.py's): make -C core bundle-tool, then
//   build/bundle_tool CARD projects                 the projects the card knows (JSON)
//   build/bundle_tool CARD export KEY [EPOCH]       -> CARD/Bundles/<KEY> <time>.sidecar.zip (the path printed)
//   build/bundle_tool CARD plan BUNDLE              verify + plan an import (BUNDLE: a card path, /Bundles/...)
//   build/bundle_tool CARD import BUNDLE [NAME]     verify, plan, import (the summary printed)
//   build/bundle_tool CARD recover                  the boot's recovery of a cut import
// Exactly what the Pi runs, a step at a time, with no card queue and no pause rules.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "bundle/bundle.h"
#include "bundle/projects.h"
#include "platform.h"

using namespace core;

static int run() {
  for (int i = 0; i < 10000000 && bundle::busy(); ++i) bundle::step();
  const bundle::Status& s = bundle::status();
  fprintf(stderr, "bundle: %s: %s\n", bundle::phase_name(s.phase), s.msg);
  return s.phase == bundle::kFailed || s.phase == bundle::kCancelled ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: bundle_tool CARD projects | export KEY [EPOCH] | plan BUNDLE | import BUNDLE [NAME] | recover\n");
    return 2;
  }
  host_set_card(argv[1]);
  host_set_log_sink([](const char*, int) {});
  bundle::Env env{};
  env.version = "mac-tool";
  env.board = "mac";
  bundle::set_env(env);
  projects::load();
  static char out[1 << 20];
  std::string op = argv[2];
  if (op == "projects") {
    bundle::projects_json(out, sizeof out);
    printf("%s\n", out);
    return 0;
  }
  if (op == "recover") {
    bundle::recover();
    return 0;
  }
  if (op == "export" && argc >= 4) {
    bundle::ExportOpts o;
    o.epoch = argc >= 5 ? (uint32_t)strtoul(argv[4], nullptr, 10) : (uint32_t)time(nullptr);
    if (!bundle::start_export(argv[3], bundle::kToCard, o)) return fprintf(stderr, "%s\n", bundle::status().msg), 1;
    int rc = run();
    if (!rc) printf("%s\n", bundle::status().file);
    return rc;
  }
  if ((op == "plan" || op == "import") && argc >= 4) {
    if (!bundle::start_verify(argv[3])) return fprintf(stderr, "%s\n", bundle::status().msg), 1;
    if (run()) return 1;
    if (op == "plan") {
      bundle::plan_json(out, sizeof out);
      printf("%s\n", out);
      return 0;
    }
    bundle::ImportOpts o;
    if (argc >= 5) snprintf(o.name, sizeof o.name, "%s", argv[4]);
    if (!bundle::start_import(o)) return fprintf(stderr, "%s\n", bundle::status().msg), 1;
    int rc = run();
    for (int i = 0; i < 8; ++i) projects::save();   // the index's request (the control core's, on the Pi)
    bundle::plan_json(out, sizeof out);
    printf("%s\n", out);
    return rc;
  }
  fprintf(stderr, "unknown: %s\n", op.c_str());
  return 2;
}
