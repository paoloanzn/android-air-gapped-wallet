#!/bin/sh
set -eu

ROOT="$(CDPATH= cd -P -- "$(dirname -- "$0")/.." && pwd -P)"
BUILD="$ROOT/build"

# Clang's -MJ fragments already contain JSON, with a trailing comma.
# Keep the separator on the first entry and remove it from the last.
test -r "$BUILD/obj/android_native_app_glue.json"
test -r "$BUILD/obj/main.json"
{
    printf '[\n'
    cat "$BUILD/obj/android_native_app_glue.json"
    sed '$s/,[[:space:]]*$//' "$BUILD/obj/main.json"
    printf ']\n'
} > "$BUILD/compile_commands.json"

printf 'Compilation database=%s\n' "$BUILD/compile_commands.json"
