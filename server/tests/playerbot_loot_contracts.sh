#!/bin/sh
# Pure cargo policy and workflow checks; no server, database, Docker, or repository artifacts.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbot_loot_contracts.cpp" \
    "$root/src/playerbotlootpolicy.cpp" "$root/src/playerbotlootworkflow.cpp" \
    "$root/src/playerbotlootsession.cpp" -o "$build/playerbot_loot_contracts"
"$build/playerbot_loot_contracts"
