// Глава 01: виртуальная файловая система и пути (docs/guide/01-core.md).
#include <oxwald/core/paths.hpp>
#include <oxwald/core/vfs.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(GuideCoreVfs, MountReadWriteList) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "oxwald_guide_vfs";
    fs::remove_all(root);
    fs::create_directories(root / "game" / "levels");
    fs::create_directories(root / "patch" / "levels");
    std::ofstream(root / "game" / "levels" / "a.txt") << "level A";
    std::ofstream(root / "game" / "levels" / "b.txt") << "level B";
    std::ofstream(root / "patch" / "levels" / "b.txt") << "level B (patched)";

    ox::Vfs vfs;
    vfs.mount("project", std::make_unique<ox::DirectoryMount>(root / "game", /*writable*/ false));
    // Источник с большим приоритетом перекрывает одноимённые файлы (так монтируются патчи и .oxpak).
    vfs.mount("project", std::make_unique<ox::DirectoryMount>(root / "patch", false), /*priority*/ 10);
    vfs.mount("user", std::make_unique<ox::DirectoryMount>(root / "user"));

    EXPECT_EQ(vfs.readText("project://levels/a.txt").valueOr(""), "level A");
    EXPECT_EQ(vfs.readText("project://levels/b.txt").valueOr(""), "level B (patched)");
    EXPECT_EQ(vfs.list("project://levels"),
              (std::vector<std::string>{"project://levels/a.txt", "project://levels/b.txt"}));

    // Пишем только в user:// (project:// смонтирован только для чтения).
    EXPECT_FALSE(vfs.writeText("project://levels/c.txt", "nope"));
    EXPECT_TRUE(vfs.writeText("user://saves/notes.txt", "hello")); // каталоги создаются сами
    EXPECT_TRUE(fs::exists(root / "user" / "saves" / "notes.txt"));
    EXPECT_EQ(vfs.resolveNative("user://saves/notes.txt"), root / "user" / "saves" / "notes.txt");

    // Выход за пределы точки монтирования запрещён.
    EXPECT_FALSE(vfs.exists("project://../secret.txt"));
    EXPECT_FALSE(ox::Vfs::parse("project://../../etc/passwd"));

    // Стандартные каталоги платформы.
    EXPECT_FALSE(ox::paths::userDataDir("MyGame").empty());
    EXPECT_TRUE(fs::exists(ox::paths::engineSourceDir() / "engine"));

    fs::remove_all(root);
}
