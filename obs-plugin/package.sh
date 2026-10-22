#!/bin/bash
# Arma dist/OBSCam-mac.zip para instalar en otra Mac (Apple Silicon) sin Android Studio.
set -euo pipefail
cd "$(dirname "$0")"

./build.sh
(cd ../android && ./gradlew -q assembleRelease)

OUT=dist/OBSCam-mac
rm -rf dist && mkdir -p "$OUT"
cp -R build/obs-android-cam.plugin "$OUT/"
cp ../android/app/build/outputs/apk/release/app-release.apk "$OUT/OBSCam.apk"
cp installer/instalar.command "$OUT/"
cp installer/LEEME.txt "$OUT/"
chmod +x "$OUT/instalar.command"
(cd dist && ditto -c -k --norsrc --noextattr --keepParent OBSCam-mac OBSCam-mac.zip)
echo "OK → $(pwd)/dist/OBSCam-mac.zip"
