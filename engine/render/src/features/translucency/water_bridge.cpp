// Gameplay → render water bridge (compiled when render links gameplay with its world integration): an ExtractHookEx
// that turns gameplay::WorldRenderData::water (WaterComponent: the waves the CPU buoyancy uses) into SnapshotWater
// entries. A WaterSurfaceComponent on the same entity supplies the look (optics, foam, caustics); entities with a
// gameplay WaterComponent are skipped by the plain WaterSurfaceComponent extract so each surface is drawn once.
#include "translucency_internal.hpp"

#if OX_RENDER_HAS_GAMEPLAY && OX_RENDER_HAS_WORLD && OX_GAMEPLAY_HAS_WORLD

#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/world/components.hpp>
#include <oxwald/gameplay/world/render_data.hpp>
#include <oxwald/scene/runtime_id.hpp>
#include <oxwald/scene/world.hpp>

#include <cstring>

namespace ox::render {

namespace {

static_assert(sizeof(world::GerstnerParamsGpu) == sizeof(GerstnerParams), "Gerstner layout mismatch");

void waterBridge(const World& world, RenderSnapshot& out, const ExtractOptions& options) {
    if (!options.services) return;
    const auto* wrd = options.services->tryGet<gameplay::WorldRenderData>();
    if (!wrd || wrd->water.empty()) return;
    const entt::registry& reg = world.registry();
    for (const gameplay::WaterRenderItem& item : wrd->water) {
        SnapshotWater s;
        std::memcpy(&s.params, &item.params, sizeof(GerstnerParams));
        s.center = {item.transform[3].x, item.transform[3].z};
        s.size = item.size;
        const entt::entity e = entityFromRuntimeId(item.entity);
        if (reg.valid(e)) {
            s.entityId = encodeEntityId(u32(entt::to_integral(e)));
            if (const auto* look = reg.try_get<WaterSurfaceComponent>(e)) {
                if (!look->visible) continue;
                s.look = *look;
                s.look.waves.clear();
            }
        }
        addWaterSurface(out, s);
    }
}

} // namespace

bool waterOwnedByGameplay(const World& world, u32 enttValue) {
    return world.registry().all_of<gameplay::WaterComponent>(entt::entity(enttValue));
}

void installTranslucencyWaterBridge() { addExtractHookEx(&waterBridge); }

} // namespace ox::render

#else

namespace ox::render {
bool waterOwnedByGameplay(const World&, u32) { return false; }
void installTranslucencyWaterBridge() {}
} // namespace ox::render

#endif
