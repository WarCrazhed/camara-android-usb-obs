package com.camara.obscam

import android.content.Context
import android.util.Log
import org.json.JSONObject
import java.io.BufferedOutputStream
import java.io.BufferedReader
import java.io.DataOutputStream
import java.io.InputStreamReader
import java.net.Socket

/**
 * Una conexión de OBS. Protocolo (big endian):
 *
 *  OBS → teléfono: una línea JSON con la configuración pedida (ver [StreamConfig]);
 *    después puede mandar más líneas con cambios en vivo, p. ej. {"zoom":2.5}.
 *  teléfono → OBS: cabecera de 16 bytes
 *      "OCAM" | version u8 | codec u8 (0=H.264, 1=HEVC, 0xFF=error) | fps u16 | ancho u16 | alto u16 | reservado u32
 *    si codec = 0xFF le siguen: largo u16 + mensaje UTF-8, y se cierra.
 *      (reservado = zoom máximo × 100)
 *    después, paquetes: pts_us i64 | flags u32 (bit0 config, bit1 keyframe, bits2-3 giro horario/90 para verlo derecho) | tamaño u32 | datos Annex-B
 */
class StreamSession(
    private val context: Context,
    private val socket: Socket,
    private val onStatus: (String) -> Unit,
) {
    private val thread = Thread(::run, "obscam-session")
    @Volatile private var stopped = false

    fun start() = thread.start()

    fun stop() {
        stopped = true
        runCatching { socket.close() }
        thread.join(3000)
    }

    private fun run() {
        var streamer: CameraStreamer? = null
        try {
            socket.tcpNoDelay = true
            socket.sendBufferSize = 4 shl 20
            val reader = BufferedReader(InputStreamReader(socket.getInputStream()))
            val line = reader.readLine() ?: return
            val request = StreamConfig.fromJson(JSONObject(line))
            val out = DataOutputStream(BufferedOutputStream(socket.getOutputStream(), 1 shl 16))

            onStatus("Abriendo cámara…")
            streamer = CameraStreamer(context, request)
            val actual = try {
                streamer.start()
            } catch (e: Exception) {
                Log.e(TAG, "No se pudo iniciar la cámara", e)
                writeError(out, e.message ?: e.javaClass.simpleName)
                onStatus("Error: ${e.message}")
                return
            }

            writeHeader(out, actual)
            val active = streamer
            Thread({ readCommands(reader, active) }, "obscam-commands").apply { isDaemon = true; start() }
            onStatus(
                "Transmitiendo ${actual.width}×${actual.height} @ ${actual.fps} fps · " +
                    "${actual.codecLabel} · ${actual.bitrate / 1_000_000} Mbps · cámara ${actual.cameraId}"
            )
            streamer.drain(out) { stopped }
        } catch (e: Exception) {
            if (!stopped) Log.i(TAG, "Sesión terminada: ${e.message}")
        } finally {
            streamer?.release()
            runCatching { socket.close() }
            if (StreamService.running) onStatus("Esperando a OBS…")
        }
    }

    private fun readCommands(reader: BufferedReader, streamer: CameraStreamer) {
        runCatching {
            while (!stopped) {
                val line = reader.readLine() ?: break
                val cmd = JSONObject(line)
                if (cmd.has("zoom")) streamer.setZoom(cmd.getDouble("zoom").toFloat())
            }
        }
    }

    private fun writeHeader(out: DataOutputStream, c: ActualConfig) {
        out.writeBytes("OCAM")
        out.writeByte(PROTOCOL_VERSION)
        out.writeByte(if (c.codec == StreamConfig.Codec.HEVC) 1 else 0)
        out.writeShort(c.fps)
        out.writeShort(c.width)
        out.writeShort(c.height)
        out.writeInt((c.maxZoom * 100).toInt())
        out.flush()
    }

    private fun writeError(out: DataOutputStream, message: String) {
        val bytes = message.take(500).toByteArray(Charsets.UTF_8)
        out.writeBytes("OCAM")
        out.writeByte(PROTOCOL_VERSION)
        out.writeByte(0xFF)
        out.writeShort(0)
        out.writeShort(0)
        out.writeShort(0)
        out.writeInt(0)
        out.writeShort(bytes.size)
        out.write(bytes)
        out.flush()
    }

    private companion object {
        const val TAG = "ObsCam"
        const val PROTOCOL_VERSION = 1
    }
}
