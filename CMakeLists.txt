cmake_minimum_required(VERSION 3.16)
project(AwesomeStudio VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Multimedia)

if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ALSA QUIET IMPORTED_TARGET alsa)
        pkg_check_modules(LILV QUIET IMPORTED_TARGET lilv-0)
        pkg_check_modules(FLAC QUIET IMPORTED_TARGET flac)
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
    src/ui/ArrangementEditor.cpp
    src/ui/ArrangementEditor.h
    src/ui/StepSequencer.cpp
    src/ui/StepSequencer.h
    src/ui/MidiEditor.cpp
    src/ui/MidiEditor.h
)

target_include_directories(AwesomeStudio PRIVATE src third_party/clap/include)
target_link_libraries(AwesomeStudio PRIVATE Qt6::Widgets Qt6::Multimedia)
target_link_libraries(AwesomeStudio PRIVATE ${CMAKE_DL_LIBS})
target_compile_features(AwesomeStudio PRIVATE cxx_std_17)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(AwesomeStudio PRIVATE AWESOME_STUDIO_HAS_CLAP=1)
endif()

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

if(MSVC)
    target_compile_options(AwesomeStudio PRIVATE /W4 /permissive-)
else()
    target_compile_options(AwesomeStudio PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS AwesomeStudio RUNTIME DESTINATION bin BUNDLE DESTINATION .)
