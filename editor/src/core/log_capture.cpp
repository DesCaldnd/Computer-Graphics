#include "core/log_capture.hpp"

#include <QCoreApplication>
#include <QMetaObject>

namespace ox::editor {

LogCapture& LogCapture::instance() {
    static LogCapture* capture = new LogCapture(); // outlives every sink call during shutdown
    return *capture;
}

void LogCapture::install() {
    if (m_sink >= 0) return;
    m_sink = log::addSink([this](const log::Record& r) { receive(r); });
}

void LogCapture::uninstall() {
    if (m_sink < 0) return;
    log::removeSink(m_sink);
    m_sink = -1;
}

void LogCapture::receive(const log::Record& r) {
    LogEntry e{r.level, QString::fromUtf8(r.category.data(), qsizetype(r.category.size())),
               QString::fromUtf8(r.message.data(), qsizetype(r.message.size())), QDateTime::currentDateTime()};
    bool queue = false;
    {
        QMutexLocker lock(&m_mutex);
        m_pending.push_back(std::move(e));
        if (!m_flushQueued) {
            m_flushQueued = true;
            queue = true;
        }
    }
    if (queue && QCoreApplication::instance()) {
        QMetaObject::invokeMethod(this, [this] { flush(); }, Qt::QueuedConnection);
    }
}

void LogCapture::flush() {
    QVector<LogEntry> batch;
    {
        QMutexLocker lock(&m_mutex);
        batch.swap(m_pending);
        m_flushQueued = false;
    }
    if (batch.isEmpty()) return;
    if (m_entries.size() + batch.size() > kMaxEntries) {
        clear();
    }
    const int first = int(m_entries.size());
    for (auto& e : batch) {
        ++m_counts[size_t(e.level)];
        m_entries.push_back(std::move(e));
    }
    Q_EMIT appended(first, int(batch.size()));
}

void LogCapture::append(log::Level level, const QString& category, const QString& message) {
    flush();
    const int first = int(m_entries.size());
    m_entries.push_back({level, category, message, QDateTime::currentDateTime()});
    ++m_counts[size_t(level)];
    Q_EMIT appended(first, 1);
}

void LogCapture::clear() {
    m_entries.clear();
    m_counts.fill(0);
    Q_EMIT cleared();
}

} // namespace ox::editor
