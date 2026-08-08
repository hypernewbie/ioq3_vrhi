// Standalone, dependency-free unit test for the VRHI bounded shader-script
// first-stage parser (code/renderervrhi/vrhi_shader_script.h). No engine
// headers, no filesystem, no third-party libraries: the test feeds shader
// script text in memory and asserts tokenization, comment handling, caps,
// the safe opaque allowlist (blendFunc GL_ONE GL_ZERO, rgbGen/alphaGen
// identity, depthWrite, depthFunc lequal/less), and the continued rejection
// of alpha/additive blends, tcGen/tcMod variants, deform/fog/animMap/
// videoMap/normal/specular/portal/sky, malformed statements, and multi-map
// shaders.
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
	testPathSafetyHelpers();

	if (g_failures != 0) {
		std::printf("vrhi_shader_script_test: %d of %d checks FAILED\n",
			g_failures, g_checks);
		return 1;
	}
	std::printf("vrhi_shader_script_test: all %d checks passed\n", g_checks);
	return 0;
}
