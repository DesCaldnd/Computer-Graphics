// Lua table `showcase` — the project's own engine API for its scripts (see Assets/Scripts/*.lua).
#include "net_demo.hpp"
#include "showcase.hpp"
#include "tour.hpp"

#include <oxwald/audio/audio_engine.hpp>
#include <oxwald/audio/audio_effects.hpp>
#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/render/features/raytracing/raytracing.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/script/script_vm.hpp>
#include <oxwald/ui/ui_system.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>

namespace ox::showcase {

namespace {

const char* modeName(LaunchMode m) {
    switch (m) {
    case LaunchMode::Tour: return "tour";
    case LaunchMode::Photo: return "photo";
    default: return "play";
    }
}

std::string formatTimestamp(i64 unixSeconds) {
    if (unixSeconds <= 0) return "—";
    std::time_t t = std::time_t(unixSeconds);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%d.%m.%Y %H:%M", &tm);
    return buf;
}

std::string formatPlayTime(f64 seconds) {
    const int s = int(seconds);
    return std::format("{}:{:02}:{:02}", s / 3600, (s / 60) % 60, s % 60);
}

const char* saveKindName(SaveKind k) {
    switch (k) {
    case SaveKind::Auto: return "auto";
    case SaveKind::Quick: return "quick";
    default: return "manual";
    }
}

std::optional<ui::RenderInfo> renderInfo(Engine& e) {
    auto* ui = e.services().tryGet<ui::UiSystem>();
    if (!ui || !ui->bridge().hasRenderInfo()) return std::nullopt;
    return ui->bridge().renderInfo();
}

// Reverb effects created on demand per bus (kept for the life of the audio engine).
std::unordered_map<std::string, audio::ReverbEffect*>& reverbs() {
    static std::unordered_map<std::string, audio::ReverbEffect*> r;
    return r;
}

} // namespace

void bindShowcaseLua(ShowcaseModule& module, script::ScriptVM& vm) {
    ShowcaseModule* M = &module;
    reverbs().clear(); // effects belong to the previous engine's audio buses
    vm.bindApi("showcase", [M](sol::state_view lua, sol::table& api) {
        // ---- launch ------------------------------------------------------------------------------------------
        api["mode"] = [M]() { return std::string(modeName(M->args().mode)); };
        api["headless"] = [M]() { return M->engine().config().headless; };
        api["args"] = [M](sol::this_state ts) {
            sol::state_view L(ts);
            sol::table t = L.create_table();
            for (usize i = 0; i < M->args().raw.size(); ++i) t[i + 1] = M->args().raw[i];
            return t;
        };
        api["stations"] = [M](sol::this_state ts) {
            sol::state_view L(ts);
            sol::table t = L.create_table();
            int i = 1;
            for (const StationInfo& s : M->stations()) {
                sol::table e = L.create_table();
                e["id"] = s.id;
                e["title"] = s.title;
                e["scene"] = s.scene;
                e["guide"] = s.guide;
                t[i++] = e;
            }
            return t;
        };

        // ---- cvars / console ---------------------------------------------------------------------------------
        api["cvar"] = [](const std::string& name) -> sol::optional<std::string> {
            if (const ICVar* c = CVarRegistry::instance().find(name)) return c->toString();
            return sol::nullopt;
        };
        // Through the console so the engine reacts exactly like to a typed command (graphics settings applied).
        api["setCVar"] = [M](const std::string& name, sol::object value) {
            std::string v = value.is<bool>() ? (value.as<bool>() ? "true" : "false")
                            : value.is<double>() ? std::format("{}", value.as<double>())
                                                 : value.as<std::string>();
            auto r = M->engine().console().execute(name + " " + v);
            return bool(r);
        };
        api["exec"] = [M](const std::string& line) {
            auto r = M->engine().console().execute(line);
            return r ? *r : "error: " + r.error().message;
        };

        // ---- levels ------------------------------------------------------------------------------------------
        api["loadLevel"] = [M](const std::string& uri) { M->queueLevel(uri); };
        api["currentLevel"] = [M]() { return M->engine().currentLevel(); };
        api["loading"] = [M]() { return M->engine().loading(); };
        api["quit"] = [M]() { M->engine().requestQuit(); };
        api["setPaused"] = [M](bool paused) { M->engine().setPaused(paused); };
        api["paused"] = [M]() { return M->engine().paused(); };
        api["frame"] = [M]() { return M->engine().stats().frame; };

        // ---- save games (applied at the start of the next frame) ---------------------------------------------
        api["save"] = [M](const std::string& slot, sol::optional<std::string> name) {
            if (!SaveGameSystem::isValidSlotName(slot)) return false;
            M->queueSave(slot, name.value_or(slot));
            return true;
        };
        api["load"] = [M](const std::string& slot) {
            if (!M->engine().saves().exists(slot)) return false;
            M->queueLoad(slot);
            return true;
        };
        api["quickSave"] = [M]() { M->queueSave("quicksave", "Быстрое сохранение"); };
        api["quickLoad"] = [M]() {
            if (!M->engine().saves().exists("quicksave")) return false;
            M->queueLoad("quicksave");
            return true;
        };
        api["hasSave"] = [M](const std::string& slot) { return M->engine().saves().exists(slot); };
        api["deleteSave"] = [M](const std::string& slot) { return M->engine().saves().deleteSlot(slot); };
        api["saves"] = [M](sol::this_state ts) {
            sol::state_view L(ts);
            sol::table t = L.create_table();
            int i = 1;
            for (const SaveSlotInfo& s : M->engine().saves().listSlots()) {
                sol::table e = L.create_table();
                e["slot"] = s.header.slot;
                e["name"] = s.header.displayName.empty() ? s.header.slot : s.header.displayName;
                e["level"] = s.header.level;
                e["playTime"] = formatPlayTime(s.header.playTimeSeconds);
                e["date"] = formatTimestamp(s.header.timestamp);
                e["kind"] = std::string(saveKindName(s.header.kind));
                e["corrupted"] = s.corrupted;
                e["entities"] = s.header.entityCount;
                e["bytes"] = s.fileSize;
                t[i++] = e;
            }
            return t;
        };
        api["saveIndicator"] = [M]() { return std::make_tuple(M->secondsSinceSave(), M->lastSaveKind()); };
        api["playTime"] = [M]() { return M->engine().saves().playTime(); };

        // ---- stats & render info -----------------------------------------------------------------------------
        api["stats"] = [M](sol::this_state ts) {
            sol::state_view L(ts);
            const EngineStats& s = M->engine().stats();
            sol::table t = L.create_table();
            t["fps"] = s.fps;
            t["frameMs"] = s.frameMs;
            t["gameMs"] = s.gameMs;
            t["renderMs"] = s.renderMs;
            if (auto info = renderInfo(M->engine())) {
                t["gpuMs"] = info->stats.gpuFrameMs;
                t["drawCalls"] = info->stats.drawCalls;
                t["triangles"] = f64(info->stats.triangles);
                t["lights"] = info->stats.lights;
                t["instances"] = info->stats.instances;
                t["pendingLoads"] = info->stats.pendingAssetLoads;
            }
            return t;
        };
        api["renderInfo"] = [M](sol::this_state ts) -> sol::object {
            sol::state_view L(ts);
            auto info = renderInfo(M->engine());
            if (!info) return sol::lua_nil;
            sol::table t = L.create_table();
            t["gpu"] = info->caps.gpuName;
            const render::rt::RtStatus rt = render::rt::rayTracingStatus(info->caps);
            t["rtAvailable"] = rt.available;
            t["rtReason"] = rt.reason;
            sol::table effects = L.create_table();
            int i = 1;
            for (const auto& e : rt.effects) {
                sol::table x = L.create_table();
                x["name"] = e.name;
                x["cvar"] = e.cvar;
                x["available"] = e.available;
                x["reason"] = e.reason;
                effects[i++] = x;
            }
            t["rtEffects"] = effects;
            sol::table ups = L.create_table();
            i = 1;
            for (const auto& u : info->upscalers) {
                sol::table x = L.create_table();
                x["name"] = u.name;
                x["available"] = u.available;
                x["reason"] = u.reason;
                ups[i++] = x;
            }
            t["upscalers"] = ups;
            t["renderWidth"] = info->renderSize.x;
            t["renderHeight"] = info->renderSize.y;
            t["outputWidth"] = info->outputSize.x;
            t["outputHeight"] = info->outputSize.y;
            return t;
        };
        // Top `count` render graph passes by GPU time of the last retired frame.
        api["gpuPasses"] = [M](sol::optional<int> count, sol::this_state ts) {
            sol::state_view L(ts);
            sol::table t = L.create_table();
            auto info = renderInfo(M->engine());
            if (!info) return t;
            auto passes = info->stats.passes;
            std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.gpuMs > b.gpuMs; });
            const usize n = std::min<usize>(passes.size(), usize(std::max(1, count.value_or(8))));
            for (usize i = 0; i < n; ++i) {
                sol::table x = L.create_table();
                x["name"] = passes[i].name;
                x["ms"] = passes[i].gpuMs;
                t[i + 1] = x;
            }
            return t;
        };
        api["screenshot"] = [M](const std::string& path) { return M->engine().renderer().requestScreenshot(path); };
        // Gameplay debug drawing per area ("physics", "ai", "animation", "audio", "splines").
        api["debugDraw"] = [M](const std::string& area, bool on) {
            Services& s = M->engine().services();
            if (area == "physics") { if (auto* r = s.tryGet<gameplay::PhysicsRuntime>()) r->debugDraw = on; }
            else if (area == "ai") { if (auto* r = s.tryGet<gameplay::AIRuntime>()) r->debugDraw = on; }
            else if (area == "animation") { if (auto* r = s.tryGet<gameplay::AnimationRuntime>()) r->debugDraw = on; }
            else if (area == "audio") { if (auto* r = s.tryGet<gameplay::AudioRuntime>()) r->debugDraw = on; }
            else if (area == "splines") { if (auto* r = s.tryGet<gameplay::SplineRuntime>()) r->debugDraw = on; }
            else return false;
            return true;
        };
        api["startTour"] = [M]() { M->startTour(); };

        // ---- input rebinding ---------------------------------------------------------------------------------
        sol::table input = lua.create_table();
        input["binding"] = [M](const std::string& ctx, const std::string& action, sol::optional<int> index) {
            return M->engine().input().bindingSource(ctx, action, u32(index.value_or(0)));
        };
        // Captures the next key/button and binds it (callback receives the new source).
        input["capture"] = [M](const std::string& ctx, const std::string& action, sol::optional<int> index,
                               sol::optional<sol::protected_function> done) {
            const std::string c = ctx, a = action;
            const u32 i = u32(index.value_or(0));
            auto fn = done ? std::optional<sol::protected_function>(*done) : std::nullopt;
            M->engine().input().captureNextInput([M, c, a, i, fn](const std::string& source) {
                if (source != "Key.Escape") M->engine().input().rebind(c, a, i, source);
                if (fn) (*fn)(source);
            });
        };
        input["capturing"] = [M]() { return M->engine().input().capturing(); };
        input["reset"] = [M]() { M->engine().input().resetAllBindings(); };
        api["input"] = input;

        // ---- audio buses -------------------------------------------------------------------------------------
        sol::table audioApi = lua.create_table();
        audioApi["reverb"] = [M](const std::string& bus, f32 wet, sol::optional<f32> roomSize) {
            auto* engine = M->engine().services().tryGet<audio::AudioEngine>();
            if (!engine) return false;
            audio::AudioBus* b = engine->bus(bus);
            if (!b) b = engine->createBus(bus, engine->bus("SFX"));
            if (!b) return false;
            auto& r = reverbs()[bus];
            audio::ReverbEffect::Params p;
            p.roomSize = roomSize.value_or(0.85f);
            p.wet = wet;
            if (!r) r = b->addEffect<audio::ReverbEffect>(p);
            else r->setParams(p);
            return true;
        };
        audioApi["duck"] = [M](const std::string& sidechain, const std::string& target, sol::optional<f32> duckVolume) {
            auto* engine = M->engine().services().tryGet<audio::AudioEngine>();
            if (!engine) return false;
            engine->addDucking({.sidechainBus = sidechain, .targetBus = target, .threshold = 0.01f,
                                .duckVolume = duckVolume.value_or(0.25f)});
            return true;
        };
        audioApi["level"] = [M](const std::string& bus) -> f32 {
            auto* engine = M->engine().services().tryGet<audio::AudioEngine>();
            audio::AudioBus* b = engine ? engine->bus(bus) : nullptr;
            return b ? b->rms() : 0.f;
        };
        audioApi["duckGain"] = [M](const std::string& bus) -> f32 {
            auto* engine = M->engine().services().tryGet<audio::AudioEngine>();
            audio::AudioBus* b = engine ? engine->bus(bus) : nullptr;
            return b ? b->duckGain() : 1.f;
        };
        audioApi["setVolume"] = [M](const std::string& bus, f32 volume) {
            auto* engine = M->engine().services().tryGet<audio::AudioEngine>();
            if (audio::AudioBus* b = engine ? engine->bus(bus) : nullptr) b->setVolume(volume);
        };
        api["audio"] = audioApi;

        // ---- net demo ----------------------------------------------------------------------------------------
        sol::table net = lua.create_table();
        net["stats"] = [M](sol::this_state ts) {
            sol::state_view L(ts);
            sol::table t = L.create_table();
            const NetDemoStats s = M->netDemo() ? M->netDemo()->stats() : NetDemoStats{};
            t["running"] = s.running;
            t["connected"] = s.connected;
            t["rttMs"] = s.rttMs;
            t["sendKBps"] = s.sendKBps;
            t["receiveKBps"] = s.receiveKBps;
            t["snapshotBytes"] = s.snapshotBytes;
            t["objects"] = s.objects;
            t["interpolationDelayMs"] = s.interpolationDelayMs;
            t["latencyMs"] = s.latencyMs;
            t["lossPercent"] = s.lossPercent;
            t["tickRate"] = s.tickRate;
            t["errorCm"] = s.errorCm;
            return t;
        };
        net["setConditions"] = [M](f32 latencyMs, f32 lossPercent, sol::optional<f32> jitterMs) {
            if (M->netDemo()) M->netDemo()->setConditions(latencyMs, lossPercent, jitterMs.value_or(10.f));
        };
        api["net"] = net;
    });
}

} // namespace ox::showcase
