#include <oxwald/world/heightfield.hpp>

#include "crc32.hpp"

#include <oxwald/core/log.hpp>

#include <glm/common.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

// stb is header-only; STB_*_STATIC keeps the symbols private to this TU so other modules (assets) can
// compile their own copies without duplicate-symbol clashes.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <tinyexr.h>

namespace ox::world {

namespace {

void setError(std::string* err, std::string msg) {
    OX_LOG_WARN("world", "{}", msg);
    if (err) {
        *err = std::move(msg);
    }
}

HeightfieldDesc descFrom(const Heightfield::ImportOptions& o, u32 res) {
    HeightfieldDesc d;
    d.resolution = res;
    d.worldSize = o.worldSize;
    d.heightScale = o.heightScale;
    d.heightOffset = o.heightOffset;
    d.origin = o.origin;
    d.format = o.format;
    return d;
}

void putBE32(std::vector<u8>& v, u32 x) {
    v.push_back(u8(x >> 24));
    v.push_back(u8(x >> 16));
    v.push_back(u8(x >> 8));
    v.push_back(u8(x));
}

void pngChunk(std::vector<u8>& out, const char type[4], const u8* data, usize n) {
    putBE32(out, u32(n));
    const usize start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data, data + n);
    putBE32(out, detail::crc32(out.data() + start, n + 4));
}

} // namespace

std::optional<Heightfield> Heightfield::loadPng(const std::filesystem::path& path, const ImportOptions& opts, std::string* error) {
    int w = 0, h = 0, comp = 0;
    stbi_us* data = stbi_load_16(path.string().c_str(), &w, &h, &comp, 1);
    if (!data) {
        setError(error, std::string("loadPng failed: ") + path.string() + ": " + stbi_failure_reason());
        return std::nullopt;
    }
    if (w != h || w < 2) {
        stbi_image_free(data);
        setError(error, "loadPng: heightmap must be square and >= 2x2: " + path.string());
        return std::nullopt;
    }
    Heightfield hf(descFrom(opts, u32(w)));
    for (int z = 0; z < h; ++z) {
        const int row = opts.flipZ ? h - 1 - z : z;
        for (int x = 0; x < w; ++x) {
            hf.setNormalized(u32(x), u32(z), f32(data[row * w + x]) / 65535.f);
        }
    }
    stbi_image_free(data);
    return hf;
}

std::optional<Heightfield> Heightfield::loadRaw16(const std::filesystem::path& path, const ImportOptions& opts, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        setError(error, "loadRaw16: cannot open " + path.string());
        return std::nullopt;
    }
    std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const usize samples = bytes.size() / 2;
    const u32 res = u32(std::lround(std::sqrt(f64(samples))));
    if (bytes.size() % 2 != 0 || usize(res) * res != samples || res < 2) {
        setError(error, "loadRaw16: file size is not a square of 16-bit samples: " + path.string());
        return std::nullopt;
    }
    Heightfield hf(descFrom(opts, res));
    for (u32 z = 0; z < res; ++z) {
        const u32 row = opts.flipZ ? res - 1 - z : z;
        for (u32 x = 0; x < res; ++x) {
            const usize i = (usize(row) * res + x) * 2;
            const u16 v = u16(bytes[i] | (u16(bytes[i + 1]) << 8)); // little-endian
            hf.setNormalized(x, z, f32(v) / 65535.f);
        }
    }
    return hf;
}

std::optional<Heightfield> Heightfield::loadExr(const std::filesystem::path& path, const ImportOptions& opts, std::string* error) {
    float* rgba = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    if (LoadEXR(&rgba, &w, &h, path.string().c_str(), &err) != TINYEXR_SUCCESS) {
        setError(error, std::string("loadExr failed: ") + path.string() + ": " + (err ? err : "unknown"));
        if (err) {
            FreeEXRErrorMessage(err);
        }
        return std::nullopt;
    }
    if (w != h || w < 2) {
        std::free(rgba);
        setError(error, "loadExr: heightmap must be square: " + path.string());
        return std::nullopt;
    }
    Heightfield hf(descFrom(opts, u32(w)));
    const f32 s = opts.heightScale != 0.f ? opts.heightScale : 1.f;
    for (int z = 0; z < h; ++z) {
        const int row = opts.flipZ ? h - 1 - z : z;
        for (int x = 0; x < w; ++x) {
            hf.setNormalized(u32(x), u32(z), (rgba[(row * w + x) * 4] - opts.heightOffset) / s);
        }
    }
    std::free(rgba);
    return hf;
}

std::optional<Heightfield> Heightfield::load(const std::filesystem::path& path, const ImportOptions& opts, std::string* error) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".png") {
        return loadPng(path, opts, error);
    }
    if (ext == ".r16" || ext == ".raw") {
        return loadRaw16(path, opts, error);
    }
    if (ext == ".exr") {
        return loadExr(path, opts, error);
    }
    setError(error, "Heightfield::load: unsupported extension " + ext);
    return std::nullopt;
}

bool Heightfield::saveRaw16(const std::filesystem::path& path) const {
    const std::vector<u16> v = extractR16(fullRect());
    std::vector<u8> bytes(v.size() * 2);
    for (usize i = 0; i < v.size(); ++i) {
        bytes[i * 2] = u8(v[i] & 0xFF);
        bytes[i * 2 + 1] = u8(v[i] >> 8);
    }
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return bool(f);
}

// stb_image_write only writes 8-bit PNGs; 16-bit grayscale is assembled here using stb's deflate.
bool Heightfield::savePng16(const std::filesystem::path& path) const {
    const u32 res = m_desc.resolution;
    const std::vector<u16> v = extractR16(fullRect());
    std::vector<u8> filtered;
    filtered.reserve(usize(res) * (res * 2 + 1));
    for (u32 z = 0; z < res; ++z) {
        filtered.push_back(0); // filter: none
        for (u32 x = 0; x < res; ++x) {
            const u16 s = v[usize(z) * res + x];
            filtered.push_back(u8(s >> 8)); // PNG is big-endian
            filtered.push_back(u8(s & 0xFF));
        }
    }
    int zlen = 0;
    unsigned char* z = stbi_zlib_compress(filtered.data(), int(filtered.size()), &zlen, 6);
    if (!z) {
        return false;
    }
    std::vector<u8> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> ihdr;
    putBE32(ihdr, res);
    putBE32(ihdr, res);
    ihdr.insert(ihdr.end(), {16, 0, 0, 0, 0}); // bit depth 16, grayscale, deflate, adaptive filter, no interlace
    pngChunk(png, "IHDR", ihdr.data(), ihdr.size());
    pngChunk(png, "IDAT", z, usize(zlen));
    pngChunk(png, "IEND", nullptr, 0);
    STBIW_FREE(z);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(f);
}

} // namespace ox::world
