#ifndef VRHI_IMAGE_DECODE_H
#define VRHI_IMAGE_DECODE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vrhi_image {

struct DecodeResult {
    bool ok = false;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

bool DecodePNG(const std::uint8_t *data, std::size_t size,
    int maxDimension, std::size_t maxBytes, DecodeResult *out);
bool DecodeJPEG(const std::uint8_t *data, std::size_t size,
    int maxDimension, std::size_t maxBytes, DecodeResult *out);

} // namespace vrhi_image

#endif
