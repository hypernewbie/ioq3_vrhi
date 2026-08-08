// Standalone, dependency-free unit test for the VRHI bounded BSP area-mask
// helper (code/renderervrhi/vrhi_areamask.h). No engine headers, no
// filesystem, no third-party libraries: the test builds masks in memory and
// asserts the "set bit closes the area" bit semantics plus the safe/unmasked
// handling of null masks, empty masks, and invalid area values.
//
// Build and run (any C++11-or-later compiler):
//   clang++ -std=c++17 -O1 -Wall -Wextra -Werror tests/vrhi_areamask_test.cpp -o vrhi_areamask_test && ./vrhi_areamask_test
//   clang-cl -TP -std:c++17 -W4 tests\vrhi_areamask_test.cpp /Fe:vrhi_areamask_test.exe && vrhi_areamask_test.exe
//
// Exit code 0 means every case passed.

#include "../code/renderervrhi/vrhi_areamask.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

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

void testNullAndEmptyMasks() {
	// A null mask never masks anything.
	CHECK(!VRHI_AreaIsMasked(nullptr, 32, 0));
	CHECK(!VRHI_AreaIsMasked(nullptr, 32, 255));
	// A zero/non-positive byte count never masks anything.
	unsigned char mask[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	CHECK(!VRHI_AreaIsMasked(mask, 0, 0));
	CHECK(!VRHI_AreaIsMasked(mask, -1, 0));
}

void testInvalidAreasAreUnmasked() {
	unsigned char mask[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
	// Negative areas are never masked.
	CHECK(!VRHI_AreaIsMasked(mask, 4, -1));
	CHECK(!VRHI_AreaIsMasked(mask, 4, -1000));
	// Areas beyond the mask's bit capacity (bytes * 8) are never masked.
	CHECK(!VRHI_AreaIsMasked(mask, 4, 32));
	CHECK(!VRHI_AreaIsMasked(mask, 4, 33));
	CHECK(!VRHI_AreaIsMasked(mask, 4, 255));
	CHECK(!VRHI_AreaIsMasked(mask, 4, 1000000));
	// The last valid bit position of a 4-byte mask is area 31.
	CHECK(VRHI_AreaIsMasked(mask, 4, 31));
}

void testBitSemantics() {
	// A cleared bit never masks; a set bit masks the exact same position.
	unsigned char mask[32];
	std::memset(mask, 0, sizeof(mask));
	for (int area = 0; area < 32 * 8; ++area) {
		CHECK(!VRHI_AreaIsMasked(mask, 32, area));
	}
	mask[0] = 0x01; // area 0 closed
	mask[1] = 0x80; // area 15 closed
	mask[4] = 0x10; // area 36 closed
	mask[31] = 0x01; // area 248 closed
	mask[31] |= 0x80; // area 255 closed
	CHECK(VRHI_AreaIsMasked(mask, 32, 0));
	CHECK(VRHI_AreaIsMasked(mask, 32, 15));
	CHECK(VRHI_AreaIsMasked(mask, 32, 36));
	CHECK(VRHI_AreaIsMasked(mask, 32, 248));
	CHECK(VRHI_AreaIsMasked(mask, 32, 255));
	// Adjacent bits stay open.
	CHECK(!VRHI_AreaIsMasked(mask, 32, 1));
	CHECK(!VRHI_AreaIsMasked(mask, 32, 14));
	CHECK(!VRHI_AreaIsMasked(mask, 32, 16));
	CHECK(!VRHI_AreaIsMasked(mask, 32, 35));
	CHECK(!VRHI_AreaIsMasked(mask, 32, 37));
	CHECK(!VRHI_AreaIsMasked(mask, 32, 247));
	CHECK(!VRHI_AreaIsMasked(mask, 32, 254));
	// A full mask closes every valid area.
	unsigned char full[2] = { 0xFF, 0xFF };
	for (int area = 0; area < 16; ++area) {
		CHECK(VRHI_AreaIsMasked(full, 2, area));
	}
}

} // namespace

int main() {
	testNullAndEmptyMasks();
	testInvalidAreasAreUnmasked();
	testBitSemantics();
	if (g_failures == 0) {
		std::printf("vrhi_areamask_test: all %d checks passed\n", g_checks);
		return 0;
	}
	std::printf("vrhi_areamask_test: %d/%d checks FAILED\n", g_failures, g_checks);
	return 1;
}
