#include "integration/gameplay_tools.hpp"

#include "core/editor_context.hpp"
#include "core/scene_templates.hpp"
#include "viewport/painter_renderer.hpp"

#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/components.hpp>

#if OX_EDITOR_HAS_GAMEPLAY
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/script/script_instance.hpp>
#include <oxwald/script/script_vm.hpp>
#if defined(OX_GAMEPLAY_HAS_WORLD)
#include <oxwald/gameplay/world.hpp>
#endif
#endif
#if OX_EDITOR_HAS_ASSETS
#include <oxwald/assets/asset_manager.hpp>
#include <oxwald/assets/mesh.hpp>
#endif

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <map>
#include <memory>

namespace ox::editor {

bool gameplayAvailable() { return OX_EDITOR_HAS_GAMEPLAY != 0; }

#if OX_EDITOR_HAS_GAMEPLAY

namespace {

using namespace ox::gameplay;

const reflect::TypeInfo* fieldType(const char* component, const char* field) {
    const ComponentInfo* info = ComponentRegistry::instance().find(component);
    if (!info || !info->type) return nullptr;
    for (const auto& f : info->type->fields) {
        if (f.name == field) return f.type;
    }
    return nullptr;
}

serial::Value fromScriptValue(const script::ScriptValue& v) {
    return std::visit(
        [](const auto& x) -> serial::Value {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, bool>) return serial::Value::makeBool(x);
            else if constexpr (std::is_same_v<T, i64>) return serial::Value::makeInt(x);
            else if constexpr (std::is_same_v<T, f64>) return serial::Value::makeF64(x);
            else if constexpr (std::is_same_v<T, std::string>) return serial::Value::makeString(x);
            else if constexpr (std::is_same_v<T, glm::vec2>) return serial::Value::makeVec2(x);
            else if constexpr (std::is_same_v<T, glm::vec3>) return serial::Value::makeVec3(x);
            else if constexpr (std::is_same_v<T, glm::vec4>) return serial::Value::makeVec4(x);
            else return serial::Value{};
        },
        v);
}

ScriptPropertyValue toPropertyValue(int type, const serial::Value& v) {
    ScriptPropertyValue p;
    p.type = script::ScriptPropertyType(type);
    switch (p.type) {
    case script::ScriptPropertyType::Float:
    case script::ScriptPropertyType::Int: p.number = v.isNumber() ? v.getDouble() : 0.0; break;
    case script::ScriptPropertyType::Bool: p.boolean = v.getBool(); break;
    case script::ScriptPropertyType::String: p.text = v.getString(); break;
    default: p.vector = v.getVec(); break;
    }
    return p;
}

serial::Value editorValue(const ScriptPropertyValue& p) {
    switch (p.type) {
    case script::ScriptPropertyType::Float: return serial::Value::makeF64(p.number);
    case script::ScriptPropertyType::Int: return serial::Value::makeInt(i64(std::llround(p.number)));
    case script::ScriptPropertyType::Bool: return serial::Value::makeBool(p.boolean);
    case script::ScriptPropertyType::String: return serial::Value::makeString(p.text);
    case script::ScriptPropertyType::Vec2: return serial::Value::makeVec2(glm::vec2(p.vector));
    case script::ScriptPropertyType::Vec3: return serial::Value::makeVec3(glm::vec3(p.vector));
    default: return serial::Value::makeVec4(p.vector);
    }
}

// Sandboxed VM used only to read `properties = {...}` declarations (no engine APIs bound: top-level code that
// touches the scene fails harmlessly). Cached by script identity + source text.
struct ScriptIntrospector {
    std::unique_ptr<script::ScriptVM> vm;
    struct Entry {
        std::string source;
        std::shared_ptr<script::ScriptAsset> asset;
    };
    std::map<std::string, Entry> cache;

    std::shared_ptr<script::ScriptAsset> load(const std::string& key, const std::string& source) {
        if (!vm) {
            script::ScriptVMConfig c;
            c.hotReloadInterval = 0.0;
            c.instructionLimit = 2'000'000;
            c.logCategory = "script.inspect";
            vm = std::make_unique<script::ScriptVM>(c);
        }
        auto it = cache.find(key);
        if (it != cache.end() && it->second.source == source) return it->second.asset;
        auto asset = vm->loadScriptFromString(key, source);
        cache[key] = Entry{source, asset};
        return asset;
    }
};

ScriptIntrospector& introspector() {
    static ScriptIntrospector s;
    return s;
}

// Source text of the entity's script: asset id (IScriptSourceProvider / asset backend) or name/path.
bool resolveScriptSource(EditorContext& ctx, const ScriptComponent& c, std::string& key, std::string& source, QString& display) {
    auto readFile = [&](const QString& path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        source = f.readAll().toStdString();
        key = QFileInfo(path).absoluteFilePath().toStdString();
        display = QFileInfo(path).fileName();
        return true;
    };
    auto fromProvider = [&](const std::optional<ScriptSource>& s) {
        if (!s) return false;
        if (!s->source.empty()) {
            source = s->source;
            key = s->name.empty() ? s->path : s->name;
            display = QString::fromStdString(key);
            return true;
        }
        return !s->path.empty() && readFile(QString::fromStdString(s->path));
    };
    auto* provider = ctx.engineServices().tryGet<IScriptSourceProvider>();
    if (c.asset.isValid()) {
        if (provider && fromProvider(provider->scriptById(c.asset))) return true;
        if (auto a = ctx.services().assets().find(c.asset)) return readFile(a->path);
        return false;
    }
    if (c.script.empty()) return false;
    if (provider && fromProvider(provider->scriptByName(c.script))) return true;
    const QString rel = QString::fromStdString(c.script);
    if (QFileInfo(rel).isAbsolute()) return readFile(rel);
    if (Project* p = ctx.project()) {
        for (const QString& base : {p->contentDir(), p->rootDir(), QDir(p->rootDir()).filePath(QStringLiteral("scripts"))}) {
            if (QFileInfo::exists(QDir(base).filePath(rel))) return readFile(QDir(base).filePath(rel));
        }
    }
    return false;
}

#if OX_EDITOR_HAS_ASSETS
AABB meshLocalBounds(EditorContext& ctx, const Uuid& mesh) {
    if (!builtin::primitiveName(mesh).isEmpty()) return AABB::fromCenterExtents(glm::vec3(0), glm::vec3(0.5f));
    if (auto* m = ctx.engineServices().tryGet<assets::AssetManager>()) {
        auto h = m->loadSync<assets::MeshData>(mesh);
        if (const assets::MeshData* d = h.get()) return d->bounds;
    }
    return {};
}
#else
AABB meshLocalBounds(EditorContext&, const Uuid& mesh) {
    if (!builtin::primitiveName(mesh).isEmpty()) return AABB::fromCenterExtents(glm::vec3(0), glm::vec3(0.5f));
    return {};
}
#endif

} // namespace

void applyGameplayDebugFlags(Services& services, const ShowFlags& flags) {
    if (auto* p = services.tryGet<PhysicsRuntime>()) {
        p->debugDraw = flags.debugDraw && (flags.physicsColliders || flags.physicsContacts);
        p->debugOptions.shapes = flags.physicsColliders;
        p->debugOptions.contacts = flags.physicsContacts;
        p->debugOptions.characters = flags.physicsColliders;
        p->debugOptions.constraints = flags.physicsColliders;
    }
    if (auto* a = services.tryGet<AIRuntime>()) a->debugDraw = flags.debugDraw && flags.navigation;
    if (auto* s = services.tryGet<SplineRuntime>()) s->debugDraw = flags.debugDraw && flags.splines;
    if (auto* an = services.tryGet<AnimationRuntime>()) an->debugDraw = flags.debugDraw && flags.skeletons;
    if (auto* au = services.tryGet<AudioRuntime>()) au->debugDraw = flags.debugDraw && flags.audio;
}

BakeResult bakeNavMesh(EditorContext& ctx, const Uuid& surfaceId) {
    if (ctx.isPlaying()) return {false, QObject::tr("Stop play mode to bake the navigation mesh")};
    World& w = ctx.editWorld();
    auto* ai = ctx.engineServices().tryGet<AIRuntime>();
    if (!ai) return {false, QObject::tr("The AI runtime is not running (engine without gameplay)")};
    Entity surface = surfaceId.isNil() ? Entity{} : w.find(surfaceId);
    if (!surface) {
        for (auto [h, s] : w.registry().view<NavMeshSurfaceComponent>().each()) {
            surface = w.wrap(h);
            break;
        }
    }
    if (!surface || !surface.has<NavMeshSurfaceComponent>()) return {false, QObject::tr("No NavMeshSurface in the scene")};
    const Uuid id = surface.uuid();
    const auto before = props::read(w, id, "NavMeshSurface", "bakedData");
    QElapsedTimer t;
    t.start();
    const bool ok = ai->bake(surface);
    const auto after = props::read(w, id, "NavMeshSurface", "bakedData");
    if (!ok || !after || !before) return {false, QObject::tr("Navigation mesh bake failed (no walkable geometry?)")};
    // Record the new data as an undoable edit.
    props::write(w, id, "NavMeshSurface", "bakedData", *before);
    ctx.setProperty({id}, "NavMeshSurface", "bakedData", *after);
    const usize bytes = surface.get<NavMeshSurfaceComponent>().bakedData.size();
    return {true, QObject::tr("Navigation mesh baked in %1 ms (%2)").arg(t.elapsed()).arg(formatBytes(bytes))};
}

BtSnapshot behaviorTreeSnapshot(Services& services, World& world, const Uuid& entity) {
    BtSnapshot snap;
    Entity e = world.find(entity);
    const auto* comp = e ? e.tryGet<BehaviorTreeComponent>() : nullptr;
    if (!comp) return snap;
    snap.hasComponent = true;
    snap.source = comp->tree.isValid() ? QObject::tr("asset %1").arg(qs(comp->tree.toString()).left(8)) : QObject::tr("inline JSON");
    auto* ai = services.tryGet<AIRuntime>();
    if (ai::BehaviorTree* tree = ai ? ai->behaviorTree(e) : nullptr) {
        snap.running = true;
        snap.tickCount = tree->tickCount();
        for (const auto& t : tree->trace()) {
            snap.nodes.push_back({t.id, t.parentId, t.depth, qs(t.name), qs(t.type), int(t.status), t.lastTick, t.tickedThisFrame});
        }
        for (const auto& [k, v] : tree->blackboard().values()) {
            const QString text = std::visit(
                [&world](const auto& x) -> QString {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, std::monostate>) return QStringLiteral("—");
                    else if constexpr (std::is_same_v<T, bool>) return x ? QStringLiteral("true") : QStringLiteral("false");
                    else if constexpr (std::is_same_v<T, std::string>) return qs(x);
                    else if constexpr (std::is_same_v<T, glm::vec3>) return QStringLiteral("(%1, %2, %3)").arg(x.x, 0, 'f', 2).arg(x.y, 0, 'f', 2).arg(x.z, 0, 'f', 2);
                    else if constexpr (std::is_same_v<T, u64>) {
                        // Entity ids in blackboards are runtime ids (toBlackboardId).
                        if (Entity e = entityFromRuntimeId(world, x)) return QObject::tr("%1 (entity)").arg(qs(e.name()));
                        return QString::number(x);
                    }
                    else return QString::number(x);
                },
                v);
            snap.blackboard.emplace_back(qs(k), text);
        }
        std::sort(snap.blackboard.begin(), snap.blackboard.end());
        return snap;
    }
    // Not running (edit mode): show the static structure of the definition.
    std::optional<nlohmann::json> def;
    if (comp->tree.isValid()) {
        if (auto* p = services.tryGet<IBehaviorTreeProvider>()) def = p->behaviorTree(comp->tree);
    } else if (!comp->treeJson.empty()) {
        auto j = nlohmann::json::parse(comp->treeJson, nullptr, false);
        if (!j.is_discarded()) def = j;
    }
    if (!def) return snap;
    u32 next = 0;
    std::function<void(const nlohmann::json&, u32, u32)> walk = [&](const nlohmann::json& n, u32 parent, u32 depth) {
        if (!n.is_object()) return;
        const u32 id = next++;
        BtNodeRow r;
        r.id = id;
        r.parentId = depth == 0 ? id : parent;
        r.depth = depth;
        r.type = qs(n.value("type", std::string("?")));
        r.name = qs(n.value("name", std::string()));
        snap.nodes.push_back(r);
        if (n.contains("children") && n["children"].is_array()) {
            for (const auto& c : n["children"]) walk(c, id, depth + 1);
        }
        if (n.contains("child")) walk(n["child"], id, depth + 1);
    };
    walk(def->contains("root") ? (*def)["root"] : *def, 0, 0);
    for (const auto& b : comp->blackboard) snap.blackboard.emplace_back(qs(b.key), QObject::tr("(initial)"));
    return snap;
}

bool fitColliderToMesh(EditorContext& ctx, const UuidList& entities, QString* message) {
    World& w = ctx.world();
    UuidList ids;
    std::vector<serial::Value> values;
    for (const Uuid& id : entities) {
        Entity e = w.find(id);
        if (!e || !e.has<ColliderComponent>()) continue;
        AABB local;
        if (const auto* mr = e.tryGet<MeshRendererComponent>()) local = meshLocalBounds(ctx, mr->mesh);
        if (!local.valid()) {
            // Children meshes in the entity's local space.
            const glm::mat4 inv = glm::inverse(e.worldMatrix());
            for (Entity c : e.children()) {
                if (const auto* cmr = c.tryGet<MeshRendererComponent>()) {
                    const AABB b = meshLocalBounds(ctx, cmr->mesh);
                    if (b.valid()) local.expand(b.transformed(inv * c.worldMatrix()));
                }
            }
        }
        if (!local.valid()) continue;
        ColliderComponent c = e.get<ColliderComponent>();
        const glm::vec3 half = glm::max(local.extents(), glm::vec3(0.005f));
        switch (c.type) {
        case ColliderType::Sphere: c.radius = std::max({half.x, half.y, half.z}); break;
        case ColliderType::Capsule:
            c.radius = std::max(half.x, half.z);
            c.halfHeight = std::max(0.0f, half.y - c.radius);
            break;
        case ColliderType::Cylinder:
            c.radius = std::max(half.x, half.z);
            c.halfHeight = half.y;
            break;
        default:
            c.type = c.type == ColliderType::Mesh || c.type == ColliderType::ConvexHull ? c.type : ColliderType::Box;
            c.halfExtents = half;
            break;
        }
        c.offsetPosition = local.center();
        c.offsetRotation = glm::quat(1, 0, 0, 0);
        c.scale = glm::vec3(1.0f);
        ids.push_back(id);
        values.push_back(serial::toValue(c));
    }
    if (ids.empty()) {
        if (message) *message = QObject::tr("No mesh bounds found (needs a MeshRenderer with a loaded mesh)");
        return false;
    }
    ctx.setProperty(ids, "Collider", "", values);
    if (message) *message = QObject::tr("Collider fitted to the mesh bounds");
    return true;
}

ScriptPropertiesResult scriptProperties(EditorContext& ctx, const Uuid& entity) {
    ScriptPropertiesResult r;
    Entity e = ctx.world().find(entity);
    const auto* c = e ? e.tryGet<ScriptComponent>() : nullptr;
    if (!c) return r;
    r.hasScript = true;
    std::string key, source;
    if (!resolveScriptSource(ctx, *c, key, source, r.scriptName)) {
        r.error = c->script.empty() && c->asset.isNil() ? QObject::tr("No script assigned") : QObject::tr("Script source not found");
        return r;
    }
    auto asset = introspector().load(key, source);
    if (!asset || !asset->valid()) {
        r.error = QObject::tr("The script has errors (see the Console)");
        if (!asset) return r;
    }
    for (const auto& d : asset->properties()) {
        ScriptPropertyRow row;
        row.name = d.name;
        row.type = int(d.type);
        row.defaultValue = fromScriptValue(d.defaultValue.index() == 0 ? script::defaultValueFor(d.type) : d.defaultValue);
        row.min = d.min;
        row.max = d.max;
        row.tooltip = qs(d.tooltip);
        if (auto it = c->properties.find(d.name); it != c->properties.end()) {
            row.overridden = true;
            row.currentValue = editorValue(it->second);
        } else {
            row.currentValue = row.defaultValue;
        }
        r.rows.push_back(std::move(row));
    }
    return r;
}

void setScriptPropertyOverride(EditorContext& ctx, const UuidList& entities, const std::string& name, int type,
                               const std::optional<serial::Value>& value, EditPhase phase) {
    const reflect::TypeInfo* mapType = fieldType("Script", "properties");
    if (!mapType) return;
    World& w = ctx.world();
    UuidList ids;
    std::vector<serial::Value> values;
    for (const Uuid& id : entities) {
        Entity e = w.find(id);
        if (!e || !e.has<ScriptComponent>()) continue;
        auto props = e.get<ScriptComponent>().properties;
        if (value) props[name] = toPropertyValue(type, *value);
        else props.erase(name);
        ids.push_back(id);
        values.push_back(serial::toValue(&props, *mapType));
    }
    if (!ids.empty()) ctx.setProperty(ids, "Script", "properties", values, phase);
}

bool hasSpline(World& world, const Uuid& entity) {
    Entity e = world.find(entity);
    return e && e.has<SplineComponent>();
}

std::vector<glm::vec3> splinePointsWorld(World& world, const Uuid& entity) {
    std::vector<glm::vec3> out;
    Entity e = world.find(entity);
    if (!e || !e.has<SplineComponent>()) return out;
    const glm::mat4 m = e.worldMatrix();
    for (const auto& p : e.get<SplineComponent>().points) out.push_back(glm::vec3(m * glm::vec4(p.position, 1.0f)));
    return out;
}

void setSplinePointWorld(EditorContext& ctx, const Uuid& entity, int index, const glm::vec3& worldPos, EditPhase phase) {
    Entity e = ctx.world().find(entity);
    if (!e || !e.has<SplineComponent>() || index < 0 || index >= int(e.get<SplineComponent>().points.size())) return;
    const glm::vec3 local = glm::vec3(glm::inverse(e.worldMatrix()) * glm::vec4(worldPos, 1.0f));
    ctx.setProperty({entity}, "Spline", "points[" + std::to_string(index) + "].position", serial::Value::makeVec3(local), phase);
}

int insertSplinePoint(EditorContext& ctx, const Uuid& entity, int after, const glm::vec3& worldPos) {
    Entity e = ctx.world().find(entity);
    const reflect::TypeInfo* t = fieldType("Spline", "points");
    if (!e || !e.has<SplineComponent>() || !t) return -1;
    auto points = e.get<SplineComponent>().points;
    SplinePoint p;
    p.position = glm::vec3(glm::inverse(e.worldMatrix()) * glm::vec4(worldPos, 1.0f));
    const int at = after < 0 || after >= int(points.size()) ? int(points.size()) : after + 1;
    points.insert(points.begin() + at, p);
    ctx.setProperty({entity}, "Spline", "points", serial::toValue(&points, *t));
    return at;
}

void removeSplinePoint(EditorContext& ctx, const Uuid& entity, int index) {
    Entity e = ctx.world().find(entity);
    const reflect::TypeInfo* t = fieldType("Spline", "points");
    if (!e || !e.has<SplineComponent>() || !t) return;
    auto points = e.get<SplineComponent>().points;
    if (index < 0 || index >= int(points.size())) return;
    points.erase(points.begin() + index);
    ctx.setProperty({entity}, "Spline", "points", serial::toValue(&points, *t));
}

void drawSplineHandles(DebugDraw& dd, World& world, const Uuid& entity, int selected, const glm::vec3& cameraPos) {
    Entity e = world.find(entity);
    if (!e || !e.has<SplineComponent>()) return;
    const auto& c = e.get<SplineComponent>();
    const glm::mat4 m = e.worldMatrix();
    const bool bezier = c.type == spline::SplineType::Bezier;
    for (usize i = 0; i < c.points.size(); ++i) {
        const glm::vec3 p = glm::vec3(m * glm::vec4(c.points[i].position, 1.0f));
        const float s = std::max(0.03f, glm::length(p - cameraPos) * 0.012f);
        const bool sel = int(i) == selected;
        dd.box(p, glm::vec3(sel ? s * 1.4f : s), glm::quat(1, 0, 0, 0), sel ? DebugColor(debug_color::kOrange) : DebugColor(debug_color::kWhite), 0.0f, false);
        if (bezier && sel) {
            const glm::vec3 in = glm::vec3(m * glm::vec4(c.points[i].position + c.points[i].inHandle, 1.0f));
            const glm::vec3 out = glm::vec3(m * glm::vec4(c.points[i].position + c.points[i].outHandle, 1.0f));
            dd.line(p, in, debug_color::kCyan, 0.0f, false);
            dd.line(p, out, debug_color::kCyan, 0.0f, false);
        }
    }
    // The curve itself (also when the spline runtime's debug draw is off).
    spline::Spline sp;
    SplineRuntime::build(c, sp);
    const float len = sp.length();
    if (len > 0.0f) {
        glm::vec3 prev = glm::vec3(m * glm::vec4(sp.evaluateAtDistance(0.0f).position, 1.0f));
        const int steps = std::clamp(int(len * 4.0f), 16, 512);
        for (int k = 1; k <= steps; ++k) {
            const glm::vec3 cur = glm::vec3(m * glm::vec4(sp.evaluateAtDistance(len * float(k) / float(steps)).position, 1.0f));
            dd.line(prev, cur, debug_color::kYellow, 0.0f, false);
            prev = cur;
        }
    }
}

bool hasTerrain(World& world, const Uuid& entity) {
#if defined(OX_GAMEPLAY_HAS_WORLD)
    Entity e = world.find(entity);
    return e && e.has<TerrainComponent>();
#else
    (void)world;
    (void)entity;
    return false;
#endif
}

std::optional<glm::vec3> raycastTerrain(Services& services, World& world, const Ray& ray, Uuid* terrain) {
#if defined(OX_GAMEPLAY_HAS_WORLD)
    auto* rt = services.tryGet<WorldRuntime>();
    if (!rt) return std::nullopt;
    (void)world;
    // March along the ray, refine the first sign change by bisection.
    const float maxDist = 4000.0f;
    float step = 0.5f;
    float prevT = 0.0f;
    std::optional<float> prevDelta;
    for (float t = 0.0f; t < maxDist; t += step) {
        const glm::vec3 p = ray.at(t);
        const auto h = rt->terrainHeight({p.x, p.z});
        if (h) {
            const float delta = p.y - *h;
            if (prevDelta && *prevDelta > 0.0f && delta <= 0.0f) {
                float a = prevT, b = t;
                for (int i = 0; i < 20; ++i) {
                    const float mid = 0.5f * (a + b);
                    const glm::vec3 q = ray.at(mid);
                    const auto hm = rt->terrainHeight({q.x, q.z});
                    if (hm && q.y - *hm > 0.0f) a = mid;
                    else b = mid;
                }
                const glm::vec3 hit = ray.at(b);
                if (terrain) {
                    Entity te = rt->terrainAt({hit.x, hit.z});
                    *terrain = te ? te.uuid() : Uuid{};
                }
                return hit;
            }
            prevDelta = delta;
        } else {
            prevDelta.reset();
        }
        prevT = t;
        step = std::min(4.0f, 0.5f + t * 0.01f);
    }
    return std::nullopt;
#else
    (void)services;
    (void)world;
    (void)ray;
    (void)terrain;
    return std::nullopt;
#endif
}

bool sculptTerrain(Services& services, World& world, const Uuid& terrain, glm::vec2 xz, const SculptSettings& s, float dt) {
#if defined(OX_GAMEPLAY_HAS_WORLD)
    auto* rt = services.tryGet<WorldRuntime>();
    Entity e = world.find(terrain);
    if (!rt || !e || !e.has<TerrainComponent>()) return false;
    world::BrushSettings b;
    switch (s.op) {
    case SculptOp::Raise: b.op = world::BrushOp::Raise; break;
    case SculptOp::Lower: b.op = world::BrushOp::Lower; break;
    case SculptOp::Smooth: b.op = world::BrushOp::Smooth; break;
    case SculptOp::Flatten: b.op = world::BrushOp::Flatten; break;
    }
    b.radius = s.radius;
    b.strength = s.op == SculptOp::Smooth || s.op == SculptOp::Flatten ? std::clamp(s.strength * 0.25f, 0.0f, 1.0f) : s.strength;
    if (s.op == SculptOp::Flatten) b.targetHeight = rt->terrainHeight(xz).value_or(0.0f);
    const world::IRect dirty = rt->applyBrush(e, xz, b, dt);
    return dirty.width() > 0 && dirty.height() > 0;
#else
    (void)services;
    (void)world;
    (void)terrain;
    (void)xz;
    (void)s;
    (void)dt;
    return false;
#endif
}

void drawTerrainWire(DebugDraw& dd, Services& services, World& world, const Uuid& terrain, std::optional<glm::vec3> brush,
                     float brushRadius) {
#if defined(OX_GAMEPLAY_HAS_WORLD)
    auto* rt = services.tryGet<WorldRuntime>();
    Entity e = world.find(terrain);
    if (!rt || !e) return;
    auto hf = rt->heightfield(e);
    if (!hf || !hf->valid()) return;
    const u32 res = hf->resolution();
    const DebugColor coarse = glm::vec4(0.55f, 0.75f, 0.45f, 0.55f);
    // Coarse wireframe of the whole terrain (~48 lines per side).
    const u32 stride = std::max(1u, res / 48);
    for (u32 z = 0; z < res; z += stride) {
        for (u32 x = 0; x + stride < res; x += stride) dd.line(hf->samplePosition(x, z), hf->samplePosition(x + stride, z), coarse);
    }
    for (u32 x = 0; x < res; x += stride) {
        for (u32 z = 0; z + stride < res; z += stride) dd.line(hf->samplePosition(x, z), hf->samplePosition(x, z + stride), coarse);
    }
    if (!brush) return;
    // Full-resolution patch under the brush + the brush ring following the surface.
    const glm::vec2 c = hf->worldToSample({brush->x, brush->z});
    const int r = int(std::ceil(brushRadius * 1.2f / hf->spacing()));
    const int x0 = std::max(0, int(c.x) - r), x1 = std::min(int(res) - 1, int(c.x) + r);
    const int z0 = std::max(0, int(c.y) - r), z1 = std::min(int(res) - 1, int(c.y) + r);
    const int fine = std::max(1, (x1 - x0) / 40);
    const DebugColor patch = glm::vec4(0.95f, 0.85f, 0.4f, 0.5f);
    for (int z = z0; z <= z1; z += fine) {
        for (int x = x0; x + fine <= x1; x += fine) dd.line(hf->samplePosition(u32(x), u32(z)), hf->samplePosition(u32(x + fine), u32(z)), patch);
    }
    for (int x = x0; x <= x1; x += fine) {
        for (int z = z0; z + fine <= z1; z += fine) dd.line(hf->samplePosition(u32(x), u32(z)), hf->samplePosition(u32(x), u32(z + fine)), patch);
    }
    const int segs = 48;
    glm::vec3 prev{};
    for (int i = 0; i <= segs; ++i) {
        const float a = float(i) / float(segs) * 6.2831853f;
        const glm::vec2 xz{brush->x + std::cos(a) * brushRadius, brush->z + std::sin(a) * brushRadius};
        const glm::vec3 p{xz.x, hf->sampleHeight(xz) + 0.05f, xz.y};
        if (i > 0) dd.line(prev, p, debug_color::kOrange, 0.0f, false);
        prev = p;
    }
    dd.point(*brush, brushRadius * 0.1f, debug_color::kOrange, 0.0f, false);
#else
    (void)dd;
    (void)services;
    (void)world;
    (void)terrain;
    (void)brush;
    (void)brushRadius;
#endif
}

#else // !OX_EDITOR_HAS_GAMEPLAY

void applyGameplayDebugFlags(Services&, const ShowFlags&) {}
BakeResult bakeNavMesh(EditorContext&, const Uuid&) { return {false, QObject::tr("Gameplay module not linked")}; }
BtSnapshot behaviorTreeSnapshot(Services&, World&, const Uuid&) { return {}; }
bool fitColliderToMesh(EditorContext&, const UuidList&, QString* message) {
    if (message) *message = QObject::tr("Gameplay module not linked");
    return false;
}
ScriptPropertiesResult scriptProperties(EditorContext&, const Uuid&) { return {}; }
void setScriptPropertyOverride(EditorContext&, const UuidList&, const std::string&, int, const std::optional<serial::Value>&, EditPhase) {}
bool hasSpline(World&, const Uuid&) { return false; }
std::vector<glm::vec3> splinePointsWorld(World&, const Uuid&) { return {}; }
void setSplinePointWorld(EditorContext&, const Uuid&, int, const glm::vec3&, EditPhase) {}
int insertSplinePoint(EditorContext&, const Uuid&, int, const glm::vec3&) { return -1; }
void removeSplinePoint(EditorContext&, const Uuid&, int) {}
void drawSplineHandles(DebugDraw&, World&, const Uuid&, int, const glm::vec3&) {}
bool hasTerrain(World&, const Uuid&) { return false; }
std::optional<glm::vec3> raycastTerrain(Services&, World&, const Ray&, Uuid*) { return std::nullopt; }
bool sculptTerrain(Services&, World&, const Uuid&, glm::vec2, const SculptSettings&, float) { return false; }
void drawTerrainWire(DebugDraw&, Services&, World&, const Uuid&, std::optional<glm::vec3>, float) {}

#endif

} // namespace ox::editor
