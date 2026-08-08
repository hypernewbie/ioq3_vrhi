// Standalone, dependency-free unit test for the VRHI bounded beam-quad
// fallback helpers (code/renderervrhi/vrhi_beam.h). No engine headers, no
// filesystem, no third-party libraries: the test asserts the supported-type
// predicate and the bounded per-type width selection.
//
// Build and run (any C++11-or-later compiler):
//   clang++ -std=c++17 -O1 -Wall -Wextra -Werror tests/vrhi_beam_test.cpp -o vrhi_beam_test && ./vrhi_beam_test
//   clang-cl -TP -std:c++17 -W4 tests\vrhi_beam_test.cpp /Fe:vrhi_beam_test.exe && vrhi_beam_test.exe
//
// Exit code 0 means every case passed.

#include "../code/renderervrhi/vrhi_beam.h"

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

void testSupported() {
	// The four beam-quad fallback types are supported.
	CHECK(VRHI_BeamFallbackSupported(VRHI_BEAM_BEAM));
	CHECK(VRHI_BeamFallbackSupported(VRHI_BEAM_RAIL_CORE));
	CHECK(VRHI_BeamFallbackSupported(VRHI_BEAM_RAIL_RINGS));
	CHECK(VRHI_BeamFallbackSupported(VRHI_BEAM_LIGHTNING));
	// Every other engine reType stays a safe no-op, including portals.
	CHECK(!VRHI_BeamFallbackSupported(0)); // RT_MODEL
	CHECK(!VRHI_BeamFallbackSupported(1)); // RT_POLY
	CHECK(!VRHI_BeamFallbackSupported(2)); // RT_SPRITE
	CHECK(!VRHI_BeamFallbackSupported(7)); // RT_PORTALSURFACE
	CHECK(!VRHI_BeamFallbackSupported(8)); // RT_MAX_REF_ENTITY_TYPE
	CHECK(!VRHI_BeamFallbackSupported(-1));
	CHECK(!VRHI_BeamFallbackSupported(1234));
}

void testBeamWidth() {
	// RT_BEAM keeps the existing frame-scaled behavior: default 4.0 when
	// frame is unset or negative.
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, 0), 4.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, -3), 4.0f, 1.0e-6f);
	// frame > 0 scales linearly and is bounded below and above.
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, 1), 0.5f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, 2), 1.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, 8), 4.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, 100), 50.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, 100000), 4096.0f, 1.0e-6f);
	// The floor keeps tiny positive frames from collapsing the quad.
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, 1), 0.5f, 1.0e-6f);
	CHECK(VRHI_BeamFallbackWidth(VRHI_BEAM_BEAM, 1) >= 0.25f);
}

void testFixedTypeWidths() {
	// Fixed bounded widths per type, independent of frame.
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_LIGHTNING, 0), 8.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_LIGHTNING, 9000), 8.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_RAIL_CORE, 0), 6.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_RAIL_CORE, 9000), 6.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_RAIL_RINGS, 0), 16.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(VRHI_BEAM_RAIL_RINGS, 9000), 16.0f, 1.0e-6f);
	// Every width is finite and within the same 0.25..4096 bound used by
	// RT_BEAM, so hostile frame values cannot scale geometry unboundedly.
	CHECK(VRHI_BeamFallbackWidth(VRHI_BEAM_LIGHTNING, 0) >= 0.25f);
	CHECK(VRHI_BeamFallbackWidth(VRHI_BEAM_LIGHTNING, 0) <= 4096.0f);
	CHECK(VRHI_BeamFallbackWidth(VRHI_BEAM_RAIL_CORE, 0) >= 0.25f);
	CHECK(VRHI_BeamFallbackWidth(VRHI_BEAM_RAIL_CORE, 0) <= 4096.0f);
	CHECK(VRHI_BeamFallbackWidth(VRHI_BEAM_RAIL_RINGS, 0) >= 0.25f);
	CHECK(VRHI_BeamFallbackWidth(VRHI_BEAM_RAIL_RINGS, 0) <= 4096.0f);
}

void testUnsupportedWidths() {
	// Unsupported types answer 0.0; callers gate on the predicate first so
	// an unsupported width can never be applied to geometry.
	CHECK_NEAR(VRHI_BeamFallbackWidth(0, 0), 0.0f, 1.0e-6f);  // RT_MODEL
	CHECK_NEAR(VRHI_BeamFallbackWidth(2, 0), 0.0f, 1.0e-6f);  // RT_SPRITE
	CHECK_NEAR(VRHI_BeamFallbackWidth(7, 0), 0.0f, 1.0e-6f);  // RT_PORTALSURFACE
	CHECK_NEAR(VRHI_BeamFallbackWidth(8, 0), 0.0f, 1.0e-6f);
	CHECK_NEAR(VRHI_BeamFallbackWidth(1234, 0), 0.0f, 1.0e-6f);
}

} // namespace

int main() {
	testSupported();
	testBeamWidth();
	testFixedTypeWidths();
	testUnsupportedWidths();
	if (g_failures != 0) {
		std::printf("vrhi_beam_test: %d/%d checks FAILED\n", g_failures, g_checks);
		return 1;
	}
	std::printf("vrhi_beam_test: all %d checks passed\n", g_checks);
	return 0;
}
