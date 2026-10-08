# Every engine module lives in engine/<name>/ with its own CMakeLists.txt calling ox_add_module().
# Modules are discovered automatically; link order does not matter because targets are
# resolved at generate time.
file(GLOB _ox_module_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _ox_module_dirs)
foreach(dir ${_ox_module_dirs})
    get_filename_component(_name ${dir} NAME)
    if(OX_MODULES AND NOT _name IN_LIST OX_MODULES)
        continue()
    endif()
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
        add_subdirectory(${dir})
    endif()
endforeach()
