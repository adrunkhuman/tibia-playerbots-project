#!/bin/sh
# Paid trainer-route policy and real progression runtime; no server or database.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CXX:-c++}" ${CPPFLAGS:-} ${CXXFLAGS:-} -std=c++17 -Wall -Wextra -Werror \
    -Wno-error=reorder -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbottrainer_contracts.cpp" "$root/src/playerbotprogressionplanners.cpp" \
    "$root/src/playerbotprogressionruntime.cpp" "$root/src/playerbotprogressionsession.cpp" \
    "$root/src/playerbotnpcsession.cpp" "$root/src/playerbotservicesession.cpp" \
    "$root/src/playerbotnavigationruntime.cpp" "$root/src/playerbotnavigationsession.cpp" "$root/src/playerbotnavigation.cpp" \
    ${LDFLAGS:-} -Wl,--gc-sections -o "$build/trainer"
"$build/trainer"
