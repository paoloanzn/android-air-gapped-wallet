#!/bin/sh
set -eu

# Builds .apk
ANDROID_SDK_ROOT="$HOME/Library/Android/sdk"
BUILD_TOOLS="$ANDROID_SDK_ROOT/build-tools/36.0.0"
ANDROID_JAR="$ANDROID_SDK_ROOT/platforms/android-36/android.jar"

ROOT="$(CDPATH= cd -P -- "$(dirname -- "$0")/.." && pwd -P)"

# Compile first
cmake --preset android -S "$ROOT" || exit 1
cmake --build "$ROOT/build" || exit 1

APK_OUT="$ROOT/build/airgap-base.apk"

"$BUILD_TOOLS/aapt2" link \
    -I "$ANDROID_JAR" \
    --manifest "$ROOT/app/AndroidManifest.xml" \
    -o "$APK_OUT"

echo "Created $APK_OUT"

mkdir -p "$ROOT/build/apk/lib/arm64-v8a"
mkdir -p "$ROOT/build/apk/assets"

cp "$ROOT/build/libairgap.so" \
   "$ROOT/build/apk/lib/arm64-v8a/libairgap.so"
cp "$ROOT/app/assets/bip39_english.csv" \
   "$ROOT/build/apk/assets/bip39_english.csv"

cp "$ROOT/build/airgap-base.apk" "$ROOT/build/airgap-unaligned.apk"

cd "$ROOT/build/apk"

zip -0 ../airgap-unaligned.apk \
    lib/arm64-v8a/libairgap.so \
    assets/bip39_english.csv

cd "$ROOT"

echo "Built $ROOT/build/airgap-unaligned.apk"

echo "Aligning..."

"$BUILD_TOOLS/zipalign" \
    -P 16 \
    -f \
    -v \
    4 \
    "$ROOT/build/airgap-unaligned.apk" \
    "$ROOT/build/airgap-aligned.apk"

"$BUILD_TOOLS/zipalign" \
    -c \
    -P 16 \
    -v \
    4 \
    "$ROOT/build/airgap-aligned.apk"

echo "Signing..."

sh "$ROOT/app/create_keys.sh"

echo "Installing..."

adb push "$ROOT/build/airgap.apk" /sdcard/

echo "🪖 Done."
