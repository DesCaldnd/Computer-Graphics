#pragma once

#include <oxwald/animation/animation.hpp>
#include <oxwald/core/events.hpp>
#include <oxwald/core/services.hpp>
#include <oxwald/gameplay/common.hpp>
#include <oxwald/gameplay/events.hpp>
#include <oxwald/core/serial/value.hpp>
#include <oxwald/scene/world.hpp>

#include <entt/signal/sigh.hpp>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ox {
class DebugDraw;
}

namespace ox::gameplay {

class IAnimationAssetProvider;

// ---- components ------------------------------------------------------------------------------------------

struct AnimatorParameterDesc {
    std::string name;
    anim::ParamType type = anim::ParamType::Float;
    f32 defaultValue = 0.f;
};

struct AnimatorStateDesc {
    std::string name;
    Uuid clip; // AnimationClip asset
    f32 speed = 1.f;
    std::string speedParameter; // optional float parameter multiplying speed
    bool loop = true;
};

struct AnimatorTransitionDesc {
    std::string from; // state name; empty or "*" = any state
    std::string to;
    std::string parameter; // condition parameter (empty = none, use exit time)
    anim::ConditionOp op = anim::ConditionOp::IsTrue;
    f32 threshold = 0.f;
    f32 duration = 0.2f;
    bool hasExitTime = false;
    f32 exitTime = 1.f;
};

// Single-layer state machine stored in the component (used when AnimatorComponent::controller is nil).
struct InlineAnimatorController {
    std::vector<AnimatorParameterDesc> parameters;
    std::vector<AnimatorStateDesc> states;
    std::vector<AnimatorTransitionDesc> transitions;
    std::string defaultState; // empty = first state
};

struct AnimatorComponent {
    Uuid skeleton;   // Skeleton asset
    Uuid controller; // AnimatorController asset; nil = inlineController
    InlineAnimatorController inlineController;
    bool applyRootMotion = false; // moves the entity (or drives its CharacterController)
    f32 playbackSpeed = 1.f;
    bool animateInEditMode = false;
    // Parameter values pushed to the animator every frame (missing names keep their current value).
    // Trigger parameters set to non-zero fire once and are reset to 0.
    std::map<std::string, f32> parameters;
    // runtime
    std::string currentState;
};

// Skinned mesh rendered with the palette of the AnimatorComponent on the same entity or nearest ancestor.
struct SkinnedMeshComponent {
    Uuid mesh;
    std::vector<Uuid> materials;
    anim::SkinningMethod skinningMethod = anim::SkinningMethod::Linear;
    bool gpuSkinning = true;
    bool castShadows = true;
    bool visible = true;
    // runtime (renderer extraction): palette[j] = model[j] * inverseBind[j], bumped version on change
    std::vector<glm::mat4> palette;
    u64 paletteVersion = 0;
};

enum class IKChainType : u8 { TwoBone, Aim };

struct IKChainDesc {
    IKChainType type = IKChainType::TwoBone;
    std::string rootJoint; // TwoBone root (thigh/upper arm)
    std::string midJoint;  // TwoBone mid (knee/elbow)
    std::string endJoint;  // TwoBone end / Aim joint
    EntityRef target;
    glm::vec3 targetOffset{0.f}; // world space, added to the target position (or the target itself if no entity)
    EntityRef pole;
    glm::vec3 poleOffset{0.f, 0.f, 1.f};
    glm::vec3 aimAxis{0.f, 0.f, 1.f}; // Aim: joint-local axis
    f32 weight = 1.f;
    bool enabled = true;
};

struct IKComponent {
    std::vector<IKChainDesc> chains;
};

// ---- runtime -------------------------------------------------------------------------------------------

class AnimationRuntime {
public:
    AnimationRuntime();
    ~AnimationRuntime();
    AnimationRuntime(const AnimationRuntime&) = delete;
    AnimationRuntime& operator=(const AnimationRuntime&) = delete;

    [[nodiscard]] anim::Animator* animator(Entity e);
    [[nodiscard]] const anim::Skeleton* skeleton(Entity e) const;
    // Model-space joint transforms after the last update (animation + IK).
    [[nodiscard]] const std::vector<anim::Transform>* modelPose(Entity e) const;
    void setFloat(Entity e, std::string_view name, f32 value);
    void setBool(Entity e, std::string_view name, bool value);
    void setTrigger(Entity e, std::string_view name);
    bool play(Entity e, std::string_view state, u32 layer = 0, f32 crossFade = 0.f);

    Signal<const AnimationEvent&> onEvent;
    bool debugDraw = false;

    // ---- driven by the gameplay systems ----
    void attach(World& world, Services& services);
    void detach();
    void update(f32 dt, bool playing);
    void drawDebug(DebugDraw& draw);

private:
    struct Record {
        std::shared_ptr<const anim::Skeleton> skeleton;
        std::shared_ptr<const anim::AnimatorController> controller;
        std::unique_ptr<anim::Animator> animator;
        std::vector<anim::Transform> model;
        Uuid skeletonId;
        Uuid controllerId;
        serial::Value inlineDesc;
        bool failed = false;
    };
    void onChanged(entt::registry& r, entt::entity e);
    void onDestroyed(entt::registry& r, entt::entity e);
    Record* ensure(Entity e);
    std::shared_ptr<const anim::AnimatorController> buildInline(const InlineAnimatorController& desc,
                                                                const anim::Skeleton& skeleton);
    void applyIK(Entity e, const anim::Skeleton& skeleton, anim::Pose& pose, std::vector<anim::Transform>& model);
    void applyRootMotion(Entity e, const anim::Transform& delta, f32 dt);
    void writePalette(Entity e, const Record& rec);

    World* m_world = nullptr;
    Services* m_services = nullptr;
    IAnimationAssetProvider* m_assets = nullptr;
    EventBus* m_bus = nullptr;
    std::unordered_map<entt::entity, Record> m_records;
    std::vector<entt::scoped_connection> m_connections;
    std::vector<AnimationEvent> m_events;
};

} // namespace ox::gameplay
