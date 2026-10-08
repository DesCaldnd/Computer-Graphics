#include <oxwald/animation/pose.hpp>

#include <oxwald/core/assert.hpp>

namespace ox::anim {

JointMask JointMask::all(const Skeleton& skeleton, f32 weight) {
    JointMask m;
    m.weights.assign(skeleton.jointCount(), weight);
    return m;
}

void JointMask::setBranch(const Skeleton& skeleton, i32 rootJoint, f32 weight) {
    if (weights.size() != skeleton.jointCount()) {
        weights.assign(skeleton.jointCount(), 0.0f);
    }
    for (usize j = 0; j < skeleton.jointCount(); ++j) {
        if (skeleton.isDescendantOf(static_cast<i32>(j), rootJoint)) {
            weights[j] = weight;
        }
    }
}

JointMask JointMask::fromBranch(const Skeleton& skeleton, i32 rootJoint, f32 weight) {
    JointMask m = none(skeleton);
    m.setBranch(skeleton, rootJoint, weight);
    return m;
}

void localToModel(const Skeleton& skeleton, const Pose& pose, std::vector<Transform>& model) {
    model.resize(pose.size());
    updateModelFrom(skeleton, pose, model, 0);
}

void updateModelFrom(const Skeleton& skeleton, const Pose& pose, std::vector<Transform>& model, i32 firstJoint) {
    const auto& parents = skeleton.parents();
    OX_ASSERT(pose.size() == parents.size(), "pose/skeleton size mismatch {} vs {}", pose.size(), parents.size());
    model.resize(pose.size());
    for (usize i = static_cast<usize>(std::max(firstJoint, 0)); i < pose.size(); ++i) {
        model[i] = parents[i] == kNoJoint ? pose.local[i] : model[static_cast<usize>(parents[i])] * pose.local[i];
    }
}

void localToModel(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& model) {
    const auto& parents = skeleton.parents();
    OX_ASSERT(pose.size() == parents.size(), "pose/skeleton size mismatch {} vs {}", pose.size(), parents.size());
    model.resize(pose.size());
    for (usize i = 0; i < pose.size(); ++i) {
        const glm::mat4 local = pose.local[i].toMatrix();
        model[i] = parents[i] == kNoJoint ? local : model[static_cast<usize>(parents[i])] * local;
    }
}

void modelToLocal(const Skeleton& skeleton, const std::vector<Transform>& model, Pose& pose) {
    const auto& parents = skeleton.parents();
    pose.resize(model.size());
    for (usize i = 0; i < model.size(); ++i) {
        pose.local[i] = parents[i] == kNoJoint ? model[i] : model[static_cast<usize>(parents[i])].inverse() * model[i];
    }
}

void blendPoses(const Pose& a, const Pose& b, f32 weight, Pose& out, const JointMask* mask) {
    OX_ASSERT(a.size() == b.size(), "blendPoses size mismatch");
    out.resize(a.size());
    for (usize i = 0; i < a.size(); ++i) {
        const f32 w = weight * (mask ? mask->weight(i) : 1.0f);
        if (w <= 0.0f) {
            out.local[i] = a.local[i];
        } else if (w >= 1.0f) {
            out.local[i] = b.local[i];
        } else {
            out.local[i] = lerp(a.local[i], b.local[i], w);
        }
    }
}

std::vector<f32> blendPosesWeighted(std::span<const Pose* const> poses, std::span<const f32> weights, Pose& out) {
    OX_ASSERT(poses.size() == weights.size() && !poses.empty(), "blendPosesWeighted needs matching non-empty inputs");
    f32 total = 0.0f;
    for (f32 w : weights) {
        total += std::max(w, 0.0f);
    }
    std::vector<f32> normalized(weights.size(), 0.0f);
    if (total <= 0.0f) {
        out = *poses[0];
        normalized[0] = 1.0f;
        return normalized;
    }
    for (usize i = 0; i < weights.size(); ++i) {
        normalized[i] = std::max(weights[i], 0.0f) / total;
    }
    PoseAccumulator acc;
    acc.begin(poses[0]->size());
    for (usize i = 0; i < poses.size(); ++i) {
        if (normalized[i] > 0.0f) {
            acc.add(*poses[i], normalized[i]);
        }
    }
    acc.finish(out, poses[0]);
    return normalized;
}

void PoseAccumulator::begin(usize jointCount) {
    m_t.assign(jointCount, glm::vec3(0.0f));
    m_r.assign(jointCount, glm::quat(0.0f, 0.0f, 0.0f, 0.0f));
    m_s.assign(jointCount, glm::vec3(0.0f));
    m_w.assign(jointCount, 0.0f);
    m_total = 0.0f;
}

void PoseAccumulator::add(const Pose& pose, f32 weight, const JointMask* mask) {
    OX_ASSERT(pose.size() == m_t.size(), "PoseAccumulator size mismatch");
    m_total += weight;
    for (usize i = 0; i < pose.size(); ++i) {
        const f32 w = weight * (mask ? mask->weight(i) : 1.0f);
        if (w <= 0.0f) {
            continue;
        }
        const Transform& t = pose.local[i];
        m_t[i] += t.translation * w;
        m_s[i] += t.scale * w;
        // Keep all contributions in the hemisphere of the first one so they don't cancel out.
        const f32 sign = (m_w[i] > 0.0f && glm::dot(m_r[i], t.rotation) < 0.0f) ? -1.0f : 1.0f;
        m_r[i] += t.rotation * (w * sign);
        m_w[i] += w;
    }
}

void PoseAccumulator::finish(Pose& out, const Pose* fallback) const {
    out.resize(m_t.size());
    for (usize i = 0; i < m_t.size(); ++i) {
        if (m_w[i] <= 0.0f) {
            out.local[i] = fallback ? fallback->local[i] : Transform{};
            continue;
        }
        const f32 inv = 1.0f / m_w[i];
        out.local[i].translation = m_t[i] * inv;
        out.local[i].scale = m_s[i] * inv;
        const f32 len = glm::length(m_r[i]);
        out.local[i].rotation = len > 1e-8f ? m_r[i] / len : glm::quat(1, 0, 0, 0);
    }
}

void makeAdditiveDelta(const Pose& pose, const Pose& reference, Pose& outDelta) {
    OX_ASSERT(pose.size() == reference.size(), "makeAdditiveDelta size mismatch");
    outDelta.resize(pose.size());
    for (usize i = 0; i < pose.size(); ++i) {
        const Transform& p = pose.local[i];
        const Transform& r = reference.local[i];
        Transform& d = outDelta.local[i];
        d.translation = p.translation - r.translation;
        glm::quat q = glm::normalize(glm::inverse(r.rotation) * p.rotation);
        d.rotation = q.w < 0.0f ? -q : q;
        d.scale = p.scale / r.scale;
    }
}

void applyAdditive(Pose& base, const Pose& delta, f32 weight, const JointMask* mask) {
    OX_ASSERT(base.size() == delta.size(), "applyAdditive size mismatch");
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    for (usize i = 0; i < base.size(); ++i) {
        const f32 w = weight * (mask ? mask->weight(i) : 1.0f);
        if (w <= 0.0f) {
            continue;
        }
        Transform& b = base.local[i];
        const Transform& d = delta.local[i];
        b.translation += d.translation * w;
        b.rotation = glm::normalize(b.rotation * (w >= 1.0f ? d.rotation : nlerpShortest(identity, d.rotation, w)));
        b.scale *= glm::mix(glm::vec3(1.0f), d.scale, w);
    }
}

} // namespace ox::anim
