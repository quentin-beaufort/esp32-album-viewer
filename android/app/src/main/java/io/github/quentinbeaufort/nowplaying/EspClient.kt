package io.github.quentinbeaufort.nowplaying

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.os.Build
import android.util.Log
import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Socket
import java.net.UnknownHostException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Talks to the screen (docs/protocol.md).
 *
 * Requests go through a minimal HTTP/1.1 client on a plain socket, so that the address can come
 * from our own resolution (system resolver, then mDNS service discovery, then the fallback IP from
 * the settings) while the app keeps cleartext HTTP disabled everywhere else: the network security
 * policy only applies to the platform HTTP stacks. See android/README.md.
 */
class EspClient private constructor(context: Context) {
    private val appContext = context.applicationContext
    private val prefs = Prefs(appContext)

    @Volatile
    private var cached: InetAddress? = null

    class Response(val code: Int, val body: String)

    fun postFrame(trackId: String, jpeg: ByteArray): Response =
        post("/frame", mapOf("Content-Type" to "image/jpeg", "X-Track-Id" to trackId), jpeg)

    fun postState(trackId: String, playing: Boolean): Response {
        val json = "{\"track_id\":\"${escape(trackId)}\",\"playing\":$playing}"
        return post("/state", mapOf("Content-Type" to "application/json"), json.toByteArray())
    }

    fun postOff(): Response = post("/off", emptyMap(), ByteArray(0))

    /** Sends a request, resolving the screen again once if the cached address stopped answering. */
    private fun post(path: String, headers: Map<String, String>, body: ByteArray): Response {
        val all = headers + ("X-Token" to prefs.token)
        val known = cached
        if (known != null) {
            try {
                return request(known, path, all, body)
            } catch (e: IOException) {
                Log.i(TAG, "$known did not answer, resolving again", e)
                cached = null
            }
        }
        val address = resolve()
        val response = request(address, path, all, body)
        cached = address
        return response
    }

    private fun resolve(): InetAddress =
        systemLookup() ?: nsdLookup() ?: fallbackLookup() ?: throw UnknownHostException(HOST)

    private fun systemLookup(): InetAddress? = try {
        InetAddress.getAllByName(HOST).firstOrNull { it is Inet4Address }
    } catch (e: IOException) {
        null
    }

    private fun fallbackLookup(): InetAddress? {
        val ip = prefs.fallbackIp.trim()
        if (ip.isEmpty()) return null
        return try {
            InetAddress.getByName(ip) // a numeric address is parsed, not looked up
        } catch (e: IOException) {
            null
        }
    }

    /** Browses for the _nowplaying._tcp service announced by the screen and resolves it. */
    private fun nsdLookup(): InetAddress? {
        val nsd = appContext.getSystemService(NsdManager::class.java) ?: return null
        val found = CountDownLatch(1)
        var service: NsdServiceInfo? = null
        val discovery = object : NsdManager.DiscoveryListener {
            override fun onServiceFound(info: NsdServiceInfo) {
                service = info
                found.countDown()
            }

            override fun onStartDiscoveryFailed(type: String, error: Int) = found.countDown()
            override fun onStopDiscoveryFailed(type: String, error: Int) = Unit
            override fun onDiscoveryStarted(type: String) = Unit
            override fun onDiscoveryStopped(type: String) = Unit
            override fun onServiceLost(info: NsdServiceInfo) = Unit
        }
        nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discovery)
        try {
            found.await(NSD_TIMEOUT_S, TimeUnit.SECONDS)
        } finally {
            runCatching { nsd.stopServiceDiscovery(discovery) }
        }
        return service?.let { resolveService(nsd, it) }
    }

    private fun resolveService(nsd: NsdManager, info: NsdServiceInfo): InetAddress? {
        val done = CountDownLatch(1)
        var address: InetAddress? = null
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            val executor = Executors.newSingleThreadExecutor()
            val callback = object : NsdManager.ServiceInfoCallback {
                override fun onServiceUpdated(updated: NsdServiceInfo) {
                    address = updated.hostAddresses.firstOrNull { it is Inet4Address }
                    if (address != null) done.countDown()
                }

                override fun onServiceInfoCallbackRegistrationFailed(error: Int) = done.countDown()
                override fun onServiceLost() = Unit
                override fun onServiceInfoCallbackUnregistered() = Unit
            }
            nsd.registerServiceInfoCallback(info, executor, callback)
            try {
                done.await(NSD_TIMEOUT_S, TimeUnit.SECONDS)
            } finally {
                runCatching { nsd.unregisterServiceInfoCallback(callback) }
                executor.shutdown()
            }
        } else {
            @Suppress("DEPRECATION")
            nsd.resolveService(info, object : NsdManager.ResolveListener {
                override fun onServiceResolved(resolved: NsdServiceInfo) {
                    @Suppress("DEPRECATION")
                    address = resolved.host
                    done.countDown()
                }

                override fun onResolveFailed(failed: NsdServiceInfo, error: Int) = done.countDown()
            })
            done.await(NSD_TIMEOUT_S, TimeUnit.SECONDS)
        }
        return address
    }

    private fun request(address: InetAddress, path: String, headers: Map<String, String>, body: ByteArray): Response {
        Socket().use { socket ->
            socket.connect(InetSocketAddress(address, PORT), CONNECT_TIMEOUT_MS)
            socket.soTimeout = READ_TIMEOUT_MS
            val head = buildString {
                append("POST ").append(path).append(" HTTP/1.1\r\n")
                append("Host: ").append(HOST).append("\r\n")
                append("Connection: close\r\n")
                append("Content-Length: ").append(body.size).append("\r\n")
                headers.forEach { (name, value) -> append(name).append(": ").append(value).append("\r\n") }
                append("\r\n")
            }
            val output = socket.getOutputStream()
            try {
                output.write(head.toByteArray(Charsets.ISO_8859_1))
                output.write(body)
                output.flush()
            } catch (e: IOException) {
                // The screen answers a bad token before reading the body and may close early:
                // its response is still worth reading.
                Log.d(TAG, "write interrupted", e)
            }
            return readResponse(BufferedInputStream(socket.getInputStream()))
        }
    }

    private fun readResponse(input: InputStream): Response {
        val status = readLine(input) ?: throw IOException("no response")
        val code = status.split(' ').getOrNull(1)?.toIntOrNull() ?: throw IOException("bad status line: $status")
        var length = -1
        while (true) {
            val line = readLine(input) ?: break
            if (line.isEmpty()) break
            val colon = line.indexOf(':')
            if (colon > 0 && line.substring(0, colon).trim().equals("Content-Length", ignoreCase = true)) {
                length = line.substring(colon + 1).trim().toIntOrNull() ?: -1
            }
        }
        val body = ByteArrayOutputStream()
        val buffer = ByteArray(512)
        var remaining = if (length >= 0) minOf(length, MAX_BODY) else MAX_BODY
        while (remaining > 0) {
            val n = input.read(buffer, 0, minOf(buffer.size, remaining))
            if (n < 0) break
            body.write(buffer, 0, n)
            remaining -= n
        }
        return Response(code, body.toString(Charsets.UTF_8.name()))
    }

    private fun readLine(input: InputStream): String? {
        val line = StringBuilder()
        while (true) {
            val c = input.read()
            if (c < 0) return if (line.isEmpty()) null else line.toString()
            if (c == '\n'.code) return line.toString().trimEnd('\r')
            line.append(c.toChar())
        }
    }

    private fun escape(s: String) = s.replace("\\", "\\\\").replace("\"", "\\\"")

    companion object {
        private const val TAG = "EspClient"
        const val HOST = "nowplaying.local"
        private const val SERVICE_TYPE = "_nowplaying._tcp"
        private const val PORT = 80
        private const val CONNECT_TIMEOUT_MS = 3000
        private const val READ_TIMEOUT_MS = 10000
        private const val NSD_TIMEOUT_S = 5L
        private const val MAX_BODY = 4096

        @Volatile
        private var instance: EspClient? = null

        fun get(context: Context): EspClient =
            instance ?: synchronized(this) { instance ?: EspClient(context).also { instance = it } }
    }
}
