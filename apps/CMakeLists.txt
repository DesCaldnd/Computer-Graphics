# Applications. Each subdirectory with a CMakeLists.txt is added.
file(GLOB _ox_app_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _ox_app_dirs)
foreach(dir ${_ox_app_dirs})
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
        add_subdirectory(${dir})
    endif()
endforeach()
