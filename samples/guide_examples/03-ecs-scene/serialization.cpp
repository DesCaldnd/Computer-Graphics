// Глава 03: сохранение/загрузка сцен, ссылки между сущностями, копирование/вставка (docs/guide/03-ecs-scene.md).
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/scene.hpp>
#include <oxwald/scene/scene_serializer.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

// Компонент со ссылкой на другую сущность: EntityRef (UUID), а не entt::entity.
struct FollowComponent {
    ox::EntityRef target;
    ox::f32 speed = 2.0f;
    ox::f32 debugDistance = 0.0f; // вычисляется в рантайме, на диск не пишется
};

void registerGameTypes() {
    ox::registerSceneTypes();
    OX_REFLECT_TYPE(FollowComponent, "Game.Follow")
        .attributes(ox::attr::Category{"Gameplay"})
        .field("target", &FollowComponent::target)
        .field("speed", &FollowComponent::speed)
        .field("debugDistance", &FollowComponent::debugDistance, ox::attr::NoSerialize{});
    ox::ComponentRegistry::instance().add<FollowComponent>();
}

std::filesystem::path makeTempDir() {
    auto dir = std::filesystem::temp_directory_path() / ("oxwald_guide_scene_" + ox::Uuid::generate().toString());
    std::filesystem::create_directories(dir);
    return dir;
}

void buildLevel(ox::World& world) {
    ox::Entity player = world.create("Player");
    player.setPosition({0, 1, 0});
    ox::Entity camera = world.create("Camera");
    camera.add<ox::CameraComponent>().primary = true;
    auto& follow = camera.add<FollowComponent>();
    follow.target = player.ref();
    follow.debugDistance = 123.0f;
}

} // namespace

TEST(GuideSceneSerialization, SaveAndLoadScene) {
    registerGameTypes();
    const auto dir = makeTempDir();

    ox::World world;
    buildLevel(world);
    // Бинарный формат (быстро, компактно) или JSON (по суффиксу .json — удобно для diff и правки руками).
    ASSERT_TRUE(ox::saveScene(world, dir / "level.oxscene"));
    ASSERT_TRUE(ox::saveScene(world, dir / "level.oxscene.json"));

    ox::World loaded; // загружаем в пустой мир
    auto status = ox::loadScene(loaded, dir / "level.oxscene");
    ASSERT_TRUE(status) << status.error().message;

    ox::Entity camera = loaded.findByName("Camera");
    const auto& follow = camera.get<FollowComponent>();
    EXPECT_EQ(loaded.resolve(follow.target).name(), "Player"); // UUID сохранились
    EXPECT_EQ(follow.debugDistance, 0.0f);                    // NoSerialize-поле не сохранялось

    // JSON читается глазами: компоненты по имени, enum — строками.
    auto text = ox::serial::readFileBytes(dir / "level.oxscene.json");
    ASSERT_TRUE(text);
    const std::string json(reinterpret_cast<const char*>(text->data()), text->size());
    EXPECT_NE(json.find("\"Game.Follow\""), std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST(GuideSceneSerialization, InMemoryDocumentAndFilter) {
    registerGameTypes();
    ox::World world;
    buildLevel(world);

    // Документ в памяти (kind "scene") — например, для сети или своего контейнера.
    ox::SceneSerializeOptions options;
    options.entityFilter = [](ox::Entity e) { return e.name() != "Camera"; }; // false: пропустить с поддеревом
    ox::serial::Document doc = ox::serializeWorld(world, options);
    EXPECT_EQ(doc.kind, "scene");

    ox::World copy;
    ASSERT_TRUE(ox::deserializeWorld(copy, doc));
    EXPECT_EQ(copy.entityCount(), 1u);
    EXPECT_TRUE(copy.findByName("Player").valid());
}

TEST(GuideSceneSerialization, CopyPaste) {
    registerGameTypes();
    ox::World world;
    buildLevel(world);
    ox::Entity player = world.findByName("Player");
    ox::Entity camera = world.findByName("Camera");

    // Копируем игрока вместе с камерой: ссылка внутри копии перенаправится на новую копию игрока.
    ox::Entity selection[] = {player, camera};
    std::vector<std::byte> clipboard = ox::copyEntities(world, selection);

    ox::Entity group = world.create("Pasted");
    auto pasted = ox::pasteEntities(world, clipboard, group);
    ASSERT_TRUE(pasted) << pasted.error().message;
    ASSERT_EQ(pasted->size(), 2u);

    ox::Entity newPlayer = group.children()[0];
    ox::Entity newCamera = group.children()[1];
    EXPECT_NE(newPlayer.uuid(), player.uuid());                     // новые UUID
    EXPECT_EQ(newCamera.get<FollowComponent>().target, newPlayer.ref()); // ссылка переназначена

    // Копируем только камеру: ссылка за пределы выделения сохраняется (на исходного игрока).
    ox::Entity onlyCamera[] = {camera};
    auto again = ox::pasteEntities(world, ox::copyEntities(world, onlyCamera));
    ASSERT_TRUE(again);
    EXPECT_EQ((*again)[0].get<FollowComponent>().target, player.ref());
}
