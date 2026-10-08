#include "core/common.hpp"

#include <cctype>

namespace ox::editor {

QString prettifyName(std::string_view name) {
    std::string out;
    out.reserve(name.size() + 8);
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == '_') {
            out += ' ';
            continue;
        }
        if (i == 0) {
            out += char(std::toupper(static_cast<unsigned char>(c)));
            continue;
        }
        const char prev = name[i - 1];
        const bool upper = std::isupper(static_cast<unsigned char>(c));
        const bool prevLower = std::islower(static_cast<unsigned char>(prev)) || std::isdigit(static_cast<unsigned char>(prev));
        const bool nextLower = i + 1 < name.size() && std::islower(static_cast<unsigned char>(name[i + 1]));
        const bool prevUpper = std::isupper(static_cast<unsigned char>(prev));
        if (upper && (prevLower || (prevUpper && nextLower))) out += ' ';
        out += c;
    }
    return QString::fromStdString(out);
}

QString formatBytes(quint64 bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', u == 0 ? 0 : 1).arg(QLatin1String(units[u]));
}

} // namespace ox::editor
