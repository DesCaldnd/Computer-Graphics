#include "core/scene_templates.hpp"

#include <oxwald/core/math.hpp>
#include <oxwald/scene/components.hpp>

#include <QObject>

namespace ox::editor {

namespace builtin {
Uuid cubeMesh() { return Uuid::fromName("engine://meshes/cube"); }
Uuid sphereMesh() { return Uuid::fromName("engine://meshes/sphere"); }
Uuid planeMesh() { return Uuid::fromName("engine://meshes/plane"); }
Uuid cylinderMesh() { return Uuid::fromName("engine://meshes/cylinder"); }
Uuid defaultMaterial() { return Uuid::fromName("engine://materials/default"); }
QString primitiveName(const Uuid& mesh) {
    if (mesh == cubeMesh()) return QStringLiteral("Cube");
    if (mesh == sphereMesh()) return QStringLiteral("Sphere");
    if (mesh == planeMesh()) return QStringLiteral("Plane");
    if (mesh == cylinderMesh()) return QStringLiteral("Cylinder");
    return {};
}
} // namespace builtin

namespace {

glm::quat euler(float pitchDeg, float yawDeg, float rollDeg = 0.0f) {
    return glm::quat(glm::vec3(glm::radians(pitchDeg), glm::radians(yawDeg), glm::radians(rollDeg)));
}

Entity mesh(World& w, const char* name, Uuid meshId, glm::vec3 pos, glm::vec3 scale = glm::vec3(1.0f), Entity parent = {},
            glm::quat rot = glm::quat(1, 0, 0, 0)) {
    Entity e = w.create(name, parent);
    e.setPosition(pos);
    e.setScale(scale);
    e.setRotation(rot);
    auto& mr = e.add<MeshRendererComponent>();
    mr.mesh = meshId;
    mr.materials = {builtin::defaultMaterial()};
    return e;
}

} // namespace

QString createKindName(CreateKind kind) {
    switch (kind) {
    case CreateKind::Empty: return QObject::tr("Empty Entity");
    case CreateKind::Cube: return QObject::tr("Cube");
    case CreateKind::Sphere: return QObject::tr("Sphere");
    case CreateKind::Plane: return QObject::tr("Plane");
    case CreateKind::Cylinder: return QObject::tr("Cylinder");
    case CreateKind::PointLight: return QObject::tr("Point Light");
    case CreateKind::SpotLight: return QObject::tr("Spot Light");
    case CreateKind::DirectionalLight: return QObject::tr("Directional Light");
    case CreateKind::AreaLight: return QObject::tr("Area Light");
    case CreateKind::Camera: return QObject::tr("Camera");
    case CreateKind::Environment: return QObject::tr("Environment");
    }
    return {};
}

QString createKindIcon(CreateKind kind) {
    switch (kind) {
    case CreateKind::Empty: return QStringLiteral("empty");
    case CreateKind::Cube: return QStringLiteral("cube");
    case CreateKind::Sphere: return QStringLiteral("sphere");
    case CreateKind::Plane: return QStringLiteral("plane");
    case CreateKind::Cylinder: return QStringLiteral("mesh");
    case CreateKind::PointLight: return QStringLiteral("light-point");
    case CreateKind::SpotLight: return QStringLiteral("light-spot");
    case CreateKind::DirectionalLight: return QStringLiteral("sun");
    case CreateKind::AreaLight: return QStringLiteral("light-area");
    case CreateKind::Camera: return QStringLiteral("camera");
    case CreateKind::Environment: return QStringLiteral("environment");
    }
    return QStringLiteral("entity");
}

Entity createPreset(World& world, CreateKind kind, Entity parent) {
    const std::string name = createKindName(kind).toStdString();
    switch (kind) {
    case CreateKind::Empty: return world.create(name, parent);
    case CreateKind::Cube: return mesh(world, name.c_str(), builtin::cubeMesh(), {0, 0.5f, 0}, glm::vec3(1), parent);
    case CreateKind::Sphere: return mesh(world, name.c_str(), builtin::sphereMesh(), {0, 0.5f, 0}, glm::vec3(1), parent);
    case CreateKind::Plane: return mesh(world, name.c_str(), builtin::planeMesh(), {0, 0, 0}, glm::vec3(10, 1, 10), parent);
    case CreateKind::Cylinder: return mesh(world, name.c_str(), builtin::cylinderMesh(), {0, 1, 0}, glm::vec3(1, 2, 1), parent);
    case CreateKind::PointLight:
    case CreateKind::SpotLight:
    case CreateKind::DirectionalLight:
    case CreateKind::AreaLight: {
        Entity e = world.create(name, parent);
        auto& l = e.add<LightComponent>();
        if (kind == CreateKind::PointLight) {
            l.type = LightType::Point;
            e.setPosition({0, 2.5f, 0});
        } else if (kind == CreateKind::SpotLight) {
            l.type = LightType::Spot;
            e.setPosition({0, 4, 0});
            e.setRotation(euler(-90, 0));
        } else if (kind == CreateKind::DirectionalLight) {
            l.type = LightType::Directional;
            l.intensity = 100000.0f;
            l.color = {1.0f, 0.96f, 0.9f};
            e.setPosition({0, 6, 0});
            e.setRotation(euler(-50, 35));
        } else {
            l.type = LightType::AreaRect;
            e.setPosition({0, 3, 0});
            e.setRotation(euler(-90, 0));
        }
        return e;
    }
    case CreateKind::Camera: {
        Entity e = world.create(name, parent);
        e.add<CameraComponent>();
        e.setPosition({0, 1.7f, 6});
        return e;
    }
    case CreateKind::Environment: {
        Entity e = world.create(name, parent);
        e.add<EnvironmentComponent>();
        return e;
    }
    }
    return {};
}

void populateDefaultScene(World& w) {
    Entity sun = createPreset(w, CreateKind::DirectionalLight);
    sun.setName("Sun");
    Entity env = createPreset(w, CreateKind::Environment);
    env.get<EnvironmentComponent>().sun = sun.ref();
    Entity cam = createPreset(w, CreateKind::Camera);
    cam.setName("Main Camera");
    cam.get<CameraComponent>().primary = true;
    cam.setPosition({0, 2.5f, 8});
    cam.setRotation(euler(-12, 0));
    Entity floor = createPreset(w, CreateKind::Plane);
    floor.setName("Floor");
    Entity cube = createPreset(w, CreateKind::Cube);
    cube.setName("Cube");
}

void populateShowcaseScene(World& w) {
    Entity sun = createPreset(w, CreateKind::DirectionalLight);
    sun.setName("Sun");
    Entity env = createPreset(w, CreateKind::Environment);
    auto& envc = env.get<EnvironmentComponent>();
    envc.sun = sun.ref();
    envc.fogEnabled = true;
    Entity cam = createPreset(w, CreateKind::Camera);
    cam.setName("Main Camera");
    cam.get<CameraComponent>().primary = true;
    cam.setPosition({-7.5f, 3.0f, 9.0f});
    cam.setRotation(euler(-14, -38));

    Entity level = w.create("Level");
    mesh(w, "Ground", builtin::planeMesh(), {0, 0, 0}, {24, 1, 24}, level);
    Entity courtyard = w.create("Courtyard", level);
    mesh(w, "Pedestal", builtin::cylinderMesh(), {0, 0.35f, 0}, {2.4f, 0.7f, 2.4f}, courtyard);
    mesh(w, "Orb", builtin::sphereMesh(), {0, 1.45f, 0}, glm::vec3(1.3f), courtyard);
    for (int i = 0; i < 6; ++i) {
        const float a = float(i) / 6.0f * kTwoPi;
        const glm::vec3 p{std::cos(a) * 5.2f, 1.5f, std::sin(a) * 5.2f};
        mesh(w, ("Pillar " + std::to_string(i + 1)).c_str(), builtin::cubeMesh(), p, {0.6f, 3.0f, 0.6f}, courtyard,
             glm::angleAxis(-a, glm::vec3(0, 1, 0)));
    }
    Entity crates = w.create("Crates", level);
    crates.setPosition({4.5f, 0, -3.5f});
    mesh(w, "Crate A", builtin::cubeMesh(), {0, 0.5f, 0}, glm::vec3(1), crates, euler(0, 12));
    mesh(w, "Crate B", builtin::cubeMesh(), {1.15f, 0.4f, 0.3f}, glm::vec3(0.8f), crates, euler(0, -20));
    mesh(w, "Crate C", builtin::cubeMesh(), {0.45f, 1.3f, 0.1f}, glm::vec3(0.7f), crates, euler(0, 35));
    Entity lights = w.create("Lights");
    Entity l1 = createPreset(w, CreateKind::PointLight, lights);
    l1.setName("Warm Fill");
    l1.setPosition({-3, 2.4f, 2});
    l1.get<LightComponent>().color = {1.0f, 0.7f, 0.4f};
    Entity l2 = createPreset(w, CreateKind::SpotLight, lights);
    l2.setName("Orb Spot");
    l2.setPosition({0, 6, 0});
    Entity tagged = mesh(w, "Beacon", builtin::sphereMesh(), {-4.5f, 0.4f, -4}, glm::vec3(0.8f), level);
    tagged.add<TagComponent>().tags = {"interactive", "quest"};
}

} // namespace ox::editor
