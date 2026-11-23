cmake_minimum_required(VERSION 3.16)
project(AwesomeStudio VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets Multimedia)

add_executable(AwesomeStudio
    src/main.cpp
    src/core/AudioEngine.cpp
    src/core/AudioEngine.h
    src/core/Project.cpp
    src/core/Project.h
    src/core/ProjectFile.cpp
    src/core/ProjectFile.h
    src/core/MidiFile.cpp
    src/core/MidiFile.h
    src/core/SynthRenderer.cpp
    src/core/SynthRenderer.h
    src/core/WavExporter.cpp
    src/core/WavExporter.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/ArrangementEditor.cpp
    src/ui/ArrangementEditor.h
    src/ui/MidiEditor.cpp
    src/ui/MidiEditor.h
)

target_include_directories(AwesomeStudio PRIVATE src)
target_link_libraries(AwesomeStudio PRIVATE Qt6::Widgets Qt6::Multimedia)
target_compile_features(AwesomeStudio PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(AwesomeStudio PRIVATE /W4 /permissive-)
else()
    target_compile_options(AwesomeStudio PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS AwesomeStudio RUNTIME DESTINATION bin BUNDLE DESTINATION .)
