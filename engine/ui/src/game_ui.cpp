#include <oxwald/ui/game_ui.hpp>

#include "rml_backend.hpp"
#include "settings_menu.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/vfs.hpp>
#include <oxwald/ui/resources.hpp>

#include <RmlUi/Core.h>
#include <RmlUi/Debugger.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>

namespace ox::ui {

namespace fs = std::filesystem;

namespace {
GameUI* s_instance = nullptr;

bool isUiFile(std::string_view path) {
    auto ends = [&](std::string_view ext) {
        return path.size() >= ext.size() &&
               std::equal(ext.begin(), ext.end(), path.end() - ext.size(),
                          [](char a, char b) { return std::tolower(u8(a)) == std::tolower(u8(b)); });
    };
    return ends(".rml") || ends(".rcss");
}
} // namespace

struct GameUI::Listener final : Rml::EventListener {
    u64 id = 0;
    std::string document, element, event;
    EventFn fn;
    Rml::Element* attached = nullptr;

    void ProcessEvent(Rml::Event& e) override {
        if (fn) fn(e);
    }
    void OnDetach(Rml::Element*) override { attached = nullptr; }
    void detach() {
        if (attached) attached->RemoveEventListener(event, this);
        attached = nullptr;
    }
};

GameUI::GameUI(UiTextureStore& textures, Vfs* vfs, GameUIConfig config)
    : m_textures(textures), m_vfs(vfs), m_config(std::move(config)) {
    OX_ASSERT(!s_instance, "only one GameUI may exist at a time (RmlUi has process-wide state)");
    s_instance = this;
    m_renderer = std::make_unique<detail::RmlRenderer>(textures, vfs);
    m_system = std::make_unique<detail::RmlSystem>();
    m_files = std::make_unique<detail::RmlFiles>(vfs);
    Rml::SetSystemInterface(m_system.get());
    Rml::SetFileInterface(m_files.get());
    Rml::SetRenderInterface(m_renderer.get());
    Rml::Initialise();
    if (m_config.loadDefaultFonts) {
        const fs::path fonts = resourceDir();
        Rml::LoadFontFace((fonts / kFontUi).string(), /*fallback*/ true);
        Rml::LoadFontFace((fonts / kFontUiBold).string());
        Rml::LoadFontFace((fonts / kFontMono).string());
    }
    m_size = {1280, 720};
    m_context = Rml::CreateContext(m_config.contextName, Rml::Vector2i(i32(m_size.x), i32(m_size.y)), m_renderer.get());
    OX_ASSERT(m_context, "RmlUi context creation failed");
    m_system->setTranslator([this](std::string_view text) -> std::optional<std::string> {
        if (!m_translations.empty() && text.size() > 1 && text.front() == '#') {
            if (auto it = m_translations.find(std::string(text.substr(1))); it != m_translations.end()) return it->second;
        }
        return std::nullopt;
    });
}

GameUI::~GameUI() {
    for (auto& l : m_listeners) l->detach();
    m_settings.reset();
    if (m_debugger) Rml::Debugger::Shutdown();
    if (m_context) Rml::RemoveContext(m_context->GetName());
    m_context = nullptr;
    m_docs.clear();
    Rml::Shutdown();
    m_listeners.clear();
    m_renderer.reset();
    m_files.reset();
    m_system.reset();
    s_instance = nullptr;
}

std::string GameUI::resolve(std::string_view path) const {
    const std::string p(path);
    if (p.find("://") != std::string::npos || fs::path(p).is_absolute()) return p;
    std::string root = m_config.root;
    if (!root.empty() && root.back() != '/') root += '/';
    return root + p;
}

GameUI::Doc* GameUI::findDoc(std::string_view name) {
    for (Doc& d : m_docs)
        if (d.name == name) return &d;
    return nullptr;
}
const GameUI::Doc* GameUI::findDoc(std::string_view name) const {
    for (const Doc& d : m_docs)
        if (d.name == name) return &d;
    return nullptr;
}

Rml::ElementDocument* GameUI::load(std::string_view path, bool show) {
    if (Doc* d = findDoc(path)) {
        if (show) this->show(path);
        return d->document;
    }
    const std::string uri = resolve(path);
    Rml::ElementDocument* doc = m_context->LoadDocument(uri);
    if (!doc) {
        OX_LOG_ERROR("ui", "cannot load UI document {}", uri);
        return nullptr;
    }
    m_docs.push_back({std::string(path), uri, doc, false, false});
    attachListeners(m_docs.back());
    if (show) this->show(path);
    snapshotMtimes();
    return doc;
}

Rml::ElementDocument* GameUI::document(std::string_view name) const {
    const Doc* d = findDoc(name);
    return d ? d->document : nullptr;
}

bool GameUI::show(std::string_view name, bool modal) {
    Doc* d = findDoc(name);
    if (!d || !d->document) return false;
    d->document->Show(modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None);
    d->visible = true;
    d->modal = modal;
    return true;
}

bool GameUI::hide(std::string_view name) {
    Doc* d = findDoc(name);
    if (!d || !d->document) return false;
    d->document->Hide();
    d->visible = false;
    return true;
}

bool GameUI::close(std::string_view name) {
    auto it = std::find_if(m_docs.begin(), m_docs.end(), [&](const Doc& d) { return d.name == name; });
    if (it == m_docs.end()) return false;
    for (auto& l : m_listeners)
        if (l->document == name) l->detach();
    if (it->document) it->document->Close();
    m_docs.erase(it);
    return true;
}

bool GameUI::isVisible(std::string_view name) const {
    const Doc* d = findDoc(name);
    return d && d->document && d->visible;
}

std::vector<std::string> GameUI::documents() const {
    std::vector<std::string> out;
    for (const Doc& d : m_docs) out.push_back(d.name);
    return out;
}

u64 GameUI::addEventListener(std::string_view document, std::string_view elementId, std::string_view event, EventFn fn) {
    auto l = std::make_unique<Listener>();
    l->id = m_nextListener++;
    l->document = document;
    l->element = elementId;
    l->event = event;
    l->fn = std::move(fn);
    const u64 id = l->id;
    m_listeners.push_back(std::move(l));
    if (Doc* d = findDoc(document)) attachListeners(*d);
    return id;
}

void GameUI::removeEventListener(u64 id) {
    auto it = std::find_if(m_listeners.begin(), m_listeners.end(), [id](const auto& l) { return l->id == id; });
    if (it == m_listeners.end()) return;
    (*it)->detach();
    m_listeners.erase(it);
}

void GameUI::attachListeners(Doc& doc) {
    if (!doc.document) return;
    for (auto& l : m_listeners) {
        if (l->document != doc.name || l->attached) continue;
        if (Rml::Element* el = doc.document->GetElementById(l->element)) {
            el->AddEventListener(l->event, l.get());
            l->attached = el;
        }
    }
}

void GameUI::update(f64 dt, glm::uvec2 framebufferSize, f32 dpiScale) {
    OX_PROFILE_ZONE();
    m_system->advance(dt);
    if (framebufferSize.x > 0 && framebufferSize.y > 0 && framebufferSize != m_size) {
        m_size = framebufferSize;
        m_context->SetDimensions(Rml::Vector2i(i32(m_size.x), i32(m_size.y)));
    }
    if (dpiScale > 0.0f && dpiScale != m_dpi) {
        m_dpi = dpiScale;
        m_context->SetDensityIndependentPixelRatio(m_dpi);
    }
    if (m_config.hotReload) {
        m_pollTimer += dt;
        if (m_pollTimer >= m_config.hotReloadInterval) {
            m_pollTimer = 0.0;
            pollHotReload();
        }
    }
    if (m_settings) m_settings->update();
    m_context->Update();
}

void GameUI::render(UiFrame& out) {
    OX_PROFILE_ZONE();
    out.size = m_size;
    m_renderer->begin(&out);
    m_context->Render();
    m_renderer->end();
}

int GameUI::modifiers() const {
    int m = 0;
    if (m_ctrl) m |= Rml::Input::KM_CTRL;
    if (m_shift) m |= Rml::Input::KM_SHIFT;
    if (m_alt) m |= Rml::Input::KM_ALT;
    if (m_meta) m |= Rml::Input::KM_META;
    return m;
}

bool GameUI::hasTextFocus() const {
    Rml::Element* f = m_context ? m_context->GetFocusElement() : nullptr;
    if (!f) return false;
    const Rml::String& tag = f->GetTagName();
    if (tag == "textarea") return true;
    if (tag != "input") return false;
    const Rml::String type = f->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password";
}

bool GameUI::wantsMouse() const { return m_context && m_context->IsMouseInteracting(); }

bool GameUI::processEvent(const InputEvent& e) {
    if (!m_context) return false;
    using T = InputEvent::Type;
    switch (e.type) {
    case T::MouseMove: m_context->ProcessMouseMove(i32(e.position.x), i32(e.position.y), modifiers()); return false;
    case T::MouseButton: {
        const int button = e.code == u16(MouseButton::Left) ? 0 : e.code == u16(MouseButton::Right) ? 1 : e.code == u16(MouseButton::Middle) ? 2 : int(e.code);
        if (e.pressed) return !m_context->ProcessMouseButtonDown(button, modifiers());
        m_context->ProcessMouseButtonUp(button, modifiers());
        return false;
    }
    case T::MouseWheel: return !m_context->ProcessMouseWheel(Rml::Vector2f(-e.delta.x, -e.delta.y), modifiers());
    case T::Key: {
        const Key k = Key(e.code);
        if (k == Key::LeftControl || k == Key::RightControl) m_ctrl = e.pressed;
        if (k == Key::LeftShift || k == Key::RightShift) m_shift = e.pressed;
        if (k == Key::LeftAlt || k == Key::RightAlt) m_alt = e.pressed;
        if (k == Key::LeftSuper || k == Key::RightSuper) m_meta = e.pressed;
        const Rml::Input::KeyIdentifier ki = detail::toRmlKey(k);
        if (!e.pressed) {
            m_context->ProcessKeyUp(ki, modifiers());
            return false;
        }
        const bool textFocus = hasTextFocus();
        const bool propagated = m_context->ProcessKeyDown(ki, modifiers());
        return !propagated || textFocus;
    }
    case T::Text: {
        if (!hasTextFocus() || e.codepoint < 32) return false;
        m_context->ProcessTextInput(Rml::Character(e.codepoint));
        return true;
    }
    case T::FocusLost:
        m_ctrl = m_shift = m_alt = m_meta = false;
        return false;
    default: return false;
    }
}

// ------------------------------------------------------------------------------------------------ hot reload

void GameUI::snapshotMtimes() {
    std::set<std::string> dirs;
    for (const Doc& d : m_docs) {
        const auto slash = d.uri.rfind('/');
        dirs.insert(slash == std::string::npos ? std::string() : d.uri.substr(0, slash));
    }
    std::map<std::string, u64, std::less<>> now;
    for (const std::string& dir : dirs) {
        if (dir.find("://") != std::string::npos) {
            if (!m_vfs) continue;
            for (const std::string& uri : m_vfs->list(dir, true)) {
                if (!isUiFile(uri)) continue;
                if (auto t = m_vfs->modificationTime(uri)) now[uri] = *t;
            }
        } else {
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(dir.empty() ? fs::path(".") : fs::path(dir), ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec) || !isUiFile(it->path().string())) continue;
                now[it->path().string()] = u64(it->last_write_time(ec).time_since_epoch().count());
            }
        }
    }
    m_mtimes = std::move(now);
}

usize GameUI::pollHotReload(bool force) {
    if (m_docs.empty()) return 0;
    const auto before = m_mtimes;
    snapshotMtimes();
    if (!force && before == m_mtimes) return 0;
    if (!force) {
        for (const auto& [uri, t] : m_mtimes) {
            auto it = before.find(uri);
            if (it == before.end() || it->second != t) OX_LOG_INFO("ui", "UI file changed: {}", uri);
        }
    }
    reloadAll();
    return m_docs.size();
}

void GameUI::reloadAll() {
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    for (auto& l : m_listeners) l->detach();
    for (Doc& d : m_docs) {
        if (d.document) d.document->Close();
        d.document = m_context->LoadDocument(d.uri);
        if (!d.document) {
            OX_LOG_ERROR("ui", "hot reload of {} failed (document stays closed until fixed)", d.uri);
            continue;
        }
        if (d.visible) d.document->Show(d.modal ? Rml::ModalFlag::Modal : Rml::ModalFlag::None);
        attachListeners(d);
    }
    m_context->Update();
    for (const Doc& d : m_docs) documentReloaded.emit(d.name);
}

// ------------------------------------------------------------------------------------------------ models

Rml::DataModelConstructor GameUI::createModel(std::string_view name) { return m_context->CreateDataModel(Rml::String(name)); }

Rml::DataModelHandle GameUI::model(std::string_view name) const {
    Rml::DataModelConstructor c = m_context->GetDataModel(Rml::String(name));
    return c ? c.GetModelHandle() : Rml::DataModelHandle();
}

bool GameUI::removeModel(std::string_view name) { return m_context->RemoveDataModel(Rml::String(name)); }

void GameUI::setTranslator(Translator translator) {
    auto table = [this](std::string_view text) -> std::optional<std::string> {
        if (!m_translations.empty() && text.size() > 1 && text.front() == '#') {
            if (auto it = m_translations.find(std::string(text.substr(1))); it != m_translations.end()) return it->second;
        }
        return std::nullopt;
    };
    m_system->setTranslator([table, translator = std::move(translator)](std::string_view text) -> std::optional<std::string> {
        if (auto r = table(text)) return r;
        return translator ? translator(text) : std::nullopt;
    });
    if (!m_docs.empty()) reloadAll();
}

void GameUI::setTranslations(std::map<std::string, std::string> table) {
    m_translations = std::move(table);
    if (!m_docs.empty()) reloadAll();
}

bool GameUI::bindSettingsMenu(Settings& settings, SettingsMenuHooks hooks) {
    if (m_settings) return true;
    m_settings = std::make_unique<SettingsModel>(*this, settings, std::move(hooks));
    if (!m_settings->bind()) {
        m_settings.reset();
        return false;
    }
    return true;
}

void GameUI::onAutoDetectResult(const std::string& summary) {
    if (m_settings) m_settings->onAutoDetectResult(summary);
}

void GameUI::refreshSettingsMenu() {
    if (m_settings) m_settings->refresh();
}

void GameUI::setDebuggerVisible(bool visible) {
    if (!m_debugger && visible) {
        Rml::Debugger::Initialise(m_context);
        m_debugger = true;
    }
    if (m_debugger) Rml::Debugger::SetVisible(visible);
}

u32 GameUI::errorCount() const { return m_system ? m_system->errorCount() : 0; }

} // namespace ox::ui
