#include <oxwald/assets/image.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <tinyexr.h>

#include <cmath>

namespace ox::assets {

f32 srgbToLinear(f32 v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

f32 linearToSrgb(f32 v) {
    if (v <= 0.0f) return 0.0f;
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

glm::vec4 Image::sampleBilinear(glm::vec2 uv, bool wrapU, bool wrapV) const {
    if (pixels.empty()) return glm::vec4(0.0f);
    const f32 x = uv.x * f32(width) - 0.5f;
    const f32 y = uv.y * f32(height) - 0.5f;
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const f32 tx = x - fx;
    const f32 ty = y - fy;
    auto addr = [](i64 i, u32 n, bool wrap) -> u32 {
        if (wrap) {
            i %= i64(n);
            if (i < 0) i += n;
            return u32(i);
        }
        return u32(std::clamp<i64>(i, 0, i64(n) - 1));
    };
    const u32 x0 = addr(i64(fx), width, wrapU), x1 = addr(i64(fx) + 1, width, wrapU);
    const u32 y0 = addr(i64(fy), height, wrapV), y1 = addr(i64(fy) + 1, height, wrapV);
    const glm::vec4 a = glm::mix(at(x0, y0), at(x1, y0), tx);
    const glm::vec4 b = glm::mix(at(x0, y1), at(x1, y1), tx);
    return glm::mix(a, b, ty);
}

namespace {

Result<Image> loadExr(std::span<const std::byte> data) {
    float* rgba = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    const int rc = LoadEXRFromMemory(&rgba, &w, &h, reinterpret_cast<const unsigned char*>(data.data()), data.size(), &err);
    if (rc != TINYEXR_SUCCESS) {
        std::string msg = err ? err : "unknown error";
        if (err) FreeEXRErrorMessage(err);
        return makeError("EXR decode failed: {}", msg);
    }
    Image img{u32(w), u32(h)};
    img.hdr = true;
    for (usize i = 0; i < img.pixels.size(); ++i) {
        img.pixels[i] = glm::vec4(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]);
    }
    free(rgba);
    return img;
}

} // namespace

Result<Image> loadImageFromMemory(std::span<const std::byte> data, std::string_view extensionHint) {
    OX_PROFILE_ZONE();
    const std::string hint = detail::toLower(extensionHint);
    if (hint == "exr" || hint == ".exr" ||
        (data.size() >= 4 && u8(data[0]) == 0x76 && u8(data[1]) == 0x2f && u8(data[2]) == 0x31 && u8(data[3]) == 0x01)) {
        return loadExr(data);
    }
    const auto* bytes = reinterpret_cast<const stbi_uc*>(data.data());
    const int len = static_cast<int>(data.size());
    int w = 0, h = 0, comp = 0;
    if (stbi_is_hdr_from_memory(bytes, len)) {
        float* px = stbi_loadf_from_memory(bytes, len, &w, &h, &comp, 4);
        if (!px) return makeError("image decode failed: {}", stbi_failure_reason());
        Image img{u32(w), u32(h)};
        img.hdr = true;
        img.channels = u32(comp);
        std::memcpy(img.pixels.data(), px, img.pixels.size() * sizeof(glm::vec4));
        stbi_image_free(px);
        return img;
    }
    if (stbi_is_16_bit_from_memory(bytes, len)) {
        stbi_us* px = stbi_load_16_from_memory(bytes, len, &w, &h, &comp, 4);
        if (!px) return makeError("image decode failed: {}", stbi_failure_reason());
        Image img{u32(w), u32(h)};
        img.channels = u32(comp);
        for (usize i = 0; i < img.pixels.size(); ++i) {
            img.pixels[i] = glm::vec4(px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]) / 65535.0f;
        }
        stbi_image_free(px);
        return img;
    }
    stbi_uc* px = stbi_load_from_memory(bytes, len, &w, &h, &comp, 4);
    if (!px) return makeError("image decode failed: {}", stbi_failure_reason());
    Image img{u32(w), u32(h)};
    img.channels = u32(comp);
    for (usize i = 0; i < img.pixels.size(); ++i) {
        img.pixels[i] = glm::vec4(px[i * 4], px[i * 4 + 1], px[i * 4 + 2], px[i * 4 + 3]) / 255.0f;
    }
    stbi_image_free(px);
    return img;
}

Result<Image> loadImage(const std::filesystem::path& path) {
    auto bytes = detail::readFile(path);
    if (!bytes) return bytes.error();
    auto img = loadImageFromMemory(*bytes, path.extension().string());
    if (!img) return makeError("{}: {}", path.string(), img.error().message);
    return img;
}

std::vector<std::byte> encodePng(const Image& image) {
    std::vector<u8> px(image.pixels.size() * 4);
    for (usize i = 0; i < image.pixels.size(); ++i) {
        for (int c = 0; c < 4; ++c) px[i * 4 + c] = u8(std::clamp(image.pixels[i][c], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    std::vector<std::byte> out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* v = static_cast<std::vector<std::byte>*>(ctx);
            const auto* p = static_cast<const std::byte*>(data);
            v->insert(v->end(), p, p + size);
        },
        &out, int(image.width), int(image.height), 4, px.data(), int(image.width * 4));
    return out;
}

Status saveImagePng(const std::filesystem::path& path, const Image& image) {
    return detail::writeFile(path, encodePng(image));
}

// ---- resampling -------------------------------------------------------------------------------------------

namespace {

f64 besselI0(f64 x) {
    f64 sum = 1.0, term = 1.0;
    const f64 q = x * x / 4.0;
    for (int k = 1; k < 32; ++k) {
        term *= q / (f64(k) * f64(k));
        sum += term;
        if (term < sum * 1e-12) break;
    }
    return sum;
}

struct Kernel {
    f32 radius;
    f32 (*fn)(f32);
};

f32 boxFilter(f32 t) { return (t > -0.5f && t <= 0.5f) ? 1.0f : 0.0f; }

f32 kaiserFilter(f32 t) {
    constexpr f32 kRadius = 3.0f;
    constexpr f64 kAlpha = 4.0;
    const f32 at = std::abs(t);
    if (at >= kRadius) return 0.0f;
    const f64 x = f64(t) * glm::pi<f64>();
    const f64 sinc = at < 1e-6f ? 1.0 : std::sin(x) / x;
    const f64 r = f64(t) / kRadius;
    static const f64 denom = besselI0(kAlpha);
    return f32(sinc * besselI0(kAlpha * std::sqrt(std::max(0.0, 1.0 - r * r))) / denom);
}

struct Contrib {
    u32 first;
    std::vector<f32> weights;
};

std::vector<Contrib> computeContribs(u32 src, u32 dst, MipFilter filter) {
    const Kernel k = filter == MipFilter::Box ? Kernel{0.5f, boxFilter} : Kernel{3.0f, kaiserFilter};
    const f32 scale = f32(src) / f32(dst);
    const f32 fscale = std::max(scale, 1.0f);
    const f32 support = k.radius * fscale;
    std::vector<Contrib> out(dst);
    for (u32 x = 0; x < dst; ++x) {
        const f32 center = (f32(x) + 0.5f) * scale;
        i64 lo = i64(std::floor(center - support));
        i64 hi = i64(std::ceil(center + support));
        std::vector<f32> w;
        f32 sum = 0.0f;
        for (i64 i = lo; i <= hi; ++i) {
            const f32 v = k.fn((f32(i) + 0.5f - center) / fscale);
            w.push_back(v);
            sum += v;
        }
        if (std::abs(sum) < 1e-8f) { // magnification with a box filter: nearest
            w.assign(w.size(), 0.0f);
            const i64 nearest = std::clamp<i64>(i64(center), lo, hi);
            w[usize(nearest - lo)] = 1.0f;
            sum = 1.0f;
        }
        // Fold clamped taps onto the edge texels.
        Contrib c;
        const i64 first = std::clamp<i64>(lo, 0, src - 1);
        const i64 last = std::clamp<i64>(hi, 0, src - 1);
        c.first = u32(first);
        c.weights.assign(usize(last - first + 1), 0.0f);
        for (i64 i = lo; i <= hi; ++i) {
            const i64 ci = std::clamp<i64>(i, 0, src - 1);
            c.weights[usize(ci - first)] += w[usize(i - lo)] / sum;
        }
        out[x] = std::move(c);
    }
    return out;
}

Image resampleLinearSpace(const Image& src, u32 w, u32 h, MipFilter filter) {
    const auto cx = computeContribs(src.width, w, filter);
    const auto cy = computeContribs(src.height, h, filter);
    Image tmp(w, src.height);
    for (u32 y = 0; y < src.height; ++y) {
        for (u32 x = 0; x < w; ++x) {
            glm::vec4 acc(0.0f);
            const auto& c = cx[x];
            for (usize i = 0; i < c.weights.size(); ++i) acc += src.at(c.first + u32(i), y) * c.weights[i];
            tmp.at(x, y) = acc;
        }
    }
    Image out(w, h);
    out.channels = src.channels;
    out.hdr = src.hdr;
    for (u32 y = 0; y < h; ++y) {
        const auto& c = cy[y];
        for (u32 x = 0; x < w; ++x) {
            glm::vec4 acc(0.0f);
            for (usize i = 0; i < c.weights.size(); ++i) acc += tmp.at(x, c.first + u32(i)) * c.weights[i];
            out.at(x, y) = acc;
        }
    }
    return out;
}

void toLinear(Image& img) {
    for (auto& p : img.pixels) {
        p.r = srgbToLinear(p.r);
        p.g = srgbToLinear(p.g);
        p.b = srgbToLinear(p.b);
    }
}
void toSrgb(Image& img) {
    for (auto& p : img.pixels) {
        p.r = linearToSrgb(p.r);
        p.g = linearToSrgb(p.g);
        p.b = linearToSrgb(p.b);
    }
}
void clampResult(Image& img) {
    for (auto& p : img.pixels) {
        p = img.hdr ? glm::max(p, glm::vec4(0.0f)) : glm::clamp(p, glm::vec4(0.0f), glm::vec4(1.0f));
    }
}
void renormalize(Image& img) {
    for (auto& p : img.pixels) {
        glm::vec3 n = glm::vec3(p) * 2.0f - 1.0f;
        const f32 len = glm::length(n);
        n = len > 1e-6f ? n / len : glm::vec3(0, 0, 1);
        p = glm::vec4(n * 0.5f + 0.5f, p.a);
    }
}

} // namespace

Image resizeImage(const Image& src, u32 width, u32 height, MipFilter filter, bool srgb) {
    OX_PROFILE_ZONE();
    if (src.width == width && src.height == height) return src;
    if (!srgb) {
        Image out = resampleLinearSpace(src, width, height, filter);
        clampResult(out);
        return out;
    }
    Image lin = src;
    toLinear(lin);
    Image out = resampleLinearSpace(lin, width, height, filter);
    clampResult(out);
    toSrgb(out);
    return out;
}

u32 mipLevelCount(u32 width, u32 height) {
    u32 n = 1;
    u32 m = std::max(width, height);
    while (m > 1) {
        m >>= 1;
        ++n;
    }
    return n;
}

f32 alphaCoverage(const Image& image, f32 cutoff, f32 scale) {
    if (image.pixels.empty()) return 0.0f;
    usize covered = 0;
    for (const auto& p : image.pixels) {
        if (p.a * scale >= cutoff) ++covered;
    }
    return f32(covered) / f32(image.pixels.size());
}

std::vector<Image> generateMipChain(const Image& base, const MipSettings& settings, u32 maxLevels) {
    OX_PROFILE_ZONE();
    u32 levels = mipLevelCount(base.width, base.height);
    if (maxLevels > 0) levels = std::min(levels, maxLevels);
    std::vector<Image> chain;
    chain.reserve(levels);
    chain.push_back(base);
    const f32 targetCoverage =
        settings.preserveAlphaCoverage ? alphaCoverage(base, settings.alphaCutoff) : 0.0f;
    // Filter every level from a linear-space copy of the previous one so sRGB round trips do not accumulate.
    Image prevLinear = base;
    if (settings.srgb) toLinear(prevLinear);
    for (u32 l = 1; l < levels; ++l) {
        const u32 w = std::max(1u, base.width >> l);
        const u32 h = std::max(1u, base.height >> l);
        Image lin = resampleLinearSpace(prevLinear, w, h, settings.filter);
        clampResult(lin);
        if (settings.normalMap) renormalize(lin);
        Image level = lin;
        if (settings.srgb) toSrgb(level);
        if (settings.preserveAlphaCoverage && targetCoverage > 0.0f) {
            // Binary search the alpha scale that restores the level-0 coverage (Castaño, "Computing alpha mipmaps").
            f32 lo = 0.0f, hi = 8.0f, best = 1.0f;
            for (int it = 0; it < 24; ++it) {
                const f32 mid = (lo + hi) * 0.5f;
                const f32 cov = alphaCoverage(level, settings.alphaCutoff, mid);
                best = mid;
                if (cov < targetCoverage) lo = mid;
                else hi = mid;
            }
            for (auto& p : level.pixels) p.a = std::min(1.0f, p.a * best);
        }
        prevLinear = std::move(lin);
        chain.push_back(std::move(level));
    }
    return chain;
}

// ---- cubemaps ---------------------------------------------------------------------------------------------

glm::vec3 cubeFaceDirection(u32 face, f32 u, f32 v) {
    const f32 s = u * 2.0f - 1.0f;
    const f32 t = v * 2.0f - 1.0f;
    glm::vec3 d;
    switch (face) {
    case 0: d = {1.0f, -t, -s}; break;
    case 1: d = {-1.0f, -t, s}; break;
    case 2: d = {s, 1.0f, t}; break;
    case 3: d = {s, -1.0f, -t}; break;
    case 4: d = {s, -t, 1.0f}; break;
    default: d = {-s, -t, -1.0f}; break;
    }
    return glm::normalize(d);
}

std::array<Image, 6> equirectToCubemap(const Image& eq, u32 faceSize) {
    OX_PROFILE_ZONE();
    std::array<Image, 6> faces;
    // 2x2 supersampling per texel avoids aliasing when the equirect is much larger than a face.
    constexpr f32 kOffsets[2] = {0.25f, 0.75f};
    for (u32 f = 0; f < 6; ++f) {
        faces[f] = Image(faceSize, faceSize);
        faces[f].hdr = eq.hdr;
        faces[f].channels = eq.channels;
        for (u32 y = 0; y < faceSize; ++y) {
            for (u32 x = 0; x < faceSize; ++x) {
                glm::vec4 acc(0.0f);
                for (f32 oy : kOffsets) {
                    for (f32 ox : kOffsets) {
                        const glm::vec3 d = cubeFaceDirection(f, (f32(x) + ox) / f32(faceSize), (f32(y) + oy) / f32(faceSize));
                        const f32 u = std::atan2(d.x, -d.z) / glm::two_pi<f32>() + 0.5f;
                        const f32 v = std::acos(glm::clamp(d.y, -1.0f, 1.0f)) / glm::pi<f32>();
                        acc += eq.sampleBilinear({u, v}, true, false);
                    }
                }
                faces[f].at(x, y) = acc * 0.25f;
            }
        }
    }
    return faces;
}

} // namespace ox::assets
