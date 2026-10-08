# Vulkan runtime deployment (loader, MoltenVK ICD, validation layer) for executables that create a device.
#
#   ox_deploy_vulkan_runtime(<target>)   copies the runtime into $<TARGET_FILE_DIR>/vulkan/ after each build
#   ox_add_gpu_test_env(<target>)        alias used by test executables
#
# The rhi library looks for <exe dir>/vulkan/ first (packaged builds) and falls back to the absolute
# vcpkg_installed paths baked in at configure time (dev builds). Modules configured before engine/rhi
# (alphabetically, e.g. engine/render) must include this file explicitly:
#   include(${OX_ENGINE_SOURCE_DIR}/engine/rhi/cmake/OxwaldVulkanRuntime.cmake)
include_guard(GLOBAL)

set(_ox_vk_prefix "")
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(_ox_vk_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

if(APPLE)
    set(_ox_vk_loader_name libvulkan.1.dylib)
    set(_ox_vk_layer_name libVkLayer_khronos_validation.dylib)
elseif(WIN32)
    set(_ox_vk_loader_name vulkan-1.dll)
    set(_ox_vk_layer_name VkLayer_khronos_validation.dll)
else()
    set(_ox_vk_loader_name libvulkan.so.1)
    set(_ox_vk_layer_name libVkLayer_khronos_validation.so)
endif()

set(OX_VK_RUNTIME_LOADER "" CACHE INTERNAL "")
set(OX_VK_RUNTIME_MOLTENVK "" CACHE INTERNAL "")
set(OX_VK_RUNTIME_LAYER_LIB "" CACHE INTERNAL "")
set(OX_VK_RUNTIME_LAYER_JSON "" CACHE INTERNAL "")
if(_ox_vk_prefix)
    foreach(_dir lib bin)
        if(NOT OX_VK_RUNTIME_LOADER AND EXISTS "${_ox_vk_prefix}/${_dir}/${_ox_vk_loader_name}")
            set(OX_VK_RUNTIME_LOADER "${_ox_vk_prefix}/${_dir}/${_ox_vk_loader_name}" CACHE INTERNAL "")
        endif()
        if(NOT OX_VK_RUNTIME_LAYER_LIB AND EXISTS "${_ox_vk_prefix}/${_dir}/${_ox_vk_layer_name}")
            set(OX_VK_RUNTIME_LAYER_LIB "${_ox_vk_prefix}/${_dir}/${_ox_vk_layer_name}" CACHE INTERNAL "")
        endif()
    endforeach()
    if(APPLE AND EXISTS "${_ox_vk_prefix}/lib/libMoltenVK.dylib")
        set(OX_VK_RUNTIME_MOLTENVK "${_ox_vk_prefix}/lib/libMoltenVK.dylib" CACHE INTERNAL "")
    endif()
    foreach(_json "${_ox_vk_prefix}/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"
                  "${_ox_vk_prefix}/bin/VkLayer_khronos_validation.json")
        if(NOT OX_VK_RUNTIME_LAYER_JSON AND EXISTS "${_json}")
            set(OX_VK_RUNTIME_LAYER_JSON "${_json}" CACHE INTERNAL "")
        endif()
    endforeach()
endif()

set(OX_VK_RUNTIME_LOADER_NAME "${_ox_vk_loader_name}" CACHE INTERNAL "")
set(OX_VK_RUNTIME_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/DeployVulkanRuntime.cmake" CACHE INTERNAL "")

function(_ox_vulkan_runtime_args out)
    set(${out}
        -DOX_LOADER=${OX_VK_RUNTIME_LOADER}
        -DOX_LOADER_NAME=${OX_VK_RUNTIME_LOADER_NAME}
        -DOX_MOLTENVK=${OX_VK_RUNTIME_MOLTENVK}
        -DOX_LAYER_LIB=${OX_VK_RUNTIME_LAYER_LIB}
        -DOX_LAYER_JSON=${OX_VK_RUNTIME_LAYER_JSON}
        PARENT_SCOPE)
endfunction()

# Deploys into an arbitrary directory at configure time (used for the dev fallback directory).
function(ox_deploy_vulkan_runtime_to dir)
    _ox_vulkan_runtime_args(_args)
    execute_process(COMMAND ${CMAKE_COMMAND} ${_args} -DOX_DEST=${dir} -P ${OX_VK_RUNTIME_SCRIPT}
                    RESULT_VARIABLE _res)
    if(NOT _res EQUAL 0)
        message(WARNING "Vulkan runtime deployment to ${dir} failed")
    endif()
endfunction()

function(ox_deploy_vulkan_runtime target)
    _ox_vulkan_runtime_args(_args)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} ${_args} -DOX_DEST=$<TARGET_FILE_DIR:${target}>/vulkan -P ${OX_VK_RUNTIME_SCRIPT}
        COMMENT "Deploying Vulkan runtime next to ${target}"
        VERBATIM)
    ox_deploy_dlss_runtime(${target})
endfunction()

# NVIDIA NGX/DLSS runtime libraries (Windows/Linux, set by engine/render when the DLSS backend is enabled) are
# loaded by NGX from the executable's directory. No-op elsewhere.
function(ox_deploy_dlss_runtime target)
    if(NOT OX_DLSS_RUNTIME_FILES)
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OX_DLSS_RUNTIME_FILES} $<TARGET_FILE_DIR:${target}>
        COMMENT "Deploying NVIDIA NGX (DLSS) runtime next to ${target}"
        VERBATIM)
endfunction()

function(ox_add_gpu_test_env target)
    ox_deploy_vulkan_runtime(${target})
endfunction()
