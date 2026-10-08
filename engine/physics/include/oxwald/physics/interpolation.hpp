#pragma once

#include <oxwald/physics/types.hpp>

#include <unordered_map>

namespace ox::physics {

class PhysicsWorld;

// Fixed-timestep accumulator for the game loop:
//   u32 n = stepper.advance(frameDt); for (i < n) { world.step(stepper.fixedDt()); interp.capture(world); }
//   render with interp.get(body, stepper.alpha())
class FixedStepper {
public:
    explicit FixedStepper(f32 hz = 60.f, u32 maxStepsPerFrame = 4) : m_dt(1.f / hz), m_maxSteps(maxStepsPerFrame) {}

    // Returns how many fixed steps to run this frame. Drops time beyond maxStepsPerFrame
    // (spiral-of-death protection).
    u32 advance(f32 frameDt);
    [[nodiscard]] f32 fixedDt() const { return m_dt; }
    // Interpolation factor between the previous and the current physics state, [0, 1).
    [[nodiscard]] f32 alpha() const { return m_accumulator / m_dt; }
    void reset() { m_accumulator = 0.f; }

private:
    f32 m_dt;
    u32 m_maxSteps;
    f32 m_accumulator = 0.f;
};

// Keeps previous & current transforms of tracked bodies for render interpolation.
class TransformInterpolator {
public:
    void track(BodyHandle body);
    void untrack(BodyHandle body);
    void clear() { m_entries.clear(); }
    [[nodiscard]] bool tracked(BodyHandle body) const { return m_entries.contains(body); }

    // Call after every fixed step: previous ← current, current ← world. Bodies that no longer
    // exist are dropped.
    void capture(const PhysicsWorld& world);
    // Snap (no interpolation) — after teleports.
    void reset(BodyHandle body, const Transform& transform);

    [[nodiscard]] Transform get(BodyHandle body, f32 alpha) const;
    [[nodiscard]] const Transform* previous(BodyHandle body) const;
    [[nodiscard]] const Transform* current(BodyHandle body) const;

    // lerp position, shortest-path slerp rotation.
    static Transform interpolate(const Transform& a, const Transform& b, f32 alpha);

private:
    struct Entry {
        Transform previous;
        Transform current;
        bool initialized = false;
    };
    std::unordered_map<BodyHandle, Entry> m_entries;
};

} // namespace ox::physics
