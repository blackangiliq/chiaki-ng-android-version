// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View
import com.metallic.chiaki.lib.ControllerState

/**
 * Debug overlay that shows which gamepad buttons are currently pressed. It's fed the merged controller
 * state on every key/motion event from the activity. Pressed chips light up (cyan); the rest stay dim
 * so the whole mapping is always visible and it's obvious the overlay is live.
 *
 * [update] is called on the UI thread (from dispatchKeyEvent / onGenericMotionEvent).
 */
class ControllerOverlayView @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null): View(context, attrs)
{
	@Volatile private var buttons = 0
	@Volatile private var l2 = 0
	@Volatile private var r2 = 0

	private val density = resources.displayMetrics.density

	private val chipOn = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF00E5FF.toInt() }
	private val chipOff = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x55000000 }
	private val textOn = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		color = Color.BLACK
		textSize = 13f * density
		textAlign = Paint.Align.CENTER
	}
	private val textOff = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		color = 0xFFDDDDDD.toInt()
		textSize = 13f * density
		textAlign = Paint.Align.CENTER
		setShadowLayer(2f, 0f, 0f, Color.BLACK)
	}

	fun update(state: ControllerState)
	{
		buttons = state.buttons.toInt()
		l2 = state.l2State.toInt()
		r2 = state.r2State.toInt()
		postInvalidateOnAnimation()
	}

	private fun pressed(chip: Chip): Boolean = when(chip.kind)
	{
		Kind.BUTTON -> (buttons and chip.mask) != 0
		Kind.L2 -> l2 > 0
		Kind.R2 -> r2 > 0
	}

	override fun onDraw(canvas: Canvas)
	{
		super.onDraw(canvas)
		val pad = 6f * density
		val chipH = 22f * density
		val gap = 4f * density
		val corner = 4f * density
		val fm = textOn.fontMetrics
		val baseline = chipH / 2f - (fm.ascent + fm.descent) / 2f
		val rect = RectF()
		var x = pad
		var y = 44f * density   // start below the detection HUD (top-left)
		for(chip in chips)
		{
			val cw = textOn.measureText(chip.label) + pad * 2
			if(x + cw > width - pad)
			{
				x = pad
				y += chipH + gap
			}
			rect.set(x, y, x + cw, y + chipH)
			val on = pressed(chip)
			canvas.drawRoundRect(rect, corner, corner, if(on) chipOn else chipOff)
			canvas.drawText(chip.label, x + cw / 2f, y + baseline, if(on) textOn else textOff)
			x += cw + gap
		}
	}

	private enum class Kind { BUTTON, L2, R2 }
	private class Chip(val label: String, val kind: Kind, val mask: Int)

	companion object
	{
		private fun btn(label: String, mask: UInt) = Chip(label, Kind.BUTTON, mask.toInt())
		private val chips = listOf(
			btn("L1", ControllerState.BUTTON_L1),
			Chip("L2", Kind.L2, 0),
			btn("L3", ControllerState.BUTTON_L3),
			btn("R1", ControllerState.BUTTON_R1),
			Chip("R2", Kind.R2, 0),
			btn("R3", ControllerState.BUTTON_R3),
			btn("Cross", ControllerState.BUTTON_CROSS),
			btn("Circle", ControllerState.BUTTON_MOON),
			btn("Square", ControllerState.BUTTON_BOX),
			btn("Triangle", ControllerState.BUTTON_PYRAMID),
			btn("Up", ControllerState.BUTTON_DPAD_UP),
			btn("Down", ControllerState.BUTTON_DPAD_DOWN),
			btn("Left", ControllerState.BUTTON_DPAD_LEFT),
			btn("Right", ControllerState.BUTTON_DPAD_RIGHT),
			btn("Share", ControllerState.BUTTON_SHARE),
			btn("Options", ControllerState.BUTTON_OPTIONS),
			btn("Touchpad", ControllerState.BUTTON_TOUCHPAD),
			btn("PS", ControllerState.BUTTON_PS)
		)
	}
}
