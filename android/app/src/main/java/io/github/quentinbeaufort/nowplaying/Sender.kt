package io.github.quentinbeaufort.nowplaying

import android.util.Log
import java.io.IOException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Serialized queue towards the screen. Only the latest request of each kind is kept: a frame
 * waiting to be sent is replaced by a newer one, and so is the play/pause state.
 */
class Sender(private val client: EspClient) {
    class Frame(val trackId: String, val jpeg: ByteArray)

    private val executor = Executors.newSingleThreadScheduledExecutor()
    private val lock = Any()
    private var pendingOff = false
    private var pendingFrame: Frame? = null
    private var pendingPlaying: Boolean? = null
    private var retries = 0

    /** Frame currently on the screen, resent when the screen answers 409. Worker thread only. */
    private var shown: Frame? = null

    fun sendFrame(frame: Frame) = enqueue { pendingFrame = frame; retries = 0 }

    fun sendState(playing: Boolean) = enqueue { pendingPlaying = playing }

    fun sendOff() = enqueue {
        pendingOff = true
        pendingFrame = null
        pendingPlaying = null
    }

    fun shutdown() = executor.shutdown()

    private fun enqueue(update: () -> Unit) {
        synchronized(lock) { update() }
        executor.execute(::drain)
    }

    private fun drain() {
        while (true) {
            val off: Boolean
            val frame: Frame?
            val playing: Boolean?
            synchronized(lock) {
                off = pendingOff
                frame = pendingFrame
                playing = pendingPlaying
                pendingOff = false
                pendingFrame = null
                pendingPlaying = null
            }
            if (!off && frame == null && playing == null) return
            try {
                if (off) {
                    shown = null
                    log("/off", client.postOff())
                }
                if (frame != null && !sendFrameNow(frame)) {
                    retryLater(frame, playing)
                    return
                }
                if (playing != null) sendStateNow(playing)
            } catch (e: IOException) {
                Log.w(TAG, "screen unreachable", e)
                if (frame != null) retryLater(frame, playing)
                return
            }
        }
    }

    private fun sendFrameNow(frame: Frame): Boolean {
        val response = client.postFrame(frame.trackId, frame.jpeg)
        log("/frame", response)
        if (response.code == 204) {
            shown = frame
            return true
        }
        shown = null
        return false
    }

    private fun sendStateNow(playing: Boolean) {
        val frame = shown ?: return
        val response = client.postState(frame.trackId, playing)
        log("/state", response)
        if (response.code == 409 && sendFrameNow(frame)) {
            // The screen lost the image (restart): show it again, then the state.
            log("/state", client.postState(frame.trackId, playing))
        }
    }

    /** Keeps a frame that could not be sent, unless a newer one arrived meanwhile. */
    private fun retryLater(frame: Frame, playing: Boolean?) {
        synchronized(lock) {
            if (pendingFrame != null || pendingOff || retries >= MAX_RETRIES) return
            retries++
            pendingFrame = frame
            if (pendingPlaying == null) pendingPlaying = playing
        }
        executor.schedule(::drain, RETRY_DELAY_S, TimeUnit.SECONDS)
    }

    private fun log(route: String, response: EspClient.Response) {
        Log.i(TAG, "$route -> ${response.code} ${response.body}")
    }

    companion object {
        private const val TAG = "Sender"
        private const val MAX_RETRIES = 3
        private const val RETRY_DELAY_S = 5L
    }
}
