#pragma once

// Little-endian byte streams for the raw-blob asset formats (.oxmesh, .oxtex, .oxpak).
// Readers are bounds-checked: any out-of-range read sets failed() and yields zeros.

#include <oxwald/core/types.hpp>
#include <oxwald/core/uuid.hpp>

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ox::assets {

class ByteWriter {
public:
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void write(const T& v) {
        writeBytes(&v, sizeof(T));
    }
    void writeBytes(const void* data, usize size) {
        if (size == 0) return;
        const auto* p = static_cast<const std::byte*>(data);
        m_data.insert(m_data.end(), p, p + size);
    }
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void writeSpan(std::span<const T> items) {
        writeBytes(items.data(), items.size_bytes());
    }
    void writeString(std::string_view s) {
        write(static_cast<u32>(s.size()));
        writeBytes(s.data(), s.size());
    }
    void writeUuid(const Uuid& id) {
        write(id.hi);
        write(id.lo);
    }
    void align(usize alignment) {
        while (m_data.size() % alignment != 0) m_data.push_back(std::byte{0});
    }
    // Overwrites previously written bytes (e.g. patching offsets).
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void patch(usize offset, const T& v) {
        std::memcpy(m_data.data() + offset, &v, sizeof(T));
    }

    [[nodiscard]] usize size() const { return m_data.size(); }
    [[nodiscard]] const std::vector<std::byte>& data() const { return m_data; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(m_data); }

private:
    std::vector<std::byte> m_data;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) : m_data(data) {}

    template <class T>
        requires std::is_trivially_copyable_v<T>
    T read() {
        T v{};
        readBytes(&v, sizeof(T));
        return v;
    }
    bool readBytes(void* out, usize size) {
        if (size == 0) return !m_failed;
        if (m_failed || m_pos + size > m_data.size() || m_pos + size < m_pos) {
            m_failed = true;
            std::memset(out, 0, size);
            return false;
        }
        std::memcpy(out, m_data.data() + m_pos, size);
        m_pos += size;
        return true;
    }
    template <class T>
        requires std::is_trivially_copyable_v<T>
    bool readVector(std::vector<T>& out, usize count) {
        if (m_failed || count > (m_data.size() - m_pos) / (sizeof(T) ? sizeof(T) : 1)) {
            m_failed = true;
            out.clear();
            return false;
        }
        out.resize(count);
        return readBytes(out.data(), count * sizeof(T));
    }
    std::string readString() {
        const u32 n = read<u32>();
        if (m_failed || n > m_data.size() - m_pos) {
            m_failed = true;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(m_data.data() + m_pos), n);
        m_pos += n;
        return s;
    }
    Uuid readUuid() {
        Uuid id;
        id.hi = read<u64>();
        id.lo = read<u64>();
        return id;
    }
    void seek(usize pos) {
        if (pos > m_data.size()) m_failed = true;
        else m_pos = pos;
    }
    void skip(usize n) { seek(m_pos + n); }

    [[nodiscard]] usize position() const { return m_pos; }
    [[nodiscard]] usize remaining() const { return m_failed ? 0 : m_data.size() - m_pos; }
    [[nodiscard]] bool failed() const { return m_failed; }
    [[nodiscard]] std::span<const std::byte> data() const { return m_data; }

private:
    std::span<const std::byte> m_data;
    usize m_pos = 0;
    bool m_failed = false;
};

} // namespace ox::assets
