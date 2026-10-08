#pragma once

#include <oxwald/core/events.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/gameplay/events.hpp>
#include <oxwald/scene/world.hpp>
#include <oxwald/spline/path_follower.hpp>
#include <oxwald/spline/spline.hpp>

#include <entt/signal/sigh.hpp>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox {
class DebugDraw;
}

namespace ox::gameplay {

// ---- components ------------------------------------------------------------------------------------------

struct SplinePoint {
    glm::vec3 position{0.f};
    glm::vec3 inHandle{0.f}; // Bezier, relative to position
    glm::vec3 outHandle{0.f};
    spline::HandleMode handleMode = spline::HandleMode::Auto;
    f32 roll = 0.f; // radians
    f32 weight = 1.f;
    std::optional<glm::vec3> up;
};

struct SplineMarkerDesc {
    std::string name;
    f32 t = 0.f;
};

// Curve in the entity's local space (points are transformed by the entity's world transform).
struct SplineComponent {
    spline::SplineType type = spline::SplineType::CatmullRom;
    bool closed = false;
    std::vector<SplinePoint> points;
    f32 catmullRomAlpha = spline::kCatmullRomCentripetal;
    u32 degree = 3;
    spline::FrameMode frameMode = spline::FrameMode::RotationMinimizing;
    glm::vec3 upVector{0.f, 1.f, 0.f};
    std::vector<SplineMarkerDesc> markers;
    bool drawInEditor = true;
    bool drawInGame = false;
    glm::vec4 color{1.f, 1.f, 1.f, 1.f};
};

struct SplineFollowerEventDesc {
    std::string name;
    f32 distance = 0.f;
};

// Moves the entity along a SplineComponent entity at constant world speed (play mode).
struct SplineFollowerComponent {
    EntityRef spline;
    f32 speed = 1.f; // units per second, negative runs backwards
    spline::LoopMode loopMode = spline::LoopMode::Loop;
    bool orientToPath = true;
    bool faceTravelDirection = true;
    glm::vec3 forwardAxis{0.f, 0.f, -1.f};
    glm::vec3 upAxis{0.f, 1.f, 0.f};
    glm::vec3 offset{0.f}; // in the path frame (x right, y up, z back)
    bool playing = true;
    bool fireMarkers = true;
    std::vector<SplineFollowerEventDesc> events;
    f32 startDistance = 0.f;
    // runtime (also SaveGame)
    f32 distance = 0.f;
    i32 direction = 1;
    bool finished = false;
};

// ---- runtime -------------------------------------------------------------------------------------------

class SplineRuntime {
public:
    SplineRuntime();
    ~SplineRuntime();
    SplineRuntime(const SplineRuntime&) = delete;
    SplineRuntime& operator=(const SplineRuntime&) = delete;

    // Local-space spline of an entity with SplineComponent (rebuilt lazily after edits), null otherwise.
    [[nodiscard]] const spline::Spline* splineOf(Entity e);
    // World-space helpers.
    [[nodiscard]] std::optional<glm::vec3> positionAtDistance(Entity splineEntity, f32 distance);
    [[nodiscard]] std::optional<glm::quat> rotationAtDistance(Entity splineEntity, f32 distance);
    [[nodiscard]] std::optional<f32> closestDistance(Entity splineEntity, const glm::vec3& worldPoint);
    [[nodiscard]] f32 length(Entity splineEntity);

    Signal<const SplineFollowerEvent&> onEvent;
    bool debugDraw = true; // editor-visible splines (SplineComponent::drawInEditor / drawInGame)

    // ---- driven by the gameplay systems ----
    void attach(World& world, Services& services);
    void detach();
    void syncPlayState(bool playing);
    void updateFollowers(f32 dt);
    void drawDebug(DebugDraw& draw, bool playing);

    // Builds a spline from a component (exposed for tools/tests).
    static void build(const SplineComponent& c, spline::Spline& out);

private:
    struct FollowerRecord {
        spline::PathFollower follower;
        u64 eventsHash = 0;
    };
    void onSplineChanged(entt::registry& r, entt::entity e);
    void onSplineDestroyed(entt::registry& r, entt::entity e);
    void onFollowerDestroyed(entt::registry& r, entt::entity e);

    World* m_world = nullptr;
    EventBus* m_bus = nullptr;
    std::unordered_map<entt::entity, spline::Spline> m_splines;
    std::unordered_map<entt::entity, FollowerRecord> m_followers;
    std::vector<entt::scoped_connection> m_connections;
    std::vector<SplineFollowerEvent> m_events;
    bool m_playing = false;
};

} // namespace ox::gameplay
