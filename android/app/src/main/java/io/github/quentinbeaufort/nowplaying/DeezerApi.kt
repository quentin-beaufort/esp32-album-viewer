package io.github.quentinbeaufort.nowplaying

import android.util.Log
import android.util.LruCache
import org.json.JSONException
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Deezer's public API, which needs no account: the media session only names the main artist,
 * while the track itself lists everyone on it.
 */
object DeezerApi {
    private const val TAG = "DeezerApi"
    private const val TIMEOUT_MS = 3000

    /** Deezer's media id for a track: "0." followed by the track id. */
    private val TRACK_MEDIA_ID = Regex("0\\.(\\d+)")

    private val cache = LruCache<String, List<String>>(32)

    fun trackId(mediaId: String?): String? = mediaId?.let { TRACK_MEDIA_ID.matchEntire(it)?.groupValues?.get(1) }

    /** Every artist credited on the track, main artist included; empty when the API fails. */
    fun contributors(trackId: String): List<String> {
        cache.get(trackId)?.let { return it }
        val names = try {
            val connection = URL("https://api.deezer.com/track/$trackId").openConnection() as HttpURLConnection
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            val body = try {
                connection.inputStream.bufferedReader().use { it.readText() }
            } finally {
                connection.disconnect()
            }
            // An unknown track comes back as 200 with an "error" object and no contributors.
            val list = JSONObject(body).optJSONArray("contributors")
            if (list == null) {
                Log.w(TAG, "track $trackId: no contributors")
                return emptyList()
            }
            (0 until list.length()).mapNotNull { i ->
                list.optJSONObject(i)?.optString("name")?.takeIf { it.isNotBlank() }
            }
        } catch (e: IOException) {
            Log.w(TAG, "track $trackId: ${e.message}")
            return emptyList()
        } catch (e: JSONException) {
            Log.w(TAG, "track $trackId: ${e.message}")
            return emptyList()
        }
        cache.put(trackId, names)
        return names
    }

    /** The contributors not already named in the session's artist ("A", "A, B" or "A & B"). */
    fun featured(contributors: List<String>, artist: String): List<String> {
        val shown = artist.split(", ", " & ").map { it.trim().lowercase() }.toSet()
        return contributors.filter { it.lowercase() !in shown }.distinct()
    }
}
