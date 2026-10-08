#include <oxwald/ai/utility_ai.hpp>

#include <algorithm>
#include <cmath>

namespace ox::ai {

f32 ResponseCurve::evaluate(f32 x) const {
    x = std::clamp(x, 0.f, 1.f);
    f32 y = 0.f;
    switch (type) {
    case Type::Linear: y = slope * (x - xShift) + yShift; break;
    case Type::Polynomial: y = slope * std::pow(std::max(x - xShift, 0.f), exponent) + yShift; break;
    case Type::Logistic: y = slope / (1.f + std::exp(-10.f * exponent * (x - 0.5f - xShift))) + yShift; break;
    case Type::Logit: {
        const f32 v = std::clamp(x - xShift, 1e-4f, 1.f - 1e-4f);
        y = slope * std::log(v / (1.f - v)) / 5.f + 0.5f + yShift;
        break;
    }
    }
    return std::clamp(y, 0.f, 1.f);
}

f32 UtilityScorer::score(const UtilityAction& action, const Blackboard& bb) const {
    if (action.considerations.empty()) {
        return action.weight;
    }
    f32 s = 1.f;
    const f32 modification = 1.f - 1.f / static_cast<f32>(action.considerations.size());
    for (const UtilityConsideration& c : action.considerations) {
        const f32 x = c.input ? std::clamp(c.input(bb), 0.f, 1.f) : 0.f;
        f32 v = c.curve.evaluate(x);
        v += (1.f - v) * modification * v; // compensation factor (Dave Mark)
        s *= v;
        if (s <= 0.f) {
            return 0.f;
        }
    }
    return s * action.weight;
}

std::vector<std::pair<std::string, f32>> UtilityScorer::scoreAll(const Blackboard& bb) const {
    std::vector<std::pair<std::string, f32>> out;
    for (const auto& a : m_actions) {
        out.emplace_back(a.name, score(a, bb));
    }
    return out;
}

std::optional<usize> UtilityScorer::best(const Blackboard& bb, std::optional<usize> current, f32 stickiness) const {
    std::optional<usize> bestIdx;
    f32 bestScore = 0.f;
    for (usize i = 0; i < m_actions.size(); ++i) {
        f32 s = score(m_actions[i], bb);
        if (current && *current == i) {
            s += stickiness;
        }
        if (s > bestScore) {
            bestScore = s;
            bestIdx = i;
        }
    }
    return bestIdx;
}

} // namespace ox::ai
