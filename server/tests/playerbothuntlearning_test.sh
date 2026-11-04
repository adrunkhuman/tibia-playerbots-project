#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbothuntlearning_test.cpp" "$root/src/playerbothuntpolicy.cpp" \
    -Wl,--gc-sections -o "$build/playerbothuntlearning_test"
"$build/playerbothuntlearning_test"
