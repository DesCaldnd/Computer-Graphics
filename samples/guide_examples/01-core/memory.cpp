// Глава 01: аллокаторы и generational-хэндлы (docs/guide/01-core.md).
#include <oxwald/core/memory.hpp>

#include <glm/vec3.hpp>
#include <gtest/gtest.h>

#include <string>

namespace {
struct Particle {
    glm::vec3 position{0.0f};
    float life = 1.0f;
};
struct Projectile {
    std::string owner;
    float speed = 0.0f;
};
} // namespace

TEST(GuideCoreMemory, FrameAllocatorScratch) {
    ox::FrameAllocator frame(64 * 1024);
    for (int f = 0; f < 3; ++f) {
        frame.reset(); // начало кадра: вся память кадра освобождается разом
        std::span<Particle> visible = frame.allocArray<Particle>(500);
        for (auto& p : visible) p = Particle{{1, 2, 3}, 0.5f};
        auto* center = frame.create<glm::vec3>(0.0f, 1.0f, 0.0f);
        EXPECT_EQ(center->y, 1.0f);
        EXPECT_TRUE(frame.owns(visible.data()));
    }
    EXPECT_GE(frame.stats().peakUsedBytes, 500 * sizeof(Particle));
    // frame.create<std::string>() не скомпилируется: деструкторы не вызываются.
}

TEST(GuideCoreMemory, HandlePoolDetectsStaleHandles) {
    ox::HandlePool<Projectile> pool;
    using ProjectileHandle = ox::HandlePool<Projectile>::HandleType;

    ProjectileHandle arrow = pool.create(Projectile{"archer", 30.0f});
    ASSERT_TRUE(pool.alive(arrow));
    EXPECT_EQ(pool.get(arrow)->owner, "archer");

    pool.destroy(arrow);
    ProjectileHandle bolt = pool.create(Projectile{"crossbow", 50.0f}); // переиспользует тот же слот
    EXPECT_EQ(bolt.index, arrow.index);
    EXPECT_NE(bolt.generation, arrow.generation);
    EXPECT_EQ(pool.get(arrow), nullptr); // старый хэндл больше не валиден — не «висячий указатель»
    EXPECT_EQ(pool.get(bolt)->speed, 50.0f);

    // Хэндл — 64-битное число: удобно хранить в скриптах, сети, сохранениях.
    EXPECT_EQ(ProjectileHandle::fromU64(bolt.toU64()), bolt);
}

TEST(GuideCoreMemory, TypedPool) {
    ox::TypedPool<Projectile> pool(128); // объекты фиксированного размера страницами по 128
    Projectile* p = pool.create(Projectile{"mage", 12.0f});
    EXPECT_EQ(pool.liveCount(), 1u);
    pool.destroy(p); // вызывает деструктор и возвращает блок в free-list
    EXPECT_EQ(pool.liveCount(), 0u);
}
