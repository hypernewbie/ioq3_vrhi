// Standalone, dependency-free unit test for the VRHI bounded shader-script
// parser (code/renderervrhi/vrhi_shader_script.h). No engine headers, no
// filesystem, no third-party libraries: the test feeds shader script text in
// memory and asserts tokenization, comment handling, caps, the safe opaque
// allowlist (blendFunc GL_ONE GL_ZERO, rgbGen/alphaGen identity, depthWrite,
// depthFunc lequal/less), malformed statements, and multi-map shaders, plus
// the bounded multi-stage slice (VRHI_ParseShaderScriptStages: at most four
// direct-image/static-first-frame stages, common transparent blend modes,
// ignored-but-bounded UV/time modifiers, and the bounded static const
// generators rgbGen const / alphaGen const).
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
	testConstGenerators();
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
