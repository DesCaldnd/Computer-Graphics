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

# The config mapping above only selects the library file. Jolt's exported compile definitions are still keyed on
# the consumer's configuration name, e.g. with MSVC
#   $<$<CONFIG:Debug,Release>:JPH_FLOATING_POINT_EXCEPTIONS_ENABLED>
# so a RelWithDebInfo build would link the Release Jolt.lib without that define and fail VerifyJoltVersionID().
# Give RelWithDebInfo/MinSizeRel every definition the Release configuration gets.
#
# Release-only triplets (vcpkg-overlays/triplets/{x64,arm64}-android.cmake: VCPKG_BUILD_TYPE release) install no debug
# libJolt.a. A Debug consumer (tools/android/build.ps1 -NativeConfig Debug) then links the release library, so it must
# also be compiled with the Release definitions (NDEBUG changes JPH_IF_DEBUG members of Jolt classes): map Debug to
# Release as well. With a debug Jolt available (x64-windows) Debug keeps its own library and definitions.
set(_ox_jolt_release_like "Release,RelWithDebInfo,MinSizeRel")
get_target_property(_ox_jolt_configs Jolt::Jolt IMPORTED_CONFIGURATIONS)
if(NOT "DEBUG" IN_LIST _ox_jolt_configs)
    set_target_properties(Jolt::Jolt PROPERTIES MAP_IMPORTED_CONFIG_DEBUG "Release")
    string(APPEND _ox_jolt_release_like ",Debug")
    message(STATUS "Oxwald: Jolt has no Debug configuration: Debug builds use the Release libJolt and its definitions")
endif()
get_target_property(_ox_jolt_defs Jolt::Jolt INTERFACE_COMPILE_DEFINITIONS)
if(_ox_jolt_defs AND NOT _ox_jolt_defs MATCHES "RelWithDebInfo")
    string(REGEX REPLACE "([:,])Release([,>])" "\\1${_ox_jolt_release_like}\\2" _ox_jolt_defs "${_ox_jolt_defs}")
    set_property(TARGET Jolt::Jolt PROPERTY INTERFACE_COMPILE_DEFINITIONS "${_ox_jolt_defs}")
endif()
