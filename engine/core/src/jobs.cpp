#include <oxwald/core/jobs.hpp>

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <enkiTS/TaskScheduler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

namespace ox {

namespace {

// Identifies which JobSystem (if any) the current thread belongs to. gtl_threadNum inside enkiTS is
// shared by every scheduler instance, so it cannot tell schedulers apart on its own.
thread_local const void* t_owner = nullptr;
thread_local u32 t_threadIndex = JobSystem::kInvalidThread;

void runGuarded(const auto& fn, auto&&... args) {
    try {
        fn(args...);
    } catch (const std::exception& e) {
        OX_LOG_ERROR("jobs", "Unhandled exception in job: {}", e.what());
    } catch (...) {
        OX_LOG_ERROR("jobs", "Unhandled non-standard exception in job");
    }
}

} // namespace

namespace detail {

struct JobTask {
    struct Set final : enki::ITaskSet {
        JobTask* owner = nullptr;
        void ExecuteRange(enki::TaskSetPartition range, uint32_t threadNum) override {
            t_owner = owner->system;
            t_threadIndex = threadNum;
            if (owner->single) {
                runGuarded(owner->single);
            } else {
                runGuarded(owner->range, range.start, range.end, threadNum);
            }
        }
    };
    // Forwards a submission from a thread enkiTS does not know about onto a worker thread.
    struct Forward final : enki::IPinnedTask {
        JobTask* owner = nullptr;
        enki::TaskScheduler* scheduler = nullptr;
        void Execute() override {
            t_owner = owner->system;
            t_threadIndex = threadNum;
            scheduler->AddTaskSetToPipe(&owner->set);
        }
    };

    const void* system = nullptr;
    JobSystem::Job single;
    JobSystem::RangeJob range;
    Set set;
    Forward forward;
    bool forwarded = false;

    [[nodiscard]] bool done() const {
        // Order matters: the forwarder completes only after the set was added to the pipe.
        if (forwarded && !forward.GetIsComplete()) {
            return false;
        }
        return set.GetIsComplete();
    }
};

} // namespace detail

bool JobHandle::done() const { return m_task == nullptr || m_task->done(); }

struct JobSystem::Impl {
    enki::TaskScheduler scheduler;
    u32 threadCount = 1;
    std::thread::id mainThread;

    std::mutex inflightMutex;
    std::vector<std::shared_ptr<detail::JobTask>> inflight;
    usize sweepThreshold = 64;

    bool onOwnThread() const { return t_owner == this; }

    void track(std::shared_ptr<detail::JobTask> task) {
        std::lock_guard lock(inflightMutex);
        if (inflight.size() >= sweepThreshold) {
            std::erase_if(inflight, [](const auto& t) { return t->done(); });
            sweepThreshold = std::max<usize>(64, inflight.size() * 2);
        }
        inflight.push_back(std::move(task));
    }

    JobHandle launch(std::shared_ptr<detail::JobTask> task) {
        task->system = this;
        task->set.owner = task.get();
        // Track only after the task is fully configured and handed to enkiTS: before that its
        // completion counters read as "complete" and a concurrent sweep would drop the keep-alive.
        // `task` itself keeps the object alive until then.
        if (onOwnThread()) {
            scheduler.AddTaskSetToPipe(&task->set);
        } else {
            task->forwarded = true;
            task->forward.owner = task.get();
            task->forward.scheduler = &scheduler;
            // Thread 1 is always a worker (we create at least one), so this never needs the main thread.
            task->forward.threadNum = 1;
            scheduler.AddPinnedTask(&task->forward);
        }
        track(task);
        return JobHandle{std::move(task)};
    }
};

JobSystem::JobSystem(u32 numThreads) : m_impl(std::make_unique<Impl>()) {
    u32 total = numThreads == 0 ? enki::GetNumHardwareThreads() : numThreads;
    total = std::max<u32>(total, 2); // enkiTS requires at least one worker thread
    enki::TaskSchedulerConfig config;
    config.numTaskThreadsToCreate = total - 1;
    config.numExternalTaskThreads = 0;
    m_impl->scheduler.Initialize(config);
    m_impl->threadCount = m_impl->scheduler.GetNumTaskThreads();
    m_impl->mainThread = std::this_thread::get_id();
    t_owner = m_impl.get();
    t_threadIndex = 0;
}

JobSystem::~JobSystem() {
    OX_ASSERT(std::this_thread::get_id() == m_impl->mainThread, "JobSystem must be destroyed on its main thread");
    m_impl->scheduler.WaitforAllAndShutdown();
    {
        std::lock_guard lock(m_impl->inflightMutex);
        m_impl->inflight.clear();
    }
    if (t_owner == m_impl.get()) {
        t_owner = nullptr;
        t_threadIndex = kInvalidThread;
    }
    if (!m_mainQueue.empty()) {
        OX_LOG_WARN("jobs", "JobSystem destroyed with {} unprocessed main-thread callbacks", m_mainQueue.size());
    }
}

u32 JobSystem::threadCount() const { return m_impl->threadCount; }

JobHandle JobSystem::submit(Job fn) {
    auto task = std::make_shared<detail::JobTask>();
    task->single = std::move(fn);
    task->set.m_SetSize = 1;
    task->set.m_MinRange = 1;
    return m_impl->launch(std::move(task));
}

JobHandle JobSystem::parallelForAsync(u32 count, u32 grain, RangeJob fn) {
    if (count == 0) {
        return {};
    }
    auto task = std::make_shared<detail::JobTask>();
    task->range = std::move(fn);
    task->set.m_SetSize = count;
    task->set.m_MinRange = std::max<u32>(grain, 1);
    return m_impl->launch(std::move(task));
}

void JobSystem::parallelFor(u32 count, u32 grain, const RangeJob& fn) {
    JobHandle h = parallelForAsync(count, grain, fn);
    wait(h);
}

void JobSystem::wait(const JobHandle& handle) {
    if (!handle.m_task) {
        return;
    }
    detail::JobTask& task = *handle.m_task;
    if (m_impl->onOwnThread()) {
        if (task.forwarded) {
            m_impl->scheduler.WaitforTask(&task.forward);
        }
        m_impl->scheduler.WaitforTask(&task.set);
        return;
    }
    // Foreign thread: cannot help with work, so back off politely.
    u32 spins = 0;
    while (!task.done()) {
        if (++spins < 64) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

void JobSystem::waitAll() {
    OX_ASSERT(std::this_thread::get_id() == m_impl->mainThread, "JobSystem::waitAll must be called on the main thread");
    // Jobs may submit more jobs (also via the forwarding path), so loop until nothing is in flight.
    for (;;) {
        m_impl->scheduler.WaitforAll();
        std::lock_guard lock(m_impl->inflightMutex);
        std::erase_if(m_impl->inflight, [](const auto& t) { return t->done(); });
        if (m_impl->inflight.empty()) {
            break;
        }
    }
}

void JobSystem::enqueueMainThread(Job fn) {
    std::lock_guard lock(m_mainQueueMutex);
    m_mainQueue.push_back(std::move(fn));
}

usize JobSystem::runMainThreadQueue() {
    std::vector<Job> batch;
    {
        std::lock_guard lock(m_mainQueueMutex);
        batch.swap(m_mainQueue);
    }
    for (Job& fn : batch) {
        runGuarded(fn);
    }
    return batch.size();
}

u32 JobSystem::currentThreadIndex() { return t_owner != nullptr ? t_threadIndex : kInvalidThread; }

// ---------------------------------------------------------------------------------------------

void TaskGroup::run(JobSystem::Job fn) {
    JobHandle h = m_jobs.submit(std::move(fn));
    std::lock_guard lock(m_mutex);
    std::erase_if(m_handles, [](const JobHandle& j) { return j.done(); });
    m_handles.push_back(std::move(h));
}

void TaskGroup::wait() {
    for (;;) {
        std::vector<JobHandle> handles;
        {
            std::lock_guard lock(m_mutex);
            handles.swap(m_handles);
        }
        if (handles.empty()) {
            return;
        }
        for (const JobHandle& h : handles) {
            m_jobs.wait(h);
        }
    }
}

usize TaskGroup::pending() const {
    std::lock_guard lock(m_mutex);
    return static_cast<usize>(std::count_if(m_handles.begin(), m_handles.end(), [](const JobHandle& h) { return !h.done(); }));
}

} // namespace ox
