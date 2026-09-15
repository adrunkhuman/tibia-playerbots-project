#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM

for source in playerbotprogressionruntime playerbotprogressionsession playerbotservicesession playerbotnpcsession; do
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I"$root/src" \
        -c "$root/src/$source.cpp" -o "$build/$source.o"
done
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -I"$root/src" \
    "$root/tests/playerbot_equipment_purchase_contracts.cpp" "$build"/*.o -Wl,--gc-sections \
    -o "$build/playerbot_equipment_purchase_contracts"
"$build/playerbot_equipment_purchase_contracts"
