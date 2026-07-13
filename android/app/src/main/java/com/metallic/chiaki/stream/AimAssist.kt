// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max

/**
 * Aim assist engine — turns the low-rate [DetectionResult] (~15 Hz) into a smooth **60 Hz** right-stick
 * output that nudges the PS5 crosshair toward the detected target's head.
 *
 * Ported from the PC "Color Detection" tracking, kept deliberately simple — on by default, four
 * hardcoded behaviours that together give a strong-but-smooth follow:
 *   1. **Input-position smoothing** — low-pass the detected point (snap on a big jump = new target).
 *      Kills the few-pixel per-frame detection jitter that otherwise flips the aim direction up close.
 *   2. **Distance-scaled EMA** — the stick eases toward the target each 60 Hz tick; the step is larger
 *      when far (fast catch-up) and smaller when near (no overshoot).
 *   3. **Near-target damping** — the output fades to a gentle floor as the crosshair settles on the
 *      head, so it holds steady instead of buzzing around it.
 *   4. **Distance power curve** — full pull far away, softened as it closes in.
 *
 * Coordinate space is the video frame normalized to [0,1]; the crosshair is assumed at the centre.
 * Threading: [onDetection] is called from the detector thread, [tick] from the aim-loop thread. The two
 * never touch the same mutable field — the hand-off is the `@Volatile` target below — so no lock needed.
 */
class AimAssist
{
	@Volatile var enabled = true
	/** Overall stick magnitude (0..1). */
	@Volatile var strength = 0.75f
	/** The head sits below the bar by this × the bar's width (shrinks with distance automatically). */
	@Volatile var headOffset = 0.6f

	// --- hardcoded tuning (mirrors the PC values that tracked smoothly) ---
	private val baseSmoothing = 0.9f    // EMA responsiveness before distance scaling
	private val posSmoothing = 0.5f     // input-position filter strength
	private val nearHoldN = 0.03f       // within this normalized distance → damp to settle on target
	private val targetMemoryMs = 250L   // drop the target if no detection arrives for this long

	// detector-thread only:
	private var posX = 0f
	private var posY = 0f
	private var hasPos = false

	// aim-loop-thread only:
	private var smoothX = 0f
	private var smoothY = 0f

	// hand-off (detector → aim loop):
	@Volatile private var haveTarget = false
	@Volatile private var lastDetectMs = 0L
	@Volatile private var tgtX = 0f
	@Volatile private var tgtY = 0f

	/** Feed a fresh detection (or null when nothing is found). Called at the detection rate. */
	fun onDetection(r: DetectionResult?, nowMs: Long)
	{
		if(!enabled || r == null || !r.isBar)
		{
			haveTarget = false
			hasPos = false
			return
		}

		// Aim point: horizontally the bar centre; vertically just below the bar (the head), offset by a
		// fraction of the bar width so the drop shrinks with distance (far bar = narrow = small drop).
		val aimX = r.centerXN
		val aimY = (r.bottomN + headOffset * r.widthN).coerceIn(0f, 1f)

		// Input-position smoothing: snap on a big jump (new target) so it doesn't slide across the
		// screen; otherwise low-pass to absorb the per-frame detection jitter.
		if(!hasPos || abs(aimX - posX) > 0.15f || abs(aimY - posY) > 0.15f)
		{
			posX = aimX; posY = aimY; hasPos = true
		}
		else
		{
			posX += (aimX - posX) * posSmoothing
			posY += (aimY - posY) * posSmoothing
		}

		tgtX = posX
		tgtY = posY
		lastDetectMs = nowMs
		haveTarget = true
	}

	/** Advance the smoothing one aim-loop step and return the right-stick output (±32767). */
	fun tick(nowMs: Long): Pair<Short, Short>
	{
		if(!enabled || !haveTarget || nowMs - lastDetectMs > targetMemoryMs)
		{
			// Target gone: ease the stick back to rest so it doesn't stay deflected.
			smoothX *= 0.5f; smoothY *= 0.5f
			return stick()
		}

		val dx = tgtX - 0.5f
		val dy = tgtY - 0.5f
		val dist = hypot(dx, dy)
		if(dist < 1e-4f)
		{
			smoothX *= 0.6f; smoothY *= 0.6f
			return stick()
		}

		// Distance-scaled speed: strong pull far, gentle near (prevents overshoot/jitter on target).
		// Thresholds are the PC pixel bands (100/50/20/15 on a 470px frame) expressed as frame fractions.
		val distMul = when
		{
			dist > 0.21f -> 1.5f
			dist > 0.106f -> 1.15f
			dist > 0.043f -> 0.85f
			else -> 0.55f
		}
		val power = when
		{
			dist < 0.032f -> 0.30f + (dist / 0.032f) * 0.30f              // 0.30 .. 0.60
			dist < 0.106f -> 0.55f + ((dist - 0.032f) / 0.074f) * 0.45f   // ~0.55 .. 1.0
			else -> 1.0f
		}

		// Target stick = unit direction × power × strength.
		val ux = dx / dist
		val uy = dy / dist
		val targetX = ux * power * strength
		val targetY = uy * power * strength

		// Plain EMA (the noise-filtering smoothing).
		val sf = (baseSmoothing * distMul).coerceIn(0.05f, 0.85f)
		smoothX += (targetX - smoothX) * sf
		smoothY += (targetY - smoothY) * sf

		// Near-target damping: fade the output to a gentle floor as we settle onto the head.
		val nearDamp = if(dist < nearHoldN) max(0.25f, dist / nearHoldN) else 1f
		return stick(smoothX * nearDamp, smoothY * nearDamp)
	}

	private fun stick(x: Float = smoothX, y: Float = smoothY): Pair<Short, Short>
	{
		val sx = (x.coerceIn(-1f, 1f) * 32767f).toInt().toShort()
		val sy = (y.coerceIn(-1f, 1f) * 32767f).toInt().toShort()
		return Pair(sx, sy)
	}

	fun reset()
	{
		posX = 0f; posY = 0f; hasPos = false
		smoothX = 0f; smoothY = 0f
		haveTarget = false
	}
}
