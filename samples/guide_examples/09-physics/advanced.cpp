// Глава 09: debug draw, собственный job executor, снапшоты (docs/guide/09-physics.md).
#include <oxwald/physics/physics.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

using namespace ox;
using namespace ox::physics;

namespace {

// Приёмник отладочной геометрии: в игре здесь вызывается ваш debug-renderer.
struct CountingSink final : PhysicsDebugSink {
    u32 lines = 0, texts = 0;
    void line(const glm::vec3&, const glm::vec3&, const Color&) override { ++lines; }
    void text(const glm::vec3&, std::string_view, const Color&) override { ++texts; }
};

// Минимальный пул потоков, через который Jolt выполняет свои задачи.
class SimpleExecutor final : public IPhysicsJobExecutor {
public:
    explicit SimpleExecutor(u32 threads) {
        for (u32 i = 0; i < threads; ++i) m_threads.emplace_back([this] { worker(); });
    }
    ~SimpleExecutor() override {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& t : m_threads) t.join();
    }
    u32 maxConcurrency() const override { return u32(m_threads.size()) + 1; }
    void submit(void (*fn)(void*), void* ctx) override { // не блокировать!
        {
            std::lock_guard lock(m_mutex);
            m_queue.emplace_back(fn, ctx);
        }
        ++submitted;
        m_cv.notify_one();
    }
    std::atomic<u32> submitted{0};

private:
    void worker() {
        for (;;) {
            std::pair<void (*)(void*), void*> job;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
                if (m_stop && m_queue.empty()) return;
                job = m_queue.front();
                m_queue.pop_front();
            }
            job.first(job.second);
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::pair<void (*)(void*), void*>> m_queue;
    std::vector<std::thread> m_threads;
    bool m_stop = false;
};

void addFloor(PhysicsWorld& world) {
    BodyDesc d;
    d.shape = createShape(ShapeDesc::box({20.f, 0.5f, 20.f}));
    d.position = {0.f, -0.5f, 0.f};
    d.motionType = MotionType::Static;
    world.createBody(d);
}

} // namespace

TEST(GuidePhysicsAdvanced, DebugDraw) {
    PhysicsWorld world;
    BodyDesc d;
    d.shape = createShape(ShapeDesc::box(glm::vec3(1.f)));
    d.motionType = MotionType::Static;
    world.createBody(d);

    CountingSink sink;
    DebugDrawOptions o;
    o.aabbs = true;  // + рамки AABB
    o.labels = true; // + подписи (id тела / userData)
    world.debugDraw(sink, o);
    EXPECT_EQ(sink.lines, 24u); // 12 рёбер коробки + 12 рёбер AABB
    EXPECT_EQ(sink.texts, 1u);
}

TEST(GuidePhysicsAdvanced, CustomJobExecutor) {
    SimpleExecutor executor(3);
    {
        PhysicsWorldDesc desc;
        desc.jobExecutor = &executor; // не владеет — executor должен пережить мир
        PhysicsWorld world(desc);
        addFloor(world);
        BodyDesc d;
        d.shape = createShape(ShapeDesc::box(glm::vec3(0.5f)));
        d.position = {0.f, 3.f, 0.f};
        BodyHandle box = world.createBody(d);
        for (int i = 0; i < 120; ++i) world.step(1.f / 60.f);
        EXPECT_NEAR(world.getPosition(box).y, 0.5f, 0.03f);
    }
    EXPECT_GT(executor.submitted.load(), 0u);
}

TEST(GuidePhysicsAdvanced, SnapshotAndResimulate) {
    PhysicsWorld world;
    addFloor(world);
    std::vector<BodyHandle> boxes;
    for (int i = 0; i < 10; ++i) {
        BodyDesc d;
        d.shape = world.shapeCache().getOrCreate(ShapeDesc::box(glm::vec3(0.3f)));
        d.position = {0.1f * f32(i), 1.f + 0.7f * f32(i), 0.f};
        boxes.push_back(world.createBody(d));
    }
    for (int i = 0; i < 30; ++i) world.step(1.f / 60.f);

    std::vector<u8> snap = world.saveState(); // скорости, контакты, соединения, персонажи
    auto simulate = [&] {
        for (int i = 0; i < 60; ++i) world.step(1.f / 60.f);
        std::vector<glm::vec3> out;
        for (BodyHandle b : boxes) out.push_back(world.getPosition(b));
        return out;
    };
    std::vector<glm::vec3> first = simulate();
    ASSERT_TRUE(world.restoreState(snap)); // те же тела должны существовать
    std::vector<glm::vec3> second = simulate();
    EXPECT_EQ(first, second); // побитово одинаково в рамках одной сборки
}
