#!/bin/sh
# Service request/quote integration with real sliced search, workflow and movement.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" ${CPPFLAGS:-} ${CXXFLAGS:-} -std=c++17 -Wall -Wextra -Werror \
    -Wno-error=reorder -Wno-missing-field-initializers -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbotserviceroute_contracts.cpp" "$root/src/playerbotserviceworkflow.cpp" \
    "$root/src/playerbotnpcsession.cpp" "$root/src/playerbotservicesession.cpp" "$root/src/playerboteconomy.cpp" \
    "$root/src/playerbotnavigationruntime.cpp" "$root/src/playerbotnavigationsession.cpp" "$root/src/playerbotnavigation.cpp" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build/service-route"
"$build/service-route"
