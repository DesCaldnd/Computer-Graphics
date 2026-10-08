#include "test_common.hpp"

#include "core/log_capture.hpp"

#include <QApplication>
#include <QElapsedTimer>

namespace ox::editor::test {

std::vector<std::pair<const char*, Factory>>& registry() {
    static std::vector<std::pair<const char*, Factory>> r;
    return r;
}

QString tempRoot() {
    QString base = qEnvironmentVariable("OX_EDITOR_TEST_TMP");
    if (base.isEmpty()) base = QDir(QDir::tempPath()).filePath(QStringLiteral("oxwald-editor-tests"));
    QDir().mkpath(base);
    return base;
}

std::unique_ptr<EditorContext> makeContext() {
    auto ctx = std::make_unique<EditorContext>();
    ctx->preferences().setStorageDir(QDir(tempRoot()).filePath(QStringLiteral("prefs")));
    ctx->preferences().setValues(PreferenceValues{});
    return ctx;
}

void pump(int ms) {
    QElapsedTimer t;
    t.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    } while (t.elapsed() < ms);
}

} // namespace ox::editor::test

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    ox::editor::EditorContext::registerEngineTypes();
    ox::editor::LogCapture::instance().install();
    // optional filter: ox_editor_tests <ClassName> [qtest args...]
    QString only;
    QStringList args = app.arguments();
    if (args.size() > 1 && !args[1].startsWith(QLatin1Char('-'))) {
        only = args[1];
        args.removeAt(1);
    }
    int failures = 0;
    for (auto& [name, factory] : ox::editor::test::registry()) {
        if (!only.isEmpty() && only != QLatin1String(name)) continue;
        auto obj = factory();
        failures += QTest::qExec(obj.get(), args);
    }
    return failures == 0 ? 0 : 1;
}
