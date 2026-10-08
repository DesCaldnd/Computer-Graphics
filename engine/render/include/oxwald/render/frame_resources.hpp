#pragma once

// FrameResources: the per-view, per-frame blackboard of named render graph resources. The core pipeline publishes
// SceneColorHDR, Depth, Normals, Velocity, ... (render_types.hpp: ox::render::res); features look them up by name,
// add their own outputs, or replace an entry (e.g. an upscaler publishes a new SceneColorHDR at output resolution).

#include <oxwald/rhi/render_graph.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ox::render {

class FrameResources {
public:
    void setTexture(std::string_view name, rhi::RGTexture t);
    void setBuffer(std::string_view name, rhi::RGBuffer b);
    // Invalid handle (valid() == false) when absent.
    [[nodiscard]] rhi::RGTexture texture(std::string_view name) const;
    [[nodiscard]] rhi::RGBuffer buffer(std::string_view name) const;
    [[nodiscard]] bool hasTexture(std::string_view name) const { return texture(name).valid(); }
    [[nodiscard]] bool hasBuffer(std::string_view name) const { return buffer(name).valid(); }
    void remove(std::string_view name);
    void clear();
    [[nodiscard]] std::vector<std::string> names() const;

private:
    struct Hash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    std::unordered_map<std::string, rhi::RGTexture, Hash, std::equal_to<>> m_textures;
    std::unordered_map<std::string, rhi::RGBuffer, Hash, std::equal_to<>> m_buffers;
};

} // namespace ox::render
