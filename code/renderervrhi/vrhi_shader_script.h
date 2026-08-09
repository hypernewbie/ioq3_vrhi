/*
===========================================================================
VRHI bounded Quake shader-script parser.

Dependency-free (C++11) helper shared by renderer_vrhi and its standalone
unit test. It exposes two slices:

  - VRHI_ParseShaderScript / VRHI_ParseShaderBlock: the conservative
    first-stage opaque-diffuse lookup described below;
  - VRHI_ParseShaderScriptStages / VRHI_ParseSimpleShaderBlock: the bounded
    simple multi-stage material slice (at most VRHI_SHADER_MAX_STAGES
    direct-image stages, stage 0 opaque, later stages source-alpha or
    additive) used by static BSP batches.

Both share the same tokenizer and bounds. The first-stage helper parses the
small subset of Quake 3 shader scripts that is semantically equivalent to
the renderer's fixed opaque diffuse pass, so otherwise opaque first-stage
BSP shader definitions no longer fall back to the lightmap/solid path. It
is intentionally NOT a shader/material parser:

  - tokenization mirrors the GL renderers' GetToken loop: whitespace and
    NULs separate tokens, '//' line comments and slash-star block comments
    are skipped, '{'/'}' are single-character tokens, and double-quoted
    tokens may contain whitespace;
  - every list, text buffer, token count, and token length is bounded
    (constants below); malformed or oversized input is rejected safely;
  - a shader block contributes a candidate only when its stage(s) carry
    semantics identical to the fixed opaque diffuse pass: the default-value
    statements `blendFunc GL_ONE GL_ZERO` (exactly that pair), `rgbGen
    identity`, `alphaGen identity`, `depthWrite`, and `depthFunc lequal`/
    `less` are accepted only with their exact validated arguments, while
    alpha/additive blends, tcGen/tcMod variants, deform/fog/animMap/
    videoMap/normal/specular/portal/sky, and any malformed or incomplete
    sequence reject the whole shader;
  - statement validation never reads past the enclosing stage or shader
    body, so an incomplete statement at the end of a stage fails instead
    of borrowing tokens from the next block;
  - the first non-special `map`/`clampmap` TGA/JPG/JPEG/PNG candidate
    (skipping `$lightmap`/`$whiteimage` style special textures) is selected
    and retained as a copied string; a second real map anywhere in the
    shader rejects it, so multi-stage blending is never faked.

Every bound is a compile-time constant below. The parser copies only bounded
strings into caller-owned containers and never retains a pointer into the
input buffer, so no FS storage can be referenced after FS_FreeFile.
//
// The bounded static const-generator slice (`rgbGen const r g b` and
// `alphaGen const a`) extends only the simple multi-stage parser: constants
// are accepted when every value is finite and inside the defined safe range
// (VRHI_SCRIPT_GENCONST_* below), preserved per stage, and carried through the
// world/scene material and draw state so the pixel shader can tint the
// material sample without touching vertex colour, lightmaps, or blend
// classification. Every other generator keeps the existing dynamic fallback.
// The conservative first-stage helper still requires identity only.
//
// The bounded stage-local `depthWrite` slice extends the same simple
// multi-stage parser: the bare `depthWrite` keyword inside a stage sets a
// stage-local flag that is carried through the world/scene material and draw
// state. It never changes blend classification or sort order; the adapter
// keeps the default translucent behavior (depth test on, depth write off)
// and applies WRITE_Z only to stages/draws that explicitly declared
// depthWrite. Opaque draws keep their existing depth-write behavior, so a
// shader without the keyword renders exactly as before.
//
// The bounded stage-local `alphaFunc` slice extends the same simple
// multi-stage parser with exactly three Quake3 alpha-test comparisons,
// matched case-insensitively: `GE128`, `GT0`, and `LT128`. The parsed mode is
// carried through the world, inline-BSP, and scene shader-cache/draw paths so
// the textured material pixel shaders can discard/cut out fragments that fail
// the comparison on the material sample alpha. Every other `alphaFunc` value
// (and a truncated statement at the end of a stage) retains the current safe
// behavior: the stage stays usable and alpha testing stays OFF, exactly as
// before this slice existed, so an otherwise usable stage is never rejected.
// The slice never changes alpha-blend modes, the bounded color/alpha const
// generators, vertex colour, or lightmap behavior. The conservative first-stage
// helper still treats `alphaFunc` as unsupported and rejects such shaders.
//
// The bounded stage-local `clampMap` slice extends the same simple multi-stage
// parser: the Quake3 `clampMap` keyword is distinguished from `map` and sets a
// stage-local flag, while the qpath validation is byte-for-byte the same safe
// validation used for `map` (VRHI_ShaderPathIsSafe plus
// VRHI_ResolveDirectImagePath, so traversal/drive/backslash/unsupported-
// extension names reject identically). The flag is carried through the world,
// inline-BSP, and scene shader-cache/draw paths, where the adapter selects the
// UVW_CLAMP diffuse sampler flags for stages/draws that declared `clampMap`
// and keeps the existing UVW_WRAP sampler for every default `map` stage, so a
// shader without the keyword samples exactly as before. The flag never changes
// blend modes, const generators, vertex colour, lightmaps, depth, or alpha
// testing. Direct-image registrations carry no clamp flag (default `map`
// wrapping semantics) and the UI path keeps its existing always-clamped
// sampler, so both retain their current behavior. The conservative first-stage
// helper continues to accept `clampmap` as a candidate with the same safe
// qpath validation (it returns only a path string and carries no sampler
// flag). `animMap` has no clamp variant and keeps its existing first-frame
// static fallback with the default wrap sampler.
//
// The bounded stage-local `tcMod scale` slice extends the same simple
// multi-stage parser with exactly the Quake3 static `tcMod scale s t`
// statement: both components must be finite, nonzero, and inside the defined
// safe magnitude range (VRHI_SCRIPT_TCMOD_SCALE_* below), so a zero, huge, or
// non-numeric scale can never collapse or explode sampling. The parsed scale
// is carried through the world, inline-BSP, and scene shader-cache/draw paths
// so the textured material pixel shaders scale the diffuse (material) UVs by
// (s, t) before sampling. It is applied ONLY to the diffuse/material sample;
// lightmap UVs, UI textures, vertex colour, blend classification, const
// generators, depth, and alpha testing are never modified. Every other tcMod
// variant (scroll, rotate, turb, stretch, ...) and every malformed, truncated,
// zero, non-finite, or out-of-range scale keeps the existing fallback: the
// stage stays accepted and samples with identity UVs exactly as before this
// slice, and the statement never borrows tokens past the stage end. The
// conservative first-stage helper still treats `tcMod` as unsupported and
// rejects such shaders.
//
// The bounded stage-local `tcMod scroll` slice extends the same simple
// multi-stage parser with exactly the Quake3 time-based `tcMod scroll s t`
// statement: both components must be finite and |value| <= the defined safe
// magnitude bound (VRHI_SCRIPT_TCMOD_SCROLL_MAX below); zero is accepted
// because a zero rate is the identity no-scroll case and the default. The
// parsed rates are carried through the world, inline-BSP, and scene
// shader-cache/draw paths, and the textured material pixel shaders receive
// the current refdef/render time in seconds so they scroll the diffuse
// (material) UVs by rate * time after the static tcMod scale is applied
// (scale then time scroll), wrapping the offset into [0,1) exactly like the
// GL1 renderer's RB_CalcScrollTexCoords. World and scene draws share one
// per-frame time value and the scroll uniform is bound only to the textured
// diffuse/overlay material programs, never to the solid/lightmap fallbacks.
// It is applied ONLY to the diffuse/material sample; lightmap UVs, UI
// textures, vertex colour, blend classification, const generators, depth,
// and alpha testing are never modified. Every other tcMod variant (rotate,
// turb, stretch, ...) and every malformed, truncated, non-finite, or
// out-of-range scroll keeps the existing fallback: the stage stays accepted
// and samples with no scroll exactly as before this slice, and the statement
// never borrows tokens past the stage end. The conservative first-stage
// helper still treats `tcMod` as unsupported and rejects such shaders.
//
// The bounded stage-local `tcMod rotate` slice extends the same simple
// multi-stage parser with exactly the Quake3 time-based `tcMod rotate
// degsPerSecond` statement: the single rate must be finite and |value| <=
// the defined safe magnitude bound (VRHI_SCRIPT_TCMOD_ROTATE_MAX below,
// degrees per second); zero is accepted because a zero rate is the identity
// no-rotate case and the default. The parsed rate is carried through the
// world, inline-BSP, and scene shader-cache/draw paths, and the textured
// material pixel shaders receive the current refdef/render time in seconds
// so they rotate the diffuse (material) UVs around the texture center
// (0.5, 0.5) by -rate * frameTimeSeconds degrees, matching the GL1
// renderer's RB_CalcRotateTexCoords sign and units exactly (degs =
// -degsPerSecond * shaderTime, then rotate about (0.5, 0.5); the angle is
// wrapped into [0,360) degrees first like the GL1 sine-table mask so long
// sessions cannot erode sin/cos precision). The fixed documented order when
// a stage carries several supported tcMods is: static tcMod scale first
// (about the UV origin), then tcMod rotate about the texture center, then
// the time-based tcMod scroll offset (wrapped into [0,1) like the GL1
// RB_CalcScrollTexCoords); script order is intentionally NOT tracked, so
// every combination renders with exactly this one deterministic
// composition. World and scene draws share one per-frame time value and the
// rotate uniform is bound only to the textured diffuse/overlay material
// programs, never to the solid/lightmap fallbacks. It is applied ONLY to
// the diffuse/material sample; lightmap UVs, UI textures, vertex colour,
// blend classification, const generators, depth, and alpha testing are
// never modified. Every other tcMod variant (turb, stretch, ...) and every
// malformed, truncated, non-finite, or out-of-range rate keeps the existing
// fallback: the stage stays accepted and samples with no rotation exactly
// as before this slice, and the statement never borrows tokens past the
// stage end. The conservative first-stage helper still treats `tcMod` as
// unsupported and rejects such shaders.
===========================================================================
*/
#ifndef __VRHI_SHADER_SCRIPT_H
#define __VRHI_SHADER_SCRIPT_H

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

// Bounded simple material stages accepted by the VRHI BSP path.
static const std::size_t VRHI_SHADER_MAX_STAGES = 4;
enum VRHI_ShaderBlendMode {
	VRHI_SHADER_BLEND_OPAQUE = 0,
	VRHI_SHADER_BLEND_ALPHA = 1,
	VRHI_SHADER_BLEND_ADDITIVE = 2,
	// Quake3 `blendFunc filter`: destination-colour modulation. This is
	// intentionally separate from source-alpha so water/filter materials do
	// not silently become additive.
	VRHI_SHADER_BLEND_FILTER = 3,
	// Quake3 `blendFunc GL_DST_COLOR GL_SRC_COLOR`: colour multiply,
	// deliberately distinct from filter (GL_DST_COLOR GL_ZERO).
	VRHI_SHADER_BLEND_MULTIPLY = 4,
	// Quake3 `blendFunc GL_ONE GL_ONE_MINUS_SRC_ALPHA`: premultiplied-alpha
	// compositing (src*1 + dst*(1-srcA)).
	VRHI_SHADER_BLEND_PREMULTIPLIED = 5,
	// Quake3 `blendFunc GL_SRC_ALPHA GL_ONE`: additive glow scaled by source
	// alpha, distinct from plain additive (GL_ONE GL_ONE).
	VRHI_SHADER_BLEND_ALPHA_ADDITIVE = 6
};
// Bounded Quake3 stage alpha-test (`alphaFunc`) comparisons accepted by the
// simple multi-stage parser. Exactly GE128, GT0, and LT128 are supported
// (case-insensitive); every other value keeps the OFF (no discard) behavior.
enum VRHI_ShaderAlphaFunc {
	// No alpha testing: the stage renders exactly as before this slice.
	VRHI_SHADER_ALPHAFUNC_OFF = 0,
	// `alphaFunc GE128`: discard fragments whose alpha < 128/255.
	VRHI_SHADER_ALPHAFUNC_GE128 = 1,
	// `alphaFunc GT0`: discard fragments whose alpha <= 0.
	VRHI_SHADER_ALPHAFUNC_GT0 = 2,
	// `alphaFunc LT128`: discard fragments whose alpha >= 128/255.
	VRHI_SHADER_ALPHAFUNC_LT128 = 3
};
struct VRHI_ShaderStage {
	std::string path;
	int blendMode = VRHI_SHADER_BLEND_OPAQUE;
	// Bounded static const generators. rgbGen const r g b tints the stage
	// texture RGB; alphaGen const a scales the stage texture alpha (both match
	// the GL1 constant-colour modulate semantics). Only finite constants inside
	// the defined safe range are accepted by the simple-stage parser; every
	// other generator keeps the existing dynamic fallback (flags stay false,
	// the stage renders with identity colour).
	bool rgbGenConst = false;
	float rgbConst[3] = { 1.0f, 1.0f, 1.0f };
	bool alphaGenConst = false;
	float alphaConst = 1.0f;
	// Bounded per-stage depthWrite (the bare `depthWrite` keyword inside the
	// stage): the stage-local flag is carried into the world/scene draw state.
	// The adapter's default translucent behavior is depth-test-on/
	// depth-write-off; WRITE_Z is applied only when this flag is set. Opaque
	// draws keep their existing depth-write behavior.
	bool depthWrite = false;
	// Bounded stage-local alpha-test (`alphaFunc`) mode: exactly GE128, GT0,
	// and LT128 (case-insensitive) are accepted; any other value keeps this
	// OFF so the stage renders with no discard, exactly as before this slice.
	int alphaFunc = VRHI_SHADER_ALPHAFUNC_OFF;
	// Bounded stage-local `clampMap` flag: true only when the stage's map
	// statement used the Quake3 `clampMap` keyword. The adapter selects the
	// CLAMP diffuse sampler for this stage and keeps the default WRAP sampler
	// for every `map` stage, so a shader without the keyword samples exactly
	// as before. The flag never changes blend modes, const generators, vertex
	// colour, lightmaps, depth, or alpha testing.
	bool clampMap = false;
	// Bounded stage-local static `tcMod scale s t`: true only when the stage's
	// tcMod statement used the Quake3 `scale` variant with two finite, nonzero,
	// safely bounded components. The scale is carried into the world/scene
	// draw state and applied in the textured material pixel shaders to the
	// diffuse (material) UVs only; lightmap UVs, UI textures, vertex colour,
	// blend modes, const generators, depth, and alpha testing are never
	// modified. Stages without the keyword (or with any other tcMod variant
	// such as scroll, rotate, turb, or stretch) keep identity UVs exactly as
	// before. Negative (mirrored) scales inside the safe magnitude range are
	// accepted like the GL1 renderer's TCModScale.
	bool tcModScale = false;
	float tcModScaleS = 1.0f;
	float tcModScaleT = 1.0f;
	// Bounded stage-local time-based `tcMod scroll s t`: true only when the
	// stage's tcMod statement used the Quake3 `scroll` variant with two
	// finite, safely bounded components (|value| <=
	// VRHI_SCRIPT_TCMOD_SCROLL_MAX; zero is accepted as the identity
	// no-scroll rate and is the default). The rates are carried into the
	// world/scene draw state and the textured material pixel shaders scroll
	// the diffuse (material) UVs by rate * frameTimeSeconds after the static
	// tcMod scale is applied (scale then time scroll); lightmap UVs, UI
	// textures, vertex colour, blend modes, const generators, depth, and
	// alpha testing are never modified. Stages without the keyword (or with
	// any other tcMod variant such as rotate, turb, or stretch) keep the
	// zero-scroll fallback exactly as before. A stage may carry both
	// tcModScale and tcModScroll; the shader applies the scale first, then
	// the time scroll.
	bool tcModScroll = false;
	float tcModScrollS = 0.0f;
	float tcModScrollT = 0.0f;
	// Bounded stage-local time-based `tcMod rotate degsPerSecond`: true only
	// when the stage's tcMod statement used the Quake3 `rotate` variant with
	// one finite, safely bounded rate (|value| <= VRHI_SCRIPT_TCMOD_ROTATE_MAX
	// degrees per second; zero is accepted as the identity no-rotate rate and
	// is the default). The rate is carried into the world/scene draw state
	// and the textured material pixel shaders rotate the diffuse (material)
	// UVs around the texture center (0.5, 0.5) by -rate * frameTimeSeconds
	// degrees exactly like the GL1 renderer's RB_CalcRotateTexCoords;
	// lightmap UVs, UI textures, vertex colour, blend modes, const
	// generators, depth, and alpha testing are never modified. Stages without
	// the keyword (or with any other tcMod variant such as turb or stretch)
	// keep the no-rotate fallback exactly as before. A stage may carry
	// tcModScale, tcModRotate, and tcModScroll together; the fixed
	// documented order is static scale, then rotate about the texture
	// center, then time scroll.
	bool tcModRotate = false;
	float tcModRotateDegreesPerSecond = 0.0f;
};

// Strict bounds, independent of the legacy qfiles.h limits so malformed or
// hostile files cannot consume unbounded memory inside the renderer. The
// qpath bound mirrors the engine's MAX_QPATH (64).
static const std::size_t VRHI_SCRIPT_MAX_QPATH = 64;
static const std::size_t VRHI_SCRIPT_MAX_TEXT_BYTES = 8u * 1024u * 1024u;
static const std::size_t VRHI_SCRIPT_MAX_TOKENS = 131072;
static const std::size_t VRHI_SCRIPT_MAX_TOKEN_BYTES = 1024;

// Bounded static const-generator range (rgbGen const / alphaGen const). The
// GL1 renderer stores the generator as a byte (255 * value), so [0,1] is the
// round-trip-safe domain for every component: the rgbGen const components and
// the alphaGen const value must all be inside [0,1] to match the GL1
// byte-colour semantics exactly. Non-finite, malformed, or out-of-range
// constants are not accepted and keep the dynamic fallback.
static const float VRHI_SCRIPT_GENCONST_MIN = 0.0f;
static const float VRHI_SCRIPT_GENCONST_RGB_MAX = 1.0f;
static const float VRHI_SCRIPT_GENCONST_ALPHA_MAX = 1.0f;

// Bounded static `tcMod scale` range. Each component must be finite, nonzero,
// and |value| inside [MIN, MAX]; zero would collapse every diffuse UV to a
// single texel and extreme magnitudes would alias/explode sampling, so both
// keep the existing identity-UV fallback. The range comfortably covers every
// practical static scale used by Quake3 maps (for example 0.25, 0.5, 2, 4)
// while remaining numerically safe for the pixel-shader multiply.
static const float VRHI_SCRIPT_TCMOD_SCALE_MIN = 1.0f / 64.0f;
static const float VRHI_SCRIPT_TCMOD_SCALE_MAX = 64.0f;

// Bounded `tcMod scroll` rate range. Each component must be finite and
// |value| <= VRHI_SCRIPT_TCMOD_SCROLL_MAX; zero is accepted (a zero rate is
// the identity no-scroll case and the default). The bound comfortably covers
// every practical Quake3 scroll rate (commonly 0.01..2 for water/lava/light
// sweeps) while keeping rate * frameTimeSeconds numerically safe for the
// pixel-shader multiply and its [0,1) wrap, so extreme magnitudes can never
// smear or explode the material sampling.
static const float VRHI_SCRIPT_TCMOD_SCROLL_MAX = 16.0f;

// Bounded `tcMod rotate` rate range (degrees per second). The rate must be
// finite and |value| <= VRHI_SCRIPT_TCMOD_ROTATE_MAX; zero is accepted (a
// zero rate is the identity no-rotate case and the default). The bound
// comfortably covers every practical Quake3 rotate rate (commonly 15..540
// degrees per second for spinning fans/teleporters/sky swirls) while keeping
// rate * frameTimeSeconds numerically safe for the pixel-shader angle wrap
// (the angle is wrapped into [0,360) degrees like the GL1 sine-table mask,
// so extreme magnitudes can never erode the sin/cos precision).
static const float VRHI_SCRIPT_TCMOD_ROTATE_MAX = 3600.0f;

struct VRHI_ShaderScriptToken {
	std::string text;
};

inline std::string VRHI_ShaderLowerASCII(const std::string &text) {
	std::string lower = text;
	for (std::size_t i = 0; i < lower.size(); ++i) {
		lower[i] = static_cast<char>(
			std::tolower(static_cast<unsigned char>(lower[i])));
	}
	return lower;
}

inline bool VRHI_ShaderTokenIs(const VRHI_ShaderScriptToken &token,
	const char *value) {
	return VRHI_ShaderLowerASCII(token.text) == value;
}

// Parses one token as a finite numeric constant inside [minValue, maxValue].
// Returns false for empty, non-numeric, partially consumed (trailing junk),
// non-finite, or out-of-range tokens, so the const-generator slice accepts
// only exact in-range constants. Locale behaviour mirrors the original
// renderer's atof-based ParseVector path.
inline bool VRHI_ShaderParseFiniteConstant(const std::string &text,
	float minValue, float maxValue, float *value) {
	if (value == nullptr || text.empty()) return false;
	char *end = nullptr;
	errno = 0;
	const float parsed = std::strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
		!std::isfinite(parsed) || parsed < minValue || parsed > maxValue)
		return false;
	*value = parsed;
	return true;
}

// Parses one token as a bounded static `tcMod scale` component: finite,
// nonzero, and |value| inside [VRHI_SCRIPT_TCMOD_SCALE_MIN,
// VRHI_SCRIPT_TCMOD_SCALE_MAX]. Negative (mirrored) scales are accepted like
// the GL1 renderer's TCModScale (st *= scale). Returns false for empty,
// non-numeric, partially consumed (trailing junk), non-finite, zero, or
// out-of-range tokens, so the tcMod scale slice accepts only exact in-range
// components and every other value keeps the identity-UV fallback. Locale
// behaviour mirrors the original renderer's atof-based ParseVector path.
inline bool VRHI_ShaderParseTcModScale(const std::string &text, float *value) {
	if (value == nullptr || text.empty()) return false;
	char *end = nullptr;
	errno = 0;
	const float parsed = std::strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
		!std::isfinite(parsed) || parsed == 0.0f) return false;
	const float magnitude = std::fabs(parsed);
	if (magnitude < VRHI_SCRIPT_TCMOD_SCALE_MIN ||
		magnitude > VRHI_SCRIPT_TCMOD_SCALE_MAX) return false;
	*value = parsed;
	return true;
}

// Parses one token as a bounded `tcMod scroll` component: finite and
// |value| <= VRHI_SCRIPT_TCMOD_SCROLL_MAX (zero is accepted: a zero rate is
// the identity no-scroll case). Returns false for empty, non-numeric,
// partially consumed (trailing junk), non-finite, or out-of-range tokens, so
// the tcMod scroll slice accepts only exact in-range components and every
// other value keeps the existing no-scroll fallback. Locale behaviour
// mirrors the original renderer's atof-based ParseVector path.
inline bool VRHI_ShaderParseTcModScroll(const std::string &text, float *value) {
	if (value == nullptr || text.empty()) return false;
	char *end = nullptr;
	errno = 0;
	const float parsed = std::strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
		!std::isfinite(parsed)) return false;
	if (std::fabs(parsed) > VRHI_SCRIPT_TCMOD_SCROLL_MAX) return false;
	*value = parsed;
	return true;
}

// Parses one token as a bounded `tcMod rotate` rate in degrees per second:
// finite and |value| <= VRHI_SCRIPT_TCMOD_ROTATE_MAX (zero is accepted: a
// zero rate is the identity no-rotate case). Returns false for empty,
// non-numeric, partially consumed (trailing junk), non-finite, or
// out-of-range tokens, so the tcMod rotate slice accepts only exact in-range
// rates and every other value keeps the existing no-rotate fallback. Locale
// behaviour mirrors the original renderer's atof-based ParseVector path.
inline bool VRHI_ShaderParseTcModRotate(const std::string &text, float *value) {
	if (value == nullptr || text.empty()) return false;
	char *end = nullptr;
	errno = 0;
	const float parsed = std::strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
		!std::isfinite(parsed)) return false;
	if (std::fabs(parsed) > VRHI_SCRIPT_TCMOD_ROTATE_MAX) return false;
	*value = parsed;
	return true;
}

// Tokenizes bounded shader-script text into single tokens. Returns false for
// null input, an unclosed block comment or quote, an overlong token, or a
// token count beyond VRHI_SCRIPT_MAX_TOKENS.
inline bool VRHI_TokenizeShaderScript(const unsigned char *data, std::size_t size,
	std::vector<VRHI_ShaderScriptToken> *tokens) {
	if (data == nullptr || tokens == nullptr) return false;
	tokens->clear();
	std::size_t cursor = 0;
	while (cursor < size) {
		const unsigned char ch = data[cursor];
		if (std::isspace(ch) || ch == '\0') {
			++cursor;
			continue;
		}
		if (ch == '/' && cursor + 1 < size && data[cursor + 1] == '/') {
			cursor += 2;
			while (cursor < size && data[cursor] != '\n') ++cursor;
			continue;
		}
		if (ch == '/' && cursor + 1 < size && data[cursor + 1] == '*') {
			cursor += 2;
			bool closed = false;
			while (cursor + 1 < size) {
				if (data[cursor] == '*' && data[cursor + 1] == '/') {
					cursor += 2;
					closed = true;
					break;
				}
				++cursor;
			}
			if (!closed) return false;
			continue;
		}
		if (tokens->size() >= VRHI_SCRIPT_MAX_TOKENS) return false;
		VRHI_ShaderScriptToken token;
		if (ch == '{' || ch == '}') {
			token.text.assign(1, static_cast<char>(ch));
			++cursor;
		} else if (ch == '"') {
			++cursor;
			while (cursor < size && data[cursor] != '"') {
				if (data[cursor] == '\\' && cursor + 1 < size) ++cursor;
				if (cursor >= size ||
					token.text.size() >= VRHI_SCRIPT_MAX_TOKEN_BYTES) return false;
				token.text.push_back(static_cast<char>(data[cursor++]));
			}
			if (cursor >= size) return false;
			++cursor;
		} else {
			while (cursor < size && !std::isspace(data[cursor]) &&
				data[cursor] != '{' && data[cursor] != '}') {
				if (token.text.size() >= VRHI_SCRIPT_MAX_TOKEN_BYTES) return false;
				token.text.push_back(static_cast<char>(data[cursor++]));
			}
		}
		tokens->push_back(std::move(token));
	}
	return true;
}

// Validates a shader/texture qpath: non-empty, shorter than
// VRHI_SCRIPT_MAX_QPATH, no drive letter or traversal, and no empty/'.'/'..'
// path parts. Used for both script paths and map/clampmap texture names.
inline bool VRHI_ShaderPathIsSafe(const std::string &path) {
	if (path.empty() || path.size() >= VRHI_SCRIPT_MAX_QPATH || path[0] == '/' ||
		path[0] == '\\' || path.find(':') != std::string::npos) return false;
	std::size_t begin = 0;
	while (begin <= path.size()) {
		const std::size_t end = path.find('/', begin);
		const std::string part = path.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin);
		if (part.empty() || part == "." || part == "..") return false;
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return true;
}

// Resolves a direct-image name into a retained qpath string. Explicit
// extensions must be tga/jpg/jpeg/png; a bare name is retained as-is and the
// bounded loader probes the four supported suffixes later. Returns false for
// unsafe, empty, overlong, or unsupported-extension names.
inline bool VRHI_ResolveDirectImagePath(const char *name, std::string *resolved) {
	if (resolved != nullptr) resolved->clear();
	if (name == nullptr || resolved == nullptr) return false;
	std::size_t length = 0;
	while (length < VRHI_SCRIPT_MAX_QPATH && name[length] != '\0') ++length;
	if (length == 0 || length >= VRHI_SCRIPT_MAX_QPATH ||
		name[0] == '/' || name[0] == '\\') return false;
	const std::string path(name, length);
	if (path.find("..") != std::string::npos) return false;
	const std::size_t slash = path.find_last_of("/\\");
	const std::size_t dot = path.find_last_of('.');
	const bool hasExtension = dot != std::string::npos &&
		(slash == std::string::npos || dot > slash) && dot + 1 < path.size();
	if (hasExtension) {
		std::string ext = path.substr(dot + 1);
		for (std::size_t i = 0; i < ext.size(); ++i) {
			ext[i] = static_cast<char>(
				std::tolower(static_cast<unsigned char>(ext[i])));
		}
		if (ext != "tga" && ext != "jpg" && ext != "jpeg" && ext != "png")
			return false;
	}
	// A bare name is intentionally retained as-is. The bounded loader below
	// probes the four supported suffixes; explicit extensions are never
	// rewritten.
	*resolved = std::move(path);
	return true;
}

// Token-level keywords that reject a shader even when a valid map is present.
// The allowlisted default-value statements (blendFunc GL_ONE GL_ZERO, rgbGen/
// alphaGen identity, depthWrite, depthFunc lequal/less) are validated
// separately by VRHI_ShaderAllowlistedStatement and are intentionally absent
// from this list.
inline bool VRHI_ShaderTokenIsUnsupported(const VRHI_ShaderScriptToken &token) {
	const std::string lower = VRHI_ShaderLowerASCII(token.text);
	static const char *unsupported[] = {
		"blend", "deform", "deformvertexes", "fogparms", "foggen",
		"alphafunc", "tcgen", "tcmod",
		"animmap", "videomap", "normalmap", "specularmap",
		"polygonoffset", "portal", "skyparms"
	};
	for (const char *value : unsupported) {
		if (lower == value) return true;
	}
	return false;
}

// Validates one allowlisted default-value statement inside `[pos, scopeEnd)`.
// Returns 0 when `pos` is not an allowlisted keyword (the caller keeps its
// normal unsupported/skip policy), SIZE_MAX when the keyword is allowlisted
// but its arguments are missing or unsupported (the whole shader must be
// rejected), and 1..3 when the statement is syntactically complete and
// semantically equivalent to the fixed opaque diffuse pass; the return value
// is the number of tokens the statement consumed. Never reads past scopeEnd.
inline std::size_t VRHI_ShaderAllowlistedStatement(
	const std::vector<VRHI_ShaderScriptToken> &tokens, std::size_t pos,
	std::size_t scopeEnd) {
	if (pos >= scopeEnd || scopeEnd > tokens.size()) return 0;
	const std::string lower = VRHI_ShaderLowerASCII(tokens[pos].text);
	if (lower == "depthwrite") return 1;
	if (lower == "rgbgen" || lower == "alphagen") {
		if (pos + 1 >= scopeEnd) return std::numeric_limits<std::size_t>::max();
		if (VRHI_ShaderLowerASCII(tokens[pos + 1].text) != "identity")
			return std::numeric_limits<std::size_t>::max();
		return 2;
	}
	if (lower == "depthfunc") {
		if (pos + 1 >= scopeEnd) return std::numeric_limits<std::size_t>::max();
		const std::string arg = VRHI_ShaderLowerASCII(tokens[pos + 1].text);
		if (arg != "lequal" && arg != "less")
			return std::numeric_limits<std::size_t>::max();
		return 2;
	}
	if (lower == "blendfunc") {
		if (pos + 2 >= scopeEnd) return std::numeric_limits<std::size_t>::max();
		if (VRHI_ShaderLowerASCII(tokens[pos + 1].text) != "gl_one" ||
			VRHI_ShaderLowerASCII(tokens[pos + 2].text) != "gl_zero")
			return std::numeric_limits<std::size_t>::max();
		return 3;
	}
	return 0;
}

// Parses one shader body `[begin, end)` and returns the first non-special
// map/clampmap candidate (a copied safe qpath), or false when the body uses
// any unsupported statement, any malformed/incomplete statement, no usable
// map, or more than one real map anywhere in the body.
inline bool VRHI_ParseShaderBlock(const std::vector<VRHI_ShaderScriptToken> &tokens,
	std::size_t begin, std::size_t end, std::string *candidate) {
	if (candidate != nullptr) candidate->clear();
	if (candidate == nullptr || begin >= end || end > tokens.size()) return false;
	bool found = false;
	for (std::size_t i = begin; i < end;) {
		if (tokens[i].text != "{") {
			const std::size_t consumed = VRHI_ShaderAllowlistedStatement(tokens, i, end);
			if (consumed == std::numeric_limits<std::size_t>::max()) return false;
			if (consumed > 0) {
				i += consumed;
				continue;
			}
			if (VRHI_ShaderTokenIsUnsupported(tokens[i])) return false;
			++i;
			continue;
		}
		const std::size_t stageBegin = ++i;
		int depth = 1;
		while (i < end && depth > 0) {
			if (tokens[i].text == "{") ++depth;
			else if (tokens[i].text == "}") --depth;
			++i;
		}
		if (depth != 0) return false;
		const std::size_t stageEnd = i - 1;
		std::size_t j = stageBegin;
		while (j < stageEnd) {
			if (VRHI_ShaderTokenIs(tokens[j], "map") ||
				VRHI_ShaderTokenIs(tokens[j], "clampmap")) {
				if (j + 1 >= stageEnd) return false;
				const std::string &texture = tokens[j + 1].text;
				j += 2;
				if (!texture.empty() && texture[0] == '$') continue;
				if (!VRHI_ShaderPathIsSafe(texture) ||
					texture.find('\\') != std::string::npos) return false;
				std::string path;
				if (found || !VRHI_ResolveDirectImagePath(texture.c_str(), &path))
					return false;
				*candidate = std::move(path);
				found = true;
				continue;
			}
			const std::size_t consumed = VRHI_ShaderAllowlistedStatement(tokens, j, stageEnd);
			if (consumed == std::numeric_limits<std::size_t>::max()) return false;
			if (consumed > 0) {
				j += consumed;
				continue;
			}
			if (VRHI_ShaderTokenIsUnsupported(tokens[j])) return false;
			++j;
		}
	}
	return found;
}

// Parses one bounded shader-script text buffer, extracting the first-stage
// opaque candidate for every shader name present in `wanted` (compared
// case-insensitively) into `results`. Returns false only for null input,
// tokenization failure, or unbalanced braces; blocks that reject simply
// produce no result entry.
inline bool VRHI_ParseShaderScript(const unsigned char *data, std::size_t size,
	const std::unordered_map<std::string, bool> &wanted,
	std::unordered_map<std::string, std::string> *results) {
	std::vector<VRHI_ShaderScriptToken> tokens;
	if (!VRHI_TokenizeShaderScript(data, size, &tokens) || results == nullptr)
		return false;
	for (std::size_t i = 0; i < tokens.size();) {
		const std::string shaderName = VRHI_ShaderLowerASCII(tokens[i++].text);
		if (i >= tokens.size() || tokens[i].text != "{") continue;
		const std::size_t bodyBegin = ++i;
		int depth = 1;
		while (i < tokens.size() && depth > 0) {
			if (tokens[i].text == "{") ++depth;
			else if (tokens[i].text == "}") --depth;
			++i;
		}
		if (depth != 0) return false;
		const std::size_t bodyEnd = i - 1;
		if (wanted.find(shaderName) == wanted.end()) continue;
		std::string path;
		if (VRHI_ParseShaderBlock(tokens, bodyBegin, bodyEnd, &path)) {
			results->emplace(shaderName, std::move(path));
		}
	}
	return true;
}

// Parses one bounded shader body for the deliberately small multi-stage
// material slice. Every stage has exactly one bounded direct-image map (or
// the first frame of an animMap), and only the blend mode affects output. The
// parser ignores bounded stage/top-level directives whose geometry/UV/time
// semantics are not implemented, preserving a visible static-material fallback
// instead of returning the green solid placeholder. Stage zero may be opaque,
// source-alpha, additive, alpha-additive, multiply, or premultiplied alpha;
// later stages without an explicit blendFunc become static source-alpha
// overlays. This remains separate from the conservative first-stage helper
// above.
inline bool VRHI_ParseSimpleShaderBlock(
	const std::vector<VRHI_ShaderScriptToken> &tokens, std::size_t begin,
	std::size_t end, std::vector<VRHI_ShaderStage> *stages) {
	if (stages != nullptr) stages->clear();
	if (stages == nullptr || begin >= end || end > tokens.size()) return false;
	for (std::size_t i = begin; i < end;) {
		// Shader-level directives (surfaceparm, cull, fogparms, q3map_* and
		// similar) are bounded metadata. The renderer does not implement their
		// full semantics, but rejecting the whole material would turn otherwise
		// usable transparent stages into a green placeholder. Stage directives
		// are handled below; only their supported map/blend subset affects draw.
		if (tokens[i].text != "{") {
			++i;
			continue;
		}
		if (stages->size() >= VRHI_SHADER_MAX_STAGES) return false;
		const std::size_t stageBegin = ++i;
		int depth = 1;
		while (i < end && depth > 0) {
			if (tokens[i].text == "{") ++depth;
			else if (tokens[i].text == "}") --depth;
			++i;
		}
		if (depth != 0) return false;
		const std::size_t stageEnd = i - 1;
		VRHI_ShaderStage stage;
		bool foundMap = false;
		bool blendSpecified = false;
		bool discardStage = false;
		for (std::size_t j = stageBegin; j < stageEnd;) {
			if (tokens[j].text == "{" || tokens[j].text == "}") return false;
			const std::string keyword = VRHI_ShaderLowerASCII(tokens[j].text);
			if (keyword == "map" || keyword == "clampmap") {
				// The Quake3 `clampMap` keyword is distinguished from `map` by
				// the stage-local clamp flag; the qpath validation below is
				// byte-for-byte the same safe validation used for `map`, so a
				// clampMap stage with a traversal/drive/backslash/unsupported-
				// extension name rejects identically. Default `map` stages keep
				// the wrap sampler, so shaders without the keyword sample
				// exactly as before.
				const bool clampMap = keyword == "clampmap";
				if (foundMap || j + 1 >= stageEnd) return false;
				const std::string &texture = tokens[j + 1].text;
				// Engine-owned maps such as $lightmap cannot be uploaded through
				// the direct-image path. Skip that stage only; earlier real image
				// stages remain useful static transparent material output.
				if (!texture.empty() && texture[0] == '$') {
					j += 2;
					continue;
				}
				if (texture.empty() || !VRHI_ShaderPathIsSafe(texture) ||
					texture.find('\\') != std::string::npos ||
					!VRHI_ResolveDirectImagePath(texture.c_str(), &stage.path)) return false;
				stage.clampMap = clampMap;
				foundMap = true;
				j += 2;
				continue;
			}
			if (keyword == "animmap") {
				// Bounded static fallback: use the first frame and ignore the
				// animation rate/remaining frame paths. Full time-varying animMap
				// remains outside this renderer slice.
				if (foundMap || j + 2 >= stageEnd) return false;
				const std::string &texture = tokens[j + 2].text;
				if (texture.empty() || texture[0] == '$' ||
					!VRHI_ShaderPathIsSafe(texture) ||
					texture.find('\\') != std::string::npos ||
					!VRHI_ResolveDirectImagePath(texture.c_str(), &stage.path)) return false;
				foundMap = true;
				j += 3;
				continue;
			}
			if (keyword == "rgbgen" || keyword == "alphagen") {
				// Bounded static const generators (`rgbGen const r g b`,
				// `alphaGen const a`) are preserved when every constant is
				// finite and inside the defined safe range. Every other
				// generator (wave, entity, vertex, ...) keeps the existing
				// dynamic fallback: the keyword and its first argument are
				// consumed, remaining generator parameters are bounded tokens
				// and are ignored, and the stage keeps identity colour.
				// Malformed or truncated const statements still reject the
				// whole shader (never borrowing tokens past the stage end).
				if (j + 1 >= stageEnd) return false;
				const bool isAlphaGen = keyword == "alphagen";
				if (VRHI_ShaderTokenIs(tokens[j + 1], "const")) {
					const std::size_t argCount = isAlphaGen ? 1 : 3;
					if (j + 1 + argCount >= stageEnd) return false;
					float values[3] = { 1.0f, 1.0f, 1.0f };
					bool valid = true;
					for (std::size_t k = 0; k < argCount; ++k) {
						if (!VRHI_ShaderParseFiniteConstant(tokens[j + 2 + k].text,
							VRHI_SCRIPT_GENCONST_MIN,
							isAlphaGen ? VRHI_SCRIPT_GENCONST_ALPHA_MAX
								: VRHI_SCRIPT_GENCONST_RGB_MAX,
							&values[k])) {
							valid = false;
							break;
						}
					}
					if (valid) {
						if (isAlphaGen) {
							stage.alphaGenConst = true;
							stage.alphaConst = values[0];
						} else {
							stage.rgbGenConst = true;
							stage.rgbConst[0] = values[0];
							stage.rgbConst[1] = values[1];
							stage.rgbConst[2] = values[2];
						}
					}
					// Both accepted and rejected const statements consume the
					// whole declared argument count (2 + argCount tokens). A
					// syntactically complete but invalid or out-of-range const
					// statement must not leave keyword-looking argument tokens
					// (for example `rgbGen const 1 1 map`) behind to be re-parsed
					// as stage directives; it simply keeps the dynamic fallback
					// tint while the stage stays accepted.
					j += 2 + argCount;
					continue;
				}
				j += 2;
				continue;
			}
			if (keyword == "blendfunc") {
				if (blendSpecified || j + 1 >= stageEnd) return false;
				const std::string arg = VRHI_ShaderLowerASCII(tokens[j + 1].text);
				std::size_t consumed = 0;
				if (arg == "blend") {
					stage.blendMode = VRHI_SHADER_BLEND_ALPHA;
					consumed = 2;
				} else if (arg == "add") {
					stage.blendMode = VRHI_SHADER_BLEND_ADDITIVE;
					consumed = 2;
				} else if (arg == "filter") {
					stage.blendMode = VRHI_SHADER_BLEND_FILTER;
					consumed = 2;
				} else {
					if (j + 2 >= stageEnd) return false;
					const std::string dst = VRHI_ShaderLowerASCII(tokens[j + 2].text);
					if (arg == "gl_src_alpha" && dst == "gl_one_minus_src_alpha")
						stage.blendMode = VRHI_SHADER_BLEND_ALPHA;
					else if (arg == "gl_one" && dst == "gl_one")
						stage.blendMode = VRHI_SHADER_BLEND_ADDITIVE;
					else if (arg == "gl_dst_color" && dst == "gl_zero")
						stage.blendMode = VRHI_SHADER_BLEND_FILTER;
					else if (arg == "gl_dst_color" && dst == "gl_src_color")
						stage.blendMode = VRHI_SHADER_BLEND_MULTIPLY;
					else if (arg == "gl_one" && dst == "gl_one_minus_src_alpha")
						stage.blendMode = VRHI_SHADER_BLEND_PREMULTIPLIED;
					else if (arg == "gl_src_alpha" && dst == "gl_one")
						stage.blendMode = VRHI_SHADER_BLEND_ALPHA_ADDITIVE;
					else if (arg == "gl_one" && dst == "gl_zero")
						stage.blendMode = VRHI_SHADER_BLEND_OPAQUE;
					else
						// A syntactically complete but unsupported blend pair drops
						// only this stage. Earlier/later supported stages remain a
						// bounded static fallback instead of turning the material green.
						discardStage = true;
					consumed = 3;
				}
				blendSpecified = true;
				j += consumed;
				continue;
			}
			if (keyword == "depthwrite") {
				// Bounded per-stage depthWrite: the bare keyword sets the
				// stage-local depth-write flag carried into the draw state. It
				// never changes blend classification, and it consumes exactly
				// one token so a following directive (map, blendFunc, ...)
				// still parses normally.
				stage.depthWrite = true;
				j += 1;
				continue;
			}
			if (keyword == "alphafunc") {
				// Bounded stage-local alpha-test: exactly GE128, GT0, and
				// LT128 are accepted case-insensitively (Quake3 comparisons).
				// Unknown values retain the current safe behavior: the stage
				// stays usable and alpha testing stays OFF, exactly as before
				// this slice, so an otherwise usable stage is never rejected.
				// A truncated statement at the end of the stage is treated the
				// same way (consume the bare keyword, keep OFF) and never
				// borrows tokens past the stage. With a comparison present the
				// statement consumes exactly two tokens so a following
				// directive (map, blendFunc, ...) still parses normally.
				if (j + 1 < stageEnd) {
					const std::string arg = VRHI_ShaderLowerASCII(tokens[j + 1].text);
					if (arg == "ge128") stage.alphaFunc = VRHI_SHADER_ALPHAFUNC_GE128;
					else if (arg == "gt0") stage.alphaFunc = VRHI_SHADER_ALPHAFUNC_GT0;
					else if (arg == "lt128") stage.alphaFunc = VRHI_SHADER_ALPHAFUNC_LT128;
					j += 2;
				} else {
					j += 1;
				}
				continue;
			}
			if (keyword == "tcmod") {
				// Bounded static `tcMod scale s t`, time-based `tcMod scroll
				// s t`, and time-based `tcMod rotate degsPerSecond`: preserved
				// only when the next token is the `scale`, `scroll`, or
				// `rotate` variant and every argument is finite and inside the
				// defined safe range (scale: nonzero, |value| in
				// [VRHI_SCRIPT_TCMOD_SCALE_MIN, VRHI_SCRIPT_TCMOD_SCALE_MAX];
				// scroll: |value| <= VRHI_SCRIPT_TCMOD_SCROLL_MAX, zero
				// accepted as the identity no-scroll rate; rotate: |value| <=
				// VRHI_SCRIPT_TCMOD_ROTATE_MAX degrees per second, zero
				// accepted as the identity no-rotate rate). Every other tcMod
				// variant (turb, stretch, entityTranslate, ...) keeps the
				// existing unsupported fallback: the keyword is consumed and its
				// argument tokens remain bounded and ignored below, exactly as
				// before this slice. A stage may carry a scale, a rotate, and a
				// scroll; the pixel shader applies the fixed documented order
				// scale, then rotate about the texture center, then time
				// scroll (script order is intentionally not tracked).
				if (j + 1 < stageEnd && VRHI_ShaderTokenIs(tokens[j + 1], "scale")) {
					// A syntactically complete `tcMod scale s t` statement
					// consumes exactly four tokens (keyword + "scale" + s + t)
					// so a following directive (map, blendFunc, ...) still
					// parses normally. A complete but invalid/out-of-range
					// statement (for example `tcMod scale 0 2` or
					// `tcMod scale abc 2`) also consumes its whole declared
					// argument count, so keyword-looking argument tokens cannot
					// be re-parsed as stage directives; the stage stays accepted
					// with identity UVs (the existing fallback).
					if (j + 3 < stageEnd) {
						float s = 1.0f;
						float t = 1.0f;
						if (VRHI_ShaderParseTcModScale(tokens[j + 2].text, &s) &&
							VRHI_ShaderParseTcModScale(tokens[j + 3].text, &t)) {
							stage.tcModScale = true;
							stage.tcModScaleS = s;
							stage.tcModScaleT = t;
						}
						j += 4;
						continue;
					}
					// Truncated `tcMod scale` at the end of a stage: consume the
					// keyword and the `scale` marker (and any partial argument
					// below via the generic fallthrough), keep identity UVs, and
					// never borrow tokens past the stage end.
					j += 2;
					continue;
				}
				if (j + 1 < stageEnd && VRHI_ShaderTokenIs(tokens[j + 1], "rotate")) {
					// A syntactically complete `tcMod rotate degsPerSecond`
					// statement consumes exactly three tokens (keyword +
					// "rotate" + one rate), so a following directive (map,
					// blendFunc, ...) still parses normally. A complete but
					// invalid/out-of-range rate (for example `tcMod rotate 1e30`
					// or `tcMod rotate abc`) also consumes its whole declared
					// argument count, so keyword-looking argument tokens cannot
					// be re-parsed as stage directives; the stage stays accepted
					// with the no-rotate fallback (the existing behavior).
					if (j + 2 < stageEnd) {
						float rate = 0.0f;
						if (VRHI_ShaderParseTcModRotate(tokens[j + 2].text, &rate)) {
							stage.tcModRotate = true;
							stage.tcModRotateDegreesPerSecond = rate;
						}
						j += 3;
						continue;
					}
					// Truncated `tcMod rotate` at the end of a stage: consume
					// the keyword and the `rotate` marker (and any partial
					// argument below via the generic fallthrough), keep the
					// no-rotate fallback, and never borrow tokens past the
					// stage end.
					j += 2;
					continue;
				}
				if (j + 1 < stageEnd && VRHI_ShaderTokenIs(tokens[j + 1], "scroll")) {
					// A syntactically complete `tcMod scroll s t` statement
					// consumes exactly four tokens (keyword + "scroll" + s + t)
					// so a following directive (map, blendFunc, ...) still
					// parses normally. A complete but invalid/out-of-range
					// statement (for example `tcMod scroll 1e30 2` or
					// `tcMod scroll abc 2`) also consumes its whole declared
					// argument count, so keyword-looking argument tokens cannot
					// be re-parsed as stage directives; the stage stays accepted
					// with the zero-scroll fallback (the existing behavior).
					if (j + 3 < stageEnd) {
						float s = 0.0f;
						float t = 0.0f;
						if (VRHI_ShaderParseTcModScroll(tokens[j + 2].text, &s) &&
							VRHI_ShaderParseTcModScroll(tokens[j + 3].text, &t)) {
							stage.tcModScroll = true;
							stage.tcModScrollS = s;
							stage.tcModScrollT = t;
						}
						j += 4;
						continue;
					}
					// Truncated `tcMod scroll` at the end of a stage: consume
					// the keyword and the `scroll` marker (and any partial
					// argument below via the generic fallthrough), keep the
					// zero-scroll fallback, and never borrow tokens past the
					// stage end.
					j += 2;
					continue;
				}
				// Non-scale/scroll/rotate tcMod (turb/stretch/entityTranslate/...):
				// keep the existing fallback by consuming just the keyword; the
				// remaining argument tokens are bounded by stageEnd and ignored
				// below.
				j += 1;
				continue;
			}
			const std::size_t consumed = VRHI_ShaderAllowlistedStatement(tokens, j, stageEnd);
			if (consumed == std::numeric_limits<std::size_t>::max()) return false;
			// Unsupported stage modifiers (tcMod, tcGen, rgbGen, alphaGen,
			// deform-like tokens nested in a stage) are deliberately ignored as
			// static fallbacks. Their tokens are still bounded by stageEnd.
			j += consumed > 0 ? consumed : 1;
		}
		// A stage containing only an engine-owned map (for example
		// `$lightmap`) or an unsupported but syntactically valid blend is
		// intentionally omitted from this direct-image slice.
		if (!foundMap || discardStage) continue;
		if (!stages->empty()) {
			if (blendSpecified && stage.blendMode == VRHI_SHADER_BLEND_OPAQUE) return false;
			if (!blendSpecified) stage.blendMode = VRHI_SHADER_BLEND_ALPHA;
		}
		stages->push_back(std::move(stage));
	}
	return !stages->empty();
}

// Parses every wanted shader and copies all accepted stage paths into results.
inline bool VRHI_ParseShaderScriptStages(
	const unsigned char *data, std::size_t size,
	const std::unordered_map<std::string, bool> &wanted,
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > *results) {
	std::vector<VRHI_ShaderScriptToken> tokens;
	if (results == nullptr || !VRHI_TokenizeShaderScript(data, size, &tokens)) return false;
	for (std::size_t i = 0; i < tokens.size();) {
		const std::string shaderName = VRHI_ShaderLowerASCII(tokens[i++].text);
		if (i >= tokens.size() || tokens[i].text != "{") continue;
		const std::size_t bodyBegin = ++i;
		int depth = 1;
		while (i < tokens.size() && depth > 0) {
			if (tokens[i].text == "{") ++depth;
			else if (tokens[i].text == "}") --depth;
			++i;
		}
		if (depth != 0) return false;
		const std::size_t bodyEnd = i - 1;
		if (wanted.find(shaderName) == wanted.end()) continue;
		std::vector<VRHI_ShaderStage> stages;
		if (VRHI_ParseSimpleShaderBlock(tokens, bodyBegin, bodyEnd, &stages))
			results->emplace(shaderName, std::move(stages));
	}
	return true;
}

#endif // __VRHI_SHADER_SCRIPT_H
