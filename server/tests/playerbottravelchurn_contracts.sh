#!/bin/sh
# Real sliced frontier, retained exposure and return coverage; no engine/runtime.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=$(mktemp)
trap 'rm -f "$binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbottravelchurn_contracts.cpp" -o "$binary"
"$binary"
