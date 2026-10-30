#!/bin/bash
# tools/tests/test_gen_version.sh - tools/gen_version.sh (the version stamp in core/version.h: /status "version",
# "commit", "dirty", the boot line, BUILD-INFO-pi.txt) on a throwaway git repository: the stamp has to say what the
# built tree is - its commit, and dirty whenever the tree holds anything the commit does not (a changed tracked file,
# an untracked source file the build globs pick up) or its state cannot be read. Run by make -C pi/engine/tests.
set -euo pipefail
SRC=$(cd "$(dirname "$0")/.." && pwd)/gen_version.sh
T=$(mktemp -d "${TMPDIR:-/tmp}/gen-version-XXXXXX")
trap 'rm -rf "$T"' EXIT
fail() { echo "test_gen_version: FAIL: $*"; exit 1; }
cd "$T"
git init -q .
git config user.email t@example.invalid && git config user.name t && git config commit.gpgsign false
mkdir -p tools core/seq pi/engine docs
cp "$SRC" tools/gen_version.sh
echo 0.1.0 > VERSION
echo 'int x;' > core/seq/seq.cpp
printf 'build/\ncore/version.h\n' > .gitignore
git add -A && git commit -qm one
run() { tools/gen_version.sh "$T/v.h"; }
commit() { sed -n 's/^#define SIDECAR_COMMIT\t"\(.*\)"$/\1/p' v.h; }
dirty() { sed -n 's/^#define SIDECAR_DIRTY\t\(.*\)$/\1/p' v.h; }
full() { sed -n 's/^#define SIDECAR_VERSION_FULL\t"\(.*\)"$/\1/p' v.h; }
expect() {   # what, dirty, [commit]
  run
  [ "$(dirty)" = "$2" ] || fail "$1: dirty $(dirty), expected $2"
  [ "$(commit)" = "${3:-$(git rev-parse --short=7 HEAD)}" ] || fail "$1: commit $(commit)"
}

expect "a clean commit" 0
[ "$(full)" = "0.1.0 ($(git rev-parse --short=7 HEAD))" ] || fail "full: $(full)"
echo 'int y;' >> core/seq/seq.cpp
expect "a tracked file changed" 1
[ "$(full)" = "0.1.0 ($(git rev-parse --short=7 HEAD)-dirty)" ] || fail "dirty full: $(full)"
git checkout -q core/seq/seq.cpp
expect "changed back" 0
# an untracked source file: core/Makefile and pi/engine/Makefile build $(wildcard core/seq/*.cpp), so it is in the
# kernel while the commit does not have it
echo 'int z;' > core/seq/extra.cpp
expect "an untracked source file the build picks up" 1
rm core/seq/extra.cpp
echo 'x' > pi/engine/new_part.h
expect "an untracked header in pi/engine" 1
rm pi/engine/new_part.h
# not build inputs: ignored outputs and notes stay clean
mkdir -p core/build && echo o > core/build/x.o
echo notes > docs/notes.md
echo notes > scratch.txt
expect "ignored and non-source untracked files" 0
# a new commit is a new stamp
echo 'int w;' >> core/seq/seq.cpp && git commit -qam two
expect "the next commit" 0
# the tree's state cannot be read (a corrupt index): not known to be clean, so dirty
cp .git/index "$T/index.ok" && echo garbage > .git/index
git rev-parse --short=7 HEAD >/dev/null
expect "git status fails" 1
cp "$T/index.ok" .git/index
expect "readable again" 0
# rewritten only when the content changes (a build that has not moved recompiles nothing)
mtime() { stat -f %m v.h 2>/dev/null || stat -c %Y v.h; }
touch -t 202001010000 v.h
M=$(mtime)
run
[ "$(mtime)" = "$M" ] || fail "rewritten without a change"
echo "test_gen_version: PASS"
