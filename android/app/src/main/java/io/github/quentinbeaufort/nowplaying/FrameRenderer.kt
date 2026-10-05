package io.github.quentinbeaufort.nowplaying

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Rect
import android.graphics.Typeface
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import androidx.core.graphics.ColorUtils
import androidx.core.graphics.createBitmap
import androidx.core.graphics.withTranslation
import androidx.palette.graphics.Palette
import java.io.ByteArrayOutputStream
import kotlin.math.roundToInt

/** Draws the portrait screen described in docs/protocol.md and encodes it for /frame. */
object FrameRenderer {
    const val WIDTH = 480
    const val HEIGHT = 800

    /**
     * The panel's pixels are not square: 0.1188 mm along the portrait height, 0.1122 mm across.
     * The screen is laid out in square units, LAYOUT_HEIGHT tall, then squeezed into HEIGHT pixels.
     */
    private const val PIXEL_ASPECT = 0.1188f / 0.1122f
    private val LAYOUT_HEIGHT = (HEIGHT * PIXEL_ASPECT).roundToInt()

    private const val COVER = 480
    private const val TEXT_MARGIN = 24
    private const val TITLE_SIZE = 38f
    private const val ARTIST_SIZE = 30f
    private const val TITLE_BASELINE = 596
    private const val ARTIST_BASELINE = 654
    private const val MAX_LINES = 2
    private const val JPEG_QUALITY = 90
    private val FALLBACK_BACKGROUND = Color.rgb(38, 52, 92)

    fun render(title: String, artist: String, art: Bitmap?): Bitmap {
        val cover = art?.let { softwareCopy(it) }
        val background = cover?.let { backgroundColor(it) } ?: FALLBACK_BACKGROUND
        val out = createBitmap(WIDTH, HEIGHT)
        val canvas = Canvas(out)
        canvas.drawColor(background)
        canvas.scale(1f, HEIGHT / LAYOUT_HEIGHT.toFloat())
        if (cover != null) {
            canvas.drawBitmap(cover, centerSquare(cover), Rect(0, 0, COVER, COVER), Paint(Paint.FILTER_BITMAP_FLAG))
        }
        drawText(canvas, title, artist, background)
        return out
    }

    /** Rotate 90° clockwise to the 800x480 landscape frame and encode it as a baseline JPEG. */
    fun toJpeg(portrait: Bitmap): ByteArray {
        val landscape = Bitmap.createBitmap(portrait, 0, 0, WIDTH, HEIGHT, Matrix().apply { postRotate(90f) }, true)
        return ByteArrayOutputStream().use { stream ->
            landscape.compress(Bitmap.CompressFormat.JPEG, JPEG_QUALITY, stream)
            stream.toByteArray()
        }
    }

    private fun softwareCopy(art: Bitmap): Bitmap =
        if (art.config == Bitmap.Config.HARDWARE) art.copy(Bitmap.Config.ARGB_8888, false) else art

    private fun backgroundColor(art: Bitmap): Int? {
        val palette = Palette.from(art).generate()
        val swatch = palette.darkMutedSwatch ?: palette.mutedSwatch ?: palette.dominantSwatch
        return swatch?.rgb
    }

    /** Center crop: the largest centered square of the image. */
    private fun centerSquare(bitmap: Bitmap): Rect {
        val side = minOf(bitmap.width, bitmap.height)
        val left = (bitmap.width - side) / 2
        val top = (bitmap.height - side) / 2
        return Rect(left, top, left + side, top + side)
    }

    private fun drawText(canvas: Canvas, title: String, artist: String, background: Int) {
        val dark = ColorUtils.calculateLuminance(background) < 0.5
        val titleColor = if (dark) Color.WHITE else Color.rgb(17, 17, 17)
        val artistColor = ColorUtils.blendARGB(titleColor, background, 0.2f)

        val titlePaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            typeface = Typeface.DEFAULT_BOLD
            textSize = TITLE_SIZE
            color = titleColor
        }
        val artistPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            typeface = Typeface.DEFAULT
            textSize = ARTIST_SIZE
            color = artistColor
        }
        val titleLayout = layout(title, titlePaint)
        val artistLayout = layout(artist, artistPaint)

        // With one line each, the baselines land exactly on TITLE_BASELINE and ARTIST_BASELINE.
        // Longer text keeps the same gap and stays centered on the same vertical middle.
        val titleFm = titlePaint.fontMetricsInt
        val artistFm = artistPaint.fontMetricsInt
        val gap = (ARTIST_BASELINE + artistFm.ascent) - (TITLE_BASELINE + titleFm.descent)
        val middle = ((TITLE_BASELINE + titleFm.ascent) + (ARTIST_BASELINE + artistFm.descent)) / 2f
        val blockHeight = titleLayout.height + gap + artistLayout.height
        var top = middle - blockHeight / 2f

        canvas.withTranslation(TEXT_MARGIN.toFloat(), top) { titleLayout.draw(this) }
        top += titleLayout.height + gap
        canvas.withTranslation(TEXT_MARGIN.toFloat(), top) { artistLayout.draw(this) }
    }

    private fun layout(text: String, paint: TextPaint): StaticLayout =
        StaticLayout.Builder.obtain(text, 0, text.length, paint, WIDTH - 2 * TEXT_MARGIN)
            .setAlignment(Layout.Alignment.ALIGN_CENTER)
            .setMaxLines(MAX_LINES)
            .setEllipsize(TextUtils.TruncateAt.END)
            .setIncludePad(false)
            .build()
}
