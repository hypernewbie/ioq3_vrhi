/*
===========================================================================
VRHI bounded BSP area-mask helper.

Dependency-free (C++11) helper shared by renderer_vrhi and its standalone
unit test. The client refdef carries a fixed-size byte area mask
(`refdef_t.areamask[MAX_MAP_AREA_BYTES]`); a SET bit closes that portal
area, so leaves whose area bit is set must be skipped during PVS leaf
traversal ("1 bits will prevent the associated area from rendering at
all"). Bits outside the supplied mask, negative area values, and null
masks are never masked, so malformed or hostile leaf data stays safe and
visible.
===========================================================================
*/
#ifndef __VRHI_AREAMASK_H
#define __VRHI_AREAMASK_H

#include <cstdint>

// True when `area` is a valid bit position inside the supplied mask AND
// that bit is set (the area is closed and must not render). Null masks,
// non-positive mask sizes, negative area values, and area values beyond
// the mask's bit capacity are never masked.
inline bool VRHI_AreaIsMasked(const unsigned char *areamask, int areamaskBytes,
	int32_t area) {
	if (areamask == nullptr || areamaskBytes <= 0 || area < 0 ||
		(area >> 3) >= areamaskBytes) {
		return false;
	}
	return (areamask[area >> 3] &
		static_cast<unsigned char>(1 << (area & 7))) != 0;
}

#endif // __VRHI_AREAMASK_H
