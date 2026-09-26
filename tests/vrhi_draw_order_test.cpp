// Engine-free test for the shared BSP/scene translucent draw ordering key.
#include "../code/renderervrhi/vrhi_draw_order.h"

#include <cstdio>
#include <limits>
#include <vector>

int main() {
	// Opaque draws are submitted before this queue. A farther scene effect
	// must render before nearer world glass, and vice versa.
	std::vector<VRHI_TranslucentDrawOrder> draws = {
		{ 4.0f, 0, 0, false }, { 36.0f, 1, 1, false },
		{ 16.0f, 2, 0, true }, { 49.0f, 3, 0, true },
		{ 16.0f, 4, 1, false }
	};
	VRHI_SortTranslucentDraws(&draws);
	if (draws.size() != 5 || !draws[0].scene || draws[0].index != 3 ||
		draws[1].scene || draws[1].index != 1 || draws[1].stage != 1 ||
		!draws[2].scene || draws[2].index != 2 ||
		draws[3].scene || draws[3].index != 4 || draws[3].stage != 1 ||
		draws[4].scene || draws[4].index != 0) return 1;
	// Invalid keys remain deterministic instead of violating the comparator.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	std::vector<VRHI_TranslucentDrawOrder> invalid = {
		{ nan, 0, 0, false }, { 1.0f, 1, 0, true }, { inf, 2, 0, false }
	};
	VRHI_SortTranslucentDraws(&invalid);
	if (invalid[0].index != 1 || invalid[1].index != 0 || invalid[2].index != 2) return 1;
	VRHI_SortTranslucentDraws(nullptr);
	std::puts("vrhi_draw_order_test: all checks passed");
	return 0;
}
