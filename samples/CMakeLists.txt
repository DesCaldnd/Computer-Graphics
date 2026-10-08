# Samples. Every subdirectory with a CMakeLists.txt is added (OxwaldShowcase is a content-only project for now).
file(GLOB _ox_sample_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _ox_sample_dirs)
foreach(dir ${_ox_sample_dirs})
    if(IS_DIRECTORY ${dir} AND EXISTS ${dir}/CMakeLists.txt)
        add_subdirectory(${dir})
    endif()
endforeach()
