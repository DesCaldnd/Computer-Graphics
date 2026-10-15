#include <oxwald/assets/byte_stream.hpp>
#include <oxwald/assets/neural/ntc.hpp>
#include <oxwald/assets/texture_import.hpp>
#include <oxwald/core/hash.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/time.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <numbers>

namespace ox::assets {

namespace {

constexpr f32 kTwoPi = 2.0f * std::numbers::pi_v<f32>;
constexpr u32 kChunk = 64;
constexpr u32 kFormatVersion = 1;

u32 hash32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
f32 hashUniform(u32 a, u32 b, u32 c) { return f32(hash32(a ^ hash32(b ^ hash32(c + 0x9e3779b9u))) >> 8) * (1.0f / 16777216.0f); }

u32 mipSize(u32 size, u32 mip) { return std::max(1u, size >> mip); }

struct Taps {
    u32 index[4];
    f32 weight[4];
};

// Hardware-equivalent bilinear footprint (repeat addressing) of uv on a w × h grid.
Taps bilinearTaps(u32 w, u32 h, f32 u, f32 v) {
    const f32 px = u * f32(w) - 0.5f, py = v * f32(h) - 0.5f;
    const f32 fx0 = std::floor(px), fy0 = std::floor(py);
    const f32 fx = px - fx0, fy = py - fy0;
    auto wrap = [](i64 c, u32 n) { return u32(((c % i64(n)) + i64(n)) % i64(n)); };
    const u32 x0 = wrap(i64(fx0), w), x1 = wrap(i64(fx0) + 1, w);
    const u32 y0 = wrap(i64(fy0), h), y1 = wrap(i64(fy0) + 1, h);
    Taps t;
    t.index[0] = y0 * w + x0;
    t.index[1] = y0 * w + x1;
    t.index[2] = y1 * w + x0;
    t.index[3] = y1 * w + x1;
    t.weight[0] = (1.0f - fx) * (1.0f - fy);
    t.weight[1] = fx * (1.0f - fy);
    t.weight[2] = (1.0f - fx) * fy;
    t.weight[3] = fx * fy;
    return t;
}

struct GridDims {
    u32 w = 1, h = 1, f = 0;
};

GridDims gridDims(u32 width, u32 height, u32 pair, u32 grid, u32 stride, u32 features) {
    const u32 mw = mipSize(width, pair * 2), mh = mipSize(height, pair * 2);
    u32 w = std::max(1u, (mw + stride - 1) / stride), h = std::max(1u, (mh + stride - 1) / stride);
    if (grid == 1) w = std::max(1u, (w + 1) / 2), h = std::max(1u, (h + 1) / 2);
    return {w, h, features};
}

// Positional encoding (sin/cos of the position inside the G0 cell) + mip parity: 5 values.
void positionalInputs(f32* in, f32 u, f32 v, u32 g0w, u32 g0h, u32 mip) {
    const f32 cx = u * f32(g0w), cy = v * f32(g0h);
    const f32 fx = cx - std::floor(cx), fy = cy - std::floor(cy);
    in[0] = std::sin(kTwoPi * fx);
    in[1] = std::cos(kTwoPi * fx);
    in[2] = std::sin(kTwoPi * fy);
    in[3] = std::cos(kTwoPi * fy);
    in[4] = (mip & 1u) ? 1.0f : -1.0f;
}

struct PresetParams {
    u32 stride, f0, f1, hidden;
    NtcLatentFormat format;
};

PresetParams presetParams(NtcPreset p) {
    switch (p) {
    case NtcPreset::Low: return {4, 4, 4, 32, NtcLatentFormat::Q4};
    case NtcPreset::High: return {2, 8, 8, 64, NtcLatentFormat::Q4};
    case NtcPreset::Medium:
    default: return {4, 12, 8, 64, NtcLatentFormat::Q4};
    }
}

struct Prepared {
    NtcSourceImages images; // resized to width × height
    u32 width = 0, height = 0;
    NtcChannelLayout layout;
};

Result<Prepared> prepareSources(const NtcSourceImages& src, u32 maxSize) {
    Prepared p;
    const Image* all[4] = {&src.albedo, &src.normal, &src.orm, &src.emissive};
    for (const Image* img : all) {
        if (img->empty()) continue;
        p.width = std::max(p.width, img->width);
        p.height = std::max(p.height, img->height);
    }
    if (p.width == 0) return makeError("neural textures: the material has no textures");
    if (maxSize) {
        while (std::max(p.width, p.height) > maxSize) {
            p.width = std::max(1u, p.width / 2);
            p.height = std::max(1u, p.height / 2);
        }
    }
    const bool srgb[4] = {true, false, false, true};
    Image* out[4] = {&p.images.albedo, &p.images.normal, &p.images.orm, &p.images.emissive};
    for (u32 i = 0; i < 4; ++i) {
        if (all[i]->empty()) continue;
        if (all[i]->width == p.width && all[i]->height == p.height) *out[i] = *all[i];
        else *out[i] = resizeImage(*all[i], p.width, p.height, MipFilter::Kaiser, srgb[i]);
        out[i]->hdr = false;
        for (glm::vec4& px : out[i]->pixels) px = glm::clamp(px, glm::vec4(0.0f), glm::vec4(1.0f));
    }
    if (!p.images.normal.empty()) {
        for (glm::vec4& px : p.images.normal.pixels) {
            glm::vec3 n = glm::vec3(px) * 2.0f - 1.0f;
            const f32 len = glm::length(n);
            n = len > 1e-6f ? n / len : glm::vec3(0, 0, 1);
            px = glm::vec4(n * 0.5f + 0.5f, 1.0f);
        }
    }
    u32 c = 0;
    if (!p.images.albedo.empty()) {
        p.layout.albedo = i32(c);
        c += 3;
        bool alpha = false;
        for (const glm::vec4& px : p.images.albedo.pixels) alpha |= px.a < 254.5f / 255.0f;
        if (alpha) p.layout.alpha = i32(c++);
    }
    if (!p.images.normal.empty()) p.layout.normal = i32(c), c += 2;
    if (!p.images.orm.empty()) p.layout.orm = i32(c), c += 3;
    if (!p.images.emissive.empty()) p.layout.emissive = i32(c), c += 3;
    p.layout.count = c;
    return p;
}

std::array<MipSettings, 4> mipSettings() {
    std::array<MipSettings, 4> m{};
    m[0].srgb = true;
    m[1].normalMap = true;
    m[3].srgb = true;
    return m;
}

TextureImportSettings fallbackSettings(u32 slot) {
    TextureImportSettings t;
    t.type = slot == 1 ? TextureType::Normal : (slot == 2 ? TextureType::Linear : TextureType::Color);
    t.srgb = slot == 0 || slot == 3;
    t.generateMips = true;
    t.compressionQuality = 1;
    return t;
}

// Packs targets of one mip: [y][x][C].
std::vector<f32> levelTargets(const std::array<const Image*, 4>& imgs, const NtcChannelLayout& l) {
    const Image* ref = nullptr;
    for (const Image* i : imgs)
        if (i) ref = i;
    const usize n = usize(ref->width) * ref->height;
    std::vector<f32> t(n * l.count);
    for (usize p = 0; p < n; ++p) {
        f32* o = &t[p * l.count];
        if (l.albedo >= 0) {
            const glm::vec4 a = imgs[0]->pixels[p];
            o[l.albedo] = a.r, o[l.albedo + 1] = a.g, o[l.albedo + 2] = a.b;
            if (l.alpha >= 0) o[l.alpha] = a.a;
        }
        if (l.normal >= 0) o[l.normal] = imgs[1]->pixels[p].r, o[l.normal + 1] = imgs[1]->pixels[p].g;
        if (l.orm >= 0) {
            const glm::vec4 a = imgs[2]->pixels[p];
            o[l.orm] = a.r, o[l.orm + 1] = a.g, o[l.orm + 2] = a.b;
        }
        if (l.emissive >= 0) {
            const glm::vec4 a = imgs[3]->pixels[p];
            o[l.emissive] = a.r, o[l.emissive + 1] = a.g, o[l.emissive + 2] = a.b;
        }
    }
    return t;
}

// Latent storage used during training ([y][x][f] floats in [0,1]) and by the decoder (quantised values).
struct LatentView {
    const NeuralTextureSet* set = nullptr; // quantised (decoder)
    const std::vector<f32>* floats = nullptr; // training
    const std::vector<usize>* offsets = nullptr;
    std::array<GridDims, kNtcMaxPairs * 2> dims{};

    f32 value(u32 pair, u32 grid, u32 texel, u32 feature) const {
        if (floats) {
            const GridDims& d = dims[pair * 2 + grid];
            return (*floats)[(*offsets)[pair * 2 + grid] + usize(texel) * d.f + feature];
        }
        const NtcLatentGrid& g = set->pairs[pair].grids[grid];
        const usize layerSize = usize(g.width) * g.height * 4;
        const f32 maxq = f32((1u << set->latentBits()) - 1u);
        return f32(g.values[(feature / 4) * layerSize + usize(texel) * 4 + (feature & 3u)]) / maxq;
    }
};

struct InputTaps {
    Taps taps[2];
};

// Network input of texel (x, y) of `mip` (also returns the bilinear taps for gradient scatter).
void buildInput(const LatentView& lat, u32 width, u32 height, u32 mip, u32 x, u32 y, f32* in, InputTaps* tapsOut) {
    const u32 pair = mip / 2;
    const u32 mw = mipSize(width, mip), mh = mipSize(height, mip);
    const f32 u = (f32(x) + 0.5f) / f32(mw), v = (f32(y) + 0.5f) / f32(mh);
    std::fill_n(in, kNtcInputs, 0.0f);
    for (u32 g = 0; g < 2; ++g) {
        const GridDims& d = lat.dims[pair * 2 + g];
        const Taps t = bilinearTaps(d.w, d.h, u, v);
        f32* dst = in + (g == 0 ? 0u : kNtcG1Offset);
        for (u32 f = 0; f < d.f; ++f) {
            f32 s = 0.0f;
            for (u32 i = 0; i < 4; ++i) s += t.weight[i] * lat.value(pair, g, t.index[i], f);
            dst[f] = s * 2.0f - 1.0f;
        }
        if (tapsOut) tapsOut->taps[g] = t;
    }
    const GridDims& d0 = lat.dims[pair * 2];
    positionalInputs(in + kNtcPositionalOffset, u, v, d0.w, d0.h, mip);
}

LatentView quantisedView(const NeuralTextureSet& set) {
    LatentView v;
    v.set = &set;
    for (u32 p = 0; p < set.pairs.size(); ++p)
        for (u32 g = 0; g < 2; ++g) {
            const NtcLatentGrid& grid = set.pairs[p].grids[g];
            v.dims[p * 2 + g] = {grid.width, grid.height, grid.features};
        }
    return v;
}

f64 channelMse(const Image& a, const Image& b, u32 ch) {
    f64 s = 0.0;
    for (usize i = 0; i < a.pixels.size(); ++i) {
        const f64 d = f64(a.pixels[i][i32(ch)]) - f64(b.pixels[i][i32(ch)]);
        s += d * d;
    }
    return s / f64(std::max<usize>(1, a.pixels.size()));
}

f64 psnrFromMse(f64 mse) { return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse); }

} // namespace

NeuralTextureSettings MaterialImportSettings::toNeuralSettings() const {
    NeuralTextureSettings s;
    s.preset = neuralPreset;
    s.targetBitsPerTexel = neuralTargetBitsPerTexel;
    s.latentStride = neuralLatentStride;
    s.features0 = neuralFeatures0;
    s.features1 = neuralFeatures1;
    s.latentFormat = neuralLatentFormat;
    s.hidden = neuralHidden;
    s.steps = neuralSteps;
    s.fallbackSize = neuralFallbackSize;
    s.maxSize = neuralMaxSize;
    s.seed = neuralSeed;
    return s;
}

std::string_view ntcPresetName(NtcPreset p) {
    switch (p) {
    case NtcPreset::Low: return "Low";
    case NtcPreset::Medium: return "Medium";
    case NtcPreset::High: return "High";
    case NtcPreset::Custom: return "Custom";
    }
    return "?";
}

NeuralTextureSettings resolveNtcSettings(const NeuralTextureSettings& in) {
    NeuralTextureSettings s = in;
    if (s.targetBitsPerTexel > 0.0f) {
        f64 best = 1e30;
        for (NtcPreset p : {NtcPreset::Low, NtcPreset::Medium, NtcPreset::High}) {
            NeuralTextureSettings t = in;
            t.targetBitsPerTexel = 0.0f;
            t.preset = p;
            const f64 d = std::abs(std::log(estimateNtcBitsPerTexel(resolveNtcSettings(t)) / s.targetBitsPerTexel));
            if (d < best) best = d, s.preset = p;
        }
        s.targetBitsPerTexel = 0.0f;
    }
    if (s.preset != NtcPreset::Custom) {
        const PresetParams pp = presetParams(s.preset);
        s.latentStride = pp.stride;
        s.features0 = pp.f0;
        s.features1 = pp.f1;
        s.hidden = pp.hidden;
        s.latentFormat = pp.format;
    }
    s.latentStride = std::clamp(s.latentStride, 1u, 16u);
    s.features0 = std::clamp((s.features0 + 3) / 4 * 4, 4u, kNtcMaxFeatures0);
    s.features1 = std::clamp((s.features1 + 3) / 4 * 4, 4u, kNtcMaxFeatures1);
    s.hidden = s.hidden <= 32 ? 32 : 64;
    s.batch = std::max(kChunk, (s.batch + kChunk - 1) / kChunk * kChunk);
    s.fallbackSize = std::max(s.fallbackSize, 4u);
    return s;
}

f64 estimateNtcBitsPerTexel(const NeuralTextureSettings& s) {
    const f64 bits = s.latentFormat == NtcLatentFormat::Q4 ? 4.0 : 8.0;
    const f64 st = f64(s.latentStride);
    const f64 g0 = s.features0 * bits / (st * st), g1 = s.features1 * bits / (4.0 * st * st);
    return (g0 + g1) * 16.0 / 15.0; // + coarser pairs
}

u32 NeuralTextureSet::hidden() const { return network.layers().empty() ? 0 : network.layers().front().outputs; }

u64 NeuralTextureSet::latentBytes() const {
    u64 bits = 0;
    for (const NtcLevelPair& p : pairs)
        for (const NtcLatentGrid& g : p.grids) bits += u64(g.width) * g.height * g.features * latentBits();
    return (bits + 7) / 8;
}

u64 NeuralTextureSet::networkBytes() const { return u64(nn::packFp16(network).bytes()); }

u64 NeuralTextureSet::fallbackBytes() const {
    u64 b = 0;
    for (const TextureData& t : fallback)
        for (const TextureMip& m : t.mips) b += m.data.size();
    return b;
}

u64 NeuralTextureSet::inferenceVramBytes() const {
    u64 b = networkBytes() + fallbackBytes();
    const u64 texelBytes = latentFormat == NtcLatentFormat::Q4 ? 2 : 4;
    for (const NtcLevelPair& p : pairs)
        for (const NtcLatentGrid& g : p.grids) b += u64(g.width) * g.height * g.layers() * texelBytes;
    return b;
}

f64 NeuralTextureSet::bitsPerTexel() const {
    return f64(latentBytes() + networkBytes()) * 8.0 / f64(std::max<u64>(1, u64(width) * height));
}

void ntcNetworkInput(const NeuralTextureSet& set, u32 mip, u32 x, u32 y, f32* input) {
    buildInput(quantisedView(set), set.width, set.height, mip, x, y, input, nullptr);
}

Result<NeuralTextureSet> encodeNeuralTextures(const NtcSourceImages& sources, const NeuralTextureSettings& settingsIn,
                                              const NtcProgress& progress) {
    Stopwatch sw(true);
    const NeuralTextureSettings s = resolveNtcSettings(settingsIn);
    auto prep = prepareSources(sources, s.maxSize);
    if (!prep) return prep.error();
    const u32 W = prep->width, H = prep->height;
    const NtcChannelLayout layout = prep->layout;
    if (layout.count > kNtcOutputs) return makeError("neural textures: too many channels ({})", layout.count);

    NeuralTextureSet set;
    set.width = W;
    set.height = H;
    set.mipCount = mipLevelCount(W, H);
    set.latentStride = s.latentStride;
    set.latentFormat = s.latentFormat;
    set.preset = s.preset;
    set.channels = layout;
    u32 neural = 0;
    while (neural < set.mipCount && std::max(mipSize(W, neural), mipSize(H, neural)) > s.fallbackSize) ++neural;
    neural = std::clamp(neural, 1u, std::min(set.mipCount, kNtcMaxPairs * 2));
    set.neuralMips = neural;
    const u32 pairs = (neural + 1) / 2;

    // Reference mip chains and per-level targets.
    const auto ms = mipSettings();
    const Image* src[4] = {&prep->images.albedo, &prep->images.normal, &prep->images.orm, &prep->images.emissive};
    std::array<std::vector<Image>, 4> chains;
    for (u32 i = 0; i < 4; ++i)
        if (!src[i]->empty()) chains[i] = generateMipChain(*src[i], ms[i]);
    std::vector<std::vector<f32>> targets(neural);
    for (u32 m = 0; m < neural; ++m) {
        std::array<const Image*, 4> imgs{};
        for (u32 i = 0; i < 4; ++i) imgs[i] = chains[i].empty() ? nullptr : &chains[i][m];
        targets[m] = levelTargets(imgs, layout);
    }
    // Fallback tail.
    for (u32 i = 0; i < 4; ++i) {
        if (chains[i].empty()) continue;
        if (neural >= set.mipCount) continue;
        auto tex = importTexture(chains[i][neural], fallbackSettings(i));
        if (!tex) return tex.error();
        set.fallback[i] = std::move(*tex);
    }

    // Latents.
    LatentView view;
    std::vector<usize> offsets(pairs * 2);
    std::vector<f32> latents;
    for (u32 p = 0; p < pairs; ++p)
        for (u32 g = 0; g < 2; ++g) {
            const GridDims d = gridDims(W, H, p, g, s.latentStride, g == 0 ? s.features0 : s.features1);
            view.dims[p * 2 + g] = d;
            offsets[p * 2 + g] = latents.size();
            latents.resize(latents.size() + usize(d.w) * d.h * d.f);
        }
    {
        nn::Pcg32 rng(s.seed, 0x1234567ull);
        for (f32& v : latents) v = rng.uniform(0.4f, 0.6f);
    }
    view.floats = &latents;
    view.offsets = &offsets;

    nn::Mlp net = nn::Mlp::make(kNtcInputs, {s.hidden, s.hidden}, kNtcOutputs);
    net.initialize(s.seed);

    // Level sampling: proportional to texel count, at least 1/8 of mip 0's share.
    std::vector<f32> cdf(neural);
    {
        f64 total = 0.0;
        const f64 base = f64(W) * H;
        for (u32 m = 0; m < neural; ++m) {
            total += std::max(f64(mipSize(W, m)) * mipSize(H, m), base / 8.0);
            cdf[m] = f32(total);
        }
        for (f32& c : cdf) c = f32(c / total);
        cdf.back() = 1.0f;
    }
    u64 texels = 0;
    for (u32 m = 0; m < neural; ++m) texels += u64(mipSize(W, m)) * mipSize(H, m);
    const u32 steps = s.steps ? s.steps
                              : u32(std::clamp<u64>(texels * 12 / s.batch, 2000, 16000));
    const u32 quantStep = u32(f32(steps) * 0.85f);
    const u32 batch = s.batch;
    const u32 chunks = batch / kChunk;
    const u32 C = layout.count;
    const f32 maxq = f32((1u << (s.latentFormat == NtcLatentFormat::Q4 ? 4 : 8)) - 1u);
    const u32 F0 = s.features0, F1 = s.features1;
    const u32 seed32 = u32(s.seed ^ (s.seed >> 32));

    struct ChunkState {
        nn::Mlp::Tape tape;
        std::vector<f32> x, t, dOut, dIn, grad;
        std::vector<InputTaps> taps;
        std::vector<u32> pair;
        f32 loss = 0.0f;
    };
    std::vector<ChunkState> cs(chunks);
    for (ChunkState& c : cs) {
        c.x.resize(kChunk * kNtcInputs);
        c.t.resize(kChunk * kNtcOutputs);
        c.dOut.resize(kChunk * kNtcOutputs);
        c.dIn.resize(kChunk * kNtcInputs);
        c.grad.resize(net.paramCount());
        c.taps.resize(kChunk);
        c.pair.resize(kChunk);
    }
    nn::WorkerPool pool(s.threads);
    nn::Adam adamNet, adamLat;
    adamNet.reset(net.paramCount());
    adamLat.reset(latents.size());
    std::vector<f32> gradNet(net.paramCount()), gradLat(latents.size());
    const f32 lrNet0 = s.learningRate, lrLat0 = s.learningRate * 10.0f;
    f32 lastLoss = 0.0f;
    bool quantised = false;

    f64 tRun = 0, tRest = 0;
    for (u32 step = 0; step < steps; ++step) {
        Stopwatch swStep(true);
        const bool phase2 = step >= quantStep;
        if (phase2 && !quantised) {
            for (f32& v : latents) v = std::round(std::clamp(v, 0.0f, 1.0f) * maxq) / maxq;
            quantised = true;
        }
        const f32 noise = phase2 ? 0.0f : 1.0f / maxq;
        pool.run(chunks, [&](u32 c) {
            ChunkState& st = cs[c];
            for (u32 i = 0; i < kChunk; ++i) {
                const u32 id = c * kChunk + i;
                const f32 r = hashUniform(seed32, step, id * 4u);
                u32 m = 0;
                while (m + 1 < neural && r >= cdf[m]) ++m;
                const u32 mw = mipSize(W, m), mh = mipSize(H, m);
                const u32 x = std::min(mw - 1, u32(hashUniform(seed32, step, id * 4u + 1) * f32(mw)));
                const u32 y = std::min(mh - 1, u32(hashUniform(seed32, step, id * 4u + 2) * f32(mh)));
                f32* in = &st.x[usize(i) * kNtcInputs];
                buildInput(view, W, H, m, x, y, in, &st.taps[i]);
                st.pair[i] = m / 2;
                if (noise > 0.0f) {
                    for (u32 g = 0; g < 2; ++g) {
                        const Taps& t = st.taps[i].taps[g];
                        f32 w2 = 0.0f;
                        for (u32 k = 0; k < 4; ++k) w2 += t.weight[k] * t.weight[k];
                        const f32 amp = 2.0f * noise * std::sqrt(w2); // input = 2·latent − 1
                        const u32 base = g == 0 ? 0 : kNtcG1Offset, nf = g == 0 ? F0 : F1;
                        for (u32 f = 0; f < nf; ++f)
                            in[base + f] += amp * (hashUniform(seed32 ^ 0xabcdu, step, (id * 32u + base + f)) - 0.5f);
                    }
                }
                const f32* tgt = &targets[m][(usize(y) * mw + x) * C];
                f32* to = &st.t[usize(i) * kNtcOutputs];
                std::fill_n(to, kNtcOutputs, 0.0f);
                std::copy_n(tgt, C, to);
            }
            net.forwardTrain(st.x.data(), kChunk, st.tape);
            const f32* y = net.tapeOutputs(st.tape);
            const f32 scale = 2.0f / f32(batch * C);
            f32 loss = 0.0f;
            for (u32 i = 0; i < kChunk; ++i) {
                for (u32 k = 0; k < kNtcOutputs; ++k) {
                    const usize j = usize(i) * kNtcOutputs + k;
                    if (k < C) {
                        const f32 d = y[j] - st.t[j];
                        loss += d * d;
                        st.dOut[j] = d * scale;
                    } else {
                        st.dOut[j] = 0.0f;
                    }
                }
            }
            st.loss = loss;
            std::fill(st.grad.begin(), st.grad.end(), 0.0f);
            net.backward(st.tape, st.dOut.data(), st.grad.data(), phase2 ? nullptr : st.dIn.data());
        });
        tRun += swStep.elapsedMs();
        std::fill(gradNet.begin(), gradNet.end(), 0.0f);
        f32 loss = 0.0f;
        for (const ChunkState& c : cs) {
            for (usize i = 0; i < gradNet.size(); ++i) gradNet[i] += c.grad[i];
            loss += c.loss;
        }
        lastLoss = loss / f32(batch * C);
        const f32 t = f32(step) / f32(steps);
        const f32 cosine = 0.5f * (1.0f + std::cos(std::numbers::pi_v<f32> * t));
        adamNet.step(net.params(), gradNet, lrNet0 * (0.02f + 0.98f * cosine));
        net.syncTransposed();
        if (!phase2) {
            std::fill(gradLat.begin(), gradLat.end(), 0.0f);
            for (const ChunkState& c : cs) {
                for (u32 i = 0; i < kChunk; ++i) {
                    const f32* d = &c.dIn[usize(i) * kNtcInputs];
                    const u32 p = c.pair[i];
                    for (u32 g = 0; g < 2; ++g) {
                        const GridDims& dims = view.dims[p * 2 + g];
                        const usize off = offsets[p * 2 + g];
                        const u32 base = g == 0 ? 0 : kNtcG1Offset;
                        const Taps& tp = c.taps[i].taps[g];
                        for (u32 k = 0; k < 4; ++k) {
                            const f32 w = 2.0f * tp.weight[k];
                            if (w == 0.0f) continue;
                            f32* gl = &gradLat[off + usize(tp.index[k]) * dims.f];
                            for (u32 f = 0; f < dims.f; ++f) gl[f] += w * d[base + f];
                        }
                    }
                }
            }
            adamLat.step(latents, gradLat, lrLat0 * (0.02f + 0.98f * cosine), {}, &pool);
            for (f32& v : latents) v = std::clamp(v, 0.0f, 1.0f);
        }
        tRest += swStep.elapsedMs();
        if (progress && (step % 64 == 0 || step + 1 == steps)) progress(f32(step + 1) / f32(steps), lastLoss);
    }
    if (std::getenv("OX_NTC_PROFILE")) std::printf("  [ntc] run %.0f ms, total %.0f ms, threads %u\n", tRun, tRest, pool.threads());
    if (!quantised)
        for (f32& v : latents) v = std::round(std::clamp(v, 0.0f, 1.0f) * maxq) / maxq;

    net.roundToHalf();
    set.network = std::move(net);
    set.pairs.resize(pairs);
    for (u32 p = 0; p < pairs; ++p)
        for (u32 g = 0; g < 2; ++g) {
            const GridDims& d = view.dims[p * 2 + g];
            NtcLatentGrid& grid = set.pairs[p].grids[g];
            grid.width = d.w;
            grid.height = d.h;
            grid.features = d.f;
            const usize layerSize = usize(d.w) * d.h * 4;
            grid.values.assign(layerSize * grid.layers(), 0);
            for (usize t = 0; t < usize(d.w) * d.h; ++t)
                for (u32 f = 0; f < d.f; ++f)
                    grid.values[(f / 4) * layerSize + t * 4 + (f & 3u)] =
                        u8(std::lround(latents[offsets[p * 2 + g] + t * d.f + f] * maxq));
        }
    set.stats.trainMs = sw.elapsedMs();
    set.stats.steps = steps;
    set.stats.finalLoss = lastLoss;
    return set;
}

std::array<Image, 4> decodeNeuralMip(const NeuralTextureSet& set, u32 mip, u32 threads) {
    std::array<Image, 4> out;
    if (mip >= set.neuralMips) return out;
    const u32 w = set.mipWidth(mip), h = set.mipHeight(mip);
    const NtcChannelLayout& l = set.channels;
    if (l.albedo >= 0) out[0] = Image(w, h, glm::vec4(1.0f));
    if (l.normal >= 0) out[1] = Image(w, h, glm::vec4(0.5f, 0.5f, 1.0f, 1.0f));
    if (l.orm >= 0) out[2] = Image(w, h, glm::vec4(1.0f));
    if (l.emissive >= 0) out[3] = Image(w, h, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    const LatentView view = quantisedView(set);
    nn::WorkerPool pool(threads);
    pool.run(h, [&](u32 y) {
        f32 in[kNtcInputs], o[kNtcOutputs];
        std::vector<f32> scratch(set.network.scratchSize());
        for (u32 x = 0; x < w; ++x) {
            buildInput(view, set.width, set.height, mip, x, y, in, nullptr);
            set.network.forward(in, o, scratch.data());
            for (f32& v : o) v = std::clamp(v, 0.0f, 1.0f);
            if (l.albedo >= 0) out[0].at(x, y) = glm::vec4(o[l.albedo], o[l.albedo + 1], o[l.albedo + 2], l.alpha >= 0 ? o[l.alpha] : 1.0f);
            if (l.normal >= 0) {
                const f32 nx = o[l.normal] * 2.0f - 1.0f, ny = o[l.normal + 1] * 2.0f - 1.0f;
                const f32 nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                out[1].at(x, y) = glm::vec4(o[l.normal], o[l.normal + 1], nz * 0.5f + 0.5f, 1.0f);
            }
            if (l.orm >= 0) out[2].at(x, y) = glm::vec4(o[l.orm], o[l.orm + 1], o[l.orm + 2], 1.0f);
            if (l.emissive >= 0) out[3].at(x, y) = glm::vec4(o[l.emissive], o[l.emissive + 1], o[l.emissive + 2], 1.0f);
        }
    });
    return out;
}

NtcDecodedTextures encodeDecodedMips(const NeuralTextureSet& set, const std::vector<std::array<Image, 4>>& mips,
                                     u32 bcQuality) {
    NtcDecodedTextures r;
    Stopwatch sw(true);
    for (u32 slot = 0; slot < 4; ++slot) {
        if (mips.empty() || mips[0][slot].empty()) continue;
        TextureData t;
        t.format = slot == 1 ? TextureFormat::BC5Unorm : (slot == 2 ? TextureFormat::BC7Unorm : TextureFormat::BC7Srgb);
        t.width = set.width;
        t.height = set.height;
        t.normalMap = slot == 1;
        t.mipCount = set.mipCount;
        if (slot == 1) {
            for (const auto& m : mips) t.mips.push_back({m[1].width, m[1].height, compressBC5(m[1])});
        } else {
            std::vector<Image> levels;
            for (const auto& m : mips) levels.push_back(m[slot]);
            auto bc = compressBC7Levels(levels, bcQuality);
            if (!bc) {
                OX_LOG_WARN("assets", "neural textures: BC7 encode failed: {}", bc.error().message);
                continue;
            }
            for (usize i = 0; i < levels.size(); ++i) t.mips.push_back({levels[i].width, levels[i].height, std::move((*bc)[i])});
        }
        for (const TextureMip& m : set.fallback[slot].mips) t.mips.push_back(m);
        t.mipCount = u32(t.mips.size());
        r.textures[slot] = std::move(t);
    }
    r.encodeMs = sw.elapsedMs();
    return r;
}

NtcDecodedTextures decodeToTextures(const NeuralTextureSet& set, u32 bcQuality, u32 threads) {
    Stopwatch sw(true);
    std::vector<std::array<Image, 4>> mips;
    for (u32 m = 0; m < set.neuralMips; ++m) mips.push_back(decodeNeuralMip(set, m, threads));
    const f64 decodeMs = sw.elapsedMs();
    NtcDecodedTextures r = encodeDecodedMips(set, mips, bcQuality);
    r.decodeMs = decodeMs;
    return r;
}

f64 computePsnrChannel(const Image& a, const Image& b, u32 channel) { return psnrFromMse(channelMse(a, b, channel)); }

f64 computeSsim(const Image& a, const Image& b, u32 ch) {
    constexpr f64 c1 = 0.01 * 0.01, c2 = 0.03 * 0.03;
    constexpr u32 kWin = 8, kStride = 4;
    if (a.width < kWin || a.height < kWin) return 1.0 - channelMse(a, b, ch);
    f64 sum = 0.0;
    u32 n = 0;
    for (u32 y = 0; y + kWin <= a.height; y += kStride) {
        for (u32 x = 0; x + kWin <= a.width; x += kStride) {
            f64 ma = 0, mb = 0, va = 0, vb = 0, cov = 0;
            for (u32 j = 0; j < kWin; ++j)
                for (u32 i = 0; i < kWin; ++i) {
                    const f64 pa = a.at(x + i, y + j)[i32(ch)], pb = b.at(x + i, y + j)[i32(ch)];
                    ma += pa, mb += pb;
                }
            ma /= kWin * kWin, mb /= kWin * kWin;
            for (u32 j = 0; j < kWin; ++j)
                for (u32 i = 0; i < kWin; ++i) {
                    const f64 da = a.at(x + i, y + j)[i32(ch)] - ma, db = b.at(x + i, y + j)[i32(ch)] - mb;
                    va += da * da, vb += db * db, cov += da * db;
                }
            const f64 k = 1.0 / (kWin * kWin - 1);
            va *= k, vb *= k, cov *= k;
            sum += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++n;
        }
    }
    return n ? sum / n : 1.0;
}

NtcStats evaluateNeuralTextures(const NeuralTextureSet& set, const NtcSourceImages& sources, bool withBaseline,
                                u32 threads) {
    NtcStats st = set.stats;
    st.channels.clear();
    auto prep = prepareSources(sources, 0);
    if (!prep) return st;
    Prepared p = std::move(*prep);
    Image* srcs[4] = {&p.images.albedo, &p.images.normal, &p.images.orm, &p.images.emissive};
    const bool srgb[4] = {true, false, false, true};
    for (u32 i = 0; i < 4; ++i) {
        if (!srcs[i]->empty() && (srcs[i]->width != set.width || srcs[i]->height != set.height))
            *srcs[i] = resizeImage(*srcs[i], set.width, set.height, MipFilter::Kaiser, srgb[i]);
    }
    const auto decoded = decodeNeuralMip(set, 0, threads);
    std::array<Image, 4> baseline;
    if (withBaseline) {
        for (u32 i = 0; i < 4; ++i) {
            if (srcs[i]->empty()) continue;
            if (i == 1) {
                baseline[i] = decodeBC5(compressBC5(*srcs[i]), srcs[i]->width, srcs[i]->height);
            } else if (auto bc = compressBC7(*srcs[i], 1)) {
                baseline[i] = decodeBC7(*bc, srcs[i]->width, srcs[i]->height);
            }
            u64 bytes = 0;
            for (u32 m = 0; m < set.mipCount; ++m)
                bytes += textureLevelSize(i == 1 ? TextureFormat::BC5Unorm : TextureFormat::BC7Unorm, set.mipWidth(m),
                                          set.mipHeight(m));
            st.sourceBcBytes += bytes;
        }
    }
    auto add = [&](const char* name, u32 slot, std::initializer_list<u32> chans) {
        if (srcs[slot]->empty() || decoded[slot].empty()) return;
        NtcChannelStats c;
        c.name = name;
        f64 mse = 0, ssim = 0, bmse = 0, bssim = 0;
        for (u32 ch : chans) {
            mse += channelMse(decoded[slot], *srcs[slot], ch);
            ssim += computeSsim(decoded[slot], *srcs[slot], ch);
            if (!baseline[slot].empty()) {
                bmse += channelMse(baseline[slot], *srcs[slot], ch);
                bssim += computeSsim(baseline[slot], *srcs[slot], ch);
            }
        }
        const f64 n = f64(chans.size());
        c.psnr = psnrFromMse(mse / n);
        c.ssim = ssim / n;
        if (!baseline[slot].empty()) c.bcPsnr = psnrFromMse(bmse / n), c.bcSsim = bssim / n;
        st.channels.push_back(c);
    };
    add("albedo", 0, {0, 1, 2});
    if (set.channels.alpha >= 0) add("alpha", 0, {3});
    add("normal", 1, {0, 1});
    add("occlusion", 2, {0});
    add("roughness", 2, {1});
    add("metallic", 2, {2});
    add("emissive", 3, {0, 1, 2});
    return st;
}

// ---- serialisation -------------------------------------------------------------------------------------------

std::vector<std::byte> serializeNeuralTextureSet(const NeuralTextureSet& set) {
    ByteWriter w;
    w.writeBytes("OXNT", 4);
    w.write(kFormatVersion);
    w.write(set.width);
    w.write(set.height);
    w.write(set.mipCount);
    w.write(set.neuralMips);
    w.write(set.latentStride);
    w.write(u8(set.latentFormat));
    w.write(u8(set.preset));
    w.write(u16(0));
    for (i32 v : {set.channels.albedo, set.channels.alpha, set.channels.normal, set.channels.orm, set.channels.emissive})
        w.write(v);
    w.write(set.channels.count);
    // Network: layer table + FP16 parameters.
    w.write(u32(set.network.layers().size()));
    for (const nn::LayerDesc& l : set.network.layers()) {
        w.write(l.inputs);
        w.write(l.outputs);
        w.write(u32(l.activation));
    }
    w.write(u32(set.network.paramCount()));
    for (f32 p : set.network.params()) w.write(nn::floatToHalf(p));
    // Latents at their bit depth (Q4: two values per byte, low nibble first).
    w.write(u32(set.pairs.size()));
    for (const NtcLevelPair& p : set.pairs) {
        for (const NtcLatentGrid& g : p.grids) {
            w.write(g.width);
            w.write(g.height);
            w.write(g.features);
            const usize layerSize = usize(g.width) * g.height * 4;
            std::vector<u8> packed;
            if (set.latentFormat == NtcLatentFormat::Q4) {
                packed.assign((usize(g.width) * g.height * g.features + 1) / 2, 0);
                usize k = 0;
                for (u32 l = 0; l < g.layers(); ++l)
                    for (usize t = 0; t < usize(g.width) * g.height; ++t)
                        for (u32 c = 0; c < 4 && l * 4 + c < g.features; ++c, ++k) {
                            const u8 v = g.values[l * layerSize + t * 4 + c] & 0xFu;
                            packed[k / 2] |= (k & 1) ? u8(v << 4) : v;
                        }
            } else {
                for (u32 l = 0; l < g.layers(); ++l)
                    for (usize t = 0; t < usize(g.width) * g.height; ++t)
                        for (u32 c = 0; c < 4 && l * 4 + c < g.features; ++c) packed.push_back(g.values[l * layerSize + t * 4 + c]);
            }
            w.write(u64(packed.size()));
            w.writeBytes(packed.data(), packed.size());
        }
    }
    for (const TextureData& t : set.fallback) {
        if (t.mips.empty()) {
            w.write(u64(0));
            continue;
        }
        const std::vector<std::byte> blob = serializeTexture(t);
        w.write(u64(blob.size()));
        w.writeBytes(blob.data(), blob.size());
    }
    nlohmann::json stats;
    stats["trainMs"] = set.stats.trainMs;
    stats["steps"] = set.stats.steps;
    stats["finalLoss"] = set.stats.finalLoss;
    stats["sourceBcBytes"] = set.stats.sourceBcBytes;
    for (const NtcChannelStats& c : set.stats.channels)
        stats["channels"].push_back({{"name", c.name}, {"psnr", c.psnr}, {"ssim", c.ssim}, {"bcPsnr", c.bcPsnr}, {"bcSsim", c.bcSsim}});
    w.writeString(stats.dump());
    const u32 crc = crc32(w.data());
    w.write(crc);
    return w.take();
}

Result<NeuralTextureSet> deserializeNeuralTextureSet(std::span<const std::byte> data) {
    if (data.size() < 8 || std::memcmp(data.data(), "OXNT", 4) != 0) return makeError("not an .oxntc file");
    if (crc32(data.first(data.size() - 4)) != ByteReader(data.subspan(data.size() - 4)).read<u32>())
        return makeError(".oxntc: CRC mismatch");
    ByteReader r(data.first(data.size() - 4));
    r.skip(4);
    if (r.read<u32>() != kFormatVersion) return makeError(".oxntc: unsupported version");
    NeuralTextureSet set;
    set.width = r.read<u32>();
    set.height = r.read<u32>();
    set.mipCount = r.read<u32>();
    set.neuralMips = r.read<u32>();
    set.latentStride = r.read<u32>();
    set.latentFormat = NtcLatentFormat(r.read<u8>());
    set.preset = NtcPreset(r.read<u8>());
    (void)r.read<u16>();
    set.channels.albedo = r.read<i32>();
    set.channels.alpha = r.read<i32>();
    set.channels.normal = r.read<i32>();
    set.channels.orm = r.read<i32>();
    set.channels.emissive = r.read<i32>();
    set.channels.count = r.read<u32>();
    if (r.failed() || set.width == 0 || set.height == 0 || set.neuralMips == 0 || set.neuralMips > set.mipCount ||
        set.channels.count > kNtcOutputs || u8(set.latentFormat) > 1)
        return makeError(".oxntc: corrupt header");
    const u32 layerCount = r.read<u32>();
    if (layerCount == 0 || layerCount > 8) return makeError(".oxntc: corrupt network");
    std::vector<nn::LayerDesc> layers(layerCount);
    for (nn::LayerDesc& l : layers) {
        l.inputs = r.read<u32>();
        l.outputs = r.read<u32>();
        l.activation = nn::Activation(r.read<u32>());
        if (l.inputs == 0 || l.outputs == 0 || l.inputs > 1024 || l.outputs > 1024) return makeError(".oxntc: corrupt network");
    }
    for (u32 i = 1; i < layerCount; ++i)
        if (layers[i].inputs != layers[i - 1].outputs) return makeError(".oxntc: corrupt network");
    if (layers.front().inputs != kNtcInputs || layers.back().outputs != kNtcOutputs) return makeError(".oxntc: unsupported network shape");
    set.network = nn::Mlp(std::move(layers));
    const u32 paramCount = r.read<u32>();
    if (paramCount != set.network.paramCount()) return makeError(".oxntc: corrupt network");
    std::vector<u16> halves;
    if (!r.readVector(halves, paramCount)) return makeError(".oxntc: truncated");
    for (u32 i = 0; i < paramCount; ++i) set.network.params()[i] = nn::halfToFloat(halves[i]);
    set.network.syncTransposed();
    const u32 pairCount = r.read<u32>();
    if (pairCount != (set.neuralMips + 1) / 2 || pairCount > kNtcMaxPairs) return makeError(".oxntc: corrupt latents");
    set.pairs.resize(pairCount);
    for (NtcLevelPair& p : set.pairs) {
        for (NtcLatentGrid& g : p.grids) {
            g.width = r.read<u32>();
            g.height = r.read<u32>();
            g.features = r.read<u32>();
            if (g.width == 0 || g.height == 0 || g.width > 65536 || g.height > 65536 || g.features > 16 || (g.features & 3u))
                return makeError(".oxntc: corrupt latent grid");
            const u64 size = r.read<u64>();
            const usize count = usize(g.width) * g.height * g.features;
            const usize expected = set.latentFormat == NtcLatentFormat::Q4 ? (count + 1) / 2 : count;
            if (size != expected) return makeError(".oxntc: corrupt latent grid");
            std::vector<u8> packed;
            if (!r.readVector(packed, size)) return makeError(".oxntc: truncated");
            const usize layerSize = usize(g.width) * g.height * 4;
            g.values.assign(layerSize * g.layers(), 0);
            usize k = 0;
            for (u32 l = 0; l < g.layers(); ++l)
                for (usize t = 0; t < usize(g.width) * g.height; ++t)
                    for (u32 c = 0; c < 4 && l * 4 + c < g.features; ++c, ++k)
                        g.values[l * layerSize + t * 4 + c] =
                            set.latentFormat == NtcLatentFormat::Q4 ? u8((packed[k / 2] >> ((k & 1) ? 4 : 0)) & 0xFu) : packed[k];
        }
    }
    for (TextureData& t : set.fallback) {
        const u64 size = r.read<u64>();
        if (size == 0) continue;
        if (size > r.remaining()) return makeError(".oxntc: truncated");
        auto tex = deserializeTexture(r.data().subspan(r.position(), size));
        if (!tex) return tex.error();
        t = std::move(*tex);
        r.skip(size);
    }
    const std::string statsText = r.readString();
    if (r.failed()) return makeError(".oxntc: truncated");
    auto stats = nlohmann::json::parse(statsText, nullptr, false);
    if (stats.is_object()) {
        set.stats.trainMs = stats.value("trainMs", 0.0);
        set.stats.steps = stats.value("steps", 0u);
        set.stats.finalLoss = stats.value("finalLoss", 0.0f);
        set.stats.sourceBcBytes = stats.value("sourceBcBytes", u64(0));
        if (stats.contains("channels"))
            for (const auto& c : stats["channels"])
                set.stats.channels.push_back({c.value("name", ""), c.value("psnr", 0.0), c.value("ssim", 0.0),
                                              c.value("bcPsnr", 0.0), c.value("bcSsim", 0.0)});
    }
    return set;
}

} // namespace ox::assets
