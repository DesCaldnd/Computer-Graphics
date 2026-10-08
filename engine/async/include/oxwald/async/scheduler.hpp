#pragma once

// CoroutineScheduler — drives gameplay coroutines on the main/game thread.
//
//   ox::CoroutineScheduler sched;                       // the constructing thread is the scheduler thread
//   ox::CoroutineHandle h = sched.spawn(openDoor(door), {.name = "OpenDoor", .owner = entityId});
//   each frame:        sched.tick(dt, frameIndex);      // timers, frames, predicates, futures, thread hops
//   each fixed step:   sched.fixedTick(fixedDt);        // resumes co_await nextFixedUpdate()
//   entity destroyed:  sched.cancelOwner(entityId);     // unwinds all its coroutines (RAII runs)
//
// Awaitables (inside a spawned coroutine): nextFrame(), frames(n), seconds(s) (game time: time scale + pause),
// realSeconds(s), nextFixedUpdate(), until(pred), whileTrue(pred), event(signal), mainThread(),
// backgroundThread(), switchTo(executor), named("x"), currentCancellation(), spawnChild(task), any Future/Task.
//
// Threading: everything here must be called on the scheduler thread except CoroutineHandle::cancel(),
// post() and the Future/Promise API (thread-safe).

#include <oxwald/async/cancellation.hpp>
#include <oxwald/async/detail/context.hpp>
#include <oxwald/async/executor.hpp>
#include <oxwald/async/future.hpp>
#include <oxwald/async/task.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/core/types.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace ox {

enum class CoroutineStatus : u8 { Running, Completed, Cancelled, Failed };
// Finer-grained live state for introspection.
enum class CoroutineState : u8 { Pending, Suspended, Running, OffThread, CancelPending };

std::string_view toString(CoroutineStatus status);
std::string_view toString(CoroutineState state);
std::string_view toString(WaitKind kind);

namespace detail {

struct TaskRecord {
    u64 id = 0;
    std::string name;
    u64 owner = 0;
    u64 parentId = 0;
    std::atomic<CoroutineStatus> status{CoroutineStatus::Running};
    CoroutineScheduler* scheduler = nullptr; // null once finished
    std::shared_ptr<Inbox> inbox;
    AsyncContext ctx;
    std::coroutine_handle<> rootFrame;
    PromiseBase* rootPromise = nullptr;
    f64 startGameTime = 0.0;
    f64 startRealTime = 0.0;
    u64 startTick = 0;
    i32 running = 0; // nesting depth of main-thread resumes
    std::atomic<bool> finished{false};
    bool started = false;
    bool destroying = false;
    usize index = 0; // in CoroutineScheduler::m_records
    std::string error;
    CancellationRegistration tokenRegistration;
    std::vector<std::weak_ptr<TaskRecord>> children;
    std::weak_ptr<TaskRecord> self;

    ~TaskRecord();
};

// A main-thread wait registered with the scheduler. Lives inside the awaiter (i.e. in the coroutine frame);
// destroying the frame unlinks it.
struct WaitNode {
    WaitKind kind = WaitKind::None;
    AsyncContext* ctx = nullptr;
    std::coroutine_handle<> handle;
    CoroutineScheduler* scheduler = nullptr;
    std::vector<WaitNode*>* list = nullptr;
    usize index = 0;
    f64 param = 0.0;  // seconds or frame count as requested
    f64 wakeTime = 0.0;
    u64 wakeTick = 0;
    std::function<bool()> predicate;

    WaitNode() = default;
    WaitNode(WaitKind k, f64 p, std::function<bool()> pred = {}) : kind(k), param(p), predicate(std::move(pred)) {}
    WaitNode(WaitNode&& o) noexcept;
    WaitNode(const WaitNode&) = delete;
    WaitNode& operator=(const WaitNode&) = delete;
    WaitNode& operator=(WaitNode&&) = delete;
    ~WaitNode();
};

// Out-of-line suspension helpers (scheduler.cpp). Each returns the await_suspend result.
bool suspendOnWait(WaitNode& node, AsyncContext* ctx, std::coroutine_handle<> h);
bool suspendToMain(AsyncContext* ctx, std::coroutine_handle<> h);
bool suspendToExecutor(AsyncContext* ctx, std::coroutine_handle<> h, IExecutor* executor);
void setTaskName(AsyncContext* ctx, std::string name);

} // namespace detail

// Reference to a spawned root coroutine. Cheap to copy; keeps only the bookkeeping record alive (not the frame).
class CoroutineHandle {
public:
    CoroutineHandle() = default;
    explicit CoroutineHandle(std::shared_ptr<detail::TaskRecord> record) : m_record(std::move(record)) {}

    [[nodiscard]] bool valid() const { return m_record != nullptr; }
    [[nodiscard]] u64 id() const { return m_record ? m_record->id : 0; }
    [[nodiscard]] u64 owner() const { return m_record ? m_record->owner : 0; }
    // Main thread only (named() may rename a running task).
    [[nodiscard]] std::string name() const { return m_record ? m_record->name : std::string{}; }
    [[nodiscard]] CoroutineStatus status() const {
        return m_record ? m_record->status.load(std::memory_order_acquire) : CoroutineStatus::Cancelled;
    }
    [[nodiscard]] bool isDone() const { return status() != CoroutineStatus::Running; }
    [[nodiscard]] bool isRunning() const { return status() == CoroutineStatus::Running; }
    // Message of the exception that ended the task (status Failed).
    [[nodiscard]] std::string error() const { return isDone() && m_record ? m_record->error : std::string{}; }
    // Thread-safe. On the scheduler thread a suspended task is unwound immediately; otherwise at the next tick
    // (or when its background part returns to the main thread).
    void cancel() const;

    bool operator==(const CoroutineHandle& o) const { return m_record == o.m_record; }

private:
    friend class CoroutineScheduler;
    std::shared_ptr<detail::TaskRecord> m_record;
};

struct SpawnOptions {
    std::string name;          // shown in the Coroutines panel / leak report
    u64 owner = 0;             // e.g. entity id: cancelOwner(owner) cancels it; 0 = none
    CancellationToken token;   // external cancellation
    CoroutineHandle parent;    // cancelled together with the parent (inherits its owner when owner == 0)
    bool deferStart = false;   // start at the next tick instead of running synchronously to the first suspension
};

struct CoroutineInfo {
    u64 id = 0;
    std::string name;
    u64 owner = 0;
    u64 parentId = 0;
    CoroutineState state = CoroutineState::Suspended;
    std::string waitingOn; // e.g. "seconds(0.42 left)", "future", "background"; several for whenAll branches
    f64 ageSeconds = 0.0;  // game time since spawn
    f64 ageRealSeconds = 0.0;
    u64 ageFrames = 0;     // ticks since spawn
};

class CoroutineScheduler final : public IExecutor {
public:
    static constexpr u64 kAutoFrame = ~0ull;

    // background: executor for backgroundThread() (e.g. a JobSystemExecutor); null = a small owned thread pool,
    // created on first use.
    explicit CoroutineScheduler(IExecutor* background = nullptr);
    ~CoroutineScheduler() override; // shutdown(): logs leaked coroutines, cancels them
    CoroutineScheduler(const CoroutineScheduler&) = delete;
    CoroutineScheduler& operator=(const CoroutineScheduler&) = delete;

    // Starts a root coroutine; by default it runs synchronously until its first suspension.
    CoroutineHandle spawn(Task<void> task, SpawnOptions options = {});
    CoroutineHandle spawn(Task<void> task, u64 owner, std::string name = {}) {
        return spawn(std::move(task), SpawnOptions{.name = std::move(name), .owner = owner});
    }
    template <class T>
        requires(!std::is_void_v<T>)
    CoroutineHandle spawn(Task<T> task, SpawnOptions options = {}) {
        return spawn(discardResult(std::move(task)), std::move(options));
    }
    // Spawns a coroutine lambda, keeping the lambda (and its captures) alive in the frame:
    //   sched.spawn([this, door]() -> ox::Task<> { co_await ...; });
    template <class F>
        requires(std::is_invocable_v<F&> && IsTask<std::invoke_result_t<F&>>::value)
    CoroutineHandle spawn(F fn, SpawnOptions options = {}) {
        return spawn(invokeKeepingAlive(std::move(fn)), std::move(options));
    }

    // Once per frame. dt = real seconds since the previous tick; frameIndex is informative (kAutoFrame = +1).
    void tick(f64 dt, u64 frameIndex = kAutoFrame);
    // Once per fixed simulation step (resumes nextFixedUpdate()).
    void fixedTick(f64 fixedDt);

    // ECS hook: cancels every coroutine bound to `owner` (call when the entity is destroyed). Returns the count.
    usize cancelOwner(u64 owner);
    void cancelAll();
    // Logs still-running coroutines (leak report), cancels them and waits (bounded) for background parts.
    void shutdown();

    // IExecutor: fn runs on the scheduler thread during the next tick. Thread-safe.
    void post(std::function<void()> fn) override;

    [[nodiscard]] f64 gameTime() const { return m_gameTime; }
    [[nodiscard]] f64 realTime() const { return m_realTime; }
    [[nodiscard]] f64 fixedTime() const { return m_fixedTime; }
    [[nodiscard]] u64 frame() const { return m_frame; }
    [[nodiscard]] u64 tickCount() const { return m_tick; }
    void setTimeScale(f64 scale) { m_timeScale = scale < 0.0 ? 0.0 : scale; }
    [[nodiscard]] f64 timeScale() const { return m_timeScale; }
    void setPaused(bool paused) { m_paused = paused; }
    [[nodiscard]] bool paused() const { return m_paused; }

    IExecutor& backgroundExecutor();
    void setBackgroundExecutor(IExecutor* executor);

    [[nodiscard]] bool isSchedulerThread() const { return std::this_thread::get_id() == m_thread; }
    // Re-binds the scheduler thread (e.g. constructed during loading on another thread). No coroutine may be live.
    void bindToCurrentThread();

    [[nodiscard]] usize liveCount() const { return m_records.size(); }
    // Snapshot for the editor "Coroutines" panel.
    [[nodiscard]] std::vector<CoroutineInfo> coroutines() const;
    [[nodiscard]] std::vector<CoroutineHandle> handlesForOwner(u64 owner) const;

private:
    friend struct detail::WaitNode;
    friend bool detail::suspendOnWait(detail::WaitNode&, detail::AsyncContext*, std::coroutine_handle<>);
    friend bool detail::suspendToMain(detail::AsyncContext*, std::coroutine_handle<>);
    friend bool detail::suspendToExecutor(detail::AsyncContext*, std::coroutine_handle<>, IExecutor*);
    friend detail::RemoteWait detail::registerRemoteWait(detail::AsyncContext*, std::coroutine_handle<>, WaitKind);
    friend void detail::unregisterRemoteWait(detail::RemoteWait&) noexcept;
    friend void detail::parkCancelled(detail::AsyncContext*, std::coroutine_handle<>);
    friend class CoroutineHandle;

    static Task<void> discardResult(auto task) { co_await std::move(task); }
    template <class F>
    static Task<void> invokeKeepingAlive(F fn) {
        co_await fn();
    }

    void registerWait(detail::WaitNode& node, detail::AsyncContext* ctx, std::coroutine_handle<> h);
    void unlinkWait(detail::WaitNode& node) noexcept;
    void collectReady(std::vector<detail::WaitNode*>& from, std::vector<detail::WaitNode*>& ready);
    void resumeReady(std::vector<detail::WaitNode*>& ready);
    [[nodiscard]] bool isReady(const detail::WaitNode& node);

    void resumeOnMain(detail::AsyncContext* ctx, std::coroutine_handle<> h);
    void afterResume(detail::TaskRecord& record);
    void handleCancelledArrival(detail::AsyncContext* ctx);
    void drainInbox();
    void start(detail::TaskRecord& record);
    void requestCancel(detail::TaskRecord& record);
    void finishRecord(detail::TaskRecord& record, bool cancelled);
    void removeRecord(detail::TaskRecord& record);
    [[nodiscard]] std::string describe(const detail::WaitNode& node) const;

    struct RemoteEntry {
        detail::AsyncContext* ctx;
        std::coroutine_handle<> handle;
        WaitKind kind;
    };

    std::thread::id m_thread;
    std::shared_ptr<detail::Inbox> m_inbox;
    std::vector<std::shared_ptr<detail::TaskRecord>> m_records;
    std::unordered_map<u64, std::vector<detail::TaskRecord*>> m_byOwner;
    std::vector<std::shared_ptr<detail::TaskRecord>> m_deferred;
    std::vector<detail::WaitNode*> m_waits;
    std::vector<detail::WaitNode*> m_fixedWaits;
    std::unordered_map<u64, RemoteEntry> m_remote;
    u64 m_nextRemote = 0;
    u64 m_nextId = 0;

    f64 m_gameTime = 0.0;
    f64 m_realTime = 0.0;
    f64 m_fixedTime = 0.0;
    f64 m_timeScale = 1.0;
    bool m_paused = false;
    u64 m_frame = 0;
    u64 m_tick = 0;
    u64 m_fixedStep = 0;

    std::mutex m_backgroundMutex;
    IExecutor* m_background = nullptr;
    std::unique_ptr<ThreadPoolExecutor> m_ownedPool;
    bool m_shutDown = false;
};

// ---------------------------------------------------------------------------------------------- awaitables

namespace detail {

class WaitAwaiter {
public:
    WaitAwaiter(WaitKind kind, f64 param, std::function<bool()> predicate = {})
        : m_node(kind, param, std::move(predicate)) {}

    bool await_ready() const {
        switch (m_node.kind) {
        case WaitKind::Frames: return m_node.param <= 0.0;
        case WaitKind::Until: return m_node.predicate();
        case WaitKind::While: return !m_node.predicate();
        default: return false;
        }
    }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        return suspendOnWait(m_node, contextOf(h), h);
    }
    void await_resume() const noexcept {}

private:
    WaitNode m_node;
};

struct MainThreadAwaiter {
    bool await_ready() const noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        return suspendToMain(contextOf(h), h);
    }
    void await_resume() const noexcept {}
};

struct ExecutorAwaiter {
    IExecutor* executor = nullptr; // null = the scheduler's background executor
    bool await_ready() const noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        return suspendToExecutor(contextOf(h), h, executor);
    }
    void await_resume() const noexcept {}
};

struct NameAwaiter {
    std::string name;
    bool await_ready() const noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        setTaskName(contextOf(h), std::move(name));
        return false;
    }
    void await_resume() const noexcept {}
};

} // namespace detail

// Resumes at the next tick.
[[nodiscard]] inline detail::WaitAwaiter nextFrame() { return {WaitKind::Frames, 1.0}; }
// Resumes after n ticks (n == 0 continues immediately).
[[nodiscard]] inline detail::WaitAwaiter frames(u32 n) { return {WaitKind::Frames, static_cast<f64>(n)}; }
// Game time: scaled by setTimeScale, frozen while paused. Always waits at least until the next tick.
[[nodiscard]] inline detail::WaitAwaiter seconds(f64 s) { return {WaitKind::GameTime, s}; }
// Unscaled, unpaused time (UI, menus).
[[nodiscard]] inline detail::WaitAwaiter realSeconds(f64 s) { return {WaitKind::RealTime, s}; }
[[nodiscard]] inline detail::WaitAwaiter nextFixedUpdate() { return {WaitKind::FixedUpdate, 0.0}; }
// Waits until pred() is true (checked immediately, then once per tick).
template <class F>
[[nodiscard]] detail::WaitAwaiter until(F pred) {
    return {WaitKind::Until, 0.0, std::function<bool()>(std::move(pred))};
}
// Waits while pred() is true.
template <class F>
[[nodiscard]] detail::WaitAwaiter whileTrue(F pred) {
    return {WaitKind::While, 0.0, std::function<bool()>(std::move(pred))};
}

// Thread hops. backgroundThread() moves the coroutine to the scheduler's background executor; mainThread()
// brings it back (resumed during the next tick). Scheduler awaitables (frames, seconds, ...) must be used on the
// main thread. Cancellation of a task is deferred while any part of it runs off-thread.
[[nodiscard]] inline detail::MainThreadAwaiter mainThread() { return {}; }
[[nodiscard]] inline detail::ExecutorAwaiter backgroundThread() { return {}; }
[[nodiscard]] inline detail::ExecutorAwaiter switchTo(IExecutor& executor) { return {&executor}; }

// Renames the current root task (shown in the Coroutines panel).
[[nodiscard]] inline detail::NameAwaiter named(std::string name) { return {std::move(name)}; }

// `auto c = co_await ox::currentCancellation(); while (!c.isCancelled()) {...}` — for long loops running on a
// background thread, where the task cannot be unwound until it returns to the main thread.
struct CancellationCheck {
    const detail::AsyncContext* ctx = nullptr;
    [[nodiscard]] bool isCancelled() const { return ctx && ctx->chainCancelled(); }
};

namespace detail {

struct CancellationAwaiter {
    const AsyncContext* ctx = nullptr;
    bool await_ready() const noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) noexcept {
        ctx = contextOf(h);
        return false;
    }
    CancellationCheck await_resume() const noexcept { return {ctx}; }
};

struct SchedulerAwaiter {
    CoroutineScheduler* scheduler = nullptr;
    bool await_ready() const noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) noexcept {
        scheduler = schedulerOf(contextOf(h));
        return false;
    }
    CoroutineScheduler* await_resume() const noexcept { return scheduler; }
};

struct SpawnChildAwaiter {
    Task<void> task;
    std::string name;
    CoroutineHandle result;
    bool await_ready() const noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        AsyncContext* ctx = contextOf(h);
        OX_ASSERT(isScheduled(ctx), "spawnChild() outside a scheduled coroutine");
        result = schedulerOf(ctx)->spawn(std::move(task), SpawnOptions{.name = std::move(name),
                                                                       .parent = CoroutineHandle{ctx->record->self.lock()}});
        return false;
    }
    CoroutineHandle await_resume() { return std::move(result); }
};

} // namespace detail

[[nodiscard]] inline detail::CancellationAwaiter currentCancellation() { return {}; }

// The scheduler driving the current coroutine (null outside a scheduler).
[[nodiscard]] inline detail::SchedulerAwaiter currentScheduler() { return {}; }

// Spawns a child root task bound to the current one (same owner; cancelled when the current task is cancelled).
// Does not wait for it: `CoroutineHandle h = co_await ox::spawnChild(fx(), "Sparks");`
[[nodiscard]] inline detail::SpawnChildAwaiter spawnChild(Task<void> task, std::string name = {}) {
    return {std::move(task), std::move(name), {}};
}

// Awaits the next emission of an ox::Signal and returns its arguments (void / single value / tuple).
// Must be used on the scheduler thread; the signal may be emitted from any thread.
template <class... Args>
class EventAwaiter {
public:
    using Values = std::tuple<std::decay_t<Args>...>;
    using Result = std::conditional_t<sizeof...(Args) == 0, void,
                                      std::conditional_t<sizeof...(Args) == 1,
                                                         std::tuple_element_t<0, std::tuple<std::decay_t<Args>..., void>>,
                                                         Values>>;

    explicit EventAwaiter(Signal<Args...>& signal) : m_signal(&signal), m_box(std::make_shared<Box>()) {}
    EventAwaiter(EventAwaiter&& o) noexcept
        : m_signal(o.m_signal), m_box(std::move(o.m_box)), m_connection(std::move(o.m_connection)),
          m_remote(std::exchange(o.m_remote, {})) {}
    ~EventAwaiter() {
        m_connection.disconnect();
        detail::unregisterRemoteWait(m_remote);
    }

    bool await_ready() const noexcept { return false; }
    template <class P>
    bool await_suspend(std::coroutine_handle<P> h) {
        detail::AsyncContext* ctx = detail::contextOf(h);
        OX_ASSERT(detail::isScheduled(ctx) && !ctx->offThread, "event() must be awaited on the scheduler thread");
        if (ctx->chainCancelled()) {
            detail::parkCancelled(ctx, h);
            return true;
        }
        m_remote = detail::registerRemoteWait(ctx, h, WaitKind::Event);
        m_box->inbox = m_remote.inbox;
        m_box->id = m_remote.id;
        std::shared_ptr<Box> box = m_box;
        m_connection = m_signal->connect([box](Args... args) {
            std::lock_guard lock(box->mutex);
            if (box->values) {
                return;
            }
            box->values.emplace(args...);
            detail::signalRemoteWait(box->inbox, box->id);
        });
        return true;
    }
    Result await_resume() {
        m_connection.disconnect();
        std::lock_guard lock(m_box->mutex);
        if constexpr (sizeof...(Args) == 1) {
            return std::move(std::get<0>(*m_box->values));
        } else if constexpr (sizeof...(Args) > 1) {
            return std::move(*m_box->values);
        }
    }

private:
    struct Box {
        std::mutex mutex;
        std::optional<Values> values;
        std::shared_ptr<detail::Inbox> inbox;
        u64 id = 0;
    };
    Signal<Args...>* m_signal;
    std::shared_ptr<Box> m_box;
    ScopedConnection m_connection;
    detail::RemoteWait m_remote;
};

template <class... Args>
[[nodiscard]] EventAwaiter<Args...> event(Signal<Args...>& signal) {
    return EventAwaiter<Args...>{signal};
}

} // namespace ox
