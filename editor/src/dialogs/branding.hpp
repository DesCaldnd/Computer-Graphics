#pragma once

#include <QDialog>
#include <QPixmap>
#include <QString>
#include <QWidget>

class QPainter;

namespace ox::editor {

class EditorContext;

// "OxwaldEngine" wordmark: logo mark + two-weight text. Returns the drawn width.
qreal paintWordmark(QPainter& p, QPointF topLeft, qreal height, bool light = false);
QPixmap logoPixmap(int size);

// Startup splash with progress messages.
class SplashScreen : public QWidget {
    Q_OBJECT
public:
    SplashScreen();
    void setMessage(const QString& msg, int progressPercent);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QString m_message;
    int m_progress = 0;
};

class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(EditorContext* ctx, QWidget* parent = nullptr);
};

// Which optional engine modules this editor build integrates ("rhi", "assets", ...) and which are pending.
QStringList integratedModules();
QStringList pendingModules();

} // namespace ox::editor
