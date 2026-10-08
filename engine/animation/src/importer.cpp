#include <oxwald/animation/importer.hpp>

#include <oxwald/core/log.hpp>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace ox::anim {

namespace {

glm::mat4 toGlm(const aiMatrix4x4& m) {
    // assimp is row-major, glm column-major.
    return glm::mat4(m.a1, m.b1, m.c1, m.d1, m.a2, m.b2, m.c2, m.d2, m.a3, m.b3, m.c3, m.d3, m.a4, m.b4, m.c4, m.d4);
}

glm::vec3 toGlm(const aiVector3D& v) { return {v.x, v.y, v.z}; }
glm::quat toGlm(const aiQuaternion& q) { return glm::normalize(glm::quat(q.w, q.x, q.y, q.z)); }

glm::mat4 globalTransform(const aiNode* node) {
    glm::mat4 m(1.0f);
    for (const aiNode* n = node; n; n = n->mParent) {
        m = toGlm(n->mTransformation) * m;
    }
    return m;
}

// Source → engine (Y-up) rotation and uniform scale.
struct Conversion {
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    f32 scale = 1.0f;

    glm::mat4 matrix() const {
        glm::mat4 m = glm::mat4_cast(rotation);
        return m * glm::mat4(glm::mat3(scale));
    }
};

template <class T>
bool metaGet(const aiScene* scene, const char* key, T& out) {
    return scene->mMetaData && scene->mMetaData->Get(key, out);
}

Conversion computeConversion(const aiScene* scene, const ImportSettings& settings) {
    Conversion c;
    UpAxis up = settings.sourceUpAxis;
    if (up == UpAxis::Auto) {
        i32 upAxis = 1, upSign = 1, frontAxis = 2, frontSign = 1, coordAxis = 0, coordSign = 1;
        const bool hasAxes = metaGet(scene, "UpAxis", upAxis) && metaGet(scene, "UpAxisSign", upSign) &&
                             metaGet(scene, "FrontAxis", frontAxis) && metaGet(scene, "FrontAxisSign", frontSign) &&
                             metaGet(scene, "CoordAxis", coordAxis) && metaGet(scene, "CoordAxisSign", coordSign);
        if (hasAxes && upAxis >= 0 && upAxis < 3 && frontAxis >= 0 && frontAxis < 3 && coordAxis >= 0 &&
            coordAxis < 3) {
            glm::vec3 coordV(0.0f), upV(0.0f), frontV(0.0f);
            coordV[coordAxis] = static_cast<f32>(coordSign);
            upV[upAxis] = static_cast<f32>(upSign);
            frontV[frontAxis] = static_cast<f32>(frontSign);
            // Rows = engine X/Y/Z expressed in source axes.
            const glm::mat3 m = glm::transpose(glm::mat3(coordV, upV, frontV));
            if (glm::determinant(m) > 0.5f) {
                c.rotation = glm::normalize(glm::quat_cast(m));
            } else {
                OX_LOG_WARN("animation", "import: left-handed source axis system ignored");
            }
        }
    } else if (up == UpAxis::Z) {
        c.rotation = glm::angleAxis(-glm::half_pi<f32>(), glm::vec3(1, 0, 0)); // +Z → +Y
    } else if (up == UpAxis::X) {
        c.rotation = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1)); // +X → +Y
    }

    f32 unit = 1.0f;
    if (settings.unitToMeters > 0.0f) {
        unit = settings.unitToMeters;
    } else {
        double fbxUnit = 0.0;
        float fbxUnitF = 0.0f;
        // FBX: UnitScaleFactor is centimetres per file unit.
        if (metaGet(scene, "UnitScaleFactor", fbxUnit) && fbxUnit > 0.0) {
            unit = static_cast<f32>(fbxUnit) * 0.01f;
        } else if (metaGet(scene, "UnitScaleFactor", fbxUnitF) && fbxUnitF > 0.0f) {
            unit = fbxUnitF * 0.01f;
        }
    }
    c.scale = unit * settings.scale;
    return c;
}

std::optional<AnimationImport> convertScene(const aiScene* scene, const ImportSettings& settings, std::string* error) {
    auto fail = [&](std::string msg) -> std::optional<AnimationImport> {
        OX_LOG_ERROR("animation", "import: {}", msg);
        if (error) *error = std::move(msg);
        return std::nullopt;
    };
    if (!scene || !scene->mRootNode) {
        return fail("scene has no root node");
    }

    // 1. Nodes that must be joints: bones, else animated nodes.
    std::unordered_map<std::string, glm::mat4> offsets;
    for (u32 m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        for (u32 b = 0; b < mesh->mNumBones; ++b) {
            offsets.try_emplace(mesh->mBones[b]->mName.C_Str(), toGlm(mesh->mBones[b]->mOffsetMatrix));
        }
    }
    std::unordered_set<std::string> neededNames;
    for (const auto& [name, _] : offsets) neededNames.insert(name);
    if (neededNames.empty()) {
        for (u32 a = 0; a < scene->mNumAnimations; ++a) {
            const aiAnimation* anim = scene->mAnimations[a];
            for (u32 c = 0; c < anim->mNumChannels; ++c) neededNames.insert(anim->mChannels[c]->mNodeName.C_Str());
        }
    }
    std::vector<const aiNode*> needed;
    for (const auto& name : neededNames) {
        if (const aiNode* n = scene->mRootNode->FindNode(name.c_str())) needed.push_back(n);
    }
    if (needed.empty()) {
        return fail("no skeleton (no bones or animated nodes)");
    }

    // 2. Lowest common ancestor and the set of joint nodes (needed + in-between ancestors).
    auto pathToRoot = [](const aiNode* n) {
        std::vector<const aiNode*> p;
        for (; n; n = n->mParent) p.push_back(n);
        std::reverse(p.begin(), p.end());
        return p;
    };
    std::vector<const aiNode*> lcaPath = pathToRoot(needed[0]);
    for (usize i = 1; i < needed.size(); ++i) {
        const auto p = pathToRoot(needed[i]);
        usize k = 0;
        while (k < lcaPath.size() && k < p.size() && lcaPath[k] == p[k]) ++k;
        lcaPath.resize(k);
    }
    const aiNode* lca = lcaPath.empty() ? scene->mRootNode : lcaPath.back();
    std::unordered_set<const aiNode*> jointSet;
    for (const aiNode* n : needed) {
        for (const aiNode* a = n; a && a != lca; a = a->mParent) jointSet.insert(a);
    }
    if (neededNames.contains(lca->mName.C_Str())) jointSet.insert(lca);
    const bool lcaIsJoint = jointSet.contains(lca);
    const aiNode* baseNode = lcaIsJoint ? lca->mParent : lca;
    const Transform base = Transform::fromMatrix(baseNode ? globalTransform(baseNode) : glm::mat4(1.0f));
    const Conversion conv = computeConversion(scene, settings);

    AnimationImport out;
    Skeleton& skel = out.skeleton;
    std::unordered_map<std::string, i32> jointByName;
    std::function<void(const aiNode*, i32)> visit = [&](const aiNode* node, i32 parent) {
        i32 self = parent;
        if (jointSet.contains(node)) {
            Transform local = Transform::fromMatrix(toGlm(node->mTransformation));
            if (parent == kNoJoint) {
                local = base * local;
                local.translation = conv.rotation * (local.translation * conv.scale);
                local.rotation = glm::normalize(conv.rotation * local.rotation);
            } else {
                local.translation *= conv.scale;
            }
            self = skel.addJoint(node->mName.C_Str(), parent, local);
            jointByName[node->mName.C_Str()] = self;
        }
        for (u32 c = 0; c < node->mNumChildren; ++c) visit(node->mChildren[c], self);
    };
    if (lcaIsJoint) {
        visit(lca, kNoJoint);
    } else {
        for (u32 c = 0; c < lca->mNumChildren; ++c) visit(lca->mChildren[c], kNoJoint);
    }
    if (skel.empty()) {
        return fail("skeleton is empty");
    }
    if (skel.jointCount() > 0xFFFF) {
        return fail("too many joints");
    }

    // Inverse binds: invBind' = Sc · invBind · C⁻¹ keeps skinning consistent after converting mesh + bones.
    const glm::mat4 C = conv.matrix();
    const glm::mat4 Cinv = glm::inverse(C);
    const glm::mat4 Sc = glm::mat4(glm::mat3(conv.scale));
    for (const auto& [name, offset] : offsets) {
        if (auto it = jointByName.find(name); it != jointByName.end()) {
            skel.setInverseBind(it->second, Sc * offset * Cinv);
        }
    }
    skel.finalize();

    // 3. Meshes.
    if (settings.importMeshes) {
        const u32 maxInf = settings.maxInfluences > 4 ? 8u : 4u;
        const glm::mat3 R = glm::mat3_cast(conv.rotation);
        for (u32 m = 0; m < scene->mNumMeshes; ++m) {
            const aiMesh* mesh = scene->mMeshes[m];
            if (mesh->mNumBones == 0 || !(mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
            SkinnedMeshData data;
            data.name = mesh->mName.C_Str();
            data.materialIndex = static_cast<i32>(mesh->mMaterialIndex);
            const u32 nv = mesh->mNumVertices;
            data.positions.resize(nv);
            for (u32 v = 0; v < nv; ++v) data.positions[v] = glm::vec3(C * glm::vec4(toGlm(mesh->mVertices[v]), 1.0f));
            if (mesh->HasNormals()) {
                data.normals.resize(nv);
                for (u32 v = 0; v < nv; ++v) data.normals[v] = glm::normalize(R * toGlm(mesh->mNormals[v]));
            }
            if (mesh->HasTangentsAndBitangents() && mesh->HasNormals()) {
                data.tangents.resize(nv);
                for (u32 v = 0; v < nv; ++v) {
                    const glm::vec3 n = toGlm(mesh->mNormals[v]), t = toGlm(mesh->mTangents[v]),
                                    b = toGlm(mesh->mBitangents[v]);
                    const f32 w = glm::dot(glm::cross(n, t), b) < 0.0f ? -1.0f : 1.0f;
                    data.tangents[v] = glm::vec4(glm::normalize(R * t), w);
                }
            }
            if (mesh->HasTextureCoords(0)) {
                data.uvs.resize(nv);
                for (u32 v = 0; v < nv; ++v) data.uvs[v] = {mesh->mTextureCoords[0][v].x, mesh->mTextureCoords[0][v].y};
            }
            for (u32 f = 0; f < mesh->mNumFaces; ++f) {
                const aiFace& face = mesh->mFaces[f];
                if (face.mNumIndices != 3) continue;
                data.indices.insert(data.indices.end(), face.mIndices, face.mIndices + 3);
            }
            std::vector<std::vector<std::pair<u16, f32>>> influences(nv);
            for (u32 b = 0; b < mesh->mNumBones; ++b) {
                const aiBone* bone = mesh->mBones[b];
                const auto it = jointByName.find(bone->mName.C_Str());
                if (it == jointByName.end()) continue;
                for (u32 w = 0; w < bone->mNumWeights; ++w) {
                    const aiVertexWeight& vw = bone->mWeights[w];
                    if (vw.mVertexId < nv) influences[vw.mVertexId].emplace_back(static_cast<u16>(it->second), vw.mWeight);
                }
            }
            data.setInfluences(influences, maxInf, 0);
            out.meshes.push_back(std::move(data));
        }
    }

    // 4. Animations.
    if (settings.importAnimations) {
        for (u32 a = 0; a < scene->mNumAnimations; ++a) {
            const aiAnimation* anim = scene->mAnimations[a];
            const f64 tps = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;
            AnimationClip clip;
            clip.name = anim->mName.length > 0 ? anim->mName.C_Str() : std::format("clip_{}", a);
            clip.tracks.resize(skel.jointCount());
            for (u32 c = 0; c < anim->mNumChannels; ++c) {
                const aiNodeAnim* ch = anim->mChannels[c];
                const auto it = jointByName.find(ch->mNodeName.C_Str());
                if (it == jointByName.end()) continue;
                const i32 j = it->second;
                const bool isRoot = skel.parent(j) == kNoJoint;
                JointTrack& jt = clip.tracks[static_cast<usize>(j)];

                bool allStep = ch->mNumPositionKeys > 0;
                for (u32 k = 0; k < ch->mNumPositionKeys; ++k) {
                    const aiVectorKey& key = ch->mPositionKeys[k];
                    glm::vec3 t = toGlm(key.mValue);
                    if (isRoot) {
                        t = conv.rotation * (base.transformPoint(t) * conv.scale);
                    } else {
                        t *= conv.scale;
                    }
                    jt.translation.times.push_back(static_cast<f32>(key.mTime / tps));
                    jt.translation.values.push_back(t);
                    allStep = allStep && key.mInterpolation == aiAnimInterpolation_Step;
                }
                jt.translation.interpolation = allStep ? Interpolation::Step : Interpolation::Linear;

                allStep = ch->mNumRotationKeys > 0;
                for (u32 k = 0; k < ch->mNumRotationKeys; ++k) {
                    const aiQuatKey& key = ch->mRotationKeys[k];
                    glm::quat q = toGlm(key.mValue);
                    if (isRoot) q = glm::normalize(conv.rotation * base.rotation * q);
                    jt.rotation.times.push_back(static_cast<f32>(key.mTime / tps));
                    jt.rotation.values.push_back(q);
                    allStep = allStep && key.mInterpolation == aiAnimInterpolation_Step;
                }
                jt.rotation.interpolation = allStep ? Interpolation::Step : Interpolation::Linear;

                allStep = ch->mNumScalingKeys > 0;
                for (u32 k = 0; k < ch->mNumScalingKeys; ++k) {
                    const aiVectorKey& key = ch->mScalingKeys[k];
                    glm::vec3 s = toGlm(key.mValue);
                    if (isRoot) s *= base.scale;
                    jt.scale.times.push_back(static_cast<f32>(key.mTime / tps));
                    jt.scale.values.push_back(s);
                    allStep = allStep && key.mInterpolation == aiAnimInterpolation_Step;
                }
                jt.scale.interpolation = allStep ? Interpolation::Step : Interpolation::Linear;
            }
            clip.computeDuration();
            clip.duration = std::max(clip.duration, static_cast<f32>(anim->mDuration / tps));
            if (settings.fixQuaternionHemispheres) clip.fixQuaternionHemispheres();
            out.clips.push_back(std::move(clip));
        }
    }
    return out;
}

u32 importFlags() {
    return aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_LimitBoneWeights |
           aiProcess_GenSmoothNormals | aiProcess_SortByPType;
}

void configure(Assimp::Importer& importer, const ImportSettings& settings) {
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, settings.maxInfluences > 4 ? 8 : 4);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
}

} // namespace

std::optional<AnimationImport> importAnimationAsset(const std::filesystem::path& path, const ImportSettings& settings,
                                                    std::string* error) {
    try {
        Assimp::Importer importer;
        configure(importer, settings);
        const aiScene* scene = importer.ReadFile(path.string(), importFlags());
        if (!scene) {
            const std::string msg = std::format("{}: {}", path.string(), importer.GetErrorString());
            OX_LOG_ERROR("animation", "import failed: {}", msg);
            if (error) *error = msg;
            return std::nullopt;
        }
        return convertScene(scene, settings, error);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        OX_LOG_ERROR("animation", "import exception: {}", e.what());
        return std::nullopt;
    }
}

std::optional<AnimationImport> importAnimationAssetFromMemory(const void* data, usize size, const char* formatHint,
                                                              const ImportSettings& settings, std::string* error) {
    try {
        Assimp::Importer importer;
        configure(importer, settings);
        const aiScene* scene = importer.ReadFileFromMemory(data, size, importFlags(), formatHint);
        if (!scene) {
            if (error) *error = importer.GetErrorString();
            OX_LOG_ERROR("animation", "import failed: {}", importer.GetErrorString());
            return std::nullopt;
        }
        return convertScene(scene, settings, error);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        OX_LOG_ERROR("animation", "import exception: {}", e.what());
        return std::nullopt;
    }
}

} // namespace ox::anim
