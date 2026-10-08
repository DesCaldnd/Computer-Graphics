#include "lua_types.hpp"

#include <oxwald/core/log.hpp>
#include <oxwald/gameplay/ai.hpp>
#include <oxwald/gameplay/animation.hpp>
#include <oxwald/gameplay/audio.hpp>
#include <oxwald/gameplay/physics.hpp>
#include <oxwald/gameplay/spline.hpp>
#include <oxwald/scene/components.hpp>

#include <glm/gtx/norm.hpp>

#include <cmath>
#include <optional>

namespace ox::gameplay::lua {

namespace {

using reflect::Kind;
using reflect::TypeInfo;
using reflect::ValueRef;
using serial::Tag;
using serial::Value;

// ---- value conversion: reflection <-> Lua ----------------------------------------------------------------

sol::object genericToLua(sol::state_view lua, ScriptRuntime& rt, const Value& v) {
    switch (v.tag()) {
    case Tag::Null: return sol::lua_nil;
    case Tag::Bool: return sol::make_object(lua, v.getBool());
    case Tag::I8:
    case Tag::I16:
    case Tag::I32:
    case Tag::I64:
    case Tag::U8:
    case Tag::U16:
    case Tag::U32:
    case Tag::U64: return sol::make_object(lua, static_cast<lua_Integer>(v.getInt()));
    case Tag::F32:
    case Tag::F64: return sol::make_object(lua, v.getDouble());
    case Tag::String:
    case Tag::Enum: return sol::make_object(lua, v.getString());
    case Tag::Vec2: return sol::make_object(lua, glm::vec2(v.getVec()));
    case Tag::Vec3:
    case Tag::IVec3: return sol::make_object(lua, glm::vec3(v.getVec()));
    case Tag::Vec4:
    case Tag::IVec4: return sol::make_object(lua, v.getVec());
    case Tag::IVec2: return sol::make_object(lua, glm::vec2(v.getVec()));
    case Tag::Quat: return sol::make_object(lua, v.getQuat());
    case Tag::Mat4: return sol::make_object(lua, v.getMat4());
    case Tag::Uuid: return sol::make_object(lua, v.getUuid().toString());
    case Tag::EntityRef: {
        World* w = rt.world();
        const Entity e = w ? w->find(v.getUuid()) : Entity{};
        return e.valid() ? sol::make_object(lua, wrap(rt, e)) : sol::object(sol::lua_nil);
    }
    case Tag::Optional: return v.hasValue() ? genericToLua(lua, rt, v.optionalValue()) : sol::object(sol::lua_nil);
    case Tag::Array: {
        sol::table t = lua.create_table();
        for (usize i = 0; i < v.size(); ++i) t[i + 1] = genericToLua(lua, rt, v.at(i));
        return t;
    }
    case Tag::Object:
    case Tag::Map: {
        sol::table t = lua.create_table();
        for (const auto& [k, fv] : v.fields()) t[k] = genericToLua(lua, rt, fv);
        return t;
    }
    }
    return sol::lua_nil;
}

ValueRef resolveRef(const LuaComponent& c) {
    if (!c.owner.valid() || !c.info) return {};
    ValueRef root = c.info->ref(*c.owner.world, c.owner.handle);
    return c.path.empty() ? root : reflect::resolvePath(root, c.path);
}

sol::object refToLua(sol::state_view lua, const LuaComponent& parent, const ValueRef& ref, std::string path) {
    if (!ref.valid()) return sol::lua_nil;
    const TypeInfo& t = *ref.type;
    switch (t.kind) {
    case Kind::Bool: return sol::make_object(lua, *static_cast<const bool*>(ref.ptr));
    case Kind::Int:
    case Kind::UInt: return sol::make_object(lua, static_cast<lua_Integer>(ref.get().getInt()));
    case Kind::Float: return sol::make_object(lua, ref.get().getDouble());
    case Kind::String: return sol::make_object(lua, *static_cast<const std::string*>(ref.ptr));
    case Kind::Uuid: return sol::make_object(lua, static_cast<const Uuid*>(ref.ptr)->toString());
    case Kind::Enum: {
        const i64 v = t.getEnum(ref.ptr);
        if (const auto* entry = t.findEnumByValue(v)) return sol::make_object(lua, entry->name);
        return sol::make_object(lua, static_cast<lua_Integer>(v));
    }
    case Kind::Math:
        switch (t.tag) {
        case Tag::Vec2: return sol::make_object(lua, *static_cast<const glm::vec2*>(ref.ptr));
        case Tag::Vec3: return sol::make_object(lua, *static_cast<const glm::vec3*>(ref.ptr));
        case Tag::Vec4: return sol::make_object(lua, *static_cast<const glm::vec4*>(ref.ptr));
        case Tag::Quat: return sol::make_object(lua, *static_cast<const glm::quat*>(ref.ptr));
        case Tag::Mat4: return sol::make_object(lua, *static_cast<const glm::mat4*>(ref.ptr));
        default: return genericToLua(lua, *parent.owner.rt, ref.get());
        }
    case Kind::Custom: return genericToLua(lua, *parent.owner.rt, ref.get());
    case Kind::Struct:
    case Kind::Array:
    case Kind::Map:
    case Kind::Optional: return sol::make_object(lua, LuaComponent{parent.owner, parent.info, std::move(path)});
    }
    return sol::lua_nil;
}

std::optional<glm::vec4> luaVector(const sol::object& o, int n) {
    if (o.is<glm::vec3>()) return glm::vec4(o.as<glm::vec3>(), 0.f);
    if (o.is<glm::vec2>()) return glm::vec4(o.as<glm::vec2>(), 0.f, 0.f);
    if (o.is<glm::vec4>()) return o.as<glm::vec4>();
    if (o.get_type() == sol::type::table) {
        sol::table t = o.as<sol::table>();
        glm::vec4 r(0.f);
        static const char* names[4] = {"x", "y", "z", "w"};
        for (int i = 0; i < n; ++i) {
            sol::object a = t[names[i]];
            if (!a.valid() || a.get_type() != sol::type::number) a = t[i + 1];
            if (a.get_type() == sol::type::number) r[i] = a.as<f32>();
        }
        return r;
    }
    if (o.get_type() == sol::type::number) return glm::vec4(o.as<f32>());
    return std::nullopt;
}

std::optional<Value> luaToValue(const sol::object& o, const TypeInfo& t);

std::optional<Value> luaToValueImpl(const sol::object& o, const TypeInfo& t) {
    if (o.is<LuaComponent>()) {
        const ValueRef ref = resolveRef(o.as<LuaComponent>());
        if (ref.valid()) return ref.get();
        return std::nullopt;
    }
    const sol::type lt = o.get_type();
    switch (t.kind) {
    case Kind::Bool:
        if (lt == sol::type::boolean) return Value::makeBool(o.as<bool>());
        if (lt == sol::type::number) return Value::makeBool(o.as<f64>() != 0.0);
        return std::nullopt;
    case Kind::Int:
        if (lt == sol::type::number) return Value::makeInt(static_cast<i64>(std::llround(o.as<f64>())));
        if (lt == sol::type::boolean) return Value::makeInt(o.as<bool>() ? 1 : 0);
        return std::nullopt;
    case Kind::UInt:
        if (lt == sol::type::number) return Value::makeUInt(static_cast<u64>(std::max<i64>(0, std::llround(o.as<f64>()))));
        if (lt == sol::type::boolean) return Value::makeUInt(o.as<bool>() ? 1 : 0);
        return std::nullopt;
    case Kind::Float:
        if (lt == sol::type::number) return Value::makeF64(o.as<f64>());
        return std::nullopt;
    case Kind::String:
        if (lt == sol::type::string || lt == sol::type::number) return Value::makeString(o.as<std::string>());
        return std::nullopt;
    case Kind::Uuid:
        if (lt == sol::type::string) {
            if (auto u = Uuid::parse(o.as<std::string>())) return Value::makeUuid(*u);
        }
        if (lt == sol::type::lua_nil) return Value::makeUuid(Uuid{});
        return std::nullopt;
    case Kind::Enum:
        if (lt == sol::type::string) return Value::makeEnum(o.as<std::string>());
        if (lt == sol::type::number) return Value::makeInt(static_cast<i64>(o.as<f64>()));
        return std::nullopt;
    case Kind::Math:
        switch (t.tag) {
        case Tag::Quat:
            if (o.is<glm::quat>()) return Value::makeQuat(o.as<glm::quat>());
            if (lt == sol::type::table) {
                sol::table q = o.as<sol::table>();
                return Value::makeQuat(glm::quat(q.get_or("w", 1.f), q.get_or("x", 0.f), q.get_or("y", 0.f),
                                                 q.get_or("z", 0.f)));
            }
            return std::nullopt;
        case Tag::Mat4:
            if (o.is<glm::mat4>()) return Value::makeMat4(o.as<glm::mat4>());
            return std::nullopt;
        case Tag::Vec2:
            if (auto v = luaVector(o, 2)) return Value::makeVec2(glm::vec2(*v));
            return std::nullopt;
        case Tag::Vec3:
            if (auto v = luaVector(o, 3)) return Value::makeVec3(glm::vec3(*v));
            return std::nullopt;
        case Tag::Vec4:
            if (auto v = luaVector(o, 4)) return Value::makeVec4(*v);
            return std::nullopt;
        case Tag::IVec2:
            if (auto v = luaVector(o, 2)) return Value::makeIVec2(glm::ivec2(*v));
            return std::nullopt;
        case Tag::IVec3:
            if (auto v = luaVector(o, 3)) return Value::makeIVec3(glm::ivec3(*v));
            return std::nullopt;
        case Tag::IVec4:
            if (auto v = luaVector(o, 4)) return Value::makeIVec4(glm::ivec4(*v));
            return std::nullopt;
        default: return std::nullopt;
        }
    case Kind::Custom:
        if (t.tag == Tag::EntityRef) {
            if (o.is<LuaEntity>()) {
                const Entity e = o.as<LuaEntity>().entity();
                return Value::makeEntityRef(e.valid() ? e.uuid() : Uuid{});
            }
            if (lt == sol::type::lua_nil) return Value::makeEntityRef(Uuid{});
            if (lt == sol::type::string) {
                if (auto u = Uuid::parse(o.as<std::string>())) return Value::makeEntityRef(*u);
            }
        }
        return std::nullopt;
    case Kind::Struct: {
        if (lt != sol::type::table) return std::nullopt;
        sol::table tbl = o.as<sol::table>();
        Value out = Value::makeObject(t.name);
        for (const auto& f : t.fields) {
            sol::object fv = tbl[f.name];
            if (!fv.valid() || fv.get_type() == sol::type::lua_nil) continue;
            if (auto v = luaToValue(fv, *f.type)) out.set(f.name, std::move(*v));
        }
        return out;
    }
    case Kind::Array: {
        if (lt != sol::type::table) return std::nullopt;
        sol::table tbl = o.as<sol::table>();
        Value out = Value::makeArray(serial::descOf(*t.element));
        const usize n = tbl.size();
        for (usize i = 1; i <= n; ++i) {
            auto v = luaToValue(tbl[i], *t.element);
            if (!v) return std::nullopt;
            out.push(std::move(*v));
        }
        return out;
    }
    case Kind::Map: {
        if (lt != sol::type::table) return std::nullopt;
        Value out = Value::makeMap(serial::descOf(*t.element));
        for (auto& [k, v] : o.as<sol::table>()) {
            if (k.get_type() != sol::type::string) continue;
            if (auto cv = luaToValue(v, *t.element)) out.set(k.as<std::string>(), std::move(*cv));
        }
        return out;
    }
    case Kind::Optional:
        if (lt == sol::type::lua_nil) return Value::makeOptional(serial::descOf(*t.element));
        if (auto v = luaToValue(o, *t.element)) return Value::makeOptional(serial::descOf(*t.element), std::move(*v));
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<Value> luaToValue(const sol::object& o, const TypeInfo& t) { return luaToValueImpl(o, t); }

std::string childPath(const std::string& base, const sol::object& key, bool& ok) {
    ok = true;
    if (key.get_type() == sol::type::string) return base.empty() ? key.as<std::string>() : base + "." + key.as<std::string>();
    if (key.get_type() == sol::type::number) {
        const i64 i = static_cast<i64>(key.as<f64>()) - 1; // Lua arrays are 1-based
        if (i >= 0) return base + "[" + std::to_string(i) + "]";
    }
    ok = false;
    return {};
}

ValueRef childRef(const LuaComponent& c, const std::string& path) {
    if (!c.owner.valid() || !c.info) return {};
    return reflect::resolvePath(c.info->ref(*c.owner.world, c.owner.handle), path);
}

bool assign(const LuaComponent& c, const std::string& path, const sol::object& value) {
    const ValueRef ref = path.empty() ? resolveRef(c) : childRef(c, path);
    if (!ref.valid()) throw std::runtime_error("unknown field '" + path + "' of " + c.info->name);
    auto v = luaToValue(value, *ref.type);
    if (!v || !ref.set(*v)) {
        throw std::runtime_error("cannot assign " + std::string(sol::type_name(value.lua_state(), value.get_type())) +
                                 " to " + c.info->name + "." + path + " (" + ref.type->name + ")");
    }
    c.info->notifyChanged(*c.owner.world, c.owner.handle);
    return true;
}

const ComponentInfo* findComponent(std::string_view name) {
    const ComponentInfo* info = ComponentRegistry::instance().find(name);
    if (!info) {
        // Accept "RigidBodyComponent" style names too.
        constexpr std::string_view suffix = "Component";
        if (name.size() > suffix.size() && name.ends_with(suffix)) {
            info = ComponentRegistry::instance().find(name.substr(0, name.size() - suffix.size()));
        }
    }
    return info;
}

// ---- entity helpers --------------------------------------------------------------------------------------

Entity need(const LuaEntity& e) {
    const Entity en = e.entity();
    if (!en.valid()) throw std::runtime_error("entity is no longer valid");
    return en;
}

sol::object getComponent(LuaEntity& e, const std::string& name, sol::this_state ts) {
    const Entity en = e.entity();
    const ComponentInfo* info = findComponent(name);
    if (!en.valid() || !info || !info->has(*e.world, e.handle)) return sol::lua_nil;
    return sol::make_object(ts, LuaComponent{e, info, {}});
}

sol::object makeEntityObject(sol::this_state ts, ScriptRuntime& rt, Entity e) {
    return e.valid() ? sol::make_object(ts, wrap(rt, e)) : sol::object(sol::lua_nil);
}

lua_Integer packSound(audio::SoundHandle h) {
    return static_cast<lua_Integer>((static_cast<u64>(h.generation) << 32) | h.index);
}
audio::SoundHandle unpackSound(lua_Integer v) {
    const u64 u = static_cast<u64>(v);
    return audio::SoundHandle{static_cast<u32>(u & 0xffffffffu), static_cast<u32>(u >> 32)};
}

} // namespace

void installBindings(ScriptRuntime& rt) {
    sol::state& lua = rt.vm().lua();
    ScriptRuntime* R = &rt;

    // ---- component proxy -----------------------------------------------------------------------------------
    lua.new_usertype<LuaComponent>(
        "OxComponent", sol::no_constructor,
        "_type", [](const LuaComponent& c) { return c.info ? c.info->name : std::string(); },
        "_path", [](const LuaComponent& c) { return c.path; },
        "_get",
        [](const LuaComponent& c, const std::string& path, sol::this_state ts) -> sol::object {
            const std::string full = c.path.empty() ? path : c.path + "." + path;
            return refToLua(ts, c, childRef(c, full), full);
        },
        "_set",
        [](const LuaComponent& c, const std::string& path, sol::object value) {
            assign(c, c.path.empty() ? path : c.path + "." + path, value);
        },
        "_fields",
        [](const LuaComponent& c, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table out = lv.create_table();
            const ValueRef ref = resolveRef(c);
            if (ref.valid() && ref.type->kind == Kind::Struct) {
                int i = 1;
                for (const auto& f : ref.type->fields) out[i++] = f.name;
            }
            return out;
        },
        "_value", [](const LuaComponent& c, sol::this_state ts) -> sol::object {
            const ValueRef ref = resolveRef(c);
            return ref.valid() ? genericToLua(ts, *c.owner.rt, ref.get()) : sol::object(sol::lua_nil);
        },
        sol::meta_function::index,
        [](const LuaComponent& c, sol::object key, sol::this_state ts) -> sol::object {
            bool ok = false;
            std::string path = childPath(c.path, key, ok);
            if (!ok) return sol::lua_nil;
            return refToLua(ts, c, childRef(c, path), std::move(path));
        },
        sol::meta_function::new_index,
        [](const LuaComponent& c, sol::object key, sol::object value) {
            bool ok = false;
            const std::string path = childPath(c.path, key, ok);
            if (!ok) throw std::runtime_error("invalid component key");
            assign(c, path, value);
        },
        sol::meta_function::length,
        [](const LuaComponent& c) -> usize {
            const ValueRef ref = resolveRef(c);
            return ref.valid() && ref.type->containerSize ? ref.type->containerSize(ref.ptr) : 0;
        },
        sol::meta_function::to_string,
        [](const LuaComponent& c) { return "Component(" + (c.info ? c.info->name : "?") + (c.path.empty() ? "" : "." + c.path) + ")"; });

    // ---- transform helper ----------------------------------------------------------------------------------
    lua.new_usertype<LuaTransform>(
        "OxTransform", sol::no_constructor,
        "position", sol::property([](const LuaTransform& t) { return need(t.owner).localTransform().position; },
                                  [](const LuaTransform& t, const glm::vec3& v) { need(t.owner).setPosition(v); }),
        "rotation", sol::property([](const LuaTransform& t) { return need(t.owner).localTransform().rotation; },
                                  [](const LuaTransform& t, const glm::quat& q) { need(t.owner).setRotation(glm::normalize(q)); }),
        "scale", sol::property([](const LuaTransform& t) { return need(t.owner).localTransform().scale; },
                               [](const LuaTransform& t, sol::object v) {
                                   if (v.get_type() == sol::type::number) need(t.owner).setScale(glm::vec3(v.as<f32>()));
                                   else need(t.owner).setScale(v.as<glm::vec3>());
                               }),
        "worldPosition", sol::property([](const LuaTransform& t) { return need(t.owner).worldPosition(); },
                                       [](const LuaTransform& t, const glm::vec3& v) { need(t.owner).setWorldPosition(v); }),
        "worldRotation", sol::property([](const LuaTransform& t) { return need(t.owner).worldRotation(); },
                                       [](const LuaTransform& t, const glm::quat& q) {
                                           need(t.owner).setWorldRotation(glm::normalize(q));
                                       }),
        "forward", sol::property([](const LuaTransform& t) { return need(t.owner).worldRotation() * kWorldForward; }),
        "right", sol::property([](const LuaTransform& t) { return need(t.owner).worldRotation() * kWorldRight; }),
        "up", sol::property([](const LuaTransform& t) { return need(t.owner).worldRotation() * kWorldUp; }),
        "translate",
        [](const LuaTransform& t, const glm::vec3& d, sol::optional<std::string> space) {
            const Entity e = need(t.owner);
            if (space && *space == "local") {
                e.setPosition(e.localTransform().position + e.localTransform().rotation * d);
            } else {
                e.setWorldPosition(e.worldPosition() + d);
            }
        },
        "rotate",
        sol::overload(
            [](const LuaTransform& t, const glm::quat& q) {
                const Entity e = need(t.owner);
                e.setRotation(glm::normalize(e.localTransform().rotation * q));
            },
            [](const LuaTransform& t, const glm::vec3& axis, f32 angle) {
                const Entity e = need(t.owner);
                if (glm::length2(axis) <= 0.f) return;
                e.setRotation(glm::normalize(e.localTransform().rotation * glm::angleAxis(angle, glm::normalize(axis))));
            }),
        "lookAt",
        sol::overload(
            [](const LuaTransform& t, const glm::vec3& target, sol::optional<glm::vec3> up) {
                const Entity e = need(t.owner);
                const glm::vec3 d = target - e.worldPosition();
                if (glm::length2(d) > 1e-10f) e.setWorldRotation(lookRotation(glm::normalize(d), up.value_or(kWorldUp)));
            },
            [](const LuaTransform& t, const LuaEntity& target, sol::optional<glm::vec3> up) {
                const Entity e = need(t.owner);
                const glm::vec3 d = need(target).worldPosition() - e.worldPosition();
                if (glm::length2(d) > 1e-10f) e.setWorldRotation(lookRotation(glm::normalize(d), up.value_or(kWorldUp)));
            }),
        "transformPoint", [](const LuaTransform& t, const glm::vec3& p) { return need(t.owner).worldTransform().transformPoint(p); },
        "inverseTransformPoint",
        [](const LuaTransform& t, const glm::vec3& p) {
            return glm::vec3(glm::inverse(need(t.owner).worldMatrix()) * glm::vec4(p, 1.f));
        });

    // ---- physics body --------------------------------------------------------------------------------------
    lua.new_usertype<LuaBody>(
        "OxBody", sol::no_constructor,
        "isValid", [R](const LuaBody& b) { return R->physics() && R->physics()->bodyOf(b.owner.entity()).valid(); },
        "addForce", [R](const LuaBody& b, const glm::vec3& f) { if (R->physics()) R->physics()->addForce(need(b.owner), f); },
        "addTorque", [R](const LuaBody& b, const glm::vec3& t) { if (R->physics()) R->physics()->addTorque(need(b.owner), t); },
        "addImpulse",
        [R](const LuaBody& b, const glm::vec3& i, sol::optional<glm::vec3> point) {
            if (!R->physics()) return;
            if (point) R->physics()->addImpulseAtPoint(need(b.owner), i, *point);
            else R->physics()->addImpulse(need(b.owner), i);
        },
        "teleport",
        [R](const LuaBody& b, const glm::vec3& p, sol::optional<glm::quat> q) {
            const Entity e = need(b.owner);
            if (R->physics()) R->physics()->teleport(e, p, q.value_or(e.worldRotation()));
        },
        "linearVelocity",
        sol::property([R](const LuaBody& b) { return R->physics() ? R->physics()->linearVelocity(need(b.owner)) : glm::vec3(0.f); },
                      [R](const LuaBody& b, const glm::vec3& v) { if (R->physics()) R->physics()->setLinearVelocity(need(b.owner), v); }),
        "angularVelocity",
        sol::property([R](const LuaBody& b) { return R->physics() ? R->physics()->angularVelocity(need(b.owner)) : glm::vec3(0.f); },
                      [R](const LuaBody& b, const glm::vec3& v) { if (R->physics()) R->physics()->setAngularVelocity(need(b.owner), v); }));

    // ---- nav agent -----------------------------------------------------------------------------------------
    lua.new_usertype<LuaAgent>(
        "OxAgent", sol::no_constructor,
        "moveTo", [R](const LuaAgent& a, const glm::vec3& p) { return R->ai() && R->ai()->moveTo(need(a.owner), p); },
        "stop", [R](const LuaAgent& a) { if (R->ai()) R->ai()->stop(need(a.owner)); },
        "reached", [R](const LuaAgent& a) { return R->ai() && R->ai()->reached(need(a.owner)); },
        "velocity", sol::property([](const LuaAgent& a) {
            const auto* c = need(a.owner).tryGet<NavAgentComponent>();
            return c ? c->velocity : glm::vec3(0.f);
        }),
        "destination", sol::property([](const LuaAgent& a) -> sol::optional<glm::vec3> {
            const auto* c = need(a.owner).tryGet<NavAgentComponent>();
            if (!c || !c->hasDestination) return sol::nullopt;
            return c->destination;
        }));

    // ---- animator ------------------------------------------------------------------------------------------
    lua.new_usertype<LuaAnimator>(
        "OxAnimator", sol::no_constructor,
        "setFloat", [R](const LuaAnimator& a, const std::string& n, f32 v) { if (R->animation()) R->animation()->setFloat(need(a.owner), n, v); },
        "setBool", [R](const LuaAnimator& a, const std::string& n, bool v) { if (R->animation()) R->animation()->setBool(need(a.owner), n, v); },
        "setTrigger", [R](const LuaAnimator& a, const std::string& n) { if (R->animation()) R->animation()->setTrigger(need(a.owner), n); },
        "play",
        [R](const LuaAnimator& a, const std::string& state, sol::optional<f32> fade) {
            return R->animation() && R->animation()->play(need(a.owner), state, 0, fade.value_or(0.f));
        },
        "currentState", sol::property([R](const LuaAnimator& a) {
            anim::Animator* an = R->animation() ? R->animation()->animator(need(a.owner)) : nullptr;
            if (!an) return std::string();
            const i32 s = an->currentState(0);
            const auto& states = an->controller().layer(0).states;
            return s >= 0 && usize(s) < states.size() ? states[usize(s)].name : std::string();
        }));

    // ---- spline --------------------------------------------------------------------------------------------
    lua.new_usertype<LuaSpline>(
        "OxSpline", sol::no_constructor,
        "length", [R](const LuaSpline& s) { return R->splines() ? R->splines()->length(need(s.owner)) : 0.f; },
        "positionAt",
        [R](const LuaSpline& s, f32 d) -> sol::optional<glm::vec3> {
            if (!R->splines()) return sol::nullopt;
            auto p = R->splines()->positionAtDistance(need(s.owner), d);
            return p ? sol::optional<glm::vec3>(*p) : sol::nullopt;
        },
        "rotationAt",
        [R](const LuaSpline& s, f32 d) -> sol::optional<glm::quat> {
            if (!R->splines()) return sol::nullopt;
            auto q = R->splines()->rotationAtDistance(need(s.owner), d);
            return q ? sol::optional<glm::quat>(*q) : sol::nullopt;
        },
        "closestDistance",
        [R](const LuaSpline& s, const glm::vec3& p) -> sol::optional<f32> {
            if (!R->splines()) return sol::nullopt;
            auto d = R->splines()->closestDistance(need(s.owner), p);
            return d ? sol::optional<f32>(*d) : sol::nullopt;
        });

    // ---- entity --------------------------------------------------------------------------------------------
    lua.new_usertype<LuaEntity>(
        "OxEntity", sol::no_constructor,
        "isValid", [](const LuaEntity& e) { return e.valid(); },
        "name", sol::property([](const LuaEntity& e) { return need(e).name(); },
                              [](const LuaEntity& e, const std::string& n) { need(e).setName(n); }),
        "id", sol::property([](const LuaEntity& e) { return need(e).uuid().toString(); }),
        "active", sol::property([](const LuaEntity& e) { return need(e).active(); },
                                [](const LuaEntity& e, bool a) { need(e).setActive(a); }),
        "parent", sol::property([](const LuaEntity& e, sol::this_state ts) {
            return makeEntityObject(ts, *e.rt, need(e).parent());
        }),
        "transform", sol::property([](const LuaEntity& e) { need(e); return LuaTransform{e}; }),
        "body", sol::property([](const LuaEntity& e) { need(e); return LuaBody{e}; }),
        "agent", sol::property([](const LuaEntity& e) { need(e); return LuaAgent{e}; }),
        "animator", sol::property([](const LuaEntity& e) { need(e); return LuaAnimator{e}; }),
        "spline", sol::property([](const LuaEntity& e) { need(e); return LuaSpline{e}; }),
        "get", &getComponent,
        "has", [](const LuaEntity& e, const std::string& name) {
            const ComponentInfo* info = findComponent(name);
            return e.valid() && info && info->has(*e.world, e.handle);
        },
        "add",
        [](LuaEntity& e, const std::string& name, sol::optional<sol::table> init, sol::this_state ts) -> sol::object {
            const Entity en = need(e);
            const ComponentInfo* info = findComponent(name);
            if (!info) throw std::runtime_error("unknown component '" + name + "'");
            info->add(*e.world, en.handle());
            LuaComponent c{e, info, {}};
            if (init) assign(c, {}, *init);
            return sol::make_object(ts, c);
        },
        "remove",
        [](const LuaEntity& e, const std::string& name) {
            const Entity en = need(e);
            const ComponentInfo* info = findComponent(name);
            if (!info || !info->removable || !info->has(*e.world, en.handle())) return false;
            info->remove(*e.world, en.handle());
            return true;
        },
        "destroy", [](const LuaEntity& e) { if (e.valid()) e.entity().destroy(); },
        "children",
        [](const LuaEntity& e, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table out = lv.create_table();
            int i = 1;
            for (const Entity& c : need(e).children()) out[i++] = wrap(*e.rt, c);
            return out;
        },
        "findChild",
        [](const LuaEntity& e, const std::string& name, sol::this_state ts) -> sol::object {
            std::vector<Entity> stack = need(e).children();
            while (!stack.empty()) {
                const Entity c = stack.back();
                stack.pop_back();
                if (c.name() == name) return sol::make_object(ts, wrap(*e.rt, c));
                for (const Entity& g : c.children()) stack.push_back(g);
            }
            return sol::lua_nil;
        },
        "setParent",
        [](const LuaEntity& e, sol::object parent) {
            const Entity en = need(e);
            en.setParent(parent.is<LuaEntity>() ? parent.as<LuaEntity>().entity() : Entity{}, true);
        },
        "hasTag", [](const LuaEntity& e, const std::string& tag) {
            const auto* t = need(e).tryGet<TagComponent>();
            return t && t->has(tag);
        },
        "sendEvent",
        [](const LuaEntity& e, const std::string& name, sol::optional<sol::object> payload) {
            e.rt->sendEvent(need(e), name, payload ? *payload : sol::object(sol::lua_nil));
        },
        "script", [](const LuaEntity& e) -> sol::object {
            sol::table t = e.rt->self(need(e));
            return t.valid() ? sol::object(t) : sol::object(sol::lua_nil);
        },
        sol::meta_function::equal_to, [](const LuaEntity& a, const LuaEntity& b) { return a == b; },
        sol::meta_function::to_string,
        [](const LuaEntity& e) { return e.valid() ? "Entity(" + e.entity().name() + ")" : std::string("Entity(invalid)"); });

    // ---- API tables ----------------------------------------------------------------------------------------
    rt.vm().bindApi("scene", [R](sol::state_view, sol::table& api) {
        api["find"] = [R](const std::string& nameOrId, sol::this_state ts) -> sol::object {
            World* w = R->world();
            if (!w) return sol::lua_nil;
            Entity e = w->findByName(nameOrId);
            if (!e.valid()) {
                if (auto id = Uuid::parse(nameOrId)) e = w->find(*id);
            }
            return makeEntityObject(ts, *R, e);
        };
        api["findAll"] = [R](const std::string& component, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table out = lv.create_table();
            World* w = R->world();
            const ComponentInfo* info = findComponent(component);
            if (!w || !info) return out;
            int i = 1;
            w->forEachInHierarchy([&](entt::entity e) {
                if (info->has(*w, e) && !w->registry().all_of<PendingDestroyTag>(e)) out[i++] = wrap(*R, w->wrap(e));
            });
            return out;
        };
        api["create"] = [R](sol::optional<std::string> name, sol::optional<LuaEntity> parent, sol::this_state ts) -> sol::object {
            World* w = R->world();
            if (!w) return sol::lua_nil;
            const Entity p = parent ? parent->entity() : Entity{};
            return makeEntityObject(ts, *R, w->create(name.value_or("Entity"), p));
        };
        api["spawn"] = [R](const std::string& prefab, sol::optional<glm::vec3> pos, sol::optional<glm::quat> rot,
                           sol::this_state ts) -> sol::object {
            const glm::vec3* p = pos ? &*pos : nullptr;
            const glm::quat* q = rot ? &*rot : nullptr;
            return makeEntityObject(ts, *R, R->spawnPrefab(prefab, p, q));
        };
#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
        // await(scene.nextFrame()) / await(scene.delay(seconds)): scheduler-driven futures.
        api["nextFrame"] = [R]() -> sol::object {
            if (!R->bridge() || !R->coroutines()) return sol::lua_nil;
            auto promise = std::make_shared<Promise<void>>();
            auto future = promise->future();
            R->coroutines()->spawn([promise]() -> Task<> {
                co_await nextFrame();
                promise->setValue();
            }, SpawnOptions{.name = "lua.nextFrame"});
            return R->bridge()->wrap(future);
        };
        api["delay"] = [R](f64 seconds) -> sol::object {
            if (!R->bridge() || !R->coroutines()) return sol::lua_nil;
            auto promise = std::make_shared<Promise<void>>();
            auto future = promise->future();
            R->coroutines()->spawn([promise, seconds]() -> Task<> {
                co_await ox::seconds(seconds);
                promise->setValue();
            }, SpawnOptions{.name = "lua.delay"});
            return R->bridge()->wrap(future);
        };
#endif
        api["destroy"] = [](sol::object e) {
            if (e.is<LuaEntity>() && e.as<LuaEntity>().valid()) e.as<LuaEntity>().entity().destroy();
        };
    });

    rt.vm().bindApi("physics", [R](sol::state_view, sol::table& api) {
        api["raycast"] = [R](const glm::vec3& origin, const glm::vec3& dir, sol::optional<f32> maxDistance,
                             sol::optional<LuaEntity> ignore, sol::this_state ts) -> sol::object {
            if (!R->physics()) return sol::lua_nil;
            auto hit = R->physics()->raycast(origin, dir, maxDistance.value_or(1000.f), ignore ? ignore->entity() : Entity{});
            if (!hit) return sol::lua_nil;
            sol::state_view lv(ts);
            sol::table t = lv.create_table();
            if (hit->entity.valid()) t["entity"] = wrap(*R, hit->entity);
            t["point"] = hit->point;
            t["normal"] = hit->normal;
            t["distance"] = hit->distance;
            return t;
        };
        api["overlapSphere"] = [R](const glm::vec3& c, f32 r, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table out = lv.create_table();
            if (!R->physics()) return out;
            int i = 1;
            for (const Entity& e : R->physics()->overlapSphere(c, r)) out[i++] = wrap(*R, e);
            return out;
        };
#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
        // Future resolved after the frame's fixed steps: local hit = await(physics.raycastAsync(o, d, 50))
        api["raycastAsync"] = [R](const glm::vec3& origin, const glm::vec3& dir, sol::optional<f32> maxDistance) -> sol::object {
            if (!R->bridge()) return sol::lua_nil;
            return R->bridge()->wrap(R->raycastAsync(origin, dir, maxDistance.value_or(1000.f)));
        };
#endif
        api["gravity"] = [R]() { return R->physics() ? R->physics()->physicsWorld().gravity() : glm::vec3(0.f); };
        api["setGravity"] = [R](const glm::vec3& g) { if (R->physics()) R->physics()->physicsWorld().setGravity(g); };
    });

    rt.vm().bindApi("audio", [R](sol::state_view, sol::table& api) {
        api["play"] = [R](const std::string& clip, sol::optional<glm::vec3> pos, sol::optional<f32> volume) -> sol::optional<lua_Integer> {
            if (!R->audio()) return sol::nullopt;
            const audio::SoundHandle h = R->audio()->playOneShot(clip, pos ? &*pos : nullptr, volume.value_or(1.f));
            if (!h) return sol::nullopt;
            return packSound(h);
        };
        api["stop"] = [R](lua_Integer id, sol::optional<f32> fade) {
            if (R->audio() && R->audio()->engine()) R->audio()->engine()->stop(unpackSound(id), fade.value_or(0.f));
        };
        api["isPlaying"] = [R](lua_Integer id) {
            return R->audio() && R->audio()->engine() && R->audio()->engine()->isPlaying(unpackSound(id));
        };
        api["playSource"] = [R](const LuaEntity& e) { return R->audio() && R->audio()->play(need(e)); };
        api["stopSource"] = [R](const LuaEntity& e, sol::optional<f32> fade) {
            if (R->audio()) R->audio()->stop(need(e), fade.value_or(0.f));
        };
    });

    rt.vm().bindApi("ai", [R](sol::state_view, sol::table& api) {
        api["findPath"] = [R](const glm::vec3& a, const glm::vec3& b, sol::this_state ts) {
            sol::state_view lv(ts);
            sol::table out = lv.create_table();
            if (!R->ai()) return out;
            int i = 1;
            for (const auto& p : R->ai()->findPath(a, b)) out[i++] = p;
            return out;
        };
        api["moveTo"] = [R](const LuaEntity& e, const glm::vec3& p) { return R->ai() && R->ai()->moveTo(need(e), p); };
#if defined(OX_GAMEPLAY_SCRIPT_ASYNC)
        // Future<bool>: true when the agent arrived, false when it was stopped/redirected. Owned by the agent
        // entity: destroying it cancels the wait (the await raises BrokenPromise).
        api["moveToAsync"] = [R](const LuaEntity& e, const glm::vec3& p) -> sol::object {
            if (!R->bridge() || !R->coroutines() || !R->ai()) return sol::lua_nil;
            const Entity en = need(e);
            if (!R->ai()->moveTo(en, p)) return R->bridge()->wrap(makeReadyFuture(false));
            auto promise = std::make_shared<Promise<bool>>();
            auto future = promise->future();
            World* world = en.world();
            const entt::entity h = en.handle();
            R->coroutines()->spawn([promise, world, h, p]() -> Task<> {
                bool arrived = false;
                co_await until([&] {
                    if (!world->valid(h)) return true;
                    const auto* a = world->registry().try_get<NavAgentComponent>(h);
                    if (!a || !a->hasDestination || glm::distance2(a->destination, p) > 1e-6f) return true;
                    arrived = a->reached;
                    return arrived;
                });
                promise->setValue(arrived);
            }, SpawnOptions{.name = "lua.moveToAsync", .owner = toRuntimeId(h)});
            return R->bridge()->wrap(future);
        };
#endif
        api["reportNoise"] = [R](const glm::vec3& p, sol::optional<f32> loudness, sol::optional<f32> radius,
                                 sol::optional<LuaEntity> instigator, sol::optional<std::string> tag) {
            if (R->ai()) {
                R->ai()->reportNoise(p, loudness.value_or(1.f), radius.value_or(10.f),
                                     instigator ? instigator->entity() : Entity{}, tag.value_or(""));
            }
        };
        api["blackboardSet"] = [R](const LuaEntity& e, const std::string& key, sol::object value) {
            ai::Blackboard* bb = R->ai() ? R->ai()->blackboard(need(e)) : nullptr;
            if (!bb) return false;
            if (value.get_type() == sol::type::boolean) bb->set(key, value.as<bool>());
            else if (value.get_type() == sol::type::number) bb->set(key, value.as<f32>());
            else if (value.get_type() == sol::type::string) bb->set(key, value.as<std::string>());
            else if (value.is<glm::vec3>()) bb->set(key, value.as<glm::vec3>());
            else if (value.is<LuaEntity>()) bb->set(key, toBlackboardId(value.as<LuaEntity>().entity()));
            else if (value.get_type() == sol::type::lua_nil) bb->erase(key);
            else return false;
            return true;
        };
    });
}

} // namespace ox::gameplay::lua
