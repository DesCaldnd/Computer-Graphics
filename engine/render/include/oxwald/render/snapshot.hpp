#pragma once

// RenderSnapshot: everything the renderer needs from the world for one frame, copied on the game thread
// (extract) and consumed on the render thread. The renderer never touches the World.
//
//   ox::render::SnapshotBuffer snapshots;                     // double buffer owned by the runtime
//   RenderSnapshot& s = snapshots.writeSlot();
//   ox::render::extract(world, s, {.debugDraw = &debugDraw}); // game thread, Extract phase
//   snapshots.publish();
//   ...
//   renderer.render(snapshots.readSlot(), views);             // render thread

#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/math.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/scene/components.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace ox {
class World;
class Services;
class JobSystem;
}

namespace ox::render {

struct SnapshotCamera {
    u32 entityId = 0; // encodeEntityId(entt)
    CameraComponent camera;
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
};

enum SnapshotMeshFlags : u32 {
    kMeshCastShadows = 1u << 0,
    kMeshReceiveShadows = 1u << 1,
    kMeshStatic = 1u << 2, // hint for shadow caching (never moved since it was extracted first)
};

struct SnapshotMesh {
    u32 entityId = 0;
    Uuid mesh;
    u32 materialOffset = 0; // into RenderSnapshot::materials
    u32 materialCount = 0;
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
    u32 flags = kMeshCastShadows | kMeshReceiveShadows;
    u32 layerMask = 1;
    u32 paletteOffset = ~0u; // into RenderSnapshot::palettes (skinned meshes)
    u32 paletteCount = 0;
    u32 prevPaletteOffset = ~0u;
};

struct SnapshotLight {
    u32 entityId = 0;
    LightComponent light;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f}; // world -Z of the entity = direction the light travels
    bool moved = false; // transform changed since last frame
};

struct SnapshotEnvironment {
    EnvironmentComponent environment;
    i32 sunLight = -1; // index into RenderSnapshot::lights (Environment::sun or brightest directional)
    // Optional analytic sky supplied by the world module: world::PreethamSky::Gpu (8 × vec4).
    std::optional<std::array<glm::vec4, 8>> preetham;
    // When set, the IBL is regenerated only when this key changes (plus IBL resolution / shader reloads) instead of
    // hashing the sky constants and the quantised sun. The world sky sets it to a 1°-quantised sun/moon key so a
    // running time of day refreshes the IBL at a throttled rate.
    std::optional<u64> iblKey;
};

// Feature data attached to a snapshot by extract hooks (particle emitters, water surfaces, fog volumes, probes...).
// One object per type, created on demand with RenderSnapshot::extension<T>(); clear() runs at the start of every
// extract so the type can keep its capacity. Features read it with findExtension<T>().
struct ISnapshotExtension {
    virtual ~ISnapshotExtension() = default;
    virtual void clear() = 0;
};

struct RenderSnapshot {
    u64 frame = 0;
    f64 time = 0.0;
    f32 deltaTime = 0.0f;

    std::vector<SnapshotCamera> cameras;
    std::vector<SnapshotMesh> meshes;
    std::vector<Uuid> materials; // per-submesh materials referenced by SnapshotMesh ranges
    std::vector<SnapshotLight> lights;
    std::vector<glm::mat4> palettes; // skinning palettes (model × inverse bind), concatenated
    std::optional<SnapshotEnvironment> environment;
    std::vector<DebugVertex> debugLines;        // line list, depth tested
    std::vector<DebugVertex> debugLinesOverlay; // line list, always on top
    std::vector<u32> selection; // editor: selected entity ids (encodeEntityId), filled by the editor

    // Type-erased feature data (see ISnapshotExtension). Shared pointers keep the snapshot copyable (shallow).
    std::unordered_map<std::type_index, std::shared_ptr<ISnapshotExtension>> extensions;

    template <class T>
    T& extension() {
        std::shared_ptr<ISnapshotExtension>& slot = extensions[std::type_index(typeid(T))];
        if (!slot) slot = std::make_shared<T>();
        return static_cast<T&>(*slot);
    }
    template <class T>
    [[nodiscard]] const T* findExtension() const {
        auto it = extensions.find(std::type_index(typeid(T)));
        return it != extensions.end() ? static_cast<const T*>(it->second.get()) : nullptr;
    }

    void clear();
    [[nodiscard]] i32 primaryCamera() const; // first camera with `primary`, else 0, -1 when empty
};

struct ExtractOptions {
    DebugDraw* debugDraw = nullptr; // copies depthTestedLines()/overlayLines() (call flush() before extract)
    bool includeInactive = false;
    f64 time = 0.0;
    f32 deltaTime = 0.0f;
    u64 frame = 0;
    Services* services = nullptr; // engine services for ExtractHookEx hooks (runtime adapter); may be null
    // Mesh renderers are extracted with parallelFor when set and there are more than parallelThreshold of them.
    JobSystem* jobs = nullptr;
    u32 parallelThreshold = 32768; // measured break-even on M4 Pro is ~30-50k meshes (docs/dev/perf.md)
};

// Game thread. Reads Camera, MeshRenderer, Light, Environment + WorldTransform (current and previous; call
// world.updateTransforms() first). Skips entities that are inactive in the hierarchy. Reuses `out`'s capacity.
void extract(const World& world, RenderSnapshot& out, const ExtractOptions& options = {});

// Hook for modules that add renderables (skinned meshes, terrain, vegetation...): called by extract() after the
// built-in components. Register once at startup (process-wide, like component registration).
using ExtractHook = void (*)(const World& world, RenderSnapshot& out);
void addExtractHook(ExtractHook hook);
// Variant receiving the extract options (services, time): integration glue that reads engine services
// (e.g. the gameplay WorldRenderData) registers this kind.
using ExtractHookEx = void (*)(const World& world, RenderSnapshot& out, const ExtractOptions& options);
void addExtractHookEx(ExtractHookEx hook);

// Two snapshots: the game thread writes one while the render thread reads the other.
class SnapshotBuffer {
public:
    RenderSnapshot& writeSlot() { return m_slots[m_write]; }
    // Makes the write slot readable; the render thread picks it up with readSlot().
    void publish();
    const RenderSnapshot& readSlot() const;

private:
    std::array<RenderSnapshot, 2> m_slots;
    u32 m_write = 0;
    std::atomic<u32> m_read{1};
    mutable std::mutex m_mutex;
};

} // namespace ox::render
