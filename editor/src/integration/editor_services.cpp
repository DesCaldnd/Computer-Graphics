#include "integration/editor_services.hpp"

#include <oxwald/core/debug_draw.hpp>

namespace ox::editor {

EditorServices::EditorServices()
    : m_caps(createDefaultCapsProvider()), m_benchmark(std::make_unique<HeuristicBenchmark>()),
      m_assets(std::make_unique<FileSystemAssetBackend>()) {
    m_engine.emplace<DebugDraw>();
}

EditorServices::~EditorServices() = default;

} // namespace ox::editor
