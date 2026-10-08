#pragma once

// ox::Task<T> — lazy, move-only coroutine with symmetric transfer.
//
//   ox::Task<int> loadCount() { co_await ox::seconds(1.0); co_return 42; }
//   ox::Task<> door() { int n = co_await loadCount(); ... }
//
// * Lazy: nothing runs until the task is awaited (or spawned on a CoroutineScheduler).
// * Exceptions thrown inside propagate to the awaiter (co_await rethrows); for engine-style error handling return
//   ox::Result<T> from the task instead.
// * Destroying a Task destroys its (suspended) frame and everything it awaits: RAII destructors run, timers and
//   future continuations are unregistered. That is how cancellation unwinds.
// * The scheduler context (cancellation, owner, time source) propagates from awaiter to awaited task.

#include <oxwald/async/detail/context.hpp>
#include <oxwald/core/assert.hpp>

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace ox {

template <class T = void>
class Task;

namespace detail {

struct TaskFinalAwaiter {
    bool await_ready() const noexcept { return false; }
    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
        std::coroutine_handle<> c = h.promise().continuation;
        return c ? c : std::noop_coroutine();
    }
    void await_resume() const noexcept {}
};

template <class T>
struct TaskPromise : PromiseBase {
    Task<T> get_return_object() noexcept;
    std::suspend_always initial_suspend() const noexcept { return {}; }
    TaskFinalAwaiter final_suspend() const noexcept { return {}; }
    void unhandled_exception() noexcept { exception = std::current_exception(); }

    template <class U = T>
        requires std::is_convertible_v<U&&, T>
    void return_value(U&& v) {
        value.emplace(std::forward<U>(v));
    }

    T result() {
        if (exception) {
            std::rethrow_exception(exception);
        }
        OX_ASSERT(value.has_value(), "Task result read before completion");
        return std::move(*value);
    }

    std::optional<T> value;
};

template <>
struct TaskPromise<void> : PromiseBase {
    Task<void> get_return_object() noexcept;
    std::suspend_always initial_suspend() const noexcept { return {}; }
    TaskFinalAwaiter final_suspend() const noexcept { return {}; }
    void unhandled_exception() noexcept { exception = std::current_exception(); }
    void return_void() const noexcept {}
    void result() const {
        if (exception) {
            std::rethrow_exception(exception);
        }
    }
};

} // namespace detail

template <class T>
class [[nodiscard]] Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using value_type = T;
    using Handle = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle h) noexcept : m_handle(h) {}
    Task(Task&& o) noexcept : m_handle(std::exchange(o.m_handle, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            reset();
            m_handle = std::exchange(o.m_handle, {});
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(m_handle); }
    [[nodiscard]] bool done() const noexcept { return m_handle && m_handle.done(); }
    [[nodiscard]] Handle handle() const noexcept { return m_handle; }
    Handle release() noexcept { return std::exchange(m_handle, {}); }
    void reset() noexcept {
        if (m_handle) {
            std::exchange(m_handle, {}).destroy();
        }
    }

    struct Awaiter {
        Handle h;
        bool await_ready() const noexcept { return !h || h.done(); }
        template <class P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) noexcept {
            h.promise().continuation = parent;
            h.promise().ctx = detail::contextOf(parent);
            return h;
        }
        T await_resume() {
            OX_ASSERT(h, "co_await on an empty Task");
            return h.promise().result();
        }
    };
    Awaiter operator co_await() & noexcept { return Awaiter{m_handle}; }
    Awaiter operator co_await() && noexcept { return Awaiter{m_handle}; }

private:
    Handle m_handle;
};

namespace detail {
template <class T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}
inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

// Result type of `co_await a`.
template <class A>
decltype(auto) getAwaiter(A&& a) {
    if constexpr (requires { std::forward<A>(a).operator co_await(); }) {
        return std::forward<A>(a).operator co_await();
    } else {
        return std::forward<A>(a);
    }
}
template <class A>
using AwaitResult = decltype(getAwaiter(std::declval<A>()).await_resume());

// void -> std::monostate so results fit in tuples/variants.
template <class T>
using Slot = std::conditional_t<std::is_void_v<T>, std::monostate, T>;
} // namespace detail

template <class T>
struct IsTask : std::false_type {};
template <class T>
struct IsTask<Task<T>> : std::true_type {};

// Turns any awaitable into a Task (moves the awaitable into the task frame).
template <class A>
Task<detail::AwaitResult<A>> toTask(A awaitable) {
    if constexpr (std::is_void_v<detail::AwaitResult<A>>) {
        co_await std::move(awaitable);
    } else {
        co_return co_await std::move(awaitable);
    }
}

} // namespace ox
