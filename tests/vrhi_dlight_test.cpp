// Standalone, dependency-free unit test for the VRHI bounded dynamic-light
// modulation helpers (code/renderervrhi/vrhi_dlight.h). No engine headers,
// no filesystem, no third-party libraries: the test builds lights in memory
// and asserts falloff/add/validation behavior.
//
// Build and run (any C++11-or-later compiler):
//   clang++ -std=c++17 -O1 -Wall -Wextra -Werror tests/vrhi_dlight_test.cpp -o vrhi_dlight_test && ./vrhi_dlight_test
//   clang-cl -TP -std:c++17 -W4 tests\vrhi_dlight_test.cpp /Fe:vrhi_dlight_test.exe && vrhi_dlight_test.exe
//
// Exit code 0 means every case passed.

#include "../code/renderervrhi/vrhi_dlight.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                        \
	do {                                                                   \
		++g_checks;                                                        \
		if (!(cond)) {                                                     \
			++g_failures;                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
		}                                                                  \
	} while (0)

#define CHECK_NEAR(a, b, eps)                                              \
	do {                                                                   \
		++g_checks;                                                        \
		const float va = (a);                                              \
		const float vb = (b);                                              \
		if (!(std::fabs(va - vb) <= (eps))) {                              \
			++g_failures;                                                  \
			std::printf("FAIL %s:%d: %s=%f !~ %s=%f (eps %f)\n",           \
				__FILE__, __LINE__, #a, static_cast<double>(va),           \
				#b, static_cast<double>(vb), static_cast<double>(eps));    \
		}                                                                  \
	} while (0)

VRHI_DLight makeLight(float x, float y, float z, float r, float g, float b,
	float radius, bool additive = false) {
	VRHI_DLight light = {};
	light.origin[0] = x;
	light.origin[1] = y;
	light.origin[2] = z;
	light.color[0] = r;
	light.color[1] = g;
	light.color[2] = b;
	light.radius = radius;
	light.additive = additive;
	return light;
}

void expectAdd(const VRHI_DLight *lights, std::size_t count,
	float px, float py, float pz, float er, float eg, float eb) {
	float add[3] = { 0.0f, 0.0f, 0.0f };
	VRHI_DLightAdd(lights, count, px, py, pz, add);
	CHECK_NEAR(add[0], er, 1.0e-5f);
	CHECK_NEAR(add[1], eg, 1.0e-5f);
	CHECK_NEAR(add[2], eb, 1.0e-5f);
}

void testValidation() {
	CHECK(VRHI_DLightValid(makeLight(0, 0, 0, 1, 0.5f, 0.25f, 100.0f)));
	// Non-finite origin components are invalid.
	VRHI_DLight nanOrigin = makeLight(0, 0, 0, 1, 1, 1, 100.0f);
	nanOrigin.origin[1] = std::nanf("");
	CHECK(!VRHI_DLightValid(nanOrigin));
	// Non-finite color channels are invalid.
	VRHI_DLight nanColor = makeLight(0, 0, 0, 1, 1, 1, 100.0f);
	nanColor.color[2] = std::nanf("");
	CHECK(!VRHI_DLightValid(nanColor));
	// Negative colors are invalid.
	VRHI_DLight negativeColor = makeLight(0, 0, 0, -0.1f, 1, 1, 100.0f);
	CHECK(!VRHI_DLightValid(negativeColor));
	// Zero, negative, and non-finite radii are invalid.
	CHECK(!VRHI_DLightValid(makeLight(0, 0, 0, 1, 1, 1, 0.0f)));
	CHECK(!VRHI_DLightValid(makeLight(0, 0, 0, 1, 1, 1, -5.0f)));
	VRHI_DLight nanRadius = makeLight(0, 0, 0, 1, 1, 1, 100.0f);
	nanRadius.radius = std::nanf("");
	CHECK(!VRHI_DLightValid(nanRadius));
	// The additive flag is retained, not validated away.
	CHECK(VRHI_DLightValid(makeLight(0, 0, 0, 1, 1, 1, 10.0f, true)));
}

void testFalloff() {
	const VRHI_DLight light = makeLight(0, 0, 0, 1, 1, 1, 10.0f);
	// At the origin the falloff is exactly 1.
	CHECK_NEAR(VRHI_DLightFalloff(light, 0, 0, 0), 1.0f, 0.0f);
	// Half the radius: falloff 0.5, squared 0.25.
	CHECK_NEAR(VRHI_DLightFalloff(light, 5, 0, 0), 0.25f, 1.0e-6f);
	// Three quarters of the radius: falloff 0.25, squared 0.0625.
	CHECK_NEAR(VRHI_DLightFalloff(light, 7.5f, 0, 0), 0.0625f, 1.0e-6f);
	// Exactly at the radius the influence ends (finite distance cutoff).
	CHECK_NEAR(VRHI_DLightFalloff(light, 10, 0, 0), 0.0f, 0.0f);
	// Beyond the radius there is no contribution.
	CHECK_NEAR(VRHI_DLightFalloff(light, 11, 0, 0), 0.0f, 0.0f);
	CHECK_NEAR(VRHI_DLightFalloff(light, 0, 0, 1000), 0.0f, 0.0f);
	// Off-axis distance is Euclidean.
	CHECK_NEAR(VRHI_DLightFalloff(light, 3, 4, 0), 0.25f, 1.0e-6f);
	// Invalid lights never contribute (zero falloff).
	CHECK_NEAR(VRHI_DLightFalloff(makeLight(0, 0, 0, 1, 1, 1, 0.0f), 0, 0, 0), 0.0f, 0.0f);
	// A huge but finite radius is fine (bounded by the caller's caps).
	const VRHI_DLight big = makeLight(0, 0, 0, 1, 1, 1, 100000.0f);
	CHECK_NEAR(VRHI_DLightFalloff(big, 50000, 0, 0), 0.25f, 1.0e-4f);
}

void testAdd() {
	// No lights and null input are zero no-ops.
	expectAdd(nullptr, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f);
	VRHI_DLight none[1] = {};
	expectAdd(none, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f);
	// Outside every light: zero.
	const VRHI_DLight light = makeLight(0, 0, 0, 1, 0.5f, 0.25f, 10.0f);
	expectAdd(&light, 1, 100, 100, 100, 0.0f, 0.0f, 0.0f);
	// At the origin: the full color.
	expectAdd(&light, 1, 0, 0, 0, 1.0f, 0.5f, 0.25f);
	// At half radius: color * 0.25.
	expectAdd(&light, 1, 5, 0, 0, 0.25f, 0.125f, 0.0625f);
	// Colors above 1.0 are clamped to the add cap, not to infinity.
	const VRHI_DLight hot = makeLight(0, 0, 0, 2.0f, 2.0f, 2.0f, 10.0f);
	expectAdd(&hot, 1, 0, 0, 0, VRHI_DLIGHT_ADD_CAP, VRHI_DLIGHT_ADD_CAP, VRHI_DLIGHT_ADD_CAP);
	// Multiple lights stack but the per-channel cap still applies.
	const VRHI_DLight lights[2] = {
		makeLight(0, 0, 0, 1, 1, 1, 10.0f),
		makeLight(10, 0, 0, 1, 1, 1, 10.0f)
	};
	expectAdd(lights, 2, 0, 0, 0, VRHI_DLIGHT_ADD_CAP, VRHI_DLIGHT_ADD_CAP, VRHI_DLIGHT_ADD_CAP);
	expectAdd(lights, 2, 5, 0, 0, 0.5f, 0.5f, 0.5f);
	// Additive and regular lights contribute identically.
	const VRHI_DLight additive = makeLight(0, 0, 0, 1, 1, 1, 10.0f, true);
	expectAdd(&additive, 1, 5, 0, 0, 0.25f, 0.25f, 0.25f);
	// Invalid lights in the array are skipped defensively.
	const VRHI_DLight mixed[2] = {
		makeLight(0, 0, 0, 1, 1, 1, 0.0f),
		makeLight(0, 0, 0, 0.5f, 0.5f, 0.5f, 10.0f)
	};
	expectAdd(mixed, 2, 0, 0, 0, 0.5f, 0.5f, 0.5f);
}

} // namespace

int main() {
	testValidation();
	testFalloff();
	testAdd();
	if (g_failures == 0) {
		std::printf("vrhi_dlight_test: all %d checks passed\n", g_checks);
		return 0;
	}
	std::printf("vrhi_dlight_test: %d/%d checks FAILED\n", g_failures, g_checks);
	return 1;
}
