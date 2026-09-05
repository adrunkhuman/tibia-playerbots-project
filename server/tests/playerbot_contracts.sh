#!/bin/sh
# Pure C++ checks; no server process, database, Docker, or generated repository files.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbot_contracts.cpp" -o "$build/playerbot_contracts"
"$build/playerbot_contracts"
