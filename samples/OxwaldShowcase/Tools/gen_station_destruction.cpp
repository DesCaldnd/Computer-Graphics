// 15. Разрушаемость: Voronoi pre-fracture, structural support graph (wall, tower), a beam resting on two breakable
// columns (Static anchors: it crumbles where a column was), crates that break on impact, debris budget.
#include "gen.hpp"

#include <oxwald/gameplay/destruction.hpp>

namespace ox::showcase::gen {

namespace {

Entity skyAndSun(SceneBuilder& sb) {
    const glm::vec3 dir(-0.45f, 0.55f, 0.4f);
    Entity sun = sb.sun(dir, 52000.0f, {1.0f, 0.94f, 0.86f}, 0.35f);
    Entity sky = sb.create("Sky");
    sky.add<gameplay::SkyComponent>().sunDirection = glm::normalize(dir);
    sb.environment(sun);
    sb.farGround();
    return sun;
}

gameplay::DestructibleComponent& destructible(Entity e, Gen& gen, u32 chunks, u32 seed, const std::string& interior) {
    auto& d = e.add<gameplay::DestructibleComponent>();
    d.chunkCount = chunks;
    d.seed = seed;
    d.interiorMaterial = gen.mat(interior);
    d.breakSound = "project://Assets/Audio/crumble.wav";
    return d;
}

} // namespace

void buildDestruction(Gen& gen) {
    const Station& s = station("destruction");
    SceneBuilder sb(gen, s.id);
    skyAndSun(sb);
    sb.postProcess();
    sb.block("Floor", "Tiles.x8", {0, -0.25f, 0}, {48, 0.5f, 48});
    sb.stationBasics(s, {0, 0.05f, 14}, 0.0f, {-3.5f, 0, 11}, 30.0f, {0, 0, 18.5f}, 180.0f);

    // Colonnade: two columns break into loose debris; the beam on top is structural with Static anchors, so it
    // crumbles above a column that is gone and keeps the part still resting on the other one.
    for (int i = 0; i < 2; ++i) {
        const f32 x = i == 0 ? -10.0f : -5.0f;
        Entity col = sb.block(i == 0 ? "Column.L" : "Column.R", "Stone", {x, 1.6f, -4.0f}, {0.9f, 3.2f, 0.9f});
        auto& d = destructible(col, gen, 12, 11u + u32(i), "Sandstone");
        d.impulseThreshold = 250.0f;
        d.impactImpulseScale = 3.0f;
        d.debrisLifetime = 14.0f;
    }
    Entity beam = sb.block("Beam", "Sandstone", {-7.5f, 3.5f, -4.0f}, {6.6f, 0.6f, 1.1f});
    {
        auto& d = destructible(beam, gen, 28, 21u, "Stone");
        d.structural = true;
        d.anchor = gameplay::DestructionAnchor::Static;
        d.breakRadius = 1.6f;
        d.impulseThreshold = 600.0f;
        d.chunkImpulseThreshold = 250.0f;
        d.debrisLifetime = 14.0f;
    }

    // Wall: structural, anchored at the bottom. Shots knock out holes; what loses its connection falls.
    Entity wall = sb.block("Wall", "Concrete.Warm", {0.5f, 1.75f, -8.0f}, {7.0f, 3.5f, 0.45f});
    {
        auto& d = destructible(wall, gen, 64, 31u, "Stone");
        d.structural = true;
        d.anchor = gameplay::DestructionAnchor::Bottom;
        d.breakRadius = 0.9f;
        d.impactImpulseScale = 4.0f; // game feel: the shot clearly punches the pieces out
        d.impulseThreshold = 300.0f;
        d.chunkImpulseThreshold = 200.0f;
        d.debrisLifetime = 12.0f;
    }

    // Tower: a blast at its foot removes the base and the shaft above collapses.
    Entity tower = sb.block("Tower", "Concrete", {8.0f, 3.0f, -4.5f}, {1.5f, 6.0f, 1.5f});
    {
        auto& d = destructible(tower, gen, 36, 41u, "Stone");
        d.structural = true;
        d.anchor = gameplay::DestructionAnchor::Bottom;
        d.breakRadius = 1.0f;
        d.impulseThreshold = 300.0f;
        d.chunkImpulseThreshold = 200.0f;
        d.debrisLifetime = 12.0f;
    }

    // Crate pyramid: dynamic destructibles that break on impact (also from falling); at most 4 moving pieces each.
    int crate = 0;
    for (int row = 0; row < 3; ++row) {
        for (int i = 0; i < 3 - row; ++i) {
            const glm::vec3 p(5.0f + (f32(i) - f32(2 - row) * 0.5f) * 1.0f, 0.45f + f32(row) * 0.9f, 3.0f);
            Entity c = sb.body("Crate", Primitive::Cube, "Crate", p, glm::vec3(0.9f), 25.0f);
            auto& d = destructible(c, gen, 8, 51u + u32(crate++), "Wood");
            d.impulseThreshold = 120.0f;
            d.maxActiveDebris = 4;
            d.debrisLifetime = 6.0f;
        }
    }

    // Cannon (visual) + station logic: shots from the camera, blasts, support graph, scripted demo for the tour.
    Entity cannon = sb.create("Cannon");
    cannon.setPosition({-1.5f, 1.2f, 6.0f});
    sb.prim("CannonBase", Primitive::Cube, "DarkMetal", {0, -0.75f, 0}, {1.2f, 0.9f, 1.4f}, {}, cannon);
    sb.prim("CannonBarrel", Primitive::Cylinder, "Steel", {0, 0, -0.6f}, {0.5f, 1.8f, 0.5f}, eulerDeg(90, 0, 0), cannon);
    Entity logic = sb.create("DestructionStation");
    sb.script(logic, "Scripts/destruction_station.lua");

    sb.tourCameras({12.0f, 6.5f, 10.0f}, {-4.0f, 1.5f, -6.0f}, {-9.0f, 3.5f, 5.0f}, {2.0f, 2.0f, -6.0f}, 55.0f);
    sb.save("Scenes/Stations/" + s.file + ".oxscene");
}

} // namespace ox::showcase::gen
