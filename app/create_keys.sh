#!/bin/sh
set -eu

ANDROID_SDK_ROOT="$HOME/Library/Android/sdk"
BUILD_TOOLS="$ANDROID_SDK_ROOT/build-tools/36.0.0"

ROOT="$(CDPATH= cd -P -- "$(dirname -- "$0")/.." && pwd -P)"

KEYSTORE="$ROOT/keys/debug.jks"

# Override these locally for an existing keystore with a custom password.
export AIRGAP_STORE_PASSWORD="${AIRGAP_STORE_PASSWORD:-android}"
export AIRGAP_KEY_PASSWORD="${AIRGAP_KEY_PASSWORD:-$AIRGAP_STORE_PASSWORD}"

if [ ! -f "$KEYSTORE" ]; then
    echo "Generating debug signing key..."

    mkdir -p "$(dirname "$KEYSTORE")"

    keytool \
        -genkeypair \
        -keystore "$KEYSTORE" \
        -storepass:env AIRGAP_STORE_PASSWORD \
        -keypass:env AIRGAP_KEY_PASSWORD \
        -alias airgap \
        -keyalg RSA \
        -keysize 2048 \
        -validity 10000 \
        -dname "CN=Android"
fi

if ! keytool -list -keystore "$KEYSTORE" -alias airgap \
    -storepass:env AIRGAP_STORE_PASSWORD >/dev/null 2>&1; then
    echo "Cannot open $KEYSTORE with alias airgap." >&2
    echo "Set AIRGAP_STORE_PASSWORD to its password (and AIRGAP_KEY_PASSWORD if different)." >&2
    exit 1
fi

SIGNED_APK_OUT="$ROOT/build/airgap.apk"

"$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYSTORE" \
    --ks-key-alias airgap \
    --ks-pass env:AIRGAP_STORE_PASSWORD \
    --key-pass env:AIRGAP_KEY_PASSWORD \
    --out "$SIGNED_APK_OUT" \
    "$ROOT/build/airgap-aligned.apk"

echo "Checking signature..."

"$BUILD_TOOLS/apksigner" verify \
    --verbose \
    "$SIGNED_APK_OUT"
