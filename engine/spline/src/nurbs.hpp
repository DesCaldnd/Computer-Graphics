#pragma once

#include <oxwald/core/types.hpp>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <vector>

namespace ox::spline::detail {

// Rational B-spline point and first two derivatives with respect to u at knot span `span`
// (knots[span] <= u <= knots[span+1]). Piegl & Tiller, The NURBS Book, A2.3 + eq. 4.8.
void nurbsEvaluate(const std::vector<glm::vec4>& homogeneous, const std::vector<f32>& knots, u32 degree, u32 span,
                   f32 u, glm::vec3* position, glm::vec3* d1, glm::vec3* d2);

} // namespace ox::spline::detail
