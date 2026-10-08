#include <oxwald/render/frame_resources.hpp>

#include <algorithm>

namespace ox::render {

void FrameResources::setTexture(std::string_view name, rhi::RGTexture t) {
    auto it = m_textures.find(name);
    if (it != m_textures.end()) it->second = t;
    else m_textures.emplace(std::string(name), t);
}

void FrameResources::setBuffer(std::string_view name, rhi::RGBuffer b) {
    auto it = m_buffers.find(name);
    if (it != m_buffers.end()) it->second = b;
    else m_buffers.emplace(std::string(name), b);
}

rhi::RGTexture FrameResources::texture(std::string_view name) const {
    auto it = m_textures.find(name);
    return it != m_textures.end() ? it->second : rhi::RGTexture{};
}

rhi::RGBuffer FrameResources::buffer(std::string_view name) const {
    auto it = m_buffers.find(name);
    return it != m_buffers.end() ? it->second : rhi::RGBuffer{};
}

void FrameResources::remove(std::string_view name) {
    if (auto it = m_textures.find(name); it != m_textures.end()) m_textures.erase(it);
    if (auto it = m_buffers.find(name); it != m_buffers.end()) m_buffers.erase(it);
}

void FrameResources::clear() {
    m_textures.clear();
    m_buffers.clear();
}

std::vector<std::string> FrameResources::names() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : m_textures) out.push_back(k);
    for (const auto& [k, v] : m_buffers) out.push_back(k);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace ox::render
