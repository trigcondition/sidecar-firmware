// core/project_key.h - the card's name for a Rytm project (docs/BUNDLES.md "Project keys"; the bundles design,
// section 6.2). Every per-project file is keyed by it: /.rytm/pools/K.txt, /.rytm/{granular,wavetable,multisample}/
// K.pool.txt, /.rytm/granular/K.txt, /.rytm/slots/K.{txt,inf}, /.rytm/fxslots/K.txt, /.rytm/juno/K.txt,
// /.rytm/fm/K.txt, /.rytm/lfo/K[.txt|/], /.rytm/ins/K[.txt|/], /.rytm/seq/K/, /.rytm/songs/K/.
//
// The rule (the one six of the seven earlier copies had): A-Z a-z 0-9 - _ space . kept, anything else '_'; at most 23
// characters; trailing spaces and dots trimmed (FAT drops them); "_default" for an empty result. The sampler's own
// folder (/Samples/<name>/, samp::safe_name) is a different, older rule and stays as it is.
#pragma once

namespace core {

constexpr int kKeyMax = 24;   // 23 characters and the NUL

void project_key(const char* rytm_name, char* out, int cap);
// 2026-10-27 (core/bundle/prjslot.h, docs/superpowers/specs/2026-10-27-project-slot-keys-design.md): a project of the
// Rytm's slot N is keyed "@NNN" ("@000": no slot, after CREATE NEW). project_key passes a slot key through unchanged; a
// Rytm name never makes one ('@' is outside the kept set).
bool is_slot_key(const char* key);
// The song folder's rule before phase 0 (spaces and dots to '_' too, no trim, "_none"): only for its migration
// (song.cpp renames /.rytm/songs/<old>/ to /.rytm/songs/<key>/ once).
void project_key_song_v0(const char* rytm_name, char* out, int cap);

// The sampler's own folder for a project, /Samples/<this>/ (samp::safe_name's rule: A-Z a-z 0-9 - _ kept, anything
// else '_', "Sidecar" for no project): the bundle collects the project's sampler takes from there.
void project_key_sampler(const char* rytm_name, char* out, int cap);

}  // namespace core
