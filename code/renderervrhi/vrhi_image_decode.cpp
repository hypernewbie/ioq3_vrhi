#include "renderervrhi/vrhi_image_decode.h"

#include <algorithm>
#include <csetjmp>
#include <cstring>
#include <limits>
#include <setjmp.h>
#include <vector>

// puff.c is a C translation unit; give it C linkage so the C++ decoder
// references the same undecorated symbol on the MSVC/clang-cl ABI.
extern "C" {
#include "renderercommon/puff.h"
}

#ifdef USE_INTERNAL_JPEG
# define JPEG_INTERNALS
#endif
#include <jpeglib.h>

namespace vrhi_image {
namespace {

static const std::size_t kMaxCompressed = 64u * 1024u * 1024u;

static void Reset(DecodeResult *out) {
    if (out != nullptr) {
        out->ok = false;
        out->width = 0;
        out->height = 0;
        out->rgba.clear();
    }
}

static std::uint32_t ReadBE32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
        (static_cast<std::uint32_t>(p[1]) << 16) |
        (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

static std::uint32_t Adler32(const std::uint8_t *p, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        a += p[i]; if (a >= 65521u) a -= 65521u;
        b += a; if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

static std::uint8_t Paeth(std::uint8_t a, std::uint8_t b, std::uint8_t c) {
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = std::abs(p - static_cast<int>(a));
    const int pb = std::abs(p - static_cast<int>(b));
    const int pc = std::abs(p - static_cast<int>(c));
    return pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
}

} // namespace

bool DecodePNG(const std::uint8_t *data, std::size_t size,
    int maxDimension, std::size_t maxBytes, DecodeResult *out) {
    Reset(out);
    if (data == nullptr || out == nullptr || size < 8 || size > kMaxCompressed)
        return false;
    static const std::uint8_t signature[8] =
        { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (std::memcmp(data, signature, sizeof(signature)) != 0) return false;

    std::uint32_t width = 0, height = 0;
    std::uint8_t colorType = 0, bitDepth = 0;
    bool haveIHDR = false, haveIEND = false, havePLTE = false, haveIDAT = false;
    bool idatFinished = false, haveTRNS = false;
    std::vector<std::uint8_t> idat, palette, trns;
    std::size_t pos = 8;
    while (pos < size) {
        if (size - pos < 12) return false;
        const std::uint32_t length = ReadBE32(data + pos);
        pos += 4;
        if (static_cast<std::size_t>(length) > size - pos - 8) return false;
        const std::uint8_t *type = data + pos;
        pos += 4;
        const std::uint8_t *chunk = data + pos;
        pos += length;
        const std::uint32_t crc = ReadBE32(data + pos);
        pos += 4;
        // Chunk names are ASCII letters, and the CRC covers type + payload.
        for (int i = 0; i < 4; ++i)
            if ((type[i] < 'A' || type[i] > 'Z') &&
                (type[i] < 'a' || type[i] > 'z')) return false;
        // Avoid a large temporary while still checking the complete chunk.
        std::uint32_t calc = 0xffffffffu;
        for (int i = 0; i < 4; ++i) {
            calc ^= type[i];
            for (int bit = 0; bit < 8; ++bit)
                calc = (calc >> 1) ^ (0xedb88320u & (0u - (calc & 1u)));
        }
        for (std::uint32_t i = 0; i < length; ++i) {
            calc ^= chunk[i];
            for (int bit = 0; bit < 8; ++bit)
                calc = (calc >> 1) ^ (0xedb88320u & (0u - (calc & 1u)));
        }
        calc ^= 0xffffffffu;
        if (calc != crc) return false;

        const auto is = [&](char a, char b, char c, char d) {
            return type[0] == static_cast<std::uint8_t>(a) &&
                type[1] == static_cast<std::uint8_t>(b) &&
                type[2] == static_cast<std::uint8_t>(c) &&
                type[3] == static_cast<std::uint8_t>(d);
        };
        if (!haveIHDR && !is('I','H','D','R')) return false;
        if (haveIEND) return false;
        if (is('I','H','D','R')) {
            if (haveIHDR || length != 13) return false;
            width = ReadBE32(chunk); height = ReadBE32(chunk + 4);
            bitDepth = chunk[8]; colorType = chunk[9];
            if (width == 0 || height == 0 || maxDimension <= 0 ||
                width > static_cast<std::uint32_t>(maxDimension) ||
                height > static_cast<std::uint32_t>(maxDimension) ||
                bitDepth != 8 || chunk[10] != 0 || chunk[11] != 0 || chunk[12] != 0)
                return false;
            if (colorType != 0 && colorType != 2 && colorType != 3 &&
                colorType != 4 && colorType != 6) return false;
            haveIHDR = true;
        } else if (is('P','L','T','E')) {
            if (havePLTE || haveIDAT || length == 0 || length % 3 != 0 || length > 768)
                return false;
            palette.assign(chunk, chunk + length); havePLTE = true;
        } else if (is('t','R','N','S')) {
            if (haveTRNS || haveIDAT || length > 256) return false;
            trns.assign(chunk, chunk + length); haveTRNS = true;
        } else if (is('I','D','A','T')) {
            if (idatFinished || idat.size() > kMaxCompressed - length) return false;
            idat.insert(idat.end(), chunk, chunk + length); haveIDAT = true;
        } else if (is('I','E','N','D')) {
            if (length != 0 || !haveIDAT) return false;
            haveIEND = true;
        } else {
            // Unknown critical chunks are not safe to ignore. Ancillary chunks
            // are ignored after their CRC has been checked above.
            if (type[0] >= 'A' && type[0] <= 'Z') return false;
        }
        if (haveIDAT && !is('I','D','A','T') && !is('I','E','N','D')) idatFinished = true;
    }
    if (!haveIHDR || !haveIEND || !haveIDAT || pos != size) return false;
    if (colorType == 3 && (!havePLTE || palette.empty() || palette.size() / 3 > 256)) return false;
    if (colorType != 3 && havePLTE && palette.size() / 3 > 256) return false;
    if (colorType == 3 && haveTRNS && trns.size() > palette.size() / 3) return false;
    if (colorType == 0 && haveTRNS && trns.size() != 2) return false;
    if (colorType == 2 && haveTRNS && trns.size() != 6) return false;
    if (colorType == 4 && haveTRNS) return false;
    if (colorType == 6 && haveTRNS) return false;
    const std::size_t channels = colorType == 0 ? 1 : colorType == 2 ? 3 :
        colorType == 3 ? 1 : colorType == 4 ? 2 : 4;
    const std::size_t w = width, h = height;
    if (w > std::numeric_limits<std::size_t>::max() / channels ||
        w * channels > std::numeric_limits<std::size_t>::max() / h)
        return false;
    const std::size_t rowBytes = w * channels;
    if (rowBytes > std::numeric_limits<std::size_t>::max() - 1 ||
        (rowBytes + 1) > std::numeric_limits<std::size_t>::max() / h)
        return false;
    const std::size_t rawBytes = (rowBytes + 1) * h;
    if (w > maxBytes / 4u / h || rawBytes > std::numeric_limits<std::uint32_t>::max()) return false;
    const std::size_t outputBytes = w * h * 4u;
    // zlib header: CMF=idat[0] (CM=8, CINFO=7 for a 32K window), FLG=idat[1].
    // The FDICT bit lives in FLG (bit 5), not CMF; a preset dictionary is not
    // supported by puff, so such streams are rejected outright.
    if (idat.size() < 6 || (idat[1] & 0x20u) || (idat[0] & 0x0fu) != 8 ||
        ((static_cast<unsigned>(idat[0]) << 8) + idat[1]) % 31 != 0 ||
        idat.size() > kMaxCompressed || idat.size() > std::numeric_limits<std::uint32_t>::max())
        return false;
    std::vector<std::uint8_t> filtered(rawBytes);
    std::uint32_t dstLen = static_cast<std::uint32_t>(filtered.size());
    std::uint32_t srcLen = static_cast<std::uint32_t>(idat.size() - 6);
    if (puff(filtered.data(), &dstLen, idat.data() + 2, &srcLen) != 0 ||
        dstLen != filtered.size() || srcLen != idat.size() - 6 ||
        Adler32(filtered.data(), filtered.size()) != ReadBE32(idat.data() + idat.size() - 4)) return false;

    std::vector<std::uint8_t> scan(rawBytes);
    const std::size_t bpp = channels;
    for (std::size_t y = 0; y < h; ++y) {
        const std::size_t in = y * (rowBytes + 1), outRow = y * rowBytes;
        const std::uint8_t filter = filtered[in];
        if (filter > 4) return false;
        for (std::size_t x = 0; x < rowBytes; ++x) {
            const std::uint8_t left = x >= bpp ? scan[outRow + x - bpp] : 0;
            const std::uint8_t up = y ? scan[outRow - rowBytes + x] : 0;
            const std::uint8_t ul = (y && x >= bpp) ? scan[outRow - rowBytes + x - bpp] : 0;
            const std::uint8_t v = filtered[in + 1 + x];
            scan[outRow + x] = static_cast<std::uint8_t>(v +
                (filter == 1 ? left : filter == 2 ? up : filter == 3 ?
                    static_cast<std::uint8_t>((static_cast<unsigned>(left) + up) / 2) :
                    filter == 4 ? Paeth(left, up, ul) : 0));
        }
    }
    std::vector<std::uint8_t> rgba(outputBytes);
    for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x) {
        const std::size_t s = y * rowBytes + x * channels, d = (y * w + x) * 4;
        if (colorType == 0) {
            rgba[d] = rgba[d+1] = rgba[d+2] = scan[s]; rgba[d+3] = 255;
            if (haveTRNS && scan[s] == trns[1] && trns[0] == 0) rgba[d+3] = 0;
        } else if (colorType == 2) {
            rgba[d] = scan[s]; rgba[d+1] = scan[s+1]; rgba[d+2] = scan[s+2]; rgba[d+3] = 255;
            if (haveTRNS && trns[0] == 0 && trns[1] == scan[s] && trns[2] == 0 &&
                trns[3] == scan[s+1] && trns[4] == 0 && trns[5] == scan[s+2]) rgba[d+3] = 0;
        } else if (colorType == 3) {
            const std::size_t index = scan[s];
            if (index >= palette.size() / 3) return false;
            rgba[d] = palette[index*3]; rgba[d+1] = palette[index*3+1]; rgba[d+2] = palette[index*3+2];
            rgba[d+3] = index < trns.size() ? trns[index] : 255;
        } else if (colorType == 4) {
            rgba[d] = rgba[d+1] = rgba[d+2] = scan[s]; rgba[d+3] = scan[s+1];
        } else {
            rgba[d] = scan[s]; rgba[d+1] = scan[s+1]; rgba[d+2] = scan[s+2]; rgba[d+3] = scan[s+3];
        }
    }
    out->ok = true; out->width = static_cast<int>(width); out->height = static_cast<int>(height);
    out->rgba = std::move(rgba);
    return true;
}

struct JPEGError { jpeg_error_mgr pub; jmp_buf jump; };
static void JPEGErrorExit(j_common_ptr cinfo) {
    JPEGError *error = reinterpret_cast<JPEGError *>(cinfo->err);
    longjmp(error->jump, 1);
}

bool DecodeJPEG(const std::uint8_t *data, std::size_t size,
    int maxDimension, std::size_t maxBytes, DecodeResult *out) {
    Reset(out);
    if (data == nullptr || out == nullptr || size == 0 || size > kMaxCompressed ||
        maxDimension <= 0 || size > std::numeric_limits<unsigned long>::max()) return false;
    jpeg_decompress_struct cinfo;
    std::memset(&cinfo, 0, sizeof(cinfo));
    JPEGError error;
    std::memset(&error, 0, sizeof(error));
    volatile bool created = false;
    cinfo.err = jpeg_std_error(&error.pub);
    error.pub.error_exit = JPEGErrorExit;
    if (setjmp(error.jump)) {
        if (created) jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo); created = true;
    jpeg_mem_src(&cinfo, const_cast<unsigned char *>(data), static_cast<unsigned long>(size));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK || cinfo.image_width == 0 || cinfo.image_height == 0 ||
        cinfo.image_width > static_cast<JDIMENSION>(maxDimension) || cinfo.image_height > static_cast<JDIMENSION>(maxDimension)) {
        jpeg_destroy_decompress(&cinfo); return false;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    const std::size_t w = cinfo.output_width, h = cinfo.output_height;
    if (w == 0 || h == 0 || w > maxBytes / 4u / h || w > std::numeric_limits<std::size_t>::max() / 4u / h) {
        jpeg_finish_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); return false;
    }
    try {
        std::vector<std::uint8_t> rgba(w * h * 4u);
        std::vector<std::uint8_t> row(w * 3u);
        while (cinfo.output_scanline < cinfo.output_height) {
            JSAMPROW rows[1] = { row.data() };
            if (jpeg_read_scanlines(&cinfo, rows, 1) != 1) { jpeg_destroy_decompress(&cinfo); return false; }
            const std::size_t y = cinfo.output_scanline - 1;
            for (std::size_t x = 0; x < w; ++x) {
                rgba[(y*w+x)*4] = row[x*3]; rgba[(y*w+x)*4+1] = row[x*3+1]; rgba[(y*w+x)*4+2] = row[x*3+2]; rgba[(y*w+x)*4+3] = 255;
            }
        }
        jpeg_finish_decompress(&cinfo); jpeg_destroy_decompress(&cinfo);
        out->ok = true; out->width = static_cast<int>(w); out->height = static_cast<int>(h); out->rgba = std::move(rgba);
        return true;
    } catch (...) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
}

} // namespace vrhi_image
