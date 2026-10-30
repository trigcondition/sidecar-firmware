// core/store/store.h - the RAM store (docs/superpowers/specs/2026-10-26-ram-store-design.md): one project's card
// files held in RAM as bytes keyed by path, so the control core (core 3) reads them without touching the SD card (every
// card call there is an audible dropout). The card core fills it (put / mark); the control core reads it (get).
//
// A path is in one of four states: kUnknown (never filled: callers treat it as locked, never as absent), kAbsent (the
// card has no such file), kPresent (the bytes are here), kLocked (the file exists but could not be held or read: never
// written over). Linear lookup in a flat array: it runs per kit change / pattern switch, not per sample.
//
// Not thread-safe by itself: the filler and the readers are ordered by the caller (the fill finishes before the reader
// runs). Portable (Pi, Mac core, tests); heap via new[] / delete[].
#pragma once
#include <stdint.h>

namespace core {
namespace store {

enum State : uint8_t { kUnknown = 0, kAbsent, kPresent, kLocked };

constexpr int kMaxEntries = 1024;  // a project's paths: 2 x 129 LFO / INSERT files, 257 of the sequencer's; a put beyond is refused
constexpr int kPathMax = 96;       // longest path held (with its NUL)

void clear();   // every entry dropped, every buffer freed (and, with the fill, every wanted path; a fill abandoned)
// the path's bytes (a copy; replaces an earlier one), kPresent. False only when full (a full store holds no new entry,
// so a path not yet held stays kUnknown) or new returned 0 (the entry then becomes kLocked) - which Circle's new never
// does when the heap is out: it halts with an "Out of memory" panic first (store.cpp put()).
bool put(const char* path, const uint8_t* d, uint32_t n);
void mark(const char* path, State s);   // kAbsent or kLocked; frees the bytes held
// kPresent sets *d / *n; kAbsent and kLocked set *d = nullptr, *n = 0; an unknown path returns kUnknown
State get(const char* path, const uint8_t** d, uint32_t* n);
uint32_t bytes();   // total bytes held (for /status and the memory test)
int entries();      // entries held, in any state
int present();      // entries held as kPresent (the files there)

#if CORE_MSAMP
// The fill (the Pi and the Mac core; the Teensy has no card queue): the control core registers the paths it wants and
// drives the fill from its idle pass; every open / read / exists is a cardq::async request the card core runs, so the
// control core never waits on the card. Folder by folder: each wanted path's folder listed once (cardq kListReq);
// a path not in it -> kAbsent with no card operation of its own (a folder not there: all of its paths); a listed one
// opened -> the whole file read, closed, held (kPresent); listed but not opened -> kLocked (logged: never written
// over). A folder that cannot be listed: its paths asked one by one (open; not opened -> kExistsReq: there ->
// kLocked, not there -> kAbsent). One request in flight at a time (a project is a few hundred small files).
void want(const char* path);   // a path the coming fill reads (repeats ignored); clear() forgets every wanted path
// every entry dropped, then every wanted path read; an earlier fill still running is abandoned (its buffers freed, its
// open file closed by the next steps: a request already on the card core is let finish first)
void fill_begin();
bool fill_step();   // one bounded step (at most one cardq::async call); true when the fill is complete
bool ready();       // the last fill_begin()'s fill is complete (false after clear(), before any fill_begin())
#endif

}  // namespace store
}  // namespace core
