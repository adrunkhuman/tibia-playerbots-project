#!/bin/sh
# Pure registry checks plus source contracts for engine activation/reset hooks.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=$(mktemp)
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbotraidhazards_contracts.cpp" -o "$binary"
"$binary" "$root/src/raids.cpp"
