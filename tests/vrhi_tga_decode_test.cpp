// Standalone, dependency-free unit test for the VRHI bounded true-color TGA
// decoder (code/renderervrhi/vrhi_tga_decode.h). No engine headers, no
// filesystem, no third-party libraries: the test builds TGA byte streams in
// memory and asserts decode results.
//
// Build and run (any C++11-or-later compiler):
//   clang++ -std=c++17 -O1 -Wall -Wextra -Werror tests/vrhi_tga_decode_test.cpp -o vrhi_tga_decode_test && ./vrhi_tga_decode_test
//   clang-cl -TP -std:c++17 -W4 tests\vrhi_tga_decode_test.cpp /Fe:vrhi_tga_decode_test.exe && vrhi_tga_decode_test.exe
//
// Exit code 0 means every case passed.

#include "../code/renderervrhi/vrhi_tga_decode.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

using Pixel = std::array<std::uint8_t, 4>; // RGBA in memory

Pixel px(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
	Pixel p = { { r, g, b, a } };
	return p;
}

void appendPixel(std::vector<std::uint8_t> &out, const Pixel &p, int pixelSize) {
	out.push_back(p[2]); // B
	out.push_back(p[1]); // G
	out.push_back(p[0]); // R
	if (pixelSize == 32) out.push_back(p[3]); // A
}

// Builds a TGA file from a top-down RGBA pixel array. For type 10 the pixel
// stream is run-length encoded (RLE packets for runs of 2+, raw packets for
// isolated pixels, packet counts capped at 128) so both packet kinds are
// exercised. bottom-up storage is produced when topDown is false.
std::vector<std::uint8_t> buildTGA(int width, int height, int pixelSize,
	bool topDown, int imageType, const std::vector<Pixel> &topDownPixels,
	bool colormap = false, std::uint8_t idLength = 0) {
	std::vector<std::uint8_t> tga(18 + idLength, 0);
	tga[0] = idLength;
	tga[1] = colormap ? 1 : 0;
	tga[2] = static_cast<std::uint8_t>(imageType);
	tga[12] = static_cast<std::uint8_t>(width & 0xff);
	tga[13] = static_cast<std::uint8_t>((width >> 8) & 0xff);
	tga[14] = static_cast<std::uint8_t>(height & 0xff);
	tga[15] = static_cast<std::uint8_t>((height >> 8) & 0xff);
	tga[16] = static_cast<std::uint8_t>(pixelSize);
	tga[17] = topDown ? 0x20 : 0x00;

	std::vector<Pixel> filePixels;
	for (int y = 0; y < height; ++y) {
		const int srcY = topDown ? y : height - 1 - y;
		for (int x = 0; x < width; ++x) {
			filePixels.push_back(topDownPixels[static_cast<std::size_t>(srcY) * width + x]);
		}
	}

	if (imageType == 2) {
		for (const Pixel &p : filePixels) appendPixel(tga, p, pixelSize);
		return tga;
	}

	std::size_t i = 0;
	while (i < filePixels.size()) {
		std::size_t run = 1;
		while (i + run < filePixels.size() && filePixels[i + run] == filePixels[i] && run < 128) {
			++run;
		}
		if (run >= 2) {
			tga.push_back(static_cast<std::uint8_t>(0x80 | (run - 1)));
			appendPixel(tga, filePixels[i], pixelSize);
			i += run;
			continue;
		}
		// Raw run of isolated pixels (stop before the next run of 2+).
		std::size_t raw = 0;
		while (i + raw < filePixels.size() && raw < 128) {
			std::size_t r2 = 1;
			while (i + raw + r2 < filePixels.size() &&
				filePixels[i + raw + r2] == filePixels[i + raw] && r2 < 128) {
				++r2;
			}
			if (r2 >= 2) break;
			++raw;
		}
		tga.push_back(static_cast<std::uint8_t>(raw - 1));
		for (std::size_t k = 0; k < raw; ++k) {
			appendPixel(tga, filePixels[i + k], pixelSize);
		}
		i += raw;
	}
	return tga;
}

bool decode(const std::vector<std::uint8_t> &file, int maxDimension,
	std::size_t maxBytes, vrhi_tga::DecodeResult *out) {
	return vrhi_tga::Decode(file.data(), file.size(), maxDimension, maxBytes, out);
}

bool checkImage(const vrhi_tga::DecodeResult &out, int width, int height,
	const std::vector<Pixel> &expectedTopDown) {
	if (!out.ok || out.width != width || out.height != height ||
		out.rgba.size() != static_cast<std::size_t>(width) * height * 4) {
		return false;
	}
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
			const Pixel &e = expectedTopDown[static_cast<std::size_t>(y) * width + x];
			if (out.rgba[i + 0] != e[0] || out.rgba[i + 1] != e[1] ||
				out.rgba[i + 2] != e[2] || out.rgba[i + 3] != e[3]) {
				return false;
			}
		}
	}
	return true;
}

void testType2() {
	const int kMaxDim = 2048;
	const std::size_t kMaxBytes = 64u * 1024u * 1024u;
	{
		// 2x2, 24-bit, bottom-up storage.
		const std::vector<Pixel> image = {
			px(255, 0, 0), px(0, 255, 0),
			px(0, 0, 255), px(255, 255, 0)
		};
		const std::vector<std::uint8_t> file = buildTGA(2, 2, 24, false, 2, image);
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, kMaxDim, kMaxBytes, &out));
		CHECK(checkImage(out, 2, 2, image));
	}
	{
		// 2x2, 32-bit, top-down storage, alpha preserved.
		const std::vector<Pixel> image = {
			px(10, 20, 30, 40), px(50, 60, 70, 80),
			px(90, 100, 110, 120), px(130, 140, 150, 160)
		};
		const std::vector<std::uint8_t> file = buildTGA(2, 2, 32, true, 2, image);
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, kMaxDim, kMaxBytes, &out));
		CHECK(checkImage(out, 2, 2, image));
	}
	{
		// Trailing bytes after the pixel block are tolerated.
		const std::vector<Pixel> image = { px(1, 2, 3), px(4, 5, 6) };
		std::vector<std::uint8_t> file = buildTGA(2, 1, 24, true, 2, image);
		file.push_back(0xde);
		file.push_back(0xad);
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, kMaxDim, kMaxBytes, &out));
		CHECK(checkImage(out, 2, 1, image));
	}
	{
		// Exact maxDimension boundary is accepted.
		std::vector<Pixel> image(static_cast<std::size_t>(2048) * 2048, px(7, 8, 9));
		const std::vector<std::uint8_t> file = buildTGA(2048, 2048, 24, false, 2, image);
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, 2048, kMaxBytes, &out));
		CHECK(out.ok && out.width == 2048 && out.height == 2048 &&
			out.rgba.size() == static_cast<std::size_t>(2048) * 2048 * 4);
		CHECK(out.ok && out.rgba[0] == 7 && out.rgba[1] == 8 && out.rgba[2] == 9 && out.rgba[3] == 255);
	}
}

void testType10() {
	const int kMaxDim = 2048;
	const std::size_t kMaxBytes = 64u * 1024u * 1024u;
	{
		// RLE-only stream, bottom-up: file rows are bottom first, so the
		// decoded output must still come out top-down.
		const std::vector<Pixel> image = {
			px(200, 100, 50), px(200, 100, 50), px(200, 100, 50),
			px(1, 2, 3), px(1, 2, 3), px(1, 2, 3)
		};
		const std::vector<std::uint8_t> file = buildTGA(3, 2, 24, false, 10, image);
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, kMaxDim, kMaxBytes, &out));
		CHECK(checkImage(out, 3, 2, image));
	}
	{
		// Mixed RLE + raw packets, 32-bit, top-down, alpha preserved.
		const std::vector<Pixel> image = {
			px(10, 10, 10, 1), px(10, 10, 10, 1), px(20, 21, 22, 2),
			px(30, 31, 32, 3), px(40, 41, 42, 4), px(40, 41, 42, 4)
		};
		const std::vector<std::uint8_t> file = buildTGA(3, 2, 32, true, 10, image);
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, kMaxDim, kMaxBytes, &out));
		CHECK(checkImage(out, 3, 2, image));
	}
	{
		// A run that spans a row boundary decodes into both rows. File-order
		// stream (bottom-up): A A A A A B over 3x2, so the bottom row is
		// A A A and the top row is A A B (the raw pixel is the last one).
		std::vector<std::uint8_t> file(18, 0);
		file[2] = 10;
		file[12] = 3;
		file[14] = 2;
		file[16] = 24;
		file.push_back(0x84);          // RLE packet, 5 pixels
		file.push_back(11); file.push_back(22); file.push_back(33); // B G R
		file.push_back(0x00);          // raw packet, 1 pixel
		file.push_back(44); file.push_back(55); file.push_back(66);
		const std::vector<Pixel> expected = {
			px(33, 22, 11), px(33, 22, 11), px(66, 55, 44),
			px(33, 22, 11), px(33, 22, 11), px(33, 22, 11)
		};
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, kMaxDim, kMaxBytes, &out));
		CHECK(checkImage(out, 3, 2, expected));
	}
	{
		// Single RLE run covering the whole image (1x2, 32-bit, top-down).
		std::vector<std::uint8_t> file(18, 0);
		file[2] = 10;
		file[12] = 1;
		file[14] = 2;
		file[16] = 32;
		file[17] = 0x20;
		file.push_back(0x81);          // RLE packet, 2 pixels
		file.push_back(30); file.push_back(20); file.push_back(10); file.push_back(40);
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, kMaxDim, kMaxBytes, &out));
		CHECK(checkImage(out, 1, 2, { px(10, 20, 30, 40), px(10, 20, 30, 40) }));
	}
	{
		// Trailing bytes after a complete RLE stream are tolerated.
		const std::vector<Pixel> image = { px(9, 9, 9), px(9, 9, 9) };
		std::vector<std::uint8_t> file = buildTGA(2, 1, 24, true, 10, image);
		file.push_back(0xff);
		vrhi_tga::DecodeResult out;
		CHECK(decode(file, kMaxDim, kMaxBytes, &out));
		CHECK(checkImage(out, 2, 1, image));
	}
}

void testEquivalence() {
	const int kMaxDim = 2048;
	const std::size_t kMaxBytes = 64u * 1024u * 1024u;
	const std::vector<Pixel> image = {
		px(1, 2, 3, 4), px(1, 2, 3, 4), px(5, 6, 7, 8),
		px(9, 10, 11, 12), px(13, 14, 15, 16), px(13, 14, 15, 16),
		px(17, 18, 19, 20), px(21, 22, 23, 24), px(21, 22, 23, 24)
	};
	// The same image encoded four ways must decode identically.
	const std::vector<std::uint8_t> type2BottomUp = buildTGA(3, 3, 32, false, 2, image);
	const std::vector<std::uint8_t> type2TopDown = buildTGA(3, 3, 32, true, 2, image);
	const std::vector<std::uint8_t> type10BottomUp = buildTGA(3, 3, 32, false, 10, image);
	const std::vector<std::uint8_t> type10TopDown = buildTGA(3, 3, 32, true, 10, image);
	vrhi_tga::DecodeResult a, b, c, d;
	CHECK(decode(type2BottomUp, kMaxDim, kMaxBytes, &a) && checkImage(a, 3, 3, image));
	CHECK(decode(type2TopDown, kMaxDim, kMaxBytes, &b) && checkImage(b, 3, 3, image));
	CHECK(decode(type10BottomUp, kMaxDim, kMaxBytes, &c) && checkImage(c, 3, 3, image));
	CHECK(decode(type10TopDown, kMaxDim, kMaxBytes, &d) && checkImage(d, 3, 3, image));
	CHECK(a.ok && b.ok && c.ok && d.ok && a.rgba == b.rgba && a.rgba == c.rgba && a.rgba == d.rgba);
}

void expectReject(const char *name, const std::vector<std::uint8_t> &file,
	int maxDimension, std::size_t maxBytes) {
	vrhi_tga::DecodeResult out;
	const bool accepted = decode(file, maxDimension, maxBytes, &out);
	if (accepted) {
		++g_failures;
		std::printf("FAIL: %s should have been rejected\n", name);
	} else if (out.ok || !out.rgba.empty()) {
		++g_failures;
		std::printf("FAIL: %s left a non-reset result behind\n", name);
	}
	++g_checks;
}

void testRejections() {
	const int kMaxDim = 2048;
	const std::size_t kMaxBytes = 64u * 1024u * 1024u;
	const std::vector<Pixel> onePixel = { px(1, 2, 3) };

	{
		vrhi_tga::DecodeResult out;
		CHECK(!vrhi_tga::Decode(nullptr, 0, kMaxDim, kMaxBytes, &out));
		CHECK(!vrhi_tga::Decode(nullptr, 18, kMaxDim, kMaxBytes, &out));
	}
	{
		const std::vector<std::uint8_t> shortHeader(17, 0);
		expectReject("header shorter than 18 bytes", shortHeader, kMaxDim, kMaxBytes);
	}
	{
		// Unsupported image types: 0, 1 (colormapped), 3 (grayscale), 9 (RLE colormapped).
		for (int type : { 0, 1, 3, 9 }) {
			const std::vector<std::uint8_t> file = buildTGA(1, 1, 24, true, type, onePixel);
			expectReject("unsupported image type", file, kMaxDim, kMaxBytes);
		}
	}
	{
		const std::vector<std::uint8_t> file = buildTGA(1, 1, 24, true, 2, onePixel, true);
		expectReject("colormap present", file, kMaxDim, kMaxBytes);
	}
	{
		// 8-bit and 16-bit pixels are outside the 24/32-bit scope.
		const std::vector<std::uint8_t> file8 = buildTGA(1, 1, 8, true, 2, onePixel);
		const std::vector<std::uint8_t> file16 = buildTGA(1, 1, 16, true, 2, onePixel);
		expectReject("8-bit pixels", file8, kMaxDim, kMaxBytes);
		expectReject("16-bit pixels", file16, kMaxDim, kMaxBytes);
	}
	{
		std::vector<std::uint8_t> file = buildTGA(2, 1, 24, true, 2,
			std::vector<Pixel>(2, px(0, 0, 0)));
		file[12] = 0; // width becomes 0
		expectReject("zero width", file, kMaxDim, kMaxBytes);
	}
	{
		std::vector<std::uint8_t> file = buildTGA(2, 1, 24, true, 2,
			std::vector<Pixel>(2, px(0, 0, 0)));
		file[14] = 0; // height becomes 0
		expectReject("zero height", file, kMaxDim, kMaxBytes);
	}
	{
		// 3000 exceeds the 2048 dimension cap.
		const std::vector<std::uint8_t> file = buildTGA(3000, 1, 24, true, 2,
			std::vector<Pixel>(3000, px(0, 0, 0)));
		expectReject("width above dimension cap", file, kMaxDim, kMaxBytes);
	}
	{
		const std::vector<std::uint8_t> file = buildTGA(1, 3000, 24, true, 2,
			std::vector<Pixel>(3000, px(0, 0, 0)));
		expectReject("height above dimension cap", file, kMaxDim, kMaxBytes);
	}
	{
		// Decoded RGBA would exceed the byte cap (1024x1024x4 > cap-1).
		std::vector<Pixel> image(1024 * 1024, px(0, 0, 0));
		const std::vector<std::uint8_t> file = buildTGA(1024, 1024, 32, true, 2, image);
		expectReject("byte cap exceeded (type 2)", file, kMaxDim, 1024u * 1024u * 4u - 1u);
		const std::vector<std::uint8_t> file10 = buildTGA(1024, 1024, 32, true, 10, image);
		expectReject("byte cap exceeded (type 10)", file10, kMaxDim, 1024u * 1024u * 4u - 1u);
	}
	{
		// Hostile dimensions that would overflow small size_t arithmetic:
		// the division guard must reject before any multiply, so only the
		// 18-byte header is needed (no pixel stream is ever touched).
		std::vector<std::uint8_t> file(18, 0);
		file[2] = 2;
		file[12] = 0xff; file[13] = 0xff; // width  = 65535
		file[14] = 0xff; file[15] = 0xff; // height = 65535
		file[16] = 32;
		expectReject("65535x65535 overflow guard", file, 65536, 4096);
	}
	{
		// Truncated uncompressed pixel block.
		std::vector<std::uint8_t> file = buildTGA(4, 4, 24, true, 2,
			std::vector<Pixel>(16, px(1, 2, 3)));
		file.pop_back();
		expectReject("truncated type 2 pixel block", file, kMaxDim, kMaxBytes);
	}
	{
		// id field runs past the end of the file.
		std::vector<std::uint8_t> file = buildTGA(1, 1, 24, true, 2, onePixel, false, 4);
		file.resize(18 + 3); // idLength says 4 but only 3 bytes remain
		expectReject("id field beyond EOF", file, kMaxDim, kMaxBytes);
	}
	{
		// Truncated packet header: the stream ends before the first header byte.
		std::vector<std::uint8_t> file(18, 0);
		file[2] = 10;
		file[12] = 2;
		file[14] = 1;
		file[16] = 24;
		expectReject("truncated RLE packet header", file, kMaxDim, kMaxBytes);
	}
	{
		// Truncated RLE payload: header claims one pixel but no bytes follow.
		std::vector<std::uint8_t> file(18, 0);
		file[2] = 10;
		file[12] = 2;
		file[14] = 1;
		file[16] = 24;
		file.push_back(0x80);
		expectReject("truncated RLE payload", file, kMaxDim, kMaxBytes);
	}
	{
		// Truncated raw payload: header claims two pixels, one is missing.
		std::vector<std::uint8_t> file(18, 0);
		file[2] = 10;
		file[12] = 2;
		file[14] = 1;
		file[16] = 24;
		file.push_back(0x01);
		file.push_back(3); file.push_back(2); file.push_back(1);
		expectReject("truncated raw payload", file, kMaxDim, kMaxBytes);
	}
	{
		// Run-length packet overruns the declared pixel count (3 > 2).
		std::vector<std::uint8_t> file(18, 0);
		file[2] = 10;
		file[12] = 2;
		file[14] = 1;
		file[16] = 24;
		file.push_back(0x82);
		file.push_back(3); file.push_back(2); file.push_back(1);
		expectReject("RLE run overruns pixel count", file, kMaxDim, kMaxBytes);
	}
	{
		// Raw packet overruns the declared pixel count (3 > 2), even though
		// the payload bytes are all present.
		std::vector<std::uint8_t> file(18, 0);
		file[2] = 10;
		file[12] = 2;
		file[14] = 1;
		file[16] = 24;
		file.push_back(0x02);
		for (int i = 0; i < 3; ++i) {
			file.push_back(static_cast<std::uint8_t>(i));
			file.push_back(static_cast<std::uint8_t>(i));
			file.push_back(static_cast<std::uint8_t>(i));
		}
		expectReject("raw packet overruns pixel count", file, kMaxDim, kMaxBytes);
	}
}

} // namespace

int main() {
	testType2();
	testType10();
	testEquivalence();
	testRejections();

	if (g_failures == 0) {
		std::printf("PASS: all %d checks passed (vrhi_tga_decode)\n", g_checks);
		return 0;
	}
	std::printf("FAIL: %d of %d checks failed (vrhi_tga_decode)\n", g_failures, g_checks);
	return 1;
}
