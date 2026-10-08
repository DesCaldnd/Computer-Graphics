#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/hash.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/gameplay/spline.hpp>
#include <oxwald/spline/spline_debug.hpp>

namespace ox::gameplay {

SplineRuntime::SplineRuntime() = default;
SplineRuntime::~SplineRuntime() { detach(); }

void SplineRuntime::build(const SplineComponent& c, spline::Spline& out) {
    out = spline::Spline(c.type, c.closed);
    spline::SplineSettings s;
    s.catmullRomAlpha = c.catmullRomAlpha;
    s.degree = c.degree;
    s.frameMode = c.frameMode;
    s.upVector = c.upVector;
    out.setSettings(s);
    std::vector<spline::ControlPoint> points;
    points.reserve(c.points.size());
    for (const SplinePoint& p : c.points) {
        spline::ControlPoint cp;
        cp.position = p.position;
        cp.inHandle = p.inHandle;
        cp.outHandle = p.outHandle;
        cp.handleMode = p.handleMode;
        cp.roll = p.roll;
        cp.weight = p.weight > 0.f ? p.weight : 1.f;
        cp.up = p.up;
        points.push_back(cp);
    }
    out.setPoints(std::move(points));
    for (const auto& m : c.markers) out.addMarker(m.name, m.t);
    out.rebuild();
}

void SplineRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_bus = services.tryGet<EventBus>();
    entt::registry& r = world.registry();
    m_connections.emplace_back(r.on_construct<SplineComponent>().connect<&SplineRuntime::onSplineChanged>(*this));
    m_connections.emplace_back(r.on_update<SplineComponent>().connect<&SplineRuntime::onSplineChanged>(*this));
    m_connections.emplace_back(r.on_destroy<SplineComponent>().connect<&SplineRuntime::onSplineDestroyed>(*this));
    m_connections.emplace_back(
        r.on_destroy<SplineFollowerComponent>().connect<&SplineRuntime::onFollowerDestroyed>(*this));
}

void SplineRuntime::detach() {
    m_connections.clear();
    m_splines.clear();
    m_followers.clear();
    m_world = nullptr;
    m_playing = false;
}

void SplineRuntime::onSplineChanged(entt::registry&, entt::entity e) { m_splines.erase(e); }
void SplineRuntime::onSplineDestroyed(entt::registry&, entt::entity e) { m_splines.erase(e); }
void SplineRuntime::onFollowerDestroyed(entt::registry&, entt::entity e) { m_followers.erase(e); }

void SplineRuntime::syncPlayState(bool playing) {
    if (playing == m_playing) return;
    m_playing = playing;
    m_followers.clear();
}

const spline::Spline* SplineRuntime::splineOf(Entity e) {
    if (!m_world || !e.valid()) return nullptr;
    const auto* c = e.tryGet<SplineComponent>();
    if (!c) return nullptr;
    auto it = m_splines.find(e.handle());
    if (it == m_splines.end()) {
        it = m_splines.emplace(e.handle(), spline::Spline{}).first;
        build(*c, it->second);
    }
    return &it->second;
}

std::optional<glm::vec3> SplineRuntime::positionAtDistance(Entity e, f32 distance) {
    const spline::Spline* s = splineOf(e);
    if (!s || s->pointCount() < 2) return std::nullopt;
    return e.worldTransform().transformPoint(s->evaluateAtDistance(distance).position);
}

std::optional<glm::quat> SplineRuntime::rotationAtDistance(Entity e, f32 distance) {
    const spline::Spline* s = splineOf(e);
    if (!s || s->pointCount() < 2) return std::nullopt;
    return glm::normalize(e.worldRotation() * s->evaluateAtDistance(distance).rotation());
}

std::optional<f32> SplineRuntime::closestDistance(Entity e, const glm::vec3& worldPoint) {
    const spline::Spline* s = splineOf(e);
    if (!s || s->pointCount() < 2) return std::nullopt;
    const glm::vec3 local = glm::vec3(glm::inverse(e.worldMatrix()) * glm::vec4(worldPoint, 1.f));
    return s->tToDistance(s->closestPoint(local).t);
}

f32 SplineRuntime::length(Entity e) {
    const spline::Spline* s = splineOf(e);
    return s && s->pointCount() >= 2 ? s->length() : 0.f;
}

void SplineRuntime::updateFollowers(f32 dt) {
    if (!m_world || !m_playing) return;
    OX_PROFILE_ZONE_N("SplineRuntime::updateFollowers");
    m_events.clear();
    entt::registry& r = m_world->registry();
    auto view = r.view<SplineFollowerComponent>();
    for (auto handle : view) {
        const Entity e = m_world->wrap(handle);
        auto& c = view.get<SplineFollowerComponent>(handle);
        if (!c.playing || !e.activeInHierarchy()) continue;
        const Entity splineEntity = m_world->resolve(c.spline);
        const spline::Spline* s = splineOf(splineEntity);
        if (!s || s->pointCount() < 2) continue;

        auto [it, created] = m_followers.try_emplace(handle);
        FollowerRecord& rec = it->second;
        if (created) {
            rec.follower.setDistance(c.distance != 0.f ? c.distance : c.startDistance);
            rec.follower.setDirection(c.direction);
        }
        spline::FollowerSettings& fs = rec.follower.settings();
        fs.speed = c.speed;
        fs.loopMode = c.loopMode;
        fs.orientToPath = c.orientToPath;
        fs.faceTravelDirection = c.faceTravelDirection;
        fs.forwardAxis = c.forwardAxis;
        fs.upAxis = c.upAxis;
        fs.fireMarkers = c.fireMarkers;
        u64 h = 0;
        for (const auto& ev : c.events) h = hashCombine(hashCombine(h, fnv1a64(ev.name)), std::hash<f32>{}(ev.distance));
        if (h != rec.eventsHash || created) {
            rec.follower.clearEvents();
            for (const auto& ev : c.events) rec.follower.addEvent(ev.name, ev.distance);
            rec.eventsHash = h;
        }
        rec.follower.setEventCallback([this, e](const spline::PathEvent& pe) {
            m_events.push_back({e, std::string(pe.name), pe.distance, pe.direction, pe.marker});
        });
        if (c.finished && c.loopMode == spline::LoopMode::Once) continue;
        rec.follower.advance(*s, dt);
        rec.follower.setEventCallback({});

        const spline::FollowerPose pose = rec.follower.pose(*s);
        const Transform splineWorld = splineEntity.worldTransform();
        Transform wt = e.worldTransform();
        wt.position = splineWorld.transformPoint(pose.position + pose.rotation * c.offset);
        if (c.orientToPath) wt.rotation = glm::normalize(splineWorld.rotation * pose.rotation);
        e.setWorldTransform(wt);
        c.distance = rec.follower.distance();
        c.direction = rec.follower.direction();
        c.finished = rec.follower.finished();
    }
    for (const auto& ev : m_events) {
        onEvent.emit(ev);
        if (m_bus) m_bus->publish(ev);
    }
}

void SplineRuntime::drawDebug(DebugDraw& draw, bool playing) {
    if (!debugDraw || !m_world) return;
    auto view = m_world->registry().view<SplineComponent>();
    for (auto handle : view) {
        const auto& c = view.get<SplineComponent>(handle);
        if (!(playing ? c.drawInGame : c.drawInEditor) || c.points.size() < 2) continue;
        const Entity e = m_world->wrap(handle);
        const spline::Spline* s = splineOf(e);
        if (!s) continue;
        const glm::mat4 m = e.worldMatrix();
        spline::DebugDrawOptions o;
        o.curveColor = c.color;
        o.frameSpacing = 0.f;
        spline::drawSpline(*s, [&](glm::vec3 a, glm::vec3 b, glm::vec4 col) {
            draw.line(glm::vec3(m * glm::vec4(a, 1.f)), glm::vec3(m * glm::vec4(b, 1.f)), col);
        }, o);
    }
}

} // namespace ox::gameplay
