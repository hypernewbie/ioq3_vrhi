#ifndef __VRHI_DRAW_ORDER_H
#define __VRHI_DRAW_ORDER_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// One bounded draw from either the BSP world or the transient scene. The
// camera-distance key is an approximation of Quake's shader/surface sort, not
// an implementation of every GL1 translucent material sort category.
struct VRHI_TranslucentDrawOrder {
	float distanceSquared;
	std::size_t index;
	std::size_t stage;
	bool scene;
};

inline void VRHI_SortTranslucentDraws(std::vector<VRHI_TranslucentDrawOrder> *draws) {
	if (draws == nullptr) return;
	std::stable_sort(draws->begin(), draws->end(),
		[](const VRHI_TranslucentDrawOrder &a, const VRHI_TranslucentDrawOrder &b) {
			// Invalid/overflowed distances cannot violate strict weak ordering.
			const float left = std::isfinite(a.distanceSquared) && a.distanceSquared >= 0.0f
				? a.distanceSquared : 0.0f;
			const float right = std::isfinite(b.distanceSquared) && b.distanceSquared >= 0.0f
				? b.distanceSquared : 0.0f;
			return left > right;
		});
}

#endif // __VRHI_DRAW_ORDER_H
