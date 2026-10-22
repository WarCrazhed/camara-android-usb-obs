#!/bin/bash
# Instala el plugin de OBS y (si hay teléfono conectado) la app OBS Cam.
cd "$(dirname "$0")"
set -e

PLUGIN=obs-android-cam.plugin
DEST="$HOME/Library/Application Support/obs-studio/plugins"

echo "== Plugin de OBS"
if pgrep -x OBS >/dev/null; then
  echo "Cierra OBS antes de instalar y vuelve a correr este instalador."
  read -r -p "Enter para salir" _
  exit 1
fi
mkdir -p "$DEST"
rm -rf "$DEST/$PLUGIN"
cp -R "$PLUGIN" "$DEST/"
# Al copiarlo por AirDrop/descarga macOS lo marca en cuarentena y OBS no podría cargarlo.
xattr -dr com.apple.quarantine "$DEST/$PLUGIN" 2>/dev/null || true
codesign --force --deep --sign - "$DEST/$PLUGIN" >/dev/null 2>&1 || true
echo "Instalado en: $DEST/$PLUGIN"

echo
echo "== App del teléfono"
ADB="$DEST/$PLUGIN/Contents/Resources/adb"
command -v adb >/dev/null && ADB=$(command -v adb)
if "$ADB" devices | grep -q "device$"; then
  "$ADB" install -r OBSCam.apk && echo "App instalada en el teléfono."
else
  echo "No hay teléfono listo (conéctalo por USB, con depuración USB, y acepta el aviso)."
  echo "Si la app ya está en el teléfono, no hace falta nada más."
fi

echo
echo "Listo. Abre OBS → Fuentes → + → «Cámara Android (USB)»."
read -r -p "Enter para cerrar" _
