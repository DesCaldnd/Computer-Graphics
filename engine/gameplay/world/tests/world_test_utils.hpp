#pragma once

#include "gameplay_test_utils.hpp"

#include <oxwald/gameplay/world.hpp>

namespace ox::gameplay::test {

inline WorldSystemsConfig worldTestConfig() {
    WorldSystemsConfig c;
    c.streamingExecutor = WorldSystemsConfig::Executor::Inline; // deterministic
    return c;
}

// GameplayHarness + the world systems/services.
class WorldHarness : public GameplayHarness {
public:
    explicit WorldHarness(WorldSystemsConfig config = worldTestConfig(), std::function<void(Services&)> preServices = {})
        : GameplayHarness(testConfig(), std::move(preServices)) {
        registerWorldGameplayTypes();
        addWorldSystems(scheduler, services, config);
    }

    WorldRuntime& worldRt() { return services.get<WorldRuntime>(); }
    WorldRenderData& renderData() { return services.get<WorldRenderData>(); }

    // Gentle procedural terrain centred on the origin: 128 m, 1 m spacing, heights in [0, 8] m.
    Entity terrain(std::string_view name = "Terrain", u32 resolution = 129, f32 size = 128.f) {
        Entity t = world.create(name);
        auto& c = t.add<TerrainComponent>();
        c.source = TerrainSource::Procedural;
        c.resolution = resolution;
        c.worldSize = size;
        c.heightScale = 8.f;
        c.noise.fractal.frequency = 1.f / 128.f;
        c.noise.fractal.octaves = 3;
        c.noise.fractal.seed = 42;
        c.lod = {.leafNodeSize = 16, .lodCount = 3, .viewDistance = 400.f};
        return t;
    }
};

} // namespace ox::gameplay::test
