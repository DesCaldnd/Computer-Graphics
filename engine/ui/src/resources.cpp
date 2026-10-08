#include <oxwald/ui/resources.hpp>

#include <oxwald/core/log.hpp>
#include <oxwald/core/paths.hpp>

#include <cstdlib>
#include <fstream>

namespace ox::ui {

namespace fs = std::filesystem;

fs::path resourceDir() {
    static const fs::path dir = [] {
        if (const char* env = std::getenv("OXWALD_UI_RESOURCE_DIR"); env && *env) return fs::path(env);
        std::error_code ec;
        const fs::path packaged = paths::executableDir() / "ui";
        if (fs::is_directory(packaged / "fonts", ec)) return packaged;
        return fs::path(OX_UI_RESOURCE_DIR);
    }();
    return dir;
}

std::vector<u8> readResource(std::string_view relative) {
    const fs::path path = resourceDir() / fs::path(std::string(relative));
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        OX_LOG_ERROR("ui", "missing resource {}", path.string());
        return {};
    }
    std::vector<u8> data(usize(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    return data;
}

} // namespace ox::ui
