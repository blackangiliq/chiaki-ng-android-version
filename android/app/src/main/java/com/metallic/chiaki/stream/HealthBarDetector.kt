// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.graphics.Bitmap
import android.graphics.Color
import android.os.Handler
import android.os.HandlerThread
import android.view.PixelCopy
import android.view.SurfaceView

/**
 * Detected colored health bar, expressed in normalized [0,1] coordinates relative to the video frame.
 * (0,0) is the top-left of the video, (1,1) the bottom-right.
 */
data class DetectionResult(
	val targetPixels: Int,   // total target-colored pixels found in the analyzed frame
	val hasBox: Boolean,     // is there any target region to draw at all?
	val isBar: Boolean,      // does the drawn region qualify as a health bar?
	val reason: String,      // short explanation of why the largest region is / isn't a bar
	val leftN: Float,
	val topN: Float,
	val rightN: Float,
	val bottomN: Float,
	val analyzeMs: Float = 0f,   // wall time to analyze this frame (color mask + shape), ms
	val detectFps: Float = 0f,   // measured detection rate (frames analyzed per second)
	val roiW: Int = 0,           // cropped ROI (FOV) size actually scanned, px
	val roiH: Int = 0
)
{
	val centerXN get() = (leftN + rightN) * 0.5f
	val centerYN get() = (topN + bottomN) * 0.5f
	val widthN get() = rightN - leftN
	val heightN get() = bottomN - topN
}

/**
 * Detects a specific colored, thin horizontal bar (e.g. a health bar) inside the video that is being
 * streamed to a [SurfaceView], and reports where it is.
 *
 * On Android the video is decoded by MediaCodec straight into the SurfaceView's Surface, so the pixels
 * never reach the CPU through the decode path. Instead we periodically grab the currently displayed
 * frame with [PixelCopy] (API 24+) into a small downscaled Bitmap on a background thread and run a
 * pure-Kotlin HSV + connected-components analysis on it. Nothing in the decode/render pipeline is
 * touched, so streaming performance is unaffected.
 *
 * All detection thresholds live in [Config] so the target color / shape can be retuned in one place.
 */
class HealthBarDetector(
	private val surfaceView: SurfaceView,
	fovWidthPercentInit: Int,
	fovHeightPercentInit: Int,
	private val onResult: (DetectionResult?) -> Unit
)
{
	// Mutable so the in-stream tuning panel can change the FOV live (read fresh every analyze()).
	@Volatile var fovWidthPercent = fovWidthPercentInit
	@Volatile var fovHeightPercent = fovHeightPercentInit
	object Config
	{
		/** How often to grab and analyze a frame (ms). ~15 Hz — the 60 Hz aim loop smooths between these. */
		const val INTERVAL_MS = 66L

		/** Longest side of the analysis bitmap. The frame is downscaled to this to keep the loop cheap. */
		const val ANALYSIS_MAX_DIMEN = 400

		// --- Target color, HSV. Green #00FF00. Values are the OpenCV range (H 0-179, S/V 0-255)
		// lower=(45,175,155) upper=(75,255,255) converted to Android's scale (H 0-360, S/V 0-1). ---
		const val HUE_MIN = 90f     // OpenCV 45 * 2
		const val HUE_MAX = 150f    // OpenCV 75 * 2
		const val SAT_MIN = 0.686f  // OpenCV 175 / 255
		const val VAL_MIN = 0.608f  // OpenCV 155 / 255

		// Morphological close radius (dilate then erode) to bridge gaps in the mask. 2 ≈ a 5x5 kernel.
		const val CLOSE_RADIUS = 2

		// --- Bar shape (as fractions of the frame so they are resolution independent). ---
		const val MIN_WIDTH_FRAC = 0.035f   // bar must span at least 3.5% of the frame width (allows depleted bars)
		const val MIN_HEIGHT_FRAC = 0.004f  // ~3px at 720p
		const val MAX_HEIGHT_FRAC = 0.06f   // ~40px at 720p
		const val MIN_ASPECT = 3.0f         // width / height, "thin horizontal" (relaxed for short/depleted bars)
		// Fraction of the bounding box that is the target color. Real health bars have RULER TICKS /
		// segment gaps / partial fill, so they are NOT 50% solid — a strict 0.5 rejected them (log said
		// "not solid"). 0.25 accepts a ticked/partly-filled bar while still rejecting scattered noise
		// (which also fails the aspect gate).
		const val MIN_FILL_RATIO = 0.25f
		const val MIN_CORNERS = 1           // at least one corner a dominant target pixel (anti-aliased edges)
	}

	private var thread: HandlerThread? = null
	private var handler: Handler? = null
	@Volatile private var running = false
	// Bumped on every start/stop so a PixelCopy callback that completes after a stop is ignored
	// instead of rescheduling itself onto a freshly started loop.
	@Volatile private var generation = 0

	// Reused across frames to avoid per-frame allocations.
	private var bitmap: Bitmap? = null
	private var pixels: IntArray? = null
	private var mask: BooleanArray? = null
	private var maskTmp: BooleanArray? = null   // scratch for the morphological close
	private var stack: IntArray? = null

	// Processing-speed stats (shown in the on-screen HUD). "As fast as possible" by default: 0ms delay =
	// analyze back-to-back as PixelCopy delivers frames (uses whatever the device/stream can give).
	@Volatile var intervalMs = 0L
	private var lastResultNanos = 0L
	private var smoothedFps = 0f
	private var lastRoiW = 0
	private var lastRoiH = 0

	fun start()
	{
		if(running)
			return
		running = true
		generation++
		val thread = HandlerThread("health-bar-detector").also { it.start() }
		this.thread = thread
		val handler = Handler(thread.looper)
		this.handler = handler
		handler.post(this::capture)
	}

	fun stop()
	{
		running = false
		generation++
		handler?.removeCallbacksAndMessages(null)
		thread?.quitSafely()
		thread = null
		handler = null
		// Don't recycle the bitmap here: a PixelCopy started on the native side may still be writing to
		// it. Just drop our references and let the GC reclaim it once any copy completes.
		bitmap = null
		pixels = null
		mask = null
		maskTmp = null
		stack = null
		onResult(null)
	}

	private fun scheduleNext()
	{
		if(running)
		{
			if(intervalMs <= 0L) handler?.post(this::capture)
			else handler?.postDelayed(this::capture, intervalMs)
		}
	}

	private fun capture()
	{
		if(!running)
			return
		val h = handler ?: return

		val surface = surfaceView.holder.surface
		val srcW = surfaceView.width
		val srcH = surfaceView.height
		if(surface == null || !surface.isValid || srcW <= 0 || srcH <= 0)
		{
			scheduleNext()
			return
		}

		val bmp = ensureBuffers(srcW, srcH)
		val gen = generation
		try
		{
			PixelCopy.request(surfaceView, bmp, { result ->
				// Ignore a copy that finished after we were stopped (or restarted).
				if(gen == generation && running)
				{
					if(result == PixelCopy.SUCCESS)
						{
						val t0 = System.nanoTime()
						val res = analyze(bmp)
						val analyzeMs = (System.nanoTime() - t0) / 1_000_000f
						val nowN = System.nanoTime()
						val instFps = if(lastResultNanos != 0L && nowN > lastResultNanos)
							1_000_000_000f / (nowN - lastResultNanos) else 0f
						lastResultNanos = nowN
						smoothedFps = if(smoothedFps <= 0f) instFps else smoothedFps * 0.8f + instFps * 0.2f
						onResult(res.copy(analyzeMs = analyzeMs, detectFps = smoothedFps, roiW = lastRoiW, roiH = lastRoiH))
					}
					scheduleNext()
				}
			}, h)
		}
		catch(e: Exception)
		{
			// Surface can become invalid between the checks above and the request; just retry next tick.
			scheduleNext()
		}
	}

	/** (Re)allocate the analysis bitmap and scratch buffers for the given source size, keeping aspect. */
	private fun ensureBuffers(srcW: Int, srcH: Int): Bitmap
	{
		val scale = minOf(1f, Config.ANALYSIS_MAX_DIMEN.toFloat() / maxOf(srcW, srcH))
		val w = maxOf(1, (srcW * scale).toInt())
		val h = maxOf(1, (srcH * scale).toInt())
		var bmp = bitmap
		if(bmp == null || bmp.width != w || bmp.height != h)
		{
			bmp?.recycle()
			bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
			bitmap = bmp
			pixels = IntArray(w * h)
			mask = BooleanArray(w * h)
			maskTmp = BooleanArray(w * h)
			stack = IntArray(w * h)
		}
		return bmp
	}

	/**
	 * True if the pixel is target-green. Fast **integer** equivalent of the old HSV test (no per-pixel
	 * `Color.RGBToHSV` float conversion — the single biggest cost in the analysis loop):
	 *   green is the dominant channel (hue ≈ green), V = max/255 ≥ 0.608, S = (max-min)/max ≥ 0.686.
	 * Since g is the max when it passes, max=g and min=min(r,b) — the whole test is a few int compares.
	 */
	private fun isTarget(r: Int, g: Int, b: Int): Boolean
	{
		if(g < 155) return false            // V ≥ 0.608 (brightness / green floor)
		if(g < r || g < b) return false     // green is the max channel (green-ish hue)
		val mn = if(r < b) r else b
		return (g - mn) * 1000 >= 686 * g   // S ≥ 0.686 (saturation), integer form
	}

	/** Stronger single-pixel confirmation (BGR dominance) used to validate a candidate's corners. */
	private fun isDominant(r: Int, g: Int, b: Int): Boolean =
		g > 150 && r < 150 && b < 150 && g > r && g > b && g > (r + b) / 2

	/**
	 * Analyze the captured frame and return the best matching health bar, or null if none is found.
	 * 4-connected flood fill labels every target-colored blob, then the bar shape filter + corner
	 * confirmation reject anything that is not a solid thin horizontal bar. The widest survivor wins.
	 */
	private fun analyze(bmp: Bitmap): DetectionResult
	{
		val w = bmp.width
		val h = bmp.height
		val px = pixels ?: return DetectionResult(0, false, false, "n/a", 0f, 0f, 0f, 0f)
		val mk = mask ?: return DetectionResult(0, false, false, "n/a", 0f, 0f, 0f, 0f)
		val st = stack ?: return DetectionResult(0, false, false, "n/a", 0f, 0f, 0f, 0f)
		val tmp = maskTmp ?: return DetectionResult(0, false, false, "n/a", 0f, 0f, 0f, 0f)
		bmp.getPixels(px, 0, w, 0, 0, w, h)

		// Region of interest: a centered FOV. Restricting all work to it speeds up the whole pass.
		val roiW = (w * fovWidthPercent / 100).coerceIn(1, w)
		val roiH = (h * fovHeightPercent / 100).coerceIn(1, h)
		lastRoiW = roiW; lastRoiH = roiH
		val x0 = (w - roiW) / 2
		val y0 = (h - roiH) / 2
		val x1 = x0 + roiW
		val y1 = y0 + roiH

		// Build the color mask inside the ROI and count how much target color is present at all.
		var targetPixels = 0
		for(y in y0 until y1)
		{
			val row = y * w
			for(x in x0 until x1)
			{
				val c = px[row + x]
				val t = isTarget((c shr 16) and 0xff, (c shr 8) and 0xff, c and 0xff)
				mk[row + x] = t
				if(t) targetPixels++
			}
		}

		// Morphological close (dilate then erode) bridges small gaps so a broken bar reads as one blob.
		morphClose(mk, tmp, x0, y0, x1, y1, w)

		val minWidthPx = Config.MIN_WIDTH_FRAC * w
		val minHeightPx = Config.MIN_HEIGHT_FRAC * h
		val maxHeightPx = Config.MAX_HEIGHT_FRAC * h

		// Track both the biggest target blob overall (for the debug HUD) and the biggest blob that
		// actually passes the health-bar shape filter (the real detection).
		var loCount = -1; var loL = 0; var loT = 0; var loR = 0; var loB = 0
		var barCount = -1; var bL = 0; var bT = 0; var bR = 0; var bB = 0

		for(sy in y0 until y1)
		{
			val srow = sy * w
			for(sx in x0 until x1)
			{
				val start = srow + sx
				if(!mk[start])
					continue

				// Flood fill this blob (4-connectivity, clamped to the ROI), consuming the mask.
				var minX = Int.MAX_VALUE; var minY = Int.MAX_VALUE
				var maxX = Int.MIN_VALUE; var maxY = Int.MIN_VALUE
				var count = 0
				var top = 0
				st[top++] = start
				mk[start] = false
				while(top > 0)
				{
					val idx = st[--top]
					val x = idx % w
					val y = idx / w
					if(x < minX) minX = x
					if(x > maxX) maxX = x
					if(y < minY) minY = y
					if(y > maxY) maxY = y
					count++

					if(x > x0 && mk[idx - 1]) { mk[idx - 1] = false; st[top++] = idx - 1 }
					if(x < x1 - 1 && mk[idx + 1]) { mk[idx + 1] = false; st[top++] = idx + 1 }
					if(y > y0 && mk[idx - w]) { mk[idx - w] = false; st[top++] = idx - w }
					if(y < y1 - 1 && mk[idx + w]) { mk[idx + w] = false; st[top++] = idx + w }
				}

				if(count > loCount)
				{
					loCount = count
					loL = minX; loT = minY; loR = maxX; loB = maxY
				}

				val bw = maxX - minX + 1
				val bh = maxY - minY + 1
				val isBar = bw >= minWidthPx && bh >= minHeightPx && bh <= maxHeightPx &&
						bw.toFloat() / bh >= Config.MIN_ASPECT &&
						count.toFloat() / (bw * bh) >= Config.MIN_FILL_RATIO &&
						cornersOk(px, w, minX, minY, maxX, maxY)
				if(isBar && count > barCount)
				{
					barCount = count
					bL = minX; bT = minY; bR = maxX; bB = maxY
				}
			}
		}

		// A qualifying bar wins; otherwise report the largest blob + why it was rejected.
		if(barCount >= 0)
			return DetectionResult(targetPixels, true, true, "OK",
				bL.toFloat() / w, bT.toFloat() / h, (bR + 1).toFloat() / w, (bB + 1).toFloat() / h)

		if(loCount >= 0)
		{
			val reason = rejectReason(px, w, loL, loT, loR, loB, loCount, minWidthPx, minHeightPx, maxHeightPx)
			return DetectionResult(targetPixels, true, false, reason,
				loL.toFloat() / w, loT.toFloat() / h, (loR + 1).toFloat() / w, (loB + 1).toFloat() / h)
		}

		return DetectionResult(targetPixels, false, false, "no target color", 0f, 0f, 0f, 0f)
	}

	private fun cornersOk(px: IntArray, w: Int, minX: Int, minY: Int, maxX: Int, maxY: Int): Boolean
	{
		val bw = maxX - minX + 1
		val bh = maxY - minY + 1
		val insetX = minOf(2, (bw - 1) / 4)
		val insetY = minOf(1, (bh - 1) / 2)
		var corners = 0
		if(isDominantAt(px, w, minX + insetX, minY + insetY)) corners++
		if(isDominantAt(px, w, maxX - insetX, minY + insetY)) corners++
		if(isDominantAt(px, w, minX + insetX, maxY - insetY)) corners++
		if(isDominantAt(px, w, maxX - insetX, maxY - insetY)) corners++
		return corners >= Config.MIN_CORNERS
	}

	/** Human-readable reason the largest blob failed the bar filter (for the debug HUD). */
	private fun rejectReason(px: IntArray, w: Int, minX: Int, minY: Int, maxX: Int, maxY: Int,
							 count: Int, minWidthPx: Float, minHeightPx: Float, maxHeightPx: Float): String
	{
		val bw = maxX - minX + 1
		val bh = maxY - minY + 1
		return when
		{
			bw < minWidthPx -> "too narrow"
			bh < minHeightPx -> "too thin"
			bh > maxHeightPx -> "too tall"
			bw.toFloat() / bh < Config.MIN_ASPECT -> "not horizontal"
			count.toFloat() / (bw * bh) < Config.MIN_FILL_RATIO -> "not solid"
			!cornersOk(px, w, minX, minY, maxX, maxY) -> "edges not pure green"
			else -> "OK"
		}
	}

	private fun isDominantAt(px: IntArray, w: Int, x: Int, y: Int): Boolean
	{
		val c = px[y * w + x]
		return isDominant((c shr 16) and 0xff, (c shr 8) and 0xff, c and 0xff)
	}

	/**
	 * In-place morphological close (dilate then erode with a square radius) over the ROI, using tmp as
	 * scratch. Separable (horizontal then vertical) so it stays cheap. Windows are clamped to the ROI.
	 */
	private fun morphClose(mk: BooleanArray, tmp: BooleanArray, x0: Int, y0: Int, x1: Int, y1: Int, w: Int)
	{
		val r = Config.CLOSE_RADIUS
		if(r <= 0) return

		// Dilate horizontal: mk -> tmp
		for(y in y0 until y1)
		{
			val row = y * w
			for(x in x0 until x1)
			{
				var v = false
				val lo = maxOf(x0, x - r); val hi = minOf(x1 - 1, x + r)
				var k = lo
				while(k <= hi) { if(mk[row + k]) { v = true; break }; k++ }
				tmp[row + x] = v
			}
		}
		// Dilate vertical: tmp -> mk
		for(y in y0 until y1)
		{
			val row = y * w
			for(x in x0 until x1)
			{
				var v = false
				val lo = maxOf(y0, y - r); val hi = minOf(y1 - 1, y + r)
				var k = lo
				while(k <= hi) { if(tmp[k * w + x]) { v = true; break }; k++ }
				mk[row + x] = v
			}
		}
		// Erode horizontal: mk -> tmp
		for(y in y0 until y1)
		{
			val row = y * w
			for(x in x0 until x1)
			{
				var v = true
				val lo = maxOf(x0, x - r); val hi = minOf(x1 - 1, x + r)
				var k = lo
				while(k <= hi) { if(!mk[row + k]) { v = false; break }; k++ }
				tmp[row + x] = v
			}
		}
		// Erode vertical: tmp -> mk
		for(y in y0 until y1)
		{
			val row = y * w
			for(x in x0 until x1)
			{
				var v = true
				val lo = maxOf(y0, y - r); val hi = minOf(y1 - 1, y + r)
				var k = lo
				while(k <= hi) { if(!tmp[k * w + x]) { v = false; break }; k++ }
				mk[row + x] = v
			}
		}
	}
}
