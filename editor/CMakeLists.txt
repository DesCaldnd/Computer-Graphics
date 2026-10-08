# OxwaldEditor — Qt 6 Widgets editor.
#   ox_editor        static library with every panel/dialog/command (tests link it)
#   OxwaldEditor     macOS app bundle / executable
#   ox_editor_tests  Qt Test based tests (label "editor", run with QT_QPA_PLATFORM=offscreen)
#
# Optional engine modules are integrated when their targets exist (OX_EDITOR_HAS_<MODULE> defines), so the
# editor builds with only core+scene and picks up rhi/assets/gameplay/runtime/async as they land.
find_package(Qt6 6.5 COMPONENTS Core Gui Widgets Svg Concurrent Test QUIET)
if(NOT Qt6_FOUND)
    message(STATUS "Oxwald: Qt6 not found - OxwaldEditor is not built")
    return()
endif()
if(NOT TARGET ox_core OR NOT TARGET ox_scene)
    message(STATUS "Oxwald: editor needs the core and scene modules")
    return()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

file(GLOB_RECURSE _ed_srcs CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp)
list(FILTER _ed_srcs EXCLUDE REGEX "/src/app/")
if(APPLE)
    file(GLOB_RECURSE _ed_mm CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm)
    list(APPEND _ed_srcs ${_ed_mm})
endif()

add_library(ox_editor STATIC ${_ed_srcs})
add_library(Oxwald::editor ALIAS ox_editor)
target_include_directories(ox_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ox_editor PUBLIC Oxwald::core Oxwald::scene Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg
                                       Qt6::Concurrent)
target_compile_definitions(ox_editor PUBLIC QT_NO_KEYWORDS
    OX_EDITOR_VERSION="${PROJECT_VERSION}"
    OX_EDITOR_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
ox_set_warnings(ox_editor)
set_target_properties(ox_editor PROPERTIES FOLDER "editor")

# Icons, theme and images (globbed so new icons need no list maintenance).
file(GLOB _ed_res RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/resources CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/*.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/theme/*
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/*.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/resources/*.png)
list(TRANSFORM _ed_res PREPEND resources/)
qt_add_resources(ox_editor "oxwald_editor_resources_files" PREFIX "/" BASE resources FILES ${_ed_res})
if(APPLE)
    target_link_libraries(ox_editor PUBLIC "-framework AppKit" "-framework QuartzCore")
endif()

# ---- optional module integration ----------------------------------------------------------------------------
foreach(_mod rhi assets gameplay runtime async render physics script ai world spline audio animation)
    string(TOUPPER ${_mod} _MOD)
    if(TARGET ox_${_mod})
        target_link_libraries(ox_editor PUBLIC ox_${_mod})
        target_compile_definitions(ox_editor PUBLIC OX_EDITOR_HAS_${_MOD}=1)
        message(STATUS "OxwaldEditor: integrating module '${_mod}'")
    else()
        target_compile_definitions(ox_editor PUBLIC OX_EDITOR_HAS_${_MOD}=0)
    endif()
endforeach()

# ---- application ---------------------------------------------------------------------------------------------
set(_app_icon "")
add_executable(OxwaldEditor MACOSX_BUNDLE ${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp)
target_link_libraries(OxwaldEditor PRIVATE ox_editor)
ox_set_warnings(OxwaldEditor)
set_target_properties(OxwaldEditor PROPERTIES
    FOLDER "editor"
    MACOSX_BUNDLE_BUNDLE_NAME "OxwaldEditor"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.oxwald.editor"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
# Static Qt: the default platform plugin (cocoa) is linked automatically; add offscreen for headless
# screenshot generation (QT_QPA_PLATFORM=offscreen OxwaldEditor --screenshots <dir>).
if(QT_KNOWN_POLICY_QTP0002 OR TRUE)
    if(TARGET Qt6::QOffscreenIntegrationPlugin)
        qt_import_plugins(OxwaldEditor INCLUDE Qt6::QOffscreenIntegrationPlugin)
    endif()
endif()
if(COMMAND ox_deploy_vulkan_runtime AND TARGET ox_rhi)
    # Vulkan loader + MoltenVK ICD + validation layer next to the executable (Contents/MacOS/vulkan).
    ox_deploy_vulkan_runtime(OxwaldEditor)
    if(APPLE)
        add_custom_command(TARGET OxwaldEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -DBUNDLE=$<TARGET_BUNDLE_DIR:OxwaldEditor> -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ShareMoltenVK.cmake
            VERBATIM)
    endif()
endif()
# vcpkg's applocal step rewrites dylib paths after linking, which invalidates the linker's ad-hoc signature
# (macOS then SIGKILLs the process). Re-sign after it (and after the Vulkan runtime deployment).
if(APPLE)
    add_custom_command(TARGET OxwaldEditor POST_BUILD
        COMMAND codesign --force --deep -s - $<TARGET_BUNDLE_DIR:OxwaldEditor>
        COMMENT "Ad-hoc signing OxwaldEditor.app" VERBATIM)
endif()

# ---- tests ---------------------------------------------------------------------------------------------------
if(OX_BUILD_TESTS)
    file(GLOB _ed_tests CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.hpp)
    if(_ed_tests)
        add_executable(ox_editor_tests ${_ed_tests})
        target_link_libraries(ox_editor_tests PRIVATE ox_editor Qt6::Test)
        target_include_directories(ox_editor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        ox_set_warnings(ox_editor_tests)
        set_target_properties(ox_editor_tests PROPERTIES FOLDER "editor")
        if(TARGET Qt6::QOffscreenIntegrationPlugin)
            qt_import_plugins(ox_editor_tests INCLUDE Qt6::QOffscreenIntegrationPlugin)
        endif()
        if(APPLE)
            # Static Qt Gui links MoltenVK (Vulkan support). Use a private, freshly signed copy next to the test
            # binary: the shared vcpkg_installed copy may carry an invalidated signature after applocal runs.
            set(_mvk ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/libMoltenVK.dylib)
            if(EXISTS ${_mvk})
                add_custom_command(TARGET ox_editor_tests POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_mvk} $<TARGET_FILE_DIR:ox_editor_tests>/libMoltenVK.dylib
                    COMMAND codesign --force -s - $<TARGET_FILE_DIR:ox_editor_tests>/libMoltenVK.dylib
                    COMMAND install_name_tool -change @rpath/libMoltenVK.dylib @executable_path/libMoltenVK.dylib $<TARGET_FILE:ox_editor_tests>
                    VERBATIM)
            endif()
            add_custom_command(TARGET ox_editor_tests POST_BUILD
                COMMAND codesign --force -s - $<TARGET_FILE:ox_editor_tests> VERBATIM)
        endif()
        # one ctest entry per QtTest class (the executable takes the class name as first argument)
        foreach(_cls InspectorTests CommandTests SettingsTests IntegrationTests)
            add_test(NAME editor.${_cls} COMMAND ox_editor_tests ${_cls})
            set_tests_properties(editor.${_cls} PROPERTIES
                LABELS "editor"
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OXWALD_SOURCE_DIR=${CMAKE_SOURCE_DIR};OX_EDITOR_TEST_TMP=${CMAKE_CURRENT_BINARY_DIR}/test_tmp/${_cls};OX_EDITOR_USER_DIR=${CMAKE_CURRENT_BINARY_DIR}/test_tmp/${_cls}/user;OX_EDITOR_HEADLESS=1"
                TIMEOUT 300)
        endforeach()
    endif()
endif()
