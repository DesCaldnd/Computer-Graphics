// Procedural skinned humanoid ("Mannequin"): a low-poly segmented figure with a 17-joint skeleton and three looping
// clips (Idle, Walk, Run), written as glTF 2.0 (Models/Mannequin.gltf + .bin) so it goes through the regular model
// importer (fastgltf mesh + skin, assimp skeleton/clips). Faces +Z, Y up, metres; feet at y = 0.
#include "gen.hpp"

#include <oxwald/core/log.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <numbers>

namespace ox::showcase::gen {

namespace {

constexpr f32 kPi = std::numbers::pi_v<f32>;

struct Joint {
    const char* name;
    i32 parent;
    glm::vec3 local; // bind translation relative to the parent
};

// Index order is the skin joint order.
const Joint kJoints[] = {
    {"Root", -1, {0, 0, 0}},
    {"Hips", 0, {0, 0.95f, 0}},
    {"Spine", 1, {0, 0.12f, 0}},
    {"Chest", 2, {0, 0.2f, 0}},
    {"Neck", 3, {0, 0.22f, 0}},
    {"Head", 4, {0, 0.09f, 0}},
    {"UpperArm.L", 3, {0.19f, 0.17f, 0}},
    {"LowerArm.L", 6, {0, -0.28f, 0}},
    {"Hand.L", 7, {0, -0.25f, 0}},
    {"UpperArm.R", 3, {-0.19f, 0.17f, 0}},
    {"LowerArm.R", 9, {0, -0.28f, 0}},
    {"Hand.R", 10, {0, -0.25f, 0}},
    {"UpperLeg.L", 1, {0.1f, -0.05f, 0}},
    {"LowerLeg.L", 12, {0, -0.43f, 0}},
    {"Foot.L", 13, {0, -0.41f, 0}},
    {"UpperLeg.R", 1, {-0.1f, -0.05f, 0}},
    {"LowerLeg.R", 15, {0, -0.43f, 0}},
    {"Foot.R", 16, {0, -0.41f, 0}},
};
constexpr i32 kJointCount = i32(std::size(kJoints));

i32 jointIndex(std::string_view n) {
    for (i32 i = 0; i < kJointCount; ++i)
        if (n == kJoints[i].name) return i;
    return 0;
}

glm::vec3 globalBind(i32 j) {
    glm::vec3 p(0);
    for (; j >= 0; j = kJoints[j].parent) p += kJoints[j].local;
    return p;
}

struct MeshBuilder {
    std::vector<glm::vec3> pos, nrm;
    std::vector<glm::u16vec4> joints;
    std::vector<glm::vec4> weights;
    std::vector<u32> idx;

    // Box between `a` and `b` (model space) with a cross-section of `w`×`d`, tapered by `taper` at b; rigidly bound
    // to joint `j` (blend `j2` with weight at the far end for smoother elbows/knees).
    void segment(glm::vec3 a, glm::vec3 b, f32 w, f32 d, i32 j, f32 taper = 1.0f, i32 j2 = -1) {
        const glm::vec3 axis = glm::normalize(b - a);
        const glm::vec3 side = std::abs(axis.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::normalize(glm::cross(axis, glm::vec3(0, 1, 0)));
        const glm::vec3 fwd = glm::normalize(glm::cross(side, axis));
        glm::vec3 c[8];
        for (int k = 0; k < 8; ++k) {
            const bool top = k >= 4;
            const f32 s = top ? taper : 1.0f;
            const f32 sx = ((k & 1) ? 0.5f : -0.5f) * w * s, sz = ((k & 2) ? 0.5f : -0.5f) * d * s;
            c[k] = (top ? b : a) + side * sx + fwd * sz;
        }
        const int faces[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
        for (const auto& f : faces) {
            glm::vec3 n = glm::normalize(glm::cross(c[f[1]] - c[f[0]], c[f[2]] - c[f[0]]));
            const glm::vec3 centre = (c[f[0]] + c[f[1]] + c[f[2]] + c[f[3]]) * 0.25f;
            const glm::vec3 mid = (a + b) * 0.5f;
            // Ensure outward winding (CCW seen from outside).
            int order[4] = {f[0], f[1], f[2], f[3]};
            if (glm::dot(n, centre - mid) < 0) {
                std::swap(order[1], order[3]);
                n = -n;
            }
            const u32 base = u32(pos.size());
            for (int k : order) {
                pos.push_back(c[k]);
                nrm.push_back(n);
                const bool top = k >= 4;
                if (j2 >= 0 && top) {
                    joints.push_back(glm::u16vec4(u16(j), u16(j2), 0, 0));
                    weights.push_back(glm::vec4(0.5f, 0.5f, 0, 0));
                } else {
                    joints.push_back(glm::u16vec4(u16(j), 0, 0, 0));
                    weights.push_back(glm::vec4(1, 0, 0, 0));
                }
            }
            idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
    }
};

// Keyframed rotation curves (per joint, Euler degrees XYZ as functions of the normalised phase).
struct ClipDef {
    const char* name;
    f32 duration;
    std::function<glm::vec3(i32 joint, f32 phase)> euler;  // degrees
    std::function<glm::vec3(f32 phase)> hipsOffset;        // added to the Hips bind translation
};

glm::quat fromEuler(glm::vec3 deg) { return glm::quat(glm::radians(deg)); }

} // namespace

void generateMannequin(Gen& gen) {
    MeshBuilder m;
    auto J = [](const char* n) { return jointIndex(n); };
    auto G = [](const char* n) { return globalBind(jointIndex(n)); };
    // Torso / head.
    m.segment(G("Hips") + glm::vec3(0, -0.08f, 0), G("Spine"), 0.32f, 0.2f, J("Hips"));
    m.segment(G("Spine"), G("Chest"), 0.3f, 0.18f, J("Spine"), 1.08f);
    m.segment(G("Chest"), G("Neck") + glm::vec3(0, -0.02f, 0), 0.4f, 0.22f, J("Chest"), 0.85f);
    m.segment(G("Neck"), G("Head"), 0.09f, 0.09f, J("Neck"));
    m.segment(G("Head"), G("Head") + glm::vec3(0, 0.24f, 0.0f), 0.19f, 0.22f, J("Head"), 0.85f);
    // Arms.
    for (const char* side : {"L", "R"}) {
        auto n = [&](const char* b) { return std::string(b) + "." + side; };
        m.segment(G(n("UpperArm").c_str()), G(n("LowerArm").c_str()), 0.1f, 0.1f, J(n("UpperArm").c_str()), 0.9f);
        m.segment(G(n("LowerArm").c_str()), G(n("Hand").c_str()), 0.085f, 0.085f, J(n("LowerArm").c_str()), 0.85f);
        m.segment(G(n("Hand").c_str()), G(n("Hand").c_str()) + glm::vec3(0, -0.15f, 0.01f), 0.07f, 0.035f, J(n("Hand").c_str()));
        m.segment(G(n("UpperLeg").c_str()), G(n("LowerLeg").c_str()), 0.15f, 0.15f, J(n("UpperLeg").c_str()), 0.8f);
        m.segment(G(n("LowerLeg").c_str()), G(n("Foot").c_str()), 0.11f, 0.11f, J(n("LowerLeg").c_str()), 0.8f);
        const glm::vec3 ankle = G(n("Foot").c_str());
        m.segment(ankle + glm::vec3(0, -0.06f, -0.06f), ankle + glm::vec3(0, -0.06f, 0.16f), 0.1f, 0.1f, J(n("Foot").c_str()));
    }
    // Shoulder pads (wider silhouette).
    m.segment(G("UpperArm.L") + glm::vec3(-0.05f, 0.02f, 0), G("UpperArm.L") + glm::vec3(0.06f, 0.02f, 0), 0.13f, 0.14f, J("UpperArm.L"));
    m.segment(G("UpperArm.R") + glm::vec3(0.05f, 0.02f, 0), G("UpperArm.R") + glm::vec3(-0.06f, 0.02f, 0), 0.13f, 0.14f, J("UpperArm.R"));

    // ---- clips ----
    const ClipDef clips[] = {
        {"Idle", 3.0f,
         [&](i32 j, f32 ph) {
             const f32 b = std::sin(ph * 2 * kPi);
             if (j == J("Chest")) return glm::vec3(1.5f * b, 0, 0);
             if (j == J("Head")) return glm::vec3(-1.0f * b, 4.0f * std::sin(ph * 2 * kPi + 1.0f), 0);
             if (j == J("UpperArm.L")) return glm::vec3(0, 0, 4.0f + 1.5f * b);
             if (j == J("UpperArm.R")) return glm::vec3(0, 0, -4.0f - 1.5f * b);
             if (j == J("LowerArm.L") || j == J("LowerArm.R")) return glm::vec3(-8.0f, 0, 0);
             return glm::vec3(0);
         },
         [&](f32 ph) { return glm::vec3(0, 0.008f * std::sin(ph * 2 * kPi), 0); }},
        {"Walk", 1.1f,
         [&](i32 j, f32 ph) {
             const f32 s = std::sin(ph * 2 * kPi), c = std::cos(ph * 2 * kPi);
             if (j == J("UpperLeg.L")) return glm::vec3(-28.0f * s, 0, 0);
             if (j == J("UpperLeg.R")) return glm::vec3(28.0f * s, 0, 0);
             if (j == J("LowerLeg.L")) return glm::vec3(std::max(0.0f, 45.0f * c), 0, 0) + glm::vec3(8, 0, 0);
             if (j == J("LowerLeg.R")) return glm::vec3(std::max(0.0f, -45.0f * c), 0, 0) + glm::vec3(8, 0, 0);
             if (j == J("Foot.L")) return glm::vec3(-10.0f * s, 0, 0);
             if (j == J("Foot.R")) return glm::vec3(10.0f * s, 0, 0);
             if (j == J("UpperArm.L")) return glm::vec3(22.0f * s, 0, 5);
             if (j == J("UpperArm.R")) return glm::vec3(-22.0f * s, 0, -5);
             if (j == J("LowerArm.L")) return glm::vec3(-15.0f - 10.0f * std::max(0.0f, -s), 0, 0);
             if (j == J("LowerArm.R")) return glm::vec3(-15.0f - 10.0f * std::max(0.0f, s), 0, 0);
             if (j == J("Spine")) return glm::vec3(3.0f, 6.0f * s, 0);
             if (j == J("Hips")) return glm::vec3(0, -6.0f * s, 2.0f * c);
             return glm::vec3(0);
         },
         [&](f32 ph) { return glm::vec3(0, 0.03f * std::abs(std::cos(ph * 2 * kPi)) - 0.02f, 0); }},
        {"Run", 0.7f,
         [&](i32 j, f32 ph) {
             const f32 s = std::sin(ph * 2 * kPi), c = std::cos(ph * 2 * kPi);
             if (j == J("UpperLeg.L")) return glm::vec3(-50.0f * s - 10.0f, 0, 0);
             if (j == J("UpperLeg.R")) return glm::vec3(50.0f * s - 10.0f, 0, 0);
             if (j == J("LowerLeg.L")) return glm::vec3(25.0f + std::max(0.0f, 75.0f * c), 0, 0);
             if (j == J("LowerLeg.R")) return glm::vec3(25.0f + std::max(0.0f, -75.0f * c), 0, 0);
             if (j == J("Foot.L")) return glm::vec3(-15.0f * s, 0, 0);
             if (j == J("Foot.R")) return glm::vec3(15.0f * s, 0, 0);
             if (j == J("UpperArm.L")) return glm::vec3(45.0f * s, 0, 10);
             if (j == J("UpperArm.R")) return glm::vec3(-45.0f * s, 0, -10);
             if (j == J("LowerArm.L") || j == J("LowerArm.R")) return glm::vec3(-75.0f, 0, 0);
             if (j == J("Spine")) return glm::vec3(10.0f, 10.0f * s, 0);
             if (j == J("Hips")) return glm::vec3(4.0f, -10.0f * s, 0);
             if (j == J("Head")) return glm::vec3(-8.0f, 0, 0);
             return glm::vec3(0);
         },
         [&](f32 ph) { return glm::vec3(0, 0.07f * std::abs(std::cos(ph * 2 * kPi)) - 0.06f, 0); }},
    };

    // ---- binary buffer ----
    std::vector<std::byte> bin;
    FloatRanges floatRanges; // FLOAT accessors: compared numerically by Gen::writeAsset
    auto align4 = [&] { while (bin.size() % 4) bin.push_back(std::byte{0}); };
    auto append = [&](const void* p, usize n) -> std::pair<usize, usize> {
        align4();
        const usize off = bin.size();
        const auto* b = static_cast<const std::byte*>(p);
        bin.insert(bin.end(), b, b + n);
        return {off, n};
    };
    nlohmann::ordered_json views = nlohmann::ordered_json::array(), accessors = nlohmann::ordered_json::array();
    auto addAccessor = [&](const void* data, usize bytes, u32 count, int componentType, const char* type,
                           std::optional<std::pair<std::vector<f32>, std::vector<f32>>> minMax = std::nullopt,
                           std::optional<int> target = std::nullopt) {
        auto [off, n] = append(data, bytes);
        if (componentType == 5126) floatRanges.emplace_back(off, n);
        nlohmann::ordered_json v = {{"buffer", 0}, {"byteOffset", off}, {"byteLength", n}};
        if (target) v["target"] = *target;
        views.push_back(v);
        nlohmann::ordered_json a = {{"bufferView", views.size() - 1}, {"componentType", componentType}, {"count", count}, {"type", type}};
        if (minMax) {
            a["min"] = minMax->first;
            a["max"] = minMax->second;
        }
        accessors.push_back(a);
        return int(accessors.size() - 1);
    };
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const auto& p : m.pos) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const int aPos = addAccessor(m.pos.data(), m.pos.size() * 12, u32(m.pos.size()), 5126, "VEC3",
                                 std::make_pair(std::vector<f32>{lo.x, lo.y, lo.z}, std::vector<f32>{hi.x, hi.y, hi.z}), 34962);
    const int aNrm = addAccessor(m.nrm.data(), m.nrm.size() * 12, u32(m.nrm.size()), 5126, "VEC3", std::nullopt, 34962);
    const int aJnt = addAccessor(m.joints.data(), m.joints.size() * 8, u32(m.joints.size()), 5123, "VEC4", std::nullopt, 34962);
    const int aWgt = addAccessor(m.weights.data(), m.weights.size() * 16, u32(m.weights.size()), 5126, "VEC4", std::nullopt, 34962);
    const int aIdx = addAccessor(m.idx.data(), m.idx.size() * 4, u32(m.idx.size()), 5125, "SCALAR", std::nullopt, 34963);
    std::vector<glm::mat4> ibm(kJointCount);
    for (i32 j = 0; j < kJointCount; ++j) ibm[usize(j)] = glm::translate(glm::mat4(1.0f), -globalBind(j));
    const int aIbm = addAccessor(ibm.data(), ibm.size() * 64, u32(ibm.size()), 5126, "MAT4");

    // Nodes: 0..kJointCount-1 joints, then the mesh node, then the scene root.
    nlohmann::ordered_json nodes = nlohmann::ordered_json::array();
    for (i32 j = 0; j < kJointCount; ++j) {
        nlohmann::ordered_json n = {{"name", kJoints[j].name}, {"translation", {kJoints[j].local.x, kJoints[j].local.y, kJoints[j].local.z}}};
        nlohmann::ordered_json children = nlohmann::ordered_json::array();
        for (i32 c = 0; c < kJointCount; ++c)
            if (kJoints[c].parent == j) children.push_back(c);
        if (!children.empty()) n["children"] = children;
        nodes.push_back(n);
    }
    const int meshNode = kJointCount;
    nodes.push_back({{"name", "MannequinMesh"}, {"mesh", 0}, {"skin", 0}});
    const int rootNode = kJointCount + 1;
    nodes.push_back({{"name", "Mannequin"}, {"children", {0, meshNode}}});

    nlohmann::ordered_json skinJoints = nlohmann::ordered_json::array();
    for (i32 j = 0; j < kJointCount; ++j) skinJoints.push_back(j);

    nlohmann::ordered_json animations = nlohmann::ordered_json::array();
    for (const ClipDef& c : clips) {
        const u32 keys = 25;
        std::vector<f32> times(keys);
        for (u32 k = 0; k < keys; ++k) times[k] = c.duration * f32(k) / f32(keys - 1);
        const int aTime = addAccessor(times.data(), times.size() * 4, keys, 5126, "SCALAR",
                                      std::make_pair(std::vector<f32>{0.0f}, std::vector<f32>{c.duration}));
        nlohmann::ordered_json samplers = nlohmann::ordered_json::array(), channels = nlohmann::ordered_json::array();
        for (i32 j = 1; j < kJointCount; ++j) {
            std::vector<glm::quat> rot(keys);
            bool any = false;
            for (u32 k = 0; k < keys; ++k) {
                const glm::vec3 e = c.euler(j, f32(k) / f32(keys - 1));
                any |= glm::length(e) > 1e-4f;
                rot[k] = fromEuler(e);
            }
            if (!any) continue;
            std::vector<f32> xyzw;
            for (const glm::quat& q : rot) xyzw.insert(xyzw.end(), {q.x, q.y, q.z, q.w});
            const int aRot = addAccessor(xyzw.data(), xyzw.size() * 4, keys, 5126, "VEC4");
            samplers.push_back({{"input", aTime}, {"output", aRot}, {"interpolation", "LINEAR"}});
            channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", j}, {"path", "rotation"}}}});
        }
        std::vector<f32> hips;
        for (u32 k = 0; k < keys; ++k) {
            const glm::vec3 p = kJoints[1].local + c.hipsOffset(f32(k) / f32(keys - 1));
            hips.insert(hips.end(), {p.x, p.y, p.z});
        }
        const int aHips = addAccessor(hips.data(), hips.size() * 4, keys, 5126, "VEC3");
        samplers.push_back({{"input", aTime}, {"output", aHips}, {"interpolation", "LINEAR"}});
        channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", 1}, {"path", "translation"}}}});
        animations.push_back({{"name", c.name}, {"samplers", samplers}, {"channels", channels}});
    }
    align4();

    nlohmann::ordered_json gltf = {
        {"asset", {{"version", "2.0"}, {"generator", "oxshowcase_generate"}}},
        {"scene", 0},
        {"scenes", {{{"name", "Mannequin"}, {"nodes", {rootNode}}}}},
        {"nodes", nodes},
        {"meshes", {{{"name", "Mannequin"},
                     {"primitives", {{{"attributes", {{"POSITION", aPos}, {"NORMAL", aNrm}, {"JOINTS_0", aJnt}, {"WEIGHTS_0", aWgt}}},
                                      {"indices", aIdx},
                                      {"material", 0}}}}}}},
        {"materials", {{{"name", "Mannequin"}, {"pbrMetallicRoughness", {{"baseColorFactor", {0.8f, 0.55f, 0.3f, 1.0f}}, {"metallicFactor", 0.0f}, {"roughnessFactor", 0.45f}}}}}},
        {"skins", {{{"name", "MannequinSkin"}, {"inverseBindMatrices", aIbm}, {"joints", skinJoints}, {"skeleton", 0}}}},
        {"animations", animations},
        {"buffers", {{{"uri", "Mannequin.bin"}, {"byteLength", bin.size()}}}},
        {"bufferViews", views},
        {"accessors", accessors},
    };
    gen.writeAsset("Models/Mannequin.bin", bin, floatRanges);
    gen.writeText(kMannequin, gltf.dump(1) + "\n");
    gen.meta(kMannequin);
    OX_LOG_INFO("generate", "mannequin: {} vertices, {} joints, {} clips", m.pos.size(), kJointCount, std::size(clips));
}

} // namespace ox::showcase::gen
