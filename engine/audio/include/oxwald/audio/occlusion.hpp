#pragma once

#include <oxwald/core/types.hpp>

#include <glm/glm.hpp>

#include <functional>

namespace ox::audio {

// Plugged in by physics / the RT raycaster. Returns 0 (clear path) .. 1 (fully occluded) for the segment
// listener → source. Called from AudioEngine::update on the game thread.
class IAudioOcclusionProvider {
public:
    virtual ~IAudioOcclusionProvider() = default;
    [[nodiscard]] virtual f32 occlusion(const glm::vec3& listener, const glm::vec3& source) = 0;
};

// Adapter: counts blocking hits along the segment and converts them to an occlusion factor:
// occlusion = 1 - (1 - perHitOcclusion)^hits.
class RaycastOcclusionProvider final : public IAudioOcclusionProvider {
public:
    using CountHitsFn = std::function<u32(const glm::vec3& from, const glm::vec3& to)>;

    explicit RaycastOcclusionProvider(CountHitsFn countHits, f32 perHitOcclusion = 0.6f)
        : m_countHits(std::move(countHits)), m_perHit(perHitOcclusion) {}

    [[nodiscard]] f32 occlusion(const glm::vec3& listener, const glm::vec3& source) override {
        if (!m_countHits) {
            return 0.f;
        }
        const u32 hits = m_countHits(listener, source);
        f32 open = 1.f;
        for (u32 i = 0; i < hits; ++i) {
            open *= (1.f - m_perHit);
        }
        return 1.f - open;
    }

private:
    CountHitsFn m_countHits;
    f32 m_perHit;
};

} // namespace ox::audio
