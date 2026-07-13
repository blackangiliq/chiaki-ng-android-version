// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
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

	@Volatile private var result: DetectionResult? = null

	private val density = resources.displayMetrics.density

	private val boxPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		style = Paint.Style.STROKE
		strokeWidth = 2f * density
		color = MARKER_COLOR
	}

	// Largest green region that did NOT qualify as a bar (shown so the user sees what's detected).
	private val candidatePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		style = Paint.Style.STROKE
		strokeWidth = 1.5f * density
		color = CANDIDATE_COLOR
	}

	private val crossPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		style = Paint.Style.STROKE
		strokeWidth = 1.5f * density
		color = MARKER_COLOR
	}

	private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		color = Color.WHITE
		textSize = 13f * density
		setShadowLayer(3f, 0f, 0f, Color.BLACK)
	}

	private val textBgPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		color = 0xAA000000.toInt()
	}

	fun update(result: DetectionResult?)
	{
		this.result = result
		postInvalidateOnAnimation()
	}

	override fun onDraw(canvas: Canvas)
	{
		super.onDraw(canvas)
		val r = result ?: return   // null only while the detector is stopped
		val w = width.toFloat()
		val h = height.toFloat()

		// Draw the detected region: bright cyan if it qualifies as a bar, dim orange otherwise.
		if(r.hasBox)
		{
			val left = r.leftN * w
			val top = r.topN * h
			val right = r.rightN * w
			val bottom = r.bottomN * h

			canvas.drawRect(left, top, right, bottom, if(r.isBar) boxPaint else candidatePaint)

			if(r.isBar)
			{
				val cx = (left + right) * 0.5f
				val cy = (top + bottom) * 0.5f
				val crossLen = 8f * density
				canvas.drawLine(cx - crossLen, cy, cx + crossLen, cy, crossPaint)
				canvas.drawLine(cx, cy - crossLen, cx, cy + crossLen, crossPaint)

				val vx = if(videoWidth > 0) (r.centerXN * videoWidth).toInt() else cx.toInt()
				val vy = if(videoHeight > 0) (r.centerYN * videoHeight).toInt() else cy.toInt()
				val vw = if(videoWidth > 0) (r.widthN * videoWidth).toInt() else (right - left).toInt()
				drawTextWithBg(canvas, "x=$vx  y=$vy  w=$vw", left, top - 6f * density, bottom)
			}
		}

		// Always-on debug HUD so it's obvious the detector is running and what it sees.
		val status = if(r.isBar) "BAR FOUND"
			else if(r.hasBox) "no bar (${r.reason})"
			else "no green"
		drawTextWithBg(canvas, "DETECT ON   green: ${r.targetPixels}px   $status",
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
		// Cyan – stands out against the green target color.
		private const val MARKER_COLOR = 0xFF00E5FF.toInt()
		// Orange – the largest green region that didn't qualify as a bar.
		private const val CANDIDATE_COLOR = 0xFFFFA000.toInt()
	}
}
