// BC4/BC5 encoder + decoder (own) and BC7 decoding (Basis Universal's unpacker shipped inside libktx).
#include <oxwald/assets/texture_import.hpp>

#include "internal.hpp"

#include <cmath>
#include <thread>

namespace basisu {
struct color_rgba;
// Defined in libktx's bundled Basis Universal encoder (basisu_gpu_texture.cpp).
bool unpack_bc7(const void* pBlock, color_rgba* pPixels);
} // namespace basisu

namespace ox::assets {

namespace {

void bc4Palette(u8 e0, u8 e1, u8 out[8]) {
    out[0] = e0;
    out[1] = e1;
    if (e0 > e1) {
        for (int i = 1; i < 7; ++i) out[i + 1] = u8(((7 - i) * e0 + i * e1 + 3) / 7);
    } else {
        for (int i = 1; i < 5; ++i) out[i + 1] = u8(((5 - i) * e0 + i * e1 + 2) / 5);
        out[6] = 0;
        out[7] = 255;
    }
}

u32 bc4Fit(const u8 values[16], u8 e0, u8 e1, u8 indices[16]) {
    u8 pal[8];
    bc4Palette(e0, e1, pal);
    u32 err = 0;
    for (int i = 0; i < 16; ++i) {
        u32 best = ~0u;
        u8 bi = 0;
        for (u8 k = 0; k < 8; ++k) {
            const int d = int(values[i]) - int(pal[k]);
            const u32 e = u32(d * d);
            if (e < best) {
                best = e;
                bi = k;
            }
        }
        indices[i] = bi;
        err += best;
    }
    return err;
}

void encodeBC4Block(const u8 values[16], std::byte out[8]) {
    u8 mn = 255, mx = 0;
    for (int i = 0; i < 16; ++i) {
        mn = std::min(mn, values[i]);
        mx = std::max(mx, values[i]);
    }
    u8 bestE0 = mx, bestE1 = mn;
    u8 bestIdx[16];
    u8 idx[16];
    u32 bestErr;
    if (mx == mn) {
        bestE0 = mx;
        bestE1 = mn;
        bestErr = bc4Fit(values, bestE0, bestE1, bestIdx);
    } else {
        bestErr = bc4Fit(values, mx, mn, bestIdx);
        // Local search around the extremes: inset endpoints usually reduce the error of interior texels.
        for (int d0 = -3; d0 <= 1 && bestErr > 0; ++d0) {
            for (int d1 = -1; d1 <= 3; ++d1) {
                const int e0 = int(mx) + d0;
                const int e1 = int(mn) + d1;
                if (e0 < 0 || e0 > 255 || e1 < 0 || e1 > 255 || e0 <= e1) continue;
                const u32 err = bc4Fit(values, u8(e0), u8(e1), idx);
                if (err < bestErr) {
                    bestErr = err;
                    bestE0 = u8(e0);
                    bestE1 = u8(e1);
                    std::memcpy(bestIdx, idx, 16);
                }
            }
        }
        // Least-squares refinement of the endpoints for the chosen indices (two iterations).
        for (int iter = 0; iter < 2 && bestErr > 0; ++iter) {
            static constexpr f32 kWeight[8] = {0.0f, 1.0f, 1 / 7.f, 2 / 7.f, 3 / 7.f, 4 / 7.f, 5 / 7.f, 6 / 7.f};
            f64 a00 = 0, a01 = 0, a11 = 0, b0 = 0, b1 = 0;
            for (int i = 0; i < 16; ++i) {
                const f64 w1 = kWeight[bestIdx[i]];
                const f64 w0 = 1.0 - w1;
                a00 += w0 * w0;
                a01 += w0 * w1;
                a11 += w1 * w1;
                b0 += w0 * values[i];
                b1 += w1 * values[i];
            }
            const f64 det = a00 * a11 - a01 * a01;
            if (std::abs(det) < 1e-9) break;
            const int e0 = int(std::lround(std::clamp((b0 * a11 - b1 * a01) / det, 0.0, 255.0)));
            const int e1 = int(std::lround(std::clamp((b1 * a00 - b0 * a01) / det, 0.0, 255.0)));
            if (e0 <= e1) break;
            const u32 err = bc4Fit(values, u8(e0), u8(e1), idx);
            if (err >= bestErr) break;
            bestErr = err;
            bestE0 = u8(e0);
            bestE1 = u8(e1);
            std::memcpy(bestIdx, idx, 16);
        }
    }
    if (bestE0 == bestE1) { // flat block: any palette works; keep e0 > e1 semantics irrelevant
        std::memset(bestIdx, 0, 16);
    }
    out[0] = std::byte(bestE0);
    out[1] = std::byte(bestE1);
    u64 bits = 0;
    for (int i = 0; i < 16; ++i) bits |= u64(bestIdx[i] & 7) << (3 * i);
    for (int i = 0; i < 6; ++i) out[2 + i] = std::byte((bits >> (8 * i)) & 0xff);
}

void decodeBC4Block(const std::byte* in, u8 out[16]) {
    u8 pal[8];
    bc4Palette(u8(in[0]), u8(in[1]), pal);
    u64 bits = 0;
    for (int i = 0; i < 6; ++i) bits |= u64(u8(in[2 + i])) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

u8 toU8(f32 v) { return u8(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

} // namespace

std::vector<std::byte> compressBC5(const Image& image) {
    const u32 bw = (image.width + 3) / 4;
    const u32 bh = (image.height + 3) / 4;
    std::vector<std::byte> out(usize(bw) * bh * 16);
    auto encodeRows = [&](u32 by0, u32 by1) {
        for (u32 by = by0; by < by1; ++by) {
            for (u32 bx = 0; bx < bw; ++bx) {
                u8 r[16], g[16];
                for (u32 y = 0; y < 4; ++y) {
                    for (u32 x = 0; x < 4; ++x) {
                        const u32 px = std::min(bx * 4 + x, image.width - 1);
                        const u32 py = std::min(by * 4 + y, image.height - 1);
                        const glm::vec4& c = image.at(px, py);
                        r[y * 4 + x] = toU8(c.r);
                        g[y * 4 + x] = toU8(c.g);
                    }
                }
                std::byte* dst = out.data() + (usize(by) * bw + bx) * 16;
                encodeBC4Block(r, dst);
                encodeBC4Block(g, dst + 8);
            }
        }
    };
    const u32 threads = std::clamp<u32>(std::thread::hardware_concurrency(), 1, 16);
    if (bh < 16 || threads == 1) {
        encodeRows(0, bh);
    } else {
        std::vector<std::thread> pool;
        const u32 step = (bh + threads - 1) / threads;
        for (u32 t = 0; t < threads; ++t) {
            const u32 a = t * step, b = std::min(bh, a + step);
            if (a < b) pool.emplace_back(encodeRows, a, b);
        }
        for (auto& th : pool) th.join();
    }
    return out;
}

Image decodeBC5(std::span<const std::byte> blocks, u32 width, u32 height) {
    Image img(width, height, glm::vec4(0, 0, 0, 1));
    const u32 bw = (width + 3) / 4;
    const u32 bh = (height + 3) / 4;
    if (blocks.size() < usize(bw) * bh * 16) return img;
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            const std::byte* src = blocks.data() + (usize(by) * bw + bx) * 16;
            u8 r[16], g[16];
            decodeBC4Block(src, r);
            decodeBC4Block(src + 8, g);
            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    const u32 px = bx * 4 + x, py = by * 4 + y;
                    if (px >= width || py >= height) continue;
                    const f32 nx = r[y * 4 + x] / 255.0f;
                    const f32 ny = g[y * 4 + x] / 255.0f;
                    const f32 sx = nx * 2 - 1, sy = ny * 2 - 1;
                    const f32 nz = std::sqrt(std::max(0.0f, 1.0f - sx * sx - sy * sy));
                    img.at(px, py) = glm::vec4(nx, ny, nz * 0.5f + 0.5f, 1.0f);
                }
            }
        }
    }
    return img;
}

Image decodeBC7(std::span<const std::byte> blocks, u32 width, u32 height) {
    Image img(width, height, glm::vec4(0.0f));
    const u32 bw = (width + 3) / 4;
    const u32 bh = (height + 3) / 4;
    if (blocks.size() < usize(bw) * bh * 16) return img;
    u8 px[64];
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            std::memset(px, 0, sizeof(px));
            basisu::unpack_bc7(blocks.data() + (usize(by) * bw + bx) * 16, reinterpret_cast<basisu::color_rgba*>(px));
            for (u32 y = 0; y < 4; ++y) {
                for (u32 x = 0; x < 4; ++x) {
                    const u32 ix = bx * 4 + x, iy = by * 4 + y;
                    if (ix >= width || iy >= height) continue;
                    const u8* p = px + (y * 4 + x) * 4;
                    img.at(ix, iy) = glm::vec4(p[0], p[1], p[2], p[3]) / 255.0f;
                }
            }
        }
    }
    return img;
}

} // namespace ox::assets
