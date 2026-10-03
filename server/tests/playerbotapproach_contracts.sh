#!/bin/sh
# Pure C++ checks for the shared approach primitive.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/playerbotapproach_contracts.cpp" -o "$build/playerbotapproach_contracts"
"$build/playerbotapproach_contracts"
