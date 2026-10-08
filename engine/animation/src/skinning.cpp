#include <oxwald/animation/skinning.hpp>

#include <oxwald/core/assert.hpp>

#include <algorithm>

namespace ox::anim {

void SkinnedMeshData::setInfluences(const std::vector<std::vector<std::pair<u16, f32>>>& perVertex,
                                    u32 maxInfluences, u16 fallbackJoint) {
    OX_ASSERT(maxInfluences == 4 || maxInfluences == 8, "maxInfluences must be 4 or 8 (got {})", maxInfluences);
    influencesPerVertex = maxInfluences;
    const usize n = perVertex.size();
    joints.assign(n * maxInfluences, 0);
    weights.assign(n * maxInfluences, 0.0f);
    std::vector<std::pair<u16, f32>> list;
    for (usize v = 0; v < n; ++v) {
        list = perVertex[v];
        // Merge duplicate joints (assimp can emit the same bone twice after mesh joins).
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.first < b.first; });
        usize w = 0;
        for (usize i = 0; i < list.size(); ++i) {
            if (w > 0 && list[w - 1].first == list[i].first) {
                list[w - 1].second += list[i].second;
            } else {
                list[w++] = list[i];
            }
        }
        list.resize(w);
        std::erase_if(list, [](auto& p) { return !(p.second > 0.0f); });
        std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (list.size() > maxInfluences) {
            list.resize(maxInfluences);
        }
        f32 sum = 0.0f;
        for (auto& p : list) sum += p.second;
        const usize base = v * maxInfluences;
        if (list.empty() || sum <= 0.0f) {
            joints[base] = fallbackJoint;
            weights[base] = 1.0f;
            continue;
        }
        for (usize i = 0; i < list.size(); ++i) {
            joints[base + i] = list[i].first;
            weights[base + i] = list[i].second / sum;
        }
    }
}

void SkinnedMeshData::normalizeWeights() {
    const usize k = influencesPerVertex;
    for (usize base = 0; base + k <= weights.size(); base += k) {
        f32 sum = 0.0f;
        for (usize i = 0; i < k; ++i) sum += weights[base + i];
        if (sum <= 0.0f) {
            weights[base] = 1.0f;
            continue;
        }
        for (usize i = 0; i < k; ++i) weights[base + i] /= sum;
    }
}

void computeSkinningMatrices(const Skeleton& skeleton, std::span<const glm::mat4> model,
                             std::vector<glm::mat4>& palette) {
    const auto& inverseBind = skeleton.inverseBindMatrices();
    OX_ASSERT(model.size() == inverseBind.size(), "model/inverse bind size mismatch");
    palette.resize(model.size());
    for (usize i = 0; i < model.size(); ++i) {
        palette[i] = model[i] * inverseBind[i];
    }
}

void computeSkinningMatrices(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& palette) {
    std::vector<glm::mat4> model;
    localToModel(skeleton, pose, model);
    computeSkinningMatrices(skeleton, model, palette);
}

DualQuat DualQuat::fromRigid(const glm::quat& rotation, const glm::vec3& translation) {
    DualQuat d;
    d.real = glm::normalize(rotation);
    d.dual = glm::quat(0.0f, translation.x, translation.y, translation.z) * d.real * 0.5f;
    return d;
}

DualQuat DualQuat::fromMatrix(const glm::mat4& m) {
    const Transform t = Transform::fromMatrix(m);
    return fromRigid(t.rotation, t.translation);
}

glm::vec3 DualQuat::translation() const {
    const glm::quat t = dual * glm::conjugate(real) * 2.0f;
    return {t.x, t.y, t.z};
}

glm::vec3 DualQuat::transformPoint(const glm::vec3& p) const {
    return real * p + translation();
}

glm::vec3 DualQuat::transformVector(const glm::vec3& v) const {
    return real * v;
}

void computeDualQuatPalette(std::span<const glm::mat4> palette, std::vector<DualQuat>& out) {
    out.resize(palette.size());
    for (usize i = 0; i < palette.size(); ++i) {
        out[i] = DualQuat::fromMatrix(palette[i]);
    }
}

void skinMesh(const SkinnedMeshData& mesh, std::span<const glm::mat4> palette, SkinningMethod method,
              std::vector<glm::vec3>& outPositions, std::vector<glm::vec3>* outNormals,
              std::vector<glm::vec4>* outTangents) {
    const usize n = mesh.vertexCount();
    const usize k = mesh.influencesPerVertex;
    OX_ASSERT(mesh.joints.size() == n * k && mesh.weights.size() == n * k, "skin data size mismatch");
    outPositions.resize(n);
    const bool doNormals = outNormals && mesh.normals.size() == n;
    const bool doTangents = outTangents && mesh.tangents.size() == n;
    if (doNormals) outNormals->resize(n);
    if (doTangents) outTangents->resize(n);

    std::vector<DualQuat> dq;
    if (method == SkinningMethod::DualQuaternion) {
        computeDualQuatPalette(palette, dq);
    }

    for (usize v = 0; v < n; ++v) {
        const usize base = v * k;
        if (method == SkinningMethod::Linear) {
            glm::mat4 m(0.0f);
            for (usize i = 0; i < k; ++i) {
                const f32 w = mesh.weights[base + i];
                if (w > 0.0f) {
                    m += palette[mesh.joints[base + i]] * w;
                }
            }
            outPositions[v] = glm::vec3(m * glm::vec4(mesh.positions[v], 1.0f));
            const glm::mat3 m3(m);
            if (doNormals) (*outNormals)[v] = glm::normalize(m3 * mesh.normals[v]);
            if (doTangents) {
                const glm::vec4& t = mesh.tangents[v];
                (*outTangents)[v] = glm::vec4(glm::normalize(m3 * glm::vec3(t)), t.w);
            }
        } else {
            DualQuat acc;
            acc.real = glm::quat(0, 0, 0, 0);
            const glm::quat pivot = dq[mesh.joints[base]].real;
            for (usize i = 0; i < k; ++i) {
                const f32 w = mesh.weights[base + i];
                if (w <= 0.0f) continue;
                const DualQuat& d = dq[mesh.joints[base + i]];
                // Antipodality: keep all blended DQs in the hemisphere of the dominant one.
                const f32 s = glm::dot(pivot, d.real) < 0.0f ? -w : w;
                acc.real += d.real * s;
                acc.dual += d.dual * s;
            }
            const f32 len = glm::length(acc.real);
            acc.real /= len;
            acc.dual /= len;
            outPositions[v] = acc.transformPoint(mesh.positions[v]);
            if (doNormals) (*outNormals)[v] = glm::normalize(acc.transformVector(mesh.normals[v]));
            if (doTangents) {
                const glm::vec4& t = mesh.tangents[v];
                (*outTangents)[v] = glm::vec4(glm::normalize(acc.transformVector(glm::vec3(t))), t.w);
            }
        }
    }
}

} // namespace ox::anim
