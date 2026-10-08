// Main menu backdrop and the hub plaza with a portal to every station.
#include "gen.hpp"

#include <oxwald/gameplay/world/components.hpp>
#include <oxwald/render/components/reflections.hpp>

#include <cmath>
#include <numbers>

namespace ox::showcase::gen {

namespace {
constexpr f32 kPi = std::numbers::pi_v<f32>;

// Sky + sun at a fixed time of day (TimeOfDay drives the sun light, ambient, fog and exposure).
Entity skyAndSun(SceneBuilder& sb, f64 hours, f64 timeScale, bool paused, f32 turbidity = 2.6f) {
    Entity sun = sb.sun({0.3f, 0.5f, -0.4f}, 60000.0f);
    Entity sky = sb.create("Sky");
    auto& s = sky.add<gameplay::SkyComponent>();
    s.turbidity = turbidity;
    auto& tod = sky.add<gameplay::TimeOfDayComponent>();
    tod.localHours = hours;
    tod.timeScale = timeScale;
    tod.paused = paused;
    tod.turbidity = turbidity;
    tod.sun = sun.ref();
    tod.month = 7;
    tod.day = 12;
    sb.environment(sun, 1.0f, 1.0f);
    return sun;
}
} // namespace

void buildMainMenu(Gen& gen) {
    SceneBuilder sb(gen, "MainMenu");
    Entity g = sb.create("Game");
    sb.script(g, "Scripts/game.lua");
    sb.prop(g, "station", std::string("menu"));
    sb.prop(g, "title", std::string("OxwaldShowcase"));
    sb.prop(g, "description", std::string("Демонстрация всех возможностей OxwaldEngine"));
    sb.prop(g, "menu", true);

    skyAndSun(sb, 18.6, 0.0, true, 3.0f);
    sb.postProcess();
    // Stage: polished dark floor, a ring of chrome / gold / glass spheres and the mannequin in the middle.
    sb.block("Floor", "PolishedFloor", {0, -0.25f, 0}, {60, 0.5f, 60});
    sb.farGround(-0.3f);
    Entity probe = sb.create("Probe");
    probe.setPosition({0, 3, 0});
    auto& rp = probe.add<render::ReflectionProbeComponent>();
    rp.extents = {30, 10, 30};
    rp.update = render::ReflectionProbeUpdate::OnEnable;
    const char* mats[] = {"Chrome", "Gold", "Glass", "Copper", "CarPaint.Red", "Glass.Blue", "Steel", "Emissive.Cyan"};
    for (int i = 0; i < 8; ++i) {
        const f32 a = f32(i) / 8.0f * 2 * kPi;
        const f32 r = 6.0f;
        const f32 size = 1.2f + 0.3f * f32(i % 3);
        sb.prim("Sphere", Primitive::Sphere, mats[i], {std::cos(a) * r, size * 0.5f, std::sin(a) * r}, glm::vec3(size));
    }
    sb.prim("Monolith", Primitive::Cube, "DarkMetal", {0, 2.5f, -14}, {4, 5, 0.6f});
    Entity logo = sb.prim("Logo", Primitive::Cube, "Logo", {0, 2.6f, -13.65f}, {3.4f, 2.4f, 0.05f});
    logo.get<MeshRendererComponent>().castShadows = false;
    for (int i = 0; i < 6; ++i) {
        const f32 x = -12.5f + f32(i) * 5.0f;
        sb.prim("Pillar", Primitive::Cylinder, "Concrete", {x, 4, -18}, {0.8f, 8, 0.8f});
        sb.pointLight("PillarLight", {x, 7.6f, -16.8f}, {1.0f, 0.7f, 0.45f}, 1500.0f, 12.0f, false);
    }
    Entity cam = sb.camera("MenuCamera", {9, 3.2f, 11}, {0, 1.0f, 0}, 50.0f, true);
    sb.script(cam, "Scripts/menu_camera.lua");
    cam.add<gameplay::AudioListenerComponent>();
    Entity music = sb.create("Music");
    auto& as = music.add<gameplay::AudioSourceComponent>();
    as.clip = gen.meta("Audio/music_loop.wav");
    as.bus = "Music";
    as.loop = true;
    as.spatial = false;
    as.volume = 0.5f;
    as.fadeInSeconds = 2.0f;
    sb.tourCameras({9, 3.2f, 11}, {0, 1.0f, 0}, {-8, 2.5f, 9}, {0, 1.2f, 0}, 50.0f);
    sb.save("Scenes/MainMenu.oxscene");
}

void buildHub(Gen& gen) {
    SceneBuilder sb(gen, "Hub");
    Entity g = sb.create("Game");
    sb.script(g, "Scripts/game.lua");
    sb.prop(g, "station", std::string("hub"));
    sb.prop(g, "title", std::string("Хаб"));
    sb.prop(g, "description",
            std::string("Площадь с порталами: каждый ведёт на станцию, посвящённую одной подсистеме движка. "
                        "Подойдите к порталу, чтобы перейти; в любой момент Esc — меню, F5/F9 — сохранение, F1 — отладка."));
    sb.prop(g, "hints", std::string("[WASD] — ходьба · [Shift] — бег · [Space] — прыжок · [E] — действие · [Esc] — меню"));
    sb.prop(g, "guide", std::string("samples/OxwaldShowcase/README.md"));

    skyAndSun(sb, 17.2, 0.0, true);
    sb.postProcess();
    // Plaza: tiled disc with a stone rim, a ring path and the central reflective monument.
    Entity plaza = sb.prim("Plaza", Primitive::Cylinder, "Tiles.x16", {0, -0.2f, 0}, {64, 0.4f, 64});
    auto& pc = plaza.add<gameplay::ColliderComponent>();
    pc.type = gameplay::ColliderType::Cylinder;
    pc.radius = 0.5f;
    pc.halfHeight = 0.5f;
    sb.farGround(-0.35f);
    for (int i = 0; i < 48; ++i) { // low stone rim
        const f32 a = f32(i) / 48.0f * 2 * kPi;
        sb.block("Rim", "Stone", {std::sin(a) * 32.3f, 0.15f, std::cos(a) * 32.3f}, {4.3f, 0.7f, 0.9f}, yawRotation(glm::degrees(a)));
    }
    // Monument: pedestal, chrome sphere, glass ring.
    sb.block("Pedestal", "DarkMetal", {0, 0.6f, 0}, {4, 1.2f, 4});
    sb.prim("Orb", Primitive::Sphere, "Chrome", {0, 3.0f, 0}, glm::vec3(3.4f));
    sb.prim("OrbRing", Primitive::Torus, "Gold", {0, 3.0f, 0}, {5.6f, 0.5f, 5.6f}, eulerDeg(20, 0, 10));
    Entity probe = sb.create("HubProbe");
    probe.setPosition({0, 4, 0});
    auto& rp = probe.add<render::ReflectionProbeComponent>();
    rp.extents = {34, 14, 34};
    rp.boxProjection = false;
    rp.update = render::ReflectionProbeUpdate::OnEnable;

    // Portals on a circle, facing the centre (portal local +Z points to the plaza centre).
    const auto& st = stations();
    const f32 radius = 24.0f;
    for (usize i = 0; i < st.size(); ++i) {
        const f32 a = (f32(i) + 0.5f) / f32(st.size()) * 2 * kPi;
        const glm::vec3 pos(std::sin(a) * radius, 0, std::cos(a) * radius);
        const f32 yaw = glm::degrees(a) + 180.0f;
        Entity p = sb.portal("Portal." + st[i].id, st[i].uri(), st[i].id, st[i].color, pos, yaw);
        // Plinth with the station number lights + a lamp post.
        sb.prim("Plinth", Primitive::Cube, "Accent." + st[i].id, {0, 0.05f, 1.6f}, {2.6f, 0.1f, 1.2f}, {}, p);
    }
    // Lamp posts between portals.
    for (usize i = 0; i < st.size(); ++i) {
        const f32 a = f32(i) / f32(st.size()) * 2 * kPi;
        const glm::vec3 pos(std::sin(a) * 27.0f, 0, std::cos(a) * 27.0f);
        sb.prim("LampPost", Primitive::Cylinder, "DarkMetal", pos + glm::vec3(0, 2.2f, 0), {0.18f, 4.4f, 0.18f});
        sb.prim("LampGlobe", Primitive::Sphere, "Lamp", pos + glm::vec3(0, 4.5f, 0), glm::vec3(0.45f));
        sb.pointLight("Lamp", pos + glm::vec3(0, 4.5f, 0), {1.0f, 0.78f, 0.5f}, 2500.0f, 14.0f, i % 2 == 0, 0.2f);
    }
    // Benches and planters around the monument.
    for (int i = 0; i < 4; ++i) {
        const f32 a = f32(i) / 4.0f * 2 * kPi + kPi / 4;
        const glm::vec3 pos(std::sin(a) * 9.0f, 0, std::cos(a) * 9.0f);
        Entity bench = sb.block("Bench", "Wood", pos + glm::vec3(0, 0.45f, 0), {2.4f, 0.12f, 0.6f}, yawRotation(glm::degrees(a)));
        (void)bench;
        sb.block("BenchLeg", "DarkMetal", pos + glm::vec3(0, 0.2f, 0), {2.0f, 0.4f, 0.4f}, yawRotation(glm::degrees(a)));
        sb.prim("Planter", Primitive::Cylinder, "Concrete", pos * 1.35f + glm::vec3(0, 0.4f, 0), {1.6f, 0.8f, 1.6f});
        sb.prim("Bush", Primitive::Sphere, "Leaves", pos * 1.35f + glm::vec3(0, 1.3f, 0), {1.8f, 1.4f, 1.8f});
    }

    sb.player({0, 0.05f, 12}, 0.0f);
    Entity wind = sb.create("Ambience");
    auto& as = wind.add<gameplay::AudioSourceComponent>();
    as.clip = gen.meta("Audio/ambience_wind.wav");
    as.bus = "Ambience";
    as.loop = true;
    as.spatial = false;
    as.volume = 0.35f;
    Entity music = sb.create("Music");
    auto& ms = music.add<gameplay::AudioSourceComponent>();
    ms.clip = gen.meta("Audio/music_loop.wav");
    ms.bus = "Music";
    ms.loop = true;
    ms.spatial = false;
    ms.volume = 0.35f;
    sb.tourCameras({0, 13, 38}, {0, 1.5f, 0}, {17, 5, 17}, {-6, 2, -8}, 55.0f);
    sb.save("Scenes/Hub.oxscene");
}

} // namespace ox::showcase::gen
