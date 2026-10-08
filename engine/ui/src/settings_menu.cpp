#include "settings_menu.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/runtime/settings.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Variant.h>

#include <charconv>

namespace ox::ui {

namespace {

std::string resolutionText(glm::ivec2 r) { return r.x > 0 && r.y > 0 ? std::format("{}x{}", r.x, r.y) : "Native"; }

glm::ivec2 parseResolution(std::string_view s) {
    const auto x = s.find('x');
    if (x == std::string_view::npos) return {0, 0};
    int w = 0, h = 0;
    std::from_chars(s.data(), s.data() + x, w);
    std::from_chars(s.data() + x + 1, s.data() + s.size(), h);
    return {w, h};
}

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

} // namespace

GameUI::SettingsModel::SettingsModel(GameUI& u, Settings& s, SettingsMenuHooks h) : ui(u), settings(s), hooks(std::move(h)) {}

GameUI::SettingsModel::~SettingsModel() { ui.removeModel("settings"); }

void GameUI::SettingsModel::changed(const char* variable) {
    if (handle) handle.DirtyVariable(variable);
    if (hooks.saveOnChange) {
        if (auto st = settings.save(); !st) OX_LOG_WARN("ui", "saving settings failed: {}", st.error().message);
    }
}

void GameUI::SettingsModel::refreshAvailability() {
    std::string reason;
    const bool rt = hooks.rayTracingAvailable ? hooks.rayTracingAvailable(reason) : false;
    if (!hooks.rayTracingAvailable) reason = "Renderer not available";
    std::vector<UpscalerRow> rows;
    if (hooks.upscalers) {
        for (const UpscalerAvailability& a : hooks.upscalers()) rows.push_back({a.name, a.available, a.reason});
    } else {
        rows = {{"Off", true, {}}, {"FSR1", true, {}}, {"DLSS", false, "Renderer not available"}};
    }
    const bool rowsChanged = rows.size() != upscalers.size() ||
                             !std::equal(rows.begin(), rows.end(), upscalers.begin(), [](const UpscalerRow& a, const UpscalerRow& b) {
                                 return a.name == b.name && a.available == b.available && a.reason == b.reason;
                             });
    if (rt != rtAvailable || reason != rtReason) {
        rtAvailable = rt;
        rtReason = reason;
        if (handle) {
            handle.DirtyVariable("rtAvailable");
            handle.DirtyVariable("rtReason");
            handle.DirtyVariable("rayTracing");
        }
    }
    if (rowsChanged) {
        upscalers = std::move(rows);
        if (handle) {
            handle.DirtyVariable("upscalers");
            handle.DirtyVariable("upscalerReason");
        }
    }
}

void GameUI::SettingsModel::setQuality(const std::string& name) {
    if (name == "Auto") {
        autoSelected = true;
        if (hooks.requestAutoDetect) {
            autoStatus = "Detecting…";
            hooks.requestAutoDetect();
        } else {
            autoStatus = "Auto-detect unavailable without a renderer";
        }
        changed("quality");
        changed("autoStatus");
        return;
    }
    if (!scalability::levelFromName(name)) {
        OX_LOG_WARN("ui", "unknown quality level '{}'", name);
        return;
    }
    autoSelected = false;
    autoStatus.clear();
    GraphicsSettings g = settings.user().graphics;
    g.quality = name;
    g.groups.clear();
    settings.setGraphics(g);
    status = "Quality: " + name;
    changed("quality");
    changed("autoStatus");
    changed("status");
}

void GameUI::SettingsModel::setUpscaler(const std::string& name) {
    for (const UpscalerRow& r : upscalers) {
        if (r.name != name) continue;
        if (!r.available) {
            status = name + " unavailable: " + r.reason;
            changed("status");
            return;
        }
        GraphicsSettings g = settings.user().graphics;
        g.upscaler = name;
        settings.setGraphics(g);
        changed("upscaler");
        return;
    }
}

void GameUI::SettingsModel::onAutoDetectResult(const std::string& summary) {
    settings.captureFromCVars(); // applyQuality() set the scalability groups
    autoSelected = true;
    autoStatus = summary;
    if (handle) handle.DirtyAllVariables();
}

void GameUI::SettingsModel::update() { refreshAvailability(); }

void GameUI::SettingsModel::refresh() {
    refreshAvailability();
    if (handle) handle.DirtyAllVariables();
}

bool GameUI::SettingsModel::bind() {
    Rml::DataModelConstructor c = ui.createModel("settings");
    if (!c) {
        OX_LOG_ERROR("ui", "data model 'settings' already exists");
        return false;
    }
    if (auto s = c.RegisterStruct<UpscalerRow>()) {
        s.RegisterMember("name", &UpscalerRow::name);
        s.RegisterMember("available", &UpscalerRow::available);
        s.RegisterMember("reason", &UpscalerRow::reason);
    }
    c.RegisterArray<std::vector<UpscalerRow>>();
    c.RegisterArray<std::vector<Rml::String>>();

    std::vector<glm::ivec2> modes = hooks.resolutions ? hooks.resolutions()
                                                      : std::vector<glm::ivec2>{{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    resolutions.clear();
    resolutions.push_back("Native");
    for (glm::ivec2 m : modes) resolutions.push_back(resolutionText(m));
    refreshAvailability();

    c.Bind("qualities", &qualities);
    c.Bind("windowModes", &windowModes);
    c.Bind("resolutions", &resolutions);
    c.Bind("upscalers", &upscalers);
    c.Bind("rtAvailable", &rtAvailable);
    c.Bind("rtReason", &rtReason);
    c.Bind("autoStatus", &autoStatus);
    c.Bind("status", &status);

    c.BindFunc("quality", [this](Rml::Variant& v) { v = autoSelected ? Rml::String("Auto") : Rml::String(settings.user().graphics.quality); });
    c.BindFunc(
        "windowMode", [this](Rml::Variant& v) { v = int(settings.user().graphics.windowMode); },
        [this](const Rml::Variant& v) {
            GraphicsSettings g = settings.user().graphics;
            g.windowMode = WindowMode(std::clamp(v.Get<int>(), 0, 2));
            settings.setGraphics(g);
            changed("windowMode");
        });
    c.BindFunc(
        "resolution", [this](Rml::Variant& v) { v = Rml::String(resolutionText(settings.user().graphics.resolution)); },
        [this](const Rml::Variant& v) {
            GraphicsSettings g = settings.user().graphics;
            g.resolution = parseResolution(v.Get<Rml::String>());
            settings.setGraphics(g);
            changed("resolution");
        });
    c.BindFunc(
        "vsync", [this](Rml::Variant& v) { v = settings.user().graphics.vsync; },
        [this](const Rml::Variant& v) {
            GraphicsSettings g = settings.user().graphics;
            g.vsync = v.Get<bool>();
            settings.setGraphics(g);
            changed("vsync");
        });
    c.BindFunc(
        "rayTracing", [this](Rml::Variant& v) { v = rtAvailable && settings.user().graphics.rayTracing; },
        [this](const Rml::Variant& v) {
            if (!rtAvailable) {
                status = "Ray tracing unavailable: " + rtReason;
                changed("status");
                changed("rayTracing");
                return;
            }
            GraphicsSettings g = settings.user().graphics;
            g.rayTracing = v.Get<bool>();
            settings.setGraphics(g);
            changed("rayTracing");
        });
    c.BindFunc("upscaler", [this](Rml::Variant& v) { v = Rml::String(settings.user().graphics.upscaler); });
    c.BindFunc("upscalerReason", [this](Rml::Variant& v) {
        Rml::String r;
        for (const UpscalerRow& row : upscalers)
            if (!row.available) r += (r.empty() ? "" : " · ") + row.name + ": " + row.reason;
        v = r;
    });
    auto volume = [this](const char* bus, const char* var) {
        return std::pair{
            [this, bus](Rml::Variant& v) { v = bus ? settings.busVolume(bus) : settings.user().audio.masterVolume; },
            [this, bus, var](const Rml::Variant& v) {
                AudioSettings a = settings.user().audio;
                const f32 x = clamp01(v.Get<f32>());
                if (bus) a.busVolumes[bus] = x;
                else a.masterVolume = x;
                settings.setAudio(a);
                changed(var);
            }};
    };
    for (auto [bus, var] : {std::pair<const char*, const char*>{nullptr, "masterVolume"}, {"Music", "musicVolume"}, {"SFX", "sfxVolume"}}) {
        auto [get, set] = volume(bus, var);
        c.BindFunc(var, get, set);
    }

    c.BindEventCallback("set_quality", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) setQuality(args[0].Get<Rml::String>());
    });
    c.BindEventCallback("set_upscaler", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (!args.empty()) setUpscaler(args[0].Get<Rml::String>());
    });
    c.BindEventCallback("set_window_mode", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
        if (args.empty()) return;
        GraphicsSettings g = settings.user().graphics;
        g.windowMode = WindowMode(std::clamp(args[0].Get<int>(), 0, 2));
        settings.setGraphics(g);
        changed("windowMode");
    });
    c.BindEventCallback("toggle_vsync", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        GraphicsSettings g = settings.user().graphics;
        g.vsync = !g.vsync;
        settings.setGraphics(g);
        changed("vsync");
    });
    c.BindEventCallback("toggle_rt", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (!rtAvailable) {
            status = "Ray tracing unavailable: " + rtReason;
            changed("status");
            return;
        }
        GraphicsSettings g = settings.user().graphics;
        g.rayTracing = !g.rayTracing;
        settings.setGraphics(g);
        changed("rayTracing");
    });
    c.BindEventCallback("save", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        const Status st = settings.save();
        status = st ? "Settings saved" : "Saving failed: " + st.error().message;
        changed("status");
    });
    c.BindEventCallback("revert", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
        if (const Status st = settings.load(); st) settings.apply();
        autoSelected = false;
        status = "Settings reverted";
        refresh();
    });
    handle = c.GetModelHandle();
    return true;
}

} // namespace ox::ui
