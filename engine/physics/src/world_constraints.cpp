#include "world_impl.hpp"

#include <oxwald/core/log.hpp>

#include <algorithm>
#include <cmath>

namespace ox::physics {

using detail::toGlm;
using detail::toJolt;
using detail::toJoltR;

namespace {

JPH::Vec3 perpendicular(const glm::vec3& axis, const glm::vec3& normal) {
    JPH::Vec3 a = toJolt(glm::normalize(axis));
    if (glm::length(normal) > 1e-6f) {
        // Orthogonalise the user normal against the axis.
        JPH::Vec3 n = toJolt(normal);
        n = n - a * a.Dot(n);
        if (n.LengthSq() > 1e-10f) {
            return n.Normalized();
        }
    }
    return a.GetNormalizedPerpendicular();
}

JPH::MotorSettings motorSettings(const ConstraintDesc& d, bool angular) {
    JPH::MotorSettings m(d.motorFrequency, d.motorDamping);
    if (angular) {
        m.SetTorqueLimit(d.motorMaxForce);
    } else {
        m.SetForceLimit(d.motorMaxForce);
    }
    return m;
}

JPH::EMotorState toJolt(MotorMode m) {
    switch (m) {
    case MotorMode::Velocity: return JPH::EMotorState::Velocity;
    case MotorMode::Position: return JPH::EMotorState::Position;
    default: return JPH::EMotorState::Off;
    }
}

void applyMotor(JPH::TwoBodyConstraint* c, ConstraintType type, MotorMode mode, f32 target) {
    if (type == ConstraintType::Hinge) {
        auto* h = static_cast<JPH::HingeConstraint*>(c);
        h->SetMotorState(toJolt(mode));
        if (mode == MotorMode::Velocity) {
            h->SetTargetAngularVelocity(target);
        } else if (mode == MotorMode::Position) {
            h->SetTargetAngle(target);
        }
    } else if (type == ConstraintType::Slider) {
        auto* s = static_cast<JPH::SliderConstraint*>(c);
        s->SetMotorState(toJolt(mode));
        if (mode == MotorMode::Velocity) {
            s->SetTargetVelocity(target);
        } else if (mode == MotorMode::Position) {
            s->SetTargetPosition(target);
        }
    }
}

} // namespace

ConstraintHandle PhysicsWorld::createConstraint(const ConstraintDesc& d) {
    auto& w = *m_impl;
    if (!isValid(d.bodyA) || (d.bodyB.valid() && !isValid(d.bodyB))) {
        OX_LOG_ERROR("physics", "createConstraint: invalid body");
        return {};
    }

    JPH::Ref<JPH::TwoBodyConstraintSettings> settings;
    JPH::RVec3 pA = toJoltR(d.pointA);
    switch (d.type) {
    case ConstraintType::Fixed: {
        auto* s = new JPH::FixedConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = s->mPoint2 = pA;
        settings = s;
        break;
    }
    case ConstraintType::Point: {
        auto* s = new JPH::PointConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = toJoltR(d.pointB);
        s->mPoint2 = pA;
        settings = s;
        break;
    }
    case ConstraintType::Hinge: {
        auto* s = new JPH::HingeConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = s->mPoint2 = pA;
        s->mHingeAxis1 = s->mHingeAxis2 = toJolt(glm::normalize(d.axis));
        s->mNormalAxis1 = s->mNormalAxis2 = perpendicular(d.axis, d.normal);
        if (d.limitsEnabled) {
            s->mLimitsMin = std::max(-JPH::JPH_PI, d.limitMin);
            s->mLimitsMax = std::min(JPH::JPH_PI, d.limitMax);
        }
        s->mMotorSettings = motorSettings(d, true);
        settings = s;
        break;
    }
    case ConstraintType::Slider: {
        auto* s = new JPH::SliderConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = s->mPoint2 = pA;
        s->mSliderAxis1 = s->mSliderAxis2 = toJolt(glm::normalize(d.axis));
        s->mNormalAxis1 = s->mNormalAxis2 = perpendicular(d.axis, d.normal);
        if (d.limitsEnabled) {
            s->mLimitsMin = d.limitMin;
            s->mLimitsMax = d.limitMax;
        }
        s->mMotorSettings = motorSettings(d, false);
        settings = s;
        break;
    }
    case ConstraintType::Distance: {
        auto* s = new JPH::DistanceConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = toJoltR(d.pointB);
        s->mPoint2 = pA;
        s->mMinDistance = d.minDistance;
        s->mMaxDistance = d.maxDistance;
        s->mLimitsSpringSettings.mFrequency = d.springFrequency;
        s->mLimitsSpringSettings.mDamping = d.springDamping;
        settings = s;
        break;
    }
    case ConstraintType::Cone: {
        auto* s = new JPH::ConeConstraintSettings();
        s->mSpace = JPH::EConstraintSpace::WorldSpace;
        s->mPoint1 = s->mPoint2 = pA;
        s->mTwistAxis1 = s->mTwistAxis2 = toJolt(glm::normalize(d.axis));
        s->mHalfConeAngle = d.coneHalfAngle;
        settings = s;
        break;
    }
    }

    // Jolt measures joint values (hinge angle, slider position) of body 2 relative to body 1, so bodyB (or the
    // static world) becomes Jolt's body 1 and bodyA its body 2: values then describe how A moves relative to B.
    JPH::TwoBodyConstraint* c = nullptr;
    if (d.bodyB.valid()) {
        JPH::BodyID ids[2] = {toJolt(d.bodyB), toJolt(d.bodyA)};
        JPH::BodyLockMultiWrite lock(w.locks(), ids, 2);
        if (lock.GetBody(0) && lock.GetBody(1)) {
            c = settings->Create(*lock.GetBody(0), *lock.GetBody(1));
        }
    } else {
        JPH::BodyLockWrite lock(w.locks(), toJolt(d.bodyA));
        if (lock.Succeeded()) {
            c = settings->Create(JPH::Body::sFixedToWorld, lock.GetBody());
        }
    }
    if (!c) {
        OX_LOG_ERROR("physics", "createConstraint: Jolt failed to create the constraint");
        return {};
    }
    w.system->AddConstraint(c);
    w.bodies().ActivateConstraint(c);

    u32 index;
    if (!w.freeConstraints.empty()) {
        index = w.freeConstraints.back();
        w.freeConstraints.pop_back();
    } else {
        index = u32(w.constraints.size());
        w.constraints.emplace_back();
    }
    auto& slot = w.constraints[index];
    slot.constraint = c;
    slot.type = d.type;
    slot.breakForce = d.breakForce;
    slot.breakTorque = d.breakTorque;
    slot.a = d.bodyA;
    slot.b = d.bodyB;
    if (d.motorMode != MotorMode::Off) {
        applyMotor(c, d.type, d.motorMode, d.motorTarget);
    }
    return ConstraintHandle{index, slot.generation};
}

void PhysicsWorld::destroyConstraint(ConstraintHandle h) {
    auto& w = *m_impl;
    auto* slot = w.constraint(h);
    if (!slot) {
        return;
    }
    w.system->RemoveConstraint(slot->constraint);
    slot->constraint = nullptr;
    ++slot->generation;
    w.freeConstraints.push_back(h.index);
}

bool PhysicsWorld::isValid(ConstraintHandle h) const { return m_impl->constraint(h) != nullptr; }

void PhysicsWorld::setConstraintEnabled(ConstraintHandle h, bool enabled) {
    if (auto* slot = m_impl->constraint(h)) {
        slot->constraint->SetEnabled(enabled);
        if (enabled) {
            m_impl->bodies().ActivateConstraint(slot->constraint);
        }
    }
}

bool PhysicsWorld::isConstraintEnabled(ConstraintHandle h) const {
    const auto* slot = m_impl->constraint(h);
    return slot && slot->constraint->GetEnabled();
}

void PhysicsWorld::setMotor(ConstraintHandle h, MotorMode mode, f32 target) {
    if (auto* slot = m_impl->constraint(h)) {
        applyMotor(slot->constraint, slot->type, mode, target);
        m_impl->bodies().ActivateConstraint(slot->constraint);
    }
}

f32 PhysicsWorld::getJointValue(ConstraintHandle h) const {
    const auto* slot = m_impl->constraint(h);
    if (!slot) {
        return 0.f;
    }
    switch (slot->type) {
    case ConstraintType::Hinge: return static_cast<const JPH::HingeConstraint*>(slot->constraint.GetPtr())->GetCurrentAngle();
    case ConstraintType::Slider:
        return static_cast<const JPH::SliderConstraint*>(slot->constraint.GetPtr())->GetCurrentPosition();
    case ConstraintType::Distance: {
        const auto* c = slot->constraint.GetPtr();
        JPH::RVec3 a = c->GetBody1()->GetCenterOfMassTransform() * c->GetConstraintToBody1Matrix().GetTranslation();
        JPH::RVec3 b = c->GetBody2()->GetCenterOfMassTransform() * c->GetConstraintToBody2Matrix().GetTranslation();
        return f32((b - a).Length());
    }
    default: return 0.f;
    }
}

void PhysicsWorld::Impl::checkBreakableConstraints() {
    if (lastSubStepDt <= 0.f) {
        return;
    }
    const f32 invDt = 1.f / lastSubStepDt; // lambdas are impulses of the last sub-step
    for (u32 i = 0; i < constraints.size(); ++i) {
        auto& slot = constraints[i];
        if (!slot.constraint || !slot.constraint->GetEnabled() ||
            (std::isinf(slot.breakForce) && std::isinf(slot.breakTorque))) {
            continue;
        }
        f32 linear = 0.f, angular = 0.f;
        JPH::TwoBodyConstraint* c = slot.constraint;
        switch (slot.type) {
        case ConstraintType::Fixed: {
            auto* f = static_cast<JPH::FixedConstraint*>(c);
            linear = f->GetTotalLambdaPosition().Length();
            angular = f->GetTotalLambdaRotation().Length();
            break;
        }
        case ConstraintType::Point:
            linear = static_cast<JPH::PointConstraint*>(c)->GetTotalLambdaPosition().Length();
            break;
        case ConstraintType::Hinge: {
            auto* h = static_cast<JPH::HingeConstraint*>(c);
            linear = h->GetTotalLambdaPosition().Length();
            JPH::Vector<2> r = h->GetTotalLambdaRotation();
            angular = std::sqrt(r[0] * r[0] + r[1] * r[1]) + std::abs(h->GetTotalLambdaRotationLimits());
            break;
        }
        case ConstraintType::Slider: {
            auto* s = static_cast<JPH::SliderConstraint*>(c);
            JPH::Vector<2> p = s->GetTotalLambdaPosition();
            linear = std::sqrt(p[0] * p[0] + p[1] * p[1]) + std::abs(s->GetTotalLambdaPositionLimits());
            angular = s->GetTotalLambdaRotation().Length();
            break;
        }
        case ConstraintType::Distance:
            linear = std::abs(static_cast<JPH::DistanceConstraint*>(c)->GetTotalLambdaPosition());
            break;
        case ConstraintType::Cone: {
            auto* k = static_cast<JPH::ConeConstraint*>(c);
            linear = k->GetTotalLambdaPosition().Length();
            angular = std::abs(k->GetTotalLambdaRotation());
            break;
        }
        }
        f32 force = linear * invDt, torque = angular * invDt;
        if (force > slot.breakForce || torque > slot.breakTorque) {
            c->SetEnabled(false);
            brokenEvents.push_back(ConstraintBrokenEvent{ConstraintHandle{i, slot.generation}, force, torque});
        }
    }
}

} // namespace ox::physics
