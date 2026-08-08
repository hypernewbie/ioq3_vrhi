// Standalone unit test for the VRHI bounded JPG/JPEG and PNG decoder
// (code/renderervrhi/vrhi_image_decode.h/.cpp). It compiles the decoder,
// renderercommon/puff.c, and the internal libjpeg sources with no engine
// runtime; input files are read from argv. Real OpenArena assets can be
// exercised by pointing the test at files extracted from the asset pk3s:
//
//   clang-cl -TP -std:c++17 -DUSE_INTERNAL_JPEG -I code -I code/thirdparty/jpeg-9f
//       tests/vrhi_image_decode_test.cpp code/renderervrhi/vrhi_image_decode.cpp
//       code/renderercommon/puff.c code/thirdparty/jpeg-9f/j*.c
//       /Fe:vrhi_image_decode_test.exe
//   vrhi_image_decode_test.exe <png-file> <jpg-file> [more-files...]
//
// Exit code 0 means every file decoded to the expected format.
#include "../code/renderervrhi/vrhi_image_decode.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

bool ReadFile(const char *path, std::vector<std::uint8_t> *out) {
	std::FILE *file = std::fopen(path, "rb");
	if (file == nullptr) return false;
	std::fseek(file, 0, SEEK_END);
	const long size = std::ftell(file);
	std::fseek(file, 0, SEEK_SET);
	if (size <= 0) { std::fclose(file); return false; }
	out->resize(static_cast<std::size_t>(size));
	const std::size_t got = std::fread(out->data(), 1, out->size(), file);
	std::fclose(file);
	return got == out->size();
}

bool IsPNG(const std::vector<std::uint8_t> &data) {
	static const std::uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	return data.size() >= 8 && std::memcmp(data.data(), signature, 8) == 0;
}

bool IsJPEG(const std::vector<std::uint8_t> &data) {
	return data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 &&
		data[2] == 0xFF;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		std::printf("usage: %s <image-file> [more-files...]\n", argv[0]);
		return 2;
	}
	for (int i = 1; i < argc; ++i) {
		std::vector<std::uint8_t> data;
		if (!ReadFile(argv[i], &data)) {
			std::printf("FAIL: cannot read '%s'\n", argv[i]);
			++g_failures;
			continue;
		}
		vrhi_image::DecodeResult result;
		bool ok = false;
		if (IsPNG(data)) {
			ok = vrhi_image::DecodePNG(data.data(), data.size(), 2048,
				64u * 1024u * 1024u, &result);
		} else if (IsJPEG(data)) {
			ok = vrhi_image::DecodeJPEG(data.data(), data.size(), 2048,
				64u * 1024u * 1024u, &result);
		}
		CHECK(ok);
		if (ok) {
			CHECK(result.width > 0 && result.height > 0);
			CHECK(result.rgba.size() ==
				static_cast<std::size_t>(result.width) * result.height * 4u);
			std::printf("OK  %s -> %dx%d rgba=%zu bytes\n", argv[i],
				result.width, result.height, result.rgba.size());
		} else {
			std::printf("FAIL decode '%s'\n", argv[i]);
		}
		// Caps must reject oversized output cleanly for a valid PNG/JPEG.
		vrhi_image::DecodeResult capped;
		const bool cappedOK = IsPNG(data)
			? vrhi_image::DecodePNG(data.data(), data.size(), 1, 1, &capped)
			: vrhi_image::DecodeJPEG(data.data(), data.size(), 1, 1, &capped);
		CHECK(!cappedOK);
		CHECK(capped.rgba.empty());
	}
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
