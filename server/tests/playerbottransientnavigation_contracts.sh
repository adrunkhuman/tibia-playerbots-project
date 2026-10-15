#!/bin/sh
# Real search kernel, depot workflow and navigation failure bounds; no live world.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" ${CPPFLAGS:-} ${CXXFLAGS:-} -std=c++17 -Wall -Wextra -Werror \
    -Wno-error=reorder -Wno-missing-field-initializers -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbottransientnavigation_contracts.cpp" "$root/src/playerbotdepotworkflow.cpp" \
    "$root/src/playerbotdepotsession.cpp" \
    "$root/src/playerbotnavigationruntime.cpp" "$root/src/playerbotnavigationsession.cpp" "$root/src/playerbotnavigation.cpp" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build/transient-navigation"
"$build/transient-navigation"
