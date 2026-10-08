// Materials, sky cubemaps, project file, prefabs and data files of the showcase.
#include "gen.hpp"

#include <oxwald/assets/image.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/runtime/project.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/core/serial/format.hpp>

#include <nlohmann/json.hpp>

#include <fstream>

#include <cmath>

namespace ox::showcase::gen {

namespace {

using assets::BlendMode;
using assets::MaterialAsset;
using assets::ShadingModel;

MaterialAsset lit(glm::vec3 color, f32 roughness, f32 metallic = 0.0f) {
    MaterialAsset m;
    m.baseColor = glm::vec4(color, 1.0f);
    m.roughness = roughness;
    m.metallic = metallic;
    return m;
}

MaterialAsset emissive(glm::vec3 color, f32 strength, glm::vec3 base = glm::vec3(0.02f)) {
    MaterialAsset m = lit(base, 0.5f);
    m.emissive = color;
    m.emissiveStrength = strength;
    return m;
}

// Splits a horizontal-cross cubemap image (4×3 faces) into six face PNGs + an .oxcube description.
void crossToCube(Gen& gen, const std::string& source, const std::string& name, u32 faceSize) {
    const fs::path src = gen.assets / source;
    auto img = assets::loadImage(src);
    if (!img) {
        OX_LOG_WARN("generate", "sky {}: {}", source, img.error().message);
        return;
    }
    const u32 cell = img->width / 4;
    // Horizontal cross: row 0 col 1 = +Y, row 1 = -X +Z +X -Z, row 2 col 1 = -Y.
    struct Face {
        const char* suffix;
        u32 col, row;
    };
    const Face faces[6] = {{"px", 2, 1}, {"nx", 0, 1}, {"py", 1, 0}, {"ny", 1, 2}, {"pz", 1, 1}, {"nz", 3, 1}};
    nlohmann::ordered_json cube = {{"faces", nlohmann::ordered_json::array()}};
    for (const Face& f : faces) {
        assets::Image face(faceSize, faceSize);
        for (u32 y = 0; y < faceSize; ++y)
            for (u32 x = 0; x < faceSize; ++x) {
                const glm::vec2 uv((f32(x) + 0.5f) / f32(faceSize), (f32(y) + 0.5f) / f32(faceSize));
                const glm::vec2 p = glm::vec2(f32(f.col * cell), f32(f.row * cell)) + uv * f32(cell);
                face.at(x, y) = img->sampleBilinear(p / glm::vec2(f32(img->width), f32(img->height)), false, false);
            }
        const std::string rel = "Textures/Sky/" + name + "_" + f.suffix + ".png";
        gen.writeAsset(rel, assets::encodePng(face));
        gen.meta(rel);
        cube["faces"].push_back(name + "_" + f.suffix + ".png");
    }
    gen.writeText("Textures/Sky/" + name + ".oxcube", cube.dump(2) + "\n");
    gen.meta("Textures/Sky/" + name + ".oxcube");
}

// Legacy leather: glossiness map -> ORM (R occlusion 1, G roughness = 1 - gloss, B metallic 0) at 1024².
void leatherOrm(Gen& gen) {
    const std::string rel = "Textures/Generated/leather_orm.png";
    if (fs::exists(gen.assets / rel)) {
        gen.meta(rel, {{"type", "Linear"}});
        return;
    }
    auto gloss = assets::loadImage(gen.assets / "Legacy/beige_leather_5_glossiness.png");
    if (!gloss) return;
    const u32 n = 1024;
    assets::Image out(n, n);
    for (u32 y = 0; y < n; ++y)
        for (u32 x = 0; x < n; ++x) {
            const f32 g = gloss->sampleBilinear({(f32(x) + 0.5f) / f32(n), (f32(y) + 0.5f) / f32(n)}).r;
            out.at(x, y) = glm::vec4(1.0f, glm::clamp(1.0f - g, 0.05f, 1.0f), 0.0f, 1.0f);
        }
    gen.writeAsset(rel, assets::encodePng(out));
    gen.meta(rel, {{"type", "Linear"}});
}

} // namespace

void generateMaterials(Gen& gen) {
    crossToCube(gen, "Legacy/env.png", "park", 512);
    crossToCube(gen, "Legacy/desert.jpg", "desert", 256);
    leatherOrm(gen);

    auto tex = [&](const std::string& rel) { return gen.meta(rel); };
    const Uuid tilesA = tex("Textures/Generated/tiles_albedo.png"), tilesN = tex("Textures/Generated/tiles_normal.png"),
               tilesO = tex("Textures/Generated/tiles_orm.png");
    auto tiles = [&](const std::string& name, f32 tiling) {
        MaterialAsset m = lit({1, 1, 1}, 1.0f);
        m.albedoTexture = tilesA;
        m.normalTexture = tilesN;
        m.ormTexture = tilesO;
        m.uvTiling = glm::vec2(tiling);
        gen.material(name, m);
    };
    tiles("Tiles", 1.0f);
    tiles("Tiles.x4", 4.0f);
    tiles("Tiles.x8", 8.0f);
    tiles("Tiles.x16", 16.0f);
    auto grid = [&](const std::string& name, f32 tiling) {
        MaterialAsset m = lit({1, 1, 1}, 0.75f);
        m.albedoTexture = tex("Textures/Generated/grid_albedo.png");
        m.uvTiling = glm::vec2(tiling);
        gen.material(name, m);
    };
    grid("Grid", 1.0f);
    grid("Grid.x4", 4.0f);
    grid("Grid.x8", 8.0f);
    grid("Grid.x16", 16.0f);
    auto concrete = [&](const std::string& name, f32 tiling, glm::vec3 tint) {
        MaterialAsset m = lit(tint, 0.85f);
        m.albedoTexture = tex("Textures/Generated/concrete_albedo.png");
        m.normalTexture = tex("Textures/Generated/concrete_normal.png");
        m.uvTiling = glm::vec2(tiling);
        gen.material(name, m);
    };
    concrete("Concrete", 1.0f, {1, 1, 1});
    concrete("Concrete.x4", 4.0f, {1, 1, 1});
    concrete("Concrete.x8", 8.0f, {1, 1, 1});
    concrete("Concrete.Warm", 2.0f, {1.0f, 0.92f, 0.82f});
    {
        MaterialAsset m = lit({1, 1, 1}, 0.55f);
        m.albedoTexture = tex("Textures/Generated/wood_albedo.png");
        gen.material("Wood", m);
        m.uvTiling = glm::vec2(4.0f);
        gen.material("Wood.x4", m);
        MaterialAsset p = lit({1, 1, 1}, 0.4f);
        p.albedoTexture = tex("Legacy/albedo.png");
        p.uvTiling = glm::vec2(3.0f);
        gen.material("Parquet", p);
        MaterialAsset c = lit({0.9f, 0.75f, 0.55f}, 0.6f);
        c.albedoTexture = tex("Textures/Generated/wood_albedo.png");
        gen.material("Crate", c);
    }
    {
        MaterialAsset m = lit({1, 1, 1}, 1.0f);
        m.albedoTexture = tex("Legacy/beige_leather_5_diffuse.png");
        m.normalTexture = gen.meta("Legacy/beige_leather_5_normal.png", {{"type", "Normal"}, {"maxSize", 1024}});
        m.ormTexture = tex("Textures/Generated/leather_orm.png");
        m.uvTiling = glm::vec2(2.0f);
        gen.material("Leather", m);
        m.baseColor = glm::vec4(0.35f, 0.16f, 0.08f, 1.0f);
        gen.material("Leather.Brown", m);
        m.baseColor = glm::vec4(0.12f, 0.12f, 0.13f, 1.0f);
        gen.material("Leather.Black", m);
        MaterialAsset pic = lit({1, 1, 1}, 0.6f);
        pic.albedoTexture = tex("Legacy/picture.png");
        gen.material("Painting", pic);
        MaterialAsset logo = lit({1, 1, 1}, 0.3f);
        logo.albedoTexture = tex("Legacy/fiit.jpg");
        logo.emissiveTexture = logo.albedoTexture;
        logo.emissive = glm::vec3(1.0f);
        logo.emissiveStrength = 0.6f;
        gen.material("Logo", logo);
    }
    // Plain surfaces.
    gen.material("White", lit({0.8f, 0.8f, 0.8f}, 0.6f));
    gen.material("OffWhite", lit({0.72f, 0.7f, 0.66f}, 0.7f));
    gen.material("Grey", lit({0.35f, 0.36f, 0.38f}, 0.6f));
    gen.material("DarkGrey", lit({0.08f, 0.085f, 0.09f}, 0.5f));
    gen.material("Black", lit({0.02f, 0.02f, 0.02f}, 0.4f));
    gen.material("RedWall", lit({0.75f, 0.06f, 0.05f}, 0.8f));
    gen.material("GreenWall", lit({0.08f, 0.6f, 0.12f}, 0.8f));
    gen.material("BlueWall", lit({0.06f, 0.18f, 0.7f}, 0.8f));
    gen.material("Yellow", lit({0.9f, 0.7f, 0.08f}, 0.5f));
    gen.material("Orange", lit({0.95f, 0.38f, 0.05f}, 0.45f));
    gen.material("Teal", lit({0.05f, 0.5f, 0.5f}, 0.4f));
    gen.material("Purple", lit({0.4f, 0.12f, 0.6f}, 0.4f));
    gen.material("Hazard", lit({0.95f, 0.75f, 0.05f}, 0.6f));
    gen.material("Rubber", lit({0.05f, 0.05f, 0.05f}, 0.9f));
    gen.material("Stone", lit({0.42f, 0.4f, 0.37f}, 0.85f));
    gen.material("Sandstone", lit({0.66f, 0.55f, 0.4f}, 0.85f));
    gen.material("PlayerBody", lit({0.2f, 0.45f, 0.9f}, 0.4f));
    gen.material("Mannequin", lit({0.85f, 0.55f, 0.28f}, 0.38f));
    gen.material("Mannequin.Blue", lit({0.2f, 0.4f, 0.9f}, 0.38f));
    gen.material("Mannequin.Grey", lit({0.55f, 0.56f, 0.6f}, 0.3f, 0.6f));
    gen.material("Guard", lit({0.6f, 0.08f, 0.08f}, 0.45f, 0.3f));
    // Metals.
    gen.material("DarkMetal", lit({0.12f, 0.13f, 0.15f}, 0.35f, 1.0f));
    gen.material("Steel", lit({0.56f, 0.57f, 0.58f}, 0.3f, 1.0f));
    gen.material("BrushedSteel", lit({0.6f, 0.6f, 0.62f}, 0.45f, 1.0f));
    gen.material("Chrome", lit({0.95f, 0.95f, 0.95f}, 0.03f, 1.0f));
    gen.material("Gold", lit({1.0f, 0.77f, 0.34f}, 0.2f, 1.0f));
    gen.material("Copper", lit({0.95f, 0.64f, 0.54f}, 0.3f, 1.0f));
    gen.material("Mirror", lit({0.97f, 0.97f, 0.97f}, 0.0f, 1.0f));
    {
        MaterialAsset floor = lit({0.04f, 0.04f, 0.045f}, 0.06f, 0.0f);
        gen.material("PolishedFloor", floor);
        MaterialAsset m = lit({0.9f, 0.9f, 0.92f}, 0.12f, 0.0f);
        m.clearcoat = 1.0f;
        m.clearcoatRoughness = 0.05f;
        gen.material("CarPaint.White", m);
        m.baseColor = glm::vec4(0.6f, 0.02f, 0.02f, 1.0f);
        gen.material("CarPaint.Red", m);
    }
    // PBR sphere grid: rows = metallic (0, 0.25, 0.5, 0.75, 1), columns = roughness 0..1 (7 steps).
    for (int r = 0; r < 7; ++r)
        for (int mtl = 0; mtl < 5; ++mtl) {
            const glm::vec3 base = mtl == 0 ? glm::vec3(0.8f, 0.12f, 0.1f) : glm::vec3(0.95f, 0.75f, 0.4f);
            gen.material("PBR.r" + std::to_string(r) + ".m" + std::to_string(mtl), lit(base, std::max(0.02f, f32(r) / 6.0f), f32(mtl) / 4.0f));
        }
    // Emissive.
    gen.material("Emissive.White", emissive({1.0f, 0.95f, 0.85f}, 8.0f));
    gen.material("Emissive.Warm", emissive({1.0f, 0.6f, 0.25f}, 6.0f));
    gen.material("Emissive.Red", emissive({1.0f, 0.1f, 0.05f}, 6.0f));
    gen.material("Emissive.Green", emissive({0.2f, 1.0f, 0.3f}, 6.0f));
    gen.material("Emissive.Blue", emissive({0.15f, 0.4f, 1.0f}, 6.0f));
    gen.material("Emissive.Cyan", emissive({0.1f, 0.9f, 1.0f}, 6.0f));
    gen.material("Emissive.Magenta", emissive({1.0f, 0.15f, 0.8f}, 6.0f));
    gen.material("Emissive.Yellow", emissive({1.0f, 0.85f, 0.1f}, 6.0f));
    for (int h = 0; h < 16; ++h) { // light garden bulbs (same hue formula as the lights)
        const glm::vec3 c = glm::clamp(glm::abs(glm::fract(glm::vec3(f32(h) / 16.0f) + glm::vec3(0, 2.0f / 3, 1.0f / 3)) * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
        gen.material("Bulb." + std::to_string(h), emissive(c * 0.85f + 0.15f, 8.0f, c * 0.3f));
    }
    gen.material("Lamp", emissive({1.0f, 0.85f, 0.6f}, 20.0f, {0.9f, 0.9f, 0.9f}));
    gen.material("Neon.Pink", emissive({1.0f, 0.1f, 0.6f}, 12.0f));
    gen.material("Neon.Blue", emissive({0.1f, 0.5f, 1.0f}, 12.0f));
    {
        MaterialAsset lava = emissive({1.0f, 0.25f, 0.03f}, 4.0f, {0.1f, 0.02f, 0.01f});
        lava.emissiveTexture = tex("Textures/Generated/concrete_albedo.png");
        gen.material("Lava", lava);
    }
    // Translucent.
    {
        MaterialAsset glass = lit({1, 1, 1}, 0.0f);
        glass.blendMode = BlendMode::Refractive;
        glass.ior = 1.5f;
        glass.transmission = 1.0f;
        glass.absorptionColor = glm::vec3(0.55f, 0.85f, 0.75f);
        glass.absorptionDistance = 0.6f;
        gen.material("Glass", glass);
        glass.absorptionColor = glm::vec3(0.95f, 0.45f, 0.15f);
        glass.absorptionDistance = 0.35f;
        gen.material("Glass.Amber", glass);
        glass.absorptionColor = glm::vec3(0.3f, 0.45f, 0.95f);
        gen.material("Glass.Blue", glass);
        glass.absorptionColor = glm::vec3(1.0f);
        glass.absorptionDistance = 0.0f;
        glass.roughness = 0.3f;
        gen.material("FrostedGlass", glass);
        glass.roughness = 0.0f;
        glass.ior = 1.33f;
        glass.absorptionColor = glm::vec3(0.4f, 0.8f, 0.95f);
        glass.absorptionDistance = 1.5f;
        gen.material("WaterGlass", glass);

        MaterialAsset pane = lit({0.75f, 0.9f, 1.0f}, 0.05f);
        pane.blendMode = BlendMode::Transparent;
        pane.baseColor.a = 0.25f;
        gen.material("WindowPane", pane);
        MaterialAsset stained = lit({1.0f, 0.35f, 0.2f}, 0.1f);
        stained.blendMode = BlendMode::Transparent;
        stained.baseColor.a = 0.45f;
        stained.emissive = glm::vec3(1.0f, 0.4f, 0.2f);
        stained.emissiveStrength = 0.4f;
        gen.material("Stained.Red", stained);
        stained.baseColor = glm::vec4(0.2f, 0.45f, 1.0f, 0.45f);
        stained.emissive = glm::vec3(0.2f, 0.45f, 1.0f);
        gen.material("Stained.Blue", stained);

        MaterialAsset veil = emissive({0.4f, 0.7f, 1.0f}, 1.2f, {0.4f, 0.7f, 1.0f});
        veil.blendMode = BlendMode::Transparent;
        veil.baseColor.a = 0.22f;
        veil.doubleSided = true;
        gen.material("PortalVeil", veil);
        MaterialAsset ghost = emissive({0.2f, 0.9f, 1.0f}, 1.5f, {0.2f, 0.9f, 1.0f});
        ghost.blendMode = BlendMode::Transparent;
        ghost.baseColor.a = 0.35f;
        gen.material("Ghost", ghost);
        MaterialAsset cone = emissive({1.0f, 0.15f, 0.08f}, 0.5f, {1.0f, 0.15f, 0.08f});
        cone.blendMode = BlendMode::Transparent;
        cone.baseColor.a = 0.07f;
        cone.doubleSided = true;
        gen.material("VisionCone", cone);
    }
    // Foliage (alpha test, double sided, foliage shading).
    {
        MaterialAsset leaves = lit({1, 1, 1}, 0.6f);
        leaves.shadingModel = ShadingModel::Foliage;
        leaves.blendMode = BlendMode::AlphaTest;
        leaves.alphaCutoff = 0.5f;
        leaves.doubleSided = true;
        leaves.albedoTexture = tex("Textures/Generated/leaves_albedo.png");
        gen.material("Leaves", leaves);
        MaterialAsset bark = lit({0.3f, 0.2f, 0.12f}, 0.9f);
        gen.material("Bark", bark);
        MaterialAsset skin = lit({0.9f, 0.62f, 0.5f}, 0.5f);
        skin.shadingModel = ShadingModel::Subsurface;
        skin.subsurface = 0.6f;
        skin.subsurfaceColor = glm::vec3(1.0f, 0.3f, 0.2f);
        gen.material("Wax", skin);
    }
    // Terrain layers.
    for (const char* layer : {"grass", "rock", "sand", "snow"}) {
        MaterialAsset m = lit({1, 1, 1}, std::string(layer) == "snow" ? 0.4f : 0.9f);
        m.albedoTexture = tex(std::string("Textures/Generated/terrain_") + layer + ".png");
        gen.material(std::string("Terrain.") + layer, m);
    }
    {
        MaterialAsset meadow = lit({0.75f, 0.85f, 0.7f}, 0.95f);
        meadow.albedoTexture = tex("Textures/Generated/terrain_grass.png");
        meadow.uvTiling = glm::vec2(1500.0f);
        gen.material("Meadow", meadow);
    }
    // Portal frames, station accents.
    gen.material("PortalFrame", lit({0.16f, 0.17f, 0.2f}, 0.4f, 0.8f));
    for (const Station& s : stations()) {
        gen.material("Glow." + s.id, emissive(s.color, 10.0f, s.color * 0.2f));
        MaterialAsset screen = emissive(s.color * 0.5f + 0.15f, 1.6f, {0.02f, 0.02f, 0.03f});
        screen.emissiveTexture = tex("Textures/Generated/screen_emissive.png");
        gen.material("Accent." + s.id, screen);
    }
    gen.material("Glow.hub", emissive({0.6f, 0.8f, 1.0f}, 10.0f, {0.1f, 0.15f, 0.2f}));
    gen.material("Glow.menu", emissive({0.6f, 0.8f, 1.0f}, 10.0f, {0.1f, 0.15f, 0.2f}));
    OX_LOG_INFO("generate", "materials done");
}

// ---------------------------------------------------------------------------------------------------------------

void generateProjectFile(Gen& gen) {
    Project p = Project::create(gen.project, "OxwaldShowcase");
    if (auto existing = Project::load(gen.project / "OxwaldShowcase.oxproj")) p = std::move(*existing);
    ProjectSettings& s = p.settings;
    s.name = "OxwaldShowcase";
    s.version = "1.0.0";
    s.company = "Oxwald";
    s.startupScene = kMenuUri;
    s.assetDirs = {"Assets"};
    s.modules["Showcase"] = true;
    s.saveVersion = 1;
    s.defaultQuality = "High";
    s.physics.fixedRate = 60.0f;
    s.audio.busVolumes = {{"Music", 0.6f}, {"SFX", 1.0f}, {"Ambience", 0.8f}, {"Voice", 1.0f}};
    s.rendering.upscaler = "Off";
    s.rendering.cvars = {{"r.AntiAliasing", "2"}};
    s.packaging.alwaysIncludeAssets = {"UI/", "Scripts/", "Data/", "Audio/"};

    InputMappingConfig& in = s.input;
    in = {};
    in.actions = {{"Move", InputValueType::Axis2D},  {"Look", InputValueType::Axis2D},  {"Jump", InputValueType::Bool},
                  {"Sprint", InputValueType::Bool},  {"Interact", InputValueType::Bool}, {"Fire", InputValueType::Bool},
                  {"Menu", InputValueType::Bool},    {"QuickSave", InputValueType::Bool}, {"QuickLoad", InputValueType::Bool},
                  {"Info", InputValueType::Bool},    {"Zoom", InputValueType::Axis1D}};
    using M = InputModifier;
    using T = InputTrigger;
    InputContextDesc foot{"OnFoot", 0, {}};
    auto& b = foot.bindings;
    b.push_back({"Move", "Key.D"});
    b.push_back({"Move", "Key.A", {M::makeNegate()}});
    b.push_back({"Move", "Key.W", {M::makeSwizzle()}});
    b.push_back({"Move", "Key.S", {M::makeNegate(), M::makeSwizzle()}});
    b.push_back({"Move", "Gamepad.LeftStick", {M::makeDeadZone(0.2f)}});
    // Mouse look reads input.mouseDelta() directly (pixels); "Look" is the gamepad stick.
    b.push_back({"Look", "Gamepad.RightStick", {M::makeDeadZone(0.15f), M::makeScale({3.0f, 2.2f, 1.0f})}});
    b.push_back({"Jump", "Key.Space", {}, {T::pressed()}});
    b.push_back({"Jump", "Gamepad.A", {}, {T::pressed()}});
    b.push_back({"Sprint", "Key.LeftShift"});
    b.push_back({"Sprint", "Gamepad.LeftThumb"});
    b.push_back({"Interact", "Key.E", {}, {T::pressed()}});
    b.push_back({"Interact", "Gamepad.X", {}, {T::pressed()}});
    b.push_back({"Fire", "Mouse.Left", {}, {T::pressed()}});
    b.push_back({"Fire", "Gamepad.RightTrigger", {}, {T::pressed()}});
    b.push_back({"Menu", "Key.Escape", {}, {T::pressed()}});
    b.push_back({"Menu", "Gamepad.Start", {}, {T::pressed()}});
    b.push_back({"QuickSave", "Key.F5", {}, {T::pressed()}});
    b.push_back({"QuickLoad", "Key.F9", {}, {T::pressed()}});
    b.push_back({"Info", "Key.I", {}, {T::pressed()}});
    b.push_back({"Info", "Gamepad.Back", {}, {T::pressed()}});
    b.push_back({"Zoom", "Mouse.Wheel"});
    in.contexts = {foot};
    in.activeContexts = {"OnFoot"};
    // Runtime part through ox::Project, then the editor-only section (OxwaldEditor opens the hub).
    auto withEditorSection = [](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        auto j = nlohmann::ordered_json::parse(in, nullptr, false);
        in.close();
        if (j.is_discarded()) return;
        j["editor"] = {{"description", "OxwaldShowcase — экскурсия по всем возможностям OxwaldEngine (сгенерировано oxshowcase_generate)"},
                       {"maps", {{"editorStartupMap", "Scenes/Hub.oxscene"}, {"gameDefaultMap", "Scenes/MainMenu.oxscene"},
                                 {"defaultGameMode", "Default"}}}};
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << j.dump(2) << "\n";
    };
    const fs::path file = gen.project / "OxwaldShowcase.oxproj";
    if (gen.checkOnly) {
        // Compare through a temporary copy (Project::save writes the file).
        const fs::path tmp = fs::temp_directory_path() / "oxshowcase_check.oxproj";
        if (auto st = p.saveAs(tmp); st) {
            withEditorSection(tmp);
            std::ifstream a(tmp, std::ios::binary), b(file, std::ios::binary);
            const std::string sa((std::istreambuf_iterator<char>(a)), {}), sb((std::istreambuf_iterator<char>(b)), {});
            if (sa != sb) gen.differences.push_back("OxwaldShowcase.oxproj");
            fs::remove(tmp);
        }
        return;
    }
    if (auto st = p.save(); !st) OX_LOG_ERROR("generate", "project: {}", st.error().message);
    withEditorSection(file);
    OX_LOG_INFO("generate", "project file written");
}

// ---------------------------------------------------------------------------------------------------------------

namespace {
void writePrefab(Gen& gen, const std::string& name, World& w, Entity root) {
    const std::string rel = "Prefabs/" + name + ".oxprefab";
    gen.writeAsset(rel, stablePrefab(w, root, rel));
    gen.meta(rel);
}
} // namespace

void generatePrefabs(Gen& gen) {
    // Network replicas (netType = prefab name; the bot client spawns them).
    for (const char* name : {"NetAvatar", "NetBot"}) {
        World w;
        Entity root = w.create(name);
        auto& id = root.add<gameplay::NetworkIdentityComponent>();
        id.netType = name;
        root.add<gameplay::NetworkTransformComponent>();
        // Same replicated layout as the server entities (Script.enabled is a replicated field).
        root.add<gameplay::ScriptComponent>().enabled = false;
        writePrefab(gen, name, w, root);
    }
    {
        World w;
        Entity root = w.create("NetCrate");
        root.add<gameplay::NetworkIdentityComponent>().netType = "NetCrate";
        auto& nt = root.add<gameplay::NetworkTransformComponent>();
        nt.syncRotation = true;
        writePrefab(gen, "NetCrate", w, root);
    }
    // Physics crate spawned from Lua (scene.spawn("Crate", pos)).
    {
        World w;
        Entity root = w.create("Crate");
        root.setScale(glm::vec3(0.8f));
        auto& mr = root.add<MeshRendererComponent>();
        mr.mesh = render::primitiveUuid(Primitive::Cube);
        mr.materials = {gen.mat("Crate")};
        root.add<gameplay::RigidBodyComponent>().mass = 25.0f;
        root.add<gameplay::ColliderComponent>().halfExtents = glm::vec3(0.5f);
        writePrefab(gen, "Crate", w, root);
    }
    // Floating crate for the water station (buoyancy).
    {
        World w;
        Entity root = w.create("FloatingCrate");
        root.setScale(glm::vec3(1.0f));
        auto& mr = root.add<MeshRendererComponent>();
        mr.mesh = render::primitiveUuid(Primitive::Cube);
        mr.materials = {gen.mat("Crate")};
        auto& rb = root.add<gameplay::RigidBodyComponent>();
        rb.mass = 220.0f;
        rb.linearDamping = 0.2f;
        rb.angularDamping = 0.3f;
        root.add<gameplay::ColliderComponent>().halfExtents = glm::vec3(0.5f);
        auto& bu = root.add<gameplay::BuoyancyComponent>();
        bu.halfExtents = glm::vec3(0.5f);
        writePrefab(gen, "FloatingCrate", w, root);
    }
    OX_LOG_INFO("generate", "prefabs done");
}

void generateData(Gen& gen) {
    nlohmann::ordered_json j;
    j["hub"] = {{"id", "hub"}, {"title", "Хаб"}, {"scene", kHubUri}};
    j["stations"] = nlohmann::ordered_json::array();
    for (const Station& s : stations())
        j["stations"].push_back({{"id", s.id}, {"title", s.title}, {"scene", s.uri()}, {"guide", s.guide}});
    gen.writeText("Data/stations.json", j.dump(2) + "\n");

    // Guard behaviour: chase while the target is visible, search its last known position, otherwise patrol.
    nlohmann::ordered_json bt = {
        {"blackboard", {{"searching", false}}},
        {"root",
         {{"type", "Selector"},
          {"name", "Guard"},
          {"children",
           {{{"type", "BlackboardCondition"}, {"name", "SeesPlayer"}, {"key", "targetVisible"}, {"op", "Equals"}, {"value", true},
             {"abort", "Both"},
             {"child", {{"type", "Sequence"}, {"name", "Chase"},
                        {"children", {{{"type", "ScriptAction"}, {"function", "onChase"}},
                                      {{"type", "MoveTo"}, {"key", "target"}, {"acceptance", 1.6}}}}}}},
            {{"type", "BlackboardCondition"}, {"name", "Searching"}, {"key", "searching"}, {"op", "Equals"}, {"value", true},
             {"abort", "Both"},
             {"child", {{"type", "Sequence"}, {"name", "Search"},
                        {"children", {{{"type", "ScriptAction"}, {"function", "onSearch"}},
                                      {{"type", "MoveTo"}, {"key", "lastKnown"}, {"acceptance", 1.0}},
                                      {{"type", "ScriptAction"}, {"function", "lookAround"}},
                                      {{"type", "Wait"}, {"seconds", 2.5}},
                                      {{"type", "ScriptAction"}, {"function", "giveUp"}}}}}}},
            {{"type", "Sequence"}, {"name", "Patrol"},
             {"children", {{{"type", "ScriptAction"}, {"function", "nextWaypoint"}},
                           {{"type", "MoveTo"}, {"key", "waypoint"}, {"acceptance", 0.6}},
                           {{"type", "Wait"}, {"seconds", 1.2}}}}}}}}}};
    gen.writeText("AI/guard.oxbt", bt.dump(2) + "\n");
    gen.meta("AI/guard.oxbt");
}

} // namespace ox::showcase::gen
