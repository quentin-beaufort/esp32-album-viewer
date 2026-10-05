package io.github.quentinbeaufort.nowplaying

import android.content.Context
import android.content.SharedPreferences
import androidx.core.content.edit

/** Settings shared by the settings screen and the service. */
class Prefs(context: Context) {
    val raw: SharedPreferences = context.getSharedPreferences("settings", Context.MODE_PRIVATE)

    /** MAC address of the Bluetooth receiver, empty when the app should always be active. */
    var deviceMac: String
        get() = raw.getString(KEY_DEVICE_MAC, "")!!
        set(value) = raw.edit { putString(KEY_DEVICE_MAC, value) }

    var token: String
        get() = raw.getString(KEY_TOKEN, "")!!
        set(value) = raw.edit { putString(KEY_TOKEN, value) }

    var fallbackIp: String
        get() = raw.getString(KEY_FALLBACK_IP, "")!!
        set(value) = raw.edit { putString(KEY_FALLBACK_IP, value) }

    companion object {
        const val KEY_DEVICE_MAC = "device_mac"
        const val KEY_TOKEN = "token"
        const val KEY_FALLBACK_IP = "fallback_ip"
    }
}
