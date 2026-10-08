#pragma once

// Job system on top of enkiTS (kept out of this header).
//
// Threading rules:
//  * The thread that constructs the JobSystem is "thread 0" (main thread). Worker threads are 1..N-1.
//  * submit()/parallelFor*/wait() may be called from the main thread, from inside jobs, and from
//    any other thread. Calls from non-job threads are routed through a worker; waiting from such a
//    thread blocks (yields) instead of helping with work.
//  * Waiting from inside a job never deadlocks: the waiting thread executes other jobs meanwhile.
//  * waitAll() and destruction must happen on the main thread.

#include <oxwald/core/types.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ox {

class JobSystem;

namespace detail {
struct JobTask; // owns the enki task; defined in jobs.cpp
} // namespace detail

// Shared, ref-counted reference to a submitted job. The underlying task object is also kept alive by
// the JobSystem until it completes, so dropping a handle early is safe.
class JobHandle {
public:
    JobHandle() = default;

    [[nodiscard]] bool valid() const { return m_task != nullptr; }
    // An invalid (default) handle counts as done.
    [[nodiscard]] bool done() const;

private:
    friend class JobSystem;
    explicit JobHandle(std::shared_ptr<detail::JobTask> task) : m_task(std::move(task)) {}
    std::shared_ptr<detail::JobTask> m_task;
};

class JobSystem {
public:
    using Job = std::function<void()>;
    // begin/end are a half-open index range, thread is the executing thread index.
    using RangeJob = std::function<void(u32 begin, u32 end, u32 thread)>;

    static constexpr u32 kInvalidThread = 0xffffffffu;

    // numThreads = total thread count including the calling (main) thread; 0 = hardware threads.
    explicit JobSystem(u32 numThreads = 0);
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    [[nodiscard]] u32 threadCount() const;

    JobHandle submit(Job fn);
    // Splits [0,count) into ranges of at least `grain` indices; every index is visited exactly once.
    void parallelFor(u32 count, u32 grain, const RangeJob& fn);
    JobHandle parallelForAsync(u32 count, u32 grain, RangeJob fn);

    // Blocks until the job finished; the main thread and job threads help execute other jobs.
    void wait(const JobHandle& handle);
    // Waits for every job submitted so far. Main thread only.
    void waitAll();

    // Callbacks executed on the main thread when it calls runMainThreadQueue() (e.g. once per frame).
    // Thread-safe, callable from any thread including jobs.
    void enqueueMainThread(Job fn);
    // Runs the callbacks queued so far (callbacks queued while running are left for the next call).
    usize runMainThreadQueue();

    // Index of the calling thread in this process's job system: 0 for the main thread, 1..N-1 for
    // workers, kInvalidThread for unrelated threads.
    [[nodiscard]] static u32 currentThreadIndex();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    std::mutex m_mainQueueMutex;
    std::vector<Job> m_mainQueue;
};

// Fork/join helper: run() submits, wait() joins all jobs started through this group so far.
// run() and wait() are thread-safe; wait() may be called from inside a job.
class TaskGroup {
public:
    explicit TaskGroup(JobSystem& jobs) : m_jobs(jobs) {}
    ~TaskGroup() { wait(); }
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;

    void run(JobSystem::Job fn);
    void wait();
    [[nodiscard]] usize pending() const;

private:
    JobSystem& m_jobs;
    mutable std::mutex m_mutex;
    std::vector<JobHandle> m_handles;
};

} // namespace ox
