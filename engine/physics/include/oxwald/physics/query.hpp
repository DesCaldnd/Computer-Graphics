#pragma once

#include <oxwald/physics/types.hpp>

#include <functional>
#include <span>

namespace ox::physics {

struct QueryFilter {
    LayerMask layerMask = kAllLayers;
    std::span<const BodyHandle> ignoreBodies; // must stay alive for the duration of the query
    bool includeSensors = false;
    // Optional extra predicate (called under the body lock — don't call back into the world).
    std::function<bool(BodyHandle, u64 userData)> predicate;
};

struct RayHit {
    BodyHandle body;
    u64 userData = 0;
    glm::vec3 point{0.f};
    glm::vec3 normal{0.f};
    f32 distance = 0.f;
    f32 fraction = 0.f; // of maxDistance
    u32 subShapeId = 0;
};

struct ShapeCastHit {
    BodyHandle body;
    u64 userData = 0;
    glm::vec3 point{0.f};  // contact point on the hit body
    glm::vec3 normal{0.f}; // hit surface normal, pointing back towards the cast shape
    f32 distance = 0.f;    // distance travelled before impact
    f32 fraction = 0.f;
    bool startedPenetrating = false;
};

struct ClosestPointResult {
    BodyHandle body;
    u64 userData = 0;
    glm::vec3 point{0.f}; // closest point on the body surface
    f32 distance = 0.f;   // 0 when the query point is inside the body
};

} // namespace ox::physics
