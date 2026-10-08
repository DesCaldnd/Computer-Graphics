#pragma once

#include <QHash>
#include <QString>
#include <QTranslator>

namespace ox::editor {

// In-code translation table (qttools/lrelease is not part of the vcpkg Qt install). Every UI string goes through
// tr()/QCoreApplication::translate; the translator looks up the source text regardless of context.
// Add a language: extend languages() and add a table in translations_<code>.cpp.
class Translator final : public QTranslator {
public:
    static Translator& instance();

    // "en" removes the translator; other codes install it. Emits QEvent::LanguageChange to all widgets.
    void setLanguage(const QString& code);
    [[nodiscard]] QString language() const { return m_language; }
    [[nodiscard]] static QStringList languages();     // codes
    [[nodiscard]] static QString languageName(const QString& code);

    QString translate(const char* context, const char* sourceText, const char* disambiguation = nullptr,
                      int n = -1) const override;
    [[nodiscard]] bool isEmpty() const override { return m_table == nullptr || m_table->isEmpty(); }
    // Number of source strings with a translation in the active language (tests).
    [[nodiscard]] int entryCount() const { return m_table ? int(m_table->size()) : 0; }

private:
    Translator() = default;
    QString m_language = QStringLiteral("en");
    const QHash<QString, QString>* m_table = nullptr;
    bool m_installed = false;
};

const QHash<QString, QString>& russianTable();

} // namespace ox::editor
