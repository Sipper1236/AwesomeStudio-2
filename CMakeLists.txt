cmake_minimum_required(VERSION 3.28)
project(AwesomeStudio VERSION 0.2.0 LANGUAGES CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Multimedia)

include(FetchContent)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(steinberg_vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG v3.8.1_build_84
    GIT_SHALLOW TRUE
    GIT_SUBMODULES base cmake pluginterfaces public.sdk
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(steinberg_vst3sdk)

if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)
        pkg_check_modules(LILV QUIET IMPORTED_TARGET lilv-0)
        pkg_check_modules(FLAC QUIET IMPORTED_TARGET flac)
        pkg_check_modules(SNDFILE QUIET IMPORTED_TARGET sndfile)
        pkg_check_modules(LAME QUIET IMPORTED_TARGET lame)
    endif()
endif()

add_executable(AwesomeStudio
    src/main.cpp
    src/core/AudioEngine.cpp
    src/core/AudioEngine.h
    src/core/Project.cpp
    src/core/Project.h
    src/core/ProjectFile.cpp
    src/core/ProjectFile.h
    src/core/AudioFile.cpp
    src/core/AudioFile.h
    src/core/Lv2AudioEffect.cpp
    src/core/Lv2AudioEffect.h
    src/core/ClapAudioEffect.cpp
    src/core/ClapAudioEffect.h
    src/core/Vst3AudioEffect.cpp
    src/core/Vst3AudioEffect.h
    src/core/AudioRecorder.cpp
    src/core/AudioRecorder.h
    src/core/MidiFile.cpp
    src/core/MidiFile.h
    src/core/MidiInput.cpp
    src/core/MidiInput.h
    src/core/SynthRenderer.cpp
    src/core/SynthRenderer.h
    src/core/WavExporter.cpp
    src/core/WavExporter.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/Vst3PluginEditor.cpp
    src/ui/Vst3PluginEditor.h
    src/ui/ArrangementEditor.cpp
    src/ui/ArrangementEditor.h
    src/ui/StepSequencer.cpp
    src/ui/StepSequencer.h
    src/ui/MidiEditor.cpp
    src/ui/MidiEditor.h
    ${steinberg_vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${steinberg_vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/eventlist.cpp
)

if(WIN32)
    target_sources(AwesomeStudio PRIVATE
        ${steinberg_vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_win32.cpp
    )
elseif(APPLE)
    target_sources(AwesomeStudio PRIVATE
        ${steinberg_vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm
    )
    set_source_files_properties(
        ${steinberg_vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_mac.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
    set_target_properties(AwesomeStudio PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
    target_link_libraries(AwesomeStudio PRIVATE "-framework Cocoa")
elseif(UNIX)
    target_sources(AwesomeStudio PRIVATE
        ${steinberg_vst3sdk_SOURCE_DIR}/public.sdk/source/vst/hosting/module_linux.cpp
    )
endif()

target_include_directories(AwesomeStudio PRIVATE src third_party/clap/include)
target_link_libraries(AwesomeStudio PRIVATE Qt6::Widgets Qt6::Multimedia sdk_hosting)
target_link_libraries(AwesomeStudio PRIVATE ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(AwesomeStudio PRIVATE winmm)
endif()
if(APPLE)
    find_library(COREMIDI_FRAMEWORK CoreMIDI)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    if(NOT COREMIDI_FRAMEWORK OR NOT COREFOUNDATION_FRAMEWORK)
        message(FATAL_ERROR "CoreMIDI and CoreFoundation frameworks are required on macOS.")
    endif()
    target_link_libraries(AwesomeStudio PRIVATE
        ${COREMIDI_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
    set_target_properties(AwesomeStudio PROPERTIES MACOSX_BUNDLE TRUE)
endif()
target_compile_features(AwesomeStudio PRIVATE cxx_std_17)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(AwesomeStudio PRIVATE AWESOME_STUDIO_HAS_CLAP=1)
endif()
target_compile_definitions(AwesomeStudio PRIVATE AWESOME_STUDIO_HAS_VST3=1)

if(TARGET PkgConfig::ALSA)
    target_link_libraries(AwesomeStudio PRIVATE PkgConfig::ALSA)
    target_compile_definitions(AwesomeStudio PRIVATE AWESOME_STUDIO_HAS_ALSA=1)
endif()

if(TARGET PkgConfig::LILV)
    target_link_libraries(AwesomeStudio PRIVATE PkgConfig::LILV)
    target_compile_definitions(AwesomeStudio PRIVATE AWESOME_STUDIO_HAS_LILV=1)
endif()

if(TARGET PkgConfig::FLAC)
    target_link_libraries(AwesomeStudio PRIVATE PkgConfig::FLAC)
    target_compile_definitions(AwesomeStudio PRIVATE AWESOME_STUDIO_HAS_FLAC=1)
endif()

if(TARGET PkgConfig::SNDFILE)
    target_link_libraries(AwesomeStudio PRIVATE PkgConfig::SNDFILE)
    target_compile_definitions(AwesomeStudio PRIVATE AWESOME_STUDIO_HAS_SNDFILE=1)
endif()

if(TARGET PkgConfig::LAME)
    target_link_libraries(AwesomeStudio PRIVATE PkgConfig::LAME)
    target_compile_definitions(AwesomeStudio PRIVATE AWESOME_STUDIO_HAS_LAME=1)
endif()

if(MSVC)
    target_compile_options(AwesomeStudio PRIVATE /W4 /permissive-)
else()
    target_compile_options(AwesomeStudio PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS AwesomeStudio RUNTIME DESTINATION bin BUNDLE DESTINATION .)
install(FILES third_party/VST3-SDK-LICENSE.txt DESTINATION share/AwesomeStudio)
