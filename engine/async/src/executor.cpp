#include <oxwald/async/cancellation.hpp>
#include <oxwald/async/executor.hpp>
#include <oxwald/core/jobs.hpp>

#include <algorithm>

namespace ox {

// ---------------------------------------------------------------- cancellation

namespace detail {

void CancelState::cancel() {
    std::vector<std::pair<u64, std::function<void()>>> callbacks;
    {
        std::lock_guard lock(mutex);
        if (cancelled.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        callbacks.swap(this->callbacks);
    }
    for (auto& [id, fn] : callbacks) {
        fn();
    }
}

u64 CancelState::add(std::function<void()> fn) {
    std::lock_guard lock(mutex);
    if (cancelled.load(std::memory_order_acquire)) {
        return 0;
    }
    const u64 id = ++nextId;
    callbacks.emplace_back(id, std::move(fn));
    return id;
}

void CancelState::remove(u64 id) {
    std::lock_guard lock(mutex);
    std::erase_if(callbacks, [id](const auto& c) { return c.first == id; });
}

} // namespace detail

// ---------------------------------------------------------------- executors

namespace {
thread_local const ThreadPoolExecutor* t_currentPool = nullptr;
}

ThreadPoolExecutor::ThreadPoolExecutor(u32 threads) {
    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u);
    }
    m_threads.reserve(threads);
    for (u32 i = 0; i < threads; ++i) {
        m_threads.emplace_back([this] { run(); });
    }
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    for (auto& t : m_threads) {
        t.join();
    }
}

void ThreadPoolExecutor::post(std::function<void()> fn) {
    {
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(fn));
    }
    m_cv.notify_one();
}

bool ThreadPoolExecutor::isWorkerThread() const { return t_currentPool == this; }

void ThreadPoolExecutor::run() {
    t_currentPool = this;
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_queue.empty()) {
                return; // stopping and drained
            }
            fn = std::move(m_queue.front());
            m_queue.pop_front();
        }
        fn();
    }
}

void JobSystemExecutor::post(std::function<void()> fn) { m_jobs.submit(std::move(fn)); }

} // namespace ox
