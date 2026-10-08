#include "inspector/reflected_object_editor.hpp"

#include "core/editor_context.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/serial/convert.hpp>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>

namespace ox::editor {

ReflectedObjectEditor::ReflectedObjectEditor(EditorContext* ctx, const reflect::TypeInfo& type, QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_type(type), m_object(type.create()) {
    setObjectName(QStringLiteral("ReflectedObjectEditor:%1").arg(qs(type.name)));
    m_grid = new QGridLayout(this);
    m_grid->setContentsMargins(12, 8, 10, 10);
    m_grid->setHorizontalSpacing(10);
    m_grid->setVerticalSpacing(5);
    m_grid->setColumnMinimumWidth(0, 120);
    m_grid->setColumnStretch(1, 1);
    build();
}

ReflectedObjectEditor::~ReflectedObjectEditor() {
    if (m_object) m_type.destroy(m_object);
}

void ReflectedObjectEditor::setValue(const serial::Value& value) {
    serial::fromValue(value, m_object, m_type);
    refreshEditors();
}

serial::Value ReflectedObjectEditor::value() const { return serial::toValue(m_object, m_type); }

bool ReflectedObjectEditor::setField(const std::string& path, const serial::Value& value) {
    reflect::ValueRef ref = reflect::resolvePath(reflect::ValueRef{m_object, &m_type}, path);
    if (!ref || !ref.set(value)) return false;
    refreshEditors();
    Q_EMIT edited();
    return true;
}

PropertyEditor* ReflectedObjectEditor::editorForPath(const std::string& path) const {
    for (const auto& r : m_rows) {
        if (r.editor && r.path == path) return r.editor;
    }
    return nullptr;
}

void ReflectedObjectEditor::build() {
    addStruct(m_type, "", 0);
    refreshEditors();
}

void ReflectedObjectEditor::addStruct(const reflect::TypeInfo& type, const std::string& base, int depth) {
    std::vector<const reflect::FieldInfo*> plain;
    std::vector<std::pair<std::string, std::vector<const reflect::FieldInfo*>>> categories;
    for (const auto& f : type.fields) {
        if (f.attributes.hidden || f.attributes.noSerialize) continue;
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
        addField(f->attributes.displayName.empty() ? prettifyName(f->name) : qs(f->attributes.displayName), *f->type, f->attributes, path, depth);
    };
    for (const auto* f : plain) emitField(f);
    for (const auto& [cat, fields] : categories) {
        auto* header = new QWidget(this);
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(0, 8, 0, 2);
        hl->setSpacing(8);
        hl->addWidget(makeSectionLabel(prettifyName(cat), header));
        hl->addWidget(makeHairline(header), 1);
        m_grid->addWidget(header, m_row++, 0, 1, 2);
        for (const auto* f : fields) emitField(f);
    }
}

void ReflectedObjectEditor::addField(const QString& label, const reflect::TypeInfo& type, const reflect::Attributes& attrs,
                                     const std::string& path, int depth) {
    auto* l = new QLabel(label, this);
    l->setContentsMargins(depth * 12, 0, 0, 0);
    l->setProperty("role", "dim");
    l->setToolTip(attrs.tooltip.empty() ? qs(path) : qs(attrs.tooltip) + QStringLiteral("\n\n") + qs(path));
    if (PropertyEditorFactory::isLeaf(type, attrs)) {
        PropertyContext pc;
        pc.editor = m_ctx;
        pc.component = m_type.name;
        pc.path = path;
        pc.type = &type;
        pc.attributes = attrs;
        PropertyEditor* ed = PropertyEditorFactory::create(pc, this);
        if (!ed) return;
        ed->setCommit([this, path](const std::string& sub, const std::vector<serial::Value>& values, EditPhase) {
            if (values.empty()) return;
            reflect::ValueRef ref = reflect::resolvePath(reflect::ValueRef{m_object, &m_type}, sub.empty() ? path : path + "." + sub);
            if (ref && ref.set(values.front())) {
                refreshEditors();
                Q_EMIT edited();
            }
        });
        ed->setToolTip(qs(attrs.tooltip));
        m_grid->addWidget(l, m_row, 0, Qt::AlignLeft | Qt::AlignVCenter);
        m_grid->addWidget(ed, m_row++, 1);
        Row r;
        r.path = path;
        r.editor = ed;
        m_rows.push_back(std::move(r));
        return;
    }
    if (type.kind == reflect::Kind::Struct) {
        QFont f = l->font();
        f.setWeight(QFont::DemiBold);
        l->setFont(f);
        m_grid->addWidget(l, m_row++, 0, 1, 2);
        addStruct(type, path, depth + 1);
        return;
    }
    if (type.kind == reflect::Kind::Array && type.element) {
        // Comma separated scalars (e.g. LOD ratios).
        auto* edit = new QLineEdit(this);
        edit->setObjectName(QStringLiteral("array:%1").arg(qs(path)));
        m_grid->addWidget(l, m_row, 0, Qt::AlignLeft | Qt::AlignVCenter);
        m_grid->addWidget(edit, m_row++, 1);
        const reflect::TypeInfo* elem = type.element;
        auto read = [this, path, edit] {
            if (edit->hasFocus()) return;
            reflect::ValueRef ref = reflect::resolvePath(reflect::ValueRef{m_object, &m_type}, path);
            QStringList parts;
            if (ref) {
                const serial::Value v = ref.get();
                for (usize i = 0; i < v.size(); ++i) {
                    const serial::Value item = v.at(i);
                    parts << (item.isString() ? qs(item.getString()) : QString::number(item.getDouble()));
                }
            }
            edit->setText(parts.join(QStringLiteral(", ")));
        };
        connect(edit, &QLineEdit::editingFinished, this, [this, path, edit, elem, &type] {
            reflect::ValueRef ref = reflect::resolvePath(reflect::ValueRef{m_object, &m_type}, path);
            if (!ref) return;
            const QStringList parts = edit->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
            type.arrayResize(ref.ptr, parts.size());
            for (int i = 0; i < parts.size(); ++i) {
                reflect::ValueRef e{type.arrayAt(ref.ptr, usize(i)), elem};
                const QString t = parts[i].trimmed();
                if (elem->kind == reflect::Kind::String) e.set(serial::Value::makeString(t.toStdString()));
                else e.set(serial::Value::makeF64(t.toDouble()));
            }
            refreshEditors();
            Q_EMIT edited();
        });
        Row r;
        r.path = path;
        r.refresh = read;
        m_rows.push_back(std::move(r));
        return;
    }
    auto* note = new QLabel(tr("(edit in the file)"), this);
    note->setProperty("role", "faint");
    m_grid->addWidget(l, m_row, 0);
    m_grid->addWidget(note, m_row++, 1);
}

void ReflectedObjectEditor::refreshEditors() {
    for (auto& r : m_rows) {
        if (r.refresh) {
            r.refresh();
            continue;
        }
        if (!r.editor || r.editor->isEditing()) continue;
        reflect::ValueRef ref = reflect::resolvePath(reflect::ValueRef{m_object, &m_type}, r.path);
        if (ref) r.editor->setValues({ref.get()});
    }
}

} // namespace ox::editor
