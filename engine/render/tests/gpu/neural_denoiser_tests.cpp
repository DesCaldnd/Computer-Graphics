// Neural denoiser (r.Denoiser Neural): the learned À-trous kernel of render/neural/denoise_neural.comp.
//
//   NeuralDenoiserTest.TrainWeights   opt-in tool (OX_TRAIN_NEURAL_DENOISER=1): renders noisy / reference pairs with
//                                     the engine's path tracer (and the synthetic 1-spp AO signal), trains the
//                                     per-iteration tap MLPs on the CPU and writes
//                                     engine/shaders/render/neural/denoiser_weights.glsl.
//   NeuralDenoiserTest.ComparedToSvgf RMSE / PSNR of SVGF and the neural kernel on the stochastic AO signal (same
//                                     temporal + variance passes), frame 1 and after accumulation, + 1080p cost.
#include "render_fixture.hpp"

#include <oxwald/assets/neural/mlp.hpp>
#include <oxwald/core/time.hpp>
#include <oxwald/render/features/raytracing/denoiser.hpp>
#include <oxwald/render/neural/neural.hpp>
#include <oxwald/render/render_types.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>

using namespace ox;
using namespace ox::render;
using namespace ox::render::test;
namespace fs = std::filesystem;

namespace {

constexpr u32 kIn = 6, kHidden = 16, kIterations = 4;
const f32 kB3[3] = {3.0f / 8.0f, 1.0f / 4.0f, 1.0f / 16.0f};

rhi::TextureHandle makeTarget(rhi::Device& dev, Extent2D e, VkFormat f, const char* name) {
    rhi::TextureDesc td;
    td.format = f;
    td.width = e.width;
    td.height = e.height;
    td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst |
               rhi::TextureUsage::TransferSrc;
    td.name = name;
    return dev.createTexture(td);
}

std::vector<glm::vec4> readRgba(rhi::Device& dev, rhi::TextureHandle t) {
    const rhi::TextureDesc& d = dev.desc(t);
    const std::vector<u8> bytes = dev.readTexture(t);
    std::vector<glm::vec4> out(usize(d.width) * d.height);
    if (d.format == VK_FORMAT_R32G32B32A32_SFLOAT) {
        std::memcpy(out.data(), bytes.data(), std::min(bytes.size(), out.size() * sizeof(glm::vec4)));
    } else {
        for (usize i = 0; i < out.size(); ++i) {
            u16 h[4];
            std::memcpy(h, &bytes[i * 8], 8);
            out[i] = {nn::halfToFloat(h[0]), nn::halfToFloat(h[1]), nn::halfToFloat(h[2]), nn::halfToFloat(h[3])};
        }
    }
    return out;
}

// Copies colour (÷ pre-exposure when `exposed`) and guides (normal, linear depth) into RGBA32F images.
void capturePass(FeatureContext& ctx, rhi::PipelineHandle pipe, rhi::RGTexture color, rhi::TextureHandle outColor,
                 rhi::TextureHandle outGeo, bool exposed) {
    const rhi::RGTexture depth = ctx.resources().texture(res::kDepth), normals = ctx.resources().texture(res::kNormals);
    const rhi::RGTexture oc = ctx.graph().importTexture(ctx.device(), outColor);
    const rhi::RGTexture og = ctx.graph().importTexture(ctx.device(), outGeo);
    const Extent2D e = ctx.renderExtent();
    const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
    ctx.graph()
        .addPass("Test.NeuralCapture", rhi::PassType::Compute)
        .read(color, rhi::Access::SampledCompute)
        .read(depth, rhi::Access::SampledCompute)
        .read(normals, rhi::Access::SampledCompute)
        .overwrite(oc, rhi::Access::StorageWriteCompute)
        .overwrite(og, rhi::Access::StorageWriteCompute)
        .execute([=](rhi::PassContext& p) {
            struct {
                u64 view, scene;
                u32 color, depth, normals, outColor, outGeo, width, height, exposed;
            } pc{view, scene, p.sampledIndex(color), p.sampledIndex(depth), p.sampledIndex(normals), p.storageIndex(oc),
                 p.storageIndex(og), e.width, e.height, exposed ? 1u : 0u};
            p.cmd.bindPipeline(pipe);
            p.cmd.pushConstants(&pc, 48);
            p.cmd.dispatch((e.width + 7) / 8, (e.height + 7) / 8);
        });
}

// Path traced HDR colour + guides after the path tracer (BeforePostProcess -10000).
struct RadianceCapture final : IRenderFeature {
    rhi::PipelineHandle pipe;
    rhi::TextureHandle color, geo;
    Extent2D extent;
    std::string_view name() const override { return "TestNeuralRadianceCapture"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::BeforePostProcess); }
    i32 order() const override { return 20000; }
    bool initialize(FeatureInitContext& ctx) override {
        pipe = createComputePipeline(ctx.device, "test.neural.capture", "render/neural/denoise_capture.comp");
        return true;
    }
    void shutdown(rhi::Device& d) override {
        d.destroy(pipe);
        for (rhi::TextureHandle t : {color, geo})
            if (t) d.destroy(t);
    }
    void setup(FeatureContext& ctx) override {
        const Extent2D e = ctx.renderExtent();
        if (!(extent == e)) {
            for (rhi::TextureHandle* t : {&color, &geo})
                if (*t) ctx.device().destroy(*t);
            color = makeTarget(ctx.device(), e, VK_FORMAT_R32G32B32A32_SFLOAT, "test.capture.color");
            geo = makeTarget(ctx.device(), e, VK_FORMAT_R32G32B32A32_SFLOAT, "test.capture.geo");
            extent = e;
        }
        capturePass(ctx, pipe, ctx.resources().texture(res::kSceneColorHDR), color, geo, true);
    }
};

// Stochastic AO (render/raytracing/denoise_test_noise.comp, no ray tracing needed): 1-spp signal + 256-spp
// reference, denoised by an rt::SvgfDenoiser (kernel chosen by r.Denoiser), everything copied for readback.
struct AoProbe final : IRenderFeature {
    rt::SvgfDenoiser denoiser;
    rhi::PipelineHandle noise, pipe;
    rhi::TextureHandle noisy, geo, denoised, reference, refGeo;
    Extent2D extent;
    u32 seed = 1;
    u32 noisySamples = 1; // samples of the noisy signal (training also uses 8: inputs after temporal accumulation)
    std::string_view name() const override { return "TestNeuralAoProbe"; }
    InjectionMask injectionPoints() const override { return maskOf(InjectionPoint::Lighting); }
    bool initialize(FeatureInitContext& ctx) override {
        noise = createComputePipeline(ctx.device, "test.neural.noise", "render/raytracing/denoise_test_noise.comp");
        pipe = createComputePipeline(ctx.device, "test.neural.capture", "render/neural/denoise_capture.comp");
        return denoiser.initialize(ctx.device);
    }
    void shutdown(rhi::Device& d) override {
        denoiser.shutdown(d);
        d.destroy(noise);
        d.destroy(pipe);
        for (rhi::TextureHandle t : {noisy, geo, denoised, reference, refGeo})
            if (t) d.destroy(t);
    }
    rhi::RGTexture aoPass(FeatureContext& ctx, u32 samples, u32 passSeed, const char* name) {
        const Extent2D e = ctx.renderExtent();
        rhi::TextureDesc td;
        td.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        td.width = e.width;
        td.height = e.height;
        td.usage = rhi::TextureUsage::None;
        td.name = name;
        const rhi::RGTexture out = ctx.graph().createTexture(td);
        const rhi::RGTexture depth = ctx.resources().texture(res::kDepth), normals = ctx.resources().texture(res::kNormals);
        const VkDeviceAddress view = ctx.viewAddress(), scene = ctx.sceneAddress();
        const rhi::PipelineHandle p = noise;
        ctx.graph()
            .addPass(name, rhi::PassType::Compute)
            .read(depth, rhi::Access::SampledCompute)
            .read(normals, rhi::Access::SampledCompute)
            .overwrite(out, rhi::Access::StorageWriteCompute)
            .execute([=](rhi::PassContext& pc) {
                struct {
                    u64 view, scene;
                    u32 depth, normals, out, width, height, scale, samples, seed;
                    f32 radius;
                } push{view, scene, pc.sampledIndex(depth), pc.sampledIndex(normals), pc.storageIndex(out), e.width, e.height,
                       1, samples, passSeed, 1.0f};
                pc.cmd.bindPipeline(p);
                pc.cmd.pushConstants(push);
                pc.cmd.dispatch((e.width + 7) / 8, (e.height + 7) / 8);
            });
        return out;
    }
    void setup(FeatureContext& ctx) override {
        rhi::Device& dev = ctx.device();
        const Extent2D e = ctx.renderExtent();
        if (!(extent == e)) {
            for (rhi::TextureHandle* t : {&noisy, &geo, &denoised, &reference, &refGeo})
                if (*t) dev.destroy(*t);
            noisy = makeTarget(dev, e, VK_FORMAT_R32G32B32A32_SFLOAT, "test.ao.noisy");
            geo = makeTarget(dev, e, VK_FORMAT_R32G32B32A32_SFLOAT, "test.ao.geo");
            denoised = makeTarget(dev, e, VK_FORMAT_R32G32B32A32_SFLOAT, "test.ao.denoised");
            reference = makeTarget(dev, e, VK_FORMAT_R32G32B32A32_SFLOAT, "test.ao.reference");
            refGeo = makeTarget(dev, e, VK_FORMAT_R32G32B32A32_SFLOAT, "test.ao.refgeo");
            extent = e;
        }
        const rhi::RGTexture signal = aoPass(ctx, noisySamples, seed++, "Test.NoisyAO");
        const rhi::RGTexture ref = aoPass(ctx, 256, 777, "Test.ReferenceAO");
        rt::DenoiserInputs in;
        in.signal = signal;
        in.depth = ctx.resources().texture(res::kDepth);
        in.normals = ctx.resources().texture(res::kNormals);
        in.velocity = ctx.resources().texture(res::kVelocity);
        in.signalExtent = e;
        in.renderExtent = e;
        rt::DenoiserSettings st;
        st.lumaWeights = {1, 0, 0, 0};
        const rt::DenoiserOutputs out = denoiser.denoise(ctx, "TestNeuralAO", in, st);
        capturePass(ctx, pipe, signal, noisy, geo, false);
        capturePass(ctx, pipe, out.result, denoised, geo, false);
        capturePass(ctx, pipe, ref, reference, refGeo, false);
    }
};

// ---- CPU model of the À-trous stage (SVGF replica and the neural kernel) -------------------------------------------

struct Frame {
    u32 w = 0, h = 0;
    std::vector<glm::vec3> noisy, ref;
    std::vector<glm::vec4> geo; // normal, linear depth
    glm::vec3 luma{0.2126f, 0.7152f, 0.0722f};
};

f32 lum(const Frame& f, glm::vec3 c) { return glm::dot(c, f.luma); }

std::vector<f32> spatialVariance(const Frame& f, const std::vector<glm::vec3>& c) {
    std::vector<f32> v(c.size(), 0.0f);
    for (u32 y = 0; y < f.h; ++y)
        for (u32 x = 0; x < f.w; ++x) {
            f64 s = 0, s2 = 0;
            u32 n = 0;
            for (i32 dy = -3; dy <= 3; ++dy)
                for (i32 dx = -3; dx <= 3; ++dx) {
                    const i32 qx = i32(x) + dx, qy = i32(y) + dy;
                    if (qx < 0 || qy < 0 || qx >= i32(f.w) || qy >= i32(f.h)) continue;
                    if (f.geo[usize(qy) * f.w + qx].w <= 0.0f) continue;
                    const f64 l = lum(f, c[usize(qy) * f.w + qx]);
                    s += l, s2 += l * l, ++n;
                }
            if (n > 1) v[usize(y) * f.w + x] = f32(std::max(0.0, s2 / n - (s / n) * (s / n)));
        }
    return v;
}

struct Tap {
    u32 index;
    f32 k;
    std::array<f32, kIn> features;
    f32 svgfWeight;
    bool center;
};

// Taps of pixel p at iteration `it` (step 2^it) with the features of denoise_neural.comp and the SVGF weight of
// denoise_atrous.comp (phiColor 4, phiNormal 128, phiDepth 1).
void gatherTaps(const Frame& f, const std::vector<glm::vec3>& c, const std::vector<f32>& var, u32 x, u32 y, u32 it,
                std::vector<Tap>& taps) {
    taps.clear();
    const usize p = usize(y) * f.w + x;
    const f32 z0 = f.geo[p].w;
    if (z0 <= 0.0f) return;
    const glm::vec3 n0 = glm::vec3(f.geo[p]);
    auto at = [&](i32 qx, i32 qy) { return usize(std::clamp(qy, 0, i32(f.h) - 1)) * f.w + usize(std::clamp(qx, 0, i32(f.w) - 1)); };
    const f32 zx = f.geo[at(i32(x) + 1, i32(y))].w, zy = f.geo[at(i32(x), i32(y) + 1)].w;
    f32 grad = std::max(std::abs(zx - z0), std::abs(zy - z0));
    grad = (zx > 0 && zy > 0) ? grad : z0 * 0.01f;
    f32 bv = 0.0f;
    for (i32 dy = -1; dy <= 1; ++dy)
        for (i32 dx = -1; dx <= 1; ++dx) bv += var[at(i32(x) + dx, i32(y) + dy)] * (dx == 0 ? 0.5f : 0.25f) * (dy == 0 ? 0.5f : 0.25f);
    const f32 sigma = std::sqrt(std::max(bv, 0.0f));
    const f32 l0 = lum(f, c[p]);
    const f32 relNoise = std::log(1.0f + sigma / (std::abs(l0) + 1e-3f));
    const i32 step = 1 << it;
    for (i32 dy = -2; dy <= 2; ++dy)
        for (i32 dx = -2; dx <= 2; ++dx) {
            const i32 qx = i32(x) + dx * step, qy = i32(y) + dy * step;
            if (qx < 0 || qy < 0 || qx >= i32(f.w) || qy >= i32(f.h)) continue;
            const usize q = usize(qy) * f.w + qx;
            Tap t{u32(q), kB3[std::abs(dx)] * kB3[std::abs(dy)], {}, 0.0f, dx == 0 && dy == 0};
            if (!t.center) {
                const f32 z = f.geo[q].w;
                if (z <= 0.0f) continue;
                const glm::vec3 n = glm::vec3(f.geo[q]);
                const f32 dist = std::sqrt(f32(dx * dx + dy * dy));
                const f32 dl = std::abs(lum(f, c[q]) - l0);
                t.features = {std::log(1.0f + std::abs(z - z0) / (grad * dist * f32(step) + z0 * 1e-3f)),
                              1.0f - std::max(glm::dot(n0, n), 0.0f), std::log(1.0f + dl / (sigma + 1e-4f)), dist * 0.5f,
                              relNoise, std::log(1.0f + dl / (std::abs(l0) + 1e-3f))};
                const f32 wz = std::abs(z - z0) / (1.0f * grad * dist * f32(step) + z0 * 1e-3f);
                const f32 wn = std::pow(std::max(glm::dot(n0, n), 0.0f), 128.0f);
                const f32 wl = dl / (4.0f * sigma + 1e-4f);
                t.svgfWeight = t.k * wn * std::exp(-wz - wl);
            } else {
                t.svgfWeight = t.k;
            }
            taps.push_back(t);
        }
}

// One À-trous iteration over the frame (mlp == nullptr: SVGF weights).
void filter(const Frame& f, const std::vector<glm::vec3>& c, const std::vector<f32>& var, u32 it, const nn::Mlp* mlp,
            std::vector<glm::vec3>& outC, std::vector<f32>& outV) {
    outC = c;
    outV = var;
    nn::WorkerPool pool;
    pool.run(f.h, [&](u32 y) {
        std::vector<Tap> taps;
        std::vector<f32> scratch(mlp ? mlp->scratchSize() : 0);
        for (u32 x = 0; x < f.w; ++x) {
            gatherTaps(f, c, var, x, y, it, taps);
            if (taps.empty()) continue;
            glm::vec3 sc(0.0f);
            f32 sv = 0.0f, sw = 0.0f;
            for (const Tap& t : taps) {
                f32 w = t.svgfWeight;
                if (mlp && !t.center) {
                    f32 logit = 0.0f;
                    mlp->forward(t.features.data(), &logit, scratch.data());
                    w = t.k * std::exp(std::clamp(logit, -30.0f, 4.0f));
                }
                sc += c[t.index] * w;
                sv += var[t.index] * w * w;
                sw += w;
            }
            const usize p = usize(y) * f.w + x;
            if (sw > 1e-8f) outC[p] = sc / sw, outV[p] = sv / (sw * sw);
        }
    });
}

std::vector<glm::vec3> denoise(const Frame& f, const std::vector<nn::Mlp>* mlps) {
    std::vector<glm::vec3> c = f.noisy, nc;
    std::vector<f32> v = spatialVariance(f, c), nv;
    for (u32 it = 0; it < kIterations; ++it) {
        filter(f, c, v, it, mlps ? &(*mlps)[it] : nullptr, nc, nv);
        c.swap(nc);
        v.swap(nv);
    }
    return c;
}

// PSNR on tonemapped values (x / (1 + x) after scaling the reference's mean luminance to 0.18; the path traced
// frames are in physical units, where an unscaled x / (1 + x) saturates and every denoiser scores ~75 dB),
// geometry pixels only.
f64 psnr(const Frame& f, const std::vector<glm::vec3>& a) {
    f64 meanLum = 0;
    u64 count = 0;
    for (usize i = 0; i < a.size(); ++i)
        if (f.geo[i].w > 0.0f) meanLum += std::max(lum(f, f.ref[i]), 0.0f), ++count;
    meanLum /= f64(std::max<u64>(count, 1));
    const f64 key = meanLum > 1e-9 ? 0.18 / meanLum : 1.0;
    f64 mse = 0;
    u64 n = 0;
    for (usize i = 0; i < a.size(); ++i) {
        if (f.geo[i].w <= 0.0f) continue;
        for (int ch = 0; ch < 3; ++ch) {
            const f64 ax = std::max(f64(a[i][ch]), 0.0) * key, ry = std::max(f64(f.ref[i][ch]), 0.0) * key;
            const f64 x = ax / (1.0 + ax), y = ry / (1.0 + ry);
            mse += (x - y) * (x - y);
            ++n;
        }
    }
    mse /= f64(std::max<u64>(n, 1));
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}

// Greedy stage-wise training: iteration `it`'s MLP is trained on the output of the trained iterations before it.
std::vector<nn::Mlp> train(std::vector<Frame>& frames, u32 steps, u64 seed) {
    std::vector<nn::Mlp> mlps;
    std::vector<std::vector<glm::vec3>> cur(frames.size());
    std::vector<std::vector<f32>> vars(frames.size());
    for (usize i = 0; i < frames.size(); ++i) cur[i] = frames[i].noisy, vars[i] = spatialVariance(frames[i], cur[i]);
    nn::WorkerPool pool;
    for (u32 it = 0; it < kIterations; ++it) {
        nn::Mlp net = nn::Mlp::make(kIn, {kHidden, kHidden}, 1);
        net.initialize(seed + it);
        // Start close to the SVGF edge stop: a negative bias on the output keeps early weights moderate.
        nn::Adam adam;
        adam.reset(net.paramCount());
        nn::Pcg32 rng(seed * 31 + it);
        constexpr u32 kChunks = 32, kPerChunk = 32;
        std::vector<std::vector<f32>> grads(kChunks, std::vector<f32>(net.paramCount()));
        std::vector<f32> losses(kChunks), total(net.paramCount());
        f32 lastLoss = 0.0f;
        for (u32 step = 0; step < steps; ++step) {
            std::vector<std::pair<u32, u32>> picks(kChunks * kPerChunk);
            for (auto& [fi, pi] : picks) {
                do {
                    fi = rng.below(u32(frames.size()));
                    pi = rng.below(frames[fi].w * frames[fi].h);
                } while (frames[fi].geo[pi].w <= 0.0f);
            }
            pool.run(kChunks, [&](u32 ch) {
                std::fill(grads[ch].begin(), grads[ch].end(), 0.0f);
                losses[ch] = 0.0f;
                std::vector<Tap> taps;
                nn::Mlp::Tape tape;
                std::vector<f32> in, dOut;
                for (u32 s = 0; s < kPerChunk; ++s) {
                    const auto [fi, pi] = picks[ch * kPerChunk + s];
                    const Frame& f = frames[fi];
                    gatherTaps(f, cur[fi], vars[fi], pi % f.w, pi / f.w, it, taps);
                    std::vector<u32> nonCenter;
                    in.clear();
                    for (u32 t = 0; t < taps.size(); ++t)
                        if (!taps[t].center) nonCenter.push_back(t), in.insert(in.end(), taps[t].features.begin(), taps[t].features.end());
                    if (nonCenter.empty()) continue;
                    net.forwardTrain(in.data(), u32(nonCenter.size()), tape);
                    const f32* logits = net.tapeOutputs(tape);
                    std::vector<f32> w(taps.size());
                    std::vector<bool> clamped(taps.size(), false);
                    for (u32 t = 0; t < taps.size(); ++t) w[t] = taps[t].k;
                    for (u32 j = 0; j < nonCenter.size(); ++j) {
                        const f32 l = logits[j];
                        clamped[nonCenter[j]] = l < -30.0f || l > 4.0f;
                        w[nonCenter[j]] = taps[nonCenter[j]].k * std::exp(std::clamp(l, -30.0f, 4.0f));
                    }
                    glm::vec3 sc(0.0f);
                    f32 sw = 0.0f;
                    for (u32 t = 0; t < taps.size(); ++t) sc += cur[fi][taps[t].index] * w[t], sw += w[t];
                    const glm::vec3 out = sc / sw;
                    const glm::vec3 ref = f.ref[pi];
                    const f32 denom = lum(f, ref) * lum(f, ref) + 1e-2f;
                    const glm::vec3 d = out - ref;
                    losses[ch] += glm::dot(d, d) / denom;
                    const glm::vec3 dLdOut = 2.0f * d / denom / f32(kChunks * kPerChunk);
                    dOut.assign(nonCenter.size(), 0.0f);
                    for (u32 j = 0; j < nonCenter.size(); ++j) {
                        const u32 t = nonCenter[j];
                        if (clamped[t]) continue;
                        const f32 dLdw = glm::dot(dLdOut, cur[fi][taps[t].index] - out) / sw;
                        dOut[j] = dLdw * w[t];
                    }
                    net.backward(tape, dOut.data(), grads[ch].data());
                }
            });
            std::fill(total.begin(), total.end(), 0.0f);
            f32 loss = 0.0f;
            for (u32 ch = 0; ch < kChunks; ++ch) {
                for (usize i = 0; i < total.size(); ++i) total[i] += grads[ch][i];
                loss += losses[ch];
            }
            lastLoss = loss / f32(kChunks * kPerChunk);
            const f32 lr = 3e-3f * (0.05f + 0.95f * 0.5f * (1.0f + std::cos(3.14159265f * f32(step) / f32(steps))));
            adam.step(net.params(), total, lr);
            net.syncTransposed();
            if (step % 500 == 0) std::printf("    iteration %u step %u loss %.5f\n", it, step, lastLoss);
        }
        std::printf("  iteration %u trained, loss %.5f\n", it, lastLoss);
        for (usize i = 0; i < frames.size(); ++i) {
            std::vector<glm::vec3> nc;
            std::vector<f32> nv;
            filter(frames[i], cur[i], vars[i], it, &net, nc, nv);
            cur[i].swap(nc);
            vars[i].swap(nv);
        }
        mlps.push_back(std::move(net));
    }
    return mlps;
}

void writeWeights(const fs::path& file, const std::vector<nn::Mlp>& mlps, const std::string& provenance) {
    std::string s;
    s += "// Neural denoiser weights (render/neural/denoise_neural.comp). GENERATED by NeuralDenoiserTest.TrainWeights\n";
    s += "// (engine/render/tests/gpu/neural_denoiser_tests.cpp); do not edit. Per iteration (step 1, 2, 4, 8) one MLP\n";
    s += "// OX_ND_IN -> OX_ND_HIDDEN -> OX_ND_HIDDEN -> 1 in ox::nn::Mlp layout (W row-major, then b, per layer).\n";
    s += "// " + provenance + "\n";
    s += "#ifndef OX_NEURAL_DENOISER_WEIGHTS_GLSL\n#define OX_NEURAL_DENOISER_WEIGHTS_GLSL\n";
    s += std::format("#define OX_ND_IN {}\n#define OX_ND_HIDDEN {}\n#define OX_ND_ITERATIONS {}\n#define OX_ND_PARAMS {}\n", kIn,
                     kHidden, kIterations, mlps[0].paramCount());
    s += std::format("const float kNdWeights[{}] = float[](\n", mlps[0].paramCount() * mlps.size());
    usize k = 0;
    const usize total = mlps[0].paramCount() * mlps.size();
    for (const nn::Mlp& m : mlps)
        for (f32 v : m.params()) {
            s += std::format("{:.7g}{}", v, ++k == total ? "" : ",");
            s += (k % 8 == 0 || k == total) ? "\n" : " ";
        }
    s += ");\n#endif\n";
    std::ofstream(file, std::ios::binary) << s;
}

class NeuralDenoiserTest : public RenderTest {
protected:
    void scene(int variant) {
        const Uuid grey = material({0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.6f);
        const Uuid red = material({0.8f, 0.15f, 0.1f, 1.0f}, 0.0f, 0.5f);
        const Uuid blue = material({0.1f, 0.25f, 0.8f, 1.0f}, 0.0f, 0.3f);
        const Uuid metal = material({0.9f, 0.8f, 0.6f, 1.0f}, 1.0f, 0.35f);
        mesh(Primitive::Plane, grey, {0, 0, 0}, glm::vec3(30.0f));
        mesh(Primitive::Cube, variant % 2 ? red : grey, {-0.8f, 0.5f, 0.0f});
        mesh(Primitive::Cube, blue, {0.9f, 0.35f, -0.6f}, glm::vec3(0.7f));
        mesh(Primitive::Sphere, variant % 3 == 0 ? metal : grey, {0.2f, 0.4f, 1.0f}, glm::vec3(0.8f));
        mesh(Primitive::Cylinder, red, {1.4f, 0.75f, 0.8f}, glm::vec3(0.4f, 1.5f, 0.4f));
        mesh(Primitive::Torus, variant % 2 ? blue : metal, {-1.6f, 0.3f, 1.3f}, glm::vec3(0.6f));
        mesh(Primitive::Cube, grey, {0.0f, 1.25f, -2.5f}, glm::vec3(5.0f, 2.5f, 0.3f));
        const f32 a = 0.6f + 0.7f * f32(variant);
        sun(glm::normalize(glm::vec3(std::cos(a), -0.8f, std::sin(a))), 20000.0f);
        if (variant >= 2) pointLight({0.5f, 1.2f, 1.6f}, 4000.0f, 8.0f, {1.0f, 0.7f, 0.4f}, true);
        environment(0.6f, 1.0f);
    }
    static CameraParams view(int i) {
        const glm::vec3 eyes[5] = {{0.3f, 2.6f, 4.2f}, {-2.5f, 1.6f, 3.4f}, {2.8f, 1.2f, 3.0f}, {0.0f, 4.0f, 2.0f}, {-1.0f, 1.0f, 4.5f}};
        return camera(eyes[i % 5], {0.1f, 0.4f, 0.0f}, 12.0f, 55.0f);
    }
};

} // namespace

TEST_F(NeuralDenoiserTest, TrainWeights) {
    const char* env = std::getenv("OX_TRAIN_NEURAL_DENOISER");
    if (!env) GTEST_SKIP() << "tool: OX_TRAIN_NEURAL_DENOISER=1 renders training pairs and rewrites denoiser_weights.glsl";
    if (!device->caps().rayTracingSupported()) GTEST_SKIP() << "the path tracer needs ray tracing";
    constexpr u32 kW = 256, kH = 192;
    std::vector<Frame> train, test;
    Stopwatch sw(true);
    auto& cap = renderer->features().emplace<RadianceCapture>();
    for (int variant = 0; variant < 5; ++variant) {
        world = std::make_unique<World>();
        scene(variant);
        world->updateTransforms();
        CVarScope rt("r.RayTracing", "true");
        CVarScope pt("r.PathTracing", "true");
        const CameraParams cam = view(variant);
        Frame f;
        f.w = kW, f.h = kH;
        {
            CVarScope spf("r.PathTracing.SamplesPerFrame", "16");
            render(cam, {.width = kW, .height = kH, .frames = 96}); // 1536 spp
        }
        for (const glm::vec4& v : readRgba(*device, cap.color)) f.ref.push_back(glm::vec3(v));
        f.geo = readRgba(*device, cap.geo);
        // Noisy inputs at 1 / 2 / 4 spp (the denoiser sees short histories after disocclusion) and 16 spp (the
        // À-trous input after temporal accumulation is much less noisy: the kernel has to learn to blur less).
        for (u32 spp : {1u, 2u, 4u, 16u}) {
            render(view(variant + 1), {.width = kW, .height = kH, .frames = 1}); // camera change resets accumulation
            render(cam, {.width = kW, .height = kH, .frames = spp});
            Frame g = f;
            g.noisy.clear();
            for (const glm::vec4& v : readRgba(*device, cap.color)) g.noisy.push_back(glm::vec3(v));
            (variant == 4 ? test : train).push_back(std::move(g));
        }
    }
    renderer->features().remove("TestNeuralRadianceCapture", device.get());
    // Synthetic 1-spp AO with its 256-spp reference (the RT shadow / AO signals look like this).
    {
        world = std::make_unique<World>();
        scene(0);
        world->updateTransforms();
        auto& probe = renderer->features().emplace<AoProbe>();
        for (int v = 0; v < 6; ++v) {
            probe.noisySamples = v < 3 ? 1u : 8u;
            render(view(v % 3), {.width = kW, .height = kH, .frames = 1});
            Frame f;
            f.w = kW, f.h = kH;
            f.luma = {1, 0, 0};
            const auto n = readRgba(*device, probe.noisy), r = readRgba(*device, probe.reference);
            f.geo = readRgba(*device, probe.geo);
            for (usize i = 0; i < n.size(); ++i) f.noisy.push_back(glm::vec3(n[i])), f.ref.push_back(glm::vec3(r[i]));
            train.push_back(std::move(f));
        }
    }
    std::printf("  data: %zu training / %zu test frames (%.1f s)\n", train.size(), test.size(), sw.elapsedMs() / 1000.0);
    const std::vector<nn::Mlp> mlps = ::train(train, 3000, 7);
    f64 nNoisy = 0, nSvgf = 0, nNeural = 0;
    for (const Frame& f : test) {
        const f64 a = psnr(f, f.noisy), b = psnr(f, denoise(f, nullptr)), c = psnr(f, denoise(f, &mlps));
        std::printf("  held-out frame: noisy %.2f dB, SVGF (spatial) %.2f dB, neural %.2f dB\n", a, b, c);
        nNoisy += a, nSvgf += b, nNeural += c;
    }
    const f64 n = f64(std::max<usize>(test.size(), 1));
    std::printf("  held-out mean: noisy %.2f dB, SVGF %.2f dB, neural %.2f dB\n", nNoisy / n, nSvgf / n, nNeural / n);
    const fs::path out = fs::path(OX_RENDER_SHADER_DIR) / "render/neural/denoiser_weights.glsl";
    writeWeights(out,
                 mlps,
                 std::format("Trained on {} path traced / AO frames ({}x{}); held-out path traced PSNR: noisy {:.2f} dB, "
                             "SVGF {:.2f} dB, neural {:.2f} dB.",
                             train.size(), kW, kH, nNoisy / n, nSvgf / n, nNeural / n));
    std::printf("  wrote %s\n", out.string().c_str());
    EXPECT_GT(nNeural, nNoisy);
}

TEST_F(NeuralDenoiserTest, ComparedToSvgf) {
    scene(1);
    world->updateTransforms();
    auto& probe = renderer->features().emplace<AoProbe>();
    const CameraParams cam = view(0);
    auto rmse = [&](rhi::TextureHandle t) {
        const auto a = readRgba(*device, t), r = readRgba(*device, probe.reference), g = readRgba(*device, probe.geo);
        f64 s = 0;
        u64 n = 0;
        for (usize i = 0; i < a.size(); ++i) {
            if (g[i].w <= 0.0f) continue;
            const f64 d = a[i].r - r[i].r;
            s += d * d;
            ++n;
        }
        return std::sqrt(s / f64(std::max<u64>(n, 1)));
    };
    struct Result {
        f64 noisy, first, converged;
    };
    auto measure = [&](const char* denoiser) {
        CVarScope d("r.Denoiser", denoiser);
        render(view(3), {.width = 192, .height = 192, .frames = 1}); // fresh history
        render(cam, {.width = 192, .height = 192, .frames = 1});
        Result r{rmse(probe.noisy), rmse(probe.denoised), 0};
        render(cam, {.width = 192, .height = 192, .frames = 24});
        r.converged = rmse(probe.denoised);
        return r;
    };
    const Result svgf = measure("SVGF"), neural = measure("Neural");
    auto db = [](f64 e) { return e > 0 ? 20.0 * std::log10(1.0 / e) : 99.0; };
    std::printf("  AO, RMSE vs 256-spp reference (PSNR): noisy %.4f (%.1f dB)\n", svgf.noisy, db(svgf.noisy));
    std::printf("  SVGF   frame 1 %.4f (%.1f dB), frame 25 %.4f (%.1f dB)\n", svgf.first, db(svgf.first), svgf.converged,
                db(svgf.converged));
    std::printf("  Neural frame 1 %.4f (%.1f dB), frame 25 %.4f (%.1f dB)\n", neural.first, db(neural.first),
                neural.converged, db(neural.converged));
    EXPECT_LT(neural.first, svgf.noisy * 0.6) << "the learned kernel removes noise";
    EXPECT_LT(neural.converged, 0.08);

    // 1080p cost of each kernel (same temporal / variance passes).
    for (const char* d : {"SVGF", "Neural"}) {
        CVarScope den("r.Denoiser", d);
        render(cam, {.width = 1920, .height = 1080, .frames = 6});
        f64 atrous = 0, total = 0;
        for (const PassTiming& t : renderer->stats().passes) {
            if (t.name.find("TestNeuralAO.") == std::string::npos) continue;
            total += t.gpuMs;
            if (t.name.find("Atrous") != std::string::npos) atrous += t.gpuMs;
        }
        std::printf("  %-6s 1080p: denoiser %.3f ms (A-trous %.3f ms, 4 iterations)\n", d, total, atrous);
    }
}
