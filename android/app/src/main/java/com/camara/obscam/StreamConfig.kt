package com.camara.obscam

import org.json.JSONObject

/** Lo que OBS pide. El teléfono puede bajar resolución o fps si el hardware no llega; ver [ActualConfig]. */
data class StreamConfig(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrate: Int,
    val codec: Codec,
    val front: Boolean,
    val cameraId: String,
    val stabilization: Boolean,
    val zoom: Float,
) {
    enum class Codec { H264, HEVC }

    companion object {
        fun fromJson(json: JSONObject) = StreamConfig(
            width = json.optInt("width", 3840),
            height = json.optInt("height", 2160),
            fps = json.optInt("fps", 30),
            bitrate = json.optInt("bitrate", 50_000_000),
            codec = if (json.optString("codec", "hevc") == "h264") Codec.H264 else Codec.HEVC,
            front = json.optString("facing", "back") == "front",
            cameraId = json.optString("camera_id", ""),
            stabilization = json.optBoolean("stabilization", true),
            zoom = json.optDouble("zoom", 1.0).toFloat(),
        )
    }
}

/** Lo que realmente se está transmitiendo. */
data class ActualConfig(
    val width: Int,
    val height: Int,
    val fps: Int,
    val bitrate: Int,
    val codec: StreamConfig.Codec,
    val cameraId: String,
    val encoderName: String,
    val maxZoom: Float,
) {
    val codecLabel get() = if (codec == StreamConfig.Codec.HEVC) "HEVC" else "H.264"
}
