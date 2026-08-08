// Standalone, dependency-free unit test for the VRHI bounded .skin text
// parser (code/renderervrhi/vrhi_skin.h). No engine headers, no filesystem,
// no third-party libraries: the test feeds skin text in memory and asserts
// tokenization, comment/whitespace/tag handling, caps, and lowercasing.
//
// Build and run (any C++11-or-later compiler):
//   clang++ -std=c++17 -O1 -Wall -Wextra -Werror tests/vrhi_skin_test.cpp -o vrhi_skin_test && ./vrhi_skin_test
//   clang-cl -TP -std:c++17 -W4 tests\vrhi_skin_test.cpp /Fe:vrhi_skin_test.exe && vrhi_skin_test.exe
//
// Exit code 0 means every case passed.

#include "../code/renderervrhi/vrhi_skin.h"

#include <cstdio>
#include <string>
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

std::vector<VRHI_SkinEntry> parse(const std::string &text, bool *ok = nullptr) {
	std::vector<VRHI_SkinEntry> entries;
	const bool result = VRHI_ParseSkinText(text.data(), text.size(), entries);
	if (ok != nullptr) *ok = result;
	return entries;
}

bool entryIs(const VRHI_SkinEntry &entry, const char *surface,
	const char *shader) {
	return entry.surface == surface && entry.shader == shader;
}

void testBasicPairs() {
	std::vector<VRHI_SkinEntry> entries = parse(
		"l_legs,models/players/assassin/lower.tga\n"
		"u_torso,models/players/assassin/upper.tga\n"
		"h_head,models/players/assassin/head.tga\n");
	CHECK_EQ_INT(entries.size(), 3u);
	CHECK(entryIs(entries[0], "l_legs", "models/players/assassin/lower.tga"));
	CHECK(entryIs(entries[1], "u_torso", "models/players/assassin/upper.tga"));
	CHECK(entryIs(entries[2], "h_head", "models/players/assassin/head.tga"));
}

void testSurfaceLowercasingShaderCaseKept() {
	std::vector<VRHI_SkinEntry> entries = parse("L_LEGS,MODELS/PLAYERS/X/UP.TGA\n");
	CHECK_EQ_INT(entries.size(), 1u);
	CHECK(entryIs(entries[0], "l_legs", "MODELS/PLAYERS/X/UP.TGA"));
}

void testCommentsAndWhitespace() {
	std::vector<VRHI_SkinEntry> entries = parse(
		"// full-line comment\r\n"
		"  \tl_legs , models/x/lower.tga   // trailing comment\r\n"
		"\r\n"
		"u_torso,models/x/upper.tga\r\n");
	CHECK_EQ_INT(entries.size(), 2u);
	CHECK(entryIs(entries[0], "l_legs", "models/x/lower.tga"));
	CHECK(entryIs(entries[1], "u_torso", "models/x/upper.tga"));
}

void testBareTagLinesSkipped() {
	// Real .skin files list tag surfaces as bare single-token lines.
	std::vector<VRHI_SkinEntry> entries = parse(
		"h_head,models/players/gargoyle/bared.tga\n"
		"h_headold,models/players/gargoyle/bared.tga\n"
		"\n"
		"tag_head\n");
	CHECK_EQ_INT(entries.size(), 2u);
	CHECK(entryIs(entries[0], "h_head", "models/players/gargoyle/bared.tga"));
	CHECK(entryIs(entries[1], "h_headold", "models/players/gargoyle/bared.tga"));
}

void testTagInMiddleDoesNotConsumeNextEntry() {
	std::vector<VRHI_SkinEntry> entries = parse(
		"h_head,models/x/head.tga\n"
		"tag_head\n"
		"l_legs,models/x/lower.tga\n");
	CHECK_EQ_INT(entries.size(), 2u);
	CHECK(entryIs(entries[0], "h_head", "models/x/head.tga"));
	CHECK(entryIs(entries[1], "l_legs", "models/x/lower.tga"));
}

void testTagSubstringSkipped() {
	std::vector<VRHI_SkinEntry> entries = parse("tag_torso\nsome_surface,models/x/a.tga\n");
	CHECK_EQ_INT(entries.size(), 1u);
	CHECK(entryIs(entries[0], "some_surface", "models/x/a.tga"));
}

void testWhitespaceSeparatedPairs() {
	std::vector<VRHI_SkinEntry> entries = parse("l_legs models/x/lower.tga\n");
	CHECK_EQ_INT(entries.size(), 1u);
	CHECK(entryIs(entries[0], "l_legs", "models/x/lower.tga"));
}

void testExtraCommasTolerated() {
	std::vector<VRHI_SkinEntry> entries = parse("l_legs,,models/x/lower.tga,,u_torso,models/x/upper.tga\n");
	CHECK_EQ_INT(entries.size(), 2u);
	CHECK(entryIs(entries[0], "l_legs", "models/x/lower.tga"));
	CHECK(entryIs(entries[1], "u_torso", "models/x/upper.tga"));
}

void testIncompletePairDropped() {
	std::vector<VRHI_SkinEntry> entries = parse("l_legs,models/x/lower.tga\nlonely_surface\n");
	CHECK_EQ_INT(entries.size(), 1u);
	CHECK(entryIs(entries[0], "l_legs", "models/x/lower.tga"));
}

void testOverlongTokensSkipped() {
	std::string surface(VRHI_SKIN_MAX_QPATH, 'a');       // exactly 64 chars: too long
	std::string shader(VRHI_SKIN_MAX_QPATH - 1, 'b');    // 63 chars: valid
	std::string tooLongShader(VRHI_SKIN_MAX_QPATH, 'c'); // 64 chars: too long
	const std::string text = "ok,models/x/ok.tga\n" + surface + ",models/x/bad.tga\n" +
		"ok2," + tooLongShader + "\ngood," + shader + "\n";
	std::vector<VRHI_SkinEntry> entries = parse(text);
	CHECK_EQ_INT(entries.size(), 2u);
	CHECK(entryIs(entries[0], "ok", "models/x/ok.tga"));
	CHECK(entryIs(entries[1], "good", shader.c_str()));
}

void testEntryCap() {
	std::string text;
	for (std::size_t i = 0; i < VRHI_SKIN_MAX_SURFACES + 8; ++i) {
		text += "surface" + std::to_string(i) + ",models/x/s" +
			std::to_string(i) + ".tga\n";
	}
	std::vector<VRHI_SkinEntry> entries = parse(text);
	CHECK_EQ_INT(entries.size(), VRHI_SKIN_MAX_SURFACES);
}

void testNullAndOversizeInput() {
	std::vector<VRHI_SkinEntry> nullEntries;
	CHECK(!VRHI_ParseSkinText(nullptr, 0, nullEntries));
	std::vector<VRHI_SkinEntry> entries;
	std::string huge(VRHI_SKIN_MAX_TEXT_BYTES + 1, 'x');
	CHECK(!VRHI_ParseSkinText(huge.data(), huge.size(), entries));
	CHECK_EQ_INT(entries.size(), 0u);
}

void testEmptyAndAllTagInput() {
	bool ok = false;
	CHECK(parse("", &ok).empty() && ok);
	CHECK(parse("// nothing here\n\n", &ok).empty() && ok);
	CHECK(parse("tag_head\ntag_torso\n", &ok).empty() && ok);
}

void testEmbeddedNulStopsSafely() {
	std::string text = "l_legs,models/x/lower.tga\n";
	text.push_back('\0');
	text += "u_torso,models/x/upper.tga\n";
	std::vector<VRHI_SkinEntry> entries = parse(text);
	CHECK_EQ_INT(entries.size(), 1u);
	CHECK(entryIs(entries[0], "l_legs", "models/x/lower.tga"));
}

void testRealOpenArenaSkinFile() {
	// models/players/assassin/lower_blue.skin verbatim (LF line endings).
	std::vector<VRHI_SkinEntry> entries = parse(
		"l_legs,models/players/assassin/lower-blue.tga\n"
		"tag_torso\n");
	CHECK_EQ_INT(entries.size(), 1u);
	CHECK(entryIs(entries[0], "l_legs", "models/players/assassin/lower-blue.tga"));
	// models/players/gargoyle/head_default.skin verbatim (blank line between
	// entries and the trailing tag line).
	entries = parse(
		"h_head,models/players/gargoyle/bared.tga\n"
		"h_headold,models/players/gargoyle/bared.tga\n"
		"\n"
		"tag_head\n");
	CHECK_EQ_INT(entries.size(), 2u);
	CHECK(entryIs(entries[0], "h_head", "models/players/gargoyle/bared.tga"));
	CHECK(entryIs(entries[1], "h_headold", "models/players/gargoyle/bared.tga"));
}

} // namespace

int main() {
	testBasicPairs();
	testSurfaceLowercasingShaderCaseKept();
	testCommentsAndWhitespace();
	testBareTagLinesSkipped();
	testTagInMiddleDoesNotConsumeNextEntry();
	testTagSubstringSkipped();
	testWhitespaceSeparatedPairs();
	testExtraCommasTolerated();
	testIncompletePairDropped();
	testOverlongTokensSkipped();
	testEntryCap();
	testNullAndOversizeInput();
	testEmptyAndAllTagInput();
	testEmbeddedNulStopsSafely();
	testRealOpenArenaSkinFile();

	if (g_failures == 0) {
		std::printf("vrhi_skin_test: all %d checks passed\n", g_checks);
		return 0;
	}
	std::printf("vrhi_skin_test: %d/%d checks FAILED\n", g_failures, g_checks);
	return 1;
}
