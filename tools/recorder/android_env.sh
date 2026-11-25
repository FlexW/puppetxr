#!/usr/bin/env bash
# Sourced by the recorder's Android scripts (docs/recorder.md, Building the APK): the pinned
# versions and where setup_android.sh puts them. ANDROID_SDK_ROOT set beforehand uses an SDK that
# has them already.

ANDROID_PLATFORM_VERSION="35"
ANDROID_BUILD_TOOLS_VERSION="35.0.0"
ANDROID_NDK_VERSION="27.3.13750724"
ANDROID_MIN_SDK="29"
ANDROID_CMDLINE_TOOLS_BUILD="13114758"
JDK_RELEASE="jdk-17.0.13+11"
JDK_FILE_VERSION="17.0.13_11"
RECORDER_PACKAGE="org.puppetxr.recorder"

RECORDER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RECORDER_ROOT="$(cd "$RECORDER_DIR/../.." && pwd)"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$RECORDER_DIR/android_sdk}"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export ANDROID_NDK_ROOT="$ANDROID_SDK_ROOT/ndk/$ANDROID_NDK_VERSION"
BUILD_TOOLS="$ANDROID_SDK_ROOT/build-tools/$ANDROID_BUILD_TOOLS_VERSION"
ADB="$ANDROID_SDK_ROOT/platform-tools/adb"

JDK_DIR="$ANDROID_SDK_ROOT/jdk"
if [ -d "$JDK_DIR/Contents/Home" ]; then
    export JAVA_HOME="$JDK_DIR/Contents/Home"
elif [ -d "$JDK_DIR" ]; then
    export JAVA_HOME="$JDK_DIR"
fi
if [ -n "${JAVA_HOME:-}" ]; then
    export PATH="$JAVA_HOME/bin:$PATH"
fi
# adb's own macOS backend can crash its server as soon as a headset is attached
export ADB_LIBUSB="${ADB_LIBUSB:-1}"
