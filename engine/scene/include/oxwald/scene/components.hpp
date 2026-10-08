#pragma once

#include <oxwald/core/math.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>
#include <oxwald/scene/entity_ref.hpp>

#include <entt/entity/entity.hpp>

#include <string>
#include <utility>
#include <vector>

// Core scene components. All are plain data, reflected in registerSceneTypes() (scene.hpp) and registered with
// the ComponentRegistry under their reflected names ("Name", "Transform", "Light", ...).
namespace ox {

struct NameComponent {
    std::string name;
};

struct IdComponent {
    Uuid id;
};

// Local transform relative to the parent. Mutating it through Entity::transform()/setters (or
// registry.patch) marks the world transform dirty; writing via registry.get<> directly does not.
struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};

    [[nodiscard]] Transform toTransform() const { return {position, rotation, scale}; }
    void setTransform(const Transform& t) {
        position = t.position;
        rotation = t.rotation;
        scale = t.scale;
    }
    [[nodiscard]] glm::mat4 matrix() const { return toTransform().toMatrix(); }
};

// Hierarchy links. A children vector (rather than first-child/next-sibling lists) keeps sibling order explicit,
// makes reordering in the editor trivial and iteration cache friendly. Runtime handles only: the serialized form
// is the parent's UUID plus the order of entities in the file.
struct HierarchyComponent {
    entt::entity parent{entt::null};
    std::vector<entt::entity> children;
};

// Cached world matrix, updated by the transform system (PostUpdate). `previous` is last frame's matrix for
// motion vectors / interpolation.
struct WorldTransformComponent {
    glm::mat4 matrix{1.0f};
    glm::mat4 previous{1.0f};
};

// Marker: the world transform of this entity (and its subtree) needs recomputation.
struct TransformDirtyTag {};
// Marker: the entity is scheduled for destruction at the end of the frame (World::flushDestroyed).
struct PendingDestroyTag {};

struct CameraComponent {
    enum class Projection : u8 { Perspective, Orthographic };

    Projection projection = Projection::Perspective;
    f32 verticalFov = 60.0f;  // degrees
    f32 orthographicSize = 5.0f; // half height in metres
    f32 nearPlane = 0.1f;
    f32 farPlane = 1000.0f; // <= 0: infinite (perspective only)
    // Physical camera (exposure): EV100 = log2(N^2 / t * 100 / S).
    f32 aperture = 16.0f;          // f-stops
    f32 shutterSpeed = 1.0f / 125; // seconds
    f32 iso = 100.0f;
    f32 exposureCompensation = 0.0f; // EV
    // Depth of field (physical lens, used by the renderer's DoF unless a post-process volume overrides it).
    f32 focusDistance = 0.0f; // metres; 0 = no depth of field from the camera
    f32 focalLength = 0.0f;   // millimetres; 0 = derived from verticalFov on a 24 mm (full frame) sensor
    bool primary = false;

    // Reversed-Z projection (depth 1 at near, 0 at far).
    [[nodiscard]] glm::mat4 projectionMatrix(f32 aspect) const;
    [[nodiscard]] f32 ev100() const;
    // Multiplier converting luminance (cd/m^2) to normalised scene values (Frostbite/Lagarde formula).
    [[nodiscard]] f32 exposure() const;
};

enum class LightType : u8 { Directional, Point, Spot, AreaRect };

struct LightComponent {
    LightType type = LightType::Point;
    glm::vec3 color{1.0f};
    // Physical units: lux for directional lights, lumens for point/spot/area lights.
    f32 intensity = 800.0f;
    f32 range = 10.0f;             // attenuation cut-off in metres (point/spot)
    f32 innerConeAngle = 20.0f;    // degrees (spot)
    f32 outerConeAngle = 30.0f;    // degrees (spot)
    glm::vec2 areaSize{1.0f, 1.0f}; // metres (area rect; placeholder until area lights land)
    bool castShadows = true;
    u32 shadowResolution = 0; // hint in texels, 0 = renderer decides from scalability
    f32 shadowBias = 0.0005f;
    f32 shadowNormalBias = 0.02f;
    f32 sourceRadius = 0.0f; // metres (point/spot) or angular radius in degrees (directional); soft shadows
    bool volumetric = true;
    f32 volumetricIntensity = 1.0f;
};

struct MeshRendererComponent {
    Uuid mesh;                   // asset reference (Mesh)
    std::vector<Uuid> materials; // one per submesh (Material)
    bool castShadows = true;
    bool receiveShadows = true;
    bool visible = true;
    u32 layerMask = 1u;
};

struct EnvironmentComponent {
    Uuid skybox; // HDRI / cubemap asset (Texture)
    f32 skyIntensity = 1.0f;
    // Luminance (cd/m²) of a white texel of an 8-bit (LDR) skybox — LDR images store display values, not radiance.
    // 0 = the r.Sky.LdrLuminance default (5000, a bright daylight sky). HDR (float / BC6H) skyboxes ignore it.
    f32 ldrSkyLuminance = 0.0f;
    EntityRef sun; // directional light driving the sky
    f32 ambientIntensity = 1.0f;
    bool fogEnabled = false;
    glm::vec3 fogColor{0.6f, 0.7f, 0.8f};
    f32 fogDensity = 0.01f;
    f32 fogHeightFalloff = 0.2f;
    f32 fogStartDistance = 0.0f;
};

struct TagComponent {
    std::vector<std::string> tags;

    [[nodiscard]] bool has(std::string_view tag) const {
        for (const auto& t : tags) {
            if (t == tag) return true;
        }
        return false;
    }
};

// Present on every entity created through World::create. Inactive entities (or with an inactive ancestor) are
// skipped by gameplay/render systems that honour Entity::activeInHierarchy().
struct ActiveComponent {
    bool active = true;
};

// Links an entity to the prefab it was instantiated from. `overrides` holds "Component.field.path" strings
// of properties changed on this instance; they survive prefab updates (see prefab.hpp).
struct PrefabInstanceComponent {
    Uuid prefab;   // prefab asset id
    Uuid sourceId; // id of the corresponding entity inside the prefab
    bool isRoot = false;
    std::vector<std::string> overrides;
};

// Marks a whole entity for save games (all serializable components are persisted, not only attr::SaveGame
// fields).
struct SaveGameComponent {
    bool saveTransform = true;
};

// Components whose type is unknown in this build (module not linked), kept verbatim so saving does not lose
// them. Entity references inside are remapped like any other.
struct UnknownComponents {
    std::vector<std::pair<std::string, serial::Value>> entries;
};

} // namespace ox
