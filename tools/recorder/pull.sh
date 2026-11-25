#!/usr/bin/env bash
# Copies the takes of the attached headset into DIR (default ./takes), one directory per recorder
# session (docs/recorder.md, Takes). The takes stay on the headset.

set -euo pipefail

source "$(dirname "$0")/android_env.sh"

dest="${1:-takes}"
mkdir -p "$dest"
"$ADB" pull "/sdcard/Android/data/$RECORDER_PACKAGE/files/takes/." "$dest"
echo "takes in $dest"
