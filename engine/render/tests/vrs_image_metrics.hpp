#pragma once

// Image quality metrics for the variable rate shading measurements (header-only, no engine dependencies):
//   psnr()  RGB peak signal-to-noise ratio in dB (8-bit display-encoded images)
//   ssim()  mean structural similarity of the luma (Gaussian 11×11, σ 1.5, Wang et al. 2004)
//   flip()  mean LDR-FLIP error (Andersson et al. 2020, "FLIP: A Difference Evaluator for Alternating Images"),
//           a re-implementation of the paper's LDR pipeline: CSF filtering in YyCxCz, Hunt-adjusted L*a*b*, HyAB
//           colour difference with the pc/pt remapping, edge/point feature differences, ΔE = ΔEc^(1−ΔEf).
//           Default viewing condition: 0.7 m from a 0.7 m wide 3840 px monitor (67 pixels per degree).
// Images are tightly packed RGBA8 (alpha ignored), both of the same size.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ox::vrs_metrics {

struct ImageView {
    uint32_t width = 0, height = 0;
    const uint8_t* rgba = nullptr;
};

inline double psnr(ImageView a, ImageView b) {
    const size_t n = size_t(a.width) * a.height;
    double se = 0.0;
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double d = double(a.rgba[i * 4 + c]) - double(b.rgba[i * 4 + c]);
            se += d * d;
        }
    }
    const double mse = se / double(n * 3);
    if (mse <= 1e-12) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

namespace detail {

inline constexpr double kPi = 3.14159265358979323846;
using Plane = std::vector<float>;

// Separable convolution with clamp-to-edge borders.
inline Plane convolveSeparable(const Plane& src, uint32_t w, uint32_t h, const std::vector<float>& kx, const std::vector<float>& ky) {
    const int rx = int(kx.size() / 2), ry = int(ky.size() / 2);
    Plane tmp(src.size()), dst(src.size());
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int i = -rx; i <= rx; ++i) {
                const int xx = std::clamp(int(x) + i, 0, int(w) - 1);
                s += kx[size_t(i + rx)] * src[size_t(y) * w + size_t(xx)];
            }
            tmp[size_t(y) * w + x] = s;
        }
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float s = 0.0f;
            for (int i = -ry; i <= ry; ++i) {
                const int yy = std::clamp(int(y) + i, 0, int(h) - 1);
                s += ky[size_t(i + ry)] * tmp[size_t(yy) * w + x];
            }
            dst[size_t(y) * w + x] = s;
        }
    }
    return dst;
}

inline float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

struct Vec3 {
    float x, y, z;
};

inline Vec3 linRgbToXyz(Vec3 c) {
    return {0.4124564f * c.x + 0.3575761f * c.y + 0.1804375f * c.z, 0.2126729f * c.x + 0.7151522f * c.y + 0.0721750f * c.z,
            0.0193339f * c.x + 0.1191920f * c.y + 0.9503041f * c.z};
}
inline Vec3 xyzToLinRgb(Vec3 c) {
    return {3.2404542f * c.x - 1.5371385f * c.y - 0.4985314f * c.z, -0.9692660f * c.x + 1.8760108f * c.y + 0.0415560f * c.z,
            0.0556434f * c.x - 0.2040259f * c.y + 1.0572252f * c.z};
}
inline constexpr Vec3 kWhite{0.950428545f, 1.0f, 1.088900371f}; // D65 white of linRgbToXyz(1,1,1)

inline Vec3 xyzToYcxcz(Vec3 c) {
    const float y = c.y / kWhite.y;
    return {116.0f * y - 16.0f, 500.0f * (c.x / kWhite.x - y), 200.0f * (y - c.z / kWhite.z)};
}
inline Vec3 ycxczToXyz(Vec3 c) {
    const float y = (c.x + 16.0f) / 116.0f;
    return {(c.y / 500.0f + y) * kWhite.x, y * kWhite.y, (y - c.z / 200.0f) * kWhite.z};
}
inline Vec3 xyzToLab(Vec3 c) {
    auto f = [](float t) {
        constexpr float d = 6.0f / 29.0f;
        return t > d * d * d ? std::cbrt(t) : t / (3.0f * d * d) + 4.0f / 29.0f;
    };
    const float fx = f(c.x / kWhite.x), fy = f(c.y / kWhite.y), fz = f(c.z / kWhite.z);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}
inline Vec3 huntLab(Vec3 rgbLinear) {
    Vec3 lab = xyzToLab(linRgbToXyz(rgbLinear));
    lab.y *= 0.01f * lab.x;
    lab.z *= 0.01f * lab.x;
    return lab;
}
inline float hyab(Vec3 a, Vec3 b) {
    const float da = a.y - b.y, db = a.z - b.z;
    return std::fabs(a.x - b.x) + std::sqrt(da * da + db * db);
}

struct FlipKernels {
    // CSF: per channel (A, RG, BY) up to two Gaussian terms {a, b}.
    std::vector<float> term[3][2];
    float weight[3][2]{};
    float norm[3]{};
    std::vector<float> edge, edgeSmooth, point, pointSmooth; // separable feature filters
};

inline FlipKernels makeFlipKernels(double ppd) {
    FlipKernels k;
    const double ab[3][2][2] = {{{1.0, 0.0047}, {0.0, 1e-5}}, {{1.0, 0.0053}, {0.0, 1e-5}}, {{34.1, 0.04}, {13.5, 0.025}}};
    const double maxB = 0.04;
    const int r = int(std::ceil(3.0 * std::sqrt(maxB / (2.0 * kPi * kPi)) * ppd));
    const double dx = 1.0 / ppd;
    for (int c = 0; c < 3; ++c) {
        double total = 0.0;
        for (int t = 0; t < 2; ++t) {
            const double a = ab[c][t][0], b = ab[c][t][1];
            std::vector<float> g(size_t(2 * r + 1));
            double s = 0.0;
            for (int i = -r; i <= r; ++i) {
                g[size_t(i + r)] = float(std::exp(-kPi * kPi * (i * dx) * (i * dx) / b));
                s += g[size_t(i + r)];
            }
            k.term[c][t] = g;
            k.weight[c][t] = float(a * std::sqrt(kPi / b));
            total += a * std::sqrt(kPi / b) * s * s;
        }
        k.norm[c] = float(1.0 / total);
    }
    // Feature detection (edges: first Gaussian derivative, points: second), σ = 0.5 · 0.082° · ppd.
    const double sd = 0.5 * 0.082 * ppd;
    const int fr = int(std::ceil(3.0 * sd));
    std::vector<double> g(size_t(2 * fr + 1)), xs(size_t(2 * fr + 1));
    for (int i = -fr; i <= fr; ++i) {
        xs[size_t(i + fr)] = i;
        g[size_t(i + fr)] = std::exp(-double(i * i) / (2.0 * sd * sd));
    }
    // 2D kernels are separable: Gx(x,y) = -x g(x) g(y); normalisation over the 2D kernel.
    double gs = 0.0;
    for (double v : g) gs += v;
    double posEdge = 0.0;
    for (size_t i = 0; i < g.size(); ++i) posEdge += std::max(-xs[i] * g[i], 0.0);
    posEdge *= gs;
    double posPoint = 0.0, negPoint = 0.0;
    for (size_t i = 0; i < g.size(); ++i) {
        const double v = (xs[i] * xs[i] / (sd * sd) - 1.0) * g[i];
        (v > 0 ? posPoint : negPoint) += v;
    }
    posPoint *= gs;
    negPoint *= gs;
    for (size_t i = 0; i < g.size(); ++i) {
        k.edge.push_back(float(-xs[i] * g[i] / posEdge));
        k.edgeSmooth.push_back(float(g[i]));
        const double v = (xs[i] * xs[i] / (sd * sd) - 1.0) * g[i];
        k.point.push_back(float(v > 0 ? v / posPoint : v / -negPoint));
        k.pointSmooth.push_back(float(g[i]));
    }
    return k;
}

struct FlipPrepared {
    std::vector<Vec3> lab;    // Hunt-adjusted L*a*b* of the CSF-filtered image
    Plane edges, points;      // feature magnitudes of the normalised luminance
};

inline FlipPrepared prepareFlip(ImageView img, const FlipKernels& k) {
    const uint32_t w = img.width, h = img.height;
    const size_t n = size_t(w) * h;
    Plane ch[3];
    for (auto& c : ch) c.resize(n);
    Plane lum(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec3 lin{srgbToLinear(img.rgba[i * 4] / 255.0f), srgbToLinear(img.rgba[i * 4 + 1] / 255.0f),
                       srgbToLinear(img.rgba[i * 4 + 2] / 255.0f)};
        const Vec3 y = xyzToYcxcz(linRgbToXyz(lin));
        ch[0][i] = y.x;
        ch[1][i] = y.y;
        ch[2][i] = y.z;
        lum[i] = (y.x + 16.0f) / 116.0f;
    }
    Plane filtered[3];
    for (int c = 0; c < 3; ++c) {
        filtered[c].assign(n, 0.0f);
        for (int t = 0; t < 2; ++t) {
            if (k.weight[c][t] == 0.0f) continue;
            const Plane f = convolveSeparable(ch[c], w, h, k.term[c][t], k.term[c][t]);
            for (size_t i = 0; i < n; ++i) filtered[c][i] += k.weight[c][t] * k.norm[c] * f[i];
        }
    }
    FlipPrepared p;
    p.lab.resize(n);
    for (size_t i = 0; i < n; ++i) {
        Vec3 rgb = xyzToLinRgb(ycxczToXyz({filtered[0][i], filtered[1][i], filtered[2][i]}));
        rgb = {std::clamp(rgb.x, 0.0f, 1.0f), std::clamp(rgb.y, 0.0f, 1.0f), std::clamp(rgb.z, 0.0f, 1.0f)};
        p.lab[i] = huntLab(rgb);
    }
    const Plane ex = convolveSeparable(lum, w, h, k.edge, k.edgeSmooth);
    const Plane ey = convolveSeparable(lum, w, h, k.edgeSmooth, k.edge);
    const Plane px = convolveSeparable(lum, w, h, k.point, k.pointSmooth);
    const Plane py = convolveSeparable(lum, w, h, k.pointSmooth, k.point);
    p.edges.resize(n);
    p.points.resize(n);
    for (size_t i = 0; i < n; ++i) {
        p.edges[i] = std::sqrt(ex[i] * ex[i] + ey[i] * ey[i]);
        p.points[i] = std::sqrt(px[i] * px[i] + py[i] * py[i]);
    }
    return p;
}

} // namespace detail

// Mean LDR-FLIP error in [0, 1] (0 = identical).
inline double flip(ImageView reference, ImageView test, double pixelsPerDegree = 67.0205) {
    using namespace detail;
    const FlipKernels k = makeFlipKernels(pixelsPerDegree);
    const FlipPrepared r = prepareFlip(reference, k), t = prepareFlip(test, k);
    constexpr float qc = 0.7f, qf = 0.5f, pc = 0.4f, pt = 0.95f;
    const float cmax = std::pow(hyab(huntLab({0, 1, 0}), huntLab({0, 0, 1})), qc);
    double sum = 0.0;
    const size_t n = r.lab.size();
    for (size_t i = 0; i < n; ++i) {
        const float dc = std::pow(hyab(r.lab[i], t.lab[i]), qc);
        const float ec = dc < pc * cmax ? pt / (pc * cmax) * dc : pt + (dc - pc * cmax) / (cmax - pc * cmax) * (1.0f - pt);
        const float df = std::max(std::fabs(r.edges[i] - t.edges[i]), std::fabs(r.points[i] - t.points[i]));
        const float ef = std::pow(float(1.0 / std::sqrt(2.0)) * df, qf);
        sum += std::pow(std::min(ec, 1.0f), 1.0f - std::min(ef, 1.0f));
    }
    return sum / double(n);
}

// Mean SSIM of the luma (Rec.709 weights on the display-encoded values).
inline double ssim(ImageView a, ImageView b) {
    using namespace detail;
    const uint32_t w = a.width, h = a.height;
    const size_t n = size_t(w) * h;
    Plane x(n), y(n), xx(n), yy(n), xy(n);
    for (size_t i = 0; i < n; ++i) {
        x[i] = 0.2126f * a.rgba[i * 4] + 0.7152f * a.rgba[i * 4 + 1] + 0.0722f * a.rgba[i * 4 + 2];
        y[i] = 0.2126f * b.rgba[i * 4] + 0.7152f * b.rgba[i * 4 + 1] + 0.0722f * b.rgba[i * 4 + 2];
        xx[i] = x[i] * x[i];
        yy[i] = y[i] * y[i];
        xy[i] = x[i] * y[i];
    }
    std::vector<float> g(11);
    float s = 0.0f;
    for (int i = -5; i <= 5; ++i) s += g[size_t(i + 5)] = std::exp(-float(i * i) / (2.0f * 1.5f * 1.5f));
    for (float& v : g) v /= s;
    const Plane mx = convolveSeparable(x, w, h, g, g), my = convolveSeparable(y, w, h, g, g);
    const Plane sxx = convolveSeparable(xx, w, h, g, g), syy = convolveSeparable(yy, w, h, g, g);
    const Plane sxy = convolveSeparable(xy, w, h, g, g);
    constexpr double c1 = (0.01 * 255) * (0.01 * 255), c2 = (0.03 * 255) * (0.03 * 255);
    double sum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double ux = mx[i], uy = my[i];
        const double vx = sxx[i] - ux * ux, vy = syy[i] - uy * uy, cxy = sxy[i] - ux * uy;
        sum += ((2 * ux * uy + c1) * (2 * cxy + c2)) / ((ux * ux + uy * uy + c1) * (vx + vy + c2));
    }
    return sum / double(n);
}

} // namespace ox::vrs_metrics
