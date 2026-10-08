#pragma once

#include <oxwald/animation/compact_clip.hpp>
#include <oxwald/animation/skinning.hpp>

#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Simple little-endian byte stream used until the engine-wide archive system (ox::serial) lands.
// Every blob starts with a 4-byte magic and a u32 version.
namespace ox::anim {

class ByteWriter {
public:
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void write(const T& v) {
        const auto* p = reinterpret_cast<const u8*>(&v);
        m_data.insert(m_data.end(), p, p + sizeof(T));
    }
    void writeBytes(const void* data, usize size);
    void writeString(const std::string& s);
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void writeVector(const std::vector<T>& v) {
        write(static_cast<u32>(v.size()));
        writeBytes(v.data(), v.size() * sizeof(T));
    }

    const std::vector<u8>& data() const { return m_data; }
    std::vector<u8> take() { return std::move(m_data); }

private:
    std::vector<u8> m_data;
};

// Bounds-checked reader: on overflow it sets the failure flag and returns zeroed values.
class ByteReader {
public:
    explicit ByteReader(std::span<const u8> data) : m_data(data) {}

    template <class T>
        requires std::is_trivially_copyable_v<T>
    T read() {
        T v{};
        readBytes(&v, sizeof(T));
        return v;
    }
    bool readBytes(void* out, usize size);
    std::string readString();
    template <class T>
        requires std::is_trivially_copyable_v<T>
    std::vector<T> readVector() {
        const u32 n = read<u32>();
        std::vector<T> v;
        if (m_failed || static_cast<u64>(n) * sizeof(T) > remaining()) {
            m_failed = true;
            return v;
        }
        v.resize(n);
        readBytes(v.data(), n * sizeof(T));
        return v;
    }

    bool failed() const { return m_failed; }
    usize remaining() const { return m_data.size() - m_pos; }

private:
    std::span<const u8> m_data;
    usize m_pos = 0;
    bool m_failed = false;
};

void serialize(ByteWriter& w, const Skeleton& skeleton);
void serialize(ByteWriter& w, const AnimationClip& clip);
void serialize(ByteWriter& w, const CompactClip& clip);
void serialize(ByteWriter& w, const SkinnedMeshData& mesh);

bool deserialize(ByteReader& r, Skeleton& skeleton);
bool deserialize(ByteReader& r, AnimationClip& clip);
bool deserialize(ByteReader& r, CompactClip& clip);
bool deserialize(ByteReader& r, SkinnedMeshData& mesh);

} // namespace ox::anim
