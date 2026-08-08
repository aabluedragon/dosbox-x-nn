#!/bin/sh
# Installs and (re)launches the app on the booted simulator.
set -e
cd "$(dirname "$0")"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
xcrun simctl terminate booted org.nukemnet.dosboxx 2>/dev/null || true
xcrun simctl install booted out/DOSBox-X.app
xcrun simctl launch booted org.nukemnet.dosboxx
echo "data container: $(xcrun simctl get_app_container booted org.nukemnet.dosboxx data)"
