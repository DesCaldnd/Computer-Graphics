#include "test_util.hpp"

#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/render_pipeline.hpp>

#include <gtest/gtest.h>

#include <mutex>
#include <thread>

using namespace ox;

namespace {

// Records which frames were extracted/rendered on which thread and checks that a snapshot slot is never written
// (extract) and read (render) at the same time.
class RecordingRenderer final : public IRenderer {
public:
    explicit RecordingRenderer(std::chrono::microseconds renderTime = std::chrono::microseconds(2000))
        : m_renderTime(renderTime) {}

    std::string_view name() const override { return "Recording"; }
    Status init(Services&, const RenderSurface& s) override {
        size = s.framebufferSize;
        return {};
    }
    void shutdown() override {}

    void extract(const World& world, const FrameContext& ctx) override {
        auto& slot = m_slots[ctx.slot];
        if (slot.rendering.load()) violations.fetch_add(1);
        slot.extracting.store(true);
        if (m_renderingAny.load()) overlappedFrames.fetch_add(1); // extract N+1 while N renders
        slot.frame = ctx.frameIndex; // plain (non-atomic) data handed over through the pipeline
        slot.entities = world.entityCount();
        {
            std::lock_guard lock(m_mutex);
            extracted.push_back({ctx.frameIndex, std::this_thread::get_id()});
        }
        slot.extracting.store(false);
    }

    void render(const FrameContext& ctx) override {
        auto& slot = m_slots[ctx.slot];
        if (slot.extracting.load()) violations.fetch_add(1);
        slot.rendering.store(true);
        m_renderingAny.store(true);
        if (slot.frame != ctx.frameIndex) wrongSnapshot.fetch_add(1);
        std::this_thread::sleep_for(m_renderTime);
        {
            std::lock_guard lock(m_mutex);
            rendered.push_back({ctx.frameIndex, std::this_thread::get_id()});
        }
        m_renderingAny.store(false);
        slot.rendering.store(false);
    }

    void resize(glm::uvec2 s) override {
        size = s;
        resizeThread = std::this_thread::get_id();
    }
    void settingsChanged() override {
        ++settingsChanges;
        settingsThread = std::this_thread::get_id();
    }

    struct Record {
        u64 frame;
        std::thread::id thread;
    };
    std::mutex m_mutex;
    std::vector<Record> extracted, rendered;
    std::atomic<int> violations{0}, wrongSnapshot{0}, overlappedFrames{0};
    glm::uvec2 size{0, 0};
    std::thread::id resizeThread, settingsThread;
    int settingsChanges = 0;

private:
    struct Slot {
        std::atomic<bool> extracting{false}, rendering{false};
        u64 frame = ~0ull;
        usize entities = 0;
    };
    std::array<Slot, kRenderSnapshotSlots> m_slots;
    std::atomic<bool> m_renderingAny{false};
    std::chrono::microseconds m_renderTime;
};

} // namespace

TEST(RenderPipeline, ThreadedFramesInOrderWithoutOverlapOnASlot) {
    World world;
    world.create("A");
    RecordingRenderer r;
    RenderPipeline pipeline;
    pipeline.start(r, RenderPipeline::Mode::Threaded);
    const auto gameThread = std::this_thread::get_id();
    constexpr u64 kFrames = 40;
    for (u64 i = 0; i < kFrames; ++i) {
        FrameContext ctx;
        ctx.frameIndex = i;
        pipeline.submit(world, ctx);
        std::this_thread::sleep_for(std::chrono::microseconds(500)); // simulated game work
    }
    pipeline.flush();
    EXPECT_EQ(pipeline.stats().framesRendered, kFrames);
    pipeline.stop();

    ASSERT_EQ(r.extracted.size(), kFrames);
    ASSERT_EQ(r.rendered.size(), kFrames);
    for (u64 i = 0; i < kFrames; ++i) {
        EXPECT_EQ(r.extracted[i].frame, i);
        EXPECT_EQ(r.rendered[i].frame, i);
        EXPECT_EQ(r.extracted[i].thread, gameThread);
        EXPECT_NE(r.rendered[i].thread, gameThread);
        EXPECT_EQ(r.rendered[i].thread, r.rendered[0].thread);
    }
    EXPECT_EQ(r.violations.load(), 0);
    EXPECT_EQ(r.wrongSnapshot.load(), 0);
    // Rendering (2 ms) is slower than the game (0.5 ms): the game thread extracts the next frame while the
    // previous one renders.
    EXPECT_GT(r.overlappedFrames.load(), 0);
}

TEST(RenderPipeline, GameThreadIsAtMostOneFrameAhead) {
    World world;
    RecordingRenderer r(std::chrono::microseconds(3000));
    RenderPipeline pipeline;
    pipeline.start(r, RenderPipeline::Mode::Threaded);
    for (u64 i = 0; i < 10; ++i) {
        FrameContext ctx;
        ctx.frameIndex = i;
        pipeline.submit(world, ctx);
        // After submitting frame i, frame i-2 must have been rendered (its slot was reused).
        std::lock_guard lock(r.m_mutex);
        if (i >= 2) EXPECT_GE(r.rendered.size(), i - 1) << "frame " << i;
    }
    pipeline.stop(); // drains queued frames
    EXPECT_EQ(r.rendered.size(), 10u);
}

TEST(RenderPipeline, SingleThreadedRunsInline) {
    World world;
    RecordingRenderer r(std::chrono::microseconds(0));
    RenderPipeline pipeline;
    pipeline.start(r, RenderPipeline::Mode::SingleThreaded);
    for (u64 i = 0; i < 5; ++i) {
        FrameContext ctx;
        ctx.frameIndex = i;
        pipeline.submit(world, ctx);
        EXPECT_EQ(r.rendered.size(), i + 1);
    }
    pipeline.stop();
    for (const auto& rec : r.rendered) EXPECT_EQ(rec.thread, std::this_thread::get_id());
    EXPECT_EQ(r.wrongSnapshot.load(), 0);
}

TEST(RenderPipeline, ResizeAndSettingsAppliedOnRenderThread) {
    World world;
    RecordingRenderer r(std::chrono::microseconds(0));
    RenderPipeline pipeline;
    pipeline.start(r, RenderPipeline::Mode::Threaded);
    pipeline.requestResize({640, 360});
    pipeline.requestSettingsChanged();
    pipeline.requestSettingsChanged(); // coalesced
    FrameContext ctx;
    pipeline.submit(world, ctx);
    pipeline.flush();
    EXPECT_EQ(r.size, glm::uvec2(640, 360));
    EXPECT_EQ(r.settingsChanges, 1);
    EXPECT_EQ(r.resizeThread, r.rendered[0].thread);
    EXPECT_EQ(r.settingsThread, r.rendered[0].thread);
    pipeline.stop();
}

TEST(RenderPipeline, EngineDrivesRendererThroughThePipeline) {
    test::TempDir dir;
    auto renderer = std::make_unique<RecordingRenderer>(std::chrono::microseconds(500));
    RecordingRenderer* r = renderer.get();
    Engine engine;
    engine.setRenderer(std::move(renderer));
    EngineConfig cfg;
    cfg.headless = true;
    cfg.workerThreads = 2;
    cfg.userDir = dir / "user";
    ASSERT_TRUE(engine.init(cfg));
    engine.world().create("E");
    for (int i = 0; i < 20; ++i) engine.tick(1.0 / 60.0);
    engine.pipeline().flush();
    {
        std::lock_guard lock(r->m_mutex);
        ASSERT_EQ(r->rendered.size(), 20u);
        for (u64 i = 0; i < 20; ++i) EXPECT_EQ(r->rendered[i].frame, i);
        EXPECT_NE(r->rendered[0].thread, std::this_thread::get_id());
    }
    EXPECT_EQ(r->violations.load(), 0);

    // Graphics settings changes reach the renderer (render thread) without restart.
    GraphicsSettings g = engine.settings().user().graphics;
    g.vsync = !g.vsync;
    engine.settings().setGraphics(g);
    engine.tick(1.0 / 60.0);
    engine.pipeline().flush();
    EXPECT_EQ(r->settingsChanges, 1);
    engine.shutdown();
}

TEST(RenderPipeline, SingleThreadedEngineMode) {
    test::TempDir dir;
    auto renderer = std::make_unique<RecordingRenderer>(std::chrono::microseconds(0));
    RecordingRenderer* r = renderer.get();
    Engine engine;
    engine.setRenderer(std::move(renderer));
    EngineConfig cfg;
    cfg.headless = true;
    cfg.threadedRendering = false;
    cfg.workerThreads = 2;
    cfg.userDir = dir / "user";
    ASSERT_TRUE(engine.init(cfg));
    for (int i = 0; i < 5; ++i) engine.tick(1.0 / 60.0);
    ASSERT_EQ(r->rendered.size(), 5u);
    for (const auto& rec : r->rendered) EXPECT_EQ(rec.thread, std::this_thread::get_id());
}
