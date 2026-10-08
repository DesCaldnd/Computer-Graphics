#include <oxwald/ai/blackboard.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace ox::ai {

const char* blackboardTypeName(const BlackboardValue& v) {
    switch (v.index()) {
    case 0: return "none";
    case 1: return "bool";
    case 2: return "int";
    case 3: return "float";
    case 4: return "string";
    case 5: return "vec3";
    case 6: return "id";
    default: return "?";
    }
}

void toJson(nlohmann::json& j, const BlackboardValue& v) {
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                j = nullptr;
            } else if constexpr (std::is_same_v<T, glm::vec3>) {
                j = nlohmann::json::array({x.x, x.y, x.z});
            } else if constexpr (std::is_same_v<T, u64>) {
                j = nlohmann::json{{"id", x}};
            } else {
                j = x;
            }
        },
        v);
}

BlackboardValue blackboardValueFromJson(const nlohmann::json& j) {
    if (j.is_boolean()) {
        return j.get<bool>();
    }
    if (j.is_number_integer()) {
        return static_cast<i32>(j.get<i64>());
    }
    if (j.is_number_float()) {
        return j.get<f32>();
    }
    if (j.is_string()) {
        return j.get<std::string>();
    }
    if (j.is_array() && j.size() == 3) {
        return glm::vec3(j[0].get<f32>(), j[1].get<f32>(), j[2].get<f32>());
    }
    if (j.is_object() && j.contains("id")) {
        return j["id"].get<u64>();
    }
    return std::monostate{};
}

void Blackboard::setValue(const std::string& key, BlackboardValue value) {
    auto it = m_values.find(key);
    if (it != m_values.end()) {
        if (it->second == value) {
            return;
        }
        BlackboardValue old = std::move(it->second);
        it->second = value;
        ++m_version;
        notify(key, old, value);
        return;
    }
    m_values.emplace(key, value);
    ++m_version;
    notify(key, BlackboardValue{}, value);
}

const BlackboardValue* Blackboard::find(const std::string& key) const {
    auto it = m_values.find(key);
    return it != m_values.end() ? &it->second : nullptr;
}

bool Blackboard::has(const std::string& key) const {
    const BlackboardValue* v = find(key);
    return v != nullptr && !std::holds_alternative<std::monostate>(*v);
}

bool Blackboard::erase(const std::string& key) {
    auto it = m_values.find(key);
    if (it == m_values.end()) {
        return false;
    }
    BlackboardValue old = std::move(it->second);
    m_values.erase(it);
    ++m_version;
    notify(key, old, BlackboardValue{});
    return true;
}

void Blackboard::clear() {
    std::vector<std::string> keys;
    for (const auto& [k, v] : m_values) {
        keys.push_back(k);
    }
    for (const auto& k : keys) {
        erase(k);
    }
}

Blackboard::ObserverId Blackboard::observe(const std::string& key, Observer observer) {
    const ObserverId id = m_nextObserver++;
    m_observers.push_back({id, key, std::move(observer)});
    return id;
}

void Blackboard::unobserve(ObserverId id) {
    std::erase_if(m_observers, [&](const Entry& e) { return e.id == id; });
}

void Blackboard::notify(const std::string& key, const BlackboardValue& oldValue, const BlackboardValue& newValue) {
    // Copy: observers may (un)register observers or write other keys.
    const auto observers = m_observers;
    for (const Entry& e : observers) {
        if (e.key.empty() || e.key == key) {
            e.fn(key, oldValue, newValue);
        }
    }
}

void Blackboard::saveJson(nlohmann::json& j) const {
    j = nlohmann::json::object();
    for (const auto& [k, v] : m_values) {
        toJson(j[k], v);
    }
}

void Blackboard::loadJson(const nlohmann::json& j) {
    for (auto it = j.begin(); it != j.end(); ++it) {
        setValue(it.key(), blackboardValueFromJson(it.value()));
    }
}

} // namespace ox::ai
