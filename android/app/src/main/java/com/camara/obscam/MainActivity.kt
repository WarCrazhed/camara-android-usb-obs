package com.camara.obscam

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.view.WindowManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.TextView
import android.widget.Toast

class MainActivity : Activity() {

    private lateinit var statusView: TextView
    private val handler = Handler(Looper.getMainLooper())
    private val refresher = object : Runnable {
        override fun run() {
            statusView.text = if (StreamService.running) StreamService.status else "Detenido"
            handler.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // OBS abre la app por adb aunque el teléfono esté bloqueado.
        setShowWhenLocked(true)
        setTurnScreenOn(true)

        statusView = findViewById(R.id.status)
        findViewById<TextView>(R.id.cameras).text = CameraInfo.describe(this)
        findViewById<Button>(R.id.start).setOnClickListener { startStreaming() }
        findViewById<Button>(R.id.stop).setOnClickListener {
            stopService(Intent(this, StreamService::class.java))
        }
        findViewById<Button>(R.id.battery).setOnClickListener { requestBatteryExemption() }
        findViewById<CheckBox>(R.id.keep_screen).setOnCheckedChangeListener { _, checked ->
            if (checked) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }

        startStreaming()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        startStreaming()
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresher)
    }

    override fun onPause() {
        handler.removeCallbacks(refresher)
        super.onPause()
    }

    private fun startStreaming() {
        val missing = buildList {
            add(Manifest.permission.CAMERA)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }

        if (Manifest.permission.CAMERA in missing) {
            requestPermissions(missing.toTypedArray(), REQ_PERMS)
            return
        }
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), REQ_PERMS)
        startForegroundService(Intent(this, StreamService::class.java))
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode != REQ_PERMS) return
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startForegroundService(Intent(this, StreamService::class.java))
        } else {
            Toast.makeText(this, "Sin permiso de cámara no se puede transmitir", Toast.LENGTH_LONG).show()
        }
    }

    @SuppressLint("BatteryLife")
    private fun requestBatteryExemption() {
        val pm = getSystemService(PowerManager::class.java)
        if (pm.isIgnoringBatteryOptimizations(packageName)) {
            Toast.makeText(this, "Ya está exenta de optimización de batería", Toast.LENGTH_SHORT).show()
            return
        }
        startActivity(
            Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))
        )
    }

    private companion object {
        const val REQ_PERMS = 1
    }
}
