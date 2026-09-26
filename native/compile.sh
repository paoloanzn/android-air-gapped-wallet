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
echo "Compiler=$CXX"

mkdir -p build/

"$CXX" \
-O2 \
-fPIE \
-pie \
$(cd -P -- "$(dirname -- "$0")" && pwd -P)/main.cpp \
-o build/main
