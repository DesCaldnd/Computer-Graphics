#pragma once

#include <oxwald/animation/skeleton.hpp>

#include <span>
#include <vector>

namespace ox::anim {

// Per-joint blend weights in [0, 1]. An empty mask means "all joints, weight 1".
struct JointMask {
    std::vector<f32> weights;

    f32 weight(usize joint) const { return weights.empty() ? 1.0f : weights[joint]; }
    bool empty() const { return weights.empty(); }

    static JointMask all(const Skeleton& skeleton, f32 weight = 1.0f);
    static JointMask none(const Skeleton& skeleton) { return all(skeleton, 0.0f); }
    // Sets the weight of `rootJoint` and its whole subtree (e.g. "Spine" → upper body layer).
    void setBranch(const Skeleton& skeleton, i32 rootJoint, f32 weight);
    static JointMask fromBranch(const Skeleton& skeleton, i32 rootJoint, f32 weight = 1.0f);
};

// Local-space pose (one Transform per joint, joint order of the skeleton).
struct Pose {
    std::vector<Transform> local;

    usize size() const { return local.size(); }
    void resize(usize n) { local.resize(n); }
    void setBind(const Skeleton& skeleton) { local = skeleton.bindPose(); }
    void setIdentity(usize jointCount) { local.assign(jointCount, Transform{}); }
};

// Local → model space (single forward pass thanks to topological ordering).
void localToModel(const Skeleton& skeleton, const Pose& pose, std::vector<Transform>& model);
void localToModel(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& model);
// Recomputes model transforms only for joints with index >= firstJoint (use after editing a local).
void updateModelFrom(const Skeleton& skeleton, const Pose& pose, std::vector<Transform>& model, i32 firstJoint);
// Model → local (used by IK to write results back).
void modelToLocal(const Skeleton& skeleton, const std::vector<Transform>& model, Pose& pose);

// out = lerp(a, b, weight * mask[j]). `out` may alias `a` or `b`.
void blendPoses(const Pose& a, const Pose& b, f32 weight, Pose& out, const JointMask* mask = nullptr);

// Weighted average of N poses. Weights are normalised (sum → 1); returns the normalised weights.
// Zero/negative total weight leaves `out` = poses[0].
std::vector<f32> blendPosesWeighted(std::span<const Pose* const> poses, std::span<const f32> weights, Pose& out);

// Incremental weighted accumulation (avoids temporary vectors in hot paths).
class PoseAccumulator {
public:
    void begin(usize jointCount);
    void add(const Pose& pose, f32 weight, const JointMask* mask = nullptr);
    // Normalises by the accumulated per-joint weight. Joints that received no weight copy `fallback`.
    void finish(Pose& out, const Pose* fallback = nullptr) const;
    f32 totalWeight() const { return m_total; }

private:
    std::vector<glm::vec3> m_t;
    std::vector<glm::quat> m_r;
    std::vector<glm::vec3> m_s;
    std::vector<f32> m_w;
    f32 m_total = 0.0f;
};

// Additive animation helpers. A delta pose stores (t - ref.t, inv(ref.r) * r, s / ref.s).
void makeAdditiveDelta(const Pose& pose, const Pose& reference, Pose& outDelta);
// base = base ⊕ (delta * weight * mask[j]).
void applyAdditive(Pose& base, const Pose& delta, f32 weight, const JointMask* mask = nullptr);

} // namespace ox::anim
