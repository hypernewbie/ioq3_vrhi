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
// flag). `animMap` has no clamp variant; its frames use the default wrap
// sampler, and the conservative first-stage helper still accepts only the
// first frame as a static candidate.
//
// The bounded `animMap frequency f1 ... fN` slice extends the same simple
// multi-stage parser with time-varying animated stages: up to
// VRHI_SHADER_MAX_ANIM_FRAMES path-safe frames (first wins on excess, like
// GL1's ignore-excess warning) with a frame rate in [0,
// VRHI_SCRIPT_ANIMFPS_MAX] frames per second (invalid rates fall back to 0,
// the static first frame). Frame collection stops before any known
// directive keyword (the line-blind equivalent of GL1's end-of-line) and
// never reads past the stage end; a `$`-special or unsafe frame, or zero
// frames, rejects the shader. The adapter decodes every frame under the
// existing image caps and selects floor(time * fps) % frames per draw.
// Malformed or truncated statements keep the prior behavior (reject, never
// borrowing past the stage).
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
//
// The bounded stage-local `tcMod turb` / `tcMod stretch` slice extends the
// same simple multi-stage parser with exactly the Quake3 liquid-motion
// forms: `tcMod turb base amplitude phase frequency` (four finite parameters
// inside VRHI_SCRIPT_TCMOD_TURB_MAX) and `tcMod stretch <func> base
// amplitude phase frequency` (one of the five wave forms plus four finite
// parameters inside VRHI_SCRIPT_TCMOD_STRETCH_MAX). Turb offsets the diffuse
// UVs by the GL1 world-position sine-table terms (amplitude-scaled, base
// ignored exactly like RB_CalcTurbulentTexCoords); stretch scales the
// diffuse UVs about (0.5, 0.5) by the inverse unclamped waveform value
// (1/EvalWaveForm, near-zero clamped to a bounded magnitude). Both compose
// before scale/rotate/scroll in the fixed documented order turb, stretch,
// scale, rotate, scroll (script order is not tracked) and apply to the
// diffuse sample only. Malformed, truncated, non-finite, or out-of-range
// statements keep the stage accepted with identity UVs and never borrow
// tokens past the stage end. The conservative first-stage helper still
// treats `tcMod` as unsupported and rejects such shaders.
//
// The bounded `skyParms` slice extends the same simple multi-stage parser
// with shader-level skybox metadata: the outerbox image prefix (GL1
// rt/bk/lf/ft/up/dn faces) and the cloud height, both validated. The adapter
// packs a same-dimensioned six-face set into one atlas image for skies
// without accepted stages; staged skies ignore the box. Truncated,
// non-finite, out-of-range, or unsafe statements keep the existing ignore
// with the stage still accepted and never borrow past the shader block.
//
// The bounded `deformVertexes` slice extends the same simple multi-stage
// parser with shader-level vertex deformation shared by every stage: `wave`
// (divisor plus a five-form waveform, spread = 1/divisor), `move` (a
// translation vector plus a waveform), and `bulge` (width/height/speed),
// all finite and inside conservative bounds. The adapter displaces vertices
// along their normals (move along its vector) every frame using the
// unclamped GL1 waveform evaluation. Only the first valid statement is
// kept; `autosprite`, `normal`, `projectionShadow`, text, unknown,
// truncated, non-finite, or out-of-range statements keep the existing
// static ignore with the stage still accepted and never borrow past the
// shader block. The conservative first-stage helper is unaffected (it never
// accepted deforming shaders as opaque candidates).
//
// The bounded `tcGen environment` slice extends the same simple multi-stage
// parser with exactly the Quake3 reflection-coordinate generator: the stage
// samples its image with per-pixel view-dependent coordinates matching GL1's
// RB_CalcEnvironmentTexCoords (reflection vector from the decoded surface
// normal and the current view origin), applied before every tcMod in the
// fixed composition order. `tcGen texture` stays the identity default and
// every other tcGen keeps the existing fallback with pre-tcGen sampling.
// Malformed or truncated statements keep the stage accepted with authored
// UVs and never borrow past the stage end. The conservative first-stage
// helper still treats `tcGen` as unsupported and rejects such shaders.
//
// The bounded stage-local `rgbGen wave` / `alphaGen wave` slice extends the
// same simple multi-stage parser with exactly the five Quake3 waveform forms
// `sin`, `square`, `triangle`, `sawtooth`, and `inversesawtooth` (matched
// case-insensitively like every directive) and their four parameters
// base/amplitude/phase/frequency: every parameter must be finite and |value|
// <= the defined safe bound (VRHI_SCRIPT_WAVE_PARAM_MAX below). The parsed
// generator is carried through the world, inline-BSP, and scene
// shader-cache/draw paths so the textured material pixel shaders modulate the
// material sample RGB/alpha by clamp(base + table((phase +
// frameTimeSeconds * frequency) wrapped to one period) * amplitude, 0, 1),
// matching the GL1 renderer's EvalWaveForm/WAVEVALUE index wrap, table
// shapes, phase sign, frequency units (cycles per second), and [0,1] clamp in
// RB_CalcWaveColor/RB_CalcWaveAlpha exactly (the sin table is evaluated with
// the GL1 tr_init.c DEG2RAD(i*360/1023) formula, and the wave index uses the
// truncation-toward-zero modulo of ((phase + time*freq) * 1024) so negative
// phases wrap identically to the GL1 & FUNCTABLE_MASK). The wave multiplies
// the same material sample that the static const generators tint, so a stage
// may carry both and the pixel shader composes them (const * wave; identity
// values reduce to the other generator); vertex colour, lightmaps, blend
// classification, depth, alpha testing, and the current UV transforms are
// never modified. Every other generator (noise, lightingSpecular, vertex,
// entity, ...) and every malformed, truncated, non-finite, or out-of-range
// wave keeps the existing dynamic fallback: the stage stays accepted and
// modulates with identity exactly as before this slice, and the statement
// never borrows tokens past the stage end. The conservative first-stage
// helper still treats `rgbGen wave`/`alphaGen wave` as unsupported and
// rejects such shaders.
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
	VRHI_SHADER_BLEND_ALPHA_ADDITIVE = 6,
	// Quake3 `blendFunc GL_DST_COLOR GL_ONE`: multiply plus dst
	// (src*dst + dst). Named systematically by factor pair.
	VRHI_SHADER_BLEND_DST_COLOR_ONE = 7,
	// Quake3 `blendFunc GL_ONE GL_SRC_ALPHA`: additive with dst scaled by
	// source alpha (src + dst*srcA).
	VRHI_SHADER_BLEND_ONE_SRC_ALPHA = 8,
	// Quake3 `blendFunc GL_DST_COLOR GL_SRC_ALPHA`: multiply with dst
	// scaled by source alpha (src*dst + dst*srcA).
	VRHI_SHADER_BLEND_DST_COLOR_SRC_ALPHA = 9,
	// Quake3 `blendFunc GL_DST_COLOR GL_ONE_MINUS_SRC_ALPHA`: multiply
	// with dst scaled by inverse source alpha (src*dst + dst*(1-srcA)).
	VRHI_SHADER_BLEND_DST_COLOR_ONE_MINUS_SRC_ALPHA = 10
};
// Bounded Quake3 wave-generator functions accepted by the simple multi-stage
// parser (`rgbGen wave <func> ...` / `alphaGen wave <func> ...`). Exactly the
// five table-based forms are supported; every other generator (noise, ...)
// keeps the existing dynamic fallback. The ids are deliberately 1..5 so 0 can
// mean "no generator" and the value doubles as the pixel-shader dispatch key.
enum VRHI_ShaderWaveFunc {
	// No wave generator: the stage modulates with identity exactly as before
	// this slice (also the fallback for unknown/malformed waves).
	VRHI_SHADER_WAVE_NONE = 0,
	VRHI_SHADER_WAVE_SIN = 1,
	VRHI_SHADER_WAVE_SQUARE = 2,
	VRHI_SHADER_WAVE_TRIANGLE = 3,
	VRHI_SHADER_WAVE_SAWTOOTH = 4,
	VRHI_SHADER_WAVE_INVERSE_SAWTOOTH = 5
};
// Bounded shader-level `deformVertexes` kinds. NONE renders statically (also
// the fallback for unknown/malformed statements); WAVE/MOVE/BULGE displace
// vertices along their normals (MOVE along its vector) every frame.
// `autosprite`, `normal`, `projectionShadow`, and text deforms keep the
// existing static ignore with this kind clear.
enum VRHI_ShaderDeformKind {
	VRHI_SHADER_DEFORM_NONE = 0,
	VRHI_SHADER_DEFORM_WAVE = 1,
	VRHI_SHADER_DEFORM_MOVE = 2,
	VRHI_SHADER_DEFORM_BULGE = 3
};
// Bounded shader-level `deformVertexes` metadata shared by every stage of
// the material. Only the first valid wave/move/bulge statement is kept
// (GL1 supports several; multi-deform shaders are rare and keep their first
// valid deformation). wave holds base/amplitude/phase/frequency in script
// order with spread = 1/divisor; move holds the translation vector plus the
// same wave layout; bulge holds width/height/speed.
// Bounded shader-level `skyParms` metadata: the outerbox image prefix (empty
// for "-"/absent) and the cloud height in world units. Only the outerbox
// feeds rendering (box-only skies); the height is validated and retained
// for documentation. innerbox is always "-" in the corpus and is parsed
// for shape only, never stored.
struct VRHI_ShaderSky {
	std::string outerBox;
	float cloudHeight = 0.0f;
};
struct VRHI_ShaderDeform {
	int kind = VRHI_SHADER_DEFORM_NONE;
	int waveFunc = VRHI_SHADER_WAVE_NONE;
	float spread = 0.0f;
	float wave[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float move[3] = { 0.0f, 0.0f, 0.0f };
	float bulge[3] = { 0.0f, 0.0f, 0.0f };
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
	// Bounded `animMap` frames (all stage frames, path strings in script
	// order) with their animation speed in frames per second. Empty for
	// plain `map`/`clampMap` stages. At most VRHI_SHADER_MAX_ANIM_FRAMES
	// frames are kept (first wins on excess, like GL1's ignore-excess
	// warning); fps 0 means the static first frame (also the fallback for
	// invalid speeds, matching GL1's atof-garbage speed of 0).
	std::vector<std::string> animPaths;
	float animFps = 0.0f;
	// Bounded `tcGen environment`: the stage samples its image with
	// view-dependent reflection coordinates (GL1 RB_CalcEnvironmentTexCoords)
	// instead of the authored diffuse UVs. Every other tcGen keeps the
	// existing fallback (identity `texture` or ignored variants) with this
	// flag clear, so those stages sample exactly as before.
	bool tcGenEnvironment = false;
	int blendMode = VRHI_SHADER_BLEND_OPAQUE;
	// Shader-level `surfaceparm trans` sorts the material with translucent
	// geometry without necessarily enabling a blend function (e.g. a two-stage
	// liquid where only stage one blends). This hint is distinct from blendMode.
	bool surfaceTranslucent = false;
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
	// Bounded stage-local tcMod turb and stretch waveforms. Turb applies the
	// GL1 world-position sine offsets to diffuse UVs; stretch applies the
	// inverse waveform scale around (0.5,0.5), clamping near-zero scales to
	// avoid unbounded coordinates. Parameter order is base/amplitude/phase/
	// frequency as in the shader script.
	bool tcModTurb = false;
	float tcModTurbValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	int tcModStretchFunc = VRHI_SHADER_WAVE_NONE;
	float tcModStretchValues[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	// Bounded stage-local wave generators (`rgbGen wave <func> <base>
	// <amplitude> <phase> <frequency>` and `alphaGen wave <func> <base>
	// <amplitude> <phase> <frequency>`): func is one of the five Quake3
	// waveforms above (VRHI_ShaderWaveFunc) and every parameter is finite
	// and inside the defined safe range (VRHI_SCRIPT_WAVE_PARAM_MAX). The
	// textured material pixel shaders modulate the material sample RGB/alpha
	// by the clamped GL1 EvalWaveForm value (base + table * amplitude,
	// clamped to [0,1] like RB_CalcWaveColor/RB_CalcWaveAlpha) using the
	// shared per-frame render time, composed with the static const
	// generators by multiplication; vertex colour, lightmaps, blend modes,
	// depth, alpha testing, and UV transforms are never modified. NONE with
	// default parameters keeps the identity fallback for every other
	// generator (noise, lightingSpecular, vertex, entity, ...).
	int rgbGenWaveFunc = VRHI_SHADER_WAVE_NONE;
	float rgbWaveBase = 0.0f;
	float rgbWaveAmplitude = 0.0f;
	float rgbWavePhase = 0.0f;
	float rgbWaveFrequency = 0.0f;
	int alphaGenWaveFunc = VRHI_SHADER_WAVE_NONE;
	float alphaWaveBase = 0.0f;
	float alphaWaveAmplitude = 0.0f;
	float alphaWavePhase = 0.0f;
	float alphaWaveFrequency = 0.0f;
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
// Bounded `animMap` slice: at most 8 frames per stage (GL1's
// MAX_IMAGE_ANIMATIONS) and a frame rate in [0, 32] frames per second
// (zero is the static-first-frame identity; common rates are 1..15).
static const std::size_t VRHI_SHADER_MAX_ANIM_FRAMES = 8;
static const float VRHI_SCRIPT_ANIMFPS_MAX = 32.0f;
// Bounded `deformVertexes` ranges. The wave divisor must be finite, nonzero,
// and |divisor| inside [MIN, MAX] (spread = 1/divisor stays numerically safe
// for the per-frame vertex math); wave parameters reuse the finite wave
// bound. Move components are world units (maps span thousands). Bulge
// width/height/speed match the GL1 atof path with finite, sane magnitudes.
static const float VRHI_SCRIPT_DEFORM_DIV_MIN = 1.0f / 1024.0f;
static const float VRHI_SCRIPT_DEFORM_DIV_MAX = 1024.0f;
static const float VRHI_SCRIPT_DEFORM_MOVE_MAX = 4096.0f;
static const float VRHI_SCRIPT_DEFORM_BULGE_WIDTH_MAX = 1024.0f;
static const float VRHI_SCRIPT_DEFORM_BULGE_HEIGHT_MAX = 64.0f;
static const float VRHI_SCRIPT_DEFORM_BULGE_SPEED_MAX = 64.0f;
// Bounded `skyParms` ranges: the cloud height must be finite and inside
// [MIN, MAX] world units (typical skies use 512..2048); zero, negative,
// non-finite, or huge heights ignore the statement. Box face suffixes follow
// GL1 (rt/bk/lf/ft/up/dn); all six faces must decode to identical dimensions
// or the box is skipped.
static const float VRHI_SCRIPT_SKY_CLOUD_MIN = 1.0f;
static const float VRHI_SCRIPT_SKY_CLOUD_MAX = 100000.0f;
// Bounded tcMod turb/stretches reuse the finite wave parameter domain.
static const float VRHI_SCRIPT_TCMOD_TURB_MAX = 16.0f;
static const float VRHI_SCRIPT_TCMOD_STRETCH_MAX = 16.0f;

// Bounded wave-generator parameter range (`rgbGen wave` / `alphaGen wave`):
// each of the four parameters (base, amplitude, phase, frequency) must be
// finite and |value| <= VRHI_SCRIPT_WAVE_PARAM_MAX. The GL1 renderer's
// EvalWaveForm evaluates base + table * amplitude and clamps the result to
// [0,1] before use, so any finite in-range parameters stay numerically safe
// for the pixel-shader multiply and its per-period wrap; the bound
// comfortably covers every practical Quake3 wave (base/amplitude in [0,1],
// phase in cycles, frequency in cycles per second, commonly 0.01..10) while
// keeping the phase/frequency arithmetic safe for long sessions.
static const float VRHI_SCRIPT_WAVE_PARAM_MAX = 16.0f;

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

// Parses one token as a bounded wave-generator parameter (base, amplitude,
// phase, or frequency): finite and |value| <= VRHI_SCRIPT_WAVE_PARAM_MAX
// (zero is accepted: every parameter defaults to the identity/static wave
// shape at 0). Returns false for empty, non-numeric, partially consumed
// (trailing junk), non-finite, or out-of-range tokens, so the wave slice
// accepts only exact in-range parameters and every other value keeps the
// existing dynamic fallback. Locale behaviour mirrors the original
// renderer's atof-based ParseVector path.
inline bool VRHI_ShaderParseWaveConstant(const std::string &text, float *value) {
	if (value == nullptr || text.empty()) return false;
	char *end = nullptr;
	errno = 0;
	const float parsed = std::strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
		!std::isfinite(parsed)) return false;
	if (std::fabs(parsed) > VRHI_SCRIPT_WAVE_PARAM_MAX) return false;
	*value = parsed;
	return true;
}

// Maps one waveform name to its bounded function id (VRHI_ShaderWaveFunc).
// Exactly the five Quake3 forms sin, square, triangle, sawtooth, and
// inversesawtooth are accepted, matched case-insensitively like every
// directive (the GL1 NameToGenFunc uses Q_stricmp). Every other name (noise
// included) maps to NONE so the stage keeps the existing dynamic fallback.
// Parses one token as a bounded `animMap` frame rate in frames per second:
// finite and inside [0, VRHI_SCRIPT_ANIMFPS_MAX] (zero is accepted: a zero
// rate renders the static first frame). Returns false for empty,
// non-numeric, partially consumed, non-finite, negative, or out-of-range
// tokens; the caller keeps the stage accepted with fps 0 (static), matching
// GL1's atof-garbage speed. Locale behaviour mirrors the atof-based path.
inline bool VRHI_ShaderParseAnimFps(const std::string &text, float *value) {
	if (value == nullptr || text.empty()) return false;
	char *end = nullptr;
	errno = 0;
	const float parsed = std::strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || errno == ERANGE ||
		!std::isfinite(parsed)) return false;
	if (parsed < 0.0f || parsed > VRHI_SCRIPT_ANIMFPS_MAX) return false;
	*value = parsed;
	return true;
}

// Stage/shader directive keywords that terminate an `animMap` frame list.
// GL1 ends the list at the line break (COM_ParseExt with no line breaks);
// this line-blind parser ends it before any known directive instead, so a
// following statement (blendFunc, rgbGen, ...) is never swallowed as a
// frame. Matching is exact after ASCII lowercasing; frame paths containing
// '/' or an extension can never equal these bare words.
inline bool VRHI_ShaderTokenEndsAnimFrames(const VRHI_ShaderScriptToken &token) {
	const std::string lower = VRHI_ShaderLowerASCII(token.text);
	static const char *stops[] = {
		"map", "clampmap", "animmap", "videomap", "blendfunc",
		"rgbgen", "alphagen", "alphafunc", "tcmod", "tcgen",
		"depthwrite", "depthfunc", "deformvertexes", "surfaceparm",
		"detail"
	};
	for (const char *stop : stops) {
		if (lower == stop) return true;
	}
	return false;
}

inline int VRHI_ShaderWaveFuncFromName(const std::string &name) {
	const std::string lower = VRHI_ShaderLowerASCII(name);
	if (lower == "sin") return VRHI_SHADER_WAVE_SIN;
	if (lower == "square") return VRHI_SHADER_WAVE_SQUARE;
	if (lower == "triangle") return VRHI_SHADER_WAVE_TRIANGLE;
	if (lower == "sawtooth") return VRHI_SHADER_WAVE_SAWTOOTH;
	if (lower == "inversesawtooth") return VRHI_SHADER_WAVE_INVERSE_SAWTOOTH;
	return VRHI_SHADER_WAVE_NONE;
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
// Defined below (after the waveform helpers): parses one shader-level
// `deformVertexes` statement. Declared here so the block parser can call it.
inline std::size_t VRHI_ParseShaderDeform(
	const std::vector<VRHI_ShaderScriptToken> &tokens,
	std::size_t pos, std::size_t end, VRHI_ShaderDeform *deform);

// Defined below (after the sky bounds): parses one shader-level `skyParms`
// statement. Declared here so the block parser can call it.
inline std::size_t VRHI_ParseShaderSkyParms(
	const std::vector<VRHI_ShaderScriptToken> &tokens,
	std::size_t pos, std::size_t end, VRHI_ShaderSky *sky);

inline bool VRHI_ParseSimpleShaderBlock(
	const std::vector<VRHI_ShaderScriptToken> &tokens, std::size_t begin,
	std::size_t end, std::vector<VRHI_ShaderStage> *stages,
	VRHI_ShaderDeform *deform = nullptr, VRHI_ShaderSky *sky = nullptr) {
	if (stages != nullptr) stages->clear();
	if (deform != nullptr) *deform = VRHI_ShaderDeform();
	if (stages == nullptr || begin >= end || end > tokens.size()) return false;
	bool surfaceTranslucent = false;
	for (std::size_t i = begin; i < end;) {
		// Shader-level directives (surfaceparm, cull, fogparms, q3map_* and
		// similar) are bounded metadata. The renderer does not implement their
		// full semantics, but rejecting the whole material would turn otherwise
		// usable transparent stages into a green placeholder. Stage directives
		// are handled below; only their supported map/blend subset affects draw.
		if (tokens[i].text != "{") {
			if (VRHI_ShaderTokenIs(tokens[i], "surfaceparm") && i + 1 < end) {
				if (VRHI_ShaderTokenIs(tokens[i + 1], "trans"))
					surfaceTranslucent = true;
				i += 2;
				continue;
			}
			if (sky != nullptr && VRHI_ShaderTokenIs(tokens[i], "skyparms")) {
				// Shader-level skybox metadata for box-only skies. The
				// parser records the outerbox prefix and cloud height;
				// anything else falls through to the existing per-token
				// ignore below.
				i = VRHI_ParseShaderSkyParms(tokens, i, end, sky);
				continue;
			}
			if (deform != nullptr && VRHI_ShaderTokenIs(tokens[i], "deformvertexes")) {
				// Shader-level vertex deformation shared by every stage of
				// this material. The parser records the first valid
				// wave/move/bulge statement and consumes it; anything else
				// falls through to the existing per-token ignore below.
				i = VRHI_ParseShaderDeform(tokens, i, end, deform);
				continue;
			}
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
				// Bounded animated frames: `animMap frequency f1 ... fN`.
				// The rate is frames per second; an invalid or out-of-range
				// rate falls back to 0 (static first frame). Frames are the
				// consecutive path-safe image tokens (at most
				// VRHI_SHADER_MAX_ANIM_FRAMES; excess stays for the generic
				// loop to ignore, like GL1's ignore-excess warning).
				// Collection stops before any known directive keyword (this
				// line-blind parser's equivalent of GL1's end-of-line) and
				// never reads past stageEnd. A `$`-special or unsafe frame,
				// or zero frames, rejects the shader exactly as before.
				if (foundMap || j + 2 >= stageEnd) return false;
				float fps = 0.0f;
				VRHI_ShaderParseAnimFps(tokens[j + 1].text, &fps);
				std::vector<std::string> frames;
				std::size_t k = j + 2;
				bool invalid = false;
				while (k < stageEnd && frames.size() < VRHI_SHADER_MAX_ANIM_FRAMES) {
					if (VRHI_ShaderTokenEndsAnimFrames(tokens[k])) break;
					const std::string &texture = tokens[k].text;
					std::string resolved;
					if (texture.empty() || texture[0] == '$' ||
						!VRHI_ShaderPathIsSafe(texture) ||
						texture.find('\\') != std::string::npos ||
						!VRHI_ResolveDirectImagePath(texture.c_str(), &resolved)) {
						invalid = true;
						break;
					}
					frames.push_back(std::move(resolved));
					++k;
				}
				if (invalid || frames.empty()) return false;
				stage.path = frames[0];
				stage.animPaths = std::move(frames);
				stage.animFps = fps;
				foundMap = true;
				j = k;
				continue;
			}
			if (keyword == "rgbgen" || keyword == "alphagen") {
				// Bounded static const generators (`rgbGen const r g b`,
				// `alphaGen const a`) are preserved when every constant is
				// finite and inside the defined safe range. Bounded wave
				// generators (`rgbGen wave <func> <base> <amplitude> <phase>
				// <frequency>`, `alphaGen wave ...`) are preserved when the
				// waveform is one of the five Quake3 forms and every
				// parameter is finite and in range (see below). Every other
				// generator (noise, lightingSpecular, entity, vertex, ...)
				// keeps the existing dynamic fallback: the keyword and its
				// first argument are consumed, remaining generator parameters
				// are bounded tokens and are ignored, and the stage keeps
				// identity colour. Malformed or truncated const statements
				// still reject the whole shader (never borrowing tokens past
				// the stage end); malformed or truncated wave statements keep
				// the stage accepted with the identity fallback.
				if (j + 1 >= stageEnd) return false;
				const bool isAlphaGen = keyword == "alphagen";
				if (VRHI_ShaderTokenIs(tokens[j + 1], "const")) {
					const std::size_t argCount = isAlphaGen ? 1 : 3;
					// Optional GL1 parenthesized vector form `const ( r g b )`
					// (rgbGen only; the corpus has no parenthesized alphaGen
					// const). A truncated or unbalanced paren scope abandons
					// just the const (identity fallback) and consumes to
					// the stage end, so the stage still closes normally;
					// only the bare form's truncation rejects the shader.
					const bool paren = !isAlphaGen && j + 2 < stageEnd &&
						tokens[j + 2].text == "(";
					const std::size_t valueBegin = j + 2 + (paren ? 1 : 0);
					const std::size_t available = valueBegin < stageEnd
						? stageEnd - valueBegin : 0;
					if (!paren && available < argCount) return false;
					if (paren && available < argCount + 1) {
						j = stageEnd;
						continue;
					}
					float values[3] = { 1.0f, 1.0f, 1.0f };
					bool valid = true;
					for (std::size_t k = 0; k < argCount; ++k) {
						if (!VRHI_ShaderParseFiniteConstant(tokens[valueBegin + k].text,
							VRHI_SCRIPT_GENCONST_MIN,
							isAlphaGen ? VRHI_SCRIPT_GENCONST_ALPHA_MAX
								: VRHI_SCRIPT_GENCONST_RGB_MAX,
							&values[k])) {
							valid = false;
							break;
						}
					}
					std::size_t statementEnd = valueBegin + argCount;
					if (paren) {
						if (tokens[valueBegin + argCount].text != ")") {
							// Unbalanced closer with tokens present: identity
							// fallback, consume through the last value; the
							// offending token flows to the generic loop.
							j = statementEnd;
							continue;
						}
						statementEnd += 1;
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
					// whole declared argument count (plus both parens when
					// present). A syntactically complete but invalid or
					// out-of-range const statement must not leave
					// keyword-looking argument tokens (for example
					// `rgbGen const 1 1 map`) behind to be re-parsed as
					// stage directives; it simply keeps the dynamic
					// fallback tint while the stage stays accepted.
					j = statementEnd;
					continue;
				}
				if (VRHI_ShaderTokenIs(tokens[j + 1], "wave")) {
					// Bounded stage-local wave generators: exactly the five
					// Quake3 waveform forms (sin/square/triangle/sawtooth/
					// inversesawtooth, case-insensitive) with four finite,
					// safely bounded parameters (base, amplitude, phase,
					// frequency) are preserved; every other waveform (noise,
					// ...) and every malformed/truncated/out-of-range wave
					// keeps the existing dynamic fallback (the stage stays
					// accepted and modulates with identity). A syntactically
					// complete statement (keyword + "wave" + func + four
					// parameters, seven tokens) always consumes its whole
					// declared argument count, so keyword-looking argument
					// tokens cannot be re-parsed as stage directives; a
					// truncated statement consumes only the keyword and the
					// "wave" marker and never borrows tokens past the stage
					// end.
					if (j + 6 < stageEnd) {
						const int func =
							VRHI_ShaderWaveFuncFromName(tokens[j + 2].text);
						float base = 0.0f;
						float amplitude = 0.0f;
						float phase = 0.0f;
						float frequency = 0.0f;
						if (func != VRHI_SHADER_WAVE_NONE &&
							VRHI_ShaderParseWaveConstant(tokens[j + 3].text, &base) &&
							VRHI_ShaderParseWaveConstant(tokens[j + 4].text, &amplitude) &&
							VRHI_ShaderParseWaveConstant(tokens[j + 5].text, &phase) &&
							VRHI_ShaderParseWaveConstant(tokens[j + 6].text, &frequency)) {
							if (isAlphaGen) {
								stage.alphaGenWaveFunc = func;
								stage.alphaWaveBase = base;
								stage.alphaWaveAmplitude = amplitude;
								stage.alphaWavePhase = phase;
								stage.alphaWaveFrequency = frequency;
							} else {
								stage.rgbGenWaveFunc = func;
								stage.rgbWaveBase = base;
								stage.rgbWaveAmplitude = amplitude;
								stage.rgbWavePhase = phase;
								stage.rgbWaveFrequency = frequency;
							}
						}
						j += 7;
						continue;
					}
					// Truncated `rgbGen wave`/`alphaGen wave` at the end of a
					// stage: consume the keyword and the "wave" marker (any
					// partial argument below is skipped by the generic bounded
					// loop), keep the identity fallback, and never borrow
					// tokens past the stage end.
					j += 2;
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
					else if (arg == "gl_dst_color" && dst == "gl_one")
						stage.blendMode = VRHI_SHADER_BLEND_DST_COLOR_ONE;
					else if (arg == "gl_one" && dst == "gl_src_alpha")
						stage.blendMode = VRHI_SHADER_BLEND_ONE_SRC_ALPHA;
					else if (arg == "gl_dst_color" && dst == "gl_src_alpha")
						stage.blendMode = VRHI_SHADER_BLEND_DST_COLOR_SRC_ALPHA;
					else if (arg == "gl_dst_color" && dst == "gl_one_minus_src_alpha")
						stage.blendMode = VRHI_SHADER_BLEND_DST_COLOR_ONE_MINUS_SRC_ALPHA;
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
			if (keyword == "depthfunc" && j + 1 < stageEnd &&
				VRHI_ShaderTokenIs(tokens[j + 1], "equal")) {
				// Quake3 lightmap/overlay stages frequently use depthFunc
				// equal. A skipped $lightmap stage must not reject the entire
				// material (including an earlier alpha-tested texture). Active
				// overlays currently use the adapter's LEQUAL fallback.
				j += 2;
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
			if (keyword == "tcgen") {
				// Bounded `tcGen environment` (exactly two tokens): preserved
				// as the per-stage reflection flag carried into the draw
				// state. `tcGen texture` is the identity default (no flag,
				// consumed so a following directive still parses). Every
				// other tcGen variant (lightmap, vector, fog, entity, ...)
				// keeps the existing fallback: only the keyword is consumed
				// and the argument tokens are bounded by stageEnd and
				// ignored below, exactly as before this slice. A truncated
				// bare `tcGen` at the stage end is consumed the same way
				// and never borrows past the stage.
				if (j + 1 < stageEnd && VRHI_ShaderTokenIs(tokens[j + 1], "environment")) {
					stage.tcGenEnvironment = true;
					j += 2;
				} else if (j + 1 < stageEnd && VRHI_ShaderTokenIs(tokens[j + 1], "texture")) {
					j += 2;
				} else {
					j += 1;
				}
				continue;
			}
			if (keyword == "tcmod") {
				// Bounded `tcMod turb`, `stretch`, static `scale`, time-based
				// `scroll`, and `rotate`: preserved only when the next token is a
				// supported variant and every argument is finite and inside the
				// defined safe range (scale: nonzero, |value| in
				// [VRHI_SCRIPT_TCMOD_SCALE_MIN, VRHI_SCRIPT_TCMOD_SCALE_MAX];
				// scroll: |value| <= VRHI_SCRIPT_TCMOD_SCROLL_MAX, zero
				// accepted as the identity no-scroll rate; rotate: |value| <=
				// VRHI_SCRIPT_TCMOD_ROTATE_MAX degrees/sec; turb/stretch wave
				// parameters use the finite wave bound. Other variants
				// (entityTranslate, ...) remain identity fallbacks. Supported
				// variants compose in the fixed order turb, stretch, scale,
				// rotate, scroll (script order is not tracked).
				if (j + 1 < stageEnd && VRHI_ShaderTokenIs(tokens[j + 1], "turb")) {
					if (j + 5 < stageEnd) {
						float values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
						bool valid = true;
						for (std::size_t k = 0; k < 4; ++k)
							if (!VRHI_ShaderParseFiniteConstant(tokens[j + 2 + k].text,
								-VRHI_SCRIPT_TCMOD_TURB_MAX, VRHI_SCRIPT_TCMOD_TURB_MAX,
								&values[k])) valid = false;
						if (valid) {
							stage.tcModTurb = true;
							for (std::size_t k = 0; k < 4; ++k)
								stage.tcModTurbValues[k] = values[k];
						}
						j += 6;
					} else j += 2;
					continue;
				}
				if (j + 1 < stageEnd && VRHI_ShaderTokenIs(tokens[j + 1], "stretch")) {
					if (j + 6 < stageEnd) {
						const int func = VRHI_ShaderWaveFuncFromName(tokens[j + 2].text);
						float values[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
						bool valid = func != VRHI_SHADER_WAVE_NONE;
						for (std::size_t k = 0; k < 4; ++k)
							if (!VRHI_ShaderParseFiniteConstant(tokens[j + 3 + k].text,
								-VRHI_SCRIPT_TCMOD_STRETCH_MAX,
								VRHI_SCRIPT_TCMOD_STRETCH_MAX, &values[k])) valid = false;
						if (valid) {
							stage.tcModStretchFunc = func;
							for (std::size_t k = 0; k < 4; ++k)
								stage.tcModStretchValues[k] = values[k];
						}
						j += 7;
					} else j += 2;
					continue;
				}
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
				// Unsupported tcMod (entityTranslate, ...): consume just the
				// keyword; bounded argument tokens are ignored below.
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
		stage.surfaceTranslucent = surfaceTranslucent;
		stages->push_back(std::move(stage));
	}
	if (surfaceTranslucent) {
		// surfaceparm metadata may legally occur after a stage in a shader
		// block, so apply its sort hint to every parsed stage after the scan.
		for (VRHI_ShaderStage &stage : *stages) stage.surfaceTranslucent = true;
	}
	return !stages->empty();
}

// Parses every wanted shader and copies all accepted stage paths into results.
inline bool VRHI_ParseShaderScriptStages(
	const unsigned char *data, std::size_t size,
	const std::unordered_map<std::string, bool> &wanted,
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > *results,
	std::unordered_map<std::string, VRHI_ShaderDeform> *deforms = nullptr,
	std::unordered_map<std::string, VRHI_ShaderSky> *skyMetas = nullptr) {
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
		VRHI_ShaderDeform deform;
		VRHI_ShaderSky sky;
		if (VRHI_ParseSimpleShaderBlock(tokens, bodyBegin, bodyEnd, &stages, &deform, &sky))
			results->emplace(shaderName, std::move(stages));
		// Shader-level metadata records independently of stage success so
		// box-only skies (no stages at all) still resolve their outerbox.
		if (deforms != nullptr && deform.kind != VRHI_SHADER_DEFORM_NONE)
			deforms->emplace(shaderName, deform);
		if (skyMetas != nullptr && !sky.outerBox.empty())
			skyMetas->emplace(shaderName, sky);
	}
	return true;
}

// A registered shader name takes its accepted script stages before a
// same-named direct image. The image is only a fallback when the name has no
// accepted stages. Keep this lookup shared and dependency-free so the world's
// and scene's source selection use the same bounded, case-insensitive key.
inline const std::vector<VRHI_ShaderStage> *VRHI_FindShaderScriptStages(
	const char *shaderName,
	const std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > &parsed) {
	if (shaderName == nullptr) return nullptr;
	std::size_t length = 0;
	while (length < VRHI_SCRIPT_MAX_QPATH && shaderName[length] != '\0') ++length;
	if (length == 0 || length >= VRHI_SCRIPT_MAX_QPATH) return nullptr;
	const auto found = parsed.find(VRHI_ShaderLowerASCII(std::string(shaderName, length)));
	return found != parsed.end() && !found->second.empty() ? &found->second : nullptr;
}

// GL1 waveform table value (tr_init.c) at a fractional cycle count, WITHOUT
// the [0,1] clamp: matches EvalWaveForm/WAVEVALUE for vertex-deformation and
// motion use. The truncation-toward-zero index wrap matches the C (int64)
// cast for both signs. func NONE returns 0 (the additive identity: no
// displacement), so unset generators are always safe to evaluate.
inline float VRHI_WaveTableValue(int func, float cycles) {
	const float scaled = cycles * 1024.0f;
	const float truncated = scaled >= 0.0f ? std::floor(scaled) : std::ceil(scaled);
	const float wrapped = truncated - 1024.0f * std::floor(truncated / 1024.0f);
	const float fraction = wrapped / 1024.0f;
	switch (func) {
	case VRHI_SHADER_WAVE_SIN:
		return static_cast<float>(std::sin(6.283185307179586 *
			static_cast<double>(fraction) * (1024.0 / 1023.0)));
	case VRHI_SHADER_WAVE_SQUARE:
		return fraction < 0.5f ? 1.0f : -1.0f;
	case VRHI_SHADER_WAVE_TRIANGLE: {
		float triangle = fraction + 0.75f;
		triangle -= std::floor(triangle);
		return 4.0f * std::fabs(triangle - 0.5f) - 1.0f;
	}
	case VRHI_SHADER_WAVE_SAWTOOTH:
		return fraction;
	case VRHI_SHADER_WAVE_INVERSE_SAWTOOTH:
		return 1.0f - fraction;
	default:
		return 0.0f;
	}
}

// Unclamped GL1 EvalWaveForm: base + table(phase + time * frequency) *
// amplitude. Deformation and motion scale by this signed value directly.
inline float VRHI_EvalWaveFormUnclamped(int func, float base, float amplitude,
	float phase, float frequency, float timeSeconds) {
	return base + VRHI_WaveTableValue(func, phase + timeSeconds * frequency) * amplitude;
}

// Shared bounded lookup for shader-level sky metadata: same safe qpath
// key discipline as the lookups above. Returns null for missing names and
// for entries without an outerbox.
inline const VRHI_ShaderSky *VRHI_FindShaderSky(const char *shaderName,
	const std::unordered_map<std::string, VRHI_ShaderSky> &parsed) {
	if (shaderName == nullptr) return nullptr;
	std::size_t length = 0;
	while (length < VRHI_SCRIPT_MAX_QPATH && shaderName[length] != '\0') ++length;
	if (length == 0 || length >= VRHI_SCRIPT_MAX_QPATH) return nullptr;
	const auto found = parsed.find(VRHI_ShaderLowerASCII(std::string(shaderName, length)));
	return found != parsed.end() && !found->second.outerBox.empty()
		? &found->second : nullptr;
}

// Shared bounded lookup for shader-level deform metadata: same safe qpath
// key discipline as the stage-table lookup above.
inline const VRHI_ShaderDeform *VRHI_FindShaderDeform(const char *shaderName,
	const std::unordered_map<std::string, VRHI_ShaderDeform> &parsed) {
	if (shaderName == nullptr) return nullptr;
	std::size_t length = 0;
	while (length < VRHI_SCRIPT_MAX_QPATH && shaderName[length] != '\0') ++length;
	if (length == 0 || length >= VRHI_SCRIPT_MAX_QPATH) return nullptr;
	const auto found = parsed.find(VRHI_ShaderLowerASCII(std::string(shaderName, length)));
	return found != parsed.end() && found->second.kind != VRHI_SHADER_DEFORM_NONE
		? &found->second : nullptr;
}

// Parses one shader-level `deformVertexes ...` statement at tokens[pos]
// (which reads "deformvertexes" case-insensitively). Records the first
// valid wave/move/bulge deformation into *deform and returns the index past
// the consumed statement. Anything unrecognized, truncated, non-finite, or
// out-of-range leaves *deform untouched and returns pos + 1, so the generic
// loop skips the arguments exactly as before this slice. Never reads past
// end. `autosprite`, `normal`, `projectionShadow`, and text deforms keep
// their existing static ignore (keyword consumed, no metadata).
inline std::size_t VRHI_ParseShaderDeform(
	const std::vector<VRHI_ShaderScriptToken> &tokens,
	std::size_t pos, std::size_t end, VRHI_ShaderDeform *deform) {
	if (deform == nullptr || pos + 1 >= end) return pos + 1;
	const std::string sub = VRHI_ShaderLowerASCII(tokens[pos + 1].text);
	if (sub == "wave") {
		// deformVertexes wave divisor func base amplitude phase frequency.
		// GL1 stores spread = 1/divisor (divisor 0 warns and uses 100);
		// this slice ignores a zero/tiny/huge divisor instead.
		if (pos + 6 >= end) return pos + 1;
		float divisor = 0.0f;
		if (!VRHI_ShaderParseFiniteConstant(tokens[pos + 2].text,
			-VRHI_SCRIPT_DEFORM_DIV_MAX, VRHI_SCRIPT_DEFORM_DIV_MAX, &divisor) ||
			divisor == 0.0f || std::fabs(divisor) < VRHI_SCRIPT_DEFORM_DIV_MIN)
			return pos + 1;
		const int func = VRHI_ShaderWaveFuncFromName(tokens[pos + 3].text);
		if (func == VRHI_SHADER_WAVE_NONE) return pos + 1;
		float wave[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (std::size_t k = 0; k < 4; ++k) {
			if (!VRHI_ShaderParseFiniteConstant(tokens[pos + 4 + k].text,
				-VRHI_SCRIPT_WAVE_PARAM_MAX, VRHI_SCRIPT_WAVE_PARAM_MAX, &wave[k]))
				return pos + 1;
		}
		if (deform->kind == VRHI_SHADER_DEFORM_NONE) {
			deform->kind = VRHI_SHADER_DEFORM_WAVE;
			deform->waveFunc = func;
			deform->spread = 1.0f / divisor;
			for (std::size_t k = 0; k < 4; ++k) deform->wave[k] = wave[k];
		}
		return pos + 7;
	}
	if (sub == "move") {
		// deformVertexes move x y z func base amplitude phase frequency.
		if (pos + 9 >= end) return pos + 1;
		float move[3] = { 0.0f, 0.0f, 0.0f };
		for (std::size_t k = 0; k < 3; ++k) {
			if (!VRHI_ShaderParseFiniteConstant(tokens[pos + 2 + k].text,
				-VRHI_SCRIPT_DEFORM_MOVE_MAX, VRHI_SCRIPT_DEFORM_MOVE_MAX, &move[k]))
				return pos + 1;
		}
		const int func = VRHI_ShaderWaveFuncFromName(tokens[pos + 5].text);
		if (func == VRHI_SHADER_WAVE_NONE) return pos + 1;
		float wave[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		for (std::size_t k = 0; k < 4; ++k) {
			if (!VRHI_ShaderParseFiniteConstant(tokens[pos + 6 + k].text,
				-VRHI_SCRIPT_WAVE_PARAM_MAX, VRHI_SCRIPT_WAVE_PARAM_MAX, &wave[k]))
				return pos + 1;
		}
		if (deform->kind == VRHI_SHADER_DEFORM_NONE) {
			deform->kind = VRHI_SHADER_DEFORM_MOVE;
			deform->waveFunc = func;
			for (std::size_t k = 0; k < 3; ++k) deform->move[k] = move[k];
			for (std::size_t k = 0; k < 4; ++k) deform->wave[k] = wave[k];
		}
		return pos + 10;
	}
	if (sub == "bulge") {
		// deformVertexes bulge width height speed.
		if (pos + 3 >= end) return pos + 1;
		float bulge[3] = { 0.0f, 0.0f, 0.0f };
		if (!VRHI_ShaderParseFiniteConstant(tokens[pos + 2].text,
			-VRHI_SCRIPT_DEFORM_BULGE_WIDTH_MAX, VRHI_SCRIPT_DEFORM_BULGE_WIDTH_MAX,
			&bulge[0])) return pos + 1;
		if (!VRHI_ShaderParseFiniteConstant(tokens[pos + 3].text,
			-VRHI_SCRIPT_DEFORM_BULGE_HEIGHT_MAX, VRHI_SCRIPT_DEFORM_BULGE_HEIGHT_MAX,
			&bulge[1])) return pos + 1;
		if (!VRHI_ShaderParseFiniteConstant(tokens[pos + 4].text,
			-VRHI_SCRIPT_DEFORM_BULGE_SPEED_MAX, VRHI_SCRIPT_DEFORM_BULGE_SPEED_MAX,
			&bulge[2])) return pos + 1;
		if (deform->kind == VRHI_SHADER_DEFORM_NONE) {
			deform->kind = VRHI_SHADER_DEFORM_BULGE;
			for (std::size_t k = 0; k < 3; ++k) deform->bulge[k] = bulge[k];
		}
		return pos + 5;
	}
	return pos + 1;
}

// Parses one shader-level `skyParms outer cloudheight inner` statement at
// tokens[pos]. Records the outerbox prefix (lowercased) and cloud height
// when all three arguments are well-formed and the height is in range;
// "-" means no box and records nothing. Anything truncated, non-finite,
// out-of-range, or unsafe leaves *sky untouched and returns pos + 1, so the
// generic loop skips the arguments exactly as before this slice. Never reads
// past end. Later statements overwrite earlier ones, matching GL1.
inline std::size_t VRHI_ParseShaderSkyParms(
	const std::vector<VRHI_ShaderScriptToken> &tokens,
	std::size_t pos, std::size_t end, VRHI_ShaderSky *sky) {
	if (sky == nullptr || pos + 3 >= end) return pos + 1;
	// Braces are never skyParms values; consuming one would eat a stage
	// boundary, so they fall through to the generic loop untouched.
	for (std::size_t k = 1; k <= 3; ++k) {
		if (tokens[pos + k].text == "{" || tokens[pos + k].text == "}")
			return pos + 1;
	}
	const std::string &outer = tokens[pos + 1].text;
	float cloud = 0.0f;
	if (!VRHI_ShaderParseFiniteConstant(tokens[pos + 2].text,
		VRHI_SCRIPT_SKY_CLOUD_MIN, VRHI_SCRIPT_SKY_CLOUD_MAX, &cloud))
		return pos + 1;
	const std::string &inner = tokens[pos + 3].text;
	if (inner != "-" && !VRHI_ShaderPathIsSafe(inner)) return pos + 1;
	if (outer != "-") {
		if (!VRHI_ShaderPathIsSafe(outer)) return pos + 1;
		sky->outerBox = VRHI_ShaderLowerASCII(outer);
		sky->cloudHeight = cloud;
	} else {
		sky->outerBox.clear();
		sky->cloudHeight = 0.0f;
	}
	return pos + 4;
}

#endif // __VRHI_SHADER_SCRIPT_H
