#pragma once

// ox::Promise<T> / ox::Future<T> — thread-safe one-shot results.
//
//   ox::Promise<Mesh> p;  ox::Future<Mesh> f = p.future();
//   worker: p.setValue(mesh);            // any thread
//   coroutine: Mesh m = co_await f;      // resumes on the scheduler that awaited it, during its next tick
//
// * A coroutine awaiting from the scheduler thread is always resumed by that scheduler (never on the completing
//   thread). A coroutine awaiting from a background thread (after co_await backgroundThread()) or outside any
//   scheduler is resumed inline on the completing thread.
// * Destroying/cancelling the awaiting coroutine detaches it from the future (no dangling continuation).
// * Futures are copyable (shared state). Several coroutines may await the same future; each gets a copy of the
//   value (move-only values are moved out — await those from one place only).
// * A Promise destroyed without a result completes the future with ox::BrokenPromise.

#include <oxwald/async/detail/context.hpp>
#include <oxwald/async/executor.hpp>
#include <oxwald/core/assert.hpp>
#include <oxwald/core/types.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ox {

class AsyncError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
class BrokenPromise final : public AsyncError {
public:
    BrokenPromise() : AsyncError("broken promise: the producer was destroyed without a result") {}
};

namespace detail {

class FutureStateBase {
public:
    virtual ~FutureStateBase() = default;

    [[nodiscard]] bool ready() const noexcept { return m_ready.load(std::memory_order_acquire); }
    [[nodiscard]] bool hasError() const noexcept { return ready() && m_exception != nullptr; }
    [[nodiscard]] std::exception_ptr exception() const noexcept { return ready() ? m_exception : nullptr; }
    // Message of the stored exception ("" when there is none).
    [[nodiscard]] std::string errorMessage() const {
        if (!hasError()) {
            return {};
        }
        try {
            std::rethrow_exception(m_exception);
        } catch (const std::exception& e) {
            return e.what();
        } catch (...) {
            return "unknown error";
        }
    }

    // Stores fn and returns its id, or returns 0 without storing when already completed. The id is also written
    // to *idOut under the lock, i.e. before fn can possibly run on another thread.
    u64 addContinuation(std::function<void()> fn, u64* idOut = nullptr) {
        std::lock_guard lock(m_mutex);
        if (ready()) {
            return 0;
        }
        const u64 id = ++m_nextId;
        m_continuations.emplace_back(id, std::move(fn));
        if (idOut) {
            *idOut = id;
        }
        return id;
    }
    // O(log n): ids are appended in increasing order; removed entries become tombstones (compacted lazily).
    void removeContinuation(u64 id) {
        std::lock_guard lock(m_mutex);
        auto it = std::lower_bound(m_continuations.begin(), m_continuations.end(), id,
                                   [](const auto& c, u64 v) { return c.first < v; });
        if (it == m_continuations.end() || it->first != id || !it->second) {
            return;
        }
        it->second = nullptr;
        if (++m_tombstones * 2 > m_continuations.size()) {
            std::erase_if(m_continuations, [](const auto& c) { return !c.second; });
            m_tombstones = 0;
        }
    }
    void setException(std::exception_ptr e) {
        complete([&] { m_exception = std::move(e); });
    }
    void wait() const {
        if (ready()) {
            return;
        }
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return ready(); });
    }

protected:
    // Runs setter under the lock, then the continuations outside of it. Returns false if already completed.
    template <class F>
    bool complete(F&& setter) {
        std::vector<std::pair<u64, std::function<void()>>> conts;
        {
            std::lock_guard lock(m_mutex);
            if (ready()) {
                return false;
            }
            setter();
            m_ready.store(true, std::memory_order_release);
            conts.swap(m_continuations);
            m_tombstones = 0;
        }
        m_cv.notify_all();
        for (auto& c : conts) {
            if (c.second) {
                c.second();
            }
        }
        return true;
    }
    void rethrowIfError() const {
        if (m_exception) {
            std::rethrow_exception(m_exception);
        }
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::atomic<bool> m_ready{false};
    std::exception_ptr m_exception;
    u64 m_nextId = 0;
    usize m_tombstones = 0;
    std::vector<std::pair<u64, std::function<void()>>> m_continuations;
};

template <class T>
class FutureState final : public FutureStateBase {
public:
    template <class U>
    bool setValue(U&& v) {
        return complete([&] { m_value.emplace(std::forward<U>(v)); });
    }
    const T& get() const {
        OX_ASSERT(ready(), "Future::get() before the result is ready");
        rethrowIfError();
        return *m_value;
    }
    T consume() {
        OX_ASSERT(ready(), "Future result consumed before ready");
        rethrowIfError();
        if constexpr (std::is_copy_constructible_v<T>) {
            return *m_value;
        } else {
            return std::move(*m_value);
        }
    }

private:
    std::optional<T> m_value;
};

template <>
class FutureState<void> final : public FutureStateBase {
public:
    bool setValue() {
        return complete([] {});
    }
    void get() const {
        OX_ASSERT(ready(), "Future::get() before the result is ready");
        rethrowIfError();
    }
    void consume() { get(); }
};

template <class T>
struct FutureAwaiter {
    std::shared_ptr<FutureState<T>> state;
    RemoteWait remote;
    u64 continuation = 0;

    explicit FutureAwaiter(std::shared_ptr<FutureState<T>> s) : state(std::move(s)) {}
    FutureAwaiter(FutureAwaiter&& o) noexcept
        : state(std::move(o.state)), remote(std::exchange(o.remote, {})), continuation(std::exchange(o.continuation, 0)) {}
    FutureAwaiter(const FutureAwaiter&) = delete;
    FutureAwaiter& operator=(const FutureAwaiter&) = delete;
    FutureAwaiter& operator=(FutureAwaiter&&) = delete;
    ~FutureAwaiter() {
        if (continuation != 0 && state) {
            state->removeContinuation(continuation);
        }
        unregisterRemoteWait(remote);
    }

    bool await_ready() const noexcept { return state->ready(); }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        AsyncContext* ctx = contextOf(h);
        if (isScheduled(ctx) && ctx->chainCancelled()) {
            parkCancelled(ctx, h);
            return true;
        }
        remote = registerRemoteWait(ctx, h, WaitKind::Future);
        if (remote.id != 0) {
            continuation = state->addContinuation([inbox = remote.inbox, id = remote.id] { signalRemoteWait(inbox, id); });
            if (continuation == 0) {
                unregisterRemoteWait(remote);
                return false; // completed meanwhile
            }
            return true;
        }
        // Inline mode: the coroutine may be resumed (and this awaiter destroyed) on another thread as soon as the
        // lock is released, so `this` must not be touched after addContinuation.
        return state->addContinuation([h] { h.resume(); }, &continuation) != 0;
    }
    T await_resume() { return state->consume(); }
};

} // namespace detail

template <class T>
class Promise;

template <class T = void>
class Future {
public:
    using value_type = T;
    using State = detail::FutureState<T>;

    Future() = default;
    explicit Future(std::shared_ptr<State> state) : m_state(std::move(state)) {}

    [[nodiscard]] bool valid() const noexcept { return m_state != nullptr; }
    [[nodiscard]] bool isReady() const noexcept { return m_state && m_state->ready(); }
    [[nodiscard]] bool hasError() const noexcept { return m_state && m_state->hasError(); }
    [[nodiscard]] std::string errorMessage() const { return m_state ? m_state->errorMessage() : std::string{}; }
    // Non-blocking: asserts readiness, rethrows a stored exception.
    decltype(auto) get() const { return m_state->get(); }
    // Blocks the calling thread. Never call it on the scheduler thread for a future resolved by that scheduler.
    void wait() const { m_state->wait(); }

    // Calls fn() once completed: inline now if already complete, else on the completing thread. Returns an id
    // for cancelOnReady (0 when it already ran).
    u64 onReady(std::function<void()> fn) const {
        const u64 id = m_state->addContinuation(fn);
        if (id == 0) {
            fn();
        }
        return id;
    }
    void cancelOnReady(u64 id) const {
        if (id != 0) {
            m_state->removeContinuation(id);
        }
    }

    detail::FutureAwaiter<T> operator co_await() const { return detail::FutureAwaiter<T>{m_state}; }

    // Type-erased access (Lua bridge, tooling).
    [[nodiscard]] const std::shared_ptr<State>& state() const { return m_state; }

private:
    std::shared_ptr<State> m_state;
};

// Producer side. Move-only; destroying it without a result breaks the promise.
template <class T = void>
class Promise {
public:
    Promise() : m_state(std::make_shared<detail::FutureState<T>>()) {}
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&& o) noexcept {
        if (this != &o) {
            breakIfPending();
            m_state = std::move(o.m_state);
        }
        return *this;
    }
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;
    ~Promise() { breakIfPending(); }

    [[nodiscard]] Future<T> future() const { return Future<T>{m_state}; }
    [[nodiscard]] bool isSet() const { return m_state && m_state->ready(); }

    // Each returns false if the promise was already completed.
    template <class U = T>
        requires(!std::is_void_v<T> && std::is_constructible_v<T, U &&>)
    bool setValue(U&& v) {
        return m_state->setValue(std::forward<U>(v));
    }
    bool setValue()
        requires std::is_void_v<T>
    {
        return m_state->setValue();
    }
    bool setException(std::exception_ptr e) {
        if (m_state->ready()) {
            return false;
        }
        m_state->setException(std::move(e));
        return true;
    }
    bool setError(std::string message) { return setException(std::make_exception_ptr(AsyncError(std::move(message)))); }

private:
    void breakIfPending() {
        if (m_state && !m_state->ready()) {
            m_state->setException(std::make_exception_ptr(BrokenPromise{}));
        }
    }
    std::shared_ptr<detail::FutureState<T>> m_state;
};

template <class T>
Future<std::decay_t<T>> makeReadyFuture(T&& value) {
    Promise<std::decay_t<T>> p;
    p.setValue(std::forward<T>(value));
    return p.future();
}
inline Future<void> makeReadyFuture() {
    Promise<void> p;
    p.setValue();
    return p.future();
}
template <class T = void>
Future<T> makeErrorFuture(std::string message) {
    Promise<T> p;
    p.setError(std::move(message));
    return p.future();
}

// Runs fn() on `executor`; the future completes with its result (or exception).
template <class F, class R = std::invoke_result_t<F&>>
Future<R> runAsync(IExecutor& executor, F fn) {
    auto promise = std::make_shared<Promise<R>>();
    Future<R> future = promise->future();
    executor.post([promise, fn = std::move(fn)]() mutable {
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                promise->setValue();
            } else {
                promise->setValue(fn());
            }
        } catch (...) {
            promise->setException(std::current_exception());
        }
    });
    return future;
}

// Adapter for callback-style APIs, in one line:
//   Mesh m = co_await ox::awaitCallback<Mesh>([&](auto resume) { assets.loadAsync(path, resume); });
//   co_await ox::awaitCallback([&](auto resume) { physics.queryAsync(q, [resume](auto&&...) { resume(); }); });
// `resume` is copyable and thread-safe; extra calls are ignored. If every copy is destroyed without being called
// the await fails with ox::BrokenPromise (no hang).
template <class T = void, class F>
Future<T> awaitCallback(F&& start) {
    auto promise = std::make_shared<Promise<T>>();
    Future<T> future = promise->future();
    if constexpr (std::is_void_v<T>) {
        std::forward<F>(start)([promise]() { promise->setValue(); });
    } else {
        std::forward<F>(start)([promise](T value) { promise->setValue(std::move(value)); });
    }
    return future;
}

} // namespace ox
