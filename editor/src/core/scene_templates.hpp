#pragma once

#include <oxwald/core/uuid.hpp>
#include <oxwald/scene/world.hpp>

#include <QString>

#include <functional>

namespace ox::editor {

// Built-in primitive meshes (asset ids the renderer/assets module map to engine://meshes/*).
namespace builtin {
Uuid cubeMesh();
Uuid sphereMesh();
Uuid planeMesh();
Uuid cylinderMesh();
Uuid defaultMaterial();
// "Cube", "Sphere", ... or empty when the id is not a built-in primitive.
QString primitiveName(const Uuid& mesh);
} // namespace builtin

// New scene: sun, environment, camera, floor.
void populateDefaultScene(World& world);
// A richer demo scene (used by the "Showcase" project template and screenshots).
void populateShowcaseScene(World& world);

// Engine modules decorate the showcase template (gameplay: physics bodies, a script, a spline follower, AI) and
// may write companion assets into the project's asset directory. Set by the integrations at startup.
using TemplateDecorator = std::function<void(World& world, const QString& assetDir)>;
void setShowcaseDecorator(TemplateDecorator decorator);
void decorateShowcase(World& world, const QString& assetDir);

enum class CreateKind { Empty, Cube, Sphere, Plane, Cylinder, PointLight, SpotLight, DirectionalLight, AreaLight, Camera, Environment };
// Creates a ready-to-use entity (shared by Entity menu, outliner context menu and tests).
Entity createPreset(World& world, CreateKind kind, Entity parent = {});
QString createKindName(CreateKind kind);
QString createKindIcon(CreateKind kind);

} // namespace ox::editor
