#include "viewport/viewport_renderer.hpp"

#include <QObject>

namespace ox::editor {

QString viewModeName(ViewMode mode) {
    switch (mode) {
    case ViewMode::Lit: return QObject::tr("Lit");
    case ViewMode::Unlit: return QObject::tr("Unlit");
    case ViewMode::Wireframe: return QObject::tr("Wireframe");
    case ViewMode::LightingOnly: return QObject::tr("Lighting Only");
    case ViewMode::Normals: return QObject::tr("World Normals");
    case ViewMode::Overdraw: return QObject::tr("Overdraw");
    case ViewMode::BufferBaseColor: return QObject::tr("Base Color");
    case ViewMode::BufferRoughness: return QObject::tr("Roughness");
    case ViewMode::BufferMetallic: return QObject::tr("Metallic");
    case ViewMode::BufferDepth: return QObject::tr("Scene Depth");
    case ViewMode::BufferMotion: return QObject::tr("Motion Vectors");
    case ViewMode::Count: break;
    }
    return {};
}

bool isBufferView(ViewMode mode) { return mode >= ViewMode::BufferBaseColor && mode < ViewMode::Count; }

glm::quat ViewportCamera::rotation() const {
    return glm::angleAxis(yaw, glm::vec3(0, 1, 0)) * glm::angleAxis(pitch, glm::vec3(1, 0, 0));
}
glm::vec3 ViewportCamera::forward() const { return rotation() * glm::vec3(0, 0, -1); }
glm::vec3 ViewportCamera::right() const { return rotation() * glm::vec3(1, 0, 0); }
glm::vec3 ViewportCamera::up() const { return rotation() * glm::vec3(0, 1, 0); }

glm::mat4 ViewportCamera::view() const {
    const glm::mat4 world = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation());
    return glm::inverse(world);
}

glm::mat4 ViewportCamera::projection(f32 aspect) const {
    if (orthographic) {
        const f32 h = orthoHeight * 0.5f;
        return orthoReversedZ(-h * aspect, h * aspect, -h, h, -farPlane, farPlane);
    }
    return perspectiveReversedZ(toRadians(verticalFovDeg), aspect, nearPlane, farPlane);
}

} // namespace ox::editor
