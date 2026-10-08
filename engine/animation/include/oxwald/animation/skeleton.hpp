#pragma once

#include <oxwald/animation/transform.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ox::anim {

inline constexpr i32 kNoJoint = -1;

// Joint hierarchy. Joints are stored topologically sorted (parent index < child index), which lets
// every hierarchy pass be a single forward loop.
class Skeleton {
public:
    // Appends a joint. `parent` must be kNoJoint or an existing index (enforces topological order).
    // The inverse bind matrix defaults to the inverse of the bind pose model matrix (see finalize()).
    i32 addJoint(std::string name, i32 parent, const Transform& bindLocal);

    // Overrides the inverse bind matrix for a joint (e.g. from glTF skins / aiBone::mOffsetMatrix).
    void setInverseBind(i32 joint, const glm::mat4& inverseBind);

    // Recomputes inverse bind matrices from the bind pose for joints that have no explicit override.
    void finalize();

    usize jointCount() const { return m_names.size(); }
    bool empty() const { return m_names.empty(); }
    i32 findJoint(std::string_view name) const;

    const std::string& jointName(i32 joint) const { return m_names[static_cast<usize>(joint)]; }
    i32 parent(i32 joint) const { return m_parents[static_cast<usize>(joint)]; }
    const std::vector<std::string>& names() const { return m_names; }
    const std::vector<i32>& parents() const { return m_parents; }
    const std::vector<Transform>& bindPose() const { return m_bindLocal; }
    std::vector<Transform>& bindPose() { return m_bindLocal; }
    const std::vector<glm::mat4>& inverseBindMatrices() const { return m_inverseBind; }

    // Bind pose in model space.
    std::vector<glm::mat4> bindModelMatrices() const;

    // True if `joint` equals `ancestor` or lies in its subtree.
    bool isDescendantOf(i32 joint, i32 ancestor) const;
    // Joints from `from` up to and including `to` (child → parent order); empty if `to` isn't an ancestor.
    std::vector<i32> chain(i32 to, i32 from) const;

private:
    std::vector<std::string> m_names;
    std::vector<i32> m_parents;
    std::vector<Transform> m_bindLocal;
    std::vector<glm::mat4> m_inverseBind;
    std::vector<u8> m_inverseBindExplicit;
};

} // namespace ox::anim
