// Built-in importers except models (model_importer.cpp).
#include <oxwald/assets/asset_data.hpp>
#include <oxwald/assets/importer.hpp>
#include <oxwald/assets/material.hpp>
#include <oxwald/assets/texture_import.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/format.hpp>

#include "internal.hpp"
#include "model_importer.hpp"

namespace ox::assets {

struct HeightmapImportSettings {
    u32 width = 0;  // 0 = square, derived from the file size
    u32 height = 0;
    std::string format = "r16"; // r8 | r16 | r32f (little-endian)
    f32 heightScale = 1.0f;     // meters at the max sample value
};

namespace detail {
void registerImporterSettingsTypes() {
    OX_REFLECT_TYPE(HeightmapImportSettings, "HeightmapImportSettings")
        .field("width", &HeightmapImportSettings::width)
        .field("height", &HeightmapImportSettings::height)
        .field("format", &HeightmapImportSettings::format)
        .field("heightScale", &HeightmapImportSettings::heightScale);
}
} // namespace detail

namespace {

nlohmann::ordered_json textureInfo(const TextureData& t) {
    nlohmann::ordered_json j;
    j["width"] = t.width;
    j["height"] = t.height;
    j["layers"] = t.layers;
    j["mips"] = t.mipCount;
    j["format"] = std::string(textureFormatName(t.format));
    return j;
}

class TextureImporter final : public IAssetImporter {
public:
    std::string_view name() const override { return "texture"; }
    u32 version() const override { return 1; }
    std::vector<std::string> extensions() const override {
        return {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".psd", ".gif", ".hdr", ".exr", ".ktx2"};
    }
    AssetType mainType() const override { return AssetType::Texture; }
    nlohmann::ordered_json defaultSettings() const override { return toSettingsJson(TextureImportSettings{}); }
    nlohmann::ordered_json defaultSettingsFor(const std::filesystem::path& source) const override {
        registerAssetTypes();
        TextureImportSettings s;
        const std::string ext = detail::toLower(source.extension().string());
        const std::string stem = detail::toLower(source.stem().string());
        auto has = [&](std::initializer_list<std::string_view> keys) {
            for (auto k : keys) {
                if (stem.find(k) != std::string::npos) return true;
            }
            return false;
        };
        if (ext == ".hdr" || ext == ".exr") {
            s.type = TextureType::HDR;
            s.srgb = false;
        } else if (has({"_normal", "_nrm", "_norm", "-normal", "normalmap"}) || stem.ends_with("_n")) {
            s.type = TextureType::Normal;
            s.srgb = false;
        } else if (has({"_orm", "_rough", "_metal", "_ao", "_occlusion", "_mask", "_gloss", "_height", "_disp",
                        "_spec"})) {
            s.type = TextureType::Linear;
            s.srgb = false;
        }
        return toSettingsJson(s);
    }
    Status import(ImportContext& ctx) override {
        auto settings = ctx.settings<TextureImportSettings>();
        auto tex = importTextureFile(ctx.sourcePath(), settings);
        if (!tex) return tex.error();
        auto& a = ctx.setMain(AssetType::Texture, serializeTexture(*tex));
        a.info = textureInfo(*tex);
        return {};
    }
};

// ".oxcube": {"faces": ["px.png", "nx.png", "py.png", "ny.png", "pz.png", "nz.png"]} (paths relative to the file).
class CubemapImporter final : public IAssetImporter {
public:
    std::string_view name() const override { return "cubemap"; }
    u32 version() const override { return 1; }
    std::vector<std::string> extensions() const override { return {".oxcube"}; }
    AssetType mainType() const override { return AssetType::Texture; }
    nlohmann::ordered_json defaultSettings() const override { return toSettingsJson(TextureImportSettings{}); }
    Status import(ImportContext& ctx) override {
        auto bytes = ctx.readSource();
        if (!bytes) return bytes.error();
        auto j = nlohmann::json::parse(detail::asString(*bytes), nullptr, false);
        if (j.is_discarded() || !j.contains("faces") || !j["faces"].is_array() || j["faces"].size() != 6) {
            return makeError("cubemap needs {{\"faces\": [6 image paths]}}");
        }
        std::vector<Image> faces;
        for (const auto& f : j["faces"]) {
            const auto path = ctx.sourcePath().parent_path() / f.get<std::string>();
            ctx.addSourceDependency(path);
            auto img = loadImage(path);
            if (!img) return img.error();
            faces.push_back(std::move(*img));
        }
        auto settings = ctx.settings<TextureImportSettings>();
        settings.cubemap = CubemapMode::None;
        auto tex = buildTexture(faces, settings);
        if (!tex) return tex.error();
        auto& a = ctx.setMain(AssetType::Texture, serializeTexture(*tex));
        a.info = textureInfo(*tex);
        return {};
    }
};

class MaterialImporter final : public IAssetImporter {
public:
    std::string_view name() const override { return "material"; }
    u32 version() const override { return 1; }
    std::vector<std::string> extensions() const override { return {".oxmat"}; }
    AssetType mainType() const override { return AssetType::Material; }
    Status import(ImportContext& ctx) override {
        auto bytes = ctx.readSource();
        if (!bytes) return bytes.error();
        auto mat = loadMaterial(*bytes);
        if (!mat) return mat.error();
        auto& a = ctx.setMain(AssetType::Material, materialToBinary(*mat));
        a.dependencies = mat->textureDependencies();
        return {};
    }
};

// Scenes and prefabs: any OXB1 or JSON archive; the artifact is always binary.
class DocumentImporter final : public IAssetImporter {
public:
    DocumentImporter(std::string name, AssetType type, std::vector<std::string> exts)
        : m_name(std::move(name)), m_type(type), m_exts(std::move(exts)) {}
    std::string_view name() const override { return m_name; }
    u32 version() const override { return 1; }
    std::vector<std::string> extensions() const override { return m_exts; }
    AssetType mainType() const override { return m_type; }
    Status import(ImportContext& ctx) override {
        auto bytes = ctx.readSource();
        if (!bytes) return bytes.error();
        auto doc = serial::decodeAny(*bytes);
        if (!doc) return doc.error();
        auto& a = ctx.setMain(m_type, serial::encodeBinary(*doc));
        // Candidate references; the registry keeps only UUIDs of known assets (entity ids are dropped).
        a.dependencies = collectUuidRefs(doc->root);
        a.info["kind"] = doc->kind;
        return {};
    }

private:
    std::string m_name;
    AssetType m_type;
    std::vector<std::string> m_exts;
};

class ScriptImporter final : public IAssetImporter {
public:
    std::string_view name() const override { return "script"; }
    u32 version() const override { return 1; }
    std::vector<std::string> extensions() const override { return {".lua"}; }
    AssetType mainType() const override { return AssetType::Script; }
    Status import(ImportContext& ctx) override {
        auto bytes = ctx.readSource();
        if (!bytes) return bytes.error();
        auto& a = ctx.setMain(AssetType::Script, std::move(*bytes));
        a.info["bytes"] = a.data.size();
        return {};
    }
};

u32 le32(const std::byte* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
u16 le16(const std::byte* p) {
    u16 v;
    std::memcpy(&v, p, 2);
    return v;
}

nlohmann::ordered_json audioInfo(std::span<const std::byte> d, const std::string& ext) {
    nlohmann::ordered_json info;
    info["format"] = ext.empty() ? "" : ext.substr(1);
    info["bytes"] = d.size();
    if (ext == ".wav" && d.size() >= 12 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(d.data() + 8, "WAVE", 4) == 0) {
        usize pos = 12;
        u32 channels = 0, rate = 0, bits = 0, dataBytes = 0;
        while (pos + 8 <= d.size()) {
            const u32 size = le32(d.data() + pos + 4);
            if (std::memcmp(d.data() + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= d.size()) {
                channels = le16(d.data() + pos + 10);
                rate = le32(d.data() + pos + 12);
                bits = le16(d.data() + pos + 22);
            } else if (std::memcmp(d.data() + pos, "data", 4) == 0) {
                dataBytes = size;
            }
            pos += 8 + size + (size & 1);
        }
        if (channels && bits) {
            const u64 frames = dataBytes / (channels * (bits / 8));
            info["sampleRate"] = rate;
            info["channels"] = channels;
            info["bitsPerSample"] = bits;
            info["frames"] = frames;
            info["duration"] = rate ? f64(frames) / rate : 0.0;
        }
    } else if (ext == ".flac" && d.size() >= 42 && std::memcmp(d.data(), "fLaC", 4) == 0) {
        // STREAMINFO is always the first metadata block.
        const auto* s = reinterpret_cast<const u8*>(d.data()) + 8;
        const u32 rate = (u32(s[10]) << 12) | (u32(s[11]) << 4) | (s[12] >> 4);
        const u32 channels = ((s[12] >> 1) & 7) + 1;
        const u64 frames = (u64(s[13] & 0x0f) << 32) | (u64(s[14]) << 24) | (u64(s[15]) << 16) | (u64(s[16]) << 8) | s[17];
        info["sampleRate"] = rate;
        info["channels"] = channels;
        info["frames"] = frames;
        info["duration"] = rate ? f64(frames) / rate : 0.0;
    } else if (ext == ".ogg" && d.size() >= 58 && std::memcmp(d.data(), "OggS", 4) == 0 &&
               std::memcmp(d.data() + 29, "vorbis", 6) == 0) {
        info["channels"] = u32(u8(d[39]));
        info["sampleRate"] = le32(d.data() + 40);
    }
    return info;
}

// Passthrough with metadata (audio, fonts, navmesh blobs, heightmaps).
class BlobImporter final : public IAssetImporter {
public:
    BlobImporter(std::string name, AssetType type, std::vector<std::string> exts)
        : m_name(std::move(name)), m_type(type), m_exts(std::move(exts)) {}
    std::string_view name() const override { return m_name; }
    u32 version() const override { return 1; }
    std::vector<std::string> extensions() const override { return m_exts; }
    AssetType mainType() const override { return m_type; }
    nlohmann::ordered_json defaultSettings() const override {
        if (m_type == AssetType::Heightmap) {
            registerAssetTypes();
            return toSettingsJson(HeightmapImportSettings{});
        }
        return IAssetImporter::defaultSettings();
    }
    nlohmann::ordered_json defaultSettingsFor(const std::filesystem::path& source) const override {
        auto j = defaultSettings();
        const std::string ext = detail::toLower(source.extension().string());
        if (m_type == AssetType::Heightmap) j["format"] = ext == ".r8" ? "r8" : ext == ".r32" ? "r32f" : "r16";
        return j;
    }
    Status import(ImportContext& ctx) override {
        auto bytes = ctx.readSource();
        if (!bytes) return bytes.error();
        const std::string ext = detail::toLower(ctx.sourcePath().extension().string());
        nlohmann::ordered_json info;
        if (m_type == AssetType::Audio) {
            info = audioInfo(*bytes, ext);
        } else if (m_type == AssetType::Heightmap) {
            auto s = ctx.settings<HeightmapImportSettings>();
            if (!ctx.meta().settings.contains("format")) s.format = ext == ".r8" ? "r8" : ext == ".r32" ? "r32f" : "r16";
            const u32 bpp = s.format == "r8" ? 1 : s.format == "r32f" ? 4 : 2;
            const u64 samples = bytes->size() / bpp;
            u32 w = s.width, h = s.height;
            if (w == 0 || h == 0) {
                w = h = u32(std::lround(std::sqrt(f64(samples))));
            }
            if (u64(w) * h * bpp != bytes->size()) {
                return makeError("heightmap size {} does not match {}x{} {}", bytes->size(), w, h, s.format);
            }
            info["width"] = w;
            info["height"] = h;
            info["format"] = s.format;
            info["heightScale"] = s.heightScale;
        } else {
            info["bytes"] = bytes->size();
        }
        auto& a = ctx.setMain(m_type, serializeBlob(m_type, info, *bytes));
        a.info = info;
        return {};
    }

private:
    std::string m_name;
    AssetType m_type;
    std::vector<std::string> m_exts;
};

} // namespace

void ImporterRegistry::addBuiltins() {
    registerAssetTypes();
    add(std::make_unique<TextureImporter>());
    add(std::make_unique<CubemapImporter>());
    add(std::make_unique<MaterialImporter>());
    add(std::make_unique<ModelImporter>());
    add(std::make_unique<DocumentImporter>("scene", AssetType::Scene,
                                           std::vector<std::string>{".oxscene", ".oxscene.json"}));
    add(std::make_unique<DocumentImporter>("prefab", AssetType::Prefab,
                                           std::vector<std::string>{".oxprefab", ".oxprefab.json"}));
    add(std::make_unique<ScriptImporter>());
    add(std::make_unique<BlobImporter>("audio", AssetType::Audio,
                                       std::vector<std::string>{".wav", ".ogg", ".mp3", ".flac"}));
    add(std::make_unique<BlobImporter>("font", AssetType::Font, std::vector<std::string>{".ttf", ".otf"}));
    add(std::make_unique<BlobImporter>("navmesh", AssetType::NavMesh, std::vector<std::string>{".oxnav", ".navmesh"}));
    add(std::make_unique<BlobImporter>("heightmap", AssetType::Heightmap,
                                       std::vector<std::string>{".r8", ".r16", ".r32", ".raw"}));
}

} // namespace ox::assets
