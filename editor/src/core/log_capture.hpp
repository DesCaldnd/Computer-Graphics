#pragma once

#include <oxwald/core/log.hpp>

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <array>

namespace ox::editor {

struct LogEntry {
    log::Level level = log::Level::Info;
    QString category;
    QString message;
    QDateTime time;
};

// ox::log sink feeding the Console panel. Records from any thread are queued and delivered on the UI thread.
class LogCapture : public QObject {
    Q_OBJECT
public:
    static LogCapture& instance();

    void install();
    void uninstall();
    [[nodiscard]] const QVector<LogEntry>& entries() const { return m_entries; }
    [[nodiscard]] int count(log::Level level) const { return m_counts[size_t(level)]; }
    void clear();
    // Delivers queued records now (normally done through a queued call).
    void flush();
    // Direct append on the UI thread (console echo, command results).
    void append(log::Level level, const QString& category, const QString& message);

    static constexpr int kMaxEntries = 20000;

Q_SIGNALS:
    void appended(int first, int count);
    void cleared();

private:
    LogCapture() = default;
    void receive(const log::Record& r);

    QMutex m_mutex;
    QVector<LogEntry> m_pending;
    bool m_flushQueued = false;
    QVector<LogEntry> m_entries;
    std::array<int, 6> m_counts{};
    int m_sink = -1;
};

} // namespace ox::editor
