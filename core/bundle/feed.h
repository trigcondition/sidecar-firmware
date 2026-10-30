// core/bundle/feed.h - what the PROJECT pages show and the commands they send (docs/BUNDLES.md "PROJECT pages"): the
// touchscreen's PROJECT page (pi/ui/sidecar_project.c, core 0's loop) and the Rytm's PROJECT remote page
// (core/ui/page_project.cpp, the control core). Pi and Mac only (CORE_MSAMP).
//
// Core 0 (the job's core) publishes a Feed: the card's projects, the mounted USB sticks, the bundles of one source
// (the card's /Bundles, or a stick's /SIDECAR and root), the job's status and an import's summary. A list is read from
// the card only when asked (a page opened, "list"), after a job ends and when the sticks change - never by the
// control core. Any core reads the last Feed (a seqlock copy). The pages' commands are console lines ("bundle=...",
// command() below): the touchscreen runs them through the console on core 0; the Rytm's page posts them into a
// single-producer ring that core 0 drains (tick), so the control core never touches the card.
//
//   bundle                        one line: the job's phase and message
//   bundle=list[:card|USB|USB2|USB3]    the lists read again (the bundles of that source: the card, or a stick)
//   bundle=export:<card|USB|..>:<key>   an export of a project to the card's /Bundles or a stick's /SIDECAR
//   bundle=verify:<path>          a bundle checked (/Bundles/x.sidecar.zip, USB:/SIDECAR/x.sidecar.zip)
//   bundle=import[:<skip bits>[:<packages 0|1>]]   after READY: the import (the slots' machines in skip left out)
//   bundle=delete:<key>           a project's files to the project trash (never the current one; samples kept)
//   bundle=restore:<id>           a trash entry back under its key (core/bundle/prjslot.h; refused over files)
//   bundle=cancel | bundle=finish | bundle=eject:<USB|USB2|USB3>
#pragma once
#include <stdint.h>

#include "bundle.h"
#include "prjslot.h"

namespace core {
namespace bundle {
namespace feed {

constexpr int kProjects = 48, kBundles = 32, kVols = 3, kTrash = 16;
enum Src : int8_t { kSrcCard = 0 };   // a source: 0 the card, 1 + i the stick vol[i]

struct Feed {
  uint32_t gen;                 // moves with the lists (projects, sticks, bundles)
  bool available;               // the card is up: the pages are shown
  int nproj;
  ProjectRow proj[kProjects];
  int nvol;
  Volume vol[kVols];
  char src[kDriveMax];          // the bundles' source: "" the card, or a stick's drive
  int nbund;
  BundleRow bund[kBundles];
  bool reading;                 // a list asked for and not read yet
  Status st;                    // the job (status())
  bool have_plan;
  PlanInfo plan;                // READY / DONE of an import
  uint64_t card_free;           // bytes, 0 unknown
  uint32_t eject_n;             // moves with each eject's answer
  char eject_msg[64];           // its answer: "USB: safe to pull now" or why not
  int ntrash;                   // 2026-10-27: the project trash (core/bundle/prjslot.h), the newest first
  prjslot::TrashRow trash[kTrash];
  char prj_last[48];            // the slot jobs' last outcome ("RESTORED PRESETS TO @005")
};

// core 0
void set_available(bool on);
void tick();                    // its loop: the ring drained, lists read when owed, the status published
bool command(const char* line, char* out, int cap);   // a console line ("bundle..."); true when it was one
// any core
bool copy(Feed* out);           // the last published (false: none yet)
bool post(const char* line);    // the Rytm's page: a command for core 0 (false: the ring full)
uint32_t published();           // a counter that moves with every publish (cheap "anything new?")
bool available();               // the published Feed's available (cheap: the Rytm page's shown())

void reset_for_tests();

}  // namespace feed
}  // namespace bundle
}  // namespace core
