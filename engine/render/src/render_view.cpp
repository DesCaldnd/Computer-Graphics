#include "renderer_impl.hpp"

#include <oxwald/scene/components.hpp>

namespace ox::render {

f32 halton(u32 index, u32 base) {
    f32 f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= f32(base);
        r += f * f32(index % base);
        index /= base;
    }
    return r;
}

glm::vec2 haltonJitter(u64 frame, u32 phases) {
    if (phases == 0) return glm::vec2(0.0f);
    const u32 i = u32(frame % phases) + 1;
    return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
}

CameraParams CameraParams::fromComponent(const CameraComponent& camera, const glm::mat4& world) {
    CameraParams p;
    p.world = world;
    p.projection = camera.projection == CameraComponent::Projection::Orthographic ? Projection::Orthographic
                                                                                   : Projection::Perspective;
    p.verticalFov = glm::radians(camera.verticalFov);
    p.orthographicHeight = camera.orthographicSize * 2.0f;
    p.nearPlane = camera.nearPlane;
    p.farPlane = camera.farPlane;
    p.ev100 = camera.ev100();
    return p;
}

CameraParams CameraParams::lookAt(glm::vec3 eye, glm::vec3 target, f32 fovDegrees, f32 nearPlane, f32 farPlane,
                                  glm::vec3 up) {
    CameraParams p;
    p.world = glm::inverse(glm::lookAtRH(eye, target, up));
    p.verticalFov = glm::radians(fovDegrees);
    p.nearPlane = nearPlane;
    p.farPlane = farPlane;
    return p;
}

glm::mat4 CameraParams::projectionMatrix(f32 aspect) const {
    glm::mat4 proj;
    if (projection == Projection::Orthographic) {
        const f32 h = orthographicHeight * 0.5f;
        const f32 w = h * aspect;
        proj = orthoReversedZ(-w, w, -h, h, nearPlane, farPlane > nearPlane ? farPlane : nearPlane + 1000.0f);
    } else if (farPlane <= 0.0f) {
        proj = perspectiveInfiniteReversedZ(verticalFov, aspect, nearPlane);
    } else {
        proj = perspectiveReversedZ(verticalFov, aspect, nearPlane, farPlane);
    }
    // Vulkan Y flip: NDC y points down, so uv = ndc * 0.5 + 0.5 has its origin at the top-left.
    proj[0][1] = -proj[0][1];
    proj[1][1] = -proj[1][1];
    proj[2][1] = -proj[2][1];
    proj[3][1] = -proj[3][1];
    return proj;
}

RenderView::RenderView(ViewId id, ViewDesc desc) : m_id(id), m_desc(std::move(desc)), m_impl(std::make_unique<Impl>()) {}

RenderView::~RenderView() = default;

glm::vec2 RenderView::jitterNdc() const {
    if (m_renderExtent.width == 0) return glm::vec2(0.0f);
    return {m_jitter.x * 2.0f / f32(m_renderExtent.width), m_jitter.y * 2.0f / f32(m_renderExtent.height)};
}

} // namespace ox::render
