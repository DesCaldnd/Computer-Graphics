#pragma once

// RmlUi backend on top of the engine: rendering into UiFrame (drawn by UiOverlayFeature), textures in the shared
// UiTextureStore, files through the core VFS (native paths as fallback), time/logging/translation/clipboard.

#include <oxwald/runtime/input.hpp>
#include <oxwald/ui/draw_data.hpp>

#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox {
class Vfs;
}

namespace ox::ui::detail {

class RmlRenderer final : public Rml::RenderInterface {
public:
    RmlRenderer(UiTextureStore& textures, Vfs* vfs) : m_textures(textures), m_vfs(vfs) {}
    ~RmlRenderer() override;

    // Recording target for Context::Render().
    void begin(UiFrame* frame);
    void end();

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

    [[nodiscard]] usize geometryCount() const { return m_geometry.size(); }
    [[nodiscard]] usize textureCount() const { return m_ownedTextures.size(); }

private:
    struct Geometry {
        std::vector<UiVertex> vertices;
        std::vector<u32> indices;
    };
    UiTextureStore& m_textures;
    Vfs* m_vfs;
    std::unordered_map<uintptr_t, Geometry> m_geometry;
    uintptr_t m_nextGeometry = 1;
    std::vector<u32> m_ownedTextures;
    UiFrame* m_frame = nullptr;
    bool m_scissor = false;
    Rml::Rectanglei m_scissorRect;
    i32 m_transform = -1;
};

class RmlSystem final : public Rml::SystemInterface {
public:
    using Translator = std::function<std::optional<std::string>(std::string_view)>;

    double GetElapsedTime() override { return m_time; }
    int TranslateString(Rml::String& translated, const Rml::String& input) override;
    void JoinPath(Rml::String& translated, const Rml::String& documentPath, const Rml::String& path) override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
    void SetClipboardText(const Rml::String& text) override { m_clipboard = text; }
    void GetClipboardText(Rml::String& text) override { text = m_clipboard; }

    void advance(f64 dt) { m_time += dt; }
    void setTranslator(Translator t) { m_translator = std::move(t); }
    [[nodiscard]] u32 errorCount() const { return m_errors; }

private:
    f64 m_time = 0.0;
    Translator m_translator;
    std::string m_clipboard;
    u32 m_errors = 0;
};

class RmlFiles final : public Rml::FileInterface {
public:
    explicit RmlFiles(Vfs* vfs) : m_vfs(vfs) {}
    Rml::FileHandle Open(const Rml::String& path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;

private:
    Vfs* m_vfs;
};

// Reads a VFS URI ("scheme://path") or a native path.
std::optional<std::vector<std::byte>> readFile(Vfs* vfs, const std::string& path);

Rml::Input::KeyIdentifier toRmlKey(Key key);

} // namespace ox::ui::detail
