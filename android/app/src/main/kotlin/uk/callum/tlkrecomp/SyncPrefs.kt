package uk.callum.tlkrecomp

import android.content.Context
import android.os.Build
import android.provider.Settings

/** The save sync settings: the server's URL, empty for no sync, and the name this device's saves go under. */
class SyncPrefs(private val context: Context) {
    private val prefs = context.getSharedPreferences("sync", Context.MODE_PRIVATE)

    var server: String
        get() = prefs.getString("server", "")!!.trim()
        set(v) = prefs.edit().putString("server", v.trim()).apply()

    var device: String
        get() = SaveSync.deviceName(prefs.getString("device", null)
            ?: Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME) ?: Build.MODEL)
        set(v) = prefs.edit().putString("device", v).apply()
}
