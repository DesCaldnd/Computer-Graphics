find_package(assimp CONFIG REQUIRED)

ox_add_module(animation
    PUBLIC_DEPS  Oxwald::core
    PRIVATE_DEPS assimp::assimp)

if(TARGET ox_animation_tests)
    target_compile_definitions(ox_animation_tests PRIVATE
        OX_ANIMATION_SHADER_DIR="${OX_SHADER_SOURCE_DIR}")
    # The skinning shader test compiles GLSL → SPIR-V. Prefer the shaderc library (rhi uses it too);
    # fall back to a glslc/glslangValidator executable; otherwise the test skips.
    find_package(unofficial-shaderc CONFIG QUIET)
    if(TARGET unofficial::shaderc::shaderc)
        target_link_libraries(ox_animation_tests PRIVATE unofficial::shaderc::shaderc)
        target_compile_definitions(ox_animation_tests PRIVATE OX_HAS_SHADERC=1)
    else()
        find_program(OX_GLSLC_EXECUTABLE NAMES glslc glslangValidator
            HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/shaderc
                  ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang
                  $ENV{VULKAN_SDK}/bin)
        if(OX_GLSLC_EXECUTABLE)
            target_compile_definitions(ox_animation_tests PRIVATE OX_GLSLC_EXECUTABLE="${OX_GLSLC_EXECUTABLE}")
        endif()
    endif()
endif()
