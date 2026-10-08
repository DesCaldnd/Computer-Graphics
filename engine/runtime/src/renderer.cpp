#include <oxwald/core/profile.hpp>
#include <oxwald/core/time.hpp>
#include <oxwald/runtime/renderer.hpp>
#include <oxwald/scene/world.hpp>

#include <thread>

namespace ox {

Status NullRenderer::init(Services&, const RenderSurface& surface) {
    m_size = surface.framebufferSize;
    return {};
}

void NullRenderer::extract(const World& world, const FrameContext& ctx) {
    OX_PROFILE_ZONE();
    Snapshot& s = m_snapshots[ctx.slot];
    s.frameIndex = ctx.frameIndex;
    s.entityCount = world.entityCount();
}

void NullRenderer::render(const FrameContext& ctx) {
    OX_PROFILE_ZONE();
    Stopwatch sw(true);
    const Snapshot& s = m_snapshots[ctx.slot];
    OX_ASSERT(s.frameIndex == ctx.frameIndex, "snapshot slot {} holds frame {}, expected {}", ctx.slot, s.frameIndex,
              ctx.frameIndex);
    if (m_simulatedRenderMs > 0.0) {
        std::this_thread::sleep_for(std::chrono::duration<f64, std::milli>(m_simulatedRenderMs));
    }
    m_frames.fetch_add(1);
    m_lastRenderMs.store(sw.elapsedMs());
}

RenderStats NullRenderer::stats() const {
    RenderStats st;
    st.framesRendered = m_frames.load();
    st.lastRenderMs = m_lastRenderMs.load();
    return st;
}

} // namespace ox
