#pragma once

#include <oxwald/ai/blackboard.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ox::ai {

// Response curve mapping a normalised input (0..1) to a score (0..1).
struct ResponseCurve {
    enum class Type : u8 { Linear, Polynomial, Logistic, Logit } type = Type::Linear;
    f32 slope = 1.f;    // m
    f32 exponent = 2.f; // k
    f32 xShift = 0.f;   // b
    f32 yShift = 0.f;   // c
    [[nodiscard]] f32 evaluate(f32 x) const;
};

struct UtilityConsideration {
    std::string name;
    std::function<f32(const Blackboard&)> input; // should return 0..1 (clamped)
    ResponseCurve curve;
};

struct UtilityAction {
    std::string name;
    std::vector<UtilityConsideration> considerations;
    f32 weight = 1.f;
};

// Picks the action with the highest product of consideration scores (with the usual compensation factor so
// actions with many considerations are not penalised).
class UtilityScorer {
public:
    void addAction(UtilityAction action) { m_actions.push_back(std::move(action)); }
    [[nodiscard]] f32 score(const UtilityAction& action, const Blackboard& bb) const;
    [[nodiscard]] std::vector<std::pair<std::string, f32>> scoreAll(const Blackboard& bb) const;
    // Index of the best action; nullopt when every score is 0. `stickiness` bonus keeps the current choice.
    [[nodiscard]] std::optional<usize> best(const Blackboard& bb, std::optional<usize> current = std::nullopt,
                                            f32 stickiness = 0.f) const;
    [[nodiscard]] const std::vector<UtilityAction>& actions() const { return m_actions; }

private:
    std::vector<UtilityAction> m_actions;
};

} // namespace ox::ai
