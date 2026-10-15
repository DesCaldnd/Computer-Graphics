#include <oxwald/android/android_assets.hpp>
#include <oxwald/core/log.hpp>

#include <android/asset_manager.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ox::android {

namespace fs = std::filesystem;

std::optional<std::vector<std::byte>> AndroidAssetBundle::read(std::string_view path) const {
    if (!m_manager) return std::nullopt;
    AAsset* asset = AAssetManager_open(m_manager, std::string(path).c_str(), AASSET_MODE_STREAMING);
    if (!asset) return std::nullopt;
    const off64_t length = AAsset_getLength64(asset);
    std::vector<std::byte> data(usize(length > 0 ? length : 0));
    usize offset = 0;
    while (offset < data.size()) {
        const int n = AAsset_read(asset, data.data() + offset, data.size() - offset);
        if (n <= 0) break;
        offset += usize(n);
    }
    AAsset_close(asset);
    if (offset != data.size()) return std::nullopt;
    return data;
}

std::vector<std::string> AndroidAssetBundle::listFiles(std::string_view dir) const {
    std::vector<std::string> out;
    if (!m_manager) return out;
    AAssetDir* d = AAssetManager_openDir(m_manager, std::string(dir).c_str());
    if (!d) return out;
    while (const char* name = AAssetDir_getNextFileName(d)) out.emplace_back(name);
    AAssetDir_close(d);
    return out;
}

namespace {

std::string toText(const std::optional<std::vector<std::byte>>& b) {
    return b ? std::string(reinterpret_cast<const char*>(b->data()), b->size()) : std::string();
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

Result<PreparedContent> prepareContent(const AndroidAssetBundle& bundle, const fs::path& filesDir) {
    PreparedContent c;
    const std::string indexText = toText(bundle.read("oxindex.txt"));
    if (indexText.empty()) return makeError("APK has no assets/oxindex.txt (was the APK built by apps/android?)");
    c.index = AssetIndex::parse(indexText);
    c.engineRoot = filesDir / "oxwald";
    c.userDir = filesDir / "user";
    std::error_code ec;
    fs::create_directories(c.userDir, ec);

    const std::string stamp = toText(bundle.read("oxbuild.txt"));
    const fs::path stampFile = filesDir / "oxbuild.extracted";
    const bool upToDate = !stamp.empty() && fs::exists(stampFile, ec) && readFile(stampFile) == stamp;
    if (!upToDate) {
        const auto t0 = std::chrono::steady_clock::now();
        fs::remove_all(c.engineRoot, ec);
        fs::remove_all(filesDir / "project", ec);
        usize files = 0, bytes = 0;
        for (const char* prefix : {"oxwald", "project"}) {
            const fs::path dest = filesDir / prefix;
            auto r = extractBundle(bundle, c.index, prefix, dest, true);
            if (!r) return r.error();
            files += r->files;
            bytes += r->bytes;
        }
        std::ofstream(stampFile, std::ios::binary | std::ios::trunc) << stamp;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        OX_LOG_INFO("android", "extracted {} files ({:.1f} MB) from the APK in {:.0f} ms (build {})", files,
                    double(bytes) / (1024.0 * 1024.0), ms, stamp.substr(0, stamp.find('\n')));
        c.extracted = true;
    }
    for (const std::string& f : c.index.filesUnder("project", false)) {
        if (f.ends_with(".oxpak")) {
            c.pak = filesDir / fs::path(f);
            break;
        }
    }
    // No /tmp on Android: std::filesystem::temp_directory_path() (paths::tempDir) reads TMPDIR.
    const fs::path tmp = filesDir / "tmp";
    fs::create_directories(tmp, ec);
    setenv("TMPDIR", tmp.c_str(), 1);
    if (!std::getenv("HOME")) setenv("HOME", filesDir.c_str(), 1);
    // Code that reads engine files by native path (shader compiler includes, UI fonts, translucency contracts).
    setenv("OXWALD_SOURCE_DIR", c.engineRoot.c_str(), 1);
    setenv("OXWALD_SHADER_DIR", (c.engineRoot / "engine" / "shaders").c_str(), 1);
    setenv("OXWALD_UI_RESOURCE_DIR", (c.engineRoot / "engine" / "ui" / "resources").c_str(), 1);
    return c;
}

} // namespace ox::android
