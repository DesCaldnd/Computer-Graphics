#include "settings/project_settings_dialog.hpp"

#include "core/editor_context.hpp"
#include "integration/input_bridge.hpp"
#include "integration/runtime_settings.hpp"
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
#include <QFileInfo>
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

// Project binding with a default for missing keys (module toggles: missing = enabled).
SettingBinding projectBindingDefault(Project* project, const QString& path, const nlohmann::json& def) {
    SettingBinding b = projectBinding(project, path);
    const std::string p = path.toStdString();
    b.get = [project, p, def]() -> QVariant {
        if (!project) return {};
        const nlohmann::json v = project->setting(p, def);
        if (v.is_boolean()) return v.get<bool>();
        if (v.is_number_integer()) return int(v.get<i64>());
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) return QString::fromStdString(v.get<std::string>());
        return {};
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
    if (auto st = m_project->save(); !st) {
        Q_EMIT m_ctx->statusMessage(QString::fromStdString(st.error().message), 4000);
    } else {
        Q_EMIT m_ctx->statusMessage(tr("Project settings saved to %1").arg(QFileInfo(m_project->projectFile()).fileName()), 2500);
        applyProjectSettingsLive(*m_ctx);
    }
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
    p->addText(tr("Project Name"), tr("Displayed in the editor title bar and used for packaged builds."), projectBinding(m_project, "name"));
    p->addText(tr("Version"), tr("Semantic version of the game (written into save game headers)."), projectBinding(m_project, "version"));
    p->addText(tr("Company"), {}, projectBinding(m_project, "company"));
    p->addText(tr("Description"), {}, projectBinding(m_project, "editor.description"));
    p->addNumber(tr("Save Game Version"), tr("Current save data version; older saves are upgraded by registered migrations."),
                 projectBinding(m_project, "saveVersion"), 1, 100000, 1, 0);
    p->addSection(tr("Startup"));
    QStringList scenes;
    QVariantList vals;
    if (m_project) {
        QDirIterator it(m_project->contentDir(), {"*.oxscene", "*.oxscene.json"}, QDir::Files, QDirIterator::Subdirectories);
        QStringList files;
        while (it.hasNext()) files << it.next();
        files.sort();
        for (const QString& f : files) {
            scenes << QDir(m_project->contentDir()).relativeFilePath(f);
            vals << m_project->uriForPath(f);
        }
    }
    p->addCombo(tr("Startup Scene"), tr("Scene the game loads first (project:// URI in the .oxproj)."), scenes, vals, projectBinding(m_project, "startupScene"));
    p->addSection(tr("Engine Modules"), tr("Built-in runtime modules started by Engine::init. Changes apply when the project is reopened."));
    struct M {
        const char* key;
        QString title, desc;
    };
    for (const M& m : {M{"assets", tr("Asset Database"), tr("AssetRegistry/AssetManager over the Assets folder (pak files when cooked).")},
                       M{"physics", tr("Physics"), tr("Jolt physics world.")},
                       M{"audio", tr("Audio"), tr("miniaudio engine with buses.")},
                       M{"script", tr("Scripting"), tr("Lua 5.4 VM and the Lua await bridge.")},
                       M{"async", tr("Coroutines"), tr("C++20 coroutine scheduler for gameplay code.")},
                       M{"ai", tr("AI"), tr("Navigation meshes, crowds, behaviour trees, perception.")},
                       M{"net", tr("Networking"), tr("Replication and client prediction.")},
                       M{"world", tr("World"), tr("Terrain, vegetation, sky, time of day, water, streaming.")},
                       M{"gameplay", tr("Gameplay"), tr("ECS components and systems of all modules.")}}) {
        p->addToggle(m.title, m.desc, projectBindingDefault(m_project, QStringLiteral("modules.") + QString::fromLatin1(m.key), true));
    }
    addPage(p);
}

void ProjectSettingsDialog::buildMaps() {
    auto* p = new SettingsPage(QStringLiteral("maps"), tr("Maps & Modes"), QStringLiteral("map"),
                               tr("Default maps for the editor and the game, and the default game mode."), this);
    p->addSection(tr("Default Maps"));
    p->addText(tr("Editor Startup Map"), tr("Opened when the editor loads this project (path inside Assets/)."), projectBinding(m_project, "editor.maps.editorStartupMap"));
    p->addText(tr("Game Default Map"), tr("Loaded by the packaged game."), projectBinding(m_project, "editor.maps.gameDefaultMap"));
    p->addSection(tr("Game Mode"));
    p->addText(tr("Default Game Mode"), tr("Gameplay rules object spawned for new maps."), projectBinding(m_project, "editor.maps.defaultGameMode"));
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
                {tr("None"), tr("FXAA"), tr("Temporal AA (TAA)")}, indices(3), cvarBinding(cvars::kAntiAliasing)); // plain int cvar
    const QStringList upscalerLabels = cvarEnumLabels(cvars::kUpscaler);
    QComboBox* up = p->addCombo(tr("Upscaler"), tr("Render at a lower resolution and reconstruct. FSR 1.0 and TAAU work on every GPU, DLSS needs NVIDIA RTX."),
                                upscalerLabels, indices(int(upscalerLabels.size())), cvarBinding(cvars::kUpscaler));
    for (usize i = 0; i < caps.upscalers.size() && int(i) < up->count(); ++i) {
        const UpscalerInfo& u = caps.upscalers[i];
        if (!u.name.isEmpty()) up->setItemData(int(i), u.temporal ? tr("%1 (temporal)").arg(u.name) : u.name, Qt::ToolTipRole);
        if (!u.available) disableComboItem(up, int(i), u.reason);
    }
    if (!caps.dlssSupported) {
        p->addBanner(tr("<b>NVIDIA DLSS unavailable:</b> %1").arg(caps.dlssUnavailableReason.toHtmlEscaped()), false, QStringLiteral("info"));
    }
    QComboBox* q = p->addCombo(tr("Upscaler Quality"), tr("Render scale: Ultra Performance 33%, Performance 50%, Balanced 58%, Quality 67%, DLAA/Native 100%."),
                               cvarEnumLabels(cvars::kUpscalerQuality), indices(5), cvarBinding(cvars::kUpscalerQuality));
    p->addRefresher([q] { q->setEnabled(cvarBinding(QString::fromLatin1(cvars::kUpscaler)).get().toInt() != 0); });
    NumberField* sharp = p->addNumber(tr("Sharpness"), tr("RCAS sharpening for FSR, sharpness for DLSS."), cvarBinding(cvars::kUpscalerSharpness), 0, 1, 0.01, 2);
    p->addRefresher([sharp] { sharp->setEnabled(cvarBinding(QString::fromLatin1(cvars::kUpscaler)).get().toInt() != 0); });

    p->addSection(tr("Tonemapping & Exposure"));
    const QStringList tonemappers = cvarEnumLabels(cvars::kTonemapper);
    p->addCombo(tr("Tonemapper"), tr("ACES filmic or AgX (better hue preservation in highlights)."), tonemappers,
                indices(int(tonemappers.size())), cvarBinding(cvars::kTonemapper));
    p->addNumber(tr("Default Exposure"), tr("Exposure compensation in EV applied when no camera overrides it."), cvarBinding(cvars::kExposure), -10, 10, 0.1, 1, QStringLiteral(" EV"));
    p->addToggle(tr("Auto Exposure"), tr("Histogram based eye adaptation."), cvarBinding(cvars::kAutoExposure));

    // Raster techniques; with Ray Tracing on, the ray traced effects above replace them where enabled.
    p->addSection(tr("Lighting"));
    p->addToggle(tr("Shadow Maps"), tr("Cascaded/atlas shadow maps (replaced by ray traced shadows when those are on)."),
                 cvarBinding(cvars::kShadows));
    p->addCombo(tr("Ambient Occlusion"), tr("Screen-space AO technique (scalability group Global Illumination)."),
                {tr("Off"), tr("SSAO"), tr("GTAO")}, indices(3), cvarBinding(cvars::kAOMethod));
    p->addToggle(tr("Screen Space Reflections"), tr("Hi-Z traced reflections on top of reflection probes."), cvarBinding(cvars::kSSR));
    p->addToggle(tr("Irradiance Volumes"), tr("Baked probe grids for indirect diffuse lighting."), cvarBinding(cvars::kIrradianceVolumes));
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
    p->addNumber(tr("Max Bodies"), tr("Capacity of the Jolt physics world."), projectBinding(m_project, "physics.maxBodies"), 1024, 1048576, 1024, 0);

    p->addSection(tr("Collision Layers"), tr("Tick a cell to let two layers collide. The matrix is symmetric."));
    auto* host = new QFrame(p);
    host->setProperty("role", "card");
    auto* grid = new QGridLayout(host);
    grid->setContentsMargins(12, 12, 12, 12);
    grid->setSpacing(4);
    const SettingBinding layersB = projectJsonBinding(m_project, "editor.physics.layers");
    const SettingBinding matrixB = projectJsonBinding(m_project, "editor.physics.collisionMatrix");
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
    const SettingBinding busB = projectJsonBinding(m_project, "audio.busVolumes");
    auto buses = m_project ? m_project->setting("audio.busVolumes") : nlohmann::json::object();
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
    auto* p = new SettingsPage(QStringLiteral("input"), tr("Input"), QStringLiteral("gamepad"),
                               tr("Action mappings of the runtime InputSystem (Enhanced-Input style): actions, and the bindings of the default context."), this);
    const SettingBinding inputB = projectJsonBinding(m_project, "input");
    auto load = [inputB] {
        auto j = nlohmann::json::parse(inputB.get().toString().toStdString(), nullptr, false);
        if (!j.is_object()) j = nlohmann::json::object();
        if (!j.contains("actions") || !j["actions"].is_array()) j["actions"] = nlohmann::json::array();
        if (!j.contains("contexts") || !j["contexts"].is_array() || j["contexts"].empty()) {
            j["contexts"] = nlohmann::json::array({{{"name", "Default"}, {"priority", 0}, {"bindings", nlohmann::json::array()}}});
        }
        if (!j["contexts"][0].contains("bindings") || !j["contexts"][0]["bindings"].is_array()) j["contexts"][0]["bindings"] = nlohmann::json::array();
        if (!j.contains("activeContexts") || !j["activeContexts"].is_array() || j["activeContexts"].empty()) {
            j["activeContexts"] = nlohmann::json::array({j["contexts"][0].value("name", std::string("Default"))});
        }
        return j;
    };
    auto store = [this, inputB](const nlohmann::json& j) { commit(inputB, QString::fromStdString(j.dump())); };
    const QStringList types = {QStringLiteral("Bool"), QStringLiteral("Axis1D"), QStringLiteral("Axis2D"), QStringLiteral("Axis3D")};

    // ---- actions ----
    p->addSection(tr("Actions"), tr("Named inputs read by gameplay code: input.action(\"Jump\"), InputSystem::axis2D(\"Move\")."));
    auto* actHost = new QFrame(p);
    actHost->setProperty("role", "card");
    auto* actGrid = new QGridLayout(actHost);
    actGrid->setContentsMargins(12, 10, 12, 10);
    actGrid->setHorizontalSpacing(10);
    auto rebuildActions = [this, actGrid, actHost, load, store, types] {
        while (QLayoutItem* it = actGrid->takeAt(0)) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        const nlohmann::json j = load();
        actGrid->addWidget(makeSectionLabel(tr("Action"), actHost), 0, 0);
        actGrid->addWidget(makeSectionLabel(tr("Value"), actHost), 0, 1);
        actGrid->addWidget(makeSectionLabel(tr("Description"), actHost), 0, 2);
        const auto& actions = j["actions"];
        for (size_t i = 0; i < actions.size(); ++i) {
            const int row = int(i) + 1;
            auto* name = new QLabel(QString::fromStdString(actions[i].value("name", "")), actHost);
            QFont f = name->font();
            f.setWeight(QFont::Medium);
            name->setFont(f);
            actGrid->addWidget(name, row, 0);
            auto* type = new QComboBox(actHost);
            type->addItems(types);
            type->setCurrentText(QString::fromStdString(actions[i].value("type", "Bool")));
            actGrid->addWidget(type, row, 1);
            connect(type, &QComboBox::currentTextChanged, this, [load, store, i](const QString& t) {
                auto jj = load();
                if (i < jj["actions"].size()) jj["actions"][i]["type"] = t.toStdString();
                store(jj);
            });
            auto* desc = new QLineEdit(QString::fromStdString(actions[i].value("description", "")), actHost);
            actGrid->addWidget(desc, row, 2);
            connect(desc, &QLineEdit::editingFinished, this, [load, store, i, desc] {
                auto jj = load();
                if (i < jj["actions"].size() && jj["actions"][i].value("description", "") != desc->text().toStdString()) {
                    jj["actions"][i]["description"] = desc->text().toStdString();
                    store(jj);
                }
            });
            auto* rm = makeToolButton(QStringLiteral("trash"), tr("Remove action and its bindings"), actHost);
            actGrid->addWidget(rm, row, 3);
            connect(rm, &QToolButton::clicked, this, [load, store, i] {
                auto jj = load();
                if (i >= jj["actions"].size()) return;
                const std::string n = jj["actions"][i].value("name", "");
                jj["actions"].erase(jj["actions"].begin() + long(i));
                for (auto& ctx : jj["contexts"]) {
                    auto& b = ctx["bindings"];
                    for (size_t k = b.size(); k-- > 0;) {
                        if (b[k].value("action", "") == n) b.erase(b.begin() + long(k));
                    }
                }
                store(jj);
            });
        }
        auto* add = new QPushButton(Icons::get(QStringLiteral("add")), tr("Add Action"), actHost);
        actGrid->addWidget(add, int(actions.size()) + 1, 0, 1, 2, Qt::AlignLeft);
        actGrid->setColumnStretch(2, 1);
        connect(add, &QPushButton::clicked, this, [this, load, store] {
            bool ok = false;
            const QString n = QInputDialog::getText(this, tr("Add Action"), tr("Action name:"), QLineEdit::Normal, {}, &ok);
            if (!ok || n.trimmed().isEmpty()) return;
            auto jj = load();
            jj["actions"].push_back({{"name", n.trimmed().toStdString()}, {"type", "Bool"}});
            store(jj);
        });
    };
    p->addFullWidth(actHost, {tr("action"), tr("axis"), tr("input")});
    p->addRefresher(rebuildActions);

    // ---- bindings of the first context ----
    p->addSection(tr("Bindings"), tr("Sources: Key.<Name>, Mouse.Left/Right/Middle/X/Y/XY/Wheel, Gamepad.A/B/.../LeftStick/RightStick/LeftTrigger. "
                                      "Click the key field and press a key to bind it. Modifiers/triggers are kept (edit the .oxproj for advanced setups)."));
    auto* bindHost = new QFrame(p);
    bindHost->setProperty("role", "card");
    auto* bindGrid = new QGridLayout(bindHost);
    bindGrid->setContentsMargins(12, 10, 12, 10);
    bindGrid->setHorizontalSpacing(8);
    auto rebuildBindings = [this, bindGrid, bindHost, load, store] {
        while (QLayoutItem* it = bindGrid->takeAt(0)) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        const nlohmann::json j = load();
        QStringList actionNames;
        for (const auto& a : j["actions"]) actionNames << QString::fromStdString(a.value("name", ""));
        const auto& bindings = j["contexts"][0]["bindings"];
        bindGrid->addWidget(makeSectionLabel(tr("Action"), bindHost), 0, 0);
        bindGrid->addWidget(makeSectionLabel(tr("Source"), bindHost), 0, 1);
        bindGrid->addWidget(makeSectionLabel(tr("Key"), bindHost), 0, 2);
        bindGrid->addWidget(makeSectionLabel(tr("Modifiers · Triggers"), bindHost), 0, 3);
        for (size_t i = 0; i < bindings.size(); ++i) {
            const int row = int(i) + 1;
            auto* action = new QComboBox(bindHost);
            action->addItems(actionNames);
            action->setCurrentText(QString::fromStdString(bindings[i].value("action", "")));
            bindGrid->addWidget(action, row, 0);
            connect(action, &QComboBox::currentTextChanged, this, [load, store, i](const QString& t) {
                auto jj = load();
                auto& b = jj["contexts"][0]["bindings"];
                if (i < b.size()) b[i]["action"] = t.toStdString();
                store(jj);
            });
            auto* source = new QLineEdit(QString::fromStdString(bindings[i].value("source", "")), bindHost);
            source->setObjectName(QStringLiteral("binding.source.%1").arg(i));
            bindGrid->addWidget(source, row, 1);
            auto setSource = [load, store, i](const QString& src) {
                auto jj = load();
                auto& b = jj["contexts"][0]["bindings"];
                if (i < b.size() && b[i].value("source", "") != src.toStdString()) {
                    b[i]["source"] = src.toStdString();
                    store(jj);
                }
            };
            connect(source, &QLineEdit::editingFinished, this, [source, setSource] { setSource(source->text().trimmed()); });
            auto* key = new QKeySequenceEdit(bindHost);
            key->setMaximumSequenceLength(1);
            key->setToolTip(tr("Press a key to bind Key.<Name>"));
            bindGrid->addWidget(key, row, 2);
            connect(key, &QKeySequenceEdit::editingFinished, this, [key, setSource] {
                if (key->keySequence().isEmpty()) return;
                const QString src = keySourceForQtKey(key->keySequence()[0].key(), key->keySequence()[0].keyboardModifiers());
                if (!src.isEmpty()) setSource(src);
            });
            QStringList mods;
            for (const auto& m : bindings[i].value("modifiers", nlohmann::json::array())) mods << QString::fromStdString(m.value("type", "?"));
            QStringList trig;
            for (const auto& t : bindings[i].value("triggers", nlohmann::json::array())) trig << QString::fromStdString(t.value("type", "?"));
            auto* info = new QLabel(QStringLiteral("%1 · %2").arg(mods.isEmpty() ? QStringLiteral("—") : mods.join(QStringLiteral(", ")),
                                                                  trig.isEmpty() ? tr("Down") : trig.join(QStringLiteral(", "))),
                                    bindHost);
            info->setProperty("role", "dim");
            bindGrid->addWidget(info, row, 3);
            auto* rm = makeToolButton(QStringLiteral("trash"), tr("Remove binding"), bindHost);
            bindGrid->addWidget(rm, row, 4);
            connect(rm, &QToolButton::clicked, this, [load, store, i] {
                auto jj = load();
                auto& b = jj["contexts"][0]["bindings"];
                if (i < b.size()) b.erase(b.begin() + long(i));
                store(jj);
            });
        }
        auto* add = new QPushButton(Icons::get(QStringLiteral("add")), tr("Add Binding"), bindHost);
        add->setEnabled(!actionNames.isEmpty());
        bindGrid->addWidget(add, int(bindings.size()) + 1, 0, 1, 2, Qt::AlignLeft);
        bindGrid->setColumnStretch(1, 1);
        bindGrid->setColumnStretch(3, 1);
        connect(add, &QPushButton::clicked, this, [load, store, actionNames] {
            auto jj = load();
            jj["contexts"][0]["bindings"].push_back({{"action", actionNames.value(0).toStdString()}, {"source", "Key.Space"}});
            store(jj);
        });
    };
    p->addFullWidth(bindHost, {tr("binding"), tr("key"), tr("mouse"), tr("gamepad"), tr("mapping")});
    p->addRefresher(rebuildBindings);
    addPage(p);
}

void ProjectSettingsDialog::buildNetworking() {
    auto* p = new SettingsPage(QStringLiteral("network"), tr("Networking"), QStringLiteral("network"), tr("Client/server transport (ENet) and replication."), this);
    p->addSection(tr("Server"));
    p->addNumber(tr("Port"), tr("UDP port of dedicated and listen servers."), projectBinding(m_project, "editor.network.port"), 1024, 65535, 1, 0);
    p->addNumber(tr("Max Clients"), {}, projectBinding(m_project, "editor.network.maxClients"), 1, 256, 1, 0);
    p->addSection(tr("Replication"));
    p->addNumber(tr("Tick Rate"), tr("Snapshots sent per second."), projectBinding(m_project, "editor.network.tickRate"), 5, 128, 1, 0, QStringLiteral(" Hz"));
    p->addNumber(tr("Interpolation Delay"), tr("Remote entities are rendered this far in the past."), projectBinding(m_project, "editor.network.interpolationDelayMs"), 0, 500, 1, 0, QStringLiteral(" ms"));
    addPage(p);
}

void ProjectSettingsDialog::buildScripting() {
    auto* p = new SettingsPage(QStringLiteral("scripting"), tr("Scripting"), QStringLiteral("script"), tr("Lua 5.4 virtual machine."), this);
    p->addSection(tr("Lua"));
    p->addToggle(tr("Hot Reload"), tr("Reload changed scripts while playing."), projectBinding(m_project, "editor.scripting.hotReload"));
    p->addToggle(tr("Sandbox"), tr("Restrict io/os libraries for user scripts."), projectBinding(m_project, "editor.scripting.sandbox"));
    addPage(p);
}

void ProjectSettingsDialog::buildPackaging() {
    auto* p = new SettingsPage(QStringLiteral("packaging"), tr("Packaging"), QStringLiteral("package"), tr("How the game is built for distribution."), this);
    p->addSection(tr("Build"));
    p->addCombo(tr("Build Configuration"), {}, {tr("Development"), tr("Shipping")}, {QStringLiteral("Development"), QStringLiteral("Shipping")},
                projectBinding(m_project, "editor.packaging.buildConfiguration"));
    p->addPath(tr("Output Directory"), tr("Relative to the project root."), projectBinding(m_project, "packaging.outputDir"), true);
    p->addToggle(tr("Compress Pak Files"), tr("zstd per entry when it saves more than 1/16 (textures stay uncompressed for mip streaming)."),
                 projectBinding(m_project, "packaging.compress"));
    p->addToggle(tr("Include Debug Files"), tr("Ship symbols for crash reports."), projectBinding(m_project, "editor.packaging.includeDebugFiles"));
    p->addSection(tr("Target Platforms"));
    for (const char* plat : {"macos", "windows", "linux"}) {
        const std::string name = plat;
        SettingBinding b;
        b.key = QStringLiteral("project:packaging.targetPlatforms.") + QString::fromLatin1(plat);
        Project* project = m_project;
        b.get = [project, name]() -> QVariant {
            if (!project) return false;
            const auto list = project->setting("packaging.targetPlatforms", nlohmann::json::array());
            return list.is_array() && std::find(list.begin(), list.end(), name) != list.end();
        };
        b.set = [project, name](const QVariant& v) {
            if (!project) return;
            auto list = project->setting("packaging.targetPlatforms", nlohmann::json::array());
            if (!list.is_array()) list = nlohmann::json::array();
            auto it = std::find(list.begin(), list.end(), name);
            if (v.toBool() && it == list.end()) list.push_back(name);
            if (!v.toBool() && it != list.end()) list.erase(it);
            project->setSetting("packaging.targetPlatforms", list);
        };
        p->addToggle(QString::fromLatin1(plat == std::string("macos") ? "macOS" : plat == std::string("windows") ? "Windows" : "Linux"), {}, b);
    }
    auto* always = new QLineEdit(p);
    always->setPlaceholderText(tr("Assets cooked even when unreferenced, comma separated (UI/, Audio/Music/*.ogg)"));
    p->addRow(tr("Always Include"), tr("URIs or globs added to every pak."), always, QStringLiteral("packaging.alwaysIncludeAssets"));
    const SettingBinding alwaysB = projectJsonBinding(m_project, "packaging.alwaysIncludeAssets");
    connect(always, &QLineEdit::editingFinished, this, [this, always, alwaysB] {
        nlohmann::json list = nlohmann::json::array();
        for (const QString& part : always->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) list.push_back(part.trimmed().toStdString());
        commit(alwaysB, QString::fromStdString(list.dump()));
    });
    p->addRefresher([always, alwaysB] {
        auto j = nlohmann::json::parse(alwaysB.get().toString().toStdString(), nullptr, false);
        QStringList parts;
        if (j.is_array()) {
            for (const auto& e : j) {
                if (e.is_string()) parts << QString::fromStdString(e.get<std::string>());
            }
        }
        if (!always->hasFocus()) always->setText(parts.join(QStringLiteral(", ")));
    });
    addPage(p);
}

} // namespace ox::editor
