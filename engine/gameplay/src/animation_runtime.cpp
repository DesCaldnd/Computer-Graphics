#include <oxwald/core/debug_draw.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/core/profile.hpp>
#include <oxwald/core/reflect.hpp>
#include <oxwald/animation/blend_space.hpp>
#include <oxwald/gameplay/animation.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/providers.hpp>

namespace ox::gameplay {

namespace {

anim::Transform toAnim(const Transform& t) { return {t.position, t.rotation, t.scale}; }

} // namespace

AnimationRuntime::AnimationRuntime() = default;
AnimationRuntime::~AnimationRuntime() { detach(); }

void AnimationRuntime::attach(World& world, Services& services) {
    detach();
    m_world = &world;
    m_services = &services;
    m_assets = services.tryGet<IAnimationAssetProvider>();
    m_bus = services.tryGet<EventBus>();
    entt::registry& r = world.registry();
    m_connections.emplace_back(r.on_construct<AnimatorComponent>().connect<&AnimationRuntime::onChanged>(*this));
    m_connections.emplace_back(r.on_update<AnimatorComponent>().connect<&AnimationRuntime::onChanged>(*this));
    m_connections.emplace_back(r.on_destroy<AnimatorComponent>().connect<&AnimationRuntime::onDestroyed>(*this));
}

void AnimationRuntime::detach() {
    m_connections.clear();
    m_records.clear();
    m_world = nullptr;
    m_services = nullptr;
}

void AnimationRuntime::onChanged(entt::registry& r, entt::entity e) {
    auto it = m_records.find(e);
    if (it == m_records.end()) return;
    // Parameter edits (scripts, inspector) must not restart the state machine: rebuild only when the assets or
    // the inline controller changed.
    const auto& c = r.get<AnimatorComponent>(e);
    const Record& rec = it->second;
    if (rec.failed || !rec.animator || rec.skeletonId != c.skeleton || rec.controllerId != c.controller ||
        !(rec.inlineDesc == serial::toValue(c.inlineController))) {
        m_records.erase(it);
    }
}

void AnimationRuntime::onDestroyed(entt::registry&, entt::entity e) { m_records.erase(e); }

std::shared_ptr<const anim::AnimatorController> buildAnimatorController(const InlineAnimatorController& desc,
                                                                    IAnimationAssetProvider* clips) {
    auto ctrl = std::make_shared<anim::AnimatorController>();
    for (const auto& p : desc.parameters) ctrl->addParameter(p.name, p.type, p.defaultValue);
    const u32 layer = ctrl->addLayer("Base");
    for (const auto& s : desc.states) {
        anim::StateDesc sd;
        sd.name = s.name;
        const i32 blendParam = s.blendParameter.empty() ? -1 : ctrl->findParameter(s.blendParameter);
        if (blendParam >= 0 && !s.blendSamples.empty() && clips) {
            auto space = std::make_shared<anim::BlendSpace1D>();
            for (const AnimatorBlendSample& b : s.blendSamples) {
                if (auto clip = clips->clip(b.clip)) space->addSample(b.position, std::move(clip));
                else OX_LOG_WARN("gameplay", "animator state '{}': blend clip {} not found", s.name, b.clip.toString());
            }
            if (!space->samples().empty()) sd.motion = anim::Motion::fromBlendSpace(std::move(space), u32(blendParam));
        } else if (s.clip.isValid() && clips) {
            if (auto clip = clips->clip(s.clip)) sd.motion = anim::Motion::fromClip(std::move(clip));
            else OX_LOG_WARN("gameplay", "animator state '{}': clip {} not found", s.name, s.clip.toString());
        }
        sd.speed = s.speed;
        sd.speedParam = s.speedParameter.empty() ? -1 : ctrl->findParameter(s.speedParameter);
        sd.loop = s.loop;
        ctrl->addState(layer, std::move(sd));
    }
    for (const auto& t : desc.transitions) {
        const i32 to = ctrl->findState(layer, t.to);
        if (to < 0) continue;
        const i32 from = t.from.empty() || t.from == "*" ? anim::kAnyState : ctrl->findState(layer, t.from);
        if (from < 0 && from != anim::kAnyState) continue;
        anim::TransitionDesc& td = ctrl->addTransition(layer, from, to, t.duration);
        if (!t.parameter.empty()) {
            const i32 p = ctrl->findParameter(t.parameter);
            if (p >= 0) td.when(static_cast<u32>(p), t.op, t.threshold);
        }
        td.hasExitTime = t.hasExitTime;
        td.exitTime = t.exitTime;
    }
    if (!desc.defaultState.empty()) {
        const i32 d = ctrl->findState(layer, desc.defaultState);
        if (d >= 0) ctrl->layer(layer).defaultState = d;
    }
    return ctrl;
}

void AnimationRuntime::invalidateAssets(const Uuid& id, bool clip) {
    std::erase_if(m_records, [&](const auto& kv) {
        const Record& r = kv.second;
        return clip || r.failed || r.skeletonId == id || r.controllerId == id;
    });
}

AnimationRuntime::Record* AnimationRuntime::ensure(Entity e) {
    auto it = m_records.find(e.handle());
    if (it != m_records.end()) return it->second.failed ? nullptr : &it->second;
    Record rec;
    const auto& c = e.get<AnimatorComponent>();
    rec.skeletonId = c.skeleton;
    rec.controllerId = c.controller;
    rec.inlineDesc = serial::toValue(c.inlineController);
    if (!m_assets && m_services) m_assets = m_services->tryGet<IAnimationAssetProvider>();
    rec.skeleton = m_assets && c.skeleton.isValid() ? m_assets->skeleton(c.skeleton) : nullptr;
    if (!rec.skeleton) {
        OX_LOG_WARN("gameplay", "animator of '{}': skeleton {} not available (IAnimationAssetProvider?)", e.name(),
                    c.skeleton.toString());
        rec.failed = true;
    } else {
        rec.controller = c.controller.isValid() ? m_assets->controller(c.controller)
                                                : buildAnimatorController(c.inlineController, m_assets);
        if (!rec.controller || rec.controller->layers().empty()) {
            OX_LOG_WARN("gameplay", "animator of '{}': no controller", e.name());
            rec.failed = true;
        } else {
            rec.animator = std::make_unique<anim::Animator>(rec.skeleton, rec.controller);
        }
    }
    auto [ins, ok] = m_records.emplace(e.handle(), std::move(rec));
    return ins->second.failed ? nullptr : &ins->second;
}

void AnimationRuntime::update(f32 dt, bool playing) {
    if (!m_world) return;
    OX_PROFILE_ZONE_N("AnimationRuntime::update");
    m_events.clear();
    entt::registry& r = m_world->registry();
    std::vector<entt::entity> entities(r.view<AnimatorComponent>().begin(), r.view<AnimatorComponent>().end());
    for (auto handle : entities) {
        if (!r.valid(handle)) continue;
        const Entity e = m_world->wrap(handle);
        if (!e.activeInHierarchy()) continue;
        Record* rec = ensure(e);
        if (!rec) continue;
        auto& c = r.get<AnimatorComponent>(handle);
        const bool animate = playing || c.animateInEditMode;
        anim::Animator& animator = *rec->animator;
        const auto& params = rec->controller->parameters();
        for (auto& [name, value] : c.parameters) {
            const i32 idx = rec->controller->findParameter(name);
            if (idx < 0) continue;
            if (params[usize(idx)].type == anim::ParamType::Trigger) {
                if (value != 0.f) {
                    animator.setTrigger(static_cast<u32>(idx));
                    value = 0.f;
                }
            } else {
                animator.setFloat(static_cast<u32>(idx), value);
            }
        }
        if (animate) {
            animator.update(dt * c.playbackSpeed);
            for (const anim::FiredEvent& fe : animator.events()) {
                if (fe.event) m_events.push_back({e, fe.event->name, fe.event->payload, fe.weight});
            }
            if (c.applyRootMotion) applyRootMotion(e, animator.rootMotionDelta(), dt);
        } else if (!rec->model.empty()) {
            continue; // edit mode: pose computed once
        }
        const i32 state = animator.currentState(0);
        const auto& states = rec->controller->layer(0).states;
        c.currentState = state >= 0 && usize(state) < states.size() ? states[usize(state)].name : std::string();

        anim::Pose pose = animator.pose();
        anim::localToModel(*rec->skeleton, pose, rec->model);
        if (animate && e.has<IKComponent>()) applyIK(e, *rec->skeleton, pose, rec->model);
        writePalette(e, *rec);
    }
    for (const AnimationEvent& ev : m_events) {
        onEvent.emit(ev);
        if (m_bus) m_bus->publish(ev);
    }
}

void AnimationRuntime::applyRootMotion(Entity e, const anim::Transform& delta, f32 dt) {
    if (auto* cc = e.tryGet<CharacterControllerComponent>(); cc && dt > 0.f) {
        const Transform wt = e.worldTransform();
        const glm::vec3 worldDelta = wt.rotation * (delta.translation * wt.scale);
        cc->desiredVelocity = glm::vec3(worldDelta.x, 0.f, worldDelta.z) / dt;
        e.setRotation(glm::normalize(e.localTransform().rotation * delta.rotation));
        return;
    }
    const TransformComponent& local = e.localTransform();
    anim::Transform owner{local.position, local.rotation, local.scale};
    anim::applyRootMotion(owner, delta);
    TransformComponent& t = e.transform();
    t.position = owner.translation;
    t.rotation = glm::normalize(owner.rotation);
}

void AnimationRuntime::applyIK(Entity e, const anim::Skeleton& skel, anim::Pose& pose,
                               std::vector<anim::Transform>& model) {
    const auto& ik = e.get<IKComponent>();
    const Transform owner = e.worldTransform();
    const Transform inv = owner.inverse();
    for (const IKChainDesc& chain : ik.chains) {
        if (!chain.enabled || chain.weight <= 0.f) continue;
        glm::vec3 targetWorld = chain.targetOffset;
        if (chain.target.valid()) {
            const Entity t = m_world->resolve(chain.target);
            if (!t.valid()) continue;
            targetWorld += t.worldPosition();
        }
        const glm::vec3 target = inv.transformPoint(targetWorld);
        if (chain.type == IKChainType::TwoBone) {
            anim::TwoBoneIKSettings s;
            s.root = skel.findJoint(chain.rootJoint);
            s.mid = skel.findJoint(chain.midJoint);
            s.end = skel.findJoint(chain.endJoint);
            if (s.root < 0 || s.mid < 0 || s.end < 0) continue;
            s.target = target;
            s.weight = chain.weight;
            if (chain.pole.valid()) {
                const Entity p = m_world->resolve(chain.pole);
                s.pole = p.valid() ? inv.transformPoint(p.worldPosition() + chain.poleOffset) : chain.poleOffset;
            } else {
                s.pole = chain.poleOffset;
            }
            anim::solveTwoBoneIK(skel, pose, model, s);
        } else {
            anim::AimIKSettings s;
            s.joint = skel.findJoint(chain.endJoint);
            if (s.joint < 0) continue;
            s.target = target;
            s.aimAxis = chain.aimAxis;
            s.weight = chain.weight;
            anim::solveAimIK(skel, pose, model, s);
        }
    }
}

void AnimationRuntime::writePalette(Entity e, const Record& rec) {
    std::vector<glm::mat4> modelMats;
    modelMats.reserve(rec.model.size());
    for (const auto& t : rec.model) modelMats.push_back(t.toMatrix());
    // Skinned meshes on this entity and on descendants without their own animator.
    std::vector<Entity> stack{e};
    while (!stack.empty()) {
        const Entity cur = stack.back();
        stack.pop_back();
        if (auto* sm = cur.tryGet<SkinnedMeshComponent>()) {
            anim::computeSkinningMatrices(*rec.skeleton, modelMats, sm->palette);
            ++sm->paletteVersion;
        }
        for (const Entity& child : cur.children()) {
            if (!child.has<AnimatorComponent>()) stack.push_back(child);
        }
    }
}

anim::Animator* AnimationRuntime::animator(Entity e) {
    if (!m_world || !e.valid() || !e.has<AnimatorComponent>()) return nullptr;
    Record* rec = ensure(e);
    return rec ? rec->animator.get() : nullptr;
}

const anim::Skeleton* AnimationRuntime::skeleton(Entity e) const {
    auto it = e.valid() ? m_records.find(e.handle()) : m_records.end();
    return it == m_records.end() || it->second.failed ? nullptr : it->second.skeleton.get();
}

const std::vector<anim::Transform>* AnimationRuntime::modelPose(Entity e) const {
    auto it = e.valid() ? m_records.find(e.handle()) : m_records.end();
    return it == m_records.end() || it->second.failed ? nullptr : &it->second.model;
}

void AnimationRuntime::setFloat(Entity e, std::string_view name, f32 value) {
    if (auto* c = e.tryGet<AnimatorComponent>()) {
        if (auto it = c->parameters.find(std::string(name)); it != c->parameters.end()) it->second = value;
    }
    if (auto* a = animator(e)) a->setFloat(name, value);
}

void AnimationRuntime::setBool(Entity e, std::string_view name, bool value) { setFloat(e, name, value ? 1.f : 0.f); }

void AnimationRuntime::setTrigger(Entity e, std::string_view name) {
    if (auto* a = animator(e)) a->setTrigger(name);
}

bool AnimationRuntime::play(Entity e, std::string_view state, u32 layer, f32 crossFade) {
    anim::Animator* a = animator(e);
    if (!a || layer >= a->controller().layers().size()) return false;
    const i32 s = a->controller().findState(layer, state);
    if (s < 0) return false;
    if (crossFade > 0.f) a->crossFade(layer, s, crossFade);
    else a->play(layer, s);
    return true;
}

void AnimationRuntime::drawDebug(DebugDraw& draw) {
    if (!debugDraw || !m_world) return;
    for (auto& [handle, rec] : m_records) {
        if (rec.failed || rec.model.empty() || !m_world->valid(handle)) continue;
        const Entity e = m_world->wrap(handle);
        anim::debugDrawSkeleton(*rec.skeleton, rec.model, toAnim(e.worldTransform()),
                                [&](glm::vec3 a, glm::vec3 b, glm::vec4 c) { draw.line(a, b, c); });
    }
}

} // namespace ox::gameplay
