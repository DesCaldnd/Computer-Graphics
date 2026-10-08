#pragma once

// Cooperative cancellation (thread-safe).
//
//   ox::CancellationSource src;
//   scheduler.spawn(work(), {.token = src.token()});
//   auto reg = src.token().onCancel([] { ... });   // runs on the cancelling thread (or immediately if cancelled)
//   src.cancel();
//
// A source can be linked to a parent token: cancelling the parent cancels the child.

#include <oxwald/core/types.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ox {

namespace detail {
struct CancelState {
    std::mutex mutex;
    std::atomic<bool> cancelled{false};
    u64 nextId = 0;
    std::vector<std::pair<u64, std::function<void()>>> callbacks;

    void cancel();
    u64 add(std::function<void()> fn); // 0 = already cancelled (fn was not stored)
    void remove(u64 id);
};
} // namespace detail

// Unregisters its callback on destruction (move-only).
class CancellationRegistration {
public:
    CancellationRegistration() = default;
    CancellationRegistration(std::weak_ptr<detail::CancelState> state, u64 id) : m_state(std::move(state)), m_id(id) {}
    CancellationRegistration(CancellationRegistration&& o) noexcept
        : m_state(std::move(o.m_state)), m_id(std::exchange(o.m_id, 0)) {}
    CancellationRegistration& operator=(CancellationRegistration&& o) noexcept {
        if (this != &o) {
            reset();
            m_state = std::move(o.m_state);
            m_id = std::exchange(o.m_id, 0);
        }
        return *this;
    }
    CancellationRegistration(const CancellationRegistration&) = delete;
    CancellationRegistration& operator=(const CancellationRegistration&) = delete;
    ~CancellationRegistration() { reset(); }

    void reset() {
        if (m_id != 0) {
            if (auto s = m_state.lock()) {
                s->remove(m_id);
            }
            m_id = 0;
        }
        m_state.reset();
    }
    [[nodiscard]] bool active() const { return m_id != 0; }

private:
    std::weak_ptr<detail::CancelState> m_state;
    u64 m_id = 0;
};

class CancellationToken {
public:
    CancellationToken() = default; // never cancelled
    explicit CancellationToken(std::shared_ptr<detail::CancelState> state) : m_state(std::move(state)) {}

    [[nodiscard]] bool canBeCancelled() const { return m_state != nullptr; }
    [[nodiscard]] bool isCancelled() const { return m_state && m_state->cancelled.load(std::memory_order_acquire); }
    explicit operator bool() const { return canBeCancelled(); }

    // fn runs exactly once: on the thread calling cancel(), or immediately (inline) if already cancelled.
    [[nodiscard]] CancellationRegistration onCancel(std::function<void()> fn) const {
        if (!m_state) {
            return {};
        }
        const u64 id = m_state->add(fn);
        if (id == 0) {
            fn();
            return {};
        }
        return CancellationRegistration{m_state, id};
    }

private:
    std::shared_ptr<detail::CancelState> m_state;
};

class CancellationSource {
public:
    CancellationSource() : m_state(std::make_shared<detail::CancelState>()) {}
    // Linked: cancelled when `parent` is cancelled (or immediately if it already is).
    explicit CancellationSource(const CancellationToken& parent) : CancellationSource() {
        std::weak_ptr<detail::CancelState> weak = m_state;
        m_link = parent.onCancel([weak] {
            if (auto s = weak.lock()) {
                s->cancel();
            }
        });
    }

    [[nodiscard]] CancellationToken token() const { return CancellationToken{m_state}; }
    void cancel() { m_state->cancel(); }
    [[nodiscard]] bool isCancelled() const { return m_state->cancelled.load(std::memory_order_acquire); }

private:
    std::shared_ptr<detail::CancelState> m_state;
    CancellationRegistration m_link;
};

} // namespace ox
