#include <oxwald/world/heightfield.hpp>

#include <oxwald/core/assert.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace ox::world {

namespace {
inline u16 toU16(f32 v) { return u16(std::lround(glm::clamp(v, 0.f, 1.f) * 65535.f)); }
inline f32 fromU16(u16 v) { return f32(v) * (1.f / 65535.f); }
} // namespace

Heightfield::Heightfield(const HeightfieldDesc& desc) : m_desc(desc) {
    OX_ASSERT(desc.resolution >= 2, "Heightfield resolution must be >= 2 (got {})", desc.resolution);
    const usize n = usize(desc.resolution) * desc.resolution;
    if (desc.format == HeightFormat::Float32) {
        m_f32.assign(n, 0.f);
    } else {
        m_u16.assign(n, 0);
    }
}

void Heightfield::setScale(f32 heightScale, f32 heightOffset) {
    m_desc.heightScale = heightScale;
    m_desc.heightOffset = heightOffset;
}

f32 Heightfield::normalized(u32 x, u32 z) const {
    const usize i = index(x, z);
    return m_desc.format == HeightFormat::Float32 ? m_f32[i] : fromU16(m_u16[i]);
}

void Heightfield::setNormalized(u32 x, u32 z, f32 v) {
    const usize i = index(x, z);
    if (m_desc.format == HeightFormat::Float32) {
        m_f32[i] = v;
    } else {
        m_u16[i] = toU16(v);
    }
}

f32 Heightfield::heightAtSample(i32 x, i32 z) const {
    const i32 r = i32(m_desc.resolution) - 1;
    x = std::clamp(x, 0, r);
    z = std::clamp(z, 0, r);
    return m_desc.heightOffset + normalized(u32(x), u32(z)) * m_desc.heightScale;
}

void Heightfield::setHeightAtSample(u32 x, u32 z, f32 worldHeight) {
    const f32 s = m_desc.heightScale != 0.f ? m_desc.heightScale : 1.f;
    setNormalized(x, z, (worldHeight - m_desc.heightOffset) / s);
}

glm::vec2 Heightfield::worldToSample(glm::vec2 w) const { return (w - m_desc.origin) / spacing(); }
glm::vec2 Heightfield::sampleToWorld(glm::vec2 s) const { return m_desc.origin + s * spacing(); }

glm::vec3 Heightfield::samplePosition(u32 x, u32 z) const {
    const glm::vec2 w = sampleToWorld(glm::vec2(f32(x), f32(z)));
    return {w.x, heightAtSample(i32(x), i32(z)), w.y};
}

bool Heightfield::containsWorld(glm::vec2 w) const {
    const glm::vec2 l = w - m_desc.origin;
    return l.x >= 0.f && l.y >= 0.f && l.x <= m_desc.worldSize && l.y <= m_desc.worldSize;
}

f32 Heightfield::sampleHeight(glm::vec2 worldXZ) const {
    const f32 r = f32(m_desc.resolution - 1);
    const glm::vec2 s = glm::clamp(worldToSample(worldXZ), glm::vec2(0.f), glm::vec2(r));
    const i32 x0 = std::min(i32(s.x), i32(r) - 1), z0 = std::min(i32(s.y), i32(r) - 1);
    const f32 fx = s.x - f32(x0), fz = s.y - f32(z0);
    const f32 h00 = heightAtSample(x0, z0), h10 = heightAtSample(x0 + 1, z0);
    const f32 h01 = heightAtSample(x0, z0 + 1), h11 = heightAtSample(x0 + 1, z0 + 1);
    return glm::mix(glm::mix(h00, h10, fx), glm::mix(h01, h11, fx), fz);
}

glm::vec3 Heightfield::sampleNormal(glm::vec2 w) const {
    const f32 d = spacing();
    const f32 dx = sampleHeight(w + glm::vec2(d, 0.f)) - sampleHeight(w - glm::vec2(d, 0.f));
    const f32 dz = sampleHeight(w + glm::vec2(0.f, d)) - sampleHeight(w - glm::vec2(0.f, d));
    return glm::normalize(glm::vec3(-dx, 2.f * d, -dz));
}

glm::vec3 Heightfield::sampleNormalAtSample(u32 x, u32 z) const {
    const i32 xi = i32(x), zi = i32(z);
    const f32 d = spacing();
    const f32 dx = heightAtSample(xi + 1, zi) - heightAtSample(xi - 1, zi);
    const f32 dz = heightAtSample(xi, zi + 1) - heightAtSample(xi, zi - 1);
    return glm::normalize(glm::vec3(-dx, 2.f * d, -dz));
}

f32 Heightfield::sampleSlope(glm::vec2 w) const {
    return std::acos(glm::clamp(sampleNormal(w).y, -1.f, 1.f));
}

void Heightfield::minMaxInRect(const IRect& rect, f32& outMin, f32& outMax) const {
    const IRect r = rect.intersected(fullRect());
    f32 lo = std::numeric_limits<f32>::max(), hi = std::numeric_limits<f32>::lowest();
    for (i32 z = r.z0; z < r.z1; ++z) {
        for (i32 x = r.x0; x < r.x1; ++x) {
            const f32 n = normalized(u32(x), u32(z));
            lo = std::min(lo, n);
            hi = std::max(hi, n);
        }
    }
    if (r.empty()) {
        lo = hi = 0.f;
    }
    const f32 a = m_desc.heightOffset + lo * m_desc.heightScale;
    const f32 b = m_desc.heightOffset + hi * m_desc.heightScale;
    outMin = std::min(a, b);
    outMax = std::max(a, b);
}

f32 Heightfield::minHeight() const {
    f32 lo, hi;
    minMaxInRect(fullRect(), lo, hi);
    return lo;
}

f32 Heightfield::maxHeight() const {
    f32 lo, hi;
    minMaxInRect(fullRect(), lo, hi);
    return hi;
}

bool Heightfield::isHole(u32 x, u32 z) const { return !m_holes.empty() && m_holes[index(x, z)] != 0; }

void Heightfield::setHole(u32 x, u32 z, bool hole) {
    if (m_holes.empty()) {
        if (!hole) {
            return;
        }
        m_holes.assign(usize(m_desc.resolution) * m_desc.resolution, 0);
    }
    m_holes[index(x, z)] = hole ? 1 : 0;
}

std::vector<f32> Heightfield::toNormalizedFloats() const {
    if (m_desc.format == HeightFormat::Float32) {
        return m_f32;
    }
    std::vector<f32> out(m_u16.size());
    std::transform(m_u16.begin(), m_u16.end(), out.begin(), fromU16);
    return out;
}

void Heightfield::fromNormalizedFloats(std::span<const f32> values) {
    OX_ASSERT(values.size() == usize(m_desc.resolution) * m_desc.resolution, "size mismatch");
    if (m_desc.format == HeightFormat::Float32) {
        m_f32.assign(values.begin(), values.end());
    } else {
        std::transform(values.begin(), values.end(), m_u16.begin(), toU16);
    }
}

std::vector<f32> Heightfield::worldHeights(const IRect& rect) const {
    std::vector<f32> out;
    out.reserve(usize(std::max(0, rect.width())) * usize(std::max(0, rect.height())));
    for (i32 z = rect.z0; z < rect.z1; ++z) {
        for (i32 x = rect.x0; x < rect.x1; ++x) {
            out.push_back(heightAtSample(x, z));
        }
    }
    return out;
}

std::span<const u8> Heightfield::rawBytes() const {
    if (m_desc.format == HeightFormat::Float32) {
        return {reinterpret_cast<const u8*>(m_f32.data()), m_f32.size() * 4};
    }
    return {reinterpret_cast<const u8*>(m_u16.data()), m_u16.size() * 2};
}

std::span<u8> Heightfield::rawBytesMutable() {
    if (m_desc.format == HeightFormat::Float32) {
        return {reinterpret_cast<u8*>(m_f32.data()), m_f32.size() * 4};
    }
    return {reinterpret_cast<u8*>(m_u16.data()), m_u16.size() * 2};
}

std::vector<u16> Heightfield::extractR16(const IRect& rect) const {
    const IRect r = rect.intersected(fullRect());
    std::vector<u16> out;
    out.reserve(usize(r.width()) * usize(r.height()));
    for (i32 z = r.z0; z < r.z1; ++z) {
        for (i32 x = r.x0; x < r.x1; ++x) {
            out.push_back(toU16(normalized(u32(x), u32(z))));
        }
    }
    return out;
}

Heightfield Heightfield::extractTile(const IRect& rect) const {
    const IRect r = rect.intersected(fullRect());
    OX_ASSERT(r.width() == r.height() && r.width() >= 2, "extractTile needs a square rect of >= 2 samples");
    HeightfieldDesc d = m_desc;
    d.resolution = u32(r.width());
    d.worldSize = spacing() * f32(r.width() - 1);
    d.origin = sampleToWorld(glm::vec2(f32(r.x0), f32(r.z0)));
    Heightfield t(d);
    for (i32 z = 0; z < r.height(); ++z) {
        for (i32 x = 0; x < r.width(); ++x) {
            t.setNormalized(u32(x), u32(z), normalized(u32(r.x0 + x), u32(r.z0 + z)));
            if (isHole(u32(r.x0 + x), u32(r.z0 + z))) {
                t.setHole(u32(x), u32(z), true);
            }
        }
    }
    return t;
}

void Heightfield::convertFormat(HeightFormat format) {
    if (format == m_desc.format) {
        return;
    }
    std::vector<f32> v = toNormalizedFloats();
    m_desc.format = format;
    m_f32.clear();
    m_u16.clear();
    if (format == HeightFormat::Float32) {
        m_f32 = std::move(v);
    } else {
        m_u16.resize(v.size());
        std::transform(v.begin(), v.end(), m_u16.begin(), toU16);
    }
}

} // namespace ox::world
