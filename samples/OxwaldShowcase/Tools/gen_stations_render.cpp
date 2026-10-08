// Rendering stations: 1 lighting, 2 materials, 3 reflections, 4 volumetrics, 5 water & particles, 13 RTX.
#include "gen.hpp"

#include <oxwald/gameplay/world/components.hpp>
#include <oxwald/render/components/reflections.hpp>
#include <oxwald/render/components/translucency.hpp>
#include <oxwald/render/components/volumetrics.hpp>

#include <cmath>
#include <numbers>

namespace ox::showcase::gen {

namespace {
constexpr f32 kPi = std::numbers::pi_v<f32>;

Entity timeOfDay(SceneBuilder& sb, Entity sun, f64 hours, f64 timeScale, bool paused, f32 turbidity = 2.5f) {
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
    return sky;
}

Entity floorBlock(SceneBuilder& sb, const std::string& mat, f32 size, f32 y = 0.0f) {
    return sb.block("Floor", mat, {0, y - 0.25f, 0}, {size, 0.5f, size});
}

Entity audioLoop(SceneBuilder& sb, std::string_view name, const std::string& clip, glm::vec3 pos, f32 volume,
                 bool spatial = true, const std::string& bus = "SFX", f32 maxDistance = 30.0f) {
    Entity e = sb.create(name);
    e.setPosition(pos);
    auto& a = e.add<gameplay::AudioSourceComponent>();
    a.clip = sb.gen.meta(clip);
    a.loop = true;
    a.spatial = spatial;
    a.bus = bus;
    a.volume = volume;
    a.maxDistance = maxDistance;
    a.minDistance = 1.5f;
    return e;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// 1. Свет и тени
// Layout (camera A looks north-west from the south-east): pergola with striped CSM shadows on the right, the
// light garden (256 clustered lights) in the middle, the spot-lit statue alcove on the left, the caged lantern
// (point light cube shadows) at the back, PCSS needle + orb in front.
void buildLighting(Gen& gen) {
    const Station& s = station("lighting");
    SceneBuilder sb(gen, s.id);
    Entity sun = sb.sun({0.4f, 0.25f, -0.6f}, 30000.0f, {1.0f, 0.85f, 0.7f}, 0.6f);
    Entity sky = timeOfDay(sb, sun, 19.55, 0.0, true, 3.0f);
    sb.script(sky, "Scripts/time_of_day.lua");
    sb.environment(sun);
    sb.postProcess(std::nullopt, 1.0f);
    sb.farGround();
    floorBlock(sb, "Grid.x16", 64);
    sb.stationBasics(s, {0, 0.05f, 16}, 0.0f, {-3.5f, 0, 13}, 30.0f, {0, 0, 21}, 180.0f);

    // Pergola (east): slats cast striped cascade shadows over a walkway.
    const glm::vec3 pg(11.0f, 0, 0);
    for (int i = 0; i < 6; ++i) {
        sb.block("PergolaPost", "Wood", pg + glm::vec3(-3.0f, 1.6f, 7.0f - f32(i) * 3.0f), {0.3f, 3.2f, 0.3f});
        sb.block("PergolaPost", "Wood", pg + glm::vec3(3.0f, 1.6f, 7.0f - f32(i) * 3.0f), {0.3f, 3.2f, 0.3f});
    }
    for (int i = 0; i < 18; ++i) sb.block("Slat", "Wood", pg + glm::vec3(0, 3.3f, 7.5f - f32(i) * 0.9f), {6.6f, 0.12f, 0.25f});
    sb.block("Beam", "Wood", pg + glm::vec3(-3.0f, 3.1f, -0.5f), {0.35f, 0.3f, 16.0f});
    sb.block("Beam", "Wood", pg + glm::vec3(3.0f, 3.1f, -0.5f), {0.35f, 0.3f, 16.0f});
    sb.prim("PergolaVase", Primitive::Cylinder, "Copper", pg + glm::vec3(0, 0.45f, 2.0f), {0.7f, 0.9f, 0.7f});
    sb.prim("PergolaBush", Primitive::Sphere, "Leaves", pg + glm::vec3(0, 1.3f, 2.0f), {1.4f, 1.2f, 1.4f});

    // PCSS: a tall needle and a floating orb — sharp at contact, softer far from the occluder.
    sb.block("Needle", "Steel", {4.0f, 2.5f, 6.0f}, {0.12f, 5.0f, 0.12f});
    sb.prim("FloatingOrb", Primitive::Sphere, "Gold", {2.2f, 2.4f, 5.0f}, glm::vec3(0.8f));
    sb.block("Cube", "OffWhite", {5.6f, 0.5f, 3.8f}, glm::vec3(1.0f), yawRotation(30));
    sb.block("Cube", "OffWhite", {6.0f, 1.45f, 3.9f}, glm::vec3(0.7f), yawRotation(10));

    // Spot-lit statue alcove (west, atlas shadows): legacy Suzanne on a plinth.
    const glm::vec3 al(-11.0f, 0, -2.0f);
    sb.block("AlcoveBack", "Concrete.Warm", al + glm::vec3(-2.5f, 2.5f, 0), {0.5f, 5.0f, 8.0f});
    sb.block("AlcoveSideA", "Concrete.Warm", al + glm::vec3(0, 2.5f, -4.0f), {5.0f, 5.0f, 0.5f});
    sb.block("AlcoveSideB", "Concrete.Warm", al + glm::vec3(0, 2.5f, 4.0f), {5.0f, 5.0f, 0.5f});
    sb.block("AlcoveRoof", "Concrete.Warm", al + glm::vec3(0, 5.25f, 0), {5.5f, 0.5f, 8.5f});
    sb.block("Plinth", "DarkMetal", al + glm::vec3(-0.5f, 0.5f, 0), {1.2f, 1.0f, 1.2f});
    if (auto monkey = gen.imported("Legacy/monkey.obj#Mesh/0")) {
        sb.mesh("Suzanne", *monkey, "Copper", al + glm::vec3(-0.5f, 1.65f, 0), glm::vec3(0.45f), yawRotation(90));
    } else {
        sb.prim("Suzanne", Primitive::Sphere, "Copper", al + glm::vec3(-0.5f, 1.65f, 0), glm::vec3(0.9f));
    }
    sb.spotLight("StatueSpot", al + glm::vec3(2.0f, 4.4f, 1.2f), al + glm::vec3(-0.5f, 1.4f, 0), {1.0f, 0.88f, 0.7f}, 14000.0f, 14.0f, 16.0f, 30.0f, true, 0.08f);
    sb.spotLight("StatueRim", al + glm::vec3(-1.6f, 4.2f, -3.2f), al + glm::vec3(-0.5f, 1.6f, 0), {0.3f, 0.5f, 1.0f}, 6000.0f, 10.0f, 15.0f, 28.0f, true, 0.05f);

    // Caged lantern (north): point light cube shadows through the bars.
    const glm::vec3 lantern(0.0f, 0, -12.0f);
    sb.block("LanternBase", "DarkMetal", lantern + glm::vec3(0, 0.15f, 0), {1.4f, 0.3f, 1.4f});
    for (int i = 0; i < 12; ++i) {
        const f32 a = f32(i) / 12.0f * 2 * kPi;
        sb.block("Bar", "DarkMetal", lantern + glm::vec3(std::cos(a) * 0.6f, 1.2f, std::sin(a) * 0.6f), {0.05f, 1.8f, 0.05f});
    }
    sb.block("LanternTop", "DarkMetal", lantern + glm::vec3(0, 2.15f, 0), {1.4f, 0.1f, 1.4f});
    sb.prim("Flame", Primitive::Sphere, "Emissive.Warm", lantern + glm::vec3(0, 1.1f, 0), glm::vec3(0.25f));
    sb.pointLight("LanternLight", lantern + glm::vec3(0, 1.1f, 0), {1.0f, 0.6f, 0.3f}, 9000.0f, 16.0f, true, 0.05f);
    sb.block("LanternWall", "Concrete.Warm", lantern + glm::vec3(0, 2.0f, -3.5f), {12.0f, 4.0f, 0.5f});

    // Light garden: 16×16 coloured point lights over a dark bed (clustered forward+).
    Entity garden = sb.create("LightGarden");
    garden.setPosition({-1.5f, 0, -3.0f});
    sb.script(garden, "Scripts/light_garden.lua");
    sb.block("GardenBed", "Concrete", {-1.5f, 0.05f, -3.0f}, {12.5f, 0.1f, 9.0f});
    for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
            const f32 hue = f32((x * 7 + z * 3) % 16) / 16.0f;
            const glm::vec3 c = glm::clamp(glm::abs(glm::fract(glm::vec3(hue) + glm::vec3(0, 2.0f / 3, 1.0f / 3)) * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
            const glm::vec3 p(-5.6f + f32(x) * 0.75f, 0.3f, -4.2f + f32(z) * 0.55f);
            Entity l = sb.pointLight("GardenLight", p, c, 320.0f, 2.4f, false, 0.02f, garden);
            sb.prim("Bulb", Primitive::Sphere, "Bulb." + std::to_string((x * 7 + z * 3) % 16), {0, 0, 0}, glm::vec3(0.1f), {}, l);
        }
    sb.tourCameras({9.0f, 4.6f, 13.0f}, {-2.0f, 0.6f, -4.0f}, {-4.0f, 2.2f, 4.0f}, {-11.5f, 1.5f, -2.0f}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 2. Материалы
void buildMaterials(Gen& gen) {
    const Station& s = station("materials");
    SceneBuilder sb(gen, s.id);
    Entity sun = sb.sun({-0.5f, 0.7f, 0.4f}, 40000.0f, {1.0f, 0.95f, 0.88f}, 0.4f);
    // LDR sky cube (legacy env.png): the renderer scales 8-bit skyboxes to daylight luminance (ldrSkyLuminance, cd/m²).
    Entity env = sb.environment(sun, 1.0f, 1.0f);
    env.get<EnvironmentComponent>().skybox = gen.meta("Textures/Sky/park.oxcube");
    env.get<EnvironmentComponent>().ldrSkyLuminance = 7000.0f;
    sb.postProcess(std::nullopt, 1.0f);
    floorBlock(sb, "Parquet", 40);
    sb.stationBasics(s, {0, 0.05f, 14}, 0.0f, {3.5f, 0, 11}, -25.0f, {0, 0, 19}, 180.0f);

    Entity debugViews = sb.create("MaterialDebugViews");
    sb.script(debugViews, "Scripts/material_debug.lua");

    // PBR grid: 7 roughness columns × 5 metallic rows on a dark stand.
    sb.block("Stand", "DarkGrey", {0, 2.6f, -6.6f}, {9.0f, 5.6f, 0.4f});
    for (int r = 0; r < 7; ++r)
        for (int m = 0; m < 5; ++m)
            sb.prim("PBRSphere", Primitive::Sphere, "PBR.r" + std::to_string(r) + ".m" + std::to_string(m),
                    {-3.6f + f32(r) * 1.2f, 0.6f + f32(m) * 1.1f, -6.0f}, glm::vec3(0.9f));
    // Leather lounge (legacy leather textures) + clear-coat car paint.
    const glm::vec3 sofa(-6.0f, 0, -1.8f);
    const glm::quat sofaRot = yawRotation(20);
    sb.block("SofaBase", "Leather.Brown", sofa + sofaRot * glm::vec3(0, 0.3f, 0), {3.2f, 0.6f, 1.2f}, sofaRot);
    sb.block("SofaBack", "Leather.Brown", sofa + sofaRot * glm::vec3(0, 0.9f, -0.5f), {3.2f, 1.0f, 0.3f}, sofaRot);
    sb.block("SofaArm", "Leather.Brown", sofa + sofaRot * glm::vec3(-1.5f, 0.7f, 0), {0.3f, 0.8f, 1.2f}, sofaRot);
    sb.block("SofaArm", "Leather.Brown", sofa + sofaRot * glm::vec3(1.5f, 0.7f, 0), {0.3f, 0.8f, 1.2f}, sofaRot);
    sb.prim("Cushion", Primitive::Cube, "Leather", sofa + sofaRot * glm::vec3(0.5f, 0.75f, 0.1f), {0.8f, 0.3f, 0.7f}, sofaRot * yawRotation(12));
    sb.prim("Pouf", Primitive::Cylinder, "Leather.Black", sofa + glm::vec3(0.8f, 0.25f, 1.9f), {1.0f, 0.5f, 1.0f});
    sb.prim("PaintBlob", Primitive::Sphere, "CarPaint.Red", {-3.0f, 0.7f, 1.6f}, glm::vec3(1.4f));
    sb.prim("PaintBlob", Primitive::Torus, "CarPaint.White", {-1.4f, 0.45f, 3.4f}, glm::vec3(1.4f), eulerDeg(70, 0, 0));
    // Soldier statue (legacy textured OBJ).
    if (auto soldier = gen.imported("Legacy/Soldier/soldier.obj#Mesh/0")) {
        sb.block("SoldierPlinth", "Stone", {6.2f, 0.4f, -2.2f}, {1.6f, 0.8f, 1.6f});
        Entity e = sb.create("Soldier");
        e.setPosition({6.2f, 0.8f, -2.2f});
        e.setRotation(yawRotation(-30) * glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0)));
        e.setScale(glm::vec3(1.25f));
        auto& mr = e.add<MeshRendererComponent>();
        mr.mesh = *soldier;
        for (const std::string& m : gen.subAssets("Legacy/Soldier/soldier.obj", "Material/"))
            if (auto id = gen.imported(m)) mr.materials.push_back(*id);
    }
    // Glass: refractive spheres with Beer–Lambert absorption, frosted glass pane in front of neon stripes.
    sb.prim("GlassSphere", Primitive::Sphere, "Glass", {3.0f, 0.75f, 1.2f}, glm::vec3(1.5f));
    sb.prim("AmberSphere", Primitive::Sphere, "Glass.Amber", {4.8f, 0.55f, 3.2f}, glm::vec3(1.1f));
    sb.prim("BlueCube", Primitive::Cube, "Glass.Blue", {1.6f, 0.5f, 3.6f}, glm::vec3(1.0f), yawRotation(35));
    for (int i = 0; i < 4; ++i)
        sb.prim("Neon", Primitive::Cube, i % 2 ? "Neon.Pink" : "Neon.Blue", {7.6f + f32(i) * 0.6f, 1.3f, 1.4f}, {0.12f, 2.2f, 0.12f});
    sb.prim("FrostedPane", Primitive::Cube, "FrostedGlass", {8.5f, 1.3f, 2.2f}, {3.2f, 2.4f, 0.08f});
    // Alpha-tested foliage: bushes of crossed leaf cards.
    for (int b = 0; b < 3; ++b) {
        const glm::vec3 base(-9.5f + f32(b) * 1.9f, 0, 2.5f + f32(b % 2) * 0.8f);
        sb.prim("Trunk", Primitive::Cylinder, "Bark", base + glm::vec3(0, 0.6f, 0), {0.15f, 1.2f, 0.15f});
        for (int c = 0; c < 6; ++c) {
            Entity card = sb.prim("LeafCard", Primitive::Plane, "Leaves", base + glm::vec3(0, 1.4f + 0.15f * f32(c % 2), 0),
                                  glm::vec3(1.6f), yawRotation(f32(c) * 30.0f) * glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0)));
            (void)card;
        }
    }
    // Emissive: glowing totems + painting.
    sb.prim("Totem", Primitive::Cylinder, "Emissive.Cyan", {-9.0f, 1.2f, -4.5f}, {0.25f, 2.4f, 0.25f});
    sb.prim("Totem", Primitive::Cylinder, "Emissive.Magenta", {-8.2f, 1.0f, -5.0f}, {0.25f, 2.0f, 0.25f});
    sb.prim("Totem", Primitive::Cylinder, "Emissive.Yellow", {-9.8f, 0.8f, -5.1f}, {0.25f, 1.6f, 0.25f});
    sb.block("Easel", "Wood", {10.0f, 1.4f, -4.5f}, {0.1f, 2.8f, 0.1f}, yawRotation(-30));
    sb.prim("Painting", Primitive::Cube, "Painting", {9.8f, 1.8f, -4.2f}, {2.6f, 1.84f, 0.05f}, yawRotation(-30));
    sb.tourCameras({0.0f, 3.4f, 11.0f}, {0.0f, 1.5f, -3.0f}, {5.5f, 1.4f, 5.5f}, {3.2f, 0.8f, 1.5f}, 60.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 3. Отражения и GI
void buildReflections(Gen& gen) {
    const Station& s = station("reflections");
    SceneBuilder sb(gen, s.id);
    Entity sun = sb.sun({0.3f, 0.6f, 0.5f}, 25000.0f, {1.0f, 0.93f, 0.85f}, 0.3f);
    Entity env = sb.environment(sun, 1.0f, 1.0f);
    env.get<EnvironmentComponent>().skybox = gen.meta("Textures/Sky/park.oxcube");
    env.get<EnvironmentComponent>().ldrSkyLuminance = 7000.0f;
    sb.postProcess(std::nullopt, 1.0f);
    sb.stationBasics(s, {0, 0.05f, 15}, 0.0f, {-3.0f, 0, 12.5f}, 25.0f, {0, 0, 19.5f}, 180.0f);
    sb.block("Outside", "Tiles.x8", {0, -0.3f, 10}, {40, 0.5f, 22});

    // Hall with a polished black floor (SSR), coloured pillars and light strips.
    sb.block("HallFloor", "PolishedFloor", {0, -0.25f, -6}, {18, 0.5f, 22});
    sb.block("HallWallL", "OffWhite", {-9.25f, 3.0f, -6}, {0.5f, 6.0f, 22});
    sb.block("HallWallBack", "OffWhite", {0, 3.0f, -17.25f}, {18.5f, 6.0f, 0.5f});
    for (int i = 0; i < 4; ++i) {
        const f32 z = 2.0f - f32(i) * 5.0f;
        sb.prim("Pillar", Primitive::Cylinder, i % 2 ? "Teal" : "Orange", {-5.5f, 3.0f, z}, {0.9f, 6.0f, 0.9f});
        sb.prim("Strip", Primitive::Cube, "Emissive.White", {-8.9f, 5.6f, z}, {0.1f, 0.1f, 3.0f});
        sb.pointLight("HallLight", {-7.5f, 5.2f, z}, {1.0f, 0.92f, 0.85f}, 6000.0f, 12.0f, false, 0.3f);
    }
    sb.prim("ChromeBall", Primitive::Sphere, "Chrome", {-2.0f, 0.8f, -4.0f}, glm::vec3(1.6f));
    sb.prim("GoldBall", Primitive::Sphere, "Gold", {0.5f, 0.6f, -7.0f}, glm::vec3(1.2f));
    // GTAO: crates in a corner.
    sb.block("Crate", "Crate", {-7.8f, 0.5f, -15.8f}, glm::vec3(1.0f));
    sb.block("Crate", "Crate", {-6.7f, 0.4f, -15.9f}, glm::vec3(0.8f), yawRotation(20));
    sb.block("Crate", "Crate", {-7.7f, 1.4f, -15.9f}, glm::vec3(0.8f), yawRotation(-10));
    // Planar mirror on the back wall (reflector plane = entity XZ: rotated to face +Z).
    Entity mirror = sb.prim("Mirror", Primitive::Cube, "Mirror", {2.0f, 2.6f, -16.9f}, {7.0f, 4.0f, 0.06f});
    Entity reflector = sb.create("MirrorPlane");
    reflector.setPosition({2.0f, 2.6f, -16.86f});
    reflector.setRotation(glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0)));
    auto& pr = reflector.add<render::PlanarReflectorComponent>();
    pr.size = {3.5f, 2.0f};
    pr.maxRoughness = 0.1f;
    (void)mirror;
    sb.block("MirrorFrame", "Gold", {2.0f, 4.7f, -16.9f}, {7.4f, 0.2f, 0.2f});
    sb.block("MirrorFrame", "Gold", {2.0f, 0.5f, -16.9f}, {7.4f, 0.2f, 0.2f});
    // Reflection probe of the hall (box projection).
    Entity probe = sb.create("HallProbe");
    probe.setPosition({0, 3.0f, -6.0f});
    auto& rp = probe.add<render::ReflectionProbeComponent>();
    rp.extents = {9.0f, 3.0f, 11.0f};
    rp.boxProjection = true;
    rp.update = render::ReflectionProbeUpdate::OnEnable;
    rp.resolution = 256;

    // GI room (Cornell box) to the right: red / green walls bleed onto a white floor through the irradiance volume.
    const glm::vec3 c(13.5f, 0, -6.0f);
    sb.block("CornellFloor", "White", c + glm::vec3(0, -0.25f, 0), {8, 0.5f, 8});
    sb.block("CornellRed", "RedWall", c + glm::vec3(-4.25f, 2.5f, 0), {0.5f, 5, 8});
    sb.block("CornellGreen", "GreenWall", c + glm::vec3(4.25f, 2.5f, 0), {0.5f, 5, 8});
    sb.block("CornellBack", "White", c + glm::vec3(0, 2.5f, -4.25f), {9, 5, 0.5f});
    sb.block("CornellRoof", "White", c + glm::vec3(0, 5.25f, 0), {9, 0.5f, 8.5f});
    sb.block("CornellBoxTall", "White", c + glm::vec3(-1.3f, 1.4f, -1.2f), {1.6f, 2.8f, 1.6f}, yawRotation(18));
    sb.block("CornellBoxShort", "White", c + glm::vec3(1.4f, 0.75f, 0.8f), {1.5f, 1.5f, 1.5f}, yawRotation(-17));
    sb.prim("CornellLamp", Primitive::Cube, "Emissive.White", c + glm::vec3(0, 4.97f, 0), {2.0f, 0.04f, 2.0f});
    sb.pointLight("CornellLight", c + glm::vec3(0, 4.6f, 0), {1.0f, 0.95f, 0.88f}, 9000.0f, 14.0f, true, 0.6f);
    Entity vol = sb.create("CornellIrradiance");
    vol.setPosition(c + glm::vec3(0, 2.5f, 0));
    auto& iv = vol.add<render::IrradianceVolumeComponent>();
    iv.extents = {4.0f, 2.5f, 4.0f};
    iv.probeCount = {6, 5, 6};
    Entity cprobe = sb.create("CornellProbe");
    cprobe.setPosition(c + glm::vec3(0, 2.5f, 0));
    auto& cp = cprobe.add<render::ReflectionProbeComponent>();
    cp.extents = {4.0f, 2.5f, 4.0f};
    cp.update = render::ReflectionProbeUpdate::OnEnable;
    Entity toggles = sb.create("ReflectionToggles");
    sb.script(toggles, "Scripts/render_toggles.lua");
    sb.prop(toggles, "mode", std::string("reflections"));
    // Mirror-floor statues: emissive cubes and spheres for SSR to pick up.
    sb.prim("GlowCube", Primitive::Cube, "Emissive.Magenta", {2.5f, 0.5f, -10.0f}, glm::vec3(1.0f), yawRotation(30));
    sb.prim("GlowCube", Primitive::Cube, "Emissive.Cyan", {4.5f, 0.35f, -6.5f}, glm::vec3(0.7f), yawRotation(10));
    sb.prim("WhiteSphere", Primitive::Sphere, "White", {-4.0f, 0.6f, -12.0f}, glm::vec3(1.2f));
    sb.tourCameras({4.0f, 2.0f, 7.0f}, {-1.0f, 1.0f, -9.0f}, {13.5f, 2.6f, 5.0f}, {13.5f, 1.6f, -5.0f}, 60.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 4. Волюметрика
void buildVolumetrics(Gen& gen) {
    const Station& s = station("volumetrics");
    SceneBuilder sb(gen, s.id);
    Entity sun = sb.sun({-0.6f, 0.35f, -0.3f}, 40000.0f, {1.0f, 0.85f, 0.65f}, 0.3f);
    Entity sky = timeOfDay(sb, sun, 18.1, 0.0, true, 3.2f);
    sb.script(sky, "Scripts/time_of_day.lua");
    sb.prop(sky, "dayNight", true);
    Entity env = sb.environment(sun);
    auto& e = env.get<EnvironmentComponent>();
    e.fogEnabled = true;
    e.fogDensity = 0.03f;
    e.fogHeightFalloff = 0.05f;
    e.fogColor = {0.75f, 0.8f, 0.9f};
    Entity clouds = sb.create("Clouds");
    auto& cl = clouds.add<render::CloudLayerComponent>();
    cl.coverage = 0.5f;
    cl.altitude = 1400.0f;
    cl.thickness = 1600.0f;
    Entity vf = sb.create("VolumetricFog");
    auto& v = vf.add<render::VolumetricFogComponent>();
    v.anisotropy = 0.6f;
    v.directionalIntensity = 8.0f;
    v.ambientIntensity = 0.05f; // the sky's ambient is not occluded inside the nave: keep it low so shafts dominate
    sb.postProcess();
    floorBlock(sb, "Tiles.x16", 80);
    sb.farGround();
    sb.stationBasics(s, {0, 0.05f, 20}, 0.0f, {3.5f, 0, 17}, -25.0f, {0, 0, 25}, 180.0f);

    // Nave: the west wall has tall narrow windows facing the evening sun, the east wall and the roof are solid,
    // so the interior stays dark and the haze shows distinct light shafts (god rays).
    const f32 len = 30.0f;
    for (int i = 0; i <= 10; ++i) sb.block("Pier", "Sandstone", {-6.0f, 5.0f, 5.0f - f32(i) * 3.0f}, {1.0f, 10.0f, 2.3f});
    sb.block("WindowSill", "Sandstone", {-6.0f, 1.0f, 5.0f - len * 0.5f}, {1.0f, 2.0f, len + 2.3f});
    sb.block("WindowHead", "Sandstone", {-6.0f, 8.75f, 5.0f - len * 0.5f}, {1.0f, 2.5f, len + 2.3f});
    sb.block("EastWall", "Sandstone", {6.0f, 5.0f, 5.0f - len * 0.5f}, {1.0f, 10.0f, len + 2.3f});
    sb.block("Roof", "Wood", {0, 10.25f, 5.0f - len * 0.5f}, {13.0f, 0.5f, len + 2.3f});
    for (int i = 0; i < 11; ++i) sb.block("RoofBeam", "Wood", {0, 9.8f, 5.0f - f32(i) * 3.0f}, {11.0f, 0.4f, 0.5f});
    sb.block("Apse", "Sandstone", {0, 5.0f, 5.0f - len - 0.5f}, {13.0f, 10.0f, 1.0f});
    sb.prim("RoseWindow", Primitive::Cylinder, "Stained.Red", {0, 7.0f, 5.0f - len}, {3.0f, 0.1f, 3.0f}, eulerDeg(90, 0, 0));
    sb.block("Altar", "Stone", {0, 0.6f, -22.0f}, {3.0f, 1.2f, 1.4f});
    // Local fog volumes: glowing mist orbs and a ground fog box.
    auto fog = [&](std::string_view name, glm::vec3 p, glm::vec3 ext, render::FogVolumeShape shape, f32 density, glm::vec3 albedo,
                   glm::vec3 emission, f32 noise) {
        Entity f = sb.create(name);
        f.setPosition(p);
        auto& fv = f.add<render::FogVolumeComponent>();
        fv.shape = shape;
        fv.extents = ext;
        fv.density = density;
        fv.albedo = albedo;
        fv.emission = emission;
        fv.noiseIntensity = noise;
        fv.noiseVelocity = {0.3f, 0.05f, 0.1f};
        return f;
    };
    fog("NaveHaze", {0, 5.0f, -10.0f}, {5.5f, 5.0f, 16.0f}, render::FogVolumeShape::Box, 0.15f, {1.0f, 0.95f, 0.9f}, {0, 0, 0}, 0.3f);
    fog("GroundMist", {0, 0.4f, -10.0f}, {5.0f, 0.6f, 15.0f}, render::FogVolumeShape::Box, 0.12f, {0.9f, 0.9f, 1.0f}, {0, 0, 0}, 0.7f);
    fog("SpiritOrb", {-2.5f, 2.2f, -18.0f}, {1.4f, 1.4f, 1.4f}, render::FogVolumeShape::Sphere, 0.8f, {0.3f, 0.6f, 1.0f}, {0.05f, 0.25f, 0.6f}, 0.5f);
    fog("SpiritOrb", {2.8f, 2.6f, -14.0f}, {1.2f, 1.2f, 1.2f}, render::FogVolumeShape::Sphere, 0.8f, {1.0f, 0.5f, 0.2f}, {0.6f, 0.2f, 0.05f}, 0.5f);
    sb.pointLight("OrbLight", {-2.5f, 2.2f, -18.0f}, {0.3f, 0.6f, 1.0f}, 3000.0f, 9.0f, false);
    sb.pointLight("OrbLight", {2.8f, 2.6f, -14.0f}, {1.0f, 0.5f, 0.2f}, 3000.0f, 9.0f, false);
    // Candles near the altar.
    for (int i = 0; i < 5; ++i) {
        const glm::vec3 p(-1.6f + f32(i) * 0.8f, 1.35f, -21.6f);
        sb.prim("Candle", Primitive::Cylinder, "OffWhite", p - glm::vec3(0, 0.12f, 0), {0.08f, 0.25f, 0.08f});
        sb.prim("CandleFlame", Primitive::Sphere, "Emissive.Warm", p + glm::vec3(0, 0.06f, 0), glm::vec3(0.06f));
    }
    sb.pointLight("AltarLight", {0, 1.8f, -21.3f}, {1.0f, 0.62f, 0.3f}, 1800.0f, 8.0f, true, 0.2f);
    sb.tourCameras({4.6f, 1.6f, 2.0f}, {-6.0f, 4.0f, -11.0f}, {12.0f, 4.0f, 14.0f}, {0.0f, 12.0f, -40.0f}, 64.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 5. Вода и частицы
void buildWater(Gen& gen) {
    const Station& s = station("water");
    SceneBuilder sb(gen, s.id);
    Entity sun = sb.sun({0.5f, 0.55f, -0.4f}, 50000.0f, {1.0f, 0.93f, 0.82f}, 0.3f);
    timeOfDay(sb, sun, 17.6, 0.0, true, 2.4f);
    sb.environment(sun);
    sb.postProcess(std::nullopt, 1.3f);
    sb.farGround(-6.0f); // sea floor beyond the basin
    // Shore (tiles) in the south, sandy basin to the north filled with water.
    sb.block("Shore", "Tiles.x8", {0, -0.25f, 14.0f}, {44, 0.5f, 16});
    sb.block("BasinFloor", "Terrain.sand", {0, -4.0f, -16.0f}, {44, 0.5f, 44});
    sb.block("BreakwaterE", "Stone", {22.0f, -1.5f, -12.0f}, {2.0f, 5.0f, 36});
    sb.block("BreakwaterW", "Stone", {-22.0f, -1.5f, -12.0f}, {2.0f, 5.0f, 36});
    sb.block("Lighthouse", "OffWhite", {22.0f, 4.0f, -29.0f}, {2.4f, 8.0f, 2.4f});
    sb.prim("LighthouseLamp", Primitive::Sphere, "Lamp", {22.0f, 8.6f, -29.0f}, glm::vec3(1.2f));
    sb.block("ShoreW", "Tiles.x8", {-38.0f, -0.25f, 0.0f}, {30, 0.5f, 60});
    sb.block("ShoreE", "Tiles.x8", {38.0f, -0.25f, 0.0f}, {30, 0.5f, 60});
    sb.block("Ramp", "Terrain.sand", {0, -2.0f, 3.5f}, {44, 0.5f, 9.0f}, glm::angleAxis(glm::radians(-24.0f), glm::vec3(1, 0, 0)));
    // Rocks + seaweed pillars under water for the caustics.
    for (int i = 0; i < 9; ++i) {
        const glm::vec3 p(-14.0f + f32(i) * 3.5f, -3.3f, -8.0f - f32((i * 5) % 7) * 2.5f);
        sb.prim("Rock", Primitive::Sphere, "Stone", p, {1.8f, 1.2f, 1.5f}, yawRotation(f32(i) * 40));
    }
    sb.stationBasics(s, {0, 0.05f, 16}, 0.0f, {4.0f, 0, 13.5f}, -25.0f, {0, 0, 21.5f}, 180.0f);
    Entity water = sb.create("Water");
    water.setPosition({0, -0.6f, -16.0f});
    auto& w = water.add<gameplay::WaterComponent>();
    w.size = {1600.0f, 1600.0f};
    w.windDirection = {0.7f, -0.7f};
    w.windSpeed = 5.5f;
    w.waveCount = 6;
    w.steepness = 0.45f;
    auto& ws = water.add<render::WaterSurfaceComponent>();
    ws.size = {1600.0f, 1600.0f};
    ws.causticsIntensity = 1.4f;
    // Pier.
    for (int i = 0; i < 6; ++i) {
        sb.block("PierPost", "Wood", {-6.0f, -1.5f, 2.0f - f32(i) * 2.5f}, {0.3f, 3.6f, 0.3f});
        sb.block("PierPost", "Wood", {-4.0f, -1.5f, 2.0f - f32(i) * 2.5f}, {0.3f, 3.6f, 0.3f});
    }
    sb.block("PierDeck", "Wood.x4", {-5.0f, 0.35f, -4.0f}, {2.6f, 0.15f, 14.0f});
    // Floating crates (buoyancy through gameplay::BuoyancyComponent + physics).
    for (int i = 0; i < 6; ++i) {
        const glm::vec3 p(2.0f + f32(i % 3) * 3.0f, 0.5f, -6.0f - f32(i / 3) * 3.5f);
        Entity crate = sb.body("FloatingCrate", Primitive::Cube, "Crate", p, glm::vec3(1.0f + 0.2f * f32(i % 2)), 180.0f,
                               yawRotation(f32(i) * 23));
        auto& bu = crate.add<gameplay::BuoyancyComponent>();
        bu.halfExtents = glm::vec3(0.5f);
        crate.get<gameplay::RigidBodyComponent>().angularDamping = 0.4f;
    }
    Entity barrel = sb.body("Barrel", Primitive::Cylinder, "Orange", {7.0f, 0.6f, -12.0f}, {0.9f, 1.3f, 0.9f}, 90.0f, eulerDeg(80, 0, 0));
    barrel.add<gameplay::BuoyancyComponent>().halfExtents = {0.45f, 0.65f, 0.45f};
    Entity spawner = sb.create("CrateSpawner");
    spawner.setPosition({3.0f, 4.0f, -8.0f});
    sb.script(spawner, "Scripts/crate_spawner.lua");

    // Campfire on the shore: fire (additive), smoke (lit, alpha), sparks (stretched, bouncing).
    const glm::vec3 fire(8.0f, 0, 9.0f);
    for (int i = 0; i < 7; ++i) {
        const f32 a = f32(i) / 7.0f * 2 * kPi;
        sb.prim("FireStone", Primitive::Sphere, "Stone", fire + glm::vec3(std::cos(a) * 0.9f, 0.12f, std::sin(a) * 0.9f), {0.5f, 0.35f, 0.45f});
    }
    sb.prim("Log", Primitive::Cylinder, "Bark", fire + glm::vec3(0, 0.15f, 0), {0.25f, 1.4f, 0.25f}, eulerDeg(0, 30, 80));
    sb.prim("Log", Primitive::Cylinder, "Bark", fire + glm::vec3(0, 0.15f, 0), {0.25f, 1.4f, 0.25f}, eulerDeg(0, -40, 80));
    sb.prim("Embers", Primitive::Sphere, "Lava", fire + glm::vec3(0, 0.08f, 0), {0.9f, 0.2f, 0.9f});
    {
        Entity f = sb.create("Fire");
        f.setPosition(fire + glm::vec3(0, 0.2f, 0));
        auto& p = f.add<render::ParticleEmitterComponent>();
        p.maxParticles = 2048;
        p.spawnRate = 220.0f;
        p.lifetime = {0.5f, 1.0f};
        p.shape = render::ParticleShape::Cone;
        p.radius = 0.35f;
        p.coneAngle = 12.0f;
        p.speed = {1.2f, 2.2f};
        p.gravity = {0, 1.5f, 0};
        p.gravityScale = 1.0f;
        p.turbulence = 2.5f;
        p.turbulenceFrequency = 2.0f;
        p.size = {0.35f, 0.6f};
        p.sizeOverLife = {{0.0f, 0.6f}, {0.3f, 1.0f}, {1.0f, 0.1f}};
        p.colorOverLife = {{0.0f, {1.0f, 0.85f, 0.4f, 1.0f}}, {0.4f, {1.0f, 0.45f, 0.1f, 0.9f}}, {1.0f, {0.6f, 0.08f, 0.02f, 0.0f}}};
        p.emissive = 6.0f;
        p.blend = render::ParticleBlend::Additive;
        p.sprite = render::ParticleSprite::Smoke;
        p.rotationSpeed = {-90.0f, 90.0f};
    }
    {
        Entity f = sb.create("Smoke");
        f.setPosition(fire + glm::vec3(0, 1.2f, 0));
        auto& p = f.add<render::ParticleEmitterComponent>();
        p.maxParticles = 1024;
        p.spawnRate = 28.0f;
        p.lifetime = {3.5f, 5.0f};
        p.shape = render::ParticleShape::Sphere;
        p.radius = 0.3f;
        p.speed = {0.6f, 1.0f};
        p.velocity = {0.3f, 0.8f, 0};
        p.turbulence = 0.8f;
        p.drag = 0.2f;
        p.size = {0.6f, 1.0f};
        p.sizeOverLife = {{0.0f, 0.5f}, {1.0f, 3.0f}};
        p.colorOverLife = {{0.0f, {0.25f, 0.24f, 0.23f, 0.0f}}, {0.15f, {0.3f, 0.29f, 0.28f, 0.5f}}, {1.0f, {0.45f, 0.45f, 0.45f, 0.0f}}};
        p.emissive = 0.0f;
        p.lit = true;
        p.blend = render::ParticleBlend::Alpha;
        p.sprite = render::ParticleSprite::Smoke;
        p.sort = true;
        p.rotationSpeed = {-20.0f, 20.0f};
    }
    {
        Entity f = sb.create("Sparks");
        f.setPosition(fire + glm::vec3(0, 0.3f, 0));
        auto& p = f.add<render::ParticleEmitterComponent>();
        p.maxParticles = 1024;
        p.spawnRate = 40.0f;
        p.bursts = {{0.0f, 40, 0, 1.7f}};
        p.lifetime = {0.8f, 1.8f};
        p.shape = render::ParticleShape::Cone;
        p.coneAngle = 35.0f;
        p.radius = 0.2f;
        p.speed = {2.5f, 5.0f};
        p.gravityScale = 0.6f;
        p.size = {0.03f, 0.05f};
        p.color = {1.0f, 0.6f, 0.2f, 1.0f};
        p.colorOverLife = {{0.0f, {1.0f, 0.9f, 0.5f, 1.0f}}, {1.0f, {1.0f, 0.3f, 0.05f, 0.0f}}};
        p.emissive = 12.0f;
        p.blend = render::ParticleBlend::Additive;
        p.renderMode = render::ParticleRenderMode::StretchedBillboard;
        p.stretch = 0.06f;
        p.sprite = render::ParticleSprite::Spark;
        p.collision = render::ParticleCollision::Bounce;
        p.bounce = 0.35f;
    }
    sb.pointLight("FireLight", fire + glm::vec3(0, 0.9f, 0), {1.0f, 0.55f, 0.2f}, 5000.0f, 12.0f, true, 0.3f);
    Entity fl = sb.create("FireFlicker");
    sb.script(fl, "Scripts/flicker.lua");
    sb.prop(fl, "light", std::string("FireLight"));
    audioLoop(sb, "FireSound", "Audio/fire_loop.wav", fire + glm::vec3(0, 0.5f, 0), 0.8f, true, "SFX", 18.0f);
    audioLoop(sb, "WaterSound", "Audio/water_loop.wav", {0, 0, -4.0f}, 0.6f, true, "Ambience", 40.0f);
    sb.tourCameras({14.5f, 3.4f, 14.5f}, {0.0f, -0.8f, -9.0f}, {0.0f, -2.2f, -6.0f}, {-2.0f, -3.2f, -18.0f}, 60.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 13. RTX и апскейлеры
void buildRtx(Gen& gen) {
    const Station& s = station("rtx");
    SceneBuilder sb(gen, s.id);
    Entity sun = sb.sun({-0.4f, 0.5f, 0.6f}, 30000.0f, {1.0f, 0.92f, 0.85f}, 0.5f);
    Entity env = sb.environment(sun, 1.0f, 1.0f);
    env.get<EnvironmentComponent>().skybox = gen.meta("Textures/Sky/desert.oxcube");
    env.get<EnvironmentComponent>().ldrSkyLuminance = 6000.0f;
    sb.postProcess(std::nullopt, 1.0f);
    floorBlock(sb, "PolishedFloor", 40);
    sb.stationBasics(s, {0, 0.05f, 13}, 0.0f, {-3.5f, 0, 10}, 25.0f, {0, 0, 18}, 180.0f);
    // Showroom: a stylised car out of clear-coat blocks, glass canopy, mirror and emissive ring.
    const glm::vec3 car(0, 0, -2.0f);
    sb.block("CarBody", "CarPaint.Red", car + glm::vec3(0, 0.7f, 0), {2.0f, 0.6f, 4.4f});
    sb.block("CarCabin", "Glass.Blue", car + glm::vec3(0, 1.3f, -0.3f), {1.7f, 0.6f, 2.0f});
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 w = car + glm::vec3(i % 2 ? 1.05f : -1.05f, 0.42f, i / 2 ? 1.4f : -1.4f);
        sb.prim("Wheel", Primitive::Cylinder, "Rubber", w, {0.84f, 0.32f, 0.84f}, eulerDeg(0, 0, 90));
        sb.prim("Rim", Primitive::Cylinder, "Chrome", w + glm::vec3(i % 2 ? 0.02f : -0.02f, 0, 0), {0.5f, 0.3f, 0.5f}, eulerDeg(0, 0, 90));
    }
    sb.prim("Headlight", Primitive::Sphere, "Emissive.White", car + glm::vec3(0.6f, 0.75f, 2.2f), glm::vec3(0.22f));
    sb.prim("Headlight", Primitive::Sphere, "Emissive.White", car + glm::vec3(-0.6f, 0.75f, 2.2f), glm::vec3(0.22f));
    sb.prim("Turntable", Primitive::Cylinder, "BrushedSteel", car + glm::vec3(0, 0.05f, 0), {7.0f, 0.1f, 7.0f});
    for (int i = 0; i < 5; ++i) sb.prim("WallStrip", Primitive::Cube, "Neon.Blue", {-8.0f + f32(i) * 4.0f, 4.8f, -9.7f}, {3.0f, 0.12f, 0.05f});
    sb.block("BackWall", "DarkGrey", {0, 3.0f, -10.0f}, {20, 6, 0.5f});
    sb.prim("GlassSphere", Primitive::Sphere, "Glass", {4.5f, 0.9f, 0.5f}, glm::vec3(1.8f));
    sb.prim("MirrorBall", Primitive::Sphere, "Mirror", {-4.5f, 0.9f, 0.5f}, glm::vec3(1.8f));
    sb.block("MirrorWall", "Mirror", {-8.0f, 2.0f, -4.0f}, {0.2f, 4.0f, 6.0f}, yawRotation(25));
    sb.block("RedPanel", "RedWall", {8.0f, 2.0f, -4.0f}, {0.2f, 4.0f, 6.0f}, yawRotation(-25));
    for (int i = 0; i < 6; ++i)
        sb.spotLight("ShowSpot", {-7.5f + f32(i) * 3.0f, 5.5f, 4.0f}, car + glm::vec3(0, 0.6f, 0), {1.0f, 0.95f, 0.9f}, 3500.0f, 16.0f, 12.0f, 26.0f, i % 2 == 0, 0.1f);
    Entity panel = sb.create("RtxPanel");
    sb.script(panel, "Scripts/rtx_panel.lua");
    sb.tourCameras({6.5f, 2.6f, 7.5f}, {0.0f, 0.9f, -2.0f}, {-5.0f, 1.2f, 4.5f}, {0.0f, 0.9f, -2.0f}, 50.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

} // namespace ox::showcase::gen
