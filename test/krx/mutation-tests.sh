#!/usr/bin/env bash
# Mull's test program (plan item 1.7, docs/MAINTENANCE.md §5): a mutant is killed when either
# suite fails. KRX_BUILD is the CMake build folder (default build/mull).
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
build=${KRX_BUILD:-$root/build/mull}

# Some mutants make a suite loop for ever; Mull's own timeout would stop only this script and leave
# the suite running, so each suite gets its own limit (a timeout counts as killed).
timeout 60 "$build/src/C++/test/krx/krx_ut" >/dev/null

# ut writes its stores and logs under ./store and ./log. A mutant can leave those files corrupt (a
# bad .session file crashes the next clean run in FileStoreTests_6), and Mull runs four of these
# scripts at once, so each run gets its own empty folder next to links to what ut reads.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/test"
ln -s "$root/spec" "$work/spec"
ln -s "$root/test/cfg" "$work/test/cfg"
ln -s "$root/test/resources" "$work/test/resources"
cd "$work/test"
timeout 120 "$build/src/C++/test/ut" --quickfix-config-file cfg/ut.cfg --quickfix-spec-path ../spec >/dev/null
