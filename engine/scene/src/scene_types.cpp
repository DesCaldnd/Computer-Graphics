#include <oxwald/scene/scene.hpp>

#include <cmath>

namespace ox {

glm::mat4 CameraComponent::projectionMatrix(f32 aspect) const {
    if (projection == Projection::Orthographic) {
        const f32 h = orthographicSize;
        const f32 w = h * aspect;
        return orthoReversedZ(-w, w, -h, h, nearPlane, farPlane > nearPlane ? farPlane : nearPlane + 1000.0f);
    }
    const f32 fov = toRadians(verticalFov);
    if (farPlane <= 0.0f) return perspectiveInfiniteReversedZ(fov, aspect, nearPlane);
    return perspectiveReversedZ(fov, aspect, nearPlane, farPlane);
}

f32 CameraComponent::ev100() const {
    return std::log2((aperture * aperture) / shutterSpeed * 100.0f / iso) - exposureCompensation;
}

f32 CameraComponent::exposure() const {
    // Saturation-based speed: max luminance that does not clip = 1.2 * 2^EV100.
    return 1.0f / (1.2f * std::exp2(ev100()));
}

void registerSceneTypes() {
    reflect::registerCustomLeaf<EntityRef>(
        "EntityRef", serial::Tag::EntityRef, [](const EntityRef& r) { return serial::Value::makeEntityRef(r.id); },
        [](EntityRef& r, const serial::Value& v) {
            if (v.tag() != serial::Tag::EntityRef && v.tag() != serial::Tag::Uuid && v.tag() != serial::Tag::String) {
                return false;
            }
            r.id = v.getUuid();
            return true;
        });

    using attr::AssetRef;
    using attr::Category;
    using attr::Color;
    using attr::DisplayName;
    using attr::Hidden;
    using attr::Meta;
    using attr::Range;
    using attr::ReadOnly;
    using attr::Step;
    using attr::Tooltip;

    OX_REFLECT_TYPE(NameComponent, "Name").attributes(Category{"Core"}).field("name", &NameComponent::name);
    OX_REFLECT_TYPE(IdComponent, "Id").attributes(Category{"Core"}).field("id", &IdComponent::id, ReadOnly{});
    OX_REFLECT_TYPE(TransformComponent, "Transform")
        .attributes(Category{"Core"}, Meta{"icon", "transform"})
        .field("position", &TransformComponent::position, Step{0.1})
        .field("rotation", &TransformComponent::rotation)
        .field("scale", &TransformComponent::scale, Step{0.1});
    OX_REFLECT_TYPE(HierarchyComponent, "Hierarchy").attributes(Category{"Core"}, Hidden{});
    OX_REFLECT_TYPE(WorldTransformComponent, "WorldTransform")
        .attributes(Category{"Core"}, Hidden{})
        .field("matrix", &WorldTransformComponent::matrix, ReadOnly{})
        .field("previous", &WorldTransformComponent::previous, ReadOnly{});

    OX_REFLECT_ENUM(CameraComponent::Projection, "CameraProjection")
        .value("Perspective", CameraComponent::Projection::Perspective)
        .value("Orthographic", CameraComponent::Projection::Orthographic);
    OX_REFLECT_TYPE(CameraComponent, "Camera")
        .attributes(Category{"Rendering"}, Meta{"icon", "camera"})
        .field("projection", &CameraComponent::projection)
        .field("verticalFov", &CameraComponent::verticalFov, DisplayName{"Vertical FOV"}, Range{1.0, 179.0},
               Tooltip{"Vertical field of view in degrees"})
        .field("orthographicSize", &CameraComponent::orthographicSize, Range{0.01, 10000.0})
        .field("nearPlane", &CameraComponent::nearPlane, Range{0.001, 1000.0})
        .field("farPlane", &CameraComponent::farPlane, Tooltip{"0 or less = infinite far plane"})
        .field("aperture", &CameraComponent::aperture, Range{1.0, 32.0}, Tooltip{"f-stops"})
        .field("shutterSpeed", &CameraComponent::shutterSpeed, Range{0.00001, 30.0}, Tooltip{"Seconds"})
        .field("iso", &CameraComponent::iso, DisplayName{"ISO"}, Range{25.0, 102400.0})
        .field("exposureCompensation", &CameraComponent::exposureCompensation, Range{-10.0, 10.0})
        .field("focusDistance", &CameraComponent::focusDistance, Category{"Depth of Field"}, Range{0.0, 100000.0},
               Tooltip{"Distance in focus (metres); 0 disables depth of field from the camera"})
        .field("focalLength", &CameraComponent::focalLength, Category{"Depth of Field"}, Range{0.0, 2000.0},
               Tooltip{"Lens focal length (mm); 0 = derived from the vertical FOV on a full-frame sensor"})
        .field("primary", &CameraComponent::primary);

    OX_REFLECT_ENUM(LightType, "LightType")
        .value("Directional", LightType::Directional)
        .value("Point", LightType::Point)
        .value("Spot", LightType::Spot)
        .value("AreaRect", LightType::AreaRect);
    OX_REFLECT_TYPE(LightComponent, "Light")
        .attributes(Category{"Rendering"}, Meta{"icon", "light"})
        .field("type", &LightComponent::type)
        .field("color", &LightComponent::color, Color{})
        .field("intensity", &LightComponent::intensity, Range{0.0, 200000.0},
               Tooltip{"Lux for directional lights, lumens for point/spot/area lights"})
        .field("range", &LightComponent::range, Range{0.0, 10000.0})
        .field("innerConeAngle", &LightComponent::innerConeAngle, Range{0.0, 89.0})
        .field("outerConeAngle", &LightComponent::outerConeAngle, Range{0.0, 89.0})
        .field("areaSize", &LightComponent::areaSize)
        .field("castShadows", &LightComponent::castShadows, Category{"Shadows"})
        .field("shadowResolution", &LightComponent::shadowResolution, Category{"Shadows"},
               Tooltip{"Shadow map size hint in texels (0 = from scalability)"})
        .field("shadowBias", &LightComponent::shadowBias, Category{"Shadows"}, Step{0.0001})
        .field("shadowNormalBias", &LightComponent::shadowNormalBias, Category{"Shadows"}, Step{0.001})
        .field("sourceRadius", &LightComponent::sourceRadius, Category{"Shadows"},
               Tooltip{"Light source size for soft shadows (metres; degrees for directional)"})
        .field("volumetric", &LightComponent::volumetric, Category{"Volumetrics"})
        .field("volumetricIntensity", &LightComponent::volumetricIntensity, Category{"Volumetrics"}, Range{0.0, 10.0});

    OX_REFLECT_TYPE(MeshRendererComponent, "MeshRenderer")
        .attributes(Category{"Rendering"}, Meta{"icon", "mesh"})
        .field("mesh", &MeshRendererComponent::mesh, AssetRef{"Mesh"})
        .field("materials", &MeshRendererComponent::materials, AssetRef{"Material"})
        .field("castShadows", &MeshRendererComponent::castShadows)
        .field("receiveShadows", &MeshRendererComponent::receiveShadows)
        .field("visible", &MeshRendererComponent::visible)
        .field("layerMask", &MeshRendererComponent::layerMask);

    OX_REFLECT_TYPE(EnvironmentComponent, "Environment")
        .attributes(Category{"Rendering"}, Meta{"icon", "globe"})
        .field("skybox", &EnvironmentComponent::skybox, AssetRef{"Texture"})
        .field("skyIntensity", &EnvironmentComponent::skyIntensity, Range{0.0, 100.0})
        .field("ldrSkyLuminance", &EnvironmentComponent::ldrSkyLuminance, Range{0.0, 100000.0},
               Tooltip{"cd/m² of a white texel of an 8-bit skybox; 0 = r.Sky.LdrLuminance"})
        .field("sun", &EnvironmentComponent::sun)
        .field("ambientIntensity", &EnvironmentComponent::ambientIntensity, Range{0.0, 100.0})
        .field("fogEnabled", &EnvironmentComponent::fogEnabled, Category{"Fog"})
        .field("fogColor", &EnvironmentComponent::fogColor, Color{}, Category{"Fog"})
        .field("fogDensity", &EnvironmentComponent::fogDensity, Range{0.0, 1.0}, Category{"Fog"})
        .field("fogHeightFalloff", &EnvironmentComponent::fogHeightFalloff, Range{0.0, 10.0}, Category{"Fog"})
        .field("fogStartDistance", &EnvironmentComponent::fogStartDistance, Range{0.0, 100000.0}, Category{"Fog"});

    OX_REFLECT_TYPE(TagComponent, "Tags").attributes(Category{"Core"}).field("tags", &TagComponent::tags);
    OX_REFLECT_TYPE(ActiveComponent, "Active").attributes(Category{"Core"}).field("active", &ActiveComponent::active);
    OX_REFLECT_TYPE(PrefabInstanceComponent, "PrefabInstance")
        .attributes(Category{"Core"})
        .field("prefab", &PrefabInstanceComponent::prefab, AssetRef{"Prefab"}, ReadOnly{})
        .field("sourceId", &PrefabInstanceComponent::sourceId, ReadOnly{})
        .field("isRoot", &PrefabInstanceComponent::isRoot, ReadOnly{})
        .field("overrides", &PrefabInstanceComponent::overrides, ReadOnly{});
    OX_REFLECT_TYPE(SaveGameComponent, "SaveGame")
        .attributes(Category{"Core"})
        .field("saveTransform", &SaveGameComponent::saveTransform);
    OX_REFLECT_TYPE(UnknownComponents, "UnknownComponents").attributes(Category{"Core"}, Hidden{});

    auto& reg = ComponentRegistry::instance();
    // Identity/hierarchy are written as entity-level fields by the scene serializer, not as components.
    reg.add<IdComponent>({.removable = false, .hiddenInInspector = true, .serializable = false});
    reg.add<NameComponent>({.removable = false, .hiddenInInspector = true, .serializable = false});
    reg.add<TransformComponent>({.removable = false});
    reg.add<HierarchyComponent>({.removable = false, .hiddenInInspector = true, .serializable = false});
    reg.add<WorldTransformComponent>({.removable = false, .hiddenInInspector = true, .serializable = false});
    reg.add<ActiveComponent>({.removable = false, .hiddenInInspector = true});
    reg.add<CameraComponent>();
    reg.add<LightComponent>();
    reg.add<MeshRendererComponent>();
    reg.add<EnvironmentComponent>();
    reg.add<TagComponent>();
    reg.add<PrefabInstanceComponent>({.removable = false, .hiddenInInspector = true});
    reg.add<SaveGameComponent>();
    reg.add<UnknownComponents>({.removable = false, .hiddenInInspector = true, .serializable = false});
}

} // namespace ox
