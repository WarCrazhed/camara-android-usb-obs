package com.camara.obscam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import android.view.OrientationEventListener
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import kotlin.math.abs
import kotlin.math.min

/**
 * Servicio en primer plano (tipo cámara) que escucha en 127.0.0.1:[PORT].
 * El plugin de OBS llega aquí a través de `adb forward`; solo se atiende un cliente a la vez.
 */
class StreamService : Service() {

    companion object {
        const val PORT = 27183
        private const val TAG = "ObsCam"
        private const val CHANNEL = "stream"
        private const val NOTIFICATION_ID = 1

        @Volatile var running = false
            private set
        @Volatile var status = "Detenido"

        /** Giro físico del teléfono en grados (0, 90, 180, 270), como lo da OrientationEventListener. */
        @Volatile var deviceOrientation = 0
            private set
    }

    private var server: ServerSocket? = null
    private var acceptThread: Thread? = null
    private var session: StreamSession? = null
    private var wakeLock: PowerManager.WakeLock? = null
    private var orientationListener: OrientationEventListener? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (running) return START_NOT_STICKY

        startForeground(NOTIFICATION_ID, buildNotification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA)
        running = true
        status = "Esperando a OBS…"
        wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "ObsCam:stream")
            .apply { acquire() }

        orientationListener = object : OrientationEventListener(this) {
            override fun onOrientationChanged(degrees: Int) {
                if (degrees == ORIENTATION_UNKNOWN) return // teléfono acostado: se queda el último
                // Histéresis: solo cambia si está claramente (±30°) en otro cuadrante.
                val diff = abs(degrees - deviceOrientation).let { min(it, 360 - it) }
                if (diff > 60) deviceOrientation = (degrees + 45) / 90 % 4 * 90
            }
        }.apply { if (canDetectOrientation()) enable() }

        acceptThread = Thread(::acceptLoop, "obscam-accept").apply { start() }
        // No reiniciar solo: un servicio de cámara iniciado desde segundo plano no tiene acceso a la cámara.
        return START_NOT_STICKY
    }

    private fun acceptLoop() {
        try {
            val socket = ServerSocket().apply {
                reuseAddress = true
                bind(InetSocketAddress(InetAddress.getLoopbackAddress(), PORT))
            }
            server = socket
            while (running) {
                val client = socket.accept()
                Log.i(TAG, "Cliente conectado")
                session?.stop()
                session = StreamSession(this, client) { status = it }.also { it.start() }
            }
        } catch (e: Exception) {
            if (running) {
                Log.e(TAG, "Servidor detenido", e)
                status = "Error del servidor: ${e.message}"
            }
        }
    }

    override fun onDestroy() {
        running = false
        status = "Detenido"
        orientationListener?.disable()
        runCatching { server?.close() }
        session?.stop()
        session = null
        wakeLock?.takeIf { it.isHeld }?.release()
        super.onDestroy()
    }

    private fun buildNotification(): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL, "Transmisión a OBS", NotificationManager.IMPORTANCE_LOW)
        )
        val open = PendingIntent.getActivity(
            this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE
        )
        return Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentTitle("OBS Cam activa")
            .setContentText("Cámara disponible para OBS por USB")
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }
}
