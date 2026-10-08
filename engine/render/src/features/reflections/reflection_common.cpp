// Shared helpers of the reflections-ao area: capture matrices, oblique projection, baked data containers.
#include "reflection_internal.hpp"

#include <oxwald/core/hash.hpp>

#include <cstring>
#include <fstream>

namespace ox::render::reflections {

// --- helpers ------------------------------------------------------------------------------------------------

rhi::TextureDesc textureDesc(VkFormat format, Extent2D e, const char* name, u32 mips) {
    rhi::TextureDesc d;
    d.format = format;
    d.width = std::max(e.width, 1u);
    d.height = std::max(e.height, 1u);
    d.mipLevels = mips;
    d.usage = rhi::TextureUsage::None;
    d.name = name;
    return d;
}

u64 frameCounter(FeatureContext& ctx) { return rendererImpl(ctx).frameCounter; }

glm::mat4 removeScale(const glm::mat4& world) {
    glm::mat4 m = world;
    for (int c = 0; c < 3; ++c) {
        const glm::vec3 axis(m[c]);
        const f32 len = glm::length(axis);
        m[c] = glm::vec4(len > 1e-8f ? axis / len : glm::vec3(c == 0, c == 1, c == 2), 0.0f);
    }
    return m;
}

glm::mat4 rigidInverse(const glm::mat4& world) { return glm::inverse(removeScale(world)); }

u64 hashBytes(const void* data, usize size, u64 seed) {
    return hashCombine(seed, fnv1a64(std::span(static_cast<const std::byte*>(data), size)));
}

rhi::RGTexture ImportCache::get(FeatureContext& ctx, rhi::TextureHandle texture) {
    const u64 key = texture.packed();
    auto it = m_map.find(key);
    if (it != m_map.end()) return it->second;
    const rhi::RGTexture t = importPersistent(ctx.graph(), ctx.device(), texture);
    m_map.emplace(key, t);
    return t;
}

// --- math ---------------------------------------------------------------------------------------------------

void cubeFaceMatrices(u32 face, glm::vec3 position, f32 nearPlane, glm::mat4& view, glm::mat4& proj) {
    // Axes of oxCubeDirection: dir = F + ndc.x · U + ndc.y · Y.
    static const glm::vec3 kF[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const glm::vec3 kU[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    static const glm::vec3 kY[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    face = std::min(face, 5u);
    const glm::vec3 F = kF[face], U = kU[face], Y = kY[face];
    view = glm::mat4(1.0f);
    view[0] = glm::vec4(U.x, Y.x, -F.x, 0.0f);
    view[1] = glm::vec4(U.y, Y.y, -F.y, 0.0f);
    view[2] = glm::vec4(U.z, Y.z, -F.z, 0.0f);
    view[3] = glm::vec4(-glm::dot(U, position), -glm::dot(Y, position), glm::dot(F, position), 1.0f);
    // clip = (x, y, near, -z): ndc.xy = view xy / forward distance (90° fov, no Y flip: Y already points down the
    // texel rows), reversed-Z infinite.
    proj = glm::mat4(0.0f);
    proj[0][0] = 1.0f;
    proj[1][1] = 1.0f;
    proj[2][3] = -1.0f;
    proj[3][2] = std::max(nearPlane, 1e-4f);
}

glm::mat4 reflectionMatrix(glm::vec4 plane) {
    const glm::vec3 n(plane);
    const f32 d = plane.w;
    glm::mat4 m(1.0f);
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) m[c][r] -= 2.0f * n[r] * n[c];
    }
    m[3] = glm::vec4(-2.0f * d * n, 1.0f);
    return m;
}

namespace {
glm::vec4 row(const glm::mat4& m, int r) { return {m[0][r], m[1][r], m[2][r], m[3][r]}; }
void setRow(glm::mat4& m, int r, glm::vec4 v) {
    for (int c = 0; c < 4; ++c) m[c][r] = v[c];
}
} // namespace

glm::mat4 finiteReversedZ(const glm::mat4& proj, f32 n, f32 f) {
    glm::mat4 p = proj;
    n = std::max(n, 1e-4f);
    f = std::max(f, n * 2.0f);
    // Conventional [0,1] depth row, then reversed: z' = w − z.
    const glm::vec4 conv{0.0f, 0.0f, f / (n - f), n * f / (n - f)};
    setRow(p, 3, {0.0f, 0.0f, -1.0f, 0.0f});
    setRow(p, 2, row(p, 3) - conv);
    return p;
}

glm::mat4 obliqueReversedZ(const glm::mat4& proj, glm::vec4 c) {
    glm::mat4 conv = proj;
    setRow(conv, 2, row(proj, 3) - row(proj, 2)); // back to conventional [0,1] depth
    const glm::vec4 cc = glm::transpose(glm::inverse(conv)) * c;
    const glm::vec4 q = glm::inverse(conv) * glm::vec4(cc.x >= 0.0f ? 1.0f : -1.0f, cc.y >= 0.0f ? 1.0f : -1.0f, 1.0f, 1.0f);
    const f32 cq = glm::dot(c, q);
    if (std::abs(cq) < 1e-12f) return proj;
    setRow(conv, 2, c / cq);
    glm::mat4 out = conv;
    setRow(out, 2, row(conv, 3) - row(conv, 2));
    return out;
}

// --- baked containers ---------------------------------------------------------------------------------------

namespace {

constexpr u32 kCubeMagic = 0x4243584Fu; // "OXCB"
constexpr u32 kIrrMagic = 0x5249584Fu;  // "OXIR"
constexpr u32 kVersion = 1;

template <class T>
void put(std::vector<u8>& out, const T& v) {
    const auto* p = reinterpret_cast<const u8*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

struct Reader {
    const std::vector<u8>& data;
    usize offset = 0;
    template <class T>
    bool get(T& v) {
        if (offset + sizeof(T) > data.size()) return false;
        std::memcpy(&v, data.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }
    bool bytes(void* dst, usize n) {
        if (offset + n > data.size()) return false;
        std::memcpy(dst, data.data() + offset, n);
        offset += n;
        return true;
    }
};

Status writeFile(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return makeError("cannot write {}", tmp.string());
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) return makeError("write failed: {}", tmp.string());
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) return makeError("rename {} failed: {}", path.string(), ec.message());
    return {};
}

Result<std::vector<u8>> readFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return makeError("cannot open {}", path.string());
    std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return bytes;
}

usize cubeBytes(u32 size, u32 mips) {
    usize n = 0;
    for (u32 m = 0; m < mips; ++m) {
        const usize s = std::max(size >> m, 1u);
        n += s * s * 6 * 8;
    }
    return n;
}

} // namespace

Status saveOxCube(const std::filesystem::path& path, const BakedCubemap& cube) {
    if (!cube.valid() || cube.data.size() != cubeBytes(cube.size, cube.mips)) return makeError("invalid cubemap data");
    std::vector<u8> out;
    put(out, kCubeMagic);
    put(out, kVersion);
    put(out, cube.size);
    put(out, cube.mips);
    put(out, u64(cube.data.size()));
    out.insert(out.end(), cube.data.begin(), cube.data.end());
    return writeFile(path, out);
}

Result<BakedCubemap> loadOxCube(const std::filesystem::path& path) {
    auto bytes = readFile(path);
    if (!bytes) return bytes.error();
    Reader r{*bytes};
    u32 magic = 0, version = 0;
    u64 n = 0;
    BakedCubemap c;
    if (!r.get(magic) || magic != kCubeMagic) return makeError("{}: not an .oxcube file", path.string());
    if (!r.get(version) || version != kVersion) return makeError("{}: unsupported version {}", path.string(), version);
    if (!r.get(c.size) || !r.get(c.mips) || !r.get(n)) return makeError("{}: truncated header", path.string());
    if (c.size == 0 || c.size > 4096 || c.mips == 0 || c.mips > 13 || n != cubeBytes(c.size, c.mips)) {
        return makeError("{}: inconsistent header", path.string());
    }
    c.data.resize(n);
    if (!r.bytes(c.data.data(), n)) return makeError("{}: truncated data", path.string());
    return c;
}

Status saveOxIrradiance(const std::filesystem::path& path, const BakedIrradianceVolume& v) {
    const u64 probes = u64(v.probeCount.x) * v.probeCount.y * v.probeCount.z;
    if (!v.valid() || v.probes.size() != probes) return makeError("invalid irradiance volume data");
    std::vector<u8> out;
    put(out, kIrrMagic);
    put(out, kVersion);
    put(out, v.probeCount);
    put(out, u64(v.probes.size()));
    put(out, u64(v.moments.size()));
    const auto* p = reinterpret_cast<const u8*>(v.probes.data());
    out.insert(out.end(), p, p + v.probes.size() * sizeof(GpuIrradianceProbe));
    out.insert(out.end(), v.moments.begin(), v.moments.end());
    return writeFile(path, out);
}

Result<BakedIrradianceVolume> loadOxIrradiance(const std::filesystem::path& path) {
    auto bytes = readFile(path);
    if (!bytes) return bytes.error();
    Reader r{*bytes};
    u32 magic = 0, version = 0;
    u64 probes = 0, moments = 0;
    BakedIrradianceVolume v;
    if (!r.get(magic) || magic != kIrrMagic) return makeError("{}: not an .oxirr file", path.string());
    if (!r.get(version) || version != kVersion) return makeError("{}: unsupported version {}", path.string(), version);
    if (!r.get(v.probeCount) || !r.get(probes) || !r.get(moments)) return makeError("{}: truncated header", path.string());
    if (glm::any(glm::lessThan(v.probeCount, glm::ivec3(1))) ||
        probes != u64(v.probeCount.x) * v.probeCount.y * v.probeCount.z || probes > (1u << 20) || moments > (1ull << 32)) {
        return makeError("{}: inconsistent header", path.string());
    }
    v.probes.resize(probes);
    v.moments.resize(moments);
    if (!r.bytes(v.probes.data(), probes * sizeof(GpuIrradianceProbe)) || !r.bytes(v.moments.data(), moments)) {
        return makeError("{}: truncated data", path.string());
    }
    return v;
}

} // namespace ox::render::reflections
