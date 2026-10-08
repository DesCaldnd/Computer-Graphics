#pragma once

#include <oxwald/core/types.hpp>

#include <glm/vec3.hpp>

#include <array>
#include <utility>

// Stateless cubic Bézier helpers. Spline uses them internally for every cubic segment kind
// (Bézier, Catmull-Rom and Linear are all converted to cubic Bézier segments).
namespace ox::spline {

using CubicBezier = std::array<glm::vec3, 4>;

// Bernstein-form evaluation, u ∈ [0, 1].
glm::vec3 bezierPosition(const CubicBezier& b, f32 u);
glm::vec3 bezierDerivative(const CubicBezier& b, f32 u);
glm::vec3 bezierSecondDerivative(const CubicBezier& b, f32 u);

// Reference de Casteljau evaluation (numerically robust, slower).
glm::vec3 deCasteljau(const CubicBezier& b, f32 u);

// Splits at u into [0,u] and [u,1]; the two halves trace exactly the original curve.
std::pair<CubicBezier, CubicBezier> splitBezier(const CubicBezier& b, f32 u);

// Cubic Hermite (p0, velocity m0) → (p1, velocity m1) on u ∈ [0,1] as a Bézier.
CubicBezier hermiteToBezier(const glm::vec3& p0, const glm::vec3& m0, const glm::vec3& p1, const glm::vec3& m1);

} // namespace ox::spline
