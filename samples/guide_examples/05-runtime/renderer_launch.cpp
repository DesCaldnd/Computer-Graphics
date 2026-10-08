// Глава 05: контракт IRenderer (extract/render, интерполяция по alpha) и параметры запуска OxwaldPlayer
// (docs/guide/05-runtime.md).
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/launch.hpp>
#include <oxwald/scene/scene.hpp>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Минимальный рендерер: копирует интерполированные позиции в свой слот снимка (extract, игровой поток)
// и «рисует» из него (render, поток рендера). Мир в render() не трогаем никогда.
class PositionsRenderer final : public ox::IRenderer {
public:
    std::string_view name() const override { return "Positions"; }
    ox::Status init(ox::Services&, const ox::RenderSurface&) override { return {}; }
    void shutdown() override {}

    void extract(const ox::World& world, const ox::FrameContext& ctx) override {
        auto& snapshot = m_slots[ctx.slot];
        snapshot.positions.clear();
        auto view = world.registry().view<const ox::WorldTransformComponent>();
        for (auto [e, wt] : view.each()) {
            // Плавное движение при фиксированном шаге: lerp(previous, current, alpha).
            const glm::vec3 prev = glm::vec3(wt.previous[3]);
            const glm::vec3 curr = glm::vec3(wt.matrix[3]);
            snapshot.positions.push_back(glm::mix(prev, curr, ctx.alpha));
        }
    }
    void render(const ox::FrameContext& ctx) override {
        const auto& snapshot = m_slots[ctx.slot]; // только свой слот
        drawn.fetch_add(snapshot.positions.size());
        frames.fetch_add(1);
    }
    void resize(glm::uvec2) override {}
    void settingsChanged() override {}

    std::atomic<std::size_t> drawn{0};
    std::atomic<std::size_t> frames{0};

private:
    struct Snapshot {
        std::vector<glm::vec3> positions;
    };
    std::array<Snapshot, ox::kRenderSnapshotSlots> m_slots; // 2 слота: extract N+1 идёт параллельно с render N
};

} // namespace

TEST(GuideRuntimeRenderer, CustomRendererOnRenderThread) {
    ox::registerSceneTypes();
    const auto userDir = std::filesystem::temp_directory_path() / ("oxwald_guide_rr_" + ox::Uuid::generate().toString());

    ox::Engine engine;
    auto renderer = std::make_unique<PositionsRenderer>();
    PositionsRenderer* r = renderer.get();
    engine.setRenderer(std::move(renderer)); // до init(); по умолчанию NullRenderer

    ox::EngineConfig config;
    config.headless = true;
    config.userDir = userDir;
    config.threadedRendering = true; // false — extract+render прямо в игровом потоке (отладка)
    ASSERT_TRUE(engine.init(config));

    engine.world().create("A");
    engine.world().create("B");
    engine.run(5);
    engine.pipeline().flush(); // дождаться, пока поток рендера дорисует отправленные кадры

    EXPECT_EQ(r->frames.load(), 5u);
    EXPECT_EQ(r->drawn.load(), 10u);
    engine.shutdown(); // уничтожает рендерер: указатель r больше недействителен
    std::filesystem::remove_all(userDir);
}

TEST(GuideRuntimeLaunch, CommandLineToEngineConfig) {
    // То же, что `OxwaldPlayer --project MyGame --headless --frames 3 --quality low --cvar r.Bloom=0 --fixed-rate 30`
    const std::vector<std::string> args = {"--project", "MyGame", "--headless", "--frames", "3",
                                           "--quality", "low", "--cvar", "r.Bloom=0", "--fixed-rate", "30"};
    auto options = ox::parseLaunchOptions(args);
    ASSERT_TRUE(options) << options.error().message;
    EXPECT_EQ(options->frames, 3u);

    ox::EngineConfig config = options->toEngineConfig("MyGame");
    EXPECT_TRUE(config.headless);
    EXPECT_EQ(config.projectPath, std::filesystem::path("MyGame"));
    EXPECT_EQ(config.fixedRate, 30.0);
    EXPECT_EQ(config.cvars, std::vector<std::string>{"r.Bloom=0"});

    // Ошибки разбора возвращаются как Result, а не исключения.
    const std::vector<std::string> bad = {"--quality", "insane"};
    EXPECT_FALSE(ox::parseLaunchOptions(bad));
}
