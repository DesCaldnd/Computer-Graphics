#include <oxwald/assets/model_import.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"
#include "model_common.hpp"

#include <meshoptimizer.h>

namespace ox::assets {

MeshProcessSettings ModelImportSettings::processSettings() const {
    MeshProcessSettings p;
    p.optimizeVertexCache = optimize;
    p.generateLods = generateLods;
    p.lodRatios = lodRatios;
    p.lodMaxError = lodMaxError;
    p.generateMeshlets = generateMeshlets;
    p.generateCollision = generateCollision;
    p.collisionSimplifyRatio = collisionSimplifyRatio;
    p.maxHullVertices = maxHullVertices;
    return p;
}

namespace detail {

void finalizeImportedMesh(MeshData& mesh, bool hasNormals, bool hasTangents, const ModelImportSettings& settings) {
    if (settings.flipUVs) {
        for (auto& a : mesh.attributes) {
            a.uv0.y = 1.0f - a.uv0.y;
            a.uv1.y = 1.0f - a.uv1.y;
        }
    }
    for (auto& sm : mesh.submeshes) {
        if (sm.lods.empty()) continue;
        std::span<u32> idx(mesh.indices.data() + sm.lods[0].indexOffset, sm.lods[0].indexCount);
        std::vector<u32> local(idx.begin(), idx.end());
        for (u32& i : local) i -= sm.vertexOffset;
        auto pos = std::span(mesh.positions).subspan(sm.vertexOffset, sm.vertexCount);
        auto attr = std::span(mesh.attributes).subspan(sm.vertexOffset, sm.vertexCount);
        if (!hasNormals) generateNormals(pos, attr, local);
        if (!hasTangents && settings.generateTangents) generateTangents(pos, attr, local);
    }
    if (!settings.weldVertices) return;
    // Weld exact duplicates per submesh (assimp/glTF often duplicate vertices per face corner).
    std::vector<glm::vec3> positions;
    std::vector<VertexAttributes> attributes;
    std::vector<SkinVertex> skin;
    std::vector<u32> indices;
    for (auto& sm : mesh.submeshes) {
        if (sm.lods.empty()) continue;
        std::vector<u32> local(mesh.indices.begin() + sm.lods[0].indexOffset,
                               mesh.indices.begin() + sm.lods[0].indexOffset + sm.lods[0].indexCount);
        for (u32& i : local) i -= sm.vertexOffset;
        meshopt_Stream streams[3] = {
            {&mesh.positions[sm.vertexOffset], sizeof(glm::vec3), sizeof(glm::vec3)},
            {&mesh.attributes[sm.vertexOffset], sizeof(VertexAttributes), sizeof(VertexAttributes)},
            {mesh.skin.empty() ? nullptr : &mesh.skin[sm.vertexOffset], sizeof(SkinVertex), sizeof(SkinVertex)},
        };
        const usize streamCount = mesh.skin.empty() ? 2 : 3;
        std::vector<u32> remap(sm.vertexCount);
        const usize unique = meshopt_generateVertexRemapMulti(remap.data(), local.data(), local.size(), sm.vertexCount,
                                                              streams, streamCount);
        const u32 base = u32(positions.size());
        positions.resize(base + unique);
        attributes.resize(base + unique);
        if (!mesh.skin.empty()) skin.resize(base + unique);
        for (u32 v = 0; v < sm.vertexCount; ++v) {
            if (remap[v] == ~0u) continue;
            positions[base + remap[v]] = mesh.positions[sm.vertexOffset + v];
            attributes[base + remap[v]] = mesh.attributes[sm.vertexOffset + v];
            if (!mesh.skin.empty()) skin[base + remap[v]] = mesh.skin[sm.vertexOffset + v];
        }
        sm.lods.resize(1);
        sm.lods[0].indexOffset = u32(indices.size());
        for (u32 i : local) indices.push_back(remap[i] + base);
        sm.vertexOffset = base;
        sm.vertexCount = u32(unique);
    }
    mesh.positions = std::move(positions);
    mesh.attributes = std::move(attributes);
    mesh.skin = std::move(skin);
    mesh.indices = std::move(indices);
}

} // namespace detail

Result<ImportedModel> importModel(const std::filesystem::path& path, const ModelImportSettings& settings) {
    OX_PROFILE_ZONE();
    const std::string ext = detail::toLower(path.extension().string());
    if (ext == ".gltf" || ext == ".glb") return importModelGltf(path, settings);
    return importModelAssimp(path, settings);
}

MeshData mergeModel(const ImportedModel& model, std::vector<i32>* slotMaterials) {
    MeshData out;
    out.name = model.nodes.empty() ? "Model" : model.nodes[0].name;
    // World transforms (nodes are ordered parents first).
    std::vector<glm::mat4> world(model.nodes.size());
    for (usize i = 0; i < model.nodes.size(); ++i) {
        const auto& n = model.nodes[i];
        const glm::mat4 local = n.local.toMatrix();
        world[i] = n.parent >= 0 ? world[usize(n.parent)] * local : local;
    }
    // One submesh per (material) across all instances; material slots keyed by model material index.
    std::vector<i64> slotOfMaterial;
    auto slotFor = [&](i64 materialIndex) -> u32 {
        for (u32 s = 0; s < slotOfMaterial.size(); ++s) {
            if (slotOfMaterial[s] == materialIndex) return s;
        }
        slotOfMaterial.push_back(materialIndex);
        MaterialSlot slot;
        slot.name = materialIndex >= 0 && usize(materialIndex) < model.materials.size()
                        ? model.materials[usize(materialIndex)].name
                        : "Default";
        out.materials.push_back(slot);
        return u32(slotOfMaterial.size() - 1);
    };
    std::vector<std::vector<u32>> slotIndices;
    for (usize ni = 0; ni < model.nodes.size(); ++ni) {
        const glm::mat4& m = world[ni];
        const glm::mat3 nm = glm::transpose(glm::inverse(glm::mat3(m)));
        const bool flip = glm::determinant(glm::mat3(m)) < 0.0f;
        for (u32 meshIndex : model.nodes[ni].meshes) {
            const MeshData& src = model.meshes[meshIndex];
            for (const auto& sm : src.submeshes) {
                if (sm.lods.empty()) continue;
                const i64 mat = meshIndex < model.meshMaterials.size() && sm.materialSlot < model.meshMaterials[meshIndex].size()
                                    ? i64(i32(model.meshMaterials[meshIndex][sm.materialSlot]))
                                    : -1;
                const u32 slot = slotFor(mat);
                if (slotIndices.size() <= slot) slotIndices.resize(slot + 1);
                const u32 base = u32(out.positions.size());
                for (u32 v = 0; v < sm.vertexCount; ++v) {
                    out.positions.push_back(glm::vec3(m * glm::vec4(src.positions[sm.vertexOffset + v], 1.0f)));
                    VertexAttributes a = src.attributes[sm.vertexOffset + v];
                    a.normal = glm::normalize(nm * a.normal);
                    a.tangent = glm::vec4(glm::normalize(glm::mat3(m) * glm::vec3(a.tangent)), a.tangent.w * (flip ? -1.0f : 1.0f));
                    out.attributes.push_back(a);
                    if (!src.skin.empty()) out.skin.push_back(src.skin[sm.vertexOffset + v]);
                }
                const auto& l = sm.lods[0];
                for (u32 t = 0; t < l.indexCount; t += 3) {
                    const u32 a = src.indices[l.indexOffset + t] - sm.vertexOffset + base;
                    const u32 b = src.indices[l.indexOffset + t + 1] - sm.vertexOffset + base;
                    const u32 c = src.indices[l.indexOffset + t + 2] - sm.vertexOffset + base;
                    slotIndices[slot].insert(slotIndices[slot].end(), {a, flip ? c : b, flip ? b : c});
                }
            }
        }
    }
    if (!out.skin.empty() && out.skin.size() != out.positions.size()) out.skin.clear();
    // Submeshes need contiguous vertex ranges: one submesh per slot spanning all vertices it references.
    MeshData result;
    result.name = out.name;
    result.materials = out.materials;
    for (u32 s = 0; s < slotIndices.size(); ++s) {
        auto& idx = slotIndices[s];
        if (idx.empty()) continue;
        std::vector<u32> remap(out.positions.size(), ~0u);
        Submesh sm;
        sm.name = out.materials[s].name;
        sm.materialSlot = s;
        sm.vertexOffset = u32(result.positions.size());
        MeshLod lod;
        lod.indexOffset = u32(result.indices.size());
        for (u32 i : idx) {
            if (remap[i] == ~0u) {
                remap[i] = u32(result.positions.size());
                result.positions.push_back(out.positions[i]);
                result.attributes.push_back(out.attributes[i]);
                if (!out.skin.empty()) result.skin.push_back(out.skin[i]);
            }
            result.indices.push_back(remap[i]);
        }
        lod.indexCount = u32(idx.size());
        sm.vertexCount = u32(result.positions.size()) - sm.vertexOffset;
        sm.lods.push_back(lod);
        result.submeshes.push_back(std::move(sm));
    }
    if (slotMaterials) {
        slotMaterials->clear();
        for (i64 m : slotOfMaterial) slotMaterials->push_back(i32(m));
    }
    computeBounds(result);
    return result;
}

Result<MeshData> importMeshFile(const std::filesystem::path& path, const ModelImportSettings& settings) {
    auto model = importModel(path, settings);
    if (!model) return model.error();
    MeshData mesh = mergeModel(*model);
    processMesh(mesh, settings.processSettings());
    return mesh;
}

} // namespace ox::assets
