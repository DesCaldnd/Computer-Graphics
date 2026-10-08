#include "panels/behavior_tree_panel.hpp"

#include "core/editor_context.hpp"
#include "integration/gameplay_tools.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

namespace ox::editor {

namespace {
QString statusName(int s) {
    switch (s) {
    case 1: return QObject::tr("Running");
    case 2: return QObject::tr("Success");
    case 3: return QObject::tr("Failure");
    default: return QObject::tr("Idle");
    }
}
QColor statusColor(int s) {
    const ThemePalette& c = colors();
    switch (s) {
    case 1: return c.info;
    case 2: return c.success;
    case 3: return c.error;
    default: return c.textFaint;
    }
}
QIcon statusDot(int s) {
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(statusColor(s));
    p.drawEllipse(QRectF(1.5, 1.5, 9, 9));
    return QIcon(pm);
}
} // namespace

BehaviorTreePanel::BehaviorTreePanel(EditorContext* ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("BehaviorTreePanel"));
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto* header = new QFrame(this);
    header->setProperty("role", "panelHeader");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 6, 8, 6);
    auto* icon = new QLabel(header);
    icon->setPixmap(Icons::get(QStringLiteral("sitemap"), Icons::Tint::Accent).pixmap(QSize(16, 16)));
    hl->addWidget(icon);
    m_title = new QLabel(header);
    QFont f = m_title->font();
    f.setWeight(QFont::DemiBold);
    m_title->setFont(f);
    hl->addWidget(m_title);
    m_status = new QLabel(header);
    m_status->setProperty("role", "dim");
    hl->addWidget(m_status, 1);
    lay->addWidget(header);

    auto* split = new QSplitter(Qt::Vertical, this);
    m_nodes = new QTreeWidget(split);
    m_nodes->setObjectName(QStringLiteral("BtNodes"));
    m_nodes->setHeaderLabels({tr("Node"), tr("Type"), tr("Status"), tr("Last tick")});
    m_nodes->setUniformRowHeights(true);
    m_nodes->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_nodes->setColumnWidth(1, 130);
    m_nodes->setColumnWidth(2, 90);
    m_nodes->setColumnWidth(3, 80);
    m_blackboard = new QTreeWidget(split);
    m_blackboard->setObjectName(QStringLiteral("BtBlackboard"));
    m_blackboard->setHeaderLabels({tr("Blackboard key"), tr("Value")});
    m_blackboard->setRootIsDecorated(false);
    m_blackboard->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_blackboard->setColumnWidth(0, 180);
    split->addWidget(m_nodes);
    split->addWidget(m_blackboard);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    lay->addWidget(split, 1);

    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &BehaviorTreePanel::refresh);
    connect(&ctx->selection(), &Selection::changed, this, &BehaviorTreePanel::refresh);
    connect(ctx, &EditorContext::playStateChanged, this, &BehaviorTreePanel::refresh);
    refresh();
}

void BehaviorTreePanel::showEvent(QShowEvent* e) {
    m_timer.start();
    refresh();
    QWidget::showEvent(e);
}

void BehaviorTreePanel::hideEvent(QHideEvent* e) {
    m_timer.stop();
    QWidget::hideEvent(e);
}

void BehaviorTreePanel::refresh() {
    const Uuid id = m_ctx->selection().primary();
    Entity e = id.isNil() ? Entity{} : m_ctx->world().find(id);
    const BtSnapshot snap = e ? behaviorTreeSnapshot(m_ctx->engineServices(), m_ctx->world(), id) : BtSnapshot{};
    m_title->setText(e ? qs(e.name()) : tr("No selection"));
    if (!snap.hasComponent) {
        m_status->setText(e ? tr("The selected entity has no BehaviorTree component") : tr("Select an entity with a BehaviorTree component"));
        m_nodes->clear();
        m_blackboard->clear();
        m_signature.clear();
        return;
    }
    m_status->setText(snap.running ? tr("%1 · live · tick %2").arg(snap.source).arg(snap.tickCount)
                                   : tr("%1 · not running (press Play)").arg(snap.source));
    // Rebuild the item tree only when the structure changes; otherwise update statuses in place.
    QString sig;
    for (const auto& n : snap.nodes) sig += QString::number(n.id) + QLatin1Char('/') + QString::number(n.parentId) + n.type + QLatin1Char(';');
    if (sig != m_signature) {
        m_signature = sig;
        m_nodes->clear();
        std::map<u32, QTreeWidgetItem*> items;
        for (const auto& n : snap.nodes) {
            auto* it = new QTreeWidgetItem();
            it->setData(0, Qt::UserRole, n.id);
            items[n.id] = it;
            auto parent = n.parentId != n.id ? items.find(n.parentId) : items.end();
            if (parent != items.end()) parent->second->addChild(it);
            else m_nodes->addTopLevelItem(it);
        }
        m_nodes->expandAll();
    }
    QTreeWidgetItemIterator iter(m_nodes);
    std::map<u32, const BtNodeRow*> rows;
    for (const auto& n : snap.nodes) rows[n.id] = &n;
    for (; *iter; ++iter) {
        QTreeWidgetItem* it = *iter;
        auto r = rows.find(it->data(0, Qt::UserRole).toUInt());
        if (r == rows.end()) continue;
        const BtNodeRow& n = *r->second;
        it->setText(0, n.name.isEmpty() ? n.type : n.name);
        it->setText(1, n.type);
        it->setText(2, snap.running ? statusName(n.status) : QStringLiteral("—"));
        it->setIcon(0, statusDot(snap.running ? n.status : 0));
        it->setForeground(2, statusColor(snap.running ? n.status : 0));
        it->setText(3, snap.running ? QString::number(n.lastTick) : QString());
        QFont font = it->font(0);
        font.setBold(n.tickedThisFrame);
        it->setFont(0, font);
    }
    m_blackboard->clear();
    for (const auto& [k, v] : snap.blackboard) m_blackboard->addTopLevelItem(new QTreeWidgetItem({k, v}));
}

} // namespace ox::editor
