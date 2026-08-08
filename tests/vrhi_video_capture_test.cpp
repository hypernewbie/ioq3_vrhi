// Dependency-free test for the bounded raw AVI pixel conversion helper.
// clang++ -std=c++17 -O1 -Wall -Wextra -Werror tests/vrhi_video_capture_test.cpp -o vrhi_video_capture_test && ./vrhi_video_capture_test
#include "../code/renderervrhi/vrhi_video_capture.h"
#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
	const int width = 2, height = 2;
	const std::uint8_t rgba[] = {
		1, 2, 3, 255,  4, 5, 6, 255,
		7, 8, 9, 255, 10,11,12, 255
	};
	std::vector<std::uint8_t> out(16, 0xff);
	if (!vrhi_video::ConvertTopFirstRGBA8ToBottomUpBGR24(
		rgba, 8, width, height, false, out.data(), 8, out.size())) return 1;
	const std::uint8_t expected[] = {
		9,8,7, 12,11,10, 0,0,
		3,2,1, 6,5,4, 0,0
	};
	for (int i = 0; i < 16; ++i) if (out[static_cast<std::size_t>(i)] != expected[i]) return 2;
	const std::uint8_t bgra[] = { 3,2,1,255 };
	std::uint8_t one[4] = {};
	if (!vrhi_video::ConvertTopFirstRGBA8ToBottomUpBGR24(
		bgra, 4, 1, 1, true, one, 4, sizeof(one)) ||
		one[0] != 3 || one[1] != 2 || one[2] != 1 || one[3] != 0) return 3;
	std::printf("vrhi_video_capture_test: passed\n");
	return 0;
}
