// Глава 01: система задач (job system) (docs/guide/01-core.md).
#include <oxwald/core/jobs.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <vector>

TEST(GuideCoreJobs, ParallelForSubmitAndGroups) {
    ox::JobSystem jobs(4); // 4 потока, включая текущий (он становится «главным», thread 0)

    // parallelFor: диапазон [0, count) режется на куски не меньше grain; вызов блокирующий.
    std::vector<float> heights(10'000);
    jobs.parallelFor(static_cast<ox::u32>(heights.size()), 256, [&](ox::u32 begin, ox::u32 end, ox::u32 /*thread*/) {
        for (ox::u32 i = begin; i < end; ++i) heights[i] = static_cast<float>(i % 10);
    });
    EXPECT_FLOAT_EQ(std::accumulate(heights.begin(), heights.end(), 0.0f), 45'000.0f);

    // submit + wait: одиночная задача.
    std::atomic<int> value{0};
    ox::JobHandle h = jobs.submit([&] { value = 42; });
    jobs.wait(h); // ожидающий поток сам выполняет другие задачи — дедлока нет
    EXPECT_TRUE(h.done());
    EXPECT_EQ(value.load(), 42);

    // TaskGroup: fork/join группы задач.
    std::atomic<int> sum{0};
    {
        ox::TaskGroup group(jobs);
        for (int i = 1; i <= 10; ++i) group.run([&sum, i] { sum += i; });
        group.wait(); // деструктор тоже ждёт
    }
    EXPECT_EQ(sum.load(), 55);

    // Результат из фоновой задачи — обратно на главный поток.
    int appliedOnMain = 0;
    ox::JobHandle bg = jobs.submit([&] {
        int computed = 7 * 6; // тяжёлая работа в фоне
        jobs.enqueueMainThread([&appliedOnMain, computed] { appliedOnMain = computed; });
    });
    jobs.wait(bg);
    EXPECT_EQ(appliedOnMain, 0);
    jobs.runMainThreadQueue(); // движок вызывает это раз в кадр
    EXPECT_EQ(appliedOnMain, 42);
}
