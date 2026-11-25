#!/usr/bin/env bash
# Installs what build_apk.sh needs into tools/recorder/android_sdk/ (docs/recorder.md, Building the
# APK): a JDK 17 (Eclipse Temurin) for apksigner and keytool, the Android command-line tools and
# through them the platform, build-tools, platform-tools and NDK, accepting Google's SDK licenses
# for you. Every download is pinned (android_env.sh) and checked against its SHA-256; sdkmanager
# checks its packages against Google's. Runs again safely. macOS and Linux, x86_64 and arm64.

set -euo pipefail

source "$(dirname "$0")/android_env.sh"

case "$(uname -s)" in
    Darwin) HOST_OS=mac ;;
    Linux)  HOST_OS=linux ;;
    *)      echo "error: setup_android.sh runs on macOS and Linux; on Windows use WSL" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    arm64|aarch64) HOST_ARCH=aarch64 ;;
    x86_64|amd64)  HOST_ARCH=x64 ;;
    *)             echo "error: no JDK for $(uname -m)" >&2; exit 1 ;;
esac
case "$HOST_ARCH-$HOST_OS" in
    aarch64-mac)   JDK_SHA256=d8b2f77f755d06e81a540834c5be22ed86f3c8a51a20396606c074303f8f9e2d ;;
    x64-mac)       JDK_SHA256=840535070200a944a6b582d258ee84608bd25c9f2b5d1cdddb58dfadb019675a ;;
    aarch64-linux) JDK_SHA256=0c17fa4f14c0d2cc9e9334f996fccdddc5da4459d768f3105c7ff0283c47bf62 ;;
    x64-linux)     JDK_SHA256=8682892fc02965930b9022c066fa164dd6f458ef4a5dc262016aa28333b30f49 ;;
esac
case "$HOST_OS" in
    mac)   CMDLINE_TOOLS_SHA256=5673201e6f3869f418eeed3b5cb6c4be7401502bd0aae1b12a29d164d647a54e ;;
    linux) CMDLINE_TOOLS_SHA256=7ec965280a073311c339e571cd5de778b9975026cfcbe79f2b1cdcb1e15317ee ;;
esac
JDK_URL="https://github.com/adoptium/temurin17-binaries/releases/download/${JDK_RELEASE/+/%2B}/OpenJDK17U-jdk_${HOST_ARCH}_${HOST_OS}_hotspot_${JDK_FILE_VERSION}.tar.gz"
CMDLINE_TOOLS_URL="https://dl.google.com/android/repository/commandlinetools-${HOST_OS}-${ANDROID_CMDLINE_TOOLS_BUILD}_latest.zip"

sha256_of () {
    if command -v sha256sum > /dev/null; then
        sha256sum "$1" | cut -d ' ' -f 1
    else
        shasum -a 256 "$1" | cut -d ' ' -f 1
    fi
}

fetch () {
    local url="$1" file="$2" sha256="$3"
    if [ -f "$file" ] && [ "$(sha256_of "$file")" = "$sha256" ]; then
        return
    fi
    echo "Downloading $url"
    curl -fL --retry 4 -o "$file.part" "$url"
    if [ "$(sha256_of "$file.part")" != "$sha256" ]; then
        echo "error: $url does not have the SHA-256 $sha256" >&2
        rm -f "$file.part"
        exit 1
    fi
    mv "$file.part" "$file"
}

mkdir -p "$ANDROID_SDK_ROOT/downloads"
if [ ! -x "$JDK_DIR/bin/java" ] && [ ! -x "$JDK_DIR/Contents/Home/bin/java" ]; then
    fetch "$JDK_URL" "$ANDROID_SDK_ROOT/downloads/jdk.tar.gz" "$JDK_SHA256"
    rm -rf "$JDK_DIR"
    mkdir -p "$JDK_DIR"
    tar -xzf "$ANDROID_SDK_ROOT/downloads/jdk.tar.gz" -C "$JDK_DIR" --strip-components=1
    source "$(dirname "$0")/android_env.sh"
fi
SDKMANAGER="$ANDROID_SDK_ROOT/cmdline-tools/bin/sdkmanager"
if [ ! -x "$SDKMANAGER" ]; then
    fetch "$CMDLINE_TOOLS_URL" "$ANDROID_SDK_ROOT/downloads/cmdline-tools.zip" "$CMDLINE_TOOLS_SHA256"
    rm -rf "$ANDROID_SDK_ROOT/cmdline-tools"
    unzip -q -o "$ANDROID_SDK_ROOT/downloads/cmdline-tools.zip" -d "$ANDROID_SDK_ROOT"
fi
yes | "$SDKMANAGER" --sdk_root="$ANDROID_SDK_ROOT" --licenses > /dev/null || true
"$SDKMANAGER" --sdk_root="$ANDROID_SDK_ROOT" \
    "platforms;android-$ANDROID_PLATFORM_VERSION" \
    "build-tools;$ANDROID_BUILD_TOOLS_VERSION" \
    "platform-tools" \
    "ndk;$ANDROID_NDK_VERSION"
rm -rf "$ANDROID_SDK_ROOT/downloads"
echo "Android SDK, NDK and JDK in $ANDROID_SDK_ROOT"
