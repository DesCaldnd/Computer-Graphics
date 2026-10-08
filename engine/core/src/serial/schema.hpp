#pragma once

#include <oxwald/core/serial/value.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace ox::serial::detail {

// Builds the string table and the type schema table for a value tree. Shared by the binary and the JSON
// encoders so both produce identical schemas for identical trees (needed for byte-identical round trips).
class SchemaBuilder {
public:
    struct TypeEntry {
        std::string name;
        std::vector<std::pair<std::string, DescPtr>> fields;
        std::unordered_map<std::string, u32> slots;
    };

    void collect(const Document& doc) {
        str(doc.root.desc()->str());
        collectValue(doc.root);
    }

    u32 str(const std::string& s) {
        auto [it, inserted] = m_strIndex.emplace(s, static_cast<u32>(strings.size()));
        if (inserted) strings.push_back(s);
        return it->second;
    }

    [[nodiscard]] u32 typeOf(const Value& object) const { return objectType.at(&object); }

    // JSON schema key for a type entry: the type name, "Name#2" for a second incompatible shape, "@N" anonymous.
    [[nodiscard]] std::vector<std::string> schemaKeys() const {
        std::vector<std::string> keys;
        std::unordered_map<std::string, u32> seen;
        u32 anonymous = 0;
        for (const auto& t : types) {
            if (t.name.empty()) {
                keys.push_back("@" + std::to_string(++anonymous));
                continue;
            }
            const u32 n = ++seen[t.name];
            keys.push_back(n == 1 ? t.name : t.name + "#" + std::to_string(n));
        }
        return keys;
    }

    std::vector<std::string> strings;
    std::vector<TypeEntry> types;
    std::unordered_map<const Value*, u32> objectType;

private:
    void collectValue(const Value& v) {
        switch (v.tag()) {
        case Tag::Object:
            objectType.emplace(&v, typeFor(v));
            for (const auto& [k, child] : v.fields()) collectValue(child);
            break;
        case Tag::Map:
            for (const auto& [k, child] : v.fields()) collectValue(child);
            break;
        case Tag::Array:
        case Tag::Optional:
            for (const auto& child : v.items()) collectValue(child);
            break;
        case Tag::Enum:
            if (!v.getString().empty()) str(v.getString());
            break;
        default: break;
        }
    }

    u32 typeFor(const Value& object) {
        auto& candidates = m_byName[object.typeName()];
        for (u32 idx : candidates) {
            TypeEntry& t = types[idx];
            bool compatible = true;
            for (const auto& [k, child] : object.fields()) {
                auto it = t.slots.find(k);
                if (it != t.slots.end() && !sameDesc(t.fields[it->second].second, child.desc())) {
                    compatible = false;
                    break;
                }
            }
            if (compatible) {
                addFields(t, object);
                return idx;
            }
        }
        const u32 idx = static_cast<u32>(types.size());
        types.push_back(TypeEntry{object.typeName(), {}, {}});
        str(object.typeName());
        candidates.push_back(idx);
        addFields(types.back(), object);
        return idx;
    }

    void addFields(TypeEntry& t, const Value& object) {
        for (const auto& [k, child] : object.fields()) {
            if (t.slots.contains(k)) continue;
            auto desc = child.desc();
            str(k);
            str(desc->str());
            t.slots.emplace(k, static_cast<u32>(t.fields.size()));
            t.fields.emplace_back(k, std::move(desc));
        }
    }

    std::unordered_map<std::string, u32> m_strIndex;
    std::unordered_map<std::string, std::vector<u32>> m_byName;
};

} // namespace ox::serial::detail
