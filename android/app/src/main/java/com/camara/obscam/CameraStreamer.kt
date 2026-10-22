package com.camara.obscam

import android.annotation.SuppressLint
import android.content.Context
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.hardware.camera2.params.StreamConfigurationMap
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import java.io.DataOutputStream
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/**
 * Camera2 → superficie de entrada de MediaCodec → paquetes Annex-B por el socket.
 * Todo se usa desde el hilo de la sesión; la cámara corre en su propio HandlerThread.
 */
class CameraStreamer(context: Context, private val request: StreamConfig) {

    private val manager = context.getSystemService(CameraManager::class.java)
    private val cameraThread = HandlerThread("obscam-camera").apply { start() }
    private val cameraHandler = Handler(cameraThread.looper)
    private val cameraExecutor = Executor { cameraHandler.post(it) }

    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var captureBuilder: CaptureRequest.Builder? = null
    private var zoomRange: Range<Float>? = null
    private var codec: MediaCodec? = null
    private var inputSurface: Surface? = null
    @Volatile private var cameraError: String? = null
    private var released = false
    private var sensorOrientation = 0
    private var frontFacing = false

    fun start(): ActualConfig {
        val cameraId = pickCamera()
        val chars = manager.getCameraCharacteristics(cameraId)
        sensorOrientation = chars[CameraCharacteristics.SENSOR_ORIENTATION] ?: 0
        frontFacing = chars[CameraCharacteristics.LENS_FACING] == CameraMetadata.LENS_FACING_FRONT
        val map = chars[CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP]
            ?: throw IllegalStateException("La cámara $cameraId no tiene salidas de video")

        val mime = if (request.codec == StreamConfig.Codec.HEVC) MediaFormat.MIMETYPE_VIDEO_HEVC
        else MediaFormat.MIMETYPE_VIDEO_AVC
        val size = pickSize(map)

        // El fps final debe poder darlo la cámara Y el encoder; si no, se baja a 30.
        val candidates = listOf(request.fps, 30, 24).distinct()
        var chosen: Triple<Int, Range<Int>, MediaCodecInfo>? = null
        for (fps in candidates) {
            val range = pickFpsRange(chars, map, size, fps) ?: continue
            val encoder = pickEncoder(mime, size, fps) ?: continue
            chosen = Triple(fps, range, encoder)
            break
        }
        val (fps, fpsRange, encoderInfo) = chosen
            ?: throw IllegalStateException(
                "Ni la cámara $cameraId ni el encoder ${mime.substringAfter('/')} soportan ${size.width}×${size.height}"
            )

        val caps = encoderInfo.getCapabilitiesForType(mime)
        val bitrate = caps.videoCapabilities!!.bitrateRange.clamp(request.bitrate)
        val encoder = MediaCodec.createByCodecName(encoderInfo.name)
        codec = encoder
        configureEncoder(encoder, caps, mime, size, fps, bitrate)
        inputSurface = encoder.createInputSurface()
        encoder.start()

        openCamera(cameraId)
        startCapture(chars, fpsRange)

        Log.i(TAG, "Transmitiendo ${size.width}x${size.height}@$fps $mime ${bitrate / 1_000_000}Mbps con ${encoderInfo.name}")
        return ActualConfig(
            size.width, size.height, fps, bitrate, request.codec, cameraId, encoderInfo.name,
            zoomRange?.upper ?: 1f,
        )
    }

    /** Envía paquetes hasta que [stop] devuelva true, falle la cámara o se cierre el socket. */
    fun drain(out: DataOutputStream, stop: () -> Boolean) {
        val encoder = codec ?: return
        val info = MediaCodec.BufferInfo()
        var buffer = ByteArray(1 shl 20)
        while (!stop()) {
            cameraError?.let { throw IOException(it) }
            val index = encoder.dequeueOutputBuffer(info, 100_000)
            if (index < 0) continue

            if (info.size > 0) {
                if (buffer.size < info.size) buffer = ByteArray(info.size + (info.size shr 1))
                val data = encoder.getOutputBuffer(index)!!
                data.position(info.offset)
                data.get(buffer, 0, info.size)

                var flags = 0
                if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) flags = flags or 1
                if (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0) flags = flags or 2
                flags = flags or (uprightRotation() / 90 shl 2)
                out.writeLong(info.presentationTimeUs)
                out.writeInt(flags)
                out.writeInt(info.size)
                out.write(buffer, 0, info.size)
                out.flush()
            }
            encoder.releaseOutputBuffer(index, false)
            if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
        }
    }

    /** Grados en sentido horario que hay que girar el cuadro para verlo derecho (fórmula de JPEG_ORIENTATION). */
    private fun uprightRotation(): Int {
        val device = StreamService.deviceOrientation
        return (sensorOrientation + (if (frontFacing) -device else device) + 360) % 360
    }

    /** Cambia el zoom en vivo (1× = sin zoom). Se llama desde el hilo que lee comandos de OBS. */
    @Synchronized
    fun setZoom(ratio: Float) {
        if (released || Build.VERSION.SDK_INT < 30) return
        val range = zoomRange ?: return
        val builder = captureBuilder ?: return
        builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, range.clamp(ratio))
        runCatching { session?.setRepeatingRequest(builder.build(), null, cameraHandler) }
    }

    @Synchronized
    fun release() {
        if (released) return
        released = true
        runCatching { session?.close() }
        runCatching { device?.close() }
        runCatching { codec?.stop() }
        runCatching { codec?.release() }
        runCatching { inputSurface?.release() }
        cameraThread.quitSafely()
    }

    // --- selección de hardware ---

    private fun pickCamera(): String {
        val ids = manager.cameraIdList
        if (request.cameraId.isNotEmpty()) {
            // Solo IDs listados: abrir los ocultos (ultra gran angular, ID 2) tumba el camerahalserver de HyperOS.
            if (request.cameraId in ids) return request.cameraId
            throw IllegalStateException("No existe la cámara con ID ${request.cameraId} (hay: ${ids.joinToString()})")
        }
        val wanted = if (request.front) CameraMetadata.LENS_FACING_FRONT else CameraMetadata.LENS_FACING_BACK
        return ids.firstOrNull { manager.getCameraCharacteristics(it)[CameraCharacteristics.LENS_FACING] == wanted }
            ?: ids.firstOrNull()
            ?: throw IllegalStateException("El teléfono no expone cámaras")
    }

    private fun pickSize(map: StreamConfigurationMap): Size {
        val sizes = map.getOutputSizes(MediaCodec::class.java).orEmpty()
        if (sizes.isEmpty()) throw IllegalStateException("La cámara no da salida para el encoder")
        sizes.firstOrNull { it.width == request.width && it.height == request.height }?.let { return it }
        val fits = sizes.filter { it.width <= request.width && it.height <= request.height }
        return fits.filter { it.width * 9 == it.height * 16 }.maxByOrNull { it.width.toLong() * it.height }
            ?: fits.maxByOrNull { it.width.toLong() * it.height }
            ?: sizes.minBy { it.width.toLong() * it.height }
    }

    private fun pickFpsRange(
        chars: CameraCharacteristics, map: StreamConfigurationMap, size: Size, fps: Int,
    ): Range<Int>? {
        val minDuration = map.getOutputMinFrameDuration(MediaCodec::class.java, size)
        if (minDuration > 0 && 1_000_000_000.0 / minDuration < fps - 0.5) return null
        val ranges = chars[CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES].orEmpty()
        // Rango fijo [fps,fps] da cadencia constante; si no existe, el que más alto tenga el mínimo.
        return ranges.filter { it.upper == fps }.maxByOrNull { it.lower }
    }

    private fun pickEncoder(mime: String, size: Size, fps: Int): MediaCodecInfo? {
        val encoders = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.filter { info ->
            info.isEncoder && info.supportedTypes.any { it.equals(mime, ignoreCase = true) } &&
                info.getCapabilitiesForType(mime).videoCapabilities
                    ?.areSizeAndRateSupported(size.width, size.height, fps.toDouble()) == true
        }
        return encoders.firstOrNull { it.isHardwareAccelerated } ?: encoders.firstOrNull()
    }

    private fun configureEncoder(
        encoder: MediaCodec, caps: MediaCodecInfo.CodecCapabilities,
        mime: String, size: Size, fps: Int, bitrate: Int,
    ) {
        fun baseFormat() = MediaFormat.createVideoFormat(mime, size.width, size.height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setFloat(MediaFormat.KEY_I_FRAME_INTERVAL, 1f)
        }

        val full = baseFormat().apply {
            val vbr = MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR
            if (caps.encoderCapabilities?.isBitrateModeSupported(vbr) == true) {
                setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            }
            val profile = if (mime == MediaFormat.MIMETYPE_VIDEO_HEVC) MediaCodecInfo.CodecProfileLevel.HEVCProfileMain
            else MediaCodecInfo.CodecProfileLevel.AVCProfileHigh
            if (caps.profileLevels.any { it.profile == profile }) setInteger(MediaFormat.KEY_PROFILE, profile)
            setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
            setInteger(MediaFormat.KEY_PRIORITY, 0)
            setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
        }
        try {
            encoder.configure(full, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        } catch (e: Exception) {
            // Algunos encoders de fabricante rechazan claves opcionales; se reintenta con lo mínimo.
            Log.w(TAG, "Encoder rechazó el formato completo, reintento básico", e)
            encoder.reset()
            encoder.configure(baseFormat(), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
    }

    // --- cámara ---

    @SuppressLint("MissingPermission")
    private fun openCamera(id: String) {
        val latch = CountDownLatch(1)
        manager.openCamera(id, object : CameraDevice.StateCallback() {
            override fun onOpened(camera: CameraDevice) {
                device = camera
                latch.countDown()
            }

            override fun onDisconnected(camera: CameraDevice) {
                cameraError = "La cámara se desconectó (otra app la tomó)"
                camera.close()
                latch.countDown()
            }

            override fun onError(camera: CameraDevice, error: Int) {
                cameraError = "Error de cámara $error"
                camera.close()
                latch.countDown()
            }
        }, cameraHandler)
        if (!latch.await(5, TimeUnit.SECONDS)) throw IllegalStateException("La cámara no abrió a tiempo")
        cameraError?.let { throw IllegalStateException(it) }
    }

    private fun startCapture(chars: CameraCharacteristics, fpsRange: Range<Int>) {
        val camera = device!!
        val surface = inputSurface!!
        val latch = CountDownLatch(1)
        val config = SessionConfiguration(
            SessionConfiguration.SESSION_REGULAR,
            listOf(OutputConfiguration(surface)),
            cameraExecutor,
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    session = s
                    latch.countDown()
                }

                override fun onConfigureFailed(s: CameraCaptureSession) {
                    cameraError = "La cámara rechazó la configuración de captura"
                    latch.countDown()
                }
            },
        )
        camera.createCaptureSession(config)
        if (!latch.await(5, TimeUnit.SECONDS)) throw IllegalStateException("La sesión de captura no arrancó")
        cameraError?.let { throw IllegalStateException(it) }

        val builder = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
            addTarget(surface)
            set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
            set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)

            val afModes = chars[CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES] ?: IntArray(0)
            if (CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO in afModes) {
                set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
            }

            val eis = chars[CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES] ?: IntArray(0)
            val eisOn = request.stabilization && CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON in eis
            set(
                CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                if (eisOn) CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON
                else CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF,
            )
            val ois = chars[CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION] ?: IntArray(0)
            if (CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON in ois) {
                set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON)
            }
        }
        if (Build.VERSION.SDK_INT >= 30) {
            zoomRange = chars[CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE]
            zoomRange?.let { builder.set(CaptureRequest.CONTROL_ZOOM_RATIO, it.clamp(request.zoom)) }
        }
        captureBuilder = builder
        session!!.setRepeatingRequest(builder.build(), null, cameraHandler)
    }

    private companion object {
        const val TAG = "ObsCam"
    }
}
