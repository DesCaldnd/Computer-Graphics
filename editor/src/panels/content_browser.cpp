#include "panels/content_browser.hpp"

#include "content/thumbnails.hpp"
#include "core/editor_context.hpp"
#include "inspector/property_editors.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QAbstractFileIconProvider>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QShortcut>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

namespace ox::editor {

QColor AssetListModel::typeColor(const QString& type) {
    static const QHash<QString, QColor> map = {
        {"Folder", QColor("#C9A35F")},   {"Scene", QColor("#8B6DFF")},    {"Prefab", QColor("#4EA8FF")},
        {"Mesh", QColor("#3DD68C")},     {"Material", QColor("#F5B642")}, {"Texture", QColor("#FF7A59")},
        {"Audio", QColor("#E86AF0")},    {"Script", QColor("#5FD0E6")},   {"Shader", QColor("#B39DFF")},
        {"Model", QColor("#2FBF7F")},    {"BehaviorTree", QColor("#E0835A")}, {"AnimatorController", QColor("#9BCB6B")},
        {"Skeleton", QColor("#B8D47A")}, {"NavMesh", QColor("#6FA8DC")},  {"Heightmap", QColor("#A08060")},
        {"Font", QColor("#9AA5B8")},     {"Animation", QColor("#7FD48B")}, {"SaveGame", QColor("#C0C4D0")},
    };
    return map.value(type, QColor("#8A90A2"));
}

AssetListModel::AssetListModel(EditorContext* ctx, QObject* parent) : QAbstractListModel(parent), m_ctx(ctx) {}

void AssetListModel::setFolder(const QString& folder) {
    m_folder = folder;
    refresh();
}

void AssetListModel::setFilter(const QString& text, const QString& type) {
    m_text = text;
    m_type = type;
    refresh();
}

void AssetListModel::refresh() {
    beginResetModel();
    m_all = m_folder.isEmpty() ? QList<AssetInfo>{} : m_ctx->services().assets().list(m_folder);
    m_items.clear();
    for (const auto& a : m_all) {
        if (!m_text.isEmpty() && !a.name.contains(m_text, Qt::CaseInsensitive)) continue;
        if (!m_type.isEmpty() && !a.isFolder && a.type != m_type) continue;
        m_items.push_back(a);
    }
    endResetModel();
}

const AssetInfo* AssetListModel::at(const QModelIndex& i) const {
    return i.isValid() && i.row() < m_items.size() ? &m_items[i.row()] : nullptr;
}

QModelIndex AssetListModel::indexOfPath(const QString& path) const {
    for (int i = 0; i < m_items.size(); ++i) {
        if (QFileInfo(m_items[i].path) == QFileInfo(path)) return index(i);
    }
    return {};
}

int AssetListModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(m_items.size()); }

QPixmap AssetListModel::thumbnail(const AssetInfo& a) const { return ThumbnailCache::instance().get(*m_ctx, a, 160); }

QVariant AssetListModel::data(const QModelIndex& index, int role) const {
    const AssetInfo* a = at(index);
    if (!a) return {};
    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole: return a->name;
    case Qt::DecorationRole: return Icons::fixed(Icons::forAssetType(a->type), typeColor(a->type));
    case Qt::ToolTipRole:
        return QStringLiteral("<b>%1</b><br/>%2 · %3<br/><span style='color:%4'>%5<br/>%6</span>")
            .arg(a->name.toHtmlEscaped(), a->type, a->isFolder ? tr("folder") : formatBytes(quint64(a->size)), cssColor(colors().textFaint),
                 a->relativePath.toHtmlEscaped(), qs(a->uuid.toString()));
    case TypeRole: return a->type;
    case PathRole: return a->path;
    case ThumbRole: return thumbnail(*a);
    default: break;
    }
    return {};
}

bool AssetListModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    const AssetInfo* a = at(index);
    if (!a || role != Qt::EditRole) return false;
    const QString name = value.toString().trimmed();
    if (name.isEmpty() || name == a->name) return false;
    QString err;
    if (m_ctx->services().assets().rename(a->path, name, &err).isEmpty()) {
        Q_EMIT m_ctx->statusMessage(err, 4000);
        return false;
    }
    refresh();
    return true;
}

Qt::ItemFlags AssetListModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags f = QAbstractListModel::flags(index);
    if (!index.isValid()) return f | Qt::ItemIsDropEnabled;
    f |= Qt::ItemIsEditable | Qt::ItemIsDragEnabled;
    if (const AssetInfo* a = at(index); a && a->isFolder) f |= Qt::ItemIsDropEnabled;
    return f;
}

QStringList AssetListModel::mimeTypes() const { return {QString::fromLatin1(kMimeAsset), QStringLiteral("text/uri-list")}; }

QMimeData* AssetListModel::mimeData(const QModelIndexList& indexes) const {
    QStringList lines;
    QList<QUrl> urls;
    for (const auto& i : indexes) {
        if (const AssetInfo* a = at(i)) {
            lines << QStringLiteral("%1|%2|%3").arg(qs(a->uuid.toString()), a->type, a->path);
            urls << QUrl::fromLocalFile(a->path);
        }
    }
    auto* m = new QMimeData();
    m->setData(kMimeAsset, lines.join(QLatin1Char('\n')).toUtf8());
    m->setUrls(urls);
    return m;
}

bool AssetListModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) const {
    const AssetInfo* a = at(parent);
    if (!data->hasFormat(kMimeAsset)) return data->hasUrls() && (!a || a->isFolder); // files from Finder: import
    return a && a->isFolder;
}

bool AssetListModel::dropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex& parent) {
    const AssetInfo* target = at(parent);
    if (!data->hasFormat(kMimeAsset) && data->hasUrls()) {
        QStringList files;
        for (const QUrl& u : data->urls()) {
            if (u.isLocalFile()) files << u.toLocalFile();
        }
        importFiles(files, target && target->isFolder ? target->path : m_folder);
        return false;
    }
    if (!target || !target->isFolder) return false;
    const QString dest = target->path;
    for (const QString& line : QString::fromUtf8(data->data(kMimeAsset)).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(QLatin1Char('|'));
        if (parts.size() < 3 || parts[2] == dest) continue;
        QString err;
        if (m_ctx->services().assets().move(parts[2], dest, &err).isEmpty()) Q_EMIT m_ctx->statusMessage(err, 4000);
    }
    refresh();
    return false;
}

QStringList AssetListModel::importFiles(const QStringList& files, const QString& folder) {
    if (files.isEmpty()) return {};
    QString err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QStringList imported = m_ctx->services().assets().importFiles(files, folder, &err);
    QApplication::restoreOverrideCursor();
    refresh();
    if (!err.isEmpty()) Q_EMIT m_ctx->statusMessage(tr("Import problems: %1").arg(err), 8000);
    else if (!imported.isEmpty()) Q_EMIT m_ctx->statusMessage(tr("Imported %n file(s)", nullptr, int(imported.size())), 3000);
    return imported;
}

namespace {

class FolderIconProvider final : public QAbstractFileIconProvider {
public:
    QIcon icon(IconType) const override { return Icons::fixed(QStringLiteral("folder"), AssetListModel::typeColor(QStringLiteral("Folder"))); }
    QIcon icon(const QFileInfo&) const override { return icon(IconType::Folder); }
};

// Rounded tiles with a type-coloured thumbnail area (grid mode) or compact rows (list mode).
class AssetTileDelegate : public QStyledItemDelegate {
public:
    explicit AssetTileDelegate(QListView* view) : QStyledItemDelegate(view), m_view(view) {}

    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        if (m_view->viewMode() == QListView::ListMode) return QSize(200, 26);
        return QStyledItemDelegate::sizeHint(o, i).expandedTo(QSize(104, 128)).boundedTo(QSize(104, 128));
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& i) const override {
        const ThemePalette& c = colors();
        const QString type = i.data(AssetListModel::TypeRole).toString();
        const QColor tcol = AssetListModel::typeColor(type);
        const bool sel = opt.state & QStyle::State_Selected;
        const bool hover = opt.state & QStyle::State_MouseOver;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        if (m_view->viewMode() == QListView::ListMode) {
            QRectF r = QRectF(opt.rect).adjusted(2, 1, -2, -1);
            if (sel || hover) {
                p->setPen(Qt::NoPen);
                p->setBrush(sel ? c.accentSelected : c.bg3);
                p->drawRoundedRect(r, 5, 5);
            }
            const QIcon icon = i.data(Qt::DecorationRole).value<QIcon>();
            icon.paint(p, QRect(int(r.left()) + 6, int(r.center().y()) - 8, 16, 16));
            p->setPen(c.text);
            p->drawText(r.adjusted(30, 0, -90, 0), Qt::AlignVCenter | Qt::AlignLeft, opt.fontMetrics.elidedText(i.data().toString(), Qt::ElideRight, int(r.width()) - 120));
            p->setPen(c.textFaint);
            p->drawText(r.adjusted(0, 0, -8, 0), Qt::AlignVCenter | Qt::AlignRight, type);
            p->restore();
            return;
        }
        QRectF r = QRectF(opt.rect).adjusted(4, 4, -4, -4);
        // card
        p->setPen(QPen(sel ? c.accent : hover ? c.borderHover : c.border, sel ? 1.6 : 1.0));
        p->setBrush(sel ? mix(c.bg2, c.accent, 0.12) : hover ? c.bg3 : c.bg2);
        p->drawRoundedRect(r, 9, 9);
        // thumbnail area
        QRectF th(r.left() + 6, r.top() + 6, r.width() - 12, r.width() - 18);
        QPainterPath clip;
        clip.addRoundedRect(th, 6, 6);
        const QPixmap pm = i.data(AssetListModel::ThumbRole).value<QPixmap>();
        if (!pm.isNull()) {
            p->save();
            p->setClipPath(clip);
            p->fillRect(th, QColor(20, 20, 24));
            const QSizeF ps = QSizeF(pm.size()).scaled(th.size(), Qt::KeepAspectRatio);
            p->drawPixmap(QRectF(th.center() - QPointF(ps.width() / 2, ps.height() / 2), ps), pm, QRectF(pm.rect()));
            p->restore();
        } else {
            QLinearGradient g(th.topLeft(), th.bottomRight());
            g.setColorAt(0, withAlpha(tcol, type == QLatin1String("Folder") ? 40 : 70));
            g.setColorAt(1, withAlpha(tcol, 18));
            p->setPen(Qt::NoPen);
            p->setBrush(g);
            p->drawPath(clip);
            const QPixmap ic = Icons::pixmap(Icons::forAssetType(type), 36 * 2, tcol);
            p->drawPixmap(QRectF(th.center().x() - 18, th.center().y() - 18, 36, 36), ic, QRectF(ic.rect()));
        }
        // type stripe
        if (type != QLatin1String("Folder")) {
            p->setPen(Qt::NoPen);
            p->setBrush(tcol);
            p->drawRoundedRect(QRectF(th.left(), th.bottom() - 3, th.width(), 3), 1.5, 1.5);
        }
        // name + type
        QRectF tr(r.left() + 6, th.bottom() + 4, r.width() - 12, r.bottom() - th.bottom() - 6);
        QFont f = opt.font;
        p->setFont(f);
        p->setPen(c.text);
        const QString name = QFontMetrics(f).elidedText(i.data().toString(), Qt::ElideMiddle, int(tr.width()));
        p->drawText(QRectF(tr.left(), tr.top(), tr.width(), 16), Qt::AlignHCenter | Qt::AlignTop, name);
        f.setPixelSize(std::max(9, Theme::instance().fontSize() - 3));
        p->setFont(f);
        p->setPen(c.textFaint);
        p->drawText(QRectF(tr.left(), tr.top() + 16, tr.width(), 14), Qt::AlignHCenter | Qt::AlignTop, type);
        p->restore();
    }

private:
    QListView* m_view;
};

} // namespace

ContentBrowserPanel::ContentBrowserPanel(EditorContext* ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("ContentBrowserPanel"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // header
    auto* header = new QFrame(this);
    header->setProperty("role", "panelHeader");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 6, 8, 6);
    hl->setSpacing(6);
    auto* addBtn = new QToolButton(header);
    addBtn->setIcon(Icons::get(QStringLiteral("add"), Icons::Tint::OnAccent));
    addBtn->setText(tr("Add"));
    addBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    addBtn->setPopupMode(QToolButton::InstantPopup);
    auto styleAdd = [addBtn] {
        addBtn->setStyleSheet(QStringLiteral("QToolButton{background:%1;color:%2;border-radius:6px;padding:4px 10px;font-weight:600;}"
                                             "QToolButton:hover{background:%3;}")
                                  .arg(cssColor(colors().accent), cssColor(colors().onAccent), cssColor(colors().accentHover)));
    };
    styleAdd();
    connect(&Theme::instance(), &Theme::changed, addBtn, styleAdd);
    auto* addMenu = new QMenu(addBtn);
    addMenu->addAction(Icons::get(QStringLiteral("folder-plus")), tr("New Folder"), this, [this] { createAsset(QStringLiteral("Folder")); });
    addMenu->addSeparator();
    addMenu->addAction(Icons::get(QStringLiteral("scene")), tr("Scene"), this, [this] { createAsset(QStringLiteral("Scene")); });
    addMenu->addAction(Icons::get(QStringLiteral("prefab")), tr("Prefab"), this, [this] { createAsset(QStringLiteral("Prefab")); });
    addMenu->addAction(Icons::get(QStringLiteral("material")), tr("Material"), this, [this] { createAsset(QStringLiteral("Material")); });
    addMenu->addAction(Icons::get(QStringLiteral("script")), tr("Lua Script"), this, [this] { createAsset(QStringLiteral("Script")); });
    addMenu->addAction(Icons::get(QStringLiteral("sitemap")), tr("Behavior Tree"), this, [this] { createAsset(QStringLiteral("BehaviorTree")); });
    addMenu->addSeparator();
    addMenu->addAction(Icons::get(QStringLiteral("import")), tr("Import…"), this, &ContentBrowserPanel::importFiles);
    addBtn->setMenu(addMenu);
    hl->addWidget(addBtn);
    auto* importBtn = makeToolButton(QStringLiteral("import"), tr("Import files"), header);
    connect(importBtn, &QToolButton::clicked, this, &ContentBrowserPanel::importFiles);
    hl->addWidget(importBtn);
    auto* up = makeToolButton(QStringLiteral("arrow-up"), tr("Parent folder"), header);
    connect(up, &QToolButton::clicked, this, [this] {
        const QString cur = m_model->folder();
        if (QDir::cleanPath(cur) != QDir::cleanPath(m_root)) navigate(QFileInfo(cur).absolutePath());
    });
    hl->addWidget(up);
    m_breadcrumb = new QWidget(header);
    m_crumbLayout = new QHBoxLayout(m_breadcrumb);
    m_crumbLayout->setContentsMargins(4, 0, 4, 0);
    m_crumbLayout->setSpacing(0);
    hl->addWidget(m_breadcrumb, 1);
    m_typeFilter = new QComboBox(header);
    m_typeFilter->addItem(Icons::get(QStringLiteral("filter")), tr("All types"), QString());
    for (const char* t : {"Scene", "Prefab", "Model", "Mesh", "Material", "Texture", "Audio", "Script", "BehaviorTree", "Shader", "Data"}) {
        m_typeFilter->addItem(Icons::fixed(Icons::forAssetType(QString::fromLatin1(t)), AssetListModel::typeColor(QString::fromLatin1(t))),
                              QString::fromLatin1(t), QString::fromLatin1(t));
    }
    hl->addWidget(m_typeFilter);
    m_search = new SearchField(tr("Search assets…"), header);
    m_search->setMaximumWidth(220);
    hl->addWidget(m_search);
    m_gridButton = makeToolButton(QStringLiteral("grid-view"), tr("Tiles"), header, true);
    m_listButton = makeToolButton(QStringLiteral("list"), tr("List"), header, true);
    m_gridButton->setChecked(true);
    hl->addWidget(m_gridButton);
    hl->addWidget(m_listButton);
    lay->addWidget(header);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setHandleWidth(1);
    m_dirs = new QFileSystemModel(this);
    m_dirs->setFilter(QDir::Dirs | QDir::NoDotAndDotDot);
    m_tree = new QTreeView(split);
    m_tree->setModel(m_dirs);
    for (int c = 1; c < m_dirs->columnCount(); ++c) m_tree->hideColumn(c);
    m_tree->setHeaderHidden(true);
    m_tree->setIndentation(14);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setMinimumWidth(150);
    static FolderIconProvider s_folderIcons;
    m_dirs->setIconProvider(&s_folderIcons);
    m_model = new AssetListModel(ctx, this);
    m_list = new QListView(split);
    m_list->setObjectName(QStringLiteral("ContentView"));
    m_list->setModel(m_model);
    m_list->setItemDelegate(new AssetTileDelegate(m_list));
    m_list->setProperty("role", "tiles");
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true);
    m_list->setAcceptDrops(true);
    m_list->setDropIndicatorShown(true);
    m_list->setDragDropMode(QAbstractItemView::DragDrop);
    m_list->setEditTriggers(QAbstractItemView::EditKeyPressed);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setMouseTracking(true);
    m_list->setUniformItemSizes(true);
    split->addWidget(m_tree);
    split->addWidget(m_list);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({190, 800});
    lay->addWidget(split, 1);
    setTileMode(true);

    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& i) {
        if (i.isValid()) navigate(m_dirs->filePath(i));
    });
    for (QKeySequence ks : {QKeySequence(Qt::Key_Backspace)}) {
        auto* sc = new QShortcut(ks, m_list, this, &ContentBrowserPanel::deleteSelected, Qt::WidgetShortcut);
        (void)sc;
    }
    connect(m_list, &QListView::doubleClicked, this, &ContentBrowserPanel::activate);
    connect(m_list, &QListView::customContextMenuRequested, this, &ContentBrowserPanel::showContextMenu);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) { m_model->setFilter(t, m_typeFilter->currentData().toString()); });
    connect(m_typeFilter, &QComboBox::currentIndexChanged, this, [this] { m_model->setFilter(m_search->text(), m_typeFilter->currentData().toString()); });
    connect(m_gridButton, &QToolButton::clicked, this, [this] { setTileMode(true); });
    connect(m_listButton, &QToolButton::clicked, this, [this] { setTileMode(false); });
    m_rescanTimer.setSingleShot(true);
    m_rescanTimer.setInterval(300);
    connect(&m_rescanTimer, &QTimer::timeout, this, [this] {
        m_ctx->services().assets().rescan(); // new/moved/deleted files from outside the editor
        m_model->refresh();
    });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_rescanTimer, qOverload<>(&QTimer::start));
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex& i) {
        const AssetInfo* a = m_model->at(i);
        if (!a || a->isFolder) return;
        m_ctx->selection().clear();
        m_ctx->inspectAsset(a->path);
    });
    connect(&m_ctx->runtime(), &RuntimeHost::started, this, [this] {
        if (m_ctx->project()) setRoot(m_ctx->project()->contentDir());
    });
    connect(ctx, &EditorContext::projectChanged, this, [this] {
        if (m_ctx->project()) setRoot(m_ctx->project()->contentDir());
    });
    if (ctx->project()) setRoot(ctx->project()->contentDir());
}

void ContentBrowserPanel::setTileMode(bool tiles) {
    m_gridButton->setChecked(tiles);
    m_listButton->setChecked(!tiles);
    m_list->setViewMode(tiles ? QListView::IconMode : QListView::ListMode);
    m_list->setFlow(tiles ? QListView::LeftToRight : QListView::TopToBottom);
    m_list->setWrapping(tiles);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(tiles ? QListView::Snap : QListView::Static);
    m_list->setSpacing(tiles ? 2 : 0);
    m_list->setGridSize(tiles ? QSize(108, 132) : QSize());
    m_list->setDragDropMode(QAbstractItemView::DragDrop);
    m_list->setDefaultDropAction(Qt::MoveAction);
}

void ContentBrowserPanel::setRoot(const QString& root) {
    m_root = root;
    QDir().mkpath(root);
    m_ctx->services().assets().setRootPath(root);
    m_tree->setRootIndex(m_dirs->setRootPath(root));
    navigate(root);
}

void ContentBrowserPanel::navigate(const QString& folder) {
    if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
    m_model->setFolder(folder);
    m_watcher.addPath(folder);
    const QModelIndex ti = m_dirs->index(folder);
    if (ti.isValid() && m_tree->currentIndex() != ti) {
        QSignalBlocker b(m_tree->selectionModel());
        m_tree->setCurrentIndex(ti);
        m_tree->expand(ti);
    }
    rebuildBreadcrumb();
}

void ContentBrowserPanel::rebuildBreadcrumb() {
    while (QLayoutItem* it = m_crumbLayout->takeAt(0)) {
        if (it->widget()) {
            it->widget()->hide();
            it->widget()->deleteLater();
        }
        delete it;
    }
    const QString rel = QDir(m_root).relativeFilePath(m_model->folder());
    QStringList parts{m_ctx->project() ? m_ctx->project()->assetDirName() : QStringLiteral("Content")};
    if (rel != QLatin1String(".") && !rel.isEmpty()) parts += rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString path = m_root;
    for (int i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            path = QDir(path).filePath(parts[i]);
            auto* sep = new QLabel(m_breadcrumb);
            sep->setPixmap(Icons::get(QStringLiteral("chevron-right"), Icons::Tint::Faint).pixmap(QSize(12, 12)));
            m_crumbLayout->addWidget(sep);
        }
        auto* b = new QToolButton(m_breadcrumb);
        b->setText(parts[i]);
        b->setProperty("role", "text");
        if (i == 0) {
            b->setIcon(Icons::get(QStringLiteral("content")));
            b->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        if (i == parts.size() - 1) {
            QFont f = b->font();
            f.setWeight(QFont::DemiBold);
            b->setFont(f);
        }
        const QString target = path;
        connect(b, &QToolButton::clicked, this, [this, target] { navigate(target); });
        m_crumbLayout->addWidget(b);
    }
    m_crumbLayout->addStretch(1);
}

void ContentBrowserPanel::activate(const QModelIndex& index) {
    const AssetInfo* a = m_model->at(index);
    if (!a) return;
    if (a->isFolder) {
        navigate(a->path);
    } else if (a->type == QLatin1String("Scene")) {
        Q_EMIT openSceneRequested(a->path);
    } else if (a->type == QLatin1String("Prefab") || a->type == QLatin1String("Model")) {
        m_ctx->instantiatePrefab(a->path, {});
    } else if (a->type == QLatin1String("Script") || a->type == QLatin1String("Shader") || a->type == QLatin1String("BehaviorTree")) {
        Q_EMIT m_ctx->openSourceRequested(a->path, 1);
    } else {
        const QString editor = m_ctx->preferences().values().codeEditorPath;
        if (!editor.isEmpty() && (a->type == QLatin1String("Script") || a->type == QLatin1String("Shader"))) {
            QStringList args = m_ctx->preferences().values().codeEditorArgs.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            for (auto& s : args) s.replace(QLatin1String("%f"), a->path).replace(QLatin1String("%l"), QStringLiteral("1"));
            QProcess::startDetached(editor, args);
        } else {
            QDesktopServices::openUrl(QUrl::fromLocalFile(a->path));
        }
    }
}

void ContentBrowserPanel::createAsset(const QString& type) {
    QString err;
    QString path;
    auto& backend = m_ctx->services().assets();
    if (type == QLatin1String("Folder")) path = backend.createFolder(m_model->folder(), tr("New Folder"), &err);
    else path = backend.createAsset(m_model->folder(), type, QStringLiteral("New") + type, &err);
    if (path.isEmpty()) {
        Q_EMIT m_ctx->statusMessage(err, 4000);
        return;
    }
    m_model->refresh();
    const QModelIndex i = m_model->indexOfPath(path);
    if (i.isValid()) {
        m_list->setCurrentIndex(i);
        m_list->edit(i);
    }
}

void ContentBrowserPanel::importFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Import Assets"), QString(),
                                                            tr("Assets (*.gltf *.glb *.bin *.fbx *.obj *.mtl *.png *.jpg *.jpeg *.tga *.bmp *.psd *.ktx2 *.exr *.hdr *.wav *.ogg *.mp3 *.flac *.lua *.ttf *.otf);;All files (*)"));
    m_model->importFiles(files, m_model->folder());
}

void ContentBrowserPanel::deleteSelected() {
    QStringList paths;
    for (const QModelIndex& i : m_list->selectionModel()->selectedIndexes()) {
        if (const AssetInfo* a = m_model->at(i)) paths << a->path;
    }
    if (paths.isEmpty()) return;
    // Dependency-aware warning: assets still referenced by other assets (materials, prefabs, scenes, ...).
    QStringList users;
    for (const QString& p : paths) {
        for (const AssetInfo& d : m_ctx->services().assets().dependents(p)) {
            if (!paths.contains(d.path)) users << QStringLiteral("%1  ←  %2").arg(QFileInfo(p).fileName(), d.relativePath);
        }
    }
    QString text = tr("Delete %n item(s)? This cannot be undone.", nullptr, int(paths.size()));
    if (!users.isEmpty()) {
        users.removeDuplicates();
        text = tr("<b>%n reference(s) will break:</b><br>", nullptr, int(users.size())) + users.mid(0, 12).join(QStringLiteral("<br>")) +
               (users.size() > 12 ? QStringLiteral("<br>…") : QString()) + QStringLiteral("<br><br>") + text;
    }
    QMessageBox box(users.isEmpty() ? QMessageBox::Question : QMessageBox::Warning, tr("Delete Assets"), text, QMessageBox::Yes | QMessageBox::No, this);
    box.setTextFormat(Qt::RichText);
    box.setObjectName(QStringLiteral("DeleteAssetsBox"));
    if (m_confirmDelete && box.exec() != QMessageBox::Yes) return;
    for (const QString& p : paths) {
        QString err;
        if (!m_ctx->services().assets().remove(p, &err)) Q_EMIT m_ctx->statusMessage(err, 4000);
    }
    m_model->refresh();
}

void ContentBrowserPanel::showContextMenu(const QPoint& pos) {
    const QModelIndex i = m_list->indexAt(pos);
    const AssetInfo* a = m_model->at(i);
    QMenu menu(this);
    if (a) {
        menu.addAction(Icons::get(QStringLiteral("open")), a->isFolder ? tr("Open Folder") : tr("Open"), this, [this, i] { activate(i); });
        menu.addAction(Icons::get(QStringLiteral("rename")), tr("Rename"), QKeySequence(Qt::Key_F2), this, [this, i] { m_list->edit(i); });
        if (!a->isFolder && m_ctx->services().assets().isDatabase() && !a->importer.isEmpty()) {
            const QString p = a->path;
            menu.addAction(Icons::get(QStringLiteral("refresh")), tr("Reimport"), this, [this, p] {
                QString err;
                if (!m_ctx->services().assets().reimport(p, &err)) Q_EMIT m_ctx->statusMessage(tr("Reimport failed: %1").arg(err), 6000);
                else Q_EMIT m_ctx->statusMessage(tr("Reimported %1").arg(QFileInfo(p).fileName()), 3000);
                m_model->refresh();
            });
        }
        if (!a->isFolder) {
            const QString p = a->path;
            menu.addAction(Icons::get(QStringLiteral("inspector")), tr("Inspect"), this, [this, p] {
                m_ctx->selection().clear();
                m_ctx->inspectAsset(p);
            });
        }
        if (a->type == QLatin1String("Model") || a->type == QLatin1String("Prefab")) {
            const QString p = a->path;
            menu.addAction(Icons::get(QStringLiteral("prefab")), tr("Place in Scene"), this, [this, p] { m_ctx->instantiatePrefab(p); });
        }
        menu.addAction(Icons::get(QStringLiteral("trash"), Icons::Tint::Error), tr("Delete"), QKeySequence::Delete, this, &ContentBrowserPanel::deleteSelected);
        menu.addSeparator();
        const QString path = a->path;
        menu.addAction(Icons::get(QStringLiteral("copy")), tr("Copy Path"), this, [path] { QApplication::clipboard()->setText(path); });
        menu.addAction(Icons::get(QStringLiteral("external")), tr("Show in Finder"), this,
                       [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath())); });
    } else {
        auto* create = menu.addMenu(Icons::get(QStringLiteral("add")), tr("Create"));
        create->addAction(Icons::get(QStringLiteral("folder-plus")), tr("New Folder"), this, [this] { createAsset(QStringLiteral("Folder")); });
        create->addAction(Icons::get(QStringLiteral("scene")), tr("Scene"), this, [this] { createAsset(QStringLiteral("Scene")); });
        create->addAction(Icons::get(QStringLiteral("prefab")), tr("Prefab"), this, [this] { createAsset(QStringLiteral("Prefab")); });
        create->addAction(Icons::get(QStringLiteral("material")), tr("Material"), this, [this] { createAsset(QStringLiteral("Material")); });
        create->addAction(Icons::get(QStringLiteral("script")), tr("Lua Script"), this, [this] { createAsset(QStringLiteral("Script")); });
        create->addAction(Icons::get(QStringLiteral("sitemap")), tr("Behavior Tree"), this, [this] { createAsset(QStringLiteral("BehaviorTree")); });
        menu.addAction(Icons::get(QStringLiteral("import")), tr("Import…"), this, &ContentBrowserPanel::importFiles);
        menu.addAction(Icons::get(QStringLiteral("refresh")), tr("Refresh"), this, [this] {
            m_ctx->services().assets().rescan();
            m_model->refresh();
        });
    }
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

} // namespace ox::editor
