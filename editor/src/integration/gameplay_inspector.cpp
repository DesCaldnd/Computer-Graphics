// Inspector extensions for gameplay components: script properties, collider fitting, navmesh baking, behaviour
// tree debugger shortcut, spline point editing and terrain sculpting toggles.
#include "integration/gameplay_inspector.hpp"

#include "core/editor_context.hpp"
#include "inspector/component_extensions.hpp"
#include "inspector/property_editors.hpp"
#include "integration/gameplay_tools.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <QDesktopServices>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#if OX_EDITOR_HAS_GAMEPLAY
#include <oxwald/script/script_value.hpp>
#endif

namespace ox::editor {

#if OX_EDITOR_HAS_GAMEPLAY
namespace {

QHBoxLayout* row(QWidget* w) {
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 6, 0, 0);
    l->setSpacing(6);
    return l;
}

const reflect::TypeInfo& editorTypeFor(int type, reflect::Attributes& attrs) {
    using T = script::ScriptPropertyType;
    switch (T(type)) {
    case T::Float: return reflect::typeOf<double>();
    case T::Int: return reflect::typeOf<i64>();
    case T::Bool: return reflect::typeOf<bool>();
    case T::String: return reflect::typeOf<std::string>();
    case T::Vec2: return reflect::typeOf<glm::vec2>();
    case T::Vec3: return reflect::typeOf<glm::vec3>();
    case T::Color: attrs.color = true; return reflect::typeOf<glm::vec4>();
    default: return reflect::typeOf<glm::vec4>();
    }
}

// Declared `properties = {...}` of the script as typed fields with ranges; values are per-entity overrides.
class ScriptPropertiesWidget final : public ComponentExtensionWidget {
public:
    ScriptPropertiesWidget(EditorContext* ctx, UuidList entities, QWidget* parent)
        : ComponentExtensionWidget(parent), m_ctx(ctx), m_entities(std::move(entities)) {
        setObjectName(QStringLiteral("ScriptProperties"));
        m_layout = new QVBoxLayout(this);
        m_layout->setContentsMargins(0, 4, 0, 0);
        m_layout->setSpacing(4);
        rebuild();
    }

    void refresh() override {
        const ScriptPropertiesResult r = scriptProperties(*m_ctx, m_entities.back());
        if (signature(r) != m_signature) {
            rebuild();
            return;
        }
        update(r);
    }

    [[nodiscard]] PropertyEditor* editorFor(const std::string& name) const {
        auto it = m_editors.find(name);
        return it == m_editors.end() ? nullptr : it->second;
    }

private:
    static QString signature(const ScriptPropertiesResult& r) {
        QString s = r.scriptName + QLatin1Char('|') + r.error;
        for (const auto& p : r.rows) s += qs(p.name) + QLatin1Char(':') + QString::number(p.type) + QLatin1Char(',');
        return s;
    }

    void rebuild() {
        while (QLayoutItem* it = m_layout->takeAt(0)) {
            if (it->widget()) {
                it->widget()->hide();
                it->widget()->deleteLater();
            }
            delete it;
        }
        m_editors.clear();
        m_labels.clear();
        const ScriptPropertiesResult r = scriptProperties(*m_ctx, m_entities.back());
        m_signature = signature(r);
        auto* header = new QWidget(this);
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(0, 8, 0, 2);
        hl->setSpacing(8);
        hl->addWidget(makeSectionLabel(tr("Script Properties"), header));
        hl->addWidget(makeHairline(header), 1);
        if (!r.scriptName.isEmpty()) {
            auto* open = makeToolButton(QStringLiteral("code"), tr("Open %1").arg(r.scriptName), header);
            connect(open, &QToolButton::clicked, this, [this] { openScript(); });
            hl->addWidget(open);
        }
        m_layout->addWidget(header);
        if (!r.error.isEmpty() || r.rows.empty()) {
            auto* l = new QLabel(r.error.isEmpty() ? tr("The script declares no properties") : r.error, this);
            l->setProperty("role", "faint");
            l->setWordWrap(true);
            m_layout->addWidget(l);
        }
        if (r.rows.empty()) return;
        auto* gridHost = new QWidget(this);
        auto* grid = new QGridLayout(gridHost);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(5);
        grid->setColumnMinimumWidth(0, 112);
        grid->setColumnStretch(1, 1);
        int i = 0;
        for (const ScriptPropertyRow& p : r.rows) {
            auto* label = new QLabel(prettifyName(p.name), gridHost);
            label->setProperty("role", "dim");
            label->setToolTip((p.tooltip.isEmpty() ? QString() : p.tooltip + QStringLiteral("\n\n")) + tr("Script property '%1'").arg(qs(p.name)));
            label->setContextMenuPolicy(Qt::CustomContextMenu);
            const std::string name = p.name;
            const int type = p.type;
            connect(label, &QLabel::customContextMenuRequested, this, [this, label, name, type](const QPoint& pos) {
                QMenu menu(this);
                menu.addAction(Icons::get(QStringLiteral("refresh")), tr("Reset to Script Default"), this,
                               [this, name, type] { setScriptPropertyOverride(*m_ctx, m_entities, name, type, std::nullopt); });
                menu.exec(label->mapToGlobal(pos));
            });
            PropertyContext pc;
            pc.editor = m_ctx;
            pc.entities = m_entities;
            pc.component = "Script";
            pc.path = "properties." + p.name;
            pc.type = &editorTypeFor(p.type, pc.attributes);
            pc.attributes.rangeMin = p.min;
            pc.attributes.rangeMax = p.max;
            pc.attributes.tooltip = p.tooltip.toStdString();
            PropertyEditor* ed = PropertyEditorFactory::create(pc, gridHost);
            if (!ed) continue;
            ed->setObjectName(QStringLiteral("scriptprop:%1").arg(qs(p.name)));
            ed->setCommit([this, name, type](const std::string& sub, const std::vector<serial::Value>& values, EditPhase phase) {
                if (values.empty()) return;
                serial::Value v = values.front();
                if (!sub.empty()) {
                    // A vector component was edited: merge into the current value.
                    const ScriptPropertiesResult cur = scriptProperties(*m_ctx, m_entities.back());
                    for (const auto& row : cur.rows) {
                        if (row.name != name) continue;
                        glm::vec4 vec = row.currentValue.getVec();
                        const int idx = sub == "x" || sub == "r" ? 0 : sub == "y" || sub == "g" ? 1 : sub == "z" || sub == "b" ? 2 : 3;
                        vec[idx] = float(v.getDouble());
                        v = serial::Value::makeVec4(vec);
                    }
                }
                setScriptPropertyOverride(*m_ctx, m_entities, name, type, v, phase);
            });
            grid->addWidget(label, i, 0, Qt::AlignLeft | Qt::AlignVCenter);
            grid->addWidget(ed, i, 1);
            m_editors[p.name] = ed;
            m_labels[p.name] = label;
            ++i;
        }
        m_layout->addWidget(gridHost);
        update(r);
    }

    void update(const ScriptPropertiesResult& r) {
        const ThemePalette& c = colors();
        for (const ScriptPropertyRow& p : r.rows) {
            if (auto* ed = editorFor(p.name); ed && !ed->isEditing()) {
                serial::Value v = p.currentValue;
                if (p.type == int(script::ScriptPropertyType::Vec2)) v = serial::Value::makeVec2(glm::vec2(v.getVec()));
                else if (p.type == int(script::ScriptPropertyType::Vec3)) v = serial::Value::makeVec3(glm::vec3(v.getVec()));
                ed->setValues({v});
            }
            if (QLabel* l = m_labels.count(p.name) ? m_labels[p.name] : nullptr) {
                const QString css = p.overridden ? QStringLiteral("QLabel{color:%1;font-weight:600;border-left:2px solid %2;padding-left:4px;}")
                                                       .arg(cssColor(c.accentText), cssColor(c.accent))
                                                 : QString();
                if (l->styleSheet() != css) l->setStyleSheet(css);
                l->setProperty("overridden", p.overridden);
            }
        }
    }

    void openScript() {
        const ScriptPropertiesResult r = scriptProperties(*m_ctx, m_entities.back());
        if (auto a = m_ctx->services().assets().allOfType(QStringLiteral("Script")); !a.isEmpty()) {
            for (const auto& s : a) {
                if (QFileInfo(s.path).fileName() == r.scriptName) {
                    Q_EMIT m_ctx->openSourceRequested(s.path, 1);
                    return;
                }
            }
        }
    }

    EditorContext* m_ctx;
    UuidList m_entities;
    QVBoxLayout* m_layout;
    QString m_signature;
    std::map<std::string, PropertyEditor*> m_editors;
    std::map<std::string, QLabel*> m_labels;
};

class ButtonsFooter final : public ComponentExtensionWidget {
public:
    using ComponentExtensionWidget::ComponentExtensionWidget;
    std::function<void()> onRefresh;
    void refresh() override {
        if (onRefresh) onRefresh();
    }
};

ComponentExtensionWidget* colliderFooter(EditorContext* ctx, const UuidList& ids, QWidget* parent) {
    auto* w = new ButtonsFooter(parent);
    auto* l = row(w);
    auto* fit = new QPushButton(Icons::get(QStringLiteral("focus")), QObject::tr("Fit to Mesh"), w);
    fit->setObjectName(QStringLiteral("collider.fit"));
    fit->setToolTip(QObject::tr("Resize the collider to the bounds of the entity's mesh (MeshRenderer)"));
    QObject::connect(fit, &QPushButton::clicked, w, [ctx, ids] {
        QString msg;
        fitColliderToMesh(*ctx, ids, &msg);
        Q_EMIT ctx->statusMessage(msg, 3000);
    });
    l->addWidget(fit);
    l->addStretch(1);
    return w;
}

ComponentExtensionWidget* navFooter(EditorContext* ctx, const UuidList& ids, QWidget* parent) {
    auto* w = new ButtonsFooter(parent);
    auto* l = row(w);
    auto* bake = new QPushButton(Icons::get(QStringLiteral("map")), QObject::tr("Bake"), w);
    bake->setObjectName(QStringLiteral("navmesh.bake"));
    auto* status = new QLabel(w);
    status->setProperty("role", "dim");
    l->addWidget(bake);
    l->addWidget(status, 1);
    const Uuid id = ids.back();
    w->onRefresh = [ctx, id, status, bake] {
        auto v = props::read(ctx->world(), id, "NavMeshSurface", "bakedData");
        const usize n = v ? v->size() : 0;
        status->setText(n ? QObject::tr("Baked data: %1").arg(formatBytes(n)) : QObject::tr("Not baked"));
        bake->setEnabled(!ctx->isPlaying());
    };
    w->onRefresh();
    QObject::connect(bake, &QPushButton::clicked, w, [ctx, id, w] {
        const BakeResult r = bakeNavMesh(*ctx, id);
        Q_EMIT ctx->statusMessage(r.message, 5000);
        w->refresh();
    });
    return w;
}

ComponentExtensionWidget* btFooter(EditorContext* ctx, const UuidList&, QWidget* parent) {
    auto* w = new ButtonsFooter(parent);
    auto* l = row(w);
    auto* open = new QPushButton(Icons::get(QStringLiteral("sitemap")), QObject::tr("Open Behavior Tree Debugger"), w);
    QObject::connect(open, &QPushButton::clicked, w, [ctx] {
        if (QAction* a = ctx->actions().action(QStringLiteral("window.behaviorTree"))) a->trigger();
    });
    l->addWidget(open);
    l->addStretch(1);
    return w;
}

ComponentExtensionWidget* splineFooter(EditorContext* ctx, const UuidList& ids, QWidget* parent) {
    auto* w = new ButtonsFooter(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 6, 0, 0);
    auto* l = new QHBoxLayout();
    auto* edit = new QPushButton(Icons::get(QStringLiteral("translate")), QObject::tr("Edit Points"), w);
    edit->setObjectName(QStringLiteral("spline.edit"));
    edit->setCheckable(true);
    auto* hint = new QLabel(QObject::tr("Click a point to move it · Ctrl+click adds · Del removes · Esc exits"), w);
    hint->setProperty("role", "faint");
    hint->setWordWrap(true);
    l->addWidget(edit);
    l->addStretch(1);
    v->addLayout(l);
    v->addWidget(hint);
    const Uuid id = ids.back();
    w->onRefresh = [ctx, id, edit, hint] {
        const bool on = ctx->tools().tool() == ViewportTool::SplinePoints && ctx->tools().target() == id;
        QSignalBlocker b(edit);
        edit->setChecked(on);
        hint->setVisible(on);
    };
    w->onRefresh();
    QObject::connect(edit, &QPushButton::toggled, w, [ctx, id](bool on) {
        ctx->tools().setTool(on ? ViewportTool::SplinePoints : ViewportTool::Transform, on ? id : Uuid{});
    });
    QObject::connect(&ctx->tools(), &ToolState::changed, w, [w] { w->refresh(); });
    return w;
}

ComponentExtensionWidget* terrainFooter(EditorContext* ctx, const UuidList& ids, QWidget* parent) {
    auto* w = new ButtonsFooter(parent);
    auto* v = new QVBoxLayout(w);
    v->setContentsMargins(0, 6, 0, 0);
    v->setSpacing(6);
    auto* top = new QHBoxLayout();
    auto* sculpt = new QPushButton(Icons::get(QStringLiteral("brush")), QObject::tr("Sculpt"), w);
    sculpt->setObjectName(QStringLiteral("terrain.sculpt"));
    sculpt->setCheckable(true);
    auto* ops = new SegmentedControl({QObject::tr("Raise"), QObject::tr("Lower"), QObject::tr("Smooth"), QObject::tr("Flatten")}, w);
    top->addWidget(sculpt);
    top->addWidget(ops, 1);
    v->addLayout(top);
    auto* params = new QHBoxLayout();
    auto* radius = new NumberField(w);
    radius->setRange(0.5, 200);
    radius->setStep(0.25);
    radius->setDecimals(1);
    radius->setSuffix(QStringLiteral(" m"));
    radius->setAxis(QObject::tr("R"), colors().accent);
    auto* strength = new NumberField(w);
    strength->setRange(0.05, 50);
    strength->setStep(0.05);
    strength->setDecimals(2);
    strength->setAxis(QObject::tr("S"), colors().warning);
    params->addWidget(radius);
    params->addWidget(strength);
    v->addLayout(params);
    auto* hint = new QLabel(QObject::tr("LMB paints · Shift smooths · Ctrl lowers · Ctrl+wheel radius. Edits live in the WorldRuntime "
                                        "(runtime-only; the component rebuild resets them)."),
                            w);
    hint->setProperty("role", "faint");
    hint->setWordWrap(true);
    v->addWidget(hint);
    const Uuid id = ids.back();
    w->onRefresh = [ctx, id, sculpt, ops, radius, strength] {
        const bool on = ctx->tools().tool() == ViewportTool::TerrainSculpt && ctx->tools().target() == id;
        QSignalBlocker b(sculpt);
        sculpt->setChecked(on);
        ops->setCurrent(int(ctx->tools().sculpt.op));
        if (!radius->isEditing()) radius->setValue(ctx->tools().sculpt.radius);
        if (!strength->isEditing()) strength->setValue(ctx->tools().sculpt.strength);
    };
    w->onRefresh();
    QObject::connect(sculpt, &QPushButton::toggled, w, [ctx, id](bool on) {
        ctx->tools().setTool(on ? ViewportTool::TerrainSculpt : ViewportTool::Transform, on ? id : Uuid{});
    });
    QObject::connect(ops, &SegmentedControl::activated, w, [ctx](int i) { ctx->tools().sculpt.op = SculptOp(i); });
    QObject::connect(radius, &NumberField::edited, w, [ctx](double v, EditPhase) { ctx->tools().sculpt.radius = float(v); });
    QObject::connect(strength, &NumberField::edited, w, [ctx](double v, EditPhase) { ctx->tools().sculpt.strength = float(v); });
    QObject::connect(&ctx->tools(), &ToolState::changed, w, [w] { w->refresh(); });
    return w;
}

} // namespace

void registerGameplayInspectorExtensions() {
    ComponentExtensions::add("Script", {{"properties"}, [](EditorContext* ctx, const UuidList& ids, QWidget* parent) -> ComponentExtensionWidget* {
                                            return new ScriptPropertiesWidget(ctx, ids, parent);
                                        }});
    ComponentExtensions::add("Collider", {{}, colliderFooter});
    ComponentExtensions::add("NavMeshSurface", {{}, navFooter});
    ComponentExtensions::add("BehaviorTree", {{}, btFooter});
    ComponentExtensions::add("Spline", {{}, splineFooter});
    ComponentExtensions::add("Terrain", {{}, terrainFooter});
}

#else

void registerGameplayInspectorExtensions() {}

#endif

} // namespace ox::editor

// ---- showcase template content (gameplay) ---------------------------------------------------------------------

#if OX_EDITOR_HAS_GAMEPLAY
#include "core/scene_templates.hpp"

#include <oxwald/gameplay/gameplay.hpp>

#include <QDir>
#include <QFile>

namespace ox::editor {

namespace {

const char* kSpinnerLua = R"LUA(-- Spinner.lua (OxwaldEditor showcase): spins + bobs the entity, pulses with an async loop.
properties = {
    speed = { type = "float", default = 0.8, min = 0, max = 10, tooltip = "Turns per second (radians)", order = 1 },
    bobHeight = { type = "float", default = 0.15, min = 0, max = 2, tooltip = "Vertical bob amplitude (m)", order = 2 },
    pulse = { type = "bool", default = true, tooltip = "Run the async pulse loop (scene.delay coroutines)", order = 3 },
    label = { type = "string", default = "Orb", order = 4 },
}

function onStart(self)
    self.baseY = self.entity.transform.position.y
    self.time = 0
    self.beats = 0
    if self.pulse then
        spawn(function()
            while true do
                await(scene.delay(0.75))
                self.beats = self.beats + 1
            end
        end)
    end
end

function onUpdate(self, dt)
    self.time = self.time + dt
    local t = self.entity.transform
    t:rotate(vec3(0, 1, 0), self.speed * dt)
    local p = t.position
    t.position = vec3(p.x, self.baseY + math.sin(self.time * 2) * self.bobHeight, p.z)
end
)LUA";

void addBox(Entity e, glm::vec3 half = glm::vec3(0.5f), glm::vec3 offset = glm::vec3(0.0f)) {
    auto& c = e.add<gameplay::ColliderComponent>();
    c.type = gameplay::ColliderType::Box;
    c.halfExtents = half;
    c.offsetPosition = offset;
}

void decorate(World& w, const QString& assetDir) {
    using namespace gameplay;
    if (Entity ground = w.findByName("Ground")) addBox(ground, {0.5f, 0.05f, 0.5f}, {0.0f, -0.05f, 0.0f});
    if (Entity pedestal = w.findByName("Pedestal")) {
        auto& c = pedestal.add<ColliderComponent>();
        c.type = ColliderType::Cylinder;
        c.radius = 0.5f;
        c.halfHeight = 0.5f;
    }
    for (int i = 1; i <= 6; ++i) {
        if (Entity p = w.findByName(("Pillar " + std::to_string(i)).c_str())) addBox(p);
    }
    for (const char* n : {"Crate A", "Crate B", "Crate C"}) {
        if (Entity c = w.findByName(n)) {
            addBox(c);
            auto& rb = c.add<RigidBodyComponent>();
            rb.motionType = physics::MotionType::Dynamic;
            rb.mass = 20.0f;
        }
    }
    // Script with declared properties (spins the orb, async pulse loop).
    if (Entity orb = w.findByName("Orb")) {
        auto& s = orb.add<ScriptComponent>();
        s.script = "Scripts/Spinner.lua";
        s.properties["speed"] = ScriptPropertyValue::makeNumber(1.2);
        QDir().mkpath(QDir(assetDir).filePath(QStringLiteral("Scripts")));
        QFile f(QDir(assetDir).filePath(QStringLiteral("Scripts/Spinner.lua")));
        if (f.open(QIODevice::WriteOnly)) f.write(kSpinnerLua);
    }
    // Patrol path + a drone following it.
    Entity level = w.findByName("Level");
    Entity path = w.create("Patrol Path", level);
    path.setPosition({0, 2.6f, 0});
    auto& sp = path.add<SplineComponent>();
    sp.type = spline::SplineType::CatmullRom;
    sp.closed = true;
    for (int i = 0; i < 6; ++i) {
        const float a = float(i) / 6.0f * 6.2831853f + 0.5f;
        SplinePoint pt;
        pt.position = {std::cos(a) * 7.5f, (i % 2) * 0.8f, std::sin(a) * 7.5f};
        sp.points.push_back(pt);
    }
    sp.color = {0.35f, 0.85f, 1.0f, 1.0f};
    Entity drone = w.create("Drone", level);
    auto& mr = drone.add<MeshRendererComponent>();
    mr.mesh = builtin::sphereMesh();
    mr.materials = {builtin::defaultMaterial()};
    drone.setScale(glm::vec3(0.45f));
    auto& follower = drone.add<SplineFollowerComponent>();
    follower.spline = path.ref();
    follower.speed = 2.5f;
    // AI: navmesh surface + a guard running a small behaviour tree.
    Entity nav = w.create("Navigation", level);
    nav.add<NavMeshSurfaceComponent>().bakeOnStart = true;
    Entity guard = w.create("Guard", level);
    guard.setPosition({-3.5f, 0.6f, 3.0f});
    guard.setScale({0.6f, 1.2f, 0.6f});
    auto& gm = guard.add<MeshRendererComponent>();
    gm.mesh = builtin::cylinderMesh();
    gm.materials = {builtin::defaultMaterial()};
    auto& bt = guard.add<BehaviorTreeComponent>();
    bt.treeJson = R"({"root":{"type":"Sequence","name":"Guard Duty","children":[
        {"type":"Wait","name":"Look Around","seconds":0.6},
        {"type":"SetBlackboard","name":"Raise Alert","key":"alert","value":true},
        {"type":"Wait","name":"Hold Position","seconds":0.9}]}})";
}

} // namespace

void registerGameplayTemplates() { setShowcaseDecorator(decorate); }

} // namespace ox::editor
#else
namespace ox::editor {
void registerGameplayTemplates() {}
} // namespace ox::editor
#endif
