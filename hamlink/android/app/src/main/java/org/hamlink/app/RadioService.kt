// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.ServiceCompat

/** Keeps the engine alive in the background (needed for an unattended relay on a hilltop). */
class RadioService : Service() {
    companion object {
        @Volatile var engine: Engine? = null
        private const val CHANNEL = "hamlink"

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, RadioService::class.java))
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, RadioService::class.java))
        }

        /**
         * Settings (relay hops, carrier sense, PTT, preamble...) are read when the engine starts, so after Save the
         * running engine is replaced by a new one. Off the UI thread: stopping waits for the engine thread.
         */
        fun restartEngine(ctx: Context) {
            val old = engine ?: return
            val app = ctx.applicationContext
            Thread {
                old.stop()
                synchronized(RadioService::class.java) {
                    // the user may have pressed Stop meanwhile: then the service is gone and nothing restarts
                    if (engine === old) engine = Engine(app).also { it.start() }
                }
            }.start()
        }
    }

    private var wake: PowerManager.WakeLock? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        MessageStore.load(this) // once per process; the activity may not have run (service restarted by Android)
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "HamLink radio", NotificationManager.IMPORTANCE_LOW))
        val notif: Notification = Notification.Builder(this, CHANNEL)
            .setContentTitle("HamLink radio running")
            .setContentText("Listening and ready to transmit")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setOngoing(true)
            .build()
        // Bluetooth-TNC mode never touches the microphone, so it must not claim (or require permission for) that type.
        val st = Settings(this)
        var type = if (st.transport == "tnc") ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
                   else ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
        // Keep GPS (and so position beacons) alive with the screen off. Only claim the type when the permission is
        // held, otherwise Android 14+ refuses to start the service.
        if (st.positionMode == "gps" && GpsProvider.permitted(this)) type = type or ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION
        ServiceCompat.startForeground(this, 1, notif, type)

        if (wake == null) {
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "hamlink:engine").apply { acquire() }
        }
        Positions.apply(applicationContext)
        synchronized(RadioService::class.java) {
            if (engine == null) engine = Engine(applicationContext).also { it.start() }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        Positions.stop()
        val e = synchronized(RadioService::class.java) { engine.also { engine = null } }
        e?.stop()
        wake?.let { if (it.isHeld) it.release() }
        wake = null
        super.onDestroy()
    }
}
