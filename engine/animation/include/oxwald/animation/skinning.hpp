#pragma once

#include <oxwald/animation/pose.hpp>

#include <span>
#include <string>
#include <vector>

namespace ox::anim {

// CPU-side skinned mesh. Joint indices refer to the Skeleton the mesh was imported with.
// Influences are stored as `influencesPerVertex` (4 or 8) consecutive entries per vertex,
// sorted by weight (descending) and normalised to sum to 1.
struct SkinnedMeshData {
    std::string name;
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents; // xyz + handedness, optional
    std::vector<glm::vec2> uvs;      // optional
    std::vector<u32> indices;
    u32 influencesPerVertex = 4;
    std::vector<u16> joints;
    std::vector<f32> weights;
    i32 materialIndex = -1;

    usize vertexCount() const { return positions.size(); }

    // Keeps the `influencesPerVertex` largest weights and renormalises. `perVertex` holds arbitrary
    // (joint, weight) lists; vertices without influences are bound to `fallbackJoint`.
    void setInfluences(const std::vector<std::vector<std::pair<u16, f32>>>& perVertex, u32 maxInfluences,
                       u16 fallbackJoint = 0);
    // Renormalises existing weights (sum → 1).
    void normalizeWeights();
};

// palette[j] = model[j] * inverseBind[j]. For the bind pose this is identity.
void computeSkinningMatrices(const Skeleton& skeleton, std::span<const glm::mat4> model,
                             std::vector<glm::mat4>& palette);
void computeSkinningMatrices(const Skeleton& skeleton, const Pose& pose, std::vector<glm::mat4>& palette);

// Unit dual quaternion (rigid transform only; scale is ignored by DQ skinning).
struct DualQuat {
    glm::quat real{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat dual{0.0f, 0.0f, 0.0f, 0.0f};

    static DualQuat fromRigid(const glm::quat& rotation, const glm::vec3& translation);
    static DualQuat fromMatrix(const glm::mat4& m); // strips scale
    glm::vec3 transformPoint(const glm::vec3& p) const;
    glm::vec3 transformVector(const glm::vec3& v) const;
    glm::vec3 translation() const;
};

// GPU-friendly packing: two vec4 per joint (real xyzw, dual xyzw).
void computeDualQuatPalette(std::span<const glm::mat4> palette, std::vector<DualQuat>& out);

enum class SkinningMethod : u8 { Linear, DualQuaternion };

// Reference CPU skinning (tests + fallback when compute skinning is unavailable).
// Outputs are resized to the vertex count; `outNormals` / `outTangents` may be null.
void skinMesh(const SkinnedMeshData& mesh, std::span<const glm::mat4> palette, SkinningMethod method,
              std::vector<glm::vec3>& outPositions, std::vector<glm::vec3>* outNormals = nullptr,
              std::vector<glm::vec4>* outTangents = nullptr);

} // namespace ox::anim
