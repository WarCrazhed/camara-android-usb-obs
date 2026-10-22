# OBS Cam — POCO X7 Pro como cámara de OBS por USB

Usa un teléfono Android (probado con POCO X7 Pro) como cámara en OBS Studio para macOS: hasta 4K a 30 fps, comprimido en HEVC en el teléfono y decodificado por hardware en la Mac.

```
POCO X7 Pro                                   Mac
┌──────────────────────────┐   USB (adb)    ┌────────────────────────────────┐
│ App OBS Cam (Kotlin)     │ ─────────────> │ Plugin obs-android-cam (C)     │
│ Camera2 → MediaCodec HEVC│  adb forward   │ VideoToolbox (HW) → NV12 → OBS │
│ servidor 127.0.0.1:27183 │                │ fuente «Cámara Android (USB)»  │
└──────────────────────────┘                └────────────────────────────────┘
```

Son dos piezas y hacen falta las dos:

| Pieza | Carpeta | Va en | Qué hace |
|---|---|---|---|
| App **OBS Cam** | `android/` | Teléfono | Abre la cámara, comprime el video y lo manda por USB |
| Plugin **obs-android-cam** | `obs-plugin/` | Mac (OBS) | Recibe el video, lo decodifica y crea la fuente «Cámara Android (USB)» |

---

## 1. Requisitos

**Para usarlo** (cualquier Mac):
- Mac con Apple Silicon (M1–M4).
- [OBS Studio](https://obsproject.com) **32.x**.
- Cable USB de datos (no solo de carga).

**Para compilarlo desde este repo**, además:
- Xcode o las Command Line Tools (`xcode-select --install`).
- Android Studio (trae el JDK y el SDK; `adb` queda en `~/Library/Android/sdk/platform-tools/`).
- `git` y conexión a internet la primera vez (el build baja los headers de OBS y SIMDe).

> ¿Solo quieres instalarlo en otra Mac sin compilar? Ve a [5. Llevarlo a otra Mac](#5-llevarlo-a-otra-mac).

---

## 2. Preparar el teléfono (una sola vez)

1. **Activar Opciones de desarrollador:** Ajustes → Sobre el teléfono → toca 7 veces **Versión de HyperOS**.
2. **Activar depuración USB:** Ajustes → Ajustes adicionales → Opciones de desarrollador:
   - **Depuración USB**: activada.
   - **Depuración USB (ajustes de seguridad)**: activada (HyperOS la pide para instalar apps por USB; requiere cuenta Xiaomi y SIM).
3. Conecta el teléfono a la Mac y acepta **«¿Permitir depuración USB?»** (marca «Permitir siempre desde este equipo»).
4. Después de instalar la app (paso 3), en la app toca **«Quitar optimización de batería»** y en Ajustes → Apps → OBS Cam → Ahorro de batería elige **«Sin restricciones»**. Si no, HyperOS puede cerrarla en segundo plano.

Comprueba la conexión:
```bash
~/Library/Android/sdk/platform-tools/adb devices
# TCQ8QWWSAECQG6WC   device      ← "device" = listo; "unauthorized" = falta aceptar el aviso
```

---

## 3. Compilar e instalar desde el repo

```bash
git clone <url-del-repo> camara
cd camara
```

### 3.1 App del teléfono
Con el teléfono conectado:
```bash
cd android
./gradlew installDebug
```
O abre la carpeta `android/` en Android Studio y dale **Run**.

Abre **OBS Cam** en el teléfono una vez y acepta el permiso de cámara. La app muestra «Esperando a OBS…» y la lista de cámaras que el teléfono deja usar.

### 3.2 Plugin de OBS
Con OBS **cerrado**:
```bash
cd obs-plugin
./build.sh install
```
- La primera vez baja los headers de libobs 32.2.2 y SIMDe a `obs-plugin/deps/`.
- Copia `adb` dentro del plugin, así funciona aunque la Mac no tenga Android Studio.
- Lo instala en `~/Library/Application Support/obs-studio/plugins/obs-android-cam.plugin`.

Abre OBS. En Ayuda → Archivos de registro debe aparecer `obs-android-cam` entre los módulos cargados.

---

## 4. Usarlo en OBS

### 4.1 Agregar la cámara
1. En la escena: Fuentes → **+** → **Cámara Android (USB)** → Aceptar.
2. **Dispositivo:** elige **POCO X7 Pro** (o «Automático» = primer teléfono conectado).
3. Deja los valores por defecto: 4K, 30 fps, HEVC, 50 Mbps.
4. Si la fuente no llena el lienzo: clic derecho → Transformar → **Ajustar a la pantalla** (⌘F).

El plugin abre solo la app en el teléfono y se reconecta si desconectas el cable.

### 4.2 Grabar también la pantalla de la Mac
En la misma escena: Fuentes → **+** → **Captura de pantalla**. Acomoda la cámara encima (por ejemplo, en una esquina).

### 4.3 Opciones de la fuente

| Opción | Valores | Notas |
|---|---|---|
| Dispositivo | Automático / POCO X7 Pro | «Actualizar dispositivos» si conectaste el teléfono después |
| Cámara | Trasera / Frontal | La trasera es la que llega a 4K |
| Resolución | 4K, 1440p, 1080p, 720p, **4:3 sensor completo** | 4:3 = 2880×2160 o 1920×1440: ~33 % más imagen en vertical |
| FPS | 60 / 30 / 24 | Este teléfono da máximo 30; si pides 60 baja solo a 30 |
| Códec | HEVC / H.264 | HEVC da mejor calidad por Mbps |
| Tasa de bits | 5–150 Mbps | 50 Mbps es buena calidad en 4K |
| **Zoom** | 1× – 10× | Cambia en vivo sin cortar el video |
| **Rotación** | Automática / 0° / 90° / 180° / 270° | Automática sigue cómo está el teléfono; fíjala si lo vas a mover mientras grabas |
| Estabilización de video | Sí / No | Apagarla da un poco más de campo (la estabilización recorta bordes) |
| Baja latencia (sin búfer) | Sí / No | Sí = menos retraso |
| Avanzado → ID de cámara | `0`, `1` | Reemplaza «Cámara»; la app lista los IDs disponibles |
| Avanzado → Ruta de adb | vacío = automático | Solo si usas un adb en otra ruta |

Cambiar zoom, rotación o latencia se aplica al instante. Cambiar resolución, fps, códec, cámara o tasa de bits reabre la cámara (~1 s sin imagen).

### 4.4 Zoom con atajos de teclado
Ajustes → Atajos de teclado → busca **«Cámara Android»** y asigna teclas a:
- **acercar** (+15 % por pulsación)
- **alejar** (−15 % por pulsación)
- **zoom 1×**

### 4.5 Teléfono vertical
Con Rotación en **Automática**, si paras el teléfono la fuente pasa a 2160×3840. Para un video totalmente vertical, cambia el lienzo en Ajustes → Video a 2160×3840.

### 4.6 Grabar en 4K
1. Ajustes → Video → Resolución base y de salida: **3840×2160**, FPS: **30**.
2. Ajustes → Salida → Modo de salida **Avanzado** → Grabación → Codificador **Apple VT HEVC Hardware Encoder**.

---

## 5. Llevarlo a otra Mac

En la Mac donde está el repo:
```bash
cd obs-plugin
./package.sh        # → obs-plugin/dist/OBSCam-mac.zip (~8 MB)
```

El zip trae:
- `obs-android-cam.plugin`: el plugin, con su propio `adb`.
- `OBSCam.apk`: la app del teléfono.
- `instalar.command`: copia el plugin y, si hay teléfono conectado, instala la app.
- `LEEME.txt`: estos pasos.

En la otra Mac (solo necesita **OBS 32.x**, no Android Studio):
1. Copia el zip (AirDrop, USB o nube) y descomprímelo.
2. Cierra OBS.
3. Clic derecho en `instalar.command` → **Abrir** → Abrir.
   Si macOS lo bloquea: Ajustes del Sistema → Privacidad y seguridad → **Abrir de todos modos**, o en Terminal:
   `bash ~/Downloads/OBSCam-mac/instalar.command`
4. Conecta el teléfono y acepta «¿Permitir depuración USB?» (cada Mac nueva lo pide una vez).
5. Abre OBS y sigue [4. Usarlo en OBS](#4-usarlo-en-obs).

---

## 6. Solución de problemas

| Síntoma | Causa / solución |
|---|---|
| No aparece «Cámara Android (USB)» en OBS | El plugin no cargó: revisa que esté en `~/Library/Application Support/obs-studio/plugins/` y reinicia OBS. En el registro de OBS busca `obs-android-cam` |
| Estado: «acepta ¿Permitir depuración USB?» | Desbloquea el teléfono y acepta el aviso |
| Estado: «Conecta el teléfono por USB…» | Cable solo de carga, o depuración USB apagada. Prueba `adb devices` |
| Estado: «Esperando la app OBS Cam…» | Abre OBS Cam en el teléfono a mano; si se cierra sola, quita la optimización de batería (paso 2.4) |
| La imagen se ve de lado | Rotación en «Automática», o fíjala a mano |
| La imagen se corta cada pocos segundos | Hay otro programa usando la cámara del teléfono (otro OBS, `probe`): el teléfono atiende una conexión a la vez |
| Imagen negra | La lente está tapada (teléfono boca abajo) o otra app tomó la cámara |
| El teléfono se calienta | Normal en 4K continuo: quítale la funda o baja a 1440p |

Mensajes del plugin: OBS → Ayuda → Archivos de registro → líneas `[android-cam]`.
Mensajes del teléfono: `adb logcat -s ObsCam`.

---

## 7. Límites de este POCO X7 Pro (HyperOS 3, Android 16)

HyperOS solo deja usar 2 cámaras a apps que no son de Xiaomi:

| Cámara | ID | Máximo |
|---|---|---|
| Trasera principal | 0 | **3840×2160 @ 30 fps**, zoom 1×–10× |
| Frontal | 1 | 2592×1458 @ 30 fps |

- **Sin 60 fps** en ninguna resolución.
- **Sin zoom menor a 1×:** el ultra gran angular existe (ID oculto 2), pero al pedirle video desde una app de terceros se cae el servicio de cámara de HyperOS (`camerahalserver`). Para ver más, usa las resoluciones 4:3.
- **4096×3072** (4:3 completo a máxima resolución): el encoder del teléfono no lo soporta; el máximo 4:3 es 2880×2160.
- **Audio:** no se transmite; usa el micrófono de la Mac o uno USB.

---

## 8. Para desarrollo

### Probar sin OBS
`probe` hace lo mismo que el plugin (abre la app, conecta, decodifica y gira) y reporta los resultados. Cierra OBS antes de usarlo.
```bash
./obs-plugin/build/probe - 3840 2160 30 hevc 10 back
#                        serial|-  ancho alto fps codec segundos back|front

ACAM_CAMERA_ID=1 ./obs-plugin/build/probe ...      # forzar ID de cámara
ACAM_ZOOM_TEST=/tmp/zoom ./obs-plugin/build/probe ... # guarda JPEG a 1× y a 3× (zoom en vivo)
```
Referencia en este equipo: 320/320 cuadros decodificados, 3840×2160, 29.7 fps, ~54 Mbps, primer cuadro en 1.4 s.

### Estructura
```
android/app/src/main/java/com/camara/obscam/
  MainActivity.kt     pantalla de la app, permisos, lista de cámaras
  StreamService.kt    servicio en primer plano, servidor TCP, orientación
  StreamSession.kt    una conexión de OBS: protocolo y comandos en vivo
  CameraStreamer.kt   Camera2 → MediaCodec, zoom, elección de tamaño/fps/encoder
  CameraInfo.kt       diagnóstico de cámaras
obs-plugin/
  src/plugin-main.c   fuente de OBS, propiedades, hilo de captura, atajos
  src/stream-client.* adb (listar, abrir app, forward) y protocolo
  src/vt-decoder.*    decodificación H.264/HEVC con VideoToolbox
  src/rotator.*       giro por hardware (VTPixelRotationSession)
  tools/probe.c       prueba de punta a punta sin OBS
  build.sh            compila plugin + probe (install = copia a OBS)
  package.sh          arma el zip para otra Mac
  installer/          instalar.command y LEEME.txt del zip
```

### Protocolo
Documentado en `android/app/src/main/java/com/camara/obscam/StreamSession.kt`. Resumen: OBS manda una línea JSON con la configuración (y después líneas como `{"zoom":2.5}`); el teléfono responde con una cabecera de 16 bytes y luego paquetes `pts | flags | tamaño | datos Annex-B`. En los flags van keyframe, config y la rotación.
