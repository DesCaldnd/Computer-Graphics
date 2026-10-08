# Command line tools. Every subdirectory with a CMakeLists.txt is added; a tool should skip itself when the
# engine modules it needs are not configured (see OX_MODULES).
file(GLOB _ox_tool_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _ox_tool_dirs)
foreach(dir ${_ox_tool_dirs})
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
        add_subdirectory(${dir})
    endif()
endforeach()
