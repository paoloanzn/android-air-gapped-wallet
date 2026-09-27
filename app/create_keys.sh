ANDROID_SDK_ROOT="$HOME/Library/Android/sdk"
BUILD_TOOLS="$ANDROID_SDK_ROOT/build-tools/36.0.0"

ROOT="$(CDPATH= cd -P -- "$(dirname -- "$0")/.." && pwd -P)"

KEYSTORE="$ROOT/keys/debug.jks"

if [ ! -f "$KEYSTORE" ]; then
    echo "Generating debug signing key..."

    mkdir -p "$(dirname "$KEYSTORE")"

    keytool \
        -genkeypair \
        -keystore "$KEYSTORE" \
        -storepass android \
        -keypass android \
        -alias airgap \
        -keyalg RSA \
        -keysize 2048 \
        -validity 10000 \
        -dname "CN=Android"
fi

SIGNED_APK_OUT="$ROOT/build/airgap.apk"

"$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias airgap \
    --ks-pass pass:android \
    --key-pass pass:android \
    --out "$SIGNED_APK_OUT" \
    "$ROOT/build/airgap-aligned.apk"

echo "Checking signature..."

"$BUILD_TOOLS/apksigner" verify \
    --verbose \
    "$SIGNED_APK_OUT"