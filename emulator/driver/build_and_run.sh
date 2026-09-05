#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/folloup-driver-trace
c++ -std=c++17 -O2 -Wall -Wextra -I"$root/emulator/driver/include" -I"$root/components/epaper_panel/include" "$root/components/epaper_panel/epaper_panel.cpp" "$root/components/epaper_panel/ssd1677_driver.cpp" "$root/emulator/driver/driver_trace.cpp" -o "$out"
exec "$out" "$@"
