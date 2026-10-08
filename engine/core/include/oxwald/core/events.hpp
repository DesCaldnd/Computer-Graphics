#pragma once

// Signals/slots and a typed event bus.
//
// Signal: connect/disconnect/emit are thread-safe. emit() calls slots without holding a lock, on the
// emitting thread, in connection order. Slots connected during an emit are not called by that emit;
// slots disconnected during an emit (by any slot) are not called afterwards. Disconnecting does not
// wait for a slot that is currently running on another thread.

#include <oxwald/core/types.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ox {

namespace detail {

struct SignalCore;

struct SlotBase {
    std::atomic<bool> connected{true};
    std::weak_ptr<SignalCore> core;
    virtual ~SlotBase() = default;
};

struct SignalCore {
    std::mutex mutex;
    std::vector<std::shared_ptr<SlotBase>> slots;

    void remove(const SlotBase* slot) {
        std::lock_guard lock(mutex);
        std::erase_if(slots, [slot](const auto& s) { return s.get() == slot; });
    }
};

} // namespace detail

class Connection {
public:
    Connection() = default;

    void disconnect() {
        if (auto slot = m_slot.lock()) {
            slot->connected.store(false, std::memory_order_release);
            if (auto core = slot->core.lock()) {
                core->remove(slot.get());
            }
        }
        m_slot.reset();
    }
    [[nodiscard]] bool connected() const {
        auto slot = m_slot.lock();
        return slot && slot->connected.load(std::memory_order_acquire) && !slot->core.expired();
    }

private:
    template <class...>
    friend class Signal;
    explicit Connection(std::weak_ptr<detail::SlotBase> slot) : m_slot(std::move(slot)) {}
    std::weak_ptr<detail::SlotBase> m_slot;
};

// Disconnects on destruction. Movable, not copyable.
class ScopedConnection {
public:
    ScopedConnection() = default;
    ScopedConnection(Connection c) : m_connection(std::move(c)) {} // NOLINT: implicit by design
    ~ScopedConnection() { m_connection.disconnect(); }
    ScopedConnection(ScopedConnection&& o) noexcept : m_connection(std::exchange(o.m_connection, {})) {}
    ScopedConnection& operator=(ScopedConnection&& o) noexcept {
        if (this != &o) {
            m_connection.disconnect();
            m_connection = std::exchange(o.m_connection, {});
        }
        return *this;
    }
    ScopedConnection(const ScopedConnection&) = delete;
    ScopedConnection& operator=(const ScopedConnection&) = delete;

    void disconnect() { m_connection.disconnect(); }
    [[nodiscard]] bool connected() const { return m_connection.connected(); }
    // Gives up ownership without disconnecting.
    Connection release() { return std::exchange(m_connection, {}); }

private:
    Connection m_connection;
};

template <class... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;

    Signal() = default;
    ~Signal() = default;
    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;
    // Existing connections follow the moved signal; the moved-from signal is empty and reusable.
    Signal(Signal&& o) noexcept : m_core(std::exchange(o.m_core, std::make_shared<detail::SignalCore>())) {}
    Signal& operator=(Signal&& o) noexcept {
        if (this != &o) {
            disconnectAll();
            m_core = std::exchange(o.m_core, std::make_shared<detail::SignalCore>());
        }
        return *this;
    }

    Connection connect(Slot fn) {
        auto slot = std::make_shared<SlotImpl>();
        slot->fn = std::move(fn);
        slot->core = m_core;
        std::lock_guard lock(m_core->mutex);
        m_core->slots.push_back(slot);
        return Connection{slot};
    }

    // Arguments are passed as lvalues to every slot, so slots cannot steal moved-in values.
    void emit(Args... args) const {
        std::vector<std::shared_ptr<detail::SlotBase>> snapshot;
        {
            std::lock_guard lock(m_core->mutex);
            snapshot = m_core->slots;
        }
        for (const auto& base : snapshot) {
            if (base->connected.load(std::memory_order_acquire)) {
                static_cast<SlotImpl&>(*base).fn(args...);
            }
        }
    }
    void operator()(Args... args) const { emit(std::forward<Args>(args)...); }

    [[nodiscard]] usize slotCount() const {
        std::lock_guard lock(m_core->mutex);
        return m_core->slots.size();
    }
    [[nodiscard]] bool empty() const { return slotCount() == 0; }

    void disconnectAll() {
        std::vector<std::shared_ptr<detail::SlotBase>> slots;
        {
            std::lock_guard lock(m_core->mutex);
            slots.swap(m_core->slots);
        }
        for (const auto& s : slots) {
            s->connected.store(false, std::memory_order_release);
        }
    }

private:
    struct SlotImpl final : detail::SlotBase {
        Slot fn;
    };
    std::shared_ptr<detail::SignalCore> m_core = std::make_shared<detail::SignalCore>();
};

// Typed publish/subscribe. subscribe/publish/enqueue/dispatch are thread-safe. publish() delivers
// immediately on the calling thread; enqueue() stores a copy and dispatch() (usually once per frame
// on the main thread) delivers queued events in FIFO order across all types. Events enqueued while
// dispatch() runs are delivered by the next dispatch().
class EventBus {
public:
    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    template <class E>
    Connection subscribe(std::function<void(const E&)> fn) {
        return channel<E>().signal.connect(std::move(fn));
    }

    template <class E>
    void publish(const E& event) {
        channel<E>().signal.emit(event);
    }

    template <class E>
    void enqueue(E event) {
        auto item = std::make_unique<Queued<std::decay_t<E>>>(std::move(event));
        std::lock_guard lock(m_queueMutex);
        m_queue.push_back(std::move(item));
    }

    // Returns the number of delivered events.
    usize dispatch() {
        std::vector<std::unique_ptr<QueuedBase>> batch;
        {
            std::lock_guard lock(m_queueMutex);
            batch.swap(m_queue);
        }
        for (auto& item : batch) {
            item->deliver(*this);
        }
        return batch.size();
    }

    [[nodiscard]] usize pendingCount() const {
        std::lock_guard lock(m_queueMutex);
        return m_queue.size();
    }
    void clearQueue() {
        std::lock_guard lock(m_queueMutex);
        m_queue.clear();
    }
    template <class E>
    [[nodiscard]] usize subscriberCount() {
        return channel<E>().signal.slotCount();
    }

private:
    struct ChannelBase {
        virtual ~ChannelBase() = default;
    };
    template <class E>
    struct Channel final : ChannelBase {
        Signal<const E&> signal;
    };
    struct QueuedBase {
        virtual ~QueuedBase() = default;
        virtual void deliver(EventBus& bus) = 0;
    };
    template <class E>
    struct Queued final : QueuedBase {
        explicit Queued(E e) : event(std::move(e)) {}
        void deliver(EventBus& bus) override { bus.publish<E>(event); }
        E event;
    };

    template <class E>
    Channel<E>& channel() {
        std::lock_guard lock(m_channelMutex);
        auto& slot = m_channels[std::type_index(typeid(E))];
        if (!slot) {
            slot = std::make_unique<Channel<E>>();
        }
        return static_cast<Channel<E>&>(*slot);
    }

    std::mutex m_channelMutex;
    std::unordered_map<std::type_index, std::unique_ptr<ChannelBase>> m_channels;
    mutable std::mutex m_queueMutex;
    std::vector<std::unique_ptr<QueuedBase>> m_queue;
};

} // namespace ox
