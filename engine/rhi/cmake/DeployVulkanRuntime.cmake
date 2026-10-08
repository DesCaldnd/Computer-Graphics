# cmake -DOX_DEST=<dir> -DOX_LOADER=... -DOX_LOADER_NAME=... -DOX_MOLTENVK=... -DOX_LAYER_LIB=... -DOX_LAYER_JSON=...
#       -P DeployVulkanRuntime.cmake
# Copies the Vulkan loader, the MoltenVK ICD (macOS) and the Khronos validation layer into OX_DEST and writes
# manifests whose library paths are relative to the manifest ("./lib..."), so the folder is relocatable.
cmake_minimum_required(VERSION 3.25)

file(MAKE_DIRECTORY "${OX_DEST}")

function(_ox_copy src dst)
    if(NOT src OR NOT EXISTS "${src}")
        return()
    endif()
    file(REAL_PATH "${src}" _real)
    file(TIMESTAMP "${_real}" _ts "%Y%m%d%H%M%S" UTC)
    file(SIZE "${_real}" _size)
    set(_stamp "${_real}|${_ts}|${_size}")
    if(EXISTS "${dst}" AND EXISTS "${dst}.stamp")
        file(READ "${dst}.stamp" _old)
        if(_old STREQUAL _stamp)
            return()
        endif()
    endif()
    # Copy to a new inode and rename: overwriting a loaded, signed Mach-O in place makes macOS kill the next
    # process that maps it (stale code-signature cache).
    # Several targets deploy into the same bin/vulkan directory from parallel POST_BUILD steps: use a private
    # temp name so concurrent copies never rename each other's file away.
    string(RANDOM LENGTH 8 _rnd)
    set(_tmp "${dst}.${_rnd}.tmp")
    file(COPY_FILE "${_real}" "${_tmp}")
    if(CMAKE_HOST_APPLE)
        # Safety net: a dylib whose install names were rewritten after signing (e.g. by vcpkg's Mach-O fixup)
        # has an invalid signature and macOS SIGKILLs on load. The moltenvk overlay port now keeps the release
        # signature intact, so this normally does nothing.
        execute_process(COMMAND codesign -v "${_tmp}" RESULT_VARIABLE _valid OUTPUT_QUIET ERROR_QUIET)
        if(NOT _valid EQUAL 0)
            execute_process(COMMAND codesign --force --sign - "${_tmp}" OUTPUT_QUIET ERROR_QUIET)
        endif()
    endif()
    file(RENAME "${_tmp}" "${dst}")
    file(WRITE "${dst}.stamp" "${_stamp}")
endfunction()

function(_ox_write_if_different path content)
    if(EXISTS "${path}")
        file(READ "${path}" _old)
        if(_old STREQUAL content)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

if(OX_LOADER)
    _ox_copy("${OX_LOADER}" "${OX_DEST}/${OX_LOADER_NAME}")
endif()

if(OX_MOLTENVK AND EXISTS "${OX_MOLTENVK}")
    _ox_copy("${OX_MOLTENVK}" "${OX_DEST}/libMoltenVK.dylib")
    _ox_write_if_different("${OX_DEST}/MoltenVK_icd.json" [=[{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "./libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
]=])
endif()

if(OX_LAYER_LIB AND OX_LAYER_JSON AND EXISTS "${OX_LAYER_JSON}")
    get_filename_component(_layer_name "${OX_LAYER_LIB}" NAME)
    _ox_copy("${OX_LAYER_LIB}" "${OX_DEST}/${_layer_name}")
    file(READ "${OX_LAYER_JSON}" _json)
    string(REGEX REPLACE "\"library_path\"[ ]*:[ ]*\"[^\"]*\"" "\"library_path\": \"./${_layer_name}\"" _json "${_json}")
    _ox_write_if_different("${OX_DEST}/VkLayer_khronos_validation.json" "${_json}")
endif()
