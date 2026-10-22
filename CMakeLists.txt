# CMakeLists.txt
# Build configuration for Awesome Studio

cmake_minimum_required(VERSION 3.16)
project(AwesomeStudio VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Third party directories
set(CMAKE_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(JUCE_SOURCE "${CMAKE_THIRD_PARTY_DIR}/JUCE")

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${JUCE_SOURCE})

# Load JUCE module
include(${JUCE_SOURCE}/CMakeLists.txt)

# Add executable
add_executable(AwesomeStudio
    src/core/Application.cpp
    src/core/AudioEngine.cpp
    src/core/Mixer.cpp
    src/core/Transport.cpp
    src/core/Automation.cpp
    src/core/Delay.cpp
    src/core/Reverb.cpp
    src/core/Compressor.cpp
    src/core/EQ3Band.cpp
    src/ui/MainWindow.cpp
    src/ui/PluginEditor.cpp
    src/ui/MidiEditor.cpp
    src/ui/NotationView.cpp
    src/ui/MeteringView.cpp
    src/core/BatchProcessor.cpp
    src/core/ProjectTemplates.cpp
    src/core/Arpeggiator.cpp
    # Additional source files will be added here
)

# Link libraries
target_link_libraries(AwesomeStudio
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
    JUCE::juce_audio_basics
    JUCE::juce_audio_devices
    JUCE::juce_audio_formats
    JUCE::juce_audio_processors
    JUCE::juce_audio_utils
    JUCE::juce_core
    JUCE::juce_data_structures
    JUCE::juce_events
    JUCE::juce_graphics
    JUCE::juce_gui_basics
    JUCE::juce_gui_extra
)

# Set properties
set_target_properties(AwesomeStudio PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Install target
install(TARGETS AwesomeStudio DESTINATION bin)

# Add tests
enable_testing()
add_subdirectory(tests)