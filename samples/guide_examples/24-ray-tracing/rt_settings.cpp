// Глава 24: режим RTX — CPU-часть (docs/guide/24-ray-tracing.md). Доступность и причины для UI, гейтинг
// (r.RayTracing + DeviceCaps + cvar эффекта), снапшот RtSettings и уровни Scalability::RayTracing, политика LOD для
// BLAS, маски инстансов, сброс накопления path tracer'а. RT-железо не нужно.
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/render/features/raytracing/rt_scene.hpp>
#include <oxwald/render/render.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>

using namespace ox;
using namespace ox::render;

namespace {

rhi::DeviceCaps appleCaps() {
    rhi::DeviceCaps c;
    c.gpuName = "Apple M4 Pro";
    c.vendor = rhi::GpuVendor::Apple;
    c.portabilitySubset = true; // MoltenVK
    return c;
}

rhi::DeviceCaps rtxCaps() {
    rhi::DeviceCaps c;
    c.gpuName = "NVIDIA GeForce RTX 4070";
    c.vendor = rhi::GpuVendor::Nvidia;
    c.accelerationStructure = true;
    c.rayQuery = true;
    c.rayTracingPipeline = true;
    return c;
}

std::set<std::string> resolvedNames(const FeatureRegistry& reg, const RenderSettings& s, const rhi::DeviceCaps& caps) {
    std::set<std::string> out;
    for (IRenderFeature* f : reg.resolve(s, caps)) out.emplace(f->name());
    return out;
}

void setCVar(const char* name, const char* value) { CVarRegistry::instance().set(name, value, CVarSource::Code); }

} // namespace

// --- Шаг 2. Доступность и причины для подсказки у выключенной галочки ---

TEST(GuideRayTracing, StatusAndReasonsForUi) {
    const rt::RtStatus mac = rt::rayTracingStatus(appleCaps());
    EXPECT_FALSE(mac.available);
    // "Apple M4 Pro does not expose VK_KHR_acceleration_structure, VK_KHR_ray_query (MoltenVK translates Vulkan to
    //  Metal and does not implement Vulkan ray tracing yet)"
    EXPECT_NE(mac.reason.find("MoltenVK"), std::string::npos) << mac.reason;
    ASSERT_EQ(mac.effects.size(), 9u); // 8 эффектов + path tracer в режиме RT pipeline
    for (const rt::RtEffectStatus& e : mac.effects) {
        EXPECT_FALSE(e.available) << e.name;
        EXPECT_EQ(e.reason, mac.reason) << e.name;
    }

    rhi::DeviceCaps rtx = rtxCaps();
    rtx.rayTracingPipeline = false; // есть ray query, нет RT pipeline (часть мобильных/старых драйверов)
    const rt::RtStatus partial = rt::rayTracingStatus(rtx);
    EXPECT_TRUE(partial.available);
    EXPECT_TRUE(partial.reason.empty());
    const rt::RtEffectStatus& pipeline = partial.effects.back();
    EXPECT_EQ(pipeline.cvar, "r.PathTracing.Mode");
    EXPECT_FALSE(pipeline.available);
    EXPECT_NE(pipeline.reason.find("ray query mode still works"), std::string::npos);

    // Те же причины даёт сам DeviceCaps (глава 17).
    EXPECT_EQ(appleCaps().whyRayTracingUnavailable(), mac.reason);
}

// --- Шаг 2. Гейтинг: галочка × железо × эффект ---

TEST(GuideRayTracing, GatingNeedsCheckboxAndHardware) {
    RenderSettings on;
    on.rayTracing = true;   // r.RayTracing
    RenderSettings off;
    EXPECT_TRUE(rt::rayTracingActive(on, rtxCaps()));
    EXPECT_FALSE(rt::rayTracingActive(on, appleCaps())) << "на Mac галочка ничего не меняет";
    EXPECT_FALSE(rt::rayTracingActive(off, rtxCaps()));

    // RT-варианты сидят в эксклюзивных группах растровых двойников с приоритетом 100.
    FeatureRegistry reg;
    registerRayTracingFeatures(reg);
    for (const char* n : {"r.RayTracing.Shadows", "r.RayTracing.Reflections", "r.RayTracing.AO", "r.RayTracing.GI",
                          "r.RayTracing.Translucency", "r.RayTracing.Volumetrics"}) {
        setCVar(n, "true");
    }
    setCVar("r.PathTracing", "false");

    EXPECT_TRUE(resolvedNames(reg, on, appleCaps()).empty()) << "без RT-железа ни одна RT-фича не включается";
    std::set<std::string> names = resolvedNames(reg, on, rtxCaps());
    for (const char* n : {"RayTracingScene", "ShadowsRT", "ReflectionsRT", "AmbientOcclusionRT", "GlobalIlluminationRT",
                          "TranslucencyRT"}) {
        EXPECT_TRUE(names.count(n)) << n;
    }
    EXPECT_FALSE(names.count("PathTracer"));

    // Отдельный эффект выключается своим cvar'ом — остаётся растровый вариант.
    setCVar("r.RayTracing.AO", "false");
    names = resolvedNames(reg, on, rtxCaps());
    EXPECT_FALSE(names.count("AmbientOcclusionRT"));
    EXPECT_TRUE(names.count("ShadowsRT"));
    setCVar("r.RayTracing.AO", "true");

    // Path tracer: r.PathTracing 1 (нужна та же галочка r.RayTracing).
    setCVar("r.PathTracing", "true");
    EXPECT_TRUE(resolvedNames(reg, on, rtxCaps()).count("PathTracer"));
    setCVar("r.PathTracing", "false");

    EXPECT_TRUE(rt::rayTracedRefractionActive(on, rtxCaps()));
    EXPECT_FALSE(rt::rayTracedRefractionActive(on, appleCaps()));
}

// --- Шаг 4. Настройки эффектов и Scalability::RayTracing ---

TEST(GuideRayTracing, SettingsAndScalability) {
    rt::registerRayTracingCVars();
    scalability::setGroup(Scalability::RayTracing, QualityLevel::Low);
    const rt::RtSettings low = rt::RtSettings::fromCVars();
    EXPECT_TRUE(low.shadows);
    EXPECT_FALSE(low.reflections);
    EXPECT_FALSE(low.ao);
    EXPECT_FALSE(low.gi);
    EXPECT_EQ(low.shadowResolutionScale, 50);        // половинное разрешение + апсемпл
    EXPECT_FLOAT_EQ(low.reflectionMaxRoughness, 0.3f);
    EXPECT_EQ(low.denoiserIterations, 3);

    scalability::setGroup(Scalability::RayTracing, QualityLevel::Ultra);
    const rt::RtSettings ultra = rt::RtSettings::fromCVars();
    EXPECT_TRUE(ultra.gi && ultra.reflections && ultra.ao && ultra.translucency && ultra.volumetrics);
    EXPECT_TRUE(ultra.shadowReSTIR);
    EXPECT_EQ(ultra.shadowSpp, 2);
    EXPECT_EQ(ultra.giProbesXZ, 32);
    EXPECT_EQ(ultra.giProbesY, 12);
    EXPECT_EQ(ultra.translucencyMaxBounces, 6);
    EXPECT_EQ(ultra.blasLod, 0);                     // Ultra трассирует LOD 0

    scalability::setGroup(Scalability::RayTracing, QualityLevel::High);
    const rt::RtSettings high = rt::RtSettings::fromCVars();
    EXPECT_EQ(high.giRaysPerProbe, 128);
    EXPECT_EQ(high.blasLod, -1);                     // самый грубый LOD
    EXPECT_FALSE(high.pathTracing);
    EXPECT_EQ(high.pathMaxSamples, 4096);

    // Все cvar'ы области — для группировки в UI настроек.
    const std::vector<std::string> names = rt::rayTracingCVarNames();
    EXPECT_NE(std::find(names.begin(), names.end(), "r.RayTracing.Shadows.ReSTIR"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "r.PathTracing.MaxBounces"), names.end());
}

// --- Шаг 3. Сцена ускоряющих структур: LOD для BLAS, маски, накопление path tracer'а ---

TEST(GuideRayTracing, BlasLodMasksAndAccumulation) {
    // -1 (по умолчанию) — самый грубый LOD меша; иначе запрошенный, зажатый в число LOD'ов.
    EXPECT_EQ(rt::selectBlasLod(4, -1), 3u);
    EXPECT_EQ(rt::selectBlasLod(4, 0), 0u);
    EXPECT_EQ(rt::selectBlasLod(2, 5), 1u);

    // BlendMode материала → маска инстанса (0 Opaque, 1 AlphaTest, 2 Transparent, 3 Refractive).
    EXPECT_EQ(rt::instanceMask(0, true), u32(rt::kMaskOpaque | rt::kMaskShadowOpaque));
    EXPECT_EQ(rt::instanceMask(0, false), u32(rt::kMaskOpaque)) << "castShadows = false — невидим для теневых лучей";
    EXPECT_EQ(rt::instanceMask(3, true) & rt::kMaskTranslucent, u32(rt::kMaskTranslucent));
    EXPECT_NE(rt::instanceMask(3, true) & rt::kMaskShadowTranslucent, 0u) << "стекло даёт цветную тень";

    // Path tracer накапливает сэмплы, пока ничего не меняется (камера, сцена, настройки).
    rt::AccumulationTracker acc;
    EXPECT_TRUE(acc.update(/*camera*/ 1, /*scene*/ 2, /*settings*/ 3));
    acc.addSamples(1);
    EXPECT_FALSE(acc.update(1, 2, 3));
    acc.addSamples(1);
    EXPECT_EQ(acc.sampleCount(), 2u);
    EXPECT_TRUE(acc.update(9, 2, 3)) << "камера сдвинулась — накопление сначала";
    EXPECT_EQ(acc.sampleCount(), 0u);
}
