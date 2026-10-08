// Procedural content of the showcase: tileable textures (PNG) and synthesized sounds (16-bit WAV). Everything is
// generated from fixed seeds, so the files are byte-identical on every run.
#include "gen.hpp"

#include <oxwald/assets/image.hpp>
#include <oxwald/core/log.hpp>

#include <cmath>
#include <cstring>
#include <numbers>

namespace ox::showcase::gen {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;

// ---- tileable value noise --------------------------------------------------------------------------------------
u32 hash(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
f32 lattice(i32 x, i32 y, i32 period, u32 seed) {
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    return f32(hash(u32(x) * 73856093u ^ u32(y) * 19349663u ^ seed * 83492791u) & 0xffffff) / f32(0xffffff);
}
f32 valueNoise(f32 u, f32 v, i32 period, u32 seed) {
    const f32 x = u * f32(period), y = v * f32(period);
    const i32 x0 = i32(std::floor(x)), y0 = i32(std::floor(y));
    const f32 fx = x - f32(x0), fy = y - f32(y0);
    const f32 sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const f32 a = lattice(x0, y0, period, seed), b = lattice(x0 + 1, y0, period, seed);
    const f32 c = lattice(x0, y0 + 1, period, seed), d = lattice(x0 + 1, y0 + 1, period, seed);
    return glm::mix(glm::mix(a, b, sx), glm::mix(c, d, sx), sy);
}
// Fractal sum in [0,1], tileable over the unit square.
f32 fbm(f32 u, f32 v, i32 basePeriod, u32 octaves, u32 seed) {
    f32 sum = 0, amp = 0.5f, norm = 0;
    i32 period = basePeriod;
    for (u32 o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(u, v, period, seed + o * 101u);
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return sum / norm;
}

using HeightFn = std::function<f32(f32 u, f32 v)>;

assets::Image makeImage(u32 size, const std::function<glm::vec4(f32 u, f32 v)>& fn) {
    assets::Image img(size, size);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) img.at(x, y) = glm::clamp(fn((f32(x) + 0.5f) / f32(size), (f32(y) + 0.5f) / f32(size)), 0.0f, 1.0f);
    return img;
}

// Tangent-space normal map (OpenGL convention, +Y up in texture space) from a height function.
assets::Image normalFromHeight(u32 size, const HeightFn& h, f32 strength) {
    std::vector<f32> heights(usize(size) * size);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) heights[usize(y) * size + x] = h((f32(x) + 0.5f) / f32(size), (f32(y) + 0.5f) / f32(size));
    auto at = [&](i32 x, i32 y) {
        x = (x + i32(size)) % i32(size);
        y = (y + i32(size)) % i32(size);
        return heights[usize(y) * size + usize(x)];
    };
    assets::Image img(size, size);
    for (i32 y = 0; y < i32(size); ++y)
        for (i32 x = 0; x < i32(size); ++x) {
            const f32 dx = (at(x + 1, y) - at(x - 1, y)) * strength * f32(size) / 512.0f;
            const f32 dy = (at(x, y + 1) - at(x, y - 1)) * strength * f32(size) / 512.0f;
            const glm::vec3 n = glm::normalize(glm::vec3(-dx, dy, 1.0f));
            img.at(u32(x), u32(y)) = glm::vec4(n * 0.5f + 0.5f, 1.0f);
        }
    return img;
}

void writePng(Gen& gen, const std::string& rel, const assets::Image& img, const nlohmann::ordered_json& settings) {
    std::vector<std::byte> png = assets::encodePng(img);
    gen.writeAsset(rel, png);
    gen.meta(rel, settings);
}

// Stone tiles: 4×4 slabs with mortar joints.
struct Tiles {
    i32 count = 4;
    f32 mortar = 0.035f;
    f32 height(f32 u, f32 v) const {
        const f32 x = u * f32(count), y = v * f32(count);
        const f32 fx = x - std::floor(x), fy = y - std::floor(y);
        const f32 edge = std::min(std::min(fx, 1 - fx), std::min(fy, 1 - fy));
        const f32 slab = glm::smoothstep(0.0f, mortar * 2.0f, edge);
        return slab * (0.85f + 0.15f * fbm(u, v, 8, 4, 7)) + 0.05f * fbm(u, v, 32, 3, 9);
    }
    glm::vec4 albedo(f32 u, f32 v) const {
        const i32 tx = i32(u * f32(count)), ty = i32(v * f32(count));
        const f32 tint = lattice(tx, ty, count, 3) * 0.18f;
        const f32 n = fbm(u, v, 16, 5, 11);
        const f32 h = height(u, v);
        glm::vec3 stone = glm::vec3(0.58f, 0.56f, 0.52f) * (0.82f + tint + 0.25f * (n - 0.5f));
        glm::vec3 mortarC(0.28f, 0.27f, 0.25f);
        return glm::vec4(glm::mix(mortarC, stone, glm::smoothstep(0.2f, 0.6f, h)), 1.0f);
    }
};

} // namespace

void generateTextures(Gen& gen) {
    const u32 N = 512;
    const nlohmann::ordered_json color = {{"maxSize", 1024}};
    const nlohmann::ordered_json linear = {{"type", "Linear"}, {"maxSize", 1024}};
    const nlohmann::ordered_json normal = {{"type", "Normal"}, {"maxSize", 1024}};

    // Floor tiles (hub plaza, station floors).
    Tiles tiles;
    writePng(gen, "Textures/Generated/tiles_albedo.png", makeImage(N, [&](f32 u, f32 v) { return tiles.albedo(u, v); }), color);
    writePng(gen, "Textures/Generated/tiles_normal.png",
             normalFromHeight(N, [&](f32 u, f32 v) { return tiles.height(u, v); }, 6.0f), normal);
    writePng(gen, "Textures/Generated/tiles_orm.png", makeImage(N, [&](f32 u, f32 v) {
                 const f32 h = tiles.height(u, v);
                 const f32 rough = glm::mix(0.95f, 0.45f + 0.25f * fbm(u, v, 8, 4, 21), glm::smoothstep(0.2f, 0.6f, h));
                 return glm::vec4(glm::mix(0.6f, 1.0f, h), rough, 0.0f, 1.0f);
             }), linear);

    // Prototype grid (1 m cells with 0.25 m sub-lines when tiled once per metre ×4).
    writePng(gen, "Textures/Generated/grid_albedo.png", makeImage(N, [&](f32 u, f32 v) {
                 auto line = [](f32 t, f32 cells, f32 w) {
                     const f32 f = t * cells - std::floor(t * cells);
                     return 1.0f - glm::smoothstep(0.0f, w, std::min(f, 1 - f));
                 };
                 const f32 major = std::max(line(u, 1, 0.012f), line(v, 1, 0.012f));
                 const f32 minor = std::max(line(u, 4, 0.02f), line(v, 4, 0.02f));
                 glm::vec3 c(0.62f, 0.63f, 0.66f);
                 c = glm::mix(c, glm::vec3(0.48f, 0.49f, 0.52f), minor * 0.6f);
                 c = glm::mix(c, glm::vec3(0.95f, 0.55f, 0.2f), major);
                 return glm::vec4(c, 1.0f);
             }), color);

    // Concrete.
    writePng(gen, "Textures/Generated/concrete_albedo.png", makeImage(N, [&](f32 u, f32 v) {
                 const f32 n = fbm(u, v, 8, 6, 31), s = fbm(u, v, 64, 2, 33);
                 const f32 spots = glm::smoothstep(0.62f, 0.7f, fbm(u, v, 16, 3, 35)) * 0.12f;
                 return glm::vec4(glm::vec3(0.5f, 0.5f, 0.49f) * (0.8f + 0.35f * n + 0.1f * s) - spots, 1.0f);
             }), color);
    writePng(gen, "Textures/Generated/concrete_normal.png",
             normalFromHeight(N, [&](f32 u, f32 v) { return fbm(u, v, 32, 4, 37); }, 1.5f), normal);

    // Wood planks.
    writePng(gen, "Textures/Generated/wood_albedo.png", makeImage(N, [&](f32 u, f32 v) {
                 const i32 plank = i32(v * 6.0f);
                 const f32 offs = lattice(plank, 0, 6, 41);
                 const f32 grain = std::sin((u * 3.0f + offs * 5.0f + fbm(u, v, 4, 4, 43) * 1.5f) * 40.0f) * 0.5f + 0.5f;
                 const f32 fv = v * 6.0f - std::floor(v * 6.0f);
                 const f32 gap = glm::smoothstep(0.0f, 0.04f, std::min(fv, 1 - fv));
                 glm::vec3 c = glm::mix(glm::vec3(0.36f, 0.2f, 0.1f), glm::vec3(0.55f, 0.34f, 0.18f), grain * 0.6f + offs * 0.4f);
                 return glm::vec4(c * (0.4f + 0.6f * gap), 1.0f);
             }), color);

    // Foliage card: three leaves with alpha (alpha-tested, double sided).
    writePng(gen, "Textures/Generated/leaves_albedo.png", makeImage(N, [&](f32 u, f32 v) {
                 f32 alpha = 0.0f;
                 glm::vec3 c(0.18f, 0.42f, 0.12f);
                 const glm::vec2 p(u, v);
                 const glm::vec2 centers[] = {{0.3f, 0.35f}, {0.68f, 0.4f}, {0.5f, 0.72f}, {0.25f, 0.75f}, {0.78f, 0.75f}};
                 const f32 angles[] = {0.6f, -0.7f, 0.1f, 1.2f, -1.1f};
                 for (int i = 0; i < 5; ++i) {
                     glm::vec2 d = p - centers[i];
                     const f32 ca = std::cos(angles[i]), sa = std::sin(angles[i]);
                     d = glm::vec2(ca * d.x - sa * d.y, sa * d.x + ca * d.y);
                     const f32 w = 0.11f * std::sqrt(std::max(0.0f, 1.0f - (d.y / 0.22f) * (d.y / 0.22f)));
                     if (std::abs(d.x) < w && std::abs(d.y) < 0.22f) {
                         alpha = 1.0f;
                         const f32 vein = 1.0f - glm::smoothstep(0.0f, 0.008f, std::abs(d.x));
                         c = glm::mix(glm::vec3(0.16f, 0.4f, 0.1f) * (0.8f + 0.4f * lattice(i, 0, 8, 5)),
                                      glm::vec3(0.45f, 0.6f, 0.2f), vein * 0.7f);
                     }
                 }
                 return glm::vec4(c, alpha);
             }), {{"maxSize", 512}, {"preserveAlphaCoverage", true}, {"alphaCutoff", 0.5}});

    // Terrain layers (grass, rock, sand, snow).
    auto layer = [&](const char* name, glm::vec3 a, glm::vec3 b, i32 period, u32 seed) {
        writePng(gen, std::string("Textures/Generated/terrain_") + name + ".png", makeImage(N, [&](f32 u, f32 v) {
                     const f32 n = fbm(u, v, period, 6, seed);
                     const f32 d = fbm(u, v, period * 8, 2, seed + 5);
                     return glm::vec4(glm::mix(a, b, n) * (0.85f + 0.3f * d), 1.0f);
                 }), color);
    };
    layer("grass", {0.12f, 0.24f, 0.06f}, {0.3f, 0.42f, 0.12f}, 8, 51);
    layer("rock", {0.28f, 0.26f, 0.24f}, {0.5f, 0.48f, 0.45f}, 4, 53);
    layer("sand", {0.55f, 0.47f, 0.33f}, {0.72f, 0.64f, 0.48f}, 16, 55);
    layer("snow", {0.82f, 0.84f, 0.88f}, {0.97f, 0.98f, 1.0f}, 8, 57);

    // Emissive panel pattern (info boards / screens): scan lines.
    writePng(gen, "Textures/Generated/screen_emissive.png", makeImage(256, [&](f32 u, f32 v) {
                 const f32 lines = 0.75f + 0.25f * std::sin(v * 256.0f * kPi * 0.5f);
                 const f32 vign = 1.0f - 0.5f * glm::length(glm::vec2(u, v) - 0.5f);
                 return glm::vec4(glm::vec3(lines * vign), 1.0f);
             }), color);
    OX_LOG_INFO("generate", "textures done");
}

// ---------------------------------------------------------------------------------------------------------------
// Audio

namespace {

struct Synth {
    u32 rate = 22050;
    std::vector<f32> s;
    explicit Synth(f32 seconds, u32 r = 22050) : rate(r), s(usize(seconds * f32(r)), 0.0f) {}
    [[nodiscard]] f32 t(usize i) const { return f32(i) / f32(rate); }
    [[nodiscard]] f32 duration() const { return f32(s.size()) / f32(rate); }
};

u32 g_noiseState = 12345u;
f32 white() {
    g_noiseState = g_noiseState * 1664525u + 1013904223u;
    return f32(g_noiseState >> 8) / f32(1u << 24) * 2.0f - 1.0f;
}

std::vector<std::byte> toWav(const Synth& syn, f32 gain = 0.9f) {
    f32 peak = 1e-6f;
    for (f32 v : syn.s) peak = std::max(peak, std::abs(v));
    const f32 k = gain / peak;
    std::vector<std::byte> out;
    auto put = [&](const void* p, usize n) {
        const auto* b = static_cast<const std::byte*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto u32le = [&](u32 v) { put(&v, 4); };
    auto u16le = [&](u16 v) { put(&v, 2); };
    const u32 dataBytes = u32(syn.s.size() * 2);
    put("RIFF", 4);
    u32le(36 + dataBytes);
    put("WAVEfmt ", 8);
    u32le(16);
    u16le(1);
    u16le(1);
    u32le(syn.rate);
    u32le(syn.rate * 2);
    u16le(2);
    u16le(16);
    put("data", 4);
    u32le(dataBytes);
    for (f32 v : syn.s) {
        const i16 q = i16(std::lround(std::clamp(v * k, -1.0f, 1.0f) * 32767.0f));
        put(&q, 2);
    }
    return out;
}

// One-pole low-pass / high-pass over a buffer.
void lowpass(std::vector<f32>& s, f32 cutoff, u32 rate) {
    const f32 a = 1.0f - std::exp(-2.0f * kPi * cutoff / f32(rate));
    f32 y = 0;
    for (f32& v : s) v = y += a * (v - y);
}

// Seamless loop: crossfade the tail into the head.
void makeLoop(std::vector<f32>& s, usize fade) {
    fade = std::min(fade, s.size() / 2);
    for (usize i = 0; i < fade; ++i) {
        const f32 w = f32(i) / f32(fade);
        s[i] = s[i] * w + s[s.size() - fade + i] * (1 - w);
    }
    s.resize(s.size() - fade);
}

void writeWav(Gen& gen, const std::string& rel, const Synth& syn, f32 gain = 0.9f) {
    gen.writeAsset(rel, toWav(syn, gain));
    gen.meta(rel);
}

} // namespace

void generateAudio(Gen& gen) {
    g_noiseState = 12345u;
    // Wind ambience: low-passed noise with slow gusts (loop).
    {
        Synth a(8.0f);
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            a.s[i] = white() * (0.55f + 0.45f * std::sin(t * 0.8f) * std::sin(t * 0.33f + 1.0f));
        }
        lowpass(a.s, 420.0f, a.rate);
        lowpass(a.s, 900.0f, a.rate);
        makeLoop(a.s, a.rate / 2);
        writeWav(gen, "Audio/ambience_wind.wav", a, 0.6f);
    }
    // Footstep: short thud + gravel noise burst.
    {
        Synth a(0.25f);
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            a.s[i] = (std::sin(2 * kPi * 70.0f * t) * 0.8f + white() * 0.6f) * std::exp(-t * 28.0f);
        }
        lowpass(a.s, 2500.0f, a.rate);
        writeWav(gen, "Audio/footstep.wav", a);
    }
    // UI / interaction beep.
    {
        Synth a(0.18f);
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            a.s[i] = std::sin(2 * kPi * (t < 0.08f ? 880.0f : 1320.0f) * t) * std::min(1.0f, t * 200.0f) * std::exp(-t * 10.0f);
        }
        writeWav(gen, "Audio/beep.wav", a, 0.7f);
    }
    // Music loop: soft pad chord progression (Am – F – C – G), 8 s.
    {
        Synth a(8.0f);
        const f32 chords[4][3] = {{220.0f, 261.63f, 329.63f}, {174.61f, 220.0f, 261.63f}, {261.63f, 329.63f, 392.0f}, {196.0f, 246.94f, 293.66f}};
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            const int c = int(t / 2.0f) % 4;
            const f32 local = std::fmod(t, 2.0f);
            const f32 env = std::min(1.0f, local * 4.0f) * (0.6f + 0.4f * std::exp(-local * 1.5f));
            f32 v = 0;
            for (f32 f : chords[c]) v += std::sin(2 * kPi * f * t) + 0.3f * std::sin(2 * kPi * f * 2.0f * t + 0.5f);
            // Arpeggio on top.
            const f32 arp = chords[c][int(local * 4.0f) % 3] * 2.0f;
            v += 0.5f * std::sin(2 * kPi * arp * t) * std::exp(-std::fmod(local, 0.25f) * 12.0f);
            a.s[i] = v * env;
        }
        lowpass(a.s, 3000.0f, a.rate);
        writeWav(gen, "Audio/music_loop.wav", a, 0.5f);
    }
    // "Announcement": formant-filtered buzz with syllable envelope (ducking sidechain).
    {
        Synth a(2.6f);
        f32 f1 = 0, f2 = 0;
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            const f32 pitch = 140.0f + 20.0f * std::sin(t * 3.0f);
            const f32 buzz = std::fmod(t * pitch, 1.0f) * 2.0f - 1.0f;
            const f32 syl = std::max(0.0f, std::sin(t * 2 * kPi * 3.2f)) * (t < 2.4f ? 1.0f : 0.0f);
            const f32 formant = std::sin(2 * kPi * (600.0f + 300.0f * std::sin(t * 7.0f)) * t);
            f1 += 0.15f * (buzz * formant - f1);
            f2 += 0.4f * (f1 - f2);
            a.s[i] = f2 * syl;
        }
        writeWav(gen, "Audio/announcement.wav", a);
    }
    // Fire crackle loop.
    {
        Synth a(4.0f);
        f32 crackle = 0;
        for (usize i = 0; i < a.s.size(); ++i) {
            if (white() > 0.9985f) crackle = 1.0f;
            crackle *= 0.995f;
            a.s[i] = white() * 0.15f + white() * crackle;
        }
        lowpass(a.s, 3500.0f, a.rate);
        makeLoop(a.s, a.rate / 4);
        writeWav(gen, "Audio/fire_loop.wav", a, 0.7f);
    }
    // Water lapping loop.
    {
        Synth a(6.0f);
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            a.s[i] = white() * (0.4f + 0.6f * std::pow(std::max(0.0f, std::sin(t * 1.3f)), 3.0f));
        }
        lowpass(a.s, 700.0f, a.rate);
        makeLoop(a.s, a.rate / 2);
        writeWav(gen, "Audio/water_loop.wav", a, 0.6f);
    }
    // Machine hum loop (occlusion demo source): harmonics of 55 Hz + rattle.
    {
        Synth a(4.0f);
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            a.s[i] = std::sin(2 * kPi * 55.0f * t) + 0.6f * std::sin(2 * kPi * 110.0f * t) + 0.4f * std::sin(2 * kPi * 165.0f * t) +
                     0.25f * std::sin(2 * kPi * 880.0f * t) * (0.5f + 0.5f * std::sin(2 * kPi * 4.0f * t)) + 0.1f * white();
        }
        writeWav(gen, "Audio/machine_loop.wav", a, 0.6f);
    }
    // Ray gun shot.
    {
        Synth a(0.45f);
        f32 phase = 0;
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            phase += 2 * kPi * (1800.0f * std::exp(-t * 9.0f) + 120.0f) / f32(a.rate);
            a.s[i] = (std::sin(phase) * 0.8f + white() * 0.3f * std::exp(-t * 30.0f)) * std::exp(-t * 7.0f);
        }
        writeWav(gen, "Audio/raygun.wav", a);
    }
    // Door servo / elevator chime.
    {
        Synth a(1.2f);
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            const f32 servo = std::sin(2 * kPi * (180.0f + 60.0f * t) * t) * 0.5f + white() * 0.15f;
            a.s[i] = servo * std::sin(std::min(1.0f, t / 1.2f) * kPi);
        }
        lowpass(a.s, 1800.0f, a.rate);
        writeWav(gen, "Audio/door.wav", a);
    }
    {
        Synth a(1.4f);
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            const f32 f = t < 0.45f ? 659.25f : 523.25f;
            const f32 local = t < 0.45f ? t : t - 0.45f;
            a.s[i] = (std::sin(2 * kPi * f * t) + 0.3f * std::sin(2 * kPi * f * 3 * t)) * std::exp(-local * 3.5f);
        }
        writeWav(gen, "Audio/chime.wav", a, 0.7f);
    }
    // Train rumble loop.
    {
        Synth a(4.0f);
        for (usize i = 0; i < a.s.size(); ++i) {
            const f32 t = a.t(i);
            const f32 clack = std::exp(-std::fmod(t, 0.5f) * 40.0f) + 0.7f * std::exp(-std::fmod(t + 0.12f, 0.5f) * 40.0f);
            a.s[i] = white() * (0.3f + clack) + 0.4f * std::sin(2 * kPi * 48.0f * t);
        }
        lowpass(a.s, 900.0f, a.rate);
        writeWav(gen, "Audio/train_loop.wav", a, 0.6f);
    }
    OX_LOG_INFO("generate", "audio done");
}

} // namespace ox::showcase::gen
