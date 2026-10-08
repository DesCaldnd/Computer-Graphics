#include "inbox.hpp"

#include <oxwald/async/combinators.hpp>

namespace ox::detail {

std::coroutine_handle<> DriverPromise::Final::await_suspend(DriverHandle h) noexcept {
    AsyncContext* ctx = h.promise().ctx;
    if (ctx->record && ctx->offThread) {
        // Completion logic runs on the scheduler thread. Nothing may touch the frame after the push.
        ctx->record->inbox->push(InboxItem{InboxItem::Kind::DriverDone, ctx, {}, 0, {}});
        return std::noop_coroutine();
    }
    std::coroutine_handle<> next = ctx->when->driverDone(ctx->index);
    return next ? next : std::noop_coroutine();
}

WhenStateBase::WhenStateBase(usize count, bool any)
    : m_drivers(count), m_ctxs(std::make_unique<AsyncContext[]>(count)),
      m_states(std::make_unique<std::atomic<u8>[]>(count)), m_any(any) {
    for (usize i = 0; i < count; ++i) {
        m_states[i].store(kNotStarted, std::memory_order_relaxed);
    }
}

WhenStateBase::~WhenStateBase() {
    for (DriverHandle& h : m_drivers) {
        if (h) {
            std::exchange(h, {}).destroy();
        }
    }
}

bool WhenStateBase::start(std::coroutine_handle<> parent, AsyncContext* parentCtx) {
    m_parent = parent;
    m_parentCtx = parentCtx;
    const usize n = m_drivers.size();
    m_outstanding.store(static_cast<i64>(n) + 1, std::memory_order_release);
    TaskRecord* record = parentCtx ? parentCtx->record : nullptr;
    for (usize i = 0; i < n; ++i) {
        AsyncContext& c = m_ctxs[i];
        c.record = record;
        c.parent = parentCtx;
        c.when = this;
        c.index = static_cast<u32>(i);
        m_drivers[i].promise().ctx = &c;
    }
    // Started from a background thread: the branches inherit the "off-thread" token of the parent (counted
    // before the parent releases its own, so the task is never observed as fully on-thread meanwhile).
    const bool parentOff = record && parentCtx->offThread;
    if (parentOff) {
        for (usize i = 0; i < n; ++i) {
            m_ctxs[i].markOff();
        }
        parentCtx->markOn();
    }

    usize started = 0;
    for (; started < n; ++started) {
        if (m_any && m_winner.load(std::memory_order_acquire) >= 0) {
            break;
        }
        m_states[started].store(kRunning, std::memory_order_release);
        m_drivers[started].resume();
    }
    for (usize j = started; j < n; ++j) {
        if (parentOff) {
            m_ctxs[j].markOn();
        }
        m_outstanding.fetch_sub(1, std::memory_order_acq_rel); // never started: nothing to tear down
    }
    return !release();
}

std::coroutine_handle<> WhenStateBase::release() noexcept {
    if (m_outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        return m_parent;
    }
    return {};
}

void WhenStateBase::setError(std::exception_ptr e) {
    std::lock_guard lock(m_errorMutex);
    if (!m_error) {
        m_error = std::move(e);
    }
}

void WhenStateBase::rethrowIfError() {
    std::exception_ptr e;
    {
        std::lock_guard lock(m_errorMutex);
        e = m_error;
    }
    if (e) {
        std::rethrow_exception(e);
    }
}

std::coroutine_handle<> WhenStateBase::driverDone(u32 index) noexcept {
    m_states[index].store(kDone, std::memory_order_release);
    std::exception_ptr ex = m_drivers[index].promise().exception;
    if (m_any) {
        i64 expected = -1;
        if (m_winner.compare_exchange_strong(expected, index, std::memory_order_acq_rel)) {
            if (ex) {
                setError(ex);
            }
            cancelLosers(index);
        }
    } else if (ex) {
        setError(ex);
    }
    return release();
}

void WhenStateBase::cancelLosers(u32 winner) noexcept {
    const usize n = m_drivers.size();
    for (usize j = 0; j < n; ++j) {
        if (j != winner) {
            m_ctxs[j].cancelRequested.store(true, std::memory_order_release);
        }
    }
    if (!m_parentCtx || !m_parentCtx->record) {
        return; // no scheduler: losers cannot be destroyed safely, wait for them to finish
    }
    for (usize j = 0; j < n; ++j) {
        if (j == winner || m_states[j].load(std::memory_order_acquire) != kRunning) {
            continue;
        }
        if (m_ctxs[j].offCount.load(std::memory_order_acquire) != 0) {
            continue; // torn down when it comes back (loserParked)
        }
        m_states[j].store(kDestroyed, std::memory_order_release);
        std::exchange(m_drivers[j], {}).destroy();
        m_outstanding.fetch_sub(1, std::memory_order_acq_rel); // the winner still holds one, never reaches 0 here
    }
}

std::coroutine_handle<> WhenStateBase::loserParked(u32 index) noexcept {
    if (m_states[index].exchange(kDestroyed, std::memory_order_acq_rel) == kDestroyed) {
        return {};
    }
    if (DriverHandle h = std::exchange(m_drivers[index], {})) {
        h.destroy();
    }
    return release();
}

} // namespace ox::detail
