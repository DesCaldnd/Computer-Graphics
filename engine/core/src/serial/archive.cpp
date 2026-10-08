#include <oxwald/core/serial/archive.hpp>

namespace ox::serial {

Writer::Writer(std::string kind, u32 version) {
    m_doc.kind = std::move(kind);
    m_doc.version = version;
}

Value& Writer::add(std::string_view key, Value v) {
    Value& parent = current();
    switch (parent.tag()) {
    case Tag::Object:
    case Tag::Map:
        OX_ASSERT(key != "$type", "'$type' is a reserved archive key");
        return parent.set(key, std::move(v));
    case Tag::Array:
        parent.push(std::move(v));
        OX_ASSERT(!parent.isPacked(), "cannot enter an element of a packed array");
        return parent.items().back();
    default: OX_UNREACHABLE();
    }
}

void Writer::beginObject(std::string_view key, std::string_view typeName) {
    Value& child = add(key, Value::makeObject(std::string(typeName)));
    m_stack.push_back(&child);
}

void Writer::endObject() {
    OX_ASSERT(!m_stack.empty() && current().isObject(), "endObject without beginObject");
    m_stack.pop_back();
}

void Writer::beginArray(std::string_view key, DescPtr elem) {
    Value& child = add(key, Value::makeArray(std::move(elem)));
    m_stack.push_back(&child);
}

void Writer::endArray() {
    OX_ASSERT(!m_stack.empty() && current().isArray(), "endArray without beginArray");
    m_stack.pop_back();
}

void Writer::beginMap(std::string_view key, DescPtr elem) {
    Value& child = add(key, Value::makeMap(std::move(elem)));
    m_stack.push_back(&child);
}

void Writer::endMap() {
    OX_ASSERT(!m_stack.empty() && current().isMap(), "endMap without beginMap");
    m_stack.pop_back();
}

void Writer::blob(std::string_view key, std::span<const std::byte> bytes) {
    raw(key, Value::makeRawArray(TypeDesc::of(Tag::U8), std::vector<std::byte>(bytes.begin(), bytes.end())));
}

void Writer::raw(std::string_view key, Value v) {
    Value& parent = current();
    if (parent.isArray()) {
        parent.push(std::move(v));
    } else {
        add(key, std::move(v));
    }
}

Document Writer::finish() && {
    OX_ASSERT(m_stack.empty(), "unbalanced begin/end in archive writer");
    return std::move(m_doc);
}

Reader::Reader(Document doc) : m_doc(std::move(doc)) {}

Result<Reader> Reader::fromBytes(std::span<const std::byte> data) {
    auto doc = decodeAny(data);
    if (!doc) return doc.error();
    return Reader(std::move(*doc));
}

Result<Reader> Reader::load(const std::filesystem::path& path) {
    auto doc = loadDocument(path);
    if (!doc) return doc.error();
    return Reader(std::move(*doc));
}

const Value* Reader::get(std::string_view key) const {
    const Value& cur = current();
    if (!cur.isObject() && !cur.isMap()) return nullptr;
    return cur.find(key);
}

std::vector<std::string> Reader::keys() const {
    std::vector<std::string> out;
    for (const auto& [k, v] : current().fields()) out.push_back(k);
    return out;
}

bool Reader::beginObject(std::string_view key) {
    const Value* v = get(key);
    if (!v || (!v->isObject() && !v->isMap())) return false;
    m_stack.push_back(v);
    return true;
}

bool Reader::beginObjectAt(usize index) {
    const Value& arr = current();
    if (!arr.isArray() || arr.isPacked() || index >= arr.size()) return false;
    const Value& v = arr.items()[index];
    if (!v.isObject() && !v.isMap()) return false;
    m_stack.push_back(&v);
    return true;
}

void Reader::endObject() {
    OX_ASSERT(!m_stack.empty(), "endObject without beginObject");
    m_stack.pop_back();
}

std::optional<usize> Reader::beginArray(std::string_view key) {
    const Value* v = get(key);
    if (!v || !v->isArray()) return std::nullopt;
    m_stack.push_back(v);
    return v->size();
}

void Reader::endArray() {
    OX_ASSERT(!m_stack.empty(), "endArray without beginArray");
    m_stack.pop_back();
}

std::optional<std::vector<std::byte>> Reader::blob(std::string_view key) const {
    const Value* v = get(key);
    if (!v || !v->isArray()) return std::nullopt;
    if (v->isPacked() && v->elemDesc()->tag == Tag::U8) {
        return std::vector<std::byte>(v->packedBytes().begin(), v->packedBytes().end());
    }
    std::vector<std::byte> out;
    for (usize i = 0; i < v->size(); ++i) out.push_back(std::byte(v->at(i).getUInt()));
    return out;
}

} // namespace ox::serial
