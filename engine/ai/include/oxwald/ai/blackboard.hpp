#pragma once

#include <oxwald/core/types.hpp>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ox::ai {

// Supported blackboard value types. u64 doubles as an entity/object id.
using BlackboardValue = std::variant<std::monostate, bool, i32, f32, std::string, glm::vec3, u64>;

[[nodiscard]] const char* blackboardTypeName(const BlackboardValue& v);
void toJson(nlohmann::json& j, const BlackboardValue& v);
[[nodiscard]] BlackboardValue blackboardValueFromJson(const nlohmann::json& j);

// Typed key/value store shared by a behaviour tree and game code. Observers fire synchronously when a
// value actually changes (set to a different value, or erased).
class Blackboard {
public:
    using ObserverId = u32;
    using Observer = std::function<void(const std::string& key, const BlackboardValue& oldValue, const BlackboardValue& newValue)>;

    template <class T>
    void set(const std::string& key, T value) {
        if constexpr (std::is_same_v<T, bool>) {
            setValue(key, BlackboardValue{value});
        } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            setValue(key, BlackboardValue{static_cast<i32>(value)});
        } else if constexpr (std::is_integral_v<T>) {
            setValue(key, BlackboardValue{static_cast<u64>(value)});
        } else if constexpr (std::is_floating_point_v<T>) {
            setValue(key, BlackboardValue{static_cast<f32>(value)});
        } else if constexpr (std::is_convertible_v<T, std::string>) {
            setValue(key, BlackboardValue{std::string(value)});
        } else {
            setValue(key, BlackboardValue{std::move(value)});
        }
    }
    void setValue(const std::string& key, BlackboardValue value);

    template <class T>
    [[nodiscard]] std::optional<T> get(const std::string& key) const {
        auto it = m_values.find(key);
        if (it == m_values.end()) {
            return std::nullopt;
        }
        if (const T* v = std::get_if<T>(&it->second)) {
            return *v;
        }
        return std::nullopt;
    }
    template <class T>
    [[nodiscard]] T getOr(const std::string& key, T fallback) const {
        return get<T>(key).value_or(std::move(fallback));
    }
    [[nodiscard]] const BlackboardValue* find(const std::string& key) const;
    [[nodiscard]] bool has(const std::string& key) const;
    bool erase(const std::string& key);
    void clear();
    [[nodiscard]] const std::unordered_map<std::string, BlackboardValue>& values() const { return m_values; }

    // key empty → observe every key.
    ObserverId observe(const std::string& key, Observer observer);
    void unobserve(ObserverId id);

    // Bumped on every change; cheap dirty check for polling systems.
    [[nodiscard]] u64 version() const { return m_version; }

    void saveJson(nlohmann::json& j) const;
    void loadJson(const nlohmann::json& j);

private:
    void notify(const std::string& key, const BlackboardValue& oldValue, const BlackboardValue& newValue);

    std::unordered_map<std::string, BlackboardValue> m_values;
    struct Entry {
        ObserverId id;
        std::string key;
        Observer fn;
    };
    std::vector<Entry> m_observers;
    ObserverId m_nextObserver = 1;
    u64 m_version = 0;
};

} // namespace ox::ai
