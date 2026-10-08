#pragma once

// whenAll / whenAny / timeout.
//
//   auto [mesh, tex] = co_await ox::whenAll(loadMesh(a), loadTexture(b));   // void results become std::monostate
//   std::vector<int> v = co_await ox::whenAll(std::move(vectorOfTasks));
//   auto r = co_await ox::whenAny(waitForPlayer(), ox::toTask(ox::seconds(5)));  // r.index, r.value (variant)
//   std::optional<Reply> reply = co_await ox::timeout(rpc.call(req), 2.0);      // nullopt on timeout
//
// Branches run concurrently inside the awaiting task (same owner, same cancellation). whenAll rethrows the first
// branch exception after all branches finished. whenAny resumes as soon as one branch finishes; the losers are
// cancelled and unwound before it returns (a loser running on a background thread is torn down when it returns to
// the main thread — check currentCancellation() in long background loops). Outside a scheduler whenAny cannot
// destroy losers and waits for them to finish.

#include <oxwald/async/scheduler.hpp>
#include <oxwald/async/task.hpp>

#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ox {

namespace detail {

struct DriverPromise;
using DriverHandle = std::coroutine_handle<DriverPromise>;

struct Driver {
    using promise_type = DriverPromise;
    DriverHandle handle;
};

struct DriverPromise : PromiseBase {
    Driver get_return_object() noexcept { return {DriverHandle::from_promise(*this)}; }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    struct Final {
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(DriverHandle h) noexcept;
        void await_resume() const noexcept {}
    };
    Final final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    void unhandled_exception() noexcept { exception = std::current_exception(); }
};

template <class T>
Driver makeDriver(Task<T> task, std::optional<Slot<T>>* out) {
    if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
        out->emplace();
    } else {
        out->emplace(co_await std::move(task));
    }
}

class WhenStateBase {
public:
    WhenStateBase(usize count, bool any);
    virtual ~WhenStateBase();
    WhenStateBase(const WhenStateBase&) = delete;
    WhenStateBase& operator=(const WhenStateBase&) = delete;

    // Starts the branches; returns true when the parent must stay suspended.
    bool start(std::coroutine_handle<> parent, AsyncContext* parentCtx);
    // A branch reached its end. Returns the parent to resume (or null).
    std::coroutine_handle<> driverDone(u32 index) noexcept;
    // A cancelled loser came back to the main thread: destroy it. Returns the parent to resume (or null).
    std::coroutine_handle<> loserParked(u32 index) noexcept;
    [[nodiscard]] AsyncContext* parentContext() const noexcept { return m_parentCtx; }
    [[nodiscard]] usize winner() const noexcept { return static_cast<usize>(m_winner.load(std::memory_order_acquire)); }
    void rethrowIfError();

protected:
    void setDriver(usize i, Driver d) { m_drivers[i] = d.handle; }
    [[nodiscard]] usize count() const noexcept { return m_drivers.size(); }

private:
    enum : u8 { kNotStarted, kRunning, kDone, kDestroyed };
    std::coroutine_handle<> release() noexcept;
    void cancelLosers(u32 winner) noexcept;
    void setError(std::exception_ptr e);

    std::vector<DriverHandle> m_drivers;
    std::unique_ptr<AsyncContext[]> m_ctxs;
    std::unique_ptr<std::atomic<u8>[]> m_states;
    std::coroutine_handle<> m_parent;
    AsyncContext* m_parentCtx = nullptr;
    std::atomic<i64> m_outstanding{0};
    std::atomic<i64> m_winner{-1};
    std::mutex m_errorMutex;
    std::exception_ptr m_error;
    bool m_any;
};

template <bool kAny, class... Ts>
class WhenVariadic final : public WhenStateBase {
public:
    explicit WhenVariadic(Task<Ts>... tasks) : WhenStateBase(sizeof...(Ts), kAny) {
        init(std::index_sequence_for<Ts...>{}, std::move(tasks)...);
    }
    bool await_ready() const noexcept { return sizeof...(Ts) == 0; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        return start(h, contextOf(h));
    }
    auto await_resume() {
        rethrowIfError();
        if constexpr (kAny) {
            return collectAny(std::index_sequence_for<Ts...>{});
        } else {
            return collectAll(std::index_sequence_for<Ts...>{});
        }
    }

private:
    template <usize... I>
    void init(std::index_sequence<I...>, Task<Ts>... tasks) {
        (setDriver(I, makeDriver<Ts>(std::move(tasks), &std::get<I>(m_slots))), ...);
    }
    template <usize... I>
    auto collectAny(std::index_sequence<I...>);
    template <usize... I>
    std::tuple<Slot<Ts>...> collectAll(std::index_sequence<I...>) {
        return std::tuple<Slot<Ts>...>{std::move(*std::get<I>(m_slots))...};
    }

    std::tuple<std::optional<Slot<Ts>>...> m_slots;
};

template <class T, bool kAny>
class WhenRange final : public WhenStateBase {
public:
    explicit WhenRange(std::vector<Task<T>> tasks) : WhenStateBase(tasks.size(), kAny), m_slots(tasks.size()) {
        for (usize i = 0; i < tasks.size(); ++i) {
            setDriver(i, makeDriver<T>(std::move(tasks[i]), &m_slots[i]));
        }
    }
    bool await_ready() const noexcept { return count() == 0; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        return start(h, contextOf(h));
    }
    auto await_resume();

private:
    std::vector<std::optional<Slot<T>>> m_slots;
};

} // namespace detail

template <class... Ts>
struct WhenAnyResult {
    usize index = 0;
    std::variant<Ts...> value;
};
template <class T>
struct WhenAnyRangeResult {
    usize index = 0;
    T value;
};

namespace detail {

template <bool kAny, class... Ts>
template <usize... I>
auto WhenVariadic<kAny, Ts...>::collectAny(std::index_sequence<I...>) {
    const usize w = winner();
    std::optional<std::variant<Slot<Ts>...>> v;
    (void)((I == w ? (v.emplace(std::in_place_index<I>, std::move(*std::get<I>(m_slots))), true) : false) || ...);
    return WhenAnyResult<Slot<Ts>...>{w, std::move(*v)};
}

template <class T, bool kAny>
auto WhenRange<T, kAny>::await_resume() {
    rethrowIfError();
    if constexpr (kAny) {
        const usize w = winner();
        return WhenAnyRangeResult<Slot<T>>{w, std::move(*m_slots[w])};
    } else if constexpr (std::is_void_v<T>) {
        return;
    } else {
        std::vector<T> out;
        out.reserve(m_slots.size());
        for (auto& s : m_slots) {
            out.push_back(std::move(*s));
        }
        return out;
    }
}

} // namespace detail

template <class... Ts>
Task<std::tuple<detail::Slot<Ts>...>> whenAll(Task<Ts>... tasks) {
    co_return co_await detail::WhenVariadic<false, Ts...>(std::move(tasks)...);
}

template <class T>
Task<std::conditional_t<std::is_void_v<T>, void, std::vector<T>>> whenAll(std::vector<Task<T>> tasks) {
    if constexpr (std::is_void_v<T>) {
        co_await detail::WhenRange<T, false>(std::move(tasks));
    } else {
        co_return co_await detail::WhenRange<T, false>(std::move(tasks));
    }
}

template <class... Ts>
    requires(sizeof...(Ts) > 0)
Task<WhenAnyResult<detail::Slot<Ts>...>> whenAny(Task<Ts>... tasks) {
    co_return co_await detail::WhenVariadic<true, Ts...>(std::move(tasks)...);
}

// The vector must not be empty.
template <class T>
Task<WhenAnyRangeResult<detail::Slot<T>>> whenAny(std::vector<Task<T>> tasks) {
    OX_ASSERT(!tasks.empty(), "whenAny() of an empty range");
    co_return co_await detail::WhenRange<T, true>(std::move(tasks));
}

template <class R>
using TimeoutResult = std::conditional_t<std::is_void_v<R>, bool, std::optional<R>>;

// Awaits `awaitable` for at most `s` seconds of game time. Returns the result (or true for void awaitables) or
// nullopt/false on timeout; the timed-out operation is cancelled (its frame unwound, futures detached).
template <class A>
Task<TimeoutResult<detail::AwaitResult<A>>> timeout(A awaitable, f64 s) {
    using R = detail::AwaitResult<A>;
    auto r = co_await whenAny(toTask(std::move(awaitable)), toTask(seconds(s)));
    if constexpr (std::is_void_v<R>) {
        co_return r.index == 0;
    } else {
        if (r.index == 0) {
            co_return std::optional<R>(std::move(std::get<0>(r.value)));
        }
        co_return std::optional<R>{};
    }
}

// Same with unscaled real time (menus, network while paused).
template <class A>
Task<TimeoutResult<detail::AwaitResult<A>>> realTimeout(A awaitable, f64 s) {
    using R = detail::AwaitResult<A>;
    auto r = co_await whenAny(toTask(std::move(awaitable)), toTask(realSeconds(s)));
    if constexpr (std::is_void_v<R>) {
        co_return r.index == 0;
    } else {
        if (r.index == 0) {
            co_return std::optional<R>(std::move(std::get<0>(r.value)));
        }
        co_return std::optional<R>{};
    }
}

} // namespace ox
