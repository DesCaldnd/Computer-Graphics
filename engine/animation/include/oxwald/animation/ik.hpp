#pragma once

#include <oxwald/animation/pose.hpp>

#include <glm/gtc/constants.hpp>

#include <functional>
#include <optional>
#include <span>

// IK solvers. All work in the skeleton's model space: callers pass the local pose plus its model-space
// transforms (from localToModel). Solvers update both so they can be chained (e.g. foot IK → look-at).
namespace ox::anim {

// Rotates `joint` by a model-space rotation (pivot = joint position) by editing its local rotation.
// Updates model transforms of the joint and every joint after it.
void applyModelRotation(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model, i32 joint,
                        const glm::quat& modelRotation);

struct TwoBoneIKSettings {
    i32 root = kNoJoint; // e.g. upper arm / thigh
    i32 mid = kNoJoint;  // elbow / knee
    i32 end = kNoJoint;  // wrist / ankle
    glm::vec3 target{0.0f};
    glm::vec3 pole{0.0f, 0.0f, 1.0f}; // model-space point the mid joint bends towards
    bool usePole = true;
    f32 weight = 1.0f;
    bool allowStretch = false;
    f32 maxStretch = 1.25f; // max length factor when stretching
    // Optional end-effector rotation in model space (e.g. hand orientation).
    std::optional<glm::quat> endRotation;
};

struct IKResult {
    bool reached = false;
    f32 error = 0.0f; // distance end effector ↔ target after solving
    u32 iterations = 0;
};

IKResult solveTwoBoneIK(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model,
                        const TwoBoneIKSettings& settings);

struct AimIKSettings {
    i32 joint = kNoJoint;
    glm::vec3 target{0.0f};
    glm::vec3 aimAxis{0.0f, 0.0f, 1.0f}; // joint-local axis that should point at the target
    glm::vec3 upAxis{0.0f, 1.0f, 0.0f};  // joint-local axis kept close to `worldUp` (0 = no twist control)
    glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
    f32 weight = 1.0f;
    f32 maxAngle = glm::pi<f32>(); // clamp of the aim correction (radians)
};

IKResult solveAimIK(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model,
                    const AimIKSettings& settings);

// Look-at distributed across a chain (e.g. spine_02, neck, head) with per-joint weights; the last joint
// is the one carrying `aimAxis` (typically the head).
struct LookAtChainSettings {
    std::span<const i32> joints;
    std::span<const f32> weights; // same size as joints; the last should usually be 1
    glm::vec3 target{0.0f};
    glm::vec3 aimAxis{0.0f, 0.0f, 1.0f}; // in the last joint's local space
    f32 maxAngle = glm::radians(80.0f);
    f32 weight = 1.0f;
};

IKResult solveLookAtChain(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model,
                          const LookAtChainSettings& settings);

struct FabrikSettings {
    std::span<const i32> chain; // root → tip, each joint a descendant of the previous
    glm::vec3 target{0.0f};
    f32 tolerance = 1e-3f;
    u32 maxIterations = 16;
    f32 weight = 1.0f;
};

IKResult solveFABRIK(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model,
                     const FabrikSettings& settings);

// Position-only FABRIK on raw points (exposed for tests/tools). Returns iterations used.
IKResult fabrikPositions(std::span<glm::vec3> points, glm::vec3 target, f32 tolerance, u32 maxIterations);

// ---- Foot IK ----
struct GroundHit {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
};

// World-space ray cast supplied by the game (physics). Return std::nullopt when nothing was hit.
using GroundRaycast = std::function<std::optional<GroundHit>(const glm::vec3& origin, const glm::vec3& direction,
                                                             f32 maxDistance)>;

struct FootIKLeg {
    i32 hip = kNoJoint;
    i32 knee = kNoJoint;
    i32 ankle = kNoJoint;
    f32 ankleHeight = 0.08f; // ankle height above the sole in the animation (model units)
};

struct FootIKSettings {
    i32 pelvis = kNoJoint;
    Transform ownerWorld;          // model → world (character transform)
    f32 rayStartHeight = 0.5f;     // ray starts this far above the animated ankle
    f32 rayLength = 1.2f;
    f32 maxPelvisDrop = 0.4f;
    f32 maxFootRaise = 0.5f;
    bool alignFeetToNormal = true;
    f32 maxFootAngle = glm::radians(45.0f);
    f32 weight = 1.0f;
};

struct FootIKResult {
    f32 pelvisOffset = 0.0f;        // applied vertical pelvis offset (model space)
    std::vector<bool> grounded;      // per leg
    std::vector<glm::vec3> targets;  // per leg ankle target (model space)
};

FootIKResult solveFootIK(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model,
                         std::span<const FootIKLeg> legs, const FootIKSettings& settings,
                         const GroundRaycast& raycast);

} // namespace ox::anim
