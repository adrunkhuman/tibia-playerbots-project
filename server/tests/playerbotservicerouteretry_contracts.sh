#!/bin/sh
# Service retry decisions with the real provider workflow and exclusion TTLs.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" ${CPPFLAGS:-} ${CXXFLAGS:-} -std=c++17 -Wall -Wextra -Werror \
    -Wno-error=reorder -Wno-missing-field-initializers -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbotservicerouteretry_contracts.cpp" "$root/src/playerbotserviceworkflow.cpp" \
    "$root/src/playerbotnpcsession.cpp" "$root/src/playerbotservicesession.cpp" "$root/src/playerboteconomy.cpp" \
    "$root/src/playerbotnavigationsession.cpp" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build/service-route-retry"
"$build/service-route-retry"
