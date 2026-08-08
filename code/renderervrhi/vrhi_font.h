/*
===========================================================================
VRHI bounded fixed-cell font fallback helpers (gfx/2d/bigchars).

Dependency-free (C++11) helpers shared by renderer_vrhi and its standalone
unit test. This is intentionally NOT proportional/FreeType font parity: it
describes only the classic Quake "bigchars" atlas. That atlas is a 256x256
image holding a 16x16 grid of fixed 16x16-pixel cells, mapped by codepoint
exactly like the engine's SCR_DrawSmallChar (row = cp >> 4, col = cp & 15),
and every glyph shares the single atlas shader handle. The point size is
bounded and only scales the fixed cell; there is no kerning, ascender
measurement, or per-glyph outline data.
===========================================================================
*/
#ifndef __VRHI_FONT_H
#define __VRHI_FONT_H

#include <cmath>

// gfx/2d/bigchars atlas geometry: 256x256 pixels organized as a 16x16 grid
// of fixed 16x16-pixel cells.
static constexpr int VRHI_FONT_ATLAS_CELLS = 16;
static constexpr int VRHI_FONT_CELL_PIXELS = 16;
static constexpr float VRHI_FONT_CELL_UV =
	1.0f / static_cast<float>(VRHI_FONT_ATLAS_CELLS);

// Bounded point size for the fixed-cell fallback. The client (UI/cgame) can
// register fonts with arbitrary menu-file point sizes; values outside this
// range are clamped so the glyph scale stays finite and bounded.
static constexpr int VRHI_FONT_MIN_POINT_SIZE = 8;
static constexpr int VRHI_FONT_MAX_POINT_SIZE = 128;

// Fixed glyph metrics for the bigchars fallback (scanlines and image pixels
// at native atlas resolution). xSkip is the full fixed-cell advance so
// consecutive cells never overlap when drawn at imageWidth x imageHeight.
static constexpr int VRHI_FONT_GLYPH_HEIGHT = VRHI_FONT_CELL_PIXELS;
static constexpr int VRHI_FONT_GLYPH_TOP = 0;
static constexpr int VRHI_FONT_GLYPH_BOTTOM = VRHI_FONT_CELL_PIXELS;
static constexpr int VRHI_FONT_GLYPH_PITCH = VRHI_FONT_CELL_PIXELS;
static constexpr int VRHI_FONT_GLYPH_XSKIP = VRHI_FONT_CELL_PIXELS;
static constexpr int VRHI_FONT_GLYPH_IMAGE_WIDTH = VRHI_FONT_CELL_PIXELS;
static constexpr int VRHI_FONT_GLYPH_IMAGE_HEIGHT = VRHI_FONT_CELL_PIXELS;

// Clamps an arbitrary registration point size into the bounded fallback
// range. Always returns a value in [VRHI_FONT_MIN_POINT_SIZE,
// VRHI_FONT_MAX_POINT_SIZE].
inline int VRHI_FontClampPointSize(int pointSize) {
	if (pointSize < VRHI_FONT_MIN_POINT_SIZE) {
		return VRHI_FONT_MIN_POINT_SIZE;
	}
	if (pointSize > VRHI_FONT_MAX_POINT_SIZE) {
		return VRHI_FONT_MAX_POINT_SIZE;
	}
	return pointSize;
}

// Glyph scale relative to the UI's 48-point reference convention, so a
// 48-point registration draws the fixed 16px cell at 1:1 (drawn size =
// imageWidth * useScale, useScale = uiScale * glyphScale). Always finite and
// positive because the point size is clamped first.
inline float VRHI_FontGlyphScale(int pointSize) {
	return 48.0f / static_cast<float>(VRHI_FontClampPointSize(pointSize));
}

// Maps one of the 256 atlas codepoints to its 16x16-cell UV rectangle in the
// bigchars atlas. Returns false (leaving the outputs untouched) for
// codepoints outside [0, 255] or null outputs.
inline bool VRHI_BigCharsCell(int codepoint, float *s, float *t, float *s2,
	float *t2) {
	if (codepoint < 0 || codepoint > 255 || s == nullptr || t == nullptr ||
		s2 == nullptr || t2 == nullptr) {
		return false;
	}
	const int col = codepoint & 15;
	const int row = codepoint >> 4;
	*s = static_cast<float>(col) * VRHI_FONT_CELL_UV;
	*t = static_cast<float>(row) * VRHI_FONT_CELL_UV;
	*s2 = *s + VRHI_FONT_CELL_UV;
	*t2 = *t + VRHI_FONT_CELL_UV;
	return true;
}

#endif // __VRHI_FONT_H
