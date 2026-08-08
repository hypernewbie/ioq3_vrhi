/*
===========================================================================
renderer_vrhi: bounded raw AVI pixel conversion helper

The renderer readback is top-first RGBA/BGRA. AVI BI_RGB stores BGR rows
bottom-first, with each row padded to a four-byte boundary. This dependency-
free helper performs that conversion after all source and destination bounds
have been checked by the caller.
===========================================================================
*/
#ifndef VRHI_VIDEO_CAPTURE_H
#define VRHI_VIDEO_CAPTURE_H

#include <cstddef>
#include <cstdint>
#include <limits>

namespace vrhi_video {

inline bool ConvertTopFirstRGBA8ToBottomUpBGR24(
	const std::uint8_t *source, std::size_t sourcePitch, int width, int height,
	bool sourceBGRA, std::uint8_t *destination, std::size_t destinationPitch,
	std::size_t destinationBytes) {
	if (source == nullptr || destination == nullptr || width <= 0 || height <= 0) {
		return false;
	}
	const std::size_t w = static_cast<std::size_t>(width);
	const std::size_t h = static_cast<std::size_t>(height);
	if (w > (std::numeric_limits<std::size_t>::max)() / 4 ||
		w > (std::numeric_limits<std::size_t>::max)() / 3) {
		return false;
	}
	const std::size_t sourceRowBytes = w * 4;
	const std::size_t destinationRowBytes = w * 3;
	if (sourcePitch < sourceRowBytes || destinationPitch < destinationRowBytes ||
		h > (std::numeric_limits<std::size_t>::max)() / sourcePitch ||
		h > (std::numeric_limits<std::size_t>::max)() / destinationPitch ||
		destinationBytes < destinationPitch * h) {
		return false;
	}

	for (std::size_t y = 0; y < h; ++y) {
		const std::uint8_t *src = source + y * sourcePitch;
		std::uint8_t *dst = destination + (h - 1 - y) * destinationPitch;
		for (std::size_t x = 0; x < w; ++x) {
			const std::uint8_t *pixel = src + x * 4;
			if (sourceBGRA) {
				dst[x * 3 + 0] = pixel[0];
				dst[x * 3 + 1] = pixel[1];
				dst[x * 3 + 2] = pixel[2];
			} else {
				dst[x * 3 + 0] = pixel[2];
				dst[x * 3 + 1] = pixel[1];
				dst[x * 3 + 2] = pixel[0];
			}
		}
		for (std::size_t padding = destinationRowBytes;
			padding < destinationPitch; ++padding) {
			dst[padding] = 0;
		}
	}
	return true;
}

} // namespace vrhi_video

#endif // VRHI_VIDEO_CAPTURE_H
