#pragma once

// Internal plumbing shared by Task, Future, the scheduler and the combinators.
//
// Every coroutine started by a CoroutineScheduler runs inside an AsyncContext. A context is one linear "fiber":
// the root task and every Task it awaits share the root context; each whenAll/whenAny branch ("driver") gets a
// child context. The context knows its root TaskRecord (scheduler, name, owner), whether it was cancelled, and
// whether its chain is currently executing away from the scheduler thread (offThread). Off-thread chains can never
// be destroyed, so cancellation of a subtree is deferred until offCount (chains off-thread in the subtree) drops
// to zero; cancelled chains that come back to the main thread are parked and torn down there.

#include <oxwald/async/detail/frame_pool.hpp>
#include <oxwald/core/types.hpp>

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <type_traits>

namespace ox {

class CoroutineScheduler;

// What a suspended coroutine waits for (introspection / editor "Coroutines" panel).
enum class WaitKind : u8 {
    None,
    Frames,
    GameTime,
    RealTime,
    Until,
    While,
    FixedUpdate,
    Future,
    Event,
    MainThread,
    Background,
};

namespace detail {

struct TaskRecord;
class WhenStateBase;
struct Inbox;

struct AsyncContext {
    TaskRecord* record = nullptr; // null when not driven by a CoroutineScheduler
    AsyncContext* parent = nullptr;
    WhenStateBase* when = nullptr; // set for whenAll/whenAny branches
    u32 index = 0;                 // branch index inside `when`
    std::atomic<bool> cancelRequested{false};
    bool offThread = false;        // this chain runs (or is queued) away from the scheduler thread
    std::atomic<i32> offCount{0};  // off-thread chains in this subtree (including this one)

    [[nodiscard]] bool chainCancelled() const noexcept {
        for (const AsyncContext* c = this; c; c = c->parent) {
            if (c->cancelRequested.load(std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }
    void markOff() noexcept {
        offThread = true;
        for (AsyncContext* c = this; c; c = c->parent) {
            c->offCount.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    void markOn() noexcept {
        offThread = false;
        for (AsyncContext* c = this; c; c = c->parent) {
            c->offCount.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
};

// Common promise state. Frames come from the pooled frame allocator.
struct PromiseBase {
    std::coroutine_handle<> continuation;
    AsyncContext* ctx = nullptr;
    std::exception_ptr exception;

    static void* operator new(std::size_t size) { return framePoolAllocate(size); }
    static void operator delete(void* p, std::size_t size) noexcept { framePoolFree(p, size); }
};

template <class P>
[[nodiscard]] AsyncContext* contextOf(std::coroutine_handle<P> h) noexcept {
    if constexpr (std::is_base_of_v<PromiseBase, P>) {
        return h.promise().ctx;
    } else {
        return nullptr;
    }
}

[[nodiscard]] inline bool isScheduled(const AsyncContext* ctx) noexcept { return ctx && ctx->record; }

// --- implemented in scheduler.cpp -------------------------------------------------------------------------

// Registration of a wait that is completed from an arbitrary thread (futures, signals). id == 0 means "not
// scheduler driven" (no context or running off-thread): the awaiter must resume inline on the completing thread.
struct RemoteWait {
    std::shared_ptr<Inbox> inbox;
    CoroutineScheduler* scheduler = nullptr;
    u64 id = 0;
};
RemoteWait registerRemoteWait(AsyncContext* ctx, std::coroutine_handle<> h, WaitKind kind);
void unregisterRemoteWait(RemoteWait& wait) noexcept;
// Thread-safe; ignored if the wait was unregistered meanwhile.
void signalRemoteWait(const std::shared_ptr<Inbox>& inbox, u64 id);

// Called by awaiters that see a cancelled chain: the coroutine stays suspended without registering anything.
// On the main thread the scheduler unwinds it after the current resume; off-thread chains are routed back to the
// main thread first (they can only be destroyed there).
void parkCancelled(AsyncContext* ctx, std::coroutine_handle<> h);

[[nodiscard]] CoroutineScheduler* schedulerOf(const AsyncContext* ctx) noexcept;

} // namespace detail
} // namespace ox
