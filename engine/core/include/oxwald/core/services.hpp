#pragma once

#include <oxwald/core/assert.hpp>
#include <oxwald/core/types.hpp>

#include <memory>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace ox {

namespace detail {
template <class T>
struct ServiceKey {
    static constexpr char tag = 0;
};
template <class T>
const void* serviceKey() {
    return &ServiceKey<std::remove_cvref_t<T>>::tag;
}
} // namespace detail

// Dependency-injection container: one instance per (interface) type. Services are destroyed in reverse
// registration order so later services may depend on earlier ones.
class Services {
public:
    Services() = default;
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;
    Services(Services&&) noexcept = default;
    Services& operator=(Services&& other) noexcept {
        if (this != &other) {
            clear();
            m_entries = std::move(other.m_entries);
        }
        return *this;
    }
    ~Services() { clear(); }

    // Takes ownership. Asserts if a service of this type already exists.
    template <class Interface, class Impl>
        requires std::is_base_of_v<Interface, Impl> || std::is_same_v<Interface, Impl>
    Interface& add(std::unique_ptr<Impl> instance) {
        OX_ASSERT(instance != nullptr, "null service {}", typeid(Interface).name());
        OX_ASSERT(find(detail::serviceKey<Interface>()) == nullptr, "service {} already registered",
                  typeid(Interface).name());
        Interface* raw = instance.get();
        m_entries.push_back(Entry{detail::serviceKey<Interface>(), raw, makeDeleter<Impl>(instance.release()),
                                  typeid(Interface).name()});
        return *raw;
    }

    template <class T, class... Args>
    T& emplace(Args&&... args) {
        return add<T>(std::make_unique<T>(std::forward<Args>(args)...));
    }

    // Registers a non-owned instance (lifetime managed by the caller).
    template <class Interface>
    Interface& addExternal(Interface& instance) {
        OX_ASSERT(find(detail::serviceKey<Interface>()) == nullptr, "service {} already registered",
                  typeid(Interface).name());
        m_entries.push_back(Entry{detail::serviceKey<Interface>(), &instance, {}, typeid(Interface).name()});
        return instance;
    }

    template <class T>
    [[nodiscard]] T& get() const {
        T* p = tryGet<T>();
        OX_ASSERT(p != nullptr, "service {} not registered", typeid(T).name());
        return *p;
    }

    template <class T>
    [[nodiscard]] T* tryGet() const {
        const Entry* e = find(detail::serviceKey<T>());
        return e ? static_cast<T*>(e->ptr) : nullptr;
    }

    template <class T>
    [[nodiscard]] bool has() const {
        return find(detail::serviceKey<T>()) != nullptr;
    }

    // Destroys (if owned) and unregisters. Returns false if not present.
    template <class T>
    bool remove() {
        const void* key = detail::serviceKey<T>();
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->key == key) {
                Entry e = std::move(*it);
                m_entries.erase(it);
                if (e.deleter) e.deleter(e.owned);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] usize size() const { return m_entries.size(); }

    void clear() {
        while (!m_entries.empty()) {
            Entry e = std::move(m_entries.back());
            m_entries.pop_back();
            if (e.deleter) e.deleter(e.owned);
        }
    }

private:
    struct Entry {
        const void* key;
        void* ptr;
        void (*deleter)(void*) = nullptr;
        const char* name;
        void* owned = nullptr;

        Entry(const void* k, void* p, std::pair<void (*)(void*), void*> del, const char* n)
            : key(k), ptr(p), deleter(del.first), name(n), owned(del.second) {}
    };

    template <class Impl>
    static std::pair<void (*)(void*), void*> makeDeleter(Impl* p) {
        return {[](void* q) { delete static_cast<Impl*>(q); }, p};
    }

    const Entry* find(const void* key) const {
        for (const auto& e : m_entries) {
            if (e.key == key) return &e;
        }
        return nullptr;
    }

    std::vector<Entry> m_entries;
};

} // namespace ox
