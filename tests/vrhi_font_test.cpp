// Standalone, dependency-free unit test for the VRHI bounded fixed-cell
// bigchars font fallback helpers (code/renderervrhi/vrhi_font.h). No engine
// headers, no filesystem, no third-party libraries: the test computes glyph
// metrics in memory and asserts bounds, grid mapping, and UV validity.
//
// Build and run (any C++11-or-later compiler):
//   clang++ -std=c++17 -O1 -Wall -Wextra -Werror tests/vrhi_font_test.cpp -o vrhi_font_test && ./vrhi_font_test
//   clang-cl -TP -std:c++17 -W4 tests\vrhi_font_test.cpp /Fe:vrhi_font_test.exe && vrhi_font_test.exe
//
// Exit code 0 means every case passed.

#include "../code/renderervrhi/vrhi_font.h"

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

void testPointSizeBounds() {
	// Zero, negative, and tiny point sizes clamp to the minimum.
	CHECK(VRHI_FontClampPointSize(0) == VRHI_FONT_MIN_POINT_SIZE);
	CHECK(VRHI_FontClampPointSize(-12) == VRHI_FONT_MIN_POINT_SIZE);
	CHECK(VRHI_FontClampPointSize(1) == VRHI_FONT_MIN_POINT_SIZE);
	// Huge point sizes clamp to the maximum.
	CHECK(VRHI_FontClampPointSize(100000) == VRHI_FONT_MAX_POINT_SIZE);
	// In-range values are preserved.
	CHECK(VRHI_FontClampPointSize(8) == 8);
	CHECK(VRHI_FontClampPointSize(48) == 48);
	CHECK(VRHI_FontClampPointSize(128) == 128);
	// The bounded range is sane and ordered.
	CHECK(VRHI_FONT_MIN_POINT_SIZE > 0);
	CHECK(VRHI_FONT_MAX_POINT_SIZE > VRHI_FONT_MIN_POINT_SIZE);

	// The glyph scale is finite and positive across the whole bounded range.
	for (int ps = VRHI_FONT_MIN_POINT_SIZE; ps <= VRHI_FONT_MAX_POINT_SIZE;
		++ps) {
		const float scale = VRHI_FontGlyphScale(ps);
		CHECK(std::isfinite(scale));
		CHECK(scale > 0.0f);
	}
	// 48 points is the 1:1 reference: the 16px cell draws at 16 * 1.0.
	CHECK_NEAR(VRHI_FontGlyphScale(48), 1.0f, 1.0e-6f);
	// The scale is strictly decreasing with the (clamped) point size, so a
	// bigFont registration stays visibly larger than a smallFont one.
	CHECK(VRHI_FontGlyphScale(VRHI_FONT_MIN_POINT_SIZE) >
		VRHI_FontGlyphScale(VRHI_FONT_MAX_POINT_SIZE));
	CHECK(VRHI_FontGlyphScale(24) > VRHI_FontGlyphScale(72));
	// Out-of-range inputs stay inside the bounded scale interval.
	const float minScale = VRHI_FontGlyphScale(VRHI_FONT_MAX_POINT_SIZE);
	const float maxScale = VRHI_FontGlyphScale(VRHI_FONT_MIN_POINT_SIZE);
	const float lowScale = VRHI_FontGlyphScale(-999);
	const float highScale = VRHI_FontGlyphScale(999999);
	CHECK(lowScale >= minScale && lowScale <= maxScale);
	CHECK(highScale >= minScale && highScale <= maxScale);
}

void testCellUVs() {
	float s = 0.0f, t = 0.0f, s2 = 0.0f, t2 = 0.0f;
	// Invalid codepoints and null outputs are rejected without touching them.
	CHECK(!VRHI_BigCharsCell(-1, &s, &t, &s2, &t2));
	CHECK(!VRHI_BigCharsCell(256, &s, &t, &s2, &t2));
	CHECK(!VRHI_BigCharsCell(65, nullptr, &t, &s2, &t2));
	CHECK(!VRHI_BigCharsCell(65, &s, nullptr, &s2, &t2));

	// All 256 codepoints produce a valid, in-atlas UV rectangle of exactly
	// one fixed cell.
	for (int cp = 0; cp < 256; ++cp) {
		CHECK(VRHI_BigCharsCell(cp, &s, &t, &s2, &t2));
		CHECK(s >= 0.0f && t >= 0.0f && s2 <= 1.0f && t2 <= 1.0f);
		CHECK(s < s2 && t < t2);
		CHECK_NEAR(s2 - s, VRHI_FONT_CELL_UV, 1.0e-6f);
		CHECK_NEAR(t2 - t, VRHI_FONT_CELL_UV, 1.0e-6f);
		// UVs are on the 1/16 grid (exactly representable).
		CHECK_NEAR(s * VRHI_FONT_ATLAS_CELLS, (float)(cp & 15), 1.0e-6f);
		CHECK_NEAR(t * VRHI_FONT_ATLAS_CELLS, (float)(cp >> 4), 1.0e-6f);
	}

	// Codepoint 0 is the top-left cell.
	VRHI_BigCharsCell(0, &s, &t, &s2, &t2);
	CHECK_NEAR(s, 0.0f, 0.0f);
	CHECK_NEAR(t, 0.0f, 0.0f);
	// Codepoint 15 is the last cell of the first row.
	VRHI_BigCharsCell(15, &s, &t, &s2, &t2);
	CHECK_NEAR(s, 15.0f * VRHI_FONT_CELL_UV, 1.0e-6f);
	CHECK_NEAR(t, 0.0f, 0.0f);
	CHECK_NEAR(s2, 1.0f, 1.0e-6f);
	// Codepoint 255 is the last cell of the last row.
	VRHI_BigCharsCell(255, &s, &t, &s2, &t2);
	CHECK_NEAR(s, 15.0f * VRHI_FONT_CELL_UV, 1.0e-6f);
	CHECK_NEAR(t, 15.0f * VRHI_FONT_CELL_UV, 1.0e-6f);
	CHECK_NEAR(s2, 1.0f, 1.0e-6f);
	CHECK_NEAR(t2, 1.0f, 1.0e-6f);
	// 'A' (65) is row 4, column 1 of the classic charset grid.
	VRHI_BigCharsCell('A', &s, &t, &s2, &t2);
	CHECK_NEAR(s, 1.0f * VRHI_FONT_CELL_UV, 1.0e-6f);
	CHECK_NEAR(t, 4.0f * VRHI_FONT_CELL_UV, 1.0e-6f);

	// The fixed glyph metrics agree with the 16x16 cell geometry.
	CHECK(VRHI_FONT_GLYPH_IMAGE_WIDTH == VRHI_FONT_CELL_PIXELS);
	CHECK(VRHI_FONT_GLYPH_IMAGE_HEIGHT == VRHI_FONT_CELL_PIXELS);
	CHECK(VRHI_FONT_GLYPH_HEIGHT == VRHI_FONT_CELL_PIXELS);
	CHECK(VRHI_FONT_GLYPH_HEIGHT ==
		VRHI_FONT_GLYPH_BOTTOM - VRHI_FONT_GLYPH_TOP);
	CHECK(VRHI_FONT_GLYPH_PITCH == VRHI_FONT_CELL_PIXELS);
	// The advance is the full fixed-cell width, so drawn cells never overlap.
	CHECK(VRHI_FONT_GLYPH_XSKIP == VRHI_FONT_GLYPH_IMAGE_WIDTH);
}

} // namespace

int main() {
	testPointSizeBounds();
	testCellUVs();
	if (g_failures == 0) {
		std::printf("vrhi_font_test: all %d checks passed\n", g_checks);
		return 0;
	}
	std::printf("vrhi_font_test: %d/%d checks FAILED\n", g_failures, g_checks);
	return 1;
}
