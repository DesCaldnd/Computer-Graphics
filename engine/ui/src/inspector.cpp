#include "inspector.hpp"

#include <oxwald/core/math.hpp>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <glm/gtc/quaternion.hpp>

#include <string>

namespace ox::ui {

namespace {

ImGuiDataType intDataType(const reflect::TypeInfo& t) {
    const bool sign = t.kind == reflect::Kind::Int;
    switch (t.size) {
    case 1: return sign ? ImGuiDataType_S8 : ImGuiDataType_U8;
    case 2: return sign ? ImGuiDataType_S16 : ImGuiDataType_U16;
    case 4: return sign ? ImGuiDataType_S32 : ImGuiDataType_U32;
    default: return sign ? ImGuiDataType_S64 : ImGuiDataType_U64;
    }
}

bool scalar(const char* label, ImGuiDataType dt, void* data, const reflect::Attributes& a, f32 speed) {
    if (a.rangeMin && a.rangeMax) {
        if (dt == ImGuiDataType_Float) {
            f32 lo = f32(*a.rangeMin), hi = f32(*a.rangeMax);
            return ImGui::SliderScalar(label, dt, data, &lo, &hi);
        }
        if (dt == ImGuiDataType_Double) {
            f64 lo = *a.rangeMin, hi = *a.rangeMax;
            return ImGui::SliderScalar(label, dt, data, &lo, &hi);
        }
        i64 lo = i64(*a.rangeMin), hi = i64(*a.rangeMax);
        if (dt == ImGuiDataType_S64 || dt == ImGuiDataType_U64) return ImGui::DragScalar(label, dt, data, speed, &lo, &hi);
        int l32 = int(lo), h32 = int(hi);
        return ImGui::DragScalar(label, dt, data, speed, &l32, &h32);
    }
    return ImGui::DragScalar(label, dt, data, a.step ? f32(*a.step) : speed);
}

} // namespace

bool inspectStruct(const reflect::TypeInfo& type, void* data, int depth) {
    bool changed = false;
    for (const reflect::FieldInfo& f : type.fields) {
        if (f.attributes.hidden || !f.type) continue;
        ImGui::PushID(f.name.c_str());
        const std::string label(f.displayName());
        changed |= inspectValue(label.c_str(), *f.type, f.get(data), f.attributes, depth + 1);
        if (!f.attributes.tooltip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.attributes.tooltip.c_str());
        ImGui::PopID();
    }
    return changed;
}

bool inspectValue(const char* label, const reflect::TypeInfo& type, void* data, const reflect::Attributes& a, int depth) {
    using reflect::Kind;
    if (depth > 12) {
        ImGui::TextDisabled("%s: ...", label);
        return false;
    }
    bool changed = false;
    ImGui::BeginDisabled(a.readOnly);
    switch (type.kind) {
    case Kind::Bool: changed = ImGui::Checkbox(label, static_cast<bool*>(data)); break;
    case Kind::Int:
    case Kind::UInt: changed = scalar(label, intDataType(type), data, a, 0.2f); break;
    case Kind::Float: changed = scalar(label, type.size == 8 ? ImGuiDataType_Double : ImGuiDataType_Float, data, a, 0.01f); break;
    case Kind::String: changed = ImGui::InputText(label, static_cast<std::string*>(data)); break;
    case Kind::Uuid: ImGui::Text("%s: %s", label, static_cast<Uuid*>(data)->toString().c_str()); break;
    case Kind::Enum: {
        const i64 cur = type.getEnum(data);
        const reflect::EnumEntry* e = type.findEnumByValue(cur);
        if (ImGui::BeginCombo(label, e ? e->name.c_str() : std::to_string(cur).c_str())) {
            for (const reflect::EnumEntry& entry : type.enumEntries) {
                if (ImGui::Selectable(entry.name.c_str(), entry.value == cur)) {
                    type.setEnum(data, entry.value);
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case Kind::Math: {
        using serial::Tag;
        const f32 speed = a.step ? f32(*a.step) : 0.01f;
        switch (type.tag) {
        case Tag::Vec2: changed = ImGui::DragFloat2(label, static_cast<f32*>(data), speed); break;
        case Tag::Vec3:
            changed = a.color ? ImGui::ColorEdit3(label, static_cast<f32*>(data), a.hdr ? ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float : 0)
                              : ImGui::DragFloat3(label, static_cast<f32*>(data), speed);
            break;
        case Tag::Vec4:
            changed = a.color ? ImGui::ColorEdit4(label, static_cast<f32*>(data), a.hdr ? ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float : 0)
                              : ImGui::DragFloat4(label, static_cast<f32*>(data), speed);
            break;
        case Tag::IVec2: changed = ImGui::DragInt2(label, static_cast<int*>(data)); break;
        case Tag::IVec3: changed = ImGui::DragInt3(label, static_cast<int*>(data)); break;
        case Tag::IVec4: changed = ImGui::DragInt4(label, static_cast<int*>(data)); break;
        case Tag::Quat: {
            // Euler degrees only as an editing convenience; the value stays a quaternion.
            auto* q = static_cast<glm::quat*>(data);
            glm::vec3 euler = glm::degrees(glm::eulerAngles(*q));
            if (ImGui::DragFloat3(label, &euler.x, 0.5f)) {
                *q = glm::normalize(glm::quat(glm::radians(euler)));
                changed = true;
            }
            break;
        }
        case Tag::Mat4: {
            if (ImGui::TreeNode(label)) {
                auto* m = static_cast<glm::mat4*>(data);
                for (int r = 0; r < 4; ++r) ImGui::Text("%8.3f %8.3f %8.3f %8.3f", (*m)[0][r], (*m)[1][r], (*m)[2][r], (*m)[3][r]);
                ImGui::TreePop();
            }
            break;
        }
        default: ImGui::TextDisabled("%s", label); break;
        }
        break;
    }
    case Kind::Struct:
        if (ImGui::TreeNodeEx(label, depth <= 1 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            changed = inspectStruct(type, data, depth);
            ImGui::TreePop();
        }
        break;
    case Kind::Array: {
        const usize n = type.containerSize(data);
        if (ImGui::TreeNode(label, "%s [%zu]", label, n)) {
            if (type.arrayResize && !a.readOnly) {
                if (ImGui::SmallButton("+")) {
                    type.arrayResize(data, n + 1);
                    changed = true;
                }
                ImGui::SameLine();
                if (n > 0 && ImGui::SmallButton("-")) {
                    type.arrayResize(data, n - 1);
                    changed = true;
                }
            }
            const usize count = type.containerSize(data);
            for (usize i = 0; i < count && i < 256; ++i) {
                ImGui::PushID(int(i));
                const std::string l = "[" + std::to_string(i) + "]";
                changed |= inspectValue(l.c_str(), *type.element, type.arrayAt(data, i), {}, depth + 1);
                ImGui::PopID();
            }
            if (count > 256) ImGui::TextDisabled("... %zu more", count - 256);
            ImGui::TreePop();
        }
        break;
    }
    case Kind::Optional: {
        bool has = type.optionalHas(data);
        if (ImGui::Checkbox(("##has" + std::string(label)).c_str(), &has)) {
            if (has) type.optionalEmplace(data);
            else type.containerClear(data);
            changed = true;
        }
        ImGui::SameLine();
        if (type.optionalHas(data)) changed |= inspectValue(label, *type.element, type.optionalGet(data), a, depth + 1);
        else ImGui::TextDisabled("%s (none)", label);
        break;
    }
    case Kind::Map: {
        if (ImGui::TreeNode(label)) {
            std::vector<std::string> keys;
            type.mapForEach(data, [&](std::string_view k, const void*) { keys.emplace_back(k); });
            for (const std::string& k : keys) {
                ImGui::PushID(k.c_str());
                changed |= inspectValue(k.c_str(), *type.element, type.mapFind(data, k), {}, depth + 1);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        break;
    }
    default: {
        if (type.customToValue) ImGui::TextDisabled("%s: <%s>", label, type.name.c_str());
        else ImGui::TextDisabled("%s", label);
        break;
    }
    }
    ImGui::EndDisabled();
    return changed;
}

} // namespace ox::ui
