#pragma once

#include <oxwald/animation/clip.hpp>

#include <array>
#include <memory>
#include <vector>

namespace ox::anim {

// 1D blend space: samples sorted by position; weights interpolate the two neighbours, clamped outside.
class BlendSpace1D {
public:
    struct Sample {
        f32 position = 0.0f;
        std::shared_ptr<const AnimationClip> clip;
    };

    void addSample(f32 position, std::shared_ptr<const AnimationClip> clip);
    const std::vector<Sample>& samples() const { return m_samples; }
    // weights.size() == samples().size(), sums to 1.
    void computeWeights(f32 x, std::vector<f32>& weights) const;

private:
    std::vector<Sample> m_samples;
};

// 2D blend space (e.g. x = strafe velocity, y = forward velocity).
//  * Delaunay: triangulates the samples; inside a triangle → barycentric weights, outside → the closest
//    point on the hull (edge interpolation).
//  * FreeformDirectional: gradient-band interpolation in polar space (Johansen 2009) — best for
//    locomotion where samples share directions at several speeds.
//  * FreeformCartesian: gradient-band interpolation in Cartesian space.
class BlendSpace2D {
public:
    enum class Mode : u8 { Delaunay, FreeformDirectional, FreeformCartesian };

    struct Sample {
        glm::vec2 position{0.0f};
        std::shared_ptr<const AnimationClip> clip;
    };

    explicit BlendSpace2D(Mode mode = Mode::Delaunay) : m_mode(mode) {}

    void addSample(glm::vec2 position, std::shared_ptr<const AnimationClip> clip);
    void setMode(Mode mode) { m_mode = mode; }
    Mode mode() const { return m_mode; }
    const std::vector<Sample>& samples() const { return m_samples; }
    // Indices into samples(), 3 per triangle (built lazily for Delaunay mode).
    const std::vector<std::array<u32, 3>>& triangles() const;

    void computeWeights(glm::vec2 p, std::vector<f32>& weights) const;

private:
    void triangulate() const;
    void delaunayWeights(glm::vec2 p, std::vector<f32>& weights) const;
    void gradientBandWeights(glm::vec2 p, bool polar, std::vector<f32>& weights) const;

    Mode m_mode;
    std::vector<Sample> m_samples;
    mutable std::vector<std::array<u32, 3>> m_triangles;
    mutable bool m_dirty = true;
};

// Delaunay triangulation (Bowyer–Watson). Exposed for tools/tests.
std::vector<std::array<u32, 3>> delaunayTriangulate(const std::vector<glm::vec2>& points);

} // namespace ox::anim
