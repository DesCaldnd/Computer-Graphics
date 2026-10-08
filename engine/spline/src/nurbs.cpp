#include "nurbs.hpp"

#include <oxwald/core/assert.hpp>
#include <oxwald/spline/spline.hpp>

#include <glm/vec4.hpp>

#include <utility>

namespace ox::spline::detail {

namespace {

constexpr int kMaxP = static_cast<int>(kMaxNurbsDegree) + 1;
constexpr int kDerivs = 2;

// ders[k][j] = k-th derivative of basis N_{span-p+j, p} at u.
void basisDerivatives(int span, f64 u, int p, const std::vector<f32>& knots, f64 ders[kDerivs + 1][kMaxP]) {
    f64 ndu[kMaxP][kMaxP];
    f64 left[kMaxP];
    f64 right[kMaxP];
    ndu[0][0] = 1.0;
    for (int j = 1; j <= p; ++j) {
        left[j] = u - knots[static_cast<usize>(span + 1 - j)];
        right[j] = static_cast<f64>(knots[static_cast<usize>(span + j)]) - u;
        f64 saved = 0.0;
        for (int r = 0; r < j; ++r) {
            ndu[j][r] = right[r + 1] + left[j - r];
            const f64 temp = ndu[r][j - 1] / ndu[j][r];
            ndu[r][j] = saved + right[r + 1] * temp;
            saved = left[j - r] * temp;
        }
        ndu[j][j] = saved;
    }
    for (int j = 0; j <= p; ++j) {
        ders[0][j] = ndu[j][p];
    }
    const int n = p < kDerivs ? p : kDerivs;
    for (int k = n + 1; k <= kDerivs; ++k) {
        for (int j = 0; j <= p; ++j) {
            ders[k][j] = 0.0;
        }
    }
    f64 a[2][kMaxP];
    for (int r = 0; r <= p; ++r) {
        int s1 = 0;
        int s2 = 1;
        a[0][0] = 1.0;
        for (int k = 1; k <= n; ++k) {
            f64 d = 0.0;
            const int rk = r - k;
            const int pk = p - k;
            if (r >= k) {
                a[s2][0] = a[s1][0] / ndu[pk + 1][rk];
                d = a[s2][0] * ndu[rk][pk];
            }
            const int j1 = rk >= -1 ? 1 : -rk;
            const int j2 = (r - 1 <= pk) ? k - 1 : p - r;
            for (int j = j1; j <= j2; ++j) {
                a[s2][j] = (a[s1][j] - a[s1][j - 1]) / ndu[pk + 1][rk + j];
                d += a[s2][j] * ndu[rk + j][pk];
            }
            if (r <= pk) {
                a[s2][k] = -a[s1][k - 1] / ndu[pk + 1][r];
                d += a[s2][k] * ndu[r][pk];
            }
            ders[k][r] = d;
            std::swap(s1, s2);
        }
    }
    f64 factor = p;
    for (int k = 1; k <= n; ++k) {
        for (int j = 0; j <= p; ++j) {
            ders[k][j] *= factor;
        }
        factor *= (p - k);
    }
}

} // namespace

void nurbsEvaluate(const std::vector<glm::vec4>& homogeneous, const std::vector<f32>& knots, u32 degree, u32 span,
                   f32 u, glm::vec3* position, glm::vec3* d1, glm::vec3* d2) {
    const int p = static_cast<int>(degree);
    OX_ASSERT(p >= 1 && p <= static_cast<int>(kMaxNurbsDegree));
    OX_ASSERT(span >= degree && span + 1 < knots.size() && span < homogeneous.size());
    f64 ders[kDerivs + 1][kMaxP];
    basisDerivatives(static_cast<int>(span), u, p, knots, ders);

    glm::dvec4 aw[kDerivs + 1] = {glm::dvec4(0.0), glm::dvec4(0.0), glm::dvec4(0.0)};
    for (int k = 0; k <= kDerivs; ++k) {
        for (int j = 0; j <= p; ++j) {
            aw[k] += ders[k][j] * glm::dvec4(homogeneous[static_cast<usize>(static_cast<int>(span) - p + j)]);
        }
    }
    // Rational derivatives: C = A/w, C' = (A' - w'C)/w, C'' = (A'' - 2w'C' - w''C)/w.
    const f64 w = aw[0].w;
    const glm::dvec3 c = glm::dvec3(aw[0]) / w;
    const glm::dvec3 c1 = (glm::dvec3(aw[1]) - aw[1].w * c) / w;
    const glm::dvec3 c2 = (glm::dvec3(aw[2]) - 2.0 * aw[1].w * c1 - aw[2].w * c) / w;
    if (position) {
        *position = glm::vec3(c);
    }
    if (d1) {
        *d1 = glm::vec3(c1);
    }
    if (d2) {
        *d2 = glm::vec3(c2);
    }
}

} // namespace ox::spline::detail
