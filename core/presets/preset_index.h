// core/presets/preset_index.h - what /JUNO and /DX7 offer, held in RAM (0.4.0): the JUNO / 6-OP FM preset browser's
// folders and names, so that scrolling it never reads the card. Walked in bounded pieces on the card core after the boot
// and again after a change under those folders; a JUNO patch's 18 bytes are in the index, a DX7 voice's 128 are read from
// its bank on the card core when added. FACTORY (the Juno-106's A11..B88) and ROM (Plaits' 96 voices) are folders of it,
// so a built-in can go into a list too. Only the control core reads it, only while ready(); no list is touched by the walk.
#ifndef CORE_PRESETS_PRESET_INDEX_H
#define CORE_PRESETS_PRESET_INDEX_H
#include <stdint.h>

namespace core {
namespace presets {

enum { kIxJuno = 0, kIxFm = 1 };   // the two roots: folders 0 and 1
// A share for each kind, so neither can starve the other; past it (the browser says FULL) the rest of that kind is left out.
// /DX7 at dx7_lib's 128 banks: 4096 voices and the ROM's 96
constexpr int kItemsOf[2] = {3072, 5120}, kFoldersOf[2] = {384, 384};
constexpr uint32_t kTextOf[2] = {176 * 1024, 80 * 1024};   // names, folder paths, JUNO patches' bytes, banks' MD5s
constexpr int kMaxFolders = kFoldersOf[0] + kFoldersOf[1], kMaxItems = kItemsOf[0] + kItemsOf[1];
constexpr uint32_t kTextBytes = kTextOf[0] + kTextOf[1];
uint32_t ram_bytes();                                // the index's whole size
struct Stats { int folders, items; uint32_t text; };
Stats stats(int kind);
bool full(int kind);

void begin();                    // the boot: walked as soon as the idle pass gets to it
void changed(const char* path);  // the card changed there: under /JUNO or /DX7 walked again, a moment after the last change
void step();                     // each idle pass: the walk's next piece, a voice read finishing
bool ready();                    // the walk finished: count() .. are valid
uint32_t gen();                  // a new index (a browser's redraw)

int count(int folder);
const char* name(int folder, int row);
bool is_dir(int folder, int row);
int child(int folder, int row);
int parent(int folder);          // -1: a root
int row_in_parent(int folder);
const char* folder_name(int folder);

// The project's list as the browser shows it: its rows (none listed: the built-ins PAT plays, 128 / 96), row's name
int rows(int kind);
const char* row_name(int kind, int row, char* b, int cap);
bool built_ins(int kind);        // nothing listed
bool row_used(int kind, int row);   // 0.5.0 FILL: the row holds a preset (the built-ins: every row; else a used slot)

// An empty list's first add seeds it with the built-ins, row = PAT, so a saved PAT keeps its preset (docs/POOLS.md): FM's 96 rows
// and room after them; JUNO's 127 rows (PAT is 7 bits and every value was a factory patch: there is no free row, and PAT 127,
// past the list's last, keeps B88: juno_pick).
void seed(int kind);
int spare_juno();                // LOAD TO PROJ on a full list: the highest row still its factory patch, -1
// A row of the index into the project's list (its first empty slot, the list's own file saved, the Rytm told):
enum { kAddRead = -4, kAddBusy = -3, kAddFull = -2, kAddNo = -1 };   // kAddRead: the voice is on its way, add_poll()
// kind: the browser's (a folder of the other kind: kAddNo). at >= 0: when the list is full, into that row
// force (the touchscreen's and the web page's REPLACE): at >= 0 is the row it goes into, whatever it holds or the list's room
int add(int kind, int folder, int row, bool* was_there, int at = -1, bool force = false);   // the slot, or kAdd*
bool add_poll(int* slot, bool* was_there);       // true once: a kAddRead's end (the Rytm's page; result() is for everyone)

// The last add's end, from whichever page asked (a kAddRead's too, once the voice is in): seq changes with each; text is
// "JUNO 005 ADDED", "FM 012 IN THE LIST ALREADY", "LIST FULL", "PROJECT LOADING", "NOT ADDED"
struct Result { uint32_t seq; int kind, slot; bool there, forced; };
Result result();
const char* result_text(char* b, int cap);

// For a page on another core than the control core's (the touchscreen's provider, the web tasks): display only, copied out
// and checked against the walk (a walk that starts or ends meanwhile: -1, the caller draws it again). Names are cut to
// kViewName - 1. The folder's rows, or (folder < 0) the project's list as the Rytm's PAT picks it: used 0 for an empty slot
// (kind: the list's), total the rows; kids[i]: the folder a flag-kViewDir row opens (-1 none); built_in: nothing listed, the rows are the built-ins'.
constexpr int kViewName = 40;
struct View { uint32_t gen; int total, up, kind; bool built_in, full; char title[48]; };
int view(int folder, int kind, int first, int max, char (*names)[kViewName], uint8_t* flags, int16_t* kids, View* v);   // rows filled, -1
enum { kViewDir = 1, kViewUsed = 2 };

}  // namespace presets
}  // namespace core
#endif
