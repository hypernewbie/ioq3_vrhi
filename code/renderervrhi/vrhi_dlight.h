/*
===========================================================================
VRHI bounded dynamic-light modulation helpers.

Dependency-free (C++11) helpers shared by renderer_vrhi and its standalone
unit test. This is intentionally NOT Quake lightmap/shader parity: it is a
bounded per-vertex modulation where each submitted point light contributes

    falloff = clamp(1 - dist / radius, 0, 1)
    add    += falloff * falloff * color

to a vertex, and the final vertex color is base * (1 + clamp(add, 0, CAP)).
There is no shadowing, no directionality, and no light grid; the additive
flag is retained for refexport API parity only (both kinds add the same way
in vertex modulation).
===========================================================================
*/
#ifndef __VRHI_DLIGHT_H
#define __VRHI_DLIGHT_H

#include <cmath>
#include <cstddef>

// A validated per-scene point light. `color` channels are in [0,1] and
// `radius` is finite and positive; VRHI_DLightValid checks exactly this
// contract, and callers must reject (or clamp) anything that fails it.
struct VRHI_DLight {
	float origin[3];
	float color[3];
	float radius;
	bool additive;
};

// Maximum summed per-vertex light add. Vertex modulation is 1 + add, so the
// brightest surface is 2x its base color; LightForPoint maps add to 0..255.
static const float VRHI_DLIGHT_ADD_CAP = 1.0f;

inline bool VRHI_DLightValid(const VRHI_DLight &light) {
	for (int i = 0; i < 3; ++i) {
		if (!std::isfinite(light.origin[i]) || !std::isfinite(light.color[i]) ||
			light.color[i] < 0.0f) {
			return false;
		}
	}
	return std::isfinite(light.radius) && light.radius > 0.0f;
}

// Squared falloff in [0,1] of one light at a world-space position. Returns
// 0 outside the finite radius or for invalid lights, 1 exactly at the
// origin, and (1 - dist/radius)^2 in between. Never returns NaN or Inf.
inline float VRHI_DLightFalloff(const VRHI_DLight &light,
	float px, float py, float pz) {
	if (!VRHI_DLightValid(light)) return 0.0f;
	const float dx = px - light.origin[0];
	const float dy = py - light.origin[1];
	const float dz = pz - light.origin[2];
	const float distSquared = dx * dx + dy * dy + dz * dz;
	if (!std::isfinite(distSquared)) return 0.0f;
	if (distSquared <= 0.0f) return 1.0f;
	const float radiusSquared = light.radius * light.radius;
	if (distSquared >= radiusSquared) return 0.0f;
	const float dist = std::sqrt(distSquared);
	if (!std::isfinite(dist) || dist <= 0.0f) return 1.0f;
	const float falloff = 1.0f - dist / light.radius;
	return falloff * falloff;
}

// Sums the bounded light add for one position across all lights. `add` is
// always written (zeroed first) and each channel is clamped to
// [0, VRHI_DLIGHT_ADD_CAP]; no-lights and null inputs are a zero no-op.
inline void VRHI_DLightAdd(const VRHI_DLight *lights, std::size_t count,
	float px, float py, float pz, float add[3]) {
	add[0] = 0.0f;
	add[1] = 0.0f;
	add[2] = 0.0f;
	if (lights == nullptr || count == 0) return;
	for (std::size_t i = 0; i < count; ++i) {
		const float falloff = VRHI_DLightFalloff(lights[i], px, py, pz);
		if (falloff <= 0.0f) continue;
		add[0] += falloff * lights[i].color[0];
		add[1] += falloff * lights[i].color[1];
		add[2] += falloff * lights[i].color[2];
	}
	for (int channel = 0; channel < 3; ++channel) {
		if (add[channel] < 0.0f) {
			add[channel] = 0.0f;
		} else if (add[channel] > VRHI_DLIGHT_ADD_CAP) {
			add[channel] = VRHI_DLIGHT_ADD_CAP;
		}
	}
}

#endif // __VRHI_DLIGHT_H
