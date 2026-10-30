// core/bundle/prjslot.h - Sidecar projects keyed by the Rytm's project slot, and the project trash (2026-10-27,
// docs/superpowers/specs/2026-10-27-project-slot-keys-design.md). Pi and Mac only (CORE_MSAMP).
//
// The Rytm keeps 128 fixed project slots and no ID inside a project: the slot is the identity. With rytm-mods rui1052
// (0026-remote-ui, the hello's caps bit 2) the Rytm reports the active slot with its name and the project manager's
// events (F0 7D 52 54 4B 01 <op> ... F7: 00 ACTIVE, 01 SAVE, 02 PASTE, 03 CLEAR, 04 DELETE, 05 RENAME, 06 LOST, 07 WRITE;
// slots 1-based in two 7-bit bytes, 0 none). In slot mode a project's files are keyed "@NNN" (project_key.h) in the
// usual per-project folders (bundle.cpp's kProjectFiles), and:
//   SAVE AS / SAVE TO / PASTE onto B   B's files to the trash, then A's copied to B
//   CLEAR / INIT / DELETE of B          B's files to the trash; for the active slot: when it is left or reloaded (a SAVE
//                                       of it cancels that - the Rytm's RAM still holds the project)
//   RENAME                              nothing moves (the index's name follows)
// A switch to a slot whose index name differs from the Rytm's, with files: the PROJECT page asks (keep / trash); a slot
// with no files whose name has files from before slots (a name key): asks to take them - except the active project at
// the first slot-mode session, which takes them silently (slot + name). Duplicate names are never merged.
// The trash: /.rytm/trash/NNNNNN/ (info.txt and the moved files), 64 entries at most (the oldest removed for good past
// that, logged), restored / emptied from the web page (bundleweb: /bundle/trash*).
//
// Cores: everything here runs on the control core (where host_sysex and idle_save run); every card operation is a card
// queue request (cardq.h), one at a time, a step each tick - the control core never touches the card nor waits. The web
// page (core 0) only posts requests (a ring the control core drains) and reads the trash folder itself (trash_json).
// Without rui1052 (no 4B, no caps bit): name mode - today's behaviour (pool.cpp set_project, the rename prompt).
//
// Review round 1 (2026-10-27): every multi-step card operation is journaled first (prjslot.txt job=: "trash K N",
// "move A B", "copy A B" / "place A B", "restore N K"; posted and landed before the first rename) and completed forward
// at the next boot after a cut. A copy goes to a staging key (B~) and is renamed into place only whole: a half copy never
// loads as the project. The FLUSH never waits on the card here (no post_wait): a job waits (kWait) until the saves have
// landed. A slot's "renamed" mark is set only once its trash / copy succeeded (a failed one leaves the name question).
// 4B 08 / 09 the Rytm's slot list (the migration of every unambiguous project from before slots; deleted while away ->
// the trash), 4B 0A LOAD (the active slot loaded again resolves its pending clear / paste at once).
#pragma once
#include <stdint.h>

#include "../project_key.h"

namespace core {
namespace prjslot {

constexpr int kSlots = 128;
constexpr int kTrashMax = 64;          // entries kept; a 65th removes the oldest (the cap rule, the spec section 6)
constexpr uint32_t kAskMs = 120000;    // a question unanswered for 2 minutes: the answer that moves nothing
constexpr uint32_t kHoldMs = 3000;     // a card keyed by slot: the 10s held this long for a hello / a 4B
constexpr uint32_t kFlushMs = 5000;    // FLUSH at most (the rename's)

enum Mode : uint8_t { kUndecided = 0, kName, kSlot };
enum Ask : uint8_t { kNoAsk = 0, kAskLegacy, kAskMismatch };

void slot_key(int slot, char* out, int cap);   // 1..128 -> "@001".."@128", 0 -> "@000"
int slot_of(const char* key);                  // "@005" -> 5, "@000" -> 0; -1 not a slot key
struct TrashRow {                              // one project trash entry (trash_rows; the PROJECT pages' feed)
  uint32_t id;
  char key[kKeyMax], name[24], why[12], date[24];
  int items;
};

#if CORE_MSAMP
void load();                                   // boot (pool.cpp project_load): /.rytm/prjslot.txt
void sysex(const uint8_t* d, int len);         // a 4B from the Rytm (host_link.cpp)
void hello(int caps);                          // the Rytm's hello (35) caps byte (host_link.cpp)
// pool.cpp set_project's: the Rytm's 10 named n. true: not a switch (slot mode, or held for a hello on a card keyed by
// slot); false: the switch as today
bool take_10(const char* n);
// pool.cpp set_project's, name mode on a card keyed by slot: the slot key whose index name is n (the most recently used)
// when the name key has no index entry; false: none (the name key as today)
bool resolve(const char* n, char* key, int cap);
void tick(uint32_t ms);                        // idle_save's: the jobs (a card-queue step a pass), a held 10, the ask
bool moving();                                 // a job moves or copies files: idle_save holds every other save
bool busy();                                   // a job queued or running, a switch waiting, a question
int mode();                                    // Mode (the session's)
bool migrated();                               // the card is keyed by slot (prjslot.txt mode=slot)

// The question (the PROJECT page; tests)
int ask();                                     // Ask
const char* ask_line(int i);                   // its lines 0..4 (upper case, fit the page)
const char* ask_footer();
uint32_t asks();                               // moves with every new question (the page opens on it)
void answer(bool yes);
const char* last();                            // the last outcome, one line ("TRASHED 005", "COPIED 001 TO 004")

// The web page (core 0): requests the control core runs at its next ticks; false: the ring is full
bool request_trash(const char* key, const char* why, const char* name);   // the web page's Delete (bundle.cpp)
bool request_restore(uint32_t id, const char* to, bool replace);
bool request_remove(uint32_t id);             // one entry, for good (the web page, confirmed)
bool request_empty();                         // every entry, for good (the web page, confirmed)
int trash_rows(TrashRow* out, int max);       // the trash read from the card (the caller's core: core 0), the newest first
int trash_json(char* out, int cap);           // the same as JSON, [] none
int status_json(char* out, int cap);          // {"mode","migrated","active","busy","last","web","ask"}
uint32_t trash_next();                        // the next entry's number (tests)
uint32_t failures();                          // jobs that could not follow the Rytm (said and logged; /bundle/prjslot)
uint32_t jobs_done();                         // any core: moves when a job ends (the PROJECT pages read the trash again)
const char* journal_line();                   // the card operation under way ("" none; prjslot.txt job=)
// The bundle import into a slot (bundle/import.cpp, core 0; 2026-10-29):
// the Rytm's name for slot (its last slot list, 4B 08 / 09, or its ACTIVE report; a stale copy at worst). 1: from the
// Rytm (*occupied set; name "" an empty slot), 0: not known (no list since the connect)
int rytm_slot(int slot, char* name, int cap, bool* occupied);
// the items a trash job moves for key, one at a time (i from 0 until false): its card path and its path in trash entry
// id - the import's replace moves the old slot's files with these (its commit journal), so a restore takes them back
bool trash_item(int i, const char* key, uint32_t id, char* from, char* to, int cap);
int trash_info(const char* key, const char* name, const char* why, char* out, int cap);   // an entry's info.txt
// the import's COMMIT (core 0) holds the jobs: none starts while it is held (its trash entry's number and the slot's
// files are the import's until its renames are done); the one running finishes (the import waits for busy() false)
void hold_jobs(bool on);
void reset();                                 // tests: the module's state, not the card's
#endif

}  // namespace prjslot
}  // namespace core
