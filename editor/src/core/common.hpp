#pragma once

#include <oxwald/core/uuid.hpp>

#include <QHashFunctions>
#include <QList>
#include <QMetaType>
#include <QString>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ox {
inline size_t qHash(const Uuid& id, size_t seed = 0) noexcept { return ::qHash(quint64(id.hi ^ (id.lo * 31u)), seed); }
} // namespace ox

namespace ox::editor {

using UuidList = std::vector<Uuid>;

enum class EditPhase {
    Single, // one-shot edit (typing + enter, checkbox, combo)
    Begin,  // first change of a continuous edit (drag) - later Updates merge into it
    Update,
    End,    // closes the continuous edit
};

inline QString qs(std::string_view s) { return QString::fromUtf8(s.data(), qsizetype(s.size())); }
inline std::string ss(const QString& s) { return s.toStdString(); }

// QString <-> std::filesystem::path. Paths never go through a narrow std::string: on Windows that is the ANSI code
// page, not UTF-8, so non-ASCII project/user directories would break. qsPath() returns '/' separators (Qt style).
inline std::filesystem::path fsPath(const QString& s) { return std::filesystem::path(s.toStdU16String()); }
inline QString qsPath(const std::filesystem::path& p) { return QString::fromStdU16String(p.generic_u16string()); }

// "verticalFov" -> "Vertical Fov", "castShadows" -> "Cast Shadows", "r.Shadows" stays.
QString prettifyName(std::string_view name);

// Formats a byte count as "12.3 MB".
QString formatBytes(quint64 bytes);

} // namespace ox::editor

Q_DECLARE_METATYPE(ox::Uuid)
