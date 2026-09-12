cmake_minimum_required(VERSION 3.22)
project(GMSynthRender VERSION 0.0.1 LANGUAGES C CXX)

get_filename_component(GMSYNTH_ROOT "../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(feature framework coreaudio coremidi aufile alsa dbus jack ladspa libsndfile
        midishare network oss pipewire portaudio pulseaudio readline sdl3 signalsmith
        openmp native-dls)
    set(enable-${feature} OFF CACHE BOOL "" FORCE)
endforeach()
set(enable-threads ON CACHE BOOL "" FORCE)
add_subdirectory("${GMSYNTH_ROOT}/external/fluidsynth" fluidsynth EXCLUDE_FROM_ALL)
add_subdirectory("${GMSYNTH_ROOT}/external/JUCE" JUCE EXCLUDE_FROM_ALL)

add_library(gmsynth_render_core STATIC
    MidiSong.cpp Renderer.cpp
    "${GMSYNTH_ROOT}/Source/FluidSynthEngine.cpp"
    "${GMSYNTH_ROOT}/Source/VariationEffect.cpp")
target_include_directories(gmsynth_render_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${GMSYNTH_ROOT}/Source"
    "${GMSYNTH_ROOT}/external/fluidsynth/include" "${CMAKE_CURRENT_BINARY_DIR}/fluidsynth/include")
target_compile_definitions(gmsynth_render_core PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(gmsynth_render_core PUBLIC libfluidsynth
    juce::juce_audio_basics juce::juce_audio_formats juce::juce_dsp)

juce_add_console_app(gmsynth-render PRODUCT_NAME "gmsynth-render")
set_target_properties(gmsynth-render PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
target_sources(gmsynth-render PRIVATE Main.cpp)
target_link_libraries(gmsynth-render PRIVATE gmsynth_render_core)

include(CTest)
if(BUILD_TESTING)
    juce_add_console_app(gmsynth-render-tests PRODUCT_NAME "gmsynth-render-tests")
    set_target_properties(gmsynth-render-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
    target_sources(gmsynth-render-tests PRIVATE Tests.cpp)
    target_link_libraries(gmsynth-render-tests PRIVATE gmsynth_render_core)
    add_test(NAME render-unit COMMAND gmsynth-render-tests)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME render-cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py" "$<TARGET_FILE:gmsynth-render>")
    # FluidSynth also registers its own tests unconditionally. Select this label
    # to run the renderer suite without building the dependency's test programs.
    set_tests_properties(render-unit render-cli PROPERTIES LABELS "gmsynth-render" TIMEOUT 120)
endif()
