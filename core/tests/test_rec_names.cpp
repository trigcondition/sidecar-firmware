// core/tests/test_rec_names.cpp - the take folders' names (core/rec/rec_names.h, docs/BACKLOG.md 5d): the project's
// slug, "<slug>_<YYYY-MM-DD_HHMM>[_N]" with a wall clock, "<slug>_take-NNN" without, the numbers of names already
// on the card (a name that is there gets the next _N / NNN), the date of a local time.
#include <string.h>

#include "check.h"
#include "rec/rec_names.h"

static const char* slug(const char* in) {
  static char b[64];
  rec::slug(in, b, sizeof b);
  return b;
}

int main() {
  // the slug: lowercase, every run of anything else one '-', none at the ends, at most 24
  CHECK(!strcmp(slug("My Project"), "my-project"));
  CHECK(!strcmp(slug("  JAM  NIGHT!! 3 "), "jam-night-3"));
  CHECK(!strcmp(slug("A/B\\C:D"), "a-b-c-d"));
  CHECK(!strcmp(slug("UNTITLED"), "untitled"));
  CHECK(!strcmp(slug("--x--"), "x"));
  CHECK(!strcmp(slug("abcdefghijklmnopqrstuvwxyz0123"), "abcdefghijklmnopqrstuvwx"));   // 24
  CHECK(!strcmp(slug("abcdefghijklmnopqrstuvw yz"), "abcdefghijklmnopqrstuvw"));       // no '-' left at the cut
  CHECK(!strcmp(slug(""), "") && !strcmp(slug("!!!"), "") && !strcmp(slug(nullptr), ""));
  CHECK(!strcmp(slug("\xC3\xA9t\xC3\xA9 2026"), "t-2026"));   // bytes past ASCII are not letters
  {
    char b[8];
    rec::slug("project", b, sizeof b);   // the buffer's size is a limit too
    CHECK(!strcmp(b, "project"));
    rec::slug("projects", b, sizeof b);
    CHECK(!strcmp(b, "project"));
  }
  CHECK(rec::valid_prefix(slug("Any Name At All, 2026")));   // a slug is always a valid prefix

  // a local time's name: 2026-10-27 14:32:59 (1793111579 is 2026-10-27T14:32:59Z)
  char n[rec::kTakeMax];
  rec::stamp_name(n, sizeof n, "my-project", 1793111579u);
  CHECK(!strcmp(n, "my-project_2026-10-27_1432"));
  rec::stamp_name(n, sizeof n, "x", 951782400u);   // 2000-02-29 00:00 (a leap day)
  CHECK(!strcmp(n, "x_2000-02-29_0000"));
  rec::stamp_name(n, sizeof n, "x", 4102444799u);   // 2099-12-31 23:59:59
  CHECK(!strcmp(n, "x_2099-12-31_2359"));
  {
    char d[20];
    rec::date_time(1793111579u, d, sizeof d);
    CHECK(!strcmp(d, "2026-10-27 14:32"));
  }
  // the longest: a 24-character slug, the date, "_99", a drive's "NVME:" - it fits a take id
  rec::stamp_name(n, sizeof n, "abcdefghijklmnopqrstuvwx", 1793111579u);
  CHECK(strlen(n) == 24 + 1 + 15 && strlen(n) + 3 + 5 < rec::kTakeMax);

  // the k-th try of a name: the name itself, then _2, _3 ...; the try of a name on the card
  rec::variant(n, sizeof n, "p_2026-10-27_1432", 1);
  CHECK(!strcmp(n, "p_2026-10-27_1432"));
  rec::variant(n, sizeof n, "p_2026-10-27_1432", 2);
  CHECK(!strcmp(n, "p_2026-10-27_1432_2"));
  rec::variant(n, sizeof n, "p_2026-10-27_1432", 12);
  CHECK(!strcmp(n, "p_2026-10-27_1432_12"));
  CHECK(rec::variant_number("p_2026-10-27_1432", "p_2026-10-27_1432") == 1);
  CHECK(rec::variant_number("p_2026-10-27_1432_2", "p_2026-10-27_1432") == 2);
  CHECK(rec::variant_number("p_2026-10-27_1432_17", "p_2026-10-27_1432") == 17);
  CHECK(rec::variant_number("p_2026-10-27_1432_", "p_2026-10-27_1432") == 0);
  CHECK(rec::variant_number("p_2026-10-27_1432_x", "p_2026-10-27_1432") == 0);
  CHECK(rec::variant_number("p_2026-10-27_14321", "p_2026-10-27_1432") == 0);
  CHECK(rec::variant_number("p_2026-10-27_1433", "p_2026-10-27_1432") == 0);
  CHECK(rec::variant_number("P_2026-10-27_1432", "p_2026-10-27_1432") == 1);   // FAT: any case is the same name

  // without a clock: <slug>_take-NNN, one more than the highest of that slug
  rec::seq_name(n, sizeof n, "my-project", 7);
  CHECK(!strcmp(n, "my-project_take-007"));
  rec::seq_name(n, sizeof n, "my-project", 1234);
  CHECK(!strcmp(n, "my-project_take-1234"));
  CHECK(rec::seq_number("my-project_take-007", "my-project") == 7);
  CHECK(rec::seq_number("my-project_take-1234", "my-project") == 1234);
  CHECK(rec::seq_number("My-Project_TAKE-003", "my-project") == 3);
  CHECK(rec::seq_number("my-project_take-", "my-project") == -1);
  CHECK(rec::seq_number("my-project_take-00x", "my-project") == -1);
  CHECK(rec::seq_number("my-projects_take-001", "my-project") == -1);
  CHECK(rec::seq_number("my-project_2026-10-27_1432", "my-project") == -1);
  CHECK(rec::seq_number("take_004", "take") == -1);   // the old names are left as they are

  // the next name, from what the card holds
  {
    static const char* const card[] = {"take_004", "my-project_take-002", "my-project_take-009", "other_take-050",
                                       "my-project_2026-10-27_1432", "my-project_2026-10-27_1432_3", ".recording"};
    const int nc = (int)(sizeof card / sizeof card[0]);
    char names[8][64];
    for (int i = 0; i < nc; ++i) strcpy(names[i], card[i]);
    CHECK(rec::next_number(names, nc, "my-project", false, nullptr) == 10);
    CHECK(rec::next_number(names, nc, "brand-new", false, nullptr) == 1);
    CHECK(rec::next_number(names, nc, "my-project", true, "my-project_2026-10-27_1432") == 4);
    CHECK(rec::next_number(names, nc, "my-project", true, "my-project_2026-10-27_1433") == 1);
    CHECK(rec::next_number(names, 0, "my-project", false, nullptr) == 1);
  }
  printf("test_rec_names: slug, stamped and numbered take names, collisions: OK\n");
  return 0;
}
