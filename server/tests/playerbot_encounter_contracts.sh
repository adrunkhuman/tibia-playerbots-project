#!/bin/sh
# Pure bounded encounter-cost and recovery timing checks; no running server.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbot_encounter_contracts.cpp" -o "$build/playerbot_encounter_contracts"
"$build/playerbot_encounter_contracts"
