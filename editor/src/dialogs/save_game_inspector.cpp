#include "dialogs/save_game_inspector.hpp"

#include "core/editor_context.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/serial/format.hpp>

#if OX_EDITOR_HAS_RUNTIME
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/save_game.hpp>
#endif

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace ox::editor {

SaveGameInspector::SaveGameInspector(EditorContext* ctx, QWidget* parent) : QDialog(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("SaveGameInspector"));
    setWindowTitle(tr("Save Game Inspector"));
    resize(1100, 680);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);

    auto* top = new QHBoxLayout();
    auto* icon = new QLabel(this);
    icon->setPixmap(Icons::get(QStringLiteral("save-game"), Icons::Tint::Accent).pixmap(QSize(20, 20)));
    top->addWidget(icon);
    auto* title = new QLabel(tr("Save Games"), this);
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.2);
    tf.setWeight(QFont::DemiBold);
    title->setFont(tf);
    top->addWidget(title);
    m_dirLabel = new QLabel(this);
    m_dirLabel->setProperty("role", "dim");
    m_dirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    top->addWidget(m_dirLabel, 1);
    auto* saveNow = new QPushButton(Icons::get(QStringLiteral("save")), tr("Save Play World…"), this);
    saveNow->setToolTip(tr("SaveGameSystem::save of the running play-in-editor world into a slot"));
    auto* open = new QPushButton(Icons::get(QStringLiteral("open")), tr("Open File…"), this);
    auto* refreshBtn = new QPushButton(Icons::get(QStringLiteral("refresh")), tr("Refresh"), this);
    auto* reveal = new QPushButton(Icons::get(QStringLiteral("external")), tr("Show in Finder"), this);
    top->addWidget(saveNow);
    top->addWidget(open);
    top->addWidget(refreshBtn);
    top->addWidget(reveal);
    lay->addLayout(top);

    auto* split = new QSplitter(Qt::Horizontal, this);
    m_slots = new QTreeWidget(split);
    m_slots->setObjectName(QStringLiteral("SaveSlots"));
    m_slots->setHeaderLabels({tr("Slot"), tr("Name"), tr("Level"), tr("Play time"), tr("Saved"), tr("Size")});
    m_slots->setRootIsDecorated(false);
    m_slots->setUniformRowHeights(true);
    m_slots->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_slots->setMinimumWidth(420);
    auto* right = new QWidget(split);
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    auto* rh = new QHBoxLayout();
    m_header = new QLabel(right);
    m_header->setProperty("role", "dim");
    m_header->setWordWrap(true);
    rh->addWidget(m_header, 1);
    m_find = new SearchField(tr("Find in JSON…"), right);
    m_find->setMaximumWidth(220);
    rh->addWidget(m_find);
    auto* copy = makeToolButton(QStringLiteral("copy"), tr("Copy JSON"), right);
    auto* exportBtn = makeToolButton(QStringLiteral("export"), tr("Export JSON… (oxdump)"), right);
    rh->addWidget(copy);
    rh->addWidget(exportBtn);
    rl->addLayout(rh);
    m_json = new QPlainTextEdit(right);
    m_json->setObjectName(QStringLiteral("SaveJson"));
    m_json->setReadOnly(true);
    m_json->setFont(Theme::monoFont());
    m_json->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_json->setPlaceholderText(tr("Select a save slot to see its contents"));
    rl->addWidget(m_json, 1);
    split->addWidget(m_slots);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    lay->addWidget(split, 1);

    connect(m_slots, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) {
        if (it) showSlot(it->text(0), it->data(0, Qt::UserRole).toString());
    });
    connect(refreshBtn, &QPushButton::clicked, this, &SaveGameInspector::refresh);
    connect(reveal, &QPushButton::clicked, this, [this] {
        QDir().mkpath(savesDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(savesDir()));
    });
    connect(open, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, tr("Open Save Game"), savesDir(), tr("Save games (*.oxsave *.bak *.oxsave.json);;All files (*)"));
        if (!f.isEmpty()) showFile(f);
    });
    connect(copy, &QToolButton::clicked, this, [this] { QApplication::clipboard()->setText(m_json->toPlainText()); });
    connect(exportBtn, &QToolButton::clicked, this, [this] {
        if (m_json->toPlainText().isEmpty()) return;
        const QString f = QFileDialog::getSaveFileName(this, tr("Export JSON"), QDir(savesDir()).filePath(QStringLiteral("save.oxsave.json")), tr("JSON (*.json)"));
        if (f.isEmpty()) return;
        QFile out(f);
        if (out.open(QIODevice::WriteOnly)) out.write(m_json->toPlainText().toUtf8());
    });
    connect(m_find, &QLineEdit::returnPressed, this, [this] {
        if (!m_json->find(m_find->text())) {
            m_json->moveCursor(QTextCursor::Start);
            m_json->find(m_find->text());
        }
    });
    connect(saveNow, &QPushButton::clicked, this, [this] {
#if OX_EDITOR_HAS_RUNTIME
        Engine* e = m_ctx->engine();
        if (!e || !m_ctx->isPlaying()) return;
        bool ok = false;
        const QString slot = QInputDialog::getText(this, tr("Save Play World"), tr("Slot name:"), QLineEdit::Normal, QStringLiteral("editor"), &ok);
        if (!ok || slot.isEmpty()) return;
        auto r = e->saveGame(slot.toStdString(), slot.toStdString());
        Q_EMIT m_ctx->statusMessage(r ? tr("Saved slot %1").arg(slot) : QString::fromStdString(r.error().message), 4000);
        refresh();
#endif
    });
    saveNow->setEnabled(m_ctx->isPlaying() && m_ctx->engine());
    refresh();
}

QString SaveGameInspector::savesDir() const {
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* e = m_ctx->engine()) return QString::fromStdString(e->saves().slotPath("slot").parent_path().string());
#endif
    return m_ctx->project() ? QDir(m_ctx->project()->savedDir()).filePath(QStringLiteral("SaveGames")) : QString();
}

QString SaveGameInspector::json() const { return m_json->toPlainText(); }

void SaveGameInspector::refresh() {
    m_slots->clear();
    m_dirLabel->setText(savesDir());
#if OX_EDITOR_HAS_RUNTIME
    if (Engine* e = m_ctx->engine()) {
        for (const SaveSlotInfo& s : e->saves().listSlots()) {
            auto* it = new QTreeWidgetItem();
            it->setText(0, QString::fromStdString(s.header.slot));
            it->setData(0, Qt::UserRole, QString::fromStdString(s.path.string()));
            it->setIcon(0, Icons::get(s.corrupted ? QStringLiteral("warning") : QStringLiteral("save-game")));
            it->setText(1, QString::fromStdString(s.header.displayName));
            it->setText(2, QFileInfo(QString::fromStdString(s.header.level)).fileName());
            it->setToolTip(2, QString::fromStdString(s.header.level));
            const int secs = int(s.header.playTimeSeconds);
            it->setText(3, QStringLiteral("%1:%2:%3").arg(secs / 3600).arg((secs / 60) % 60, 2, 10, QLatin1Char('0')).arg(secs % 60, 2, 10, QLatin1Char('0')));
            it->setText(4, QDateTime::fromSecsSinceEpoch(s.header.timestamp).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
            it->setText(5, formatBytes(s.fileSize));
            QStringList flags;
            if (s.corrupted) flags << tr("corrupted");
            if (s.hasBackup) flags << tr("backup");
            if (s.hasThumbnail) flags << tr("thumbnail");
            it->setToolTip(0, flags.join(QStringLiteral(", ")));
            m_slots->addTopLevelItem(it);
        }
        return;
    }
#endif
    QDir dir(savesDir());
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.oxsave")}, QDir::Files, QDir::Time)) {
        auto* it = new QTreeWidgetItem({fi.completeBaseName(), {}, {}, {}, fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")), formatBytes(quint64(fi.size()))});
        it->setData(0, Qt::UserRole, fi.absoluteFilePath());
        m_slots->addTopLevelItem(it);
    }
}

void SaveGameInspector::showSlot(const QString& slot, const QString& path) {
    if (!showFile(path)) m_header->setText(tr("Slot %1 could not be decoded").arg(slot));
}

bool SaveGameInspector::showFile(const QString& path) {
    if (path.endsWith(QLatin1String(".json"), Qt::CaseInsensitive)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        m_json->setPlainText(QString::fromUtf8(f.readAll()));
        m_header->setText(QFileInfo(path).fileName());
        return true;
    }
    auto bytes = serial::readFileBytes(path.toStdString());
    if (!bytes) {
        m_json->clear();
        m_header->setText(QString::fromStdString(bytes.error().message));
        return false;
    }
    auto json = serial::binaryToJson(*bytes, 2);
    if (!json) {
        m_json->setPlainText(QString::fromStdString(json.error().message));
        m_header->setText(tr("%1 — not a valid OXB1 document").arg(QFileInfo(path).fileName()));
        return false;
    }
    m_json->setPlainText(QString::fromStdString(*json));
    // Start at the data (the schema table comes first in OXB1 documents).
    if (m_json->find(QStringLiteral("\"header\": {"))) m_json->centerCursor();
    m_header->setText(tr("%1 · %2 · read-only (oxdump)").arg(QFileInfo(path).fileName(), formatBytes(bytes->size())));
    return true;
}

} // namespace ox::editor
