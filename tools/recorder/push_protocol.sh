#!/usr/bin/env bash
# Gives the recorder on the attached headset a protocol of its own (docs/recorder.md, Protocol);
# with --remove it goes back to the built-in one. Read at the next start. The file goes into the
# app's own storage through run-as, as a file adb pushes elsewhere belongs to the shell and the app
# cannot open it.

set -euo pipefail

source "$(dirname "$0")/android_env.sh"

if [ "${1:-}" = "--remove" ]; then
    "$ADB" shell run-as "$RECORDER_PACKAGE" rm -f files/protocol.json
    exit 0
fi
if [ $# -ne 1 ] || [ ! -f "$1" ]; then
    echo "usage: push_protocol.sh <protocol.json> | --remove" >&2
    exit 2
fi
"$ADB" push "$1" /data/local/tmp/puppetxr-recorder-protocol.json > /dev/null
"$ADB" shell run-as "$RECORDER_PACKAGE" cp /data/local/tmp/puppetxr-recorder-protocol.json files/protocol.json
"$ADB" shell rm /data/local/tmp/puppetxr-recorder-protocol.json
echo "protocol.json on the headset; restart the recorder"
