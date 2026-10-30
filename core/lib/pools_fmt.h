// core/lib/pools_fmt.h - the pool lists' text formats (core/lib/pools.h; the design's 3.3), pure (no card, no globals
// written): the module's load and save, the format registry's readers (core/fmt/formats.cpp) and the fuzz target.
//
//   #sidecar samplepool 1            (wtpool, instpool: the same lines)
//   A010 sample <sha256|-> <size> <path hint>[TAB<name>[TAB<frames> <channels> <rate>]]
//   H001 input IN
//   H011 builtin PAD
//   B003 instrument <ref>[TAB<name>]
//    m <ref>                         (an instrument's member, in the .sfz's order; the line starts with a space)
//
//   #sidecar poolused 1
//   smp A010 3                       (<list tag> <row> <times seen>)
//
// Sparse: only filled rows, in row order. A row is A..H then 001..127 (row 000 is OFF, never written). The ref is
// lib::format_ref's text form. A name is at most 39 characters. Rows a list does not take, unknown types, a member with
// no instrument before it, names it does not know: the file is refused (fmt kUnreadable: locked).
#pragma once
#include <stdint.h>

#include "library.h"

namespace core {
namespace pools {
namespace text {

constexpr int kName = 40;
struct Line {
  int code;           // bank * 128 + row (members: their instrument's)
  uint8_t type;       // pools::Type
  uint8_t id;         // an input's / built-in's index
  bool member;
  lib::Ref ref;
  char name[kName];
  uint32_t frames, rate;
  uint8_t ch;
};
// each line, in order; fn false stops (the parse fails with "too many")
typedef bool (*LineFn)(void* ctx, const Line& l);
// list: pools::List; text from its body (after the header line); why gets the reason on false
bool parse(int list, const char* text, uint32_t n, LineFn fn, void* ctx, char* why, int cap);
// The same in pieces (whole lines each: a control core's step at a time): the state between them
struct Parser {
  int last;    // the last row (-1 none)
  bool inst;   // an instrument line came before (members may follow)
};
inline void parser_init(Parser* p) { p->last = -1, p->inst = false; }
bool parse_more(int list, Parser* p, const char* text, uint32_t n, LineFn fn, void* ctx, char* why, int cap);
// one line (its '\n' included) into out: its length, 0 when it does not fit
int put(int list, const Line& l, char* out, int cap);

struct Used {
  int list, code;
  uint32_t count;
  uint8_t ram;   // a "ram" line (docs/STEREO.md "always in RAM"): 1 a row's (code), 2 the whole list's (code -1)
};
typedef bool (*UsedFn)(void* ctx, const Used& u);
bool parse_used(const char* text, uint32_t n, UsedFn fn, void* ctx, char* why, int cap);
int put_used(const Used& u, char* out, int cap);

}  // namespace text
}  // namespace pools
}  // namespace core
