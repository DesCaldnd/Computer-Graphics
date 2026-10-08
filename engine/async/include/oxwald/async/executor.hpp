#pragma once

// Pluggable executors. The runtime wires the engine JobSystem in through JobSystemExecutor; ThreadPoolExecutor is
// the standalone default (tests, tools). CoroutineScheduler is itself an IExecutor for the main/game thread.

#include <oxwald/core/types.hpp>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ox {

class JobSystem;

class IExecutor {
public:
    virtual ~IExecutor() = default;
    // Thread-safe. Runs fn at some later point on one of the executor's threads.
    virtual void post(std::function<void()> fn) = 0;
};

// Runs work immediately on the posting thread (tests, trivial continuations).
class InlineExecutor final : public IExecutor {
public:
    void post(std::function<void()> fn) override { fn(); }
};

// Fixed-size std::thread pool, FIFO. The destructor finishes queued work, then joins.
class ThreadPoolExecutor final : public IExecutor {
public:
    explicit ThreadPoolExecutor(u32 threads = 0); // 0 = max(1, hardware_concurrency - 1)
    ~ThreadPoolExecutor() override;
    ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
    ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;

    void post(std::function<void()> fn) override;
    [[nodiscard]] u32 threadCount() const { return static_cast<u32>(m_threads.size()); }
    // True when called from one of this pool's threads.
    [[nodiscard]] bool isWorkerThread() const;

private:
    void run();

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    std::vector<std::thread> m_threads;
    bool m_stop = false;
};

// Adapter: posts to ox::JobSystem::submit (enkiTS workers). The JobSystem must outlive the executor and all work
// posted to it.
class JobSystemExecutor final : public IExecutor {
public:
    explicit JobSystemExecutor(JobSystem& jobs) : m_jobs(jobs) {}
    void post(std::function<void()> fn) override;
    [[nodiscard]] JobSystem& jobs() const { return m_jobs; }

private:
    JobSystem& m_jobs;
};

} // namespace ox
