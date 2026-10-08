#include <oxwald/assets/model_import.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"
#include "model_common.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>

#include <unordered_map>

namespace ox::assets {

namespace {

std::string mimeExtension(fastgltf::MimeType m) {
    switch (m) {
    case fastgltf::MimeType::JPEG: return "jpg";
    case fastgltf::MimeType::PNG: return "png";
    case fastgltf::MimeType::KTX2: return "ktx2";
    case fastgltf::MimeType::DDS: return "dds";
    case fastgltf::MimeType::WEBP: return "webp";
    default: return "png";
    }
}

struct GltfContext {
    const fastgltf::Asset& asset;
    ImportedModel& model;
    std::unordered_map<usize, i32> embeddedByImage;
};

ModelTextureRef textureRef(GltfContext& g, usize textureIndex) {
    ModelTextureRef ref;
    if (textureIndex >= g.asset.textures.size()) return ref;
    const auto& tex = g.asset.textures[textureIndex];
    std::optional<usize> image;
    if (tex.basisuImageIndex) image = *tex.basisuImageIndex;
    else if (tex.imageIndex) image = *tex.imageIndex;
    if (!image || *image >= g.asset.images.size()) return ref;
    const auto& img = g.asset.images[*image];
    std::visit(fastgltf::visitor{
                   [&](const fastgltf::sources::URI& uri) {
                       ref.path = std::string(uri.uri.path());
                   },
                   [&](const fastgltf::sources::BufferView& bv) {
                       if (auto it = g.embeddedByImage.find(*image); it != g.embeddedByImage.end()) {
                           ref.embedded = it->second;
                           return;
                       }
                       const auto& view = g.asset.bufferViews[bv.bufferViewIndex];
                       const auto& buffer = g.asset.buffers[view.bufferIndex];
                       EmbeddedImage e;
                       e.name = img.name.empty() ? "image" + std::to_string(*image) : std::string(img.name);
                       e.extension = mimeExtension(bv.mimeType);
                       std::visit(fastgltf::visitor{
                                      [&](const fastgltf::sources::Array& a) {
                                          e.data.assign(a.bytes.data() + view.byteOffset,
                                                        a.bytes.data() + view.byteOffset + view.byteLength);
                                      },
                                      [&](const fastgltf::sources::Vector& a) {
                                          e.data.assign(a.bytes.data() + view.byteOffset,
                                                        a.bytes.data() + view.byteOffset + view.byteLength);
                                      },
                                      [&](const fastgltf::sources::ByteView& a) {
                                          e.data.assign(a.bytes.data() + view.byteOffset,
                                                        a.bytes.data() + view.byteOffset + view.byteLength);
                                      },
                                      [](const auto&) {},
                                  },
                                  buffer.data);
                       if (e.data.empty()) return;
                       ref.embedded = i32(g.model.embeddedImages.size());
                       g.embeddedByImage[*image] = ref.embedded;
                       g.model.embeddedImages.push_back(std::move(e));
                   },
                   [&](const fastgltf::sources::Array& a) {
                       if (auto it = g.embeddedByImage.find(*image); it != g.embeddedByImage.end()) {
                           ref.embedded = it->second;
                           return;
                       }
                       EmbeddedImage e;
                       e.name = img.name.empty() ? "image" + std::to_string(*image) : std::string(img.name);
                       e.extension = mimeExtension(a.mimeType);
                       e.data.assign(a.bytes.begin(), a.bytes.end());
                       ref.embedded = i32(g.model.embeddedImages.size());
                       g.embeddedByImage[*image] = ref.embedded;
                       g.model.embeddedImages.push_back(std::move(e));
                   },
                   [](const auto&) {},
               },
               img.data);
    return ref;
}

ImportedMaterial convertMaterial(GltfContext& g, const fastgltf::Material& m) {
    ImportedMaterial out;
    out.name = std::string(m.name);
    MaterialAsset& mat = out.material;
    const auto& pbr = m.pbrData;
    mat.baseColor = glm::vec4(pbr.baseColorFactor[0], pbr.baseColorFactor[1], pbr.baseColorFactor[2], pbr.baseColorFactor[3]);
    mat.metallic = pbr.metallicFactor;
    mat.roughness = pbr.roughnessFactor;
    mat.emissive = glm::vec3(m.emissiveFactor[0], m.emissiveFactor[1], m.emissiveFactor[2]);
    mat.emissiveStrength = m.emissiveStrength;
    mat.doubleSided = m.doubleSided;
    mat.alphaCutoff = m.alphaCutoff;
    mat.ior = m.ior;
    mat.shadingModel = m.unlit ? ShadingModel::Unlit : ShadingModel::Lit;
    switch (m.alphaMode) {
    case fastgltf::AlphaMode::Mask: mat.blendMode = BlendMode::AlphaTest; break;
    case fastgltf::AlphaMode::Blend: mat.blendMode = BlendMode::Transparent; break;
    default: mat.blendMode = BlendMode::Opaque; break;
    }
    if (m.transmission && m.transmission->transmissionFactor > 0.0f) {
        mat.transmission = m.transmission->transmissionFactor;
        mat.blendMode = BlendMode::Refractive;
    }
    if (m.volume) {
        mat.thickness = m.volume->thicknessFactor;
        mat.absorptionColor = glm::vec3(m.volume->attenuationColor[0], m.volume->attenuationColor[1],
                                        m.volume->attenuationColor[2]);
        mat.absorptionDistance = std::isfinite(m.volume->attenuationDistance) ? f32(m.volume->attenuationDistance) : 0.0f;
    }
    if (m.clearcoat) {
        mat.clearcoat = m.clearcoat->clearcoatFactor;
        mat.clearcoatRoughness = m.clearcoat->clearcoatRoughnessFactor;
    }
    if (pbr.baseColorTexture) {
        out.albedo = textureRef(g, pbr.baseColorTexture->textureIndex);
        if (const auto& t = pbr.baseColorTexture->transform) {
            mat.uvTiling = glm::vec2(t->uvScale[0], t->uvScale[1]);
            mat.uvOffset = glm::vec2(t->uvOffset[0], t->uvOffset[1]);
        }
    }
    if (m.normalTexture) {
        out.normal = textureRef(g, m.normalTexture->textureIndex);
        mat.normalStrength = m.normalTexture->scale;
    }
    if (pbr.metallicRoughnessTexture) out.orm = textureRef(g, pbr.metallicRoughnessTexture->textureIndex);
    if (m.occlusionTexture) {
        auto occ = textureRef(g, m.occlusionTexture->textureIndex);
        mat.occlusionStrength = m.occlusionTexture->strength;
        const bool packed = out.orm.valid() && occ.path == out.orm.path && occ.embedded == out.orm.embedded;
        if (!packed) out.occlusion = occ;
    }
    // Without a packed occlusion channel the ORM texture's R is undefined: disable AO.
    if (!m.occlusionTexture || out.occlusion.valid()) mat.occlusionStrength = out.orm.valid() ? 0.0f : mat.occlusionStrength;
    if (m.emissiveTexture) out.emissive = textureRef(g, m.emissiveTexture->textureIndex);
    return out;
}

} // namespace

Result<ImportedModel> importModelGltf(const std::filesystem::path& path, const ModelImportSettings& settings) {
    OX_PROFILE_ZONE();
    constexpr auto kExtensions =
        fastgltf::Extensions::KHR_texture_transform | fastgltf::Extensions::KHR_texture_basisu |
        fastgltf::Extensions::KHR_mesh_quantization | fastgltf::Extensions::KHR_materials_ior |
        fastgltf::Extensions::KHR_materials_volume | fastgltf::Extensions::KHR_materials_transmission |
        fastgltf::Extensions::KHR_materials_clearcoat | fastgltf::Extensions::KHR_materials_emissive_strength |
        fastgltf::Extensions::KHR_materials_unlit | fastgltf::Extensions::KHR_lights_punctual;
    fastgltf::Parser parser(kExtensions);
    auto data = fastgltf::GltfDataBuffer::FromPath(path);
    if (data.error() != fastgltf::Error::None) {
        return makeError("{}: {}", path.string(), fastgltf::getErrorMessage(data.error()));
    }
    constexpr auto kOptions = fastgltf::Options::LoadExternalBuffers | fastgltf::Options::DecomposeNodeMatrices |
                              fastgltf::Options::GenerateMeshIndices;
    auto loaded = parser.loadGltf(data.get(), path.parent_path(), kOptions);
    if (loaded.error() != fastgltf::Error::None) {
        return makeError("{}: {}", path.string(), fastgltf::getErrorMessage(loaded.error()));
    }
    const fastgltf::Asset& asset = loaded.get();

    ImportedModel model;
    model.importer = "fastgltf";
    GltfContext g{asset, model, {}};
    const auto conv = detail::AxisConversion::make(settings.upAxis == UpAxis::Auto ? UpAxis::Y : settings.upAxis,
                                                   settings.unitToMeters, settings.scale);

    if (settings.importMaterials) {
        for (const auto& m : asset.materials) model.materials.push_back(convertMaterial(g, m));
    }

    // Joint names per skin, for remapping JOINTS_0 to model-wide joint names.
    std::vector<std::string> jointNames;
    auto jointIndex = [&](const std::string& name) -> u16 {
        for (usize i = 0; i < jointNames.size(); ++i) {
            if (jointNames[i] == name) return u16(i);
        }
        jointNames.push_back(name);
        return u16(jointNames.size() - 1);
    };
    // Skinned meshes use the skin of the first node referencing them.
    std::vector<std::optional<usize>> meshSkin(asset.meshes.size());
    for (const auto& n : asset.nodes) {
        if (n.meshIndex && n.skinIndex && !meshSkin[*n.meshIndex]) meshSkin[*n.meshIndex] = *n.skinIndex;
    }

    for (usize mi = 0; mi < asset.meshes.size(); ++mi) {
        const auto& gm = asset.meshes[mi];
        MeshData mesh;
        mesh.name = gm.name.empty() ? "Mesh" + std::to_string(mi) : std::string(gm.name);
        std::vector<u32> slotMaterials;
        bool hasNormals = true, hasTangents = true;
        std::vector<u16> skinJointMap;
        if (meshSkin[mi]) {
            const auto& skin = asset.skins[*meshSkin[mi]];
            for (usize j : skin.joints) {
                const auto& jn = asset.nodes[j];
                skinJointMap.push_back(jointIndex(jn.name.empty() ? "joint" + std::to_string(j) : std::string(jn.name)));
            }
            model.hasSkin = true;
        }
        for (usize pi = 0; pi < gm.primitives.size(); ++pi) {
            const auto& prim = gm.primitives[pi];
            if (prim.type != fastgltf::PrimitiveType::Triangles) continue;
            auto posIt = prim.findAttribute("POSITION");
            if (posIt == prim.attributes.end() || !prim.indicesAccessor) continue;
            const auto& posAcc = asset.accessors[posIt->accessorIndex];
            const u32 base = mesh.vertexCount();
            const usize count = posAcc.count;
            mesh.positions.resize(base + count);
            mesh.attributes.resize(base + count);
            fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, posAcc, [&](glm::vec3 p, usize i) { mesh.positions[base + i] = p; });
            if (auto it = prim.findAttribute("NORMAL"); it != prim.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec3>(asset, asset.accessors[it->accessorIndex],
                                                              [&](glm::vec3 n, usize i) { mesh.attributes[base + i].normal = n; });
            } else {
                hasNormals = false;
            }
            if (auto it = prim.findAttribute("TANGENT"); it != prim.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, asset.accessors[it->accessorIndex],
                                                              [&](glm::vec4 t, usize i) { mesh.attributes[base + i].tangent = t; });
            } else {
                hasTangents = false;
            }
            if (auto it = prim.findAttribute("TEXCOORD_0"); it != prim.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, asset.accessors[it->accessorIndex],
                                                              [&](glm::vec2 uv, usize i) { mesh.attributes[base + i].uv0 = uv; });
            }
            if (auto it = prim.findAttribute("TEXCOORD_1"); it != prim.attributes.end()) {
                fastgltf::iterateAccessorWithIndex<glm::vec2>(asset, asset.accessors[it->accessorIndex],
                                                              [&](glm::vec2 uv, usize i) { mesh.attributes[base + i].uv1 = uv; });
            }
            if (auto it = prim.findAttribute("COLOR_0"); it != prim.attributes.end()) {
                const auto& acc = asset.accessors[it->accessorIndex];
                if (acc.type == fastgltf::AccessorType::Vec3) {
                    fastgltf::iterateAccessorWithIndex<glm::vec3>(
                        asset, acc, [&](glm::vec3 c, usize i) { mesh.attributes[base + i].color = packColor(glm::vec4(c, 1.0f)); });
                } else {
                    fastgltf::iterateAccessorWithIndex<glm::vec4>(
                        asset, acc, [&](glm::vec4 c, usize i) { mesh.attributes[base + i].color = packColor(c); });
                }
            }
            auto jIt = prim.findAttribute("JOINTS_0");
            auto wIt = prim.findAttribute("WEIGHTS_0");
            if (jIt != prim.attributes.end() && wIt != prim.attributes.end() && !skinJointMap.empty()) {
                if (mesh.skin.size() < base) mesh.skin.resize(base);
                mesh.skin.resize(base + count);
                fastgltf::iterateAccessorWithIndex<glm::u16vec4>(asset, asset.accessors[jIt->accessorIndex], [&](glm::u16vec4 j, usize i) {
                    for (int k = 0; k < 4; ++k) mesh.skin[base + i].joints[k] = j[k] < skinJointMap.size() ? skinJointMap[j[k]] : 0;
                });
                fastgltf::iterateAccessorWithIndex<glm::vec4>(asset, asset.accessors[wIt->accessorIndex], [&](glm::vec4 w, usize i) {
                    const f32 sum = w.x + w.y + w.z + w.w;
                    if (sum > 0.0f) w /= sum;
                    u32 total = 0;
                    for (int k = 0; k < 4; ++k) {
                        mesh.skin[base + i].weights[k] = u16(std::lround(glm::clamp(w[k], 0.0f, 1.0f) * 65535.0f));
                        total += mesh.skin[base + i].weights[k];
                    }
                    mesh.skin[base + i].weights[0] = u16(i64(mesh.skin[base + i].weights[0]) + 65535 - i64(total));
                });
            } else if (!mesh.skin.empty()) {
                mesh.skin.resize(base + count);
            }
            Submesh sm;
            sm.name = mesh.name + "_" + std::to_string(pi);
            sm.vertexOffset = base;
            sm.vertexCount = u32(count);
            const u32 mat = prim.materialIndex ? u32(*prim.materialIndex) : ~0u;
            auto slotIt = std::find(slotMaterials.begin(), slotMaterials.end(), mat);
            sm.materialSlot = u32(slotIt - slotMaterials.begin());
            if (slotIt == slotMaterials.end()) {
                slotMaterials.push_back(mat);
                MaterialSlot slot;
                slot.name = mat != ~0u && mat < asset.materials.size() ? std::string(asset.materials[mat].name) : "Default";
                mesh.materials.push_back(slot);
            }
            MeshLod lod;
            lod.indexOffset = u32(mesh.indices.size());
            const auto& idxAcc = asset.accessors[*prim.indicesAccessor];
            fastgltf::iterateAccessor<u32>(asset, idxAcc, [&](u32 i) { mesh.indices.push_back(i + base); });
            lod.indexCount = u32(idxAcc.count);
            sm.lods.push_back(lod);
            mesh.submeshes.push_back(std::move(sm));
        }
        if (!mesh.skin.empty() && mesh.skin.size() != mesh.positions.size()) mesh.skin.resize(mesh.positions.size());
        conv.apply(mesh);
        detail::finalizeImportedMesh(mesh, hasNormals, hasTangents, settings);
        model.meshes.push_back(std::move(mesh));
        model.meshMaterials.push_back(slotMaterials);
    }

    // Node hierarchy of the default scene, parents first.
    const usize sceneIndex = asset.defaultScene ? *asset.defaultScene : 0;
    std::vector<std::pair<usize, i32>> stack;
    if (sceneIndex < asset.scenes.size()) {
        const auto& roots = asset.scenes[sceneIndex].nodeIndices;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.emplace_back(*it, -1);
    } else {
        for (usize i = asset.nodes.size(); i-- > 0;) stack.emplace_back(i, -1); // no scenes: every node is a root
    }
    while (!stack.empty()) {
        auto [ni, parent] = stack.back();
        stack.pop_back();
        const auto& n = asset.nodes[ni];
        ModelNode node;
        node.name = n.name.empty() ? "Node" + std::to_string(ni) : std::string(n.name);
        node.parent = parent;
        if (const auto* trs = std::get_if<fastgltf::TRS>(&n.transform)) {
            node.local.position = glm::vec3(trs->translation[0], trs->translation[1], trs->translation[2]);
            node.local.rotation = glm::quat(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]);
            node.local.scale = glm::vec3(trs->scale[0], trs->scale[1], trs->scale[2]);
        }
        node.local = conv.node(node.local);
        if (n.meshIndex) node.meshes.push_back(u32(*n.meshIndex));
        const i32 self = i32(model.nodes.size());
        model.nodes.push_back(std::move(node));
        for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) stack.emplace_back(*it, self);
    }
    model.jointNames = jointNames;
    model.hasAnimations = !asset.animations.empty();
    return model;
}

} // namespace ox::assets
