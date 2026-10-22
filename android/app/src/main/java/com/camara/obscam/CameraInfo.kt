package com.camara.obscam

import android.content.Context
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraManager
import android.media.MediaCodec

/** Texto de diagnóstico: qué cámaras expone el teléfono a apps de terceros y hasta cuántos fps en 4K. */
object CameraInfo {

    fun describe(context: Context): String {
        val manager = context.getSystemService(CameraManager::class.java)
        return runCatching {
            manager.cameraIdList.joinToString("\n\n") { id -> describeCamera(manager, id) }
        }.getOrElse { "No se pudieron leer las cámaras: ${it.message}" }
    }

    private fun describeCamera(manager: CameraManager, id: String): String {
        val chars = manager.getCameraCharacteristics(id)
        val facing = when (chars[CameraCharacteristics.LENS_FACING]) {
            CameraCharacteristics.LENS_FACING_FRONT -> "frontal"
            CameraCharacteristics.LENS_FACING_BACK -> "trasera"
            else -> "externa"
        }
        val map = chars[CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP]
            ?: return "ID $id · $facing · sin salidas de video"
        val sizes = map.getOutputSizes(MediaCodec::class.java).orEmpty()
        val best = sizes.maxByOrNull { it.width.toLong() * it.height }
        val fourK = sizes.firstOrNull { it.width == 3840 && it.height == 2160 }
        val fourKFps = fourK?.let { 1_000_000_000.0 / map.getOutputMinFrameDuration(MediaCodec::class.java, it) }
        val ranges = chars[CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES].orEmpty()
        val focal = chars[CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS]?.firstOrNull()

        return buildString {
            append("ID $id · $facing")
            if (focal != null) append(" · ${"%.1f".format(focal)} mm")
            append("\n  máx. video: ${best?.width}×${best?.height}")
            append("\n  4K: ")
            append(if (fourK != null) "sí, hasta ${fourKFps?.toInt()} fps" else "no")
            append("\n  rangos fps: ${ranges.joinToString(" ") { "[${it.lower},${it.upper}]" }}")
        }
    }
}
