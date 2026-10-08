#include "bindings.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/mat4x4.hpp>

#include <format>

namespace ox::script {

namespace {

template <class V>
V safeNormalize(const V& v) {
    const f32 len = glm::length(v);
    return len > 1e-12f ? v / len : V(0.f);
}

template <class V>
std::string vecToString(const V& v) {
    if constexpr (V::length() == 2) return std::format("vec2({}, {})", v.x, v.y);
    else if constexpr (V::length() == 3) return std::format("vec3({}, {}, {})", v.x, v.y, v.z);
    else return std::format("vec4({}, {}, {}, {})", v.x, v.y, v.z, v.w);
}

// Common vector API; per-type extras are added by the callers.
template <class V>
sol::usertype<V> bindVector(sol::state& lua, const char* name) {
    auto ut = lua.new_usertype<V>(name, sol::no_constructor);
    ut[sol::meta_function::addition] = [](const V& a, const V& b) { return a + b; };
    ut[sol::meta_function::subtraction] = [](const V& a, const V& b) { return a - b; };
    ut[sol::meta_function::multiplication] =
        sol::overload([](const V& a, const V& b) { return a * b; }, [](const V& a, f32 s) { return a * s; },
                      [](f32 s, const V& a) { return s * a; });
    ut[sol::meta_function::division] =
        sol::overload([](const V& a, const V& b) { return a / b; }, [](const V& a, f32 s) { return a / s; });
    ut[sol::meta_function::unary_minus] = [](const V& a) { return -a; };
    ut[sol::meta_function::equal_to] = [](const V& a, const V& b) { return a == b; };
    ut[sol::meta_function::to_string] = [](const V& v) { return vecToString(v); };
    ut["length"] = [](const V& v) { return glm::length(v); };
    ut["lengthSquared"] = [](const V& v) { return glm::dot(v, v); };
    ut["normalize"] = [](const V& v) { return safeNormalize(v); };
    ut["normalized"] = [](const V& v) { return safeNormalize(v); };
    ut["dot"] = [](const V& a, const V& b) { return glm::dot(a, b); };
    ut["distance"] = [](const V& a, const V& b) { return glm::distance(a, b); };
    ut["lerp"] = [](const V& a, const V& b, f32 t) { return glm::mix(a, b, t); };
    ut["min"] = [](const V& a, const V& b) { return glm::min(a, b); };
    ut["max"] = [](const V& a, const V& b) { return glm::max(a, b); };
    ut["abs"] = [](const V& a) { return glm::abs(a); };
    ut["clone"] = [](const V& a) { return a; };
    ut["unpack"] = [](const V& v) {
        if constexpr (V::length() == 2) return std::make_tuple(v.x, v.y);
        else if constexpr (V::length() == 3) return std::make_tuple(v.x, v.y, v.z);
        else return std::make_tuple(v.x, v.y, v.z, v.w);
    };
    return ut;
}

} // namespace

void bindMath(sol::state& lua) {
    using glm::mat4;
    using glm::quat;
    using glm::vec2;
    using glm::vec3;
    using glm::vec4;

    {
        auto ctor = sol::factories([] { return vec2(0.f); }, [](f32 s) { return vec2(s); },
                                   [](f32 x, f32 y) { return vec2(x, y); });
        auto ut = bindVector<vec2>(lua, "vec2");
        ut[sol::call_constructor] = ctor;
        ut["new"] = ctor;
        ut["x"] = &vec2::x;
        ut["y"] = &vec2::y;
        ut["zero"] = sol::var(vec2(0.f));
        ut["one"] = sol::var(vec2(1.f));
    }
    {
        auto ctor = sol::factories([] { return vec3(0.f); }, [](f32 s) { return vec3(s); },
                                   [](f32 x, f32 y, f32 z) { return vec3(x, y, z); });
        auto ut = bindVector<vec3>(lua, "vec3");
        ut[sol::call_constructor] = ctor;
        ut["new"] = ctor;
        ut["x"] = &vec3::x;
        ut["y"] = &vec3::y;
        ut["z"] = &vec3::z;
        ut["cross"] = [](const vec3& a, const vec3& b) { return glm::cross(a, b); };
        ut["reflect"] = [](const vec3& i, const vec3& n) { return glm::reflect(i, n); };
        ut["zero"] = sol::var(vec3(0.f));
        ut["one"] = sol::var(vec3(1.f));
        ut["up"] = sol::var(vec3(0.f, 1.f, 0.f));
        ut["right"] = sol::var(vec3(1.f, 0.f, 0.f));
        ut["forward"] = sol::var(vec3(0.f, 0.f, -1.f)); // engine convention: -Z forward
    }
    {
        auto ctor = sol::factories([] { return vec4(0.f); }, [](f32 s) { return vec4(s); },
                                   [](const vec3& v, f32 w) { return vec4(v, w); },
                                   [](f32 x, f32 y, f32 z, f32 w) { return vec4(x, y, z, w); });
        auto ut = bindVector<vec4>(lua, "vec4");
        ut[sol::call_constructor] = ctor;
        ut["new"] = ctor;
        ut["x"] = &vec4::x;
        ut["y"] = &vec4::y;
        ut["z"] = &vec4::z;
        ut["w"] = &vec4::w;
        ut["xyz"] = [](const vec4& v) { return vec3(v); };
    }
    {
        // quat(w, x, y, z) like glm; quat() is identity.
        auto ctor = sol::factories([] { return quat(1.f, 0.f, 0.f, 0.f); },
                                   [](f32 w, f32 x, f32 y, f32 z) { return quat(w, x, y, z); });
        auto ut = lua.new_usertype<quat>("quat", sol::no_constructor);
        ut[sol::call_constructor] = ctor;
        ut["new"] = ctor;
        ut["x"] = &quat::x;
        ut["y"] = &quat::y;
        ut["z"] = &quat::z;
        ut["w"] = &quat::w;
        ut["identity"] = sol::var(quat(1.f, 0.f, 0.f, 0.f));
        ut["fromEuler"] = sol::overload([](const vec3& e) { return quat(e); },
                                        [](f32 pitch, f32 yaw, f32 roll) { return quat(vec3(pitch, yaw, roll)); });
        ut["fromEulerDegrees"] = [](const vec3& e) { return quat(glm::radians(e)); };
        ut["toEuler"] = [](const quat& q) { return glm::eulerAngles(q); };
        ut["toEulerDegrees"] = [](const quat& q) { return glm::degrees(glm::eulerAngles(q)); };
        ut["angleAxis"] = [](f32 angle, const vec3& axis) { return glm::angleAxis(angle, safeNormalize(axis)); };
        ut["lookRotation"] = [](const vec3& forward, sol::optional<vec3> up) {
            return glm::quatLookAtRH(safeNormalize(forward), up.value_or(vec3(0.f, 1.f, 0.f)));
        };
        ut["fromTo"] = [](const vec3& from, const vec3& to) {
            return glm::rotation(safeNormalize(from), safeNormalize(to));
        };
        ut[sol::meta_function::multiplication] =
            sol::overload([](const quat& a, const quat& b) { return a * b; },
                          [](const quat& q, const vec3& v) { return q * v; });
        ut[sol::meta_function::equal_to] = [](const quat& a, const quat& b) { return a == b; };
        ut[sol::meta_function::unary_minus] = [](const quat& q) { return -q; };
        ut[sol::meta_function::to_string] = [](const quat& q) {
            return std::format("quat({}, {}, {}, {})", q.w, q.x, q.y, q.z);
        };
        ut["normalize"] = [](const quat& q) { return glm::normalize(q); };
        ut["normalized"] = [](const quat& q) { return glm::normalize(q); };
        ut["inverse"] = [](const quat& q) { return glm::inverse(q); };
        ut["conjugate"] = [](const quat& q) { return glm::conjugate(q); };
        ut["length"] = [](const quat& q) { return glm::length(q); };
        ut["dot"] = [](const quat& a, const quat& b) { return glm::dot(a, b); };
        ut["slerp"] = [](const quat& a, const quat& b, f32 t) { return glm::slerp(a, b, t); };
        ut["lerp"] = [](const quat& a, const quat& b, f32 t) {
            const quat bb = glm::dot(a, b) < 0.f ? -b : b;
            return glm::normalize(quat(glm::mix(a.w, bb.w, t), glm::mix(a.x, bb.x, t), glm::mix(a.y, bb.y, t),
                                       glm::mix(a.z, bb.z, t)));
        };
        ut["angle"] = [](const quat& q) { return glm::angle(q); };
        ut["axis"] = [](const quat& q) { return glm::axis(q); };
        ut["rotate"] = [](const quat& q, const vec3& v) { return q * v; };
        ut["forward"] = [](const quat& q) { return q * vec3(0.f, 0.f, -1.f); };
        ut["up"] = [](const quat& q) { return q * vec3(0.f, 1.f, 0.f); };
        ut["right"] = [](const quat& q) { return q * vec3(1.f, 0.f, 0.f); };
    }
    {
        auto ctor = sol::factories([] { return mat4(1.f); }, [](f32 diagonal) { return mat4(diagonal); });
        auto ut = lua.new_usertype<mat4>("mat4", sol::no_constructor);
        ut[sol::call_constructor] = ctor;
        ut["new"] = ctor;
        ut["identity"] = [] { return mat4(1.f); };
        ut["translation"] = [](const vec3& t) { return glm::translate(mat4(1.f), t); };
        ut["rotation"] = [](const quat& q) { return glm::mat4_cast(q); };
        ut["scaling"] = [](const vec3& s) { return glm::scale(mat4(1.f), s); };
        ut["trs"] = [](const vec3& t, const quat& r, const vec3& s) {
            return glm::translate(mat4(1.f), t) * glm::mat4_cast(r) * glm::scale(mat4(1.f), s);
        };
        ut["lookAt"] = [](const vec3& eye, const vec3& center, sol::optional<vec3> up) {
            return glm::lookAt(eye, center, up.value_or(vec3(0.f, 1.f, 0.f)));
        };
        ut["perspective"] = [](f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
            return glm::perspective(fovY, aspect, zNear, zFar);
        };
        ut[sol::meta_function::multiplication] =
            sol::overload([](const mat4& a, const mat4& b) { return a * b; },
                          [](const mat4& m, const vec4& v) { return m * v; });
        ut[sol::meta_function::equal_to] = [](const mat4& a, const mat4& b) { return a == b; };
        ut[sol::meta_function::to_string] = [](const mat4& m) {
            return std::format("mat4(({}, {}, {}, {}), ({}, {}, {}, {}), ({}, {}, {}, {}), ({}, {}, {}, {}))",
                               m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3], m[2][0],
                               m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]);
        };
        ut["inverse"] = [](const mat4& m) { return glm::inverse(m); };
        ut["transpose"] = [](const mat4& m) { return glm::transpose(m); };
        ut["transformPoint"] = [](const mat4& m, const vec3& p) {
            const vec4 r = m * vec4(p, 1.f);
            return vec3(r) / (r.w != 0.f ? r.w : 1.f);
        };
        ut["transformVector"] = [](const mat4& m, const vec3& v) { return vec3(m * vec4(v, 0.f)); };
        ut["getTranslation"] = [](const mat4& m) { return vec3(m[3]); };
        // 0-based column, row (glm is column-major).
        ut["get"] = [](const mat4& m, int column, int row) { return m[column & 3][row & 3]; };
        ut["set"] = [](mat4& m, int column, int row, f32 v) { m[column & 3][row & 3] = v; };
    }

    // Scalar helpers live in the math library itself (sandboxes see it read-only).
    lua.script(R"lua(
        function math.clamp(x, lo, hi) if x < lo then return lo elseif x > hi then return hi end return x end
        function math.lerp(a, b, t) return a + (b - a) * t end
        function math.inverseLerp(a, b, x) if a == b then return 0 end return (x - a) / (b - a) end
        function math.remap(x, a0, a1, b0, b1) return b0 + (b1 - b0) * math.inverseLerp(a0, a1, x) end
        function math.smoothstep(e0, e1, x)
            local t = math.clamp((x - e0) / (e1 - e0), 0, 1)
            return t * t * (3 - 2 * t)
        end
        function math.sign(x) if x > 0 then return 1 elseif x < 0 then return -1 end return 0 end
        function math.round(x) return math.floor(x + 0.5) end
        function math.approximately(a, b, eps) return math.abs(a - b) <= (eps or 1e-6) end
    )lua");
}

} // namespace ox::script
