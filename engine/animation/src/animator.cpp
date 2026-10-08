#include <oxwald/animation/animator.hpp>

#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>

#include <cmath>

namespace ox::anim {

namespace {

constexpr f32 kWeightEpsilon = 1e-4f;

const AnimationClip* clipAt(const Motion& m, usize i) {
    switch (m.kind) {
    case Motion::Kind::Clip: return m.clip.get();
    case Motion::Kind::BlendSpace1D: return m.space1D->samples()[i].clip.get();
    case Motion::Kind::BlendSpace2D: return m.space2D->samples()[i].clip.get();
    default: return nullptr;
    }
}

usize clipCount(const Motion& m) {
    switch (m.kind) {
    case Motion::Kind::Clip: return m.clip ? 1 : 0;
    case Motion::Kind::BlendSpace1D: return m.space1D ? m.space1D->samples().size() : 0;
    case Motion::Kind::BlendSpace2D: return m.space2D ? m.space2D->samples().size() : 0;
    default: return 0;
    }
}

// Clip-local time for a normalised time (unbounded for looping states).
f32 phaseTime(f32 norm, f32 duration, bool loop) {
    if (!loop) {
        return std::clamp(norm, 0.0f, 1.0f) * duration;
    }
    return (norm - std::floor(norm)) * duration;
}

} // namespace

Motion Motion::fromClip(std::shared_ptr<const AnimationClip> clip) {
    Motion m;
    m.kind = Kind::Clip;
    m.clip = std::move(clip);
    return m;
}

Motion Motion::fromBlendSpace(std::shared_ptr<const BlendSpace1D> space, u32 paramX) {
    Motion m;
    m.kind = Kind::BlendSpace1D;
    m.space1D = std::move(space);
    m.paramX = static_cast<i32>(paramX);
    return m;
}

Motion Motion::fromBlendSpace(std::shared_ptr<const BlendSpace2D> space, u32 paramX, u32 paramY) {
    Motion m;
    m.kind = Kind::BlendSpace2D;
    m.space2D = std::move(space);
    m.paramX = static_cast<i32>(paramX);
    m.paramY = static_cast<i32>(paramY);
    return m;
}

// ---- AnimatorController ----

u32 AnimatorController::addParameter(std::string name, ParamType type, f32 defaultValue) {
    m_params.push_back({std::move(name), type, defaultValue});
    return static_cast<u32>(m_params.size() - 1);
}

i32 AnimatorController::findParameter(std::string_view name) const {
    for (usize i = 0; i < m_params.size(); ++i) {
        if (m_params[i].name == name) return static_cast<i32>(i);
    }
    return -1;
}

u32 AnimatorController::addLayer(std::string name, LayerBlend blend, f32 weight) {
    LayerDesc l;
    l.name = std::move(name);
    l.blend = blend;
    l.weight = weight;
    m_layers.push_back(std::move(l));
    return static_cast<u32>(m_layers.size() - 1);
}

i32 AnimatorController::addState(u32 layer, StateDesc state) {
    auto& states = m_layers[layer].states;
    states.push_back(std::move(state));
    return static_cast<i32>(states.size() - 1);
}

i32 AnimatorController::findState(u32 layer, std::string_view name) const {
    const auto& states = m_layers[layer].states;
    for (usize i = 0; i < states.size(); ++i) {
        if (states[i].name == name) return static_cast<i32>(i);
    }
    return -1;
}

TransitionDesc& AnimatorController::addTransition(u32 layer, i32 from, i32 to, f32 duration) {
    TransitionDesc t;
    t.from = from;
    t.to = to;
    t.duration = duration;
    auto& list = m_layers[layer].transitions;
    list.push_back(std::move(t));
    return list.back();
}

// ---- Animator ----

Animator::Animator(const Skeleton& skeleton, std::shared_ptr<const AnimatorController> controller)
    : Animator(std::shared_ptr<const Skeleton>(std::shared_ptr<const Skeleton>{}, &skeleton), std::move(controller)) {}

Animator::Animator(std::shared_ptr<const Skeleton> skeletonPtr, std::shared_ptr<const AnimatorController> controller)
    : m_skeleton(std::move(skeletonPtr)), m_controller(std::move(controller)) {
    OX_ASSERT(m_skeleton != nullptr, "Animator needs a skeleton");
    OX_ASSERT(m_controller != nullptr, "Animator needs a controller");
    const Skeleton& skeleton = *m_skeleton;
    for (const auto& p : m_controller->parameters()) {
        m_params.push_back(p.defaultValue);
    }
    m_bind.setBind(skeleton);
    m_pose = m_bind;
    m_layers.resize(m_controller->layers().size());
    for (usize i = 0; i < m_layers.size(); ++i) {
        const LayerDesc& d = m_controller->layers()[i];
        m_layers[i].weight = d.weight;
        if (!d.states.empty()) {
            m_layers[i].current.state = std::clamp(d.defaultState, 0, static_cast<i32>(d.states.size()) - 1);
        }
    }
}

void Animator::setFloat(u32 param, f32 value) {
    OX_ASSERT(param < m_params.size(), "parameter index {} out of range", param);
    m_params[param] = value;
}

void Animator::setFloat(std::string_view name, f32 value) {
    const i32 p = m_controller->findParameter(name);
    if (p < 0) {
        OX_LOG_WARN("animation", "unknown animator parameter '{}'", name);
        return;
    }
    m_params[static_cast<usize>(p)] = value;
}

void Animator::setBool(std::string_view name, bool value) { setFloat(name, value ? 1.0f : 0.0f); }
void Animator::setTrigger(std::string_view name) { setFloat(name, 1.0f); }

Pose& Animator::scratch(usize index) {
    while (m_scratch.size() <= index) {
        m_scratch.emplace_back();
    }
    return m_scratch[index];
}

void Animator::motionWeights(const Motion& motion, std::vector<f32>& weights) const {
    switch (motion.kind) {
    case Motion::Kind::Clip:
        weights.assign(1, 1.0f);
        break;
    case Motion::Kind::BlendSpace1D:
        motion.space1D->computeWeights(motion.paramX >= 0 ? m_params[static_cast<usize>(motion.paramX)] : 0.0f,
                                       weights);
        break;
    case Motion::Kind::BlendSpace2D: {
        const glm::vec2 p(motion.paramX >= 0 ? m_params[static_cast<usize>(motion.paramX)] : 0.0f,
                          motion.paramY >= 0 ? m_params[static_cast<usize>(motion.paramY)] : 0.0f);
        motion.space2D->computeWeights(p, weights);
        break;
    }
    default:
        weights.clear();
    }
}

f32 Animator::motionDuration(const Motion& motion, std::vector<f32>& weights) const {
    motionWeights(motion, weights);
    f32 d = 0.0f;
    for (usize i = 0; i < weights.size(); ++i) {
        if (weights[i] > 0.0f) {
            d += weights[i] * clipAt(motion, i)->duration;
        }
    }
    return d;
}

void Animator::advance(u32 layerIndex, StateInstance& inst, f32 dt, f32 blendWeight, bool emit) {
    const LayerDesc& layer = m_controller->layers()[layerIndex];
    if (inst.state < 0) return;
    const StateDesc& state = layer.states[static_cast<usize>(inst.state)];
    const f32 duration = motionDuration(state.motion, m_weights);
    if (duration <= 0.0f) {
        inst.prevNormTime = inst.normTime;
        return;
    }
    const f32 speed = state.speed * (state.speedParam >= 0 ? m_params[static_cast<usize>(state.speedParam)] : 1.0f);
    const f32 prev = inst.normTime;
    f32 next = prev + dt * speed / duration;
    if (!state.loop) {
        next = std::clamp(next, 0.0f, 1.0f);
    }
    inst.prevNormTime = prev;
    inst.normTime = next;

    const bool rootMotion = layerIndex == 0;
    for (usize i = 0; i < m_weights.size(); ++i) {
        const f32 w = m_weights[i] * blendWeight;
        if (w <= kWeightEpsilon) continue;
        const AnimationClip* clip = clipAt(state.motion, i);
        const f32 cd = clip->duration;
        const f32 t0 = phaseTime(prev, cd, state.loop);
        const f32 t1 = phaseTime(next, cd, state.loop);
        const i32 wraps = state.loop ? static_cast<i32>(std::floor(next) - std::floor(prev)) : 0;

        if (rootMotion && !clip->rootMotion.empty()) {
            const Transform d = clip->rootMotion.deltaLooped(t0, t1, wraps, cd);
            m_rootAccumT += d.translation * w;
            const f32 sign = (m_rootAccumW > 0.0f && glm::dot(m_rootAccumR, d.rotation) < 0.0f) ? -1.0f : 1.0f;
            m_rootAccumR += d.rotation * (w * sign);
            m_rootAccumW += w;
        }

        if (emit && !clip->events.empty() && next >= prev) {
            m_eventScratch.clear();
            const f32 start = inst.justStarted ? t0 - 1e-5f : t0;
            if (wraps == 0) {
                clip->collectEvents(start, t1, m_eventScratch);
            } else {
                clip->collectEvents(start, cd, m_eventScratch);
                for (i32 k = 1; k < wraps; ++k) clip->collectEvents(-1.0f, cd, m_eventScratch);
                clip->collectEvents(-1.0f, t1, m_eventScratch);
            }
            for (const AnimEvent* e : m_eventScratch) {
                m_events.push_back({e, layerIndex, inst.state, w});
            }
        }
    }
    inst.justStarted = false;
}

void Animator::sampleState(const LayerDesc& layer, StateInstance& inst, Pose& out, usize scratchBase) {
    const bool additive = layer.blend == LayerBlend::Additive;
    if (inst.state < 0) {
        if (additive) out.setIdentity(m_skeleton->jointCount()); else out = m_bind;
        return;
    }
    const StateDesc& state = layer.states[static_cast<usize>(inst.state)];
    const usize n = clipCount(state.motion);
    if (n == 0) {
        if (additive) out.setIdentity(m_skeleton->jointCount()); else out = m_bind;
        return;
    }
    inst.cursors.resize(n);
    motionWeights(state.motion, m_weights);
    usize active = 0, last = 0;
    for (usize i = 0; i < n; ++i) {
        if (m_weights[i] > kWeightEpsilon) {
            ++active;
            last = i;
        }
    }
    if (active <= 1) {
        const AnimationClip* clip = clipAt(state.motion, last);
        clip->sample(*m_skeleton, phaseTime(inst.normTime, clip->duration, state.loop), out, &inst.cursors[last]);
        return;
    }
    PoseAccumulator acc;
    acc.begin(m_skeleton->jointCount());
    Pose& tmp = scratch(scratchBase);
    const std::vector<f32> weights = m_weights; // sampling must not clobber the weights we iterate
    for (usize i = 0; i < n; ++i) {
        if (weights[i] <= kWeightEpsilon) continue;
        const AnimationClip* clip = clipAt(state.motion, i);
        clip->sample(*m_skeleton, phaseTime(inst.normTime, clip->duration, state.loop), tmp, &inst.cursors[i]);
        acc.add(tmp, weights[i]);
    }
    acc.finish(out);
}

bool Animator::conditionsHold(const TransitionDesc& t, const StateInstance& source, const LayerDesc& layer) const {
    for (const Condition& c : t.conditions) {
        const f32 v = m_params[c.param];
        bool ok = false;
        switch (c.op) {
        case ConditionOp::Greater: ok = v > c.threshold; break;
        case ConditionOp::Less: ok = v < c.threshold; break;
        case ConditionOp::Equal: ok = std::abs(v - c.threshold) < 1e-4f; break;
        case ConditionOp::NotEqual: ok = std::abs(v - c.threshold) >= 1e-4f; break;
        case ConditionOp::IsTrue:
        case ConditionOp::Triggered: ok = v != 0.0f; break;
        case ConditionOp::IsFalse: ok = v == 0.0f; break;
        }
        if (!ok) return false;
    }
    if (t.hasExitTime) {
        if (source.state < 0) return false;
        const bool loop = layer.states[static_cast<usize>(source.state)].loop;
        if (!loop || t.exitTime >= 1.0f) {
            return source.normTime >= t.exitTime;
        }
        // Looping state with fractional exit time: fire on the frame the exit point is crossed in any cycle.
        return std::floor(source.normTime - t.exitTime) > std::floor(source.prevNormTime - t.exitTime);
    }
    return true;
}

void Animator::consumeTriggers(const TransitionDesc& t) {
    const auto& params = m_controller->parameters();
    for (const Condition& c : t.conditions) {
        if (c.op == ConditionOp::Triggered || params[c.param].type == ParamType::Trigger) {
            m_params[c.param] = 0.0f;
        }
    }
}

void Animator::startTransition(u32 layerIndex, i32 to, f32 duration, f32 offset) {
    LayerRuntime& L = m_layers[layerIndex];
    StateInstance inst;
    inst.state = to;
    inst.normTime = offset;
    inst.prevNormTime = offset;
    if (duration <= 0.0f) {
        L.current = std::move(inst);
        L.transitioning = false;
        L.frozenSource = false;
        return;
    }
    if (L.transitioning) {
        // Interrupted mid-blend: freeze the last output as the source instead of juggling three states.
        L.frozen = L.output;
        L.frozenSource = true;
        L.current = std::move(L.next);
    } else {
        L.frozenSource = false;
    }
    L.next = std::move(inst);
    L.transitioning = true;
    L.transitionTime = 0.0f;
    L.transitionDuration = duration;
}

void Animator::evaluateTransitions(u32 layerIndex) {
    LayerRuntime& L = m_layers[layerIndex];
    const LayerDesc& D = m_controller->layers()[layerIndex];
    const StateInstance& active = L.transitioning ? L.next : L.current;
    for (const TransitionDesc& t : D.transitions) {
        if (t.from != kAnyState) continue;
        if (!t.canTransitionToSelf && t.to == active.state) continue;
        if (conditionsHold(t, active, D)) {
            consumeTriggers(t);
            startTransition(layerIndex, t.to, t.duration, t.destinationOffset);
            return;
        }
    }
    if (L.transitioning) return;
    for (const TransitionDesc& t : D.transitions) {
        if (t.from != L.current.state) continue;
        if (conditionsHold(t, L.current, D)) {
            consumeTriggers(t);
            startTransition(layerIndex, t.to, t.duration, t.destinationOffset);
            return;
        }
    }
}

void Animator::update(f32 dt) {
    m_events.clear();
    m_rootAccumT = glm::vec3(0.0f);
    m_rootAccumR = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
    m_rootAccumW = 0.0f;
    const usize joints = m_skeleton->jointCount();

    for (u32 li = 0; li < m_layers.size(); ++li) {
        LayerRuntime& L = m_layers[li];
        const LayerDesc& D = m_controller->layers()[li];
        if (D.states.empty()) {
            if (D.blend == LayerBlend::Additive) L.output.setIdentity(joints); else L.output = m_bind;
            continue;
        }
        const f32 progress = L.transitioning ? std::clamp(L.transitionTime / L.transitionDuration, 0.0f, 1.0f) : 0.0f;
        if (!L.transitioning || !L.frozenSource) {
            advance(li, L.current, dt, 1.0f - progress, true);
        }
        if (L.transitioning) {
            advance(li, L.next, dt, progress, true);
            L.transitionTime += dt;
        }
        evaluateTransitions(li);
        if (L.transitioning && L.transitionTime >= L.transitionDuration) {
            L.current = std::move(L.next);
            L.next = StateInstance{};
            L.transitioning = false;
            L.frozenSource = false;
        }

        if (!L.transitioning) {
            sampleState(D, L.current, L.output, 2);
        } else {
            const f32 w = std::clamp(L.transitionTime / L.transitionDuration, 0.0f, 1.0f);
            Pose& src = scratch(0);
            Pose& dst = scratch(1);
            if (L.frozenSource) {
                if (L.frozen.size() == joints) src = L.frozen; else src = m_bind;
            } else {
                sampleState(D, L.current, src, 2);
            }
            sampleState(D, L.next, dst, 2);
            blendPoses(src, dst, w, L.output);
        }
    }

    // Compose layers.
    if (!m_layers.empty() && m_controller->layers()[0].blend == LayerBlend::Override) {
        m_pose = m_layers[0].output;
    } else {
        m_pose = m_bind;
    }
    for (u32 li = 0; li < m_layers.size(); ++li) {
        const LayerDesc& D = m_controller->layers()[li];
        const LayerRuntime& L = m_layers[li];
        if (li == 0 && D.blend == LayerBlend::Override) continue;
        if (L.weight <= 0.0f || D.states.empty()) continue;
        const JointMask* mask = D.mask.empty() ? nullptr : &D.mask;
        if (D.blend == LayerBlend::Override) {
            blendPoses(m_pose, L.output, L.weight, m_pose, mask);
        } else {
            applyAdditive(m_pose, L.output, L.weight, mask);
        }
    }

    if (m_rootAccumW > 0.0f) {
        m_rootDelta.translation = m_rootAccumT / m_rootAccumW;
        m_rootDelta.rotation = glm::normalize(m_rootAccumR);
    } else {
        m_rootDelta = Transform{};
    }
    m_rootDelta.scale = glm::vec3(1.0f);
}

void Animator::play(u32 layer, i32 state, f32 normalizedTime) {
    startTransition(layer, state, 0.0f, normalizedTime);
}

void Animator::crossFade(u32 layer, i32 state, f32 duration, f32 normalizedTime) {
    startTransition(layer, state, duration, normalizedTime);
}

i32 Animator::currentState(u32 layer) const { return m_layers[layer].current.state; }
i32 Animator::nextState(u32 layer) const { return m_layers[layer].transitioning ? m_layers[layer].next.state : -1; }
bool Animator::inTransition(u32 layer) const { return m_layers[layer].transitioning; }
f32 Animator::normalizedTime(u32 layer) const { return m_layers[layer].current.normTime; }
f32 Animator::transitionProgress(u32 layer) const {
    const auto& L = m_layers[layer];
    return L.transitioning ? std::clamp(L.transitionTime / L.transitionDuration, 0.0f, 1.0f) : 0.0f;
}
void Animator::setLayerWeight(u32 layer, f32 weight) { m_layers[layer].weight = weight; }
f32 Animator::layerWeight(u32 layer) const { return m_layers[layer].weight; }

void applyRootMotion(Transform& owner, const Transform& delta) {
    owner.translation += owner.rotation * (owner.scale * delta.translation);
    owner.rotation = glm::normalize(owner.rotation * delta.rotation);
}

} // namespace ox::anim
