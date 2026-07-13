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
	val leftN: Float,
	val topN: Float,
	val rightN: Float,
	val bottomN: Float,
	val pixelCount: Int
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
	private val onResult: (DetectionResult?) -> Unit
)
{
	object Config
	{
		/** How often to grab and analyze a frame (ms). ~6-7 Hz is plenty and cheap. */
		const val INTERVAL_MS = 150L

		/** Longest side of the analysis bitmap. The frame is downscaled to this to keep the loop cheap. */
		const val ANALYSIS_MAX_DIMEN = 640

		// --- Target color, HSV (Android scale: H 0..360, S 0..1, V 0..1). Default: green (#00FF00). ---
		// Equivalent to the OpenCV wide range lower=(38,50,90) upper=(82,255,255) on the H:0-179 scale.
		const val HUE_MIN = 76f
		const val HUE_MAX = 164f
		const val SAT_MIN = 0.196f
		const val VAL_MIN = 0.353f

		// --- Bar shape (as fractions of the frame so they are resolution independent). ---
		const val MIN_WIDTH_FRAC = 0.05f    // bar must span at least 5% of the frame width
		const val MIN_HEIGHT_FRAC = 0.004f  // ~3px at 720p
		const val MAX_HEIGHT_FRAC = 0.06f   // ~40px at 720p
		const val MIN_ASPECT = 4.0f         // width / height, "thin horizontal"
		const val MIN_FILL_RATIO = 0.5f     // fraction of the bounding box that is the target color
		const val MIN_CORNERS = 2           // how many of the 4 corners must be a dominant target pixel
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
	private var stack: IntArray? = null
	private val hsv = FloatArray(3)

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
		stack = null
		onResult(null)
	}

	private fun scheduleNext()
	{
		if(running)
			handler?.postDelayed(this::capture, Config.INTERVAL_MS)
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
						onResult(analyze(bmp))
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
			stack = IntArray(w * h)
		}
		return bmp
	}

	/** True if the pixel falls inside the target HSV range. */
	private fun isTarget(r: Int, g: Int, b: Int): Boolean
	{
		Color.RGBToHSV(r, g, b, hsv)
		return hsv[0] in Config.HUE_MIN..Config.HUE_MAX &&
				hsv[1] >= Config.SAT_MIN &&
				hsv[2] >= Config.VAL_MIN
	}

	/** Stronger single-pixel confirmation (BGR dominance) used to validate a candidate's corners. */
	private fun isDominant(r: Int, g: Int, b: Int): Boolean =
		g > 150 && r < 150 && b < 150 && g > r && g > b && g > (r + b) / 2

	/**
	 * Analyze the captured frame and return the best matching health bar, or null if none is found.
	 * 4-connected flood fill labels every target-colored blob, then the bar shape filter + corner
	 * confirmation reject anything that is not a solid thin horizontal bar. The widest survivor wins.
	 */
	private fun analyze(bmp: Bitmap): DetectionResult?
	{
		val w = bmp.width
		val h = bmp.height
		val px = pixels ?: return null
		val mk = mask ?: return null
		val st = stack ?: return null
		bmp.getPixels(px, 0, w, 0, 0, w, h)

		// Build the color mask.
		for(i in 0 until w * h)
		{
			val c = px[i]
			mk[i] = isTarget((c shr 16) and 0xff, (c shr 8) and 0xff, c and 0xff)
		}

		val minWidthPx = Config.MIN_WIDTH_FRAC * w
		val minHeightPx = Config.MIN_HEIGHT_FRAC * h
		val maxHeightPx = Config.MAX_HEIGHT_FRAC * h

		var bestCount = -1
		var bLeft = 0; var bTop = 0; var bRight = 0; var bBottom = 0

		for(start in 0 until w * h)
		{
			if(!mk[start])
				continue

			// Flood fill this blob (4-connectivity), consuming the mask as we go.
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

				if(x > 0 && mk[idx - 1]) { mk[idx - 1] = false; st[top++] = idx - 1 }
				if(x < w - 1 && mk[idx + 1]) { mk[idx + 1] = false; st[top++] = idx + 1 }
				if(y > 0 && mk[idx - w]) { mk[idx - w] = false; st[top++] = idx - w }
				if(y < h - 1 && mk[idx + w]) { mk[idx + w] = false; st[top++] = idx + w }
			}

			val bw = maxX - minX + 1
			val bh = maxY - minY + 1

			// Shape filter: thin horizontal bar of the expected size, mostly filled.
			if(bw < minWidthPx) continue
			if(bh < minHeightPx || bh > maxHeightPx) continue
			if(bw.toFloat() / bh < Config.MIN_ASPECT) continue
			if(count.toFloat() / (bw * bh) < Config.MIN_FILL_RATIO) continue

			// Corner confirmation: at least MIN_CORNERS of the 4 (slightly inset) corners must be a
			// dominant target pixel.
			val insetX = minOf(2, (bw - 1) / 4)
			val insetY = minOf(1, (bh - 1) / 2)
			var corners = 0
			if(isDominantAt(px, w, minX + insetX, minY + insetY)) corners++
			if(isDominantAt(px, w, maxX - insetX, minY + insetY)) corners++
			if(isDominantAt(px, w, minX + insetX, maxY - insetY)) corners++
			if(isDominantAt(px, w, maxX - insetX, maxY - insetY)) corners++
			if(corners < Config.MIN_CORNERS) continue

			// Keep the widest bar (health bars are usually the largest solid colored bar on screen).
			if(count > bestCount)
			{
				bestCount = count
				bLeft = minX; bTop = minY; bRight = maxX; bBottom = maxY
			}
		}

		if(bestCount < 0)
			return null

		return DetectionResult(
			leftN = bLeft.toFloat() / w,
			topN = bTop.toFloat() / h,
			rightN = (bRight + 1).toFloat() / w,
			bottomN = (bBottom + 1).toFloat() / h,
			pixelCount = bestCount
		)
	}

	private fun isDominantAt(px: IntArray, w: Int, x: Int, y: Int): Boolean
	{
		val c = px[y * w + x]
		return isDominant((c shr 16) and 0xff, (c shr 8) and 0xff, c and 0xff)
	}
}
