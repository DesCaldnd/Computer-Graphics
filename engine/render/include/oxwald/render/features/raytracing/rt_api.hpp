#pragma once

// Integration surface of the ray tracing scene for other feature areas.
//
//   * world-skinning: provide post-skinning positions so skinned BLASes are refitted every frame:
//       if (auto* rt = ox::render::rt::RayTracingSceneApi::find(renderer.features()))
//           rt->setDeformedGeometryProvider([&](u32 gpuInstance) { return mySkinnedOutput(gpuInstance); });
//   * volumetrics: when `rt->activeThisFrame()`, read the "VolumetricShadow" froxel texture (rt::res) from
//     FrameResources at InjectionPoint::Lighting (see raytracing/volumetric_visibility.glsl for the mapping), or trace
//     your own rays with `sceneHeaderAddress()` + raytracing/rt_common.glsl.
//   * any ray query shader: push `sceneHeaderAddress()` (RtSceneBuffer in rt_common.glsl) and declare
//     `.read(resources().buffer(rt::res::kRtScene), rhi::Access::AccelStructRead)` on the pass.

#include <oxwald/render/features/raytracing/rt_scene.hpp>
#include <oxwald/rhi/vulkan.hpp>

namespace ox::render {
class FeatureRegistry;
}

namespace ox::render::rt {

inline constexpr std::string_view kRayTracingSceneFeature = "RayTracingScene";

class RayTracingSceneApi {
public:
    virtual ~RayTracingSceneApi() = default;
    // The "RayTracingScene" feature of a registry (nullptr when the area is not registered).
    static RayTracingSceneApi* find(FeatureRegistry& registry);

    virtual void setDeformedGeometryProvider(DeformedGeometryProvider provider) = 0;
    // True between the TLAS pass declaration (AfterDepth) and the end of the frame when ray tracing runs.
    [[nodiscard]] virtual bool activeThisFrame() const = 0;
    // Per-frame RtSceneHeader (TLAS address, instance table, index arena, DDGI volume); 0 when inactive.
    [[nodiscard]] virtual VkDeviceAddress sceneHeaderAddress() const = 0;
    [[nodiscard]] virtual u64 tlasAddress() const = 0;
    [[nodiscard]] virtual const BlasScheduler::Stats& blasStats() const = 0;
    [[nodiscard]] virtual u32 tlasInstanceCount() const = 0;
};

} // namespace ox::render::rt
