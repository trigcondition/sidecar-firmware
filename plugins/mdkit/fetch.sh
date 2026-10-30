#!/bin/bash
# plugins/mdkit/fetch.sh - mdkit (https://github.com/soejrd/mdkit, CC0 1.0) at commit 96157fe is vendored in
# third_party/mdkit (third_party/mdkit/VENDORED.md); the build reads it from there. Kept for old instructions: no network,
# it only checks the vendored copy is in place.
set -e; cd "$(dirname "$0")"
[ -f ../../third_party/mdkit/include/mdkit.h ] || { echo "fetch.sh: third_party/mdkit missing"; exit 1; }
echo "mdkit at 96157fe (third_party/mdkit, vendored)"
