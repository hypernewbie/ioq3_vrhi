// Standalone, dependency-free unit test for the VRHI bounded shader-script
// parser (code/renderervrhi/vrhi_shader_script.h). No engine headers, no
// filesystem, no third-party libraries: the test feeds shader script text in
// memory and asserts tokenization, comment handling, caps, the safe opaque
// allowlist (blendFunc GL_ONE GL_ZERO, rgbGen/alphaGen identity, depthWrite,
// depthFunc lequal/less), malformed statements, and multi-map shaders, plus
// the bounded multi-stage slice (VRHI_ParseShaderScriptStages: at most four
// direct-image/animated stages, common transparent blend modes,
// ignored-but-bounded UV/time modifiers, the bounded static const
// generators rgbGen const / alphaGen const, and the stage-local depthWrite
// flag that the adapter applies as WRITE_Z only for explicitly requesting
// stages/draws). The same slice also carries the bounded stage-local
// alphaFunc comparisons GE128/GT0/LT128 (case-insensitive) through to the
// textured material shaders; unknown comparisons and truncated statements
// keep the stage usable with alpha testing OFF. The stage-local clampMap
// flag distinguishes the Quake3 `clampMap` keyword from `map` (default wrap
// sampler) with the same safe qpath validation; the adapter selects CLAMP
// UVW sampler flags for clamped stages/draws and keeps WRAP UVW for every
// default `map` stage. The bounded stage-local `tcMod scale s t` slice
// accepts only finite, nonzero, safely bounded (|value| in [1/64, 64])
// components and carries the per-stage scale to the textured material UV
// sampling; every other tcMod variant (scroll/rotate/turb/stretch) and every
// malformed, truncated, zero, or out-of-range scale keeps the identity-UV
// fallback with the stage still accepted. The bounded stage-local `tcMod
// scroll s t` slice accepts only finite, safely bounded (|value| <= 16)
// rates (zero accepted as the identity no-scroll rate) and carries the
// per-stage rates to the textured material UV sampling, where the pixel
// shader combines them with the current refdef/render time in seconds after
// the static scale (scale then time scroll); every other tcMod variant
// (turb/stretch) and every malformed, truncated, non-finite, or
// out-of-range rate keeps the zero-scroll fallback with the stage still
// accepted. The bounded stage-local `tcMod rotate degsPerSecond` slice
// accepts only one finite, safely bounded (|value| <= 3600) rate in degrees
// per second (zero accepted as the identity no-rotate rate) and carries the
// per-stage rate to the textured material UV sampling, where the pixel
// shader rotates the diffuse UVs around the texture center (0.5, 0.5) by
// -rate * frameTimeSeconds degrees, matching the GL1 renderer's
// RB_CalcRotateTexCoords sign and units exactly; the fixed documented order
// when a stage carries several supported tcMods is turb, then stretch, then
// scale, then rotate, then time scroll (script order is intentionally not
// tracked). Every other tcMod variant (entityTranslate) and every malformed,
// truncated, non-finite, or out-of-range rate keeps the no-rotate fallback
// with the stage still accepted. The bounded stage-local `tcMod turb base
// amplitude phase frequency` slice offsets the diffuse UVs by the GL1
// world-position sine-table terms (per-pixel approximation of
// RB_CalcTurbulentTexCoords; base ignored like GL1), and `tcMod stretch
// <func> base amplitude phase frequency` scales about (0.5, 0.5) by the
// inverse unclamped waveform value (near-zero guarded); malformed,
// truncated, non-finite, or out-of-range statements keep identity UVs with
// the stage still accepted. The bounded stage-local `rgbGen wave` / `alphaGen
// wave` slice accepts exactly the five Quake3 waveform forms sin/square/
// triangle/sawtooth/inversesawtooth (case-insensitive) with four finite,
// safely bounded (|value| <= 16) parameters base/amplitude/phase/frequency
// and carries the per-stage generator to the textured material pixel
// shaders, which modulate the material sample RGB/alpha by the clamped GL1
// EvalWaveForm value (base + table * amplitude, [0,1]) using the shared
// per-frame render time; every other generator (noise, lightingSpecular,
// vertex, entity, ...) and every malformed, truncated, non-finite, or
// out-of-range wave keeps the identity fallback with the stage still
// accepted.
//
// Build and run (any C++11-or-later compiler):
//   clang++ -std=c++17 -O1 -Wall -Wextra -Werror tests/vrhi_shader_script_test.cpp -o vrhi_shader_script_test && ./vrhi_shader_script_test
//   clang-cl -TP -std:c++17 -W4 tests\vrhi_shader_script_test.cpp /Fe:vrhi_shader_script_test.exe && vrhi_shader_script_test.exe
//
// Exit code 0 means every case passed.

#include "../code/renderervrhi/vrhi_shader_script.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

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

#define CHECK_EQ_INT(a, b)                                                 \
	do {                                                                   \
		++g_checks;                                                        \
		const long long va = static_cast<long long>(a);                    \
		const long long vb = static_cast<long long>(b);                    \
		if (va != vb) {                                                    \
			++g_failures;                                                  \
			std::printf("FAIL %s:%d: %s=%lld != %s=%lld\n", __FILE__,      \
				__LINE__, #a, va, #b, vb);                                 \
		}                                                                  \
	} while (0)

#define CHECK_EQ_STR(a, b)                                                 \
	do {                                                                   \
		++g_checks;                                                        \
		const std::string va = (a);                                        \
		const std::string vb = (b);                                        \
		if (va != vb) {                                                    \
			++g_failures;                                                  \
			std::printf("FAIL %s:%d: %s='%s' != %s='%s'\n", __FILE__,      \
				__LINE__, #a, va.c_str(), #b, vb.c_str());                 \
		}                                                                  \
	} while (0)

const unsigned char *asBytes(const std::string &text) {
	return reinterpret_cast<const unsigned char *>(text.data());
}

// Wraps `body` in a complete one-shader script named "textures/foo/bar".
std::string script(const std::string &body) {
	return "textures/foo/bar\n{\n" + body + "\n}\n";
}

// Parses one script and returns the resolved candidate path for the wanted
// shader, or an empty string when the block rejected or produced no map.
std::string candidate(const std::string &text, bool *parsedOk = nullptr) {
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::string> results;
	const bool ok = VRHI_ParseShaderScript(asBytes(text), text.size(), wanted, &results);
	if (parsedOk != nullptr) *parsedOk = ok;
	const std::unordered_map<std::string, std::string>::const_iterator found =
		results.find("textures/foo/bar");
	if (found == results.end()) return std::string();
	return found->second;
}

std::string candidateBody(const std::string &body, bool *parsedOk = nullptr) {
	return candidate(script(body), parsedOk);
}

// Parses one shader body directly at the block level (stripping the shader
// name and outer braces) so stage-boundary argument validation can be
// asserted without the script-level wrapper.
bool parseBlockBody(const std::string &body, std::string *candidateOut) {
	const std::string text = script(body);
	std::vector<VRHI_ShaderScriptToken> tokens;
	if (!VRHI_TokenizeShaderScript(asBytes(text), text.size(), &tokens)) return false;
	if (tokens.size() < 3 || tokens[0].text != "textures/foo/bar" ||
		tokens[1].text != "{" || tokens.back().text != "}") return false;
	return VRHI_ParseShaderBlock(tokens, 2, tokens.size() - 1, candidateOut);
}

void testTokenizer() {
	std::vector<VRHI_ShaderScriptToken> tokens;
	// Braces are single tokens; comments are skipped; quotes keep spaces.
	const std::string text =
		"textures/foo/bar // line comment\n"
		"{ /* block comment */ map \"textures/foo bar.tga\" }\n";
	CHECK(VRHI_TokenizeShaderScript(asBytes(text), text.size(), &tokens));
	CHECK_EQ_INT(tokens.size(), 5u);
	if (tokens.size() == 5) {
		CHECK_EQ_STR(tokens[0].text, "textures/foo/bar");
		CHECK_EQ_STR(tokens[1].text, "{");
		CHECK_EQ_STR(tokens[2].text, "map");
		CHECK_EQ_STR(tokens[3].text, "textures/foo bar.tga");
		CHECK_EQ_STR(tokens[4].text, "}");
	}
	// Unclosed block comment rejects the whole buffer.
	CHECK(!VRHI_TokenizeShaderScript(asBytes("textures/x { /* oops"), 20, &tokens));
	// Unclosed quote rejects the whole buffer.
	CHECK(!VRHI_TokenizeShaderScript(asBytes("textures/x { \"oops"), 18, &tokens));
	// An overlong token (>= VRHI_SCRIPT_MAX_TOKEN_BYTES) rejects.
	const std::string longToken(VRHI_SCRIPT_MAX_TOKEN_BYTES, 'a');
	const std::string overlong = "textures/x\n{\nmap " + longToken + "x\n}\n";
	CHECK(!VRHI_TokenizeShaderScript(asBytes(overlong), overlong.size(), &tokens));
	// A token exactly at the cap is accepted.
	const std::string atCap = "textures/x\n{\nmap " + longToken + "\n}\n";
	CHECK(VRHI_TokenizeShaderScript(asBytes(atCap), atCap.size(), &tokens));
	// The token count cap rejects oversized token streams.
	std::string many;
	for (std::size_t i = 0; i <= VRHI_SCRIPT_MAX_TOKENS; ++i) many += "x ";
	CHECK(!VRHI_TokenizeShaderScript(asBytes(many), many.size(), &tokens));
	// Null input rejects.
	CHECK(!VRHI_TokenizeShaderScript(nullptr, 0, &tokens));
}

void testOpaqueAcceptance() {
	bool ok = false;
	// Plain single-stage opaque map.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
	// clampmap is equivalent here.
	CHECK_EQ_STR(candidateBody("{ clampmap textures/foo/bar.tga }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
	// The full default-value boilerplate of the opaque diffuse pass.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga blendFunc GL_ONE GL_ZERO rgbGen identity "
		"alphaGen identity depthWrite depthFunc lequal }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
	// depthFunc less is also accepted; keywords are case-insensitive.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga blendfunc gl_one gl_zero rgbgen Identity "
		"alphagen IDENTITY depthwrite depthfunc LESS }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
	// A bare name (no extension) is retained for the extension probe.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar }", &ok),
		"textures/foo/bar");
	CHECK(ok);
	// Special $ textures are skipped; the first real map is the candidate.
	CHECK_EQ_STR(candidateBody("{ map $lightmap map textures/foo/bar.tga }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
	// A stage with only a special $ texture has no usable candidate.
	CHECK_EQ_STR(candidateBody("{ map $lightmap }", &ok), "");
	CHECK(ok);
	// One allowed statement per stage form, independently.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthWrite }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthFunc less }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
}

void testBlendFuncRejection() {
	bool ok = false;
	// Only GL_ONE GL_ZERO is accepted; every other blend is a multi-stage or
	// alpha/additive semantic and rejects the whole shader.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga blendFunc GL_ONE GL_ONE }", &ok), "");
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga blendFunc GL_DST_COLOR GL_ZERO }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga blendFunc filter }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga blendFunc add }", &ok), "");
	// Missing or truncated arguments are malformed and reject.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga blendFunc GL_ONE }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga blendFunc }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ blendFunc GL_ONE }", &ok), "");
	// A stage that ends exactly after the complete pair is fine; the pair
	// must never borrow tokens from beyond the stage.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga blendFunc GL_ONE GL_ZERO }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
}

void testRgbGenAlphaGen() {
	bool ok = false;
	// identity is accepted; any other generator rejects.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga rgbGen lightingDiffuse }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga rgbGen wave sin 1 0 0 1 }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga rgbGen }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga alphaGen portal 0.5 }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga alphaGen const 0.5 }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga alphaGen }", &ok), "");
	// identity forms are accepted.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga rgbGen identity alphaGen identity }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
}

void testDepthFunc() {
	bool ok = false;
	// Only lequal/less are accepted; other tests reject.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthFunc equal }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthFunc gequal }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthFunc always }", &ok), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthFunc }", &ok), "");
	// lequal and less are accepted, including as the last tokens of a stage.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthFunc lequal }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthFunc less }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
}

void testStillUnsupported() {
	// These keywords keep rejecting the whole shader.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga tcGen lightmap }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga tcMod scale 2 2 }", nullptr), "");
	CHECK_EQ_STR(candidateBody(
		"{ animMap 8 textures/a.tga textures/b.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ videoMap video/foo.roq }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga normalMap textures/foo_n.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga specularMap textures/foo_s.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga alphaFunc GE128 }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga fogGen skybox }", nullptr), "");
	// Shader-body (top-level) unsupported keywords.
	CHECK_EQ_STR(candidateBody("deformVertexes wave 100 sin 1 0 0 1\n{ map textures/foo/bar.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("deform wave 100 sin 1 0 0 1\n{ map textures/foo/bar.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("fogParms ( 0 0 0 ) 1000\n{ map textures/foo/bar.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("portal\n{ map textures/foo/bar.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("skyParms textures/skies/foo 512 -\n{ map textures/foo/bar.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("polygonOffset\n{ map textures/foo/bar.tga }", nullptr), "");
}

void testMultiMapRejection() {
	// A second real map anywhere in the shader rejects it: multi-stage
	// blending is never faked.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/a.tga }\n{ map textures/b.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody(
		"{ map textures/a.tga map textures/b.tga }", nullptr), "");
	// A second $ texture does not count as a real map, so the first real
	// map stays the candidate.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/a.tga }\n{ map $lightmap }", nullptr), "textures/a.tga");
}

void testMapPathValidation() {
	// map without an argument at stage end is malformed.
	CHECK_EQ_STR(candidateBody("{ map }", nullptr), "");
	// Traversal, drive, backslash, and unsupported-extension names reject.
	CHECK_EQ_STR(candidateBody("{ map ../evil.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures/../evil.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures/evil.exe }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures/evil.bmp }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map textures\\evil.tga }", nullptr), "");
	CHECK_EQ_STR(candidateBody("{ map /textures/evil.tga }", nullptr), "");
	// Case-insensitive extension check; explicit TGA/JPG/JPEG/PNG only.
	CHECK_EQ_STR(candidateBody("{ map textures/evil.TGA }", nullptr), "textures/evil.TGA");
	CHECK_EQ_STR(candidateBody("{ map textures/evil.JPEG }", nullptr), "textures/evil.JPEG");
	// An overlong path (>= VRHI_SCRIPT_MAX_QPATH) rejects.
	std::string longPath("textures/");
	longPath.append(VRHI_SCRIPT_MAX_QPATH, 'a');
	CHECK_EQ_STR(candidateBody("{ map " + longPath + " }", nullptr), "");
}

void testMalformedStructure() {
	bool ok = false;
	// Unclosed stage rejects the whole script.
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga", &ok), "");
	CHECK(!ok);
	// Unclosed shader body rejects the whole script.
	CHECK_EQ_STR(candidate("textures/foo/bar\n{\n{ map textures/foo/bar.tga }\n", &ok), "");
	CHECK(!ok);
	// A name without a body is skipped, not an error.
	CHECK_EQ_STR(candidate("textures/foo/bar\n", &ok), "");
	CHECK(ok);
}

void testWantedFilteringAndNames() {
	// Wanted keys are lowercased by the caller (the renderer lowercases BSP
	// shader names before the scan); script shader names are matched
	// case-insensitively against those keys. Only wanted shaders produce
	// results.
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::string> results;
	const std::string text =
		"TEXTURES/FOO/BAR\n{\n{\nmap textures/foo/bar.tga\n}\n}\n"
		"textures/other/thing\n{\n{\nmap textures/other/thing.tga\n}\n}\n";
	CHECK(VRHI_ParseShaderScript(asBytes(text), text.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	CHECK_EQ_STR(results["textures/foo/bar"], "textures/foo/bar.tga");
	// A missing shader is simply absent from the results.
	std::unordered_map<std::string, bool> wanted2;
	wanted2.emplace("textures/not/there", true);
	std::unordered_map<std::string, std::string> results2;
	CHECK(VRHI_ParseShaderScript(asBytes(text), text.size(), wanted2, &results2));
	CHECK_EQ_INT(results2.size(), 0u);
}

void testSimpleMultiStageExtraction() {
	const std::string text = script(
		"{ map textures/base.tga depthWrite }\n"
		"{ map textures/alpha.png blendFunc blend }\n"
		"{ clampmap textures/add.jpg blendFunc GL_ONE GL_ONE }");
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > rejected;
	CHECK(VRHI_ParseShaderScriptStages(asBytes(text), text.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
	CHECK_EQ_INT(stages.size(), 3u);
	if (stages.size() == 3) {
		CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_OPAQUE);
		CHECK_EQ_STR(stages[1].path, "textures/alpha.png");
		CHECK_EQ_INT(stages[1].blendMode, VRHI_SHADER_BLEND_ALPHA);
		CHECK_EQ_INT(stages[2].blendMode, VRHI_SHADER_BLEND_ADDITIVE);
	}
	const std::string alphaStage0 = script("{ map textures/base.tga blendFunc blend }\n{ map textures/a.png blendFunc blend }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(alphaStage0), alphaStage0.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 1u);
	if (rejected.size() == 1) {
		const std::vector<VRHI_ShaderStage> &alphaStages = rejected["textures/foo/bar"];
		CHECK_EQ_INT(alphaStages.size(), 2u);
		if (alphaStages.size() == 2) {
			CHECK_EQ_INT(alphaStages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK_EQ_INT(alphaStages[1].blendMode, VRHI_SHADER_BLEND_ALPHA);
		}
	}
	const std::string boundedModifiers = script(
		"surfaceparm trans cull disable fogparms ( 0 0 0 ) 1024\n"
		"{ map textures/base.tga tcMod scroll 1 1 rgbGen wave sin 0.5 0.5 0 1 }\n"
		"{ map textures/a.png blendFunc filter tcMod turb 0 1 0 0.2 alphaGen const 0.5 }\n"
		"{ animMap 15 textures/frame0.tga textures/frame1.tga blendFunc add }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(boundedModifiers), boundedModifiers.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 1u);
	if (rejected.size() == 1) {
		const std::vector<VRHI_ShaderStage> &modifierStages = rejected["textures/foo/bar"];
		CHECK_EQ_INT(modifierStages.size(), 3u);
		if (modifierStages.size() == 3) {
			CHECK_EQ_INT(modifierStages[0].blendMode, VRHI_SHADER_BLEND_OPAQUE);
			CHECK_EQ_INT(modifierStages[1].blendMode, VRHI_SHADER_BLEND_FILTER);
			CHECK_EQ_INT(modifierStages[2].blendMode, VRHI_SHADER_BLEND_ADDITIVE);
			CHECK_EQ_STR(modifierStages[2].path, "textures/frame0.tga");
		}
	}
	const std::string compatibilityStages = script(
		"{ map textures/glow.tga blendFunc GL_SRC_ALPHA GL_ONE }\n"
		"{ map $lightmap rgbGen identity }\n"
		"{ map textures/overlay.tga }\n"
		"{ map textures/detail.tga blendFunc GL_DST_COLOR GL_SRC_COLOR }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(compatibilityStages), compatibilityStages.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 1u);
	if (rejected.size() == 1) {
		const std::vector<VRHI_ShaderStage> &compatStages = rejected["textures/foo/bar"];
		CHECK_EQ_INT(compatStages.size(), 3u);
		if (compatStages.size() == 3) {
			CHECK_EQ_INT(compatStages[0].blendMode, VRHI_SHADER_BLEND_ALPHA_ADDITIVE);
			CHECK_EQ_INT(compatStages[1].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK_EQ_INT(compatStages[2].blendMode, VRHI_SHADER_BLEND_MULTIPLY);
		}
	}
	// Each new Q3 blend pair maps to its own mode in isolation.
	const std::string alphaAdditive = script(
		"{ map textures/glow.tga blendFunc GL_SRC_ALPHA GL_ONE }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(alphaAdditive), alphaAdditive.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 1u);
	if (rejected.size() == 1)
		CHECK_EQ_INT(rejected["textures/foo/bar"][0].blendMode, VRHI_SHADER_BLEND_ALPHA_ADDITIVE);
	const std::string multiply = script(
		"{ map textures/detail.tga blendFunc GL_DST_COLOR GL_SRC_COLOR }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(multiply), multiply.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 1u);
	if (rejected.size() == 1)
		CHECK_EQ_INT(rejected["textures/foo/bar"][0].blendMode, VRHI_SHADER_BLEND_MULTIPLY);
	const std::string premultiplied = script(
		"{ map textures/premultiplied.tga blendFunc GL_ONE GL_ONE_MINUS_SRC_ALPHA }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(premultiplied), premultiplied.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 1u);
	if (rejected.size() == 1)
		CHECK_EQ_INT(rejected["textures/foo/bar"][0].blendMode, VRHI_SHADER_BLEND_PREMULTIPLIED);
	// Factor-pair blends keep their exact GL semantics instead of dropping
	// the stage.
	const std::string dstColorOne = script(
		"{ map textures/grate.tga blendFunc GL_DST_COLOR GL_ONE }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(dstColorOne), dstColorOne.size(), wanted, &rejected));
	if (rejected.size() == 1)
		CHECK_EQ_INT(rejected["textures/foo/bar"][0].blendMode, VRHI_SHADER_BLEND_DST_COLOR_ONE);
	const std::string oneSrcAlpha = script(
		"{ map textures/haze.tga blendFunc GL_ONE GL_SRC_ALPHA }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(oneSrcAlpha), oneSrcAlpha.size(), wanted, &rejected));
	if (rejected.size() == 1)
		CHECK_EQ_INT(rejected["textures/foo/bar"][0].blendMode, VRHI_SHADER_BLEND_ONE_SRC_ALPHA);
	const std::string dstColorSrcAlpha = script(
		"{ map textures/shade.tga blendFunc GL_DST_COLOR GL_SRC_ALPHA }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(dstColorSrcAlpha), dstColorSrcAlpha.size(), wanted, &rejected));
	if (rejected.size() == 1)
		CHECK_EQ_INT(rejected["textures/foo/bar"][0].blendMode, VRHI_SHADER_BLEND_DST_COLOR_SRC_ALPHA);
	const std::string dstColorInvSrcAlpha = script(
		"{ map textures/tint.tga blendFunc GL_DST_COLOR GL_ONE_MINUS_SRC_ALPHA }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(dstColorInvSrcAlpha), dstColorInvSrcAlpha.size(), wanted, &rejected));
	if (rejected.size() == 1)
		CHECK_EQ_INT(rejected["textures/foo/bar"][0].blendMode,
			VRHI_SHADER_BLEND_DST_COLOR_ONE_MINUS_SRC_ALPHA);
	const std::string explicitOpaqueOverlay = script(
		"{ map textures/base.tga } { map textures/opaque.tga blendFunc GL_ONE GL_ZERO }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(explicitOpaqueOverlay), explicitOpaqueOverlay.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 0u);
	const std::string truncatedAnimMap = script("{ animMap 15 }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedAnimMap), truncatedAnimMap.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 0u);
	const std::string tooMany = script(
		"{ map textures/0.tga } { map textures/1.tga blendFunc blend } "
		"{ map textures/2.tga blendFunc blend } { map textures/3.tga blendFunc blend } "
		"{ map textures/4.tga blendFunc blend }");
	rejected.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(tooMany), tooMany.size(), wanted, &rejected));
	CHECK_EQ_INT(rejected.size(), 0u);
}

void testScriptStagesOverrideDirectImages() {
	// These OpenArena-style shaders also have same-named JPG/TGA images. The
	// accepted script must win when the adapter selects a world/scene source.
	const std::string text =
		"textures/sfx/beam { surfaceparm trans { map textures/sfx/beam.jpg "
		"tcMod Scroll .3 0 blendFunc GL_ONE GL_ONE } }\n"
		"textures/acc_dm3/wires2 { surfaceparm trans { map "
		"textures/acc_dm3/wires2.tga alphaFunc GE128 depthWrite } "
		"{ map $lightmap rgbGen identity blendFunc filter depthFunc equal } }\n"
		"textures/flares/lava { { clampMap textures/flares/lava.tga "
		"blendFunc add rgbGen Vertex tcMod rotate 12 } "
		"{ clampMap textures/flares/lava.tga blendFunc add tcMod rotate -5 } }\n"
		"textures/unsupported { { map textures/unsupported.tga } "
		"{ map textures/2.tga } { map textures/3.tga } "
		"{ map textures/4.tga } { map textures/5.tga } }\n";
	std::unordered_map<std::string, bool> wanted;
	for (const char *name : { "textures/sfx/beam", "textures/acc_dm3/wires2",
		"textures/flares/lava", "textures/unsupported" }) wanted.emplace(name, true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > parsed;
	CHECK(VRHI_ParseShaderScriptStages(asBytes(text), text.size(), wanted, &parsed));
	CHECK_EQ_INT(parsed.size(), 3u);
	const auto *beam = VRHI_FindShaderScriptStages("TEXTURES/SFX/BEAM", parsed);
	CHECK(beam != nullptr);
	if (beam != nullptr) {
		CHECK_EQ_INT(beam->size(), 1u);
		CHECK_EQ_STR((*beam)[0].path, "textures/sfx/beam.jpg");
		CHECK_EQ_INT((*beam)[0].blendMode, VRHI_SHADER_BLEND_ADDITIVE);
		CHECK((*beam)[0].tcModScroll);
	}
	const auto *wires = VRHI_FindShaderScriptStages("textures/acc_dm3/wires2", parsed);
	CHECK(wires != nullptr);
	if (wires != nullptr) {
		CHECK_EQ_INT(wires->size(), 1u);
		CHECK_EQ_INT((*wires)[0].blendMode, VRHI_SHADER_BLEND_OPAQUE);
		CHECK_EQ_INT((*wires)[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
		CHECK((*wires)[0].depthWrite);
		CHECK((*wires)[0].surfaceTranslucent);
	}
	const auto *flare = VRHI_FindShaderScriptStages("textures/flares/lava", parsed);
	CHECK(flare != nullptr);
	if (flare != nullptr) {
		CHECK_EQ_INT(flare->size(), 2u);
		CHECK_EQ_INT((*flare)[0].blendMode, VRHI_SHADER_BLEND_ADDITIVE);
		CHECK((*flare)[0].clampMap);
		CHECK_EQ_INT((*flare)[1].blendMode, VRHI_SHADER_BLEND_ADDITIVE);
	}
	// An absent or rejected script leaves direct-image fallback available.
	CHECK(VRHI_FindShaderScriptStages("textures/unsupported", parsed) == nullptr);
	CHECK(VRHI_FindShaderScriptStages("textures/directonly", parsed) == nullptr);
	CHECK(VRHI_FindShaderScriptStages("", parsed) == nullptr);
	CHECK(VRHI_FindShaderScriptStages(nullptr, parsed) == nullptr);
	std::string tooLong(VRHI_SCRIPT_MAX_QPATH, 'x');
	CHECK(VRHI_FindShaderScriptStages(tooLong.c_str(), parsed) == nullptr);
	// The simple stage parser accepts GL1's depthFunc equal on an active
	// overlay and in a skipped $lightmap stage, without relaxing the strict
	// opaque-only first-stage parser or reading past a stage boundary.
	const std::string equalOverlay = script(
		"{ map textures/base.tga } { map textures/overlay.tga "
		"blendFunc blend depthFunc equal }");
	std::unordered_map<std::string, bool> overlayWanted;
	overlayWanted.emplace("textures/foo/bar", true);
	parsed.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(equalOverlay),
		equalOverlay.size(), overlayWanted, &parsed));
	CHECK_EQ_INT(parsed["textures/foo/bar"].size(), 2u);
	std::unordered_map<std::string, std::string> opaqueOnly;
	CHECK(VRHI_ParseShaderScript(asBytes(equalOverlay), equalOverlay.size(),
		overlayWanted, &opaqueOnly));
	CHECK(opaqueOnly.empty());
	const std::string badEqual = script("{ map textures/base.tga depthFunc never }");
	parsed.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(badEqual), badEqual.size(),
		overlayWanted, &parsed));
	CHECK(parsed.empty());
	const std::string missingEqual = script("{ map textures/base.tga depthFunc }");
	parsed.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(missingEqual), missingEqual.size(),
		overlayWanted, &parsed));
	CHECK(parsed.empty());
}

void testShaderFilePriority() {
	// GL1 combines FS_ListFiles in reverse order, then uses the first
	// definition. An OA patch replaces the old two-stage e8tinylight script
	// whose optional blend texture no longer ships in the pak files.
	const std::string newer =
		"textures/evil8_lights/e8tinylight { "
		"{ map textures/evil8_lights/e8tinylight.tga } "
		"{ map $lightmap blendFunc filter tcGen lightmap } }";
	const std::string older =
		"textures/evil8_lights/e8tinylight { "
		"{ map textures/evil8_lights/e8tinylight.tga } "
		"{ map $lightmap blendFunc filter tcGen lightmap } "
		"{ map textures/evil8_lights/e8tinylight.blend.tga blendFunc add } }";
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/evil8_lights/e8tinylight", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > selected;
	CHECK(VRHI_ParseShaderScriptStages(asBytes(newer), newer.size(), wanted, &selected));
	CHECK(VRHI_ParseShaderScriptStages(asBytes(older), older.size(), wanted, &selected));
	CHECK_EQ_INT(selected.size(), 1u);
	const auto *stages = VRHI_FindShaderScriptStages(
		"textures/evil8_lights/e8tinylight", selected);
	CHECK(stages != nullptr);
	if (stages != nullptr) {
		CHECK_EQ_INT(stages->size(), 1u);
		CHECK_EQ_STR((*stages)[0].path, "textures/evil8_lights/e8tinylight.tga");
		CHECK(!(*stages)[0].surfaceTranslucent);
	}
	const std::string lateTrans = script(
		"{ map textures/base.tga } surfaceparm trans");
	std::unordered_map<std::string, bool> lateWanted;
	lateWanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > late;
	CHECK(VRHI_ParseShaderScriptStages(asBytes(lateTrans), lateTrans.size(),
		lateWanted, &late));
	CHECK(late["textures/foo/bar"][0].surfaceTranslucent);
}

void testBlockLevelBoundaries() {
	// Direct block-level checks: the allowlist validator never reads past the
	// stage, so a truncated statement at stage end rejects instead of
	// consuming the closing brace or the next stage.
	std::string out;
	CHECK(!parseBlockBody("{ map textures/foo/bar.tga blendFunc GL_ONE }", &out));
	CHECK(!parseBlockBody("{ map textures/foo/bar.tga rgbGen }", &out));
	CHECK(!parseBlockBody("{ map textures/foo/bar.tga depthFunc }", &out));
	CHECK(!parseBlockBody("{ blendFunc GL_ONE GL_ZERO }", &out)); // no map
	CHECK(out.empty());
	CHECK(parseBlockBody("{ map textures/foo/bar.tga blendFunc GL_ONE GL_ZERO }", &out));
	CHECK_EQ_STR(out, "textures/foo/bar.tga");
	// A complete allowlisted statement at top level (misplaced but
	// syntactically complete) is tolerated; a truncated one rejects.
	CHECK(parseBlockBody("blendFunc GL_ONE GL_ZERO\n{ map textures/foo/bar.tga }", &out));
	CHECK_EQ_STR(out, "textures/foo/bar.tga");
	CHECK(!parseBlockBody("blendFunc GL_ONE\n{ map textures/foo/bar.tga }", &out));
	CHECK(!parseBlockBody("rgbGen\n{ map textures/foo/bar.tga }", &out));
}

void testPathSafetyHelpers() {
	CHECK(VRHI_ShaderPathIsSafe("textures/foo/bar.tga"));
	CHECK(VRHI_ShaderPathIsSafe("models/mapobjects/torch/torch"));
	CHECK(!VRHI_ShaderPathIsSafe(""));
	CHECK(!VRHI_ShaderPathIsSafe("../evil"));
	CHECK(!VRHI_ShaderPathIsSafe("textures/../evil"));
	CHECK(!VRHI_ShaderPathIsSafe("textures//evil"));
	CHECK(!VRHI_ShaderPathIsSafe("textures/./evil"));
	CHECK(!VRHI_ShaderPathIsSafe("C:/textures/evil"));
	CHECK(!VRHI_ShaderPathIsSafe("textures/evil/"));

	std::string resolved;
	CHECK(VRHI_ResolveDirectImagePath("textures/foo/bar.tga", &resolved));
	CHECK_EQ_STR(resolved, "textures/foo/bar.tga");
	CHECK(VRHI_ResolveDirectImagePath("textures/foo/bar", &resolved));
	CHECK_EQ_STR(resolved, "textures/foo/bar");
	CHECK(!VRHI_ResolveDirectImagePath("textures/foo/bar.exe", &resolved));
	CHECK(!VRHI_ResolveDirectImagePath("../evil.tga", &resolved));
	CHECK(!VRHI_ResolveDirectImagePath("textures/foo/bar.tgax", &resolved));
}

void testConstGenerators() {
	// Bounded static const generators (rgbGen const / alphaGen const) are
	// accepted by the simple multi-stage slice and preserved in stage data.
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	const std::string tinted = script(
		"{ map textures/base.tga rgbGen const 0.5 0.75 1.0 alphaGen const 0.25 }\n"
		"{ map textures/glow.tga blendFunc add rgbGen const 0.2 0.4 0.8 }\n");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(tinted), tinted.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(stages[0].rgbGenConst);
			CHECK(stages[0].alphaGenConst);
			CHECK(stages[0].rgbConst[0] > 0.49f && stages[0].rgbConst[0] < 0.51f);
			CHECK(stages[0].rgbConst[1] > 0.74f && stages[0].rgbConst[1] < 0.76f);
			CHECK(stages[0].rgbConst[2] > 0.99f && stages[0].rgbConst[2] <= 1.01f);
			CHECK(stages[0].alphaConst > 0.24f && stages[0].alphaConst < 0.26f);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_OPAQUE);
			CHECK(stages[1].rgbGenConst);
			CHECK(!stages[1].alphaGenConst);
			CHECK(stages[1].rgbConst[0] > 0.19f && stages[1].rgbConst[0] < 0.21f);
			CHECK(stages[1].rgbConst[1] > 0.39f && stages[1].rgbConst[1] < 0.41f);
			CHECK(stages[1].rgbConst[2] > 0.79f && stages[1].rgbConst[2] < 0.81f);
			CHECK_EQ_INT(stages[1].blendMode, VRHI_SHADER_BLEND_ADDITIVE);
		}
	}
	// The accepted range matches the GL1 byte-colour semantics exactly:
	// every rgbGen const component and the alphaGen const value must be
	// inside [0,1], boundaries inclusive.
	const std::string boundary = script(
		"{ map textures/base.tga rgbGen const 0 1 1 alphaGen const 1 }\n");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(boundary), boundary.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].rgbGenConst);
			CHECK(stages[0].alphaGenConst);
			CHECK(stages[0].rgbConst[0] == 0.0f);
			CHECK(stages[0].rgbConst[1] == 1.0f);
			CHECK(stages[0].rgbConst[2] == 1.0f);
			CHECK(stages[0].alphaConst == 1.0f);
		}
	}
	// Out-of-range, non-finite, or non-numeric constants are not part of the
	// bounded slice: the stage stays accepted with the dynamic fallback
	// (identity colour, flags false) instead of rejecting the material.
	// Split across two scripts (the parser caps every shader at four stages).
	const std::string fallbacksA = script(
		"{ map textures/base.tga rgbGen const -0.5 0 0 depthWrite }\n"
		"{ map textures/a.png blendFunc blend alphaGen const 2 tcMod scroll 0.1 0.1 }\n"
		"{ map textures/b.png blendFunc blend rgbGen const 9 9 9 depthWrite }\n");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(fallbacksA), fallbacksA.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].rgbGenConst);
				CHECK(!stages[i].alphaGenConst);
			}
		}
	}
	const std::string fallbacksB = script(
		"{ map textures/c.png blendFunc blend rgbGen const nan 0 0 depthWrite }\n"
		"{ map textures/d.png blendFunc blend alphaGen const inf depthWrite }\n"
		"{ map textures/e.png blendFunc blend rgbGen const abc def ghi depthWrite }\n");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(fallbacksB), fallbacksB.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].rgbGenConst);
				CHECK(!stages[i].alphaGenConst);
			}
		}
	}
	// A syntactically complete const statement with invalid/out-of-range
	// values consumes its whole declared argument count, so keyword-looking
	// argument tokens (for example `rgbGen const 1 1 map`) cannot be
	// re-parsed as stage directives: the stage stays accepted with the
	// dynamic fallback (flags false, identity tint) instead of rejecting the
	// material, stealing a map token, or misreading a blend keyword.
	const std::string keywordValues = script(
		"{ map textures/base.tga rgbGen const 1 1 map }\n"
		"{ map textures/a.png blendFunc blend rgbGen const wave 0 1 depthWrite }\n"
		"{ map textures/b.png blendFunc blend alphaGen const blendFunc }\n");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(keywordValues), keywordValues.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
			CHECK(!stages[0].rgbGenConst);
			CHECK(!stages[0].alphaGenConst);
			CHECK_EQ_STR(stages[1].path, "textures/a.png");
			CHECK(!stages[1].rgbGenConst);
			CHECK(!stages[1].alphaGenConst);
			CHECK_EQ_STR(stages[2].path, "textures/b.png");
			CHECK(!stages[2].rgbGenConst);
			CHECK(!stages[2].alphaGenConst);
		}
	}
	// Truncated const statements at stage end reject the whole shader; the
	// parser never borrows tokens past the stage boundary.
	const std::string truncated = script("{ map textures/base.tga rgbGen const 0.5 0.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncated), truncated.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 0u);
	const std::string truncatedAlpha = script("{ map textures/base.tga alphaGen const }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedAlpha), truncatedAlpha.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 0u);
	// The conservative first-stage opaque helper keeps rejecting const
	// generators: only identity matches the fixed opaque diffuse pass.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga rgbGen const 0.5 0.5 0.5 }", nullptr), "");
	// A dynamic generator followed by a const one on the same stage keeps the
	// const slice intact (wave falls back, the const is preserved).
	const std::string mixed = script(
		"{ map textures/base.tga rgbGen wave sin 0.5 0.5 0 1 rgbGen const 0.5 0.5 0.5 }\n");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixed), mixed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) CHECK(stages[0].rgbGenConst);
	}
}

void testStageDepthWrite() {
	// Bounded per-stage depthWrite: the bare keyword sets the stage-local
	// flag in the simple multi-stage slice; every stage without it keeps
	// depth-write disabled (the adapter's default translucent behavior is
	// depth-test-on/depth-write-off). The flag never changes blend
	// classification: stage zero stays opaque, later stages keep their
	// declared/default alpha blend.
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	const std::string depthWriteStages = script(
		"{ map textures/base.tga depthWrite }\n"
		"{ map textures/alpha.png blendFunc blend }\n"
		"{ map textures/overlay.tga blendFunc blend depthWrite }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(depthWriteStages), depthWriteStages.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK(stages[0].depthWrite);
			CHECK(!stages[1].depthWrite);
			CHECK(stages[2].depthWrite);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_OPAQUE);
			CHECK_EQ_INT(stages[1].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK_EQ_INT(stages[2].blendMode, VRHI_SHADER_BLEND_ALPHA);
		}
	}
	// Keyword matching is case-insensitive, like every other directive.
	const std::string upper = script(
		"{ map textures/base.tga DEPTHWRITE }\n"
		"{ map textures/a.png blendFunc blend }\n");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(stages[0].depthWrite);
			CHECK(!stages[1].depthWrite);
		}
	}
	// The default for every stage is depth-write disabled, including
	// translucent overlay stages with no explicit depthWrite.
	const std::string noFlag = script(
		"{ map textures/base.tga }\n{ map textures/glow.tga blendFunc add }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(noFlag), noFlag.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(!stages[0].depthWrite);
			CHECK(!stages[1].depthWrite);
		}
	}
	// The conservative first-stage opaque helper still accepts depthWrite as
	// part of the fixed opaque diffuse pass (it consumes exactly one token).
	bool ok = false;
	CHECK_EQ_STR(candidateBody("{ map textures/foo/bar.tga depthWrite }", &ok),
		"textures/foo/bar.tga");
	CHECK(ok);
	// depthWrite consumes exactly one token, so a following directive on the
	// same stage still parses normally and the flag stays set.
	const std::string mixed = script(
		"{ map textures/base.tga depthWrite blendFunc blend }\n");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixed), mixed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].depthWrite);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
		}
	}
}

void testStageAlphaFunc() {
	// Bounded stage-local alphaFunc: exactly GE128, GT0, and LT128 are
	// accepted (case-insensitive). The parsed mode lives per stage and never
	// changes blend classification: a stage with alphaFunc keeps its declared
	// blend mode (opaque here).
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	const std::string modes = script(
		"{ map textures/base.tga alphaFunc GE128 }\n"
		"{ map textures/a.png blendFunc blend alphaFunc GT0 }\n"
		"{ map textures/b.png alphaFunc LT128 }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(modes), modes.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_OPAQUE);
			CHECK_EQ_INT(stages[1].alphaFunc, VRHI_SHADER_ALPHAFUNC_GT0);
			CHECK_EQ_INT(stages[1].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK_EQ_INT(stages[2].alphaFunc, VRHI_SHADER_ALPHAFUNC_LT128);
		}
	}
	// Keyword and comparison matching are case-insensitive, like every other
	// directive.
	const std::string upper = script(
		"{ map textures/base.tga ALPHAFUNC ge128 }\n"
		"{ map textures/a.png AlphaFunc Gt0 }\n"
		"{ map textures/b.png alphafunc LT128 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
			CHECK_EQ_INT(stages[1].alphaFunc, VRHI_SHADER_ALPHAFUNC_GT0);
			CHECK_EQ_INT(stages[2].alphaFunc, VRHI_SHADER_ALPHAFUNC_LT128);
		}
	}
	// Default-off: every stage without an explicit alphaFunc keeps alpha
	// testing disabled, including translucent overlay stages.
	const std::string noFlag = script(
		"{ map textures/base.tga }\n{ map textures/glow.tga blendFunc add }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(noFlag), noFlag.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_OFF);
			CHECK_EQ_INT(stages[1].alphaFunc, VRHI_SHADER_ALPHAFUNC_OFF);
		}
	}
	// The mode is stage-local: only the stage that declares alphaFunc gets
	// it, later stages without the keyword stay OFF.
	const std::string stageLocal = script(
		"{ map textures/base.tga alphaFunc GE128 }\n"
		"{ map textures/overlay.tga blendFunc blend }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(stageLocal), stageLocal.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
			CHECK_EQ_INT(stages[1].alphaFunc, VRHI_SHADER_ALPHAFUNC_OFF);
		}
	}
	// Token consumption: alphaFunc consumes exactly two tokens (keyword +
	// comparison), so a following directive on the same stage still parses
	// normally and the mode stays set.
	const std::string mixed = script(
		"{ map textures/base.tga alphaFunc GE128 blendFunc blend }\n"
		"{ alphaFunc GT0 map textures/a.png }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixed), mixed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
			CHECK_EQ_INT(stages[1].alphaFunc, VRHI_SHADER_ALPHAFUNC_GT0);
			CHECK_EQ_STR(stages[1].path, "textures/a.png");
		}
	}
	// Unknown alphaFunc values retain the current safe behavior: the stage
	// stays usable, its map is resolved, and alpha testing stays OFF instead
	// of rejecting an otherwise usable stage.
	const std::string unknown = script(
		"{ map textures/base.tga alphaFunc GE192 }\n"
		"{ map textures/a.png alphaFunc custom blendFunc blend }\n"
		"{ map textures/b.png ALPHAFUNC Ge64 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(unknown), unknown.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_OFF);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
			CHECK_EQ_INT(stages[1].alphaFunc, VRHI_SHADER_ALPHAFUNC_OFF);
			CHECK_EQ_INT(stages[1].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK_EQ_INT(stages[2].alphaFunc, VRHI_SHADER_ALPHAFUNC_OFF);
		}
	}
	// A truncated alphaFunc at the end of a stage is treated the same way as
	// an unknown value: the stage stays usable, never borrowing tokens past
	// the stage, and alpha testing stays OFF.
	const std::string truncated = script("{ map textures/base.tga alphaFunc }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncated), truncated.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_OFF);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
}

void testStageClampMap() {
	// Bounded stage-local clampMap: the Quake3 `clampMap` keyword is
	// distinguished from `map` by a per-stage flag, while the qpath
	// validation stays byte-for-byte the safe validation used for `map`.
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	// Stage-locality: the flag is per stage; `map` keeps the default wrap
	// (false) and `clampmap` sets clamp (true), without changing blend
	// classification or the resolved path.
	const std::string mixed = script(
		"{ map textures/base.tga }\n"
		"{ clampmap textures/clamped.tga blendFunc blend }\n"
		"{ map textures/overlay.tga blendFunc blend }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixed), mixed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK(!stages[0].clampMap);
			CHECK(stages[1].clampMap);
			CHECK(!stages[2].clampMap);
			CHECK_EQ_STR(stages[1].path, "textures/clamped.tga");
			CHECK_EQ_INT(stages[1].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK_EQ_INT(stages[2].blendMode, VRHI_SHADER_BLEND_ALPHA);
		}
	}
	// Keyword matching is case-insensitive, like every other directive.
	const std::string upper = script(
		"{ CLAMPMAP textures/a.tga }\n"
		"{ ClampMap textures/b.tga blendFunc blend }\n"
		"{ map textures/c.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK(stages[0].clampMap);
			CHECK(stages[1].clampMap);
			CHECK(!stages[2].clampMap);
		}
	}
	// Token consumption: clampmap consumes exactly two tokens (keyword +
	// path), so a following directive on the same stage still parses and the
	// flag stays set alongside blend/depthWrite/alphaFunc.
	const std::string mixedDirectives = script(
		"{ clampmap textures/clamped.tga blendFunc blend depthWrite alphaFunc GE128 }\n");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixedDirectives), mixedDirectives.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].clampMap);
			CHECK_EQ_STR(stages[0].path, "textures/clamped.tga");
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK(stages[0].depthWrite);
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
		}
	}
	// clampmap keeps the exact same safe qpath validation as map: traversal
	// and unsupported-extension paths reject the whole shader identically,
	// and a bare name is retained for the extension probe.
	const std::string traversal = script("{ clampmap ../evil.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(traversal), traversal.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 0u);
	const std::string badExt = script("{ clampmap textures/evil.exe }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(badExt), badExt.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 0u);
	const std::string bare = script("{ clampmap textures/foo/bar }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(bare), bare.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].clampMap);
			CHECK_EQ_STR(stages[0].path, "textures/foo/bar");
		}
	}
	// A truncated clampmap at the end of a stage rejects (never borrowing
	// tokens past the stage), and a second map on the same stage rejects.
	const std::string truncated = script("{ clampmap }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncated), truncated.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 0u);
	const std::string secondMap = script("{ clampmap textures/a.tga map textures/b.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(secondMap), secondMap.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 0u);
	// Engine-owned maps cannot be clamped through the direct-image path:
	// `clampmap $lightmap` skips that stage exactly like `map $lightmap`, and
	// each real map stage keeps its own flag.
	const std::string special = script(
		"{ clampmap $lightmap }\n"
		"{ map textures/base.tga }\n"
		"{ clampmap textures/clamped.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(special), special.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(!stages[0].clampMap);
			CHECK(stages[1].clampMap);
		}
	}
	// Default remains wrap: every `map` stage (including animMap's static
	// first-frame fallback, which has no clamp variant) carries clampMap
	// false.
	const std::string defaults = script(
		"{ map textures/base.tga }\n"
		"{ animMap 8 textures/a.tga textures/b.tga blendFunc blend }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(defaults), defaults.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(!stages[0].clampMap);
			CHECK(!stages[1].clampMap);
		}
	}
}

void testStageTcModScale() {
	// Bounded static `tcMod scale s t`: both components must be finite,
	// nonzero, and inside the defined safe magnitude range
	// [1/64, 64]; the parsed scale is carried per stage for the textured
	// material UV scaling. Every other tcMod variant (scroll, rotate, turb,
	// stretch, ...) and every malformed/truncated/zero/out-of-range scale
	// keeps the existing fallback: the stage stays accepted and samples with
	// identity UVs exactly as before.
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	// Basic acceptance: integer, fractional, mirrored (negative), and
	// boundary scales all parse; identity values (1,1) are also accepted and
	// change nothing. Split across two scripts (the parser caps every shader
	// at four stages).
	const std::string accepted = script(
		"{ map textures/base.tga tcMod scale 2 3 }\n"
		"{ map textures/a.png blendFunc blend tcMod scale 0.5 0.25 }\n"
		"{ map textures/b.png blendFunc blend tcMod scale -2 -3 }\n"
		"{ map textures/c.png blendFunc blend tcMod scale 64 0.015625 }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(accepted), accepted.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModScaleS > 1.99f && stages[0].tcModScaleS < 2.01f);
			CHECK(stages[0].tcModScaleT > 2.99f && stages[0].tcModScaleT < 3.01f);
			CHECK(stages[1].tcModScale);
			CHECK(stages[1].tcModScaleS > 0.49f && stages[1].tcModScaleS < 0.51f);
			CHECK(stages[1].tcModScaleT > 0.24f && stages[1].tcModScaleT < 0.26f);
			CHECK(stages[2].tcModScale);
			CHECK(stages[2].tcModScaleS < -1.99f && stages[2].tcModScaleS > -2.01f);
			CHECK(stages[2].tcModScaleT < -2.99f && stages[2].tcModScaleT > -3.01f);
			CHECK(stages[3].tcModScale);
			CHECK(stages[3].tcModScaleS > 63.99f && stages[3].tcModScaleS < 64.01f);
			CHECK(stages[3].tcModScaleT > 0.015f && stages[3].tcModScaleT < 0.016f);
		}
	}
	const std::string identityScale = script(
		"{ map textures/d.png blendFunc blend tcMod scale 1 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(identityScale), identityScale.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModScaleS == 1.0f);
			CHECK(stages[0].tcModScaleT == 1.0f);
		}
	}
	// Stage locality: each stage keeps its own scale, and stages without the
	// keyword keep identity (flag false), exactly as before the slice.
	const std::string locality = script(
		"{ map textures/base.tga tcMod scale 2 3 }\n"
		"{ map textures/a.png blendFunc blend }\n"
		"{ map textures/b.png blendFunc blend tcMod scale 4 5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(locality), locality.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModScaleS > 1.99f && stages[0].tcModScaleS < 2.01f);
			CHECK(!stages[1].tcModScale);
			CHECK(stages[1].tcModScaleS == 1.0f);
			CHECK(stages[1].tcModScaleT == 1.0f);
			CHECK(stages[2].tcModScale);
			CHECK(stages[2].tcModScaleT > 4.99f && stages[2].tcModScaleT < 5.01f);
		}
	}
	// The scale statement consumes exactly four tokens, so a following
	// directive on the same stage still parses, and the scale composes with
	// every other bounded stage slice (blend, depthWrite, alphaFunc,
	// clampMap, const generators) without changing any of them.
	const std::string composed = script(
		"{ clampmap textures/clamped.tga tcMod scale 2 3 blendFunc blend depthWrite alphaFunc GE128 rgbGen const 0.5 0.5 0.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(composed), composed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModScaleS > 1.99f && stages[0].tcModScaleS < 2.01f);
			CHECK(stages[0].tcModScaleT > 2.99f && stages[0].tcModScaleT < 3.01f);
			CHECK(stages[0].clampMap);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK(stages[0].depthWrite);
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
			CHECK(stages[0].rgbGenConst);
		}
	}
	// Keyword matching is case-insensitive, like every other directive.
	const std::string upper = script(
		"{ map textures/base.tga TCMOD SCALE 2 2 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) CHECK(stages[0].tcModScale);
	}
	// Statement order is independent: a scale declared before the map on the
	// same stage still parses and still consumes exactly its own tokens.
	const std::string scaleFirst = script(
		"{ tcMod scale 4 4 map textures/base.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(scaleFirst), scaleFirst.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModScaleS > 3.99f && stages[0].tcModScaleS < 4.01f);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// Fallbacks: zero, out-of-range, non-numeric, trailing-junk, and
	// truncated scales keep the stage accepted with identity UVs (flag
	// false) instead of rejecting the material.
	const std::string fallbacks = script(
		"{ map textures/base.tga tcMod scale 0 2 }\n"
		"{ map textures/a.png blendFunc blend tcMod scale 1e30 2 }\n"
		"{ map textures/b.png blendFunc blend tcMod scale 0.0001 1 }\n"
		"{ map textures/c.png blendFunc blend tcMod scale abc def }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(fallbacks), fallbacks.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].tcModScale);
				CHECK(stages[i].tcModScaleS == 1.0f);
				CHECK(stages[i].tcModScaleT == 1.0f);
			}
		}
	}
	// Truncated `tcMod scale` statements at the end of a stage keep the
	// fallback (identity UVs) and never borrow tokens past the stage end.
	const std::string truncated = script("{ map textures/base.tga tcMod scale 2 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncated), truncated.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(!stages[0].tcModScale);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	const std::string truncatedMarker = script("{ map textures/base.tga tcMod scale }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedMarker), truncatedMarker.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) CHECK(!stages[0].tcModScale);
	}
	const std::string truncatedBare = script("{ map textures/base.tga tcMod }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedBare), truncatedBare.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) CHECK(!stages[0].tcModScale);
	}
	// A syntactically complete scale statement with invalid/out-of-range
	// values consumes its whole declared argument count, so keyword-looking
	// argument tokens (for example `tcMod scale 2 map`) cannot be re-parsed
	// as stage directives: the stage stays accepted with identity UVs.
	const std::string keywordValues = script(
		"{ map textures/base.tga tcMod scale 2 map }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(keywordValues), keywordValues.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(!stages[0].tcModScale);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// Unsupported tcMod variants keep the existing fallback: the stage stays
	// accepted with identity UVs, exactly as before this slice.
	const std::string unsupported = script(
		"{ map textures/base.tga tcMod scroll 0.1 0.1 }\n"
		"{ map textures/a.png blendFunc blend tcMod entityTranslate }\n"
		"{ map textures/b.png blendFunc blend tcMod turb 0 1 1 0.5 0.5 }\n"
		"{ map textures/c.png blendFunc blend tcMod stretch sin 0 1 0.5 0.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(unsupported), unsupported.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].tcModScale);
				CHECK(stages[i].tcModScaleS == 1.0f);
				CHECK(stages[i].tcModScaleT == 1.0f);
			}
		}
	}
	// A dynamic tcMod followed by a static scale keeps the scale slice
	// intact (scroll falls back, the scale is preserved).
	const std::string mixed = script(
		"{ map textures/base.tga tcMod scroll 0.1 0.1 tcMod scale 2 2 }\n"
		"{ map textures/a.png blendFunc blend tcMod scale 3 3 tcMod rotate 90 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixed), mixed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModScaleS > 1.99f && stages[0].tcModScaleS < 2.01f);
			CHECK(stages[1].tcModScale);
			CHECK(stages[1].tcModScaleS > 2.99f && stages[1].tcModScaleS < 3.01f);
		}
	}
	// The conservative first-stage opaque helper still rejects tcMod: only
	// identity matches the fixed opaque diffuse pass, so a tcMod scale shader
	// keeps its existing fallback there.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga tcMod scale 2 2 }", nullptr), "");
}

void testStageTcModScroll() {
	// Direct parser-helper bounds/token safety: VRHI_ShaderParseTcModScroll
	// accepts finite in-range rates (zero included), rejects empty,
	// non-numeric, trailing-junk, non-finite, and out-of-range tokens, and
	// never writes through a null output pointer.
	float value = 123.0f;
	CHECK(VRHI_ShaderParseTcModScroll("0", &value));
	CHECK(value == 0.0f);
	CHECK(VRHI_ShaderParseTcModScroll("-16", &value));
	CHECK(value == -16.0f);
	CHECK(VRHI_ShaderParseTcModScroll("0.0625", &value));
	CHECK(value > 0.062f && value < 0.063f);
	CHECK(!VRHI_ShaderParseTcModScroll("", &value));
	CHECK(!VRHI_ShaderParseTcModScroll("abc", &value));
	CHECK(!VRHI_ShaderParseTcModScroll("0.5x", &value));
	CHECK(!VRHI_ShaderParseTcModScroll("nan", &value));
	CHECK(!VRHI_ShaderParseTcModScroll("inf", &value));
	CHECK(!VRHI_ShaderParseTcModScroll("1e30", &value));
	CHECK(!VRHI_ShaderParseTcModScroll("16.0001", &value));
	CHECK(!VRHI_ShaderParseTcModScroll("-16.5", &value));
	CHECK(!VRHI_ShaderParseTcModScroll("0.1", nullptr));

	// Bounded time-based `tcMod scroll s t`: both components must be finite
	// and |value| <= VRHI_SCRIPT_TCMOD_SCROLL_MAX (zero accepted as the
	// identity no-scroll rate); the parsed rates are carried per stage for
	// the textured material UV scroll, composed AFTER the static tcMod
	// scale. Every other tcMod variant (rotate, turb, stretch, ...) and
	// every malformed/truncated/non-finite/out-of-range rate keeps the
	// existing fallback: the stage stays accepted and samples with no
	// scroll exactly as before.
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	// Basic acceptance: fractional, mirrored (negative), boundary, and zero
	// rates all parse; the magnitude bound is inclusive. Split across two
	// scripts (the parser caps every shader at four stages).
	const std::string accepted = script(
		"{ map textures/base.tga tcMod scroll 0.1 0.2 }\n"
		"{ map textures/a.png blendFunc blend tcMod scroll -2 -3 }\n"
		"{ map textures/b.png blendFunc blend tcMod scroll 16 -16 }\n"
		"{ map textures/c.png blendFunc blend tcMod scroll 0 0 }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(accepted), accepted.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			CHECK(stages[0].tcModScroll);
			CHECK(stages[0].tcModScrollS > 0.09f && stages[0].tcModScrollS < 0.11f);
			CHECK(stages[0].tcModScrollT > 0.19f && stages[0].tcModScrollT < 0.21f);
			CHECK(stages[1].tcModScroll);
			CHECK(stages[1].tcModScrollS < -1.99f && stages[1].tcModScrollS > -2.01f);
			CHECK(stages[1].tcModScrollT < -2.99f && stages[1].tcModScrollT > -3.01f);
			CHECK(stages[2].tcModScroll);
			CHECK(stages[2].tcModScrollS == 16.0f);
			CHECK(stages[2].tcModScrollT == -16.0f);
			// A zero rate is accepted as the identity no-scroll case.
			CHECK(stages[3].tcModScroll);
			CHECK(stages[3].tcModScrollS == 0.0f);
			CHECK(stages[3].tcModScrollT == 0.0f);
		}
	}
	// Stage locality: each stage keeps its own rates, and stages without
	// the keyword keep no scroll (flag false, rates 0).
	const std::string locality = script(
		"{ map textures/base.tga tcMod scroll 0.5 0.25 }\n"
		"{ map textures/a.png blendFunc blend }\n"
		"{ map textures/b.png blendFunc blend tcMod scroll 1 2 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(locality), locality.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK(stages[0].tcModScroll);
			CHECK(stages[0].tcModScrollS > 0.49f && stages[0].tcModScrollS < 0.51f);
			CHECK(!stages[1].tcModScroll);
			CHECK(stages[1].tcModScrollS == 0.0f);
			CHECK(stages[1].tcModScrollT == 0.0f);
			CHECK(stages[2].tcModScroll);
			CHECK(stages[2].tcModScrollT > 1.99f && stages[2].tcModScrollT < 2.01f);
		}
	}
	// The scroll statement consumes exactly four tokens, so a following
	// directive still parses, and the scroll composes with the static scale
	// (scale then time scroll) plus every other bounded stage slice without
	// changing any of them.
	const std::string composed = script(
		"{ clampmap textures/clamped.tga tcMod scale 2 3 tcMod scroll 0.5 0.25 blendFunc blend depthWrite alphaFunc GE128 rgbGen const 0.5 0.5 0.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(composed), composed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModScroll);
			CHECK(stages[0].tcModScrollS > 0.49f && stages[0].tcModScrollS < 0.51f);
			CHECK(stages[0].tcModScrollT > 0.24f && stages[0].tcModScrollT < 0.26f);
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModScaleS > 1.99f && stages[0].tcModScaleS < 2.01f);
			CHECK(stages[0].clampMap);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK(stages[0].depthWrite);
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
			CHECK(stages[0].rgbGenConst);
		}
	}
	// Keyword matching is case-insensitive, like every other directive.
	const std::string upper = script(
		"{ map textures/base.tga TCMOD SCROLL 0.1 0.1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) CHECK(stages[0].tcModScroll);
	}
	// Statement order is independent: a scroll declared before the map still
	// parses and consumes exactly its own tokens.
	const std::string scrollFirst = script(
		"{ tcMod scroll 0.5 0.5 map textures/base.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(scrollFirst), scrollFirst.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModScroll);
			CHECK(stages[0].tcModScrollS > 0.49f && stages[0].tcModScrollS < 0.51f);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// Fallbacks: out-of-range, non-numeric, trailing-junk, and truncated
	// rates keep the stage accepted with no scroll (flag false, rates 0)
	// instead of rejecting the material.
	const std::string fallbacks = script(
		"{ map textures/base.tga tcMod scroll 1e30 0.1 }\n"
		"{ map textures/a.png blendFunc blend tcMod scroll 100 -100 }\n"
		"{ map textures/b.png blendFunc blend tcMod scroll abc def }\n"
		"{ map textures/c.png blendFunc blend tcMod scroll 0.1 2x }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(fallbacks), fallbacks.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].tcModScroll);
				CHECK(stages[i].tcModScrollS == 0.0f);
				CHECK(stages[i].tcModScrollT == 0.0f);
			}
		}
	}
	// Truncated `tcMod scroll` statements at the end of a stage keep the
	// fallback (no scroll) and never borrow tokens past the stage end.
	const std::string truncated = script("{ map textures/base.tga tcMod scroll 0.1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncated), truncated.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(!stages[0].tcModScroll);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	const std::string truncatedMarker = script("{ map textures/base.tga tcMod scroll }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedMarker), truncatedMarker.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) CHECK(!stages[0].tcModScroll);
	}
	// A syntactically complete scroll statement with invalid/out-of-range
	// rates consumes its whole declared argument count, so keyword-looking
	// argument tokens (for example `tcMod scroll 0.1 map`) cannot be
	// re-parsed as stage directives: the stage stays accepted with no
	// scroll.
	const std::string keywordValues = script(
		"{ map textures/base.tga tcMod scroll 0.1 map }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(keywordValues), keywordValues.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(!stages[0].tcModScroll);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// Unsupported tcMod variants keep the existing fallback: the stage stays
	// accepted with no scroll, exactly as before this slice.
	const std::string unsupported = script(
		"{ map textures/base.tga tcMod entityTranslate }\n"
		"{ map textures/a.png blendFunc blend tcMod turb 0 1 1 0.5 0.5 }\n"
		"{ map textures/b.png blendFunc blend tcMod stretch sin 0 1 0.5 0.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(unsupported), unsupported.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].tcModScroll);
				CHECK(stages[i].tcModScrollS == 0.0f);
				CHECK(stages[i].tcModScrollT == 0.0f);
			}
		}
	}
	// A rotate plus a scroll compose on one stage (fixed order: scale, then
	// rotate, then scroll), and a scroll plus a scale compose (scale then
	// time scroll).
	const std::string mixed = script(
		"{ map textures/base.tga tcMod rotate 90 tcMod scroll 0.1 0.2 }\n"
		"{ map textures/a.png blendFunc blend tcMod scroll 0.5 0.5 tcMod scale 2 2 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixed), mixed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(stages[0].tcModScroll);
			CHECK(stages[0].tcModScrollS > 0.09f && stages[0].tcModScrollS < 0.11f);
			CHECK(stages[0].tcModRotate);
			CHECK(stages[0].tcModRotateDegreesPerSecond == 90.0f);
			CHECK(!stages[0].tcModScale);
			CHECK(stages[1].tcModScroll);
			CHECK(stages[1].tcModScrollT > 0.49f && stages[1].tcModScrollT < 0.51f);
			CHECK(stages[1].tcModScale);
			CHECK(stages[1].tcModScaleS > 1.99f && stages[1].tcModScaleS < 2.01f);
		}
	}
	// The conservative first-stage opaque helper still rejects tcMod: only
	// identity matches the fixed opaque diffuse pass, so a tcMod scroll
	// shader keeps its existing fallback there.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga tcMod scroll 0.1 0.1 }", nullptr), "");
}

void testStageTcModRotate() {
	// Direct parser-helper bounds/token safety: VRHI_ShaderParseTcModRotate
	// accepts finite in-range rates in degrees per second (zero included),
	// rejects empty, non-numeric, trailing-junk, non-finite, and
	// out-of-range tokens, and never writes through a null output pointer.
	// The GL1 renderer's ParseTcMod reads exactly one atof token and stores
	// it as rotateSpeed; RB_CalcRotateTexCoords (tr_shade_calc.c) then
	// computes degs = -degsPerSecond * shaderTime and rotates the UVs about
	// (0.5, 0.5), so the sign is negative and the unit is degrees per
	// second.
	float value = 123.0f;
	CHECK(VRHI_ShaderParseTcModRotate("0", &value));
	CHECK(value == 0.0f);
	CHECK(VRHI_ShaderParseTcModRotate("90", &value));
	CHECK(value == 90.0f);
	CHECK(VRHI_ShaderParseTcModRotate("-45.5", &value));
	CHECK(value == -45.5f);
	CHECK(VRHI_ShaderParseTcModRotate("3600", &value));
	CHECK(value == 3600.0f);
	CHECK(!VRHI_ShaderParseTcModRotate("", &value));
	CHECK(!VRHI_ShaderParseTcModRotate("abc", &value));
	CHECK(!VRHI_ShaderParseTcModRotate("90x", &value));
	CHECK(!VRHI_ShaderParseTcModRotate("nan", &value));
	CHECK(!VRHI_ShaderParseTcModRotate("inf", &value));
	CHECK(!VRHI_ShaderParseTcModRotate("1e30", &value));
	CHECK(!VRHI_ShaderParseTcModRotate("3600.001", &value));
	CHECK(!VRHI_ShaderParseTcModRotate("-3600.5", &value));
	CHECK(!VRHI_ShaderParseTcModRotate("90", nullptr));

	// Bounded time-based `tcMod rotate degsPerSecond`: the single rate must
	// be finite and |value| <= VRHI_SCRIPT_TCMOD_ROTATE_MAX degrees per
	// second (zero accepted as the identity no-rotate rate); the parsed rate
	// is carried per stage for the textured material UV rotation around the
	// texture center (0.5, 0.5) by -rate * frameTimeSeconds degrees,
	// composed between the static tcMod scale and the time-based scroll
	// (fixed order: scale, then rotate, then scroll). Every other tcMod
	// variant (turb, stretch, entityTranslate, ...) and every
	// malformed/truncated/non-finite/out-of-range rate keeps the existing
	// fallback: the stage stays accepted and samples with no rotation
	// exactly as before.
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	// Basic acceptance: fractional, mirrored (negative), boundary, and zero
	// rates all parse; the magnitude bound is inclusive. Split across two
	// scripts (the parser caps every shader at four stages).
	const std::string accepted = script(
		"{ map textures/base.tga tcMod rotate 90 }\n"
		"{ map textures/a.png blendFunc blend tcMod rotate -45.5 }\n"
		"{ map textures/b.png blendFunc blend tcMod rotate 3600 }\n"
		"{ map textures/c.png blendFunc blend tcMod rotate 0 }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(accepted), accepted.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			CHECK(stages[0].tcModRotate);
			CHECK(stages[0].tcModRotateDegreesPerSecond == 90.0f);
			CHECK(stages[1].tcModRotate);
			CHECK(stages[1].tcModRotateDegreesPerSecond == -45.5f);
			CHECK(stages[2].tcModRotate);
			CHECK(stages[2].tcModRotateDegreesPerSecond == 3600.0f);
			// A zero rate is accepted as the identity no-rotate case.
			CHECK(stages[3].tcModRotate);
			CHECK(stages[3].tcModRotateDegreesPerSecond == 0.0f);
		}
	}
	// Stage locality: each stage keeps its own rate, and stages without the
	// keyword keep no rotate (flag false, rate 0).
	const std::string locality = script(
		"{ map textures/base.tga tcMod rotate 90 }\n"
		"{ map textures/a.png blendFunc blend }\n"
		"{ map textures/b.png blendFunc blend tcMod rotate -30 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(locality), locality.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK(stages[0].tcModRotate);
			CHECK(stages[0].tcModRotateDegreesPerSecond == 90.0f);
			CHECK(!stages[1].tcModRotate);
			CHECK(stages[1].tcModRotateDegreesPerSecond == 0.0f);
			CHECK(stages[2].tcModRotate);
			CHECK(stages[2].tcModRotateDegreesPerSecond == -30.0f);
		}
	}
	// The rotate statement consumes exactly three tokens, so a following
	// directive still parses, and the rotate composes with the static scale
	// and the time scroll (fixed order: scale, then rotate, then scroll)
	// plus every other bounded stage slice without changing any of them.
	const std::string composed = script(
		"{ clampmap textures/clamped.tga tcMod scale 2 3 tcMod rotate 90 tcMod scroll 0.5 0.25 blendFunc blend depthWrite alphaFunc GE128 rgbGen const 0.5 0.5 0.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(composed), composed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModRotate);
			CHECK(stages[0].tcModRotateDegreesPerSecond == 90.0f);
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModScaleS > 1.99f && stages[0].tcModScaleS < 2.01f);
			CHECK(stages[0].tcModScroll);
			CHECK(stages[0].tcModScrollS > 0.49f && stages[0].tcModScrollS < 0.51f);
			CHECK(stages[0].clampMap);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK(stages[0].depthWrite);
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
			CHECK(stages[0].rgbGenConst);
		}
	}
	// Keyword matching is case-insensitive, like every other directive.
	const std::string upper = script(
		"{ map textures/base.tga TCMOD ROTATE 90 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) CHECK(stages[0].tcModRotate);
	}
	// Statement order is independent: a rotate declared before the map still
	// parses and consumes exactly its own tokens.
	const std::string rotateFirst = script(
		"{ tcMod rotate 90 map textures/base.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(rotateFirst), rotateFirst.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModRotate);
			CHECK(stages[0].tcModRotateDegreesPerSecond == 90.0f);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// Fallbacks: out-of-range, non-numeric, trailing-junk, and truncated
	// rates keep the stage accepted with no rotate (flag false, rate 0)
	// instead of rejecting the material.
	const std::string fallbacks = script(
		"{ map textures/base.tga tcMod rotate 1e30 }\n"
		"{ map textures/a.png blendFunc blend tcMod rotate abc }\n"
		"{ map textures/b.png blendFunc blend tcMod rotate 90x }\n"
		"{ map textures/c.png blendFunc blend tcMod rotate -3600.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(fallbacks), fallbacks.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].tcModRotate);
				CHECK(stages[i].tcModRotateDegreesPerSecond == 0.0f);
			}
		}
	}
	// Truncated `tcMod rotate` statements at the end of a stage keep the
	// fallback (no rotate) and never borrow tokens past the stage end.
	const std::string truncated = script("{ map textures/base.tga tcMod rotate }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncated), truncated.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(!stages[0].tcModRotate);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	const std::string truncatedBare = script("{ map textures/base.tga tcMod }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedBare), truncatedBare.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) CHECK(!stages[0].tcModRotate);
	}
	// A syntactically complete but invalid rotate statement consumes its
	// whole declared argument count (three tokens: keyword + "rotate" + one
	// rate), so leftover numeric argument tokens (for example
	// `tcMod rotate 1e30 2`) cannot be re-parsed as stage directives: the
	// stage stays accepted with no rotate. A valid rate followed by extra
	// tokens keeps the rotate and the extras are bounded and ignored, exactly
	// like the GL1 single-atof ParseTcMod.
	const std::string invalidConsumed = script(
		"{ map textures/base.tga tcMod rotate 1e30 2 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(invalidConsumed), invalidConsumed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(!stages[0].tcModRotate);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	const std::string extraArg = script(
		"{ map textures/base.tga tcMod rotate 90 2 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(extraArg), extraArg.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModRotate);
			CHECK(stages[0].tcModRotateDegreesPerSecond == 90.0f);
		}
	}
	// Unsupported tcMod variants keep the existing fallback: the stage stays
	// accepted with no rotate, exactly as before this slice.
	const std::string unsupported = script(
		"{ map textures/base.tga tcMod turb 0 1 1 0.5 0.5 }\n"
		"{ map textures/a.png blendFunc blend tcMod stretch sin 0 1 0.5 0.5 }\n"
		"{ map textures/b.png blendFunc blend tcMod entityTranslate }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(unsupported), unsupported.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].tcModRotate);
				CHECK(stages[i].tcModRotateDegreesPerSecond == 0.0f);
			}
		}
	}
	// A rotate composes with the supported scale and scroll slices: each
	// statement keeps its own parsed values regardless of script order.
	const std::string mixed = script(
		"{ map textures/base.tga tcMod rotate 90 tcMod scroll 0.1 0.2 }\n"
		"{ map textures/a.png blendFunc blend tcMod scale 2 2 tcMod rotate -45 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixed), mixed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(stages[0].tcModRotate);
			CHECK(stages[0].tcModRotateDegreesPerSecond == 90.0f);
			CHECK(stages[0].tcModScroll);
			CHECK(stages[0].tcModScrollS > 0.09f && stages[0].tcModScrollS < 0.11f);
			CHECK(!stages[0].tcModScale);
			CHECK(stages[1].tcModRotate);
			CHECK(stages[1].tcModRotateDegreesPerSecond == -45.0f);
			CHECK(stages[1].tcModScale);
			CHECK(stages[1].tcModScaleS > 1.99f && stages[1].tcModScaleS < 2.01f);
			CHECK(!stages[1].tcModScroll);
		}
	}
	// The conservative first-stage opaque helper still rejects tcMod: only
	// identity matches the fixed opaque diffuse pass, so a tcMod rotate
	// shader keeps its existing fallback there.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga tcMod rotate 90 }", nullptr), "");
}

void testStageTcModTurbStretch() {
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	// Lava-style turb plus a flame-style stretch: both preserved with
	// script-order parameter values.
	const std::string accepted = script(
		"{ map textures/base.tga tcMod turb .5 -0.4 0.5 0.1 }\n"
		"{ map textures/a.png blendFunc blend tcMod stretch sin 0 1 0 8 }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(accepted), accepted.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(stages[0].tcModTurb);
			CHECK(stages[0].tcModTurbValues[0] > 0.49f && stages[0].tcModTurbValues[0] < 0.51f);
			CHECK(stages[0].tcModTurbValues[1] > -0.41f && stages[0].tcModTurbValues[1] < -0.39f);
			CHECK(stages[0].tcModTurbValues[2] > 0.49f && stages[0].tcModTurbValues[2] < 0.51f);
			CHECK(stages[0].tcModTurbValues[3] > 0.09f && stages[0].tcModTurbValues[3] < 0.11f);
			CHECK_EQ_INT(stages[0].tcModStretchFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_INT(stages[1].tcModStretchFunc, VRHI_SHADER_WAVE_SIN);
			CHECK(stages[1].tcModStretchValues[1] > 0.99f && stages[1].tcModStretchValues[1] < 1.01f);
			CHECK(stages[1].tcModStretchValues[3] > 7.99f && stages[1].tcModStretchValues[3] < 8.01f);
			CHECK(!stages[1].tcModTurb);
		}
	}
	// Every stretch waveform is accepted; matching is case-insensitive and a
	// stage may carry turb, stretch, scale, rotate, and scroll together.
	const std::string composed = script(
		"{ map textures/base.tga tcMod TURB 0 1 0 1 tcMod STRETCH inverseSawTooth 1 0.5 0 2 tcMod scale 2 3 tcMod rotate 30 tcMod scroll 0.5 -0.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(composed), composed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].tcModTurb);
			CHECK_EQ_INT(stages[0].tcModStretchFunc, VRHI_SHADER_WAVE_INVERSE_SAWTOOTH);
			CHECK(stages[0].tcModScale);
			CHECK(stages[0].tcModRotate);
			CHECK(stages[0].tcModScroll);
		}
	}
	const std::string funcs = script(
		"{ map textures/0.tga tcMod stretch square 0 1 0 1 } "
		"{ map textures/1.tga blendFunc blend tcMod stretch triangle 0 1 0 1 } "
		"{ map textures/2.tga blendFunc blend tcMod stretch sawtooth 0 1 0 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(funcs), funcs.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK_EQ_INT(stages[0].tcModStretchFunc, VRHI_SHADER_WAVE_SQUARE);
			CHECK_EQ_INT(stages[1].tcModStretchFunc, VRHI_SHADER_WAVE_TRIANGLE);
			CHECK_EQ_INT(stages[2].tcModStretchFunc, VRHI_SHADER_WAVE_SAWTOOTH);
		}
	}
	// Invalid statements keep the stage accepted with identity UVs: unknown
	// stretch func (noise), non-numeric, and out-of-range parameters.
	const std::string fallbacks = script(
		"{ map textures/base.tga tcMod stretch noise 0 1 0 1 }\n"
		"{ map textures/a.png blendFunc blend tcMod turb 0 abc 0 1 }\n"
		"{ map textures/b.png blendFunc blend tcMod turb 0 100 0 1 }\n"
		"{ map textures/c.png blendFunc blend tcMod stretch sin 0 1 0 100 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(fallbacks), fallbacks.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK(!stages[i].tcModTurb);
				CHECK_EQ_INT(stages[i].tcModStretchFunc, VRHI_SHADER_WAVE_NONE);
			}
		}
	}
	// Truncated statements at the stage end consume only the keyword and the
	// variant marker, keep identity UVs, and never borrow past the stage.
	const std::string truncated = script(
		"{ map textures/base.tga tcMod turb }\n"
		"{ map textures/a.png blendFunc blend tcMod turb 0.5 0.5 }\n"
		"{ map textures/b.png blendFunc blend tcMod stretch sin 0 1 0 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncated), truncated.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK(!stages[0].tcModTurb);
			CHECK(!stages[1].tcModTurb);
			CHECK_EQ_INT(stages[2].tcModStretchFunc, VRHI_SHADER_WAVE_NONE);
		}
	}
	// Keyword-looking arguments are fully consumed, never re-parsed as stage
	// directives: `blend` stays a turb amplitude, `map` stays a stretch func.
	const std::string keywordValues = script(
		"{ map textures/base.tga tcMod turb blend 1 2 3 tcMod stretch map 0 1 0 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(keywordValues), keywordValues.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(!stages[0].tcModTurb);
			CHECK_EQ_INT(stages[0].tcModStretchFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_OPAQUE);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// A repeated turb keeps the last statement, matching scale/scroll/rotate.
	const std::string repeated = script(
		"{ map textures/base.tga tcMod turb 0 1 0 1 tcMod turb 0 2 0 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(repeated), repeated.size(), wanted, &results));
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		if (stages.size() == 1) {
			CHECK(stages[0].tcModTurb);
			CHECK(stages[0].tcModTurbValues[1] > 1.99f && stages[0].tcModTurbValues[1] < 2.01f);
		}
	}
	// The conservative first-stage opaque helper still rejects tcMod: only
	// identity matches the fixed opaque diffuse pass.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga tcMod turb 0 1 0 1 }", nullptr), "");
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga tcMod stretch sin 0 1 0 1 }", nullptr), "");
}

void testStageAnimMap() {
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	// Flame-style multi-frame animation: rate plus every frame in order.
	const std::string accepted = script(
		"{ animMap 8 textures/sfx/flame1.tga textures/sfx/flame2.tga textures/sfx/flame3.tga blendFunc GL_SRC_ALPHA GL_ONE }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(accepted), accepted.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK(stages[0].animFps > 7.99f && stages[0].animFps < 8.01f);
			CHECK_EQ_INT(stages[0].animPaths.size(), 3u);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA_ADDITIVE);
			if (stages[0].animPaths.size() == 3) {
				CHECK_EQ_STR(stages[0].animPaths[0], "textures/sfx/flame1.tga");
				CHECK_EQ_STR(stages[0].animPaths[2], "textures/sfx/flame3.tga");
				CHECK_EQ_STR(stages[0].path, "textures/sfx/flame1.tga");
			}
		}
	}
	// Keyword matching is case-insensitive; collection stops before a
	// following directive instead of swallowing it as a frame.
	const std::string upper = script(
		"{ ANIMMAP 2 textures/a.tga textures/b.tga BLENDFUNC BLEND RGBGEN WAVE SIN 0.5 0.5 0 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		if (stages.size() == 1) {
			CHECK_EQ_INT(stages[0].animPaths.size(), 2u);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_SIN);
		}
	}
	// Invalid rates fall back to the static first frame with all frames kept.
	const std::string badFps = script(
		"{ map textures/base.tga }\n"
		"{ animMap abc textures/a.tga textures/b.tga blendFunc blend }\n"
		"{ animMap -1 textures/c.tga blendFunc blend }\n"
		"{ animMap 100 textures/d.tga blendFunc blend }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(badFps), badFps.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			CHECK(stages[0].animPaths.empty());
			for (std::size_t i = 1; i < 4; ++i) {
				CHECK(stages[i].animFps == 0.0f);
				CHECK(!stages[i].animPaths.empty());
			}
			CHECK_EQ_INT(stages[1].animPaths.size(), 2u);
		}
	}
	// Excess frames keep the first eight; the rest are ignored like GL1.
	const std::string excess = script(
		"{ animMap 5 textures/0.tga textures/1.tga textures/2.tga textures/3.tga textures/4.tga textures/5.tga textures/6.tga textures/7.tga textures/8.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(excess), excess.size(), wanted, &results));
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		if (stages.size() == 1) CHECK_EQ_INT(stages[0].animPaths.size(), 8u);
	}
	// Special, unsafe, missing, and truncated frame lists reject the shader.
	const std::string special = script("{ animMap 8 $lightmap blendFunc blend }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(special), special.size(), wanted, &results));
	CHECK(results.empty());
	const std::string unsafe = script("{ animMap 8 textures/ok.tga ../evil.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(unsafe), unsafe.size(), wanted, &results));
	CHECK(results.empty());
	const std::string noFrames = script("{ animMap 8 blendFunc blend }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(noFrames), noFrames.size(), wanted, &results));
	CHECK(results.empty());
	const std::string truncatedRate = script("{ animMap }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedRate), truncatedRate.size(), wanted, &results));
	CHECK(results.empty());
	const std::string truncatedFrames = script("{ animMap 8 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedFrames), truncatedFrames.size(), wanted, &results));
	CHECK(results.empty());
	// A second image statement in the same stage still rejects the shader.
	const std::string doubleAnim = script(
		"{ animMap 8 textures/a.tga animMap 8 textures/b.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(doubleAnim), doubleAnim.size(), wanted, &results));
	CHECK(results.empty());
	// The conservative first-stage opaque helper still rejects animMap.
	CHECK_EQ_STR(candidateBody(
		"{ animMap 8 textures/foo/bar.tga textures/baz.tga }", nullptr), "");
}

void testStageTcGenEnvironment() {
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	// Chrome-style window stage: environment reflection plus blend.
	const std::string accepted = script(
		"{ map textures/base.tga }\n"
		"{ map textures/chrome.tga tcGen environment rgbGen identity blendFunc blend depthFunc equal }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(accepted), accepted.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK(!stages[0].tcGenEnvironment);
			CHECK(stages[1].tcGenEnvironment);
			CHECK_EQ_INT(stages[1].blendMode, VRHI_SHADER_BLEND_ALPHA);
		}
	}
	// Matching is case-insensitive; `texture` is the identity default.
	const std::string upper = script(
		"{ map textures/a.tga tcGen ENVIRONMENT }\n"
		"{ map textures/b.tga blendFunc blend tcGen TEXTURE }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		if (stages.size() == 2) {
			CHECK(stages[0].tcGenEnvironment);
			CHECK(!stages[1].tcGenEnvironment);
		}
	}
	// Other tcGen variants and a bare keyword keep the stage accepted with
	// authored UVs, exactly as before this slice.
	const std::string fallbacks = script(
		"{ map textures/a.tga tcGen lightmap }\n"
		"{ map textures/b.tga blendFunc blend tcGen vector 0 1 0 0 1 0 }\n"
		"{ map textures/c.tga blendFunc blend tcGen fog }\n"
		"{ map textures/d.tga blendFunc blend tcGen }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(fallbacks), fallbacks.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			for (std::size_t i = 0; i < stages.size(); ++i)
				CHECK(!stages[i].tcGenEnvironment);
		}
	}
	// The conservative first-stage opaque helper still rejects tcGen.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga tcGen environment }", nullptr), "");
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga tcGen texture }", nullptr), "");
}

void testDeformWaveMath() {
	// GL1 tr_init.c table shapes evaluated through the shared CPU helper:
	// exact values at canonical fractions, truncation wrap for negatives.
	CHECK(VRHI_WaveTableValue(VRHI_SHADER_WAVE_SIN, 0.0f) == 0.0f);
	const float sinQuarter = VRHI_WaveTableValue(VRHI_SHADER_WAVE_SIN, 0.25f);
	CHECK(sinQuarter > 0.999f && sinQuarter <= 1.0f);
	CHECK_EQ_INT(VRHI_WaveTableValue(VRHI_SHADER_WAVE_SQUARE, 0.0f) > 0.0f ? 1 : 0, 1);
	CHECK_EQ_INT(VRHI_WaveTableValue(VRHI_SHADER_WAVE_SQUARE, 0.75f) < 0.0f ? 1 : 0, 1);
	const float saw = VRHI_WaveTableValue(VRHI_SHADER_WAVE_SAWTOOTH, 0.5f);
	CHECK(saw > 0.499f && saw < 0.501f);
	const float invSaw = VRHI_WaveTableValue(VRHI_SHADER_WAVE_INVERSE_SAWTOOTH, 0.5f);
	CHECK(invSaw > 0.499f && invSaw < 0.501f);
	const float triZero = VRHI_WaveTableValue(VRHI_SHADER_WAVE_TRIANGLE, 0.0f);
	CHECK(triZero > -0.001f && triZero < 0.001f);
	const float triQuarter = VRHI_WaveTableValue(VRHI_SHADER_WAVE_TRIANGLE, 0.25f);
	CHECK(triQuarter > 0.999f && triQuarter <= 1.0f);
	CHECK_EQ_INT(VRHI_WaveTableValue(VRHI_SHADER_WAVE_NONE, 0.3f) == 0.0f ? 1 : 0, 1);
	// Negative cycles wrap identically to positive ones (C-cast semantics).
	CHECK(VRHI_WaveTableValue(VRHI_SHADER_WAVE_SIN, -0.25f) ==
		VRHI_WaveTableValue(VRHI_SHADER_WAVE_SIN, 0.75f));
	CHECK(VRHI_WaveTableValue(VRHI_SHADER_WAVE_SAWTOOTH, -0.5f) ==
		VRHI_WaveTableValue(VRHI_SHADER_WAVE_SAWTOOTH, 0.5f));
	// Unclamped evaluation keeps signed values (no [0,1] clamp): base plus
	// table times amplitude at phase + time * frequency.
	const float waveZero = VRHI_EvalWaveFormUnclamped(
		VRHI_SHADER_WAVE_SIN, 0.5f, 0.5f, 0.0f, 1.0f, 0.0f);
	CHECK(waveZero > 0.499f && waveZero < 0.501f);
	const float waveNeg = VRHI_EvalWaveFormUnclamped(
		VRHI_SHADER_WAVE_SAWTOOTH, 0.0f, 2.0f, 0.75f, 0.0f, 100.0f);
	CHECK(waveNeg > 1.499f && waveNeg < 1.501f);
}

void testStageDeform() {
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	std::unordered_map<std::string, VRHI_ShaderDeform> deforms;
	// Lava-style wave plus a blended stage: shader-level deform attaches to
	// the material while stages parse normally.
	const std::string wave = script(
		"deformVertexes wave 1 sin .01 .03 0 0.2 "
		"{ map textures/base.tga } "
		"{ map textures/a.png blendFunc blend }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(wave), wave.size(), wanted, &results, &deforms));
	CHECK_EQ_INT(results.size(), 1u);
	CHECK_EQ_INT(deforms.size(), 1u);
	if (deforms.size() == 1) {
		const VRHI_ShaderDeform &deform = deforms["textures/foo/bar"];
		CHECK_EQ_INT(deform.kind, VRHI_SHADER_DEFORM_WAVE);
		CHECK_EQ_INT(deform.waveFunc, VRHI_SHADER_WAVE_SIN);
		CHECK(deform.spread > 0.99f && deform.spread < 1.01f);
		CHECK(deform.wave[0] > 0.009f && deform.wave[0] < 0.011f);
		CHECK(deform.wave[1] > 0.029f && deform.wave[1] < 0.031f);
		CHECK(deform.wave[3] > 0.19f && deform.wave[3] < 0.21f);
	}
	// Divisor semantics: spread is 1/divisor, matched case-insensitively.
	const std::string spread = script(
		"{ map textures/base.tga } DEFORMVERTEXES WAVE 32 SIN 1.5 1.7 .5 1");
	results.clear();
	deforms.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(spread), spread.size(), wanted, &results, &deforms));
	if (deforms.size() == 1) {
		const VRHI_ShaderDeform &deform = deforms["textures/foo/bar"];
		CHECK_EQ_INT(deform.kind, VRHI_SHADER_DEFORM_WAVE);
		CHECK(deform.spread > 0.031f && deform.spread < 0.032f);
	}
	// Mover-style translation and bulge forms.
	const std::string move = script(
		"deformVertexes move 0 0 8 sin 0 1 0 0.5 { map textures/base.tga }");
	results.clear();
	deforms.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(move), move.size(), wanted, &results, &deforms));
	if (deforms.size() == 1) {
		const VRHI_ShaderDeform &deform = deforms["textures/foo/bar"];
		CHECK_EQ_INT(deform.kind, VRHI_SHADER_DEFORM_MOVE);
		CHECK(deform.move[2] > 7.99f && deform.move[2] < 8.01f);
		CHECK_EQ_INT(deform.waveFunc, VRHI_SHADER_WAVE_SIN);
		CHECK(deform.wave[1] > 0.99f && deform.wave[1] < 1.01f);
	}
	const std::string bulge = script(
		"{ map textures/base.tga } deformVertexes bulge 4 2 1.5");
	results.clear();
	deforms.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(bulge), bulge.size(), wanted, &results, &deforms));
	if (deforms.size() == 1) {
		const VRHI_ShaderDeform &deform = deforms["textures/foo/bar"];
		CHECK_EQ_INT(deform.kind, VRHI_SHADER_DEFORM_BULGE);
		CHECK(deform.bulge[0] > 3.99f && deform.bulge[0] < 4.01f);
		CHECK(deform.bulge[1] > 1.99f && deform.bulge[1] < 2.01f);
		CHECK(deform.bulge[2] > 1.49f && deform.bulge[2] < 1.51f);
	}
	// Only the first valid statement is kept; ignored subtypes (autosprite,
	// normal, unknown) and invalid statements (zero divisor, bad func,
	// truncated, out-of-range) leave the material static but accepted.
	const std::string firstWins = script(
		"deformVertexes wave 1 sin 0 1 0 1 deformVertexes move 0 0 8 sin 0 1 0 1 "
		"{ map textures/base.tga }");
	results.clear();
	deforms.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(firstWins), firstWins.size(), wanted, &results, &deforms));
	if (deforms.size() == 1)
		CHECK_EQ_INT(deforms["textures/foo/bar"].kind, VRHI_SHADER_DEFORM_WAVE);
	const std::string ignored = script(
		"deformVertexes autosprite deformVertexes normal 3 0.5 deformVertexes frobnicate 1 2 "
		"deformVertexes wave 0 sin 0 1 0 1 deformVertexes wave 1 noise 0 1 0 1 "
		"deformVertexes move 0 0 99999 sin 0 1 0 1 deformVertexes bulge 1 2 "
		"{ map textures/base.tga }");
	results.clear();
	deforms.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(ignored), ignored.size(), wanted, &results, &deforms));
	CHECK_EQ_INT(results.size(), 1u);
	CHECK(deforms.empty());
}

void testStageWaveGenerators() {
	// Direct parser-helper bounds/token safety: VRHI_ShaderParseWaveConstant
	// accepts finite in-range parameters (zero and the inclusive |16| bound
	// included), rejects empty, non-numeric, trailing-junk, non-finite, and
	// out-of-range tokens, and never writes through a null output pointer.
	float value = 123.0f;
	CHECK(VRHI_ShaderParseWaveConstant("0", &value));
	CHECK(value == 0.0f);
	CHECK(VRHI_ShaderParseWaveConstant("-16", &value));
	CHECK(value == -16.0f);
	CHECK(VRHI_ShaderParseWaveConstant("0.5", &value));
	CHECK(value == 0.5f);
	CHECK(VRHI_ShaderParseWaveConstant("16", &value));
	CHECK(value == 16.0f);
	CHECK(VRHI_ShaderParseWaveConstant("-2.5", &value));
	CHECK(value == -2.5f);
	CHECK(!VRHI_ShaderParseWaveConstant("", &value));
	CHECK(!VRHI_ShaderParseWaveConstant("abc", &value));
	CHECK(!VRHI_ShaderParseWaveConstant("0.5x", &value));
	CHECK(!VRHI_ShaderParseWaveConstant("nan", &value));
	CHECK(!VRHI_ShaderParseWaveConstant("inf", &value));
	CHECK(!VRHI_ShaderParseWaveConstant("1e30", &value));
	CHECK(!VRHI_ShaderParseWaveConstant("16.0001", &value));
	CHECK(!VRHI_ShaderParseWaveConstant("-16.5", &value));
	CHECK(!VRHI_ShaderParseWaveConstant("0.5", nullptr));
	// The waveform-name mapper accepts exactly the five Quake3 forms,
	// case-insensitively (the GL1 NameToGenFunc uses Q_stricmp), and maps
	// every other name (noise included) to NONE so the stage keeps the
	// existing dynamic fallback.
	CHECK_EQ_INT(VRHI_ShaderWaveFuncFromName("sin"), VRHI_SHADER_WAVE_SIN);
	CHECK_EQ_INT(VRHI_ShaderWaveFuncFromName("SQUARE"), VRHI_SHADER_WAVE_SQUARE);
	CHECK_EQ_INT(VRHI_ShaderWaveFuncFromName("Triangle"), VRHI_SHADER_WAVE_TRIANGLE);
	CHECK_EQ_INT(VRHI_ShaderWaveFuncFromName("sawtooth"), VRHI_SHADER_WAVE_SAWTOOTH);
	CHECK_EQ_INT(VRHI_ShaderWaveFuncFromName("InverseSawtooth"), VRHI_SHADER_WAVE_INVERSE_SAWTOOTH);
	CHECK_EQ_INT(VRHI_ShaderWaveFuncFromName("noise"), VRHI_SHADER_WAVE_NONE);
	CHECK_EQ_INT(VRHI_ShaderWaveFuncFromName("bogus"), VRHI_SHADER_WAVE_NONE);
	CHECK_EQ_INT(VRHI_ShaderWaveFuncFromName(""), VRHI_SHADER_WAVE_NONE);

	// Bounded stage-local `rgbGen wave`/`alphaGen wave`: the five Quake3
	// waveforms are accepted with four finite, in-range parameters
	// (base/amplitude/phase/frequency in script order), carried per stage for
	// the textured material pixel-shader modulation. Every other generator
	// and every malformed/truncated/non-finite/out-of-range wave keeps the
	// existing fallback: the stage stays accepted and modulates with
	// identity (func NONE) exactly as before.
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("textures/foo/bar", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	// All five rgb waveforms parse with their exact parameters; negative
	// amplitude and phase are legal GL1 values (the table lookup wraps both
	// signs and the final value is clamped to [0,1]). Split across two
	// scripts (the parser caps every shader at four stages).
	const std::string accepted = script(
		"{ map textures/base.tga rgbGen wave sin 0.5 0.5 0 1 }\n"
		"{ map textures/a.png blendFunc blend rgbGen wave square 1 -0.25 0.5 2 }\n"
		"{ map textures/b.png blendFunc blend rgbGen wave triangle 0 1 0.25 0.5 }\n"
		"{ map textures/c.png blendFunc blend rgbGen wave sawtooth 0 1 0 1 }");
	CHECK(VRHI_ParseShaderScriptStages(asBytes(accepted), accepted.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_SIN);
			CHECK(stages[0].rgbWaveBase == 0.5f);
			CHECK(stages[0].rgbWaveAmplitude == 0.5f);
			CHECK(stages[0].rgbWavePhase == 0.0f);
			CHECK(stages[0].rgbWaveFrequency == 1.0f);
			CHECK_EQ_INT(stages[0].alphaGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_INT(stages[1].rgbGenWaveFunc, VRHI_SHADER_WAVE_SQUARE);
			CHECK(stages[1].rgbWaveBase == 1.0f);
			CHECK(stages[1].rgbWaveAmplitude == -0.25f);
			CHECK(stages[1].rgbWavePhase == 0.5f);
			CHECK(stages[1].rgbWaveFrequency == 2.0f);
			CHECK_EQ_INT(stages[2].rgbGenWaveFunc, VRHI_SHADER_WAVE_TRIANGLE);
			CHECK_EQ_INT(stages[3].rgbGenWaveFunc, VRHI_SHADER_WAVE_SAWTOOTH);
			CHECK(stages[3].rgbWaveFrequency == 1.0f);
		}
	}
	const std::string acceptedMore = script(
		"{ map textures/base.tga rgbGen wave inversesawtooth 0 1 0 1 }\n"
		"{ map textures/a.png blendFunc blend alphaGen wave sin 0 1 0.25 2 }\n"
		"{ map textures/b.png blendFunc blend alphaGen wave triangle 0.5 0.5 0 1 }\n"
		"{ map textures/c.png blendFunc blend alphaGen wave sawtooth 1 -1 0 0.5 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(acceptedMore), acceptedMore.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_INVERSE_SAWTOOTH);
			CHECK_EQ_INT(stages[0].alphaGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_INT(stages[1].alphaGenWaveFunc, VRHI_SHADER_WAVE_SIN);
			CHECK(stages[1].alphaWaveBase == 0.0f);
			CHECK(stages[1].alphaWaveAmplitude == 1.0f);
			CHECK(stages[1].alphaWavePhase == 0.25f);
			CHECK(stages[1].alphaWaveFrequency == 2.0f);
			CHECK_EQ_INT(stages[1].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_INT(stages[2].alphaGenWaveFunc, VRHI_SHADER_WAVE_TRIANGLE);
			CHECK(stages[2].alphaWaveBase == 0.5f);
			CHECK(stages[2].alphaWaveAmplitude == 0.5f);
			CHECK_EQ_INT(stages[3].alphaGenWaveFunc, VRHI_SHADER_WAVE_SAWTOOTH);
			CHECK(stages[3].alphaWaveBase == 1.0f);
			CHECK(stages[3].alphaWaveAmplitude == -1.0f);
			CHECK(stages[3].alphaWaveFrequency == 0.5f);
		}
	}
	// Keyword and waveform matching are case-insensitive, like every other
	// directive.
	const std::string upper = script(
		"{ map textures/base.tga RGBGEN WAVE SIN 0 1 0 1 ALPHAGEN Wave Square 0.25 0.5 0 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upper), upper.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_SIN);
			CHECK_EQ_INT(stages[0].alphaGenWaveFunc, VRHI_SHADER_WAVE_SQUARE);
		}
	}
	// Stage locality: each stage keeps its own generators, and stages without
	// the keyword keep identity (func NONE), exactly as before the slice.
	const std::string locality = script(
		"{ map textures/base.tga rgbGen wave sin 0.5 0.5 0 1 }\n"
		"{ map textures/a.png blendFunc blend }\n"
		"{ map textures/b.png blendFunc blend alphaGen wave triangle 0 1 0 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(locality), locality.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 3u);
		if (stages.size() == 3) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_SIN);
			CHECK_EQ_INT(stages[0].alphaGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_INT(stages[1].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_INT(stages[1].alphaGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK(stages[1].rgbWaveBase == 0.0f);
			CHECK_EQ_INT(stages[2].alphaGenWaveFunc, VRHI_SHADER_WAVE_TRIANGLE);
			CHECK_EQ_INT(stages[2].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
		}
	}
	// The wave statement consumes exactly seven tokens, so a following
	// directive on the same stage still parses, and the wave composes with
	// every other bounded stage slice (blend, depthWrite, alphaFunc,
	// clampMap, const generators, tcMods) without changing any of them. The
	// pixel shader multiplies const * wave, so both generators stay set on
	// the same stage.
	const std::string composed = script(
		"{ clampmap textures/clamped.tga tcMod scale 2 3 rgbGen wave sin 0.5 0.5 0 1 rgbGen const 0.5 0.5 0.5 alphaGen wave square 0.25 0.5 0 1 blendFunc blend depthWrite alphaFunc GE128 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(composed), composed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_SIN);
			CHECK(stages[0].rgbWaveBase == 0.5f);
			CHECK(stages[0].rgbWaveAmplitude == 0.5f);
			CHECK(stages[0].rgbGenConst);
			CHECK_EQ_INT(stages[0].alphaGenWaveFunc, VRHI_SHADER_WAVE_SQUARE);
			CHECK(stages[0].alphaWaveAmplitude == 0.5f);
			CHECK(stages[0].clampMap);
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_ALPHA);
			CHECK(stages[0].depthWrite);
			CHECK_EQ_INT(stages[0].alphaFunc, VRHI_SHADER_ALPHAFUNC_GE128);
			CHECK(stages[0].tcModScale);
		}
	}
	// Statement order is independent: a wave declared before the map on the
	// same stage still parses and still consumes exactly its own tokens.
	const std::string waveFirst = script(
		"{ rgbGen wave sawtooth 0 1 0 1 map textures/base.tga }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(waveFirst), waveFirst.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_SAWTOOTH);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// Fallbacks: unknown waveform, out-of-range, non-numeric, trailing-junk,
	// and truncated waves keep the stage accepted with identity modulation
	// (func NONE) instead of rejecting the material.
	const std::string fallbacks = script(
		"{ map textures/base.tga rgbGen wave noise 0 1 0 1 }\n"
		"{ map textures/a.png blendFunc blend rgbGen wave sin 1e30 1 0 1 }\n"
		"{ map textures/b.png blendFunc blend alphaGen wave sin abc def ghi jkl }\n"
		"{ map textures/c.png blendFunc blend alphaGen wave sin 0 1 0 1x }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(fallbacks), fallbacks.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK_EQ_INT(stages[i].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
				CHECK_EQ_INT(stages[i].alphaGenWaveFunc, VRHI_SHADER_WAVE_NONE);
				CHECK(stages[i].rgbWaveBase == 0.0f);
				CHECK(stages[i].rgbWaveAmplitude == 0.0f);
				CHECK(stages[i].rgbWavePhase == 0.0f);
				CHECK(stages[i].rgbWaveFrequency == 0.0f);
				CHECK(stages[i].alphaWaveBase == 0.0f);
				CHECK(stages[i].alphaWaveAmplitude == 0.0f);
				CHECK(stages[i].alphaWavePhase == 0.0f);
				CHECK(stages[i].alphaWaveFrequency == 0.0f);
			}
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
			CHECK_EQ_STR(stages[3].path, "textures/c.png");
		}
	}
	// Out-of-range magnitudes on any single parameter reject only that
	// generator (identity fallback), never the stage.
	const std::string outOfRange = script(
		"{ map textures/base.tga rgbGen wave sin 0.5 17 0 1 }\n"
		"{ map textures/a.png blendFunc blend rgbGen wave sin 0.5 1 -17 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(outOfRange), outOfRange.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_INT(stages[1].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
		}
	}
	// Truncated `rgbGen wave` statements at the end of a stage keep the
	// fallback (identity) and never borrow tokens past the stage end.
	const std::string truncated = script("{ map textures/base.tga rgbGen wave sin 0.5 0.5 0 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncated), truncated.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	const std::string truncatedMarker = script("{ map textures/base.tga alphaGen wave }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(truncatedMarker), truncatedMarker.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK_EQ_INT(stages[0].alphaGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// A syntactically complete wave statement with an invalid parameter
	// consumes its whole declared argument count (seven tokens), so
	// keyword-looking argument tokens (for example `rgbGen wave sin 0.5 map
	// 0 1`) cannot be re-parsed as stage directives: the stage stays
	// accepted with identity modulation and the real map still resolves.
	const std::string keywordValues = script(
		"{ map textures/base.tga rgbGen wave sin 0.5 map 0 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(keywordValues), keywordValues.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			CHECK_EQ_STR(stages[0].path, "textures/base.tga");
		}
	}
	// Unsupported generators keep the existing dynamic fallback: the stage
	// stays accepted with identity modulation, exactly as before this slice.
	const std::string unsupported = script(
		"{ map textures/base.tga rgbGen lightingSpecular }\n"
		"{ map textures/a.png blendFunc blend alphaGen lightingSpecular }\n"
		"{ map textures/b.png blendFunc blend rgbGen vertex }\n"
		"{ map textures/c.png blendFunc blend alphaGen entity }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(unsupported), unsupported.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 4u);
		if (stages.size() == 4) {
			for (std::size_t i = 0; i < stages.size(); ++i) {
				CHECK_EQ_INT(stages[i].rgbGenWaveFunc, VRHI_SHADER_WAVE_NONE);
				CHECK_EQ_INT(stages[i].alphaGenWaveFunc, VRHI_SHADER_WAVE_NONE);
			}
		}
	}
	// A dynamic generator followed by a const one on the same stage keeps
	// both bounded slices intact (wave is preserved and the const is
	// preserved; the pixel shader multiplies const * wave).
	const std::string mixed = script(
		"{ map textures/base.tga rgbGen wave sin 0.5 0.5 0 1 rgbGen const 0.5 0.5 0.5 }\n"
		"{ map textures/a.png blendFunc blend alphaGen const 0.5 alphaGen wave sin 0 1 0 1 }");
	results.clear();
	CHECK(VRHI_ParseShaderScriptStages(asBytes(mixed), mixed.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	if (results.size() == 1) {
		const std::vector<VRHI_ShaderStage> &stages = results["textures/foo/bar"];
		CHECK_EQ_INT(stages.size(), 2u);
		if (stages.size() == 2) {
			CHECK_EQ_INT(stages[0].rgbGenWaveFunc, VRHI_SHADER_WAVE_SIN);
			CHECK(stages[0].rgbGenConst);
			CHECK_EQ_INT(stages[1].alphaGenWaveFunc, VRHI_SHADER_WAVE_SIN);
			CHECK(stages[1].alphaGenConst);
		}
	}
	// The conservative first-stage opaque helper still rejects wave
	// generators: only identity matches the fixed opaque diffuse pass, so a
	// wave shader keeps its existing fallback there.
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga rgbGen wave sin 1 0 0 1 }", nullptr), "");
	CHECK_EQ_STR(candidateBody(
		"{ map textures/foo/bar.tga alphaGen wave square 0 1 0 1 }", nullptr), "");
}

void testConsoleScriptResolution() {
	// OpenArena defines the in-game console background as a script shader
	// (scripts/oanew.shader): `console { { map textures/sfx/logo256.tga } }`.
	// The VRHI UI texture path resolves such shader names through the bounded
	// script-stage table ONLY when the name is on the adapter's closed,
	// case-insensitive UI allowlist (vrhi_stub.cpp VRHI_IsUIScriptShader,
	// which contains exactly 'console'); normal world/model registration can
	// never add names to that list. This test pins the parser side of the
	// contract: the exact table entry the adapter consumes (one opaque stage
	// whose path is the logo image) and the case-insensitive name matching
	// the allowlist relies on. If the parser ever stops accepting this block,
	// the console background silently falls back to a solid box again.
	const std::string text =
		"console\n{\n{\nmap textures/sfx/logo256.tga\n}\n}\n";
	std::unordered_map<std::string, bool> wanted;
	wanted.emplace("console", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > results;
	CHECK(VRHI_ParseShaderScriptStages(asBytes(text), text.size(), wanted, &results));
	CHECK_EQ_INT(results.size(), 1u);
	const std::unordered_map<std::string, std::vector<VRHI_ShaderStage> >::const_iterator
		found = results.find("console");
	CHECK(found != results.end());
	if (found != results.end()) {
		const std::vector<VRHI_ShaderStage> &stages = found->second;
		CHECK_EQ_INT(stages.size(), 1u);
		if (stages.size() == 1) {
			CHECK_EQ_STR(stages[0].path, "textures/sfx/logo256.tga");
			CHECK_EQ_INT(stages[0].blendMode, VRHI_SHADER_BLEND_OPAQUE);
			CHECK(VRHI_ShaderPathIsSafe(stages[0].path));
			// The stage path must be a real image qpath, never a special
			// texture placeholder like $whiteimage/$lightmap.
			CHECK(!stages[0].path.empty() && stages[0].path[0] != '$');
		}
	}
	// The allowlist is case-insensitive: a script block written in any case
	// still resolves under the lowercased 'console' key the adapter uses for
	// its exact-match lookup.
	const std::string upperText =
		"CONSOLE\n{\n{\nmap textures/sfx/logo256.tga\n}\n}\n";
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > upperResults;
	CHECK(VRHI_ParseShaderScriptStages(asBytes(upperText), upperText.size(),
		wanted, &upperResults));
	CHECK_EQ_INT(upperResults.size(), 1u);
	CHECK(upperResults.find("console") != upperResults.end());
	// A shader of the same name that stays unresolved must not appear.
	std::unordered_map<std::string, bool> wantedMissing;
	wantedMissing.emplace("console2", true);
	std::unordered_map<std::string, std::vector<VRHI_ShaderStage> > missing;
	CHECK(VRHI_ParseShaderScriptStages(asBytes(text), text.size(), wantedMissing, &missing));
	CHECK_EQ_INT(missing.size(), 0u);
}

} // namespace

int main() {
	testTokenizer();
	testOpaqueAcceptance();
	testBlendFuncRejection();
	testRgbGenAlphaGen();
	testDepthFunc();
	testStillUnsupported();
	testMultiMapRejection();
	testMapPathValidation();
	testMalformedStructure();
	testWantedFilteringAndNames();
	testBlockLevelBoundaries();
	testSimpleMultiStageExtraction();
	testScriptStagesOverrideDirectImages();
	testShaderFilePriority();
	testConstGenerators();
	testStageDepthWrite();
	testStageAlphaFunc();
	testStageClampMap();
	testStageTcModScale();
	testStageTcModScroll();
	testStageTcModRotate();
	testStageTcModTurbStretch();
	testStageAnimMap();
	testStageTcGenEnvironment();
	testDeformWaveMath();
	testStageDeform();
	testStageWaveGenerators();
	testConsoleScriptResolution();
	testPathSafetyHelpers();

	if (g_failures != 0) {
		std::printf("vrhi_shader_script_test: %d of %d checks FAILED\n",
			g_failures, g_checks);
		return 1;
	}
	std::printf("vrhi_shader_script_test: all %d checks passed\n", g_checks);
	return 0;
}
