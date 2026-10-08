#include "gen.hpp"

#include <oxwald/assets/asset_meta.hpp>
#include <oxwald/assets/asset_registry.hpp>
#include <oxwald/assets/importer.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/gameplay/asset_providers.hpp>
#include <oxwald/render/components/postprocess.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene_serializer.hpp>
#include <oxwald/gameplay/world/components.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace ox::showcase::gen {

// ---------------------------------------------------------------------------------------------------------------
// Stations (titles/descriptions shown on the in-world info boards and in the tour captions)

const std::vector<Station>& stations() {
    static const std::vector<Station> s = {
        {"lighting", "01_Lighting", "Свет и тени",
         "Солнце с каскадными тенями (CSM), прожекторы и точечные источники с тенями из атласа и кубических карт. "
         "PCSS делает полутень шире с расстоянием до заслоняющего объекта, а 256 цветных огней собираются "
         "кластерным forward+ за один проход.",
         "[T] — время суток · [L] — гирлянда 256 огней · [P] — PCSS вкл/выкл", "docs/guide/20-lighting-shadows.md",
         {1.0f, 0.75f, 0.3f}},
        {"materials", "02_Materials", "Материалы",
         "PBR по металличности/шероховатости: сетка сфер от диэлектрика к металлу, текстурированные legacy-модели с "
         "кожей и солдатом. Эмиссия, alpha-test листва, стекло с преломлением и поглощением по Беру–Ламберту, "
         "матовое стекло.",
         "Подойдите к стендам · [V] — режим отладки (альбедо / нормали / шероховатость)", "docs/guide/19-materials.md",
         {0.95f, 0.45f, 0.35f}},
        {"reflections", "03_Reflections", "Отражения и GI",
         "Зеркальный пол с экранными отражениями (SSR), комната с пробами отражений и box projection, плоское "
         "зеркало, GTAO в углах и объём освещённости: красная и зелёная стены окрашивают белый пол.",
         "[R] — SSR вкл/выкл · [G] — GTAO вкл/выкл", "docs/guide/21-reflections-gi.md", {0.4f, 0.8f, 1.0f}},
        {"volumetrics", "04_Volumetrics", "Волюметрика",
         "Froxel-туман с высотным спадом, лучи света сквозь окна собора, локальные объёмы тумана и объёмные "
         "облака. Цикл дня и ночи: закат, звёзды и луна.",
         "[T] — ускорить время · [N] — ночь/день", "docs/guide/22-volumetrics.md", {0.7f, 0.6f, 1.0f}},
        {"water", "05_Water", "Вода и частицы",
         "Волны Герстнера, на которых качаются физические ящики (плавучесть), каустика на дне и эффект под водой. "
         "GPU-частицы: огонь, дым и искры с коллизиями по буферу глубины.",
         "Зайдите в воду, чтобы нырнуть · [B] — бросить ящик", "docs/guide/23-transparency-water-particles.md",
         {0.25f, 0.65f, 0.95f}},
        {"world", "06_World", "Открытый мир",
         "Процедурный ландшафт с эрозией и слоями сплат-карты, растительность с ветром и импосторами вдали, "
         "источники стриминга подгружают чанки вокруг игрока.",
         "[Shift] — бег · [F] — показать отладку стриминга", "docs/guide/16-world.md", {0.45f, 0.85f, 0.4f}},
        {"physics", "07_Physics", "Физика",
         "Jolt: башни из ящиков, цепи на шарнирах, триггеры, персонаж толкает тела. Лучевое ружьё на Lua "
         "стреляет physics.raycast и прикладывает импульс.",
         "[ЛКМ] или [E] — выстрел · [R] — собрать башни заново", "docs/guide/09-physics.md", {1.0f, 0.55f, 0.2f}},
        {"animation", "08_Animation", "Анимация",
         "Скиннинговый манекен (сгенерирован процедурно в glTF): blend space ходьба/бег по скорости, two-bone IK "
         "ставит стопы на ступени, aim IK поворачивает голову к игроку.",
         "Подойдите ближе — манекены смотрят на вас · [1–3] — скорость", "docs/guide/10-animation.md",
         {0.95f, 0.85f, 0.4f}},
        {"ai", "09_AI", "Искусственный интеллект",
         "Навмеш Recast/Detour и охранники с деревьями поведения: патруль → погоня, когда видят игрока → поиск в "
         "последней известной точке. Отладка восприятия показывает конусы зрения.",
         "Попадитесь на глаза охране · [F] — отладка навмеша и восприятия", "docs/guide/13-ai.md",
         {1.0f, 0.3f, 0.3f}},
        {"splines", "10_Splines", "Сплайны и корутины",
         "Поезд едет по замкнутому сплайну с постоянной скоростью. Двери, лифт и диалог написаны корутинами на Lua "
         "с await: сценарий читается сверху вниз.",
         "[E] у пульта — открыть дверь, вызвать лифт, начать диалог", "docs/guide/08-coroutines.md",
         {0.3f, 0.9f, 0.8f}},
        {"audio", "11_Audio", "Звук",
         "3D-источники с затуханием и доплером, окклюзия за стенами, зона реверберации со своей шиной и "
         "ducking: объявление по громкой связи приглушает музыку.",
         "Обойдите стену с источником · [E] — объявление (ducking)", "docs/guide/12-audio.md",
         {0.85f, 0.4f, 0.95f}},
        {"network", "12_Network", "Сеть",
         "Listen-сервер прямо в игре: бот-клиент подключён через in-memory транспорт с задержкой и потерями. "
         "Полупрозрачные «призраки» — то, что видит клиент после репликации и интерполяции.",
         "[+]/[−] — задержка · [L] — потери пакетов", "docs/guide/14-networking.md", {0.35f, 0.6f, 1.0f}},
        {"rtx", "13_RTX", "RTX и апскейлеры",
         "Переключатели трассировки лучей (тени, отражения, GI, path tracer) и апскейлеров Off/FSR1/TAAU/DLSS. "
         "Недоступные опции показаны серыми с причиной; справа — GPU-тайминги проходов.",
         "[Tab] — панель RTX · [U] — следующий апскейлер · [[]/[]] — масштаб рендера", "docs/guide/24-ray-tracing.md",
         {0.5f, 1.0f, 0.3f}},
        {"saves", "14_SaveGames", "Сохранения",
         "Точки сохранения, быстрые сохранение/загрузка (F5/F9), индикатор автосохранения и браузер слотов. "
         "Состояние ящиков и переключателей возвращается после загрузки.",
         "[F5] — быстрое сохранение · [F9] — загрузка · [E] у терминала — слоты", "docs/guide/07-savegames.md",
         {0.9f, 0.9f, 0.9f}},
    };
    return s;
}

const Station& station(std::string_view id) {
    for (const Station& s : stations())
        if (s.id == id) return s;
    OX_ASSERT(false, "unknown station {}", id);
    return stations().front();
}

// ---------------------------------------------------------------------------------------------------------------
// Gen

Gen::Gen(fs::path projectDir) : project(std::move(projectDir)), assets(project / "Assets") {
    assets::AssetRegistry::Options o;
    o.deleteOrphanMetas = false; // never delete what a human may have added
    registry = std::make_unique<assets::AssetRegistry>(project, o);
    gameplay::registerGameplayImporters(registry->importers());
}

Gen::~Gen() = default;

static std::vector<std::byte> readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::vector<char> c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(c.size());
    std::memcpy(out.data(), c.data(), c.size());
    return out;
}

bool Gen::writeAsset(const std::string& rel, const std::vector<std::byte>& bytes) {
    const fs::path p = assets / rel;
    if (fs::exists(p) && readAll(p) == bytes) {
        ++filesUnchanged;
        return false;
    }
    if (checkOnly) {
        differences.push_back(rel);
        return true;
    }
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    ++filesWritten;
    return true;
}

bool Gen::writeText(const std::string& rel, const std::string& text) {
    std::vector<std::byte> b(text.size());
    std::memcpy(b.data(), text.data(), text.size());
    return writeAsset(rel, b);
}

Uuid Gen::meta(const std::string& rel, const nlohmann::ordered_json& settings, bool force) {
    const fs::path src = assets / rel;
    const fs::path metaPath = assets::metaPathFor(src);
    if (fs::exists(metaPath)) {
        if (auto m = assets::readMeta(metaPath)) {
            bool changed = false;
            for (auto it = settings.begin(); force && it != settings.end(); ++it) {
                if (!m->settings.contains(it.key()) || m->settings[it.key()] != it.value()) {
                    m->settings[it.key()] = it.value();
                    changed = true;
                }
            }
            if (changed && checkOnly) differences.push_back(rel + ".meta");
            else if (changed) (void)assets::writeMeta(metaPath, *m);
            return m->uuid;
        }
    }
    assets::AssetMeta m;
    m.uuid = assetId(rel);
    if (auto* imp = registry->importers().findForPath(src)) {
        m.importer = std::string(imp->name());
        m.importerVersion = imp->version();
        m.settings = imp->defaultSettingsFor(src);
    }
    for (auto it = settings.begin(); it != settings.end(); ++it) m.settings[it.key()] = it.value();
    if (checkOnly) {
        differences.push_back(rel + ".meta");
        return m.uuid;
    }
    (void)assets::writeMeta(metaPath, m);
    ++filesWritten;
    return m.uuid;
}

void Gen::scanAndMetaEverything() {
    for (const auto& entry : fs::recursive_directory_iterator(assets)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        if (p.extension() == ".meta" || p.filename().string().starts_with(".")) continue;
        if (!registry->importers().findForPath(p)) continue;
        const std::string rel = fs::relative(p, assets).generic_string();
        nlohmann::ordered_json settings = nlohmann::ordered_json::object();
        // Large legacy textures: keep imports fast and VRAM small.
        if (rel.starts_with("Legacy/") && (p.extension() == ".png" || p.extension() == ".jpg" || p.extension() == ".tga"))
            settings["maxSize"] = 2048;
        meta(rel, settings, /*force*/ false);
    }
    registry->scan();
}

Uuid Gen::material(const std::string& name, const assets::MaterialAsset& m) {
    const std::string rel = "Materials/" + name + ".oxmat";
    const std::string json = assets::materialToJson(m);
    writeText(rel, json);
    const fs::path metaPath = assets::metaPathFor(assets / rel);
    const Uuid id = materialId(name);
    if (!fs::exists(metaPath) && checkOnly) {
        differences.push_back(rel + ".meta");
    } else if (!fs::exists(metaPath)) {
        assets::AssetMeta meta;
        meta.uuid = id;
        if (auto* imp = registry->importers().findForPath(assets / rel)) {
            meta.importer = std::string(imp->name());
            meta.importerVersion = imp->version();
            meta.settings = imp->defaultSettings();
        }
        (void)assets::writeMeta(metaPath, meta);
    }
    m_materials[name] = id;
    return id;
}

Uuid Gen::mat(const std::string& name) const {
    auto it = m_materials.find(name);
    OX_ASSERT(it != m_materials.end(), "material '{}' not defined", name);
    return it == m_materials.end() ? Uuid{} : it->second;
}

std::optional<Uuid> Gen::imported(const std::string& assetPath) {
    const auto hash = assetPath.find('#');
    if (hash != std::string::npos) {
        if (auto main = registry->uuidForPath(assetPath.substr(0, hash))) {
            (void)registry->import(*main);
        }
    }
    return registry->uuidForPath(assetPath);
}

std::vector<std::string> Gen::subAssets(const std::string& sourceRel, const std::string& prefix) {
    // Sub-assets are numbered ("Material/0", "Material/1", ...): probe until the first gap.
    std::vector<std::string> out;
    for (int i = 0; i < 256; ++i) {
        const std::string path = sourceRel + "#" + prefix + std::to_string(i);
        if (!imported(path)) break;
        out.push_back(path);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// helpers

void stableIds(World& world, const std::string& seed) {
    u32 index = 0;
    std::vector<entt::entity> all;
    world.forEachInHierarchy([&](entt::entity e) { all.push_back(e); });
    for (entt::entity e : all) world.setUuid(e, Uuid::fromName("showcase.prefab." + seed + "." + std::to_string(index++)));
}

std::vector<std::byte> stablePrefab(World& world, Entity root, const std::string& rel) {
    stableIds(world, rel);
    const serial::Document doc = createPrefab(world, root, {.prefabId = Gen::assetId(rel), .linkSource = false});
    std::string json = serial::toJsonString(doc);
    const serial::Value* entities = doc.root.find("entities");
    for (usize i = 0; entities && i < entities->size(); ++i) {
        const std::string from = entities->at(i).find("id")->getUuid().toString();
        const std::string to = Uuid::fromName("showcase.prefab-local." + rel + "." + std::to_string(i)).toString();
        for (usize pos = json.find(from); pos != std::string::npos; pos = json.find(from, pos + to.size())) json.replace(pos, from.size(), to);
    }
    auto bin = serial::jsonToBinary(json);
    OX_ASSERT(bool(bin), "prefab json round trip failed");
    return bin ? *bin : serial::encodeBinary(doc);
}

glm::quat yawRotation(f32 degrees) { return glm::angleAxis(glm::radians(degrees), glm::vec3(0, 1, 0)); }

glm::quat lookRotation(glm::vec3 from, glm::vec3 to) {
    glm::vec3 d = to - from;
    if (glm::length(d) < 1e-5f) return glm::quat(1, 0, 0, 0);
    d = glm::normalize(d);
    const glm::vec3 up = std::abs(d.y) > 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    return glm::quatLookAt(d, up);
}

glm::quat eulerDeg(f32 pitch, f32 yaw, f32 roll) {
    return glm::quat(glm::radians(glm::vec3(pitch, yaw, roll)));
}

// ---------------------------------------------------------------------------------------------------------------
// SceneBuilder

SceneBuilder::SceneBuilder(Gen& g, std::string name) : gen(g), sceneName(std::move(name)) {}

Entity SceneBuilder::create(std::string_view name, Entity parent) {
    const Uuid id = Uuid::fromName("showcase.entity." + sceneName + "." + std::to_string(m_counter++) + "." + std::string(name));
    return world.createWithId(id, name, parent);
}

Entity SceneBuilder::mesh(std::string_view name, const Uuid& meshId, const std::string& material, glm::vec3 position,
                          glm::vec3 scale, glm::quat rotation, Entity parent) {
    Entity e = create(name, parent);
    e.setPosition(position);
    e.setRotation(rotation);
    e.setScale(scale);
    auto& mr = e.add<MeshRendererComponent>();
    mr.mesh = meshId;
    if (!material.empty()) mr.materials = {gen.mat(material)};
    return e;
}

Entity SceneBuilder::prim(std::string_view name, Primitive p, const std::string& material, glm::vec3 position,
                          glm::vec3 scale, glm::quat rotation, Entity parent) {
    return mesh(name, render::primitiveUuid(p), material, position, scale, rotation, parent);
}

Entity SceneBuilder::block(std::string_view name, const std::string& material, glm::vec3 center, glm::vec3 size,
                           glm::quat rotation, Entity parent) {
    Entity e = prim(name, Primitive::Cube, material, center, size, rotation, parent);
    auto& c = e.add<gameplay::ColliderComponent>();
    c.type = gameplay::ColliderType::Box;
    c.halfExtents = glm::vec3(0.5f); // × entity scale
    return e;
}

Entity SceneBuilder::body(std::string_view name, Primitive p, const std::string& material, glm::vec3 position,
                          glm::vec3 size, f32 mass, glm::quat rotation) {
    Entity e = prim(name, p, material, position, size, rotation);
    auto& rb = e.add<gameplay::RigidBodyComponent>();
    rb.motionType = physics::MotionType::Dynamic;
    rb.mass = mass;
    auto& c = e.add<gameplay::ColliderComponent>();
    switch (p) {
    case Primitive::Sphere:
        c.type = gameplay::ColliderType::Sphere;
        c.radius = 0.5f;
        break;
    case Primitive::Cylinder:
        c.type = gameplay::ColliderType::Cylinder;
        c.radius = 0.5f;
        c.halfHeight = 0.5f;
        break;
    case Primitive::Capsule:
        c.type = gameplay::ColliderType::Capsule;
        c.radius = 0.5f;
        c.halfHeight = 0.25f;
        break;
    default:
        c.type = gameplay::ColliderType::Box;
        c.halfExtents = glm::vec3(0.5f);
        break;
    }
    return e;
}

Entity SceneBuilder::sun(glm::vec3 directionToSun, f32 lux, glm::vec3 color, f32 angularRadiusDeg) {
    Entity e = create("Sun");
    // Directional lights shine along their -Z axis.
    e.setRotation(lookRotation(glm::vec3(0), -glm::normalize(directionToSun)));
    auto& l = e.add<LightComponent>();
    l.type = LightType::Directional;
    l.color = color;
    l.intensity = lux;
    l.castShadows = true;
    l.sourceRadius = angularRadiusDeg;
    l.shadowNormalBias = 0.02f;
    return e;
}

Entity SceneBuilder::pointLight(std::string_view name, glm::vec3 position, glm::vec3 color, f32 lumens, f32 range,
                                bool shadows, f32 sourceRadius, Entity parent) {
    Entity e = create(name, parent);
    e.setPosition(position);
    auto& l = e.add<LightComponent>();
    l.type = LightType::Point;
    l.color = color;
    l.intensity = lumens;
    l.range = range;
    l.castShadows = shadows;
    l.sourceRadius = sourceRadius;
    return e;
}

Entity SceneBuilder::spotLight(std::string_view name, glm::vec3 position, glm::vec3 target, glm::vec3 color,
                               f32 lumens, f32 range, f32 innerDeg, f32 outerDeg, bool shadows, f32 sourceRadius) {
    Entity e = create(name);
    e.setPosition(position);
    e.setRotation(lookRotation(position, target));
    auto& l = e.add<LightComponent>();
    l.type = LightType::Spot;
    l.color = color;
    l.intensity = lumens;
    l.range = range;
    l.innerConeAngle = innerDeg;
    l.outerConeAngle = outerDeg;
    l.castShadows = shadows;
    l.sourceRadius = sourceRadius;
    return e;
}

Entity SceneBuilder::camera(std::string_view name, glm::vec3 position, glm::vec3 target, f32 fovDeg, bool primary) {
    Entity e = create(name);
    e.setPosition(position);
    e.setRotation(lookRotation(position, target));
    auto& c = e.add<CameraComponent>();
    c.verticalFov = fovDeg;
    c.nearPlane = 0.05f;
    c.farPlane = 4000.0f;
    c.primary = primary;
    return e;
}

Entity SceneBuilder::environment(Entity sunEntity, f32 skyIntensity, f32 ambient) {
    Entity e = create("Environment");
    auto& env = e.add<EnvironmentComponent>();
    env.sun = sunEntity ? sunEntity.ref() : EntityRef{};
    env.skyIntensity = skyIntensity;
    env.ambientIntensity = ambient;
    return e;
}

Entity SceneBuilder::postProcess(std::optional<f32> fixedEv100, f32 exposureCompensation) {
    Entity e = create("PostProcess");
    auto& v = e.add<render::PostProcessVolumeComponent>();
    v.unbound = true;
    auto& s = v.settings;
    s.overrideExposure = true;
    s.autoExposure = !fixedEv100.has_value();
    s.exposureCompensation = exposureCompensation;
    s.minEV100 = 2.0f;
    s.maxEV100 = 16.0f;
    s.overrideBloom = true;
    s.bloomIntensity = 0.05f;
    s.overrideLens = true;
    s.vignetteIntensity = 0.25f;
    s.filmGrainIntensity = 0.0f;
    s.chromaticAberration = 0.0f;
    s.overrideGrading = true;
    s.contrast = 1.05f;
    s.saturation = 1.05f;
    (void)fixedEv100;
    return e;
}

Entity SceneBuilder::farGround(f32 y) {
    Entity e = prim("FarGround", Primitive::Plane, "Meadow", {0, y, 0}, {6000.0f, 1.0f, 6000.0f});
    e.get<MeshRendererComponent>().castShadows = false;
    auto& c = e.add<gameplay::ColliderComponent>();
    c.halfExtents = {0.5f, 0.1f, 0.5f};
    c.offsetPosition = {0, -0.1f, 0};
    return e;
}

gameplay::ScriptComponent& SceneBuilder::script(Entity e, const std::string& path) {
    auto& s = e.addOrReplace<gameplay::ScriptComponent>();
    s.script = path;
    return s;
}

void SceneBuilder::prop(Entity e, const std::string& name, const std::string& text) {
    gameplay::ScriptPropertyValue v;
    v.type = script::ScriptPropertyType::String;
    v.text = text;
    e.get<gameplay::ScriptComponent>().properties[name] = v;
}
void SceneBuilder::prop(Entity e, const std::string& name, f64 number) {
    e.get<gameplay::ScriptComponent>().properties[name] = gameplay::ScriptPropertyValue::makeNumber(number);
}
void SceneBuilder::prop(Entity e, const std::string& name, bool value) {
    gameplay::ScriptPropertyValue v;
    v.type = script::ScriptPropertyType::Bool;
    v.boolean = value;
    e.get<gameplay::ScriptComponent>().properties[name] = v;
}
void SceneBuilder::prop(Entity e, const std::string& name, glm::vec3 vec) {
    gameplay::ScriptPropertyValue v;
    v.type = script::ScriptPropertyType::Vec3;
    v.vector = glm::vec4(vec, 0.0f);
    e.get<gameplay::ScriptComponent>().properties[name] = v;
}

void SceneBuilder::tag(Entity e, const std::string& t) {
    auto& tags = e.tryGet<TagComponent>() ? e.get<TagComponent>() : e.add<TagComponent>();
    if (!tags.has(t)) tags.tags.push_back(t);
}

Entity SceneBuilder::game(const Station& s) {
    Entity g = create("Game");
    script(g, "Scripts/game.lua");
    prop(g, "station", s.id);
    prop(g, "title", s.title);
    prop(g, "description", s.description);
    prop(g, "hints", s.hints);
    prop(g, "guide", s.guide);
    return g;
}

Entity SceneBuilder::player(glm::vec3 feet, f32 yawDeg) {
    Entity p = create("Player");
    p.setPosition(feet);
    p.setRotation(yawRotation(yawDeg));
    tag(p, "Player");
    auto& cc = p.add<gameplay::CharacterControllerComponent>();
    cc.height = 1.8f;
    cc.radius = 0.32f;
    cc.jumpSpeed = 5.2f;
    cc.maxStepHeight = 0.4f;
    script(p, "Scripts/player.lua");
    // Visual: the legacy Bio-soldier (Z-up OBJ, 1.45 m) when imported, a capsule otherwise.
    if (auto soldier = gen.imported("Legacy/Soldier/soldier.obj#Mesh/0")) {
        Entity v = create("PlayerModel", p);
        v.setRotation(glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0)) );
        v.setScale(glm::vec3(1.22f));
        auto& mr = v.add<MeshRendererComponent>();
        mr.mesh = *soldier;
        for (const std::string& m : gen.subAssets("Legacy/Soldier/soldier.obj", "Material/"))
            if (auto id = gen.imported(m)) mr.materials.push_back(*id);
    } else {
        prim("PlayerModel", Primitive::Capsule, "PlayerBody", {0, 0.9f, 0}, {0.64f, 1.8f, 0.64f}, {}, p);
    }
    // Third-person camera (camera.lua follows the player; the first active listener hears the world).
    Entity cam = camera("PlayerCamera", feet + yawRotation(yawDeg) * glm::vec3(0, 2.2f, 4.5f), feet + glm::vec3(0, 1.4f, 0),
                        60.0f, true);
    cam.add<gameplay::AudioListenerComponent>();
    script(cam, "Scripts/camera.lua");
    return p;
}

Entity SceneBuilder::infoBoard(const Station& s, glm::vec3 position, f32 yawDeg) {
    Entity root = create("InfoBoard");
    root.setPosition(position);
    root.setRotation(yawRotation(yawDeg));
    prim("Post", Primitive::Cylinder, "DarkMetal", {0, 0.9f, -0.05f}, {0.12f, 1.8f, 0.12f}, {}, root);
    prim("Panel", Primitive::Cube, "DarkMetal", {0, 1.85f, 0}, {1.9f, 1.15f, 0.08f}, {}, root);
    Entity screen = prim("Screen", Primitive::Cube, "Accent." + s.id, {0, 1.85f, 0.045f}, {1.75f, 1.0f, 0.02f}, {}, root);
    screen.get<MeshRendererComponent>().castShadows = false;
    pointLight("BoardLight", {0, 1.9f, 0.6f}, s.color, 120.0f, 3.0f, false, 0.05f, root);
    return root;
}

Entity SceneBuilder::portal(std::string_view name, const std::string& targetUri, const std::string& label,
                            glm::vec3 color, glm::vec3 position, f32 yawDeg, Entity parent) {
    Entity root = create(name, parent);
    root.setPosition(position);
    root.setRotation(yawRotation(yawDeg));
    const std::string frame = "PortalFrame";
    block("PillarL", frame, {-1.35f, 1.6f, 0}, {0.3f, 3.2f, 0.4f}, {}, root);
    block("PillarR", frame, {1.35f, 1.6f, 0}, {0.3f, 3.2f, 0.4f}, {}, root);
    block("Lintel", frame, {0, 3.35f, 0}, {3.0f, 0.3f, 0.4f}, {}, root);
    Entity veil = prim("Veil", Primitive::Cube, label.empty() ? "PortalVeil" : "PortalVeil", {0, 1.6f, 0}, {2.4f, 3.2f, 0.04f}, {}, root);
    veil.get<MeshRendererComponent>().castShadows = false;
    // Station-coloured glow strip on the lintel.
    Entity strip = prim("Glow", Primitive::Cube, "Glow." + label, {0, 3.2f, 0.21f}, {2.6f, 0.08f, 0.04f}, {}, root);
    strip.get<MeshRendererComponent>().castShadows = false;
    pointLight("PortalLight", {0, 2.4f, 0.9f}, color, 600.0f, 6.0f, false, 0.1f, root);
    Entity trigger = create("Trigger", root);
    trigger.setPosition({0, 1.2f, 0});
    auto& c = trigger.add<gameplay::ColliderComponent>();
    c.type = gameplay::ColliderType::Box;
    c.halfExtents = {1.1f, 1.2f, 0.45f};
    c.isSensor = true;
    trigger.add<gameplay::TriggerComponent>().requiredTag = "Player";
    script(trigger, "Scripts/portal.lua");
    prop(trigger, "target", targetUri);
    prop(trigger, "label", label);
    return root;
}

void SceneBuilder::tourCameras(glm::vec3 aPos, glm::vec3 aTarget, glm::vec3 bPos, glm::vec3 bTarget, f32 fov) {
    camera("TourCam.A", aPos, aTarget, fov, false);
    camera("TourCam.B", bPos, bTarget, fov, false);
}

void SceneBuilder::stationBasics(const Station& s, glm::vec3 spawn, f32 spawnYaw, glm::vec3 boardPos, f32 boardYaw,
                                 glm::vec3 portalPos, f32 portalYaw) {
    game(s);
    player(spawn, spawnYaw);
    infoBoard(s, boardPos, boardYaw);
    portal("HubPortal", kHubUri, "hub", {0.6f, 0.8f, 1.0f}, portalPos, portalYaw);
}

void SceneBuilder::save(const std::string& relPath) {
    world.updateTransforms();
    gen.writeAsset(relPath, serial::encodeBinary(serializeWorld(world)));
    gen.meta(relPath);
    OX_LOG_INFO("generate", "scene {} ({} entities)", relPath, world.entityCount());
}

} // namespace ox::showcase::gen
