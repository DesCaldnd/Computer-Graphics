#include "model_importer.hpp"

#include <oxwald/assets/asset_data.hpp>
#include <oxwald/assets/model_import.hpp>
#include <oxwald/assets/texture_import.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/scene/prefab.hpp>
#include <oxwald/scene/scene.hpp>

#include "internal.hpp"

#if OX_ASSETS_HAS_ANIMATION
#include <oxwald/animation/importer.hpp>
#endif

#include <map>

namespace ox::assets {

std::vector<std::string> ModelImporter::extensions() const {
    return {".gltf", ".glb", ".obj", ".fbx", ".dae", ".3ds", ".ply", ".stl", ".blend", ".x", ".lwo", ".ms3d"};
}

nlohmann::ordered_json ModelImporter::defaultSettings() const {
    registerAssetTypes();
    return toSettingsJson(ModelImportSettings{});
}

namespace {

enum class TexRole { Color, Normal, Linear };

nlohmann::ordered_json roleSettings(TexRole role) {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    if (role == TexRole::Normal) {
        j["type"] = "Normal";
        j["srgb"] = false;
    } else if (role == TexRole::Linear) {
        j["type"] = "Linear";
        j["srgb"] = false;
    }
    return j;
}

nlohmann::ordered_json meshInfo(const MeshData& m) {
    nlohmann::ordered_json j;
    j["vertices"] = m.vertexCount();
    j["triangles"] = m.triangleCount(0);
    j["submeshes"] = m.submeshes.size();
    j["lods"] = m.lodCount();
    j["meshlets"] = m.meshlets.size();
    j["skinned"] = m.skinned();
    j["boundsMin"] = {m.bounds.min.x, m.bounds.min.y, m.bounds.min.z};
    j["boundsMax"] = {m.bounds.max.x, m.bounds.max.y, m.bounds.max.z};
    return j;
}

} // namespace

Status ModelImporter::import(ImportContext& ctx) {
    registerAssetTypes();
    registerSceneTypes();
    const auto settings = ctx.settings<ModelImportSettings>();
    auto imported = importModel(ctx.sourcePath(), settings);
    if (!imported) return imported.error();
    ImportedModel& model = *imported;
    const auto dir = ctx.sourcePath().parent_path();
    // .bin buffers / .mtl files next to the model trigger reimports when edited.
    const std::string ext = detail::toLower(ctx.sourcePath().extension().string());
    if (ext == ".obj") {
        auto mtl = ctx.sourcePath();
        mtl.replace_extension(".mtl");
        if (std::filesystem::exists(mtl)) ctx.addSourceDependency(mtl);
    }

    // ---- textures + materials -------------------------------------------------------------------------------
    std::map<std::pair<i32, TexRole>, Uuid> embeddedTextures;
    auto resolveTexture = [&](const ModelTextureRef& ref, TexRole role) -> Uuid {
        if (!ref.valid()) return {};
        if (ref.embedded >= 0) {
            const auto key = std::make_pair(ref.embedded, role);
            if (auto it = embeddedTextures.find(key); it != embeddedTextures.end()) return it->second;
            const EmbeddedImage& img = model.embeddedImages[usize(ref.embedded)];
            TextureImportSettings ts;
            fromSettingsJson(roleSettings(role), ts);
            Result<TextureData> tex = img.extension == "ktx2" ? importKtx2(img.data, ts) : [&]() -> Result<TextureData> {
                auto decoded = loadImageFromMemory(img.data, img.extension);
                if (!decoded) return decoded.error();
                return importTexture(*decoded, ts);
            }();
            if (!tex) {
                ctx.warn("embedded texture '" + img.name + "': " + tex.error().message);
                return {};
            }
            const std::string suffix = role == TexRole::Normal ? "_normal" : role == TexRole::Linear ? "_linear" : "";
            auto& a = ctx.addSubAsset("Texture/" + std::to_string(ref.embedded) + suffix, AssetType::Texture,
                                      serializeTexture(*tex));
            a.info["source"] = img.name;
            embeddedTextures[key] = a.uuid;
            return a.uuid;
        }
        const auto path = (dir / ref.path).lexically_normal();
        if (!std::filesystem::exists(path)) {
            ctx.warn("missing texture " + ref.path);
            return {};
        }
        return ctx.resolveAsset(path, roleSettings(role)).value_or(Uuid{});
    };
    std::vector<Uuid> materialUuids(model.materials.size());
    for (usize i = 0; i < model.materials.size(); ++i) {
        ImportedMaterial& im = model.materials[i];
        MaterialAsset mat = im.material;
        mat.albedoTexture = resolveTexture(im.albedo, TexRole::Color);
        mat.normalTexture = resolveTexture(im.normal, TexRole::Normal);
        mat.ormTexture = resolveTexture(im.orm.valid() ? im.orm : im.occlusion, TexRole::Linear);
        mat.emissiveTexture = resolveTexture(im.emissive, TexRole::Color);
        if (mat.emissiveTexture.isValid() && glm::all(glm::equal(mat.emissive, glm::vec3(0.0f)))) mat.emissive = glm::vec3(1.0f);
        auto& a = ctx.addSubAsset("Material/" + std::to_string(i), AssetType::Material, materialToBinary(mat));
        a.dependencies = mat.textureDependencies();
        a.info["name"] = im.name;
        materialUuids[i] = a.uuid;
    }
    auto slotUuid = [&](i64 materialIndex) -> Uuid {
        return materialIndex >= 0 && usize(materialIndex) < materialUuids.size() ? materialUuids[usize(materialIndex)] : Uuid{};
    };

    // ---- skeleton + clips ---------------------------------------------------------------------------------------
    Uuid skeletonUuid;
    std::vector<u16> jointRemap; // model joint index -> skeleton joint
#if OX_ASSETS_HAS_ANIMATION
    if (settings.importAnimations && (model.hasSkin || model.hasAnimations)) {
        anim::ImportSettings as;
        as.sourceUpAxis = anim::UpAxis(u8(settings.upAxis));
        as.scale = settings.scale;
        as.unitToMeters = settings.unitToMeters;
        as.importMeshes = false;
        std::string error;
        if (auto anim = anim::importAnimationAsset(ctx.sourcePath(), as, &error)) {
            if (anim->skeleton.jointCount() > 0) {
                auto& sk = ctx.addSubAsset("Skeleton", AssetType::Skeleton, serializeSkeletonAsset(anim->skeleton));
                sk.info["joints"] = anim->skeleton.jointCount();
                skeletonUuid = sk.uuid;
                for (const auto& name : model.jointNames) {
                    const i32 j = anim->skeleton.findJoint(name);
                    jointRemap.push_back(j >= 0 ? u16(j) : u16(0));
                }
            }
            for (usize c = 0; c < anim->clips.size(); ++c) {
                const auto& clip = anim->clips[c];
                auto& a = ctx.addSubAsset("Clip/" + std::to_string(c), AssetType::AnimationClip,
                                          serializeClipAsset(clip, skeletonUuid));
                a.info["name"] = clip.name;
                a.info["duration"] = clip.duration;
                if (skeletonUuid.isValid()) a.dependencies.push_back(skeletonUuid);
            }
        } else if (!error.empty()) {
            ctx.warn("animation import: " + error);
        }
    }
#endif
    auto finishMesh = [&](MeshData& mesh) {
        if (!jointRemap.empty()) {
            for (auto& sv : mesh.skin) {
                for (u16& j : sv.joints) j = j < jointRemap.size() ? jointRemap[j] : 0;
            }
        }
        mesh.skeleton = skeletonUuid;
        processMesh(mesh, settings.processSettings());
    };

    // ---- meshes ---------------------------------------------------------------------------------------------
    std::vector<Uuid> meshUuids;
    std::vector<std::vector<Uuid>> meshSlotMaterials;
    const bool merge = settings.mergeMeshes || !settings.createPrefab;
    if (merge) {
        std::vector<i32> slots;
        MeshData mesh = mergeModel(model, &slots);
        mesh.name = ctx.sourcePath().stem().string();
        std::vector<Uuid> mats;
        for (usize s = 0; s < mesh.materials.size(); ++s) {
            mesh.materials[s].material = slotUuid(s < slots.size() ? slots[s] : -1);
            mats.push_back(mesh.materials[s].material);
        }
        finishMesh(mesh);
        nlohmann::ordered_json info = meshInfo(mesh);
        if (!settings.createPrefab) {
            auto& a = ctx.setMain(AssetType::Mesh, serializeMesh(mesh));
            a.info = info;
            a.dependencies = skeletonUuid.isValid() ? std::vector<Uuid>{skeletonUuid} : std::vector<Uuid>{};
            return {};
        }
        auto& a = ctx.addSubAsset("Mesh/merged", AssetType::Mesh, serializeMesh(mesh));
        if (skeletonUuid.isValid()) a.dependencies.push_back(skeletonUuid);
        a.info = info;
        meshUuids.push_back(a.uuid);
        meshSlotMaterials.push_back(mats);
    } else {
        for (usize i = 0; i < model.meshes.size(); ++i) {
            MeshData& mesh = model.meshes[i];
            std::vector<Uuid> mats;
            for (usize s = 0; s < mesh.materials.size(); ++s) {
                const u32 mi = s < model.meshMaterials[i].size() ? model.meshMaterials[i][s] : ~0u;
                mesh.materials[s].material = slotUuid(mi == ~0u ? -1 : i64(mi));
                mats.push_back(mesh.materials[s].material);
            }
            finishMesh(mesh);
            auto& a = ctx.addSubAsset("Mesh/" + std::to_string(i), AssetType::Mesh, serializeMesh(mesh));
            if (skeletonUuid.isValid()) a.dependencies.push_back(skeletonUuid);
            a.info = meshInfo(mesh);
            a.info["name"] = mesh.name;
            meshUuids.push_back(a.uuid);
            meshSlotMaterials.push_back(mats);
        }
    }

    // ---- prefab ---------------------------------------------------------------------------------------------
    World world;
    const std::string mainId = ctx.uuid().toString();
    Entity root = world.createWithId(Uuid::fromName(mainId + "/root"), ctx.sourcePath().stem().string());
    auto addRenderer = [&](Entity e, usize meshIndex) {
        auto& mr = e.add<MeshRendererComponent>();
        mr.mesh = meshUuids[meshIndex];
        mr.materials = meshSlotMaterials[meshIndex];
    };
    if (merge) {
        addRenderer(root, 0);
    } else {
        std::vector<Entity> entities;
        for (usize i = 0; i < model.nodes.size(); ++i) {
            const ModelNode& n = model.nodes[i];
            Entity parent = n.parent >= 0 ? entities[usize(n.parent)] : root;
            Entity e = world.createWithId(Uuid::fromName(mainId + "/node/" + std::to_string(i)), n.name, parent);
            e.setLocalTransform(n.local);
            for (u32 m : n.meshes) {
                if (m >= meshUuids.size()) continue;
                if (!e.tryGet<MeshRendererComponent>()) {
                    addRenderer(e, m);
                } else { // several meshes on one node: child entities
                    Entity child = world.createWithId(
                        Uuid::fromName(mainId + "/node/" + std::to_string(i) + "/" + std::to_string(m)), n.name, e);
                    addRenderer(child, m);
                }
            }
            entities.push_back(e);
        }
    }
    world.updateTransforms();
    CreatePrefabOptions po;
    po.prefabId = ctx.uuid();
    po.linkSource = false;
    serial::Document prefab = createPrefab(world, root, po);
    auto& main = ctx.setMain(AssetType::Prefab, serial::encodeBinary(prefab));
    for (const auto& a : ctx.artifacts()) {
        if (!a.name.empty() && (a.type == AssetType::Mesh || a.type == AssetType::Material)) main.dependencies.push_back(a.uuid);
    }
    main.info["meshes"] = meshUuids.size();
    main.info["materials"] = materialUuids.size();
    main.info["nodes"] = model.nodes.size();
    main.info["importer"] = model.importer;
    return {};
}

} // namespace ox::assets
