// Gameplay stations: 6 world, 7 physics, 8 animation, 9 AI, 10 splines & coroutines, 11 audio, 12 network, 14 saves.
#include "gen.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/gameplay/world/components.hpp>
#include <oxwald/render/components/reflections.hpp>
#include <oxwald/render/components/world.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/spline/spline.hpp>

#include <cmath>
#include <numbers>

namespace ox::showcase::gen {

namespace {
constexpr f32 kPi = std::numbers::pi_v<f32>;

Entity sunAndSky(SceneBuilder& sb, glm::vec3 dir, f32 lux, f64 hours = -1.0) {
    Entity sun = sb.sun(dir, lux, {1.0f, 0.94f, 0.86f}, 0.35f);
    Entity sky = sb.create("Sky");
    sky.add<gameplay::SkyComponent>();
    if (hours >= 0.0) {
        auto& tod = sky.add<gameplay::TimeOfDayComponent>();
        tod.localHours = hours;
        tod.timeScale = 0.0;
        tod.paused = true;
        tod.sun = sun.ref();
        tod.month = 7;
        tod.day = 12;
    } else {
        sky.get<gameplay::SkyComponent>().sunDirection = glm::normalize(dir);
    }
    sb.environment(sun);
    sb.farGround();
    return sun;
}

Entity audioSource(SceneBuilder& sb, std::string_view name, const std::string& clip, glm::vec3 pos, f32 volume,
                   bool loop, const std::string& bus, bool spatial = true, f32 maxDistance = 30.0f, bool occlusion = false,
                   Entity parent = {}) {
    Entity e = sb.create(name, parent);
    e.setPosition(pos);
    auto& a = e.add<gameplay::AudioSourceComponent>();
    a.clip = sb.gen.meta(clip);
    a.loop = loop;
    a.playOnStart = loop;
    a.spatial = spatial;
    a.bus = bus;
    a.volume = volume;
    a.minDistance = 1.5f;
    a.maxDistance = maxDistance;
    a.occlusion = occlusion;
    a.distanceLowPass = spatial;
    return e;
}

// Mannequin (skinned glTF): Animator on `root`, skinned mesh on a child turned to face the root's -Z.
struct MannequinAssets {
    Uuid skeleton, mesh, idle, walk, run;
    bool valid = false;
};

MannequinAssets mannequinAssets(Gen& gen) {
    MannequinAssets m;
    auto sk = gen.imported(std::string(kMannequin) + "#Skeleton");
    auto mesh = gen.imported(std::string(kMannequin) + "#Mesh/0");
    auto c0 = gen.imported(std::string(kMannequin) + "#Clip/0");
    auto c1 = gen.imported(std::string(kMannequin) + "#Clip/1");
    auto c2 = gen.imported(std::string(kMannequin) + "#Clip/2");
    if (!sk || !mesh || !c0 || !c1 || !c2) {
        OX_LOG_WARN("generate", "mannequin sub-assets missing (skeleton {}, mesh {}, clips {}/{}/{})", bool(sk), bool(mesh), bool(c0),
                    bool(c1), bool(c2));
        return m;
    }
    m = {*sk, *mesh, *c0, *c1, *c2, true};
    return m;
}

Entity mannequin(SceneBuilder& sb, const MannequinAssets& a, std::string_view name, glm::vec3 pos, f32 yaw,
                 const std::string& material, f32 speedParam = 0.0f, Entity parent = {}) {
    Entity root = sb.create(name, parent);
    root.setPosition(pos);
    root.setRotation(yawRotation(yaw));
    if (!a.valid) {
        sb.prim("Body", Primitive::Capsule, material, {0, 0.9f, 0}, {0.6f, 1.8f, 0.6f}, {}, root);
        return root;
    }
    auto& an = root.add<gameplay::AnimatorComponent>();
    an.skeleton = a.skeleton;
    auto& c = an.inlineController;
    c.parameters = {{"speed", anim::ParamType::Float, speedParam}};
    // One 1D blend space state: idle (0 m/s) → walk (1.4 m/s) → run (4.5 m/s) weighted by "speed".
    gameplay::AnimatorStateDesc loco;
    loco.name = "Locomotion";
    loco.blendParameter = "speed";
    loco.blendSamples = {{a.idle, 0.0f}, {a.walk, 1.4f}, {a.run, 4.5f}};
    c.states = {loco};
    c.defaultState = "Locomotion";
    an.parameters["speed"] = speedParam;
    an.animateInEditMode = true;
    // The glTF figure faces +Z; engine forward is -Z.
    Entity skin = sb.create("Skin", root);
    skin.setRotation(yawRotation(180));
    auto& sm = skin.add<gameplay::SkinnedMeshComponent>();
    sm.mesh = a.mesh;
    sm.materials = {sb.gen.mat(material)};
    return root;
}

void writePrefabDoc(Gen& gen, const std::string& rel, World& w, Entity root) {
    gen.writeAsset(rel, stablePrefab(w, root, rel));
    gen.meta(rel);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// 6. Открытый мир
void buildWorld(Gen& gen) {
    const Station& s = station("world");
    SceneBuilder sb(gen, s.id);
    Entity sun = sb.sun({0.4f, 0.5f, -0.5f}, 60000.0f, {1.0f, 0.95f, 0.88f}, 0.3f);
    Entity sky = sb.create("Sky");
    sky.add<gameplay::SkyComponent>();
    auto& tod = sky.add<gameplay::TimeOfDayComponent>();
    tod.localHours = 9.5;
    tod.timeScale = 0.0;
    tod.paused = true;
    tod.sun = sun.ref();
    sb.environment(sun);
    sb.postProcess();
    Entity wind = sb.create("Wind");
    auto& wc = wind.add<gameplay::WindComponent>();
    wc.direction = {0.8f, 0.6f};
    wc.speed = 6.0f;
    wc.gustStrength = 0.6f;

    Entity terrain = sb.create("Terrain");
    auto& t = terrain.add<gameplay::TerrainComponent>();
    t.source = gameplay::TerrainSource::Procedural;
    t.resolution = 513;
    t.worldSize = 768.0f;
    t.heightScale = 90.0f;
    t.heightOffset = -12.0f;
    t.noise.fractal.basis = world::NoiseBasis::Simplex;
    t.noise.fractal.type = world::FractalType::Ridged;
    t.noise.fractal.seed = 21;
    t.noise.fractal.frequency = 1.0f / 420.0f;
    t.noise.fractal.octaves = 6;
    t.noise.fractal.warpStrength = 60.0f;
    t.noise.exponent = 1.6f;
    t.hydraulicErosion = true;
    t.hydraulic.droplets = 60000;
    t.layers = {gen.mat("Terrain.grass"), gen.mat("Terrain.rock"), gen.mat("Terrain.sand"), gen.mat("Terrain.snow")};
    t.splatRules = {
        {.layer = 2, .maxHeight = -8.0f, .heightBlend = 4.0f},
        {.layer = 1, .minSlopeDeg = 32.0f, .maxSlopeDeg = 90.0f, .slopeBlendDeg = 6.0f, .noiseAmount = 0.3f},
        {.layer = 3, .minHeight = 55.0f, .heightBlend = 10.0f, .maxSlopeDeg = 40.0f, .noiseAmount = 0.4f},
    };
    t.lod = {.leafNodeSize = 32, .lodCount = 5, .viewDistance = 2500.0f};
    terrain.add<render::TerrainRenderComponent>();
    sb.farGround(-13.0f);
    auto& veg = terrain.add<gameplay::VegetationComponent>();
    world::VegetationLayer trees{.name = "pines", .kind = world::VegetationKind::Tree, .prototype = 0, .seed = 3, .minDistance = 7.0f,
                                 .density = 0.8f, .minHeight = -6.0f, .maxHeight = 60.0f, .maxSlopeDeg = 28.0f, .splatLayer = 0,
                                 .minScale = 0.8f, .maxScale = 1.4f, .boundingRadius = 5.0f, .collider = true,
                                 .colliderRadius = 0.35f, .colliderHalfHeight = 3.0f};
    trees.lod.impostorDistance = 140.0f;
    trees.lod.cullDistance = 900.0f;
    world::VegetationLayer bushes{.name = "bushes", .kind = world::VegetationKind::Detail, .prototype = 2, .seed = 5,
                                  .minDistance = 3.0f, .density = 0.6f, .maxHeight = 50.0f, .maxSlopeDeg = 30.0f, .splatLayer = 0,
                                  .boundingRadius = 1.2f};
    bushes.lod.cullDistance = 160.0f;
    bushes.lod.impostorDistance = 0.0f;
    world::VegetationLayer grass{.name = "grass", .kind = world::VegetationKind::Grass, .prototype = 1, .seed = 7, .minDistance = 0.45f,
                                 .density = 1.0f, .maxHeight = 45.0f, .maxSlopeDeg = 30.0f, .splatLayer = 0, .alignToNormal = 0.8f,
                                 .boundingRadius = 0.5f};
    grass.lod.cullDistance = 70.0f;
    grass.lod.impostorDistance = 0.0f;
    veg.layers = {trees, grass, bushes};
    veg.scatterRadius = 320.0f;
    // Streaming: survey markers in chunk prefabs (Assets/Chunks/chunk_x_z.oxprefab) load around streaming sources.
    Entity streaming = sb.create("Streaming");
    auto& ws = streaming.add<gameplay::WorldStreamingComponent>();
    ws.settings.chunkSize = 96.0f;
    ws.settings.loadRadius = 150.0f;
    ws.settings.unloadRadius = 200.0f;
    ws.prefabPattern = "Chunks/chunk_{x}_{z}";
    for (int z = -4; z < 4; ++z)
        for (int x = -4; x < 4; ++x) {
            World w;
            Entity root = w.create("Chunk " + std::to_string(x) + "," + std::to_string(z));
            // A tall beacon (visible from any terrain height) + a lamp ring around it.
            const glm::vec3 c(48.0f, 0, 48.0f);
            Entity beacon = w.create("Beacon", root);
            beacon.setPosition(c + glm::vec3(0, 40.0f, 0));
            beacon.setScale({0.35f, 80.0f, 0.35f});
            auto& mr = beacon.add<MeshRendererComponent>();
            mr.mesh = render::primitiveUuid(Primitive::Cylinder);
            mr.materials = {gen.mat((x + z) % 2 ? "Emissive.Cyan" : "Emissive.Yellow")};
            mr.castShadows = false;
            Entity cap = w.create("BeaconLight", root);
            cap.setPosition(c + glm::vec3(0, 82.0f, 0));
            auto& l = cap.add<LightComponent>();
            l.type = LightType::Point;
            l.color = (x + z) % 2 ? glm::vec3(0.2f, 0.9f, 1.0f) : glm::vec3(1.0f, 0.85f, 0.2f);
            l.intensity = 40000.0f;
            l.range = 60.0f;
            l.castShadows = false;
            writePrefabDoc(gen, "Chunks/chunk_" + std::to_string(x) + "_" + std::to_string(z) + ".oxprefab", w, root);
        }
    Entity debug = sb.create("StreamingDebug");
    sb.script(debug, "Scripts/world_station.lua");

    Entity g = sb.game(s);
    (void)g;
    Entity player = sb.player({0, 40.0f, 20.0f}, 0.0f);
    player.add<gameplay::StreamingSourceComponent>();
    sb.prop(player, "snapToTerrain", true);
    sb.infoBoard(s, {-3.0f, 40.0f, 16.0f}, 25.0f);
    sb.portal("HubPortal", kHubUri, "hub", {0.6f, 0.8f, 1.0f}, {3.0f, 40.0f, 25.0f}, 180.0f);
    for (const char* n : {"InfoBoard", "HubPortal"}) {
        Entity e = sb.world.findByName(n);
        auto& sc = e.add<gameplay::ScriptComponent>();
        sc.script = "Scripts/snap_to_terrain.lua";
    }
    sb.tourCameras({-120.0f, 95.0f, 160.0f}, {30.0f, 0.0f, -60.0f}, {60.0f, 60.0f, 60.0f}, {-40.0f, 10.0f, -100.0f}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 7. Физика
void buildPhysics(Gen& gen) {
    const Station& s = station("physics");
    SceneBuilder sb(gen, s.id);
    sunAndSky(sb, {0.5f, 0.6f, 0.3f}, 50000.0f, 14.5);
    sb.postProcess();
    sb.block("Floor", "Grid.x16", {0, -0.25f, 0}, {60, 0.5f, 60});
    sb.stationBasics(s, {0, 0.05f, 16}, 0.0f, {-3.5f, 0, 13}, 30.0f, {0, 0, 20.5f}, 180.0f);
    Entity game = sb.world.findByName("Game");
    (void)game;
    // Box towers.
    Entity towers = sb.create("Towers");
    sb.script(towers, "Scripts/physics_station.lua");
    const char* mats[] = {"Crate", "Orange", "Teal", "Crate", "Yellow"};
    for (int t = 0; t < 3; ++t) {
        const f32 x = -8.0f + f32(t) * 4.0f;
        for (int y = 0; y < 7; ++y)
            for (int i = 0; i < 2; ++i) {
                const bool alongX = y % 2 == 0;
                const glm::vec3 p = alongX ? glm::vec3(x + (f32(i) - 0.5f) * 1.02f, 0.5f + f32(y) * 1.0f, -6.0f)
                                           : glm::vec3(x, 0.5f + f32(y) * 1.0f, -6.0f + (f32(i) - 0.5f) * 1.02f);
                Entity b = sb.body("TowerBlock", Primitive::Cube, mats[(t + y) % 5], p, alongX ? glm::vec3(1.0f, 1.0f, 2.0f) : glm::vec3(2.0f, 1.0f, 1.0f), 30.0f);
                b.add<SaveGameComponent>();
                sb.tag(b, "TowerBlock");
            }
    }
    // Pyramid of spheres and loose balls to push around.
    for (int i = 0; i < 10; ++i)
        sb.body("Ball", Primitive::Sphere, i % 2 ? "Rubber" : "Orange", {4.0f + f32(i % 5) * 1.1f, 0.5f, 2.0f + f32(i / 5) * 1.1f}, glm::vec3(0.9f), 8.0f);
    // Chains hanging from a gantry (point joints between capsule links).
    sb.block("GantryPostL", "DarkMetal", {5.0f, 3.5f, -8.0f}, {0.4f, 7.0f, 0.4f});
    sb.block("GantryPostR", "DarkMetal", {13.0f, 3.5f, -8.0f}, {0.4f, 7.0f, 0.4f});
    Entity beam = sb.block("GantryBeam", "DarkMetal", {9.0f, 7.0f, -8.0f}, {8.4f, 0.4f, 0.4f});
    for (int c = 0; c < 3; ++c) {
        const f32 x = 6.5f + f32(c) * 2.5f;
        Entity prev = beam;
        glm::vec3 prevAnchorWorld(x, 6.8f, -8.0f);
        const int links = 8;
        for (int l = 0; l < links; ++l) {
            const glm::vec3 p(x, 6.5f - f32(l) * 0.62f, -8.0f);
            Entity link = sb.body("ChainLink", Primitive::Capsule, l == links - 1 ? "Gold" : "Steel", p, {0.22f, 0.62f, 0.22f}, 4.0f);
            auto& j = link.add<gameplay::JointComponent>();
            j.type = physics::ConstraintType::Point;
            j.target = prev.ref();
            j.anchor = {0, 0.5f, 0}; // local (scaled shape: half height of the 0.62 m link)
            j.targetAnchor = prev == beam ? glm::vec3((x - 9.0f) / 8.4f, -0.5f, 0) : glm::vec3(0, -0.5f, 0);
            prev = link;
            (void)prevAnchorWorld;
        }
        if (c == 1) { // a heavy wrecking ball at the end of the middle chain
            Entity ball = sb.body("WreckingBall", Primitive::Sphere, "DarkMetal", {x, 6.5f - f32(links) * 0.62f - 0.4f, -8.0f}, glm::vec3(1.0f), 60.0f);
            auto& j = ball.add<gameplay::JointComponent>();
            j.type = physics::ConstraintType::Point;
            j.target = prev.ref();
            j.anchor = {0, 0.5f, 0};
            j.targetAnchor = {0, -0.5f, 0};
        }
    }
    // Hinged gate with a motor and a seesaw.
    Entity post = sb.block("GatePost", "Stone", {-12.0f, 1.5f, 4.0f}, {0.4f, 3.0f, 0.4f});
    Entity gate = sb.body("Gate", Primitive::Cube, "Wood", {-10.5f, 1.5f, 4.0f}, {2.6f, 2.6f, 0.15f}, 40.0f);
    auto& hinge = gate.add<gameplay::JointComponent>();
    hinge.type = physics::ConstraintType::Hinge;
    hinge.target = post.ref();
    hinge.anchor = {-0.5f, 0, 0};
    hinge.targetAnchor = {0.5f, 0, 0};
    hinge.axis = {0, 1, 0};
    hinge.motorMode = physics::MotorMode::Velocity;
    hinge.motorTarget = 0.8f;
    hinge.motorMaxForce = 400.0f;
    Entity pivot = sb.block("SeesawPivot", "Stone", {-10.0f, 0.4f, -2.0f}, {0.6f, 0.8f, 1.2f});
    Entity plank = sb.body("Seesaw", Primitive::Cube, "Wood", {-10.0f, 0.9f, -2.0f}, {6.0f, 0.15f, 1.0f}, 20.0f);
    auto& sj = plank.add<gameplay::JointComponent>();
    sj.type = physics::ConstraintType::Hinge;
    sj.target = pivot.ref();
    sj.anchor = {0, 0, 0};
    sj.targetAnchor = {0, 0.6f, 0};
    sj.axis = {0, 0, 1};
    sj.limitsEnabled = true;
    sj.limitMin = -0.35f;
    sj.limitMax = 0.35f;
    sb.body("SeesawLoad", Primitive::Cube, "Orange", {-12.2f, 1.6f, -2.0f}, glm::vec3(0.8f), 30.0f);
    // Trigger pad: launches whatever enters it (Lua onTriggerEnter + addImpulse).
    Entity pad = sb.prim("LaunchPad", Primitive::Cylinder, "Emissive.Green", {8.0f, 0.05f, 6.0f}, {2.6f, 0.1f, 2.6f});
    Entity padTrigger = sb.create("LaunchTrigger", pad);
    auto& tc = padTrigger.add<gameplay::ColliderComponent>();
    tc.type = gameplay::ColliderType::Box;
    tc.halfExtents = {0.5f, 8.0f, 0.5f};
    tc.isSensor = true;
    padTrigger.add<gameplay::TriggerComponent>();
    sb.script(padTrigger, "Scripts/launch_pad.lua");
    // Ray gun range targets.
    for (int i = 0; i < 5; ++i)
        sb.body("Target", Primitive::Cylinder, i % 2 ? "RedWall" : "OffWhite", {-4.0f + f32(i) * 2.0f, 0.6f, -14.0f}, {0.8f, 1.2f, 0.8f}, 6.0f);
    Entity gun = sb.create("RayGun");
    sb.script(gun, "Scripts/raygun.lua");
    sb.tourCameras({14.0f, 6.5f, 12.0f}, {0.0f, 2.0f, -6.0f}, {-2.0f, 2.2f, 6.0f}, {9.0f, 3.0f, -8.0f}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 8. Анимация
void buildAnimation(Gen& gen) {
    const Station& s = station("animation");
    SceneBuilder sb(gen, s.id);
    sunAndSky(sb, {-0.4f, 0.55f, 0.5f}, 45000.0f, 15.5);
    sb.postProcess();
    sb.block("Floor", "Tiles.x8", {0, -0.25f, 0}, {48, 0.5f, 48});
    sb.stationBasics(s, {0, 0.05f, 14}, 0.0f, {3.5f, 0, 11}, -30.0f, {0, 0, 18.5f}, 180.0f);
    const MannequinAssets ma = mannequinAssets(gen);

    // Walker on a circular track: speed -> blend between idle/walk/run (keys 1–3 change the target speed).
    Entity track = sb.create("WalkTrack");
    track.setPosition({-6.0f, 0.02f, -2.0f});
    auto& sp = track.add<gameplay::SplineComponent>();
    sp.type = spline::SplineType::CatmullRom;
    sp.closed = true;
    for (int i = 0; i < 8; ++i) {
        const f32 a = f32(i) / 8.0f * 2 * kPi;
        sp.points.push_back({.position = {std::cos(a) * 4.5f, 0, std::sin(a) * 4.5f}});
    }
    sp.drawInGame = false;
    for (int i = 0; i < 36; ++i) {
        const f32 a = f32(i) / 36.0f * 2 * kPi;
        sb.prim("TrackMark", Primitive::Cylinder, "Hazard", {-6.0f + std::cos(a) * 4.5f, 0.01f, -2.0f + std::sin(a) * 4.5f}, {0.25f, 0.02f, 0.25f});
    }
    Entity walker = mannequin(sb, ma, "Walker", {-1.5f, 0, -2.0f}, 0.0f, "Mannequin", 1.4f);
    auto& f = walker.add<gameplay::SplineFollowerComponent>();
    f.spline = track.ref();
    f.speed = 1.4f;
    f.orientToPath = true;
    f.forwardAxis = {0, 0, -1};
    sb.script(walker, "Scripts/locomotion.lua");

    // Stairs with an IK-planted mannequin: two-bone leg chains reach targets on different steps.
    const glm::vec3 stairs(6.0f, 0, -4.0f);
    for (int i = 0; i < 6; ++i) sb.block("Step", "Concrete", stairs + glm::vec3(0, 0.1f + f32(i) * 0.2f, -f32(i) * 0.35f), {3.0f, 0.2f + f32(i) * 0.4f, 0.35f + 0.0f});
    Entity climber = mannequin(sb, ma, "StairClimber", stairs + glm::vec3(0, 0.18f, -0.62f), 0.0f, "Mannequin.Blue", 0.0f);
    Entity footL = sb.create("FootTarget.L");
    footL.setPosition(stairs + glm::vec3(0.12f, 0.46f, -0.85f));
    Entity footR = sb.create("FootTarget.R");
    footR.setPosition(stairs + glm::vec3(-0.12f, 0.27f, -0.48f));
    if (ma.valid) {
        auto& ik = climber.add<gameplay::IKComponent>();
        ik.chains.push_back({.type = gameplay::IKChainType::TwoBone, .rootJoint = "UpperLeg.L", .midJoint = "LowerLeg.L", .endJoint = "Foot.L",
                             .target = footL.ref(), .poleOffset = {0, 0, -1}});
        ik.chains.push_back({.type = gameplay::IKChainType::TwoBone, .rootJoint = "UpperLeg.R", .midJoint = "LowerLeg.R", .endJoint = "Foot.R",
                             .target = footR.ref(), .poleOffset = {0, 0, -1}});
        ik.chains.push_back({.type = gameplay::IKChainType::Aim, .endJoint = "Head", .target = sb.world.findByName("Player").ref(),
                             .targetOffset = {0, 1.6f, 0}, .aimAxis = {0, 0, 1}, .weight = 0.8f});
    }
    // Look-at crowd: idle mannequins turning their heads to the player.
    for (int i = 0; i < 3; ++i) {
        Entity m = mannequin(sb, ma, "Watcher", {2.0f + f32(i) * 1.6f, 0, 3.0f - f32(i) * 0.4f}, 200.0f - f32(i) * 15.0f,
                             i == 1 ? "Mannequin.Grey" : "Mannequin", 0.0f);
        if (ma.valid) {
            auto& ik = m.add<gameplay::IKComponent>();
            ik.chains.push_back({.type = gameplay::IKChainType::Aim, .endJoint = "Head", .target = sb.world.findByName("Player").ref(),
                                 .targetOffset = {0, 1.6f, 0}, .aimAxis = {0, 0, 1}, .weight = 0.9f});
        }
    }
    // A runner on the outer path.
    Entity outer = sb.create("RunTrack");
    outer.setPosition({0, 0.02f, -2.0f});
    auto& op = outer.add<gameplay::SplineComponent>();
    op.type = spline::SplineType::CatmullRom;
    op.closed = true;
    for (int i = 0; i < 10; ++i) {
        const f32 a = f32(i) / 10.0f * 2 * kPi;
        op.points.push_back({.position = {std::cos(a) * 14.0f, 0, std::sin(a) * 9.0f}});
    }
    Entity runner = mannequin(sb, ma, "Runner", {14.0f, 0, -2.0f}, 0.0f, "Mannequin.Grey", 4.5f);
    auto& rf = runner.add<gameplay::SplineFollowerComponent>();
    rf.spline = outer.ref();
    rf.speed = 4.5f;
    rf.forwardAxis = {0, 0, -1};
    sb.script(runner, "Scripts/locomotion.lua");
    sb.prop(runner, "fixedSpeed", 4.5);
    sb.tourCameras({0.5f, 2.0f, 8.0f}, {1.0f, 1.0f, -2.0f}, {9.0f, 1.6f, 0.5f}, {6.0f, 1.0f, -4.5f}, 50.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 9. ИИ
void buildAI(Gen& gen) {
    const Station& s = station("ai");
    SceneBuilder sb(gen, s.id);
    sunAndSky(sb, {0.3f, 0.45f, -0.6f}, 35000.0f, 16.0);
    sb.postProcess();
    // The navmesh is baked from the arena only (onlyChildren): the endless far ground would blow up the bounds.
    Entity nav = sb.create("NavMesh");
    auto& ns = nav.add<gameplay::NavMeshSurfaceComponent>();
    ns.agentRadius = 0.45f;
    ns.agentHeight = 1.9f;
    ns.cellSize = 0.25f;
    ns.bakeOnStart = true;
    ns.drawInEditor = true;
    ns.onlyChildren = true;
    sb.block("Floor", "Concrete.x8", {0, -0.25f, -4.0f}, {50, 0.5f, 50}, {}, nav);
    sb.stationBasics(s, {0, 0.05f, 17}, 0.0f, {-3.5f, 0, 14}, 30.0f, {0, 0, 21}, 180.0f);
    // Arena walls and cover.
    sb.block("WallN", "Concrete.Warm", {0, 1.5f, -20.0f}, {36, 3, 0.6f}, {}, nav);
    sb.block("WallW", "Concrete.Warm", {-18.0f, 1.5f, -4.0f}, {0.6f, 3, 32}, {}, nav);
    sb.block("WallE", "Concrete.Warm", {18.0f, 1.5f, -4.0f}, {0.6f, 3, 32}, {}, nav);
    sb.block("WallS1", "Concrete.Warm", {-11.0f, 1.5f, 12.0f}, {14, 3, 0.6f}, {}, nav);
    sb.block("WallS2", "Concrete.Warm", {11.0f, 1.5f, 12.0f}, {14, 3, 0.6f}, {}, nav);
    const glm::vec3 cover[] = {{-8, 0, -4}, {7, 0, -9}, {0, 0, -13}, {-10, 0, -14}, {10, 0, 2}, {-4, 0, 4}, {4, 0, -2}};
    for (usize i = 0; i < std::size(cover); ++i)
        sb.block("Cover", i % 2 ? "Crate" : "DarkMetal", cover[i] + glm::vec3(0, 1.0f, 0),
                 i % 3 ? glm::vec3(3.0f, 2.0f, 1.0f) : glm::vec3(1.2f, 2.0f, 4.0f), {}, nav);
    Entity aiDebug = sb.create("AIDebug");
    sb.script(aiDebug, "Scripts/ai_station.lua");

    // The player is perceivable (team 2).
    Entity player = sb.world.findByName("Player");
    auto& pp = player.add<gameplay::PerceptionComponent>();
    pp.team = 2;
    pp.listener = false;
    pp.source = true;

    const MannequinAssets ma = mannequinAssets(gen);
    const Uuid tree = gen.meta("AI/guard.oxbt");
    const char* routes[] = {"-14,-16;-14,6;-6,8;-6,-16", "14,-16;14,6;6,-6;12,-12", "-4,-17;6,-17;8,-12;-2,-8"};
    const glm::vec3 starts[] = {{-14, 0, -16}, {14, 0, -16}, {-4, 0, -17}};
    for (int i = 0; i < 3; ++i) {
        Entity guard = mannequin(sb, ma, "Guard", starts[i], 0.0f, "Guard", 0.0f);
        auto& ag = guard.add<gameplay::NavAgentComponent>();
        ag.maxSpeed = 2.2f;
        ag.radius = 0.4f;
        ag.height = 1.8f;
        ag.driveCharacterController = false;
        auto& bt = guard.add<gameplay::BehaviorTreeComponent>();
        bt.tree = tree;
        auto& pc = guard.add<gameplay::PerceptionComponent>();
        pc.team = 1;
        pc.sight.range = 12.0f;
        pc.sight.loseSightRange = 15.0f;
        pc.sight.fovDegrees = 100.0f;
        pc.sight.forgetAfter = 10.0f;
        sb.script(guard, "Scripts/guard.lua");
        sb.prop(guard, "route", std::string(routes[i]));
        // Vision cone (translucent) + eye lamp coloured by the guard's state.
        // Cone primitive: apex at +Y; rotated so the apex sits at the eyes and the base 12 m ahead (-Z).
        Entity cone = sb.prim("VisionCone", Primitive::Cone, "VisionCone", {0, 1.6f, -4.0f}, {9.0f, 8.0f, 3.0f},
                              glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0)), guard);
        cone.get<MeshRendererComponent>().castShadows = false;
        sb.pointLight("EyeLight", {0, 1.75f, -0.3f}, {0.2f, 1.0f, 0.3f}, 400.0f, 4.0f, false, 0.05f, guard);
    }
    sb.tourCameras({9.0f, 9.0f, 9.0f}, {-4.0f, 0.0f, -10.0f}, {-14.0f, 3.0f, 8.0f}, {0.0f, 1.0f, -10.0f}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 10. Сплайны и корутины
void buildSplines(Gen& gen) {
    const Station& s = station("splines");
    SceneBuilder sb(gen, s.id);
    sunAndSky(sb, {0.5f, 0.5f, 0.4f}, 50000.0f, 11.0);
    sb.postProcess();
    sb.block("Floor", "Grid.x16", {0, -0.25f, -4.0f}, {64, 0.5f, 56});
    sb.stationBasics(s, {0, 0.05f, 18}, 0.0f, {-3.5f, 0, 15}, 30.0f, {0, 0, 22.5f}, 180.0f);

    // Railway: closed Catmull-Rom loop; sleepers + rails placed by sampling the same curve here.
    std::vector<glm::vec3> pts = {{-20, 0, -4}, {-16, 0, -16}, {-4, 0, -20}, {10, 0, -18}, {20, 0, -10},
                                  {19, 0, 4},   {10, 0, 10},   {0, 0, 8},    {-10, 0, 10}, {-19, 0, 6}};
    Entity track = sb.create("Track");
    track.setPosition({0, 0.0f, 0});
    auto& sp = track.add<gameplay::SplineComponent>();
    sp.type = spline::SplineType::CatmullRom;
    sp.closed = true;
    spline::Spline curve(spline::SplineType::CatmullRom);
    for (const glm::vec3& p : pts) {
        sp.points.push_back({.position = p});
        curve.addPoint(p);
    }
    curve.setClosed(true);
    sp.markers = {{"Station", 7.0f}};
    const f32 length = curve.length();
    const int sleepers = int(length / 1.2f);
    for (int i = 0; i < sleepers; ++i) {
        const auto smp = curve.evaluateAtDistance(f32(i) * length / f32(sleepers));
        const glm::quat q = smp.rotation();
        sb.prim("Sleeper", Primitive::Cube, "Wood", smp.position + glm::vec3(0, 0.05f, 0), {2.2f, 0.1f, 0.35f}, q);
    }
    const int railSegs = int(length / 1.0f);
    for (int i = 0; i < railSegs; ++i) {
        const auto a = curve.evaluateAtDistance(f32(i) * length / f32(railSegs));
        const auto b = curve.evaluateAtDistance(f32(i + 1) * length / f32(railSegs));
        for (f32 side : {-0.72f, 0.72f}) {
            const glm::vec3 pa = a.position + a.binormal * side, pb = b.position + b.binormal * side;
            const glm::vec3 mid = (pa + pb) * 0.5f + glm::vec3(0, 0.16f, 0);
            sb.prim("Rail", Primitive::Cube, "Steel", mid, {0.1f, 0.12f, glm::distance(pa, pb) + 0.02f}, lookRotation(pa, pb));
        }
    }
    // Train: locomotive + 2 wagons following the spline at constant speed.
    const char* bodies[] = {"CarPaint.Red", "Teal", "Teal"};
    for (int c = 0; c < 3; ++c) {
        Entity car = sb.create(c == 0 ? "Locomotive" : "Wagon");
        auto& f = car.add<gameplay::SplineFollowerComponent>();
        f.spline = track.ref();
        f.speed = 6.0f;
        f.startDistance = 40.0f - f32(c) * 4.6f;
        f.forwardAxis = {0, 0, -1};
        sb.prim("Body", Primitive::Cube, bodies[c], {0, 1.15f, 0}, {1.9f, 1.7f, 4.2f}, {}, car);
        sb.prim("Roof", Primitive::Cube, "DarkGrey", {0, 2.1f, 0}, {2.0f, 0.2f, 4.3f}, {}, car);
        sb.prim("Windows", Primitive::Cube, "Emissive.Warm", {0, 1.45f, 0}, {1.95f, 0.45f, 3.4f}, {}, car);
        for (int w = 0; w < 4; ++w)
            sb.prim("Wheel", Primitive::Cylinder, "DarkMetal", {w % 2 ? 0.75f : -0.75f, 0.35f, w / 2 ? 1.3f : -1.3f}, {0.6f, 0.2f, 0.6f}, eulerDeg(0, 0, 90), car);
        if (c == 0) {
            sb.prim("Chimney", Primitive::Cylinder, "DarkMetal", {0, 2.5f, -1.4f}, {0.5f, 0.8f, 0.5f}, {}, car);
            sb.pointLight("Headlamp", {0, 1.2f, -2.3f}, {1.0f, 0.9f, 0.7f}, 1500.0f, 12.0f, false, 0.1f, car);
            sb.script(car, "Scripts/train.lua");
            audioSource(sb, "TrainSound", "Audio/train_loop.wav", {0, 1, 0}, 0.8f, true, "SFX", true, 30.0f, false, car);
        }
    }
    // Station platform at the "Station" marker + scripted sequences.
    sb.block("Platform", "Concrete", {0, 0.4f, 12.5f}, {10, 0.8f, 2.5f});
    // Vault door sequence (coroutine: alarm light -> unlock -> slide -> wait -> close).
    const glm::vec3 vault(-8.0f, 0, -8.0f);
    sb.block("VaultWallL", "Concrete.Warm", vault + glm::vec3(-3.0f, 2.0f, 0), {3.0f, 4.0f, 1.0f});
    sb.block("VaultWallR", "Concrete.Warm", vault + glm::vec3(3.0f, 2.0f, 0), {3.0f, 4.0f, 1.0f});
    sb.block("VaultLintel", "Concrete.Warm", vault + glm::vec3(0, 3.6f, 0), {3.0f, 0.8f, 1.0f});
    Entity door = sb.prim("VaultDoor", Primitive::Cube, "Steel", vault + glm::vec3(0, 1.6f, 0), {3.0f, 3.2f, 0.5f});
    auto& drb = door.add<gameplay::RigidBodyComponent>();
    drb.motionType = physics::MotionType::Kinematic;
    door.add<gameplay::ColliderComponent>().halfExtents = glm::vec3(0.5f);
    sb.prim("Treasure", Primitive::Sphere, "Gold", vault + glm::vec3(0, 0.6f, -2.5f), glm::vec3(1.2f));
    sb.pointLight("AlarmLight", vault + glm::vec3(0, 4.3f, 0.7f), {1.0f, 0.1f, 0.05f}, 0.0f, 8.0f, false);
    Entity console = sb.block("DoorConsole", "DarkMetal", vault + glm::vec3(2.2f, 0.6f, 1.6f), {0.6f, 1.2f, 0.4f});
    sb.prim("DoorConsoleScreen", Primitive::Cube, "Emissive.Green", vault + glm::vec3(2.2f, 1.25f, 1.82f), {0.45f, 0.3f, 0.02f});
    sb.script(console, "Scripts/door_sequence.lua");
    // Elevator: kinematic platform moved by a coroutine between two floors.
    const glm::vec3 lift(10.0f, 0, -4.0f);
    sb.block("UpperDeck", "Concrete", lift + glm::vec3(4.5f, 3.75f, 0), {6.0f, 0.5f, 6.0f});
    sb.block("DeckPillar", "DarkMetal", lift + glm::vec3(6.8f, 1.75f, 2.4f), {0.4f, 3.5f, 0.4f});
    sb.block("DeckPillar", "DarkMetal", lift + glm::vec3(6.8f, 1.75f, -2.4f), {0.4f, 3.5f, 0.4f});
    Entity platform = sb.prim("Elevator", Primitive::Cube, "Hazard", lift + glm::vec3(0, 0.1f, 0), {3.0f, 0.2f, 3.0f});
    auto& erb = platform.add<gameplay::RigidBodyComponent>();
    erb.motionType = physics::MotionType::Kinematic;
    platform.add<gameplay::ColliderComponent>().halfExtents = glm::vec3(0.5f);
    sb.script(platform, "Scripts/elevator.lua");
    sb.prop(platform, "height", 3.9);
    Entity liftConsole = sb.block("ElevatorConsole", "DarkMetal", lift + glm::vec3(-2.2f, 0.6f, 1.8f), {0.5f, 1.2f, 0.4f});
    sb.prim("ElevatorScreen", Primitive::Cube, "Emissive.Yellow", lift + glm::vec3(-2.2f, 1.25f, 2.02f), {0.4f, 0.3f, 0.02f});
    (void)liftConsole;
    // Dialogue: the station keeper waits for the player's choice (ui event awaited by a coroutine).
    const MannequinAssets ma = mannequinAssets(gen);
    Entity keeper = mannequin(sb, ma, "Keeper", {4.0f, 0.8f, 12.5f}, 150.0f, "Mannequin.Blue", 0.0f);
    sb.script(keeper, "Scripts/dialogue.lua");
    sb.prop(keeper, "speaker", std::string("Смотритель станции"));
    sb.tourCameras({-14.0f, 9.0f, 18.0f}, {0.0f, 0.0f, -4.0f}, {2.0f, 2.5f, 6.0f}, {-8.0f, 1.5f, -8.0f}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 11. Звук
void buildAudio(Gen& gen) {
    const Station& s = station("audio");
    SceneBuilder sb(gen, s.id);
    sunAndSky(sb, {-0.4f, 0.5f, 0.3f}, 30000.0f, 15.0);
    sb.postProcess();
    sb.block("Floor", "Concrete.x8", {0, -0.25f, -4.0f}, {50, 0.5f, 50});
    sb.stationBasics(s, {0, 0.05f, 16}, 0.0f, {-3.5f, 0, 13}, 30.0f, {0, 0, 20.5f}, 180.0f);
    Entity station = sb.create("AudioStation");
    sb.script(station, "Scripts/audio_station.lua");
    // Occlusion: a humming machine behind a thick wall.
    sb.block("OcclusionWall", "Concrete.Warm", {-8.0f, 2.0f, -2.0f}, {8.0f, 4.0f, 0.8f});
    Entity machine = sb.block("Machine", "DarkMetal", {-8.0f, 1.0f, -6.0f}, {2.2f, 2.0f, 1.6f});
    sb.prim("MachineLight", Primitive::Sphere, "Emissive.Red", {-8.0f, 2.2f, -6.0f}, glm::vec3(0.3f));
    audioSource(sb, "MachineHum", "Audio/machine_loop.wav", {0, 0.5f, 0}, 1.0f, true, "SFX", true, 25.0f, true, machine);
    // Reverb zone: a long concrete tunnel; the "Cave" bus gets a reverb while the listener is inside.
    const glm::vec3 tunnel(9.0f, 0, -6.0f);
    sb.block("TunnelL", "Stone", tunnel + glm::vec3(-2.2f, 2.0f, 0), {0.6f, 4.0f, 14.0f});
    sb.block("TunnelR", "Stone", tunnel + glm::vec3(2.2f, 2.0f, 0), {0.6f, 4.0f, 14.0f});
    sb.block("TunnelRoof", "Stone", tunnel + glm::vec3(0, 4.25f, 0), {5.0f, 0.5f, 14.0f});
    for (int i = 0; i < 4; ++i)
        sb.pointLight("TunnelLamp", tunnel + glm::vec3(0, 3.6f, -5.0f + f32(i) * 3.4f), {0.4f, 0.7f, 1.0f}, 600.0f, 6.0f, false);
    Entity zone = sb.create("ReverbZone");
    zone.setPosition(tunnel + glm::vec3(0, 1.5f, 0));
    auto& zc = zone.add<gameplay::ColliderComponent>();
    zc.halfExtents = {1.6f, 1.5f, 7.0f};
    zc.isSensor = true;
    zone.add<gameplay::TriggerComponent>().requiredTag = "Player";
    sb.script(zone, "Scripts/reverb_zone.lua");
    audioSource(sb, "Drips", "Audio/water_loop.wav", tunnel + glm::vec3(0, 1.0f, -4.0f), 0.6f, true, "Cave", true, 20.0f);
    // Speaker tower: music (Music bus) ducked by announcements (Voice bus).
    const glm::vec3 tower(0, 0, -12.0f);
    sb.block("TowerPole", "DarkMetal", tower + glm::vec3(0, 2.5f, 0), {0.3f, 5.0f, 0.3f});
    for (int i = 0; i < 3; ++i) {
        Entity spk = sb.block("Speaker", "Black", tower + glm::vec3(0, 3.8f - f32(i) * 1.1f, 0.3f), {1.0f, 0.9f, 0.6f});
        sb.prim("SpeakerRing", Primitive::Torus, "Emissive.Magenta", {0, 0, 0.52f}, {0.7f, 0.7f, 0.2f}, eulerDeg(90, 0, 0), spk);
    }
    audioSource(sb, "Music", "Audio/music_loop.wav", tower + glm::vec3(0, 3.5f, 0.5f), 0.9f, true, "Music", true, 40.0f);
    Entity announcer = audioSource(sb, "Announcer", "Audio/announcement.wav", tower + glm::vec3(0, 4.5f, 0.5f), 1.0f, false, "Voice", true, 45.0f);
    (void)announcer;
    Entity button = sb.block("AnnounceButton", "Emissive.Yellow", tower + glm::vec3(2.0f, 0.5f, 2.0f), {0.6f, 1.0f, 0.6f});
    (void)button;
    // Doppler: a beeping drone circling the plaza.
    Entity orbit = sb.create("DroneOrbit");
    orbit.setPosition({0, 3.0f, -4.0f});
    auto& os = orbit.add<gameplay::SplineComponent>();
    os.closed = true;
    for (int i = 0; i < 8; ++i) {
        const f32 a = f32(i) / 8.0f * 2 * kPi;
        os.points.push_back({.position = {std::cos(a) * 7.0f, 0.5f * std::sin(a * 2.0f), std::sin(a) * 7.0f}});
    }
    Entity drone = sb.prim("Drone", Primitive::Sphere, "Emissive.Cyan", {7.0f, 3.0f, -4.0f}, glm::vec3(0.4f));
    auto& df = drone.add<gameplay::SplineFollowerComponent>();
    df.spline = orbit.ref();
    df.speed = 9.0f;
    audioSource(sb, "DroneBeep", "Audio/beep.wav", {0, 0, 0}, 0.5f, true, "SFX", true, 25.0f, false, drone)
        .get<gameplay::AudioSourceComponent>()
        .dopplerFactor = 2.0f;
    sb.tourCameras({-1.0f, 3.2f, 8.0f}, {2.0f, 1.8f, -9.0f}, {9.0f, 1.7f, 3.0f}, {9.0f, 1.5f, -12.0f}, 58.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 12. Сеть
void buildNetwork(Gen& gen) {
    const Station& s = station("network");
    SceneBuilder sb(gen, s.id);
    sunAndSky(sb, {0.2f, 0.4f, -0.7f}, 25000.0f, 16.5);
    sb.postProcess();
    sb.block("Floor", "Grid.x8", {0, -0.25f, -4.0f}, {40, 0.5f, 40});
    sb.stationBasics(s, {0, 0.05f, 14}, 0.0f, {-3.5f, 0, 11}, 30.0f, {0, 0, 18.5f}, 180.0f);
    Entity demo = sb.create("NetDemo");
    sb.script(demo, "Scripts/net_panel.lua");
    Entity player = sb.world.findByName("Player");
    auto& pid = player.add<gameplay::NetworkIdentityComponent>();
    pid.netType = "NetAvatar";
    player.add<gameplay::NetworkTransformComponent>();
    // Server-side bot avatar (the "remote player" the bot client controls) running a figure eight.
    Entity bot = sb.create("Bot");
    bot.setPosition({4.0f, 0, -6.0f});
    bot.add<gameplay::NetworkIdentityComponent>().netType = "NetBot";
    bot.add<gameplay::NetworkTransformComponent>();
    sb.prim("BotBody", Primitive::Capsule, "Orange", {0, 0.9f, 0}, {0.62f, 1.8f, 0.62f}, {}, bot);
    sb.prim("BotVisor", Primitive::Cube, "Emissive.Cyan", {0, 1.55f, -0.27f}, {0.4f, 0.12f, 0.1f}, {}, bot);
    sb.script(bot, "Scripts/bot.lua");
    // Replicated physics crates.
    for (int i = 0; i < 6; ++i) {
        Entity crate = sb.body("NetCrate", Primitive::Cube, "Crate", {-6.0f + f32(i % 3) * 1.4f, 0.5f + f32(i / 3) * 1.05f, -8.0f}, glm::vec3(1.0f), 15.0f);
        crate.add<gameplay::NetworkIdentityComponent>().netType = "NetCrate";
        crate.add<gameplay::NetworkTransformComponent>();
    }
    // Server rack decoration.
    for (int i = 0; i < 4; ++i) {
        sb.block("Rack", "DarkMetal", {12.0f, 1.2f, -12.0f + f32(i) * 1.4f}, {1.0f, 2.4f, 1.2f});
        for (int l = 0; l < 6; ++l)
            sb.prim("RackLed", Primitive::Cube, l % 3 ? "Emissive.Green" : "Emissive.Blue", {11.48f, 0.4f + f32(l) * 0.35f, -12.0f + f32(i) * 1.4f}, {0.02f, 0.05f, 0.6f});
    }
    sb.pointLight("RackGlow", {10.5f, 2.0f, -10.0f}, {0.3f, 0.8f, 1.0f}, 2000.0f, 10.0f, false);
    sb.pointLight("ArenaLight", {0, 6.0f, -6.0f}, {1.0f, 0.9f, 0.8f}, 12000.0f, 22.0f, true, 0.3f);
    sb.tourCameras({8.0f, 6.0f, 8.0f}, {0.0f, 0.8f, -6.0f}, {-6.0f, 2.0f, 2.0f}, {3.0f, 1.0f, -6.0f}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

// ---------------------------------------------------------------------------------------------------------------
// 14. Сохранения
void buildSaves(Gen& gen) {
    const Station& s = station("saves");
    SceneBuilder sb(gen, s.id);
    sunAndSky(sb, {0.4f, 0.6f, 0.4f}, 40000.0f, 13.0);
    sb.postProcess();
    sb.block("Floor", "Wood.x4", {0, -0.25f, -2.0f}, {36, 0.5f, 36});
    sb.stationBasics(s, {0, 0.05f, 13}, 0.0f, {-3.5f, 0, 10}, 30.0f, {0, 0, 17.5f}, 180.0f);
    Entity station = sb.create("SaveStation");
    sb.script(station, "Scripts/save_station.lua");
    // Save points: glowing pillars (E saves into "savepoint_<n>").
    for (int i = 0; i < 2; ++i) {
        const glm::vec3 p(i ? 7.0f : -7.0f, 0, -4.0f);
        Entity sp = sb.block("SavePoint", "DarkMetal", p + glm::vec3(0, 0.6f, 0), {1.0f, 1.2f, 1.0f});
        sb.prim("SaveCrystal", Primitive::Sphere, "Emissive.Cyan", p + glm::vec3(0, 1.7f, 0), {0.6f, 0.9f, 0.6f});
        sb.pointLight("SaveGlow", p + glm::vec3(0, 1.8f, 0), {0.2f, 0.9f, 1.0f}, 800.0f, 6.0f, false);
        sb.script(sp, "Scripts/save_point.lua");
        sb.prop(sp, "slot", std::string(i ? "savepoint_east" : "savepoint_west"));
    }
    // Saved state: movable crates (SaveGameComponent = whole entity persisted) and switchable lamps.
    for (int i = 0; i < 6; ++i) {
        Entity c = sb.body("SavedCrate", Primitive::Cube, i % 2 ? "Crate" : "Orange", {-3.0f + f32(i % 3) * 1.5f, 0.5f + f32(i / 3) * 1.05f, -2.0f}, glm::vec3(1.0f), 20.0f);
        c.add<SaveGameComponent>();
    }
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 p(-6.0f + f32(i) * 6.0f, 0, -11.0f);
        sb.prim("LampPost", Primitive::Cylinder, "DarkMetal", p + glm::vec3(0, 1.5f, 0), {0.15f, 3.0f, 0.15f});
        Entity lamp = sb.pointLight("SavedLamp", p + glm::vec3(0, 3.1f, 0), {1.0f, 0.75f, 0.4f}, i == 1 ? 0.0f : 2500.0f, 10.0f, false);
        lamp.add<SaveGameComponent>();
        sb.prim("LampBulb", Primitive::Sphere, "Lamp", {0, 0, 0}, glm::vec3(0.35f), {}, lamp);
        Entity sw = sb.block("Switch", "Hazard", p + glm::vec3(1.2f, 0.6f, 1.0f), {0.4f, 1.2f, 0.4f});
        sb.script(sw, "Scripts/switch.lua");
        sb.prop(sw, "lamp", lamp.uuid().toString());
    }
    // Terminal with the save slot browser.
    Entity term = sb.block("Terminal", "DarkMetal", {0, 0.8f, -14.0f}, {2.0f, 1.6f, 0.6f});
    sb.prim("TerminalScreen", Primitive::Cube, "Accent.saves", {0, 1.2f, -13.68f}, {1.7f, 0.9f, 0.02f});
    sb.script(term, "Scripts/terminal.lua");
    sb.tourCameras({8.0f, 4.0f, 8.0f}, {-1.0f, 1.0f, -6.0f}, {-6.0f, 2.0f, 2.0f}, {0.0f, 1.0f, -12.0f}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

} // namespace ox::showcase::gen
