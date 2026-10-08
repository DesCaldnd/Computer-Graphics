#pragma once

#include <oxwald/net/bit_stream.hpp>

#include <glm/vec4.hpp>

#include <algorithm>
#include <concepts>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace ox::net {

// FNV-1a; used for message ids, RPC ids and replicated type ids. Stable across platforms and builds.
constexpr u32 netHash(std::string_view s) {
    u32 h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<u8>(c);
        h *= 16777619u;
    }
    return h;
}

// A codec knows how to write/read a T and when two values are "the same on the wire" (quantising codecs compare
// quantised values so sub-resolution jitter does not cause resends).
template <class C, class T>
concept NetCodecFor = requires(const C& c, BitWriter& w, BitReader& r, const T& a, T& out) {
    c.write(w, a);
    c.read(r, out);
    { c.equal(a, a) } -> std::convertible_to<bool>;
};

template <class T, class = void>
struct NetCodec; // default codec, specialised below. Specialise for your own types.

template <class T>
struct NetCodec<T, std::enable_if_t<std::is_same_v<T, bool>>> {
    void write(BitWriter& w, bool v) const { w.writeBool(v); }
    void read(BitReader& r, bool& v) const { v = r.readBool(); }
    bool equal(bool a, bool b) const { return a == b; }
};

template <class T>
struct NetCodec<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    void write(BitWriter& w, T v) const {
        if constexpr (std::is_signed_v<T>) {
            w.writeVarI64(static_cast<i64>(v));
        } else {
            w.writeVarU64(static_cast<u64>(v));
        }
    }
    void read(BitReader& r, T& v) const {
        if constexpr (std::is_signed_v<T>) {
            v = static_cast<T>(r.readVarI64());
        } else {
            v = static_cast<T>(r.readVarU64());
        }
    }
    bool equal(T a, T b) const { return a == b; }
};

template <class T>
struct NetCodec<T, std::enable_if_t<std::is_enum_v<T>>> {
    using U = std::underlying_type_t<T>;
    void write(BitWriter& w, T v) const { NetCodec<U>{}.write(w, static_cast<U>(v)); }
    void read(BitReader& r, T& v) const {
        U u{};
        NetCodec<U>{}.read(r, u);
        v = static_cast<T>(u);
    }
    bool equal(T a, T b) const { return a == b; }
};

template <>
struct NetCodec<f32> {
    void write(BitWriter& w, f32 v) const { w.writeF32(v); }
    void read(BitReader& r, f32& v) const { v = r.readF32(); }
    bool equal(f32 a, f32 b) const { return a == b; }
};

template <>
struct NetCodec<f64> {
    void write(BitWriter& w, f64 v) const { w.writeF64(v); }
    void read(BitReader& r, f64& v) const { v = r.readF64(); }
    bool equal(f64 a, f64 b) const { return a == b; }
};

template <>
struct NetCodec<std::string> {
    void write(BitWriter& w, const std::string& v) const { w.writeString(v); }
    void read(BitReader& r, std::string& v) const { v = r.readString(); }
    bool equal(const std::string& a, const std::string& b) const { return a == b; }
};

template <glm::length_t N>
struct NetCodec<glm::vec<N, f32, glm::defaultp>> {
    using V = glm::vec<N, f32, glm::defaultp>;
    void write(BitWriter& w, const V& v) const {
        for (glm::length_t i = 0; i < N; ++i) w.writeF32(v[i]);
    }
    void read(BitReader& r, V& v) const {
        for (glm::length_t i = 0; i < N; ++i) v[i] = r.readF32();
    }
    bool equal(const V& a, const V& b) const { return a == b; }
};

template <>
struct NetCodec<glm::quat> {
    void write(BitWriter& w, const glm::quat& v) const { w.writeQuatRaw(v); }
    void read(BitReader& r, glm::quat& v) const { v = r.readQuatRaw(); }
    bool equal(const glm::quat& a, const glm::quat& b) const { return a == b; }
};

// ---- containers: element count + elements (each with the element type's default codec)

inline constexpr u32 kMaxReplicatedElements = 1u << 16; // malformed packets must not allocate unbounded memory

template <class T>
struct NetCodec<std::vector<T>> {
    void write(BitWriter& w, const std::vector<T>& v) const {
        w.writeVarU32(static_cast<u32>(v.size()));
        for (const T& e : v) NetCodec<T>{}.write(w, e);
    }
    void read(BitReader& r, std::vector<T>& v) const {
        const u64 n = r.readVarU64();
        v.clear();
        if (!r.ok() || n > kMaxReplicatedElements) return;
        v.resize(static_cast<usize>(n));
        for (T& e : v) {
            NetCodec<T>{}.read(r, e);
            if (!r.ok()) return;
        }
    }
    bool equal(const std::vector<T>& a, const std::vector<T>& b) const {
        if (a.size() != b.size()) return false;
        for (usize i = 0; i < a.size(); ++i) {
            if (!NetCodec<T>{}.equal(a[i], b[i])) return false;
        }
        return true;
    }
};

template <class Map>
struct NetStringMapCodec {
    using T = typename Map::mapped_type;
    void write(BitWriter& w, const Map& m) const {
        w.writeVarU32(static_cast<u32>(m.size()));
        // Sorted keys: equal maps always produce identical bits (unordered_map iteration order varies).
        std::vector<const typename Map::value_type*> items;
        items.reserve(m.size());
        for (const auto& kv : m) items.push_back(&kv);
        std::sort(items.begin(), items.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
        for (const auto* kv : items) {
            w.writeString(kv->first);
            NetCodec<T>{}.write(w, kv->second);
        }
    }
    void read(BitReader& r, Map& m) const {
        const u64 n = r.readVarU64();
        m.clear();
        if (!r.ok() || n > kMaxReplicatedElements) return;
        for (u64 i = 0; i < n; ++i) {
            std::string key = r.readString();
            T value{};
            NetCodec<T>{}.read(r, value);
            if (!r.ok()) return;
            m.emplace(std::move(key), std::move(value));
        }
    }
    bool equal(const Map& a, const Map& b) const {
        if (a.size() != b.size()) return false;
        for (const auto& [k, v] : a) {
            auto it = b.find(k);
            if (it == b.end() || !NetCodec<T>{}.equal(v, it->second)) return false;
        }
        return true;
    }
};

template <class T>
struct NetCodec<std::map<std::string, T>> : NetStringMapCodec<std::map<std::string, T>> {};
template <class T>
struct NetCodec<std::unordered_map<std::string, T>> : NetStringMapCodec<std::unordered_map<std::string, T>> {};

// ---- quantising codecs (use them explicitly in NetObject::replicate)

struct QuantizedFloatCodec {
    f32 min = -1.f, max = 1.f, resolution = 0.01f;
    u32 index(f32 v) const { return quantizeFloat(v, min, max, quantizedFloatSteps(min, max, resolution)); }
    void write(BitWriter& w, f32 v) const { w.writeQuantizedFloat(v, min, max, resolution); }
    void read(BitReader& r, f32& v) const { v = r.readQuantizedFloat(min, max, resolution); }
    bool equal(f32 a, f32 b) const { return index(a) == index(b); }
};

struct QuantizedVec3Codec {
    f32 min = -1024.f, max = 1024.f, resolution = 0.01f;
    void write(BitWriter& w, const glm::vec3& v) const { w.writeQuantizedVec3(v, min, max, resolution); }
    void read(BitReader& r, glm::vec3& v) const { v = r.readQuantizedVec3(min, max, resolution); }
    bool equal(const glm::vec3& a, const glm::vec3& b) const {
        const QuantizedFloatCodec f{min, max, resolution};
        return f.equal(a.x, b.x) && f.equal(a.y, b.y) && f.equal(a.z, b.z);
    }
};

struct CompressedQuatCodec {
    u32 bitsPerComponent = 10;
    void write(BitWriter& w, const glm::quat& v) const { w.writeQuat(v, bitsPerComponent); }
    void read(BitReader& r, glm::quat& v) const { v = r.readQuat(bitsPerComponent); }
    bool equal(const glm::quat& a, const glm::quat& b) const {
        // Same rotation within quantisation precision (q and -q are equal).
        const f32 tolerance = 1.f / static_cast<f32>(1u << bitsPerComponent);
        return 1.f - std::abs(glm::dot(a, b)) < tolerance * tolerance;
    }
};

struct RangedIntCodec {
    i64 min = 0, max = 255;
    void write(BitWriter& w, i32 v) const { w.writeRangedInt(v, min, max); }
    void read(BitReader& r, i32& v) const { v = static_cast<i32>(r.readRangedInt(min, max)); }
    bool equal(i32 a, i32 b) const { return a == b; }
};

template <class T>
void netWrite(BitWriter& w, const T& v) {
    NetCodec<T>{}.write(w, v);
}

template <class T>
void netRead(BitReader& r, T& v) {
    NetCodec<T>{}.read(r, v);
}

} // namespace ox::net
