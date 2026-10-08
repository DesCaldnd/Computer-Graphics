#pragma once

#include <oxwald/animation/blend_space.hpp>

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ox::anim {

enum class ParamType : u8 { Float, Int, Bool, Trigger };

struct ParamDesc {
    std::string name;
    ParamType type = ParamType::Float;
    f32 defaultValue = 0.0f; // bools/triggers: 0 or 1
};

enum class ConditionOp : u8 { Greater, Less, Equal, NotEqual, IsTrue, IsFalse, Triggered };

struct Condition {
    u32 param = 0;
    ConditionOp op = ConditionOp::IsTrue;
    f32 threshold = 0.0f;
};

// What a state plays.
struct Motion {
    enum class Kind : u8 { None, Clip, BlendSpace1D, BlendSpace2D };
    Kind kind = Kind::None;
    std::shared_ptr<const AnimationClip> clip;
    std::shared_ptr<const BlendSpace1D> space1D;
    std::shared_ptr<const BlendSpace2D> space2D;
    i32 paramX = -1; // blend space inputs (parameter indices)
    i32 paramY = -1;

    static Motion fromClip(std::shared_ptr<const AnimationClip> clip);
    static Motion fromBlendSpace(std::shared_ptr<const BlendSpace1D> space, u32 paramX);
    static Motion fromBlendSpace(std::shared_ptr<const BlendSpace2D> space, u32 paramX, u32 paramY);
};

struct StateDesc {
    std::string name;
    Motion motion;
    f32 speed = 1.0f;
    i32 speedParam = -1; // optional float parameter multiplying `speed`
    bool loop = true;
};

inline constexpr i32 kAnyState = -1;

struct TransitionDesc {
    i32 from = kAnyState; // kAnyState = may fire from any state (checked first, can interrupt transitions)
    i32 to = 0;
    std::vector<Condition> conditions; // all must hold
    bool hasExitTime = false;
    f32 exitTime = 1.0f;     // normalised time of the source state; < 1 on looping states checks every cycle
    f32 duration = 0.2f;     // crossfade seconds
    f32 destinationOffset = 0.0f; // normalised start time of the destination
    bool canTransitionToSelf = false; // any-state only

    TransitionDesc& when(u32 param, ConditionOp op, f32 threshold = 0.0f) {
        conditions.push_back({param, op, threshold});
        return *this;
    }
};

enum class LayerBlend : u8 { Override, Additive };

struct LayerDesc {
    std::string name;
    std::vector<StateDesc> states;
    std::vector<TransitionDesc> transitions;
    i32 defaultState = 0;
    f32 weight = 1.0f;
    LayerBlend blend = LayerBlend::Override;
    JointMask mask; // empty = whole body
};

// Immutable, shareable description of parameters, layers, states and transitions (the "asset").
class AnimatorController {
public:
    u32 addParameter(std::string name, ParamType type, f32 defaultValue = 0.0f);
    i32 findParameter(std::string_view name) const;
    const std::vector<ParamDesc>& parameters() const { return m_params; }

    u32 addLayer(std::string name, LayerBlend blend = LayerBlend::Override, f32 weight = 1.0f);
    LayerDesc& layer(u32 index) { return m_layers[index]; }
    const LayerDesc& layer(u32 index) const { return m_layers[index]; }
    const std::vector<LayerDesc>& layers() const { return m_layers; }

    i32 addState(u32 layer, StateDesc state);
    i32 findState(u32 layer, std::string_view name) const;
    TransitionDesc& addTransition(u32 layer, i32 from, i32 to, f32 duration = 0.2f);

private:
    std::vector<ParamDesc> m_params;
    std::vector<LayerDesc> m_layers;
};

struct FiredEvent {
    const AnimEvent* event = nullptr;
    u32 layer = 0;
    i32 state = -1;
    f32 weight = 0.0f; // blend weight of the clip that fired it
};

// Runtime instance of an AnimatorController for one skeleton: parameters, layer state machines,
// crossfades, events, root motion. Usage per frame: set parameters → update(dt) → pose() →
// (IK) → computeSkinningMatrices.
class Animator {
public:
    // Shares ownership of the skeleton (asset caches can drop or hot-reload theirs while animators live on).
    Animator(std::shared_ptr<const Skeleton> skeleton, std::shared_ptr<const AnimatorController> controller);
    // Non-owning: `skeleton` must outlive the animator (stack/test setups).
    Animator(const Skeleton& skeleton, std::shared_ptr<const AnimatorController> controller);

    // Parameters (by index or name; unknown names are logged once and ignored).
    void setFloat(u32 param, f32 value);
    void setInt(u32 param, i32 value) { setFloat(param, static_cast<f32>(value)); }
    void setBool(u32 param, bool value) { setFloat(param, value ? 1.0f : 0.0f); }
    void setTrigger(u32 param) { setFloat(param, 1.0f); }
    void resetTrigger(u32 param) { setFloat(param, 0.0f); }
    void setFloat(std::string_view name, f32 value);
    void setBool(std::string_view name, bool value);
    void setTrigger(std::string_view name);
    f32 getFloat(u32 param) const { return m_params[param]; }
    bool getBool(u32 param) const { return m_params[param] != 0.0f; }

    void update(f32 dt);

    const Pose& pose() const { return m_pose; }
    // Root motion accumulated during the last update (base layer), in character space.
    const Transform& rootMotionDelta() const { return m_rootDelta; }
    const std::vector<FiredEvent>& events() const { return m_events; }

    // Forces a state (no blend) or crossfades to it.
    void play(u32 layer, i32 state, f32 normalizedTime = 0.0f);
    void crossFade(u32 layer, i32 state, f32 duration, f32 normalizedTime = 0.0f);

    i32 currentState(u32 layer) const;
    i32 nextState(u32 layer) const; // -1 when not transitioning
    bool inTransition(u32 layer) const;
    f32 normalizedTime(u32 layer) const;
    f32 transitionProgress(u32 layer) const;
    void setLayerWeight(u32 layer, f32 weight);
    f32 layerWeight(u32 layer) const;

    const Skeleton& skeleton() const { return *m_skeleton; }
    // Null owner for animators built with the non-owning constructor.
    const std::shared_ptr<const Skeleton>& skeletonPtr() const { return m_skeleton; }
    const AnimatorController& controller() const { return *m_controller; }

private:
    struct StateInstance {
        i32 state = -1;
        f32 normTime = 0.0f;
        f32 prevNormTime = 0.0f;
        std::vector<SamplingCursor> cursors;
        bool justStarted = true;
    };
    struct LayerRuntime {
        StateInstance current;
        StateInstance next;
        bool transitioning = false;
        bool frozenSource = false; // source is a snapshot (interrupted transition)
        f32 transitionTime = 0.0f;
        f32 transitionDuration = 0.0f;
        f32 weight = 1.0f;
        Pose frozen;
        Pose output;
    };

    void startTransition(u32 layerIndex, i32 to, f32 duration, f32 offset);
    void evaluateTransitions(u32 layerIndex);
    bool conditionsHold(const TransitionDesc& t, const StateInstance& source, const LayerDesc& layer) const;
    void consumeTriggers(const TransitionDesc& t);
    f32 motionDuration(const Motion& motion, std::vector<f32>& weights) const;
    void motionWeights(const Motion& motion, std::vector<f32>& weights) const;
    void advance(u32 layerIndex, StateInstance& inst, f32 dt, f32 blendWeight, bool emit);
    void sampleState(const LayerDesc& layer, StateInstance& inst, Pose& out, usize scratchBase);
    Pose& scratch(usize index);

    std::shared_ptr<const Skeleton> m_skeleton; // aliasing (non-owning) for the reference constructor
    std::shared_ptr<const AnimatorController> m_controller;
    std::vector<f32> m_params;
    std::vector<LayerRuntime> m_layers;
    Pose m_pose;
    Pose m_bind;
    Transform m_rootDelta;
    std::vector<FiredEvent> m_events;
    std::deque<Pose> m_scratch; // deque: references stay valid while growing
    std::vector<f32> m_weights;
    std::vector<const AnimEvent*> m_eventScratch;
    // Root motion accumulated per layer-0 instance during advance().
    glm::vec3 m_rootAccumT{0.0f};
    glm::quat m_rootAccumR{0.0f, 0.0f, 0.0f, 0.0f};
    f32 m_rootAccumW = 0.0f;
};

// owner ← owner ∘ delta (delta is in the owner's local space; translation scaled by owner scale).
void applyRootMotion(Transform& owner, const Transform& delta);

} // namespace ox::anim
