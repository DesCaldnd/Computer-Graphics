// Guide chapter 16 «Открытый мир»: chunk streaming and the binary chunk format.
#include <oxwald/world/chunk_data.hpp>
#include <oxwald/world/streaming.hpp>
#include <oxwald/world/terrain_gen.hpp>

#include <gtest/gtest.h>

#include <map>
#include <mutex>
#include <set>

using namespace ox;
using namespace ox::world;

namespace {

// «Диск»: заранее сериализованные чанки 64 × 64 м (в игре — файлы или .oxpak).
using Disk = std::map<std::pair<i32, i32>, std::vector<u8>>;

std::vector<u8> bakeChunk(ChunkCoord c) {
    ChunkData chunk;
    chunk.coord = c;
    HeightfieldDesc d;
    d.resolution = 65; // 64 квада + общий край с соседом
    d.worldSize = 64.f;
    d.heightScale = 40.f;
    d.origin = {f32(c.x) * 64.f, f32(c.z) * 64.f};
    d.format = HeightFormat::UNorm16;
    Heightfield hf(d);
    generateNoise(hf, {}); // шум в мировых координатах — чанки стыкуются
    chunk.heightfield = std::move(hf);
    chunk.userData = {1, 2, 3}; // свой блоб игры: сущности, правки игрока…
    return serializeChunk(chunk);
}

} // namespace

TEST(GuideWorldStreaming, ChunkFormatRoundTrip) {
    const std::vector<u8> bytes = bakeChunk({2, -1});
    ChunkData loaded;
    std::string error;
    ASSERT_TRUE(deserializeChunk(bytes, loaded, &error)) << error;
    EXPECT_EQ(loaded.coord, (ChunkCoord{2, -1}));
    ASSERT_TRUE(loaded.heightfield);
    EXPECT_EQ(loaded.heightfield->resolution(), 65u);
    EXPECT_EQ(loaded.userData, (std::vector<u8>{1, 2, 3}));

    std::vector<u8> corrupted = bytes;
    corrupted[corrupted.size() / 2] ^= 0xFF; // каждая секция защищена CRC32
    ChunkData bad;
    EXPECT_FALSE(deserializeChunk(corrupted, bad, &error));
}

TEST(GuideWorldStreaming, StreamerLoadsAroundTheViewer) {
    Disk disk;
    for (i32 z = -4; z <= 4; ++z) {
        for (i32 x = -4; x <= 4; ++x) {
            disk[{x, z}] = bakeChunk({x, z});
        }
    }

    std::set<std::pair<i32, i32>> world; // что сейчас «заспавнено» в мире
    std::mutex diskMutex;
    ChunkCallbacks cb;
    // Рабочий поток: чтение и распаковка. Никаких обращений к ECS/рендеру здесь.
    cb.load = [&](ChunkCoord c, const std::atomic<bool>& cancelled) -> std::unique_ptr<ChunkPayload> {
        if (cancelled) {
            return nullptr; // чанк уже не нужен
        }
        std::vector<u8> bytes;
        {
            std::lock_guard lock(diskMutex);
            auto it = disk.find({c.x, c.z});
            if (it == disk.end()) {
                return std::make_unique<ChunkData>(); // пустой чанк (океан)
            }
            bytes = it->second;
        }
        auto data = std::make_unique<ChunkData>();
        return deserializeChunk(bytes, *data) ? std::move(data) : nullptr; // nullptr → повтор позже
    };
    // Главный поток (внутри update()): создать сущности, залить данные в GPU…
    cb.onLoaded = [&](ChunkCoord c, ChunkPayload& p) {
        auto& data = static_cast<ChunkData&>(p);
        if (data.heightfield) {
            // здесь: создать коллайдер ландшафта, загрузить высоты в GPU, заспавнить растительность
        }
        world.insert({c.x, c.z});
    };
    cb.onUnload = [&](ChunkCoord c, ChunkPayload&) { world.erase({c.x, c.z}); };

    ChunkStreamerSettings settings;
    settings.chunkSize = 64.f;
    settings.loadRadius = 100.f;   // грузим чанки ближе 100 м…
    settings.unloadRadius = 150.f; // …выгружаем дальше 150 м (гистерезис)
    // InlineExecutor выполняет загрузки сразу — детерминированно для теста. В игре — пул потоков (по умолчанию).
    ChunkStreamer streamer(settings, cb, std::make_shared<InlineExecutor>());

    StreamingViewer player{.position = {32.f, 0.f, 32.f}, .forward = {0.f, 0.f, -1.f}};
    for (int frame = 0; frame < 30; ++frame) {
        streamer.update(std::span(&player, 1)); // раз в кадр; бюджеты ограничивают работу за кадр
    }
    EXPECT_TRUE(streamer.isAreaReady(player.position, 100.f));
    EXPECT_EQ(streamer.state({0, 0}), ChunkState::Loaded);
    EXPECT_EQ(streamer.state({4, 4}), ChunkState::Unloaded);
    EXPECT_TRUE(world.count({0, 0}));

    // Игрок ушёл на восток на 256 м: старые чанки выгружаются, новые грузятся.
    player.position.x += 256.f;
    streamer.flush(std::span(&player, 1)); // дождаться всех загрузок (экран загрузки, тесты)
    EXPECT_TRUE(streamer.isAreaReady(player.position, 100.f));
    for (int frame = 0; frame < 10; ++frame) {
        streamer.update(std::span(&player, 1));
    }
    EXPECT_EQ(streamer.state({0, 0}), ChunkState::Unloaded);
    EXPECT_FALSE(world.count({0, 0}));
    EXPECT_TRUE(world.count({4, 0}));

    streamer.unloadAll(); // перед уничтожением, если onUnload должен отработать
    EXPECT_TRUE(world.empty());
}
