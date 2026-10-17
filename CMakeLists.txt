cmake_minimum_required(VERSION 3.22)
project(AxellDeReverb VERSION 2.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.15
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(JUCE)

# Axell core (shared DSP + look): next to this project, or vendored into external/axell-core for releases
set(AXELL_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/external/axell-core" CACHE PATH "Axell core")
if(NOT EXISTS "${AXELL_CORE_DIR}/CMakeLists.txt")
  set(AXELL_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../axell-core" CACHE PATH "Axell core" FORCE)
endif()
add_subdirectory(${AXELL_CORE_DIR} axell-core)

# the network's weights (research/export_dr.py), embedded
juce_add_binary_data(AxDRWeights SOURCES Resources/dereverb.bin)
set_target_properties(AxDRWeights PROPERTIES POSITION_INDEPENDENT_CODE ON)

juce_add_plugin(AxellDeReverb
  COMPANY_NAME              "Axell Yobag"
  COMPANY_EMAIL             "axellpetre@protonmail.com"
  BUNDLE_ID                 com.axellyobag.axelldereverb
  PLUGIN_MANUFACTURER_CODE  Axyb
  PLUGIN_CODE               Drv1
  FORMATS                   VST3
  PRODUCT_NAME              "Axell De-Reverb"
  IS_SYNTH                  FALSE
  NEEDS_MIDI_INPUT          FALSE
  NEEDS_MIDI_OUTPUT         FALSE
  IS_MIDI_EFFECT            FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD   FALSE
  VST3_CATEGORIES           Fx Restoration)

set(DR_SOURCES Source/PluginProcessor.cpp Source/PluginEditor.cpp)
set(DR_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
target_sources(AxellDeReverb PRIVATE ${DR_SOURCES})
target_compile_definitions(AxellDeReverb PUBLIC ${DR_DEFS})
axell_core_add_gui(AxellDeReverb)
target_link_libraries(AxellDeReverb PRIVATE juce::juce_audio_utils AxDRWeights
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

option(DR_SANITIZE "AddressSanitizer + UBSan for the test programs" OFF)
function(dr_sanitize target)
  if(DR_SANITIZE)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endif()
endfunction()

# DSP suite (JUCE-free: the engine headers exactly as they ship)
enable_testing()
add_executable(DrDspTests tests/DspTests.cpp)
target_include_directories(DrDspTests PRIVATE Source)
target_link_libraries(DrDspTests PRIVATE axell_core_dsp)
target_compile_options(DrDspTests PRIVATE -Wall -Wextra -Wshadow -Wno-mismatched-new-delete $<$<CONFIG:Release>:-O3>)
dr_sanitize(DrDspTests)
add_test(NAME dsp_suite COMMAND DrDspTests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# plugin suite + GUI snapshots (JUCE)
foreach(tool DrPluginTests DrGuiSnapshot)
  juce_add_console_app(${tool} PRODUCT_NAME "${tool}")
  target_compile_definitions(${tool} PRIVATE ${DR_DEFS} JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Name="Axell De-Reverb")
  target_link_libraries(${tool} PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags AxDRWeights)
  axell_core_add_gui(${tool})
  dr_sanitize(${tool})
endforeach()
target_sources(DrPluginTests PRIVATE tests/PluginTests.cpp ${DR_SOURCES})
target_sources(DrGuiSnapshot PRIVATE tests/GuiSnapshot.cpp ${DR_SOURCES})
add_test(NAME plugin_suite COMMAND DrPluginTests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# independent black-box audit host (from the core)
juce_add_console_app(DrAuditHost PRODUCT_NAME "DrAuditHost")
target_sources(DrAuditHost PRIVATE ${AXELL_CORE_DIR}/tools/AuditHost.cpp)
target_compile_definitions(DrAuditHost PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(DrAuditHost PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)

# research tool (JUCE-free; the shipping engine)
add_executable(DRRender tools/DRRender.cpp)
target_include_directories(DRRender PRIVATE Source)
target_link_libraries(DRRender PRIVATE axell_core_dsp)
target_compile_options(DRRender PRIVATE -O3)
