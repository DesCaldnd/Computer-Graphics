// Guide chapter 13 «ИИ»: navmesh build, path queries, areas & flags, off-mesh links, serialization.
#include <oxwald/ai/nav_query.hpp>
#include <oxwald/ai/navmesh.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace ox;
using namespace ox::ai;

namespace {

// Уровень: пол 20×20 м и стена вдоль X = 0 с проходом при z < −6.
NavMeshInput makeLevel() {
    NavMeshInput in;
    // Четырёхугольник против часовой стрелки при взгляде сверху = проходимая сторона вверх.
    in.addQuad({-10, 0, -10}, {-10, 0, 10}, {10, 0, 10}, {10, 0, -10});
    in.addBox({-0.5f, 0.f, -6.f}, {0.5f, 3.f, 10.f}); // стена высотой 3 м
    return in;
}

NavMeshBuildSettings agentSettings() {
    NavMeshBuildSettings s;
    s.cellSize = 0.2f;   // точность вокселизации по XZ
    s.cellHeight = 0.1f; // и по Y
    s.agentRadius = 0.4f;
    s.agentHeight = 1.8f;
    s.agentMaxClimb = 0.5f;
    s.agentMaxSlope = 45.f;
    return s;
}

} // namespace

TEST(GuideAiNavMesh, BuildAndFindPathAroundAWall) {
    std::unique_ptr<NavMesh> mesh = NavMesh::build(makeLevel(), agentSettings());
    ASSERT_NE(mesh, nullptr); // nullptr (+ лог) при ошибке сборки
    EXPECT_GT(mesh->stats().polygons, 2u);

    NavQuery query(*mesh); // не потокобезопасен: по объекту на поток
    const NavPath path = query.findPath({-5, 0, 5}, {5, 0, 5});
    ASSERT_EQ(path.status, PathStatus::Complete);
    ASSERT_GE(path.points.size(), 3u); // старт, обход края стены, финиш

    // Путь огибает конец стены (z < −6), поэтому заметно длиннее прямой в 10 м.
    f32 minZ = 1e9f;
    for (const glm::vec3& p : path.points) {
        minZ = std::min(minZ, p.z);
    }
    EXPECT_LT(minZ, -6.f);
    EXPECT_GT(path.length(), 20.f);

    // «Гладкий» путь: точки через ~0.5 м по поверхности — удобно для движения персонажа.
    const std::vector<glm::vec3> smooth = query.smoothPath({-5, 0, 5}, {5, 0, 5}, {}, 0.5f);
    ASSERT_FALSE(smooth.empty());
    EXPECT_LT(glm::distance(smooth.back(), glm::vec3(5, 0, 5)), 0.3f);
}

TEST(GuideAiNavMesh, PointQueries) {
    auto mesh = NavMesh::build(makeLevel(), agentSettings());
    ASSERT_NE(mesh, nullptr);
    NavQuery query(*mesh);

    // Ближайшая точка навмеша (например, «приземлить» точку спавна).
    const std::optional<NavPoint> n = query.nearestPoint({3.f, 1.f, 3.f});
    ASSERT_TRUE(n);
    EXPECT_NEAR(n->position.y, 0.f, 0.2f);
    EXPECT_FALSE(query.nearestPoint({50, 0, 50})) << "далеко от навмеша — nullopt";

    // Луч по навмешу: «видно ли» напрямую, не упрёмся ли в стену.
    const NavRaycastHit hit = query.raycast({-5, 0, 5}, {5, 0, 5});
    EXPECT_TRUE(hit.hit);
    EXPECT_LT(hit.position.x, -0.5f);

    // Случайная точка: детерминирована по seed — удобно для патрулей и тестов.
    const auto a = query.randomPointInRadius({-5, 0, 5}, 3.f, /*seed*/ 42);
    const auto b = query.randomPointInRadius({-5, 0, 5}, 3.f, /*seed*/ 42);
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->position, b->position);
}

TEST(GuideAiNavMesh, AreasFlagsAndFilters) {
    NavMeshInput in;
    in.addQuad({-10, 0, -10}, {-10, 0, 10}, {10, 0, 10}, {10, 0, -10});
    // Полоса воды поперёк уровня: всё внутри призмы получает area = Water (флаг Swim).
    in.volumes.push_back({.points = {{-1, 0, -11}, {-1, 0, 11}, {1, 0, 11}, {1, 0, -11}},
                          .minY = -1.f, .maxY = 1.f, .area = NavArea::Water});
    auto mesh = NavMesh::build(in, agentSettings());
    ASSERT_NE(mesh, nullptr);
    NavQuery query(*mesh);

    const auto water = query.nearestPoint({0, 0, 0});
    ASSERT_TRUE(water);
    EXPECT_EQ(mesh->polyArea(water->poly), NavArea::Water);
    EXPECT_EQ(mesh->polyFlags(water->poly), NavFlags::Swim);

    // Фильтр по умолчанию: вода дорогая (стоимость 10), но проходимая.
    EXPECT_EQ(query.findPath({-5, 0, 0}, {5, 0, 0}).status, PathStatus::Complete);

    // Агент, который не умеет плавать: исключаем флаг Swim → дойдёт только до берега.
    NavQueryFilter landOnly;
    landOnly.excludeFlags |= NavFlags::Swim;
    const NavPath partial = query.findPath({-5, 0, 0}, {5, 0, 0}, landOnly);
    EXPECT_EQ(partial.status, PathStatus::Partial);
    EXPECT_LT(partial.points.back().x, -0.5f);
}

TEST(GuideAiNavMesh, OffMeshLinkJumpsAGap) {
    NavMeshInput in;
    in.addQuad({-10, 0, -10}, {-10, 0, 10}, {-1, 0, 10}, {-1, 0, -10}); // левый «остров»
    in.addQuad({1, 0, -10}, {1, 0, 10}, {10, 0, 10}, {10, 0, -10});     // правый «остров»
    // Связь через пропасть (прыжок). area = Jump → флаг Jump.
    in.offMeshLinks.push_back({.start = {-1.6f, 0, 0}, .end = {1.6f, 0, 0}, .radius = 0.5f, .area = NavArea::Jump});
    auto mesh = NavMesh::build(in, agentSettings());
    ASSERT_NE(mesh, nullptr);
    NavQuery query(*mesh);

    const NavPath path = query.findPath({-5, 0, 0}, {5, 0, 0});
    ASSERT_EQ(path.status, PathStatus::Complete);
    bool jumps = false;
    for (u8 f : path.pointFlags) {
        jumps |= (f & static_cast<u8>(PathPointFlag::OffMeshLink)) != 0; // здесь проиграть анимацию прыжка
    }
    EXPECT_TRUE(jumps);

    NavQueryFilter noJump;
    noJump.excludeFlags |= NavFlags::Jump;
    EXPECT_EQ(query.findPath({-5, 0, 0}, {5, 0, 0}, noJump).status, PathStatus::Partial);
}

TEST(GuideAiNavMesh, SerializeAndCloseADoorAtRuntime) {
    auto mesh = NavMesh::build(makeLevel(), agentSettings());
    ASSERT_NE(mesh, nullptr);

    // Запекли навмеш в редакторе → сохранили блоб → загрузили в игре без пересборки.
    const std::vector<u8> blob = mesh->serialize();
    std::unique_ptr<NavMesh> loaded = NavMesh::deserialize(blob);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->stats().polygons, mesh->stats().polygons);

    // «Закрываем дверь»: отключаем полигоны прохода флагом Disabled (исключён фильтром по умолчанию).
    NavQuery query(*loaded);
    const NavPath open = query.findPath({-5, 0, 5}, {5, 0, 5});
    ASSERT_EQ(open.status, PathStatus::Complete);
    const auto gap = query.nearestPoint({0, 0, -8}); // полигон в проходе под стеной
    ASSERT_TRUE(gap);
    loaded->setPolyFlags(gap->poly, NavFlags::Disabled);
    EXPECT_NE(query.findPath({-5, 0, 5}, {5, 0, 5}).status, PathStatus::Complete);

    loaded->setPolyFlags(gap->poly, NavFlags::Walk); // открыли обратно
    EXPECT_EQ(query.findPath({-5, 0, 5}, {5, 0, 5}).status, PathStatus::Complete);
}
