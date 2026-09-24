#!/bin/sh
# Pure C++ checks; no server process, database, Docker, or generated repository files.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbot_contracts.cpp" "$root/src/playerbotgoalplanner.cpp" \
    "$root/src/playerbothuntruntime.cpp" "$root/src/playerbothuntrouteselection.cpp" "$root/src/playerbothuntplanningsession.cpp" \
    "$root/src/playerbothuntpolicy.cpp" "$root/src/playerbotequipmentpolicy.cpp" \
    "$root/src/playerbotprogressionplanners.cpp" -Wl,--gc-sections -o "$build/playerbot_contracts"
"$build/playerbot_contracts"
