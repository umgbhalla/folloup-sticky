#!/bin/sh
set -eu
: "${IDF_PATH:?Source ESP-IDF export.sh first}"
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
cc -c "$IDF_PATH/components/json/cJSON/cJSON.c" -o "$test_dir/cJSON.o"
c++ -std=c++17 -Wall -Wextra -Werror \
  -I"$IDF_PATH/components/json/cJSON" -I"$repo/components/gemini_service" \
  "$repo/tests/openrouter_protocol_test.cpp" "$test_dir/cJSON.o" -o "$test_dir/test"
"$test_dir/test"
printf 'OpenRouter protocol tests passed.\n'
