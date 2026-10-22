cmake_minimum_required(VERSION 3.22)
project(Rootbound VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(JUCE_PATH "C:/JUCE" CACHE PATH "Path to JUCE")
add_subdirectory(${JUCE_PATH} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

juce_add_plugin(Rootbound
    VERSION                     "1.0.0"
    COMPANY_NAME                "Khyle Audio"
    PLUGIN_MANUFACTURER_CODE    KhAu
    PLUGIN_CODE                 RBnd
    FORMATS                     VST3 Standalone
    PRODUCT_NAME                "Rootbound"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             "Fx|Reverb"
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

juce_generate_juce_header(Rootbound)

juce_add_binary_data(RootboundFonts
    SOURCES
        Assets/Fonts/Inter-Regular.ttf
        Assets/Fonts/Inter-Medium.ttf
        Assets/Fonts/Inter-SemiBold.ttf
        Assets/Fonts/Inter-Bold.ttf
        Assets/Fonts/BarlowCondensed-ExtraBold.ttf
        Assets/Fonts/BarlowCondensed-Regular.ttf
        Assets/Fonts/BarlowCondensed-Light.ttf
        Assets/Fonts/OpenDyslexic-Regular.otf
)

target_sources(Rootbound PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/dsp/GrainEngine.cpp
    Source/dsp/GrainEngine.h
    Source/dsp/TiltFilter.cpp
    Source/dsp/TiltFilter.h
    Source/dsp/Saturator.cpp
    Source/dsp/Saturator.h
    Source/dsp/Diffuser.cpp
    Source/dsp/Diffuser.h
    Source/dsp/InputOutputGain.cpp
    Source/dsp/InputOutputGain.h
    Source/gui/RootboundLookAndFeel.cpp
    Source/gui/RootboundLookAndFeel.h
)

target_include_directories(Rootbound PRIVATE Source)

target_compile_definitions(Rootbound PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(Rootbound
    PRIVATE
        RootboundFonts
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ---------------------------------------------------------------------------
# Offline DSP measurement bench (NOT part of the plugin). Off by default.
#   cmake -DROOTBOUND_BENCH=ON .   then build target RootboundBench
# Runs the real DSP classes through the processBlock chain and prints CPU,
# latency, headroom, frequency-response and decay measurements.
# ---------------------------------------------------------------------------
option (ROOTBOUND_BENCH "Build the offline DSP measurement bench" OFF)
if (ROOTBOUND_BENCH)
    juce_add_console_app (RootboundBench PRODUCT_NAME "RootboundBench")
    juce_generate_juce_header (RootboundBench)
    target_sources (RootboundBench PRIVATE
        Source/bench/RootboundBench.cpp
        Source/dsp/GrainEngine.cpp
        Source/dsp/Diffuser.cpp
        Source/dsp/TiltFilter.cpp
        Source/dsp/Saturator.cpp)
    target_include_directories (RootboundBench PRIVATE Source)
    target_compile_definitions (RootboundBench PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 ROOTBOUND_PROBE=1)
    target_link_libraries (RootboundBench
        PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_audio_formats juce::juce_dsp
        PUBLIC  juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
