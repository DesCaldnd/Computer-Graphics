#include "inspector/component_extensions.hpp"

#include <map>

namespace ox::editor {

namespace {
std::map<std::string, ComponentExtension>& registry() {
    static std::map<std::string, ComponentExtension> r;
    return r;
}
} // namespace

void ComponentExtensions::add(const std::string& component, ComponentExtension ext) { registry()[component] = std::move(ext); }

const ComponentExtension* ComponentExtensions::find(const std::string& component) {
    auto it = registry().find(component);
    return it == registry().end() ? nullptr : &it->second;
}

} // namespace ox::editor
