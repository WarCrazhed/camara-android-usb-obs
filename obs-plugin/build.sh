#!/bin/bash
# Compila el plugin (y la herramienta probe). Uso: ./build.sh [install]
set -euo pipefail
cd "$(dirname "$0")"

OBS_APP=${OBS_APP:-/Applications/OBS.app}
OBS_TAG=${OBS_TAG:-32.2.2}
LIBOBS=deps/obs-studio/libobs
SIMDE=deps/simde
SIMDE_TAG=${SIMDE_TAG:-v0.8.2}
NAME=obs-android-cam
BUNDLE=build/$NAME.plugin

if [ ! -f "$LIBOBS/obs-module.h" ]; then
  echo "Bajando headers de libobs $OBS_TAG…"
  rm -rf deps/obs-studio
  git clone -q --depth 1 --branch "$OBS_TAG" --filter=blob:none --sparse \
    https://github.com/obsproject/obs-studio.git deps/obs-studio
  git -C deps/obs-studio sparse-checkout set libobs
fi

if [ ! -f "$SIMDE/simde/x86/sse2.h" ]; then
  echo "Bajando SIMDe $SIMDE_TAG (headers que pide libobs)…"
  rm -rf "$SIMDE"
  git clone -q --depth 1 --branch "$SIMDE_TAG" https://github.com/simd-everywhere/simde.git "$SIMDE"
fi

CFLAGS=(-O2 -Wall -Wextra -Wno-unused-parameter -arch arm64 -mmacosx-version-min=13.0)
FRAMEWORKS=(-framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation)

mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources/locale"
clang "${CFLAGS[@]}" -bundle -Iinclude -I"$LIBOBS" -I"$SIMDE" -F"$OBS_APP/Contents/Frameworks" -framework libobs "${FRAMEWORKS[@]}" \
  src/plugin-main.c src/stream-client.c src/vt-decoder.c src/rotator.c -o "$BUNDLE/Contents/MacOS/$NAME"
cp Info.plist "$BUNDLE/Contents/Info.plist"
cp data/locale/*.ini "$BUNDLE/Contents/Resources/locale/"

# adb dentro del plugin, para Macs sin Android SDK.
ADB_SRC=${ADB_SRC:-$HOME/Library/Android/sdk/platform-tools/adb}
if [ -x "$ADB_SRC" ]; then
  cp "$ADB_SRC" "$BUNDLE/Contents/Resources/adb"
else
  echo "Aviso: no encontré adb en $ADB_SRC; el plugin dependerá del adb instalado en la Mac."
fi
codesign --force --sign - "$BUNDLE" >/dev/null

clang "${CFLAGS[@]}" "${FRAMEWORKS[@]}" tools/probe.c src/stream-client.c src/vt-decoder.c src/rotator.c \
  -framework ImageIO -framework CoreGraphics -o build/probe
echo "OK → $BUNDLE"

if [ "${1:-}" = "install" ]; then
  DEST="$HOME/Library/Application Support/obs-studio/plugins"
  mkdir -p "$DEST"
  rm -rf "$DEST/$NAME.plugin"
  cp -R "$BUNDLE" "$DEST/"
  echo "Instalado en $DEST/$NAME.plugin — reinicia OBS."
fi
