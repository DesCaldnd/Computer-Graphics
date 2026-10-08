#include <oxwald/ui/draw_data.hpp>

namespace ox::ui {

void UiFrame::clear() {
    vertices.clear();
    indices.clear();
    commands.clear();
    transforms.clear();
}

UiDrawCmd& UiFrame::add(std::span<const UiVertex> v, std::span<const u32> i, const UiDrawCmd& cmd) {
    UiDrawCmd c = cmd;
    c.vertexOffset = i32(vertices.size());
    c.firstIndex = u32(indices.size());
    c.indexCount = u32(i.size());
    vertices.insert(vertices.end(), v.begin(), v.end());
    indices.insert(indices.end(), i.begin(), i.end());
    commands.push_back(c);
    return commands.back();
}

// ---------------------------------------------------------------------------------------------------------------

u32 UiTextureStore::create(u32 width, u32 height, std::vector<u8> rgba, std::string name) {
    std::lock_guard lock(m_mutex);
    const u32 id = m_nextId++;
    Entry& e = m_entries[id];
    e.width = width;
    e.height = height;
    e.pixels = std::make_shared<const std::vector<u8>>(std::move(rgba));
    e.version = m_version.fetch_add(1, std::memory_order_acq_rel) + 1;
    e.name = std::move(name);
    return id;
}

bool UiTextureStore::update(u32 id, u32 width, u32 height, std::vector<u8> rgba) {
    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return false;
    it->second.width = width;
    it->second.height = height;
    it->second.pixels = std::make_shared<const std::vector<u8>>(std::move(rgba));
    it->second.version = m_version.fetch_add(1, std::memory_order_acq_rel) + 1;
    return true;
}

void UiTextureStore::destroy(u32 id) {
    std::lock_guard lock(m_mutex);
    if (m_entries.erase(id)) m_version.fetch_add(1, std::memory_order_acq_rel);
}

bool UiTextureStore::contains(u32 id) const {
    std::lock_guard lock(m_mutex);
    return m_entries.contains(id);
}

std::optional<UiTextureStore::Entry> UiTextureStore::get(u32 id) const {
    std::lock_guard lock(m_mutex);
    auto it = m_entries.find(id);
    if (it == m_entries.end()) return std::nullopt;
    return it->second;
}

usize UiTextureStore::size() const {
    std::lock_guard lock(m_mutex);
    return m_entries.size();
}

std::unordered_map<u32, UiTextureStore::Entry> UiTextureStore::snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_entries;
}

// ---------------------------------------------------------------------------------------------------------------

std::vector<UpscalerAvailability> upscalerAvailability(const rhi::DeviceCaps& caps) {
    std::vector<UpscalerAvailability> out;
    out.push_back({"Off", true, {}});
    out.push_back({"FSR1", true, {}}); // portable compute/fragment upscaler
    UpscalerAvailability dlss{"DLSS", false, {}};
#if defined(_WIN32) || defined(__linux__)
    if (caps.vendor != rhi::GpuVendor::Nvidia) dlss.reason = "DLSS needs an NVIDIA RTX GPU (found " + caps.gpuName + ")";
    else if (caps.gpuName.find("RTX") == std::string::npos) dlss.reason = "DLSS needs an NVIDIA RTX GPU";
    else dlss.available = true;
#else
    dlss.reason = "DLSS (NVIDIA NGX) is only available on Windows/Linux with an NVIDIA RTX GPU";
#endif
    out.push_back(std::move(dlss));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------

void UiRenderBridge::publish(std::shared_ptr<const UiFrame> frame) {
    std::lock_guard lock(m_mutex);
    m_frame = std::move(frame);
}

std::shared_ptr<const UiFrame> UiRenderBridge::latest() const {
    std::lock_guard lock(m_mutex);
    return m_frame;
}

void UiRenderBridge::setRenderInfo(RenderInfo info) {
    std::lock_guard lock(m_mutex);
    m_info = std::move(info);
    m_hasInfo.store(true, std::memory_order_release);
}

RenderInfo UiRenderBridge::renderInfo() const {
    std::lock_guard lock(m_mutex);
    return m_info;
}

void UiRenderBridge::setRenderGraph(RenderGraphInfo info) {
    std::lock_guard lock(m_mutex);
    m_graph = std::move(info);
}

std::optional<RenderGraphInfo> UiRenderBridge::renderGraph() const {
    std::lock_guard lock(m_mutex);
    return m_graph;
}

void UiRenderBridge::setBenchmark(render::BenchmarkResult result) {
    std::lock_guard lock(m_mutex);
    m_benchmark = std::move(result);
}

std::optional<render::BenchmarkResult> UiRenderBridge::takeBenchmark() {
    std::lock_guard lock(m_mutex);
    return std::exchange(m_benchmark, std::nullopt);
}

} // namespace ox::ui
