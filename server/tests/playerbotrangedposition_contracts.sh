#!/bin/sh
# Pure C++ checks for the ranged position policy.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbotrangedposition_contracts.cpp" "$root/src/playerbotnavigationsession.cpp" \
    -Wl,--gc-sections -o "$build/playerbotrangedposition_contracts"
"$build/playerbotrangedposition_contracts"
