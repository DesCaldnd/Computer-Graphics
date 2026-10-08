# cmake -DOX_DEST=<dir> -DOX_LOADER=... -DOX_LOADER_NAME=... -DOX_MOLTENVK=... -DOX_LAYER_LIB=... -DOX_LAYER_JSON=...
#       [-DOX_NGX_DEST=<dir> -DOX_NGX_FILES=<file>::<file>...]
#       -P DeployVulkanRuntime.cmake
# Copies the Vulkan loader, the MoltenVK ICD (macOS) and the Khronos validation layer into OX_DEST and writes
# manifests whose library paths are relative to the manifest ("./lib...", ".\\lib..." on Windows), so the folder is
# relocatable. On Windows the DLLs the validation layer imports from its own directory (vcpkg bin/) are copied too.
# OX_NGX_FILES ("::"-separated NVIDIA NGX/DLSS runtime libraries) are copied into OX_NGX_DEST (the executable's
# directory, where NGX looks for them).
cmake_minimum_required(VERSION 3.25)

# Every executable of a build tree lives in the same bin/ directory, so the POST_BUILD steps of several targets
# deploy into the same folders in parallel. Windows refuses to replace or write a file another process has open:
# serialize the deployments per destination directory there.
function(_ox_lock_dir dir)
    file(MAKE_DIRECTORY "${dir}")
    if(CMAKE_HOST_WIN32)
        file(LOCK "${dir}/.ox_deploy.lock" GUARD PROCESS TIMEOUT 300 RESULT_VARIABLE _locked)
        if(NOT _locked EQUAL 0)
            message(STATUS "Oxwald deploy: could not lock ${dir} (${_locked}); continuing without the lock")
        endif()
    endif()
endfunction()

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
    file(RENAME "${_tmp}" "${dst}" RESULT _renamed)
    if(NOT _renamed EQUAL 0)
        # Windows: the destination is loaded by a running process (a test, the editor) and cannot be replaced.
        # Keep the copy that is already there and try again on the next build.
        file(REMOVE "${_tmp}")
        if(EXISTS "${dst}")
            message(STATUS "Oxwald deploy: ${dst} is in use, keeping the existing copy (${_renamed})")
            return()
        endif()
        message(FATAL_ERROR "Oxwald deploy: cannot create ${dst}: ${_renamed}")
    endif()
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

# Windows: the layer DLL sits in vcpkg's bin/ next to the DLLs it imports. In OX_DEST it is loaded by full path,
# so those DLLs must travel with it (the loader searches the layer's own directory). The import list is resolved
# with dumpbin once per layer build and cached next to the copy.
function(_ox_copy_layer_dependencies layer dest_dir)
    file(REAL_PATH "${layer}" _real)
    get_filename_component(_src_dir "${_real}" DIRECTORY)
    get_filename_component(_name "${_real}" NAME)
    file(TIMESTAMP "${_real}" _ts "%Y%m%d%H%M%S" UTC)
    file(SIZE "${_real}" _size)
    set(_key "${_real}|${_ts}|${_size}")
    set(_cache "${dest_dir}/${_name}.deps")
    set(_deps "")
    set(_cached OFF)
    if(EXISTS "${_cache}")
        file(STRINGS "${_cache}" _lines)
        list(POP_FRONT _lines _old_key)
        if(_old_key STREQUAL _key)
            set(_deps "${_lines}")
            set(_cached ON)
        endif()
    endif()
    if(NOT _cached)
        find_program(_ox_dumpbin NAMES dumpbin)
        if(NOT _ox_dumpbin)
            message(STATUS "Oxwald deploy: dumpbin not found, dependencies of ${_name} are not deployed "
                           "(build from a Visual Studio developer environment)")
            return()
        endif()
        set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
        set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "dumpbin")
        set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${_ox_dumpbin}")
        file(GET_RUNTIME_DEPENDENCIES
            LIBRARIES "${_real}"
            RESOLVED_DEPENDENCIES_VAR _resolved
            UNRESOLVED_DEPENDENCIES_VAR _unresolved
            CONFLICTING_DEPENDENCIES_PREFIX _conflict
            PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
            # Do not walk into the system DLLs (slow, and nothing of theirs is deployed).
            POST_EXCLUDE_REGEXES "[/\\\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\\\]" "[/\\\\][Ss][Yy][Ss][Ww][Oo][Ww]64[/\\\\]"
                                 "[/\\\\][Ww][Ii][Nn][Ss][Xx][Ss][/\\\\]")
        foreach(_dep IN LISTS _resolved)
            get_filename_component(_dep_name "${_dep}" NAME)
            # Only what comes from the layer's own directory; system and CRT DLLs stay where they are.
            if(EXISTS "${_src_dir}/${_dep_name}")
                list(APPEND _deps "${_dep_name}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _deps)
        list(JOIN _deps "\n" _text)
        file(WRITE "${_cache}" "${_key}\n${_text}\n")
    endif()
    foreach(_dep_name IN LISTS _deps)
        _ox_copy("${_src_dir}/${_dep_name}" "${dest_dir}/${_dep_name}")
    endforeach()
endfunction()

if(OX_DEST)
    _ox_lock_dir("${OX_DEST}")
endif()

if(OX_DEST AND OX_LOADER)
    _ox_copy("${OX_LOADER}" "${OX_DEST}/${OX_LOADER_NAME}")
endif()

if(OX_DEST AND OX_MOLTENVK AND EXISTS "${OX_MOLTENVK}")
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

if(OX_DEST AND OX_LAYER_LIB AND OX_LAYER_JSON AND EXISTS "${OX_LAYER_JSON}")
    get_filename_component(_layer_name "${OX_LAYER_LIB}" NAME)
    _ox_copy("${OX_LAYER_LIB}" "${OX_DEST}/${_layer_name}")
    # The Windows loader treats a library_path as manifest-relative only when it contains a backslash; "./x.dll"
    # would be handed to LoadLibrary as is and resolved against the working directory. JSON needs it escaped.
    if(_layer_name MATCHES "\\.dll$")
        set(_layer_rel [=[.\\]=])
        _ox_copy_layer_dependencies("${OX_LAYER_LIB}" "${OX_DEST}")
    else()
        set(_layer_rel "./")
    endif()
    file(READ "${OX_LAYER_JSON}" _json)
    string(REGEX REPLACE "\"library_path\"[ ]*:[ ]*\"[^\"]*\"" "\"library_path\": \"@OX_LAYER_PATH@\"" _json "${_json}")
    string(REPLACE "@OX_LAYER_PATH@" "${_layer_rel}${_layer_name}" _json "${_json}")
    _ox_write_if_different("${OX_DEST}/VkLayer_khronos_validation.json" "${_json}")
endif()

if(OX_NGX_DEST AND OX_NGX_FILES)
    _ox_lock_dir("${OX_NGX_DEST}")
    string(REPLACE "::" ";" _ngx_files "${OX_NGX_FILES}")
    foreach(_ngx IN LISTS _ngx_files)
        get_filename_component(_ngx_name "${_ngx}" NAME)
        _ox_copy("${_ngx}" "${OX_NGX_DEST}/${_ngx_name}")
    endforeach()
endif()
