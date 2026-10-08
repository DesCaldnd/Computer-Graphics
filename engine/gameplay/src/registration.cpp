#include <oxwald/core/reflect.hpp>
#include <oxwald/gameplay/gameplay.hpp>
#include <oxwald/scene/component_registry.hpp>
#include <oxwald/scene/scene.hpp>

namespace ox {

namespace {

using namespace ox::gameplay;
using attr::Category;
using attr::DisplayName;
using attr::Hidden;
using attr::Meta;
using attr::NoSerialize;
using attr::Range;
using attr::ReadOnly;
using attr::Replicated;
using attr::SaveGame;
using attr::Step;
using attr::Tooltip;

void registerEnums() {
    OX_REFLECT_ENUM(physics::MotionType, "PhysicsMotionType")
        .value("Static", physics::MotionType::Static)
        .value("Kinematic", physics::MotionType::Kinematic)
        .value("Dynamic", physics::MotionType::Dynamic);
    OX_REFLECT_ENUM(physics::GroundState, "CharacterGroundState")
        .value("OnGround", physics::GroundState::OnGround)
        .value("OnSteepGround", physics::GroundState::OnSteepGround)
        .value("NotSupported", physics::GroundState::NotSupported)
        .value("InAir", physics::GroundState::InAir);
    OX_REFLECT_ENUM(physics::ConstraintType, "JointType")
        .value("Fixed", physics::ConstraintType::Fixed)
        .value("Point", physics::ConstraintType::Point)
        .value("Hinge", physics::ConstraintType::Hinge)
        .value("Slider", physics::ConstraintType::Slider)
        .value("Distance", physics::ConstraintType::Distance)
        .value("Cone", physics::ConstraintType::Cone);
    OX_REFLECT_ENUM(physics::MotorMode, "JointMotorMode")
        .value("Off", physics::MotorMode::Off)
        .value("Velocity", physics::MotorMode::Velocity)
        .value("Position", physics::MotorMode::Position);
    OX_REFLECT_ENUM(ColliderType, "ColliderType")
        .value("Box", ColliderType::Box)
        .value("Sphere", ColliderType::Sphere)
        .value("Capsule", ColliderType::Capsule)
        .value("Cylinder", ColliderType::Cylinder)
        .value("ConvexHull", ColliderType::ConvexHull)
        .value("Mesh", ColliderType::Mesh)
        .value("HeightField", ColliderType::HeightField)
        .value("Compound", ColliderType::Compound);

    OX_REFLECT_ENUM(anim::ParamType, "AnimatorParamType")
        .value("Float", anim::ParamType::Float)
        .value("Int", anim::ParamType::Int)
        .value("Bool", anim::ParamType::Bool)
        .value("Trigger", anim::ParamType::Trigger);
    OX_REFLECT_ENUM(anim::ConditionOp, "AnimatorConditionOp")
        .value("Greater", anim::ConditionOp::Greater)
        .value("Less", anim::ConditionOp::Less)
        .value("Equal", anim::ConditionOp::Equal)
        .value("NotEqual", anim::ConditionOp::NotEqual)
        .value("IsTrue", anim::ConditionOp::IsTrue)
        .value("IsFalse", anim::ConditionOp::IsFalse)
        .value("Triggered", anim::ConditionOp::Triggered);
    OX_REFLECT_ENUM(anim::SkinningMethod, "SkinningMethod")
        .value("Linear", anim::SkinningMethod::Linear)
        .value("DualQuaternion", anim::SkinningMethod::DualQuaternion);
    OX_REFLECT_ENUM(IKChainType, "IKChainType").value("TwoBone", IKChainType::TwoBone).value("Aim", IKChainType::Aim);

    OX_REFLECT_ENUM(spline::SplineType, "SplineType")
        .value("Linear", spline::SplineType::Linear)
        .value("Bezier", spline::SplineType::Bezier)
        .value("CatmullRom", spline::SplineType::CatmullRom)
        .value("BSpline", spline::SplineType::BSpline)
        .value("Nurbs", spline::SplineType::Nurbs);
    OX_REFLECT_ENUM(spline::HandleMode, "SplineHandleMode")
        .value("Free", spline::HandleMode::Free)
        .value("Aligned", spline::HandleMode::Aligned)
        .value("Mirrored", spline::HandleMode::Mirrored)
        .value("Auto", spline::HandleMode::Auto);
    OX_REFLECT_ENUM(spline::FrameMode, "SplineFrameMode")
        .value("RotationMinimizing", spline::FrameMode::RotationMinimizing)
        .value("UpVector", spline::FrameMode::UpVector);
    OX_REFLECT_ENUM(spline::LoopMode, "SplineLoopMode")
        .value("Once", spline::LoopMode::Once)
        .value("Loop", spline::LoopMode::Loop)
        .value("PingPong", spline::LoopMode::PingPong);

    OX_REFLECT_ENUM(audio::AttenuationModel, "AudioAttenuation")
        .value("None", audio::AttenuationModel::None)
        .value("Inverse", audio::AttenuationModel::Inverse)
        .value("Linear", audio::AttenuationModel::Linear)
        .value("Exponential", audio::AttenuationModel::Exponential)
        .value("Custom", audio::AttenuationModel::Custom);

    OX_REFLECT_ENUM(NavObstacleShape, "NavObstacleShape")
        .value("Cylinder", NavObstacleShape::Cylinder)
        .value("Box", NavObstacleShape::Box);
    OX_REFLECT_ENUM(BlackboardEntryType, "BlackboardEntryType")
        .value("Bool", BlackboardEntryType::Bool)
        .value("Int", BlackboardEntryType::Int)
        .value("Float", BlackboardEntryType::Float)
        .value("String", BlackboardEntryType::String)
        .value("Vec3", BlackboardEntryType::Vec3)
        .value("Entity", BlackboardEntryType::Entity);
    OX_REFLECT_ENUM(ai::BTStatus, "BTStatus")
        .value("Idle", ai::BTStatus::Idle)
        .value("Running", ai::BTStatus::Running)
        .value("Success", ai::BTStatus::Success)
        .value("Failure", ai::BTStatus::Failure);

    OX_REFLECT_ENUM(script::ScriptPropertyType, "ScriptPropertyType")
        .value("Float", script::ScriptPropertyType::Float)
        .value("Int", script::ScriptPropertyType::Int)
        .value("Bool", script::ScriptPropertyType::Bool)
        .value("String", script::ScriptPropertyType::String)
        .value("Vec2", script::ScriptPropertyType::Vec2)
        .value("Vec3", script::ScriptPropertyType::Vec3)
        .value("Vec4", script::ScriptPropertyType::Vec4)
        .value("Color", script::ScriptPropertyType::Color);

    OX_REFLECT_ENUM(net::Relevancy, "NetRelevancy")
        .value("Always", net::Relevancy::Always)
        .value("Distance", net::Relevancy::Distance)
        .value("OwnerOnly", net::Relevancy::OwnerOnly)
        .value("Custom", net::Relevancy::Custom);
}

void registerPhysics(ComponentRegistry& reg) {
    OX_REFLECT_TYPE(RigidBodyComponent, "RigidBody")
        .attributes(Category{"Physics"}, Meta{"icon", "cube"}, Tooltip{"Rigid body simulated by the physics world (needs a Collider)"})
        .field("motionType", &RigidBodyComponent::motionType, Tooltip{"Static, Kinematic (moved by the transform) or Dynamic"})
        .field("layer", &RigidBodyComponent::layer, Tooltip{"Collision layer name; empty = automatic"})
        .field("mass", &RigidBodyComponent::mass, Range{0.0, 100000.0}, Tooltip{"kg; 0 = from collider density"})
        .field("inertiaOverride", &RigidBodyComponent::inertiaOverride, Tooltip{"Diagonal inertia; zero = from the shape"})
        .field("friction", &RigidBodyComponent::friction, Range{0.0, 2.0}, Step{0.01})
        .field("restitution", &RigidBodyComponent::restitution, Range{0.0, 1.0}, Step{0.01})
        .field("linearDamping", &RigidBodyComponent::linearDamping, Range{0.0, 10.0})
        .field("angularDamping", &RigidBodyComponent::angularDamping, Range{0.0, 10.0})
        .field("gravityFactor", &RigidBodyComponent::gravityFactor, Range{-10.0, 10.0})
        .field("ccd", &RigidBodyComponent::ccd, DisplayName{"Continuous Collision"}, Tooltip{"For fast, small bodies"})
        .field("allowSleeping", &RigidBodyComponent::allowSleeping)
        .field("startActive", &RigidBodyComponent::startActive)
        .field("lockAxes", &RigidBodyComponent::lockAxes, Meta{"bitmask", "TranslationX|TranslationY|TranslationZ|RotationX|RotationY|RotationZ"})
        .field("reportContacts", &RigidBodyComponent::reportContacts)
        .field("interpolate", &RigidBodyComponent::interpolate, Tooltip{"Smooth rendering between fixed steps"})
        .field("initialLinearVelocity", &RigidBodyComponent::initialLinearVelocity)
        .field("initialAngularVelocity", &RigidBodyComponent::initialAngularVelocity)
        .field("bodyId", &RigidBodyComponent::bodyId, NoSerialize{}, Hidden{}, ReadOnly{});
    reg.add<RigidBodyComponent>({.icon = "cube"});

    OX_REFLECT_TYPE(ColliderChild, "ColliderChild")
        .field("type", &ColliderChild::type)
        .field("halfExtents", &ColliderChild::halfExtents)
        .field("radius", &ColliderChild::radius, Range{0.0, 1000.0})
        .field("halfHeight", &ColliderChild::halfHeight, Range{0.0, 1000.0})
        .field("points", &ColliderChild::points)
        .field("position", &ColliderChild::position)
        .field("rotation", &ColliderChild::rotation);
    OX_REFLECT_TYPE(ColliderComponent, "Collider")
        .attributes(Category{"Physics"}, Meta{"icon", "shapes"}, Tooltip{"Collision shape; without RigidBody the entity is static"})
        .field("type", &ColliderComponent::type)
        .field("halfExtents", &ColliderComponent::halfExtents, Tooltip{"Box half size"})
        .field("radius", &ColliderComponent::radius, Range{0.0, 1000.0})
        .field("halfHeight", &ColliderComponent::halfHeight, Range{0.0, 1000.0}, Tooltip{"Capsule/cylinder half height"})
        .field("convexRadius", &ColliderComponent::convexRadius, Tooltip{"< 0 = automatic"})
        .field("density", &ColliderComponent::density, Range{0.001, 100000.0}, Tooltip{"kg/m^3"})
        .field("mesh", &ColliderComponent::mesh, attr::AssetRef{"Mesh"})
        .field("points", &ColliderComponent::points, Tooltip{"Convex hull points"})
        .field("heights", &ColliderComponent::heights, Hidden{})
        .field("sampleCount", &ColliderComponent::sampleCount)
        .field("heightFieldOffset", &ColliderComponent::heightFieldOffset)
        .field("heightFieldScale", &ColliderComponent::heightFieldScale)
        .field("children", &ColliderComponent::children)
        .field("offsetPosition", &ColliderComponent::offsetPosition)
        .field("offsetRotation", &ColliderComponent::offsetRotation)
        .field("scale", &ColliderComponent::scale)
        .field("isSensor", &ColliderComponent::isSensor, Tooltip{"Trigger volume: overlap events, no collision response"});
    reg.add<ColliderComponent>({.icon = "shapes"});

    OX_REFLECT_TYPE(CharacterControllerComponent, "CharacterController")
        .attributes(Category{"Physics"}, Meta{"icon", "person-walking"})
        .field("height", &CharacterControllerComponent::height, Range{0.1, 10.0})
        .field("radius", &CharacterControllerComponent::radius, Range{0.01, 5.0})
        .field("maxSlopeAngle", &CharacterControllerComponent::maxSlopeAngle, Range{0.0, 90.0}, Tooltip{"Degrees"})
        .field("maxStepHeight", &CharacterControllerComponent::maxStepHeight, Range{0.0, 2.0})
        .field("stickToFloorDistance", &CharacterControllerComponent::stickToFloorDistance, Range{0.0, 2.0})
        .field("mass", &CharacterControllerComponent::mass, Range{0.0, 10000.0})
        .field("maxStrength", &CharacterControllerComponent::maxStrength, Range{0.0, 100000.0})
        .field("jumpSpeed", &CharacterControllerComponent::jumpSpeed, Range{0.0, 50.0})
        .field("airControl", &CharacterControllerComponent::airControl, Range{0.0, 1.0})
        .field("layer", &CharacterControllerComponent::layer)
        .field("desiredVelocity", &CharacterControllerComponent::desiredVelocity, NoSerialize{}, Hidden{})
        .field("jump", &CharacterControllerComponent::jump, NoSerialize{}, Hidden{})
        .field("groundState", &CharacterControllerComponent::groundState, NoSerialize{}, ReadOnly{})
        .field("velocity", &CharacterControllerComponent::velocity, NoSerialize{}, ReadOnly{});
    reg.add<CharacterControllerComponent>({.icon = "person-walking"});

    OX_REFLECT_TYPE(TriggerComponent, "Trigger")
        .attributes(Category{"Physics"}, Meta{"icon", "bolt"}, Tooltip{"Turns the collider into a trigger volume"})
        .field("requiredTag", &TriggerComponent::requiredTag)
        .field("reportStay", &TriggerComponent::reportStay)
        .field("once", &TriggerComponent::once)
        .field("overlapCount", &TriggerComponent::overlapCount, NoSerialize{}, ReadOnly{})
        .field("fired", &TriggerComponent::fired, SaveGame{}, ReadOnly{});
    reg.add<TriggerComponent>({.icon = "bolt"});

    OX_REFLECT_TYPE(JointComponent, "Joint")
        .attributes(Category{"Physics"}, Meta{"icon", "link"})
        .field("type", &JointComponent::type)
        .field("target", &JointComponent::target, Tooltip{"Other body; empty = the world"})
        .field("anchor", &JointComponent::anchor)
        .field("targetAnchor", &JointComponent::targetAnchor)
        .field("axis", &JointComponent::axis)
        .field("limitsEnabled", &JointComponent::limitsEnabled)
        .field("limitMin", &JointComponent::limitMin)
        .field("limitMax", &JointComponent::limitMax)
        .field("motorMode", &JointComponent::motorMode)
        .field("motorTarget", &JointComponent::motorTarget)
        .field("motorMaxForce", &JointComponent::motorMaxForce, Range{0.0, 1e9})
        .field("minDistance", &JointComponent::minDistance)
        .field("maxDistance", &JointComponent::maxDistance)
        .field("springFrequency", &JointComponent::springFrequency, Range{0.0, 100.0})
        .field("springDamping", &JointComponent::springDamping, Range{0.0, 10.0})
        .field("coneHalfAngle", &JointComponent::coneHalfAngle, Range{0.0, 180.0}, Tooltip{"Degrees"})
        .field("breakForce", &JointComponent::breakForce, Range{0.0, 1e9}, Tooltip{"0 = unbreakable"})
        .field("breakTorque", &JointComponent::breakTorque, Range{0.0, 1e9}, Tooltip{"0 = unbreakable"})
        .field("broken", &JointComponent::broken, SaveGame{}, ReadOnly{});
    reg.add<JointComponent>({.icon = "link"});
}

void registerAnimation(ComponentRegistry& reg) {
    OX_REFLECT_TYPE(AnimatorParameterDesc, "AnimatorParameterDesc")
        .field("name", &AnimatorParameterDesc::name)
        .field("type", &AnimatorParameterDesc::type)
        .field("defaultValue", &AnimatorParameterDesc::defaultValue);
    OX_REFLECT_TYPE(AnimatorStateDesc, "AnimatorStateDesc")
        .field("name", &AnimatorStateDesc::name)
        .field("clip", &AnimatorStateDesc::clip, attr::AssetRef{"AnimationClip"})
        .field("speed", &AnimatorStateDesc::speed, Range{-10.0, 10.0})
        .field("speedParameter", &AnimatorStateDesc::speedParameter)
        .field("loop", &AnimatorStateDesc::loop);
    OX_REFLECT_TYPE(AnimatorTransitionDesc, "AnimatorTransitionDesc")
        .field("from", &AnimatorTransitionDesc::from, Tooltip{"State name; empty or * = any state"})
        .field("to", &AnimatorTransitionDesc::to)
        .field("parameter", &AnimatorTransitionDesc::parameter)
        .field("op", &AnimatorTransitionDesc::op)
        .field("threshold", &AnimatorTransitionDesc::threshold)
        .field("duration", &AnimatorTransitionDesc::duration, Range{0.0, 10.0})
        .field("hasExitTime", &AnimatorTransitionDesc::hasExitTime)
        .field("exitTime", &AnimatorTransitionDesc::exitTime, Range{0.0, 10.0});
    OX_REFLECT_TYPE(InlineAnimatorController, "InlineAnimatorController")
        .field("parameters", &InlineAnimatorController::parameters)
        .field("states", &InlineAnimatorController::states)
        .field("transitions", &InlineAnimatorController::transitions)
        .field("defaultState", &InlineAnimatorController::defaultState);
    OX_REFLECT_TYPE(AnimatorComponent, "Animator")
        .attributes(Category{"Animation"}, Meta{"icon", "person-running"})
        .field("skeleton", &AnimatorComponent::skeleton, attr::AssetRef{"Skeleton"})
        .field("controller", &AnimatorComponent::controller, attr::AssetRef{"AnimatorController"},
               Tooltip{"Empty = use the inline controller"})
        .field("inlineController", &AnimatorComponent::inlineController)
        .field("applyRootMotion", &AnimatorComponent::applyRootMotion)
        .field("playbackSpeed", &AnimatorComponent::playbackSpeed, Range{0.0, 10.0}, Step{0.01}, Replicated{})
        .field("animateInEditMode", &AnimatorComponent::animateInEditMode)
        .field("parameters", &AnimatorComponent::parameters, SaveGame{})
        .field("currentState", &AnimatorComponent::currentState, NoSerialize{}, ReadOnly{});
    reg.add<AnimatorComponent>({.icon = "person-running"});

    OX_REFLECT_TYPE(SkinnedMeshComponent, "SkinnedMesh")
        .attributes(Category{"Animation"}, Meta{"icon", "user"})
        .field("mesh", &SkinnedMeshComponent::mesh, attr::AssetRef{"Mesh"})
        .field("materials", &SkinnedMeshComponent::materials, attr::AssetRef{"Material"})
        .field("skinningMethod", &SkinnedMeshComponent::skinningMethod)
        .field("gpuSkinning", &SkinnedMeshComponent::gpuSkinning)
        .field("castShadows", &SkinnedMeshComponent::castShadows)
        .field("visible", &SkinnedMeshComponent::visible);
    reg.add<SkinnedMeshComponent>({.icon = "user"});

    OX_REFLECT_TYPE(IKChainDesc, "IKChain")
        .field("type", &IKChainDesc::type)
        .field("rootJoint", &IKChainDesc::rootJoint)
        .field("midJoint", &IKChainDesc::midJoint)
        .field("endJoint", &IKChainDesc::endJoint)
        .field("target", &IKChainDesc::target)
        .field("targetOffset", &IKChainDesc::targetOffset)
        .field("pole", &IKChainDesc::pole)
        .field("poleOffset", &IKChainDesc::poleOffset)
        .field("aimAxis", &IKChainDesc::aimAxis)
        .field("weight", &IKChainDesc::weight, Range{0.0, 1.0})
        .field("enabled", &IKChainDesc::enabled);
    OX_REFLECT_TYPE(IKComponent, "IK")
        .attributes(Category{"Animation"}, Meta{"icon", "hand"})
        .field("chains", &IKComponent::chains);
    reg.add<IKComponent>({.icon = "hand"});
}

void registerSplines(ComponentRegistry& reg) {
    OX_REFLECT_TYPE(SplinePoint, "SplinePoint")
        .field("position", &SplinePoint::position)
        .field("inHandle", &SplinePoint::inHandle)
        .field("outHandle", &SplinePoint::outHandle)
        .field("handleMode", &SplinePoint::handleMode)
        .field("roll", &SplinePoint::roll, Tooltip{"Radians"})
        .field("weight", &SplinePoint::weight, Range{0.001, 100.0})
        .field("up", &SplinePoint::up);
    OX_REFLECT_TYPE(SplineMarkerDesc, "SplineMarker").field("name", &SplineMarkerDesc::name).field("t", &SplineMarkerDesc::t);
    OX_REFLECT_TYPE(SplineComponent, "Spline")
        .attributes(Category{"Splines"}, Meta{"icon", "bezier-curve"})
        .field("type", &SplineComponent::type)
        .field("closed", &SplineComponent::closed)
        .field("points", &SplineComponent::points)
        .field("catmullRomAlpha", &SplineComponent::catmullRomAlpha, Range{0.0, 1.0})
        .field("degree", &SplineComponent::degree, Range{1.0, 7.0})
        .field("frameMode", &SplineComponent::frameMode)
        .field("upVector", &SplineComponent::upVector)
        .field("markers", &SplineComponent::markers)
        .field("drawInEditor", &SplineComponent::drawInEditor)
        .field("drawInGame", &SplineComponent::drawInGame)
        .field("color", &SplineComponent::color, attr::Color{});
    reg.add<SplineComponent>({.icon = "bezier-curve"});

    OX_REFLECT_TYPE(SplineFollowerEventDesc, "SplineFollowerEvent")
        .field("name", &SplineFollowerEventDesc::name)
        .field("distance", &SplineFollowerEventDesc::distance);
    OX_REFLECT_TYPE(SplineFollowerComponent, "SplineFollower")
        .attributes(Category{"Splines"}, Meta{"icon", "route"})
        .field("spline", &SplineFollowerComponent::spline, Tooltip{"Entity with a Spline component"})
        .field("speed", &SplineFollowerComponent::speed, Range{-1000.0, 1000.0}, Replicated{}, SaveGame{})
        .field("loopMode", &SplineFollowerComponent::loopMode)
        .field("orientToPath", &SplineFollowerComponent::orientToPath)
        .field("faceTravelDirection", &SplineFollowerComponent::faceTravelDirection)
        .field("forwardAxis", &SplineFollowerComponent::forwardAxis)
        .field("upAxis", &SplineFollowerComponent::upAxis)
        .field("offset", &SplineFollowerComponent::offset)
        .field("playing", &SplineFollowerComponent::playing, Replicated{}, SaveGame{})
        .field("fireMarkers", &SplineFollowerComponent::fireMarkers)
        .field("events", &SplineFollowerComponent::events)
        .field("startDistance", &SplineFollowerComponent::startDistance)
        .field("distance", &SplineFollowerComponent::distance, SaveGame{}, ReadOnly{})
        .field("direction", &SplineFollowerComponent::direction, SaveGame{}, Hidden{})
        .field("finished", &SplineFollowerComponent::finished, SaveGame{}, ReadOnly{});
    reg.add<SplineFollowerComponent>({.icon = "route"});
}

void registerAudio(ComponentRegistry& reg) {
    OX_REFLECT_TYPE(AudioSourceComponent, "AudioSource")
        .attributes(Category{"Audio"}, Meta{"icon", "volume-high"})
        .field("clip", &AudioSourceComponent::clip, attr::AssetRef{"AudioClip"})
        .field("clipPath", &AudioSourceComponent::clipPath, Tooltip{"Used when no clip asset is set"})
        .field("stream", &AudioSourceComponent::stream)
        .field("bus", &AudioSourceComponent::bus)
        .field("volume", &AudioSourceComponent::volume, Range{0.0, 4.0}, Step{0.01}, Replicated{})
        .field("pitch", &AudioSourceComponent::pitch, Range{0.1, 4.0}, Step{0.01}, Replicated{})
        .field("loop", &AudioSourceComponent::loop)
        .field("playOnStart", &AudioSourceComponent::playOnStart)
        .field("spatial", &AudioSourceComponent::spatial)
        .field("attenuation", &AudioSourceComponent::attenuation)
        .field("minDistance", &AudioSourceComponent::minDistance, Range{0.0, 10000.0})
        .field("maxDistance", &AudioSourceComponent::maxDistance, Range{0.0, 10000.0})
        .field("rolloff", &AudioSourceComponent::rolloff, Range{0.0, 10.0})
        .field("coneInnerAngle", &AudioSourceComponent::coneInnerAngle, Range{0.0, 360.0}, Tooltip{"Degrees"})
        .field("coneOuterAngle", &AudioSourceComponent::coneOuterAngle, Range{0.0, 360.0}, Tooltip{"Degrees"})
        .field("coneOuterGain", &AudioSourceComponent::coneOuterGain, Range{0.0, 1.0})
        .field("dopplerFactor", &AudioSourceComponent::dopplerFactor, Range{0.0, 10.0})
        .field("distanceLowPass", &AudioSourceComponent::distanceLowPass)
        .field("occlusion", &AudioSourceComponent::occlusion, Tooltip{"Physics raycast occlusion"})
        .field("priority", &AudioSourceComponent::priority)
        .field("fadeInSeconds", &AudioSourceComponent::fadeInSeconds, Range{0.0, 60.0})
        .field("playing", &AudioSourceComponent::playing, NoSerialize{}, ReadOnly{});
    reg.add<AudioSourceComponent>({.icon = "volume-high"});

    OX_REFLECT_TYPE(AudioListenerComponent, "AudioListener")
        .attributes(Category{"Audio"}, Meta{"icon", "headphones"})
        .field("active", &AudioListenerComponent::active);
    reg.add<AudioListenerComponent>({.icon = "headphones"});
}

void registerAI(ComponentRegistry& reg) {
    OX_REFLECT_TYPE(NavMeshSurfaceComponent, "NavMeshSurface")
        .attributes(Category{"AI"}, Meta{"icon", "map"})
        .field("cellSize", &NavMeshSurfaceComponent::cellSize, Range{0.01, 10.0})
        .field("cellHeight", &NavMeshSurfaceComponent::cellHeight, Range{0.01, 10.0})
        .field("agentHeight", &NavMeshSurfaceComponent::agentHeight, Range{0.1, 10.0})
        .field("agentRadius", &NavMeshSurfaceComponent::agentRadius, Range{0.0, 10.0})
        .field("agentMaxClimb", &NavMeshSurfaceComponent::agentMaxClimb, Range{0.0, 10.0})
        .field("agentMaxSlope", &NavMeshSurfaceComponent::agentMaxSlope, Range{0.0, 90.0})
        .field("regionMinSize", &NavMeshSurfaceComponent::regionMinSize)
        .field("regionMergeSize", &NavMeshSurfaceComponent::regionMergeSize)
        .field("edgeMaxLen", &NavMeshSurfaceComponent::edgeMaxLen)
        .field("edgeMaxError", &NavMeshSurfaceComponent::edgeMaxError)
        .field("vertsPerPoly", &NavMeshSurfaceComponent::vertsPerPoly, Range{3.0, 6.0})
        .field("detailSampleDist", &NavMeshSurfaceComponent::detailSampleDist)
        .field("detailSampleMaxError", &NavMeshSurfaceComponent::detailSampleMaxError)
        .field("tiled", &NavMeshSurfaceComponent::tiled)
        .field("tileSize", &NavMeshSurfaceComponent::tileSize)
        .field("includeStaticColliders", &NavMeshSurfaceComponent::includeStaticColliders)
        .field("includeMeshes", &NavMeshSurfaceComponent::includeMeshes)
        .field("onlyChildren", &NavMeshSurfaceComponent::onlyChildren)
        .field("bakeOnStart", &NavMeshSurfaceComponent::bakeOnStart)
        .field("dynamicObstacles", &NavMeshSurfaceComponent::dynamicObstacles)
        .field("drawInEditor", &NavMeshSurfaceComponent::drawInEditor)
        .field("bakedData", &NavMeshSurfaceComponent::bakedData, Hidden{})
        .field("baked", &NavMeshSurfaceComponent::baked, NoSerialize{}, ReadOnly{});
    reg.add<NavMeshSurfaceComponent>({.icon = "map"});

    OX_REFLECT_TYPE(NavAgentComponent, "NavAgent")
        .attributes(Category{"AI"}, Meta{"icon", "location-arrow"})
        .field("radius", &NavAgentComponent::radius, Range{0.05, 5.0})
        .field("height", &NavAgentComponent::height, Range{0.1, 10.0})
        .field("maxSpeed", &NavAgentComponent::maxSpeed, Range{0.0, 100.0})
        .field("maxAcceleration", &NavAgentComponent::maxAcceleration, Range{0.0, 1000.0})
        .field("separationWeight", &NavAgentComponent::separationWeight, Range{0.0, 20.0})
        .field("avoidanceQuality", &NavAgentComponent::avoidanceQuality, Range{0.0, 3.0})
        .field("stoppingDistance", &NavAgentComponent::stoppingDistance, Range{0.0, 10.0})
        .field("updatePosition", &NavAgentComponent::updatePosition)
        .field("updateRotation", &NavAgentComponent::updateRotation)
        .field("driveCharacterController", &NavAgentComponent::driveCharacterController)
        .field("destination", &NavAgentComponent::destination, SaveGame{}, ReadOnly{})
        .field("hasDestination", &NavAgentComponent::hasDestination, SaveGame{}, ReadOnly{})
        .field("reached", &NavAgentComponent::reached, NoSerialize{}, ReadOnly{})
        .field("velocity", &NavAgentComponent::velocity, NoSerialize{}, ReadOnly{});
    reg.add<NavAgentComponent>({.icon = "location-arrow"});

    OX_REFLECT_TYPE(NavObstacleComponent, "NavObstacle")
        .attributes(Category{"AI"}, Meta{"icon", "ban"}, Tooltip{"Carves surfaces with dynamicObstacles"})
        .field("shape", &NavObstacleComponent::shape)
        .field("radius", &NavObstacleComponent::radius, Range{0.0, 100.0})
        .field("height", &NavObstacleComponent::height, Range{0.0, 100.0})
        .field("halfExtents", &NavObstacleComponent::halfExtents)
        .field("moveThreshold", &NavObstacleComponent::moveThreshold, Range{0.0, 10.0});
    reg.add<NavObstacleComponent>({.icon = "ban"});

    OX_REFLECT_TYPE(BlackboardEntry, "BlackboardEntry")
        .field("key", &BlackboardEntry::key)
        .field("type", &BlackboardEntry::type)
        .field("boolValue", &BlackboardEntry::boolValue)
        .field("intValue", &BlackboardEntry::intValue)
        .field("floatValue", &BlackboardEntry::floatValue)
        .field("stringValue", &BlackboardEntry::stringValue)
        .field("vec3Value", &BlackboardEntry::vec3Value)
        .field("entityValue", &BlackboardEntry::entityValue);
    OX_REFLECT_TYPE(BehaviorTreeComponent, "BehaviorTree")
        .attributes(Category{"AI"}, Meta{"icon", "sitemap"})
        .field("tree", &BehaviorTreeComponent::tree, attr::AssetRef{"BehaviorTree"})
        .field("treeJson", &BehaviorTreeComponent::treeJson, Tooltip{"Inline BTFactory JSON (when no asset)"}, Meta{"multiline", "true"})
        .field("blackboard", &BehaviorTreeComponent::blackboard, Tooltip{"Initial blackboard values"})
        .field("enabled", &BehaviorTreeComponent::enabled, SaveGame{})
        .field("tickInterval", &BehaviorTreeComponent::tickInterval, Range{0.0, 10.0})
        .field("restartOnFinish", &BehaviorTreeComponent::restartOnFinish)
        .field("status", &BehaviorTreeComponent::status, NoSerialize{}, ReadOnly{});
    reg.add<BehaviorTreeComponent>({.icon = "sitemap"});

    OX_REFLECT_TYPE(ai::SightConfig, "SightConfig")
        .field("enabled", &ai::SightConfig::enabled)
        .field("range", &ai::SightConfig::range, Range{0.0, 1000.0})
        .field("fovDegrees", &ai::SightConfig::fovDegrees, Range{0.0, 360.0})
        .field("peripheralFovDegrees", &ai::SightConfig::peripheralFovDegrees, Range{0.0, 360.0})
        .field("peripheralRange", &ai::SightConfig::peripheralRange, Range{0.0, 1000.0})
        .field("loseSightRange", &ai::SightConfig::loseSightRange, Range{0.0, 1000.0})
        .field("eyeHeight", &ai::SightConfig::eyeHeight)
        .field("targetHeight", &ai::SightConfig::targetHeight)
        .field("forgetAfter", &ai::SightConfig::forgetAfter, Range{0.0, 600.0});
    OX_REFLECT_TYPE(ai::HearingConfig, "HearingConfig")
        .field("enabled", &ai::HearingConfig::enabled)
        .field("rangeMultiplier", &ai::HearingConfig::rangeMultiplier, Range{0.0, 100.0})
        .field("threshold", &ai::HearingConfig::threshold, Range{0.0, 1.0})
        .field("useOcclusion", &ai::HearingConfig::useOcclusion)
        .field("occludedFactor", &ai::HearingConfig::occludedFactor, Range{0.0, 1.0})
        .field("forgetAfter", &ai::HearingConfig::forgetAfter, Range{0.0, 600.0});
    OX_REFLECT_TYPE(PerceptionComponent, "Perception")
        .attributes(Category{"AI"}, Meta{"icon", "eye"})
        .field("team", &PerceptionComponent::team, Replicated{})
        .field("listener", &PerceptionComponent::listener, Tooltip{"Has senses"})
        .field("source", &PerceptionComponent::source, Tooltip{"Can be perceived"})
        .field("visible", &PerceptionComponent::visible, Replicated{})
        .field("sight", &PerceptionComponent::sight)
        .field("hearing", &PerceptionComponent::hearing)
        .field("detectHostile", &PerceptionComponent::detectHostile)
        .field("detectNeutral", &PerceptionComponent::detectNeutral)
        .field("detectFriendly", &PerceptionComponent::detectFriendly)
        .field("target", &PerceptionComponent::target, NoSerialize{}, ReadOnly{})
        .field("targetVisible", &PerceptionComponent::targetVisible, NoSerialize{}, ReadOnly{});
    reg.add<PerceptionComponent>({.icon = "eye"});
}

void registerScripting(ComponentRegistry& reg) {
    OX_REFLECT_TYPE(ScriptPropertyValue, "ScriptPropertyValue")
        .field("type", &ScriptPropertyValue::type)
        .field("number", &ScriptPropertyValue::number)
        .field("boolean", &ScriptPropertyValue::boolean)
        .field("text", &ScriptPropertyValue::text)
        .field("vector", &ScriptPropertyValue::vector);
    OX_REFLECT_TYPE(ScriptComponent, "Script")
        .attributes(Category{"Scripting"}, Meta{"icon", "code"})
        .field("script", &ScriptComponent::script, Tooltip{"Lua file (relative to the script roots) or registered name"})
        .field("asset", &ScriptComponent::asset, attr::AssetRef{"Script"})
        .field("properties", &ScriptComponent::properties, Tooltip{"Overrides of the script's declared properties"})
        .field("enabled", &ScriptComponent::enabled, Replicated{}, SaveGame{});
    reg.add<ScriptComponent>({.icon = "code"});
}

void registerNetworking(ComponentRegistry& reg) {
    OX_REFLECT_TYPE(NetworkIdentityComponent, "NetworkIdentity")
        .attributes(Category{"Networking"}, Meta{"icon", "network-wired"})
        .field("netType", &NetworkIdentityComponent::netType, Tooltip{"Prefab/type the clients spawn"})
        .field("relevancy", &NetworkIdentityComponent::relevancy)
        .field("relevancyRadius", &NetworkIdentityComponent::relevancyRadius, Range{0.0, 100000.0})
        .field("priority", &NetworkIdentityComponent::priority, Range{0.0, 100.0})
        .field("viewer", &NetworkIdentityComponent::viewer)
        .field("netId", &NetworkIdentityComponent::netId, NoSerialize{}, ReadOnly{})
        .field("owner", &NetworkIdentityComponent::owner, NoSerialize{}, ReadOnly{});
    reg.add<NetworkIdentityComponent>({.icon = "network-wired"});

    OX_REFLECT_TYPE(NetworkTransformComponent, "NetworkTransform")
        .attributes(Category{"Networking"}, Meta{"icon", "arrows-up-down-left-right"})
        .field("syncPosition", &NetworkTransformComponent::syncPosition)
        .field("syncRotation", &NetworkTransformComponent::syncRotation)
        .field("syncScale", &NetworkTransformComponent::syncScale)
        .field("positionRange", &NetworkTransformComponent::positionRange, Range{1.0, 100000.0})
        .field("positionResolution", &NetworkTransformComponent::positionResolution, Range{0.0001, 1.0})
        .field("rotationBits", &NetworkTransformComponent::rotationBits, Range{6.0, 15.0})
        .field("interpolate", &NetworkTransformComponent::interpolate)
        .field("predicted", &NetworkTransformComponent::predicted)
        .field("correctionThreshold", &NetworkTransformComponent::correctionThreshold, Range{0.0, 100.0});
    reg.add<NetworkTransformComponent>({.icon = "arrows-up-down-left-right"});
}

} // namespace

void registerGameplayTypes() {
    registerSceneTypes();
    registerEnums();
    ComponentRegistry& reg = ComponentRegistry::instance();
    registerPhysics(reg);
    registerAnimation(reg);
    registerSplines(reg);
    registerAudio(reg);
    registerAI(reg);
    registerScripting(reg);
    registerNetworking(reg);
}

} // namespace ox
