#include <oxwald/ui/debug_tools.hpp>

#include "inspector.hpp"

#include <oxwald/core/cvar.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/scalability.hpp>
#include <oxwald/runtime/console.hpp>
#include <oxwald/runtime/engine.hpp>
#include <oxwald/runtime/settings.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/world.hpp>
#include <oxwald/ui/draw_data.hpp>
#include <oxwald/ui/game_ui.hpp>
#include <oxwald/ui/imgui_layer.hpp>

#if OX_UI_HAS_ASYNC
#include <oxwald/async/scheduler.hpp>
#endif
#if OX_UI_HAS_GAMEPLAY
#include <oxwald/gameplay/ai.hpp>
#include <oxwald/gameplay/animation.hpp>
#include <oxwald/gameplay/audio.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/spline.hpp>
#endif

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>

namespace ox::ui {

static CVar<bool> cvShowStats("ui.ShowStats", false, "Stats HUD in the corner (also while the debug overlay is hidden)",
                              CVarFlags::Persist);

namespace {

constexpr usize kHistory = 240;

std::string bytesText(u64 b) {
    if (b >= (1ull << 30)) return std::format("{:.2f} GiB", f64(b) / f64(1ull << 30));
    if (b >= (1ull << 20)) return std::format("{:.1f} MiB", f64(b) / f64(1ull << 20));
    if (b >= (1ull << 10)) return std::format("{:.1f} KiB", f64(b) / f64(1ull << 10));
    return std::format("{} B", b);
}

ImVec4 levelColor(const Console::Line& l) {
    switch (l.kind) {
    case Console::Line::Kind::Input: return ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
    case Console::Line::Kind::Error: return ImVec4(1.0f, 0.42f, 0.38f, 1.0f);
    case Console::Line::Kind::Log:
        if (l.level >= log::Level::Error) return ImVec4(1.0f, 0.42f, 0.38f, 1.0f);
        if (l.level == log::Level::Warn) return ImVec4(1.0f, 0.82f, 0.35f, 1.0f);
        if (l.level <= log::Level::Debug) return ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
        return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    default: return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    }
}

// Default placement on first use (no imgui.ini yet): spread the windows instead of stacking them.
void placeWindow(ImVec2 pos01, ImVec2 size, ImVec2 pivot = ImVec2(0, 0)) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const f32 menuBar = vp->WorkPos.y > vp->Pos.y ? 0.0f : ImGui::GetFrameHeight(); // work area may not know it yet
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * pos01.x,
                                   std::max(vp->WorkPos.y + vp->WorkSize.y * pos01.y, vp->WorkPos.y + menuBar)),
                            ImGuiCond_FirstUseEver, pivot);
    ImGui::SetNextWindowSize(ImVec2(std::min(size.x, vp->WorkSize.x * 0.9f), std::min(size.y, vp->WorkSize.y * 0.9f)),
                             ImGuiCond_FirstUseEver);
}

void helpMarker(const std::string& text) {
    if (text.empty()) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

} // namespace

struct DebugTools::Impl {
    ImGuiLayer& imgui;
    DebugToolsContext ctx;
    std::vector<u32> ids;

    std::array<f32, kHistory> frameMs{};
    usize historyPos = 0;
    usize historyCount = 0;
    f64 smoothedMs = 16.0;

    // console
    std::string input;
    ImGuiTextFilter logFilter;
    bool showInfo = true, showWarn = true, showError = true, showDebug = false, showCommands = true;
    bool scrollToBottom = true, focusInput = true, capturedLog = false;
    usize lastLineCount = 0;
    std::vector<std::string> candidates;

    // settings / cvars
    ImGuiTextFilter cvarFilter;

    // inspector
    entt::entity selected = entt::null;
    World* selectedWorld = nullptr;
    ImGuiTextFilter entityFilter;

    Impl(ImGuiLayer& l, DebugToolsContext c) : imgui(l), ctx(c) {}

    Console* console() const { return ctx.console ? ctx.console : ctx.engine ? &ctx.engine->console() : nullptr; }
    Services* services() const { return ctx.services ? ctx.services : ctx.engine ? &ctx.engine->services() : nullptr; }
    Settings* settings() const { return ctx.settings ? ctx.settings : ctx.engine ? &ctx.engine->settings() : nullptr; }
    World* world() const { return ctx.world ? ctx.world : ctx.engine ? &ctx.engine->world() : nullptr; }
    RenderInfo renderInfo() const { return ctx.bridge ? ctx.bridge->renderInfo() : RenderInfo{}; }

    // Cvar edits go through the console when there is one, so the engine reacts like to a typed command
    // (graphics settings re-applied, cvarChanged signal).
    void setCVar(const std::string& name, const std::string& value) {
        if (Console* c = console()) {
            std::string v = value;
            if (v.empty() || v.find(' ') != std::string::npos) v = "\"" + v + "\"";
            if (auto r = c->execute(name + " " + v); !r) OX_LOG_WARN("ui", "{}", r.error().message);
        } else {
            CVarRegistry::instance().set(name, value, CVarSource::Console);
        }
    }

    // ------------------------------------------------------------------------------------------- stats
    f32 historyAt(usize i) const { return frameMs[(historyPos + kHistory - historyCount + i) % kHistory]; }

    void drawStatsHud() {
        if (!cvShowStats.get()) return;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 10.0f, vp->WorkPos.y + (imgui.visible() ? 30.0f : 10.0f)));
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDocking;
        if (ImGui::Begin("##statsHud", nullptr, flags)) {
            const RenderInfo ri = renderInfo();
            ImGui::Text("%.0f FPS  %.2f ms", smoothedMs > 0 ? 1000.0 / smoothedMs : 0.0, smoothedMs);
            if (ctx.engine) ImGui::Text("game %.2f ms  render %.2f ms", ctx.engine->stats().gameMs, ctx.engine->stats().renderMs);
            if (ri.valid) ImGui::Text("GPU %.2f ms  %u draws  %.2fM tris", ri.stats.gpuFrameMs, ri.stats.drawCalls, f64(ri.stats.triangles) / 1e6);
        }
        ImGui::End();
    }

    void drawStats(bool& open) {
        placeWindow(ImVec2(0.99f, 0.0f), ImVec2(460, 560), ImVec2(1.0f, 0.0f));
        if (!ImGui::Begin(DebugTools::kStats.data(), &open)) {
            ImGui::End();
            return;
        }
        const RenderInfo ri = renderInfo();
        const f64 fps = smoothedMs > 0 ? 1000.0 / smoothedMs : 0.0;
        ImGui::Text("%.1f FPS (%.2f ms)", fps, smoothedMs);
        std::array<f32, kHistory> graph{};
        f32 maxMs = 1.0f;
        for (usize i = 0; i < historyCount; ++i) {
            graph[i] = historyAt(i);
            maxMs = std::max(maxMs, graph[i]);
        }
        const std::string overlay = std::format("max {:.1f} ms", maxMs);
        ImGui::PlotLines("##frametime", graph.data(), int(historyCount), 0, overlay.c_str(), 0.0f, std::max(33.3f, maxMs * 1.1f),
                         ImVec2(-1.0f, 70.0f));
        bool hud = cvShowStats.get();
        if (ImGui::Checkbox("Show HUD (ui.ShowStats)", &hud)) setCVar("ui.ShowStats", hud ? "true" : "false");

        if (ctx.engine && ImGui::CollapsingHeader("CPU", ImGuiTreeNodeFlags_DefaultOpen)) {
            const EngineStats& s = ctx.engine->stats();
            ImGui::Text("Frame %llu", (unsigned long long)s.frame);
            ImGui::Text("Game thread   %.2f ms (simulation %.2f, extract %.2f)", s.gameMs, s.simulationMs, s.extractMs);
            ImGui::Text("Render thread %.2f ms", s.renderMs);
            ImGui::Text("Wait for render %.2f ms, pacing sleep %.2f ms", s.waitForRenderMs, s.sleepMs);
            ImGui::Text("Fixed steps %u (total %llu), alpha %.2f", s.fixedSteps, (unsigned long long)s.totalFixedSteps, s.alpha);
        }
        if (!ri.valid) {
            ImGui::TextDisabled("Renderer statistics unavailable (no UI render feature attached)");
            ImGui::End();
            return;
        }
        const render::RenderStats& r = ri.stats;
        if (ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("GPU frame %.2f ms, renderer CPU %.2f ms", r.gpuFrameMs, r.cpuRenderMs);
            ImGui::Text("Output %ux%u, render %ux%u", ri.outputSize.x, ri.outputSize.y, ri.renderSize.x, ri.renderSize.y);
            ImGui::Text("Draw calls %u, triangles %.3f M", r.drawCalls, f64(r.triangles) / 1e6);
            ImGui::Text("Instances %u visible / %u (culled %u)", r.visibleInstances, r.instances,
                        r.instances > r.visibleInstances ? r.instances - r.visibleInstances : 0u);
            ImGui::Text("Lights %u (shadowed %u), shadow maps %u rendered / %u cached", r.lights, r.shadowedLights,
                        r.shadowMapsRendered, r.shadowMapsCached);
            ImGui::Text("Graph passes %u, recompiles %u, features %u", r.renderGraphPasses, r.renderGraphCompiles, r.featuresEnabled);
            const u64 used = ri.memory.totalUsageBytes ? ri.memory.totalUsageBytes : r.vramUsageBytes;
            const u64 budget = ri.memory.totalBudgetBytes ? ri.memory.totalBudgetBytes : r.vramBudgetBytes;
            ImGui::Text("VRAM %s / %s", bytesText(used).c_str(), bytesText(budget).c_str());
            if (budget) ImGui::ProgressBar(f32(f64(used) / f64(budget)), ImVec2(-1.0f, 0.0f));
            ImGui::Text("Textures %u, buffers %u, geometry %s, pending loads %u", r.textures, r.buffers,
                        bytesText(r.geometryBytes).c_str(), r.pendingAssetLoads);
            ImGui::TextDisabled("%s", ri.caps.gpuName.c_str());
        }
        if (ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen)) {
            std::vector<render::PassTiming> passes = r.passes;
            std::sort(passes.begin(), passes.end(), [](const auto& a, const auto& b) { return a.gpuMs > b.gpuMs; });
            const f64 total = std::max(r.gpuFrameMs, 1e-6);
            if (ImGui::BeginTable("passes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, 240.0f))) {
                ImGui::TableSetupColumn("Pass");
                ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableHeadersRow();
                for (const auto& p : passes) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(p.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", p.gpuMs);
                    ImGui::TableNextColumn();
                    ImGui::ProgressBar(f32(p.gpuMs / total), ImVec2(-1.0f, 0.0f));
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    // ------------------------------------------------------------------------------------------- console
    static int consoleCallback(ImGuiInputTextCallbackData* data) {
        auto* self = static_cast<Impl*>(data->UserData);
        Console* c = self->console();
        if (!c) return 0;
        if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
            const std::string partial(data->Buf, usize(data->BufTextLen));
            self->candidates = c->complete(partial);
            const std::string prefix = c->completeCommonPrefix(partial);
            if (!prefix.empty() && prefix != partial) {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, prefix.c_str());
                if (self->candidates.size() == 1) data->InsertChars(data->CursorPos, " ");
            }
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
            std::string text;
            if (data->EventKey == ImGuiKey_UpArrow) text = c->historyPrev();
            else if (data->EventKey == ImGuiKey_DownArrow) text = c->historyNext();
            else return 0;
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, text.c_str());
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit) {
            self->candidates.clear();
        }
        return 0;
    }

    void submit(std::string_view line) {
        Console* c = console();
        if (!c || line.empty()) return;
        const Result<std::string> r = c->execute(line); // echoes input + output/error into the console buffer
        (void)r;
        c->resetHistoryCursor();
        candidates.clear();
        scrollToBottom = true;
    }

    void drawConsole(bool& open) {
        placeWindow(ImVec2(0.01f, 1.0f), ImVec2(720, 380), ImVec2(0.0f, 1.0f));
        if (!ImGui::Begin(DebugTools::kConsole.data(), &open)) {
            ImGui::End();
            return;
        }
        Console* c = console();
        if (!c) {
            ImGui::TextDisabled("No console backend");
            ImGui::End();
            return;
        }
        ImGui::Checkbox("Commands", &showCommands);
        ImGui::SameLine();
        ImGui::Checkbox("Debug", &showDebug);
        ImGui::SameLine();
        ImGui::Checkbox("Info", &showInfo);
        ImGui::SameLine();
        ImGui::Checkbox("Warnings", &showWarn);
        ImGui::SameLine();
        ImGui::Checkbox("Errors", &showError);
        ImGui::SameLine();
        if (ImGui::Button("Clear")) c->clearOutput();
        ImGui::SameLine();
        logFilter.Draw("Filter", 180.0f);

        const f32 footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("##log", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::PushFont(imgui.monoFont(), 0.0f);
            const std::vector<Console::Line> lines = c->output();
            std::vector<const Console::Line*> shown;
            shown.reserve(lines.size());
            for (const Console::Line& l : lines) {
                using K = Console::Line::Kind;
                bool ok = true;
                if (l.kind == K::Log) {
                    if (l.level >= log::Level::Error) ok = showError;
                    else if (l.level == log::Level::Warn) ok = showWarn;
                    else if (l.level <= log::Level::Debug) ok = showDebug;
                    else ok = showInfo;
                } else if (l.kind == K::Error) {
                    ok = showError || showCommands;
                } else {
                    ok = showCommands;
                }
                if (ok && logFilter.IsActive()) ok = logFilter.PassFilter(l.text.c_str());
                if (ok) shown.push_back(&l);
            }
            ImGuiListClipper clipper;
            clipper.Begin(int(shown.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const Console::Line& l = *shown[usize(i)];
                    ImGui::PushStyleColor(ImGuiCol_Text, levelColor(l));
                    ImGui::TextUnformatted(l.text.c_str()); // input lines already carry "> "
                    ImGui::PopStyleColor();
                }
            }
            if (lines.size() != lastLineCount) {
                lastLineCount = lines.size();
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) scrollToBottom = true;
            }
            if (scrollToBottom) ImGui::SetScrollHereY(1.0f);
            scrollToBottom = false;
            ImGui::PopFont();
        }
        ImGui::EndChild();

        ImGui::Separator();
        const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion |
                                          ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackEdit |
                                          ImGuiInputTextFlags_EscapeClearsAll;
        ImGui::SetNextItemWidth(-1.0f);
        if (focusInput) {
            ImGui::SetKeyboardFocusHere();
            focusInput = false;
        }
        if (ImGui::InputTextWithHint("##input", "cvar value | command args   (Tab completes, Up/Down history)", &input, flags,
                                     &Impl::consoleCallback, this)) {
            const std::string line = input;
            input.clear();
            submit(line);
            focusInput = true; // keep typing
        }
        if (!candidates.empty() && ImGui::IsItemActive()) {
            const ImVec2 pos(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y);
            const f32 height = std::min<f32>(f32(candidates.size()), 10.0f) * ImGui::GetTextLineHeightWithSpacing() + 8.0f;
            ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y - height));
            ImGui::SetNextWindowSize(ImVec2(ImGui::GetItemRectSize().x, height));
            if (ImGui::Begin("##completions", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_Tooltip)) {
                for (const std::string& cand : candidates) {
                    std::string extra;
                    const std::string first = cand.substr(0, cand.find(' '));
                    if (ICVar* v = CVarRegistry::instance().find(first)) extra = "  = " + v->toString();
                    ImGui::Text("%s", cand.c_str());
                    if (!extra.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", extra.c_str());
                    }
                }
            }
            ImGui::End();
        }
        ImGui::End();
    }

    // ------------------------------------------------------------------------------------------- settings/cvars
    void setOverall(QualityLevel level) {
        if (Settings* s = settings()) {
            GraphicsSettings g = s->user().graphics;
            g.quality = std::string(scalability::levelName(level));
            g.groups.clear();
            s->setGraphics(g);
        } else {
            scalability::setOverall(level);
        }
    }

    void setGroup(Scalability group, QualityLevel level) {
        setCVar("sg." + std::string(scalability::groupName(group)), std::to_string(int(level)));
        if (Settings* s = settings()) s->captureFromCVars();
    }

    void cvarEditor(ICVar& v) {
        const std::string id = "##" + v.name();
        const bool readOnly = v.hasFlag(CVarFlags::ReadOnly);
        ImGui::BeginDisabled(readOnly);
        ImGui::SetNextItemWidth(-1.0f);
        switch (v.type()) {
        case CVarType::Bool: {
            bool b = v.toString() == "true";
            if (ImGui::Checkbox(id.c_str(), &b)) setCVar(v.name(), b ? "true" : "false");
            break;
        }
        case CVarType::Int: {
            auto* c = dynamic_cast<CVar<int>*>(&v);
            int i = c ? c->get() : 0;
            if (!v.enumNames().empty()) {
                const auto& names = v.enumNames();
                const char* cur = i >= 0 && usize(i) < names.size() ? names[usize(i)].c_str() : "?";
                if (ImGui::BeginCombo(id.c_str(), cur)) {
                    for (usize n = 0; n < names.size(); ++n)
                        if (ImGui::Selectable(names[n].c_str(), int(n) == i)) setCVar(v.name(), names[n]);
                    ImGui::EndCombo();
                }
            } else if (ImGui::InputInt(id.c_str(), &i, 1, 100, ImGuiInputTextFlags_EnterReturnsTrue)) {
                setCVar(v.name(), std::to_string(i));
            }
            break;
        }
        case CVarType::Float: {
            auto* c = dynamic_cast<CVar<float>*>(&v);
            float f = c ? c->get() : 0.0f;
            bool changed = false;
            if (v.minValue() && v.maxValue()) changed = ImGui::SliderFloat(id.c_str(), &f, f32(*v.minValue()), f32(*v.maxValue()));
            else changed = ImGui::DragFloat(id.c_str(), &f, 0.01f);
            if (changed) setCVar(v.name(), ox::detail::formatCVarFloat(f));
            break;
        }
        case CVarType::String: {
            std::string s = v.toString();
            if (ImGui::InputText(id.c_str(), &s, ImGuiInputTextFlags_EnterReturnsTrue)) setCVar(v.name(), s);
            break;
        }
        }
        ImGui::EndDisabled();
    }

    void drawSettings(bool& open) {
        placeWindow(ImVec2(0.01f, 0.01f), ImVec2(640, 620));
        if (!ImGui::Begin(DebugTools::kSettings.data(), &open)) {
            ImGui::End();
            return;
        }
        const RenderInfo ri = renderInfo();
        // Overall preset.
        const QualityLevel overall = scalability::overallLevel();
        ImGui::TextUnformatted("Quality");
        for (int l = 0; l < int(kQualityLevelCount); ++l) {
            ImGui::SameLine();
            const bool current = overall == QualityLevel(l);
            if (current) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(std::string(scalability::levelName(QualityLevel(l))).c_str())) setOverall(QualityLevel(l));
            if (current) ImGui::PopStyleColor();
        }
        ImGui::SameLine();
        if (overall == QualityLevel::Custom) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Custom");
        if (ctx.bridge) {
            ImGui::SameLine();
            if (ImGui::Button("Auto")) ctx.bridge->autoDetectRequested.store(true);
            helpMarker("Runs the GPU benchmark between two frames and applies the recommended levels");
        }

        if (ImGui::BeginTable("groups", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg)) {
            for (usize g = 0; g < kScalabilityGroupCount; ++g) {
                const Scalability group = Scalability(g);
                const QualityLevel level = scalability::currentLevel(group);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(std::string(scalability::groupName(group)).c_str());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0f);
                const std::string id = "##sg" + std::to_string(g);
                const std::string cur(scalability::levelName(level));
                const bool rtGroup = group == Scalability::RayTracing && ri.valid && !ri.caps.rayTracingSupported();
                ImGui::BeginDisabled(rtGroup);
                if (ImGui::BeginCombo(id.c_str(), cur.c_str())) {
                    for (int l = 0; l < int(kQualityLevelCount); ++l)
                        if (ImGui::Selectable(std::string(scalability::levelName(QualityLevel(l))).c_str(), level == QualityLevel(l)))
                            setGroup(group, QualityLevel(l));
                    ImGui::EndCombo();
                }
                ImGui::EndDisabled();
                if (rtGroup) helpMarker(ri.caps.whyRayTracingUnavailable());
            }
            ImGui::EndTable();
        }

        // Ray tracing.
        ImGui::SeparatorText("Ray tracing & upscaling");
        {
            const bool supported = ri.valid && ri.caps.rayTracingSupported();
            const std::string reason = !ri.valid ? std::string("Renderer not running") : ri.caps.whyRayTracingUnavailable();
            bool rt = false;
            if (ICVar* v = CVarRegistry::instance().find("r.RayTracing")) rt = v->toString() == "true";
            ImGui::BeginDisabled(!supported);
            if (ImGui::Checkbox("Ray tracing (r.RayTracing)", &rt)) {
                if (Settings* s = settings()) {
                    GraphicsSettings g = s->user().graphics;
                    g.rayTracing = rt;
                    s->setGraphics(g);
                } else {
                    setCVar("r.RayTracing", rt ? "true" : "false");
                }
            }
            ImGui::EndDisabled();
            if (!supported) {
                ImGui::SameLine();
                ImGui::TextDisabled("unavailable");
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", reason.c_str());
                ImGui::TextWrapped("%s", reason.c_str());
            }
        }
        {
            std::vector<UpscalerAvailability> ups = ri.valid ? ri.upscalers : std::vector<UpscalerAvailability>{};
            ICVar* up = CVarRegistry::instance().find("r.Upscaler");
            if (ups.empty() && up)
                for (const std::string& n : up->enumNames()) ups.push_back({n, n != "DLSS", n == "DLSS" ? "Renderer not running" : ""});
            const std::string current = up ? up->toString() : "Off";
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::BeginCombo("Upscaler (r.Upscaler)", current.c_str())) {
                for (const UpscalerAvailability& u : ups) {
                    ImGui::BeginDisabled(!u.available);
                    if (ImGui::Selectable(u.name.c_str(), u.name == current)) {
                        if (Settings* s = settings()) {
                            GraphicsSettings g = s->user().graphics;
                            g.upscaler = u.name;
                            s->setGraphics(g);
                        } else {
                            setCVar("r.Upscaler", u.name);
                        }
                    }
                    ImGui::EndDisabled();
                    if (!u.available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", u.reason.c_str());
                }
                ImGui::EndCombo();
            }
            for (const UpscalerAvailability& u : ups)
                if (!u.available) ImGui::TextDisabled("%s: %s", u.name.c_str(), u.reason.c_str());
            if (ICVar* q = CVarRegistry::instance().find("r.Upscaler.Quality")) {
                ImGui::SetNextItemWidth(200.0f);
                cvarEditor(*q);
                ImGui::SameLine();
                ImGui::TextUnformatted("Upscaler quality");
            }
        }

        ImGui::SeparatorText("Console variables");
        cvarFilter.Draw("Filter##cvars", 220.0f);
        std::vector<ICVar*> all = CVarRegistry::instance().all();
        std::vector<ICVar*> shown;
        for (ICVar* v : all)
            if (!cvarFilter.IsActive() || cvarFilter.PassFilter(v->name().c_str())) shown.push_back(v);
        if (ImGui::BeginTable("cvars", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                              ImVec2(0, 0))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Default", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(int(shown.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    ICVar& v = *shown[usize(i)];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (v.isDefault()) ImGui::TextUnformatted(v.name().c_str());
                    else ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "%s", v.name().c_str());
                    if (ImGui::IsItemHovered()) {
                        std::string tip = v.description();
                        if (v.group() != Scalability::None) tip += "\nScalability group: " + std::string(scalability::groupName(v.group()));
                        ImGui::SetTooltip("%s", tip.c_str());
                    }
                    ImGui::TableNextColumn();
                    cvarEditor(v);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", v.defaultString().c_str());
                }
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // ------------------------------------------------------------------------------------------- inspector
    void entityNode(World& w, entt::entity e) {
        Entity ent = w.wrap(e);
        const auto children = ent.childHandles();
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (selected == e && selectedWorld == &w) flags |= ImGuiTreeNodeFlags_Selected;
        if (!ent.active()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        const bool openNode = ImGui::TreeNodeEx(reinterpret_cast<void*>(uintptr_t(entt::to_integral(e)) + 1), flags, "%s", ent.name().c_str());
        if (!ent.active()) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selected = e;
            selectedWorld = &w;
        }
        if (openNode) {
            const std::vector<entt::entity> kids(children.begin(), children.end());
            for (entt::entity c : kids)
                if (w.valid(c)) entityNode(w, c);
            ImGui::TreePop();
        }
    }

    void drawInspector(bool& open) {
        placeWindow(ImVec2(0.04f, 0.05f), ImVec2(760, 560));
        if (!ImGui::Begin(DebugTools::kInspector.data(), &open)) {
            ImGui::End();
            return;
        }
        World* w = world();
        if (!w) {
            ImGui::TextDisabled("No world");
            ImGui::End();
            return;
        }
        if (selectedWorld != w) {
            selected = entt::null;
            selectedWorld = w;
        }
        if (ImGui::BeginChild("##entities", ImVec2(260.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX)) {
            ImGui::Text("%zu entities", w->entityCount());
            entityFilter.Draw("##filter", -1.0f);
            if (entityFilter.IsActive()) {
                w->forEachInHierarchy([&](entt::entity e) {
                    const std::string& name = w->wrap(e).name();
                    if (!entityFilter.PassFilter(name.c_str())) return;
                    ImGui::PushID(int(entt::to_integral(e)));
                    if (ImGui::Selectable(name.c_str(), selected == e)) selected = e;
                    ImGui::PopID();
                });
            } else {
                const std::vector<entt::entity> roots(w->rootHandles().begin(), w->rootHandles().end());
                for (entt::entity e : roots)
                    if (w->valid(e)) entityNode(*w, e);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##components", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
            if (selected == entt::null || !w->valid(selected)) {
                ImGui::TextDisabled("Select an entity");
            } else {
                Entity ent = w->wrap(selected);
                std::string name = ent.name();
                ImGui::SetNextItemWidth(-80.0f);
                if (ImGui::InputText("Name", &name, ImGuiInputTextFlags_EnterReturnsTrue)) ent.setName(name);
                bool active = ent.active();
                if (ImGui::Checkbox("Active", &active)) ent.setActive(active);
                ImGui::SameLine();
                ImGui::TextDisabled("%s", ent.uuid().toString().c_str());
                for (const ComponentInfo* info : ComponentRegistry::instance().componentsOf(*w, selected)) {
                    if (!info || info->hiddenInInspector || !info->type) continue;
                    if (!ImGui::CollapsingHeader(info->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
                    void* data = info->get(*w, selected);
                    if (!data) continue;
                    ImGui::PushID(info->name.c_str());
                    if (inspectStruct(*info->type, data)) info->notifyChanged(*w, selected);
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
        ImGui::End();
    }

    // ------------------------------------------------------------------------------------------- render graph
    void drawRenderGraph(bool& open) {
        placeWindow(ImVec2(0.06f, 0.07f), ImVec2(760, 520));
        if (!ImGui::Begin(DebugTools::kRenderGraph.data(), &open)) {
            ImGui::End();
            return;
        }
        if (!ctx.bridge) {
            ImGui::TextDisabled("No renderer bridge");
            ImGui::End();
            return;
        }
        const std::optional<RenderGraphInfo> g = ctx.bridge->renderGraph();
        if (!g) {
            ImGui::TextDisabled("Waiting for the next frame…");
            ImGui::End();
            return;
        }
        ImGui::Text("View '%s', frame %llu: %zu passes, %u batches", g->view.c_str(), (unsigned long long)g->frame,
                    g->passes.size(), g->batches);
        ImGui::Text("Transient memory %s (unaliased %s)", bytesText(g->transientBytesAliased).c_str(),
                    bytesText(g->transientBytesUnaliased).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy Graphviz")) ImGui::SetClipboardText(g->graphviz.c_str());
        if (ImGui::BeginTabBar("rg")) {
            if (ImGui::BeginTabItem("Passes")) {
                if (ImGui::BeginTable("rgpasses", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                    ImGui::TableSetupColumn("Pass");
                    ImGui::TableSetupColumn("Queue", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGui::TableSetupColumn("Batch", ImGuiTableColumnFlags_WidthFixed, 44.0f);
                    ImGui::TableSetupColumn("Barriers", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableSetupColumn("GPU ms", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                    ImGui::TableHeadersRow();
                    int index = 0;
                    for (const RenderGraphPassInfo& p : g->passes) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (p.culled) ImGui::TextDisabled("-");
                        else ImGui::Text("%d", index++);
                        ImGui::TableNextColumn();
                        if (p.culled) ImGui::TextDisabled("%s (culled)", p.name.c_str());
                        else ImGui::TextUnformatted(p.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(p.queue.c_str());
                        ImGui::TableNextColumn();
                        if (!p.culled) ImGui::Text("%u", p.batch);
                        ImGui::TableNextColumn();
                        if (!p.culled) ImGui::Text("%u", p.barriers);
                        ImGui::TableNextColumn();
                        if (p.gpuMs >= 0.0) ImGui::Text("%.3f", p.gpuMs);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Resources")) {
                if (ImGui::BeginTable("rgres", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Resource");
                    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Alias", ImGuiTableColumnFlags_WidthFixed, 44.0f);
                    ImGui::TableSetupColumn("First", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupColumn("Last", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableHeadersRow();
                    for (const RenderGraphResourceInfo& r : g->resources) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (r.used) ImGui::TextUnformatted(r.name.c_str());
                        else ImGui::TextDisabled("%s (unused)", r.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%s%s", r.texture ? "texture" : "buffer", r.imported ? " imp" : "");
                        ImGui::TableNextColumn();
                        if (r.size) ImGui::TextUnformatted(bytesText(r.size).c_str());
                        ImGui::TableNextColumn();
                        if (r.aliasSlot >= 0) ImGui::Text("%d", r.aliasSlot);
                        ImGui::TableNextColumn();
                        if (r.used && r.firstPass != ~0u) ImGui::Text("%u", r.firstPass);
                        ImGui::TableNextColumn();
                        if (r.used && r.firstPass != ~0u) ImGui::Text("%u", r.lastPass);
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    // ------------------------------------------------------------------------------------------- coroutines
    void drawCoroutines(bool& open) {
        placeWindow(ImVec2(0.08f, 0.10f), ImVec2(680, 360));
        if (!ImGui::Begin(DebugTools::kCoroutines.data(), &open)) {
            ImGui::End();
            return;
        }
#if OX_UI_HAS_ASYNC
        CoroutineScheduler* sched = services() ? services()->tryGet<CoroutineScheduler>() : nullptr;
        if (!sched) {
            ImGui::TextDisabled("No CoroutineScheduler service");
        } else {
            const std::vector<CoroutineInfo> list = sched->coroutines();
            ImGui::Text("%zu live coroutines, game time %.2f s, %s", list.size(), sched->gameTime(), sched->paused() ? "paused" : "running");
            if (ImGui::BeginTable("coros", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Owner", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Waiting on");
                ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableHeadersRow();
                u64 cancelOwner = 0;
                for (const CoroutineInfo& c : list) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%llu", (unsigned long long)c.id);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(c.name.empty() ? "<unnamed>" : c.name.c_str());
                    if (c.parentId) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(child of %llu)", (unsigned long long)c.parentId);
                    }
                    ImGui::TableNextColumn();
                    if (c.owner) ImGui::Text("%llu", (unsigned long long)c.owner);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(std::string(toString(c.state)).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(c.waitingOn.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2fs / %llu f", c.ageSeconds, (unsigned long long)c.ageFrames);
                    ImGui::TableNextColumn();
                    if (c.owner) {
                        ImGui::PushID(int(c.id));
                        if (ImGui::SmallButton("Cancel")) cancelOwner = c.owner;
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cancel every coroutine of owner %llu", (unsigned long long)c.owner);
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
                if (cancelOwner) sched->cancelOwner(cancelOwner);
            }
        }
#else
        ImGui::TextDisabled("The async module is not part of this build");
#endif
        ImGui::End();
    }

    // ------------------------------------------------------------------------------------------- debug draw
    void drawDebugDraw(bool& open) {
        placeWindow(ImVec2(0.99f, 0.99f), ImVec2(360, 320), ImVec2(1.0f, 1.0f));
        if (!ImGui::Begin(DebugTools::kDebugDraw.data(), &open)) {
            ImGui::End();
            return;
        }
        bool any = false;
#if OX_UI_HAS_GAMEPLAY
        if (Services* s = services()) {
            auto toggle = [&](const char* label, bool* flag) {
                if (!flag) return;
                any = true;
                ImGui::Checkbox(label, flag);
            };
            auto* physics = s->tryGet<gameplay::PhysicsRuntime>();
            toggle("Physics bodies & colliders", physics ? &physics->debugDraw : nullptr);
            auto* ai = s->tryGet<gameplay::AIRuntime>();
            toggle("AI: navmesh, paths, perception", ai ? &ai->debugDraw : nullptr);
            auto* anim = s->tryGet<gameplay::AnimationRuntime>();
            toggle("Animation skeletons", anim ? &anim->debugDraw : nullptr);
            auto* audio = s->tryGet<gameplay::AudioRuntime>();
            toggle("Audio sources", audio ? &audio->debugDraw : nullptr);
            auto* spline = s->tryGet<gameplay::SplineRuntime>();
            toggle("Splines", spline ? &spline->debugDraw : nullptr);
        }
#endif
        if (!any) ImGui::TextDisabled("No gameplay runtimes (gameplay module not running)");
        ImGui::SeparatorText("Renderer");
        for (const char* name : {"r.DebugView", "r.Wireframe", "r.FrustumCulling", "r.GpuTimings"}) {
            if (ICVar* v = CVarRegistry::instance().find(name)) {
                ImGui::SetNextItemWidth(180.0f);
                cvarEditor(*v);
                ImGui::SameLine();
                ImGui::TextUnformatted(name);
            }
        }
        if (ctx.gameUI) {
            ImGui::SeparatorText("Game UI");
            static bool rmlDebugger = false;
            if (ImGui::Checkbox("RmlUi debugger", &rmlDebugger)) ctx.gameUI->setDebuggerVisible(rmlDebugger);
            if (ImGui::Button("Reload UI documents")) ctx.gameUI->pollHotReload(true);
        }
        ImGui::End();
    }
};

DebugTools::DebugTools(ImGuiLayer& imgui, DebugToolsContext context) : m_impl(std::make_unique<Impl>(imgui, context)) {
    Impl* i = m_impl.get();
    if (Console* c = i->console(); c && context.engine) {
        c->captureLog(log::Level::Info);
        i->capturedLog = true;
    }
    i->ids.push_back(imgui.addAlwaysOnTop([i] { i->drawStatsHud(); }));
    i->ids.push_back(imgui.addWindow(std::string(kStats), [i](bool& o) { i->drawStats(o); }));
    i->ids.push_back(imgui.addWindow(std::string(kConsole), [i](bool& o) { i->drawConsole(o); }, true));
    i->ids.push_back(imgui.addWindow(std::string(kSettings), [i](bool& o) { i->drawSettings(o); }));
    i->ids.push_back(imgui.addWindow(std::string(kInspector), [i](bool& o) { i->drawInspector(o); }));
    i->ids.push_back(imgui.addWindow(std::string(kRenderGraph), [i](bool& o) { i->drawRenderGraph(o); }));
    i->ids.push_back(imgui.addWindow(std::string(kCoroutines), [i](bool& o) { i->drawCoroutines(o); }));
    i->ids.push_back(imgui.addWindow(std::string(kDebugDraw), [i](bool& o) { i->drawDebugDraw(o); }));
}

DebugTools::~DebugTools() {
    for (u32 id : m_impl->ids) m_impl->imgui.removeWindow(id);
    if (m_impl->capturedLog)
        if (Console* c = m_impl->console()) c->stopCaptureLog();
    if (m_impl->ctx.bridge) m_impl->ctx.bridge->captureRenderGraph.store(false);
}

void DebugTools::tick(f64 realDt) {
    Impl& i = *m_impl;
    const f32 ms = f32(realDt * 1000.0);
    i.frameMs[i.historyPos] = ms;
    i.historyPos = (i.historyPos + 1) % kHistory;
    i.historyCount = std::min(i.historyCount + 1, kHistory);
    i.smoothedMs = i.smoothedMs * 0.9 + f64(ms) * 0.1;
    if (i.ctx.bridge) i.ctx.bridge->captureRenderGraph.store(i.imgui.visible() && i.imgui.windowOpen(kRenderGraph));
}

void DebugTools::open(std::string_view window, bool open) {
    m_impl->imgui.setWindowOpen(window, open);
    if (window == kConsole && open) m_impl->focusInput = true;
}

void DebugTools::consoleSubmit(std::string_view line) { m_impl->submit(line); }

Console* DebugTools::console() const { return m_impl->console(); }

} // namespace ox::ui
