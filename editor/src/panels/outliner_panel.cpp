#include "panels/outliner_panel.hpp"

#include "core/editor_context.hpp"
#include "inspector/property_editors.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/scene/components.hpp>

#include <QEvent>
#include <QFileDialog>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include <set>

namespace ox::editor {

namespace {

QString iconFor(Entity e, QString* type) {
    if (auto* l = e.tryGet<LightComponent>()) {
        *type = QObject::tr("Light Source");
        switch (l->type) {
        case LightType::Directional: return QStringLiteral("sun");
        case LightType::Spot: return QStringLiteral("light-spot");
        case LightType::AreaRect: return QStringLiteral("light-area");
        default: return QStringLiteral("light-point");
        }
    }
    if (e.has<CameraComponent>()) {
        *type = QObject::tr("Camera");
        return QStringLiteral("camera");
    }
    if (e.has<MeshRendererComponent>()) {
        *type = QObject::tr("Static Mesh");
        return QStringLiteral("cube");
    }
    if (e.has<EnvironmentComponent>()) {
        *type = QObject::tr("Environment");
        return QStringLiteral("environment");
    }
    if (e.childCount() > 0) {
        *type = QObject::tr("Group");
        return QStringLiteral("folder");
    }
    *type = QObject::tr("Entity");
    return QStringLiteral("empty");
}

// Rows with a subtle hover/selection pill, prefab tint and dimmed inactive entities.
class OutlinerDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        QSize s = QStyledItemDelegate::sizeHint(o, i);
        s.setHeight(std::max(s.height(), 24));
        return s;
    }
};

} // namespace

// ---- model --------------------------------------------------------------------------------------------------

OutlinerModel::OutlinerModel(EditorContext* ctx, QObject* parent) : QAbstractItemModel(parent), m_ctx(ctx) { rebuild(); }

void OutlinerModel::rebuild() {
    beginResetModel();
    m_root.children.clear();
    m_byId.clear();
    World& w = m_ctx->world();
    std::function<void(Node*, Entity)> add = [&](Node* parent, Entity e) {
        auto n = std::make_unique<Node>();
        n->id = e.uuid();
        n->parent = parent;
        n->row = int(parent->children.size());
        n->name = qs(e.name());
        n->icon = iconFor(e, &n->type);
        n->active = e.active();
        n->prefab = e.has<PrefabInstanceComponent>();
        if (n->prefab) n->icon = e.get<PrefabInstanceComponent>().isRoot ? QStringLiteral("prefab") : n->icon;
        Node* raw = n.get();
        m_byId[n->id] = raw;
        parent->children.push_back(std::move(n));
        for (Entity c : e.children()) add(raw, c);
    };
    for (Entity r : w.roots()) add(&m_root, r);
    endResetModel();
}

OutlinerModel::Node* OutlinerModel::nodeOf(const QModelIndex& index) const {
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : const_cast<Node*>(&m_root);
}

QModelIndex OutlinerModel::index(int row, int column, const QModelIndex& parent) const {
    Node* p = nodeOf(parent);
    if (row < 0 || row >= int(p->children.size()) || column < 0 || column >= ColumnCount) return {};
    return createIndex(row, column, p->children[size_t(row)].get());
}

QModelIndex OutlinerModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    Node* n = nodeOf(child);
    if (!n->parent || n->parent == &m_root) return {};
    return createIndex(n->parent->row, 0, n->parent);
}

int OutlinerModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() && parent.column() != 0) return 0;
    return int(nodeOf(parent)->children.size());
}

QModelIndex OutlinerModel::indexOf(const Uuid& id, int column) const {
    auto it = m_byId.find(id);
    if (it == m_byId.end()) return {};
    return createIndex(it->second->row, column, it->second);
}

Uuid OutlinerModel::idOf(const QModelIndex& index) const { return index.isValid() ? nodeOf(index)->id : Uuid{}; }

QVariant OutlinerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const Node* n = nodeOf(index);
    const ThemePalette& c = colors();
    if (role == UuidRole) return QVariant::fromValue(n->id);
    if (role == TypeRole) return n->type;
    switch (index.column()) {
    case NameColumn:
        if (role == Qt::DisplayRole || role == Qt::EditRole) return n->name;
        if (role == Qt::DecorationRole) {
            if (n->prefab) return Icons::get(n->icon, Icons::Tint::Accent);
            return Icons::get(n->icon);
        }
        if (role == Qt::ForegroundRole) {
            if (!n->active || m_ctx->isHidden(n->id)) return c.textFaint;
            if (n->prefab) return c.accentText;
            return c.text;
        }
        if (role == Qt::ToolTipRole) {
            return QStringLiteral("<b>%1</b><br/>%2%3<br/><span style='color:%4'>%5</span>")
                .arg(n->name.toHtmlEscaped(), n->type, n->prefab ? tr(" · prefab instance") : QString(), cssColor(c.textFaint),
                     qs(n->id.toString()));
        }
        break;
    case VisibleColumn:
        if (role == Qt::DecorationRole) {
            const bool hidden = m_ctx->isHidden(n->id);
            return Icons::get(hidden ? QStringLiteral("eye-off") : QStringLiteral("eye"), hidden ? Icons::Tint::Faint : Icons::Tint::Dim);
        }
        if (role == Qt::ToolTipRole) return tr("Toggle visibility in the editor viewport");
        break;
    case LockColumn:
        if (role == Qt::DecorationRole) {
            const bool locked = m_ctx->isLocked(n->id);
            return locked ? Icons::get(QStringLiteral("lock"), Icons::Tint::Warning) : Icons::get(QStringLiteral("unlock"), Icons::Tint::Faint);
        }
        if (role == Qt::ToolTipRole) return tr("Lock: prevents selecting the entity in the viewport");
        break;
    default: break;
    }
    return {};
}

bool OutlinerModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (role != Qt::EditRole || index.column() != NameColumn) return false;
    const QString name = value.toString().trimmed();
    if (name.isEmpty()) return false;
    m_ctx->renameEntity(idOf(index), name);
    return true;
}

Qt::ItemFlags OutlinerModel::flags(const QModelIndex& index) const {
    if (!index.isValid()) return Qt::ItemIsDropEnabled;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled;
    if (index.column() == NameColumn) f |= Qt::ItemIsEditable;
    return f;
}

QStringList OutlinerModel::mimeTypes() const { return {QString::fromLatin1(kMimeEntities), QString::fromLatin1(kMimeAsset)}; }

QMimeData* OutlinerModel::mimeData(const QModelIndexList& indexes) const {
    QStringList ids;
    for (const auto& i : indexes) {
        if (i.column() != NameColumn) continue;
        const QString s = qs(idOf(i).toString());
        if (!ids.contains(s)) ids << s;
    }
    auto* m = new QMimeData();
    m->setData(kMimeEntities, ids.join(QLatin1Char('\n')).toUtf8());
    return m;
}

bool OutlinerModel::canDropMimeData(const QMimeData* data, Qt::DropAction, int, int, const QModelIndex&) const {
    if (data->hasFormat(kMimeEntities)) return true;
    if (data->hasFormat(kMimeAsset)) return QString::fromUtf8(data->data(kMimeAsset)).contains(QLatin1String("|Prefab|"));
    return false;
}

bool OutlinerModel::dropMimeData(const QMimeData* data, Qt::DropAction action, int row, int column, const QModelIndex& parent) {
    (void)action;
    (void)column;
    const Uuid parentId = parent.isValid() ? idOf(parent.siblingAtColumn(0)) : Uuid{};
    if (data->hasFormat(kMimeAsset)) {
        for (const QString& line : QString::fromUtf8(data->data(kMimeAsset)).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const QStringList parts = line.split(QLatin1Char('|'));
            if (parts.size() >= 3 && parts[1] == QLatin1String("Prefab")) m_ctx->instantiatePrefab(parts[2], parentId);
        }
        return false;
    }
    UuidList ids;
    for (const QString& s : QString::fromUtf8(data->data(kMimeEntities)).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (auto id = Uuid::parse(s.toStdString())) ids.push_back(*id);
    }
    if (ids.empty()) return false;
    m_ctx->reparentEntities(ids, parentId, row);
    return false; // the model is rebuilt from the world; nothing for the view to remove
}

// ---- panel --------------------------------------------------------------------------------------------------

void OutlinerPanel::populateCreateMenu(QMenu* menu, EditorContext* ctx, const Uuid& parent) {
    auto add = [&](CreateKind k) {
        menu->addAction(Icons::get(createKindIcon(k)), createKindName(k), ctx, [ctx, k, parent] { ctx->createEntity(k, parent); });
    };
    add(CreateKind::Empty);
    menu->addSection(QObject::tr("Shapes"));
    add(CreateKind::Cube);
    add(CreateKind::Sphere);
    add(CreateKind::Cylinder);
    add(CreateKind::Plane);
    menu->addSection(QObject::tr("Lights"));
    add(CreateKind::DirectionalLight);
    add(CreateKind::PointLight);
    add(CreateKind::SpotLight);
    add(CreateKind::AreaLight);
    menu->addSection(QObject::tr("Other"));
    add(CreateKind::Camera);
    add(CreateKind::Environment);
}

OutlinerPanel::OutlinerPanel(EditorContext* ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("OutlinerPanel"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* header = new QFrame(this);
    header->setProperty("role", "panelHeader");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 6, 8, 6);
    hl->setSpacing(6);
    auto* create = new QToolButton(header);
    create->setIcon(Icons::get(QStringLiteral("add")));
    create->setToolTip(tr("Create entity"));
    create->setPopupMode(QToolButton::InstantPopup);
    auto* createMenu = new QMenu(create);
    connect(createMenu, &QMenu::aboutToShow, this, [this, createMenu] {
        createMenu->clear();
        populateCreateMenu(createMenu, m_ctx, {});
    });
    create->setMenu(createMenu);
    m_search = new SearchField(tr("Search entities…"), header);
    hl->addWidget(create);
    hl->addWidget(m_search, 1);
    lay->addWidget(header);

    m_model = new OutlinerModel(ctx, this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setRecursiveFilteringEnabled(true);
    m_proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxy->setFilterKeyColumn(OutlinerModel::NameColumn);
    m_proxy->setAutoAcceptChildRows(true);

    m_view = new QTreeView(this);
    m_view->setObjectName(QStringLiteral("OutlinerView"));
    m_view->setModel(m_proxy);
    m_view->setItemDelegate(new OutlinerDelegate(m_view));
    m_view->setHeaderHidden(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setDragDropMode(QAbstractItemView::DragDrop);
    m_view->setDefaultDropAction(Qt::MoveAction);
    m_view->setDragEnabled(true);
    m_view->setAcceptDrops(true);
    m_view->setDropIndicatorShown(true);
    m_view->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
    m_view->setUniformRowHeights(true);
    m_view->setIndentation(16);
    m_view->setIconSize(QSize(16, 16));
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setAnimated(true);
    m_view->header()->setStretchLastSection(false);
    m_view->header()->setSectionResizeMode(OutlinerModel::NameColumn, QHeaderView::Stretch);
    m_view->header()->setSectionResizeMode(OutlinerModel::VisibleColumn, QHeaderView::Fixed);
    m_view->header()->setSectionResizeMode(OutlinerModel::LockColumn, QHeaderView::Fixed);
    m_view->header()->resizeSection(OutlinerModel::VisibleColumn, 26);
    m_view->header()->resizeSection(OutlinerModel::LockColumn, 26);
    lay->addWidget(m_view, 1);

    auto* footer = new QFrame(this);
    footer->setProperty("role", "footer");
    auto* fl = new QHBoxLayout(footer);
    fl->setContentsMargins(10, 4, 10, 4);
    m_footer = new QLabel(footer);
    m_footer->setProperty("role", "faint");
    fl->addWidget(m_footer);
    lay->addWidget(footer);

    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &OutlinerPanel::rebuildNow);
    connect(ctx, &EditorContext::structureChanged, &m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(ctx, &EditorContext::worldReset, &m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(ctx, &EditorContext::visibilityChanged, m_view->viewport(), qOverload<>(&QWidget::update));
    connect(&ctx->selection(), &Selection::changed, this, &OutlinerPanel::syncSelectionToView);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, &OutlinerPanel::syncSelectionFromView);
    connect(m_search, &QLineEdit::textChanged, this, &OutlinerPanel::setFilterText);
    connect(m_view, &QTreeView::customContextMenuRequested, this, &OutlinerPanel::showContextMenu);
    connect(m_view, &QTreeView::clicked, this, [this](const QModelIndex& pi) {
        const QModelIndex i = m_proxy->mapToSource(pi);
        const Uuid id = m_model->idOf(i);
        if (i.column() == OutlinerModel::VisibleColumn) m_ctx->setHidden(id, !m_ctx->isHidden(id));
        if (i.column() == OutlinerModel::LockColumn) m_ctx->setLocked(id, !m_ctx->isLocked(id));
    });
    connect(&Theme::instance(), &Theme::changed, m_view->viewport(), qOverload<>(&QWidget::update));
    rebuildNow();
}

void OutlinerPanel::rebuildNow() {
    m_rebuildTimer.stop();
    // remember expansion by UUID
    std::set<Uuid> expanded;
    std::set<Uuid> known;
    std::function<void(const QModelIndex&)> walk = [&](const QModelIndex& parent) {
        for (int r = 0; r < m_proxy->rowCount(parent); ++r) {
            QModelIndex i = m_proxy->index(r, 0, parent);
            const Uuid id = m_model->idOf(m_proxy->mapToSource(i));
            known.insert(id);
            if (m_view->isExpanded(i)) expanded.insert(id);
            walk(i);
        }
    };
    walk({});
    const bool first = known.empty();
    m_syncing = true;
    m_model->rebuild();
    std::function<void(const QModelIndex&)> restore = [&](const QModelIndex& parent) {
        for (int r = 0; r < m_proxy->rowCount(parent); ++r) {
            QModelIndex i = m_proxy->index(r, 0, parent);
            const Uuid id = m_model->idOf(m_proxy->mapToSource(i));
            if (first || expanded.count(id) || !known.count(id)) m_view->setExpanded(i, true);
            restore(i);
        }
    };
    restore({});
    m_syncing = false;
    syncSelectionToView();
    updateFooter();
}

void OutlinerPanel::setFilterText(const QString& text) {
    m_proxy->setFilterFixedString(text);
    if (!text.isEmpty()) m_view->expandAll();
}

void OutlinerPanel::syncSelectionToView() {
    if (m_syncing) return;
    m_syncing = true;
    QItemSelection sel;
    for (const auto& id : m_ctx->selection().ids()) {
        QModelIndex i = m_proxy->mapFromSource(m_model->indexOf(id));
        if (!i.isValid()) continue;
        sel.select(i, i.siblingAtColumn(OutlinerModel::ColumnCount - 1));
        for (QModelIndex p = i.parent(); p.isValid(); p = p.parent()) m_view->setExpanded(p, true);
    }
    m_view->selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
    const QModelIndex cur = m_proxy->mapFromSource(m_model->indexOf(m_ctx->selection().primary()));
    if (cur.isValid()) {
        m_view->selectionModel()->setCurrentIndex(cur, QItemSelectionModel::NoUpdate);
        m_view->scrollTo(cur);
    }
    m_syncing = false;
    updateFooter();
}

void OutlinerPanel::syncSelectionFromView() {
    if (m_syncing) return;
    m_syncing = true;
    UuidList ids;
    for (const QModelIndex& i : m_view->selectionModel()->selectedRows(0)) ids.push_back(m_model->idOf(m_proxy->mapToSource(i)));
    const Uuid cur = m_model->idOf(m_proxy->mapToSource(m_view->selectionModel()->currentIndex()));
    if (!cur.isNil() && std::find(ids.begin(), ids.end(), cur) != ids.end()) {
        std::erase(ids, cur);
        ids.push_back(cur);
    }
    m_ctx->selection().set(ids);
    m_syncing = false;
    updateFooter();
}

void OutlinerPanel::renameSelected() {
    const QModelIndex cur = m_proxy->mapFromSource(m_model->indexOf(m_ctx->selection().primary()));
    if (cur.isValid()) m_view->edit(cur);
}

void OutlinerPanel::showContextMenu(const QPoint& pos) {
    const QModelIndex pi = m_view->indexAt(pos);
    const Uuid clicked = m_model->idOf(m_proxy->mapToSource(pi));
    if (!clicked.isNil() && !m_ctx->selection().contains(clicked)) m_ctx->selection().select(clicked);
    const UuidList sel = m_ctx->selection().ids();
    QMenu menu(this);
    auto* createMenu = menu.addMenu(Icons::get(QStringLiteral("add")), clicked.isNil() ? tr("Create") : tr("Create Child"));
    populateCreateMenu(createMenu, m_ctx, clicked);
    if (!sel.empty()) {
        menu.addSeparator();
        menu.addAction(Icons::get(QStringLiteral("rename")), tr("Rename"), QKeySequence(Qt::Key_F2), this, &OutlinerPanel::renameSelected);
        menu.addAction(Icons::get(QStringLiteral("duplicate")), tr("Duplicate"), QKeySequence(Qt::CTRL | Qt::Key_D), this,
                       [this, sel] { m_ctx->duplicateEntities(sel); });
        menu.addAction(Icons::get(QStringLiteral("copy")), tr("Copy"), QKeySequence::Copy, this, [this, sel] { m_ctx->copy(sel); });
        menu.addAction(Icons::get(QStringLiteral("cut")), tr("Cut"), QKeySequence::Cut, this, [this, sel] { m_ctx->cut(sel); });
    }
    auto* paste = menu.addAction(Icons::get(QStringLiteral("paste")), tr("Paste"), QKeySequence::Paste, this,
                                 [this, clicked] { m_ctx->paste(clicked); });
    paste->setEnabled(m_ctx->canPaste());
    if (!sel.empty()) {
        menu.addSeparator();
        menu.addAction(Icons::get(QStringLiteral("arrow-up")), tr("Unparent"), this, [this, sel] { m_ctx->reparentEntities(sel, {}); });
        menu.addAction(Icons::get(QStringLiteral("prefab")), tr("Create Prefab…"), this, [this, sel] {
            const QString dir = m_ctx->project() ? m_ctx->project()->contentDir() + QStringLiteral("/Prefabs") : QString();
            Entity e = m_ctx->world().find(sel.back());
            const QString path = QFileDialog::getSaveFileName(this, tr("Create Prefab"), dir + QLatin1Char('/') + qs(e.name()) + ".oxprefab",
                                                              tr("Prefab (*.oxprefab);;Prefab JSON (*.oxprefab.json)"));
            if (path.isEmpty()) return;
            QString err;
            if (!m_ctx->createPrefab(sel.back(), path, &err)) Q_EMIT m_ctx->statusMessage(err, 4000);
        });
        menu.addSeparator();
        menu.addAction(Icons::get(QStringLiteral("trash"), Icons::Tint::Error), tr("Delete"), QKeySequence::Delete, this,
                       [this, sel] { m_ctx->deleteEntities(sel); });
    }
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

void OutlinerPanel::updateFooter() {
    const int n = m_model->entityCount();
    const int s = int(m_ctx->selection().size());
    QString t = tr("%n entities", nullptr, n);
    if (s > 0) t += tr(" · %n selected", nullptr, s);
    if (m_ctx->isPlaying()) t += tr(" · play world");
    m_footer->setText(t);
}

void OutlinerPanel::changeEvent(QEvent* e) {
    if (e->type() == QEvent::LanguageChange) m_rebuildTimer.start();
    QWidget::changeEvent(e);
}

} // namespace ox::editor
