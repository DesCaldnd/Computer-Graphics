// Lua bindings: `ui` (RmlUi game UI: documents, data models, events) and `debug` (immediate-mode ImGui subset).
#include <oxwald/ui/ui_system.hpp>

#include "lua_state.hpp"

#include <cfloat>
#include <tuple>

#if OX_UI_HAS_SCRIPT

#include <oxwald/core/log.hpp>
#include <oxwald/script/script_vm.hpp>

#include <RmlUi/Core.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <cmath>
#include <map>

namespace ox::ui {

namespace {

Rml::Variant toVariant(const sol::object& o) {
    switch (o.get_type()) {
    case sol::type::boolean: return Rml::Variant(o.as<bool>());
    case sol::type::number: {
        const f64 d = o.as<f64>();
        if (std::floor(d) == d && std::abs(d) < 2147483647.0) return Rml::Variant(int(d));
        return Rml::Variant(d);
    }
    case sol::type::string: return Rml::Variant(o.as<std::string>());
    default: return Rml::Variant(o.is<glm::vec4>() ? Rml::String("<vec4>") : Rml::String());
    }
}

sol::object fromVariant(sol::state_view lua, const Rml::Variant& v) {
    switch (v.GetType()) {
    case Rml::Variant::BOOL: return sol::make_object(lua, v.Get<bool>());
    case Rml::Variant::BYTE:
    case Rml::Variant::INT:
    case Rml::Variant::INT64:
    case Rml::Variant::UINT:
    case Rml::Variant::UINT64: return sol::make_object(lua, v.Get<i64>());
    case Rml::Variant::FLOAT:
    case Rml::Variant::DOUBLE: {
        // Data expressions evaluate numbers as doubles; give Lua integers back when the value is integral.
        const f64 d = v.Get<f64>();
        if (std::floor(d) == d && std::abs(d) < 9.0e15) return sol::make_object(lua, i64(d));
        return sol::make_object(lua, d);
    }
    case Rml::Variant::NONE: return sol::make_object(lua, sol::lua_nil);
    default: return sol::make_object(lua, v.Get<Rml::String>());
    }
}

std::string joinArgs(const sol::variadic_args& args) {
    std::string out;
    for (const sol::object& a : args) {
        if (!out.empty()) out += ' ';
        switch (a.get_type()) {
        case sol::type::string: out += a.as<std::string>(); break;
        case sol::type::number: {
            const f64 d = a.as<f64>();
            out += std::floor(d) == d && std::abs(d) < 1e15 ? std::to_string(i64(d)) : std::format("{:.4g}", d);
            break;
        }
        case sol::type::boolean: out += a.as<bool>() ? "true" : "false"; break;
        case sol::type::lua_nil: out += "nil"; break;
        default: {
            sol::state_view lua(a.lua_state());
            sol::protected_function ts = lua["tostring"];
            auto r = ts(a);
            out += r.valid() ? r.get<std::string>() : std::string("?");
        }
        }
    }
    return out;
}

} // namespace


void closeDanglingLuaWindows(UiSystem::LuaState* state) {
    if (!state) return;
    while (state->openWindows > 0) {
        ImGui::End();
        --state->openWindows;
    }
}

void destroyLuaState(std::unique_ptr<UiSystem::LuaState>& state) {
    if (!state) return;
    if (GameUI* g = state->ui->gameUI()) {
        for (u64 id : state->listeners) g->removeEventListener(id);
        for (const auto& [name, m] : state->models) g->removeModel(name);
    }
    state.reset();
}

void bindUiLua(script::ScriptVM& vm, UiSystem& ui, std::unique_ptr<UiSystem::LuaState>& state) {
    destroyLuaState(state);
    state = std::make_unique<UiSystem::LuaState>();
    state->vm = &vm;
    state->ui = &ui;
    UiSystem::LuaState* S = state.get();

    // ------------------------------------------------------------------------------------------------ ui
    vm.lua().new_usertype<LuaModel>(
        "OxUiModel", sol::no_constructor,
        "name", sol::readonly(&LuaModel::name),
        "set", [](LuaModel& m, const std::string& key, const sol::object& value) {
            m.values[key] = toVariant(value);
            if (m.handle) m.handle.DirtyVariable(key);
        },
        "get", [](LuaModel& m, const std::string& key, sol::this_state ts) -> sol::object {
            auto it = m.values.find(key);
            return it == m.values.end() ? sol::make_object(ts, sol::lua_nil) : fromVariant(ts, it->second);
        },
        "on", [S](LuaModel& m, const std::string& event, sol::protected_function fn) {
            const bool known = m.events.contains(event);
            m.events[event] = std::move(fn);
            if (!known && m.constructor) {
                LuaModel* mp = &m;
                m.constructor.BindEventCallback(event, [S, mp, event](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                    auto it = mp->events.find(event);
                    if (it == mp->events.end()) return;
                    sol::state_view lua = S->vm->lua();
                    sol::table t = lua.create_table();
                    for (usize i = 0; i < args.size(); ++i) t[i + 1] = fromVariant(lua, args[i]);
                    S->vm->call(it->second, "ui model event " + event, t);
                });
            }
        });

    vm.bindApi("ui", [S](sol::state_view lua, sol::table& api) {
        api["load"] = [S](const std::string& path, sol::optional<bool> show) {
            GameUI* g = S->ui->gameUI();
            return g && g->load(path, show.value_or(false)) != nullptr;
        };
        api["show"] = [S](const std::string& name, sol::optional<bool> modal) {
            GameUI* g = S->ui->gameUI();
            return g && g->show(name, modal.value_or(false));
        };
        api["hide"] = [S](const std::string& name) {
            GameUI* g = S->ui->gameUI();
            return g && g->hide(name);
        };
        api["close"] = [S](const std::string& name) {
            GameUI* g = S->ui->gameUI();
            return g && g->close(name);
        };
        api["isVisible"] = [S](const std::string& name) {
            GameUI* g = S->ui->gameUI();
            return g && g->isVisible(name);
        };
        api["setText"] = [S](const std::string& doc, const std::string& id, const std::string& rml) {
            GameUI* g = S->ui->gameUI();
            Rml::ElementDocument* d = g ? g->document(doc) : nullptr;
            Rml::Element* e = d ? d->GetElementById(id) : nullptr;
            if (e) e->SetInnerRML(rml);
            return e != nullptr;
        };
        api["getText"] = [S](const std::string& doc, const std::string& id) -> std::string {
            GameUI* g = S->ui->gameUI();
            Rml::ElementDocument* d = g ? g->document(doc) : nullptr;
            Rml::Element* e = d ? d->GetElementById(id) : nullptr;
            return e ? e->GetInnerRML() : std::string();
        };
        api["setClass"] = [S](const std::string& doc, const std::string& id, const std::string& cls, bool on) {
            GameUI* g = S->ui->gameUI();
            Rml::ElementDocument* d = g ? g->document(doc) : nullptr;
            if (Rml::Element* e = d ? d->GetElementById(id) : nullptr) e->SetClass(cls, on);
        };
        // ui.on(document, elementId, eventType, function(event) ... end) -> listener id
        api["on"] = [S](const std::string& doc, const std::string& id, const std::string& type, sol::protected_function fn) -> i64 {
            GameUI* g = S->ui->gameUI();
            if (!g) return 0;
            const u64 lid = g->addEventListener(doc, id, type, [S, fn = std::move(fn)](Rml::Event& e) {
                sol::state_view lua = S->vm->lua();
                sol::table t = lua.create_table();
                t["type"] = e.GetType();
                Rml::Element* target = e.GetTargetElement();
                t["target"] = target ? target->GetId() : std::string();
                t["value"] = e.GetParameter<Rml::String>("value", "");
                t["x"] = e.GetParameter<f32>("mouse_x", 0.0f);
                t["y"] = e.GetParameter<f32>("mouse_y", 0.0f);
                S->vm->call(fn, "ui.on " + e.GetType(), t);
            });
            S->listeners.push_back(lid);
            return i64(lid);
        };
        api["off"] = [S](i64 id) {
            if (GameUI* g = S->ui->gameUI()) g->removeEventListener(u64(id));
            std::erase(S->listeners, u64(id));
        };
        // ui.createModel(name, { field = value, ... }, { event = function(args) end, ... }) -> model
        // Must run before loading documents that use the model (data-model="name").
        api["createModel"] = [S](const std::string& name, sol::optional<sol::table> fields, sol::optional<sol::table> events) -> LuaModel* {
            GameUI* g = S->ui->gameUI();
            if (!g) return nullptr;
            if (auto it = S->models.find(name); it != S->models.end()) return it->second.get();
            auto model = std::make_unique<LuaModel>();
            model->name = name;
            model->constructor = g->createModel(name);
            if (!model->constructor) {
                OX_LOG_ERROR("ui", "ui.createModel('{}'): a model with this name already exists", name);
                return nullptr;
            }
            LuaModel* m = model.get();
            if (fields) {
                for (const auto& [k, v] : *fields) {
                    if (k.get_type() != sol::type::string) continue;
                    const std::string key = k.as<std::string>();
                    m->values[key] = toVariant(v);
                    m->constructor.BindFunc(
                        key, [m, key](Rml::Variant& out) { out = m->values[key]; },
                        [m, key](const Rml::Variant& in) { m->values[key] = in; });
                }
            }
            if (events) {
                for (const auto& [k, v] : *events) {
                    if (k.get_type() != sol::type::string || v.get_type() != sol::type::function) continue;
                    const std::string event = k.as<std::string>();
                    m->events[event] = v.as<sol::protected_function>();
                    m->constructor.BindEventCallback(event, [S, m, event](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
                        auto it = m->events.find(event);
                        if (it == m->events.end()) return;
                        sol::state_view lua = S->vm->lua();
                        sol::table t = lua.create_table();
                        for (usize i = 0; i < args.size(); ++i) t[i + 1] = fromVariant(lua, args[i]);
                        S->vm->call(it->second, "ui model event " + event, t);
                    });
                }
            }
            m->handle = m->constructor.GetModelHandle();
            S->models[name] = std::move(model);
            return m;
        };
        api["model"] = [S](const std::string& name) -> LuaModel* {
            auto it = S->models.find(name);
            return it == S->models.end() ? nullptr : it->second.get();
        };
        api["setTranslations"] = [S](sol::table t) {
            std::map<std::string, std::string> table;
            for (const auto& [k, v] : t)
                if (k.get_type() == sol::type::string && v.get_type() == sol::type::string) table[k.as<std::string>()] = v.as<std::string>();
            if (GameUI* g = S->ui->gameUI()) g->setTranslations(std::move(table));
        };
        api["debugOverlayVisible"] = [S]() { return S->ui->imgui() && S->ui->imgui()->visible(); };
        (void)lua;
    });

    // ------------------------------------------------------------------------------------------------ debug
    vm.bindApi("debug", [S](sol::state_view lua, sol::table& api) {
        // debug.window(title, function() ... end) -> visible
        api["window"] = [S](const std::string& title, sol::protected_function fn) {
            if (!S->imguiReady()) return false;
            const bool visible = ImGui::Begin(title.c_str());
            if (visible) S->vm->call(fn, "debug.window " + title);
            ImGui::End();
            return visible;
        };
        api["beginWindow"] = [S](const std::string& title) {
            if (!S->imguiReady()) return false;
            ++S->openWindows;
            return ImGui::Begin(title.c_str());
        };
        api["endWindow"] = [S]() {
            if (!S->imguiReady() || S->openWindows <= 0) return;
            --S->openWindows;
            ImGui::End();
        };
        api["text"] = [S](sol::variadic_args args) {
            if (S->imguiReady()) ImGui::TextUnformatted(joinArgs(args).c_str());
        };
        api["button"] = [S](const std::string& label) { return S->imguiReady() && ImGui::Button(label.c_str()); };
        api["checkbox"] = [S](const std::string& label, bool value) {
            bool changed = S->imguiReady() && ImGui::Checkbox(label.c_str(), &value);
            return std::make_tuple(value, changed);
        };
        api["sliderFloat"] = [S](const std::string& label, f32 value, sol::optional<f32> lo, sol::optional<f32> hi) {
            bool changed = S->imguiReady() && ImGui::SliderFloat(label.c_str(), &value, lo.value_or(0.0f), hi.value_or(1.0f));
            return std::make_tuple(value, changed);
        };
        api["inputText"] = [S](const std::string& label, std::string text) {
            bool changed = S->imguiReady() && ImGui::InputText(label.c_str(), &text);
            return std::make_tuple(text, changed);
        };
        // Accepts and returns vec3 / vec4 (or a {r,g,b[,a]} table, returned as vec4).
        api["colorEdit"] = [S](const std::string& label, sol::object color, sol::this_state ts) {
            sol::state_view l(ts);
            if (color.is<glm::vec3>()) {
                glm::vec3 c = color.as<glm::vec3>();
                bool changed = S->imguiReady() && ImGui::ColorEdit3(label.c_str(), &c.x);
                return std::make_tuple(sol::make_object(l, c), changed);
            }
            glm::vec4 c(1.0f);
            if (color.is<glm::vec4>()) {
                c = color.as<glm::vec4>();
            } else if (color.get_type() == sol::type::table) {
                sol::table t = color;
                c = {t.get_or(1, 1.0f), t.get_or(2, 1.0f), t.get_or(3, 1.0f), t.get_or(4, 1.0f)};
            }
            bool changed = S->imguiReady() && ImGui::ColorEdit4(label.c_str(), &c.x);
            return std::make_tuple(sol::make_object(l, c), changed);
        };
        // debug.plotLines(label, {values...} [, overlay [, min [, max [, height]]]])
        api["plotLines"] = [S](const std::string& label, sol::table values, sol::optional<std::string> overlay, sol::optional<f32> lo,
                               sol::optional<f32> hi, sol::optional<f32> height) {
            if (!S->imguiReady()) return;
            std::vector<f32> v;
            v.reserve(values.size());
            for (usize i = 1; i <= values.size(); ++i) v.push_back(values.get_or(i, 0.0f));
            ImGui::PlotLines(label.c_str(), v.data(), int(v.size()), 0, overlay ? overlay->c_str() : nullptr,
                             lo.value_or(FLT_MAX), hi.value_or(FLT_MAX), ImVec2(0.0f, height.value_or(60.0f)));
        };
        api["separator"] = [S]() {
            if (S->imguiReady()) ImGui::Separator();
        };
        api["sameLine"] = [S]() {
            if (S->imguiReady()) ImGui::SameLine();
        };
        api["overlayVisible"] = [S]() { return S->ui->imgui() && S->ui->imgui()->visible(); };
        (void)lua;
    });
}

} // namespace ox::ui

#else

namespace ox::ui {} // script module not built

#endif
