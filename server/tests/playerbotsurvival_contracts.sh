#!/bin/sh
# Real C++ runtime checks; no server process, database, Docker, or generated repository files.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM

# CPPFLAGS can supply headers from a non-system Boost installation.
# Word splitting is intentional for the conventional compiler flag variables.
for source in playerbotsurvivalruntime playerbotrecoverysession playerbotspellruntime playerbotspellcalibration; do
    # Existing food/task warnings are unrelated to this policy. Keep other warnings fatal.
    "${CXX:-c++}" ${CPPFLAGS:-} ${CXXFLAGS:-} -std=c++17 -Wall -Wextra -Werror \
        -Wno-error=unused-variable -Wno-error=reorder -ffunction-sections -fdata-sections -I"$root/src" \
        -c "$root/src/$source.cpp" -o "$build/$source.o"
done
"${CXX:-c++}" ${CPPFLAGS:-} ${CXXFLAGS:-} -std=c++17 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbotsurvival_contracts.cpp" "$build"/*.o ${LDFLAGS:-} -Wl,--gc-sections \
    -o "$build/playerbotsurvival_contracts"
"$build/playerbotsurvival_contracts"
