#include "world_impl.hpp"

#include <oxwald/core/log.hpp>

#include <algorithm>

namespace ox::physics {

using detail::toGlm;
using detail::toHandle;
using detail::toJolt;
using detail::toJoltR;

namespace {

GroundState toGroundState(JPH::CharacterBase::EGroundState s) {
    switch (s) {
    case JPH::CharacterBase::EGroundState::OnGround: return GroundState::OnGround;
    case JPH::CharacterBase::EGroundState::OnSteepGround: return GroundState::OnSteepGround;
    case JPH::CharacterBase::EGroundState::NotSupported: return GroundState::NotSupported;
    default: return GroundState::InAir;
    }
}

// Runs a character update with the character layer's collision filters.
struct CharacterFilters {
    JPH::DefaultBroadPhaseLayerFilter bp;
    JPH::DefaultObjectLayerFilter obj;
    JPH::BodyFilter body;
    JPH::ShapeFilter shape;
    CharacterFilters(const PhysicsWorld::Impl& w, ObjectLayer layer)
        : bp(w.objVsBp, JPH::ObjectLayer(layer)), obj(w.objPair, JPH::ObjectLayer(layer)) {}
};

} // namespace

CharacterHandle PhysicsWorld::createCharacter(const CharacterDesc& desc) {
    auto& w = *m_impl;
    JPH::RefConst<JPH::Shape> shape;
    if (desc.customShape) {
        shape = static_cast<const JPH::Shape*>(desc.customShape.native());
    } else {
        f32 radius = std::max(0.01f, desc.radius);
        f32 halfCylinder = std::max(0.f, 0.5f * desc.height - radius);
        JPH::RefConst<JPH::Shape> capsule = halfCylinder > 0.f ? JPH::RefConst<JPH::Shape>(new JPH::CapsuleShape(halfCylinder, radius))
                                                                : JPH::RefConst<JPH::Shape>(new JPH::SphereShape(radius));
        // Origin at the feet.
        shape = JPH::RotatedTranslatedShapeSettings(JPH::Vec3(0.f, halfCylinder + radius, 0.f), JPH::Quat::sIdentity(),
                                                    capsule)
                    .Create()
                    .Get();
    }

    JPH::Ref<JPH::CharacterVirtualSettings> s = new JPH::CharacterVirtualSettings();
    s->mShape = shape;
    s->mUp = toJolt(glm::normalize(desc.up));
    s->mMaxSlopeAngle = desc.maxSlopeAngle;
    s->mMass = desc.mass;
    s->mMaxStrength = desc.maxStrength;
    s->mCharacterPadding = desc.characterPadding;
    s->mPenetrationRecoverySpeed = desc.penetrationRecoverySpeed;
    s->mPredictiveContactDistance = desc.predictiveContactDistance;
    // Only contacts with the lower hemisphere can support the character.
    s->mSupportingVolume = JPH::Plane(s->mUp, -std::max(0.01f, desc.radius));
    s->mEnhancedInternalEdgeRemoval = true;
    if (desc.innerBody) {
        s->mInnerBodyShape = shape;
        s->mInnerBodyLayer = JPH::ObjectLayer(desc.layer);
    }

    u32 index;
    if (!w.freeCharacters.empty()) {
        index = w.freeCharacters.back();
        w.freeCharacters.pop_back();
    } else {
        index = u32(w.characters.size());
        w.characters.emplace_back();
    }
    auto& slot = w.characters[index];
    slot.character = new JPH::CharacterVirtual(s, toJoltR(desc.position), toJolt(glm::normalize(desc.rotation)),
                                               desc.userData, w.system.get());
    slot.maxStepHeight = desc.maxStepHeight;
    slot.stickToFloorDistance = desc.stickToFloorDistance;
    slot.layer = desc.layer;
    slot.character->SetCharacterVsCharacterCollision(&w.charVsChar);
    w.charVsChar.Add(slot.character);
    JPH::BodyID inner = slot.character->GetInnerBodyID();
    if (!inner.IsInvalid()) {
        w.setBodyFlags(inner, detail::kFlagReportContacts);
    }

    CharacterFilters f(w, slot.layer);
    slot.character->RefreshContacts(f.bp, f.obj, f.body, f.shape, *w.tempAllocator);
    return CharacterHandle{index, slot.generation};
}

void PhysicsWorld::destroyCharacter(CharacterHandle h) {
    auto& w = *m_impl;
    auto* slot = w.character(h);
    if (!slot) {
        return;
    }
    JPH::BodyID inner = slot->character->GetInnerBodyID();
    w.charVsChar.Remove(slot->character);
    slot->character = nullptr; // destroys the inner body
    if (!inner.IsInvalid()) {
        w.setBodyFlags(inner, 0);
        w.purgeBodyPairs(inner.GetIndexAndSequenceNumber());
    }
    ++slot->generation;
    w.freeCharacters.push_back(h.index);
}

bool PhysicsWorld::isValid(CharacterHandle h) const { return m_impl->character(h) != nullptr; }

void PhysicsWorld::setCharacterVelocity(CharacterHandle h, const glm::vec3& v) {
    if (auto* slot = m_impl->character(h)) {
        slot->character->SetLinearVelocity(toJolt(v));
    }
}

void PhysicsWorld::updateCharacter(CharacterHandle h, f32 dt) {
    auto& w = *m_impl;
    auto* slot = w.character(h);
    if (!slot || dt <= 0.f) {
        return;
    }
    JPH::CharacterVirtual& ch = *slot->character;
    JPH::Vec3 up = ch.GetUp();
    JPH::CharacterVirtual::ExtendedUpdateSettings us;
    us.mStickToFloorStepDown = -up * slot->stickToFloorDistance;
    us.mWalkStairsStepUp = up * slot->maxStepHeight;
    CharacterFilters f(w, slot->layer);
    ch.ExtendedUpdate(dt, w.system->GetGravity(), us, f.bp, f.obj, f.body, f.shape, *w.tempAllocator);
}

void PhysicsWorld::moveCharacter(CharacterHandle h, f32 dt, const CharacterMoveInput& input) {
    auto& w = *m_impl;
    auto* slot = w.character(h);
    if (!slot || dt <= 0.f) {
        return;
    }
    JPH::CharacterVirtual& ch = *slot->character;
    ch.UpdateGroundVelocity();
    JPH::Vec3 up = ch.GetUp();
    JPH::Vec3 current = ch.GetLinearVelocity();
    JPH::Vec3 currentVertical = up * up.Dot(current);
    JPH::Vec3 currentHorizontal = current - currentVertical;
    JPH::Vec3 groundVelocity = ch.GetGroundVelocity();
    JPH::Vec3 desired = toJolt(input.desiredVelocity);
    desired -= up * up.Dot(desired);

    bool movingTowardsGround = (currentVertical - up * up.Dot(groundVelocity)).Dot(up) < 0.1f;
    bool onGround = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;

    JPH::Vec3 velocity;
    if (onGround && movingTowardsGround) {
        // Inherit the platform velocity (moving platforms) and allow jumping.
        velocity = groundVelocity;
        if (input.jump) {
            velocity += up * input.jumpSpeed;
        }
        velocity += desired;
    } else {
        f32 control = std::clamp(input.airControl, 0.f, 1.f);
        velocity = currentVertical + currentHorizontal + (desired - currentHorizontal) * control;
    }
    velocity += w.system->GetGravity() * dt;
    ch.SetLinearVelocity(velocity);
    updateCharacter(h, dt);
}

void PhysicsWorld::setCharacterTransform(CharacterHandle h, const glm::vec3& position, const glm::quat& rotation) {
    auto& w = *m_impl;
    auto* slot = w.character(h);
    if (!slot) {
        return;
    }
    slot->character->SetPosition(toJoltR(position));
    slot->character->SetRotation(toJolt(glm::normalize(rotation)));
    CharacterFilters f(w, slot->layer);
    slot->character->RefreshContacts(f.bp, f.obj, f.body, f.shape, *w.tempAllocator);
}

bool PhysicsWorld::setCharacterShape(CharacterHandle h, const ShapeRef& shape, f32 maxPenetration) {
    auto& w = *m_impl;
    auto* slot = w.character(h);
    if (!slot || !shape) {
        return false;
    }
    CharacterFilters f(w, slot->layer);
    bool ok = slot->character->SetShape(static_cast<const JPH::Shape*>(shape.native()), maxPenetration, f.bp, f.obj,
                                        f.body, f.shape, *w.tempAllocator);
    if (ok && !slot->character->GetInnerBodyID().IsInvalid()) {
        slot->character->SetInnerBodyShape(static_cast<const JPH::Shape*>(shape.native()));
    }
    return ok;
}

CharacterState PhysicsWorld::getCharacterState(CharacterHandle h) const {
    CharacterState st;
    const auto* slot = m_impl->character(h);
    if (!slot) {
        return st;
    }
    const JPH::CharacterVirtual& ch = *slot->character;
    st.position = toGlm(ch.GetPosition());
    st.rotation = toGlm(ch.GetRotation());
    st.linearVelocity = toGlm(ch.GetLinearVelocity());
    st.groundState = toGroundState(ch.GetGroundState());
    st.groundNormal = toGlm(ch.GetGroundNormal());
    st.groundVelocity = toGlm(ch.GetGroundVelocity());
    st.groundBody = ch.GetGroundBodyID().IsInvalid() ? BodyHandle{} : toHandle(ch.GetGroundBodyID());
    st.groundUserData = ch.GetGroundUserData();
    st.slopeTooSteep = ch.IsSlopeTooSteep(ch.GetGroundNormal());
    return st;
}

BodyHandle PhysicsWorld::getCharacterInnerBody(CharacterHandle h) const {
    const auto* slot = m_impl->character(h);
    if (!slot || slot->character->GetInnerBodyID().IsInvalid()) {
        return {};
    }
    return toHandle(slot->character->GetInnerBodyID());
}

} // namespace ox::physics
