#include <oxwald/core/jobs.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace ox;

TEST(Jobs, ThreadCountAndMainThreadIndex) {
    JobSystem jobs(4);
    EXPECT_EQ(jobs.threadCount(), 4u);
    EXPECT_EQ(JobSystem::currentThreadIndex(), 0u);
    std::atomic<u32> workerIndex{JobSystem::kInvalidThread};
    auto h = jobs.submit([&] { workerIndex = JobSystem::currentThreadIndex(); });
    jobs.wait(h);
    EXPECT_LT(workerIndex.load(), 4u);

    u32 foreignIndex = 0;
    std::thread([&] { foreignIndex = JobSystem::currentThreadIndex(); }).join();
    EXPECT_EQ(foreignIndex, JobSystem::kInvalidThread);
}

TEST(Jobs, ManyTasksSum) {
    JobSystem jobs(4);
    std::atomic<u64> sum{0};
    std::vector<JobHandle> handles;
    for (u64 i = 1; i <= 2000; ++i) {
        handles.push_back(jobs.submit([&sum, i] { sum.fetch_add(i, std::memory_order_relaxed); }));
    }
    for (const auto& h : handles) {
        jobs.wait(h);
        EXPECT_TRUE(h.done());
    }
    EXPECT_EQ(sum.load(), 2000u * 2001u / 2u);
}

TEST(Jobs, DroppedHandlesStillRunAndWaitAll) {
    JobSystem jobs(3);
    std::atomic<int> count{0};
    for (int i = 0; i < 500; ++i) {
        (void)jobs.submit([&] { count.fetch_add(1); });
    }
    jobs.waitAll();
    EXPECT_EQ(count.load(), 500);
}

TEST(Jobs, ParallelForCoversEveryIndexOnce) {
    JobSystem jobs(4);
    for (u32 count : {1u, 7u, 100u, 1000u, 4099u}) {
        for (u32 grain : {0u, 1u, 3u, 64u, 5000u}) {
            std::vector<std::atomic<int>> hits(count);
            std::atomic<u32> badThread{0};
            jobs.parallelFor(count, grain, [&](u32 begin, u32 end, u32 thread) {
                if (thread >= jobs.threadCount()) {
                    badThread.fetch_add(1);
                }
                for (u32 i = begin; i < end; ++i) {
                    hits[i].fetch_add(1, std::memory_order_relaxed);
                }
            });
            for (u32 i = 0; i < count; ++i) {
                ASSERT_EQ(hits[i].load(), 1) << "count=" << count << " grain=" << grain << " i=" << i;
            }
            EXPECT_EQ(badThread.load(), 0u);
        }
    }
    bool called = false;
    jobs.parallelFor(0, 1, [&](u32, u32, u32) { called = true; });
    EXPECT_FALSE(called);
}

TEST(Jobs, ParallelForAsync) {
    JobSystem jobs(4);
    std::vector<int> data(10000, 1);
    auto h = jobs.parallelForAsync(static_cast<u32>(data.size()), 128, [&](u32 b, u32 e, u32) {
        for (u32 i = b; i < e; ++i) {
            data[i] *= 2;
        }
    });
    jobs.wait(h);
    for (int v : data) {
        ASSERT_EQ(v, 2);
    }
}

TEST(Jobs, NestedJobsAndWaitInsideJob) {
    JobSystem jobs(4);
    std::atomic<int> leaves{0};
    auto root = jobs.submit([&] {
        TaskGroup group(jobs);
        for (int i = 0; i < 16; ++i) {
            group.run([&] {
                TaskGroup inner(jobs);
                for (int j = 0; j < 16; ++j) {
                    inner.run([&] { leaves.fetch_add(1); });
                }
                inner.wait(); // waiting inside a job must not deadlock
            });
        }
        group.wait();
        // parallelFor from inside a job as well
        jobs.parallelFor(100, 10, [&](u32 b, u32 e, u32) { leaves.fetch_add(static_cast<int>(e - b)); });
    });
    jobs.wait(root);
    EXPECT_EQ(leaves.load(), 16 * 16 + 100);
}

TEST(Jobs, NestedWaitWithSingleWorker) {
    // Minimum configuration: main thread + one worker. Waiting inside jobs must still make progress.
    JobSystem jobs(1);
    EXPECT_EQ(jobs.threadCount(), 2u);
    std::atomic<int> n{0};
    auto h = jobs.submit([&] {
        auto a = jobs.submit([&] { n.fetch_add(1); });
        auto b = jobs.submit([&] {
            auto c = jobs.submit([&] { n.fetch_add(10); });
            jobs.wait(c);
        });
        jobs.wait(a);
        jobs.wait(b);
    });
    jobs.wait(h);
    EXPECT_EQ(n.load(), 11);
}

TEST(Jobs, MainThreadQueueFromWorkers) {
    JobSystem jobs(4);
    std::atomic<int> ranOnMain{0};
    const auto mainId = std::this_thread::get_id();
    jobs.parallelFor(64, 1, [&](u32 b, u32 e, u32) {
        for (u32 i = b; i < e; ++i) {
            jobs.enqueueMainThread([&] {
                if (std::this_thread::get_id() == mainId) {
                    ranOnMain.fetch_add(1);
                }
            });
        }
    });
    EXPECT_EQ(jobs.runMainThreadQueue(), 64u);
    EXPECT_EQ(ranOnMain.load(), 64);
    EXPECT_EQ(jobs.runMainThreadQueue(), 0u);

    // Callbacks enqueued while the queue runs are deferred to the next call.
    jobs.enqueueMainThread([&] { jobs.enqueueMainThread([] {}); });
    EXPECT_EQ(jobs.runMainThreadQueue(), 1u);
    EXPECT_EQ(jobs.runMainThreadQueue(), 1u);
}

TEST(Jobs, WaitOnFinishedAndInvalidHandles) {
    JobSystem jobs(2);
    JobHandle empty;
    EXPECT_FALSE(empty.valid());
    EXPECT_TRUE(empty.done());
    jobs.wait(empty);

    auto h = jobs.submit([] {});
    jobs.wait(h);
    EXPECT_TRUE(h.done());
    jobs.wait(h); // waiting again is fine
    JobHandle copy = h;
    jobs.wait(copy);
    EXPECT_TRUE(copy.done());
}

TEST(Jobs, SubmitFromForeignThread) {
    JobSystem jobs(3);
    std::atomic<int> sum{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            std::vector<JobHandle> hs;
            for (int i = 0; i < 50; ++i) {
                hs.push_back(jobs.submit([&] { sum.fetch_add(1); }));
            }
            jobs.parallelFor(100, 7, [&](u32 b, u32 e, u32) { sum.fetch_add(static_cast<int>(e - b)); });
            for (const auto& h : hs) {
                jobs.wait(h);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(sum.load(), 4 * (50 + 100));
    jobs.waitAll();
}

TEST(Jobs, ExceptionsAreContained) {
    JobSystem jobs(2);
    auto h = jobs.submit([] { throw std::runtime_error("boom"); });
    jobs.wait(h);
    EXPECT_TRUE(h.done());
    std::atomic<int> after{0};
    jobs.wait(jobs.submit([&] { after = 1; }));
    EXPECT_EQ(after.load(), 1);
}
