#include "settings/project_settings_dialog.hpp"

#include "core/editor_context.hpp"
#include "settings/render_cvars.hpp"
#include "settings/scalability_widget.hpp"
#include "theme/icons.hpp"
#include "theme/theme.hpp"
#include "widgets/widgets.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/scalability.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDirIterator>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QStandardItemModel>
#include <QVBoxLayout>

namespace ox::editor {

namespace {

// Binding for a JSON sub-tree of the project settings (custom editors: matrices, mappings, buses).
SettingBinding projectJsonBinding(Project* project, const QString& path) {
    SettingBinding b;
    b.key = QStringLiteral("project:") + path;
    const std::string p = path.toStdString();
    b.get = [project, p]() -> QVariant { return QString::fromStdString(project->setting(p).dump()); };
    b.set = [project, p](const QVariant& v) {
        auto j = nlohmann::json::parse(v.toString().toStdString(), nullptr, false);
        if (!j.is_discarded()) project->setSetting(p, j);
    };
    return b;
}

QStringList cvarEnumLabels(const char* name) {
    QStringList out;
    if (ICVar* c = CVarRegistry::instance().find(name)) {
        for (const auto& n : c->enumNames()) out << cvarValueLabel(QString::fromLatin1(name), QString::fromStdString(n));
    }
    return out;
}

QVariantList indices(int n) {
    QVariantList v;
    for (int i = 0; i < n; ++i) v << i;
    return v;
}

void disableComboItem(QComboBox* combo, int index, const QString& reason) {
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    if (!model || index < 0 || index >= model->rowCount()) return;
    QStandardItem* it = model->item(index);
    it->setFlags(it->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable));
    it->setToolTip(reason);
    it->setText(it->text() + QObject::tr("  (unavailable)"));
}

} // namespace

ProjectSettingsDialog::ProjectSettingsDialog(EditorContext* ctx, QWidget* parent)
    : SettingsDialog(tr("Project Settings"), tr("Settings stored in the project and shipped with the game."), parent), m_ctx(ctx),
      m_project(ctx->project()) {
    setObjectName(QStringLiteral("ProjectSettingsDialog"));
    ensureRenderingCVars();
    captureSnapshot();
    addGroup(tr("Project"));
    buildGeneral();
    buildMaps();
    buildPackaging();
    addGroup(tr("Engine"));
    buildRendering();
    buildScalability();
    buildPhysics();
    buildAudio();
    buildInput();
    buildNetworking();
    buildScripting();
    showPage(QStringLiteral("general"));
    connect(this, &SettingsDialog::settingChanged, this, [this] {
        if (m_scalability) m_scalability->refresh();
    });
}

void ProjectSettingsDialog::captureSnapshot() {
    if (m_project) m_settingsSnapshot = m_project->settings();
    m_cvarSnapshot.clear();
    for (ICVar* c : CVarRegistry::instance().all()) m_cvarSnapshot[c->name()] = c->toJson();
}

void ProjectSettingsDialog::applyChanges() {
    if (!m_project) return;
    m_project->captureCVarSettings();
    if (auto st = m_project->save(); !st) Q_EMIT m_ctx->statusMessage(QString::fromStdString(st.error().message), 4000);
    else Q_EMIT m_ctx->statusMessage(tr("Project settings saved"), 2500);
    captureSnapshot();
}

void ProjectSettingsDialog::revertChanges() {
    if (m_project) {
        m_project->settings() = m_settingsSnapshot;
        Q_EMIT m_project->settingsChanged();
    }
    // group levels first, then individual cvars (so custom overrides survive)
    for (auto& [name, value] : m_cvarSnapshot) {
        if (name.rfind("sg.", 0) == 0) {
            if (ICVar* c = CVarRegistry::instance().find(name)) c->setFromJson(value, CVarSource::Config);
        }
    }
    for (auto& [name, value] : m_cvarSnapshot) {
        if (ICVar* c = CVarRegistry::instance().find(name); c && !c->hasFlag(CVarFlags::ReadOnly)) c->setFromJson(value, CVarSource::Config);
    }
    if (m_scalability) m_scalability->refresh();
}

void ProjectSettingsDialog::buildGeneral() {
    auto* p = new SettingsPage(QStringLiteral("general"), tr("General"), QStringLiteral("general"),
                               tr("Project identity and startup behaviour."), this);
    p->addSection(tr("Project"));
    p->addText(tr("Project Name"), tr("Displayed in the editor title bar and used for packaged builds."), projectBinding(m_project, "general.name"));
    p->addText(tr("Version"), tr("Semantic version of the game (written into save game headers)."), projectBinding(m_project, "general.version"));
    p->addText(tr("Company"), {}, projectBinding(m_project, "general.company"));
    p->addText(tr("Description"), {}, projectBinding(m_project, "general.description"));
    p->addSection(tr("Startup"));
    QStringList scenes;
    if (m_project) {
        QDirIterator it(m_project->contentDir(), {"*.oxscene", "*.oxscene.json"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) scenes << QDir(m_project->contentDir()).relativeFilePath(it.next());
    }
    scenes.sort();
    QVariantList vals;
    for (const auto& s : scenes) vals << s;
    p->addCombo(tr("Startup Scene"), tr("Scene the game loads first."), scenes, vals, projectBinding(m_project, "general.startupScene"));
    addPage(p);
}

void ProjectSettingsDialog::buildMaps() {
    auto* p = new SettingsPage(QStringLiteral("maps"), tr("Maps & Modes"), QStringLiteral("map"),
                               tr("Default maps for the editor and the game, and the default game mode."), this);
    p->addSection(tr("Default Maps"));
    p->addText(tr("Editor Startup Map"), tr("Opened when the editor loads this project."), projectBinding(m_project, "maps.editorStartupMap"));
    p->addText(tr("Game Default Map"), tr("Loaded by the packaged game."), projectBinding(m_project, "maps.gameDefaultMap"));
    p->addSection(tr("Game Mode"));
    p->addText(tr("Default Game Mode"), tr("Gameplay rules object spawned for new maps."), projectBinding(m_project, "maps.defaultGameMode"));
    addPage(p);
}

void ProjectSettingsDialog::buildRendering() {
    const RenderingCaps caps = m_ctx->services().caps().caps();
    auto* p = new SettingsPage(QStringLiteral("rendering"), tr("Rendering"), QStringLiteral("rendering"),
                               tr("Renderer features. Changes rebuild the render graph live, no restart needed."), this);
    p->addSection(tr("Hardware Ray Tracing"), tr("Uses VK_KHR_acceleration_structure + VK_KHR_ray_query. Raster fallbacks are used for disabled effects."));
    if (!caps.rayTracingSupported) {
        p->addBanner(tr("<b>Ray tracing is not available on this machine.</b><br/>%1<br/>GPU: %2").arg(caps.rayTracingUnavailableReason.toHtmlEscaped(), caps.gpuName.toHtmlEscaped()),
                     true, QStringLiteral("rtx"));
    }
    ToggleSwitch* rt = p->addToggle(tr("Ray Tracing"), tr("Master switch for all hardware ray traced effects."), cvarBinding(cvars::kRayTracing));
    if (!caps.rayTracingSupported) {
        rt->setEnabled(false);
        rt->setToolTip(caps.rayTracingUnavailableReason);
        rt->parentWidget()->setToolTip(caps.rayTracingUnavailableReason);
    }
    struct Fx {
        const char* cvar;
        QString title, desc;
    };
    const Fx effects[] = {{cvars::kRtShadows, tr("Ray Traced Shadows"), tr("Exact contact-hardening shadows for all light types.")},
                          {cvars::kRtReflections, tr("Ray Traced Reflections"), tr("Off-screen accurate reflections (falls back to SSR + probes).")},
                          {cvars::kRtAO, tr("Ray Traced Ambient Occlusion"), tr("Replaces SSAO.")},
                          {cvars::kRtGI, tr("Ray Traced Global Illumination"), tr("One-bounce diffuse GI with denoising.")},
                          {cvars::kRtTranslucency, tr("Ray Traced Translucency"), tr("Refraction through glass and water.")}};
    for (const Fx& fx : effects) {
        ToggleSwitch* t = p->addToggle(fx.title, fx.desc, cvarBinding(QString::fromLatin1(fx.cvar)));
        p->addRefresher([t, caps] {
            const bool on = caps.rayTracingSupported && cvarBinding(QString::fromLatin1(cvars::kRayTracing)).get().toBool();
            t->setEnabled(on);
            t->setToolTip(caps.rayTracingSupported ? (on ? QString() : tr("Enable Ray Tracing first")) : caps.rayTracingUnavailableReason);
        });
    }

    p->addSection(tr("Anti-Aliasing & Upscaling"));
    p->addCombo(tr("Anti-Aliasing Method"), tr("TAA gives the best quality and is required for temporal upscalers."),
                cvarEnumLabels(cvars::kAntiAliasing), indices(3), cvarBinding(cvars::kAntiAliasing));
    QComboBox* up = p->addCombo(tr("Upscaler"), tr("Render at a lower resolution and reconstruct. FSR 1.0 works on every GPU."),
                                cvarEnumLabels(cvars::kUpscaler), indices(3), cvarBinding(cvars::kUpscaler));
    if (!caps.dlssSupported) {
        disableComboItem(up, 2, caps.dlssUnavailableReason);
        p->addBanner(tr("<b>NVIDIA DLSS unavailable:</b> %1").arg(caps.dlssUnavailableReason.toHtmlEscaped()), false, QStringLiteral("info"));
    }
    QComboBox* q = p->addCombo(tr("Upscaler Quality"), tr("Render scale: Ultra Performance 33%, Performance 50%, Balanced 58%, Quality 67%, DLAA/Native 100%."),
                               cvarEnumLabels(cvars::kUpscalerQuality), indices(5), cvarBinding(cvars::kUpscalerQuality));
    p->addRefresher([q] { q->setEnabled(cvarBinding(QString::fromLatin1(cvars::kUpscaler)).get().toInt() != 0); });
    NumberField* sharp = p->addNumber(tr("Sharpness"), tr("RCAS sharpening for FSR, sharpness for DLSS."), cvarBinding(cvars::kUpscalerSharpness), 0, 1, 0.01, 2);
    p->addRefresher([sharp] { sharp->setEnabled(cvarBinding(QString::fromLatin1(cvars::kUpscaler)).get().toInt() != 0); });

    p->addSection(tr("Tonemapping & Exposure"));
    p->addCombo(tr("Tonemapper"), tr("ACES filmic or AgX (better hue preservation in highlights)."), cvarEnumLabels(cvars::kTonemapper), indices(2),
                cvarBinding(cvars::kTonemapper));
    p->addNumber(tr("Default Exposure"), tr("Exposure compensation in EV applied when no camera overrides it."), cvarBinding(cvars::kExposure), -10, 10, 0.1, 1, QStringLiteral(" EV"));
    p->addToggle(tr("Auto Exposure"), tr("Histogram based eye adaptation."), cvarBinding(cvars::kAutoExposure));

    p->addSection(tr("Lighting"));
    QComboBox* sm = p->addCombo(tr("Shadow Method"), {}, cvarEnumLabels(cvars::kShadowMethod), indices(2), cvarBinding(cvars::kShadowMethod));
    QComboBox* gi = p->addCombo(tr("Global Illumination Method"), {}, cvarEnumLabels(cvars::kGIMethod), indices(4), cvarBinding(cvars::kGIMethod));
    QComboBox* refl = p->addCombo(tr("Reflection Method"), {}, cvarEnumLabels(cvars::kReflectionMethod), indices(3), cvarBinding(cvars::kReflectionMethod));
    if (!caps.rayTracingSupported) {
        disableComboItem(sm, 1, caps.rayTracingUnavailableReason);
        disableComboItem(gi, 3, caps.rayTracingUnavailableReason);
        disableComboItem(refl, 2, caps.rayTracingUnavailableReason);
    }
    p->addSection(tr("Display"));
    p->addToggle(tr("VSync"), {}, cvarBinding(cvars::kVSync));
    p->addNumber(tr("Frame Rate Limit"), tr("0 = unlimited"), cvarBinding(cvars::kMaxFps), 0, 1000, 1, 0, QStringLiteral(" fps"));
    addPage(p);
}

void ProjectSettingsDialog::buildScalability() {
    auto* p = new SettingsPage(QStringLiteral("scalability"), tr("Scalability"), QStringLiteral("speedometer"),
                               tr("Quality presets per feature group (like Unreal's sg.* groups). Changes apply live; Apply stores them as the project default."), this);
    m_scalability = new ScalabilityWidget(m_ctx, false, p);
    m_scalability->setCommit([this](int group, QualityLevel level) {
        SettingBinding b;
        if (group < 0) {
            b.key = QStringLiteral("sg.Overall");
            b.get = [] { return QVariant(int(scalability::overallLevel())); };
            b.set = [](const QVariant& v) {
                if (v.toInt() >= 0) scalability::setOverall(QualityLevel(v.toInt()));
            };
        } else {
            const auto g = Scalability(group);
            b.key = QStringLiteral("sg.") + QString::fromLatin1(scalability::groupName(g).data());
            b.get = [g] { return QVariant(int(scalability::currentLevel(g))); };
            b.set = [g](const QVariant& v) {
                if (v.toInt() >= 0) scalability::setGroup(g, QualityLevel(v.toInt()));
            };
        }
        commit(b, int(level));
    });
    QStringList kw;
    for (usize i = 0; i < kScalabilityGroupCount; ++i) kw << groupDisplayName(Scalability(i));
    kw << QStringLiteral("quality preset low medium high ultra auto detect benchmark");
    p->addFullWidth(m_scalability, kw);
    addPage(p);
}

void ProjectSettingsDialog::buildPhysics() {
    auto* p = new SettingsPage(QStringLiteral("physics"), tr("Physics"), QStringLiteral("physics"), tr("Jolt physics world defaults and collision layers."), this);
    p->addSection(tr("Simulation"));
    auto* grav = new VectorField(3, p);
    grav->setStep(0.1);
    grav->setFixedWidth(260);
    p->addRow(tr("Gravity"), tr("m/s², world space (Y up)."), grav, QStringLiteral("physics.gravity"));
    const SettingBinding gb = projectJsonBinding(m_project, "physics.gravity");
    connect(grav, &VectorField::edited, this, [this, grav, gb](int, EditPhase ph) {
        const auto v = grav->values();
        commit(gb, QString::fromStdString(nlohmann::json::array({v[0], v[1], v[2]}).dump()), ph);
    });
    p->addRefresher([grav, gb] {
        auto j = nlohmann::json::parse(gb.get().toString().toStdString(), nullptr, false);
        if (j.is_array() && j.size() == 3) grav->setValues({j[0].get<double>(), j[1].get<double>(), j[2].get<double>()});
    });
    p->addNumber(tr("Fixed Update Rate"), tr("Physics and gameplay fixed step frequency."), projectBinding(m_project, "physics.fixedRate"), 10, 240, 1, 0, QStringLiteral(" Hz"));
    p->addNumber(tr("Max Sub-steps"), tr("Fixed steps per frame before time is dropped."), projectBinding(m_project, "physics.maxSubsteps"), 1, 16, 1, 0);

    p->addSection(tr("Collision Layers"), tr("Tick a cell to let two layers collide. The matrix is symmetric."));
    auto* host = new QFrame(p);
    host->setProperty("role", "card");
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setSpacing(4);
    const SettingBinding layersB = projectJsonBinding(m_project, "physics.layers");
    const SettingBinding matrixB = projectJsonBinding(m_project, "physics.collisionMatrix");
    auto rebuild = [this, grid, host, layersB, matrixB] {
        while (QLayoutItem* it = grid->takeAt(0)) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        auto layers = nlohmann::json::parse(layersB.get().toString().toStdString(), nullptr, false);
        auto matrix = nlohmann::json::parse(matrixB.get().toString().toStdString(), nullptr, false);
        if (!layers.is_array()) return;
        const int n = int(layers.size());
        auto collides = [&](int a, int b) {
            if (!matrix.is_array() || int(matrix.size()) != n) return true;
            return matrix[size_t(a)].is_array() && int(matrix[size_t(a)].size()) == n ? matrix[size_t(a)][size_t(b)].get<bool>() : true;
        };
        for (int c = 0; c < n; ++c) {
            auto* h = new QLabel(QString::fromStdString(layers[size_t(n - 1 - c)].get<std::string>()), host);
            h->setProperty("role", "dim");
            h->setAlignment(Qt::AlignCenter);
            grid->addWidget(h, 0, c + 1);
        }
        for (int r = 0; r < n; ++r) {
            auto* l = new QLabel(QString::fromStdString(layers[size_t(r)].get<std::string>()), host);
            grid->addWidget(l, r + 1, 0);
            for (int c = 0; c < n - r; ++c) {
                const int b = n - 1 - c;
                auto* box = new QCheckBox(host);
                box->setChecked(collides(r, b));
                box->setToolTip(tr("%1 ↔ %2").arg(QString::fromStdString(layers[size_t(r)].get<std::string>()), QString::fromStdString(layers[size_t(b)].get<std::string>())));
                grid->addWidget(box, r + 1, c + 1, Qt::AlignCenter);
                connect(box, &QCheckBox::clicked, this, [this, matrixB, layersB, r, b](bool on) {
                    auto layersJ = nlohmann::json::parse(layersB.get().toString().toStdString(), nullptr, false);
                    auto m = nlohmann::json::parse(matrixB.get().toString().toStdString(), nullptr, false);
                    const size_t n2 = layersJ.size();
                    if (!m.is_array() || m.size() != n2) {
                        m = nlohmann::json::array();
                        for (size_t i = 0; i < n2; ++i) m.push_back(nlohmann::json(std::vector<bool>(n2, true)));
                    }
                    m[size_t(r)][size_t(b)] = on;
                    m[size_t(b)][size_t(r)] = on;
                    commit(matrixB, QString::fromStdString(m.dump()));
                });
            }
        }
        auto* add = new QPushButton(Icons::get(QStringLiteral("add")), tr("Add Layer"), host);
        grid->addWidget(add, n + 1, 0, 1, std::max(1, n + 1), Qt::AlignLeft);
        connect(add, &QPushButton::clicked, this, [this, layersB, matrixB] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Add Collision Layer"), tr("Layer name:"), QLineEdit::Normal, {}, &ok);
            if (!ok || name.isEmpty()) return;
            auto layersJ = nlohmann::json::parse(layersB.get().toString().toStdString(), nullptr, false);
            layersJ.push_back(name.toStdString());
            commit(layersB, QString::fromStdString(layersJ.dump()));
            commit(matrixB, QStringLiteral("[]"));
        });
    };
    p->addFullWidth(host, {tr("collision"), tr("layers"), tr("matrix")});
    p->addRefresher(rebuild);
    addPage(p);
}

void ProjectSettingsDialog::buildAudio() {
    auto* p = new SettingsPage(QStringLiteral("audio"), tr("Audio"), QStringLiteral("audio"), tr("Mixer buses and device defaults."), this);
    p->addSection(tr("Buses"), tr("Default volume of each mixer bus."));
    const SettingBinding busB = projectJsonBinding(m_project, "audio.buses");
    auto buses = m_project ? m_project->setting("audio.buses") : nlohmann::json::object();
    for (auto it = buses.begin(); it != buses.end(); ++it) {
        const std::string name = it.key();
        auto* host = new QWidget(p);
        auto* l = new QHBoxLayout(host);
        l->setContentsMargins(0, 0, 0, 0);
        auto* slider = new QSlider(Qt::Horizontal, host);
        slider->setRange(0, 100);
        slider->setFixedWidth(200);
        auto* val = new QLabel(host);
        val->setFixedWidth(44);
        val->setProperty("role", "dim");
        l->addWidget(slider);
        l->addWidget(val);
        p->addRow(QString::fromStdString(name), {}, host, QStringLiteral("audio.buses.") + QString::fromStdString(name));
        connect(slider, &QSlider::valueChanged, this, [this, busB, name, val, slider](int v) {
            val->setText(QStringLiteral("%1%").arg(v));
            auto j = nlohmann::json::parse(busB.get().toString().toStdString(), nullptr, false);
            if (!j.is_object() || std::abs(j.value(name, 1.0) - v / 100.0) < 1e-6) return;
            j[name] = v / 100.0;
            commit(busB, QString::fromStdString(j.dump()), slider->isSliderDown() ? EditPhase::Update : EditPhase::Single);
        });
        connect(slider, &QSlider::sliderReleased, this, [this, busB] { commit(busB, busB.get(), EditPhase::End); });
        p->addRefresher([slider, val, busB, name] {
            auto j = nlohmann::json::parse(busB.get().toString().toStdString(), nullptr, false);
            const int v = int(std::lround((j.is_object() ? j.value(name, 1.0) : 1.0) * 100));
            QSignalBlocker b(slider);
            slider->setValue(v);
            val->setText(QStringLiteral("%1%").arg(v));
        });
    }
    p->addSection(tr("Device"));
    p->addCombo(tr("Sample Rate"), {}, {QStringLiteral("44100 Hz"), QStringLiteral("48000 Hz"), QStringLiteral("96000 Hz")}, {44100, 48000, 96000},
                projectBinding(m_project, "audio.sampleRate"));
    p->addNumber(tr("Max Voices"), tr("Simultaneously playing sounds before virtualisation."), projectBinding(m_project, "audio.maxVoices"), 8, 512, 1, 0);
    addPage(p);
}

void ProjectSettingsDialog::buildInput() {
    auto* p = new SettingsPage(QStringLiteral("input"), tr("Input"), QStringLiteral("gamepad"), tr("Action mappings used by gameplay code (InputSystem)."), this);
    p->addSection(tr("Action Mappings"), tr("Click a key slot and press the new key. Each action may have several keys."));
    const SettingBinding actB = projectJsonBinding(m_project, "input.actions");
    auto* host = new QFrame(p);
    host->setProperty("role", "card");
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(12, 10, 12, 10);
    grid->setHorizontalSpacing(10);
    auto rebuild = [this, grid, host, actB] {
        while (QLayoutItem* it = grid->takeAt(0)) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        auto actions = nlohmann::json::parse(actB.get().toString().toStdString(), nullptr, false);
        if (!actions.is_array()) actions = nlohmann::json::array();
        grid->addWidget(makeSectionLabel(tr("Action"), host), 0, 0);
        grid->addWidget(makeSectionLabel(tr("Primary"), host), 0, 1);
        grid->addWidget(makeSectionLabel(tr("Secondary"), host), 0, 2);
        for (size_t i = 0; i < actions.size(); ++i) {
            const int row = int(i) + 1;
            auto* name = new QLabel(QString::fromStdString(actions[i].value("name", "")), host);
            QFont f = name->font();
            f.setWeight(QFont::Medium);
            name->setFont(f);
            grid->addWidget(name, row, 0);
            const auto keys = actions[i].value("keys", nlohmann::json::array());
            for (int k = 0; k < 2; ++k) {
                auto* edit = new QKeySequenceEdit(host);
                edit->setMaximumSequenceLength(1);
                edit->setClearButtonEnabled(true);
                if (size_t(k) < keys.size()) edit->setKeySequence(QKeySequence::fromString(QString::fromStdString(keys[size_t(k)].get<std::string>())));
                grid->addWidget(edit, row, 1 + k);
                connect(edit, &QKeySequenceEdit::editingFinished, this, [this, actB, i, k, edit] {
                    auto a = nlohmann::json::parse(actB.get().toString().toStdString(), nullptr, false);
                    if (!a.is_array() || i >= a.size()) return;
                    auto ks = a[i].value("keys", nlohmann::json::array());
                    while (ks.size() < 2) ks.push_back("");
                    ks[size_t(k)] = edit->keySequence().toString(QKeySequence::PortableText).toStdString();
                    nlohmann::json clean = nlohmann::json::array();
                    for (auto& s : ks) {
                        if (!s.get<std::string>().empty()) clean.push_back(s);
                    }
                    a[i]["keys"] = clean;
                    commit(actB, QString::fromStdString(a.dump()));
                });
            }
            auto* rm = makeToolButton(QStringLiteral("trash"), tr("Remove action"), host);
            grid->addWidget(rm, row, 3);
            connect(rm, &QToolButton::clicked, this, [this, actB, i] {
                auto a = nlohmann::json::parse(actB.get().toString().toStdString(), nullptr, false);
                if (a.is_array() && i < a.size()) a.erase(a.begin() + long(i));
                commit(actB, QString::fromStdString(a.dump()));
            });
        }
        auto* add = new QPushButton(Icons::get(QStringLiteral("add")), tr("Add Action"), host);
        grid->addWidget(add, int(actions.size()) + 1, 0, 1, 2, Qt::AlignLeft);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(2, 1);
        connect(add, &QPushButton::clicked, this, [this, actB] {
            bool ok = false;
            const QString n = QInputDialog::getText(this, tr("Add Action"), tr("Action name:"), QLineEdit::Normal, {}, &ok);
            if (!ok || n.isEmpty()) return;
            auto a = nlohmann::json::parse(actB.get().toString().toStdString(), nullptr, false);
            if (!a.is_array()) a = nlohmann::json::array();
            a.push_back({{"name", n.toStdString()}, {"keys", nlohmann::json::array()}});
            commit(actB, QString::fromStdString(a.dump()));
        });
    };
    p->addFullWidth(host, {tr("action"), tr("mapping"), tr("key"), tr("binding")});
    p->addRefresher(rebuild);
    addPage(p);
}

void ProjectSettingsDialog::buildNetworking() {
    auto* p = new SettingsPage(QStringLiteral("network"), tr("Networking"), QStringLiteral("network"), tr("Client/server transport (ENet) and replication."), this);
    p->addSection(tr("Server"));
    p->addNumber(tr("Port"), tr("UDP port of dedicated and listen servers."), projectBinding(m_project, "network.port"), 1024, 65535, 1, 0);
    p->addNumber(tr("Max Clients"), {}, projectBinding(m_project, "network.maxClients"), 1, 256, 1, 0);
    p->addSection(tr("Replication"));
    p->addNumber(tr("Tick Rate"), tr("Snapshots sent per second."), projectBinding(m_project, "network.tickRate"), 5, 128, 1, 0, QStringLiteral(" Hz"));
    p->addNumber(tr("Interpolation Delay"), tr("Remote entities are rendered this far in the past."), projectBinding(m_project, "network.interpolationDelayMs"), 0, 500, 1, 0, QStringLiteral(" ms"));
    addPage(p);
}

void ProjectSettingsDialog::buildScripting() {
    auto* p = new SettingsPage(QStringLiteral("scripting"), tr("Scripting"), QStringLiteral("script"), tr("Lua 5.4 virtual machine."), this);
    p->addSection(tr("Lua"));
    p->addToggle(tr("Hot Reload"), tr("Reload changed scripts while playing."), projectBinding(m_project, "scripting.hotReload"));
    p->addToggle(tr("Sandbox"), tr("Restrict io/os libraries for user scripts."), projectBinding(m_project, "scripting.sandbox"));
    addPage(p);
}

void ProjectSettingsDialog::buildPackaging() {
    auto* p = new SettingsPage(QStringLiteral("packaging"), tr("Packaging"), QStringLiteral("package"), tr("How the game is built for distribution."), this);
    p->addSection(tr("Build"));
    p->addCombo(tr("Build Configuration"), {}, {tr("Development"), tr("Shipping")}, {QStringLiteral("Development"), QStringLiteral("Shipping")},
                projectBinding(m_project, "packaging.buildConfiguration"));
    p->addPath(tr("Output Directory"), tr("Relative to the project root."), projectBinding(m_project, "packaging.outputDir"), true);
    p->addToggle(tr("Compress Pak Files"), tr("Smaller downloads, slightly slower loading."), projectBinding(m_project, "packaging.compressPak"));
    p->addToggle(tr("Include Debug Files"), tr("Ship symbols for crash reports."), projectBinding(m_project, "packaging.includeDebugFiles"));
    addPage(p);
}

} // namespace ox::editor
