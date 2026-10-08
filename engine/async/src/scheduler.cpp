#include "inbox.hpp"

#include <oxwald/async/combinators.hpp>
#include <oxwald/async/scheduler.hpp>
#include <oxwald/core/log.hpp>

#include <algorithm>
#include <chrono>
#include <format>

namespace ox {

namespace {
constexpr std::string_view kLog = "async";
}

std::string_view toString(CoroutineStatus status) {
    switch (status) {
    case CoroutineStatus::Running: return "Running";
    case CoroutineStatus::Completed: return "Completed";
    case CoroutineStatus::Cancelled: return "Cancelled";
    case CoroutineStatus::Failed: return "Failed";
    }
    return "?";
}

std::string_view toString(CoroutineState state) {
    switch (state) {
    case CoroutineState::Pending: return "Pending";
    case CoroutineState::Suspended: return "Suspended";
    case CoroutineState::Running: return "Running";
    case CoroutineState::OffThread: return "OffThread";
    case CoroutineState::CancelPending: return "CancelPending";
    }
    return "?";
}

std::string_view toString(WaitKind kind) {
    switch (kind) {
    case WaitKind::None: return "none";
    case WaitKind::Frames: return "frames";
    case WaitKind::GameTime: return "seconds";
    case WaitKind::RealTime: return "realSeconds";
    case WaitKind::Until: return "until";
    case WaitKind::While: return "whileTrue";
    case WaitKind::FixedUpdate: return "nextFixedUpdate";
    case WaitKind::Future: return "future";
    case WaitKind::Event: return "event";
    case WaitKind::MainThread: return "mainThread";
    case WaitKind::Background: return "background";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------- root task

namespace detail {

namespace {

void onRootFinal(AsyncContext* ctx) noexcept;

struct RootTask {
    struct promise_type : PromiseBase {
        RootTask get_return_object() noexcept {
            return {std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() const noexcept { return {}; }
        struct Final {
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept { onRootFinal(h.promise().ctx); }
            void await_resume() const noexcept {}
        };
        Final final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        void unhandled_exception() noexcept { exception = std::current_exception(); }
    };
    std::coroutine_handle<promise_type> handle;
};

RootTask makeRoot(Task<void> task) { co_await std::move(task); }

void onRootFinal(AsyncContext* ctx) noexcept {
    TaskRecord* r = ctx->record;
    r->finished.store(true, std::memory_order_release);
    if (ctx->offThread) {
        // The scheduler finalises it on its own thread. Nothing may touch the frame after the push.
        r->inbox->push(InboxItem{InboxItem::Kind::RootDone, ctx, {}, 0, {}});
    }
}

std::string exceptionMessage(const std::exception_ptr& e) {
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        return ex.what();
    } catch (...) {
        return "unknown exception";
    }
}

} // namespace

TaskRecord::~TaskRecord() {
    if (rootFrame) {
        rootFrame.destroy(); // never started (scheduler gone)
    }
}

// ---------------------------------------------------------------------------------------------- wait nodes

WaitNode::WaitNode(WaitNode&& o) noexcept
    : kind(o.kind), param(o.param), wakeTime(o.wakeTime), wakeTick(o.wakeTick), predicate(std::move(o.predicate)) {
    OX_ASSERT(o.list == nullptr, "moving a registered wait");
}

WaitNode::~WaitNode() {
    if (list && scheduler) {
        scheduler->unlinkWait(*this);
    }
}

CoroutineScheduler* schedulerOf(const AsyncContext* ctx) noexcept {
    return ctx && ctx->record ? ctx->record->scheduler : nullptr;
}

bool suspendOnWait(WaitNode& node, AsyncContext* ctx, std::coroutine_handle<> h) {
    OX_ASSERT(isScheduled(ctx), "co_await {}() outside a coroutine spawned on a CoroutineScheduler", toString(node.kind));
    OX_ASSERT(!ctx->offThread, "co_await {}() on a background thread: co_await ox::mainThread() first",
              toString(node.kind));
    if (ctx->chainCancelled()) {
        parkCancelled(ctx, h);
        return true;
    }
    ctx->record->scheduler->registerWait(node, ctx, h);
    return true;
}

bool suspendToMain(AsyncContext* ctx, std::coroutine_handle<> h) {
    OX_ASSERT(isScheduled(ctx), "co_await mainThread() outside a coroutine spawned on a CoroutineScheduler");
    if (!ctx->offThread) {
        return false; // already there
    }
    ctx->record->inbox->push(InboxItem{InboxItem::Kind::Resume, ctx, h, 0, {}});
    return true;
}

bool suspendToExecutor(AsyncContext* ctx, std::coroutine_handle<> h, IExecutor* executor) {
    if (isScheduled(ctx)) {
        CoroutineScheduler* sched = ctx->record->scheduler;
        if (executor == sched) {
            return suspendToMain(ctx, h);
        }
        if (!executor) {
            executor = &sched->backgroundExecutor();
        }
        if (!ctx->offThread) {
            if (ctx->chainCancelled()) {
                return true; // unwound by the scheduler after this resume
            }
            ctx->markOff();
        }
    }
    OX_ASSERT(executor, "backgroundThread() outside a scheduler: use switchTo(executor)");
    executor->post([h] { h.resume(); });
    return true;
}

void setTaskName(AsyncContext* ctx, std::string name) {
    if (ctx && ctx->record) {
        ctx->record->name = std::move(name);
    }
}

RemoteWait registerRemoteWait(AsyncContext* ctx, std::coroutine_handle<> h, WaitKind kind) {
    if (!isScheduled(ctx) || ctx->offThread) {
        return {};
    }
    CoroutineScheduler* s = ctx->record->scheduler;
    OX_ASSERT(s->isSchedulerThread(), "scheduler awaitable used off the scheduler thread");
    const u64 id = ++s->m_nextRemote;
    s->m_remote.emplace(id, CoroutineScheduler::RemoteEntry{ctx, h, kind});
    return RemoteWait{s->m_inbox, s, id};
}

void unregisterRemoteWait(RemoteWait& wait) noexcept {
    if (wait.id != 0 && wait.scheduler) {
        wait.scheduler->m_remote.erase(wait.id);
    }
    wait = {};
}

void signalRemoteWait(const std::shared_ptr<Inbox>& inbox, u64 id) {
    if (inbox && id != 0) {
        inbox->push(InboxItem{InboxItem::Kind::Signal, nullptr, {}, id, {}});
    }
}

void parkCancelled(AsyncContext* ctx, std::coroutine_handle<> h) {
    if (isScheduled(ctx) && ctx->offThread) {
        ctx->record->inbox->push(InboxItem{InboxItem::Kind::Resume, ctx, h, 0, {}});
    }
    // On the main thread nothing to do: the scheduler unwinds the task when the current resume returns.
}

} // namespace detail

// ---------------------------------------------------------------------------------------------- handle

void CoroutineHandle::cancel() const {
    if (!m_record || m_record->status.load(std::memory_order_acquire) != CoroutineStatus::Running) {
        return;
    }
    CoroutineScheduler* s = m_record->scheduler;
    if (s && s->isSchedulerThread()) {
        s->requestCancel(*m_record);
        return;
    }
    m_record->ctx.cancelRequested.store(true, std::memory_order_release); // visible to background loops now
    std::weak_ptr<detail::TaskRecord> weak = m_record;
    m_record->inbox->push(detail::InboxItem{detail::InboxItem::Kind::Call, nullptr, {}, 0, [weak] {
                                                if (auto r = weak.lock(); r && r->scheduler) {
                                                    r->scheduler->requestCancel(*r);
                                                }
                                            }});
}

// ---------------------------------------------------------------------------------------------- scheduler

CoroutineScheduler::CoroutineScheduler(IExecutor* background)
    : m_thread(std::this_thread::get_id()), m_inbox(std::make_shared<detail::Inbox>()), m_background(background) {}

CoroutineScheduler::~CoroutineScheduler() {
    shutdown();
    m_ownedPool.reset();
}

void CoroutineScheduler::bindToCurrentThread() {
    OX_ASSERT(m_records.empty(), "bindToCurrentThread() with live coroutines");
    m_thread = std::this_thread::get_id();
}

IExecutor& CoroutineScheduler::backgroundExecutor() {
    std::lock_guard lock(m_backgroundMutex);
    if (!m_background) {
        const u32 hw = std::thread::hardware_concurrency();
        m_ownedPool = std::make_unique<ThreadPoolExecutor>(std::clamp(hw > 1 ? hw - 1 : 1u, 1u, 4u));
        m_background = m_ownedPool.get();
    }
    return *m_background;
}

void CoroutineScheduler::setBackgroundExecutor(IExecutor* executor) {
    std::lock_guard lock(m_backgroundMutex);
    m_background = executor;
}

void CoroutineScheduler::post(std::function<void()> fn) {
    m_inbox->push(detail::InboxItem{detail::InboxItem::Kind::Call, nullptr, {}, 0, std::move(fn)});
}

CoroutineHandle CoroutineScheduler::spawn(Task<void> task, SpawnOptions options) {
    OX_ASSERT(isSchedulerThread(), "spawn() must be called on the scheduler thread");
    OX_ASSERT(task.valid(), "spawn() of an empty Task");

    auto rec = std::allocate_shared<detail::TaskRecord>(detail::PoolAllocator<detail::TaskRecord>{});
    rec->self = rec;
    rec->id = ++m_nextId;
    rec->name = std::move(options.name);
    rec->owner = options.owner;
    rec->scheduler = this;
    rec->inbox = m_inbox;
    rec->ctx.record = rec.get();
    rec->startGameTime = m_gameTime;
    rec->startRealTime = m_realTime;
    rec->startTick = m_tick;

    detail::RootTask root = detail::makeRoot(std::move(task));
    root.handle.promise().ctx = &rec->ctx;
    rec->rootFrame = root.handle;
    rec->rootPromise = &root.handle.promise();

    if (const auto& parent = options.parent.m_record;
        parent && parent->status.load() == CoroutineStatus::Running && parent->scheduler == this) {
        parent->children.push_back(rec);
        rec->parentId = parent->id;
        if (rec->owner == 0) {
            rec->owner = parent->owner;
        }
    }

    rec->index = m_records.size();
    m_records.push_back(rec);
    if (rec->owner != 0) {
        m_byOwner[rec->owner].push_back(rec.get());
    }

    CoroutineHandle handle{rec};
    if (options.token.canBeCancelled()) {
        if (options.token.isCancelled()) {
            finishRecord(*rec, true);
            return handle;
        }
        std::weak_ptr<detail::TaskRecord> weak = rec;
        rec->tokenRegistration = options.token.onCancel([weak] { CoroutineHandle{weak.lock()}.cancel(); });
    }

    if (options.deferStart) {
        m_deferred.push_back(rec);
    } else {
        start(*rec);
    }
    return handle;
}

void CoroutineScheduler::start(detail::TaskRecord& record) {
    if (record.started || record.status.load() != CoroutineStatus::Running) {
        return;
    }
    record.started = true;
    resumeOnMain(&record.ctx, record.rootFrame);
}

void CoroutineScheduler::tick(f64 dt, u64 frameIndex) {
    OX_ASSERT(isSchedulerThread(), "tick() must be called on the scheduler thread");
    if (dt < 0.0 || dt != dt) {
        dt = 0.0;
    }
    ++m_tick;
    m_frame = frameIndex == kAutoFrame ? m_frame + 1 : frameIndex;
    m_realTime += dt;
    if (!m_paused) {
        m_gameTime += dt * m_timeScale;
    }

    drainInbox();

    if (!m_deferred.empty()) {
        auto deferred = std::move(m_deferred);
        m_deferred.clear();
        for (auto& r : deferred) {
            start(*r);
        }
    }

    std::vector<detail::WaitNode*> ready;
    collectReady(m_waits, ready);
    resumeReady(ready);
}

void CoroutineScheduler::fixedTick(f64 fixedDt) {
    OX_ASSERT(isSchedulerThread(), "fixedTick() must be called on the scheduler thread");
    ++m_fixedStep;
    m_fixedTime += fixedDt;
    std::vector<detail::WaitNode*> ready;
    collectReady(m_fixedWaits, ready);
    resumeReady(ready);
}

bool CoroutineScheduler::isReady(const detail::WaitNode& n) {
    switch (n.kind) {
    case WaitKind::Frames: return m_tick >= n.wakeTick;
    // Small tolerance: ten ticks of 0.1 s must complete seconds(1.0) despite accumulated rounding.
    case WaitKind::GameTime: return m_tick > n.wakeTick && m_gameTime + 1e-9 >= n.wakeTime;
    case WaitKind::RealTime: return m_tick > n.wakeTick && m_realTime + 1e-9 >= n.wakeTime;
    case WaitKind::Until: return n.predicate();
    case WaitKind::While: return !n.predicate();
    case WaitKind::FixedUpdate: return m_fixedStep > n.wakeTick;
    default: return false;
    }
}

void CoroutineScheduler::registerWait(detail::WaitNode& node, detail::AsyncContext* ctx, std::coroutine_handle<> h) {
    node.ctx = ctx;
    node.handle = h;
    node.scheduler = this;
    switch (node.kind) {
    case WaitKind::Frames: node.wakeTick = m_tick + static_cast<u64>(node.param); break;
    case WaitKind::GameTime: node.wakeTick = m_tick; node.wakeTime = m_gameTime + node.param; break;
    case WaitKind::RealTime: node.wakeTick = m_tick; node.wakeTime = m_realTime + node.param; break;
    case WaitKind::FixedUpdate: node.wakeTick = m_fixedStep; break;
    default: break;
    }
    std::vector<detail::WaitNode*>& list = node.kind == WaitKind::FixedUpdate ? m_fixedWaits : m_waits;
    node.list = &list;
    node.index = list.size();
    list.push_back(&node);
}

void CoroutineScheduler::unlinkWait(detail::WaitNode& node) noexcept {
    if (node.list) {
        (*node.list)[node.index] = nullptr;
        node.list = nullptr;
    }
}

void CoroutineScheduler::collectReady(std::vector<detail::WaitNode*>& from, std::vector<detail::WaitNode*>& ready) {
    // Index-based: predicates may spawn coroutines (push_back) or cancel others (null slots).
    for (usize i = 0; i < from.size(); ++i) {
        detail::WaitNode* n = from[i];
        if (n && isReady(*n)) {
            from[i] = nullptr;
            n->list = &ready;
            n->index = ready.size();
            ready.push_back(n);
        }
    }
    usize w = 0;
    for (usize r = 0; r < from.size(); ++r) {
        if (from[r]) {
            from[w] = from[r];
            from[w]->index = w;
            ++w;
        }
    }
    from.resize(w);
}

void CoroutineScheduler::resumeReady(std::vector<detail::WaitNode*>& ready) {
    for (usize i = 0; i < ready.size(); ++i) {
        detail::WaitNode* n = ready[i];
        if (!n) {
            continue; // frame destroyed by an earlier resume (e.g. whenAny loser)
        }
        ready[i] = nullptr;
        n->list = nullptr;
        resumeOnMain(n->ctx, n->handle);
    }
}

void CoroutineScheduler::resumeOnMain(detail::AsyncContext* ctx, std::coroutine_handle<> h) {
    detail::TaskRecord& r = *ctx->record;
    if (r.status.load(std::memory_order_relaxed) != CoroutineStatus::Running || r.destroying) {
        return;
    }
    if (ctx->chainCancelled()) {
        handleCancelledArrival(ctx);
        return;
    }
    std::shared_ptr<detail::TaskRecord> keep = r.self.lock();
    ++r.running;
    h.resume();
    --r.running;
    afterResume(r);
}

void CoroutineScheduler::afterResume(detail::TaskRecord& r) {
    if (r.running > 0 || r.destroying || r.status.load(std::memory_order_relaxed) != CoroutineStatus::Running) {
        return;
    }
    if (r.ctx.offCount.load(std::memory_order_acquire) != 0) {
        return;
    }
    if (r.finished.load(std::memory_order_acquire)) {
        finishRecord(r, false);
    } else if (r.ctx.cancelRequested.load(std::memory_order_acquire)) {
        finishRecord(r, true);
    }
}

void CoroutineScheduler::handleCancelledArrival(detail::AsyncContext* ctx) {
    detail::AsyncContext* top = nullptr;
    for (detail::AsyncContext* c = ctx; c; c = c->parent) {
        if (c->cancelRequested.load(std::memory_order_acquire)) {
            top = c;
        }
    }
    detail::TaskRecord& r = *ctx->record;
    if (!top || top == &r.ctx || !top->when) {
        afterResume(r);
        return;
    }
    if (top->offCount.load(std::memory_order_acquire) != 0) {
        return; // other parts of the cancelled branch are still on a worker
    }
    std::shared_ptr<detail::TaskRecord> keep = r.self.lock();
    if (std::coroutine_handle<> parent = top->when->loserParked(top->index)) {
        resumeOnMain(top->when->parentContext(), parent);
    } else {
        afterResume(r);
    }
}

void CoroutineScheduler::drainInbox() {
    std::vector<detail::InboxItem> items = m_inbox->take();
    using Kind = detail::InboxItem::Kind;
    for (detail::InboxItem& item : items) {
        switch (item.kind) {
        case Kind::Call:
            if (item.fn) {
                item.fn();
            }
            break;
        case Kind::Signal: {
            auto it = m_remote.find(item.id);
            if (it == m_remote.end()) {
                break; // awaiter gone (cancelled / timed out)
            }
            RemoteEntry e = it->second;
            m_remote.erase(it);
            resumeOnMain(e.ctx, e.handle);
            break;
        }
        case Kind::Resume:
            item.ctx->markOn();
            resumeOnMain(item.ctx, item.handle);
            break;
        case Kind::RootDone:
            item.ctx->markOn();
            afterResume(*item.ctx->record);
            break;
        case Kind::DriverDone: {
            detail::AsyncContext* ctx = item.ctx;
            ctx->markOn();
            if (ctx->chainCancelled()) {
                handleCancelledArrival(ctx);
                break;
            }
            detail::TaskRecord& r = *ctx->record;
            std::shared_ptr<detail::TaskRecord> keep = r.self.lock();
            if (std::coroutine_handle<> parent = ctx->when->driverDone(ctx->index)) {
                resumeOnMain(ctx->when->parentContext(), parent);
            } else {
                afterResume(r);
            }
            break;
        }
        }
    }
}

void CoroutineScheduler::requestCancel(detail::TaskRecord& r) {
    if (r.destroying || r.status.load() != CoroutineStatus::Running) {
        return;
    }
    r.ctx.cancelRequested.store(true, std::memory_order_release);
    auto children = r.children;
    for (auto& weak : children) {
        if (auto child = weak.lock()) {
            requestCancel(*child);
        }
    }
    if (!r.started) {
        std::erase_if(m_deferred, [&r](const auto& p) { return p.get() == &r; });
        finishRecord(r, true);
        return;
    }
    afterResume(r);
}

void CoroutineScheduler::finishRecord(detail::TaskRecord& r, bool cancelled) {
    std::shared_ptr<detail::TaskRecord> keep = r.self.lock();
    r.destroying = true;
    CoroutineStatus status = cancelled ? CoroutineStatus::Cancelled : CoroutineStatus::Completed;
    if (!cancelled && r.rootPromise && r.rootPromise->exception) {
        status = CoroutineStatus::Failed;
        r.error = detail::exceptionMessage(r.rootPromise->exception);
        OX_LOG_ERROR(kLog, "coroutine #{} '{}' failed: {}", r.id, r.name, r.error);
    }
    // Status first: destructors running during the unwind may query handles.
    r.status.store(status, std::memory_order_release);
    r.tokenRegistration.reset();
    if (std::coroutine_handle<> frame = std::exchange(r.rootFrame, {})) {
        r.rootPromise = nullptr;
        frame.destroy(); // unwinds every awaited task, whenAll branch and awaiter (RAII runs)
    }
    removeRecord(r);
}

void CoroutineScheduler::removeRecord(detail::TaskRecord& r) {
    if (r.owner != 0) {
        if (auto it = m_byOwner.find(r.owner); it != m_byOwner.end()) {
            std::erase(it->second, &r);
            if (it->second.empty()) {
                m_byOwner.erase(it);
            }
        }
    }
    if (r.index < m_records.size() && m_records[r.index].get() == &r) {
        const usize i = r.index;
        if (i + 1 != m_records.size()) {
            m_records[i] = std::move(m_records.back());
            m_records[i]->index = i;
        }
        m_records.pop_back();
    }
    r.scheduler = nullptr;
}

usize CoroutineScheduler::cancelOwner(u64 owner) {
    OX_ASSERT(isSchedulerThread(), "cancelOwner() must be called on the scheduler thread");
    if (owner == 0) {
        return 0;
    }
    auto it = m_byOwner.find(owner);
    if (it == m_byOwner.end()) {
        return 0;
    }
    std::vector<std::shared_ptr<detail::TaskRecord>> victims;
    for (detail::TaskRecord* r : it->second) {
        victims.push_back(r->self.lock());
    }
    for (auto& r : victims) {
        requestCancel(*r);
    }
    return victims.size();
}

void CoroutineScheduler::cancelAll() {
    OX_ASSERT(isSchedulerThread(), "cancelAll() must be called on the scheduler thread");
    auto victims = m_records;
    for (auto& r : victims) {
        requestCancel(*r);
    }
}

std::vector<CoroutineHandle> CoroutineScheduler::handlesForOwner(u64 owner) const {
    std::vector<CoroutineHandle> out;
    if (auto it = m_byOwner.find(owner); it != m_byOwner.end()) {
        for (detail::TaskRecord* r : it->second) {
            out.emplace_back(r->self.lock());
        }
    }
    return out;
}

std::string CoroutineScheduler::describe(const detail::WaitNode& n) const {
    switch (n.kind) {
    case WaitKind::Frames: return std::format("frames({} left)", n.wakeTick > m_tick ? n.wakeTick - m_tick : 0);
    case WaitKind::GameTime: return std::format("seconds({:.2f} left)", std::max(0.0, n.wakeTime - m_gameTime));
    case WaitKind::RealTime: return std::format("realSeconds({:.2f} left)", std::max(0.0, n.wakeTime - m_realTime));
    default: return std::string(toString(n.kind));
    }
}

std::vector<CoroutineInfo> CoroutineScheduler::coroutines() const {
    std::unordered_map<const detail::TaskRecord*, std::string> waits;
    auto add = [&waits](const detail::TaskRecord* r, std::string text) {
        std::string& s = waits[r];
        if (!s.empty()) {
            s += ", ";
        }
        s += text;
    };
    for (const auto* list : {&m_waits, &m_fixedWaits}) {
        for (const detail::WaitNode* n : *list) {
            if (n) {
                add(n->ctx->record, describe(*n));
            }
        }
    }
    for (const auto& [id, e] : m_remote) {
        add(e.ctx->record, std::string(toString(e.kind)));
    }

    std::vector<CoroutineInfo> out;
    out.reserve(m_records.size());
    for (const auto& r : m_records) {
        CoroutineInfo info;
        info.id = r->id;
        info.name = r->name;
        info.owner = r->owner;
        info.parentId = r->parentId;
        if (!r->started) {
            info.state = CoroutineState::Pending;
        } else if (r->ctx.cancelRequested.load()) {
            info.state = CoroutineState::CancelPending;
        } else if (r->ctx.offCount.load() != 0) {
            info.state = CoroutineState::OffThread;
        } else if (r->running > 0) {
            info.state = CoroutineState::Running;
        } else {
            info.state = CoroutineState::Suspended;
        }
        if (auto it = waits.find(r.get()); it != waits.end()) {
            info.waitingOn = it->second;
        } else if (r->ctx.offCount.load() != 0) {
            info.waitingOn = "background";
        }
        info.ageSeconds = m_gameTime - r->startGameTime;
        info.ageRealSeconds = m_realTime - r->startRealTime;
        info.ageFrames = m_tick - r->startTick;
        out.push_back(std::move(info));
    }
    return out;
}

void CoroutineScheduler::shutdown() {
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
    drainInbox();
    if (!m_records.empty()) {
        OX_LOG_WARN(kLog, "CoroutineScheduler shutdown: {} coroutine(s) still running (cancelling):", m_records.size());
        for (const CoroutineInfo& c : coroutines()) {
            OX_LOG_WARN(kLog, "  #{} '{}' owner={} state={} waiting on [{}] age {:.2f}s / {} frames", c.id,
                        c.name.empty() ? "<unnamed>" : c.name, c.owner, toString(c.state), c.waitingOn, c.ageSeconds,
                        c.ageFrames);
        }
        cancelAll();
        // Parts running on background threads must come back before their frames can be destroyed.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!m_records.empty() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            drainInbox();
        }
        if (!m_records.empty()) {
            OX_LOG_ERROR(kLog, "CoroutineScheduler shutdown: {} coroutine(s) stuck on background threads; leaking them",
                         m_records.size());
            for (auto& r : m_records) {
                r->scheduler = nullptr;
                r->rootFrame = {}; // cannot be destroyed safely
            }
            m_records.clear();
        }
    }
    m_deferred.clear();
    m_byOwner.clear();
}

} // namespace ox
