#include <oxwald/core/reflect.hpp>

#include <charconv>
#include <cstdlib>
#include <cxxabi.h>
#include <new>

namespace ox::reflect {

struct TypeRegistry::Impl {
    mutable std::recursive_mutex mutex;
    std::unordered_map<const void*, std::unique_ptr<TypeInfo>> byKey;
    std::vector<TypeInfo*> ordered;
    std::unordered_map<std::string, TypeInfo*> byName;
    std::unordered_map<u64, TypeInfo*> byId;
};

TypeRegistry::TypeRegistry() : m_impl(std::make_unique<Impl>()) {}
TypeRegistry::~TypeRegistry() = default;

TypeRegistry& TypeRegistry::instance() {
    // Intentionally leaked: TypeInfo pointers are cached in function statics that may be used during exit.
    static TypeRegistry* registry = new TypeRegistry();
    return *registry;
}

std::recursive_mutex& TypeRegistry::mutex() { return m_impl->mutex; }

TypeInfo* TypeRegistry::findByKey(const void* key) const {
    auto it = m_impl->byKey.find(key);
    return it == m_impl->byKey.end() ? nullptr : it->second.get();
}

TypeInfo& TypeRegistry::insert(const void* key, std::unique_ptr<TypeInfo> info) {
    TypeInfo* raw = info.get();
    m_impl->byKey.emplace(key, std::move(info));
    m_impl->ordered.push_back(raw);
    return *raw;
}

void TypeRegistry::setName(TypeInfo& info, std::string_view name) {
    std::lock_guard lock(m_impl->mutex);
    if (!info.name.empty()) {
        auto it = m_impl->byName.find(info.name);
        if (it != m_impl->byName.end() && it->second == &info) m_impl->byName.erase(it);
        auto jt = m_impl->byId.find(info.id);
        if (jt != m_impl->byId.end() && jt->second == &info) m_impl->byId.erase(jt);
    }
    info.name = std::string(name);
    info.id = fnv1a64(name);
    auto [it, inserted] = m_impl->byName.emplace(info.name, &info);
    // Explicit registrations win over built-in aliases / placeholders sharing the same name.
    if (!inserted && (info.kind == Kind::Struct || info.kind == Kind::Enum || info.kind == Kind::Custom)) {
        it->second = &info;
    }
    m_impl->byId.try_emplace(info.id, &info);
}

const TypeInfo* TypeRegistry::find(std::string_view name) const {
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->byName.find(std::string(name));
    return it == m_impl->byName.end() ? nullptr : it->second;
}

const TypeInfo* TypeRegistry::findById(u64 id) const {
    std::lock_guard lock(m_impl->mutex);
    auto it = m_impl->byId.find(id);
    return it == m_impl->byId.end() ? nullptr : it->second;
}

std::vector<const TypeInfo*> TypeRegistry::all() const {
    std::lock_guard lock(m_impl->mutex);
    return {m_impl->ordered.begin(), m_impl->ordered.end()};
}

std::vector<const TypeInfo*> TypeRegistry::registeredStructs() const {
    std::lock_guard lock(m_impl->mutex);
    std::vector<const TypeInfo*> out;
    for (auto* t : m_impl->ordered) {
        if (t->kind == Kind::Struct && t->registered) out.push_back(t);
    }
    return out;
}

namespace detail {
std::string defaultTypeName(const std::type_info& ti) {
    int status = 0;
    char* demangled = abi::__cxa_demangle(ti.name(), nullptr, nullptr, &status);
    std::string name = (status == 0 && demangled) ? demangled : ti.name();
    std::free(demangled);
    return name;
}
} // namespace detail

// ---- TypeInfo ----------------------------------------------------------------------------------------------

const FieldInfo* TypeInfo::findField(std::string_view fieldName) const {
    for (const auto& f : fields) {
        if (f.name == fieldName) return &f;
    }
    return nullptr;
}

const FieldInfo* TypeInfo::findFieldOrFormer(std::string_view fieldName) const {
    if (const auto* f = findField(fieldName)) return f;
    for (const auto& f : fields) {
        for (const auto& former : f.attributes.formerNames) {
            if (former == fieldName) return &f;
        }
    }
    return nullptr;
}

const EnumEntry* TypeInfo::findEnum(std::string_view entryName) const {
    for (const auto& e : enumEntries) {
        if (e.name == entryName) return &e;
    }
    for (const auto& e : enumEntries) {
        for (const auto& former : e.attributes.formerNames) {
            if (former == entryName) return &e;
        }
    }
    return nullptr;
}

const EnumEntry* TypeInfo::findEnumByValue(i64 value) const {
    for (const auto& e : enumEntries) {
        if (e.value == value) return &e;
    }
    return nullptr;
}

const EnumEntry* TypeInfo::findEnumByHash(u64 nameHash) const {
    for (const auto& e : enumEntries) {
        if (e.nameHash == nameHash) return &e;
    }
    for (const auto& e : enumEntries) {
        for (const auto& former : e.attributes.formerNames) {
            if (fnv1a64(former) == nameHash) return &e;
        }
    }
    return nullptr;
}

bool TypeInfo::isDerivedFrom(const TypeInfo& other) const {
    for (const TypeInfo* t = this; t; t = t->base) {
        if (t == &other) return true;
    }
    return false;
}

void* TypeInfo::create() const {
    OX_ASSERT(construct != nullptr, "type {} is not default constructible", name);
    void* p = ::operator new(size, std::align_val_t(align));
    construct(p);
    return p;
}

void TypeInfo::destroy(void* object) const {
    if (!object) return;
    destruct(object);
    ::operator delete(object, std::align_val_t(align));
}

// ---- ValueRef ----------------------------------------------------------------------------------------------

namespace {

int componentIndex(std::string_view name) {
    if (name.size() != 1) return -1;
    switch (name[0]) {
    case 'x':
    case 'r': return 0;
    case 'y':
    case 'g': return 1;
    case 'z':
    case 'b': return 2;
    case 'w':
    case 'a': return 3;
    default: return -1;
    }
}

int componentCount(serial::Tag tag) {
    switch (tag) {
    case serial::Tag::Vec2:
    case serial::Tag::IVec2: return 2;
    case serial::Tag::Vec3:
    case serial::Tag::IVec3: return 3;
    case serial::Tag::Vec4:
    case serial::Tag::IVec4:
    case serial::Tag::Quat: return 4;
    default: return 0;
    }
}

std::optional<usize> parseIndex(std::string_view s) {
    usize v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size() || s.empty()) return std::nullopt;
    return v;
}

} // namespace

ValueRef ValueRef::child(std::string_view name) const {
    if (!valid()) return {};
    switch (type->kind) {
    case Kind::Struct: {
        const FieldInfo* f = type->findFieldOrFormer(name);
        return f ? ValueRef{f->get(ptr), f->type} : ValueRef{};
    }
    case Kind::Math: {
        const int n = componentCount(type->tag);
        const int c = componentIndex(name);
        if (c < 0 || c >= n) return {};
        const bool isInt = type->tag == serial::Tag::IVec2 || type->tag == serial::Tag::IVec3 ||
                           type->tag == serial::Tag::IVec4;
        if (type->tag == serial::Tag::Quat) {
            auto* q = static_cast<glm::quat*>(ptr);
            f32* comps[4] = {&q->x, &q->y, &q->z, &q->w};
            return {comps[c], &typeOf<f32>()};
        }
        if (isInt) return {static_cast<i32*>(ptr) + c, &typeOf<i32>()};
        return {static_cast<f32*>(ptr) + c, &typeOf<f32>()};
    }
    case Kind::Array: {
        if (auto i = parseIndex(name)) return index(*i);
        return {};
    }
    case Kind::Map: {
        void* v = type->mapFind(ptr, name);
        return v ? ValueRef{v, type->element} : ValueRef{};
    }
    case Kind::Optional: {
        if (name != "value") return {};
        void* v = type->optionalGet(ptr);
        return v ? ValueRef{v, type->element} : ValueRef{};
    }
    default: return {};
    }
}

ValueRef ValueRef::index(usize i) const {
    if (!valid() || type->kind != Kind::Array || i >= type->containerSize(ptr)) return {};
    return {type->arrayAt(ptr, i), type->element};
}

serial::Value ValueRef::get() const {
    if (!valid()) return {};
    return serial::toValue(ptr, *type);
}

bool ValueRef::set(const serial::Value& value) const {
    if (!valid()) return false;
    return serial::fromValue(value, ptr, *type);
}

ValueRef resolvePath(ValueRef root, std::string_view path) {
    ValueRef cur = root;
    usize i = 0;
    while (cur.valid() && i < path.size()) {
        if (path[i] == '.') {
            ++i;
            continue;
        }
        if (path[i] == '[') {
            const usize close = path.find(']', i);
            if (close == std::string_view::npos) return {};
            const auto inner = path.substr(i + 1, close - i - 1);
            if (cur.type->kind == Kind::Map) {
                std::string_view key = inner;
                if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'')) key = key.substr(1, key.size() - 2);
                cur = cur.child(key);
            } else {
                auto idx = parseIndex(inner);
                if (!idx) return {};
                cur = cur.index(*idx);
            }
            i = close + 1;
            continue;
        }
        usize end = i;
        while (end < path.size() && path[end] != '.' && path[end] != '[') ++end;
        cur = cur.child(path.substr(i, end - i));
        i = end;
    }
    return cur;
}

} // namespace ox::reflect
