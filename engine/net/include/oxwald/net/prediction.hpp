#pragma once

#include <oxwald/core/types.hpp>

#include <deque>
#include <functional>
#include <utility>

namespace ox::net {

// Client-side prediction for an owned object.
//  - applyInput(): simulate locally right away, remember (seq, input, predicted state)
//  - send pending() inputs to the server every tick (redundantly, unreliable — the server dedupes by seq)
//  - reconcile(): when the server reports (lastProcessedInputSeq, authoritativeState), drop acked inputs; if the
//    authoritative state disagrees with what we predicted for that seq, rewind to it and replay unacked inputs.
// Simulate must be deterministic and identical on server and client.
template <class State, class Input>
class ClientPrediction {
public:
    using SimulateFn = std::function<void(State& state, const Input& input, f32 dt)>;
    using EqualFn = std::function<bool(const State& a, const State& b)>; // equal within tolerance

    struct Pending {
        u32 seq;
        Input input;
        f32 dt;
        State predicted; // state after applying this input
    };

    ClientPrediction(SimulateFn simulate, EqualFn equal, State initial = {}, usize maxPending = 256)
        : m_simulate(std::move(simulate)), m_equal(std::move(equal)), m_state(initial),
          m_ackedPredicted(std::move(initial)), m_maxPending(maxPending) {}

    u32 applyInput(const Input& input, f32 dt) {
        const u32 seq = ++m_lastSeq;
        m_simulate(m_state, input, dt);
        m_pending.push_back({seq, input, dt, m_state});
        while (m_pending.size() > m_maxPending) {
            m_pending.pop_front();
        }
        return seq;
    }

    // Returns true when a correction (rewind + replay) happened.
    bool reconcile(u32 lastProcessedSeq, const State& authoritative) {
        if (lastProcessedSeq < m_lastAcked) {
            return false; // stale server state
        }
        while (!m_pending.empty() && m_pending.front().seq <= lastProcessedSeq) {
            m_ackedPredicted = std::move(m_pending.front().predicted);
            m_ackedPredictedSeq = m_pending.front().seq;
            m_pending.pop_front();
        }
        m_lastAcked = lastProcessedSeq;
        if (m_ackedPredictedSeq == lastProcessedSeq && m_equal(m_ackedPredicted, authoritative)) {
            return false;
        }
        m_state = authoritative;
        for (Pending& p : m_pending) {
            m_simulate(m_state, p.input, p.dt);
            p.predicted = m_state;
        }
        m_ackedPredicted = authoritative;
        m_ackedPredictedSeq = lastProcessedSeq;
        ++m_corrections;
        return true;
    }

    const State& state() const { return m_state; }
    void reset(const State& state) {
        m_state = state;
        m_ackedPredicted = state;
        m_ackedPredictedSeq = m_lastSeq;
        m_lastAcked = m_lastSeq;
        m_pending.clear();
    }
    const std::deque<Pending>& pending() const { return m_pending; }
    u32 lastSequence() const { return m_lastSeq; }
    u32 lastAcked() const { return m_lastAcked; }
    u32 corrections() const { return m_corrections; }

private:
    SimulateFn m_simulate;
    EqualFn m_equal;
    State m_state;
    State m_ackedPredicted; // predicted (or corrected) state for m_ackedPredictedSeq
    u32 m_ackedPredictedSeq = 0;
    usize m_maxPending;
    std::deque<Pending> m_pending;
    u32 m_lastSeq = 0;
    u32 m_lastAcked = 0;
    u32 m_corrections = 0;
};

// Server-side per-client input queue: accepts redundant/out-of-order input batches, executes each seq once and
// in order, and tells the client which seq the authoritative state corresponds to.
template <class Input>
class ServerInputQueue {
public:
    explicit ServerInputQueue(usize maxQueued = 128) : m_maxQueued(maxQueued) {}

    void receive(u32 seq, const Input& input, f32 dt) {
        if (seq <= m_lastProcessed || m_queue.size() >= m_maxQueued) {
            return;
        }
        auto it = m_queue.begin();
        while (it != m_queue.end() && it->seq < seq) {
            ++it;
        }
        if (it != m_queue.end() && it->seq == seq) {
            return;
        }
        m_queue.insert(it, Entry{seq, input, dt});
    }

    // fn(const Input&, f32 dt). Returns how many inputs ran. Gaps are skipped (lost input never arrives in time).
    template <class F>
    u32 process(F&& fn, u32 maxInputs = ~0u) {
        u32 n = 0;
        while (!m_queue.empty() && n < maxInputs) {
            Entry e = std::move(m_queue.front());
            m_queue.pop_front();
            fn(e.input, e.dt);
            m_lastProcessed = e.seq;
            ++n;
        }
        return n;
    }

    u32 lastProcessed() const { return m_lastProcessed; }
    usize queued() const { return m_queue.size(); }

private:
    struct Entry {
        u32 seq;
        Input input;
        f32 dt;
    };
    usize m_maxQueued;
    std::deque<Entry> m_queue;
    u32 m_lastProcessed = 0;
};

} // namespace ox::net
