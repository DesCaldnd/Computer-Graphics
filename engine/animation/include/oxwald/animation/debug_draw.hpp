#pragma once

#include <oxwald/animation/pose.hpp>

#include <functional>

namespace ox::anim {

// Debug visualisation sink: (from, to, rgba). The integration layer forwards these to ox::DebugDraw.
using DebugLineFn = std::function<void(glm::vec3, glm::vec3, glm::vec4)>;

// Bones as parent→child lines plus small axis tripods per joint (in world space via `ownerWorld`).
void debugDrawSkeleton(const Skeleton& skeleton, const std::vector<Transform>& model, const Transform& ownerWorld,
                       const DebugLineFn& line, glm::vec4 color = {1.0f, 0.8f, 0.2f, 1.0f}, f32 axisLength = 0.05f);

} // namespace ox::anim
