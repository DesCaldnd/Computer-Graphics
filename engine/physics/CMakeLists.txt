find_package(Jolt CONFIG REQUIRED)

# Jolt is a private dependency: public headers expose only glm/core types, so the Jolt
# compile definitions (JPH_OBJECT_STREAM, NDEBUG mapping, ...) never leak into consumers.
ox_add_module(physics
    PUBLIC_DEPS Oxwald::core
    PRIVATE_DEPS Jolt::Jolt)

# The vcpkg Jolt port is built without the `rtti` feature (-fno-rtti, no typeinfo symbols in
# libJolt.a). Classes deriving from Jolt types with out-of-line key functions would otherwise
# reference missing typeinfo, so the module's own sources are compiled without RTTI as well.
if(NOT MSVC)
    target_compile_options(ox_physics PRIVATE -fno-rtti)
else()
    target_compile_options(ox_physics PRIVATE /GR-)
endif()

# Make RelWithDebInfo/MinSizeRel pick the release libJolt.a so the JPH_VERSION_ID feature bits
# (asserts on/off) match the library; VerifyJoltVersionID() checks this at start-up.
set_target_properties(Jolt::Jolt PROPERTIES
    MAP_IMPORTED_CONFIG_RELWITHDEBINFO "Release"
    MAP_IMPORTED_CONFIG_MINSIZEREL "Release")
