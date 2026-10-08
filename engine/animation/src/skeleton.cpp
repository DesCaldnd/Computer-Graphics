#include <oxwald/animation/debug_draw.hpp>
#include <oxwald/animation/skeleton.hpp>

#include <oxwald/core/assert.hpp>

#include <utility>

namespace ox::anim {

i32 Skeleton::addJoint(std::string name, i32 parent, const Transform& bindLocal) {
    const i32 index = static_cast<i32>(m_names.size());
    OX_ASSERT(parent == kNoJoint || (parent >= 0 && parent < index), "joint '{}' parent {} breaks topological order",
              name, parent);
    m_names.push_back(std::move(name));
    m_parents.push_back(parent);
    m_bindLocal.push_back(bindLocal);
    m_inverseBind.emplace_back(1.0f);
    m_inverseBindExplicit.push_back(0);
    return index;
}

void Skeleton::setInverseBind(i32 joint, const glm::mat4& inverseBind) {
    m_inverseBind[static_cast<usize>(joint)] = inverseBind;
    m_inverseBindExplicit[static_cast<usize>(joint)] = 1;
}

void Skeleton::finalize() {
    const auto model = bindModelMatrices();
    for (usize i = 0; i < model.size(); ++i) {
        if (!m_inverseBindExplicit[i]) {
            m_inverseBind[i] = glm::inverse(model[i]);
        }
    }
}

i32 Skeleton::findJoint(std::string_view name) const {
    for (usize i = 0; i < m_names.size(); ++i) {
        if (m_names[i] == name) {
            return static_cast<i32>(i);
        }
    }
    return kNoJoint;
}

std::vector<glm::mat4> Skeleton::bindModelMatrices() const {
    std::vector<glm::mat4> model(m_names.size());
    for (usize i = 0; i < m_names.size(); ++i) {
        const glm::mat4 local = m_bindLocal[i].toMatrix();
        model[i] = m_parents[i] == kNoJoint ? local : model[static_cast<usize>(m_parents[i])] * local;
    }
    return model;
}

bool Skeleton::isDescendantOf(i32 joint, i32 ancestor) const {
    while (joint != kNoJoint) {
        if (joint == ancestor) {
            return true;
        }
        joint = m_parents[static_cast<usize>(joint)];
    }
    return false;
}

std::vector<i32> Skeleton::chain(i32 to, i32 from) const {
    std::vector<i32> out;
    for (i32 j = from; j != kNoJoint; j = m_parents[static_cast<usize>(j)]) {
        out.push_back(j);
        if (j == to) {
            return out;
        }
    }
    return {};
}

void debugDrawSkeleton(const Skeleton& skeleton, const std::vector<Transform>& model, const Transform& ownerWorld,
                       const DebugLineFn& line, glm::vec4 color, f32 axisLength) {
    if (!line) {
        return;
    }
    for (usize i = 0; i < model.size() && i < skeleton.jointCount(); ++i) {
        const glm::vec3 p = ownerWorld.transformPoint(model[i].translation);
        const i32 parent = skeleton.parent(static_cast<i32>(i));
        if (parent != kNoJoint) {
            line(ownerWorld.transformPoint(model[static_cast<usize>(parent)].translation), p, color);
        }
        if (axisLength > 0.0f) {
            const glm::quat r = ownerWorld.rotation * model[i].rotation;
            line(p, p + r * glm::vec3(axisLength, 0, 0), {1, 0, 0, 1});
            line(p, p + r * glm::vec3(0, axisLength, 0), {0, 1, 0, 1});
            line(p, p + r * glm::vec3(0, 0, axisLength), {0, 0, 1, 1});
        }
    }
}

} // namespace ox::anim
