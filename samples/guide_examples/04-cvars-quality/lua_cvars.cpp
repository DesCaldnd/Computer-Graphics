// Глава 04: доступ к cvar'ам из Lua через собственный API-модуль (docs/guide/04-cvars-quality.md).
#include <oxwald/core/cvar.hpp>
#include <oxwald/script/script_vm.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>

namespace {

ox::CVar<float> cvGrassDensity("fx.Grass.Density", 1.0f, "Grass instances per m2", 0.0f, 4.0f);
ox::CVar<bool> cvShowFps("ui.ShowFps", false, "Show FPS counter", ox::CVarFlags::Persist);

// В движке нет встроенного Lua-API для cvar'ов — пробрасываем реестр сами (один раз при старте VM).
void bindCVarApi(ox::script::ScriptVM& vm) {
    vm.bindApi("cvar", [](sol::state_view, sol::table& api) {
        api["get"] = [](const std::string& name) -> std::optional<std::string> {
            const ox::ICVar* cv = ox::CVarRegistry::instance().find(name);
            return cv ? std::optional<std::string>(cv->toString()) : std::nullopt;
        };
        api["set"] = [](const std::string& name, const std::string& value) {
            return ox::CVarRegistry::instance().set(name, value, ox::CVarSource::Console); // права как у консоли
        };
        api["exec"] = [](const std::string& line) {
            auto r = ox::CVarRegistry::instance().execute(line);
            return r ? *r : r.error().message;
        };
    });
}

} // namespace

TEST(GuideCVarsLua, ScriptReadsAndWritesCVars) {
    ox::script::ScriptVM vm;
    bindCVarApi(vm);
    sol::environment env = vm.createEnvironment();
    auto result = vm.runFile(std::string(OX_GUIDE_DIR) + "/cvars_example.lua", &env);
    ASSERT_TRUE(result) << result.error;

    EXPECT_TRUE(cvShowFps.get());
    EXPECT_FLOAT_EQ(cvGrassDensity.get(), 0.5f);
    EXPECT_EQ(env["densityBefore"].get<std::string>(), "1");
    EXPECT_FALSE(env["unknownSet"].get<bool>());
    cvShowFps.reset();
    cvGrassDensity.reset();
}
