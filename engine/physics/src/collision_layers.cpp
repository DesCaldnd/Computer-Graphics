#include <oxwald/core/assert.hpp>
#include <oxwald/physics/collision_layers.hpp>

#include <algorithm>

namespace ox::physics {

CollisionLayers CollisionLayers::makeDefault() {
    using namespace layers;
    CollisionLayers l;
    l.m_count = FirstUser;
    l.m_names[Static] = "Static";
    l.m_names[Dynamic] = "Dynamic";
    l.m_names[Kinematic] = "Kinematic";
    l.m_names[Character] = "Character";
    l.m_names[Trigger] = "Trigger";
    l.m_names[Debris] = "Debris";
    l.m_broadPhase[Static] = broadphase::NonMoving;
    l.m_broadPhase[Dynamic] = broadphase::Moving;
    l.m_broadPhase[Kinematic] = broadphase::Moving;
    l.m_broadPhase[Character] = broadphase::Moving;
    l.m_broadPhase[Trigger] = broadphase::Sensor;
    l.m_broadPhase[Debris] = broadphase::Debris;

    l.setCollides(Static, Dynamic, true);
    l.setCollides(Static, Character, true);
    l.setCollides(Static, Debris, true);
    for (ObjectLayer o : {Static, Dynamic, Kinematic, Character, Trigger, Debris}) {
        l.setCollides(Dynamic, o, true);
    }
    l.setCollides(Kinematic, Character, true);
    l.setCollides(Kinematic, Trigger, true);
    l.setCollides(Kinematic, Debris, true);
    l.setCollides(Character, Character, true);
    l.setCollides(Character, Trigger, true);
    return l;
}

std::optional<ObjectLayer> CollisionLayers::addLayer(std::string_view name, u8 broadPhaseLayer, LayerMask collidesWith) {
    if (m_count >= layers::kMaxLayers) {
        return std::nullopt;
    }
    OX_ASSERT(broadPhaseLayer < broadphase::kMaxLayers, "broad-phase layer {} out of range", broadPhaseLayer);
    auto layer = static_cast<ObjectLayer>(m_count++);
    m_names[layer] = std::string(name);
    m_broadPhase[layer] = broadPhaseLayer;
    for (u32 other = 0; other < m_count; ++other) {
        if (collidesWith & layerBit(static_cast<ObjectLayer>(other))) {
            setCollides(layer, static_cast<ObjectLayer>(other), true);
        }
    }
    return layer;
}

void CollisionLayers::setCollides(ObjectLayer a, ObjectLayer b, bool collide) {
    OX_ASSERT(a < layers::kMaxLayers && b < layers::kMaxLayers, "layer out of range");
    if (collide) {
        m_matrix[a] |= layerBit(b);
        m_matrix[b] |= layerBit(a);
    } else {
        m_matrix[a] &= ~layerBit(b);
        m_matrix[b] &= ~layerBit(a);
    }
}

bool CollisionLayers::collides(ObjectLayer a, ObjectLayer b) const {
    return a < layers::kMaxLayers && b < layers::kMaxLayers && (m_matrix[a] & layerBit(b)) != 0;
}

LayerMask CollisionLayers::collisionMask(ObjectLayer layer) const {
    return layer < layers::kMaxLayers ? m_matrix[layer] : 0;
}

void CollisionLayers::setBroadPhaseLayer(ObjectLayer layer, u8 broadPhaseLayer) {
    OX_ASSERT(layer < layers::kMaxLayers && broadPhaseLayer < broadphase::kMaxLayers, "layer out of range");
    m_broadPhase[layer] = broadPhaseLayer;
}

u8 CollisionLayers::broadPhaseLayer(ObjectLayer layer) const { return m_broadPhase[layer]; }

u32 CollisionLayers::broadPhaseLayerCount() const {
    u32 count = 1;
    for (u32 i = 0; i < m_count; ++i) {
        count = std::max<u32>(count, m_broadPhase[i] + 1u);
    }
    return count;
}

u32 CollisionLayers::broadPhaseMaskFor(LayerMask mask) const {
    u32 result = 0;
    for (u32 i = 0; i < m_count; ++i) {
        if (mask & layerBit(static_cast<ObjectLayer>(i))) {
            result |= 1u << m_broadPhase[i];
        }
    }
    return result;
}

std::string_view CollisionLayers::name(ObjectLayer layer) const {
    return layer < layers::kMaxLayers ? std::string_view(m_names[layer]) : std::string_view{};
}

void CollisionLayers::setName(ObjectLayer layer, std::string_view name) {
    OX_ASSERT(layer < layers::kMaxLayers, "layer out of range");
    m_names[layer] = std::string(name);
}

std::optional<ObjectLayer> CollisionLayers::find(std::string_view name) const {
    for (u32 i = 0; i < m_count; ++i) {
        if (m_names[i] == name) {
            return static_cast<ObjectLayer>(i);
        }
    }
    return std::nullopt;
}

} // namespace ox::physics
