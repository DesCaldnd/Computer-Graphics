#include "settings/settings_dialog.hpp"

#include "core/project.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/cvar.hpp>

#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace ox::editor {

// ---- bindings -----------------------------------------------------------------------------------------------

SettingBinding cvarBinding(const QString& name) {
    SettingBinding b;
    b.key = name;
    const std::string n = name.toStdString();
    b.get = [n]() -> QVariant {
        ICVar* c = CVarRegistry::instance().find(n);
        if (!c) return {};
        const nlohmann::json j = c->toJson();
        switch (c->type()) {
        case CVarType::Bool: return j.get<bool>();
        case CVarType::Int: return j.get<int>();
        case CVarType::Float: return double(j.get<float>());
        case CVarType::String: return QString::fromStdString(j.get<std::string>());
        }
        return {};
    };
    b.set = [n](const QVariant& v) {
        ICVar* c = CVarRegistry::instance().find(n);
        if (!c) return;
        switch (c->type()) {
        case CVarType::Bool: c->setFromJson(nlohmann::json(v.toBool()), CVarSource::Config); break;
        case CVarType::Int: c->setFromJson(nlohmann::json(v.toInt()), CVarSource::Config); break;
        case CVarType::Float: c->setFromJson(nlohmann::json(v.toDouble()), CVarSource::Config); break;
        case CVarType::String: c->setFromJson(nlohmann::json(v.toString().toStdString()), CVarSource::Config); break;
        }
    };
    return b;
}

namespace {
QVariant fromJson(const nlohmann::json& j) {
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number_integer()) return qlonglong(j.get<long long>());
    if (j.is_number()) return j.get<double>();
    if (j.is_string()) return QString::fromStdString(j.get<std::string>());
    if (j.is_null()) return {};
    return QString::fromStdString(j.dump());
}
nlohmann::json toJson(const QVariant& v) {
    switch (v.typeId()) {
    case QMetaType::Bool: return v.toBool();
    case QMetaType::Int:
    case QMetaType::LongLong: return v.toLongLong();
    case QMetaType::Double:
    case QMetaType::Float: return v.toDouble();
    default: return v.toString().toStdString();
    }
}
} // namespace

SettingBinding projectBinding(Project* project, const QString& path) {
    SettingBinding b;
    b.key = QStringLiteral("project:") + path;
    const std::string p = path.toStdString();
    b.get = [project, p]() -> QVariant { return project ? fromJson(project->setting(p)) : QVariant(); };
    b.set = [project, p](const QVariant& v) {
        if (project) project->setSetting(p, toJson(v));
    };
    return b;
}

// ---- undo command -------------------------------------------------------------------------------------------

namespace {
class SettingCommand final : public QUndoCommand {
public:
    SettingCommand(SettingsDialog* dlg, SettingBinding b, QVariant before, QVariant after, bool open)
        : QUndoCommand(QObject::tr("Change %1").arg(b.key)), m_dlg(dlg), m_b(std::move(b)), m_before(std::move(before)),
          m_after(std::move(after)), m_open(open) {}
    void redo() override {
        m_b.set(m_after);
        m_dlg->refreshAll();
        Q_EMIT m_dlg->settingChanged(m_b.key);
    }
    void undo() override {
        m_b.set(m_before);
        m_dlg->refreshAll();
        Q_EMIT m_dlg->settingChanged(m_b.key);
    }
    int id() const override { return 0x5E77; }
    bool mergeWith(const QUndoCommand* other) override {
        auto* o = static_cast<const SettingCommand*>(other);
        if (!m_open || o->m_b.key != m_b.key) return false;
        m_after = o->m_after;
        m_open = o->m_open;
        return true;
    }

private:
    SettingsDialog* m_dlg;
    SettingBinding m_b;
    QVariant m_before, m_after;
    bool m_open;
};
} // namespace

// ---- page ---------------------------------------------------------------------------------------------------

SettingsPage::SettingsPage(QString id, QString title, QString icon, QString description, SettingsDialog* dialog)
    : QWidget(dialog), m_id(std::move(id)), m_title(std::move(title)), m_icon(std::move(icon)), m_description(std::move(description)),
      m_dialog(dialog) {
    setObjectName(QStringLiteral("page:") + m_id);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(24, 8, 24, 24);
    m_layout->setSpacing(0);
    m_layout->addStretch(1);
}

void SettingsPage::addSection(const QString& title, const QString& description) {
    auto* w = new QWidget(this);
    auto* l = new QVBoxLayout(w);
    l->setContentsMargins(0, m_sections.empty() ? 8 : 24, 0, 8);
    l->setSpacing(4);
    auto* t = new QLabel(title, w);
    t->setProperty("role", "title");
    l->addWidget(t);
    if (!description.isEmpty()) {
        auto* d = new QLabel(description, w);
        d->setProperty("role", "faint");
        d->setWordWrap(true);
        l->addWidget(d);
    }
    l->addWidget(makeHairline(w));
    m_layout->insertWidget(m_layout->count() - 1, w);
    m_sections.push_back(w);
    m_currentSection = w;
}

QWidget* SettingsPage::addRow(const QString& title, const QString& description, QWidget* control, const QString& key,
                              const QStringList& keywords) {
    auto* row = new QWidget(this);
    row->setObjectName(QStringLiteral("row:") + (key.isEmpty() ? title : key));
    auto* l = new QHBoxLayout(row);
    l->setContentsMargins(0, 8, 0, 8);
    l->setSpacing(24);
    auto* left = new QVBoxLayout();
    left->setSpacing(2);
    auto* t = new QLabel(title, row);
    QFont f = t->font();
    f.setWeight(QFont::Medium);
    t->setFont(f);
    left->addWidget(t);
    if (!description.isEmpty()) {
        auto* d = new QLabel(description, row);
        d->setProperty("role", "faint");
        d->setWordWrap(true);
        left->addWidget(d);
    }
    if (!key.isEmpty() && key.startsWith(QLatin1String("r.")) ) {
        auto* k = new QLabel(key, row);
        k->setProperty("role", "faint");
        k->setFont(Theme::monoFont());
        k->setTextInteractionFlags(Qt::TextSelectableByMouse);
        left->addWidget(k);
    }
    l->addLayout(left, 3);
    if (control) {
        control->setParent(row);
        if (!key.isEmpty() && control->objectName().isEmpty()) control->setObjectName(QStringLiteral("setting:") + key);
        auto* right = new QHBoxLayout();
        right->addStretch(1);
        right->addWidget(control);
        l->addLayout(right, 2);
    }
    m_layout->insertWidget(m_layout->count() - 1, row);
    m_rows.push_back({row, (title + QLatin1Char(' ') + description + QLatin1Char(' ') + key + QLatin1Char(' ') + keywords.join(QLatin1Char(' '))), m_currentSection});
    return row;
}

void SettingsPage::addFullWidth(QWidget* w, const QStringList& keywords) {
    w->setParent(this);
    m_layout->insertWidget(m_layout->count() - 1, w);
    m_rows.push_back({w, keywords.join(QLatin1Char(' ')), m_currentSection});
}

QWidget* SettingsPage::addBanner(const QString& text, bool warning, const QString& icon) {
    auto* f = new QFrame(this);
    f->setProperty("role", warning ? "warningBanner" : "banner");
    auto* l = new QHBoxLayout(f);
    l->setContentsMargins(10, 8, 10, 8);
    l->setSpacing(8);
    auto* ic = new QLabel(f);
    ic->setPixmap(Icons::get(icon.isEmpty() ? (warning ? QStringLiteral("warning") : QStringLiteral("info")) : icon,
                             warning ? Icons::Tint::Warning : Icons::Tint::Accent)
                      .pixmap(QSize(16, 16)));
    ic->setAlignment(Qt::AlignTop);
    auto* t = new QLabel(text, f);
    t->setWordWrap(true);
    t->setTextFormat(Qt::RichText);
    l->addWidget(ic);
    l->addWidget(t, 1);
    auto* wrap = new QWidget(this);
    auto* wl = new QVBoxLayout(wrap);
    wl->setContentsMargins(0, 6, 0, 6);
    wl->addWidget(f);
    m_layout->insertWidget(m_layout->count() - 1, wrap);
    m_rows.push_back({wrap, text, m_currentSection});
    return wrap;
}

ToggleSwitch* SettingsPage::addToggle(const QString& title, const QString& description, const SettingBinding& b) {
    auto* t = new ToggleSwitch(this);
    addRow(title, description, t, b.key);
    connect(t, &ToggleSwitch::toggled, this, [this, b](bool on) {
        if (b.get().toBool() != on) m_dialog->commit(b, on);
    });
    m_refreshers.push_back([t, b] {
        QSignalBlocker blk(t);
        t->setChecked(b.get().toBool());
    });
    return t;
}

QComboBox* SettingsPage::addCombo(const QString& title, const QString& description, const QStringList& labels,
                                  const QVariantList& values, const SettingBinding& b) {
    auto* c = new QComboBox(this);
    c->setMinimumWidth(220);
    for (int i = 0; i < labels.size(); ++i) c->addItem(labels[i], i < values.size() ? values[i] : QVariant(i));
    addRow(title, description, c, b.key);
    connect(c, &QComboBox::activated, this, [this, c, b](int i) { m_dialog->commit(b, c->itemData(i)); });
    m_refreshers.push_back([c, b] {
        QSignalBlocker blk(c);
        const QVariant v = b.get();
        int idx = -1;
        for (int i = 0; i < c->count(); ++i) {
            const QVariant d = c->itemData(i);
            if (d == v || d.toString() == v.toString()) {
                idx = i;
                break;
            }
        }
        c->setCurrentIndex(idx);
    });
    return c;
}

NumberField* SettingsPage::addNumber(const QString& title, const QString& description, const SettingBinding& b, double min,
                                     double max, double step, int decimals, const QString& suffix) {
    auto* n = new NumberField(this);
    n->setRange(min, max);
    n->setStep(step);
    n->setInteger(decimals == 0);
    if (decimals > 0) n->setDecimals(decimals);
    n->setSuffix(suffix);
    n->setFixedWidth(160);
    addRow(title, description, n, b.key);
    connect(n, &NumberField::edited, this, [this, b, decimals](double v, EditPhase phase) {
        m_dialog->commit(b, decimals == 0 ? QVariant(int(std::llround(v))) : QVariant(v), phase);
    });
    m_refreshers.push_back([n, b] { n->setValue(b.get().toDouble()); });
    return n;
}

QLineEdit* SettingsPage::addText(const QString& title, const QString& description, const SettingBinding& b) {
    auto* e = new QLineEdit(this);
    e->setMinimumWidth(260);
    addRow(title, description, e, b.key);
    connect(e, &QLineEdit::editingFinished, this, [this, e, b] {
        if (e->text() != b.get().toString()) m_dialog->commit(b, e->text());
    });
    m_refreshers.push_back([e, b] {
        if (!e->hasFocus()) e->setText(b.get().toString());
    });
    return e;
}

ColorButton* SettingsPage::addColor(const QString& title, const QString& description, const SettingBinding& b) {
    auto* c = new ColorButton(this);
    c->setAlphaEnabled(true);
    c->setFixedWidth(160);
    addRow(title, description, c, b.key);
    connect(c, &ColorButton::colorEdited, this, [this, b](const QColor& col, EditPhase phase) {
        m_dialog->commit(b, col.name(QColor::HexArgb), phase);
    });
    m_refreshers.push_back([c, b] { c->setColor(QColor(b.get().toString())); });
    return c;
}

QLineEdit* SettingsPage::addPath(const QString& title, const QString& description, const SettingBinding& b, bool directory,
                                 const QString& filter) {
    auto* host = new QWidget(this);
    auto* l = new QHBoxLayout(host);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    auto* e = new QLineEdit(host);
    e->setMinimumWidth(240);
    auto* browse = makeToolButton(QStringLiteral("folder-open"), tr("Browse…"), host);
    l->addWidget(e, 1);
    l->addWidget(browse);
    host->setObjectName(QStringLiteral("setting:") + b.key);
    addRow(title, description, host, b.key);
    connect(e, &QLineEdit::editingFinished, this, [this, e, b] {
        if (e->text() != b.get().toString()) m_dialog->commit(b, e->text());
    });
    connect(browse, &QToolButton::clicked, this, [this, e, b, directory, filter] {
        const QString p = directory ? QFileDialog::getExistingDirectory(this, tr("Choose Folder"), e->text())
                                    : QFileDialog::getOpenFileName(this, tr("Choose File"), e->text(), filter);
        if (!p.isEmpty()) m_dialog->commit(b, p);
    });
    m_refreshers.push_back([e, b] {
        if (!e->hasFocus()) e->setText(b.get().toString());
    });
    return e;
}

int SettingsPage::filter(const QString& text) {
    int matches = 0;
    std::vector<QWidget*> visibleSections;
    for (auto& r : m_rows) {
        const bool show = text.isEmpty() || r.text.contains(text, Qt::CaseInsensitive);
        r.widget->setVisible(show);
        if (show) {
            ++matches;
            if (r.section) visibleSections.push_back(r.section);
        }
    }
    for (QWidget* s : m_sections) {
        s->setVisible(text.isEmpty() || std::find(visibleSections.begin(), visibleSections.end(), s) != visibleSections.end());
    }
    return matches;
}

void SettingsPage::refresh() {
    for (auto& fn : m_refreshers) fn();
}

// ---- dialog -------------------------------------------------------------------------------------------------

SettingsDialog::SettingsDialog(const QString& title, const QString& subtitle, QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("OxDialog"));
    setWindowTitle(title);
    resize(1120, 760);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* body = new QHBoxLayout();
    body->setSpacing(0);

    // sidebar
    auto* side = new QFrame(this);
    side->setProperty("role", "sidebar");
    side->setFixedWidth(250);
    auto* sl = new QVBoxLayout(side);
    sl->setContentsMargins(12, 16, 12, 12);
    sl->setSpacing(10);
    auto* head = new QLabel(title, side);
    head->setProperty("role", "heading");
    QFont hf = head->font();
    hf.setPixelSize(Theme::instance().fontSize() + 6);
    head->setFont(hf);
    auto* sub = new QLabel(subtitle, side);
    sub->setProperty("role", "faint");
    sub->setWordWrap(true);
    m_search = new SearchField(tr("Search settings…"), side);
    m_search->setObjectName(QStringLiteral("SettingsSearch"));
    m_sidebar = new QListWidget(side);
    m_sidebar->setObjectName(QStringLiteral("SettingsSidebar"));
    m_sidebar->setIconSize(QSize(16, 16));
    sl->addWidget(head);
    sl->addWidget(sub);
    sl->addWidget(m_search);
    sl->addWidget(m_sidebar, 1);
    body->addWidget(side);

    // content
    auto* content = new QWidget(this);
    auto* cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    auto* pageHead = new QWidget(content);
    auto* phl = new QHBoxLayout(pageHead);
    phl->setContentsMargins(24, 20, 24, 4);
    phl->setSpacing(12);
    m_pageIcon = new QLabel(pageHead);
    m_pageIcon->setFixedSize(36, 36);
    m_pageIcon->setAlignment(Qt::AlignCenter);
    m_pageIcon->setStyleSheet(QStringLiteral("QLabel{background:%1;border-radius:9px;}").arg(cssColor(colors().accentSubtle)));
    auto* titles = new QVBoxLayout();
    titles->setSpacing(2);
    m_pageTitle = new QLabel(pageHead);
    m_pageTitle->setProperty("role", "heading");
    m_pageDescription = new QLabel(pageHead);
    m_pageDescription->setProperty("role", "faint");
    m_pageDescription->setWordWrap(true);
    titles->addWidget(m_pageTitle);
    titles->addWidget(m_pageDescription);
    phl->addWidget(m_pageIcon, 0, Qt::AlignTop);
    phl->addLayout(titles, 1);
    cl->addWidget(pageHead);
    auto* scroll = new QScrollArea(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_stack = new QStackedWidget(scroll);
    scroll->setWidget(m_stack);
    cl->addWidget(scroll, 1);
    body->addWidget(content, 1);
    root->addLayout(body, 1);

    // footer
    auto* footer = new QFrame(this);
    footer->setProperty("role", "footer");
    auto* fl = new QHBoxLayout(footer);
    fl->setContentsMargins(16, 10, 16, 10);
    fl->setSpacing(8);
    auto* undoBtn = makeToolButton(QStringLiteral("undo"), tr("Undo setting change (Ctrl+Z)"), footer);
    auto* redoBtn = makeToolButton(QStringLiteral("redo"), tr("Redo (Ctrl+Shift+Z)"), footer);
    undoBtn->setEnabled(false);
    redoBtn->setEnabled(false);
    connect(&m_undo, &QUndoStack::canUndoChanged, undoBtn, &QToolButton::setEnabled);
    connect(&m_undo, &QUndoStack::canRedoChanged, redoBtn, &QToolButton::setEnabled);
    connect(undoBtn, &QToolButton::clicked, &m_undo, &QUndoStack::undo);
    connect(redoBtn, &QToolButton::clicked, &m_undo, &QUndoStack::redo);
    new QShortcut(QKeySequence::Undo, this, [this] { m_undo.undo(); });
    new QShortcut(QKeySequence::Redo, this, [this] { m_undo.redo(); });
    auto* status = new QLabel(footer);
    status->setProperty("role", "faint");
    connect(&m_undo, &QUndoStack::indexChanged, this, [this, status] {
        status->setText(m_undo.count() ? tr("%n change(s) in this session", nullptr, m_undo.index()) : QString());
    });
    fl->addWidget(undoBtn);
    fl->addWidget(redoBtn);
    fl->addWidget(status);
    fl->addStretch(1);
    m_revert = new QPushButton(Icons::get(QStringLiteral("history")), tr("Revert"), footer);
    m_apply = new QPushButton(tr("Apply"), footer);
    m_apply->setProperty("role", "primary");
    auto* close = new QPushButton(tr("Close"), footer);
    for (QPushButton* b : {m_revert, m_apply, close}) b->setAutoDefault(false);
    m_revert->setEnabled(false);
    m_apply->setEnabled(false);
    fl->addWidget(m_revert);
    fl->addWidget(m_apply);
    fl->addWidget(close);
    root->addWidget(footer);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_apply, &QPushButton::clicked, this, [this] {
        applyChanges();
        setDirty(false);
    });
    connect(m_revert, &QPushButton::clicked, this, [this] {
        revertChanges();
        m_undo.clear();
        refreshAll();
        setDirty(false);
    });

    connect(m_sidebar, &QListWidget::currentRowChanged, this, [this](int row) {
        QListWidgetItem* it = m_sidebar->item(row);
        if (!it) return;
        if (SettingsPage* p = page(it->data(Qt::UserRole).toString())) {
            m_stack->setCurrentWidget(p);
            m_pageTitle->setText(p->title());
            m_pageDescription->setText(p->description());
            m_pageIcon->setPixmap(Icons::get(p->icon(), Icons::Tint::Accent).pixmap(QSize(20, 20)));
        }
    });
    connect(m_search, &QLineEdit::textChanged, this, &SettingsDialog::setSearchText);
}

void SettingsDialog::addGroup(const QString& name) {
    auto* it = new QListWidgetItem(name.toUpper(), m_sidebar);
    it->setFlags(Qt::NoItemFlags);
    QFont f = it->font();
    f.setPixelSize(Theme::instance().fontSize() - 2);
    f.setWeight(QFont::Bold);
    it->setFont(f);
    it->setForeground(colors().textFaint);
    it->setSizeHint(QSize(10, 30));
}

void SettingsDialog::addPage(SettingsPage* page) {
    m_pages.push_back(page);
    m_stack->addWidget(page);
    auto* it = new QListWidgetItem(Icons::get(page->icon()), page->title(), m_sidebar);
    it->setData(Qt::UserRole, page->id());
    page->refresh();
    if (m_sidebar->currentRow() < 0 || !m_sidebar->currentItem() || !(m_sidebar->currentItem()->flags() & Qt::ItemIsSelectable)) {
        m_sidebar->setCurrentItem(it);
    }
}

SettingsPage* SettingsDialog::page(const QString& id) const {
    for (auto* p : m_pages) {
        if (p->id() == id) return p;
    }
    return nullptr;
}

void SettingsDialog::showPage(const QString& id) {
    for (int i = 0; i < m_sidebar->count(); ++i) {
        if (m_sidebar->item(i)->data(Qt::UserRole).toString() == id) {
            m_sidebar->setCurrentRow(i);
            return;
        }
    }
}

void SettingsDialog::setSearchText(const QString& text) {
    if (m_search->text() != text) m_search->setText(text);
    int firstMatch = -1;
    for (int i = 0; i < m_sidebar->count(); ++i) {
        QListWidgetItem* it = m_sidebar->item(i);
        const QString id = it->data(Qt::UserRole).toString();
        if (id.isEmpty()) {
            it->setHidden(!text.isEmpty());
            continue;
        }
        SettingsPage* p = page(id);
        const int n = p ? p->filter(text) : 0;
        const bool titleMatch = !text.isEmpty() && p && p->title().contains(text, Qt::CaseInsensitive);
        if (titleMatch && n == 0) p->filter(QString());
        it->setHidden(!text.isEmpty() && n == 0 && !titleMatch);
        if (!it->isHidden() && firstMatch < 0) firstMatch = i;
    }
    QListWidgetItem* cur = m_sidebar->currentItem();
    if ((!cur || cur->isHidden()) && firstMatch >= 0) m_sidebar->setCurrentRow(firstMatch);
}

void SettingsDialog::commit(const SettingBinding& b, const QVariant& value, EditPhase phase) {
    const bool open = phase == EditPhase::Begin || phase == EditPhase::Update;
    m_undo.push(new SettingCommand(this, b, b.get(), value, open));
    setDirty(true);
}

void SettingsDialog::refreshAll() {
    for (auto* p : m_pages) p->refresh();
}

void SettingsDialog::setDirty(bool d) {
    m_dirty = d;
    m_apply->setEnabled(d);
    m_revert->setEnabled(d);
    setWindowModified(d);
}

void SettingsDialog::closeEvent(QCloseEvent* e) {
    if (m_dirty) {
        const auto r = QMessageBox::question(this, windowTitle(), tr("Apply the changed settings?"),
                                             QMessageBox::Apply | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Apply);
        if (r == QMessageBox::Cancel) {
            e->ignore();
            return;
        }
        if (r == QMessageBox::Apply) applyChanges();
        else revertChanges();
        setDirty(false);
    }
    QDialog::closeEvent(e);
}

void SettingsDialog::reject() { close(); }

} // namespace ox::editor
