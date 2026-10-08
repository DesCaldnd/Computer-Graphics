#pragma once

// oxshowcase_generate — builds every OxwaldShowcase scene through the engine API (World/Entity/components) and
// writes the generated assets (textures, sounds, the skinned mannequin, materials, prefabs, project file).
// Deterministic: entity/asset UUIDs are derived from names (Uuid::fromName), so re-running produces the same files.

#include <oxwald/assets/material.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/render/mesh_primitives.hpp>
#include <oxwald/scene/components.hpp>
#include <oxwald/scene/world.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ox::assets {
class AssetRegistry;
}

namespace ox::showcase::gen {

namespace fs = std::filesystem;
using render::Primitive;

inline constexpr const char* kHubUri = "project://Assets/Scenes/Hub.oxscene";
inline constexpr const char* kMenuUri = "project://Assets/Scenes/MainMenu.oxscene";

struct Station {
    std::string id;          // "lighting"
    std::string file;        // "01_Lighting"
    std::string title;       // "Свет и тени"
    std::string description; // 2–3 sentences (Russian)
    std::string hints;       // key hints (Russian)
    std::string guide;       // "Подробнее" target (guide chapter)
    glm::vec3 color{1.0f};   // portal / accent colour
    [[nodiscard]] std::string uri() const { return "project://Assets/Scenes/Stations/" + file + ".oxscene"; }
};

[[nodiscard]] const std::vector<Station>& stations();
[[nodiscard]] const Station& station(std::string_view id);

// (offset, size) byte ranges of a buffer that hold little-endian f32 values.
using FloatRanges = std::vector<std::pair<usize, usize>>;

// Shared generator state: project paths, asset database, generated asset UUIDs.
class Gen {
public:
    explicit Gen(fs::path project);
    ~Gen();

    fs::path project;
    fs::path assets; // <project>/Assets
    std::unique_ptr<assets::AssetRegistry> registry;

    // Deterministic UUIDs.
    static Uuid materialId(std::string_view name) { return Uuid::fromName("showcase.material." + std::string(name)); }
    static Uuid assetId(std::string_view relPath) { return Uuid::fromName("showcase.asset." + std::string(relPath)); }

    // Writes Assets/Materials/<name>.oxmat (+ meta) when its content changed; returns the material UUID.
    Uuid material(const std::string& name, const assets::MaterialAsset& m);
    [[nodiscard]] Uuid mat(const std::string& name) const; // previously defined material (asserts)

    // Writes `bytes` to Assets/<rel> when different. Returns true when the file changed. PNG / WAV / scene and
    // prefab payloads that match the existing file within a tight numeric tolerance count as unchanged (last-bit
    // libm and FMA differences between platforms); `floats` marks the f32 ranges of a raw buffer for the same test.
    bool writeAsset(const std::string& rel, const std::vector<std::byte>& bytes, const FloatRanges& floats = {});
    bool writeText(const std::string& rel, const std::string& text);
    // Ensures a meta with a deterministic UUID exists for Assets/<rel>; `settings` merged into the importer
    // defaults. Returns the asset UUID (existing metas keep theirs).
    // `force`: also apply `settings` to an existing meta (otherwise they only seed new metas).
    Uuid meta(const std::string& rel, const nlohmann::ordered_json& settings = nlohmann::ordered_json::object(),
              bool force = true);
    // UUID of an imported (sub-)asset path such as "Legacy/Soldier/soldier.obj#Mesh/0" (imports on demand).
    [[nodiscard]] std::optional<Uuid> imported(const std::string& assetPath);
    [[nodiscard]] std::vector<std::string> subAssets(const std::string& sourceRel, const std::string& prefix);

    void scanAndMetaEverything(); // metas for every importable file without one (deterministic ids)

    u32 filesWritten = 0;
    u32 filesUnchanged = 0;
    u32 filesEquivalent = 0; // of filesUnchanged: equal within the numeric tolerance, not byte for byte
    // --check: nothing is written; files that would change are listed in `differences`.
    bool checkOnly = false;
    std::vector<std::string> differences;

private:
    std::map<std::string, Uuid> m_materials;
};

// Builds one scene with deterministic entity UUIDs.
class SceneBuilder {
public:
    SceneBuilder(Gen& gen, std::string sceneName);

    Gen& gen;
    std::string sceneName;
    World world;

    Entity create(std::string_view name, Entity parent = {});
    // Mesh entity (primitive or mesh asset) with one material for every submesh.
    Entity mesh(std::string_view name, const Uuid& meshId, const std::string& material, glm::vec3 position,
                glm::vec3 scale = glm::vec3(1.0f), glm::quat rotation = glm::quat(1, 0, 0, 0), Entity parent = {});
    Entity prim(std::string_view name, Primitive p, const std::string& material, glm::vec3 position,
                glm::vec3 scale = glm::vec3(1.0f), glm::quat rotation = glm::quat(1, 0, 0, 0), Entity parent = {});
    // Cube with a static box collider (size = full extents).
    Entity block(std::string_view name, const std::string& material, glm::vec3 center, glm::vec3 size,
                 glm::quat rotation = glm::quat(1, 0, 0, 0), Entity parent = {});
    // Dynamic rigid body primitive (cube / sphere / cylinder / capsule).
    Entity body(std::string_view name, Primitive p, const std::string& material, glm::vec3 position, glm::vec3 size,
                f32 mass = 0.0f, glm::quat rotation = glm::quat(1, 0, 0, 0));

    Entity sun(glm::vec3 directionToSun, f32 lux, glm::vec3 color = {1.0f, 0.96f, 0.9f}, f32 angularRadiusDeg = 0.27f);
    Entity pointLight(std::string_view name, glm::vec3 position, glm::vec3 color, f32 lumens, f32 range,
                      bool shadows = true, f32 sourceRadius = 0.05f, Entity parent = {});
    Entity spotLight(std::string_view name, glm::vec3 position, glm::vec3 target, glm::vec3 color, f32 lumens,
                     f32 range, f32 innerDeg, f32 outerDeg, bool shadows = true, f32 sourceRadius = 0.05f);
    Entity camera(std::string_view name, glm::vec3 position, glm::vec3 target, f32 fovDeg = 60.0f, bool primary = false);
    // Sky + environment (+ optional height fog); `sun` entity referenced by the environment.
    Entity environment(Entity sun, f32 skyIntensity = 1.0f, f32 ambient = 1.0f);
    // Global post-process volume (exposure + bloom + grading defaults of the showcase).
    Entity postProcess(std::optional<f32> fixedEv100 = std::nullopt, f32 exposureCompensation = 1.5f);
    // Endless meadow under the station floor (hides the sky's lower hemisphere at the horizon).
    Entity farGround(f32 y = -0.04f);

    gameplay::ScriptComponent& script(Entity e, const std::string& path);
    void prop(Entity e, const std::string& name, const std::string& text);
    void prop(Entity e, const std::string& name, f64 number);
    void prop(Entity e, const std::string& name, bool value);
    void prop(Entity e, const std::string& name, glm::vec3 v);
    void tag(Entity e, const std::string& tag);

    // Showcase scaffolding: Game entity (game.lua + station info), player + third-person camera, info board,
    // return portal, tour cameras.
    Entity game(const Station& s);
    Entity player(glm::vec3 feet, f32 yawDeg = 0.0f);
    Entity infoBoard(const Station& s, glm::vec3 position, f32 yawDeg);
    Entity portal(std::string_view name, const std::string& targetUri, const std::string& label, glm::vec3 color,
                  glm::vec3 position, f32 yawDeg, Entity parent = {});
    void tourCameras(glm::vec3 aPos, glm::vec3 aTarget, glm::vec3 bPos, glm::vec3 bTarget, f32 fov = 55.0f);
    // Standard station: game entity, player at `spawn`, info board, hub portal, a floor of `floorSize`.
    void stationBasics(const Station& s, glm::vec3 spawn, f32 spawnYaw, glm::vec3 boardPos, f32 boardYaw,
                       glm::vec3 portalPos, f32 portalYaw);

    void save(const std::string& relPath); // Assets/<relPath>

private:
    u32 m_counter = 0;
};

// Deterministic entity UUIDs for worlds built with World::create (prefab sources).
void stableIds(World& world, const std::string& seed);
// Prefab document with deterministic prefab-local entity ids (createPrefab draws random ones), binary encoded.
[[nodiscard]] std::vector<std::byte> stablePrefab(World& world, Entity root, const std::string& rel);

[[nodiscard]] glm::quat yawRotation(f32 degrees);
[[nodiscard]] glm::quat lookRotation(glm::vec3 from, glm::vec3 to);
[[nodiscard]] glm::quat eulerDeg(f32 pitch, f32 yaw, f32 roll);

// gen_assets.cpp
void generateTextures(Gen& gen);
void generateAudio(Gen& gen);
void generateMannequin(Gen& gen);
void generateMaterials(Gen& gen);
void generateProjectFile(Gen& gen);
void generatePrefabs(Gen& gen);
void generateData(Gen& gen); // stations.json, behaviour trees
// gen_hub.cpp
void buildMainMenu(Gen& gen);
void buildHub(Gen& gen);
// gen_stations_*.cpp
void buildLighting(Gen& gen);
void buildMaterials(Gen& gen);
void buildReflections(Gen& gen);
void buildVolumetrics(Gen& gen);
void buildWater(Gen& gen);
void buildWorld(Gen& gen);
void buildPhysics(Gen& gen);
void buildAnimation(Gen& gen);
void buildAI(Gen& gen);
void buildSplines(Gen& gen);
void buildAudio(Gen& gen);
void buildNetwork(Gen& gen);
void buildRtx(Gen& gen);
void buildSaves(Gen& gen);

// Mannequin asset paths (gen_assets.cpp).
inline constexpr const char* kMannequin = "Models/Mannequin.gltf";

} // namespace ox::showcase::gen
