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
		val r = result ?: return
		val w = width.toFloat()
		val h = height.toFloat()

		val left = r.leftN * w
		val top = r.topN * h
		val right = r.rightN * w
		val bottom = r.bottomN * h
		val cx = (left + right) * 0.5f
		val cy = (top + bottom) * 0.5f

		canvas.drawRect(left, top, right, bottom, boxPaint)

		val crossLen = 8f * density
		canvas.drawLine(cx - crossLen, cy, cx + crossLen, cy, crossPaint)
		canvas.drawLine(cx, cy - crossLen, cx, cy + crossLen, crossPaint)

		// Coordinates in source-video pixels (falls back to the on-screen center if unknown).
		val vx = if(videoWidth > 0) (r.centerXN * videoWidth).toInt() else cx.toInt()
		val vy = if(videoHeight > 0) (r.centerYN * videoHeight).toInt() else cy.toInt()
		val vw = if(videoWidth > 0) (r.widthN * videoWidth).toInt() else (right - left).toInt()
		val label = "x=$vx  y=$vy  w=$vw"

		val pad = 4f * density
		val textW = textPaint.measureText(label)
		val fm = textPaint.fontMetrics
		val textH = fm.descent - fm.ascent
		var tx = left
		var ty = top - pad * 2
		if(ty - textH < 0f)          // if the box is at the very top, put the label just below it
			ty = bottom + textH + pad * 2
		if(tx + textW + pad * 2 > w) // keep the label on screen horizontally
			tx = w - textW - pad * 2
		if(tx < 0f) tx = 0f

		canvas.drawRect(tx, ty - textH, tx + textW + pad * 2, ty + pad, textBgPaint)
		canvas.drawText(label, tx + pad, ty - fm.descent, textPaint)
	}

	companion object
	{
		// Cyan – stands out against the green target color.
		private const val MARKER_COLOR = 0xFF00E5FF.toInt()
	}
}
