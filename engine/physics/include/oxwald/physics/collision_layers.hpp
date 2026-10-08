#pragma once

#include <oxwald/physics/types.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace ox::physics {

// Broad-phase layers group object layers into separate acceleration trees. Fewer is better;
// typical split: static geometry, moving bodies, sensors, debris.
namespace broadphase {
inline constexpr u8 NonMoving = 0;
inline constexpr u8 Moving = 1;
inline constexpr u8 Sensor = 2;
inline constexpr u8 Debris = 3;
inline constexpr u32 kMaxLayers = 8;
} // namespace broadphase

// Object layers (up to 32) + symmetric collision matrix + mapping to broad-phase layers.
// Copied into the world at creation; immutable afterwards (Jolt caches the filters).
class CollisionLayers {
public:
    // Static, Dynamic, Kinematic, Character, Trigger, Debris with a sensible matrix:
    //   Static    ↔ Dynamic, Character, Debris
    //   Dynamic   ↔ everything
    //   Kinematic ↔ Dynamic, Character, Trigger, Debris
    //   Character ↔ Static, Dynamic, Kinematic, Character, Trigger
    //   Trigger   ↔ Dynamic, Kinematic, Character
    //   Debris    ↔ Static, Dynamic, Kinematic
    static CollisionLayers makeDefault();

    // Adds a user layer (returns its index, or nullopt when 32 layers exist). It collides with
    // the layers in `collidesWith` (symmetric).
    std::optional<ObjectLayer> addLayer(std::string_view name, u8 broadPhaseLayer = broadphase::Moving,
                                        LayerMask collidesWith = layerBit(layers::Static) | layerBit(layers::Dynamic) |
                                                                 layerBit(layers::Kinematic) |
                                                                 layerBit(layers::Character));

    void setCollides(ObjectLayer a, ObjectLayer b, bool collide);
    [[nodiscard]] bool collides(ObjectLayer a, ObjectLayer b) const;
    // Bit mask of all layers `layer` collides with — usable directly as a query LayerMask.
    [[nodiscard]] LayerMask collisionMask(ObjectLayer layer) const;

    void setBroadPhaseLayer(ObjectLayer layer, u8 broadPhaseLayer);
    [[nodiscard]] u8 broadPhaseLayer(ObjectLayer layer) const;
    [[nodiscard]] u32 broadPhaseLayerCount() const;
    // Mask of broad-phase layers that contain at least one object layer from `mask`.
    [[nodiscard]] u32 broadPhaseMaskFor(LayerMask mask) const;

    [[nodiscard]] u32 layerCount() const { return m_count; }
    [[nodiscard]] std::string_view name(ObjectLayer layer) const;
    void setName(ObjectLayer layer, std::string_view name);
    [[nodiscard]] std::optional<ObjectLayer> find(std::string_view name) const;

private:
    u32 m_count = 0;
    std::array<LayerMask, layers::kMaxLayers> m_matrix{};
    std::array<u8, layers::kMaxLayers> m_broadPhase{};
    std::array<std::string, layers::kMaxLayers> m_names{};
};

} // namespace ox::physics
