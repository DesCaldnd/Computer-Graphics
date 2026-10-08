#include <oxwald/assets/image.hpp>
#include <oxwald/assets/model_import.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>

#include "internal.hpp"
#include "model_common.hpp"

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <exception>
#include <map>

namespace ox::assets {

namespace {

glm::mat4 toGlm(const aiMatrix4x4& m) {
    // assimp is row-major (a1..a4 = first row); glm is column-major.
    return glm::transpose(glm::mat4(m.a1, m.a2, m.a3, m.a4, m.b1, m.b2, m.b3, m.b4, m.c1, m.c2, m.c3, m.c4, m.d1, m.d2,
                                    m.d3, m.d4));
}

struct AssimpContext {
    const aiScene* scene;
    std::filesystem::path dir;
    ImportedModel& model;
    std::map<std::string, i32> embedded;
};

ModelTextureRef textureRef(AssimpContext& a, const aiMaterial* m, aiTextureType type) {
    ModelTextureRef ref;
    if (m->GetTextureCount(type) == 0) return ref;
    aiString path;
    if (m->GetTexture(type, 0, &path) != AI_SUCCESS || path.length == 0) return ref;
    std::string p = path.C_Str();
    if (const aiTexture* tex = a.scene->GetEmbeddedTexture(p.c_str())) {
        if (auto it = a.embedded.find(p); it != a.embedded.end()) {
            ref.embedded = it->second;
            return ref;
        }
        EmbeddedImage e;
        e.name = tex->mFilename.length ? std::filesystem::path(tex->mFilename.C_Str()).stem().string() : "embedded" + p;
        if (tex->mHeight == 0) { // compressed file in memory
            e.extension = tex->achFormatHint[0] ? std::string(tex->achFormatHint) : "png";
            const auto* bytes = reinterpret_cast<const std::byte*>(tex->pcData);
            e.data.assign(bytes, bytes + tex->mWidth);
        } else { // raw BGRA texels: re-encode as PNG
            Image img(tex->mWidth, tex->mHeight);
            for (usize i = 0; i < img.pixels.size(); ++i) {
                const aiTexel& t = tex->pcData[i];
                img.pixels[i] = glm::vec4(t.r, t.g, t.b, t.a) / 255.0f;
            }
            e.extension = "png";
            e.data = encodePng(img);
        }
        ref.embedded = i32(a.model.embeddedImages.size());
        a.embedded[p] = ref.embedded;
        a.model.embeddedImages.push_back(std::move(e));
        return ref;
    }
    std::replace(p.begin(), p.end(), '\\', '/');
    // Absolute paths from the authoring machine (e.g. "E:/textures/x.png"): fall back to the file name.
    std::filesystem::path fp(p);
    if (fp.is_absolute() || (p.size() > 1 && p[1] == ':') || !std::filesystem::exists(a.dir / fp)) {
        if (std::filesystem::exists(a.dir / fp.filename())) {
            p = fp.filename().generic_string();
        } else if (fp.is_absolute() || (p.size() > 1 && p[1] == ':')) {
            OX_LOG_WARN("assets", "texture '{}' not found next to the model", p);
            return ref;
        }
    }
    ref.path = p;
    return ref;
}

ImportedMaterial convertMaterial(AssimpContext& a, const aiMaterial* m) {
    ImportedMaterial out;
    aiString name;
    if (m->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) out.name = name.C_Str();
    MaterialAsset& mat = out.material;
    aiColor4D c;
    if (m->Get(AI_MATKEY_BASE_COLOR, c) == AI_SUCCESS || m->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS) {
        mat.baseColor = glm::vec4(c.r, c.g, c.b, 1.0f);
    }
    f32 opacity = 1.0f;
    if (m->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 0.999f) {
        mat.baseColor.a = opacity;
        mat.blendMode = BlendMode::Transparent;
    }
    f32 v = 0.0f;
    if (m->Get(AI_MATKEY_METALLIC_FACTOR, v) == AI_SUCCESS) mat.metallic = v;
    if (m->Get(AI_MATKEY_ROUGHNESS_FACTOR, v) == AI_SUCCESS) {
        mat.roughness = v;
    } else if (m->Get(AI_MATKEY_SHININESS, v) == AI_SUCCESS && v > 0.0f) {
        mat.roughness = glm::clamp(std::sqrt(2.0f / (v + 2.0f)), 0.04f, 1.0f); // Blinn-Phong exponent -> roughness
    }
    aiColor3D e;
    if (m->Get(AI_MATKEY_COLOR_EMISSIVE, e) == AI_SUCCESS) mat.emissive = glm::vec3(e.r, e.g, e.b);
    if (m->Get(AI_MATKEY_EMISSIVE_INTENSITY, v) == AI_SUCCESS) mat.emissiveStrength = v;
    if (m->Get(AI_MATKEY_REFRACTI, v) == AI_SUCCESS && v >= 1.0f) mat.ior = v;
    if (m->Get(AI_MATKEY_TRANSMISSION_FACTOR, v) == AI_SUCCESS && v > 0.0f) {
        mat.transmission = v;
        mat.blendMode = BlendMode::Refractive;
    }
    int twoSided = 0;
    if (m->Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS) mat.doubleSided = twoSided != 0;
    aiString alphaMode;
    if (m->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
        if (std::string(alphaMode.C_Str()) == "MASK") mat.blendMode = BlendMode::AlphaTest;
        if (std::string(alphaMode.C_Str()) == "BLEND") mat.blendMode = BlendMode::Transparent;
    }
    if (m->Get(AI_MATKEY_GLTF_ALPHACUTOFF, v) == AI_SUCCESS) mat.alphaCutoff = v;

    out.albedo = textureRef(a, m, aiTextureType_BASE_COLOR);
    if (!out.albedo.valid()) out.albedo = textureRef(a, m, aiTextureType_DIFFUSE);
    out.normal = textureRef(a, m, aiTextureType_NORMALS);
    if (!out.normal.valid()) {
        // OBJ "bump"/"map_Bump" arrive as HEIGHT; they are normal maps in practice when named so.
        auto h = textureRef(a, m, aiTextureType_HEIGHT);
        if (h.valid() && detail::toLower(h.path).find("normal") != std::string::npos) out.normal = h;
    }
    out.orm = textureRef(a, m, aiTextureType_GLTF_METALLIC_ROUGHNESS);
    if (!out.orm.valid()) out.orm = textureRef(a, m, aiTextureType_UNKNOWN);
    out.emissive = textureRef(a, m, aiTextureType_EMISSIVE);
    out.occlusion = textureRef(a, m, aiTextureType_AMBIENT_OCCLUSION);
    if (out.orm.valid()) {
        if (out.occlusion.valid() && out.occlusion.path == out.orm.path && out.occlusion.embedded == out.orm.embedded) {
            out.occlusion = {};
        } else {
            mat.occlusionStrength = 0.0f; // ORM.r undefined
        }
    }
    // Alpha-tested diffuse (OBJ map_d pointing at the diffuse texture).
    auto opacityTex = textureRef(a, m, aiTextureType_OPACITY);
    if (opacityTex.valid() && mat.blendMode == BlendMode::Opaque) mat.blendMode = BlendMode::AlphaTest;
    return out;
}

UpAxis detectUpAxis(const aiScene* scene) {
    if (!scene->mMetaData) return UpAxis::Y;
    int up = 1;
    if (scene->mMetaData->Get("UpAxis", up)) {
        if (up == 2) return UpAxis::Z;
        if (up == 0) return UpAxis::X;
    }
    return UpAxis::Y;
}

} // namespace

Result<ImportedModel> importModelAssimp(const std::filesystem::path& path, const ModelImportSettings& settings) {
    OX_PROFILE_ZONE();
    Assimp::Importer importer;
    // Keep FBX pivots out of the hierarchy and let us do unit/axis conversion.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 4);
    unsigned flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals |
                     aiProcess_SortByPType | aiProcess_LimitBoneWeights | aiProcess_FlipUVs |
                     aiProcess_ValidateDataStructure | aiProcess_RemoveRedundantMaterials;
    const aiScene* scene = nullptr;
    try {
        scene = importer.ReadFile(path.string(), flags);
    } catch (const std::exception& e) {
        return makeError("{}: assimp exception: {}", path.string(), e.what());
    }
    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        return makeError("{}: {}", path.string(), importer.GetErrorString());
    }

    ImportedModel model;
    model.importer = "assimp";
    AssimpContext a{scene, path.parent_path(), model, {}};

    f32 unit = settings.unitToMeters;
    if (unit <= 0.0f) {
        double factor = 0.0;
        if (scene->mMetaData && scene->mMetaData->Get("UnitScaleFactor", factor) && factor > 0.0) {
            unit = f32(factor * 0.01); // FBX: centimetres per unit
        }
    }
    const UpAxis up = settings.upAxis == UpAxis::Auto ? detectUpAxis(scene) : settings.upAxis;
    const auto conv = detail::AxisConversion::make(up, unit, settings.scale);

    if (settings.importMaterials) {
        for (u32 i = 0; i < scene->mNumMaterials; ++i) model.materials.push_back(convertMaterial(a, scene->mMaterials[i]));
    }

    std::vector<std::string> jointNames;
    auto jointIndex = [&](const std::string& name) -> u16 {
        for (usize i = 0; i < jointNames.size(); ++i) {
            if (jointNames[i] == name) return u16(i);
        }
        jointNames.push_back(name);
        return u16(jointNames.size() - 1);
    };

    // One MeshData per distinct set of aiMeshes referenced by a node (assimp splits meshes per material).
    std::map<std::vector<u32>, u32> meshForSet;
    auto buildMesh = [&](const std::vector<u32>& set, const std::string& name) -> u32 {
        if (auto it = meshForSet.find(set); it != meshForSet.end()) return it->second;
        MeshData mesh;
        mesh.name = name;
        std::vector<u32> slotMaterials;
        bool hasNormals = true;
        bool anySkin = false;
        for (u32 mi : set) anySkin |= scene->mMeshes[mi]->HasBones();
        for (u32 mi : set) {
            const aiMesh* am = scene->mMeshes[mi];
            if (!(am->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
            const u32 base = mesh.vertexCount();
            for (u32 v = 0; v < am->mNumVertices; ++v) {
                mesh.positions.emplace_back(am->mVertices[v].x, am->mVertices[v].y, am->mVertices[v].z);
                VertexAttributes attr;
                if (am->HasNormals()) attr.normal = glm::vec3(am->mNormals[v].x, am->mNormals[v].y, am->mNormals[v].z);
                if (am->HasTextureCoords(0)) attr.uv0 = glm::vec2(am->mTextureCoords[0][v].x, am->mTextureCoords[0][v].y);
                if (am->HasTextureCoords(1)) attr.uv1 = glm::vec2(am->mTextureCoords[1][v].x, am->mTextureCoords[1][v].y);
                if (am->HasVertexColors(0)) {
                    const aiColor4D& c = am->mColors[0][v];
                    attr.color = packColor(glm::vec4(c.r, c.g, c.b, c.a));
                }
                mesh.attributes.push_back(attr);
            }
            hasNormals &= am->HasNormals();
            if (anySkin) {
                mesh.skin.resize(mesh.positions.size(), SkinVertex{});
                std::vector<std::array<std::pair<f32, u16>, 4>> influences(am->mNumVertices);
                for (auto& inf : influences) inf.fill({0.0f, 0});
                for (u32 b = 0; b < am->mNumBones; ++b) {
                    const aiBone* bone = am->mBones[b];
                    const u16 joint = jointIndex(bone->mName.C_Str());
                    for (u32 w = 0; w < bone->mNumWeights; ++w) {
                        auto& inf = influences[bone->mWeights[w].mVertexId];
                        auto smallest = std::min_element(inf.begin(), inf.end(),
                                                         [](auto& x, auto& y) { return x.first < y.first; });
                        if (bone->mWeights[w].mWeight > smallest->first) *smallest = {bone->mWeights[w].mWeight, joint};
                    }
                }
                for (u32 v = 0; v < am->mNumVertices; ++v) {
                    auto& inf = influences[v];
                    f32 sum = 0.0f;
                    for (auto& [w, j] : inf) sum += w;
                    SkinVertex sv;
                    u32 total = 0;
                    for (int k = 0; k < 4; ++k) {
                        sv.joints[k] = inf[k].second;
                        sv.weights[k] = sum > 0.0f ? u16(std::lround(inf[k].first / sum * 65535.0f)) : (k == 0 ? 65535 : 0);
                        total += sv.weights[k];
                    }
                    sv.weights[0] = u16(i64(sv.weights[0]) + 65535 - i64(total));
                    mesh.skin[base + v] = sv;
                }
                model.hasSkin = true;
            }
            Submesh sm;
            sm.name = am->mName.length ? am->mName.C_Str() : name;
            sm.vertexOffset = base;
            sm.vertexCount = am->mNumVertices;
            auto slotIt = std::find(slotMaterials.begin(), slotMaterials.end(), am->mMaterialIndex);
            sm.materialSlot = u32(slotIt - slotMaterials.begin());
            if (slotIt == slotMaterials.end()) {
                slotMaterials.push_back(settings.importMaterials ? am->mMaterialIndex : ~0u);
                MaterialSlot slot;
                slot.name = am->mMaterialIndex < model.materials.size() ? model.materials[am->mMaterialIndex].name : "Default";
                mesh.materials.push_back(slot);
            }
            MeshLod lod;
            lod.indexOffset = u32(mesh.indices.size());
            for (u32 f = 0; f < am->mNumFaces; ++f) {
                const aiFace& face = am->mFaces[f];
                if (face.mNumIndices != 3) continue;
                for (u32 k = 0; k < 3; ++k) mesh.indices.push_back(face.mIndices[k] + base);
            }
            lod.indexCount = u32(mesh.indices.size()) - lod.indexOffset;
            sm.lods.push_back(lod);
            mesh.submeshes.push_back(std::move(sm));
        }
        conv.apply(mesh);
        // assimp tangents depend on post-process order; generate our own consistently after UV flip/conversion.
        detail::finalizeImportedMesh(mesh, hasNormals, false, settings);
        const u32 index = u32(model.meshes.size());
        model.meshes.push_back(std::move(mesh));
        model.meshMaterials.push_back(slotMaterials);
        meshForSet[set] = index;
        return index;
    };

    std::vector<std::pair<const aiNode*, i32>> stack{{scene->mRootNode, -1}};
    while (!stack.empty()) {
        auto [n, parent] = stack.back();
        stack.pop_back();
        ModelNode node;
        node.name = n->mName.length ? n->mName.C_Str() : "Node";
        node.parent = parent;
        Transform t;
        decompose(toGlm(n->mTransformation), t.position, t.rotation, t.scale);
        node.local = conv.node(t);
        if (n->mNumMeshes > 0) {
            std::vector<u32> set(n->mMeshes, n->mMeshes + n->mNumMeshes);
            node.meshes.push_back(buildMesh(set, node.name));
        }
        const i32 self = i32(model.nodes.size());
        model.nodes.push_back(std::move(node));
        for (u32 c = n->mNumChildren; c-- > 0;) stack.emplace_back(n->mChildren[c], self);
    }
    model.jointNames = jointNames;
    model.hasAnimations = scene->mNumAnimations > 0;
    return model;
}

} // namespace ox::assets
