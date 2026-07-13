// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View

/**
 * Transparent overlay drawn directly on top of the video [android.view.SurfaceView] (it is placed as a
 * sibling of the SurfaceView inside the aspect-ratio container, so its bounds match the video exactly).
 * It marks the currently detected health bar with a box + crosshair and prints its coordinates.
 *
 * [update] is safe to call from a background thread.
 */
class DetectionOverlayView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null): View(context, attrs)
{
	/** Video frame size, used to report coordinates in source pixels. */
	var videoWidth = 0
	var videoHeight = 0

	/** Centered detection FOV as a percentage of the frame; drawn so the user sees the search region. */
	var fovWidthPercent = 100
	var fovHeightPercent = 100

	@Volatile private var result: DetectionResult? = null

	// The aim point (head, below the bar) the aim assist is tracking, in normalized frame coords.
	@Volatile private var aimX = 0.5f
	@Volatile private var aimY = 0.5f
	@Volatile private var aimActive = false

	private val density = resources.displayMetrics.density

	private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		color = Color.WHITE
		textSize = 13f * density
		setShadowLayer(3f, 0f, 0f, Color.BLACK)
	}

	private val textBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		color = 0xAA000000.toInt()
	}

	private val fovPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		style = Paint.Style.STROKE
		strokeWidth = 1f * density
		color = 0x66FFFFFF
		pathEffect = DashPathEffect(floatArrayOf(8f * density, 6f * density), 0f)
	}

	// Aim point marker — filled dot + ring, magenta so it stands out from the green target and cyan box.
	private val aimFillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		style = Paint.Style.FILL
		color = AIM_COLOR
	}
	private val aimRingPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		style = Paint.Style.STROKE
		strokeWidth = 2f * density
		color = AIM_COLOR
	}

	fun update(result: DetectionResult?)
	{
		this.result = result
		postInvalidateOnAnimation()
	}

	/** Publish the aim point the assist is tracking (normalized), so it can be drawn on the head. */
	fun setAimPoint(x: Float, y: Float, active: Boolean)
	{
		aimX = x; aimY = y; aimActive = active
		postInvalidateOnAnimation()
	}

	override fun onDraw(canvas: Canvas)
	{
		super.onDraw(canvas)
		val r = result ?: return   // null only while the detector is stopped
		val w = width.toFloat()
		val h = height.toFloat()

		// Draw the detection FOV so the user sees where it's actually looking.
		if(fovWidthPercent < 100 || fovHeightPercent < 100)
		{
			val fw = fovWidthPercent / 100f * w
			val fh = fovHeightPercent / 100f * h
			val fl = (w - fw) / 2f
			val ft = (h - fh) / 2f
			canvas.drawRect(fl, ft, fl + fw, ft + fh, fovPaint)
		}

		// A SINGLE marker: the aim point (head, below the bar) the assist is steering toward. The box +
		// crosshair were removed — one indicator is enough and avoids two markers on the health bar.
		if(aimActive)
		{
			val ax = aimX * w
			val ay = aimY * h
			canvas.drawCircle(ax, ay, 4f * density, aimFillPaint)
			canvas.drawCircle(ax, ay, 10f * density, aimRingPaint)
		}

		// Always-on debug HUD: processing speed (rate + per-frame analyze time + cropped ROI size) so the
		// user can see how fast the color detection on the ROI runs, plus what it currently sees.
		val status = if(r.isBar) "BAR FOUND"
			else if(r.hasBox) "no bar (${r.reason})"
			else "no green"
		val perf = "%.0fHz  %.1fms  ROI %dx%d".format(r.detectFps, r.analyzeMs, r.roiW, r.roiH)
		drawTextWithBg(canvas, "DETECT  $perf   green:${r.targetPixels}px   $status",
			6f * density, 6f * density + (textPaint.fontMetrics.descent - textPaint.fontMetrics.ascent), h)
	}

	/** Draws text with a translucent background at (x, yBaseline-ish), kept on screen; falls back below altY. */
	private fun drawTextWithBg(canvas: Canvas, text: String, x: Float, y: Float, altY: Float)
	{
		val pad = 4f * density
		val textW = textPaint.measureText(text)
		val fm = textPaint.fontMetrics
		val textH = fm.descent - fm.ascent
		var tx = x
		var ty = y
		if(ty - textH < 0f)
			ty = altY + textH + pad * 2
		if(tx + textW + pad * 2 > width) tx = width - textW - pad * 2
		if(tx < 0f) tx = 0f
		canvas.drawRect(tx, ty - textH, tx + textW + pad * 2, ty + pad, textBgPaint)
		canvas.drawText(text, tx + pad, ty - fm.descent, textPaint)
	}

	companion object
	{
		// Magenta – the single aim-point (head) marker the assist steers toward.
		private const val AIM_COLOR = 0xFFFF2D95.toInt()
	}
}
