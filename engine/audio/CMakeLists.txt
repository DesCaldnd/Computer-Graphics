# miniaudio is a single-header library in vcpkg (no CMake config); stb_vorbis (from the stb port, same
# include root) gives it Ogg Vorbis decoding.
find_path(OX_MINIAUDIO_INCLUDE_DIR miniaudio.h REQUIRED)

set(_ox_audio_impl ${CMAKE_CURRENT_SOURCE_DIR}/src/miniaudio_impl.c)
set_source_files_properties(${_ox_audio_impl} PROPERTIES COMPILE_OPTIONS "-w")
if(MSVC)
    # The miniaudio implementation is the one translation unit that wants the full Win32 audio API (WASAPI,
    # DirectSound, WinMM); the project-wide WIN32_LEAN_AND_MEAN strips mmsystem.h / ole2.h from <windows.h>.
    set_source_files_properties(${_ox_audio_impl} PROPERTIES COMPILE_OPTIONS "-w;/UWIN32_LEAN_AND_MEAN")
endif()

ox_add_module(audio
    PUBLIC_DEPS  Oxwald::core
    EXTRA_SOURCES ${_ox_audio_impl})

target_include_directories(ox_audio SYSTEM PRIVATE ${OX_MINIAUDIO_INCLUDE_DIR})
if(APPLE)
    target_link_libraries(ox_audio PRIVATE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
    target_compile_definitions(ox_audio PRIVATE MA_NO_RUNTIME_LINKING)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(ox_audio PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(NOT APPLE)
        target_link_libraries(ox_audio PRIVATE m)
    endif()
endif()
