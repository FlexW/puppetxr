#!/usr/bin/env bash
# Builds build/android-recorder/puppetxr-recorder.apk (docs/recorder.md, Building the APK): the
# native library with the NDK and CMake, then aapt2, zip, zipalign and apksigner; no Gradle.
# Signed with a debug key made in build/android-recorder/ on the first build. With --install it
# also installs it on the attached headset, with --run it starts it too.

set -euo pipefail

source "$(dirname "$0")/android_env.sh"

install=0
run=0
for arg in "$@"; do
    case "$arg" in
        --install) install=1 ;;
        --run)     install=1; run=1 ;;
        *)         echo "usage: build_apk.sh [--install] [--run]" >&2; exit 2 ;;
    esac
done

if [ ! -d "$ANDROID_NDK_ROOT" ] || [ ! -x "$BUILD_TOOLS/aapt2" ] || ! command -v keytool > /dev/null; then
    "$RECORDER_DIR/setup_android.sh"
    source "$RECORDER_DIR/android_env.sh"
fi

OUT="$RECORDER_ROOT/build/android-recorder"
GENERATOR=()
if command -v ninja > /dev/null; then
    GENERATOR=(-G Ninja)
fi
cmake -S "$RECORDER_DIR" -B "$OUT/cxx" "${GENERATOR[@]}" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$ANDROID_MIN_SDK" \
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo > /dev/null
cmake --build "$OUT/cxx" --target puppetxr_recorder

rm -rf "$OUT/apk" "$OUT/unsigned.apk" "$OUT/aligned.apk"
mkdir -p "$OUT/apk/lib/arm64-v8a"
STRIP="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$(ls "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt" | head -1)/bin/llvm-strip"
"$STRIP" -o "$OUT/apk/lib/arm64-v8a/libpuppetxr_recorder.so" "$OUT/cxx/libpuppetxr_recorder.so"
"$STRIP" -o "$OUT/apk/lib/arm64-v8a/libopenxr_loader.so" "$OUT/cxx/openxr/src/loader/libopenxr_loader.so"

"$BUILD_TOOLS/aapt2" link -o "$OUT/unsigned.apk" \
    --manifest "$RECORDER_DIR/AndroidManifest.xml" \
    -I "$ANDROID_SDK_ROOT/platforms/android-$ANDROID_PLATFORM_VERSION/android.jar" \
    --min-sdk-version "$ANDROID_MIN_SDK" --target-sdk-version 32 --debug-mode
# Native libraries stored uncompressed and page aligned, so they load from the APK in place
(cd "$OUT/apk" && zip -q -0 -r "$OUT/unsigned.apk" lib)
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

KEYSTORE="$OUT/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android -alias debug \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=puppetxr recorder debug" > /dev/null
fi
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT/puppetxr-recorder.apk" "$OUT/aligned.apk"
rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk" "$OUT/puppetxr-recorder.apk.idsig"
echo "$OUT/puppetxr-recorder.apk"

if [ "$install" = 1 ]; then
    "$ADB" install -r "$OUT/puppetxr-recorder.apk"
fi
if [ "$run" = 1 ]; then
    "$ADB" shell am start -n "$RECORDER_PACKAGE/android.app.NativeActivity"
fi
