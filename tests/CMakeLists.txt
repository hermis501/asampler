add_executable(ASamplerTests
    test_voice.cpp
    test_voice_live.cpp
    test_voice_pool.cpp
    test_production.cpp
    test_melodic.cpp
    test_formant_correct.cpp
    test_formant_pitch.cpp
    test_velocity_curve.cpp
    test_saturation.cpp
    test_lfo.cpp
    test_transient.cpp
    test_phasevocoder.cpp
    test_plugin_processor.cpp
    test_vocal.cpp
)

# Link the plugin's SharedCode static library so tests can construct the real
# ASamplerProcessor. ASampler_SharedCode propagates the JucePlugin_* compile
# defines that PluginProcessor.h relies on (e.g. JucePlugin_Name in getName()).
target_link_libraries(ASamplerTests PRIVATE
    asampler_core
    ASampler_SharedCode
    Catch2::Catch2WithMain
)

# juce_add_plugin places ASampler_SharedCode.lib under ASampler_artefacts/<config>/
# (an artefacts subdir, not the default per-config bin dir). CMake links it by
# bare name, so the test binary's linker needs that directory on its search path.
target_link_directories(ASamplerTests PRIVATE
    "${CMAKE_BINARY_DIR}/ASampler_artefacts/$<CONFIG>"
)

target_compile_features(ASamplerTests PRIVATE cxx_std_20)

list(APPEND CMAKE_MODULE_PATH "${ASAMPLER_CATCH2_DIR}/extras")
include(Catch)
catch_discover_tests(ASamplerTests)
