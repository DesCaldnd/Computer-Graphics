#include "rml_backend.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/core/vfs.hpp>

#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Math.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include <stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <glm/gtc/type_ptr.hpp>

#include <cstring>
#include <fstream>

namespace ox::ui::detail {

namespace {

struct OpenFile {
    std::vector<std::byte> data;
    usize pos = 0;
};

std::string normalizeJoined(std::string path) {
    // Collapse "a/./b" and "a/x/../b" inside the path part (after an optional "scheme://").
    std::string prefix;
    if (auto p = path.find("://"); p != std::string::npos) {
        prefix = path.substr(0, p + 3);
        path = path.substr(p + 3);
    } else if (!path.empty() && path.front() == '/') {
        prefix = "/";
        path = path.substr(1);
    }
    std::vector<std::string> parts;
    usize start = 0;
    while (start <= path.size()) {
        usize end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.push_back(std::move(part));
        }
        start = end + 1;
    }
    std::string out = prefix;
    for (usize i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out;
}

} // namespace

std::optional<std::vector<std::byte>> readFile(Vfs* vfs, const std::string& path) {
    if (path.find("://") != std::string::npos) {
        if (!vfs) return std::nullopt;
        auto r = vfs->readBytes(path);
        if (!r) return std::nullopt;
        return std::move(*r);
    }
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    std::vector<std::byte> data(usize(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    return data;
}

// ---------------------------------------------------------------------------------------------------- renderer

RmlRenderer::~RmlRenderer() {
    for (u32 id : m_ownedTextures) m_textures.destroy(id);
}

void RmlRenderer::begin(UiFrame* frame) {
    m_frame = frame;
    m_scissor = false;
    m_transform = -1;
}

void RmlRenderer::end() { m_frame = nullptr; }

Rml::CompiledGeometryHandle RmlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) {
    Geometry g;
    g.vertices.resize(vertices.size());
    for (usize i = 0; i < vertices.size(); ++i) {
        const Rml::Vertex& v = vertices[i];
        UiVertex& o = g.vertices[i];
        o.pos = {v.position.x, v.position.y};
        o.uv = {v.tex_coord.x, v.tex_coord.y};
        o.color = u32(v.colour.red) | (u32(v.colour.green) << 8) | (u32(v.colour.blue) << 16) | (u32(v.colour.alpha) << 24);
    }
    g.indices.assign(indices.begin(), indices.end());
    const uintptr_t handle = m_nextGeometry++;
    m_geometry.emplace(handle, std::move(g));
    return Rml::CompiledGeometryHandle(handle);
}

void RmlRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) {
    if (!m_frame) return;
    auto it = m_geometry.find(uintptr_t(geometry));
    if (it == m_geometry.end()) return;
    UiDrawCmd cmd;
    cmd.texture = UiTextureRef(texture);
    cmd.xform = {1.0f, 1.0f, translation.x, translation.y};
    cmd.transform = m_transform;
    cmd.flags = kUiPremultiplied;
    if (m_scissor) cmd.clip = {m_scissorRect.Left(), m_scissorRect.Top(), m_scissorRect.Right(), m_scissorRect.Bottom()};
    else cmd.clip = {0, 0, i32(m_frame->size.x), i32(m_frame->size.y)};
    m_frame->add(it->second.vertices, it->second.indices, cmd);
}

void RmlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) { m_geometry.erase(uintptr_t(geometry)); }

Rml::TextureHandle RmlRenderer::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
    auto bytes = readFile(m_vfs, source);
    if (!bytes) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "texture not found: %s", source.c_str());
        return 0;
    }
    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes->data()), int(bytes->size()), &w, &h, &n, 4);
    if (!px) {
        Rml::Log::Message(Rml::Log::LT_WARNING, "cannot decode texture %s: %s", source.c_str(), stbi_failure_reason());
        return 0;
    }
    std::vector<u8> rgba(px, px + usize(w) * h * 4);
    stbi_image_free(px);
    for (usize i = 0; i < rgba.size(); i += 4) { // RmlUi 6 expects premultiplied alpha
        const u32 a = rgba[i + 3];
        rgba[i + 0] = u8((rgba[i + 0] * a + 127) / 255);
        rgba[i + 1] = u8((rgba[i + 1] * a + 127) / 255);
        rgba[i + 2] = u8((rgba[i + 2] * a + 127) / 255);
    }
    dimensions = {w, h};
    const u32 id = m_textures.create(u32(w), u32(h), std::move(rgba), "rml." + source);
    m_ownedTextures.push_back(id);
    return Rml::TextureHandle(id);
}

Rml::TextureHandle RmlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    if (dimensions.x <= 0 || dimensions.y <= 0) return 0;
    std::vector<u8> rgba(source.begin(), source.end());
    rgba.resize(usize(dimensions.x) * usize(dimensions.y) * 4);
    const u32 id = m_textures.create(u32(dimensions.x), u32(dimensions.y), std::move(rgba), "rml.generated");
    m_ownedTextures.push_back(id);
    return Rml::TextureHandle(id);
}

void RmlRenderer::ReleaseTexture(Rml::TextureHandle texture) {
    const u32 id = u32(texture);
    m_textures.destroy(id);
    std::erase(m_ownedTextures, id);
}

void RmlRenderer::EnableScissorRegion(bool enable) { m_scissor = enable; }
void RmlRenderer::SetScissorRegion(Rml::Rectanglei region) { m_scissorRect = region; }

void RmlRenderer::SetTransform(const Rml::Matrix4f* transform) {
    if (!transform || !m_frame) {
        m_transform = -1;
        return;
    }
    // Rml::Matrix4f is column-major by default, like glm.
    m_frame->transforms.push_back(glm::make_mat4(transform->data()));
    m_transform = i32(m_frame->transforms.size() - 1);
}

// ---------------------------------------------------------------------------------------------------- system

int RmlSystem::TranslateString(Rml::String& translated, const Rml::String& input) {
    if (m_translator) {
        if (auto r = m_translator(input); r && *r != input) {
            translated = *r;
            return 1;
        }
    }
    translated = input;
    return 0;
}

void RmlSystem::JoinPath(Rml::String& translated, const Rml::String& documentPath, const Rml::String& path) {
    if (path.find("://") != Rml::String::npos || (!path.empty() && path.front() == '/')) {
        translated = normalizeJoined(path);
        return;
    }
    const auto slash = documentPath.rfind('/');
    const Rml::String dir = slash == Rml::String::npos ? Rml::String() : documentPath.substr(0, slash + 1);
    translated = normalizeJoined(dir + path);
}

bool RmlSystem::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    switch (type) {
    case Rml::Log::LT_ERROR:
    case Rml::Log::LT_ASSERT:
        ++m_errors;
        OX_LOG_ERROR("rmlui", "{}", message);
        break;
    case Rml::Log::LT_WARNING: OX_LOG_WARN("rmlui", "{}", message); break;
    case Rml::Log::LT_INFO: OX_LOG_INFO("rmlui", "{}", message); break;
    default: OX_LOG_DEBUG("rmlui", "{}", message); break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- files

Rml::FileHandle RmlFiles::Open(const Rml::String& path) {
    auto data = readFile(m_vfs, path);
    if (!data) return 0;
    auto* f = new OpenFile{std::move(*data), 0};
    return Rml::FileHandle(f);
}

void RmlFiles::Close(Rml::FileHandle file) { delete reinterpret_cast<OpenFile*>(file); }

size_t RmlFiles::Read(void* buffer, size_t size, Rml::FileHandle file) {
    auto* f = reinterpret_cast<OpenFile*>(file);
    const usize n = std::min(size, f->data.size() - f->pos);
    std::memcpy(buffer, f->data.data() + f->pos, n);
    f->pos += n;
    return n;
}

bool RmlFiles::Seek(Rml::FileHandle file, long offset, int origin) {
    auto* f = reinterpret_cast<OpenFile*>(file);
    long base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? long(f->pos) : long(f->data.size());
    const long p = base + offset;
    if (p < 0 || usize(p) > f->data.size()) return false;
    f->pos = usize(p);
    return true;
}

size_t RmlFiles::Tell(Rml::FileHandle file) { return reinterpret_cast<OpenFile*>(file)->pos; }
size_t RmlFiles::Length(Rml::FileHandle file) { return reinterpret_cast<OpenFile*>(file)->data.size(); }

// ---------------------------------------------------------------------------------------------------- keys

Rml::Input::KeyIdentifier toRmlKey(Key key) {
    using namespace Rml::Input;
    const int k = int(key);
    if (k >= int(Key::A) && k <= int(Key::Z)) return KeyIdentifier(KI_A + (k - int(Key::A)));
    if (k >= int(Key::Num0) && k <= int(Key::Num9)) return KeyIdentifier(KI_0 + (k - int(Key::Num0)));
    if (k >= int(Key::F1) && k <= int(Key::F12)) return KeyIdentifier(KI_F1 + (k - int(Key::F1)));
    if (k >= int(Key::Kp0) && k <= int(Key::Kp9)) return KeyIdentifier(KI_NUMPAD0 + (k - int(Key::Kp0)));
    switch (key) {
    case Key::Space: return KI_SPACE;
    case Key::Apostrophe: return KI_OEM_7;
    case Key::Comma: return KI_OEM_COMMA;
    case Key::Minus: return KI_OEM_MINUS;
    case Key::Period: return KI_OEM_PERIOD;
    case Key::Slash: return KI_OEM_2;
    case Key::Semicolon: return KI_OEM_1;
    case Key::Equal: return KI_OEM_PLUS;
    case Key::LeftBracket: return KI_OEM_4;
    case Key::Backslash: return KI_OEM_5;
    case Key::RightBracket: return KI_OEM_6;
    case Key::GraveAccent: return KI_OEM_3;
    case Key::Escape: return KI_ESCAPE;
    case Key::Enter: return KI_RETURN;
    case Key::Tab: return KI_TAB;
    case Key::Backspace: return KI_BACK;
    case Key::Insert: return KI_INSERT;
    case Key::Delete: return KI_DELETE;
    case Key::Right: return KI_RIGHT;
    case Key::Left: return KI_LEFT;
    case Key::Down: return KI_DOWN;
    case Key::Up: return KI_UP;
    case Key::PageUp: return KI_PRIOR;
    case Key::PageDown: return KI_NEXT;
    case Key::Home: return KI_HOME;
    case Key::End: return KI_END;
    case Key::CapsLock: return KI_CAPITAL;
    case Key::ScrollLock: return KI_SCROLL;
    case Key::NumLock: return KI_NUMLOCK;
    case Key::PrintScreen: return KI_SNAPSHOT;
    case Key::Pause: return KI_PAUSE;
    case Key::KpDecimal: return KI_DECIMAL;
    case Key::KpDivide: return KI_DIVIDE;
    case Key::KpMultiply: return KI_MULTIPLY;
    case Key::KpSubtract: return KI_SUBTRACT;
    case Key::KpAdd: return KI_ADD;
    case Key::KpEnter: return KI_NUMPADENTER;
    case Key::KpEqual: return KI_OEM_NEC_EQUAL;
    case Key::LeftShift: return KI_LSHIFT;
    case Key::RightShift: return KI_RSHIFT;
    case Key::LeftControl: return KI_LCONTROL;
    case Key::RightControl: return KI_RCONTROL;
    case Key::LeftAlt: return KI_LMENU;
    case Key::RightAlt: return KI_RMENU;
    case Key::LeftSuper: return KI_LMETA;
    case Key::RightSuper: return KI_RMETA;
    case Key::Menu: return KI_APPS;
    default: return KI_UNKNOWN;
    }
}

} // namespace ox::ui::detail
