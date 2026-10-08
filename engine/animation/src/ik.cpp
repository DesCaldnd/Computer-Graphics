#include <oxwald/animation/ik.hpp>

#include <oxwald/core/assert.hpp>

#include <algorithm>
#include <cmath>

namespace ox::anim {

namespace {

constexpr f32 kEps = 1e-6f;
const glm::quat kIdentity(1.0f, 0.0f, 0.0f, 0.0f);

glm::vec3 safeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
    const f32 l = glm::length(v);
    return l > kEps ? v / l : fallback;
}

glm::vec3 anyPerpendicular(const glm::vec3& v) {
    const glm::vec3 a = std::abs(v.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    return glm::normalize(glm::cross(v, a));
}

f32 quatAngle(const glm::quat& q) {
    return 2.0f * std::acos(std::clamp(std::abs(q.w), 0.0f, 1.0f));
}

glm::quat clampAngle(const glm::quat& q, f32 maxAngle) {
    const f32 angle = quatAngle(q);
    if (angle <= maxAngle || angle < kEps) return q;
    return slerpShortest(kIdentity, q, maxAngle / angle);
}

void checkJoint(const Skeleton& s, i32 j) {
    OX_ASSERT(j >= 0 && static_cast<usize>(j) < s.jointCount(), "invalid IK joint {}", j);
}

} // namespace

void applyModelRotation(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model, i32 joint,
                        const glm::quat& modelRotation) {
    const glm::quat g = model[static_cast<usize>(joint)].rotation;
    Transform& local = pose.local[static_cast<usize>(joint)];
    // G' = q * G  ⇒  L' = L * G⁻¹ * q * G (exact for uniform parent scale).
    local.rotation = glm::normalize(local.rotation * glm::inverse(g) * modelRotation * g);
    updateModelFrom(skeleton, pose, model, joint);
}

IKResult solveTwoBoneIK(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model,
                        const TwoBoneIKSettings& s) {
    checkJoint(skeleton, s.root);
    checkJoint(skeleton, s.mid);
    checkJoint(skeleton, s.end);
    const usize ri = static_cast<usize>(s.root), mi = static_cast<usize>(s.mid), ei = static_cast<usize>(s.end);
    const Transform origRoot = pose.local[ri], origMid = pose.local[mi], origEnd = pose.local[ei];
    const glm::quat origEndModelRot = model[ei].rotation;

    glm::vec3 a = model[ri].translation;
    f32 lab = glm::length(model[mi].translation - a);
    f32 lcb = glm::length(model[ei].translation - model[mi].translation);
    const glm::vec3 toTarget = s.target - a;
    const f32 dist = glm::length(toTarget);

    if (s.allowStretch && dist > lab + lcb && lab + lcb > kEps) {
        const f32 k = std::min(dist / (lab + lcb), s.maxStretch);
        pose.local[mi].translation *= k;
        pose.local[ei].translation *= k;
        updateModelFrom(skeleton, pose, model, s.root);
        lab *= k;
        lcb *= k;
    }
    if (lab < kEps || lcb < kEps) {
        return {};
    }

    const glm::vec3 b = model[mi].translation;
    const glm::vec3 dir = safeNormalize(toTarget, safeNormalize(model[ei].translation - a, glm::vec3(0, -1, 0)));
    const f32 lat = std::clamp(dist, std::abs(lab - lcb) + 1e-4f, lab + lcb - 1e-4f);

    // Bend direction: towards the pole (projected onto the plane ⟂ dir), else keep the current bend.
    glm::vec3 bend(0.0f);
    if (s.usePole) bend = (s.pole - a) - dir * glm::dot(s.pole - a, dir);
    if (glm::length(bend) < kEps) bend = (b - a) - dir * glm::dot(b - a, dir);
    bend = safeNormalize(bend, anyPerpendicular(dir));

    const f32 cosA = std::clamp((lab * lab + lat * lat - lcb * lcb) / (2.0f * lab * lat), -1.0f, 1.0f);
    const f32 sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
    const glm::vec3 desiredMid = a + dir * (lab * cosA) + bend * (lab * sinA);
    const glm::vec3 desiredEnd = a + dir * lat;

    applyModelRotation(skeleton, pose, model, s.root,
                       rotationBetween(safeNormalize(b - a, dir), safeNormalize(desiredMid - a, dir)));
    const glm::vec3 b1 = model[mi].translation;
    const glm::vec3 c1 = model[ei].translation;
    applyModelRotation(skeleton, pose, model, s.mid,
                       rotationBetween(safeNormalize(c1 - b1, dir), safeNormalize(desiredEnd - b1, dir)));

    // Keep the end effector's model orientation (or apply the requested one) instead of inheriting the swing.
    const glm::quat wantEnd = s.endRotation ? *s.endRotation : origEndModelRot;
    applyModelRotation(skeleton, pose, model, s.end, wantEnd * glm::inverse(model[ei].rotation));

    if (s.weight < 1.0f) {
        const f32 w = std::max(s.weight, 0.0f);
        pose.local[ri] = lerp(origRoot, pose.local[ri], w);
        pose.local[mi] = lerp(origMid, pose.local[mi], w);
        pose.local[ei] = lerp(origEnd, pose.local[ei], w);
        updateModelFrom(skeleton, pose, model, s.root);
    }

    IKResult r;
    r.iterations = 1;
    r.error = glm::length(model[ei].translation - s.target);
    r.reached = r.error < 1e-3f * std::max(1.0f, lab + lcb);
    return r;
}

IKResult solveAimIK(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model, const AimIKSettings& s) {
    checkJoint(skeleton, s.joint);
    const usize j = static_cast<usize>(s.joint);
    const Transform& m = model[j];
    const glm::vec3 current = safeNormalize(m.rotation * s.aimAxis, glm::vec3(0, 0, 1));
    const glm::vec3 desired = safeNormalize(s.target - m.translation, current);
    glm::quat q = clampAngle(rotationBetween(current, desired), s.maxAngle);

    if (glm::length(s.upAxis) > kEps && glm::length(s.worldUp) > kEps) {
        // Roll around the new aim axis so the joint's up axis is as close as possible to worldUp.
        const glm::vec3 aim = q * current;
        const glm::vec3 up = q * (m.rotation * glm::normalize(s.upAxis));
        const glm::vec3 upP = up - aim * glm::dot(up, aim);
        const glm::vec3 wantP = s.worldUp - aim * glm::dot(s.worldUp, aim);
        if (glm::length(upP) > kEps && glm::length(wantP) > kEps) {
            q = rotationBetween(glm::normalize(upP), glm::normalize(wantP)) * q;
        }
    }
    if (s.weight < 1.0f) q = slerpShortest(kIdentity, q, std::max(s.weight, 0.0f));
    applyModelRotation(skeleton, pose, model, s.joint, q);

    IKResult r;
    r.iterations = 1;
    const glm::vec3 now = model[j].rotation * s.aimAxis;
    r.error = std::acos(std::clamp(glm::dot(safeNormalize(now, desired), desired), -1.0f, 1.0f));
    r.reached = r.error < 1e-3f;
    return r;
}

IKResult solveLookAtChain(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model,
                          const LookAtChainSettings& s) {
    OX_ASSERT(!s.joints.empty() && s.joints.size() == s.weights.size(), "look-at chain needs joints + weights");
    const i32 head = s.joints.back();
    checkJoint(skeleton, head);
    // Total allowed correction, measured once, then distributed.
    const auto headAim = [&] {
        const Transform& h = model[static_cast<usize>(head)];
        return safeNormalize(h.rotation * s.aimAxis, glm::vec3(0, 0, 1));
    };
    const glm::vec3 headPos = model[static_cast<usize>(head)].translation;
    const glm::quat total = clampAngle(rotationBetween(headAim(), safeNormalize(s.target - headPos, headAim())),
                                       s.maxAngle);
    const f32 totalAngle = quatAngle(total) * std::clamp(s.weight, 0.0f, 1.0f);
    f32 applied = 0.0f;
    for (usize k = 0; k < s.joints.size(); ++k) {
        checkJoint(skeleton, s.joints[k]);
        const glm::vec3 hp = model[static_cast<usize>(head)].translation;
        const glm::quat need = rotationBetween(headAim(), safeNormalize(s.target - hp, headAim()));
        const f32 needAngle = quatAngle(need);
        const f32 budget = std::max(0.0f, totalAngle - applied);
        f32 angle = std::min(needAngle, budget) * std::clamp(s.weights[k], 0.0f, 1.0f);
        if (needAngle < kEps || angle < kEps) continue;
        applyModelRotation(skeleton, pose, model, s.joints[k], slerpShortest(kIdentity, need, angle / needAngle));
        applied += angle;
    }
    IKResult r;
    r.iterations = static_cast<u32>(s.joints.size());
    const glm::vec3 hp = model[static_cast<usize>(head)].translation;
    r.error = std::acos(std::clamp(glm::dot(headAim(), safeNormalize(s.target - hp, headAim())), -1.0f, 1.0f));
    r.reached = r.error < 1e-2f;
    return r;
}

IKResult fabrikPositions(std::span<glm::vec3> p, glm::vec3 target, f32 tolerance, u32 maxIterations) {
    IKResult r;
    const usize n = p.size();
    if (n < 2) return r;
    std::vector<f32> len(n - 1);
    f32 total = 0.0f;
    for (usize i = 0; i + 1 < n; ++i) {
        len[i] = glm::length(p[i + 1] - p[i]);
        total += len[i];
    }
    const glm::vec3 root = p[0];
    if (glm::length(target - root) >= total) {
        // Unreachable: stretch straight towards the target.
        const glm::vec3 d = safeNormalize(target - root, glm::vec3(0, 1, 0));
        for (usize i = 1; i < n; ++i) p[i] = p[i - 1] + d * len[i - 1];
        r.iterations = 1;
        r.error = glm::length(p[n - 1] - target);
        r.reached = r.error <= tolerance;
        return r;
    }
    for (u32 it = 0; it < maxIterations; ++it) {
        r.iterations = it + 1;
        p[n - 1] = target;
        for (usize i = n - 1; i-- > 0;) {
            p[i] = p[i + 1] + safeNormalize(p[i] - p[i + 1], glm::vec3(0, -1, 0)) * len[i];
        }
        p[0] = root;
        for (usize i = 1; i < n; ++i) {
            p[i] = p[i - 1] + safeNormalize(p[i] - p[i - 1], glm::vec3(0, 1, 0)) * len[i - 1];
        }
        r.error = glm::length(p[n - 1] - target);
        if (r.error <= tolerance) {
            r.reached = true;
            break;
        }
    }
    return r;
}

IKResult solveFABRIK(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model, const FabrikSettings& s) {
    const usize n = s.chain.size();
    if (n < 2) return {};
    std::vector<glm::vec3> pts(n);
    for (usize i = 0; i < n; ++i) {
        checkJoint(skeleton, s.chain[i]);
        pts[i] = model[static_cast<usize>(s.chain[i])].translation;
    }
    const glm::quat tipRot = model[static_cast<usize>(s.chain.back())].rotation;
    IKResult r = fabrikPositions(pts, s.target, s.tolerance, s.maxIterations);
    const f32 w = std::clamp(s.weight, 0.0f, 1.0f);
    for (usize i = 0; i + 1 < n; ++i) {
        const glm::vec3 a = model[static_cast<usize>(s.chain[i])].translation;
        const glm::vec3 cur = model[static_cast<usize>(s.chain[i + 1])].translation - a;
        const glm::vec3 want = pts[i + 1] - a;
        if (glm::length(cur) < kEps || glm::length(want) < kEps) continue;
        glm::quat q = rotationBetween(glm::normalize(cur), glm::normalize(want));
        if (w < 1.0f) q = slerpShortest(kIdentity, q, w);
        applyModelRotation(skeleton, pose, model, s.chain[i], q);
    }
    // Tip keeps its model-space orientation.
    const i32 tip = s.chain.back();
    applyModelRotation(skeleton, pose, model, tip,
                       slerpShortest(kIdentity, tipRot * glm::inverse(model[static_cast<usize>(tip)].rotation), w));
    r.error = glm::length(model[static_cast<usize>(tip)].translation - s.target);
    r.reached = r.error <= s.tolerance * 2.0f;
    return r;
}

FootIKResult solveFootIK(const Skeleton& skeleton, Pose& pose, std::vector<Transform>& model,
                         std::span<const FootIKLeg> legs, const FootIKSettings& s, const GroundRaycast& raycast) {
    FootIKResult result;
    result.grounded.assign(legs.size(), false);
    result.targets.assign(legs.size(), glm::vec3(0.0f));
    if (!raycast || legs.empty()) return result;

    const Transform toModel = s.ownerWorld.inverse();
    const glm::vec3 worldUp(0.0f, 1.0f, 0.0f);
    const f32 w = std::clamp(s.weight, 0.0f, 1.0f);
    std::vector<glm::vec3> normals(legs.size(), glm::vec3(0, 1, 0));
    std::vector<f32> offsets(legs.size(), 0.0f);

    f32 lowest = 0.0f;
    for (usize i = 0; i < legs.size(); ++i) {
        checkJoint(skeleton, legs[i].ankle);
        const glm::vec3 ankleModel = model[static_cast<usize>(legs[i].ankle)].translation;
        const glm::vec3 ankleWorld = s.ownerWorld.transformPoint(ankleModel);
        const auto hit = raycast(ankleWorld + worldUp * s.rayStartHeight, -worldUp, s.rayStartHeight + s.rayLength);
        if (!hit) continue;
        const glm::vec3 targetModel = toModel.transformPoint(hit->position + worldUp * legs[i].ankleHeight);
        const f32 off = std::min(targetModel.y - ankleModel.y, s.maxFootRaise);
        result.grounded[i] = true;
        offsets[i] = off;
        normals[i] = safeNormalize(toModel.transformVector(hit->normal), glm::vec3(0, 1, 0));
        result.targets[i] = glm::vec3(ankleModel.x, ankleModel.y + off, ankleModel.z);
        lowest = std::min(lowest, off);
    }

    // Drop the pelvis so the lowest foot can reach; the other legs bend more.
    const f32 pelvisOffset = std::max(lowest, -s.maxPelvisDrop) * w;
    if (s.pelvis != kNoJoint && pelvisOffset != 0.0f) {
        checkJoint(skeleton, s.pelvis);
        const usize p = static_cast<usize>(s.pelvis);
        const glm::vec3 delta(0.0f, pelvisOffset, 0.0f);
        const i32 parent = skeleton.parent(s.pelvis);
        if (parent == kNoJoint) {
            pose.local[p].translation += delta;
        } else {
            const Transform& pm = model[static_cast<usize>(parent)];
            pose.local[p].translation += (glm::inverse(pm.rotation) * delta) / pm.scale;
        }
        updateModelFrom(skeleton, pose, model, s.pelvis);
        result.pelvisOffset = pelvisOffset;
    }

    for (usize i = 0; i < legs.size(); ++i) {
        if (!result.grounded[i]) continue;
        const FootIKLeg& leg = legs[i];
        const glm::vec3 hip = model[static_cast<usize>(leg.hip)].translation;
        const glm::vec3 knee = model[static_cast<usize>(leg.knee)].translation;
        const glm::vec3 ankle = model[static_cast<usize>(leg.ankle)].translation;
        const glm::vec3 bendDir = safeNormalize(knee - (hip + ankle) * 0.5f, glm::vec3(0, 0, 1));

        TwoBoneIKSettings ik;
        ik.root = leg.hip;
        ik.mid = leg.knee;
        ik.end = leg.ankle;
        ik.target = result.targets[i];
        ik.pole = knee + bendDir;
        ik.weight = w;
        if (s.alignFeetToNormal) {
            const glm::quat tilt = clampAngle(rotationBetween(glm::vec3(0, 1, 0), normals[i]), s.maxFootAngle);
            ik.endRotation = glm::normalize(slerpShortest(kIdentity, tilt, w) * model[static_cast<usize>(leg.ankle)].rotation);
        }
        solveTwoBoneIK(skeleton, pose, model, ik);
    }
    return result;
}

} // namespace ox::anim
