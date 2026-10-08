// BC4/BC5 encoder + decoder and a BC7 (BPTC) block decoder, all self-contained.
#include <oxwald/assets/texture_import.hpp>

#include "internal.hpp"

#include <cmath>
#include <thread>

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

struct Bc7Mode {
    u8 subsets, partitionBits, rotationBits, indexSelBits, colorBits, alphaBits, endpointPBits, sharedPBits, indexBits, index2Bits;
};

constexpr Bc7Mode kBc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

constexpr u8 kBc7Weights2[4] = {0, 21, 43, 64};
constexpr u8 kBc7Weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr u8 kBc7Weights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// Subset of each texel, texel 0 in the low bits: 1 bit per texel for two subsets, 2 bits per texel for three.
constexpr u16 kBc7Partition2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000,
    0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c,
    0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660,
    0x0272, 0x04e4, 0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22,
};
constexpr u32 kBc7Partition3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
    0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
    0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
    0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
};

// Anchor texels of subsets 1 and 2 (an anchor's index is stored one bit short); texel 0 always anchors subset 0.
constexpr u8 kBc7Anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2,  8,  8, 15, 2,  8,  2,  2,  8,  8, 2, 2,
    15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,  6,  2, 6, 8, 15, 15, 2, 2,  15, 15, 15, 15, 15, 2, 2, 15,
};
constexpr u8 kBc7Anchor3a[64] = {
    3, 3,  15, 15, 8, 3,  15, 15, 8,  8, 6,  6, 6,  5,  3,  3,  3, 3,  8, 15, 3, 3, 6, 10, 5, 8,  8, 6,  8,  5,  15, 15,
    8, 15, 3,  5,  6, 10, 8,  15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5,  5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3,  3,
};
constexpr u8 kBc7Anchor3b[64] = {
    15, 8, 8,  3,  15, 15, 3, 8,  15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8,  9, 10, 6,  15, 8,  15, 3,  6,  6,  8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

struct Bc7Bits {
    u64 lo = 0, hi = 0;
    u32 pos = 0;

    explicit Bc7Bits(const std::byte* block) {
        for (u32 i = 0; i < 8; ++i) {
            lo |= u64(u8(block[i])) << (8 * i);
            hi |= u64(u8(block[8 + i])) << (8 * i);
        }
    }

    u32 read(u32 n) { // LSB first, n <= 8
        if (n == 0) return 0;
        u64 v = pos < 64 ? lo >> pos : hi >> (pos - 64);
        if (pos < 64 && pos + n > 64) v |= hi << (64 - pos);
        pos += n;
        return u32(v) & ((1u << n) - 1);
    }
};

u8 bc7Interpolate(u32 e0, u32 e1, u32 index, u32 bits) {
    const u32 w = bits == 2 ? kBc7Weights2[index] : bits == 3 ? kBc7Weights3[index] : kBc7Weights4[index];
    return u8((e0 * (64 - w) + e1 * w + 32) >> 6);
}

// 16 RGBA8 texels, row-major. A reserved block (mode byte 0) decodes to transparent black.
void decodeBC7Block(const std::byte* block, u8 out[64]) {
    std::memset(out, 0, 64);
    const u8 first = u8(block[0]);
    if (first == 0) return;
    u32 mode = 0;
    while (!(first & (1u << mode))) ++mode;
    const Bc7Mode& m = kBc7Modes[mode];

    Bc7Bits bits(block);
    bits.read(mode + 1);
    const u32 partition = bits.read(m.partitionBits);
    const u32 rotation = bits.read(m.rotationBits);
    const u32 indexSel = bits.read(m.indexSelBits);

    const u32 endpoints = m.subsets * 2u;
    u8 ep[6][4];
    for (u32 c = 0; c < 3; ++c) {
        for (u32 e = 0; e < endpoints; ++e) ep[e][c] = u8(bits.read(m.colorBits));
    }
    for (u32 e = 0; e < endpoints; ++e) ep[e][3] = u8(bits.read(m.alphaBits));

    u32 pbits[6] = {};
    if (m.endpointPBits) {
        for (u32 e = 0; e < endpoints; ++e) pbits[e] = bits.read(1);
    } else if (m.sharedPBits) {
        for (u32 s = 0; s < m.subsets; ++s) pbits[s * 2] = pbits[s * 2 + 1] = bits.read(1);
    }
    const bool hasPBit = m.endpointPBits || m.sharedPBits;
    for (u32 e = 0; e < endpoints; ++e) {
        for (u32 c = 0; c < 4; ++c) {
            u32 n = c == 3 ? m.alphaBits : m.colorBits;
            if (n == 0) {
                ep[e][c] = 255;
                continue;
            }
            u32 v = ep[e][c];
            if (hasPBit) {
                v = (v << 1) | pbits[e];
                ++n;
            }
            v <<= 8 - n;
            ep[e][c] = u8(v | (v >> n));
        }
    }

    u8 subset[16] = {};
    bool anchor[16] = {true};
    if (m.subsets == 2) {
        for (u32 i = 0; i < 16; ++i) subset[i] = u8((kBc7Partition2[partition] >> i) & 1);
        anchor[kBc7Anchor2[partition]] = true;
    } else if (m.subsets == 3) {
        for (u32 i = 0; i < 16; ++i) subset[i] = u8((kBc7Partition3[partition] >> (2 * i)) & 3);
        anchor[kBc7Anchor3a[partition]] = true;
        anchor[kBc7Anchor3b[partition]] = true;
    }

    u8 idx[16], idx2[16] = {};
    for (u32 i = 0; i < 16; ++i) idx[i] = u8(bits.read(m.indexBits - (anchor[i] ? 1u : 0u)));
    if (m.index2Bits) {
        for (u32 i = 0; i < 16; ++i) idx2[i] = u8(bits.read(m.index2Bits - (i == 0 ? 1u : 0u)));
    }

    // Dual-plane modes carry two index sets; the selection bit says which one drives colour.
    const u8* colorIdx = idx;
    const u8* alphaIdx = idx;
    u32 colorIdxBits = m.indexBits, alphaIdxBits = m.indexBits;
    if (m.index2Bits) {
        if (indexSel) {
            colorIdx = idx2;
            colorIdxBits = m.index2Bits;
        } else {
            alphaIdx = idx2;
            alphaIdxBits = m.index2Bits;
        }
    }

    for (u32 i = 0; i < 16; ++i) {
        const u8* e0 = ep[subset[i] * 2];
        const u8* e1 = ep[subset[i] * 2 + 1];
        u8* p = out + i * 4;
        for (u32 c = 0; c < 3; ++c) p[c] = bc7Interpolate(e0[c], e1[c], colorIdx[i], colorIdxBits);
        p[3] = m.alphaBits ? bc7Interpolate(e0[3], e1[3], alphaIdx[i], alphaIdxBits) : u8(255);
        if (rotation) std::swap(p[3], p[rotation - 1]);
    }
}

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
            decodeBC7Block(blocks.data() + (usize(by) * bw + bx) * 16, px);
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
