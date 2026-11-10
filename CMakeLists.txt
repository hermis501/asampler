cmake_minimum_required(VERSION 3.22)
project(ASampler VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC: treat all sources as UTF-8 so non-ASCII string literals (e.g. the
# manufacturer name in JUCE's generated JucePluginDefines.h) round-trip
# correctly into the binary. Without /utf-8, MSVC reads sources in the
# system codepage and mangles multi-byte UTF-8 sequences.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Where the SDKs live. Override on the cmake command line if they move.
#
# These are CACHE variables, so the first configure of a build dir freezes them.
# Moving an SDK afterwards needs either a fresh build dir or an explicit
# -DASAMPLER_JUCE_DIR=... on a reconfigure; build.bat detects the stale case.
set(ASAMPLER_JUCE_DIR    "C:/_tools/JUCE"           CACHE PATH "Path to JUCE source tree")
set(ASAMPLER_CATCH2_DIR  "C:/_tools/Catch2-3.5.4"   CACHE PATH "Path to Catch2 source tree")

if(NOT EXISTS "${ASAMPLER_JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${ASAMPLER_JUCE_DIR}. Set -DASAMPLER_JUCE_DIR=...")
endif()
if(NOT EXISTS "${ASAMPLER_CATCH2_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Catch2 not found at ${ASAMPLER_CATCH2_DIR}. Set -DASAMPLER_CATCH2_DIR=...")
endif()

add_subdirectory("${ASAMPLER_JUCE_DIR}"   "${CMAKE_BINARY_DIR}/_juce"   EXCLUDE_FROM_ALL)
add_subdirectory("${ASAMPLER_CATCH2_DIR}" "${CMAKE_BINARY_DIR}/_catch2" EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# asampler_core: pure DSP/engine code, no JUCE plugin entry points.
# Both the plugin and the test binary link against this — keeps tests fast and
# avoids dragging plugin format wrappers into ctest.
# -----------------------------------------------------------------------------
add_library(asampler_core STATIC
    source/engine/AtomicSharedPtr.h
    source/engine/Sample.h
    source/engine/Voice.h
    source/engine/Voice.cpp
    source/engine/VoicePool.h
    source/engine/VoicePool.cpp
    source/engine/DefaultSample.h
    source/engine/VelocityCurve.h
    source/engine/FilterMode.h
    source/engine/LoopMode.h
    source/engine/Lfo.h
    source/engine/Saturation.h
    source/engine/Transient.h
    source/engine/Transient.cpp
    source/engine/SampleAnalysis.h
    source/engine/SampleAnalysis.cpp
    source/engine/PhaseVocoder.cpp
    source/engine/PhaseVocoder.h
)
target_include_directories(asampler_core PUBLIC source)
target_compile_features(asampler_core PUBLIC cxx_std_20)
target_link_libraries(asampler_core PUBLIC
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_dsp
)
target_compile_definitions(asampler_core PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    # Sample packs ship MP3s. macOS decodes them through CoreAudio already;
    # this enables JUCE's own decoder so Windows reads them too. PUBLIC so the
    # plugin target compiles juce_audio_formats with the same setting.
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

# -----------------------------------------------------------------------------
# ASampler plugin: VST3 everywhere, plus AU on macOS (Logic and GarageBand load
# only AU; JUCE silently drops AU on other platforms). Standalone was previously
# built as an accessibility test harness; it is currently disabled — loaded in
# REAPER instead.
# -----------------------------------------------------------------------------
juce_add_plugin(ASampler
    PRODUCT_NAME                "ASampler"
    COMPANY_NAME                "Hermis Kasperavičius"
    BUNDLE_ID                   com.asampler.asampler
    PLUGIN_MANUFACTURER_CODE    ASmp
    PLUGIN_CODE                 ASm1
    FORMATS                     VST3 AU
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Instrument Sampler
)

target_sources(ASampler PRIVATE
    source/PluginProcessor.cpp
    source/PluginProcessor.h
    source/PluginEditor.cpp
    source/PluginEditor.h
    source/FocusProxyParameter.h
    source/ScreenReaderAnnouncer.cpp
    source/ReaperIntegration.cpp
    source/ClipboardFiles.cpp
    source/ClipboardFiles.h
    source/ScreenReaderAnnouncer.h
)

target_compile_definitions(ASampler PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(ASampler PRIVATE
    asampler_core
    juce::juce_audio_utils
)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
