#include "tour.hpp"

#include <oxwald/async/async.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/gameplay/script.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/ui/ui_system.hpp>

#include <algorithm>
#include <format>

namespace ox::showcase {

namespace {

constexpr const char* kHubScene = "project://Assets/Scenes/Hub.oxscene";

std::string scriptText(Entity e, const char* property) {
    if (!e) return {};
    const auto* sc = e.tryGet<gameplay::ScriptComponent>();
    if (!sc) return {};
    auto it = sc->properties.find(property);
    return it == sc->properties.end() ? std::string() : it->second.text;
}

f32 smooth(f32 t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

Tour::Tour(ShowcaseModule& module) : m_module(module) {
    m_order.push_back({"hub", "Хаб", kHubScene, "README.md"});
    for (const StationInfo& s : module.stations()) m_order.push_back(s);
}

Tour::~Tour() = default;

u32 Tour::pendingAssetLoads() const {
    if (auto* ui = m_module.engine().services().tryGet<ui::UiSystem>()) {
        if (ui->bridge().hasRenderInfo()) return ui->bridge().renderInfo().stats.pendingAssetLoads;
    }
    return 0;
}

i32 Tour::nextIndex(i32 from) const {
    const std::string& only = m_module.args().onlyStation;
    for (i32 i = from + 1; i < i32(m_order.size()); ++i) {
        if (only.empty() || m_order[usize(i)].id == only) return i;
    }
    return -1;
}

void Tour::requestStation(i32 index) {
    m_index = index;
    if (index < 0) {
        m_finished = true;
        OX_LOG_INFO("showcase", "Tour finished ({} stations)", m_order.size());
        m_module.publish("showcase.tourFinished", {});
        if (m_module.args().quitAfterTour) m_module.engine().requestQuit();
        else m_module.finishTour();
        return;
    }
    const StationInfo& s = m_order[usize(index)];
    OX_LOG_INFO("showcase", "Tour: station {}/{} '{}' ({})", index, m_order.size() - 1, s.id, s.scene);
    if (m_module.engine().currentLevel() == s.scene) onWorldChanged(m_module.engine().world());
    else m_module.queueLevel(s.scene);
}

void Tour::preUpdate(const FrameTime&) {
    if (m_module.args().mode != LaunchMode::Tour || m_started || m_module.engine().loading()) return;
    m_started = true;
    requestStation(nextIndex(-1));
}

void Tour::onWorldChanged(World& world) {
    const LaunchMode mode = m_module.args().mode;
    if (mode == LaunchMode::Play) return;
    auto* sched = m_module.engine().services().tryGet<CoroutineScheduler>();
    if (!sched) return;
    if (mode == LaunchMode::Tour) {
        if (m_index < 0 || m_finished || m_order[usize(m_index)].scene != m_module.engine().currentLevel()) return;
    }
    sched->spawn(stationShow(&world, mode == LaunchMode::Tour ? m_index : -1), {.name = "Showcase.Tour"});
}

Task<> Tour::stationShow(World* world, i32 index) {
    co_await named("Showcase.Tour");
    co_await nextFrame();
    Entity a = world->findByName("TourCam.A");
    Entity b = world->findByName("TourCam.B");
    if (!a) {
        OX_LOG_WARN("showcase", "tour: no TourCam.A in '{}'", m_module.engine().currentLevel());
    } else {
        // The tour camera becomes the primary camera; the player's third-person camera stays but is not rendered.
        for (auto [e, cam] : world->view<CameraComponent>().each()) cam.primary = false;
        a.get<CameraComponent>().primary = true;
    }
    const Entity game = world->findByName("Game");
    const std::string title = scriptText(game, "title");
    const std::string text = scriptText(game, "description");
    const std::string id = index >= 0 ? m_order[usize(index)].id : scriptText(game, "station");
    const bool shots = !m_module.args().shotsDir.empty();
    auto caption = [&] {
        m_module.publish("showcase.caption", {{"title", title}, {"text", text}, {"id", id},
                                              {"index", std::to_string(std::max(index, 0))},
                                              {"count", std::to_string(m_order.size() - 1)}});
    };
    if (index < 0 || !a) co_return; // photo mode: hold the shot (the info panel tells the story)
    if (!shots) caption();          // screenshots are taken without the caption bar

    // Warm-up: streamed assets, probe/irradiance captures, TAA/SSR/cloud history, auto exposure.
    const bool headless = m_module.engine().config().headless;
    // At least 100 frames and 2.5 s of real time (network handshakes, smoothed stats, particles need wall time).
    u32 frameCount = 0;
    f64 elapsed = 0.0;
    while (frameCount < 2000 && (frameCount < 100 || elapsed < 2.5 || pendingAssetLoads() > 0)) {
        co_await nextFrame();
        ++frameCount;
        elapsed += m_module.engine().frameTime().realDt;
    }
    if (!m_module.args().shotsDir.empty()) {
        m_shotState = std::make_shared<std::atomic<int>>(-1);
        auto state = m_shotState;
        const auto path = m_module.args().shotsDir / (id + ".png");
        if (m_module.engine().renderer().requestScreenshot(path, [state](bool ok) { state->store(ok ? 1 : 0); })) {
            for (int i = 0; i < 60 && state->load() < 0; ++i) co_await nextFrame();
            if (state->load() != 1) OX_LOG_WARN("showcase", "tour screenshot {} not written", path.string());
        } else {
            OX_LOG_WARN("showcase", "tour screenshots need the headless Vulkan renderer");
        }
    }

    if (shots) caption();

    // Flythrough A -> B (frame based when headless so runs are deterministic, time based otherwise).
    if (b) {
        const Transform from = a.worldTransform();
        const Transform to = b.worldTransform();
        const f64 duration = 5.0;
        f64 t = 0.0;
        const u32 frames = 90;
        for (u32 i = 0; t < duration && (!headless || i < frames); ++i) {
            co_await nextFrame();
            t += headless ? duration / frames : m_module.engine().frameTime().realDt;
            const f32 k = smooth(f32(t / duration));
            Transform cur = from;
            cur.position = glm::mix(from.position, to.position, k);
            cur.rotation = glm::slerp(from.rotation, to.rotation, k);
            a.setWorldTransform(cur);
        }
    } else {
        co_await (headless ? toTask(frames(30)) : toTask(seconds(3.0)));
    }
    requestStation(nextIndex(index));
}

} // namespace ox::showcase
