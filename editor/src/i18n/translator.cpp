#include "i18n/translator.hpp"

#include <QCoreApplication>

namespace ox::editor {

Translator& Translator::instance() {
    static Translator* t = new Translator(); // lives as long as the application (QTranslator must outlive it)
    return *t;
}

QStringList Translator::languages() { return {QStringLiteral("en"), QStringLiteral("ru")}; }

QString Translator::languageName(const QString& code) {
    if (code == QLatin1String("ru")) return QStringLiteral("Русский");
    return QStringLiteral("English");
}

void Translator::setLanguage(const QString& code) {
    const QString lang = languages().contains(code) ? code : QStringLiteral("en");
    if (lang == m_language && (m_installed || lang == QLatin1String("en"))) return;
    m_language = lang;
    m_table = lang == QLatin1String("ru") ? &russianTable() : nullptr;
    if (!QCoreApplication::instance()) return;
    if (m_installed) {
        QCoreApplication::removeTranslator(this);
        m_installed = false;
    }
    if (m_table) m_installed = QCoreApplication::installTranslator(this);
    else {
        // removing the translator does not send LanguageChange by itself
        QEvent ev(QEvent::LanguageChange);
        QCoreApplication::sendEvent(QCoreApplication::instance(), &ev);
    }
}

QString Translator::translate(const char* context, const char* sourceText, const char* disambiguation, int n) const {
    Q_UNUSED(context);
    Q_UNUSED(disambiguation);
    Q_UNUSED(n);
    if (!m_table || !sourceText) return {};
    auto it = m_table->find(QString::fromUtf8(sourceText));
    return it == m_table->end() ? QString() : *it;
}

} // namespace ox::editor
