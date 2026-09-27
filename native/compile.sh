#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -P -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"

PREBUILT="$(find "$NDK/toolchains/llvm/prebuilt" \
    -mindepth 1 -maxdepth 1 -type d | head -1)"

echo "NDK toolchain=$PREBUILT"

ABI="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
DEVICE_API="$(adb shell getprop ro.build.version.sdk | tr -d '\r')"

echo "ABI=$ABI"
echo "Android API=$DEVICE_API"

case "$ABI" in
  arm64-v8a)
    TRIPLE="aarch64-linux-android"
    ;;
  armeabi-v7a)
    TRIPLE="armv7a-linux-androideabi"
    ;;
  x86_64)
    TRIPLE="x86_64-linux-android"
    ;;
  x86)
    TRIPLE="i686-linux-android"
    ;;
  *)
    echo "Unsupported ABI: $ABI"
    exit 1
    ;;
esac

echo "Compile target=$TRIPLE"

if [ "$DEVICE_API" -ge 34 ]; then
    API=34
else
    API="$DEVICE_API"
fi

CXX="$PREBUILT/bin/${TRIPLE}${API}-clang++"
"$CXX" --version &> /dev/null
echo "Compiler++=$CXX"

CC="$PREBUILT/bin/${TRIPLE}${API}-clang"
"$CC" --version &> /dev/null
echo "Compiler=$CC"

GLUE="$NDK/sources/android/native_app_glue"
echo "Glue=$GLUE"

mkdir -p build/obj

"$CC" \
    -MJ "$ROOT/build/obj/android_native_app_glue.json" \
    -O2 \
    -fPIC \
    -I"$GLUE" \
    -c "$GLUE/android_native_app_glue.c" \
    -o "$ROOT/build/obj/android_native_app_glue.o"

"$CXX" \
    -MJ "$ROOT/build/obj/main.json" \
    -std=c++20 \
    -O2 \
    -fPIC \
    -I"$GLUE" \
    -c "$ROOT/native/main.cpp" \
    -o "$ROOT/build/obj/main.o"

sh "$ROOT/native/generate-compile-commands.sh"

"$CXX" \
    -shared \
    "$ROOT/build/obj/main.o" \
    "$ROOT/build/obj/android_native_app_glue.o" \
    -landroid \
    -llog \
    -o "$ROOT/build/libairgap.so"
