#!/bin/sh
# Pure dispatcher lifecycle state/token contracts; no live server or database.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
    "$root/tests/playerbotlifecycle_contracts.cpp" -o "$build/lifecycle"
"$build/lifecycle"
