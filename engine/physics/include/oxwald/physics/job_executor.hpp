#pragma once

#include <oxwald/core/types.hpp>

namespace ox::physics {

// Lets Jolt run its jobs on the engine's job system instead of its own thread pool.
// `submit` must eventually call fn(ctx) exactly once on any thread; it must not block.
// The physics world adapts this to JPH::JobSystemWithBarrier.
class IPhysicsJobExecutor {
public:
    virtual ~IPhysicsJobExecutor() = default;
    [[nodiscard]] virtual u32 maxConcurrency() const = 0;
    virtual void submit(void (*fn)(void*), void* ctx) = 0;
};

} // namespace ox::physics
