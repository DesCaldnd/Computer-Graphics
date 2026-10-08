#include "inspector/property_editors.hpp"

#include "core/editor_context.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/scene/components.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QWidgetAction>

#include <glm/gtc/quaternion.hpp>

namespace ox::editor {

glm::vec3 quatToEulerDegrees(const glm::quat& q) { return glm::degrees(glm::eulerAngles(glm::normalize(q))); }
glm::quat eulerDegreesToQuat(const glm::vec3& e) { return glm::normalize(glm::quat(glm::radians(e))); }

PropertyEditor::PropertyEditor(PropertyContext ctx, QWidget* parent) : QWidget(parent), m_ctx(std::move(ctx)) {
    setObjectName(QStringLiteral("prop:%1.%2").arg(qs(m_ctx.component), qs(m_ctx.path)));
}

bool PropertyEditor::allEqual(const std::vector<serial::Value>& values) {
    for (size_t i = 1; i < values.size(); ++i) {
        if (!(values[i] == values[0])) return false;
    }
    return true;
}

namespace {

QHBoxLayout* hbox(QWidget* w) {
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(4);
    return l;
}

// ---- bool ---------------------------------------------------------------------------------------------------
class BoolEditor final : public PropertyEditor {
public:
    BoolEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        auto* l = hbox(this);
        m_box = new QCheckBox(this);
        m_box->setEnabled(!c.attributes.readOnly);
        l->addWidget(m_box);
        l->addStretch(1);
        connect(m_box, &QCheckBox::clicked, this, [this](bool on) {
            m_box->setTristate(false);
            commit(serial::Value::makeBool(on), EditPhase::Single);
        });
    }
    void setValues(const std::vector<serial::Value>& v) override {
        QSignalBlocker b(m_box);
        if (!allEqual(v)) {
            m_box->setTristate(true);
            m_box->setCheckState(Qt::PartiallyChecked);
        } else {
            m_box->setTristate(false);
            m_box->setChecked(!v.empty() && v[0].getBool());
        }
    }
    QCheckBox* m_box;
};

// ---- numbers ------------------------------------------------------------------------------------------------
class NumberEditor final : public PropertyEditor {
public:
    NumberEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        auto* l = hbox(this);
        m_field = new NumberField(this);
        const bool integer = c.type->kind != reflect::Kind::Float;
        m_field->setInteger(integer);
        double mn = -1e12, mx = 1e12;
        if (c.type->kind == reflect::Kind::UInt) mn = 0;
        if (c.attributes.rangeMin) mn = *c.attributes.rangeMin;
        if (c.attributes.rangeMax) mx = *c.attributes.rangeMax;
        m_field->setRange(mn, mx);
        double step = c.attributes.step.value_or(integer ? 1.0 : 0.01);
        if (!c.attributes.step && c.attributes.rangeMin && c.attributes.rangeMax && !integer) {
            step = std::max(0.0001, (mx - mn) / 1000.0);
        }
        m_field->setStep(step);
        if (!integer) m_field->setDecimals(step < 0.001 ? 5 : step < 0.01 ? 4 : 3);
        m_field->setReadOnly(c.attributes.readOnly);
        l->addWidget(m_field);
        m_float = !integer;
        connect(m_field, &NumberField::edited, this, [this](double v, EditPhase phase) {
            commit(m_float ? serial::Value::makeF64(v) : serial::Value::makeInt(i64(v)), phase);
        });
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty()) return;
        if (!allEqual(v)) m_field->setMixed(true);
        else m_field->setValue(v[0].getDouble());
    }
    bool isEditing() const override { return m_field->isEditing(); }
    NumberField* m_field;
    bool m_float = true;
};

// ---- string -------------------------------------------------------------------------------------------------
class StringEditor final : public PropertyEditor {
public:
    StringEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        auto* l = hbox(this);
        m_edit = new QLineEdit(this);
        m_edit->setReadOnly(c.attributes.readOnly);
        l->addWidget(m_edit);
        connect(m_edit, &QLineEdit::editingFinished, this, [this] {
            if (!m_edit->isModified()) return;
            m_edit->setModified(false);
            m_edit->setProperty("mixed", false);
            commit(serial::Value::makeString(m_edit->text().toStdString()), EditPhase::Single);
        });
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (m_edit->hasFocus() || v.empty()) return;
        const bool mixed = !allEqual(v);
        m_edit->setProperty("mixed", mixed);
        m_edit->setPlaceholderText(mixed ? tr("Multiple values") : QString());
        m_edit->setText(mixed ? QString() : qs(v[0].getString()));
        repolish(m_edit);
    }
    bool isEditing() const override { return m_edit->hasFocus(); }
    QLineEdit* m_edit;
};

// ---- vectors ------------------------------------------------------------------------------------------------
class VectorEditor final : public PropertyEditor {
public:
    VectorEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        const serial::Tag t = c.type->tag;
        m_n = (t == serial::Tag::Vec2 || t == serial::Tag::IVec2) ? 2 : (t == serial::Tag::Vec3 || t == serial::Tag::IVec3) ? 3 : 4;
        m_int = t == serial::Tag::IVec2 || t == serial::Tag::IVec3 || t == serial::Tag::IVec4;
        auto* l = hbox(this);
        m_field = new VectorField(m_n, this, m_int);
        m_field->setStep(c.attributes.step.value_or(m_int ? 1.0 : 0.01));
        if (c.attributes.rangeMin && c.attributes.rangeMax) m_field->setRange(*c.attributes.rangeMin, *c.attributes.rangeMax);
        m_field->setReadOnly(c.attributes.readOnly);
        l->addWidget(m_field);
        connect(m_field, &VectorField::edited, this, [this](int i, EditPhase phase) {
            static const char* comps[] = {"x", "y", "z", "w"};
            const double v = m_field->values()[i];
            commit(m_int ? serial::Value::makeInt(i64(v)) : serial::Value::makeF64(v), phase, comps[i]);
        });
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty()) return;
        QVector<double> vals;
        QVector<bool> mixed;
        for (int i = 0; i < m_n; ++i) {
            const double a = m_int ? double(v[0].getIVec()[i]) : double(v[0].getVec()[i]);
            bool mix = false;
            for (const auto& x : v) {
                const double b = m_int ? double(x.getIVec()[i]) : double(x.getVec()[i]);
                if (b != a) mix = true;
            }
            vals.push_back(a);
            mixed.push_back(mix);
        }
        m_field->setValues(vals);
        m_field->setMixed(mixed);
    }
    bool isEditing() const override { return m_field->isEditing(); }
    VectorField* m_field;
    int m_n = 3;
    bool m_int = false;
};

// ---- quaternion as Euler degrees ----------------------------------------------------------------------------
class QuatEditor final : public PropertyEditor {
public:
    QuatEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        auto* l = hbox(this);
        m_field = new VectorField(3, this);
        m_field->setStep(0.5);
        m_field->setDecimals(2);
        m_field->setReadOnly(c.attributes.readOnly);
        for (int i = 0; i < 3; ++i) m_field->field(i)->setSuffix(QStringLiteral("°"));
        l->addWidget(m_field);
        connect(m_field, &VectorField::edited, this, [this](int comp, EditPhase phase) {
            const double deg = m_field->values()[comp];
            std::vector<serial::Value> out;
            for (const auto& e : m_eulers) {
                glm::vec3 ne = e;
                ne[comp] = float(deg);
                out.push_back(serial::Value::makeQuat(eulerDegreesToQuat(ne)));
            }
            if (phase == EditPhase::Single || phase == EditPhase::End) {
                for (auto& e : m_eulers) e[comp] = float(deg);
            }
            commit(out, phase);
        });
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty() || m_field->isEditing()) return;
        // Keep the user's Euler angles when they still describe the same rotation (avoids 180° flips).
        std::vector<glm::vec3> eulers;
        for (size_t i = 0; i < v.size(); ++i) {
            const glm::quat q = v[i].getQuat();
            glm::vec3 e = quatToEulerDegrees(q);
            if (i < m_eulers.size()) {
                const glm::quat cached = eulerDegreesToQuat(m_eulers[i]);
                if (std::abs(glm::dot(cached, glm::normalize(q))) > 0.99999f) e = m_eulers[i];
            }
            eulers.push_back(e);
        }
        m_eulers = eulers;
        QVector<double> vals;
        QVector<bool> mixed;
        for (int c = 0; c < 3; ++c) {
            vals.push_back(m_eulers[0][c]);
            bool mix = false;
            for (const auto& e : m_eulers) mix |= std::abs(e[c] - m_eulers[0][c]) > 1e-3f;
            mixed.push_back(mix);
        }
        m_field->setValues(vals);
        m_field->setMixed(mixed);
    }
    bool isEditing() const override { return m_field->isEditing(); }
    VectorField* m_field;
    std::vector<glm::vec3> m_eulers;
};

// ---- colour -------------------------------------------------------------------------------------------------
class ColorEditor final : public PropertyEditor {
public:
    ColorEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        m_vec4 = c.type->tag == serial::Tag::Vec4;
        auto* l = hbox(this);
        m_button = new ColorButton(this);
        m_button->setAlphaEnabled(m_vec4);
        m_button->setEnabled(!c.attributes.readOnly);
        l->addWidget(m_button, 1);
        if (c.attributes.hdr) {
            m_intensity = new NumberField(this);
            m_intensity->setRange(0.0, 1e6);
            m_intensity->setStep(0.05);
            m_intensity->setAxis(QStringLiteral("I"), colors().warning);
            m_intensity->setToolTip(tr("HDR intensity multiplier"));
            m_intensity->setFixedWidth(76);
            l->addWidget(m_intensity);
            connect(m_intensity, &NumberField::edited, this, [this](double, EditPhase phase) { emitColor(phase); });
        }
        connect(m_button, &ColorButton::colorEdited, this, [this](const QColor&, EditPhase phase) { emitColor(phase); });
    }
    void emitColor(EditPhase phase) {
        const QColor c = m_button->color();
        const float k = m_intensity ? float(m_intensity->value()) : 1.0f;
        glm::vec4 v(c.redF() * k, c.greenF() * k, c.blueF() * k, c.alphaF());
        commit(m_vec4 ? serial::Value::makeVec4(v) : serial::Value::makeVec3(glm::vec3(v)), phase);
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty()) return;
        if (!allEqual(v)) {
            m_button->setMixed(true);
            if (m_intensity) m_intensity->setMixed(true);
            return;
        }
        glm::vec4 c = v[0].getVec();
        if (!m_vec4) c.a = 1.0f;
        float k = 1.0f;
        if (m_intensity) {
            k = std::max({c.r, c.g, c.b, 1.0f});
            m_intensity->setValue(k);
        }
        m_button->setColor(QColor::fromRgbF(std::clamp(c.r / k, 0.f, 1.f), std::clamp(c.g / k, 0.f, 1.f),
                                            std::clamp(c.b / k, 0.f, 1.f), std::clamp(c.a, 0.f, 1.f)));
    }
    bool isEditing() const override { return m_intensity && m_intensity->isEditing(); }
    ColorButton* m_button;
    NumberField* m_intensity = nullptr;
    bool m_vec4 = false;
};

// ---- enum ---------------------------------------------------------------------------------------------------
class EnumEditor final : public PropertyEditor {
public:
    EnumEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        auto* l = hbox(this);
        m_combo = new QComboBox(this);
        m_combo->setEnabled(!c.attributes.readOnly);
        for (const auto& e : c.type->enumEntries) {
            const QString label = e.attributes.displayName.empty() ? prettifyName(e.name) : qs(e.attributes.displayName);
            m_combo->addItem(label, qs(e.name));
            if (!e.attributes.tooltip.empty()) m_combo->setItemData(m_combo->count() - 1, qs(e.attributes.tooltip), Qt::ToolTipRole);
        }
        l->addWidget(m_combo);
        connect(m_combo, &QComboBox::activated, this, [this](int i) {
            m_combo->setProperty("mixed", false);
            commit(serial::Value::makeEnum(m_combo->itemData(i).toString().toStdString()), EditPhase::Single);
        });
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty()) return;
        QSignalBlocker b(m_combo);
        const bool mixed = !allEqual(v);
        m_combo->setProperty("mixed", mixed);
        if (mixed) {
            m_combo->setCurrentIndex(-1);
            m_combo->setPlaceholderText(tr("Multiple values"));
        } else {
            QString name = qs(v[0].getString());
            if (name.isEmpty()) {
                if (const auto* e = m_ctx.type->findEnumByValue(v[0].getInt())) name = qs(e->name);
            }
            m_combo->setCurrentIndex(m_combo->findData(name));
        }
        repolish(m_combo);
    }
    QComboBox* m_combo;
};

// ---- asset reference / uuid ---------------------------------------------------------------------------------
void collectAssets(IAssetBackend& backend, const QString& folder, const QString& type, QList<AssetInfo>& out, int depth = 0) {
    if (depth > 12 || out.size() > 500) return;
    for (const auto& a : backend.list(folder)) {
        if (a.isFolder) collectAssets(backend, a.path, type, out, depth + 1);
        else if (type.isEmpty() || a.type == type || (type == QLatin1String("Texture") && a.type == QLatin1String("Texture"))) out.push_back(a);
    }
}

class AssetRefEditor final : public PropertyEditor {
public:
    AssetRefEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        setAcceptDrops(!c.attributes.readOnly);
        m_type = c.attributes.assetType ? qs(*c.attributes.assetType) : QString();
        auto* l = hbox(this);
        m_button = new QPushButton(this);
        m_button->setIcon(Icons::get(Icons::forAssetType(m_type)));
        m_button->setStyleSheet(QStringLiteral("QPushButton{text-align:left;padding-left:8px;}"));
        m_button->setEnabled(!c.attributes.readOnly);
        m_button->setMinimumWidth(40);
        m_button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_button->setToolTip(tr("Click to pick a %1 asset, or drop one from the Content Browser").arg(m_type));
        l->addWidget(m_button, 1);
        m_clear = makeToolButton(QStringLiteral("close"), tr("Clear"), this);
        m_clear->setEnabled(!c.attributes.readOnly);
        l->addWidget(m_clear);
        connect(m_clear, &QToolButton::clicked, this, [this] { commit(serial::Value::makeUuid(Uuid{}), EditPhase::Single); });
        connect(m_button, &QPushButton::clicked, this, [this] { pick(); });
    }
    void pick() {
        QMenu menu(this);
        menu.addAction(Icons::get(QStringLiteral("close")), tr("None"), this, [this] { commit(serial::Value::makeUuid(Uuid{}), EditPhase::Single); });
        menu.addSeparator();
        QList<AssetInfo> assets;
        if (m_ctx.editor) collectAssets(m_ctx.editor->services().assets(), {}, m_type, assets);
        if (assets.isEmpty()) menu.addAction(tr("No %1 assets in the project").arg(m_type))->setEnabled(false);
        for (const auto& a : assets) {
            const Uuid id = a.uuid;
            menu.addAction(Icons::get(Icons::forAssetType(a.type)), a.name, this, [this, id] { commit(serial::Value::makeUuid(id), EditPhase::Single); })
                ->setToolTip(a.relativePath);
        }
        menu.exec(m_button->mapToGlobal(QPoint(0, m_button->height())));
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty()) return;
        if (!allEqual(v)) {
            m_button->setText(tr("Multiple values"));
            return;
        }
        const Uuid id = v[0].getUuid();
        m_clear->setVisible(id.isValid() && !m_ctx.attributes.readOnly);
        if (id.isNil()) {
            m_button->setText(tr("None (%1)").arg(m_type.isEmpty() ? tr("Asset") : m_type));
            return;
        }
        QString name;
        if (const QString prim = builtin::primitiveName(id); !prim.isEmpty()) name = tr("%1 (built-in)").arg(prim);
        else if (id == builtin::defaultMaterial()) name = tr("Default Material (built-in)");
        else if (m_ctx.editor) {
            if (auto a = m_ctx.editor->services().assets().find(id)) name = a->name;
        }
        if (name.isEmpty()) name = qs(id.toString()).left(13) + QStringLiteral("…");
        m_button->setText(name);
        m_button->setToolTip(qs(id.toString()));
    }
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasFormat(kMimeAsset)) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        const QStringList lines = QString::fromUtf8(e->mimeData()->data(kMimeAsset)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (lines.isEmpty()) return;
        const QStringList parts = lines.first().split(QLatin1Char('|'));
        if (parts.size() < 2) return;
        if (!m_type.isEmpty() && parts[1] != m_type) return;
        if (auto id = Uuid::parse(parts[0].toStdString())) {
            commit(serial::Value::makeUuid(*id), EditPhase::Single);
            e->acceptProposedAction();
        }
    }
    QPushButton* m_button;
    QToolButton* m_clear;
    QString m_type;
};

class UuidEditor final : public PropertyEditor {
public:
    UuidEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        auto* l = hbox(this);
        m_edit = new QLineEdit(this);
        m_edit->setReadOnly(true);
        m_edit->setFont(Theme::monoFont());
        l->addWidget(m_edit);
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty()) return;
        m_edit->setText(allEqual(v) ? qs(v[0].getUuid().toString()) : tr("Multiple values"));
    }
    QLineEdit* m_edit;
};

// ---- entity reference ---------------------------------------------------------------------------------------
class EntityRefEditor final : public PropertyEditor {
public:
    EntityRefEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        setAcceptDrops(!c.attributes.readOnly);
        auto* l = hbox(this);
        m_button = new QPushButton(this);
        m_button->setIcon(Icons::get(QStringLiteral("entity")));
        m_button->setStyleSheet(QStringLiteral("QPushButton{text-align:left;padding-left:8px;}"));
        m_button->setToolTip(tr("Pick an entity, or drag one from the Outliner"));
        m_button->setMinimumWidth(40);
        m_button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        m_button->setEnabled(!c.attributes.readOnly);
        l->addWidget(m_button, 1);
        m_select = makeToolButton(QStringLiteral("focus"), tr("Select referenced entity"), this);
        l->addWidget(m_select);
        connect(m_button, &QPushButton::clicked, this, [this] { pick(); });
        connect(m_select, &QToolButton::clicked, this, [this] {
            if (m_ctx.editor && m_current.isValid()) m_ctx.editor->selection().select(m_current);
        });
    }
    void pick() {
        if (!m_ctx.editor) return;
        QMenu menu(this);
        menu.addAction(Icons::get(QStringLiteral("close")), tr("None"), this, [this] { commit(serial::Value::makeEntityRef(Uuid{}), EditPhase::Single); });
        menu.addSeparator();
        World& w = m_ctx.editor->world();
        int n = 0;
        w.forEachInHierarchy([&](entt::entity h) {
            if (n++ > 400) return;
            Entity e = w.wrap(h);
            int depth = 0;
            for (Entity p = e.parent(); p; p = p.parent()) ++depth;
            const Uuid id = e.uuid();
            menu.addAction(QString(depth * 3, QLatin1Char(' ')) + qs(e.name()), this,
                           [this, id] { commit(serial::Value::makeEntityRef(id), EditPhase::Single); });
        });
        menu.exec(m_button->mapToGlobal(QPoint(0, m_button->height())));
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty()) return;
        if (!allEqual(v)) {
            m_button->setText(tr("Multiple values"));
            return;
        }
        m_current = v[0].getUuid();
        m_select->setEnabled(m_current.isValid());
        if (m_current.isNil()) {
            m_button->setText(tr("None (Entity)"));
            return;
        }
        Entity e = m_ctx.editor ? m_ctx.editor->world().find(m_current) : Entity{};
        m_button->setText(e ? qs(e.name()) : tr("Missing entity"));
    }
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasFormat(kMimeEntities)) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        const QStringList ids = QString::fromUtf8(e->mimeData()->data(kMimeEntities)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (ids.isEmpty()) return;
        if (auto id = Uuid::parse(ids.first().toStdString())) {
            commit(serial::Value::makeEntityRef(*id), EditPhase::Single);
            e->acceptProposedAction();
        }
    }
    QPushButton* m_button;
    QToolButton* m_select;
    Uuid m_current;
};

// ---- matrix (read-only) -------------------------------------------------------------------------------------
class MatrixEditor final : public PropertyEditor {
public:
    MatrixEditor(const PropertyContext& c, QWidget* p) : PropertyEditor(c, p) {
        auto* g = new QGridLayout(this);
        g->setContentsMargins(0, 0, 0, 0);
        g->setSpacing(2);
        for (int r = 0; r < 4; ++r) {
            for (int col = 0; col < 4; ++col) {
                auto* l = new QLabel(this);
                l->setFont(Theme::monoFont());
                l->setProperty("role", "dim");
                g->addWidget(l, r, col);
                m_cells.push_back(l);
            }
        }
    }
    void setValues(const std::vector<serial::Value>& v) override {
        if (v.empty()) return;
        const glm::mat4 m = v[0].getMat4();
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) m_cells[r * 4 + c]->setText(QString::number(m[c][r], 'f', 3));
        }
    }
    QVector<QLabel*> m_cells;
};

struct Entry {
    int priority;
    PropertyEditorFactory::Predicate predicate;
    PropertyEditorFactory::Creator creator;
};
std::vector<Entry>& entries() {
    static std::vector<Entry> e;
    return e;
}

bool isColorType(const reflect::TypeInfo& t, const reflect::Attributes& a) {
    return a.color && (t.tag == serial::Tag::Vec3 || t.tag == serial::Tag::Vec4);
}

} // namespace

void PropertyEditorFactory::registerEditor(int priority, Predicate predicate, Creator creator) {
    registerBuiltins();
    auto& e = entries();
    e.push_back({priority, std::move(predicate), std::move(creator)});
    std::stable_sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) { return a.priority > b.priority; });
}

void PropertyEditorFactory::registerBuiltins() {
    static bool done = false;
    if (done) return;
    done = true;
    using K = reflect::Kind;
    auto& e = entries();
    auto add = [&](int prio, Predicate p, Creator c) { e.push_back({prio, std::move(p), std::move(c)}); };
    add(10, [](const auto& t, const auto&) { return t.kind == K::Bool; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new BoolEditor(c, p); });
    add(10, [](const auto& t, const auto&) { return t.kind == K::Int || t.kind == K::UInt || t.kind == K::Float; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new NumberEditor(c, p); });
    add(10, [](const auto& t, const auto&) { return t.kind == K::String; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new StringEditor(c, p); });
    add(20, [](const auto& t, const auto& a) { return isColorType(t, a); },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new ColorEditor(c, p); });
    add(10, [](const auto& t, const auto&) { return t.kind == K::Math && t.tag == serial::Tag::Quat; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new QuatEditor(c, p); });
    add(10, [](const auto& t, const auto&) { return t.kind == K::Math && t.tag == serial::Tag::Mat4; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new MatrixEditor(c, p); });
    add(5, [](const auto& t, const auto&) { return t.kind == K::Math; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new VectorEditor(c, p); });
    add(10, [](const auto& t, const auto&) { return t.kind == K::Enum && !t.enumEntries.empty(); },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new EnumEditor(c, p); });
    add(5, [](const auto& t, const auto&) { return t.kind == K::Enum; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new NumberEditor(c, p); });
    add(20, [](const auto& t, const auto& a) { return t.kind == K::Uuid && a.assetType.has_value(); },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new AssetRefEditor(c, p); });
    add(10, [](const auto& t, const auto&) { return t.kind == K::Uuid; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new UuidEditor(c, p); });
    add(10, [](const auto& t, const auto&) { return t.kind == K::Custom && t.tag == serial::Tag::EntityRef; },
        [](const auto& c, QWidget* p) -> PropertyEditor* { return new EntityRefEditor(c, p); });
    std::stable_sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) { return a.priority > b.priority; });
}

bool PropertyEditorFactory::isLeaf(const reflect::TypeInfo& type, const reflect::Attributes& attrs) {
    registerBuiltins();
    for (const auto& e : entries()) {
        if (e.predicate(type, attrs)) return true;
    }
    return false;
}

PropertyEditor* PropertyEditorFactory::create(const PropertyContext& ctx, QWidget* parent) {
    registerBuiltins();
    if (!ctx.type) return nullptr;
    for (const auto& e : entries()) {
        if (e.predicate(*ctx.type, ctx.attributes)) return e.creator(ctx, parent);
    }
    return nullptr;
}

} // namespace ox::editor
