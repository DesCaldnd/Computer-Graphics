#pragma once

// Data model "settings" for the sample settings menu (resources/sample/settings.rml): reads and writes the runtime
// UserSettings through ox::Settings, so the menu, the console and settings.json stay in sync.
//
// Variables: qualities[], quality, autoStatus, windowModes[], windowMode, resolutions[], resolution, vsync,
//            rayTracing, rtAvailable, rtReason, upscalers[] {name, available, reason}, upscaler, upscalerReason,
//            masterVolume, musicVolume, sfxVolume, status
// Events:    set_quality(name), set_upscaler(name), set_window_mode(index), toggle_vsync, toggle_rt, save, revert

#include <oxwald/ui/game_ui.hpp>

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

namespace ox::ui {

struct UpscalerRow {
    Rml::String name;
    bool available = true;
    Rml::String reason;
};

struct GameUI::SettingsModel {
    SettingsModel(GameUI& ui, Settings& settings, SettingsMenuHooks hooks);
    ~SettingsModel();

    bool bind();
    void update();   // per frame: availability changes
    void refresh();  // everything dirty
    void onAutoDetectResult(const std::string& summary);

    void setQuality(const std::string& name);
    void setUpscaler(const std::string& name);

    GameUI& ui;
    Settings& settings;
    SettingsMenuHooks hooks;
    Rml::DataModelHandle handle;
    std::vector<Rml::String> qualities{"Low", "Medium", "High", "Ultra", "Auto"};
    std::vector<Rml::String> windowModes{"Windowed", "Borderless", "Fullscreen"};
    std::vector<Rml::String> resolutions;
    std::vector<UpscalerRow> upscalers;
    bool rtAvailable = false;
    Rml::String rtReason;
    Rml::String autoStatus;
    Rml::String status;
    bool autoSelected = false;

private:
    void refreshAvailability();
    void changed(const char* variable);
};

} // namespace ox::ui
