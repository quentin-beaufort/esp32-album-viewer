package io.github.quentinbeaufort.nowplaying

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothManager
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Shader
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.ImageView
import android.widget.Spinner
import android.widget.TextView
import androidx.core.app.NotificationManagerCompat
import androidx.core.graphics.createBitmap
import androidx.core.widget.doAfterTextChanged
import java.io.IOException
import kotlin.concurrent.thread

class SettingsActivity : Activity() {
    private lateinit var prefs: Prefs
    private lateinit var deviceSpinner: Spinner
    private lateinit var devicePermission: Button
    private lateinit var accessStatus: TextView
    private lateinit var testResult: TextView
    private lateinit var sendTest: Button

    /** Spinner entries: label and MAC address, the first one being "none". */
    private var devices: List<Pair<String, String>> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        prefs = Prefs(this)
        deviceSpinner = findViewById(R.id.device)
        devicePermission = findViewById(R.id.device_permission)
        accessStatus = findViewById(R.id.access_status)
        testResult = findViewById(R.id.test_result)
        sendTest = findViewById(R.id.send_test)

        findViewById<EditText>(R.id.token).apply {
            setText(prefs.token)
            doAfterTextChanged { prefs.token = it.toString().trim() }
        }
        findViewById<EditText>(R.id.fallback_ip).apply {
            setText(prefs.fallbackIp)
            doAfterTextChanged { prefs.fallbackIp = it.toString().trim() }
        }
        devicePermission.setOnClickListener {
            requestPermissions(arrayOf(Manifest.permission.BLUETOOTH_CONNECT), REQUEST_BLUETOOTH)
        }
        deviceSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                val mac = devices.getOrNull(position)?.second ?: return
                if (mac != prefs.deviceMac) prefs.deviceMac = mac
            }

            override fun onNothingSelected(parent: AdapterView<*>) = Unit
        }
        findViewById<Button>(R.id.notification_access).setOnClickListener { openNotificationAccess() }
        findViewById<ImageView>(R.id.preview).setImageBitmap(testFrame())
        sendTest.setOnClickListener { sendTestFrame() }
    }

    override fun onResume() {
        super.onResume()
        val granted = NotificationManagerCompat.getEnabledListenerPackages(this).contains(packageName)
        accessStatus.setText(if (granted) R.string.status_access_ok else R.string.status_access_missing)
        loadDevices()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_BLUETOOTH) loadDevices()
    }

    @SuppressLint("MissingPermission") // checked at the top
    private fun loadDevices() {
        val granted = checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
        devicePermission.visibility = if (granted) View.GONE else View.VISIBLE
        val bonded = if (granted) {
            getSystemService(BluetoothManager::class.java)?.adapter?.bondedDevices.orEmpty()
                .map { "${it.name ?: it.address} (${it.address})" to it.address }
                .sortedBy { it.first.lowercase() }
        } else {
            emptyList()
        }
        devices = listOf(getString(R.string.device_none) to "") + bonded
        // Keep a saved receiver visible even when its name cannot be read.
        val saved = prefs.deviceMac
        if (saved.isNotEmpty() && devices.none { it.second == saved }) devices = devices + (saved to saved)
        deviceSpinner.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, devices.map { it.first })
        deviceSpinner.setSelection(devices.indexOfFirst { it.second == saved }.coerceAtLeast(0))
    }

    private fun openNotificationAccess() {
        val component = ComponentName(this, NowPlayingService::class.java).flattenToString()
        val detail = Intent(Settings.ACTION_NOTIFICATION_LISTENER_DETAIL_SETTINGS)
            .putExtra(Settings.EXTRA_NOTIFICATION_LISTENER_COMPONENT_NAME, component)
        try {
            startActivity(detail)
        } catch (e: android.content.ActivityNotFoundException) {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
    }

    private fun testFrame(): Bitmap =
        FrameRenderer.render(
            getString(R.string.test_title),
            getString(R.string.test_artist),
            listOf(getString(R.string.test_featured)),
            testCover(),
        )

    private fun sendTestFrame() {
        sendTest.isEnabled = false
        testResult.setText(R.string.sending)
        val client = EspClient.get(this)
        thread {
            val result = try {
                val jpeg = FrameRenderer.toJpeg(testFrame())
                val frame = client.postFrame(TEST_TRACK, jpeg)
                if (frame.code == 204) {
                    val state = client.postState(TEST_TRACK, true)
                    "/frame : 204, /state : ${state.code} ${state.body}"
                } else {
                    "/frame : ${frame.code} ${frame.body}"
                }
            } catch (e: IOException) {
                "Écran injoignable : ${e.message}"
            }
            runOnUiThread {
                testResult.text = result
                sendTest.isEnabled = true
            }
        }
    }

    /** A colorful square standing in for a cover. */
    private fun testCover(): Bitmap {
        val size = 480f
        val bitmap = createBitmap(480, 480)
        val paint = Paint().apply {
            shader = LinearGradient(0f, 0f, size, size, Color.rgb(230, 80, 60), Color.rgb(60, 70, 200), Shader.TileMode.CLAMP)
        }
        Canvas(bitmap).drawRect(0f, 0f, size, size, paint)
        return bitmap
    }

    companion object {
        private const val REQUEST_BLUETOOTH = 1
        private const val TEST_TRACK = "test"
    }
}
