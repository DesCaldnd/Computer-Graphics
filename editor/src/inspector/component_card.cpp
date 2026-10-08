#include "inspector/component_card.hpp"

#include "core/editor_context.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"

#include <oxwald/core/serial/convert.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/component_registry.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>

namespace ox::editor {

namespace {

std::string shapeOf(const serial::Value& v) {
    std::string s;
    switch (v.tag()) {
    case serial::Tag::Array:
        s += "a" + std::to_string(v.size()) + "(";
        if (!v.isPacked()) {
            for (const auto& it : v.items()) s += shapeOf(it);
        }
        s += ")";
        break;
    case serial::Tag::Map:
        s += "m(";
        for (const auto& [k, it] : v.fields()) s += k + ":" + shapeOf(it) + ",";
        s += ")";
        break;
    case serial::Tag::Optional:
        s += v.hasValue() ? "o1(" + shapeOf(v.optionalValue()) + ")" : "o0";
        break;
    case serial::Tag::Object:
        s += "{";
        for (const auto& [k, it] : v.fields()) s += shapeOf(it);
        s += "}";
        break;
    default: break;
    }
    return s;
}

QWidget* hboxWidget(QWidget* parent, std::initializer_list<QWidget*> items, int stretchIndex = 0) {
    auto* w = new QWidget(parent);
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    int i = 0;
    for (QWidget* it : items) l->addWidget(it, i++ == stretchIndex ? 1 : 0);
    return w;
}

} // namespace

ComponentCard::ComponentCard(EditorContext* ctx, const ComponentInfo* info, UuidList entities, QWidget* parent)
    : CollapsibleSection(prettifyName(info->name), Icons::forComponent(qs(info->icon), qs(info->name)), parent), m_ctx(ctx),
      m_info(info), m_entities(std::move(entities)) {
    setObjectName(QStringLiteral("card:%1").arg(qs(info->name)));
    if (!info->category.empty()) setSubtitle(qs(info->category));
    auto* more = makeToolButton(QStringLiteral("more"), tr("Component options"), header());
    more->setIconSize(QSize(14, 14));
    addHeaderWidget(more);
    auto showMenu = [this](const QPoint& pos) {
        QMenu menu(this);
        menu.addAction(Icons::get(QStringLiteral("refresh")), tr("Reset to Defaults"), this,
                       [this] { m_ctx->resetComponent(m_entities, m_info->name); });
        menu.addAction(Icons::get(QStringLiteral("copy")), tr("Copy Values"), this, [this] {
            World& w = m_ctx->world();
            if (Entity e = w.find(m_entities.front())) {
                serial::Document doc{"component", 1, m_info->serialize(w, e.handle())};
                QApplication::clipboard()->setText(qs(m_info->name) + QLatin1Char('\n') + qs(serial::toJsonString(doc)));
            }
        });
        menu.addAction(Icons::get(QStringLiteral("paste")), tr("Paste Values"), this, [this] {
            const QString text = QApplication::clipboard()->text();
            const int nl = text.indexOf(QLatin1Char('\n'));
            if (nl < 0 || text.left(nl) != qs(m_info->name)) return;
            if (auto doc = serial::parseJsonString(text.mid(nl + 1).toStdString())) m_ctx->setProperty(m_entities, m_info->name, "", doc->root);
        });
        menu.addSeparator();
        auto* rm = menu.addAction(Icons::get(QStringLiteral("trash")), tr("Remove Component"), this,
                                  [this] { m_ctx->removeComponent(m_entities, m_info->name); });
        rm->setEnabled(m_info->removable);
        menu.exec(pos);
    };
    connect(more, &QToolButton::clicked, this, [more, showMenu] { showMenu(more->mapToGlobal(QPoint(0, more->height()))); });
    connect(this, &CollapsibleSection::headerContextMenu, this, showMenu);
    build();
}

std::vector<serial::Value> ComponentCard::readValues(const std::string& path) const {
    std::vector<serial::Value> out;
    World& w = m_ctx->world();
    for (const auto& id : m_entities) {
        auto v = props::read(w, id, m_info->name, path);
        out.push_back(v ? *v : serial::Value{});
    }
    return out;
}

std::string ComponentCard::shapeSignature() const {
    std::string s;
    World& w = m_ctx->world();
    for (const auto& id : m_entities) {
        Entity e = w.find(id);
        if (e && m_info->has(w, e.handle())) s += shapeOf(m_info->serialize(w, e.handle())) + "|";
        else s += "missing|";
    }
    return s;
}

void ComponentCard::build() {
    m_rows.clear();
    m_rowIndex = 0;
    auto* grid = new QWidget(this);
    m_layout = new QGridLayout(grid);
    m_layout->setContentsMargins(12, 8, 10, 10);
    m_layout->setHorizontalSpacing(10);
    m_layout->setVerticalSpacing(5);
    m_layout->setColumnStretch(0, 0);
    m_layout->setColumnStretch(1, 1);
    m_layout->setColumnMinimumWidth(0, 112);
    m_grid = grid;
    m_shape = shapeSignature();
    if (m_info->type) addStruct(*m_info->type, "", 0);
    if (m_rows.empty()) {
        auto* l = new QLabel(tr("No editable properties"), grid);
        l->setProperty("role", "faint");
        addSpanningRow(l);
    }
    setBody(grid);
    for (auto& r : m_rows) {
        if (r.editor) r.editor->setValues(readValues(r.path));
    }
    updateOverrideMarkers();
    if (!m_filter.isEmpty()) setFilter(m_filter);
}

QLabel* ComponentCard::makeLabel(const QString& text, const QString& tooltip, int depth, const std::string& path) {
    auto* l = new QLabel(text, m_grid);
    l->setContentsMargins(depth * 12, 0, 0, 0);
    l->setProperty("role", "dim");
    l->setMaximumWidth(170);
    QString tip = tooltip.isEmpty() ? QString() : tooltip + QStringLiteral("\n\n");
    tip += qs(m_info->name) + (path.empty() ? QString() : QLatin1Char('.') + qs(path));
    l->setToolTip(tip);
    l->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(l, &QLabel::customContextMenuRequested, this, [this, l, path](const QPoint& p) { showLabelMenu(path, l->mapToGlobal(p)); });
    return l;
}

void ComponentCard::addRow(QLabel* label, QWidget* field, PropertyEditor* editor, const std::string& path, const QString& search) {
    if (label) m_layout->addWidget(label, m_rowIndex, 0, Qt::AlignLeft | Qt::AlignVCenter);
    if (field) m_layout->addWidget(field, m_rowIndex, 1);
    Row r;
    r.path = path;
    r.overridePath = props::overridePath(m_info->name, path);
    r.label = label;
    r.field = field;
    r.editor = editor;
    r.searchText = search;
    m_rows.push_back(r);
    ++m_rowIndex;
}

void ComponentCard::addSpanningRow(QWidget* w) { m_layout->addWidget(w, m_rowIndex++, 0, 1, 2); }

void ComponentCard::addStruct(const reflect::TypeInfo& type, const std::string& base, int depth) {
    std::vector<const reflect::FieldInfo*> plain;
    std::vector<std::pair<std::string, std::vector<const reflect::FieldInfo*>>> categories;
    for (const auto& f : type.fields) {
        if (f.attributes.hidden) continue;
        if (depth == 0 && !f.attributes.category.empty()) {
            auto it = std::find_if(categories.begin(), categories.end(), [&](const auto& c) { return c.first == f.attributes.category; });
            if (it == categories.end()) categories.push_back({f.attributes.category, {&f}});
            else it->second.push_back(&f);
        } else {
            plain.push_back(&f);
        }
    }
    auto emitField = [&](const reflect::FieldInfo* f) {
        const std::string path = base.empty() ? f->name : base + "." + f->name;
        const QString label = f->attributes.displayName.empty() ? prettifyName(f->name) : qs(f->attributes.displayName);
        addValue(f->name, label, *f->type, f->attributes, path, depth);
    };
    for (const auto* f : plain) emitField(f);
    for (const auto& [cat, fields] : categories) {
        auto* header = new QWidget(m_grid);
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(0, 8, 0, 2);
        hl->setSpacing(8);
        hl->addWidget(makeSectionLabel(prettifyName(cat), header));
        hl->addWidget(makeHairline(header), 1);
        addSpanningRow(header);
        for (const auto* f : fields) emitField(f);
    }
}

void ComponentCard::addValue(const std::string& name, const QString& label, const reflect::TypeInfo& type,
                             const reflect::Attributes& attrs, const std::string& path, int depth) {
    (void)name;
    if (PropertyEditorFactory::isLeaf(type, attrs)) {
        PropertyContext pc;
        pc.editor = m_ctx;
        pc.entities = m_entities;
        pc.component = m_info->name;
        pc.path = path;
        pc.type = &type;
        pc.attributes = attrs;
        PropertyEditor* ed = PropertyEditorFactory::create(pc, m_grid);
        if (!ed) return;
        ed->setCommit([this, path](const std::string& sub, const std::vector<serial::Value>& values, EditPhase phase) {
            m_ctx->setProperty(m_entities, m_info->name, sub.empty() ? path : path + "." + sub, values, phase);
        });
        ed->setToolTip(qs(attrs.tooltip));
        addRow(makeLabel(label, qs(attrs.tooltip), depth, path), ed, ed, path, label + QLatin1Char(' ') + qs(path));
        return;
    }
    switch (type.kind) {
    case reflect::Kind::Struct: {
        auto* l = makeLabel(label, qs(attrs.tooltip), depth, path);
        QFont f = l->font();
        f.setWeight(QFont::DemiBold);
        l->setFont(f);
        l->setProperty("role", QVariant());
        addRow(l, nullptr, nullptr, path, label);
        addStruct(type, path, depth + 1);
        break;
    }
    case reflect::Kind::Array:
    case reflect::Kind::Map:
    case reflect::Kind::Optional: addContainer(label, type, attrs, path, depth); break;
    default: {
        auto* l = new QLabel(tr("(unsupported type %1)").arg(qs(type.name)), m_grid);
        l->setProperty("role", "faint");
        addRow(makeLabel(label, {}, depth, path), l, nullptr, path, label);
        break;
    }
    }
}

void ComponentCard::mutateContainer(const std::string& path, const reflect::TypeInfo& type,
                                    const std::function<void(void*)>& fn, const QString& what) {
    (void)what;
    std::vector<serial::Value> out;
    for (const auto& v : readValues(path)) {
        void* tmp = type.create();
        serial::fromValue(v, tmp, type);
        fn(tmp);
        out.push_back(serial::toValue(tmp, type));
        type.destroy(tmp);
    }
    m_ctx->setProperty(m_entities, m_info->name, path, out, EditPhase::Single);
}

void ComponentCard::addContainer(const QString& label, const reflect::TypeInfo& type, const reflect::Attributes& attrs,
                                 const std::string& path, int depth) {
    const auto values = readValues(path);
    const reflect::TypeInfo* elem = type.element;
    if (!elem) return;
    reflect::Attributes elemAttrs = attrs;
    elemAttrs.displayName.clear();

    if (type.kind == reflect::Kind::Optional) {
        auto* box = new QCheckBox(tr("Set"), m_grid);
        bool all = true, any = false;
        for (const auto& v : values) {
            all &= v.hasValue();
            any |= v.hasValue();
        }
        box->setTristate(any && !all);
        box->setCheckState(all ? Qt::Checked : any ? Qt::PartiallyChecked : Qt::Unchecked);
        box->setEnabled(!attrs.readOnly);
        connect(box, &QCheckBox::clicked, this, [this, path, &type](bool on) {
            mutateContainer(path, type, [&type, on](void* c) {
                if (on) {
                    if (!type.optionalHas(c)) type.optionalEmplace(c);
                } else {
                    type.containerClear(c);
                }
            }, tr("Optional"));
        });
        addRow(makeLabel(label, qs(attrs.tooltip), depth, path), hboxWidget(m_grid, {box}, 1), nullptr, path, label);
        if (all) addValue("value", tr("Value"), *elem, elemAttrs, path + ".value", depth + 1);
        return;
    }

    bool sameSize = true;
    for (const auto& v : values) sameSize &= v.size() == values.front().size();
    const usize n = values.empty() ? 0 : values.front().size();

    auto* count = new QLabel(m_grid);
    count->setProperty("role", "faint");
    auto* add = makeToolButton(QStringLiteral("add"), tr("Add element"), m_grid);
    auto* clear = makeToolButton(QStringLiteral("trash"), tr("Remove all elements"), m_grid);
    add->setEnabled(!attrs.readOnly && sameSize);
    clear->setEnabled(!attrs.readOnly && n > 0);
    addRow(makeLabel(label, qs(attrs.tooltip), depth, path), hboxWidget(m_grid, {count, add, clear}, 0), nullptr, path, label);
    connect(clear, &QToolButton::clicked, this, [this, path, &type] {
        mutateContainer(path, type, [&type](void* c) { type.containerClear(c); }, tr("Clear"));
    });

    if (!sameSize) {
        count->setText(tr("Different sizes in selection"));
        return;
    }
    if (type.kind == reflect::Kind::Array) {
        count->setText(tr("%n element(s)", nullptr, int(n)));
        connect(add, &QToolButton::clicked, this, [this, path, &type] {
            mutateContainer(path, type, [&type](void* c) { type.arrayResize(c, type.containerSize(c) + 1); }, tr("Add"));
        });
        for (usize i = 0; i < n; ++i) {
            const std::string ep = path + "[" + std::to_string(i) + "]";
            auto* rm = makeToolButton(QStringLiteral("remove"), tr("Remove element"), m_grid);
            rm->setEnabled(!attrs.readOnly);
            connect(rm, &QToolButton::clicked, this, [this, path, &type, i] {
                mutateContainer(path, type, [&type, i](void* c) {
                    const usize size = type.containerSize(c);
                    const reflect::TypeInfo* e = type.element;
                    for (usize k = i; k + 1 < size; ++k) e->copyAssign(type.arrayAt(c, k), type.arrayAt(c, k + 1));
                    type.arrayResize(c, size - 1);
                }, tr("Remove"));
            });
            const QString elemLabel = QStringLiteral("[%1]").arg(i);
            if (PropertyEditorFactory::isLeaf(*elem, elemAttrs)) {
                PropertyContext pc{m_ctx, m_entities, m_info->name, ep, elem, elemAttrs};
                PropertyEditor* ed = PropertyEditorFactory::create(pc, m_grid);
                if (!ed) continue;
                ed->setCommit([this, ep](const std::string& sub, const std::vector<serial::Value>& v, EditPhase phase) {
                    m_ctx->setProperty(m_entities, m_info->name, sub.empty() ? ep : ep + "." + sub, v, phase);
                });
                addRow(makeLabel(elemLabel, {}, depth + 1, ep), hboxWidget(m_grid, {ed, rm}, 0), ed, ep, label + elemLabel);
            } else {
                auto* l = makeLabel(elemLabel, {}, depth + 1, ep);
                addRow(l, hboxWidget(m_grid, {new QWidget(m_grid), rm}, 0), nullptr, ep, label);
                addStruct(*elem, ep, depth + 2);
            }
        }
    } else { // map with string keys
        count->setText(tr("%n entries", nullptr, int(n)));
        connect(add, &QToolButton::clicked, this, [this, path, &type] {
            bool ok = false;
            const QString key = QInputDialog::getText(this, tr("Add Entry"), tr("Key:"), QLineEdit::Normal, {}, &ok);
            if (!ok || key.isEmpty()) return;
            const std::string k = key.toStdString();
            mutateContainer(path, type, [&type, k](void* c) { type.mapInsert(c, k); }, tr("Add"));
        });
        if (values.empty()) return;
        for (const auto& [key, v] : values.front().fields()) {
            (void)v;
            const std::string ep = path + "." + key;
            auto* rm = makeToolButton(QStringLiteral("remove"), tr("Remove entry"), m_grid);
            const std::string k = key;
            connect(rm, &QToolButton::clicked, this, [this, path, &type, k] {
                mutateContainer(path, type, [&type, k](void* c) { type.mapErase(c, k); }, tr("Remove"));
            });
            if (PropertyEditorFactory::isLeaf(*elem, elemAttrs)) {
                PropertyContext pc{m_ctx, m_entities, m_info->name, ep, elem, elemAttrs};
                PropertyEditor* ed = PropertyEditorFactory::create(pc, m_grid);
                if (!ed) continue;
                ed->setCommit([this, ep](const std::string& sub, const std::vector<serial::Value>& vals, EditPhase phase) {
                    m_ctx->setProperty(m_entities, m_info->name, sub.empty() ? ep : ep + "." + sub, vals, phase);
                });
                addRow(makeLabel(qs(key), {}, depth + 1, ep), hboxWidget(m_grid, {ed, rm}, 0), ed, ep, qs(key));
            } else {
                addRow(makeLabel(qs(key), {}, depth + 1, ep), hboxWidget(m_grid, {new QWidget(m_grid), rm}, 0), nullptr, ep, qs(key));
                addStruct(*elem, ep, depth + 2);
            }
        }
    }
}

void ComponentCard::refresh() {
    if (shapeSignature() != m_shape) {
        build();
        return;
    }
    for (auto& r : m_rows) {
        if (r.editor && !r.editor->isEditing()) r.editor->setValues(readValues(r.path));
    }
    updateOverrideMarkers();
}

void ComponentCard::updateOverrideMarkers() {
    const ThemePalette& c = colors();
    for (auto& r : m_rows) {
        if (!r.label) continue;
        bool overridden = false;
        for (const auto& id : m_entities) overridden |= m_ctx->isOverridden(id, r.overridePath);
        const QString css = overridden ? QStringLiteral("QLabel{color:%1;font-weight:600;border-left:2px solid %2;padding-left:4px;}")
                                             .arg(cssColor(c.accentText), cssColor(c.accent))
                                       : QString();
        if (r.label->styleSheet() != css) r.label->setStyleSheet(css);
        r.label->setProperty("overridden", overridden);
    }
}

void ComponentCard::showLabelMenu(const std::string& path, const QPoint& globalPos) {
    QMenu menu(this);
    bool overridden = false;
    const std::string op = props::overridePath(m_info->name, path);
    for (const auto& id : m_entities) overridden |= m_ctx->isOverridden(id, op);
    auto* revert = menu.addAction(Icons::get(QStringLiteral("prefab")), tr("Revert to Prefab"), this, [this, op] {
        for (const auto& id : m_entities) {
            if (m_ctx->isOverridden(id, op)) m_ctx->revertPrefabOverride(id, op);
        }
    });
    revert->setEnabled(overridden);
    menu.addAction(Icons::get(QStringLiteral("refresh")), tr("Reset to Default"), this, [this, path] {
        void* tmp = m_info->type->create();
        reflect::ValueRef ref = reflect::resolvePath(reflect::ValueRef{tmp, m_info->type}, path);
        if (ref) m_ctx->setProperty(m_entities, m_info->name, path, ref.get());
        m_info->type->destroy(tmp);
    });
    menu.addAction(Icons::get(QStringLiteral("copy")), tr("Copy Property Path"), this, [this, path] {
        QApplication::clipboard()->setText(qs(m_info->name) + QLatin1Char('.') + qs(path));
    });
    menu.exec(globalPos);
}

void ComponentCard::setFilter(const QString& text) {
    m_filter = text;
    for (auto& r : m_rows) {
        const bool show = text.isEmpty() || r.searchText.contains(text, Qt::CaseInsensitive) || !r.editor;
        if (r.label) r.label->setVisible(show);
        if (r.field) r.field->setVisible(show);
    }
}

PropertyEditor* ComponentCard::editorForPath(const std::string& path) const {
    for (const auto& r : m_rows) {
        if (r.editor && r.path == path) return r.editor;
    }
    return nullptr;
}

} // namespace ox::editor
