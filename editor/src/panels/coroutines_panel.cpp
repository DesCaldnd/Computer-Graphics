#include "panels/coroutines_panel.hpp"

#include "core/editor_context.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/scene/runtime_id.hpp>

#if OX_EDITOR_HAS_ASYNC
#include <oxwald/async/scheduler.hpp>
#endif

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

namespace ox::editor {

namespace {
enum Column { ColName, ColOwner, ColState, ColWaiting, ColAge, ColCount };
enum Role { IdRole = Qt::UserRole + 1, OwnerRole, EntityRole };
} // namespace

CoroutinesPanel::CoroutinesPanel(EditorContext* ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("CoroutinesPanel"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* header = new QFrame(this);
    header->setProperty("role", "panelHeader");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 6, 8, 6);
    hl->setSpacing(6);
    m_search = new SearchField(tr("Filter by name, owner or wait…"), header);
    m_search->setMaximumWidth(260);
    hl->addWidget(m_search);
    m_summary = new QLabel(header);
    m_summary->setProperty("role", "dim");
    hl->addWidget(m_summary, 1);
    m_pause = makeToolButton(QStringLiteral("pause"), tr("Freeze the list"), header, true);
    hl->addWidget(m_pause);
    m_cancel = makeToolButton(QStringLiteral("stop"), tr("Cancel the selected coroutines"), header);
    m_cancel->setObjectName(QStringLiteral("coroutines.cancel"));
    hl->addWidget(m_cancel);
    lay->addWidget(header);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("CoroutinesTree"));
    m_tree->setColumnCount(ColCount);
    m_tree->setHeaderLabels({tr("Coroutine"), tr("Owner"), tr("State"), tr("Waiting on"), tr("Age")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(ColWaiting, QHeaderView::Stretch);
    m_tree->setColumnWidth(ColOwner, 150);
    m_tree->setColumnWidth(ColState, 100);
    m_tree->setColumnWidth(ColAge, 120);
    lay->addWidget(m_tree, 1);

    connect(m_cancel, &QToolButton::clicked, this, [this] {
        int n = 0;
        for (QTreeWidgetItem* it : m_tree->selectedItems()) {
            n += cancel(it->data(ColName, IdRole).toULongLong(), it->data(ColName, OwnerRole).toULongLong()) ? 1 : 0;
        }
        if (n) Q_EMIT m_ctx->statusMessage(tr("Cancelled %n coroutine(s)", nullptr, n), 2500);
        refresh();
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        const QString id = it->data(ColOwner, EntityRole).toString();
        if (auto u = Uuid::parse(id.toStdString())) m_ctx->selection().select(*u);
    });
    connect(m_search, &QLineEdit::textChanged, this, &CoroutinesPanel::refresh);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] { m_cancel->setEnabled(!m_tree->selectedItems().isEmpty()); });
    m_cancel->setEnabled(false);
    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!m_pause->isChecked()) refresh();
    });
    refresh();
}

void CoroutinesPanel::showEvent(QShowEvent* e) {
    m_timer.start();
    refresh();
    QWidget::showEvent(e);
}

void CoroutinesPanel::hideEvent(QHideEvent* e) {
    m_timer.stop();
    QWidget::hideEvent(e);
}

bool CoroutinesPanel::cancel(unsigned long long id, unsigned long long owner) {
#if OX_EDITOR_HAS_ASYNC
    auto* s = m_ctx->engineServices().tryGet<CoroutineScheduler>();
    if (!s || owner == 0) return false;
    for (const CoroutineHandle& h : s->handlesForOwner(owner)) {
        if (h.id() == id) {
            h.cancel();
            return true;
        }
    }
#else
    (void)id;
    (void)owner;
#endif
    return false;
}

void CoroutinesPanel::refresh() {
#if OX_EDITOR_HAS_ASYNC
    auto* sched = m_ctx->engineServices().tryGet<CoroutineScheduler>();
    if (!sched) {
        m_tree->clear();
        m_count = 0;
        m_summary->setText(tr("No coroutine scheduler (engine without the async module)"));
        return;
    }
    const std::vector<CoroutineInfo> infos = sched->coroutines();
    // Preserve selection/expansion across rebuilds by id.
    QSet<qulonglong> selected;
    for (QTreeWidgetItem* it : m_tree->selectedItems()) selected.insert(it->data(ColName, IdRole).toULongLong());
    const QString filter = m_search->text().trimmed();
    World& world = m_ctx->world();
    const ThemePalette& c = colors();
    QSignalBlocker b(m_tree);
    m_tree->clear();
    std::map<u64, QTreeWidgetItem*> items;
    int running = 0, waiting = 0, background = 0;
    m_count = 0;
    for (const CoroutineInfo& info : infos) {
        QString owner = info.owner ? QStringLiteral("#%1").arg(info.owner) : tr("—");
        QString entityId;
        if (info.owner) {
            const entt::entity h = entityFromRuntimeId(info.owner);
            if (h != entt::null && world.valid(h)) {
                Entity e = world.wrap(h);
                owner = qs(e.name());
                entityId = qs(e.uuid().toString());
            }
        }
        const QString name = info.name.empty() ? tr("(unnamed #%1)").arg(info.id) : qs(info.name);
        const QString wait = qs(info.waitingOn);
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive) && !owner.contains(filter, Qt::CaseInsensitive) &&
            !wait.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        auto* it = new QTreeWidgetItem();
        it->setText(ColName, name);
        it->setIcon(ColName, Icons::get(QStringLiteral("coroutine")));
        it->setData(ColName, IdRole, qulonglong(info.id));
        it->setData(ColName, OwnerRole, qulonglong(info.owner));
        it->setText(ColOwner, owner);
        it->setData(ColOwner, EntityRole, entityId);
        if (!entityId.isEmpty()) it->setIcon(ColOwner, Icons::get(QStringLiteral("entity")));
        const QString state = qs(toString(info.state));
        it->setText(ColState, state);
        QColor sc = c.textDim;
        switch (info.state) {
        case CoroutineState::Running: sc = c.success; ++running; break;
        case CoroutineState::OffThread: sc = c.info; ++background; break;
        case CoroutineState::CancelPending: sc = c.error; break;
        default: sc = c.warning; ++waiting; break;
        }
        it->setForeground(ColState, sc);
        it->setText(ColWaiting, wait.isEmpty() ? tr("—") : wait);
        it->setText(ColAge, tr("%1 s · %2 fr").arg(info.ageSeconds, 0, 'f', 2).arg(info.ageFrames));
        it->setToolTip(ColAge, tr("Game time %1 s, real time %2 s").arg(info.ageSeconds, 0, 'f', 3).arg(info.ageRealSeconds, 0, 'f', 3));
        if (!info.owner) it->setToolTip(ColName, tr("Coroutines without an owner cannot be cancelled from the editor"));
        items[info.id] = it;
        ++m_count;
    }
    for (const CoroutineInfo& info : infos) {
        auto it = items.find(info.id);
        if (it == items.end()) continue;
        auto parent = info.parentId ? items.find(info.parentId) : items.end();
        if (parent != items.end()) parent->second->addChild(it->second);
        else m_tree->addTopLevelItem(it->second);
        if (selected.contains(qulonglong(info.id))) it->second->setSelected(true);
    }
    m_tree->expandAll();
    m_summary->setText(tr("%1 coroutines · %2 suspended · %3 background · tick %4").arg(infos.size()).arg(waiting).arg(background).arg(sched->tickCount()));
    (void)running;
#else
    m_tree->clear();
    m_count = 0;
    m_summary->setText(tr("The async module is not linked"));
#endif
}

} // namespace ox::editor
