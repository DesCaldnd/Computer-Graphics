#include <oxwald/core/paths.hpp>
#include <oxwald/core/vfs.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <map>
#include <random>

namespace fs = std::filesystem;
using namespace ox;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        m_path = fs::temp_directory_path() /
                 std::format("oxwald_vfs_{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), rd());
        fs::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    const fs::path& path() const { return m_path; }
    void write(const std::string& rel, const std::string& content) const {
        const fs::path p = m_path / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << content;
    }

private:
    fs::path m_path;
};

// In-memory, read-only source (stands in for a pak archive).
class MemoryMount final : public IMountSource {
public:
    std::map<std::string, std::string> files;

    bool exists(std::string_view rel) const override { return files.contains(std::string(rel)); }
    Result<std::vector<std::byte>> read(std::string_view rel) const override {
        auto it = files.find(std::string(rel));
        if (it == files.end()) {
            return makeError("missing {}", rel);
        }
        std::vector<std::byte> out(it->second.size());
        std::memcpy(out.data(), it->second.data(), out.size());
        return out;
    }
    std::vector<std::string> list(std::string_view relDir, bool recursive) const override {
        std::vector<std::string> out;
        const std::string prefix = relDir.empty() ? "" : std::string(relDir) + "/";
        for (const auto& [name, _] : files) {
            if (name.starts_with(prefix) && (recursive || name.find('/', prefix.size()) == std::string::npos)) {
                out.push_back(name);
            }
        }
        return out;
    }
};

} // namespace

TEST(Paths, Basics) {
    const fs::path exe = paths::executablePath();
    ASSERT_FALSE(exe.empty());
    EXPECT_TRUE(fs::exists(exe));
    EXPECT_EQ(paths::executableDir(), exe.parent_path());
    EXPECT_TRUE(fs::exists(paths::engineSourceDir() / "engine" / "core"));
    EXPECT_TRUE(fs::is_directory(paths::tempDir()));
    const fs::path user = paths::userDataDir("OxTest");
    EXPECT_EQ(user.filename(), "OxTest");
    EXPECT_TRUE(user.is_absolute());
}

TEST(Vfs, ParseAndNormalize) {
    auto u = Vfs::parse("project://levels/./sub//../a.oxscene");
    ASSERT_TRUE(u);
    EXPECT_EQ(u->scheme, "project");
    EXPECT_EQ(u->path, "levels/a.oxscene");
    EXPECT_EQ(Vfs::parse("engine://shaders\\common\\pbr.glsl")->path, "shaders/common/pbr.glsl");
    EXPECT_EQ(Vfs::parse("user://")->path, "");
    EXPECT_EQ(Vfs::parse("user:///abs/x")->path, "abs/x"); // leading slashes stay inside the mount

    EXPECT_FALSE(Vfs::parse("no-scheme/path"));
    EXPECT_FALSE(Vfs::parse("://x"));
    EXPECT_FALSE(Vfs::parse("bad scheme://x"));
    EXPECT_FALSE(Vfs::parse("project://../secret"));
    EXPECT_FALSE(Vfs::parse("project://a/../../secret"));
    EXPECT_FALSE(Vfs::parse("project://a/b/../../../x"));
    EXPECT_FALSE(Vfs::parse("project://C:/Windows/x"));
    EXPECT_FALSE(Vfs::parse("project://..\\x"));
    EXPECT_EQ(Vfs::normalizePath("a/b/.."), std::optional<std::string>("a"));
    EXPECT_EQ(Vfs::makeUri("engine", "a/b"), "engine://a/b");
}

TEST(Vfs, DirectoryMountReadWriteList) {
    TempDir dir;
    dir.write("levels/a.oxscene", "level-a");
    dir.write("levels/deep/b.oxscene", "level-b");
    dir.write("readme.txt", "hi");

    Vfs vfs;
    vfs.mount("project", std::make_unique<DirectoryMount>(dir.path()));
    EXPECT_TRUE(vfs.isMounted("project"));
    EXPECT_TRUE(vfs.exists("project://levels/a.oxscene"));
    EXPECT_FALSE(vfs.exists("project://levels")); // directories are not files
    EXPECT_FALSE(vfs.exists("project://nope"));
    EXPECT_FALSE(vfs.exists("engine://levels/a.oxscene")); // unmounted scheme

    auto text = vfs.readText("project://levels/a.oxscene");
    ASSERT_TRUE(text) << text.error().message;
    EXPECT_EQ(*text, "level-a");
    EXPECT_FALSE(vfs.readBytes("project://missing.bin"));
    EXPECT_FALSE(vfs.readBytes("engine://x"));

    EXPECT_TRUE(vfs.writeText("project://saves/slot1/save.json", "{}"));
    EXPECT_EQ(*vfs.readText("project://saves/slot1/save.json"), "{}");
    EXPECT_TRUE(fs::exists(dir.path() / "saves" / "slot1" / "save.json"));
    EXPECT_TRUE(vfs.writeText("project://saves/slot1/save.json", "{\"v\":2}")); // overwrite
    EXPECT_EQ(*vfs.readText("project://saves/slot1/save.json"), "{\"v\":2}");
    const std::vector<std::byte> empty;
    EXPECT_TRUE(vfs.writeBytes("project://empty.bin", empty));
    EXPECT_TRUE(vfs.readBytes("project://empty.bin")->empty());

    EXPECT_EQ(vfs.list("project://levels", false), (std::vector<std::string>{"project://levels/a.oxscene"}));
    EXPECT_EQ(vfs.list("project://levels", true),
              (std::vector<std::string>{"project://levels/a.oxscene", "project://levels/deep/b.oxscene"}));
    EXPECT_EQ(vfs.list("project://", false).size(), 2u); // readme.txt, empty.bin

    auto native = vfs.resolveNative("project://levels/a.oxscene");
    ASSERT_TRUE(native);
    EXPECT_TRUE(fs::equivalent(*native, dir.path() / "levels" / "a.oxscene"));
    auto newFile = vfs.resolveNative("project://new/file.txt");
    ASSERT_TRUE(newFile);
    EXPECT_EQ(newFile->filename(), "file.txt");
    EXPECT_TRUE(vfs.modificationTime("project://readme.txt").has_value());
    EXPECT_FALSE(vfs.modificationTime("project://missing").has_value());

    vfs.unmount("project");
    EXPECT_FALSE(vfs.isMounted("project"));
    EXPECT_FALSE(vfs.exists("project://levels/a.oxscene"));
}

TEST(Vfs, RejectsPathTraversal) {
    TempDir outer;
    outer.write("secret.txt", "top secret");
    outer.write("root/inside.txt", "ok");
    Vfs vfs;
    vfs.mount("project", std::make_unique<DirectoryMount>(outer.path() / "root"));
    EXPECT_TRUE(vfs.exists("project://inside.txt"));
    EXPECT_FALSE(vfs.exists("project://../secret.txt"));
    EXPECT_FALSE(vfs.readBytes("project://../secret.txt"));
    EXPECT_FALSE(vfs.readBytes("project://sub/../../secret.txt"));
    EXPECT_FALSE(vfs.writeText("project://../evil.txt", "x"));
    EXPECT_FALSE(fs::exists(outer.path() / "evil.txt"));
    EXPECT_FALSE(vfs.resolveNative("project://../secret.txt"));
    EXPECT_TRUE(vfs.list("project://..", true).empty());

    // The source itself also refuses unsafe paths when used directly.
    DirectoryMount direct(outer.path() / "root");
    EXPECT_FALSE(direct.exists("../secret.txt"));
    EXPECT_FALSE(direct.read("../secret.txt"));
    const std::byte data[1]{};
    EXPECT_FALSE(direct.write("../evil.txt", data));
}

TEST(Vfs, PriorityOverlayAndMemorySource) {
    TempDir dir;
    dir.write("textures/a.png", "loose-a");
    dir.write("textures/b.png", "loose-b");

    auto pak = std::make_unique<MemoryMount>();
    pak->files["textures/a.png"] = "pak-a";
    pak->files["textures/c.png"] = "pak-c";
    pak->files["textures/sub/d.png"] = "pak-d";
    MemoryMount* pakPtr = pak.get();

    Vfs vfs;
    vfs.mount("engine", std::make_unique<DirectoryMount>(dir.path(), /*writable*/ true), 0);
    vfs.mount("engine", std::move(pak), 10); // overlay wins
    EXPECT_EQ(*vfs.readText("engine://textures/a.png"), "pak-a");
    EXPECT_EQ(*vfs.readText("engine://textures/b.png"), "loose-b"); // falls through
    EXPECT_EQ(*vfs.readText("engine://textures/c.png"), "pak-c");
    EXPECT_EQ(vfs.list("engine://textures", false),
              (std::vector<std::string>{"engine://textures/a.png", "engine://textures/b.png", "engine://textures/c.png"}));
    EXPECT_EQ(vfs.list("engine://textures", true).size(), 4u);
    // The memory source has no native path; resolveNative falls back to the directory source for b.
    EXPECT_FALSE(pakPtr->nativePath("textures/a.png"));
    EXPECT_TRUE(vfs.resolveNative("engine://textures/b.png"));

    // Writes go to the first writable source (the directory), reads still prefer the overlay.
    EXPECT_TRUE(vfs.writeText("engine://textures/a.png", "new-loose-a"));
    EXPECT_EQ(*vfs.readText("engine://textures/a.png"), "pak-a");

    EXPECT_TRUE(vfs.unmount("engine", pakPtr));
    EXPECT_FALSE(vfs.unmount("engine", pakPtr));
    EXPECT_EQ(*vfs.readText("engine://textures/a.png"), "new-loose-a");
    EXPECT_FALSE(vfs.exists("engine://textures/c.png"));

    // Read-only directory source refuses writes.
    Vfs ro;
    ro.mount("engine", std::make_unique<DirectoryMount>(dir.path(), false));
    EXPECT_FALSE(ro.writeText("engine://x.txt", "x"));
    EXPECT_FALSE(fs::exists(dir.path() / "x.txt"));
}

TEST(Vfs, EqualPriorityPrefersLatestMount) {
    auto first = std::make_unique<MemoryMount>();
    first->files["a.txt"] = "first";
    auto second = std::make_unique<MemoryMount>();
    second->files["a.txt"] = "second";
    Vfs vfs;
    vfs.mount("user", std::move(first));
    vfs.mount("user", std::move(second));
    EXPECT_EQ(*vfs.readText("user://a.txt"), "second");
    EXPECT_FALSE(vfs.writeText("user://a.txt", "x")); // no writable source
}
