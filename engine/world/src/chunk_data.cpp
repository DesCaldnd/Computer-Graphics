#include <oxwald/world/chunk_data.hpp>

#include "crc32.hpp"

#include <bit>
#include <cstring>
#include <type_traits>

namespace ox::world {

static_assert(std::endian::native == std::endian::little, "chunk format assumes a little-endian host");

namespace {

constexpr u32 fourcc(const char (&s)[5]) { return u32(u8(s[0])) | u32(u8(s[1])) << 8 | u32(u8(s[2])) << 16 | u32(u8(s[3])) << 24; }
constexpr u32 kMagic = fourcc("OXCH");
constexpr u32 kHfld = fourcc("HFLD"), kHole = fourcc("HOLE"), kSplt = fourcc("SPLT"), kVegi = fourcc("VEGI"), kUser = fourcc("USER");

struct Writer {
    std::vector<u8> out;
    template <class T>
    void put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const usize n = out.size();
        out.resize(n + sizeof(T));
        std::memcpy(out.data() + n, &v, sizeof(T));
    }
    void bytes(std::span<const u8> b) { out.insert(out.end(), b.begin(), b.end()); }
};

struct Reader {
    std::span<const u8> in;
    usize pos = 0;
    bool ok = true;
    template <class T>
    T get() {
        T v{};
        if (pos + sizeof(T) > in.size()) {
            ok = false;
            return v;
        }
        std::memcpy(&v, in.data() + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    std::span<const u8> bytes(usize n) {
        if (pos + n > in.size()) {
            ok = false;
            return {};
        }
        auto s = in.subspan(pos, n);
        pos += n;
        return s;
    }
};

void section(Writer& w, u32 tag, const Writer& body, u32& count) {
    w.put(tag);
    w.put(u32(body.out.size()));
    w.bytes(body.out);
    w.put(detail::crc32(body.out.data(), body.out.size()));
    ++count;
}

bool fail(std::string* err, const char* msg) {
    if (err) {
        *err = msg;
    }
    return false;
}

} // namespace

std::vector<u8> serializeChunk(const ChunkData& c) {
    Writer w;
    w.put(kMagic);
    w.put(kChunkFormatVersion);
    w.put(c.coord.x);
    w.put(c.coord.z);
    const usize countPos = w.out.size();
    u32 count = 0;
    w.put(count);

    if (c.heightfield) {
        const Heightfield& hf = *c.heightfield;
        const HeightfieldDesc& d = hf.desc();
        Writer b;
        b.put(d.resolution);
        b.put(d.worldSize);
        b.put(d.heightScale);
        b.put(d.heightOffset);
        b.put(d.origin.x);
        b.put(d.origin.y);
        b.put(u32(d.format));
        b.bytes(hf.rawBytes());
        section(w, kHfld, b, count);
        if (hf.hasHoles()) {
            Writer h;
            h.bytes(hf.holeMask());
            section(w, kHole, h, count);
        }
    }
    if (c.splat) {
        const SplatMap& s = *c.splat;
        Writer b;
        b.put(s.resolution());
        b.put(s.layerCount());
        b.put(s.origin().x);
        b.put(s.origin().y);
        b.put(s.worldSize());
        b.bytes(s.raw());
        section(w, kSplt, b, count);
    }
    if (!c.vegetation.empty()) {
        Writer b;
        b.put(u32(c.vegetation.size()));
        for (const VegetationInstance& v : c.vegetation) {
            b.put(v.position);
            b.put(v.rotation.w);
            b.put(v.rotation.x);
            b.put(v.rotation.y);
            b.put(v.rotation.z);
            b.put(v.scale);
            b.put(v.tint);
            b.put(v.random);
            b.put(v.layer);
            b.put(v.prototype);
        }
        section(w, kVegi, b, count);
    }
    if (!c.userData.empty()) {
        Writer b;
        b.bytes(c.userData);
        section(w, kUser, b, count);
    }
    std::memcpy(w.out.data() + countPos, &count, sizeof(count));
    return std::move(w.out);
}

bool deserializeChunk(std::span<const u8> bytes, ChunkData& out, std::string* err) {
    Reader r{bytes};
    if (r.get<u32>() != kMagic || !r.ok) {
        return fail(err, "not a chunk file (bad magic)");
    }
    const u32 version = r.get<u32>();
    if (version == 0 || version > kChunkFormatVersion) {
        return fail(err, "unsupported chunk format version");
    }
    out.coord.x = r.get<i32>();
    out.coord.z = r.get<i32>();
    const u32 count = r.get<u32>();
    out.heightfield.reset();
    out.splat.reset();
    out.vegetation.clear();
    out.userData.clear();
    std::span<const u8> holes;
    for (u32 i = 0; i < count && r.ok; ++i) {
        const u32 tag = r.get<u32>();
        const u32 size = r.get<u32>();
        const std::span<const u8> body = r.bytes(size);
        const u32 crc = r.get<u32>();
        if (!r.ok) {
            break;
        }
        if (detail::crc32(body.data(), body.size()) != crc) {
            return fail(err, "chunk section CRC mismatch (corrupted data)");
        }
        Reader b{body};
        if (tag == kHfld) {
            HeightfieldDesc d;
            d.resolution = b.get<u32>();
            d.worldSize = b.get<f32>();
            d.heightScale = b.get<f32>();
            d.heightOffset = b.get<f32>();
            d.origin.x = b.get<f32>();
            d.origin.y = b.get<f32>();
            d.format = HeightFormat(b.get<u32>());
            if (!b.ok || d.resolution < 2 || d.resolution > 16385 || u32(d.format) > 1) {
                return fail(err, "invalid heightfield section");
            }
            Heightfield hf(d);
            auto dst = hf.rawBytesMutable();
            const auto src = b.bytes(dst.size());
            if (!b.ok) {
                return fail(err, "truncated heightfield samples");
            }
            std::memcpy(dst.data(), src.data(), dst.size());
            out.heightfield = std::move(hf);
        } else if (tag == kHole) {
            holes = body;
        } else if (tag == kSplt) {
            const u32 res = b.get<u32>(), layers = b.get<u32>();
            const f32 ox = b.get<f32>(), oz = b.get<f32>(), size = b.get<f32>();
            if (!b.ok || res < 2 || res > 16385 || layers < 1 || layers > kMaxSplatLayers) {
                return fail(err, "invalid splat section");
            }
            SplatMap s(res, layers, {ox, oz}, size);
            auto dst = s.rawMutable();
            const auto src = b.bytes(dst.size());
            if (!b.ok) {
                return fail(err, "truncated splat data");
            }
            std::memcpy(dst.data(), src.data(), dst.size());
            out.splat = std::move(s);
        } else if (tag == kVegi) {
            const u32 n = b.get<u32>();
            if (!b.ok || usize(n) * 44 > body.size()) {
                return fail(err, "invalid vegetation section");
            }
            out.vegetation.resize(n);
            for (VegetationInstance& v : out.vegetation) {
                v.position = b.get<glm::vec3>();
                v.rotation.w = b.get<f32>();
                v.rotation.x = b.get<f32>();
                v.rotation.y = b.get<f32>();
                v.rotation.z = b.get<f32>();
                v.scale = b.get<f32>();
                v.tint = b.get<u32>();
                v.random = b.get<f32>();
                v.layer = b.get<u16>();
                v.prototype = b.get<u16>();
            }
            if (!b.ok) {
                return fail(err, "truncated vegetation data");
            }
        } else if (tag == kUser) {
            out.userData.assign(body.begin(), body.end());
        }
        // Unknown tags: skipped (already consumed).
    }
    if (!r.ok) {
        return fail(err, "truncated chunk data");
    }
    if (!holes.empty() && out.heightfield) {
        const u32 res = out.heightfield->resolution();
        if (holes.size() != usize(res) * res) {
            return fail(err, "hole mask size mismatch");
        }
        for (u32 z = 0; z < res; ++z) {
            for (u32 x = 0; x < res; ++x) {
                if (holes[usize(z) * res + x]) {
                    out.heightfield->setHole(x, z, true);
                }
            }
        }
    }
    return true;
}

} // namespace ox::world
