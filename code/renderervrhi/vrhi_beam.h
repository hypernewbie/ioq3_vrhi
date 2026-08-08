/*
===========================================================================
VRHI bounded camera-facing beam-quad fallback helpers.

Dependency-free (C++11) helpers shared by renderer_vrhi and its standalone
unit test. RT_BEAM, RT_LIGHTNING, RT_RAIL_CORE, and RT_RAIL_RINGS entities
are all emitted through the same bounded camera-facing beam quad path: a
single view-facing quad spanning origin..oldorigin with a bounded per-type
width, textured with the entity customShader (solid fallback) and modulated
by entity color plus the bounded per-scene dynamic-light add.

This is intentionally a BEAM-QUAD FALLBACK APPROXIMATION, NOT full Quake
rail/lightning geometry or shader-stage parity: there are no rail rings, no
rotated rail-core passes, no shader stages, no segment animation, and no
per-edge color gradients. Widths are fixed per type and bounded so a hostile
or malformed entity can never emit unbounded geometry.

The reType ids mirror the engine's refEntityType_t enum
(renderercommon/tr_types.h): RT_MODEL=0, RT_POLY=1, RT_SPRITE=2, RT_BEAM=3,
RT_RAIL_CORE=4, RT_RAIL_RINGS=5, RT_LIGHTNING=6, RT_PORTALSURFACE=7. The
local copy keeps this helper (and its test) free of engine headers.
===========================================================================
*/
#ifndef __VRHI_BEAM_H
#define __VRHI_BEAM_H

// ReType ids used by the beam-quad fallback (mirrors tr_types.h values).
enum VRHI_BeamType {
	VRHI_BEAM_BEAM = 3,
	VRHI_BEAM_RAIL_CORE = 4,
	VRHI_BEAM_RAIL_RINGS = 5,
	VRHI_BEAM_LIGHTNING = 6,
};

// True for exactly the ref entity types that the beam-quad fallback emits.
// RT_PORTALSURFACE and every other reType stay outside this path so they
// remain safe no-ops.
inline bool VRHI_BeamFallbackSupported(int reType) {
	return reType == VRHI_BEAM_BEAM || reType == VRHI_BEAM_RAIL_CORE ||
		reType == VRHI_BEAM_RAIL_RINGS || reType == VRHI_BEAM_LIGHTNING;
}

// Bounded quad half-width for one beam-quad entity. `frame` is an int (the
// engine's refEntity_t.frame), so it is finite by construction.
//
//   RT_BEAM        entity.frame > 0 ? clamp(frame * 0.5, 0.25, 4096) : 4.0
//                  (existing behavior, unchanged)
//   RT_LIGHTNING   8.0  (GL lightning uses four rotated rail-core passes of
//                        spanWidth 8; the flat fallback keeps that width)
//   RT_RAIL_CORE   6.0  (r_railCoreWidth default)
//   RT_RAIL_RINGS  16.0 (r_railWidth default; rings themselves are not
//                        faked, only the bounding quad width is kept)
//   anything else  0.0  (callers must gate on VRHI_BeamFallbackSupported)
inline float VRHI_BeamFallbackWidth(int reType, int frame) {
	switch (reType) {
	case VRHI_BEAM_LIGHTNING:
		return 8.0f;
	case VRHI_BEAM_RAIL_CORE:
		return 6.0f;
	case VRHI_BEAM_RAIL_RINGS:
		return 16.0f;
	case VRHI_BEAM_BEAM:
	default:
		break;
	}
	if (reType != VRHI_BEAM_BEAM) return 0.0f;
	if (frame <= 0) return 4.0f;
	const float scaled = static_cast<float>(frame) * 0.5f;
	if (scaled < 0.25f) return 0.25f;
	if (scaled > 4096.0f) return 4096.0f;
	return scaled;
}

#endif // __VRHI_BEAM_H
