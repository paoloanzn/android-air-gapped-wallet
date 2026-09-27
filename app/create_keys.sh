ANDROID_SDK_ROOT="$HOME/Library/Android/sdk"
BUILD_TOOLS="$ANDROID_SDK_ROOT/build-tools/36.0.0"

ROOT="$(CDPATH= cd -P -- "$(dirname -- "$0")/.." && pwd -P)"
KEYS="$ROOT/keys"

mkdir -p "$KEYS"

keytool \
    -genkeypair \
    -keystore "$KEYS/debug.jks" \
    -alias airgap \
    -keyalg RSA \
    -keysize 2048 \
    -validity 10000

SIGNED_APK_OUT="$ROOT/build/airgap.apk"

"$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYS/debug.jks" \
    --ks-key-alias airgap \
    --out "$SIGNED_APK_OUT" \
    "$ROOT/build/airgap-aligned.apk"

echo "Checking signature..."

"$BUILD_TOOLS/apksigner" verify \
    --verbose \
    "$SIGNED_APK_OUT"