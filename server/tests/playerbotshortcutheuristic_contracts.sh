#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbotshortcutheuristic_contracts.cpp" -o "$build/playerbotshortcutheuristic_contracts"
"$build/playerbotshortcutheuristic_contracts"
