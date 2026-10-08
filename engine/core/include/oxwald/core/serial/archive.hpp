#pragma once

#include <oxwald/core/reflect.hpp>
#include <oxwald/core/serial/convert.hpp>
#include <oxwald/core/serial/format.hpp>
#include <oxwald/core/serial/value.hpp>

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

// Archive front-end. Writers build a Document (value tree) that is then encoded as binary or JSON; readers
// navigate a decoded Document. Reflected types go through serial::toValue/fromValue; custom sections use the
// manual begin/end API:
//
//   serial::Writer w("save", 3);
//   w.value("player", playerComponent);          // any reflected type / built-in
//   w.beginObject("quests");
//   w.value("active", activeQuestIds);
//   w.endObject();
//   w.blob("thumbnail", pngBytes);
//   w.save("slot1.oxsave");
//
//   auto r = serial::Reader::load("slot1.oxsave");
//   r->value("player", playerComponent);         // missing => unchanged
//   if (r->beginObject("quests")) { r->value("active", ids); r->endObject(); }
namespace ox::serial {

class Writer {
public:
    explicit Writer(std::string kind = {}, u32 version = 0);

    // Inside an object: `key` names the new member. Inside an array: `key` is ignored and the value is appended.
    void beginObject(std::string_view key, std::string_view typeName = {});
    void endObject();
    void beginArray(std::string_view key, DescPtr elem = nullptr);
    void endArray();
    void beginMap(std::string_view key, DescPtr elem = nullptr);
    void endMap();

    template <class T>
    void value(std::string_view key, const T& v, const ConvertOptions& options = {}) {
        raw(key, toValue(&v, reflect::typeOf<T>(), options));
    }
    template <class T>
    void element(const T& v) {
        value({}, v);
    }
    void object(std::string_view key, const void* object, const reflect::TypeInfo& type,
                const ConvertOptions& options = {}) {
        raw(key, toValue(object, type, options));
    }
    void blob(std::string_view key, std::span<const std::byte> bytes);
    void raw(std::string_view key, Value v);

    [[nodiscard]] Value& current() { return m_stack.empty() ? m_doc.root : *m_stack.back(); }
    [[nodiscard]] const Document& document() const { return m_doc; }
    [[nodiscard]] Document finish() &&;

    [[nodiscard]] std::vector<std::byte> toBinary() const { return encodeBinary(m_doc); }
    [[nodiscard]] std::string toJson(int indent = 2) const { return toJsonString(m_doc, indent); }
    // Format chosen from the extension (".json" => JSON).
    Status save(const std::filesystem::path& path) const { return saveDocument(path, m_doc, formatForPath(path)); }
    Status save(const std::filesystem::path& path, Format format) const { return saveDocument(path, m_doc, format); }

private:
    Value& add(std::string_view key, Value v);

    Document m_doc;
    std::vector<Value*> m_stack; // excludes the root so the writer stays movable
};

class Reader {
public:
    explicit Reader(Document doc);
    [[nodiscard]] static Result<Reader> fromBytes(std::span<const std::byte> data);
    [[nodiscard]] static Result<Reader> load(const std::filesystem::path& path);

    [[nodiscard]] const Document& document() const { return m_doc; }
    [[nodiscard]] u32 version() const { return m_doc.version; }
    [[nodiscard]] const std::string& kind() const { return m_doc.kind; }

    [[nodiscard]] const Value& current() const { return m_stack.empty() ? m_doc.root : *m_stack.back(); }
    [[nodiscard]] const Value* get(std::string_view key) const;
    [[nodiscard]] bool has(std::string_view key) const { return get(key) != nullptr; }
    [[nodiscard]] std::vector<std::string> keys() const;

    // Enter a child object/map by key (or array element by index); false (and no push) when absent.
    bool beginObject(std::string_view key);
    bool beginObjectAt(usize index);
    void endObject();
    // Element count, or nullopt (nothing entered) when absent. Elements: value(index, out) / beginObjectAt.
    std::optional<usize> beginArray(std::string_view key);
    void endArray();

    template <class T>
    bool value(std::string_view key, T& out, const ConvertOptions& options = {}) const {
        const Value* v = get(key);
        return v && fromValue(*v, &out, reflect::typeOf<T>(), options);
    }
    template <class T>
    bool value(usize index, T& out, const ConvertOptions& options = {}) const {
        const Value& arr = current();
        if (!arr.isArray() || index >= arr.size()) return false;
        if (arr.isPacked()) return fromValue(arr.at(index), &out, reflect::typeOf<T>(), options);
        return fromValue(arr.items()[index], &out, reflect::typeOf<T>(), options);
    }
    bool object(std::string_view key, void* object, const reflect::TypeInfo& type,
                const ConvertOptions& options = {}) const {
        const Value* v = get(key);
        return v && fromValue(*v, object, type, options);
    }
    [[nodiscard]] std::optional<std::vector<std::byte>> blob(std::string_view key) const;

private:
    Document m_doc;
    std::vector<const Value*> m_stack; // excludes the root so the reader stays movable
};

} // namespace ox::serial
