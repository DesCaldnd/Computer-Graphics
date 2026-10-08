#include <oxwald/gameplay/common.hpp>
#include <oxwald/gameplay/world.hpp>
#include <oxwald/scene/system.hpp>
#include <oxwald/scene/world.hpp>
#include <oxwald/script/script_vm.hpp>
#include <oxwald/script/sol_glm.hpp>

#include <functional>
#include <memory>
#include <string>

namespace ox::gameplay {

namespace {

class WorldSystem final : public ISystem {
public:
    using UpdateFn = std::function<void(SystemContext&)>;
    using AttachFn = std::function<void(World&, Services&)>;
    using DetachFn = std::function<void()>;

    WorldSystem(std::string_view name, SystemPhase phase, i32 order, bool playOnly, UpdateFn fn, AttachFn attach = {},
                DetachFn detach = {})
        : m_name(name), m_phase(phase), m_order(order), m_playOnly(playOnly), m_fn(std::move(fn)), m_attach(std::move(attach)),
          m_detach(std::move(detach)) {}

    std::string_view name() const override { return m_name; }
    SystemPhase phase() const override { return m_phase; }
    i32 order() const override { return m_order; }
    bool playModeOnly() const override { return m_playOnly; }
    void onAttach(World& w, Services& s) override {
        if (m_attach) m_attach(w, s);
    }
    void onDetach(World&, Services&) override {
        if (m_detach) m_detach();
    }
    void update(SystemContext& ctx) override {
        if (m_fn) m_fn(ctx);
    }

private:
    std::string m_name;
    SystemPhase m_phase;
    i32 m_order;
    bool m_playOnly;
    UpdateFn m_fn;
    AttachFn m_attach;
    DetachFn m_detach;
};

void add(SystemScheduler& scheduler, std::string_view name, SystemPhase phase, i32 order, bool playOnly, WorldSystem::UpdateFn fn,
         WorldSystem::AttachFn attach = {}, WorldSystem::DetachFn detach = {}) {
    if (scheduler.find(name)) return; // idempotent
    scheduler.add(std::make_unique<WorldSystem>(name, phase, order, playOnly, std::move(fn), std::move(attach), std::move(detach)));
}

std::optional<world::WeatherPreset> presetByName(std::string_view name) {
    if (name == "clear") return world::WeatherPreset::clear();
    if (name == "overcast") return world::WeatherPreset::overcast();
    if (name == "rainy" || name == "rain") return world::WeatherPreset::rainy();
    if (name == "storm") return world::WeatherPreset::storm();
    if (name == "snowy" || name == "snow") return world::WeatherPreset::snowy();
    return std::nullopt;
}

void bindLuaApi(script::ScriptVM& vm, WorldRuntime* rt) {
    if (vm.hasApi("world")) return;
    vm.bindApi("world", [rt](sol::state_view, sol::table& api) {
        api["terrainHeight"] = [rt](f32 x, f32 z) -> sol::optional<f32> {
            if (auto h = rt->terrainHeight({x, z})) return *h;
            return sol::nullopt;
        };
        api["terrainNormal"] = [rt](f32 x, f32 z) -> sol::optional<glm::vec3> {
            if (auto n = rt->terrainNormal({x, z})) return *n;
            return sol::nullopt;
        };
        api["waterHeight"] = [rt](f32 x, f32 z) -> sol::optional<f32> {
            if (auto h = rt->waterHeight({x, z})) return *h;
            return sol::nullopt;
        };
        api["timeOfDay"] = [rt]() -> sol::optional<f64> {
            if (auto h = rt->timeOfDay()) return *h;
            return sol::nullopt;
        };
        api["setTimeOfDay"] = [rt](f64 hours) { return rt->setTimeOfDay(hours); };
        api["isDay"] = [rt]() {
            const world::SkyState* s = rt->skyState();
            return s ? s->isDay : true;
        };
        api["sunDirection"] = [rt]() {
            const world::SkyState* s = rt->skyState();
            return s ? s->sunDirection : glm::vec3(0.f, 1.f, 0.f);
        };
        api["wind"] = [rt](glm::vec3 position) { return rt->windAt(position); };
        api["setWeather"] = [rt](const std::string& preset, sol::optional<f32> seconds) {
            auto p = presetByName(preset);
            return p ? rt->setWeather(*p, seconds.value_or(10.f)) : false;
        };
        api["isAreaReady"] = [rt](glm::vec3 position, f32 radius) { return rt->isAreaReady(position, radius); };
        api["time"] = [rt]() { return rt->gameTime(); };
    });
}

} // namespace

void addWorldSystems(SystemScheduler& scheduler, Services& services, const WorldSystemsConfig& config) {
    registerWorldGameplayTypes();
    WorldRuntime* rt = services.tryGet<WorldRuntime>();
    if (!rt) rt = &services.emplace<WorldRuntime>(config);
    WorldRenderData* rd = services.tryGet<WorldRenderData>();
    if (!rd) {
        rd = &services.emplace<WorldRenderData>();
        rd->aspect = config.aspect;
    }
    if (config.bindLua) {
        if (auto* vm = services.tryGet<script::ScriptVM>(); vm && !vm->hasApi("world")) bindLuaApi(*vm, rt);
    }

    // Lifecycle runs right after Gameplay.Lifecycle (-1000), so physics bodies of other components already exist /
    // are already destroyed when the world's colliders follow the play state.
    auto state = std::make_shared<PlayStateTracker>();
    add(
        scheduler, systems::kWorldLifecycle, SystemPhase::PreUpdate, -990, false,
        [=](SystemContext& c) {
            if (const i32 change = state->update(c.playing); change != 0) rt->syncPlayState(change > 0);
            rt->advanceTime(c.dt);
        },
        [=](World& w, Services& s) {
            state->reset();
            rt->attach(w, s);
        },
        [=] {
            rt->detach();
            rd->clear();
            state->reset();
        });
    add(scheduler, systems::kWorldStreaming, SystemPhase::PreUpdate, -500, true, [=](SystemContext&) { rt->updateStreaming(); });
    add(scheduler, systems::kWorldTerrain, SystemPhase::PreUpdate, -400, false, [=](SystemContext&) { rt->updateTerrains(); });
    // Before Gameplay.Physics.Step (FixedUpdate 0): forces are applied in the same step.
    add(scheduler, systems::kWorldBuoyancy, SystemPhase::FixedUpdate, -10, true, [=](SystemContext& c) { rt->fixedBuoyancy(c.dt); });
    // Before the scripts (PreUpdate 0) so water/wind/sky queries are valid from the first onCreate/onStart, and well
    // before the PostUpdate transform propagation (the sun light's rotation is propagated this frame). Script changes
    // made in Update apply next frame.
    add(scheduler, systems::kWorldEnvironment, SystemPhase::PreUpdate, -300, false,
        [=](SystemContext& c) { rt->updateEnvironment(c.dt, c.playing); });
    // After the transform propagation: viewers' world matrices are current.
    add(scheduler, systems::kWorldVegetation, SystemPhase::PostUpdate, 300, false, [=](SystemContext&) { rt->updateVegetation(); });
    add(scheduler, systems::kWorldExtract, SystemPhase::Extract, 50, false, [=](SystemContext& c) { rt->extract(*rd, c.frame); });
}

} // namespace ox::gameplay
