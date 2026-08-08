/*
===========================================================================
renderer_vrhi: bounded true-color TGA decoder (diffuse slice)

Dependency-free bounded decoder for the direct-TGA diffuse subset of the
VRHI BSP world renderer. It supports exactly the image types the GL
renderers' LoadTGA accepts for 24/32-bit pixels:

  - type 2: uncompressed true-color
  - type 10: run-length encoded true-color (RLE packets + raw packets)

It intentionally does not implement grayscale (type 3), colormapped (type 1),
or any other format; those remain unsupported and fail decode, letting the
caller fall back to its existing lightmap/solid path. This header has no
dependencies outside the C++ standard library so the same code is exercised
both by the renderer and by the standalone test in tests/vrhi_tga_decode_test.cpp.

Safety contract (all enforced before any out-of-bounds access):

  - header must be at least 18 bytes and the id field must fit in the file
  - image type is exactly 2 or 10, colormap type is 0, pixel size is 24 or 32
  - width/height are nonzero and within the caller-supplied dimension cap
  - decoded RGBA size is within the caller-supplied byte cap (checked with
    division so hostile dimensions cannot overflow size_t arithmetic)
  - type 2 additionally requires the whole raw pixel block to fit in the file
  - type 10 is validated packet by packet: the packet header byte, the RLE
    payload, and the raw payload are each bounds-checked against the file
    end, and every packet's run length is checked against the remaining
    pixel count before any pixel is written

Output convention: rgba is width*height*4 bytes, row 0 is the top row of
the image. The 0x20 attribute bit (top-down storage) is honored; bottom-up
files are flipped so callers never need to special-case orientation.
===========================================================================
*/

#ifndef VRHI_TGA_DECODE_H
#define VRHI_TGA_DECODE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vrhi_tga {

struct DecodeResult {
	bool ok = false;
	int width = 0;
	int height = 0;
	// width*height*4 RGBA pixels; row 0 is the top row of the image.
	std::vector<std::uint8_t> rgba;
};

// TGA header field offsets (the on-disk layout is little-endian).
enum {
	TGA_OFFSET_ID_LENGTH = 0,
	TGA_OFFSET_COLORMAP_TYPE = 1,
	TGA_OFFSET_IMAGE_TYPE = 2,
	TGA_OFFSET_WIDTH = 12,
	TGA_OFFSET_HEIGHT = 14,
	TGA_OFFSET_PIXEL_SIZE = 16,
	TGA_OFFSET_ATTRIBUTES = 17,
	TGA_HEADER_BYTES = 18,
	TGA_IMAGE_TYPE_UNCOMPRESSED = 2,
	TGA_IMAGE_TYPE_RLE = 10,
	TGA_ATTRIB_TOP_DOWN = 0x20
};

// Reads a little-endian uint16 from the header without relying on host
// byte order or unaligned access.
inline std::uint16_t VRHI_TGAReadU16(const std::uint8_t *data) {
	return static_cast<std::uint16_t>(data[0]) |
		static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

// Bounded decode of a true-color TGA (types 2 and 10, 24/32-bit pixels).
//
// data/size describe the whole file. maxDimension bounds width and height;
// maxBytes bounds the decoded RGBA allocation. On success the result is
// written to *out and true is returned. On any malformed, oversized, or
// unsupported input *out is left reset and false is returned; the caller
// keeps its existing fallback behavior. Allocation failure is reported
// through the standard library exceptions the caller already handles.
inline bool Decode(const std::uint8_t *data, std::size_t size,
	int maxDimension, std::size_t maxBytes, DecodeResult *out) {
	if (out != nullptr) {
		out->ok = false;
		out->width = 0;
		out->height = 0;
		out->rgba.clear();
	}
	if (data == nullptr || size < TGA_HEADER_BYTES || out == nullptr) {
		return false;
	}

	const std::uint8_t idLength = data[TGA_OFFSET_ID_LENGTH];
	const std::uint8_t colormapType = data[TGA_OFFSET_COLORMAP_TYPE];
	const std::uint8_t imageType = data[TGA_OFFSET_IMAGE_TYPE];
	const int width = static_cast<int>(VRHI_TGAReadU16(data + TGA_OFFSET_WIDTH));
	const int height = static_cast<int>(VRHI_TGAReadU16(data + TGA_OFFSET_HEIGHT));
	const std::uint8_t pixelSize = data[TGA_OFFSET_PIXEL_SIZE];
	const std::uint8_t attributes = data[TGA_OFFSET_ATTRIBUTES];

	if (imageType != TGA_IMAGE_TYPE_UNCOMPRESSED &&
		imageType != TGA_IMAGE_TYPE_RLE) {
		return false; // grayscale, colormapped, and other types unsupported
	}
	if (colormapType != 0) {
		return false; // colormaps unsupported
	}
	if (pixelSize != 24 && pixelSize != 32) {
		return false;
	}

	const std::size_t widthU = static_cast<std::size_t>(width);
	const std::size_t heightU = static_cast<std::size_t>(height);
	if (widthU == 0 || heightU == 0 ||
		widthU > static_cast<std::size_t>(maxDimension) ||
		heightU > static_cast<std::size_t>(maxDimension)) {
		return false;
	}
	// Division-based cap so hostile dimensions cannot overflow the multiply.
	if (widthU > maxBytes / 4u / heightU) {
		return false;
	}
	const std::size_t pixelBytes = widthU * heightU * 4u;
	if (pixelBytes > maxBytes) {
		return false;
	}
	// The id field (image comment) must fit in the file.
	if (static_cast<std::size_t>(idLength) > size - TGA_HEADER_BYTES) {
		return false;
	}
	const std::size_t pixelOffset = TGA_HEADER_BYTES + static_cast<std::size_t>(idLength);
	const std::size_t sourcePixelBytes = pixelSize / 8u;
	const std::size_t totalPixels = pixelBytes / 4u;

	std::vector<std::uint8_t> rgba(pixelBytes);
	const std::uint8_t *src = data + pixelOffset;
	const std::uint8_t *end = data + size;

	if (imageType == TGA_IMAGE_TYPE_UNCOMPRESSED) {
		// Whole-file bound for the uncompressed path; every pixel is read
		// exactly once below.
		const std::size_t rawBytes = totalPixels * sourcePixelBytes;
		if (rawBytes > static_cast<std::size_t>(end - src)) {
			return false; // file truncated
		}
		for (std::size_t i = 0; i < totalPixels; ++i) {
			rgba[i * 4u + 0] = src[i * sourcePixelBytes + 2];
			rgba[i * 4u + 1] = src[i * sourcePixelBytes + 1];
			rgba[i * 4u + 2] = src[i * sourcePixelBytes + 0];
			rgba[i * 4u + 3] = sourcePixelBytes == 4u ? src[i * sourcePixelBytes + 3] : 255u;
		}
	} else {
		// RLE: the packet stream is a continuous run of pixels in file
		// order (row 0 of the file first, rows implicit). Each packet is a
		// header byte followed by either one repeated pixel (0x80 set) or
		// packetSize literal pixels. Every packet header, RLE payload, raw
		// payload, and run length is checked before writing.
		std::size_t written = 0;
		while (written < totalPixels) {
			if (src >= end) {
				return false; // truncated packet header
			}
			const std::uint8_t packetHeader = *src++;
			const std::size_t packetSize = 1u + static_cast<std::size_t>(packetHeader & 0x7fu);
			if (packetSize > totalPixels - written) {
				return false; // run overruns the declared pixel count
			}
			if (packetHeader & 0x80u) {
				// Run-length packet: one pixel value repeated packetSize times.
				if (static_cast<std::size_t>(end - src) < sourcePixelBytes) {
					return false; // truncated RLE payload
				}
				const std::uint8_t blue = src[0];
				const std::uint8_t green = src[1];
				const std::uint8_t red = src[2];
				const std::uint8_t alpha = sourcePixelBytes == 4u ? src[3] : 255u;
				src += sourcePixelBytes;
				for (std::size_t j = 0; j < packetSize; ++j) {
					const std::size_t pixel = (written + j) * 4u;
					rgba[pixel + 0] = red;
					rgba[pixel + 1] = green;
					rgba[pixel + 2] = blue;
					rgba[pixel + 3] = alpha;
				}
			} else {
				// Raw packet: packetSize literal pixels.
				if (static_cast<std::size_t>(end - src) < packetSize * sourcePixelBytes) {
					return false; // truncated raw payload
				}
				for (std::size_t j = 0; j < packetSize; ++j) {
					const std::size_t pixel = (written + j) * 4u;
					rgba[pixel + 0] = src[j * sourcePixelBytes + 2];
					rgba[pixel + 1] = src[j * sourcePixelBytes + 1];
					rgba[pixel + 2] = src[j * sourcePixelBytes + 0];
					rgba[pixel + 3] = sourcePixelBytes == 4u ? src[j * sourcePixelBytes + 3] : 255u;
				}
				src += packetSize * sourcePixelBytes;
			}
			written += packetSize;
		}
	}

	// Row 0 of the output is always the top row. Files with the 0x20
	// attribute bit are stored top-down; bottom-up files store the bottom
	// row first and need a vertical flip.
	const bool topDown = (attributes & TGA_ATTRIB_TOP_DOWN) != 0;
	if (!topDown) {
		const std::size_t rowBytes = pixelBytes / heightU;
		std::vector<std::uint8_t> row(rowBytes);
		for (int y = 0; y < height / 2; ++y) {
			std::uint8_t *top = rgba.data() + static_cast<std::size_t>(y) * rowBytes;
			std::uint8_t *bottom = rgba.data() + static_cast<std::size_t>(height - 1 - y) * rowBytes;
			std::memcpy(row.data(), top, rowBytes);
			std::memcpy(top, bottom, rowBytes);
			std::memcpy(bottom, row.data(), rowBytes);
		}
	}

	out->width = width;
	out->height = height;
	out->rgba = std::move(rgba);
	out->ok = true;
	return true;
}

} // namespace vrhi_tga

#endif // VRHI_TGA_DECODE_H
