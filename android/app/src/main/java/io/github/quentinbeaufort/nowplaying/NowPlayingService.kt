package io.github.quentinbeaufort.nowplaying

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothA2dp
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper
import android.service.notification.NotificationListenerService
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import androidx.core.net.toUri
import java.net.URL
import java.security.MessageDigest
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Follows Deezer's media session while the Bluetooth receiver is connected and pushes the screen
 * to the ESP32. Notification access is what lets the app read other apps' media sessions.
 */
class NowPlayingService : NotificationListenerService() {
    private val handler = Handler(Looper.getMainLooper())
    private lateinit var prefs: Prefs
    private lateinit var sender: Sender
    private lateinit var renderer: ExecutorService
    private lateinit var sessions: MediaSessionManager

    private var listening = false
    private var receiverConnected = false
    private var attached = false
    private var controller: MediaController? = null
    private var lastSignature: String? = null
    private var lastPlaying: Boolean? = null

    override fun onListenerConnected() {
        prefs = Prefs(this)
        sender = Sender(EspClient.get(this))
        renderer = Executors.newSingleThreadExecutor()
        sessions = getSystemService(MediaSessionManager::class.java)
        listening = true

        ContextCompat.registerReceiver(
            this, a2dpReceiver, IntentFilter(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        prefs.raw.registerOnSharedPreferenceChangeListener(prefsListener)
        queryReceiverState()
        update()
    }

    override fun onListenerDisconnected() {
        if (!listening) return
        listening = false
        unregisterReceiver(a2dpReceiver)
        prefs.raw.unregisterOnSharedPreferenceChangeListener(prefsListener)
        detach()
        renderer.shutdown()
        sender.shutdown()
    }

    private fun isActive() = listening && (prefs.deviceMac.isEmpty() || receiverConnected)

    private fun update() {
        val active = isActive()
        if (active && !attached) {
            attached = true
            val component = ComponentName(this, NowPlayingService::class.java)
            sessions.addOnActiveSessionsChangedListener(sessionsListener, component, handler)
            onSessionsChanged(sessions.getActiveSessions(component))
        } else if (!active && attached) {
            detach()
            sender.sendOff()
        }
    }

    private fun detach() {
        if (!attached) return
        attached = false
        sessions.removeOnActiveSessionsChangedListener(sessionsListener)
        follow(null)
    }

    // --- Bluetooth receiver ---

    private val a2dpReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = IntentCompat.getParcelableExtra(intent, BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
            if (device == null || !device.address.equals(prefs.deviceMac, ignoreCase = true)) return
            val state = intent.getIntExtra(BluetoothProfile.EXTRA_STATE, BluetoothProfile.STATE_DISCONNECTED)
            receiverConnected = state == BluetoothProfile.STATE_CONNECTED
            Log.i(TAG, "receiver connected: $receiverConnected")
            update()
        }
    }

    private val prefsListener = SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
        if (key == Prefs.KEY_DEVICE_MAC) {
            receiverConnected = false
            queryReceiverState()
            update()
        }
    }

    /** Initial state: is the chosen receiver already connected? */
    @SuppressLint("MissingPermission") // checked just below
    private fun queryReceiverState() {
        val mac = prefs.deviceMac
        if (mac.isEmpty()) return
        if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) return
        val adapter = getSystemService(BluetoothManager::class.java)?.adapter ?: return
        adapter.getProfileProxy(this, object : BluetoothProfile.ServiceListener {
            override fun onServiceConnected(profile: Int, proxy: BluetoothProfile) {
                receiverConnected = proxy.connectedDevices.any { it.address.equals(mac, ignoreCase = true) }
                adapter.closeProfileProxy(profile, proxy)
                handler.post { update() }
            }

            override fun onServiceDisconnected(profile: Int) = Unit
        }, BluetoothProfile.A2DP)
    }

    // --- Media sessions ---

    private val sessionsListener = MediaSessionManager.OnActiveSessionsChangedListener { controllers ->
        onSessionsChanged(controllers.orEmpty())
    }

    private fun onSessionsChanged(controllers: List<MediaController>) {
        val deezer = controllers.firstOrNull { it.packageName == PLAYER_PACKAGE }
        if (deezer?.sessionToken != controller?.sessionToken) follow(deezer)
    }

    private fun follow(next: MediaController?) {
        controller?.unregisterCallback(controllerCallback)
        handler.removeCallbacks(renderRunnable)
        controller = next
        lastSignature = null
        lastPlaying = null
        if (next != null) {
            next.registerCallback(controllerCallback, handler)
            scheduleRender()
        }
    }

    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) = scheduleRender()

        override fun onPlaybackStateChanged(state: PlaybackState?) {
            if (lastSignature != null) sendPlaying()
        }

        override fun onSessionDestroyed() = follow(null)
    }

    private fun sendPlaying() {
        val playing = controller?.playbackState?.state == PlaybackState.STATE_PLAYING
        if (playing != lastPlaying) {
            lastPlaying = playing
            sender.sendState(playing)
        }
    }

    /** Metadata often arrives in several updates: wait for it to settle. */
    private fun scheduleRender() {
        handler.removeCallbacks(renderRunnable)
        handler.postDelayed(renderRunnable, DEBOUNCE_MS)
    }

    private val renderRunnable = Runnable {
        val metadata = controller?.metadata ?: return@Runnable
        val title = metadata.getString(MediaMetadata.METADATA_KEY_TITLE).orEmpty()
        val artist = metadata.getString(MediaMetadata.METADATA_KEY_ARTIST).orEmpty()
        val album = metadata.getString(MediaMetadata.METADATA_KEY_ALBUM).orEmpty()
        if (title.isEmpty() && artist.isEmpty()) return@Runnable
        val trackId = trackId(metadata.getString(MediaMetadata.METADATA_KEY_MEDIA_ID), title, artist, album)
        val bitmap = ART_BITMAP_KEYS.firstNotNullOfOrNull { metadata.getBitmap(it) }
        val uri = if (bitmap == null) ART_URI_KEYS.firstNotNullOfOrNull { metadata.getString(it) } else null

        // Same track, same artwork: nothing to resend. A late or better artwork changes the signature.
        val art = bitmap?.let { "${it.width}x${it.height}" } ?: uri ?: "none"
        val signature = "$trackId|$title|$artist|$art"
        if (signature == lastSignature) return@Runnable
        lastSignature = signature
        lastPlaying = null

        renderer.execute {
            val cover = bitmap ?: uri?.let { loadArt(it) }
            val jpeg = FrameRenderer.toJpeg(FrameRenderer.render(title, artist, cover))
            sender.sendFrame(Sender.Frame(trackId, jpeg))
            handler.post { sendPlaying() }
        }
    }

    private fun loadArt(uri: String): Bitmap? = try {
        val parsed = uri.toUri()
        val stream = when (parsed.scheme) {
            "content", "android.resource", "file" -> contentResolver.openInputStream(parsed)
            "https" -> URL(uri).openStream()
            else -> null
        }
        stream?.use { BitmapFactory.decodeStream(it) }
    } catch (e: Exception) {
        Log.w(TAG, "cannot load artwork $uri", e)
        null
    }

    companion object {
        private const val TAG = "NowPlayingService"
        private const val PLAYER_PACKAGE = "deezer.android.app"
        private const val DEBOUNCE_MS = 300L
        private const val TRACK_ID_MAX = 64

        private val ART_BITMAP_KEYS = listOf(
            MediaMetadata.METADATA_KEY_ALBUM_ART,
            MediaMetadata.METADATA_KEY_ART,
            MediaMetadata.METADATA_KEY_DISPLAY_ICON,
        )
        private val ART_URI_KEYS = listOf(
            MediaMetadata.METADATA_KEY_ALBUM_ART_URI,
            MediaMetadata.METADATA_KEY_ART_URI,
        )

        /** The media id when it fits in an HTTP header, otherwise a hash of the track. */
        fun trackId(mediaId: String?, title: String, artist: String, album: String): String {
            if (!mediaId.isNullOrEmpty() && mediaId.length <= TRACK_ID_MAX && mediaId.all { it in ' '..'~' }) {
                return mediaId
            }
            val digest = MessageDigest.getInstance("SHA-1").digest("$title|$artist|$album".toByteArray())
            return digest.joinToString("") { "%02x".format(it) }
        }
    }
}
