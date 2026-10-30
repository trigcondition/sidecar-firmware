// core/tests/check.h - the core's Mac tests: CHECK stops the test with its line on failure.
#pragma once
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

// The text-fit check (UX review R1): with the core's UI tools (the Mac build), every text item a page puts on the
// Rytm's screen must fit (ui::Canvas::fit_report: cut at the font's widest advance, or past the bottom row). Each
// one is printed and the test fails at exit. A test that cuts on purpose (canvas tests) sets text_fit_off = true.
#if CORE_UI_TOOLS && __has_include("ui/ui.h")
#include <cstring>
#include <unistd.h>
#include "ui/ui.h"
inline bool text_fit_off = false;
inline int text_fit_fails = 0;
inline void text_fit_report(const char* s, int need, int room) {
  if (text_fit_off) return;
  static char seen[64][40];
  static int nseen = 0;
  for (int i = 0; i < nseen; ++i) if (!strncmp(seen[i], s, 39)) return;
  if (nseen < 64) snprintf(seen[nseen++], 40, "%s", s);
  fprintf(stderr, "TEXT-FIT: \"%s\" needs %d, has %d\n", s, need, room);
  ++text_fit_fails;
}
inline void text_fit_atexit() {
  if (text_fit_fails) { fprintf(stderr, "TEXT-FIT: %d strings do not fit the Rytm's screen\n", text_fit_fails); fflush(stderr); _exit(1); }
}
inline const bool text_fit_installed = [] { core::ui::Canvas::fit_report = text_fit_report; atexit(text_fit_atexit); return true; }();
#endif
