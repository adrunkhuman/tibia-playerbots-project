#!/bin/sh
# Pure economy checks; no server headers, database, or Docker required.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbothealing_contracts.cpp" -o "$build/playerbothealing_contracts"
"$build/playerbothealing_contracts"
