#pragma once

#include <oxwald/assets/model_import.hpp>

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace ox::assets::detail {

// Source -> engine conversion C = s·R (R: axis-aligned rotation). Applied to every node as C·T·C⁻¹ and to every
// vertex as C·p, so hierarchies keep their structure while vertices/translations end up in meters, Y-up.
struct AxisConversion {
    glm::quat rotation{1, 0, 0, 0};
    f32 scale = 1.0f;

    static AxisConversion make(UpAxis sourceUp, f32 unitToMeters, f32 extraScale) {
        AxisConversion c;
        c.scale = (unitToMeters > 0.0f ? unitToMeters : 1.0f) * extraScale;
        switch (sourceUp) {
        case UpAxis::Z: c.rotation = glm::angleAxis(-glm::half_pi<f32>(), glm::vec3(1, 0, 0)); break; // +Z -> +Y
        case UpAxis::X: c.rotation = glm::angleAxis(glm::half_pi<f32>(), glm::vec3(0, 0, 1)); break;  // +X -> +Y
        default: break;
        }
        return c;
    }
    [[nodiscard]] bool identity() const { return scale == 1.0f && rotation == glm::quat(1, 0, 0, 0); }
    [[nodiscard]] glm::vec3 point(glm::vec3 p) const { return rotation * p * scale; }
    [[nodiscard]] glm::vec3 direction(glm::vec3 d) const { return rotation * d; }
    [[nodiscard]] Transform node(const Transform& t) const {
        Transform out;
        out.position = point(t.position);
        out.rotation = glm::normalize(rotation * t.rotation * glm::conjugate(rotation));
        out.scale = glm::abs(rotation * t.scale);
        return out;
    }
    void apply(MeshData& mesh) const {
        if (identity()) return;
        for (auto& p : mesh.positions) p = point(p);
        for (auto& a : mesh.attributes) {
            a.normal = direction(a.normal);
            a.tangent = glm::vec4(direction(glm::vec3(a.tangent)), a.tangent.w);
        }
    }
};

// Generates missing normals/tangents, welds duplicates, sets LOD 0 ranges.
void finalizeImportedMesh(MeshData& mesh, bool hasNormals, bool hasTangents, const ModelImportSettings& settings);

} // namespace ox::assets::detail
