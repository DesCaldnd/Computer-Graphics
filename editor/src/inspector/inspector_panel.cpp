#include "inspector/inspector_panel.hpp"

#include "inspector/asset_inspector.hpp"

#include "core/editor_context.hpp"
#include "inspector/component_card.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/prefab.hpp>

#include <QCheckBox>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

namespace ox::editor {

// ---- AddComponentPopup --------------------------------------------------------------------------------------

AddComponentPopup::AddComponentPopup(EditorContext* ctx, const UuidList& entities, QWidget* parent)
    : QFrame(parent, Qt::Popup), m_ctx(ctx), m_entities(entities) {
    setObjectName(QStringLiteral("AddComponentPopup"));
    setAttribute(Qt::WA_DeleteOnClose);
    setProperty("role", "card");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setSpacing(6);
    auto* title = new QLabel(tr("Add Component"), this);
    title->setProperty("role", "title");
    lay->addWidget(title);
    m_search = new SearchField(tr("Search components…"), this);
    lay->addWidget(m_search);
    m_tree = new QTreeWidget(this);
    m_tree->setHeaderHidden(true);
    m_tree->setRootIsDecorated(true);
    m_tree->setIndentation(14);
    m_tree->setIconSize(QSize(16, 16));
    m_tree->setMinimumHeight(260);
    lay->addWidget(m_tree, 1);
    connect(m_search, &QLineEdit::textChanged, this, &AddComponentPopup::rebuild);
    connect(m_search, &QLineEdit::returnPressed, this, &AddComponentPopup::activate);
    connect(m_tree, &QTreeWidget::itemActivated, this, &AddComponentPopup::activate);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it) {
        if (it && !it->data(0, Qt::UserRole).toString().isEmpty()) activate();
    });
    m_search->installEventFilter(this);
    rebuild({});
}

void AddComponentPopup::rebuild(const QString& filter) {
    m_tree->clear();
    std::map<QString, QTreeWidgetItem*> groups;
    World& w = m_ctx->world();
    QTreeWidgetItem* first = nullptr;
    for (const ComponentInfo* info : ComponentRegistry::instance().all()) {
        if (info->hiddenInInspector || !info->removable) continue;
        const QString name = prettifyName(info->name);
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive) && !qs(info->category).contains(filter, Qt::CaseInsensitive)) continue;
        bool allHave = true;
        for (const auto& id : m_entities) {
            Entity e = w.find(id);
            allHave &= e && info->has(w, e.handle());
        }
        const QString cat = info->category.empty() ? tr("Other") : prettifyName(info->category);
        QTreeWidgetItem*& g = groups[cat];
        if (!g) {
            g = new QTreeWidgetItem(m_tree, {cat});
            g->setIcon(0, Icons::get(Icons::forCategory(qs(info->category)), Icons::Tint::Accent));
            QFont f = g->font(0);
            f.setWeight(QFont::DemiBold);
            g->setFont(0, f);
            g->setFlags(Qt::ItemIsEnabled);
            g->setExpanded(true);
        }
        auto* it = new QTreeWidgetItem(g, {name});
        it->setIcon(0, Icons::get(Icons::forComponent(qs(info->icon), qs(info->name))));
        it->setData(0, Qt::UserRole, qs(info->name));
        if (allHave) {
            it->setFlags(Qt::NoItemFlags);
            it->setToolTip(0, tr("Already present on the selection"));
        } else if (!first) {
            first = it;
        }
    }
    m_tree->expandAll();
    if (first) m_tree->setCurrentItem(first);
}

void AddComponentPopup::activate() {
    QTreeWidgetItem* it = m_tree->currentItem();
    if (!it) return;
    const QString name = it->data(0, Qt::UserRole).toString();
    if (name.isEmpty() || !(it->flags() & Qt::ItemIsEnabled)) return;
    m_ctx->addComponent(m_entities, name.toStdString());
    close();
}

void AddComponentPopup::popup(const QPoint& globalPos, int width) {
    resize(std::max(width, 280), 360);
    move(globalPos);
    show();
    m_search->setFocus();
}

// ---- InspectorPanel -----------------------------------------------------------------------------------------

InspectorPanel::InspectorPanel(EditorContext* ctx, QWidget* parent) : QWidget(parent), m_ctx(ctx) {
    setObjectName(QStringLiteral("InspectorPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack);

    // empty state
    m_empty = new QWidget(m_stack);
    {
        auto* l = new QVBoxLayout(m_empty);
        l->addStretch(2);
        auto* icon = new QLabel(m_empty);
        icon->setPixmap(Icons::get(QStringLiteral("inspector"), Icons::Tint::Faint).pixmap(QSize(40, 40)));
        icon->setAlignment(Qt::AlignCenter);
        auto* t = new QLabel(tr("Nothing selected"), m_empty);
        t->setAlignment(Qt::AlignCenter);
        t->setProperty("role", "title");
        auto* s = new QLabel(tr("Select an entity in the viewport or the outliner\nto inspect and edit its components."), m_empty);
        s->setAlignment(Qt::AlignCenter);
        s->setProperty("role", "faint");
        l->addWidget(icon);
        l->addWidget(t);
        l->addWidget(s);
        l->addStretch(3);
    }
    m_stack->addWidget(m_empty);

    m_content = new QWidget(m_stack);
    auto* cl = new QVBoxLayout(m_content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);

    // header: icon, name, active, id
    auto* header = new QFrame(m_content);
    header->setProperty("role", "panelHeader");
    auto* hl = new QVBoxLayout(header);
    hl->setContentsMargins(10, 10, 10, 10);
    hl->setSpacing(8);
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(8);
    m_entityIcon = new QLabel(header);
    m_entityIcon->setFixedSize(28, 28);
    m_entityIcon->setAlignment(Qt::AlignCenter);
    m_entityIcon->setStyleSheet(QStringLiteral("QLabel{background:%1;border-radius:7px;}").arg(cssColor(colors().accentSubtle)));
    m_nameEdit = new QLineEdit(header);
    m_nameEdit->setObjectName(QStringLiteral("InspectorName"));
    QFont nf = m_nameEdit->font();
    nf.setWeight(QFont::DemiBold);
    nf.setPixelSize(Theme::instance().fontSize() + 1);
    m_nameEdit->setFont(nf);
    m_activeBox = new QCheckBox(header);
    m_activeBox->setToolTip(tr("Active: inactive entities (and their children) are skipped by gameplay and rendering"));
    row1->addWidget(m_activeBox);
    row1->addWidget(m_entityIcon);
    row1->addWidget(m_nameEdit, 1);
    hl->addLayout(row1);
    auto* row2 = new QHBoxLayout();
    m_idLabel = new QLabel(header);
    m_idLabel->setProperty("role", "faint");
    m_idLabel->setFont(Theme::monoFont());
    m_idLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row2->addWidget(m_idLabel, 1);
    hl->addLayout(row2);

    // prefab banner
    m_prefabBanner = new QFrame(header);
    m_prefabBanner->setProperty("role", "banner");
    auto* pl = new QHBoxLayout(m_prefabBanner);
    pl->setContentsMargins(8, 6, 6, 6);
    auto* picon = new QLabel(m_prefabBanner);
    picon->setPixmap(Icons::get(QStringLiteral("prefab"), Icons::Tint::Accent).pixmap(QSize(16, 16)));
    m_prefabLabel = new QLabel(m_prefabBanner);
    auto* applyBtn = new QPushButton(tr("Apply"), m_prefabBanner);
    applyBtn->setToolTip(tr("Write this instance's changes back into the prefab asset"));
    auto* revertBtn = new QPushButton(tr("Revert"), m_prefabBanner);
    revertBtn->setToolTip(tr("Discard all overrides of this instance"));
    applyBtn->setProperty("role", "flat");
    revertBtn->setProperty("role", "flat");
    pl->addWidget(picon);
    pl->addWidget(m_prefabLabel, 1);
    pl->addWidget(revertBtn);
    pl->addWidget(applyBtn);
    hl->addWidget(m_prefabBanner);
    connect(applyBtn, &QPushButton::clicked, this, [this] {
        QString err;
        if (!m_ctx->selection().empty() && !m_ctx->applyPrefab(m_ctx->selection().primary(), &err)) {
            Q_EMIT m_ctx->statusMessage(err, 4000);
        }
    });
    connect(revertBtn, &QPushButton::clicked, this, [this] {
        if (!m_ctx->selection().empty()) m_ctx->revertAllPrefabOverrides(m_ctx->selection().primary());
    });

    // filter + add component
    auto* row3 = new QHBoxLayout();
    row3->setSpacing(6);
    m_filter = new SearchField(tr("Filter properties…"), header);
    m_addButton = new QPushButton(Icons::get(QStringLiteral("add"), Icons::Tint::OnAccent), tr("Add"), header);
    m_addButton->setObjectName(QStringLiteral("AddComponentButton"));
    m_addButton->setProperty("role", "primary");
    m_addButton->setToolTip(tr("Add a component"));
    row3->addWidget(m_filter, 1);
    row3->addWidget(m_addButton);
    hl->addLayout(row3);
    cl->addWidget(header);

    m_scroll = new QScrollArea(m_content);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* cardsHost = new QWidget(m_scroll);
    m_cardsLayout = new QVBoxLayout(cardsHost);
    m_cardsLayout->setContentsMargins(8, 8, 8, 8);
    m_cardsLayout->setSpacing(8);
    m_cardsLayout->addStretch(1);
    m_scroll->setWidget(cardsHost);
    cl->addWidget(m_scroll, 1);
    m_stack->addWidget(m_content);
    m_assetPage = new AssetInspector(ctx, m_stack);
    m_stack->addWidget(m_assetPage);

    connect(m_nameEdit, &QLineEdit::editingFinished, this, [this] {
        if (!m_nameEdit->isModified()) return;
        m_nameEdit->setModified(false);
        const QString name = m_nameEdit->text().trimmed();
        if (name.isEmpty()) return;
        UuidList ids = m_ctx->selection().ids();
        m_ctx->setProperty(ids, "Name", "name", serial::Value::makeString(name.toStdString()));
    });
    connect(m_activeBox, &QCheckBox::clicked, this, [this](bool on) {
        m_activeBox->setTristate(false);
        m_ctx->setActive(m_ctx->selection().ids(), on);
    });
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& t) {
        for (auto* c : m_cards) c->setFilter(t);
    });
    connect(m_addButton, &QPushButton::clicked, this, [this] {
        auto* popup = new AddComponentPopup(m_ctx, m_ctx->selection().ids(), this);
        popup->popup(m_addButton->mapToGlobal(QPoint(m_addButton->width() - 300, m_addButton->height() + 4)), 300);
    });

    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(0);
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_refreshTimer, &QTimer::timeout, this, &InspectorPanel::refresh);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &InspectorPanel::rebuild);
    connect(&ctx->selection(), &Selection::changed, &m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(ctx, &EditorContext::assetInspected, this, [this](const QString& path) {
        if (!path.isEmpty()) m_assetPage->setAsset(path);
        m_rebuildTimer.start();
    });
    connect(ctx, &EditorContext::worldReset, &m_rebuildTimer, qOverload<>(&QTimer::start));
    connect(ctx, &EditorContext::structureChanged, this, [this] {
        if (componentSetSignature() != m_signature) m_rebuildTimer.start();
        else m_refreshTimer.start();
    });
    connect(ctx, &EditorContext::propertiesChanged, &m_refreshTimer, qOverload<>(&QTimer::start));
    connect(&Theme::instance(), &Theme::changed, &m_rebuildTimer, qOverload<>(&QTimer::start));
    rebuild();
}

ComponentCard* InspectorPanel::card(const QString& componentName) const {
    for (auto* c : m_cards) {
        if (qs(c->info()->name) == componentName) return c;
    }
    return nullptr;
}

QString InspectorPanel::componentSetSignature() const {
    QString s;
    World& w = m_ctx->world();
    for (const auto& id : m_ctx->selection().ids()) {
        Entity e = w.find(id);
        if (!e) continue;
        s += qs(id.toString()) + QLatin1Char(':');
        for (const ComponentInfo* info : ComponentRegistry::instance().componentsOf(w, e.handle())) s += qs(info->name) + QLatin1Char(',');
        s += QLatin1Char(';');
    }
    return s;
}

void InspectorPanel::rebuild() {
    m_rebuildTimer.stop();
    for (auto* c : m_cards) {
        // Hidden + detached right away: deleteLater only runs once control returns to the event loop.
        c->hide();
        m_cardsLayout->removeWidget(c);
        c->deleteLater();
    }
    m_cards.clear();
    World& w = m_ctx->world();
    UuidList ids;
    for (const auto& id : m_ctx->selection().ids()) {
        if (w.find(id)) ids.push_back(id);
    }
    m_shown = ids;
    m_signature = componentSetSignature();
    if (ids.empty() && !m_ctx->inspectedAsset().isEmpty()) {
        m_stack->setCurrentWidget(m_assetPage);
        return;
    }
    if (ids.empty()) {
        m_stack->setCurrentWidget(m_empty);
        return;
    }
    m_stack->setCurrentWidget(m_content);
    // components present on every selected entity, in registry order
    for (const ComponentInfo* info : ComponentRegistry::instance().all()) {
        if (info->hiddenInInspector) continue;
        bool all = true;
        for (const auto& id : ids) all &= info->has(w, w.find(id).handle());
        if (!all) continue;
        auto* card = new ComponentCard(m_ctx, info, ids, m_scroll->widget());
        m_cardsLayout->insertWidget(m_cardsLayout->count() - 1, card);
        m_cards.push_back(card);
        if (!m_filter->text().isEmpty()) card->setFilter(m_filter->text());
    }
    updateHeader();
}

void InspectorPanel::refresh() {
    m_refreshTimer.stop();
    if (m_shown != m_ctx->selection().ids()) {
        rebuild();
        return;
    }
    for (auto* c : m_cards) c->refresh();
    updateHeader();
}

void InspectorPanel::updateHeader() {
    World& w = m_ctx->world();
    const UuidList& ids = m_shown;
    if (ids.empty()) return;
    Entity primary = w.find(ids.back());
    if (!primary) return;
    if (!m_nameEdit->hasFocus()) {
        if (ids.size() == 1) {
            m_nameEdit->setText(qs(primary.name()));
            m_nameEdit->setPlaceholderText({});
        } else {
            m_nameEdit->clear();
            m_nameEdit->setPlaceholderText(tr("%n entities selected", nullptr, int(ids.size())));
        }
    }
    QString icon = QStringLiteral("entity");
    if (primary.has<LightComponent>()) icon = QStringLiteral("light");
    else if (primary.has<CameraComponent>()) icon = QStringLiteral("camera");
    else if (primary.has<MeshRendererComponent>()) icon = QStringLiteral("cube");
    else if (primary.has<EnvironmentComponent>()) icon = QStringLiteral("environment");
    if (primary.has<PrefabInstanceComponent>()) icon = QStringLiteral("prefab");
    m_entityIcon->setPixmap(Icons::get(icon, Icons::Tint::Accent).pixmap(QSize(16, 16)));
    int active = 0;
    for (const auto& id : ids) active += w.find(id).active() ? 1 : 0;
    {
        QSignalBlocker b(m_activeBox);
        m_activeBox->setTristate(active != 0 && active != int(ids.size()));
        m_activeBox->setCheckState(active == 0 ? Qt::Unchecked : active == int(ids.size()) ? Qt::Checked : Qt::PartiallyChecked);
    }
    m_idLabel->setText(ids.size() == 1 ? QStringLiteral("ID  ") + qs(primary.uuid().toString())
                                       : tr("Editing %n entities - mixed values are shown as —", nullptr, int(ids.size())));
    const auto* pi = primary.tryGet<PrefabInstanceComponent>();
    m_prefabBanner->setVisible(pi != nullptr && ids.size() == 1);
    if (pi) {
        const PrefabAsset* asset = m_ctx->prefabOfEntity(primary.uuid());
        const QString name = asset ? QFileInfo(asset->path).fileName() : qs(pi->prefab.toString()).left(8);
        Entity root = prefabInstanceRoot(primary);
        usize overrides = 0;
        if (root) {
            std::vector<Entity> stack{root};
            while (!stack.empty()) {
                Entity e = stack.back();
                stack.pop_back();
                if (auto* p = e.tryGet<PrefabInstanceComponent>()) overrides += p->overrides.size();
                for (Entity c : e.children()) stack.push_back(c);
            }
        }
        m_prefabLabel->setText(tr("Prefab <b>%1</b> · %n override(s)", nullptr, int(overrides)).arg(name));
    }
}

void InspectorPanel::changeEvent(QEvent* e) {
    if (e->type() == QEvent::LanguageChange) m_rebuildTimer.start();
    QWidget::changeEvent(e);
}

} // namespace ox::editor
