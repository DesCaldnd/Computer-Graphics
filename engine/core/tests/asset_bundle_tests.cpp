// APK asset VFS logic (BundleMount / AssetIndex / extractBundle) against a mock asset manager that behaves like
// AAssetManager: files open by full path, directory listings show files only (no subdirectories).
#include <oxwald/core/asset_bundle.hpp>
#include <oxwald/core/vfs.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <map>
#include <random>

namespace fs = std::filesystem;
using namespace ox;

namespace {

class MockAssetManager final : public IAssetBundle {
public:
    void add(const std::string& path, const std::string& content) { m_files[path] = content; }

    std::optional<std::vector<std::byte>> read(std::string_view path) const override {
        ++reads;
        auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return std::nullopt;
        const auto* b = reinterpret_cast<const std::byte*>(it->second.data());
        return std::vector<std::byte>(b, b + it->second.size());
    }
    // Like AAssetDir_getNextFileName: only files directly in `dir`.
    std::vector<std::string> listFiles(std::string_view dir) const override {
        std::vector<std::string> out;
        const std::string prefix = dir.empty() ? std::string() : std::string(dir) + "/";
        for (const auto& [path, content] : m_files) {
            if (!path.starts_with(prefix)) continue;
            const std::string rest = path.substr(prefix.size());
            if (rest.find('/') == std::string::npos) out.push_back(rest);
        }
        return out;
    }

    mutable int reads = 0;

private:
    std::map<std::string, std::string> m_files;
};

std::shared_ptr<MockAssetManager> makeApk() {
    auto apk = std::make_shared<MockAssetManager>();
    apk->add("oxindex.txt", "# generated\nengine/shaders/common/bindless.glsl\r\nengine/shaders/render/mesh.vert\n"
                            "engine/ui/fonts/Inter.ttf\n\nproject/Showcase.oxpak\n");
    apk->add("engine/shaders/common/bindless.glsl", "// bindless");
    apk->add("engine/shaders/render/mesh.vert", "void main(){}");
    apk->add("engine/ui/fonts/Inter.ttf", "TTF");
    apk->add("project/Showcase.oxpak", "PAK!");
    return apk;
}

std::string text(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

AssetIndex indexOf(const MockAssetManager& apk) { return AssetIndex::parse(text(*apk.read("oxindex.txt"))); }

} // namespace

TEST(AssetIndex, ParsesCommentsBlankLinesAndCrlf) {
    const AssetIndex idx = AssetIndex::parse("# c\n\n  b/x.txt \r\na.txt\nb/x.txt\n../evil\nC:/x\nc\\\\win.txt");
    EXPECT_EQ(idx.files(), (std::vector<std::string>{"a.txt", "b/x.txt", "c/win.txt"}));
    EXPECT_TRUE(idx.contains("b/x.txt"));
    EXPECT_FALSE(idx.contains("b"));
    EXPECT_TRUE(idx.containsDirectory("b"));
    EXPECT_FALSE(idx.containsDirectory("bx"));
    EXPECT_EQ(AssetIndex::parse(idx.serialize()).files(), idx.files());
}

TEST(AssetIndex, FilesUnderRecursiveAndFlat) {
    const AssetIndex idx({"e/a.txt", "e/s/b.txt", "e/s/t/c.txt", "eX/d.txt", "f.txt"});
    EXPECT_EQ(idx.filesUnder("e", false), (std::vector<std::string>{"e/a.txt"}));
    EXPECT_EQ(idx.filesUnder("e", true), (std::vector<std::string>{"e/a.txt", "e/s/b.txt", "e/s/t/c.txt"}));
    EXPECT_EQ(idx.filesUnder("", false), (std::vector<std::string>{"f.txt"}));
    EXPECT_EQ(idx.filesUnder("", true).size(), 5u);
}

TEST(BundleMount, PathJoiningRejectsEscapes) {
    EXPECT_EQ(bundlePath("engine", "shaders/x.glsl"), "engine/shaders/x.glsl");
    EXPECT_EQ(bundlePath("", "a/./b/../c"), "a/c");
    EXPECT_EQ(bundlePath("engine/", "\\shaders\\x"), "engine/shaders/x"); // separators normalised like Vfs paths
    EXPECT_EQ(bundlePath("engine", "../project/x"), std::nullopt);
    EXPECT_EQ(bundlePath("engine", "C:/x"), std::nullopt);
    EXPECT_EQ(bundlePath("engine", ""), "engine");
}

TEST(BundleMount, ReadsAndListsThroughIndex) {
    auto apk = makeApk();
    BundleMount engine(apk, "engine", indexOf(*apk));
    EXPECT_TRUE(engine.exists("shaders/common/bindless.glsl"));
    EXPECT_FALSE(engine.exists("shaders/common"));
    EXPECT_FALSE(engine.exists("../project/Showcase.oxpak"));
    apk->reads = 0;
    EXPECT_FALSE(engine.exists("shaders/missing.glsl"));
    EXPECT_EQ(apk->reads, 0) << "the index answers exists() without opening assets";

    auto data = engine.read("shaders/render/mesh.vert");
    ASSERT_TRUE(data);
    EXPECT_EQ(text(*data), "void main(){}");
    EXPECT_FALSE(engine.read("shaders/missing.glsl"));
    EXPECT_FALSE(engine.isWritable());
    EXPECT_FALSE(engine.nativePath("shaders/render/mesh.vert"));

    EXPECT_EQ(engine.list("shaders", true),
              (std::vector<std::string>{"shaders/common/bindless.glsl", "shaders/render/mesh.vert"}));
    EXPECT_TRUE(engine.list("shaders", false).empty());
    EXPECT_EQ(engine.list("", false).size(), 0u);
    EXPECT_EQ(engine.list("", true).size(), 3u);
}

TEST(BundleMount, WithoutIndexFallsBackToFlatListing) {
    auto apk = makeApk();
    BundleMount root(apk, "");
    EXPECT_TRUE(root.exists("project/Showcase.oxpak"));
    EXPECT_FALSE(root.exists("project"));
    // AAssetDir cannot see subdirectories: only files directly inside the directory are listed.
    EXPECT_EQ(root.list("engine/ui/fonts", true), (std::vector<std::string>{"engine/ui/fonts/Inter.ttf"}));
    EXPECT_TRUE(root.list("engine", true).empty());
}

TEST(BundleMount, WorksAsVfsSource) {
    auto apk = makeApk();
    const AssetIndex idx = indexOf(*apk);
    Vfs vfs;
    vfs.mount("apk", std::make_unique<BundleMount>(apk, "", idx));
    vfs.mount("engine", std::make_unique<BundleMount>(apk, "engine", idx));
    EXPECT_TRUE(vfs.exists("apk://project/Showcase.oxpak"));
    auto t = vfs.readText("engine://ui/fonts/Inter.ttf");
    ASSERT_TRUE(t);
    EXPECT_EQ(*t, "TTF");
    EXPECT_FALSE(vfs.writeText("engine://ui/new.txt", "x")) << "APK assets are read-only";
    EXPECT_EQ(vfs.list("engine://shaders", true),
              (std::vector<std::string>{"engine://shaders/common/bindless.glsl", "engine://shaders/render/mesh.vert"}));
}

TEST(BundleMount, ExtractCopiesSubtreeIncrementally) {
    auto apk = makeApk();
    const AssetIndex idx = indexOf(*apk);
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() /
                         std::format("oxwald_bundle_{}_{}", std::chrono::steady_clock::now().time_since_epoch().count(), rd());
    auto first = extractBundle(*apk, idx, "engine", dir);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->files, 3u);
    EXPECT_EQ(first->skipped, 0u);
    EXPECT_TRUE(fs::exists(dir / "shaders" / "common" / "bindless.glsl"));
    EXPECT_FALSE(fs::exists(dir / "Showcase.oxpak"));
    std::ifstream in(dir / "ui" / "fonts" / "Inter.ttf", std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(content, "TTF");
    in.close();

    auto second = extractBundle(*apk, idx, "engine", dir);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->files, 0u);
    EXPECT_EQ(second->skipped, 3u);
    auto forced = extractBundle(*apk, idx, "engine", dir, true);
    ASSERT_TRUE(forced);
    EXPECT_EQ(forced->files, 3u);

    // An index entry the bundle cannot read is an error, not a silent skip.
    const AssetIndex broken({"engine/ghost.bin"});
    EXPECT_FALSE(extractBundle(*apk, broken, "engine", dir));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
