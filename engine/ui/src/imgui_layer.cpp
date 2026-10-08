#include <oxwald/ui/imgui_layer.hpp>

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/ui/resources.hpp>

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace ox::ui {

namespace {

struct KeyTable {
    int keys[kKeyCount]{};
    KeyTable() {
        for (int& k : keys) k = ImGuiKey_None;
        auto set = [&](Key k, ImGuiKey v) { keys[usize(k)] = v; };
        set(Key::Space, ImGuiKey_Space);
        set(Key::Apostrophe, ImGuiKey_Apostrophe);
        set(Key::Comma, ImGuiKey_Comma);
        set(Key::Minus, ImGuiKey_Minus);
        set(Key::Period, ImGuiKey_Period);
        set(Key::Slash, ImGuiKey_Slash);
        for (int i = 0; i < 10; ++i) set(Key(int(Key::Num0) + i), ImGuiKey(ImGuiKey_0 + i));
        set(Key::Semicolon, ImGuiKey_Semicolon);
        set(Key::Equal, ImGuiKey_Equal);
        for (int i = 0; i < 26; ++i) set(Key(int(Key::A) + i), ImGuiKey(ImGuiKey_A + i));
        set(Key::LeftBracket, ImGuiKey_LeftBracket);
        set(Key::Backslash, ImGuiKey_Backslash);
        set(Key::RightBracket, ImGuiKey_RightBracket);
        set(Key::GraveAccent, ImGuiKey_GraveAccent);
        set(Key::Escape, ImGuiKey_Escape);
        set(Key::Enter, ImGuiKey_Enter);
        set(Key::Tab, ImGuiKey_Tab);
        set(Key::Backspace, ImGuiKey_Backspace);
        set(Key::Insert, ImGuiKey_Insert);
        set(Key::Delete, ImGuiKey_Delete);
        set(Key::Right, ImGuiKey_RightArrow);
        set(Key::Left, ImGuiKey_LeftArrow);
        set(Key::Down, ImGuiKey_DownArrow);
        set(Key::Up, ImGuiKey_UpArrow);
        set(Key::PageUp, ImGuiKey_PageUp);
        set(Key::PageDown, ImGuiKey_PageDown);
        set(Key::Home, ImGuiKey_Home);
        set(Key::End, ImGuiKey_End);
        set(Key::CapsLock, ImGuiKey_CapsLock);
        set(Key::ScrollLock, ImGuiKey_ScrollLock);
        set(Key::NumLock, ImGuiKey_NumLock);
        set(Key::PrintScreen, ImGuiKey_PrintScreen);
        set(Key::Pause, ImGuiKey_Pause);
        for (int i = 0; i < 12; ++i) set(Key(int(Key::F1) + i), ImGuiKey(ImGuiKey_F1 + i));
        for (int i = 0; i < 10; ++i) set(Key(int(Key::Kp0) + i), ImGuiKey(ImGuiKey_Keypad0 + i));
        set(Key::KpDecimal, ImGuiKey_KeypadDecimal);
        set(Key::KpDivide, ImGuiKey_KeypadDivide);
        set(Key::KpMultiply, ImGuiKey_KeypadMultiply);
        set(Key::KpSubtract, ImGuiKey_KeypadSubtract);
        set(Key::KpAdd, ImGuiKey_KeypadAdd);
        set(Key::KpEnter, ImGuiKey_KeypadEnter);
        set(Key::KpEqual, ImGuiKey_KeypadEqual);
        set(Key::LeftShift, ImGuiKey_LeftShift);
        set(Key::LeftControl, ImGuiKey_LeftCtrl);
        set(Key::LeftAlt, ImGuiKey_LeftAlt);
        set(Key::LeftSuper, ImGuiKey_LeftSuper);
        set(Key::RightShift, ImGuiKey_RightShift);
        set(Key::RightControl, ImGuiKey_RightCtrl);
        set(Key::RightAlt, ImGuiKey_RightAlt);
        set(Key::RightSuper, ImGuiKey_RightSuper);
        set(Key::Menu, ImGuiKey_Menu);
    }
};

std::vector<u8> toRgba(ImTextureData& tex) {
    const usize n = usize(tex.Width) * usize(tex.Height);
    std::vector<u8> rgba(n * 4);
    if (tex.Format == ImTextureFormat_RGBA32) {
        std::memcpy(rgba.data(), tex.GetPixels(), n * 4);
    } else { // Alpha8: white + coverage
        const u8* a = static_cast<const u8*>(tex.GetPixels());
        for (usize i = 0; i < n; ++i) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = a[i];
        }
    }
    return rgba;
}

} // namespace

int toImGuiKey(Key key) {
    static const KeyTable table;
    return usize(key) < kKeyCount ? table.keys[usize(key)] : ImGuiKey_None;
}

ImGuiLayer::ImGuiLayer(UiTextureStore& textures, ImGuiLayerConfig config)
    : m_textures(textures), m_config(std::move(config)), m_visible(m_config.visible) {
    IMGUI_CHECKVERSION();
    ImGuiContext* previous = ImGui::GetCurrentContext();
    m_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_context);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "oxwald_rhi";
    io.BackendPlatformName = "oxwald_input";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    if (m_config.docking) io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = m_config.iniPath.empty() ? nullptr : m_config.iniPath.c_str();
#if defined(__APPLE__)
    io.ConfigMacOSXBehaviors = true;
#endif
    setupStyle();

    // Fonts: Inter (UI, Latin + Cyrillic) and JetBrains Mono (console). ImGui 1.92 rasterises glyphs on demand,
    // so no glyph ranges are needed; the TTF data must outlive the atlas.
    auto addFont = [&](std::string_view file, const char* name) -> ImFont* {
        std::vector<u8> data = readResource(file);
        if (data.empty()) return nullptr;
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", name);
        m_fontData.push_back(std::move(data));
        std::vector<u8>& d = m_fontData.back();
        return io.Fonts->AddFontFromMemoryTTF(d.data(), int(d.size()), m_config.fontSize, &cfg);
    };
    m_fontData.reserve(2);
    m_uiFont = addFont(kFontUi, "Inter");
    m_monoFont = addFont(kFontMono, "JetBrains Mono");
    if (!m_uiFont) m_uiFont = io.Fonts->AddFontDefault();
    if (!m_monoFont) m_monoFont = m_uiFont;
    io.FontDefault = m_uiFont;
    if (previous) ImGui::SetCurrentContext(previous);
}

ImGuiLayer::~ImGuiLayer() {
    if (!m_context) return;
    makeCurrent();
    if (m_inFrame) ImGui::EndFrame();
    destroyTextures();
    ImGui::DestroyContext(m_context);
    m_context = nullptr;
}

void ImGuiLayer::makeCurrent() const { ImGui::SetCurrentContext(m_context); }

void ImGuiLayer::setupStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.FramePadding = ImVec2(6.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 5.0f);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.97f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.29f, 0.48f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.20f, 0.36f, 0.58f, 0.55f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.46f, 0.72f, 0.80f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0, 0, 0, 0);
}

void ImGuiLayer::beginFrame(f64 dt, glm::uvec2 framebufferSize, f32 dpiScale) {
    OX_PROFILE_ZONE();
    makeCurrent();
    if (m_inFrame) ImGui::EndFrame(); // previous frame never rendered (e.g. minimised)
    m_dpiScale = dpiScale > 0.0f ? dpiScale : 1.0f;
    m_framebuffer = framebufferSize;
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = f32(std::max(dt, 1e-4));
    io.DisplaySize = ImVec2(f32(framebufferSize.x) / m_dpiScale, f32(framebufferSize.y) / m_dpiScale);
    io.DisplayFramebufferScale = ImVec2(m_dpiScale, m_dpiScale);
    ImGui::NewFrame();
    m_inFrame = true;
    frameStarted.emit();
    drawOverlay();
}

void ImGuiLayer::drawOverlay() {
    for (auto& [id, fn] : m_alwaysOnTop) fn();
    if (!m_visible) return;
    if (m_config.docking) ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Windows")) {
            std::vector<Window*> sorted;
            for (Window& w : m_windows) sorted.push_back(&w);
            std::sort(sorted.begin(), sorted.end(), [](const Window* a, const Window* b) { return a->path < b->path; });
            for (Window* w : sorted) ImGui::MenuItem(w->path.c_str(), nullptr, &w->open);
            ImGui::EndMenu();
        }
        for (auto& [id, fn] : m_menus) fn();
        const char* hint = "F1 / ~";
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(hint).x - 16.0f);
        ImGui::TextDisabled("%s", hint);
        ImGui::EndMainMenuBar();
    }
    // Copy: a window callback may add/remove windows.
    std::vector<u32> ids;
    for (const Window& w : m_windows)
        if (w.open) ids.push_back(w.id);
    for (u32 id : ids) {
        auto it = std::find_if(m_windows.begin(), m_windows.end(), [id](const Window& w) { return w.id == id; });
        if (it == m_windows.end()) continue;
        bool open = true;
        WindowFn fn = it->fn;
        fn(open);
        it = std::find_if(m_windows.begin(), m_windows.end(), [id](const Window& w) { return w.id == id; });
        if (it != m_windows.end() && !open) it->open = false;
    }
}

void ImGuiLayer::endFrame(UiFrame& out) {
    OX_PROFILE_ZONE();
    if (!m_inFrame) return;
    makeCurrent();
    ImGui::Render();
    m_inFrame = false;
    applyTextures();

    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || !dd->Valid) return;
    out.size = m_framebuffer;
    const ImVec2 fbs = dd->FramebufferScale, dp = dd->DisplayPos;
    const glm::vec4 xform{fbs.x, fbs.y, -dp.x * fbs.x, -dp.y * fbs.y};
    for (const ImDrawList* list : dd->CmdLists) {
        const i32 baseVertex = i32(out.vertices.size());
        const u32 baseIndex = u32(out.indices.size());
        static_assert(sizeof(ImDrawVert) == sizeof(UiVertex));
        const usize nv = usize(list->VtxBuffer.Size);
        out.vertices.resize(out.vertices.size() + nv);
        std::memcpy(out.vertices.data() + baseVertex, list->VtxBuffer.Data, nv * sizeof(UiVertex));
        out.indices.reserve(out.indices.size() + usize(list->IdxBuffer.Size));
        for (const ImDrawIdx i : list->IdxBuffer) out.indices.push_back(u32(i));
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue; // callbacks would run on the game thread, not at draw time
            UiDrawCmd c;
            c.firstIndex = baseIndex + cmd.IdxOffset;
            c.indexCount = cmd.ElemCount;
            c.vertexOffset = baseVertex + i32(cmd.VtxOffset);
            c.texture = cmd.GetTexID();
            c.clip = glm::ivec4(i32(std::floor((cmd.ClipRect.x - dp.x) * fbs.x)), i32(std::floor((cmd.ClipRect.y - dp.y) * fbs.y)),
                                i32(std::ceil((cmd.ClipRect.z - dp.x) * fbs.x)), i32(std::ceil((cmd.ClipRect.w - dp.y) * fbs.y)));
            c.xform = xform;
            c.flags = 0; // straight alpha
            if (c.indexCount) out.commands.push_back(c);
        }
    }
}

void ImGuiLayer::applyTextures() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        switch (tex->Status) {
        case ImTextureStatus_WantCreate: {
            const u32 id = m_textures.create(u32(tex->Width), u32(tex->Height), toRgba(*tex), "imgui.atlas");
            tex->SetTexID(ImTextureID(id));
            tex->SetStatus(ImTextureStatus_OK);
            break;
        }
        case ImTextureStatus_WantUpdates:
            // Whole-texture re-upload: updates only happen when new glyphs get rasterised.
            m_textures.update(u32(tex->GetTexID()), u32(tex->Width), u32(tex->Height), toRgba(*tex));
            tex->SetStatus(ImTextureStatus_OK);
            break;
        case ImTextureStatus_WantDestroy:
            if (tex->UnusedFrames > 0) {
                m_textures.destroy(u32(tex->GetTexID()));
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
            break;
        default: break;
        }
    }
}

void ImGuiLayer::destroyTextures() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount == 1 && tex->GetTexID() != ImTextureID_Invalid) {
            m_textures.destroy(u32(tex->GetTexID()));
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

bool ImGuiLayer::processEvent(const InputEvent& e) {
    makeCurrent();
    ImGuiIO& io = ImGui::GetIO();
    using T = InputEvent::Type;
    switch (e.type) {
    case T::Key: {
        const Key key = Key(e.code);
        if (e.pressed && !e.repeat) {
            const bool toggleKey =
                std::find(m_config.toggleKeys.begin(), m_config.toggleKeys.end(), key) != m_config.toggleKeys.end();
            // `~` types into a focused text field instead of toggling; F-keys always toggle.
            if (toggleKey && (key == Key::F1 || !(m_visible && io.WantTextInput))) {
                toggle();
                m_swallowText = key != Key::F1;
                return true;
            }
        }
        auto side = [&](Key l, Key r, bool (&state)[2], ImGuiKey mod) {
            if (key != l && key != r) return;
            state[key == l ? 0 : 1] = e.pressed;
            io.AddKeyEvent(mod, state[0] || state[1]);
        };
        side(Key::LeftControl, Key::RightControl, m_ctrl, ImGuiMod_Ctrl);
        side(Key::LeftShift, Key::RightShift, m_shift, ImGuiMod_Shift);
        side(Key::LeftAlt, Key::RightAlt, m_alt, ImGuiMod_Alt);
        side(Key::LeftSuper, Key::RightSuper, m_super, ImGuiMod_Super);
        const int ik = toImGuiKey(key);
        if (ik != ImGuiKey_None) io.AddKeyEvent(ImGuiKey(ik), e.pressed);
        return e.pressed && io.WantCaptureKeyboard;
    }
    case T::Text:
        if (m_swallowText) {
            m_swallowText = false;
            return true;
        }
        if (e.codepoint) io.AddInputCharacter(e.codepoint);
        return io.WantTextInput;
    case T::MouseMove: io.AddMousePosEvent(e.position.x / m_dpiScale, e.position.y / m_dpiScale); return false;
    case T::MouseButton:
        if (e.code < 5) io.AddMouseButtonEvent(int(e.code), e.pressed);
        return e.pressed && io.WantCaptureMouse;
    case T::MouseWheel: io.AddMouseWheelEvent(e.delta.x, e.delta.y); return io.WantCaptureMouse;
    case T::FocusLost:
        io.AddFocusEvent(false);
        std::fill(std::begin(m_ctrl), std::end(m_ctrl), false);
        std::fill(std::begin(m_shift), std::end(m_shift), false);
        std::fill(std::begin(m_alt), std::end(m_alt), false);
        std::fill(std::begin(m_super), std::end(m_super), false);
        return false;
    default: return false;
    }
}

bool ImGuiLayer::wantsMouse() const {
    makeCurrent();
    return ImGui::GetIO().WantCaptureMouse;
}
bool ImGuiLayer::wantsKeyboard() const {
    makeCurrent();
    return ImGui::GetIO().WantCaptureKeyboard;
}
bool ImGuiLayer::wantsTextInput() const {
    makeCurrent();
    return ImGui::GetIO().WantTextInput;
}

u32 ImGuiLayer::addWindow(std::string menuPath, WindowFn fn, bool open) {
    const u32 id = m_nextId++;
    m_windows.push_back({id, std::move(menuPath), std::move(fn), open});
    return id;
}

void ImGuiLayer::removeWindow(u32 id) {
    std::erase_if(m_windows, [id](const Window& w) { return w.id == id; });
    std::erase_if(m_alwaysOnTop, [id](const auto& p) { return p.first == id; });
    std::erase_if(m_menus, [id](const auto& p) { return p.first == id; });
}

void ImGuiLayer::setWindowOpen(std::string_view menuPath, bool open) {
    for (Window& w : m_windows)
        if (w.path == menuPath) w.open = open;
}

bool ImGuiLayer::windowOpen(std::string_view menuPath) const {
    for (const Window& w : m_windows)
        if (w.path == menuPath) return w.open;
    return false;
}

u32 ImGuiLayer::addAlwaysOnTop(std::function<void()> fn) {
    const u32 id = m_nextId++;
    m_alwaysOnTop.emplace_back(id, std::move(fn));
    return id;
}

u32 ImGuiLayer::addMenu(std::function<void()> fn) {
    const u32 id = m_nextId++;
    m_menus.emplace_back(id, std::move(fn));
    return id;
}

} // namespace ox::ui
