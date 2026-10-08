#include <oxwald/script/script_instance.hpp>

namespace ox::script {

ScriptInstance::ScriptInstance(ScriptVM& vm, std::shared_ptr<ScriptAsset> asset, ScriptVM::InstanceInit init)
    : m_vm(&vm), m_asset(std::move(asset)), m_init(std::move(init)), m_owner(vm.newOwnerId()) {
    vm.registerInstance(this);
}

ScriptInstance::~ScriptInstance() {
    if (!m_vm) {
        return;
    }
    destroy();
    m_vm->unregisterInstance(this);
}

void ScriptInstance::detach() {
    m_env = sol::environment();
    m_self = sol::table();
    m_vm = nullptr;
}

std::string ScriptInstance::contextFor(std::string_view function) const {
    return std::format("{}:{}", m_asset->name(), function);
}

bool ScriptInstance::loadChunk(sol::environment& env) {
    return m_vm->runString(m_asset->m_source, m_asset->m_chunkName, &env).ok;
}

void ScriptInstance::applyProperties(bool onlyMissing) {
    for (const ScriptPropertyDesc& desc : m_asset->properties()) {
        if (onlyMissing) {
            sol::object current = m_self[desc.name];
            if (current.valid() && current.get_type() != sol::type::lua_nil) {
                continue;
            }
        }
        ScriptValue value = desc.defaultValue;
        if (auto it = m_overrides.find(desc.name); it != m_overrides.end()) {
            value = coerceProperty(desc, it->second).value_or(desc.defaultValue);
        }
        m_self[desc.name] = m_vm->toLua(value);
    }
}

bool ScriptInstance::create() {
    if (!m_vm || m_state != State::Uninitialized) {
        return false;
    }
    if (!m_self.valid()) {
        m_self = m_vm->lua().create_table();
    }
    sol::environment env = m_vm->createEnvironment(m_owner);
    if (!m_asset->valid() || !loadChunk(env)) {
        ++m_errors;
        return false; // stays Uninitialized; a fixed hot reload calls create() again
    }
    m_env = std::move(env);
    m_loadedVersion = m_asset->version();
    applyProperties(false);
    if (m_init) {
        m_init(*this, m_self);
    }
    m_state = State::Created;
    invoke("onCreate");
    return true;
}

void ScriptInstance::start() {
    if (m_state != State::Created) {
        return;
    }
    m_state = State::Started;
    invoke("onStart");
}

void ScriptInstance::update(f32 dt) {
    if (m_state == State::Created) {
        start();
    }
    if (m_state == State::Started) {
        invoke("onUpdate", dt);
    }
}

void ScriptInstance::fixedUpdate(f32 dt) {
    if (m_state == State::Created) {
        start();
    }
    if (m_state == State::Started) {
        invoke("onFixedUpdate", dt);
    }
}

void ScriptInstance::destroy() {
    if (!m_vm || (m_state != State::Created && m_state != State::Started)) {
        return;
    }
    invoke("onDestroy");
    m_state = State::Destroyed;
    m_vm->releaseOwner(m_owner);
}

void ScriptInstance::sendEvent(std::string_view name, const sol::object& payload) {
    invoke("onEvent", std::string(name), payload);
}

void ScriptInstance::sendEvent(std::string_view name, const ScriptValue& payload) {
    if (m_vm) {
        sendEvent(name, m_vm->toLua(payload));
    }
}

bool ScriptInstance::setProperty(std::string_view name, const ScriptValue& value) {
    const ScriptPropertyDesc* desc = m_asset->findProperty(name);
    if (!desc) {
        return false;
    }
    auto coerced = coerceProperty(*desc, value);
    if (!coerced) {
        return false;
    }
    m_overrides[desc->name] = *coerced;
    if (m_vm && (m_state == State::Created || m_state == State::Started)) {
        m_self[desc->name] = m_vm->toLua(*coerced);
    }
    return true;
}

ScriptValue ScriptInstance::getProperty(std::string_view name) const {
    if (m_self.valid() && m_state != State::Uninitialized) {
        sol::object v = m_self[std::string(name)];
        return ScriptVM::fromLua(v);
    }
    if (auto it = m_overrides.find(std::string(name)); it != m_overrides.end()) {
        return it->second;
    }
    const ScriptPropertyDesc* desc = m_asset->findProperty(name);
    return desc ? desc->defaultValue : ScriptValue{};
}

bool ScriptInstance::reload() {
    if (!m_vm || m_state == State::Destroyed) {
        return false;
    }
    if (m_state == State::Uninitialized) {
        return create();
    }
    // Coroutines, timers and subscriptions created by the old code keep running their old closures.
    sol::environment env = m_vm->createEnvironment(m_owner);
    if (!loadChunk(env)) {
        ++m_errors;
        return false; // keep the previous code running
    }
    m_env = std::move(env);
    m_loadedVersion = m_asset->version();
    applyProperties(true);
    sol::object hook = m_env["on_reload"];
    invoke(hook.get_type() == sol::type::function ? "on_reload" : "onReload");
    return true;
}

} // namespace ox::script
