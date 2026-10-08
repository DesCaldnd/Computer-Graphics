#include "script_test_utils.hpp"

#include <glm/gtc/quaternion.hpp>

using namespace ox;
using namespace ox::script;
using namespace ox::script::test;

// ---------------------------------------------------------------- math bindings

TEST(ScriptMath, VectorOperatorsAndMethods) {
    ScriptVM vm;
    EXPECT_EQ(eval<glm::vec3>(vm, "return vec3(1,2,3) + vec3(4,5,6)"), glm::vec3(5, 7, 9));
    EXPECT_EQ(eval<glm::vec3>(vm, "return vec3(1,2,3) * 2"), glm::vec3(2, 4, 6));
    EXPECT_EQ(eval<glm::vec3>(vm, "return 2 * vec3(1,2,3)"), glm::vec3(2, 4, 6));
    EXPECT_EQ(eval<glm::vec3>(vm, "return -vec3(1,2,3) / 2"), glm::vec3(-0.5f, -1, -1.5f));
    EXPECT_EQ(eval<glm::vec3>(vm, "return vec3(1,0,0):cross(vec3(0,1,0))"), glm::vec3(0, 0, 1));
    EXPECT_FLOAT_EQ(eval<f32>(vm, "return vec3(1,2,3):dot(vec3(4,5,6))"), 32.f);
    EXPECT_FLOAT_EQ(eval<f32>(vm, "return vec3(3,4,0):length()"), 5.f);
    EXPECT_FLOAT_EQ(eval<f32>(vm, "return vec3(0,3,4):normalize():length()"), 1.f);
    EXPECT_EQ(eval<glm::vec3>(vm, "return vec3(0):normalize()"), glm::vec3(0)); // no NaN
    EXPECT_EQ(eval<glm::vec3>(vm, "return vec3.lerp(vec3(0), vec3(10, 20, 30), 0.5)"), glm::vec3(5, 10, 15));
    EXPECT_FLOAT_EQ(eval<f32>(vm, "return vec2(1, 1):distance(vec2(4, 5))"), 5.f);
    EXPECT_EQ(eval<glm::vec4>(vm, "return vec4(vec3(1,2,3), 1) * vec4(2)"), glm::vec4(2, 4, 6, 2));
    EXPECT_TRUE(eval<bool>(vm, "local v = vec3(1,2,3); v.y = 7; return v == vec3(1,7,3)"));
    EXPECT_EQ(eval<std::string>(vm, "return tostring(vec2(1, 2.5))"), "vec2(1, 2.5)");
    EXPECT_TRUE(eval<bool>(vm, "local a = vec3(1); local b = a:clone(); b.x = 5; return a.x == 1"));
    EXPECT_EQ(eval<glm::vec3>(vm, "return vec3.forward"), glm::vec3(0, 0, -1));
}

TEST(ScriptMath, QuaternionsAndMatrices) {
    ScriptVM vm;
    const glm::vec3 r = eval<glm::vec3>(vm, "return quat.angleAxis(math.pi / 2, vec3.up) * vec3(1, 0, 0)");
    EXPECT_NEAR(r.x, 0.f, 1e-6f);
    EXPECT_NEAR(r.z, -1.f, 1e-6f);

    const glm::vec3 e = eval<glm::vec3>(vm, "return quat.fromEuler(vec3(0.1, 0.2, 0.3)):toEuler()");
    EXPECT_NEAR(e.x, 0.1f, 1e-5f);
    EXPECT_NEAR(e.y, 0.2f, 1e-5f);
    EXPECT_NEAR(e.z, 0.3f, 1e-5f);

    EXPECT_NEAR(eval<f32>(vm, "return quat.slerp(quat(), quat.angleAxis(2, vec3(0, 0, 1)), 0.25):angle()"), 0.5f,
                1e-5f);
    EXPECT_NEAR(eval<f32>(vm, "local q = quat.angleAxis(1, vec3(1, 1, 0)); return (q * q:inverse()):angle()"), 0.f,
                1e-3f);
    EXPECT_NEAR(eval<f32>(vm, "return quat(2, 0, 0, 0):normalize():length()"), 1.f, 1e-6f);
    EXPECT_NEAR(eval<f32>(vm, "return quat.identity:dot(quat())"), 1.f, 1e-6f);
    const glm::quat cq = eval<glm::quat>(vm, "return quat.angleAxis(0.7, vec3.up)");
    EXPECT_NEAR(glm::angle(cq), 0.7f, 1e-5f);

    const glm::vec3 p = eval<glm::vec3>(
        vm, "return mat4.trs(vec3(10, 0, 0), quat.angleAxis(math.pi, vec3.up), vec3(2)):transformPoint(vec3(1, 0, 0))");
    EXPECT_NEAR(p.x, 8.f, 1e-5f);
    EXPECT_NEAR(p.z, 0.f, 1e-5f);
    EXPECT_TRUE(eval<bool>(vm, R"(
        local m = mat4.translation(vec3(1, 2, 3)) * mat4.scaling(vec3(2))
        local back = m:inverse() * m
        return back == mat4.identity() and m:getTranslation() == vec3(1, 2, 3) and m:get(0, 0) == 2
    )"));
}

TEST(ScriptMath, ScalarHelpers) {
    ScriptVM vm;
    EXPECT_EQ(eval<f64>(vm, "return math.clamp(5, 0, 3)"), 3.0);
    EXPECT_EQ(eval<f64>(vm, "return math.lerp(10, 20, 0.25)"), 12.5);
    EXPECT_EQ(eval<f64>(vm, "return math.remap(5, 0, 10, 100, 200)"), 150.0);
    EXPECT_EQ(eval<f64>(vm, "return math.smoothstep(0, 1, 0.5)"), 0.5);
    EXPECT_EQ(eval<i64>(vm, "return math.sign(-3)"), -1);
}

// ---------------------------------------------------------------- sandbox

TEST(ScriptSandbox, DangerousFunctionsAreUnavailable) {
    ScriptVM vm;
    EXPECT_TRUE(eval<bool>(vm, "return os.execute == nil and os.exit == nil and os.remove == nil and os.getenv == nil"));
    EXPECT_TRUE(eval<bool>(vm, "return io == nil and dofile == nil and loadfile == nil and load == nil"));
    EXPECT_TRUE(eval<bool>(vm, "return debug == nil and package == nil and collectgarbage == nil"));
    EXPECT_TRUE(eval<bool>(vm, "return string.dump == nil and ('').dump == nil"));
    EXPECT_TRUE(eval<bool>(vm, "return getmetatable('') == false and getmetatable(vec3()) == nil"));
    EXPECT_TRUE(eval<bool>(vm, "return type(os.time()) == 'number' and type(os.clock()) == 'number'"));

    LogCapture logs;
    sol::environment env = vm.createEnvironment();
    EXPECT_FALSE(vm.runString("io.open('/tmp/x', 'w')", "io_test", &env).ok);
    EXPECT_FALSE(vm.runString("os.execute('echo pwned')", "os_test", &env).ok);
    EXPECT_TRUE(logs.contains("attempt to call a nil value (field 'execute')"));
}

TEST(ScriptSandbox, SharedLibrariesAreReadOnlyAndGlobalsIsolated) {
    ScriptVM vm;
    sol::environment a = vm.createEnvironment();
    sol::environment b = vm.createEnvironment();
    EXPECT_FALSE(vm.runString("math.sin = nil", "a", &a).ok);
    EXPECT_FALSE(vm.runString("vec3.up = 1", "a", &a).ok);
    EXPECT_TRUE(vm.runString("rawset(math, 'sin', nil); shared = 1", "a", &a).ok); // only its own proxy
    ScriptResult r = vm.runString("return math.sin(0) == 0 and shared == nil", "b", &b);
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.value.as<bool>());
    EXPECT_TRUE(eval<bool>(vm, "local n = 0; for k in pairs(math) do n = n + 1 end; return n > 20"));
}

TEST(ScriptSandbox, IoCanBeEnabledExplicitly) {
    ScriptVMConfig cfg;
    cfg.allowIo = true;
    ScriptVM vm(cfg);
    EXPECT_TRUE(eval<bool>(vm, "return type(io.open) == 'function'"));
}

// ---------------------------------------------------------------- errors & limits

TEST(ScriptErrors, RuntimeErrorsAreLoggedWithLineAndTraceback) {
    ScriptVM vm;
    LogCapture logs;
    sol::environment env = vm.createEnvironment();
    ScriptResult r = vm.runString("local t = nil\n\nlocal function f()\n  return t.field\nend\nreturn f()",
                                  "scripts/broken.lua", &env);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("scripts/broken.lua:4:"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("stack traceback"), std::string::npos) << r.error;
    EXPECT_TRUE(logs.contains("scripts/broken.lua:4:"));
    EXPECT_EQ(vm.errorCount(), 1u);

    ScriptResult s = vm.runString("local x = = 1", "scripts/syntax.lua", &env);
    EXPECT_FALSE(s.ok);
    EXPECT_NE(s.error.find("scripts/syntax.lua:1:"), std::string::npos) << s.error;

    // error() with a table value still yields a message.
    EXPECT_FALSE(vm.runString("error({code = 1})", "t", &env).ok);
    EXPECT_TRUE(vm.runString("return 1", "after", &env).ok); // VM still healthy
}

TEST(ScriptErrors, InstructionLimitAbortsRunawayScripts) {
    ScriptVMConfig cfg;
    cfg.instructionLimit = 200'000;
    ScriptVM vm(cfg);
    sol::environment env = vm.createEnvironment();
    ScriptResult r = vm.runString("while true do end", "loop.lua", &env);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("instruction limit"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("loop.lua:1:"), std::string::npos) << r.error;

    // Scripts cannot swallow the abort with pcall or coroutines.
    EXPECT_FALSE(vm.runString("while true do pcall(function() while true do end end) end", "p", &env).ok);
    EXPECT_FALSE(vm.runString("while true do xpcall(function() while true do end end, print) end", "x", &env).ok);
    EXPECT_FALSE(vm.runString(
                       "while true do coroutine.resume(coroutine.create(function() while true do end end)) end", "c",
                       &env)
                     .ok);

    // The budget is per call: normal work afterwards is fine.
    ScriptResult ok = vm.runString("local s = 0; for i = 1, 1000 do s = s + i end; return s", "ok", &env);
    ASSERT_TRUE(ok.ok) << ok.error;
    EXPECT_EQ(ok.value.as<i64>(), 500500);
}

TEST(ScriptErrors, MemoryLimitIsEnforced) {
    ScriptVMConfig cfg;
    cfg.memoryLimit = 8u << 20;
    cfg.instructionLimit = 0;
    ScriptVM vm(cfg);
    sol::environment env = vm.createEnvironment();
    ScriptResult r = vm.runString("local t = {} for i = 1, 1e8 do t[i] = ('x'):rep(64) .. i end", "hog", &env);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("memory"), std::string::npos) << r.error;
    EXPECT_LE(vm.memoryUsed(), cfg.memoryLimit);
    vm.lua().collect_garbage();
    EXPECT_TRUE(vm.runString("return 1", "after", &env).ok);
}

// ---------------------------------------------------------------- modules & extension APIs

TEST(ScriptModules, RequireUsesSearchRoots) {
    ScriptVMConfig cfg;
    cfg.searchRoots = {OX_TEST_DATA_DIR};
    ScriptVM vm(cfg);
    EXPECT_EQ(eval<f64>(vm, "return require('util.mathx').round2(1.23456)"), 1.23);
    EXPECT_TRUE(eval<bool>(vm, "return require('util.mathx') == require('util.mathx')")); // cached
    sol::environment env = vm.createEnvironment();
    ScriptResult r = vm.runString("require('../../etc/passwd')", "r", &env);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("not found"), std::string::npos) << r.error;
}

TEST(ScriptApi, BindApiExtensionPoint) {
    ScriptVM vm;
    int calls = 0;
    vm.bindApi("physics", [&](sol::state_view, sol::table& api) {
        api["raycast"] = [&](glm::vec3 from, glm::vec3 dir, f32 maxDist) -> sol::optional<glm::vec3> {
            ++calls;
            if (dir.y < 0.f) return from + dir * std::min(maxDist, from.y);
            return sol::nullopt;
        };
        api["gravity"] = -9.81;
    });
    EXPECT_TRUE(vm.hasApi("physics"));
    const glm::vec3 hit = eval<glm::vec3>(vm, "return physics.raycast(vec3(0, 5, 0), vec3(0, -1, 0), 100)");
    EXPECT_EQ(hit, glm::vec3(0.f));
    EXPECT_TRUE(eval<bool>(vm, "return physics.raycast(vec3(0), vec3(0, 1, 0), 10) == nil"));
    EXPECT_EQ(calls, 2);
    sol::environment env = vm.createEnvironment();
    EXPECT_FALSE(vm.runString("physics.gravity = 0", "x", &env).ok);
}
