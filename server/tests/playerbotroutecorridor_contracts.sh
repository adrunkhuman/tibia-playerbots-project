#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbotroutecorridor_contracts.cpp" -o "$build/playerbotroutecorridor_contracts"
"$build/playerbotroutecorridor_contracts"
